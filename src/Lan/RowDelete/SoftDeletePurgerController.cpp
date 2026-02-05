#include "Lan/RowDelete/SoftDeletePurgerController.h"
#include "Lan/RowDelete/SoftDeletePurgerPlugin.h"
#include "Loger/Logger.h"

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

drogon::Task<drogon::HttpResponsePtr> SoftDeletePurgerController::purge(drogon::HttpRequestPtr req)
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

        auto plugin = app().getPlugin<SoftDeletePurgerPlugin>();
        if (!plugin)
        {
            Logger::instance().error("SoftDeletePurgerController: plugin is not initialized");
            co_return makeErrorResponse("internal", "Soft delete purger is not initialized", k500InternalServerError);
        }

        const int purged = co_await plugin->runOnce();
        co_return makeSuccessResponse(purged);
    }
    catch (const std::exception &e)
    {
        Logger::instance().error("SoftDeletePurgerController: fatal error: " + std::string(e.what()));
        co_return makeErrorResponse("internal", "Internal error: " + std::string(e.what()), k500InternalServerError);
    }
}

drogon::HttpResponsePtr SoftDeletePurgerController::makeSuccessResponse(int purged)
{
    Json::Value root;
    root["ok"] = true;
    root["data"]["purged"] = purged;
    return makeJsonResponse(root, drogon::k200OK);
}

drogon::HttpResponsePtr SoftDeletePurgerController::makeErrorResponse(const std::string &code,
                                                                     const std::string &message,
                                                                     drogon::HttpStatusCode status)
{
    return makeJsonResponse(makeErrorObj(code, message), status);
}
