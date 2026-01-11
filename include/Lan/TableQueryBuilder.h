#pragma once

#include <json/json.h>

#include <string>
#include <unordered_set>

/// Сборка SQL-фрагмента WHERE из фильтров клиента.
/// ВАЖНО: dbName должен быть уже проверен по whitelist (allowedColumns).
class TableQueryBuilder
{
public:
    /// Построить WHERE ... (или пустую строку, если условий нет).
    ///
    /// filters: JSON array объектов вида {dbName, type, op, nullMode?, v1?, v2?}
    /// allowedColumns: whitelist колонок для подстановки в SQL
    ///
    /// Бросает std::runtime_error при некорректных типах значений.
    static std::string buildWhere(const Json::Value &filters,
                                  const std::unordered_set<std::string> &allowedColumns);
};

