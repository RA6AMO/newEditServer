#include "Lan/RowController.h"
#include "Config/MinioConfig.h"
#include "Storage/MinioClient.h"
#include "Loger/Logger.h"

#include <drogon/drogon.h>
#include <drogon/MultiPart.h>
#include <drogon/orm/DbClient.h>
#include <drogon/orm/Exception.h>
#include <json/reader.h>
#include <json/writer.h>
#include <sstream>
#include <algorithm>
#include <random>
#include <iomanip>

// UUID поддержка (проверяем доступность)
#ifdef __has_include
#if __has_include(<uuid/uuid.h>)
#include <uuid/uuid.h>
#define HAS_UUID 1
#endif
#endif

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

/// Генерация UUID для object key
std::string generateUUID()
{
#ifdef HAS_UUID
    uuid_t uuid;
    uuid_generate_random(uuid);
    char uuid_str[37];
    uuid_unparse_lower(uuid, uuid_str);
    return std::string(uuid_str);
#else
    // Простая генерация UUID-подобной строки, если библиотека недоступна
    static thread_local std::mt19937_64 rng{std::random_device{}()};
    std::uniform_int_distribution<uint32_t> dist(0, 0xFFFFFFFF);
    
    std::ostringstream uuid;
    uuid << std::hex << std::setfill('0');
    uuid << std::setw(8) << dist(rng) << '-';
    uuid << std::setw(4) << (dist(rng) & 0xFFFF) << '-';
    uuid << std::setw(4) << ((dist(rng) & 0x0FFF) | 0x4000) << '-'; // версия 4
    uuid << std::setw(4) << ((dist(rng) & 0x3FFF) | 0x8000) << '-'; // вариант
    uuid << std::setw(8) << dist(rng);
    uuid << std::setw(4) << (dist(rng) & 0xFFFF);
    return uuid.str();
#endif
}

struct BadRequestError : public std::runtime_error
{
    using std::runtime_error::runtime_error;
};
} // namespace

drogon::Task<drogon::HttpResponsePtr> RowController::addRow(drogon::HttpRequestPtr req)
{
    using namespace drogon;
    using namespace drogon::orm;

    try
    {
        // 1. Проверка токена
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

        // 2. Парсинг multipart запроса
        ParsedRequest parsed;
        try
        {
            parsed = parseMultipartRequest(req);
        }
        catch (const std::exception &e)
        {
            co_return makeErrorResponse("bad_request", "Failed to parse multipart request: " + std::string(e.what()), k400BadRequest);
        }

        // 3. Валидация payload
        if (!parsed.payload.isMember("table") || !parsed.payload["table"].isString())
        {
            co_return makeErrorResponse("bad_request", "Missing or invalid 'table' field in payload", k400BadRequest);
        }

        const std::string tableName = parsed.payload["table"].asString();

        // 4. Создание обработчика таблицы
        auto handler = createHandler(tableName);
        if (!handler)
        {
            co_return makeErrorResponse("bad_request", "Table '" + tableName + "' is not supported", k400BadRequest);
        }

        // 5. Получение конфигурации MinIO (вынесено из config.json в код)
        MinioClient::Config minioClientConfig = workshop::config::getMinioConfig();

        MinioClient minioClient(minioClientConfig);

        // 6. Валидация полей
        if (!parsed.payload.isMember("fields") || !parsed.payload.isMember("types"))
        {
            co_return makeErrorResponse("bad_request", "Missing 'fields' or 'types' in payload", k400BadRequest);
        }

        const Json::Value &fields = parsed.payload["fields"];
        const Json::Value &types = parsed.payload["types"];

        std::string validationError = handler->validateFields(fields, types);
        if (!validationError.empty())
        {
            co_return makeErrorResponse("bad_request", validationError, k400BadRequest);
        }

        // 7. Начало транзакции
        auto dbClient = app().getDbClient("default");
        auto transPtr = co_await dbClient->newTransactionCoro();

        int64_t rowId = 0;
        std::vector<std::string> uploadedObjectKeys; // для отката при ошибке
        bool committed = false;
        drogon::HttpResponsePtr finalResp;
        std::string opErrorMessage;

        try
        {
            // 8. INSERT в основную таблицу
            auto [insertQuery, _] = handler->buildInsertQuery(fields, types);
            auto resultRows = co_await transPtr->execSqlCoro(insertQuery);
            if (resultRows.empty())
            {
                throw std::runtime_error("INSERT did not return id");
            }
            rowId = resultRows[0]["id"].as<int64_t>();

            // 9. Предвалидация image-операций до upload (чтобы не коммитить БД при неприлипших attachments)
            const std::string imageTableName = handler->getImageTableName();

            // Если таблица не поддерживает images, но файлы передали — bad_request
            if (imageTableName.empty() && !parsed.attachments.empty())
            {
                throw BadRequestError("Attachments provided, but this table does not support images");
            }

            // Ключи заранее + UPDATE-запрос заранее (до загрузки в MinIO)
            std::map<std::string, std::string> objectKeysMap; // attachment id -> object key
            std::string imagesUpdateQuery;

            if (!parsed.attachments.empty())
            {
                const std::string tablePrefix = handler->getMainTableName();

                for (const auto &attachment : parsed.attachments)
                {
                    const std::string objectKey =
                        generateObjectKey(tablePrefix, rowId, attachment.dbName, attachment.role, attachment.filename);
                    objectKeysMap[attachment.id] = objectKey;
                }

                const Json::Value &meta = parsed.payload.get("meta", Json::objectValue);
                imagesUpdateQuery = handler
                                        ->buildImagesUpdateQuery(
                                            rowId, parsed.attachments, minioClientConfig.bucket, objectKeysMap, meta)
                                        .first;

                // Ключевое: attachments есть, но UPDATE пустой => роли/id не сматчились => 400
                if (imagesUpdateQuery.empty())
                {
                    throw BadRequestError(
                        "Attachments provided, but none can be mapped to image columns (check attachment roles/id)");
                }
            }

            // 10. INSERT в таблицу изображений — только если реально есть файлы
            if (!imageTableName.empty() && !parsed.attachments.empty())
            {
                std::ostringstream imageInsertQuery;
                imageInsertQuery << "INSERT INTO public." << imageTableName << " (tool_id) VALUES (" << rowId << ")";
                co_await transPtr->execSqlCoro(imageInsertQuery.str());
            }

            // 11. Загрузка файлов в MinIO (после успешной предвалидации imagesUpdateQuery)
            for (const auto &attachment : parsed.attachments)
            {
                auto it = objectKeysMap.find(attachment.id);
                if (it == objectKeysMap.end())
                {
                    throw std::runtime_error("Internal: object key not found for attachment id: " + attachment.id);
                }

                const std::string &objectKey = it->second;

                bool uploadSuccess = minioClient.putObject(
                    minioClientConfig.bucket,
                    objectKey,
                    attachment.data,
                    attachment.mimeType);

                if (!uploadSuccess)
                {
                    throw std::runtime_error("Failed to upload file to MinIO: " + attachment.id);
                }

                uploadedObjectKeys.push_back(objectKey);
            }

            // 12. UPDATE таблицы изображений (и image_exists) — только если были вложения
            if (!imagesUpdateQuery.empty())
            {
                co_await transPtr->execSqlCoro(imagesUpdateQuery);

                std::string imageExistsQuery = handler->buildImageExistsUpdateQuery(rowId);
                if (!imageExistsQuery.empty())
                {
                    co_await transPtr->execSqlCoro(imageExistsQuery);
                }
            }

            // 13. COMMIT транзакции
            co_await transPtr->execSqlCoro("COMMIT");
            committed = true;
            finalResp = makeSuccessResponse(rowId);
        }
        catch (const BadRequestError &e)
        {
            opErrorMessage = e.what();
            LOG_WARNING(std::string("addRow bad_request: ") + opErrorMessage);
            finalResp = makeErrorResponse("bad_request", opErrorMessage, k400BadRequest);
        }
        catch (const std::exception &e)
        {
            opErrorMessage = e.what();
            LOG_ERROR(std::string("addRow internal error: ") + opErrorMessage);
            finalResp = makeErrorResponse("internal", "Database or storage error: " + opErrorMessage, k500InternalServerError);
        }
        catch (...)
        {
            opErrorMessage = "unknown error";
            LOG_ERROR("addRow internal error: unknown error");
            finalResp = makeErrorResponse("internal", "Database or storage error: " + opErrorMessage, k500InternalServerError);
        }

        // ВАЖНО: co_await запрещён внутри catch-блоков.
        // Cleanup делаем здесь, уже после обработки исключения.
        // Требование: при любой ошибке ДО завершения транзакции (COMMIT) — удаляем уже загруженные файлы,
        // чтобы MinIO не пополнялась.
        if (!committed)
        {
            // Откат транзакции
            try
            {
                co_await transPtr->execSqlCoro("ROLLBACK");
            }
            catch (...)
            {
                // Игнорируем ошибки отката
            }

            // Удаляем загруженные файлы из MinIO
            for (auto it = uploadedObjectKeys.rbegin(); it != uploadedObjectKeys.rend(); ++it)
            {
                bool deleted = minioClient.deleteObject(minioClientConfig.bucket, *it);
                if (!deleted)
                {
                    LOG_WARNING(std::string("Failed to cleanup MinIO object during rollback: ") + *it);
                }
            }
        }

        co_return finalResp;
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

    // Парсим JSON из поля payload
    Json::Reader reader;
    if (!reader.parse(payloadIt->second, result.payload))
    {
        throw std::runtime_error("Invalid JSON in payload field");
    }

    // Получаем файлы из multipart parser
    auto filesMap = parser.getFilesMap();
    
    // Получаем метаданные attachments из payload
    if (!result.payload.isMember("attachments") || !result.payload["attachments"].isArray())
    {
        throw std::runtime_error("Missing or invalid 'attachments' array in payload");
    }

    const Json::Value &attachmentsArray = result.payload["attachments"];

    // Сопоставляем attachments с файлами
    for (const Json::Value &attObj : attachmentsArray)
    {
        if (!attObj.isMember("id") || !attObj["id"].isString())
        {
            throw std::runtime_error("Attachment missing 'id' field");
        }

        std::string attachmentId = attObj["id"].asString();
        auto fileIt = filesMap.find(attachmentId);
        if (fileIt == filesMap.end())
        {
            throw std::runtime_error("File not found for attachment id: " + attachmentId);
        }

        const drogon::HttpFile &uploadedFile = fileIt->second;

        AttachmentInfo attachment;
        attachment.id = attachmentId;
        attachment.dbName = attObj.get("dbName", "").asString();
        attachment.role = attObj.get("role", "").asString();
        attachment.filename = attObj.get("filename", "").asString();
        attachment.mimeType = attObj.get("mimeType", "").asString();

        // Читаем данные файла
        std::string_view fileContent = uploadedFile.fileContent();
        attachment.data.assign(fileContent.begin(), fileContent.end());

        result.attachments.push_back(std::move(attachment));
    }

    return result;
}

std::unique_ptr<ITableHandler> RowController::createHandler(const std::string &tableName) const
{
    if (tableName == "milling_tool_catalog")
    {
        return std::make_unique<MillingToolCatalogHandler>();
    }
    // Здесь можно добавить другие обработчики для других таблиц
    return nullptr;
}

std::string RowController::generateObjectKey(const std::string &tablePrefix,
                                            int64_t rowId,
                                            const std::string &dbName,
                                            const std::string &role,
                                            const std::string &filename) const
{
    // Генерируем ключ вида: table_name/row_id/dbName_role_uuid.ext
    // Например: milling_tool_catalog/123/image_image_550e8400-e29b-41d4-a716-446655440000.png
    
    std::ostringstream key;
    key << tablePrefix << "/" << rowId << "/" << dbName << "_" << role << "_" << generateUUID();
    
    // Добавляем расширение из оригинального имени файла, если есть
    if (!filename.empty())
    {
        size_t dotPos = filename.find_last_of('.');
        if (dotPos != std::string::npos)
        {
            key << filename.substr(dotPos);
        }
    }
    
    return key.str();
}

drogon::HttpResponsePtr RowController::makeSuccessResponse(int64_t rowId)
{
    Json::Value root;
    root["ok"] = true;
    root["data"]["id"] = static_cast<Json::Int64>(rowId);
    return makeJsonResponse(root, drogon::k200OK);
}

drogon::HttpResponsePtr RowController::makeErrorResponse(const std::string &code,
                                                        const std::string &message,
                                                        drogon::HttpStatusCode status)
{
    return makeJsonResponse(makeErrorObj(code, message), status);
}

