#pragma once

#include "Lan/RowAdd/RowController.h"

#include <cstddef>
#include <drogon/HttpRequest.h>
#include <json/json.h>
#include <string>

/// Логирует `RowController::ParsedRequest` в отдельный pretty-JSON файл.
/// - Бинарные данные вложений (AttachmentInput::data) не пишутся.
/// - Best-effort: при ошибках записи/серилизации метод `log()` возвращает false и не бросает исключений.
///
/// Пример:
/// \code
///     RowController::ParsedRequest parsed = /* после парсинга запроса */;
///     RequestJsonLogger logger;
///     logger.log(parsed); // создаст ./logs/requests/<timestamp>_<uuid>.json
/// \endcode
class RequestJsonLogger
{
public:
    struct Options
    {
        /// Базовая директория для файлов логов (по умолчанию "./logs/requests").
        std::string baseDir = "./logs/requests";

        /// Ограничение на размер JSON-строки payload (в байтах).
        /// 0 = без лимита. Если лимит превышен — логирование пропускается (log() вернёт false).
        std::size_t maxPayloadBytes = 0;

        /// Ограничение на размер body (в байтах) при логировании `HttpRequestPtr`.
        /// 0 = без лимита. Если лимит превышен — логирование пропускается (log() вернёт false).
        std::size_t maxBodyBytes = 0;

        /// Писать ли заголовки при логировании `HttpRequestPtr`.
        bool writeHeaders = true;

        /// Писать ли query string при логировании `HttpRequestPtr`.
        bool writeQuery = true;

        /// Писать ли body при логировании `HttpRequestPtr`.
        bool writeBody = true;

        /// Не писать body для multipart запросов (иначе там будет бинарь).
        bool skipBodyForMultipart = true;

        /// Писать ли метаданные attachments (id/dbName/role/filename/mimeType/sizeBytes).
        bool writeAttachmentsMeta = true;
    };

    /// Конструктор с дефолтными опциями.
    RequestJsonLogger();

    /// Конструктор с пользовательскими опциями.
    explicit RequestJsonLogger(Options opt);

    /// Записать один JSON-файл на запрос.
    bool log(const RowController::ParsedRequest &parsed) noexcept;

    /// Записать один JSON-файл на запрос (сырой HTTP request).
    bool log(const drogon::HttpRequestPtr &req) noexcept;

    /// Преобразовать запрос в Json::Value (без записи на диск).
    static Json::Value toJson(const RowController::ParsedRequest &parsed);

    /// Преобразовать HTTP request в Json::Value (без записи на диск).
    static Json::Value toJson(const drogon::HttpRequestPtr &req);

private:
    Options opt_;
};

