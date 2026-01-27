#pragma once

#include "AuthController.h"
#include "allTableList.h"

#include <drogon/HttpController.h>
#include <drogon/drogon.h>
#include <drogon/utils/coroutine.h>
#include <json/json.h>

/// Выдача изображений для таблиц со схемой "images-by-slot".
/// POST /table/images/get
/// Headers: token
/// Body: { "nodeId": <int, 1-based>, "small": <bool>, "ids": [<rowId>, ...] }
class TableImageSender : public drogon::HttpController<TableImageSender>
{
public:
    METHOD_LIST_BEGIN
    ADD_METHOD_TO(TableImageSender::getTableImages, "/table/images/get", drogon::Post);
    METHOD_LIST_END

    drogon::Task<drogon::HttpResponsePtr> getTableImages(drogon::HttpRequestPtr req);
};
