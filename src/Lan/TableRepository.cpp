#include "Lan/TableRepository.h"

#include <sstream>

namespace
{
std::string quoteIdent(const std::string &ident)
{
    std::string out;
    out.reserve(ident.size() + 2);
    out.push_back('"');
    for (char c : ident)
    {
        if (c == '"')
            out += "\"\"";
        else
            out.push_back(c);
    }
    out.push_back('"');
    return out;
}
} // namespace

drogon::Task<int64_t> TableRepository::countRows(const std::string &schema,
                                                const std::string &tableName,
                                                const std::string &whereSql) const
{
    using namespace drogon;
    using namespace drogon::orm;

    auto dbClient = app().getDbClient(dbClientName_);
    std::ostringstream sql;
    sql << "SELECT COUNT(*) AS cnt FROM " << quoteIdent(schema) << "." << quoteIdent(tableName) << " ";
    if (!whereSql.empty())
        sql << whereSql;

    auto rows = co_await dbClient->execSqlCoro(sql.str());
    if (rows.empty())
        co_return 0;
    co_return rows[0]["cnt"].as<int64_t>();
}

drogon::Task<drogon::orm::Result> TableRepository::selectPage(const std::string &schema,
                                                              const std::string &tableName,
                                                              const std::string &whereSql,
                                                              int offset,
                                                              int limit) const
{
    using namespace drogon;
    using namespace drogon::orm;

    auto dbClient = app().getDbClient(dbClientName_);
    std::ostringstream sql;
    sql << "SELECT * FROM " << quoteIdent(schema) << "." << quoteIdent(tableName) << " ";
    if (!whereSql.empty())
        sql << whereSql << " ";
    sql << "ORDER BY " << quoteIdent("id") << " ASC "
        << "LIMIT " << limit << " OFFSET " << offset;

    auto rows = co_await dbClient->execSqlCoro(sql.str());
    co_return rows;
}

drogon::Task<drogon::orm::Result> TableRepository::selectById(const std::string &schema,
                                                              const std::string &tableName,
                                                              int64_t id) const
{
    using namespace drogon;
    using namespace drogon::orm;

    // Заготовка: тут уже используем bind-параметр (фиксированное число аргументов).
    auto dbClient = app().getDbClient(dbClientName_);
    std::ostringstream sql;
    sql << "SELECT * FROM " << quoteIdent(schema) << "." << quoteIdent(tableName)
        << " WHERE " << quoteIdent("id") << " = $1";
    auto rows = co_await dbClient->execSqlCoro(sql.str(), id);
    co_return rows;
}

