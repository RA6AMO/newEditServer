#include "Lan/RowAdd/RowWritePlanner.h"
#include "Lan/allTableList.h"
#include "TableInfoCache.h"

#include <drogon/drogon.h>
#include <algorithm>
#include <cctype>
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

struct SqlCommand
{
    std::string sql;
    std::vector<std::function<void(drogon::orm::internal::SqlBinder &)>> binders;
};

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
        // Передаем числа как строки. PostgreSQL сам сконвертирует их в нужный тип 
        // (int4, int8, numeric и т.д.) на стороне сервера. Это позволяет избежать 
        // ошибок бинарного формата ("incorrect binary data format"), которые возникают 
        // при несовпадении размера типа в C++ и в БД (например, int64_t vs INTEGER/int4).
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

SqlCommand buildInsertCommand(const Json::Value &payload,
                              const std::string &schema,
                              const std::string &table)
{
    if (!isSafeIdentifier(schema) || !isSafeIdentifier(table))
    {
        throw std::runtime_error("Unsafe schema/table name");
    }

    const Json::Value &fields = payload["fields"];
    if (!fields.isObject())
    {
        throw std::runtime_error("Invalid payload: fields must be object");
    }

    std::vector<std::string> columns;
    std::vector<std::function<void(drogon::orm::internal::SqlBinder &)>> binders;
    const auto names = fields.getMemberNames();
    for (const auto &name : names)
    {
        if (name == "id")
        {
            continue;
        }
        if (!isSafeIdentifier(name))
        {
            throw std::runtime_error("Unsafe column name: " + name);
        }
        columns.push_back(name);
        Json::Value v = fields[name];
        binders.push_back([v](drogon::orm::internal::SqlBinder &binder) { bindJsonValue(binder, v); });
    }

    SqlCommand cmd;
    if (columns.empty())
    {
        cmd.sql = "INSERT INTO " + quoteIdent(schema) + "." + quoteIdent(table) + " DEFAULT VALUES RETURNING id";
        return cmd;
    }

    std::string colsSql;
    std::string valsSql;
    for (size_t i = 0; i < columns.size(); ++i)
    {
        if (i > 0)
        {
            colsSql += ", ";
            valsSql += ", ";
        }
        colsSql += quoteIdent(columns[i]);
        valsSql += "$" + std::to_string(i + 1);
    }
    cmd.sql = "INSERT INTO " + quoteIdent(schema) + "." + quoteIdent(table) + " (" + colsSql +
              ") VALUES (" + valsSql + ") RETURNING id";
    cmd.binders = std::move(binders);



    
    return cmd;
}

class ImageSlotsPlanner : public ITableRowWritePlanner
{
public:
    /// Планировщик под таблицы со схемой "images by slot".
    /// Расширение:
    /// - Для другой таблицы с такой же схемой достаточно зарегистрировать ещё один ImageSlotsPlanner
    ///   в createDefaultRowWritePlannerRegistry(), указав свои table/images/fk/schema.
    /// - Для новых типов файлов (не image) создавайте отдельный planner и регистрируйте его рядом.
    ImageSlotsPlanner(std::string tableName,
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

    drogon::Task<std::optional<ValidationError>> validate(const RowController::ParsedRequest &parsed) const override
    {
        // Валидация расширяемости:
        // - Здесь фиксируем допустимые роли для image ("image", "image_small").
        // - Здесь проверяем, что dbName начинается с "image_" и реально существует в таблице.
        // - Здесь проверяем типы ("Image"/"ImageWithLink"), чтобы не принимать чужие файлы.
        // - Здесь проверяем структуру fields/types и разрешённые колонки.
        ValidationError err;
        const Json::Value &payload = parsed.payload;
        if (!payload.isObject() || !payload.isMember("table") || !payload["table"].isString())
        {
            err.code = "bad_request";
            err.message = "Invalid payload: missing table";
            co_return err;
        }

        const std::string payloadTable = payload["table"].asString();
        const std::string payloadBaseTable = resolveBaseTable(payloadTable);
        if (payloadBaseTable != baseTable_)
        {
            err.code = "bad_request";
            err.message = "Invalid payload: unexpected table";
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

        auto cache = drogon::app().getPlugin<TableInfoCache>();
        if (!cache)
        {
            throw std::runtime_error("TableInfoCache is not initialized");
        }
        auto colsPtr = co_await cache->getColumns(payloadTable);
        if (!colsPtr)
        {
            throw std::runtime_error("TableInfoCache returned null columns pointer");
        }
        if (!colsPtr->isArray())
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

        auto validateObjectKeys = [&](const char *fieldName)
        {
            const Json::Value &obj = payload[fieldName];
            const auto names = obj.getMemberNames();
            for (const auto &k : names)
            {
                if (allowedColumns.find(k) == allowedColumns.end())
                {
                    err.code = "bad_request";
                    err.message = std::string("Invalid payload: unknown column in '") + fieldName + "': " + k;
                    return false;
                }
            }
            return true;
        };

        if (!validateObjectKeys("fields"))
        {
            co_return err;
        }
        if (!validateObjectKeys("types"))
        {
            co_return err;
        }

        const Json::Value &fields = payload["fields"];
        const Json::Value &types = payload["types"];
        const auto fieldKeys = fields.getMemberNames();
        for (const auto &k : fieldKeys)
        {
            if (k == "id")
            {
                continue;
            }
            if (!types.isMember(k))
            {
                err.code = "bad_request";
                err.message = std::string("Invalid payload: types missing key for field: ") + k;
                co_return err;
            }
        }

        std::unordered_map<std::string, std::unordered_map<std::string, bool>> roleSeen;
        for (const auto &att : parsed.attachments)
        {
            if (!isSafeIdentifier(att.dbName))
            {
                err.code = "bad_request";
                err.message = "Invalid attachment dbName";
                err.details["dbName"] = att.dbName;
                co_return err;
            }
            if (att.dbName.rfind("image_", 0) != 0)
            {
                err.code = "bad_request";
                err.message = "Invalid attachment dbName: expected image_*";
                err.details["dbName"] = att.dbName;
                co_return err;
            }
            if (allowedColumns.find(att.dbName) == allowedColumns.end())
            {
                err.code = "bad_request";
                err.message = "Invalid attachment dbName: column not found";
                err.details["dbName"] = att.dbName;
                co_return err;
            }

            if (!types.isMember(att.dbName) || !types[att.dbName].isString())
            {
                err.code = "bad_request";
                err.message = "Invalid payload: types missing dbName for attachment";
                err.details["dbName"] = att.dbName;
                co_return err;
            }
            const std::string typeStr = types[att.dbName].asString();
            if (typeStr != "Image" && typeStr != "ImageWithLink")
            {
                err.code = "bad_request";
                err.message = "Invalid attachment type for dbName";
                err.details["dbName"] = att.dbName;
                err.details["type"] = typeStr;
                co_return err;
            }
            if (att.role != "image" && att.role != "image_small")
            {
                err.code = "bad_request";
                err.message = "Invalid attachment role";
                err.details["role"] = att.role;
                co_return err;
            }
            if (roleSeen[att.dbName][att.role])
            {
                err.code = "bad_request";
                err.message = "Duplicate attachment role for dbName";
                err.details["dbName"] = att.dbName;
                err.details["role"] = att.role;
                co_return err;
            }
            roleSeen[att.dbName][att.role] = true;
        }

        co_return std::nullopt;
    }

    drogon::Task<int64_t> insertBaseRow(const RowController::ParsedRequest &parsed,
                                        const std::shared_ptr<drogon::orm::Transaction> &trans) const override
    {
        // Универсальная вставка базовой строки:
        // - Берём payload.fields как набор колонок.
        // - Все значения параметризованы.
        // - Возвращаем rowId.
        Json::Value payload = parsed.payload;
        if (payload.isMember("fields") && payload["fields"].isObject())
        {
            Json::Value &fields = payload["fields"];
            if (!fields.isMember(kChildTypeIdColumn) || fields[kChildTypeIdColumn].isNull())
            {
                int tableId = 0;
                const std::string tableForType = payload.isMember("table") && payload["table"].isString()
                                                     ? payload["table"].asString()
                                                     : tableName_;
                if (tryGetTableIdByName(tableForType, tableId))
                {
                    fields[kChildTypeIdColumn] = static_cast<Json::Int64>(tableId);
                }
            }
        }

        SqlCommand cmd = buildInsertCommand(payload, schema_, baseTable_);
        auto binder = (*trans << cmd.sql);
        for (const auto &bind : cmd.binders)
        {
            bind(binder);
        }
        const auto result = co_await drogon::orm::internal::SqlAwaiter(std::move(binder));
        if (result.empty())
        {
            throw std::runtime_error("Insert did not return id");
        }
        const auto idField = result[0]["id"];
        co_return idField.as<int64_t>();
    }

    RowWritePlan buildWritePlan(int64_t rowId,
                                const RowController::ParsedRequest &parsed,
                                const std::unordered_map<std::string, std::string> &objectKeys,
                                const MinioClient::Config &minioConfig) const override
    {
        // План записи расширяем так:
        // - Для каждой image-колонки в attachments формируем UploadOp.
        // - После upload формируем UPSERT в *images по (tool_id, slot).
        // - Контроллер ничего об этом не знает.
        RowWritePlan plan;
        const Json::Value &payload = parsed.payload;
        const Json::Value &types = payload["types"];
        const Json::Value &meta = payload.isMember("meta") ? payload["meta"] : Json::Value(Json::objectValue);
        const Json::Value &imageMeta = meta.isMember("imageMeta") ? meta["imageMeta"] : Json::Value(Json::objectValue);

        std::unordered_map<std::string, std::vector<const AttachmentInput *>> byDbName;
        for (const auto &att : parsed.attachments)
        {
            byDbName[att.dbName].push_back(&att);
        }

        for (const auto &kv : byDbName)
        {
            const std::string &dbName = kv.first;
            if (!types.isMember(dbName) || !types[dbName].isString())
            {
                continue;
            }
            const std::string typeStr = types[dbName].asString();
            if (typeStr != "Image" && typeStr != "ImageWithLink")
            {
                continue;
            }
            appendImageSlotPlan(plan,
                                rowId,
                                dbName,
                                kv.second,
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
        // Конкретная стратегия image-слота:
        // - поддерживает роли "image"/"image_small"
        // - формирует UploadOp для каждого файла
        // - делает UPSERT, обновляя только переданные big/small поля
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
        op.exec = [rowId, dbName, bucket, big, small, objectKeys, imageMeta, sql](const std::shared_ptr<drogon::orm::Transaction> &trans) -> drogon::Task<void> {
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

void RowWritePlannerRegistry::registerPlanner(const std::string &tableName,
                                              std::shared_ptr<ITableRowWritePlanner> planner)
{
    planners_[tableName] = std::move(planner);
}

std::shared_ptr<ITableRowWritePlanner> RowWritePlannerRegistry::getPlanner(const std::string &tableName) const
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

std::shared_ptr<RowWritePlannerRegistry> createDefaultRowWritePlannerRegistry()
{
    // Реестр расширения:
    // - Добавляйте сюда новые таблицы/планировщики.
    // - Если таблица использует ту же схему images-by-slot, используйте ImageSlotsPlanner.
    // - Если появляется новый тип вложений, создайте свой planner (например FileSlotsPlanner)
    //   и зарегистрируйте его здесь.
    auto registry = std::make_shared<RowWritePlannerRegistry>();
    const std::string &defaultTableName = kTableNames.at(kDefaultTableId);
    registry->registerPlanner(defaultTableName,
                              std::make_shared<ImageSlotsPlanner>(
                                    defaultTableName,
                                    kTableMinioBySlot.at(defaultTableName),
                                    "tool_id",
                                    "public"));
    return registry;
}

