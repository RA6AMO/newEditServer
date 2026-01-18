#include "Lan/RowAdd/RowController.h"
#include "Loger/Logger.h"
#include "Lan/RowAdd/RowWriteService.h"

#include <drogon/drogon.h>
#include <drogon/MultiPart.h>
#include <json/reader.h>
#include <json/writer.h>
#include <sstream>
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

        // 2) Базовая проверка формата: извлекаем JSON payload и attachments
        ParsedRequest parsed;
        try
        {
            parsed = parseMultipartRequest(req);
        }
        catch (const std::exception &e)
        {
            co_return makeErrorResponse("bad_request", "Failed to parse request payload: " + std::string(e.what()), k400BadRequest);
        }

        // 3) Минимальная проверка payload: он должен быть JSON-объектом
        if (!parsed.payload.isObject())
        {
            co_return makeErrorResponse("bad_request", "Invalid payload: expected JSON object", k400BadRequest);
        }

        // 4) Универсальная запись строки (DB + storage) через сервис
        try
        {
            RowWriteService writer;
            const WriteResult result = co_await writer.write(parsed);
            co_return makeSuccessResponse(result.rowId, result.extra);
        }
        catch (const RowWriteError &e)
        {
            co_return makeJsonResponse(makeErrorObj(e.code(), e.what(), e.details()), e.status());
        }

    }
    catch (const std::exception &e)
    {
        LOG_ERROR(std::string("addRow fatal error: ") + e.what());
        co_return makeErrorResponse("internal", "Internal error: " + std::string(e.what()), k500InternalServerError);
    }
}

RowController::ParsedRequest RowController::parseMultipartRequest(drogon::HttpRequestPtr req) const
{
    ParsedRequest result;

    // 1) Тело с files+payload
    if (req->contentType() == drogon::CT_MULTIPART_FORM_DATA)
    {
        drogon::MultiPartParser parser;
        if (parser.parse(req) != 0)
        {
            throw std::runtime_error("Failed to parse request body");
        }

        const auto &params = parser.getParameters();
        auto payloadIt = params.find("payload");
        if (payloadIt == params.end())
        {
            throw std::runtime_error("Missing 'payload' field in request");
        }

        Json::Reader reader;
        if (!reader.parse(payloadIt->second, result.payload))
        {
            throw std::runtime_error("Invalid JSON in payload field");
        }

        const auto filesMap = parser.getFilesMap();
        if (result.payload.isObject() && result.payload.isMember("attachments"))
        {
            const Json::Value &attachments = result.payload["attachments"];
            if (!attachments.isArray())
            {
                throw std::runtime_error("Invalid payload: attachments must be array");
            }
            if (attachments.empty())
            {
                return result;
            }
            if (filesMap.empty())
            {
                throw std::runtime_error("Invalid payload: attachments without file parts");
            }

            std::unordered_set<std::string> seen;
            for (const auto &att : attachments)
            {
                if (!att.isObject())
                {
                    throw std::runtime_error("Invalid payload: attachment item must be object");
                }
                if (!att.isMember("id") || !att["id"].isString())
                {
                    throw std::runtime_error("Invalid payload: attachment.id is required");
                }
                if (!att.isMember("dbName") || !att["dbName"].isString())
                {
                    throw std::runtime_error("Invalid payload: attachment.dbName is required");
                }
                if (!att.isMember("role") || !att["role"].isString())
                {
                    throw std::runtime_error("Invalid payload: attachment.role is required");
                }

                const std::string id = att["id"].asString();
                auto fileIt = filesMap.find(id);
                if (fileIt == filesMap.end())
                {
                    throw std::runtime_error("Missing file part for attachment id: " + id);
                }

                AttachmentInput input;
                input.id = id;
                input.dbName = att["dbName"].asString();
                input.role = att["role"].asString();
                if (att.isMember("filename") && att["filename"].isString())
                {
                    input.filename = att["filename"].asString();
                }
                if (input.filename.empty())
                {
                    input.filename = fileIt->second.getFileName();
                }
                if (att.isMember("mimeType") && att["mimeType"].isString())
                {
                    input.mimeType = att["mimeType"].asString();
                }

                const auto content = fileIt->second.fileContent();
                input.data.assign(content.begin(), content.end());
                result.attachments.push_back(std::move(input));
                seen.insert(id);
            }

            for (const auto &kv : filesMap)
            {
                if (seen.find(kv.first) == seen.end())
                {
                    throw std::runtime_error("Unexpected file part without payload attachment: " + kv.first);
                }
            }
        }
        return result;
    }


    // 2) application/json (без файлов)
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
    if (result.payload.isObject() && result.payload.isMember("attachments"))
    {
        throw std::runtime_error("Invalid payload: attachments require file parts");
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

