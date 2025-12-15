#pragma once

#include <drogon/HttpController.h>
#include <drogon/drogon.h>
#include <json/json.h>
#include <functional>
#include <map>

class TableInfoSender : public drogon::HttpController<TableInfoSender>
{
public:
    METHOD_LIST_BEGIN
    ADD_METHOD_TO(TableInfoSender::getTableInfo, "/table/get", drogon::Get);
    METHOD_LIST_END

    drogon::Task<drogon::HttpResponsePtr> getTableInfo(drogon::HttpRequestPtr req);
private:
    std::map<std::string, std::string> defoltTableNames{
        {"All_Default_Instrument_Table", "public.milling_tool_catalog"},
    }
};