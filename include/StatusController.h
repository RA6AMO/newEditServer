#pragma once

#include <drogon/HttpController.h>
#include <json/json.h>

/// Простейший контроллер для проверки статуса сервера.
/// Маршрут: GET /status
class StatusController : public drogon::HttpController<StatusController>
{
public:
    // Описание маршрутов этого контроллера.
    METHOD_LIST_BEGIN
    ADD_METHOD_TO(StatusController::getStatus, "/status", drogon::Get);
    METHOD_LIST_END

    /// Единственный метод контроллера: возвращает простой JSON со статусом.
    void getStatus(const drogon::HttpRequestPtr &req,
                   std::function<void(const drogon::HttpResponsePtr &)> &&callback);
};