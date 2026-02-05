#include "Lan/RowDelete/RowDeleteService.h"

#include <drogon/drogon.h>

#include "Storage/MinioPlugin.h"
#include "Loger/Logger.h"

#include <sstream>
#include <stdexcept>

RowDeleteService::RowDeleteService()
    : registry_(createDefaultRowDeletePlannerRegistry())
{
}

drogon::Task<DeleteResult> RowDeleteService::deleteRow(const RowDeleteRequest &request)
{
    if (request.table.empty())
    {
        Logger::instance().error("RowDeleteError: missing table");
        throw RowDeleteError("bad_request", "Invalid request: missing table", drogon::k400BadRequest);
    }
    if (request.rowId <= 0)
    {
        Logger::instance().error("RowDeleteError: invalid rowId");
        throw RowDeleteError("bad_request", "Invalid request: rowId must be positive", drogon::k400BadRequest);
    }

    auto planner = registry_->getPlanner(request.table);
    if (!planner)
    {
        Json::Value details;
        details["table"] = request.table;
        Logger::instance().error("RowDeleteError: table is not supported");
        throw RowDeleteError("bad_request", "Table is not supported", drogon::k400BadRequest, details);
    }

    if (auto validationErr = co_await planner->validate(request))
    {
        std::ostringstream oss;
        oss << "RowDeleteError: validation failed"
            << " code=" << validationErr->code
            << " status=" << static_cast<int>(validationErr->status)
            << " message=" << validationErr->message;
        Logger::instance().error(oss.str());
        throw RowDeleteError(validationErr->code,
                             validationErr->message,
                             validationErr->status,
                             validationErr->details);
    }

    auto dbClient = drogon::app().getDbClient("default");
    auto trans = co_await dbClient->newTransactionCoro();
    auto minioPlugin = drogon::app().getPlugin<MinioPlugin>();
    if (!minioPlugin)
    {
        Logger::instance().error("RowDeleteError: MinioPlugin is not initialized");
        throw RowDeleteError("internal", "MinioPlugin is not initialized", drogon::k500InternalServerError);
    }
    MinioClient &minioClient = minioPlugin->client();

    RowDeletePlan plan;
    try
    {
        plan = co_await planner->buildDeletePlan(request, trans, minioPlugin->minioConfig());
        for (const auto &op : plan.dbOps)
        {
            co_await op.exec(trans);
        }
    }
    catch (const RowDeleteError &)
    {
        if (trans)
        {
            trans->rollback();
        }
        throw;
    }
    catch (const std::exception &e)
    {
        if (trans)
        {
            trans->rollback();
        }
        Logger::instance().error(std::string("RowDeleteError: delete failed: ") + e.what());
        throw RowDeleteError("internal", std::string("Row delete failed: ") + e.what(), drogon::k500InternalServerError);
    }

    Json::Value warnings(Json::arrayValue);
    if (!plan.warnings.isNull())
    {
        if (plan.warnings.isArray())
        {
            for (const auto &w : plan.warnings)
            {
                warnings.append(w);
            }
        }
        else
        {
            warnings.append(plan.warnings);
        }
    }

    for (const auto &op : plan.storageDeletes)
    {
        const bool ok = minioClient.deleteObject(op.bucket, op.objectKey);
        if (!ok)
        {
            Json::Value warning(Json::objectValue);
            warning["bucket"] = op.bucket;
            warning["objectKey"] = op.objectKey;
            warnings.append(warning);
            Logger::instance().error("RowDeleteWarning: MinIO delete failed bucket=" + op.bucket + " key=" + op.objectKey);
        }
    }

    DeleteResult result;
    result.rowId = request.rowId;
    if (!warnings.empty())
    {
        result.warnings = warnings;
    }
    co_return result;
}
