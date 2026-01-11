#pragma once

#include <drogon/drogon.h>
#include <json/json.h>

#include <cstdint>
#include <string>

class TableRepository;

/// Бизнес-логика выдачи табличных данных (list/page) без MinIO.
class TableDataService
{
public:
    struct PageResult
    {
        int64_t total{0};
        int offset{0};
        int limit{20};
        Json::Value rows{Json::arrayValue};
    };

    TableDataService();

    drogon::Task<PageResult> getPage(const std::string &tableName,
                                     const Json::Value &filters,
                                     int offset,
                                     int limit) const;

    // Заготовка под будущее (не используем сейчас).
    drogon::Task<Json::Value> getById(const std::string &tableName, int64_t id) const;

private:
    std::string schema_{"public"};
    std::shared_ptr<TableRepository> repo_;
};

