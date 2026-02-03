#include "Lan/RowAdd/RowWriteService.h"
#include "Loger/Logger.h"
#include "Storage/MinioPlugin.h"

#include <drogon/drogon.h>
#include <drogon/utils/Utilities.h>

#include <algorithm>
#include <sstream>
#include <stdexcept>

RowWriteService::RowWriteService()
    : registry_(createDefaultRowWritePlannerRegistry())
{
}

std::unordered_map<std::string, const AttachmentInput *> RowWriteService::buildAttachmentIndex(
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

std::string RowWriteService::buildObjectKey(const std::string &table,
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

drogon::Task<void> RowWriteService::executePlan(
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
            oss << "RowWriteError: attachment not found for upload op"
                << " attachmentId=" << upload.attachmentId;
            Logger::instance().error(oss.str());
            throw RowWriteError("bad_request", "Attachment not found for upload op", drogon::k400BadRequest);
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
            oss << "RowWriteError: MinIO upload failed"
                << " bucket=" << upload.bucket
                << " key=" << upload.objectKey
                << " size=" << att->data.size();
            Logger::instance().error(oss.str());
            throw RowWriteError("storage_error", "Failed to upload object to storage", drogon::k500InternalServerError, details);
        }
        uploadedObjects.push_back(UploadedObject{upload.bucket, upload.objectKey});
    }

    for (const auto &op : plan.postUploadDbOps)
    {
        co_await op.exec(trans);
    }

    co_return;
}

drogon::Task<WriteResult> RowWriteService::write(const RowController::ParsedRequest &parsed)
{
    // Точка расширения по таблицам:
    // - новые таблицы добавляются через RowWritePlannerRegistry
    // - логика конкретной таблицы/типов НЕ должна появляться здесь
    if (!parsed.payload.isObject() || !parsed.payload.isMember("table") || !parsed.payload["table"].isString())
    {
        Logger::instance().error("RowWriteError: invalid payload, missing table");
        throw RowWriteError("bad_request", "Invalid payload: missing table", drogon::k400BadRequest);
    }
    const std::string table = parsed.payload["table"].asString();
    auto planner = registry_->getPlanner(table);
    if (!planner)
    {
        Json::Value details;
        details["table"] = table;
        std::ostringstream oss;
        oss << "RowWriteError: table is not supported table=" << table;
        Logger::instance().error(oss.str());
        throw RowWriteError("bad_request", "Table is not supported", drogon::k400BadRequest, details);
    }

    if (auto validationErr = co_await planner->validate(parsed))
    {
        std::ostringstream oss;
        oss << "RowWriteError: validation failed"
            << " code=" << validationErr->code
            << " status=" << static_cast<int>(validationErr->status)
            << " message=" << validationErr->message;
        Logger::instance().error(oss.str());
        throw RowWriteError(validationErr->code,
                            validationErr->message,
                            validationErr->status,
                            validationErr->details);
    }

    auto dbClient = drogon::app().getDbClient("default");
    auto trans = co_await dbClient->newTransactionCoro();
    auto minioPlugin = drogon::app().getPlugin<MinioPlugin>();
    if (!minioPlugin)
    {
        Logger::instance().error("RowWriteError: MinioPlugin is not initialized");
        throw RowWriteError("internal", "MinioPlugin is not initialized", drogon::k500InternalServerError);
    }
    MinioClient &minioClient = minioPlugin->client();

    // Вставка базовой строки — делегируется planner-у.
    const int64_t rowId = co_await planner->insertBaseRow(parsed, trans);


    std::unordered_map<std::string, std::string> objectKeys;
    objectKeys.reserve(parsed.attachments.size());
    for (const auto &att : parsed.attachments)
    {
        objectKeys.emplace(att.id, buildObjectKey(table, rowId, att));
    }

    // Построение плана записи (DB ops + uploads) — зона расширения по типам вложений.
    const RowWritePlan plan = planner->buildWritePlan(rowId, parsed, objectKeys, minioPlugin->minioConfig());
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

