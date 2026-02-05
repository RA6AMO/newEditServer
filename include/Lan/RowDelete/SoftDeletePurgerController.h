#pragma once

#include "Lan/AuthController.h"

#include <drogon/HttpController.h>
#include <drogon/drogon.h>
#include <json/json.h>

#include <string>

/// Контроллер для ручного запуска purge soft delete.
/// Маршрут: POST /row/purge
class SoftDeletePurgerController : public drogon::HttpController<SoftDeletePurgerController>
{
public:
    METHOD_LIST_BEGIN
    ADD_METHOD_TO(SoftDeletePurgerController::purge, "/row/purge", drogon::Post);
    METHOD_LIST_END

    drogon::Task<drogon::HttpResponsePtr> purge(drogon::HttpRequestPtr req);

private:
    static drogon::HttpResponsePtr makeSuccessResponse(int purged);
    static drogon::HttpResponsePtr makeErrorResponse(const std::string &code,
                                                     const std::string &message,
                                                     drogon::HttpStatusCode status);
};
