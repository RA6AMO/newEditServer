#pragma once

#include "AuthController.h"
#include "Lan/RowAdd/RowWriteTypes.h"

#include <drogon/HttpController.h>
#include <drogon/drogon.h>
#include <drogon/utils/coroutine.h>
#include <json/json.h>

#include <string>

/// Контроллер обновления одной ячейки по table/rowId/dbName.
class CellUpdateController : public drogon::HttpController<CellUpdateController>
{
public:
    METHOD_LIST_BEGIN
    ADD_METHOD_TO(CellUpdateController::updateCell, "/row/updateCell", drogon::Post);
    METHOD_LIST_END

    /// Парсинг запроса: извлечение JSON payload и файлов.
    struct ParsedRequest
    {
        Json::Value payload;
        std::vector<AttachmentInput> attachments;
    };

    drogon::Task<drogon::HttpResponsePtr> updateCell(drogon::HttpRequestPtr req);

private:
    ParsedRequest parseMultipartRequest(drogon::HttpRequestPtr req) const;

    static drogon::HttpResponsePtr makeSuccessResponse(int64_t rowId,
                                                       const std::string &dbName,
                                                       const Json::Value &dataExtra = Json::nullValue);
    static drogon::HttpResponsePtr makeErrorResponse(const std::string &code,
                                                     const std::string &message,
                                                     drogon::HttpStatusCode status);
};
