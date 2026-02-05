#pragma once

#include "AuthController.h"

#include <drogon/HttpController.h>
#include <drogon/drogon.h>
#include <json/json.h>

#include <cstdint>
#include <string>

/// Контроллер для soft delete записи.
/// Маршрут: POST /row/delete
/// Ожидает JSON: { "table": "...", "rowId": 123 }
class RowDeleteController : public drogon::HttpController<RowDeleteController>
{
public:
    METHOD_LIST_BEGIN
    ADD_METHOD_TO(RowDeleteController::deleteRow, "/row/delete", drogon::Post);
    ADD_METHOD_TO(RowDeleteController::restoreRow, "/row/restore", drogon::Post);
    METHOD_LIST_END

    drogon::Task<drogon::HttpResponsePtr> deleteRow(drogon::HttpRequestPtr req);
    drogon::Task<drogon::HttpResponsePtr> restoreRow(drogon::HttpRequestPtr req);

private:
    struct ParsedRequest
    {
        std::string table;
        int64_t rowId = 0;
    };

    static drogon::HttpResponsePtr makeSuccessResponse(int64_t rowId);
    static drogon::HttpResponsePtr makeErrorResponse(const std::string &code,
                                                     const std::string &message,
                                                     drogon::HttpStatusCode status);
    static ParsedRequest parseJsonRequest(drogon::HttpRequestPtr req);
};
