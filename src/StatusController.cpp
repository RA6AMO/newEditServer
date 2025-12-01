#include "StatusController.h"

void StatusController::getStatus(
    const drogon::HttpRequestPtr & /*req*/,
    std::function<void(const drogon::HttpResponsePtr &)> &&callback)
{
    Json::Value body;
    body["status"] = "ok";

    auto resp = drogon::HttpResponse::newHttpJsonResponse(body);
    callback(resp);
}

