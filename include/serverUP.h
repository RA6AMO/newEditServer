#pragma once

#include <string>
#include <drogon/HttpController.h>
#include <string>

// Функция запускает HTTP‑сервер Drogon.
// Параметры можно расширить при необходимости (порт, адрес и т.п.).
// Бросает исключения drogon/trantor при ошибке и не перехватывает их.
void runDrogonServer(const std::string &address = "0.0.0.0", unsigned short port = 8080);


class SystemController : public drogon::HttpController<SystemController>
{
public:
    METHOD_LIST_BEGIN
        // GET /health
        METHOD_ADD(SystemController::health, "/health", drogon::Get);

    METHOD_LIST_END

    void health(const drogon::HttpRequestPtr &req,
        std::function<void (const drogon::HttpResponsePtr &)> &&callback);

};