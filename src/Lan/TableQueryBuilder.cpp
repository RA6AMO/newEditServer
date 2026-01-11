#include "Lan/TableQueryBuilder.h"

#include "Lan/ServiceErrors.h"

#include <cmath>
#include <locale>
#include <sstream>
#include <stdexcept>
#include <vector>

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

// Маппинг "type" (int) из клиента в ожидаемый тип значения.
// Пока поддерживаем только Integer/Double/Boolean.
enum class FilterType
{
    Integer = 0,
    Double = 2,
    Boolean = 3
};

FilterType parseFilterType(const Json::Value &t)
{
    if (!t.isInt())
        throw BadRequestError("filter type must be int");
    const int v = t.asInt();
    if (v == 0)
        return FilterType::Integer;
    if (v == 2)
        return FilterType::Double;
    if (v == 3)
        return FilterType::Boolean;
    throw BadRequestError("unsupported filter type");
}

std::string toSqlLiteralInteger(const Json::Value &v)
{
    if (v.isInt64() || v.isInt())
        return std::to_string(v.asInt64());
    if (v.isUInt64() || v.isUInt())
        return std::to_string(v.asUInt64());
    if (v.isString())
    {
        // строгий парсинг строки
        const std::string s = v.asString();
        size_t pos = 0;
        long long val = std::stoll(s, &pos, 10);
        if (pos != s.size())
            throw BadRequestError("invalid integer literal");
        return std::to_string(val);
    }
    throw BadRequestError("invalid integer literal");
}

std::string toSqlLiteralBoolean(const Json::Value &v)
{
    if (v.isBool())
        return v.asBool() ? "TRUE" : "FALSE";
    if (v.isInt() || v.isInt64())
        return v.asInt64() != 0 ? "TRUE" : "FALSE";
    if (v.isString())
    {
        std::string s = v.asString();
        for (auto &c : s)
            c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        if (s == "true" || s == "1" || s == "yes" || s == "on")
            return "TRUE";
        if (s == "false" || s == "0" || s == "no" || s == "off")
            return "FALSE";
    }
    throw BadRequestError("invalid boolean literal");
}

std::string toSqlLiteralDouble(const Json::Value &v)
{
    double d = 0.0;
    if (v.isDouble() || v.isNumeric())
    {
        d = v.asDouble();
    }
    else if (v.isString())
    {
        const std::string s = v.asString();
        size_t pos = 0;
        d = std::stod(s, &pos);
        if (pos != s.size())
            throw BadRequestError("invalid double literal");
    }
    else
    {
        throw BadRequestError("invalid double literal");
    }

    if (!std::isfinite(d))
        throw BadRequestError("invalid double literal");

    std::ostringstream oss;
    oss.imbue(std::locale::classic());
    oss.setf(std::ios::fmtflags(0), std::ios::floatfield);
    oss.precision(17);
    oss << d;
    return oss.str();
}

std::string toSqlLiteralByType(FilterType type, const Json::Value &v)
{
    switch (type)
    {
    case FilterType::Integer:
        return toSqlLiteralInteger(v);
    case FilterType::Double:
        return toSqlLiteralDouble(v);
    case FilterType::Boolean:
        return toSqlLiteralBoolean(v);
    }
    throw BadRequestError("unsupported filter type");
}
} // namespace

std::string TableQueryBuilder::buildWhere(const Json::Value &filters,
                                         const std::unordered_set<std::string> &allowedColumns)
{
    if (!filters.isArray() || filters.empty())
        return {};

    std::vector<std::string> clauses;
    clauses.reserve(filters.size());

    for (Json::ArrayIndex i = 0; i < filters.size(); ++i)
    {
        const Json::Value &f = filters[i];
        if (!f.isObject())
            throw BadRequestError("each filter must be object");

        if (!f.isMember("dbName") || !f["dbName"].isString())
            throw BadRequestError("filter dbName missing");
        const std::string dbName = f["dbName"].asString();
        if (allowedColumns.find(dbName) == allowedColumns.end())
            throw BadRequestError("dbName not allowed");

        if (!f.isMember("op") || !f["op"].isString())
            throw BadRequestError("filter op missing");
        const std::string op = f["op"].asString();

        // nullMode optional: any / not_null / null
        std::string nullMode = "any";
        if (f.isMember("nullMode"))
        {
            if (!f["nullMode"].isString())
                throw BadRequestError("invalid nullMode");
            nullMode = f["nullMode"].asString();
        }

        const std::string colSql = quoteIdent(dbName);

        if (nullMode == "null")
        {
            clauses.push_back(colSql + " IS NULL");
            continue;
        }
        if (nullMode == "not_null")
        {
            clauses.push_back(colSql + " IS NOT NULL");
            continue;
        }
        if (nullMode != "any")
        {
            throw BadRequestError("unsupported nullMode");
        }

        // type обязателен для any-режима, чтобы мы знали, как безопасно сериализовать literal
        if (!f.isMember("type"))
            throw BadRequestError("filter type missing");
        const FilterType type = parseFilterType(f["type"]);

        if (op == "equals")
        {
            if (!f.isMember("v1"))
                throw BadRequestError("equals requires v1");
            const std::string lit = toSqlLiteralByType(type, f["v1"]);
            clauses.push_back(colSql + " = " + lit);
            continue;
        }
        if (op == "range")
        {
            const bool hasV1 = f.isMember("v1");
            const bool hasV2 = f.isMember("v2");
            if (!hasV1 && !hasV2)
                throw BadRequestError("range requires v1 and/or v2");

            if (hasV1)
            {
                const std::string lit1 = toSqlLiteralByType(type, f["v1"]);
                clauses.push_back(colSql + " >= " + lit1);
            }
            if (hasV2)
            {
                const std::string lit2 = toSqlLiteralByType(type, f["v2"]);
                clauses.push_back(colSql + " <= " + lit2);
            }
            continue;
        }

        throw BadRequestError("unsupported op");
    }

    if (clauses.empty())
        return {};

    std::ostringstream where;
    where << "WHERE ";
    for (size_t i = 0; i < clauses.size(); ++i)
    {
        if (i)
            where << " AND ";
        where << clauses[i];
    }
    return where.str();
}

