#pragma once

#include <drogon/drogon.h>
#include <drogon/orm/DbClient.h>

#include <cstdint>
#include <string>

/// Низкоуровневый слой доступа к БД для табличных выборок (COUNT + SELECT page).
/// Не знает про HTTP и не парсит filters: принимает готовые SQL-фрагменты.
class TableRepository
{
public:
    explicit TableRepository(std::string dbClientName = "default")
        : dbClientName_(std::move(dbClientName))
    {
    }

    drogon::Task<int64_t> countRows(const std::string &schema,
                                   const std::string &tableName,
                                   const std::string &whereSql) const;

    drogon::Task<drogon::orm::Result> selectPage(const std::string &schema,
                                                 const std::string &tableName,
                                                 const std::string &whereSql,
                                                 int offset,
                                                 int limit) const;

    // Заготовка под будущее: получить одну строку по id.
    drogon::Task<drogon::orm::Result> selectById(const std::string &schema,
                                                 const std::string &tableName,
                                                 int64_t id) const;

private:
    std::string dbClientName_;
};

