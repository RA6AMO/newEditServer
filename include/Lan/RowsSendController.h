#pragma once

#include "AuthController.h"
#include "allTableList.h"

#include <drogon/HttpController.h>
#include <drogon/drogon.h>
#include <json/json.h>

/// Controller слой для выдачи данных таблиц
/// GET /table/data/get?nodeId=...&offset=...&limit=...&filters=...
class RowsSendController : public drogon::HttpController<RowsSendController>
{
public:
    METHOD_LIST_BEGIN
    ADD_METHOD_TO(RowsSendController::getTableData, "/table/data/get", drogon::Get);
    METHOD_LIST_END

    drogon::Task<drogon::HttpResponsePtr> getTableData(drogon::HttpRequestPtr req);
};
