#include "TableInfoSender.h"
#include "TableInfoCache.h"

#include <drogon/drogon.h>
#include <drogon/orm/DbClient.h>
#include <drogon/orm/Exception.h>

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

} // namespace

drogon::Task<drogon::HttpResponsePtr> TableInfoSender::getTableInfo(drogon::HttpRequestPtr req)
{
    using namespace drogon;
    using namespace drogon::orm;

    // GET + headers: token/nodeId берём строго из заголовков.
    const std::string token = req->getHeader("token");

    TokenValidator validator;
    auto status = co_await validator.check(token, req->getPeerAddr().toIp());
    if (status != TokenValidator::Status::Ok)
    {
        const auto httpCode = TokenValidator::toHttpCode(status);
        const std::string msg = TokenValidator::toError(status);
        const std::string code = (httpCode == k401Unauthorized) ? "unauthorized" : "internal";
        co_return makeJsonResponse(makeErrorObj(code, msg), httpCode);
    }

    const auto nodeIdHeader = req->getHeader("nodeId");
    if (nodeIdHeader.empty())
    {
        co_return makeJsonResponse(makeErrorObj("bad_request", "missing nodeId header"), k400BadRequest);
    }

    int nodeId = -1;
    try
    {
        nodeId = std::stoi(nodeIdHeader);
        nodeId -= 1;
    }
    catch (...)
    {
        co_return makeJsonResponse(makeErrorObj("bad_request", "invalid nodeId header"), k400BadRequest);
    }

    if (nodeId < 0 || static_cast<size_t>(nodeId) >= kTableNames.size())
    {
        Json::Value details;
        details["expected_range"] =
            "0.." + std::to_string(kTableNames.size() == 0 ? 0 : (kTableNames.size() - 1));
        co_return makeJsonResponse(makeErrorObj("bad_request", "invalid nodeId", details), k400BadRequest);
    }

    const std::string tableName = kTableNames[static_cast<size_t>(nodeId)];
    Json::Value data;
    data["nodeId"] = nodeId;
    data["table"] = tableName;
    data["columns"] = Json::arrayValue;

    try
    {
        auto cache = app().getPlugin<TableInfoCache>();
        if (!cache)
        {
            co_return makeJsonResponse(makeErrorObj("internal", "TableInfoCache is not initialized"),
                                       k500InternalServerError);
        }

        auto cols = co_await cache->getColumns(tableName);
        data["columns"] = *cols;

        Json::Value root;
        root["ok"] = true;
        root["data"] = std::move(data);
        co_return makeJsonResponse(root, k200OK);
    }
    catch (const DrogonDbException &)
    {
        co_return makeJsonResponse(makeErrorObj("internal", "db error"), k500InternalServerError);
    }
    catch (const std::exception &)
    {
        co_return makeJsonResponse(makeErrorObj("internal", "internal error"), k500InternalServerError);
    }
}