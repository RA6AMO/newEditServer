#include "Lan/RowDelete/RowDeleteController.h"
#include "Lan/allTableList.h"
#include "Loger/Logger.h"

#include <json/reader.h>

#include <cctype>
#include <limits>
#include <stdexcept>

namespace
{
Json::Value makeErrorObj(const std::string &code,
                         const std::string &message,
                         const Json::Value &details = Json::nullValue)
{
    Json::Value root;
    root["ok"] = false;
    root["error"]["code"] = code;
    root["error"]["message"] = message;
    if (!details.isNull())
    {
        root["error"]["details"] = details;
    }
    return root;
}

drogon::HttpResponsePtr makeJsonResponse(const Json::Value &body, drogon::HttpStatusCode status)
{
    auto resp = drogon::HttpResponse::newHttpJsonResponse(body);
    resp->setStatusCode(status);
    return resp;
}

bool isSafeIdentifier(const std::string &name)
{
    if (name.empty())
    {
        return false;
    }
    for (const char c : name)
    {
        if (!(std::isalnum(static_cast<unsigned char>(c)) || c == '_'))
        {
            return false;
        }
    }
    return true;
}

std::string quoteIdent(const std::string &name)
{
    return "\"" + name + "\"";
}

bool parseRowId(const Json::Value &value, int64_t &out)
{
    if (value.isInt64())
    {
        out = value.asInt64();
        return true;
    }
    if (value.isUInt64())
    {
        const auto v = value.asUInt64();
        if (v > static_cast<uint64_t>(std::numeric_limits<int64_t>::max()))
        {
            return false;
        }
        out = static_cast<int64_t>(v);
        return true;
    }
    if (value.isInt())
    {
        out = value.asInt();
        return true;
    }
    if (value.isUInt())
    {
        out = static_cast<int64_t>(value.asUInt());
        return true;
    }
    if (value.isString())
    {
        try
        {
            size_t idx = 0;
            const std::string s = value.asString();
            const long long parsed = std::stoll(s, &idx, 10);
            if (idx != s.size())
            {
                return false;
            }
            out = static_cast<int64_t>(parsed);
            return true;
        }
        catch (const std::exception &)
        {
            return false;
        }
    }
    return false;
}
} // namespace

drogon::Task<drogon::HttpResponsePtr> RowDeleteController::deleteRow(drogon::HttpRequestPtr req)
{
    using namespace drogon;

    try
    {
        const std::string token = req->getHeader("token");
        TokenValidator validator;
        auto tokenStatus = co_await validator.check(token, req->getPeerAddr().toIp());
        if (tokenStatus != TokenValidator::Status::Ok)
        {
            const auto httpCode = TokenValidator::toHttpCode(tokenStatus);
            const std::string msg = TokenValidator::toError(tokenStatus);
            const std::string code = (httpCode == k401Unauthorized) ? "unauthorized" : "internal";
            co_return makeJsonResponse(makeErrorObj(code, msg), httpCode);
        }

        ParsedRequest parsed;
        try
        {
            parsed = parseJsonRequest(req);
        }
        catch (const std::exception &e)
        {
            co_return makeErrorResponse("bad_request",
                                        "Failed to parse request payload: " + std::string(e.what()),
                                        k400BadRequest);
        }

        const std::string baseTable = resolveBaseTable(parsed.table);
        int tableId = 0;
        if (!tryGetTableIdByName(baseTable, tableId))
        {
            Logger::instance().error("RowDeleteController: table is not supported: " + parsed.table);
            co_return makeErrorResponse("bad_request", "Table is not supported", k400BadRequest);
        }
        if (!isSafeIdentifier(baseTable))
        {
            Logger::instance().error("RowDeleteController: unsafe table name: " + baseTable);
            co_return makeErrorResponse("bad_request", "Invalid table name", k400BadRequest);
        }
        if (parsed.rowId <= 0)
        {
            co_return makeErrorResponse("bad_request", "Invalid request: rowId must be positive", k400BadRequest);
        }

        const std::string sql =
            "UPDATE public." + quoteIdent(baseTable) + " SET is_deleted = TRUE, deleted_at = now() WHERE id = $1";
        auto dbClient = app().getDbClient("default");
        co_await dbClient->execSqlCoro(sql, parsed.rowId);

        Logger::instance().info("RowDeleteController: soft deleted " + baseTable + " id=" + std::to_string(parsed.rowId));
        co_return makeSuccessResponse(parsed.rowId);
    }
    catch (const std::exception &e)
    {
        LOG_ERROR(std::string("deleteRow fatal error: ") + e.what());
        co_return makeErrorResponse("internal", "Internal error: " + std::string(e.what()), k500InternalServerError);
    }
}

drogon::Task<drogon::HttpResponsePtr> RowDeleteController::restoreRow(drogon::HttpRequestPtr req)
{
    using namespace drogon;

    try
    {
        const std::string token = req->getHeader("token");
        TokenValidator validator;
        auto tokenStatus = co_await validator.check(token, req->getPeerAddr().toIp());
        if (tokenStatus != TokenValidator::Status::Ok)
        {
            const auto httpCode = TokenValidator::toHttpCode(tokenStatus);
            const std::string msg = TokenValidator::toError(tokenStatus);
            const std::string code = (httpCode == k401Unauthorized) ? "unauthorized" : "internal";
            co_return makeJsonResponse(makeErrorObj(code, msg), httpCode);
        }

        ParsedRequest parsed;
        try
        {
            parsed = parseJsonRequest(req);
        }
        catch (const std::exception &e)
        {
            co_return makeErrorResponse("bad_request",
                                        "Failed to parse request payload: " + std::string(e.what()),
                                        k400BadRequest);
        }

        const std::string baseTable = resolveBaseTable(parsed.table);
        int tableId = 0;
        if (!tryGetTableIdByName(baseTable, tableId))
        {
            Logger::instance().error("RowDeleteController: table is not supported: " + parsed.table);
            co_return makeErrorResponse("bad_request", "Table is not supported", k400BadRequest);
        }
        if (!isSafeIdentifier(baseTable))
        {
            Logger::instance().error("RowDeleteController: unsafe table name: " + baseTable);
            co_return makeErrorResponse("bad_request", "Invalid table name", k400BadRequest);
        }
        if (parsed.rowId <= 0)
        {
            co_return makeErrorResponse("bad_request", "Invalid request: rowId must be positive", k400BadRequest);
        }

        const std::string sql =
            "UPDATE public." + quoteIdent(baseTable) +
            " SET is_deleted = FALSE, deleted_at = NULL WHERE id = $1 AND is_deleted = TRUE";
        auto dbClient = app().getDbClient("default");
        const auto result = co_await dbClient->execSqlCoro(sql, parsed.rowId);
        if (result.affectedRows() == 0)
        {
            co_return makeErrorResponse("not_found",
                                        "Row not found or not deleted",
                                        k404NotFound);
        }

        Logger::instance().info("RowDeleteController: restored " + baseTable + " id=" + std::to_string(parsed.rowId));
        co_return makeSuccessResponse(parsed.rowId);
    }
    catch (const std::exception &e)
    {
        LOG_ERROR(std::string("restoreRow fatal error: ") + e.what());
        co_return makeErrorResponse("internal", "Internal error: " + std::string(e.what()), k500InternalServerError);
    }
}

RowDeleteController::ParsedRequest RowDeleteController::parseJsonRequest(drogon::HttpRequestPtr req)
{
    const std::string body(req->body());
    if (body.empty())
    {
        throw std::runtime_error("Empty request body");
    }

    Json::Value payload;
    Json::Reader reader;
    if (!reader.parse(body, payload))
    {
        throw std::runtime_error("Invalid JSON in request body");
    }
    if (!payload.isObject())
    {
        throw std::runtime_error("Invalid payload: expected JSON object");
    }

    ParsedRequest result;
    if (!payload.isMember("table") || !payload["table"].isString())
    {
        throw std::runtime_error("Invalid payload: table is required");
    }
    if (!payload.isMember("rowId"))
    {
        throw std::runtime_error("Invalid payload: rowId is required");
    }

    result.table = payload["table"].asString();
    if (!parseRowId(payload["rowId"], result.rowId))
    {
        throw std::runtime_error("Invalid payload: rowId must be integer");
    }

    return result;
}

drogon::HttpResponsePtr RowDeleteController::makeSuccessResponse(int64_t rowId)
{
    Json::Value root;
    root["ok"] = true;
    root["data"]["id"] = static_cast<Json::Int64>(rowId);
    return makeJsonResponse(root, drogon::k200OK);
}

drogon::HttpResponsePtr RowDeleteController::makeErrorResponse(const std::string &code,
                                                              const std::string &message,
                                                              drogon::HttpStatusCode status)
{
    return makeJsonResponse(makeErrorObj(code, message), status);
}
