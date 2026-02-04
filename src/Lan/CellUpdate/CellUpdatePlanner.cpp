#include "Lan/CellUpdate/CellUpdatePlanner.h"
#include "Lan/CellUpdate/CellUpdateErrors.h"
#include "Lan/allTableList.h"
#include "TableInfoCache.h"

#include <drogon/drogon.h>
#include <algorithm>
#include <cctype>
#include <cmath>
#include <optional>
#include <stdexcept>
#include <unordered_map>

namespace
{
bool isSafeIdentifier(const std::string &name)
{
    if (name.empty())
    {
        return false;
    }
    for (const char c : name)
    {
        if (!(std::isalnum(static_cast<unsigned char>(c)) || c == '_'))
        {
            return false;
        }
    }
    return true;
}

std::string quoteIdent(const std::string &name)
{
    return "\"" + name + "\"";
}

void bindJsonValue(drogon::orm::internal::SqlBinder &binder, const Json::Value &value)
{
    if (value.isNull())
    {
        binder << nullptr;
    }
    else if (value.isBool())
    {
        binder << value.asBool();
    }
    else if (value.isInt() || value.isInt64() || value.isUInt() || value.isUInt64() || value.isDouble())
    {
        binder << value.asString();
    }
    else if (value.isString())
    {
        binder << value.asString();
    }
    else
    {
        throw std::runtime_error("Invalid field value type: expected scalar");
    }
}

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

bool isImageType(const std::string &typeStr)
{
    return typeStr == "Image" || typeStr == "ImageWithLink";
}

class ImageSlotsUpdatePlanner : public ITableCellUpdatePlanner
{
public:
    ImageSlotsUpdatePlanner(std::string tableName,
                            std::string imagesTableName,
                            std::string fkColumn,
                            std::string schema)
        : tableName_(std::move(tableName)),
          baseTable_(resolveBaseTable(tableName_)),
          imagesTableName_(std::move(imagesTableName)),
          fkColumn_(std::move(fkColumn)),
          schema_(std::move(schema))
    {
    }

    drogon::Task<std::optional<ValidationError>>
    validate(const CellUpdateController::ParsedRequest &parsed) const override
    {
        ValidationError err;
        const Json::Value &payload = parsed.payload;
        if (!payload.isObject())
        {
            err.code = "bad_request";
            err.message = "Invalid payload: expected object";
            co_return err;
        }
        if (!payload.isMember("table") || !payload["table"].isString())
        {
            err.code = "bad_request";
            err.message = "Invalid payload: missing table";
            co_return err;
        }
        if (!payload.isMember("dbName") || !payload["dbName"].isString())
        {
            err.code = "bad_request";
            err.message = "Invalid payload: missing dbName";
            co_return err;
        }
        const std::string dbName = payload["dbName"].asString();

        const auto rowIdOpt = parseRowId(payload);
        if (!rowIdOpt || *rowIdOpt <= 0)
        {
            err.code = "bad_request";
            err.message = "Invalid payload: missing or invalid rowId";
            co_return err;
        }

        if (!payload.isMember("fields") || !payload["fields"].isObject())
        {
            err.code = "bad_request";
            err.message = "Invalid payload: fields must be object";
            co_return err;
        }
        if (!payload.isMember("types") || !payload["types"].isObject())
        {
            err.code = "bad_request";
            err.message = "Invalid payload: types must be object";
            co_return err;
        }

        const Json::Value &types = payload["types"];
        if (!types.isMember(dbName) || !types[dbName].isString())
        {
            err.code = "bad_request";
            err.message = "Invalid payload: types missing dbName";
            err.details["dbName"] = dbName;
            co_return err;
        }
        const std::string typeStr = types[dbName].asString();

        auto cache = drogon::app().getPlugin<TableInfoCache>();
        if (!cache)
        {
            throw std::runtime_error("TableInfoCache is not initialized");
        }
        const std::string payloadTable = payload["table"].asString();
        auto colsPtr = co_await cache->getColumns(payloadTable);
        if (!colsPtr || !colsPtr->isArray())
        {
            throw std::runtime_error("TableInfoCache returned invalid columns");
        }
        if (colsPtr->empty())
        {
            err.code = "bad_request";
            err.message = "Invalid payload: unknown table or empty schema";
            co_return err;
        }

        std::unordered_map<std::string, bool> allowedColumns;
        for (const auto &c : *colsPtr)
        {
            if (c.isObject() && c.isMember("name") && c["name"].isString())
            {
                allowedColumns.emplace(c["name"].asString(), true);
            }
        }
        allowedColumns.emplace("id", true);

        if (allowedColumns.find(dbName) == allowedColumns.end())
        {
            err.code = "bad_request";
            err.message = "Invalid payload: unknown column";
            err.details["dbName"] = dbName;
            co_return err;
        }

        const Json::Value &fields = payload["fields"];
        const auto fieldKeys = fields.getMemberNames();
        if (!fieldKeys.empty())
        {
            if (fieldKeys.size() != 1 || fieldKeys[0] != dbName)
            {
                err.code = "bad_request";
                err.message = "Invalid payload: fields must contain only dbName";
                err.details["dbName"] = dbName;
                co_return err;
            }
        }
        else if (!isImageType(typeStr) && parsed.attachments.empty())
        {
            err.code = "bad_request";
            err.message = "Invalid payload: empty fields for non-image type";
            err.details["dbName"] = dbName;
            co_return err;
        }

        if (!parsed.attachments.empty())
        {
            if (!isImageType(typeStr))
            {
                err.code = "bad_request";
                err.message = "Invalid payload: attachments only allowed for Image types";
                co_return err;
            }

            std::unordered_map<std::string, bool> roleSeen;
            for (const auto &att : parsed.attachments)
            {
                if (att.dbName != dbName)
                {
                    err.code = "bad_request";
                    err.message = "Invalid attachment dbName: expected payload dbName";
                    err.details["dbName"] = att.dbName;
                    co_return err;
                }
                if (!isSafeIdentifier(att.dbName))
                {
                    err.code = "bad_request";
                    err.message = "Invalid attachment dbName";
                    err.details["dbName"] = att.dbName;
                    co_return err;
                }
                if (att.role != "image" && att.role != "image_small")
                {
                    err.code = "bad_request";
                    err.message = "Invalid attachment role";
                    err.details["role"] = att.role;
                    co_return err;
                }
                if (roleSeen[att.role])
                {
                    err.code = "bad_request";
                    err.message = "Duplicate attachment role for dbName";
                    err.details["dbName"] = att.dbName;
                    err.details["role"] = att.role;
                    co_return err;
                }
                roleSeen[att.role] = true;
            }
        }

        co_return std::nullopt;
    }

    RowWritePlan buildUpdatePlan(int64_t rowId,
                                 const CellUpdateController::ParsedRequest &parsed,
                                 const std::unordered_map<std::string, std::string> &objectKeys,
                                 const MinioClient::Config &minioConfig) const override
    {
        RowWritePlan plan;
        const Json::Value &payload = parsed.payload;
        const std::string payloadTable = payload["table"].asString();
        const std::string payloadBase = resolveBaseTable(payloadTable);
        const std::string dbName = payload["dbName"].asString();

        const Json::Value &fields = payload["fields"];
        if (fields.isObject() && fields.isMember(dbName))
        {
            const Json::Value fieldValue = fields[dbName];
            const bool isChild = (payloadBase != payloadTable);
            int childTypeId = 0;
            if (isChild && !tryGetTableIdByName(payloadTable, childTypeId))
            {
                Json::Value details;
                details["table"] = payloadTable;
                throw CellUpdateError("bad_request",
                                      "Unknown child table",
                                      drogon::k400BadRequest,
                                      details);
            }

            DbOp op;
            op.debugName = "update_cell";
            op.exec = [schema = schema_,
                       table = payloadBase,
                       dbName,
                       rowId,
                       fieldValue,
                       isChild,
                       childTypeId](const std::shared_ptr<drogon::orm::Transaction> &trans) -> drogon::Task<void> {
                if (!isSafeIdentifier(schema) || !isSafeIdentifier(table) || !isSafeIdentifier(dbName))
                {
                    Json::Value details;
                    details["table"] = table;
                    details["dbName"] = dbName;
                    throw CellUpdateError("bad_request",
                                          "Unsafe schema/table/column name",
                                          drogon::k400BadRequest,
                                          details);
                }

                std::string sql = "UPDATE " + quoteIdent(schema) + "." + quoteIdent(table) +
                                  " SET " + quoteIdent(dbName) + " = $1 WHERE id = $2";
                if (isChild)
                {
                    sql += " AND " + quoteIdent(kChildTypeIdColumn) + " = $3";
                }

                auto binder = (*trans << sql);
                bindJsonValue(binder, fieldValue);
                binder << rowId;
                if (isChild)
                {
                    binder << childTypeId;
                }
                const auto result = co_await drogon::orm::internal::SqlAwaiter(std::move(binder));
                if (result.affectedRows() == 0)
                {
                    Json::Value details;
                    details["rowId"] = static_cast<Json::Int64>(rowId);
                    details["dbName"] = dbName;
                    throw CellUpdateError("not_found",
                                          "Row not found for update",
                                          drogon::k404NotFound,
                                          details);
                }
                co_return;
            };
            plan.preUploadDbOps.push_back(std::move(op));
        }

        const Json::Value &types = payload["types"];
        const std::string typeStr = types.isMember(dbName) && types[dbName].isString()
                                        ? types[dbName].asString()
                                        : std::string();
        if (!isImageType(typeStr))
        {
            return plan;
        }

        const Json::Value &meta = payload.isMember("meta") ? payload["meta"] : Json::Value(Json::objectValue);
        const Json::Value &imageMeta = meta.isMember("imageMeta") ? meta["imageMeta"] : Json::Value(Json::objectValue);

        std::vector<const AttachmentInput *> attachments;
        for (const auto &att : parsed.attachments)
        {
            if (att.dbName == dbName)
            {
                attachments.push_back(&att);
            }
        }
        if (!attachments.empty() || (imageMeta.isObject() && imageMeta.isMember(dbName)))
        {
            appendImageSlotPlan(plan,
                                rowId,
                                dbName,
                                attachments,
                                objectKeys,
                                minioConfig.bucket,
                                typeStr == "ImageWithLink" ? imageMeta[dbName] : Json::Value(Json::nullValue));
        }

        return plan;
    }

private:
    void appendImageSlotPlan(RowWritePlan &plan,
                             int64_t rowId,
                             const std::string &dbName,
                             const std::vector<const AttachmentInput *> &attachments,
                             const std::unordered_map<std::string, std::string> &objectKeys,
                             const std::string &bucket,
                             const Json::Value &imageMeta) const
    {
        const AttachmentInput *big = nullptr;
        const AttachmentInput *small = nullptr;
        for (const auto *att : attachments)
        {
            if (att->role == "image")
            {
                big = att;
            }
            else if (att->role == "image_small")
            {
                small = att;
            }
        }

        if (big)
        {
            auto it = objectKeys.find(big->id);
            if (it != objectKeys.end())
            {
                plan.uploads.push_back(UploadOp{big->id, bucket, it->second, big->mimeType});
            }
        }
        if (small)
        {
            auto it = objectKeys.find(small->id);
            if (it != objectKeys.end())
            {
                plan.uploads.push_back(UploadOp{small->id, bucket, it->second, small->mimeType});
            }
        }

        if (!isSafeIdentifier(schema_) || !isSafeIdentifier(imagesTableName_) || !isSafeIdentifier(fkColumn_))
        {
            throw std::runtime_error("Unsafe image table identifier");
        }
        const std::string imagesTable = quoteIdent(schema_) + "." + quoteIdent(imagesTableName_);
        const std::string fkCol = quoteIdent(fkColumn_);
        const std::string sql =
            "INSERT INTO " + imagesTable +
            " (" + fkCol + ", slot, big_bucket, big_object_key, big_mime_type, big_size_bytes, "
            "small_bucket, small_object_key, small_mime_type, small_size_bytes, link_name, link_url) "
            "VALUES ($1,$2,$3,$4,$5,$6,$7,$8,$9,$10,$11,$12) "
            "ON CONFLICT (" + fkCol + ", slot) DO UPDATE SET "
            "big_bucket = COALESCE(EXCLUDED.big_bucket, " + imagesTable + ".big_bucket), "
            "big_object_key = COALESCE(EXCLUDED.big_object_key, " + imagesTable + ".big_object_key), "
            "big_mime_type = COALESCE(EXCLUDED.big_mime_type, " + imagesTable + ".big_mime_type), "
            "big_size_bytes = COALESCE(EXCLUDED.big_size_bytes, " + imagesTable + ".big_size_bytes), "
            "small_bucket = COALESCE(EXCLUDED.small_bucket, " + imagesTable + ".small_bucket), "
            "small_object_key = COALESCE(EXCLUDED.small_object_key, " + imagesTable + ".small_object_key), "
            "small_mime_type = COALESCE(EXCLUDED.small_mime_type, " + imagesTable + ".small_mime_type), "
            "small_size_bytes = COALESCE(EXCLUDED.small_size_bytes, " + imagesTable + ".small_size_bytes), "
            "link_name = COALESCE(EXCLUDED.link_name, " + imagesTable + ".link_name), "
            "link_url = COALESCE(EXCLUDED.link_url, " + imagesTable + ".link_url), "
            "updated_at = now() "
            "RETURNING id";

        DbOp op;
        op.debugName = "upsert_image_slot";
        op.exec = [rowId, dbName, bucket, big, small, objectKeys, imageMeta, sql](
                      const std::shared_ptr<drogon::orm::Transaction> &trans) -> drogon::Task<void> {
            std::string bigObjectKey;
            std::string bigMime;
            int64_t bigSize = 0;
            const bool hasBig = (big != nullptr);
            if (hasBig)
            {
                auto it = objectKeys.find(big->id);
                if (it != objectKeys.end())
                {
                    bigObjectKey = it->second;
                }
                bigMime = big->mimeType;
                bigSize = static_cast<int64_t>(big->data.size());
            }

            std::string smallObjectKey;
            std::string smallMime;
            int64_t smallSize = 0;
            const bool hasSmall = (small != nullptr);
            if (hasSmall)
            {
                auto it = objectKeys.find(small->id);
                if (it != objectKeys.end())
                {
                    smallObjectKey = it->second;
                }
                smallMime = small->mimeType;
                smallSize = static_cast<int64_t>(small->data.size());
            }

            std::string linkName;
            std::string linkUrl;
            if (imageMeta.isObject())
            {
                if (imageMeta.isMember("name") && imageMeta["name"].isString())
                {
                    linkName = imageMeta["name"].asString();
                }
                if (imageMeta.isMember("link") && imageMeta["link"].isString())
                {
                    linkUrl = imageMeta["link"].asString();
                }
            }

            auto binder = (*trans << sql);
            binder << rowId;
            binder << dbName;
            if (hasBig)
                binder << bucket;
            else
                binder << nullptr;
            if (!bigObjectKey.empty())
                binder << bigObjectKey;
            else
                binder << nullptr;
            if (!bigMime.empty())
                binder << bigMime;
            else
                binder << nullptr;
            if (hasBig)
                binder << bigSize;
            else
                binder << nullptr;
            if (hasSmall)
                binder << bucket;
            else
                binder << nullptr;
            if (!smallObjectKey.empty())
                binder << smallObjectKey;
            else
                binder << nullptr;
            if (!smallMime.empty())
                binder << smallMime;
            else
                binder << nullptr;
            if (hasSmall)
                binder << smallSize;
            else
                binder << nullptr;
            if (!linkName.empty())
                binder << linkName;
            else
                binder << nullptr;
            if (!linkUrl.empty())
                binder << linkUrl;
            else
                binder << nullptr;

            (void)co_await drogon::orm::internal::SqlAwaiter(std::move(binder));
            co_return;
        };
        plan.postUploadDbOps.push_back(std::move(op));
    }

private:
    std::string tableName_;
    std::string baseTable_;
    std::string imagesTableName_;
    std::string fkColumn_;
    std::string schema_;
};
} // namespace

void CellUpdatePlannerRegistry::registerPlanner(const std::string &tableName,
                                                std::shared_ptr<ITableCellUpdatePlanner> planner)
{
    planners_[tableName] = std::move(planner);
}

std::shared_ptr<ITableCellUpdatePlanner> CellUpdatePlannerRegistry::getPlanner(const std::string &tableName) const
{
    auto it = planners_.find(tableName);
    if (it == planners_.end())
    {
        const std::string baseTable = resolveBaseTable(tableName);
        if (baseTable != tableName)
        {
            auto itBase = planners_.find(baseTable);
            if (itBase != planners_.end())
            {
                return itBase->second;
            }
        }
        return nullptr;
    }
    return it->second;
}

std::shared_ptr<CellUpdatePlannerRegistry> createDefaultCellUpdatePlannerRegistry()
{
    auto registry = std::make_shared<CellUpdatePlannerRegistry>();
    const std::string &defaultTableName = kTableNames.at(kDefaultTableId);
    registry->registerPlanner(defaultTableName,
                              std::make_shared<ImageSlotsUpdatePlanner>(
                                  defaultTableName,
                                  kTableMinioBySlot.at(defaultTableName),
                                  "tool_id",
                                  "public"));
    return registry;
}
