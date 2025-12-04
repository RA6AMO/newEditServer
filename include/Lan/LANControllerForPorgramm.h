#pragma once

#include <drogon/HttpController.h>
#include <json/json.h>

/// Простейший контроллер для проверки статуса сервера.
/// Маршрут: GET /status
class LANControllerForPorgramm : public drogon::HttpController<LANControllerForPorgramm>
{
public:
    // Описание маршрутов этого контроллера.
    METHOD_LIST_BEGIN
    ADD_METHOD_TO(LANControllerForPorgramm::getStatus, "/status", drogon::Get);
    METHOD_LIST_END

    /// Единственный метод контроллера: возвращает простой JSON со статусом.
    void getStatus(const drogon::HttpRequestPtr &req,
                   std::function<void(const drogon::HttpResponsePtr &)> &&callback);
};