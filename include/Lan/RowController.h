#pragma once

#include "AuthController.h"
#include "TableHandler.h"

#include <drogon/HttpController.h>
#include <drogon/drogon.h>
#include <json/json.h>
#include <memory>
#include <string>
#include <vector>

/// Контроллер для создания записей с файлами
/// Обрабатывает POST /row/addRow с multipart/form-data
class RowController : public drogon::HttpController<RowController>
{
public:
    METHOD_LIST_BEGIN
    ADD_METHOD_TO(RowController::addRow, "/row/addRow", drogon::Post);
    METHOD_LIST_END

    /// Основной метод обработки создания записи
    drogon::Task<drogon::HttpResponsePtr> addRow(drogon::HttpRequestPtr req);

private:
    /// Парсинг multipart запроса: извлечение JSON payload и файлов
    struct ParsedRequest
    {
        Json::Value payload;                    // JSON из поля "payload"
        std::vector<AttachmentInfo> attachments; // файлы, сопоставленные с метаданными
    };

    /// Распарсить multipart/form-data запрос
    /// @return ParsedRequest или выбрасывает исключение при ошибке
    ParsedRequest parseMultipartRequest(drogon::HttpRequestPtr req) const;

    /// Создать обработчик для таблицы
    /// @param tableName имя таблицы
    /// @return указатель на обработчик или nullptr, если таблица не поддерживается
    std::unique_ptr<ITableHandler> createHandler(const std::string &tableName) const;

    /// Генерация object key для MinIO
    /// @param rowId ID записи
    /// @param dbName имя колонки
    /// @param role роль файла (image/image_small)
    /// @param filename оригинальное имя файла
    /// @return object key (например: "milling_tool_catalog/123/image.png")
    std::string generateObjectKey(int64_t rowId,
                                  const std::string &dbName,
                                  const std::string &role,
                                  const std::string &filename) const;

    /// Создать успешный JSON ответ
    static drogon::HttpResponsePtr makeSuccessResponse(int64_t rowId);

    /// Создать JSON ответ с ошибкой
    static drogon::HttpResponsePtr makeErrorResponse(const std::string &code,
                                                    const std::string &message,
                                                    drogon::HttpStatusCode status);
};

