#include "TableInfoCache.h"
#include "Lan/allTableList.h"

#include <drogon/drogon.h>
#include <drogon/orm/DbClient.h>
#include <drogon/orm/Exception.h>
#include <unordered_set>

void TableInfoCache::initAndStart(const Json::Value &config)
{
    if (config.isObject() && !config.empty())
    {
        if (config.isMember("schema") && config["schema"].isString())
        {
            schema_ = config["schema"].asString();
        }
        if (config.isMember("db_client") && config["db_client"].isString())
        {
            dbClientName_ = config["db_client"].asString();
        }
    }
}

void TableInfoCache::shutdown()
{
    clear();
}

drogon::Task<std::shared_ptr<const Json::Value>>
TableInfoCache::getColumns(const std::string &tableName)
{
    // 1) Быстрый hit под shared_lock
    {
        std::shared_lock lk(mu_);
        auto it = columnsByTable_.find(tableName);
        if (it != columnsByTable_.end())
        {
            co_return it->second;
        }
    }

    std::string baseTable;
    std::vector<std::string> exclude;
    const bool isChild = resolveChildChain(tableName, baseTable, exclude);

    auto loadFromDb = [&](const std::string &dbTable) -> drogon::Task<std::shared_ptr<const Json::Value>>
    {
        auto dbClient = drogon::app().getDbClient(dbClientName_);
        auto rows = co_await dbClient->execSqlCoro(
            "SELECT "
            "  ordinal_position, "
            "  column_name, "
            "  data_type, "
            "  udt_name, "
            "  numeric_precision, "
            "  numeric_scale "
            "FROM information_schema.columns "
            "WHERE table_schema = $1 "
            "  AND table_name   = $2 "
            "ORDER BY ordinal_position",
            schema_,
            dbTable);

        Json::Value columns(Json::arrayValue);
        for (const auto &r : rows)
        {
            Json::Value col;
            col["name"] = r["column_name"].as<std::string>();
            col["type"] = r["data_type"].as<std::string>();
            col["udt_name"] = r["udt_name"].as<std::string>();

            if (!r["numeric_precision"].isNull())
            {
                col["numeric_precision"] = r["numeric_precision"].as<int>();
            }
            if (!r["numeric_scale"].isNull())
            {
                col["numeric_scale"] = r["numeric_scale"].as<int>();
            }

            columns.append(std::move(col));
        }

        auto computedPtr = std::shared_ptr<const Json::Value>(
            std::make_shared<Json::Value>(std::move(columns)));
        co_return computedPtr;
    };

    std::shared_ptr<const Json::Value> basePtr;
    {
        std::shared_lock lk(mu_);
        auto it = columnsByTable_.find(baseTable);
        if (it != columnsByTable_.end())
        {
            basePtr = it->second;
        }
    }

    if (!basePtr)
    {
        auto computedPtr = co_await loadFromDb(baseTable);
        {
            std::unique_lock lk(mu_);
            auto [it, inserted] = columnsByTable_.emplace(baseTable, computedPtr);
            if (!inserted)
            {
                basePtr = it->second;
            }
            else
            {
                basePtr = std::move(computedPtr);
            }
        }
    }

    if (!isChild)
    {
        co_return basePtr;
    }

    std::unordered_set<std::string> excluded(exclude.begin(), exclude.end());
    Json::Value filtered(Json::arrayValue);
    for (const auto &c : *basePtr)
    {
        if (!c.isObject() || !c.isMember("name") || !c["name"].isString())
        {
            continue;
        }
        const std::string name = c["name"].asString();
        if (name == "id")
        {
            filtered.append(c);
            continue;
        }
        if (excluded.find(name) != excluded.end())
        {
            continue;
        }
        filtered.append(c);
    }

    auto filteredPtr = std::shared_ptr<const Json::Value>(
        std::make_shared<Json::Value>(std::move(filtered)));

    {
        std::unique_lock lk(mu_);
        auto [it, inserted] = columnsByTable_.emplace(tableName, filteredPtr);
        if (!inserted)
        {
            co_return it->second;
        }
    }

    co_return filteredPtr;
}

void TableInfoCache::invalidate(const std::string &tableName)
{
    std::unique_lock lk(mu_);
    columnsByTable_.erase(tableName);
}

void TableInfoCache::clear()
{
    std::unique_lock lk(mu_);
    columnsByTable_.clear();
}

