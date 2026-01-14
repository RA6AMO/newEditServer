#include "Lan/RowController.h"
#include "Loger/Logger.h"
#include "TableInfoCache.h"
#include "Lan/allTableList.h"

#include <drogon/drogon.h>
#include <drogon/MultiPart.h>
#include <json/reader.h>
#include <json/writer.h>
#include <sstream>
#include <algorithm>
#include <unordered_set>

namespace
{
Json::Value makeErrorObj(const std::string &code,
                        const std::string &message,
                        const Json::Value &details = Json::nullValue)
{
    Json::Value root;
    root["ok"] = false;
    root["error"]["code"] = code;
    root["error"]["message"] = message;
    if (!details.isNull())
    {
        root["error"]["details"] = details;
    }
    return root;
}

drogon::HttpResponsePtr makeJsonResponse(const Json::Value &body, drogon::HttpStatusCode status)
{
    auto resp = drogon::HttpResponse::newHttpJsonResponse(body);
    resp->setStatusCode(status);
    return resp;
}

struct BadRequestError : public std::runtime_error
{
    using std::runtime_error::runtime_error;
};
} // namespace

drogon::Task<drogon::HttpResponsePtr> RowController::addRow(drogon::HttpRequestPtr req)
{
    using namespace drogon;

    try
    {
        // 1) Базовая проверка токена (точно понадобится всегда)
        const std::string token = req->getHeader("token");
        TokenValidator validator;
        auto tokenStatus = co_await validator.check(token, req->getPeerAddr().toIp());
        if (tokenStatus != TokenValidator::Status::Ok)
        {
            const auto httpCode = TokenValidator::toHttpCode(tokenStatus);
            const std::string msg = TokenValidator::toError(tokenStatus);
            const std::string code = (httpCode == k401Unauthorized) ? "unauthorized" : "internal";
            co_return makeJsonResponse(makeErrorObj(code, msg), httpCode);
        }

        // 2) Базовая проверка формата: извлекаем JSON payload (multipart или json body)
        ParsedRequest parsed;
        try
        {
            parsed = parseMultipartRequest(req);
        }
        catch (const std::exception &e)
        {
            co_return makeErrorResponse("bad_request", "Failed to parse multipart request: " + std::string(e.what()), k400BadRequest);
        }

        // 3) Минимальная проверка payload: он должен быть JSON-объектом
        if (!parsed.payload.isObject())
        {
            co_return makeErrorResponse("bad_request", "Invalid payload: expected JSON object", k400BadRequest);
        }

        // 4) Валидация таблицы/колонок (сравниваем payload с information_schema через TableInfoCache)
        try
        {
            co_await customer_table_validation(parsed);
        }
        catch (const BadRequestError &e)
        {
            co_return makeErrorResponse("bad_request", e.what(), k400BadRequest);
        }
        catch (const std::exception &e)
        {
            LOG_ERROR(std::string("customer_table_validation error: ") + e.what());
            co_return makeErrorResponse("internal", "Internal validation error", k500InternalServerError);
        }



        // 5) Принципиально: бизнес-логика пока не реализована (ты будешь менять её сам).
        // Здесь оставляем только базовый каркас и единый формат ошибок.
        Json::Value details;
        if (parsed.payload.isMember("table") && parsed.payload["table"].isString())
        {
            details["table"] = parsed.payload["table"].asString();

        }
        details["validated"] = true;
        co_return makeJsonResponse(makeErrorObj("not_implemented", "Request validated, but row creation logic is not implemented yet", details),
                                  k501NotImplemented);

    }
    catch (const std::exception &e)
    {
        LOG_ERROR(std::string("addRow fatal error: ") + e.what());
        co_return makeErrorResponse("internal", "Internal error: " + std::string(e.what()), k500InternalServerError);
    }
}

drogon::Task<void> RowController::customer_table_validation(const RowController::ParsedRequest &parsed) const
{
    if (!parsed.payload.isObject())
    {
        throw BadRequestError("Invalid payload: expected JSON object");
    }

    const Json::Value &p = parsed.payload;
    if (!p.isMember("table") || !p["table"].isString())
    {
        throw BadRequestError("Invalid payload: missing string field 'table'");
    }

    const std::string tableName = p["table"].asString();
    if (tableName.empty())
    {
        throw BadRequestError("Invalid payload: empty 'table'");
    }

    // 1) Разрешаем только таблицы из whitelist (защита от подстановки произвольного имени)
    if (std::find(kTableNames.begin(), kTableNames.end(), tableName) == kTableNames.end())
    {
        throw BadRequestError("Invalid payload: table is not allowed");
    }

    // 2) Берём список колонок из кеша (information_schema.columns)
    auto cache = drogon::app().getPlugin<TableInfoCache>();
    if (!cache)
    {
        throw std::runtime_error("TableInfoCache is not initialized");
    }

    auto colsPtr = co_await cache->getColumns(tableName);
    if (!colsPtr)
    {
        throw std::runtime_error("TableInfoCache returned null columns pointer");
    }
    const Json::Value &cols = *colsPtr;
    if (!cols.isArray() || cols.empty())
    {
        throw BadRequestError("Invalid payload: unknown table or empty schema");
    }

    std::unordered_set<std::string> allowedColumns;
    allowedColumns.reserve(cols.size() + 1);
    for (const auto &c : cols)
    {
        if (c.isObject() && c.isMember("name") && c["name"].isString())
        {
            allowedColumns.insert(c["name"].asString());
        }
    }
    allowedColumns.insert("id");

    auto validateObjectKeys = [&](const char *fieldName)
    {
        if (!p.isMember(fieldName))
        {
            throw BadRequestError(std::string("Invalid payload: missing '") + fieldName + "'");
        }
        if (!p[fieldName].isObject())
        {
            throw BadRequestError(std::string("Invalid payload: '") + fieldName + "' must be an object");
        }
        const auto names = p[fieldName].getMemberNames();
        for (const auto &k : names)
        {
            if (allowedColumns.find(k) == allowedColumns.end())
            {
                throw BadRequestError(std::string("Invalid payload: unknown column in '") + fieldName + "': " + k);
            }
        }
    };

    // 3) Проверяем, что keys в fields/types — только известные колонки
    validateObjectKeys("fields");
    validateObjectKeys("types");

    // 4) Консистентность: каждый ключ из fields (кроме id) должен иметь тип в types
    const auto fieldKeys = p["fields"].getMemberNames();
    for (const auto &k : fieldKeys)
    {
        if (k == "id")
            continue;
        if (!p["types"].isMember(k))
        {
            throw BadRequestError(std::string("Invalid payload: types missing key for field: ") + k);
        }
    }

    co_return;
}

RowController::ParsedRequest RowController::parseMultipartRequest(drogon::HttpRequestPtr req) const
{
    ParsedRequest result;

    // 1) multipart/form-data (старый клиентский контракт)
    if (req->contentType() == drogon::CT_MULTIPART_FORM_DATA)
    {
        drogon::MultiPartParser parser;
        if (parser.parse(req) != 0)
        {
            throw std::runtime_error("Failed to parse multipart/form-data");
        }

        const auto &params = parser.getParameters();
        auto payloadIt = params.find("payload");
        if (payloadIt == params.end())
        {
            throw std::runtime_error("Missing 'payload' field in multipart request");
        }

        Json::Reader reader;
        if (!reader.parse(payloadIt->second, result.payload))
        {
            throw std::runtime_error("Invalid JSON in payload field");
        }

        // Файлы могут присутствовать (клиент шлёт attachments).
        // На этапе "только валидация таблицы" мы их не обрабатываем, но и не отклоняем запрос.
        return result;
    }


    // 2) application/json (полезно для будущих тестов и альтернативных клиентов)
    const std::string body(req->body());
    if (body.empty())
    {
        throw std::runtime_error("Empty request body");
    }
    Json::Reader reader;
    if (!reader.parse(body, result.payload))
    {
        throw std::runtime_error("Invalid JSON in request body");
    }

    return result;
}

drogon::HttpResponsePtr RowController::makeSuccessResponse(int64_t rowId, const Json::Value &dataExtra)
{
    Json::Value root;
    root["ok"] = true;
    root["data"]["id"] = static_cast<Json::Int64>(rowId);
    if (!dataExtra.isNull() && dataExtra.isObject())
    {
        for (const auto &name : dataExtra.getMemberNames())
        {
            root["data"][name] = dataExtra[name];
        }
    }
    return makeJsonResponse(root, drogon::k200OK);
}

drogon::HttpResponsePtr RowController::makeErrorResponse(const std::string &code,
                                                        const std::string &message,
                                                        drogon::HttpStatusCode status)
{
    return makeJsonResponse(makeErrorObj(code, message), status);
}

