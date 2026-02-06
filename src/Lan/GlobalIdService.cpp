#include "Lan/GlobalIdService.h"

#include "Lan/ServiceErrors.h"
#include "Lan/allTableList.h"
#include "Loger/Logger.h"

#include <drogon/drogon.h>
#include <drogon/orm/Exception.h>

#include <sstream>
#include <unordered_set>

drogon::Task<std::unordered_map<int64_t, int64_t>>
GlobalIdService::getGlobalIdsByLocalIds(const std::string &tableName,
                                        const std::vector<int64_t> &localIds) const
{
    std::unordered_map<int64_t, int64_t> out;
    if (localIds.empty())
    {
        co_return out;
    }

    const std::string baseTable = resolveBaseTable(tableName);
    std::string objectType;
    if (!tryGetObjectTypeByTableName(baseTable, objectType))
    {
        throw BadRequestError("unknown object type for table");
    }

    std::unordered_set<int64_t> uniqueIds(localIds.begin(), localIds.end());
    if (uniqueIds.empty())
    {
        co_return out;
    }

    std::ostringstream sql;
    sql << "SELECT object_id, global_id FROM public.global_object_registry "
        << "WHERE object_type = $1 AND object_id IN (";

    bool first = true;
    for (const auto &id : uniqueIds)
    {
        if (!first)
            sql << ",";
        first = false;
        sql << id;
    }
    sql << ")";

    try
    {
        auto dbClient = drogon::app().getDbClient("default");
        auto rows = co_await dbClient->execSqlCoro(sql.str(), objectType);
        for (const auto &r : rows)
        {
            const int64_t localId = r["object_id"].as<int64_t>();
            const int64_t globalId = r["global_id"].as<int64_t>();
            out[localId] = globalId;
        }
    }
    catch (const drogon::orm::DrogonDbException &e)
    {
        LOG_ERROR(std::string("GlobalIdService DB error: ") + e.base().what());
        throw;
    }

    co_return out;
}
