#include "Lan/RowsSendController.h"
#include "Lan/TableDataService.h"
#include "Lan/ServiceErrors.h"

#include <drogon/drogon.h>
#include <drogon/orm/Exception.h>
#include <json/json.h>

#include <cctype>
#include <string>
#include <unordered_set>

namespace
{
constexpr Json::ArrayIndex kMaxFilters = 100;

// Единый формат ошибок в LAN-эндпоинтах (см. RowController/TableInfoSender):
// { "ok": false, "error": { "code": "...", "message": "...", "details": {...?} } }
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

// Минимальный URL-decode для query-параметров (filters приходит как URL-encoded JSON):
// - %XX (hex) -> байт
// - '+' -> ' ' (форм-энкодинг; на практике часто встречается)
// Делаем строгий режим: если '%'-последовательность битая — 400.
inline int hexToNibble(char c)
{
    if (c >= '0' && c <= '9')
        return c - '0';
    if (c >= 'a' && c <= 'f')
        return 10 + (c - 'a');
    if (c >= 'A' && c <= 'F')
        return 10 + (c - 'A');
    return -1;
}

bool urlDecodeStrict(const std::string &in, std::string &out, std::string &err)
{
    out.clear();
    out.reserve(in.size());

    for (size_t i = 0; i < in.size(); ++i)
    {
        const char c = in[i];
        if (c == '%')
        {
            if (i + 2 >= in.size())
            {
                err = "invalid percent-encoding";
                return false;
            }
            const int hi = hexToNibble(in[i + 1]);
            const int lo = hexToNibble(in[i + 2]);
            if (hi < 0 || lo < 0)
            {
                err = "invalid percent-encoding";
                return false;
            }
            out.push_back(static_cast<char>((hi << 4) | lo));
            i += 2;
            continue;
        }
        if (c == '+')
        {
            // form-style encoding
            out.push_back(' ');
            continue;
        }
        out.push_back(c);
    }

    return true;
}

// Базовая защита от SQL-инъекций на уровне контроллера:
// - НЕ подставляем dbName в SQL тут вообще
// - но дополнительно отсеиваем заведомо "опасные" значения
// Окончательный whitelist по колонкам должен делать Service (зная схему таблицы).
bool isDbIdentifierSafe(const std::string &s)
{
    if (s.empty())
        return false;
    const unsigned char c0 = static_cast<unsigned char>(s[0]);
    if (!(std::isalpha(c0) || s[0] == '_'))
        return false;
    for (size_t i = 1; i < s.size(); ++i)
    {
        const unsigned char c = static_cast<unsigned char>(s[i]);
        if (!(std::isalnum(c) || s[i] == '_'))
            return false;
    }
    return true;
}

bool parseInt64Strict(const std::string &s, int64_t &out)
{
    try
    {
        size_t pos = 0;
        long long v = std::stoll(s, &pos, 10);
        if (pos != s.size())
            return false;
        out = static_cast<int64_t>(v);
        return true;
    }
    catch (...)
    {
        return false;
    }
}

bool parseIntStrict(const std::string &s, int &out)
{
    try
    {
        size_t pos = 0;
        long v = std::stol(s, &pos, 10);
        if (pos != s.size())
            return false;
        out = static_cast<int>(v);
        return true;
    }
    catch (...)
    {
        return false;
    }
}

drogon::HttpResponsePtr badRequest(const std::string &message, const Json::Value &details = Json::nullValue)
{
    return makeJsonResponse(makeErrorObj("bad_request", message, details), drogon::k400BadRequest);
}
} // namespace

drogon::Task<drogon::HttpResponsePtr> RowsSendController::getTableData(drogon::HttpRequestPtr req)
{
    using namespace drogon;

    // 1) token обязателен (авторизация)
    const std::string token = req->getHeader("token");
    if (token.empty())
    {
        co_return makeJsonResponse(makeErrorObj("unauthorized", "missing token header"), k401Unauthorized);
    }

    // 2) проверка токена + привязка к IP (TokenValidator использует AppCache + users.last_token/last_ip)
    TokenValidator validator;
    const auto status = co_await validator.check(token, req->getPeerAddr().toIp());
    if (status != TokenValidator::Status::Ok)
    {
        const auto httpCode = TokenValidator::toHttpCode(status);
        const std::string msg = TokenValidator::toError(status);
        const std::string code = (httpCode == k401Unauthorized) ? "unauthorized" : "internal";
        co_return makeJsonResponse(makeErrorObj(code, msg), httpCode);
    }

    // 3) nodeId обязателен (1-based), клиент НЕ передаёт имя таблицы.
    // Сервер маппит nodeId -> kTableNames[nodeId-1].
    const std::string nodeIdStr = req->getParameter("nodeId");
    if (nodeIdStr.empty())
    {
        co_return badRequest("missing nodeId query parameter");
    }

    int64_t nodeId = 0;
    if (!parseInt64Strict(nodeIdStr, nodeId))
    {
        co_return badRequest("invalid nodeId query parameter");
    }
    if (nodeId <= 0)
    {
        Json::Value details;
        details["expected_range"] =
            "1.." + std::to_string(kTableNames.size() == 0 ? 0 : kTableNames.size());
        co_return badRequest("invalid nodeId", details);
    }

    const int64_t idx64 = nodeId - 1; // 1-based -> 0-based
    if (idx64 < 0 || static_cast<size_t>(idx64) >= kTableNames.size())
    {
        Json::Value details;
        details["expected_range"] =
            "1.." + std::to_string(kTableNames.size() == 0 ? 0 : kTableNames.size());
        co_return badRequest("invalid nodeId", details);
    }

    // 4) offset/limit (опционально)
    // Нормализация: отрицательные значения приводим к 0, как делает клиент (qMax(0, ...)).
    int offset = 0;
    int limit = 0;

    const std::string offsetStr = req->getParameter("offset");
    if (!offsetStr.empty())
    {
        if (!parseIntStrict(offsetStr, offset))
        {
            co_return badRequest("invalid offset query parameter");
        }
        if (offset < 0)
            offset = 0;
    }

    const std::string limitStr = req->getParameter("limit");
    if (!limitStr.empty())
    {
        if (!parseIntStrict(limitStr, limit))
        {
            co_return badRequest("invalid limit query parameter");
        }
        if (limit < 0)
            limit = 0;
    }

    const std::string tableName = kTableNames[static_cast<size_t>(idx64)];

    // 5) filters (опционально): URL-encoded JSON array.
    // Если filters отсутствует или пустой — считаем, что фильтров нет и возвращаем все строки (в будущем Service).
    Json::Value filters = Json::arrayValue;
    const std::string filtersStr = req->getParameter("filters");
    if (!filtersStr.empty())
    {
        std::string decoded;
        std::string decodeErr;
        if (!urlDecodeStrict(filtersStr, decoded, decodeErr))
        {
            co_return badRequest("invalid filters encoding");
        }

        // JSON парсинг (jsoncpp). Требуем, чтобы верхний уровень был массивом.
        Json::CharReaderBuilder builder;
        builder["collectComments"] = false;
        std::unique_ptr<Json::CharReader> reader(builder.newCharReader());
        std::string errs;
        if (!reader->parse(decoded.data(), decoded.data() + decoded.size(), &filters, &errs))
        {
            Json::Value details;
            details["parse_error"] = errs;
            co_return badRequest("invalid filters json", details);
        }

        if (!filters.isArray())
        {
            co_return badRequest("filters must be a json array");
        }

        static const std::unordered_set<std::string> allowedKeys{
            "dbName", "type", "op", "nullMode", "v1", "v2"};

        if (filters.size() > kMaxFilters)
        {
            Json::Value details;
            details["maxFilters"] = static_cast<Json::UInt>(kMaxFilters);
            details["filtersCount"] = static_cast<Json::UInt>(filters.size());
            co_return badRequest("too many filters", details);
        }

        for (Json::ArrayIndex i = 0; i < filters.size(); ++i)
        {
            const Json::Value &f = filters[i];
            if (!f.isObject())
            {
                Json::Value details;
                details["index"] = i;
                co_return badRequest("each filter must be an object", details);
            }

            // strict keys: запрещаем неожиданные поля, чтобы контракт был стабильным
            // (и чтобы случайно не начать интерпретировать "лишние" поля в будущем).
            for (const auto &name : f.getMemberNames())
            {
                if (allowedKeys.find(name) == allowedKeys.end())
                {
                    Json::Value details;
                    details["index"] = i;
                    details["field"] = name;
                    co_return badRequest("unexpected field in filter object", details);
                }
            }

            // Обязательные поля: dbName/type/op
            if (!f.isMember("dbName") || !f["dbName"].isString())
            {
                Json::Value details;
                details["index"] = i;
                co_return badRequest("filter missing or invalid dbName", details);
            }
            const std::string dbName = f["dbName"].asString();
            if (!isDbIdentifierSafe(dbName))
            {
                Json::Value details;
                details["index"] = i;
                details["dbName"] = dbName;
                co_return badRequest("invalid dbName", details);
            }

            if (!f.isMember("type") || !f["type"].isInt())
            {
                Json::Value details;
                details["index"] = i;
                co_return badRequest("filter missing or invalid type", details);
            }

            if (!f.isMember("op") || !f["op"].isString())
            {
                Json::Value details;
                details["index"] = i;
                co_return badRequest("filter missing or invalid op", details);
            }
            const std::string op = f["op"].asString();

            // nullMode (опционально): any / not_null / null
            std::string nullMode = "any";
            if (f.isMember("nullMode"))
            {
                if (!f["nullMode"].isString())
                {
                    Json::Value details;
                    details["index"] = i;
                    co_return badRequest("filter invalid nullMode", details);
                }
                nullMode = f["nullMode"].asString();
                if (nullMode != "any" && nullMode != "not_null" && nullMode != "null")
                {
                    Json::Value details;
                    details["index"] = i;
                    details["nullMode"] = nullMode;
                    co_return badRequest("unsupported nullMode", details);
                }
            }

            // Семантика:
            // - equals: WHERE col = v1 (v1 обязателен)
            // - range:  WHERE col >= v1 / col <= v2 (обязателен хотя бы один)
            // Здесь мы только валидируем форму, SQL будет строить Service.
            if (op == "equals")
            {
                // Если nullMode != any, это может быть чисто nullable-фильтр (IS NULL / IS NOT NULL),
                // поэтому v1 не обязателен.
                if (nullMode == "any" && !f.isMember("v1"))
                {
                    Json::Value details;
                    details["index"] = i;
                    co_return badRequest("equals filter requires v1", details);
                }
            }
            else if (op == "range")
            {
                const bool hasV1 = f.isMember("v1");
                const bool hasV2 = f.isMember("v2");
                // Если nullMode != any, допускаем отсутствие v1/v2 (тогда это IS NULL / IS NOT NULL).
                if (nullMode == "any" && !hasV1 && !hasV2)
                {
                    Json::Value details;
                    details["index"] = i;
                    co_return badRequest("range filter requires v1 and/or v2", details);
                }
            }
            else
            {
                Json::Value details;
                details["index"] = i;
                details["op"] = op;
                co_return badRequest("unsupported filter op", details);
            }
        }
    }

    // 6) Service слой: выбираем данные из БД, считаем total, применяем фильтры/пагинацию.
    try
    {
        TableDataService service;
        auto page = co_await service.getPage(tableName, filters, offset, limit);

        Json::Value root;
        root["ok"] = true;
        root["data"]["nodeId"] = static_cast<Json::Int64>(nodeId); // 1-based (как пришло)
        root["data"]["table"] = tableName;
        root["data"]["total"] = static_cast<Json::Int64>(page.total);
        root["data"]["offset"] = page.offset;
        root["data"]["limit"] = page.limit;
        root["data"]["returned"] = static_cast<Json::UInt>(page.rows.size());
        root["data"]["rows"] = std::move(page.rows);
        root["data"]["sort"]["by"] = "id";
        root["data"]["sort"]["dir"] = "asc";

        co_return makeJsonResponse(root, k200OK);
    }
    catch (const BadRequestError &e)
    {
        co_return badRequest(e.what());
    }
    catch (const drogon::orm::DrogonDbException &)
    {
        co_return makeJsonResponse(makeErrorObj("internal", "db error"), k500InternalServerError);
    }
    catch (const std::exception &)
    {
        co_return makeJsonResponse(makeErrorObj("internal", "internal error"), k500InternalServerError);
    }
}
