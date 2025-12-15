#include "TableInfoSender.h"

#include <drogon/drogon.h>
#include <drogon/orm/DbClient.h>
#include <drogon/orm/Exception.h>

drogon::Task<drogon::HttpResponsePtr> TableInfoSender::getTableInfo(drogon::HttpRequestPtr req)
{
    (void)req;
    using namespace drogon;
    using namespace drogon::orm;

    Json::Value body;
    body["table"] = "public.milling_tool_catalog";
    body["columns"] = Json::arrayValue;

    try
    {
        auto dbClient = app().getDbClient("default");

        // Берём метаданные столбцов из information_schema.
        // Для numeric дополнительно достаём precision/scale (scale = "величина округления").
        auto rows = co_await dbClient->execSqlCoro(
            "SELECT "
            "  ordinal_position, "
            "  column_name, "
            "  data_type, "
            "  udt_name, "
            "  numeric_precision, "
            "  numeric_scale "
            "FROM information_schema.columns "
            "WHERE table_schema = 'public' "
            "  AND table_name   = 'milling_tool_catalog' "
            "ORDER BY ordinal_position");

        for (const auto &r : rows)
        {
            Json::Value col;
            col["name"] = r["column_name"].as<std::string>();

            // data_type более “человеческий”, udt_name полезен для доменов/enum/массивов.
            col["type"] = r["data_type"].as<std::string>();
            col["udt_name"] = r["udt_name"].as<std::string>();

            // precision/scale — только если они реально есть (обычно для numeric).
            if (!r["numeric_precision"].isNull())
            {
                col["numeric_precision"] = r["numeric_precision"].as<int>();
            }
            if (!r["numeric_scale"].isNull())
            {
                col["numeric_scale"] = r["numeric_scale"].as<int>();
            }

            body["columns"].append(std::move(col));
        }

        auto resp = HttpResponse::newHttpJsonResponse(body);
        resp->setStatusCode(k200OK);
        co_return resp;
    }
    catch (const DrogonDbException &)
    {
        Json::Value err;
        err["error"] = "db error";
        auto resp = HttpResponse::newHttpJsonResponse(err);
        resp->setStatusCode(k500InternalServerError);
        co_return resp;
    }
    catch (const std::exception &)
    {
        Json::Value err;
        err["error"] = "internal error";
        auto resp = HttpResponse::newHttpJsonResponse(err);
        resp->setStatusCode(k500InternalServerError);
        co_return resp;
    }
}