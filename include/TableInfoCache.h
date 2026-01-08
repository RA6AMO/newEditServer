#pragma once

#include <drogon/plugins/Plugin.h>
#include <drogon/utils/coroutine.h>
#include <json/json.h>

#include <shared_mutex>
#include <string>
#include <unordered_map>
#include <memory>

/// Кэш метаданных таблиц (information_schema.columns).
/// Достаёт список колонок по имени таблицы и хранит результат в памяти.
/// Потокобезопасен через std::shared_mutex.
class TableInfoCache : public drogon::Plugin<TableInfoCache>
{
public:
    void initAndStart(const Json::Value &config) override;
    void shutdown() override;

    /// Получить (и при необходимости закешировать) список колонок таблицы.
    /// Возвращаем готовый Json-массив columns (как в ответе /table/get).
    drogon::Task<std::shared_ptr<const Json::Value>> getColumns(const std::string &tableName);

    /// Удалить запись из кеша для конкретной таблицы.
    void invalidate(const std::string &tableName);

    /// Очистить весь кеш.
    void clear();

private:
    std::string schema_{"public"};
    std::string dbClientName_{"default"};

    mutable std::shared_mutex mu_;
    std::unordered_map<std::string, std::shared_ptr<const Json::Value>> columnsByTable_;
};

