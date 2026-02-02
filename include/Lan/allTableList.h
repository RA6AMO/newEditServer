#pragma once

#include <algorithm>
#include <string>
#include <unordered_map>
#include <vector>

inline const std::unordered_map<int, std::string> kTableNames = {
    {1, "milling_tool_catalog"},
    {1001, "mills_catalog"},
    {2, ""}
};

inline const int kDefaultTableId = 1;

inline const std::unordered_map<std::string, std::string> kTableMinioBySlot = {
    {"milling_tool_catalog", "milling_tool_images"},
};

inline bool tryGetTableNameById(int nodeId, std::string &outName)
{
    auto it = kTableNames.find(nodeId);
    if (it == kTableNames.end())
    {
        return false;
    }
    outName = it->second;
    return true;
}

inline bool tryGetTableIdByName(const std::string &name, int &outId)
{
    for (const auto &entry : kTableNames)
    {
        if (!entry.second.empty() && entry.second == name)
        {
            outId = entry.first;
            return true;
        }
    }
    return false;
}

inline std::string formatTableIdRange()
{
    if (kTableNames.empty())
    {
        return "0";
    }

    std::vector<int> ids;
    ids.reserve(kTableNames.size());
    for (const auto &entry : kTableNames)
    {
        ids.push_back(entry.first);
    }
    std::sort(ids.begin(), ids.end());

    if (ids.size() == 1)
    {
        return std::to_string(ids.front());
    }

    bool contiguous = true;
    for (size_t i = 1; i < ids.size(); ++i)
    {
        if (ids[i] != ids[i - 1] + 1)
        {
            contiguous = false;
            break;
        }
    }

    if (contiguous)
    {
        return std::to_string(ids.front()) + ".." + std::to_string(ids.back());
    }

    std::string result;
    for (size_t i = 0; i < ids.size(); ++i)
    {
        if (i > 0)
        {
            result += ",";
        }
        result += std::to_string(ids[i]);
    }
    return result;
}