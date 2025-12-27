#include "Lan/TableHandler.h"

#include <sstream>
#include <stdexcept>
#include <algorithm>

namespace
{
/// Экранирование строки для SQL (простая версия, для чисел и NULL использовать параметры!)
std::string sqlEscape(const std::string &str)
{
    std::string result;
    result.reserve(str.length() + 10);
    for (char c : str)
    {
        if (c == '\'')
        {
            result += "''";
        }
        else if (c == '\\')
        {
            result += "\\\\";
        }
        else
        {
            result += c;
        }
    }
    return result;
}

/// Преобразование JSON типа в SQL значение
std::string jsonValueToSql(const Json::Value &value, const std::string &type)
{
    if (value.isNull())
    {
        return "NULL";
    }

    if (type == "Integer")
    {
        if (value.isInt() || value.isInt64())
        {
            return std::to_string(value.asInt64());
        }
        if (value.isString())
        {
            // Попытка парсинга строки как числа
            try
            {
                return std::to_string(std::stoll(value.asString()));
            }
            catch (...)
            {
                throw std::runtime_error("Cannot convert to Integer: " + value.asString());
            }
        }
        throw std::runtime_error("Invalid Integer value");
    }

    if (type == "Double")
    {
        if (value.isNumeric())
        {
            return std::to_string(value.asDouble());
        }
        if (value.isString())
        {
            try
            {
                return std::to_string(std::stod(value.asString()));
            }
            catch (...)
            {
                throw std::runtime_error("Cannot convert to Double: " + value.asString());
            }
        }
        throw std::runtime_error("Invalid Double value");
    }

    if (type == "Boolean")
    {
        if (value.isBool())
        {
            return value.asBool() ? "TRUE" : "FALSE";
        }
        if (value.isString())
        {
            std::string str = value.asString();
            // Приводим к нижнему регистру для сравнения
            std::transform(str.begin(), str.end(), str.begin(), ::tolower);
            if (str == "true" || str == "1" || str == "yes" || str == "on")
            {
                return "TRUE";
            }
            if (str == "false" || str == "0" || str == "no" || str == "off")
            {
                return "FALSE";
            }
            throw std::runtime_error("Cannot convert to Boolean: " + value.asString());
        }
        if (value.isInt() || value.isInt64())
        {
            return value.asInt64() != 0 ? "TRUE" : "FALSE";
        }
        throw std::runtime_error("Invalid Boolean value");
    }

    if (type == "String" || type == "Link" || type == "File" || type == "FileWithLink" || type == "Folder")
    {
        return "'" + sqlEscape(value.asString()) + "'";
    }

    if (type == "Date")
    {
        std::string dateStr = value.asString();
        // ISO 8601 формат, PostgreSQL принимает напрямую
        return "'" + sqlEscape(dateStr) + "'";
    }

    // Для остальных типов - как строка (но в реальности они не должны попадать сюда)
    return "'" + sqlEscape(value.asString()) + "'";
}
} // namespace

void MillingToolCatalogHandler::buildColumnsAndValues(const Json::Value &fields,
                                                      const Json::Value &types,
                                                      std::vector<std::string> &columns,
                                                      std::vector<std::string> &values) const
{
    columns.clear();
    values.clear();

    // Проходим по всем полям в types (кроме id, он генерируется автоматически)
    for (const auto &typeKey : types.getMemberNames())
    {
        if (typeKey == "id")
        {
            continue; // id генерируется автоматически (BIGSERIAL)
        }

        const std::string dbName = typeKey;
        const std::string type = types[dbName].asString();

        // Пропускаем типы, которые не сохраняются в основную таблицу
        if (type == "Image" || type == "ImageWithLink")
        {
            continue; // Изображения сохраняются в отдельную таблицу
        }

        if (type == "ButtonDelegate" || type == "CustomDelegate")
        {
            continue; // Эти типы не сохраняются
        }

        columns.push_back(dbName);

        if (fields.isMember(dbName))
        {
            const Json::Value &fieldValue = fields[dbName];
            if (fieldValue.isNull())
            {
                values.push_back("NULL");
            }
            else
            {
                values.push_back(jsonValueToSql(fieldValue, type));
            }
        }
        else
        {
            values.push_back("NULL");
        }
    }
}

std::pair<std::string, std::vector<std::string>>
MillingToolCatalogHandler::buildInsertQuery(const Json::Value &fields, const Json::Value &types) const
{
    std::vector<std::string> columns;
    std::vector<std::string> values;

    buildColumnsAndValues(fields, types, columns, values);

    if (columns.empty())
    {
        throw std::runtime_error("No columns to insert");
    }

    std::ostringstream query;
    query << "INSERT INTO public.milling_tool_catalog (";
    for (size_t i = 0; i < columns.size(); ++i)
    {
        if (i > 0)
            query << ", ";
        query << columns[i];
    }
    query << ") VALUES (";
    for (size_t i = 0; i < values.size(); ++i)
    {
        if (i > 0)
            query << ", ";
        query << values[i];
    }
    query << ") RETURNING id";

    // Для этой версии мы не используем параметризованные запросы
    // (это можно улучшить позже)
    return {query.str(), {}};
}

std::string MillingToolCatalogHandler::getImageTableName() const
{
    return "milling_tool_images";
}

std::pair<std::string, std::vector<std::string>>
MillingToolCatalogHandler::buildImagesUpdateQuery(int64_t rowId,
                                                  const std::vector<AttachmentInfo> &attachments,
                                                  const std::string &bucket,
                                                  const std::map<std::string, std::string> &objectKeysMap,
                                                  const Json::Value &meta) const
{
    // Собираем SET части для UPDATE
    std::vector<std::string> setParts;

    // Обрабатываем attachments: ищем big (role="image") и small (role="image_small")
    std::string bigBucket, bigKey, bigMime, smallBucket, smallKey, smallMime;
    int64_t bigSize = 0, smallSize = 0;

    for (const auto &att : attachments)
    {
        if (att.role == "image")
        {
            auto it = objectKeysMap.find(att.id);
            if (it != objectKeysMap.end())
            {
                bigBucket = bucket;
                bigKey = it->second;
                bigMime = att.mimeType;
                bigSize = static_cast<int64_t>(att.data.size());
            }
        }
        else if (att.role == "image_small")
        {
            auto it = objectKeysMap.find(att.id);
            if (it != objectKeysMap.end())
            {
                smallBucket = bucket;
                smallKey = it->second;
                smallMime = att.mimeType;
                smallSize = static_cast<int64_t>(att.data.size());
            }
        }
    }

    // Формируем SET части
    if (!bigBucket.empty())
    {
        setParts.push_back("big_bucket = '" + sqlEscape(bigBucket) + "'");
    }
    if (!bigKey.empty())
    {
        setParts.push_back("big_object_key = '" + sqlEscape(bigKey) + "'");
    }
    if (!bigMime.empty())
    {
        setParts.push_back("big_mime_type = '" + sqlEscape(bigMime) + "'");
    }
    if (bigSize > 0)
    {
        setParts.push_back("big_size_bytes = " + std::to_string(bigSize));
    }

    if (!smallBucket.empty())
    {
        setParts.push_back("small_bucket = '" + sqlEscape(smallBucket) + "'");
    }
    if (!smallKey.empty())
    {
        setParts.push_back("small_object_key = '" + sqlEscape(smallKey) + "'");
    }
    if (!smallMime.empty())
    {
        setParts.push_back("small_mime_type = '" + sqlEscape(smallMime) + "'");
    }
    if (smallSize > 0)
    {
        setParts.push_back("small_size_bytes = " + std::to_string(smallSize));
    }

    // Метаданные для ImageWithLink (link_name, link_url)
    if (meta.isMember("imageMeta"))
    {
        const Json::Value &imageMeta = meta["imageMeta"];
        // Предполагаем, что в imageMeta только один ключ (dbName колонки с изображением)
        if (!imageMeta.getMemberNames().empty())
        {
            const std::string dbName = imageMeta.getMemberNames()[0];
            const Json::Value &metaObj = imageMeta[dbName];
            if (metaObj.isMember("name") && metaObj["name"].isString())
            {
                setParts.push_back("link_name = '" + sqlEscape(metaObj["name"].asString()) + "'");
            }
            if (metaObj.isMember("link") && metaObj["link"].isString())
            {
                setParts.push_back("link_url = '" + sqlEscape(metaObj["link"].asString()) + "'");
            }
        }
    }

    if (setParts.empty())
    {
        // Нет данных для обновления, просто возвращаем пустой запрос
        return {"", {}};
    }

    std::ostringstream query;
    query << "UPDATE public.milling_tool_images SET ";
    for (size_t i = 0; i < setParts.size(); ++i)
    {
        if (i > 0)
            query << ", ";
        query << setParts[i];
    }
    query << ", updated_at = now() WHERE tool_id = " << rowId;

    return {query.str(), {}};
}

std::string MillingToolCatalogHandler::validateFields(const Json::Value &fields, const Json::Value &types) const
{
    (void)types;
    // Базовая валидация: проверка обязательных полей
    // Для milling_tool_catalog обязательным является только name
    if (!fields.isMember("name") || fields["name"].isNull() || !fields["name"].isString())
    {
        return "Field 'name' is required and must be a string";
    }

    std::string name = fields["name"].asString();
    if (name.empty())
    {
        return "Field 'name' cannot be empty";
    }

    return ""; // Валидация пройдена
}

std::string MillingToolCatalogHandler::getMainTableName() const
{
    return "milling_tool_catalog";
}

std::string MillingToolCatalogHandler::buildImageExistsUpdateQuery(int64_t rowId) const
{
    return "UPDATE public.milling_tool_catalog SET image_exists = TRUE WHERE id = " + std::to_string(rowId);
}
