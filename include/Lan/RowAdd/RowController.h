#pragma once

#include "AuthController.h"
#include "Lan/RowWriteTypes.h"

#include <drogon/HttpController.h>
#include <drogon/drogon.h>
#include <drogon/utils/coroutine.h>
#include <json/json.h>
#include <string>

/// Минимальный контроллер для создания записи.
/// На текущем этапе содержит только базовые проверки (token, формат payload).
class RowController : public drogon::HttpController<RowController>
{
public:
    METHOD_LIST_BEGIN
    ADD_METHOD_TO(RowController::addRow, "/row/addRow", drogon::Post);
    METHOD_LIST_END

    /// Парсинг запроса: извлечение JSON payload.
    /// Вынесено в public, чтобы бизнес-слои (SQL/MinIO) могли принимать "parsed" после валидации.
    struct ParsedRequest
    {
        Json::Value payload; // JSON из поля "payload" (multipart) или из body (application/json)
        std::vector<AttachmentInput> attachments;
    };

    /// Основной метод обработки создания записи
    drogon::Task<drogon::HttpResponsePtr> addRow(drogon::HttpRequestPtr req);

private:
    /// Проверить, что payload соответствует ожидаемой таблице/схеме (whitelist + колонки из information_schema).
    /// Бросает исключение при проблемах.
    drogon::Task<void> customer_table_validation(const ParsedRequest &parsed) const;

    /// Распарсить запрос и получить payload
    /// @return ParsedRequest или выбрасывает исключение при ошибке формата/JSON
    ParsedRequest parseMultipartRequest(drogon::HttpRequestPtr req) const;

    /// Создать успешный JSON ответ
    static drogon::HttpResponsePtr makeSuccessResponse(int64_t rowId,
                                                       const Json::Value &dataExtra = Json::nullValue);

    /// Создать JSON ответ с ошибкой
    static drogon::HttpResponsePtr makeErrorResponse(const std::string &code,
                                                    const std::string &message,
                                                    drogon::HttpStatusCode status);
};
