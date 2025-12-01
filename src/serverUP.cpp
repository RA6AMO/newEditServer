#include "serverUP.h"
#include <drogon/drogon.h>

void runDrogonServer(const std::string &address, unsigned short port)
{
    // Простейшая настройка: слушаем указанный адрес и порт.
    drogon::app()
        .addListener(address, port)
        .run();
}

void SystemController::health(const HttpRequestPtr &,
                              std::function<void (const HttpResponsePtr &)> &&callback)
{
    auto resp = HttpResponse::newHttpJsonResponse(
        Json::Value{
            {"status",  "ok"},
            {"service", kServiceName}
        }
    );
    callback(resp);
}