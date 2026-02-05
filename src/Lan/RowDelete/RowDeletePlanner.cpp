#include "Lan/RowDelete/RowDeletePlanner.h"
#include "Lan/allTableList.h"

#include <drogon/drogon.h>

#include <cctype>
#include <stdexcept>
#include <string>
#include <unordered_set>

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

class ImageSlotsDeletePlanner : public ITableRowDeletePlanner
{
public:
    /// Планировщик под таблицы со схемой "images by slot".
    /// Расширение:
    /// - Для другой таблицы с такой же схемой достаточно зарегистрировать ещё один ImageSlotsDeletePlanner
    ///   в createDefaultRowDeletePlannerRegistry(), указав свои table/images/fk/schema.
    ImageSlotsDeletePlanner(std::string tableName,
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

    drogon::Task<std::optional<DeleteValidationError>> validate(const RowDeleteRequest &request) const override
    {
        DeleteValidationError err;
        if (request.table.empty())
        {
            err.code = "bad_request";
            err.message = "Invalid request: missing table";
            co_return err;
        }
        if (request.rowId <= 0)
        {
            err.code = "bad_request";
            err.message = "Invalid request: rowId must be positive";
            co_return err;
        }

        const std::string payloadBaseTable = resolveBaseTable(request.table);
        if (payloadBaseTable != baseTable_)
        {
            err.code = "bad_request";
            err.message = "Invalid request: unexpected table";
            co_return err;
        }

        co_return std::nullopt;
    }

    drogon::Task<RowDeletePlan> buildDeletePlan(const RowDeleteRequest &request,
                                                const std::shared_ptr<drogon::orm::Transaction> &trans,
                                                const MinioClient::Config &minioConfig) const override
    {
        RowDeletePlan plan;
        if (!isSafeIdentifier(schema_) || !isSafeIdentifier(imagesTableName_) || !isSafeIdentifier(fkColumn_))
        {
            throw std::runtime_error("Unsafe image table identifier");
        }
        if (!isSafeIdentifier(baseTable_))
        {
            throw std::runtime_error("Unsafe base table identifier");
        }

        const std::string imagesTable = quoteIdent(schema_) + "." + quoteIdent(imagesTableName_);
        const std::string fkCol = quoteIdent(fkColumn_);
        const std::string sqlSelect =
            "SELECT big_bucket, big_object_key, small_bucket, small_object_key FROM " + imagesTable +
            " WHERE " + fkCol + " = $1";

        auto binder = (*trans << sqlSelect);
        binder << request.rowId;
        const auto result = co_await drogon::orm::internal::SqlAwaiter(std::move(binder));

        std::unordered_set<std::string> seen;
        auto addStorageOp = [&](const drogon::orm::Field &bucketField, const drogon::orm::Field &keyField)
        {
            if (keyField.isNull())
            {
                return;
            }
            const std::string objectKey = keyField.as<std::string>();
            if (objectKey.empty())
            {
                return;
            }
            std::string bucket = minioConfig.bucket;
            if (!bucketField.isNull())
            {
                bucket = bucketField.as<std::string>();
            }
            const std::string uniq = bucket + "/" + objectKey;
            if (seen.insert(uniq).second)
            {
                plan.storageDeletes.push_back(RowDeleteStorageOp{bucket, objectKey});
            }
        };

        for (const auto &row : result)
        {
            addStorageOp(row["big_bucket"], row["big_object_key"]);
            addStorageOp(row["small_bucket"], row["small_object_key"]);
        }

        const std::string sqlDeleteImages =
            "DELETE FROM " + imagesTable + " WHERE " + fkCol + " = $1";
        RowDeleteDbOp deleteImagesOp;
        deleteImagesOp.debugName = "delete_images_by_fk";
        deleteImagesOp.exec = [rowId = request.rowId, sqlDeleteImages](const std::shared_ptr<drogon::orm::Transaction> &trans) -> drogon::Task<void> {
            auto binder = (*trans << sqlDeleteImages);
            binder << rowId;
            (void)co_await drogon::orm::internal::SqlAwaiter(std::move(binder));
            co_return;
        };
        plan.dbOps.push_back(std::move(deleteImagesOp));

        const std::string baseTable = quoteIdent(schema_) + "." + quoteIdent(baseTable_);
        const std::string sqlDeleteRow =
            "DELETE FROM " + baseTable + " WHERE id = $1";
        RowDeleteDbOp deleteRowOp;
        deleteRowOp.debugName = "delete_base_row";
        deleteRowOp.exec = [rowId = request.rowId, sqlDeleteRow](const std::shared_ptr<drogon::orm::Transaction> &trans) -> drogon::Task<void> {
            auto binder = (*trans << sqlDeleteRow);
            binder << rowId;
            (void)co_await drogon::orm::internal::SqlAwaiter(std::move(binder));
            co_return;
        };
        plan.dbOps.push_back(std::move(deleteRowOp));

        co_return plan;
    }

private:
    std::string tableName_;
    std::string baseTable_;
    std::string imagesTableName_;
    std::string fkColumn_;
    std::string schema_;
};
} // namespace

void RowDeletePlannerRegistry::registerPlanner(const std::string &tableName,
                                               std::shared_ptr<ITableRowDeletePlanner> planner)
{
    planners_[tableName] = std::move(planner);
}

std::shared_ptr<ITableRowDeletePlanner> RowDeletePlannerRegistry::getPlanner(const std::string &tableName) const
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

std::shared_ptr<RowDeletePlannerRegistry> createDefaultRowDeletePlannerRegistry()
{
    // Реестр расширения:
    // - Добавляйте сюда новые таблицы/планировщики.
    // - Если таблица использует ту же схему images-by-slot, используйте ImageSlotsDeletePlanner.
    auto registry = std::make_shared<RowDeletePlannerRegistry>();
    const std::string &defaultTableName = kTableNames.at(kDefaultTableId);
    registry->registerPlanner(defaultTableName,
                              std::make_shared<ImageSlotsDeletePlanner>(
                                  defaultTableName,
                                  kTableMinioBySlot.at(defaultTableName),
                                  "tool_id",
                                  "public"));
    return registry;
}
