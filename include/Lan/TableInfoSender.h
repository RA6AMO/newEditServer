#pragma once

#include "AuthController.h"

#include "allTableList.h"
#include <drogon/HttpController.h>
#include <drogon/drogon.h>
#include <json/json.h>
#include <functional>
#include <map>
#include <vector>
#include <string>

// Список разрешённых таблиц (whitelist). Inline переменная нужна, т.к. это заголовок.


class TableInfoSender : public drogon::HttpController<TableInfoSender>
{
public:
    METHOD_LIST_BEGIN
    // GET + headers: token/nodeId передаются в заголовках запроса.
    ADD_METHOD_TO(TableInfoSender::getTableInfo, "/table/get", drogon::Get);
    METHOD_LIST_END

    drogon::Task<drogon::HttpResponsePtr> getTableInfo(drogon::HttpRequestPtr req);
private:

};