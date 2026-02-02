#include "Lan/TableDataService.h"

#include "Lan/TableQueryBuilder.h"
#include "Lan/TableRepository.h"
#include "Lan/ServiceErrors.h"
#include "TableInfoCache.h"
#include "Lan/allTableList.h"
#include "Loger/Logger.h"

#include <drogon/orm/Exception.h>

#include <unordered_set>

namespace
{
constexpr int kDefaultLimit = 20;
constexpr int kMaxLimit = 200;
constexpr Json::ArrayIndex kMaxFilters = 100;

Json::Value fieldToJson(const drogon::orm::Field &f, const std::string &dataType)
{
    if (f.isNull())
        return Json::nullValue;

    try
    {
        if (dataType == "integer")
            return Json::Value(f.as<int32_t>());
        if (dataType == "bigint")
            return Json::Value(static_cast<Json::Int64>(f.as<int64_t>()));
        if (dataType == "boolean")
            return Json::Value(f.as<bool>());

        // numeric/real/double precision
        if (dataType == "numeric" || dataType == "real" || dataType == "double precision")
            return Json::Value(f.as<double>());

        // text/varchar/etc
        return Json::Value(f.as<std::string>());
    }
    catch (...)
    {
        // Фоллбек: как строка (на случай экзотических типов, чтобы не 500-ить).
        try
        {
            return Json::Value(f.as<std::string>());
        }
        catch (...)
        {
            return Json::nullValue;
        }
    }
}
} // namespace

TableDataService::TableDataService()
    : repo_(std::make_shared<TableRepository>("default"))
{
}

drogon::Task<TableDataService::PageResult>
TableDataService::getPage(const std::string &tableName,
                          const Json::Value &filters,
                          int offset,
                          int limit) const
{
    using namespace drogon;
    using namespace drogon::orm;

    PageResult out;
    out.offset = offset < 0 ? 0 : offset;
    out.limit = limit <= 0 ? kDefaultLimit : limit;
    if (out.limit > kMaxLimit)
        out.limit = kMaxLimit;

    // filters может быть пустым массивом или Json::nullValue (контроллер шлёт arrayValue).
    if (!filters.isNull())
    {
        if (!filters.isArray())
            throw BadRequestError("filters must be array");
        if (filters.size() > kMaxFilters)
            throw BadRequestError("too many filters");
    }

    auto cache = app().getPlugin<TableInfoCache>();
    if (!cache)
        throw std::runtime_error("TableInfoCache is not initialized");

    const std::string baseTable = resolveBaseTable(tableName);

    // Колонки + whitelist
    auto colsPtr = co_await cache->getColumns(tableName);
    const Json::Value &cols = *colsPtr;

    std::unordered_set<std::string> allowedColumns;
    allowedColumns.reserve(cols.size());
    for (const auto &c : cols)
    {
        if (c.isObject() && c.isMember("name") && c["name"].isString())
            allowedColumns.insert(c["name"].asString());
    }

    // WHERE
    std::string whereSql;
    if (!filters.isNull() && filters.isArray() && !filters.empty())
    {
        whereSql = TableQueryBuilder::buildWhere(filters, allowedColumns);
    }

    try
    {
        out.total = co_await repo_->countRows(schema_, baseTable, whereSql);
        auto result = co_await repo_->selectPage(schema_, baseTable, whereSql, out.offset, out.limit);

        Json::Value rows(Json::arrayValue);
        rows.resize(0);

        for (const auto &r : result)
        {
            Json::Value obj(Json::objectValue);
            for (const auto &c : cols)
            {
                if (!c.isObject() || !c.isMember("name") || !c["name"].isString())
                    continue;
                const std::string name = c["name"].asString();
                const std::string type = c.get("type", "text").asString();

                const auto &field = r[name];
                obj[name] = fieldToJson(field, type);
            }
            rows.append(std::move(obj));
        }

        out.rows = std::move(rows);
        co_return out;
    }
    catch (const DrogonDbException &e)
    {
        LOG_ERROR(std::string("TableDataService DB error: ") + e.base().what());
        throw;
    }
}

drogon::Task<Json::Value> TableDataService::getById(const std::string &tableName, int64_t id) const
{
    (void)tableName;
    (void)id;
    Json::Value notImplemented;
    notImplemented["ok"] = false;
    notImplemented["error"] = "not implemented";
    co_return notImplemented;
}

