#pragma once

#include <algorithm>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

inline const std::unordered_map<int, std::string> kTableNames = {
    {1, "milling_tool_catalog"},
    {1001, "mills_catalog"},
    {2, ""}
};

inline const int kDefaultTableId = 1;

struct ChildTableSpec
{
    std::string parent;
    std::vector<std::string> exclude;
};

// Виртуальные "дети": используют таблицу parent, но скрывают указанные колонки.
inline const std::unordered_map<std::string, ChildTableSpec> kChildTables = {
    // {"mills_catalog", {"milling_tool_catalog", {"col_a", "col_b"}}},
};

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

inline bool tryGetChildSpec(const std::string &name, ChildTableSpec &outSpec)
{
    auto it = kChildTables.find(name);
    if (it == kChildTables.end())
    {
        return false;
    }
    outSpec = it->second;
    return true;
}

inline bool resolveChildChain(const std::string &name,
                              std::string &outBase,
                              std::vector<std::string> &outExclude)
{
    outBase = name;
    outExclude.clear();
    std::unordered_set<std::string> seen;
    while (true)
    {
        auto it = kChildTables.find(outBase);
        if (it == kChildTables.end())
        {
            break;
        }
        if (seen.find(outBase) != seen.end())
        {
            break;
        }
        seen.insert(outBase);
        const auto &spec = it->second;
        outExclude.insert(outExclude.end(), spec.exclude.begin(), spec.exclude.end());
        outBase = spec.parent;
    }
    return outBase != name;
}

inline std::string resolveBaseTable(const std::string &name)
{
    std::string base;
    std::vector<std::string> exclude;
    resolveChildChain(name, base, exclude);
    return base;
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