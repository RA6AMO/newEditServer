#include "Lan/CellUpdate/CellUpdateService.h"
#include "Loger/Logger.h"
#include "Storage/MinioPlugin.h"

#include <drogon/drogon.h>
#include <drogon/utils/Utilities.h>

#include <algorithm>
#include <cmath>
#include <optional>
#include <sstream>
#include <stdexcept>

namespace
{
std::optional<int64_t> parseRowId(const Json::Value &payload)
{
    if (!payload.isObject() || !payload.isMember("rowId"))
    {
        return std::nullopt;
    }
    const Json::Value &v = payload["rowId"];
    if (v.isInt64())
    {
        return v.asInt64();
    }
    if (v.isUInt64())
    {
        return static_cast<int64_t>(v.asUInt64());
    }
    if (v.isInt())
    {
        return v.asInt();
    }
    if (v.isUInt())
    {
        return static_cast<int64_t>(v.asUInt());
    }
    if (v.isDouble())
    {
        const double d = v.asDouble();
        if (std::floor(d) == d)
        {
            return static_cast<int64_t>(d);
        }
        return std::nullopt;
    }
    if (v.isString())
    {
        try
        {
            size_t idx = 0;
            const std::string s = v.asString();
            const long long n = std::stoll(s, &idx, 10);
            if (idx == s.size())
            {
                return static_cast<int64_t>(n);
            }
        }
        catch (...)
        {
        }
    }
    return std::nullopt;
}
} // namespace

CellUpdateService::CellUpdateService()
    : registry_(createDefaultCellUpdatePlannerRegistry())
{
}

std::unordered_map<std::string, const AttachmentInput *> CellUpdateService::buildAttachmentIndex(
    const std::vector<AttachmentInput> &attachments) const
{
    std::unordered_map<std::string, const AttachmentInput *> index;
    index.reserve(attachments.size());
    for (const auto &att : attachments)
    {
        index.emplace(att.id, &att);
    }
    return index;
}

std::string CellUpdateService::buildObjectKey(const std::string &table,
                                              int64_t rowId,
                                              const AttachmentInput &attachment) const
{
    std::string ext;
    const std::string &name = attachment.filename;
    const auto pos = name.find_last_of('.');
    if (pos != std::string::npos && pos + 1 < name.size())
    {
        ext = name.substr(pos + 1);
    }

    const std::string uuid = drogon::utils::getUuid(true);
    std::string key = table + "/" + std::to_string(rowId) + "/" + attachment.dbName + "_" + attachment.role + "_" + uuid;
    if (!ext.empty())
    {
        key += "." + ext;
    }
    return key;
}

drogon::Task<void> CellUpdateService::executePlan(
    const std::shared_ptr<drogon::orm::Transaction> &trans,
    MinioClient &minioClient,
    const RowWritePlan &plan,
    const std::unordered_map<std::string, const AttachmentInput *> &attachmentIndex,
    std::vector<UploadedObject> &uploadedObjects)
{
    for (const auto &op : plan.preUploadDbOps)
    {
        co_await op.exec(trans);
    }

    for (const auto &upload : plan.uploads)
    {
        auto it = attachmentIndex.find(upload.attachmentId);
        if (it == attachmentIndex.end())
        {
            std::ostringstream oss;
            oss << "CellUpdateError: attachment not found for upload op"
                << " attachmentId=" << upload.attachmentId;
            Logger::instance().error(oss.str());
            throw CellUpdateError("bad_request", "Attachment not found for upload op", drogon::k400BadRequest);
        }
        const AttachmentInput *att = it->second;
        const bool ok = minioClient.putObject(upload.bucket,
                                              upload.objectKey,
                                              att->data,
                                              upload.mimeType);
        if (!ok)
        {
            Json::Value details(Json::objectValue);
            details["bucket"] = upload.bucket;
            details["objectKey"] = upload.objectKey;
            details["mimeType"] = upload.mimeType;
            details["sizeBytes"] = static_cast<Json::UInt64>(att->data.size());
            std::ostringstream oss;
            oss << "CellUpdateError: MinIO upload failed"
                << " bucket=" << upload.bucket
                << " key=" << upload.objectKey
                << " size=" << att->data.size();
            Logger::instance().error(oss.str());
            throw CellUpdateError("storage_error", "Failed to upload object to storage", drogon::k500InternalServerError, details);
        }
        uploadedObjects.push_back(UploadedObject{upload.bucket, upload.objectKey});
    }

    for (const auto &op : plan.postUploadDbOps)
    {
        co_await op.exec(trans);
    }

    co_return;
}

drogon::Task<WriteResult> CellUpdateService::update(const CellUpdateController::ParsedRequest &parsed)
{
    if (!parsed.payload.isObject() || !parsed.payload.isMember("table") || !parsed.payload["table"].isString())
    {
        Logger::instance().error("CellUpdateError: invalid payload, missing table");
        throw CellUpdateError("bad_request", "Invalid payload: missing table", drogon::k400BadRequest);
    }
    const std::string table = parsed.payload["table"].asString();
    auto planner = registry_->getPlanner(table);
    if (!planner)
    {
        Json::Value details;
        details["table"] = table;
        std::ostringstream oss;
        oss << "CellUpdateError: table is not supported table=" << table;
        Logger::instance().error(oss.str());
        throw CellUpdateError("bad_request", "Table is not supported", drogon::k400BadRequest, details);
    }

    if (auto validationErr = co_await planner->validate(parsed))
    {
        std::ostringstream oss;
        oss << "CellUpdateError: validation failed"
            << " code=" << validationErr->code
            << " status=" << static_cast<int>(validationErr->status)
            << " message=" << validationErr->message;
        Logger::instance().error(oss.str());
        throw CellUpdateError(validationErr->code,
                              validationErr->message,
                              validationErr->status,
                              validationErr->details);
    }

    const auto rowIdOpt = parseRowId(parsed.payload);
    if (!rowIdOpt || *rowIdOpt <= 0)
    {
        throw CellUpdateError("bad_request", "Invalid payload: missing rowId", drogon::k400BadRequest);
    }
    const int64_t rowId = *rowIdOpt;

    auto dbClient = drogon::app().getDbClient("default");
    auto trans = co_await dbClient->newTransactionCoro();
    auto minioPlugin = drogon::app().getPlugin<MinioPlugin>();
    if (!minioPlugin)
    {
        Logger::instance().error("CellUpdateError: MinioPlugin is not initialized");
        throw CellUpdateError("internal", "MinioPlugin is not initialized", drogon::k500InternalServerError);
    }
    MinioClient &minioClient = minioPlugin->client();

    std::unordered_map<std::string, std::string> objectKeys;
    objectKeys.reserve(parsed.attachments.size());
    for (const auto &att : parsed.attachments)
    {
        objectKeys.emplace(att.id, buildObjectKey(table, rowId, att));
    }

    const RowWritePlan plan = planner->buildUpdatePlan(rowId, parsed, objectKeys, minioPlugin->minioConfig());
    const auto attachmentIndex = buildAttachmentIndex(parsed.attachments);

    std::vector<UploadedObject> uploadedObjects;
    std::exception_ptr eptr;
    try
    {
        co_await executePlan(trans, minioClient, plan, attachmentIndex, uploadedObjects);
    }
    catch (...)
    {
        eptr = std::current_exception();
    }

    if (eptr)
    {
        if (trans)
        {
            trans->rollback();
        }
        for (const auto &obj : uploadedObjects)
        {
            (void)minioClient.deleteObject(obj.bucket, obj.objectKey);
        }
        std::rethrow_exception(eptr);
    }

    WriteResult result;
    result.rowId = rowId;
    Json::Value extra(Json::objectValue);
    if (!objectKeys.empty())
    {
        Json::Value attachments(Json::objectValue);
        for (const auto &kv : objectKeys)
        {
            attachments[kv.first] = kv.second;
        }
        extra["attachments"] = attachments;
    }
    if (!plan.successExtra.isNull())
    {
        extra["plan"] = plan.successExtra;
    }
    result.extra = extra;
    co_return result;
}
