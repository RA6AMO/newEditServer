#pragma once

#include <string>
#include <map>
#include <vector>
#include <json/json.h>

/// Структура для описания вложения (файла)
struct AttachmentInfo
{
    std::string id;         // например "file_0"
    std::string dbName;     // имя колонки в БД (например "image")
    std::string role;       // "image" или "image_small"
    std::string filename;   // оригинальное имя файла
    std::string mimeType;   // MIME-тип
    std::vector<uint8_t> data; // данные файла
};

/// Интерфейс для обработчиков разных таблиц
/// Позволяет расширять систему для работы с разными таблицами
class ITableHandler
{
public:
    virtual ~ITableHandler() = default;

    /// Построить SQL запрос INSERT для основной таблицы
    /// @param fields JSON объект с полями
    /// @param types JSON объект с типами полей (dbName -> type)
    /// @param placeholders вектор для заполнения параметрами ($1, $2, ...)
    /// @return SQL строка и вектор значений для подстановки
    virtual std::pair<std::string, std::vector<std::string>>
    buildInsertQuery(const Json::Value &fields, const Json::Value &types) const = 0;

    /// Получить имя таблицы изображений (1:1 связь)
    /// @return имя таблицы или пустая строка, если таблица изображений не нужна
    virtual std::string getImageTableName() const = 0;

    /// Построить SQL запрос UPDATE для таблицы изображений
    /// @param rowId ID созданной записи в основной таблице
    /// @param attachments список вложений с данными
    /// @param bucket имя bucket в MinIO
    /// @param objectKeysMap карта role -> objectKey для каждого вложения
    /// @param meta метаданные (например, для ImageWithLink: name, link)
    /// @return SQL строка и вектор значений для подстановки
    virtual std::pair<std::string, std::vector<std::string>>
    buildImagesUpdateQuery(int64_t rowId,
                          const std::vector<AttachmentInfo> &attachments,
                          const std::string &bucket,
                          const std::map<std::string, std::string> &objectKeysMap,
                          const Json::Value &meta) const = 0;

    /// Валидация полей перед вставкой
    /// @param fields JSON объект с полями
    /// @param types JSON объект с типами
    /// @return пустая строка при успехе, иначе сообщение об ошибке
    virtual std::string validateFields(const Json::Value &fields, const Json::Value &types) const = 0;

    /// Получить имя основной таблицы
    virtual std::string getMainTableName() const = 0;

    /// Построить SQL запрос для UPDATE image_exists = TRUE
    /// @param rowId ID записи
    /// @return SQL строка (пустая, если не нужно обновлять)
    virtual std::string buildImageExistsUpdateQuery(int64_t rowId) const = 0;
};

/// Реализация для таблицы milling_tool_catalog
class MillingToolCatalogHandler : public ITableHandler
{
public:
    std::pair<std::string, std::vector<std::string>>
    buildInsertQuery(const Json::Value &fields, const Json::Value &types) const override;

    std::string getImageTableName() const override;

    std::pair<std::string, std::vector<std::string>>
    buildImagesUpdateQuery(int64_t rowId,
                          const std::vector<AttachmentInfo> &attachments,
                          const std::string &bucket,
                          const std::map<std::string, std::string> &objectKeysMap,
                          const Json::Value &meta) const override;

    std::string validateFields(const Json::Value &fields, const Json::Value &types) const override;

    std::string getMainTableName() const override;

    std::string buildImageExistsUpdateQuery(int64_t rowId) const override;

private:
    /// Сформировать список колонок и значений для INSERT
    void buildColumnsAndValues(const Json::Value &fields,
                              const Json::Value &types,
                              std::vector<std::string> &columns,
                              std::vector<std::string> &values) const;
};

