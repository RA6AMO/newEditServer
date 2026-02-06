#pragma once

#include <drogon/drogon.h>
#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

/// Сервис маппинга локальных id -> глобальные id из global_object_registry.
class GlobalIdService
{
public:
    drogon::Task<std::unordered_map<int64_t, int64_t>>
    getGlobalIdsByLocalIds(const std::string &tableName, const std::vector<int64_t> &localIds) const;
};
