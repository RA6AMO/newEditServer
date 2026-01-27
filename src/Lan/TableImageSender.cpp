#include "Lan/TableImageSender.h"

#ifdef LOG_TRACE
#undef LOG_TRACE
#endif
#ifdef LOG_DEBUG
#undef LOG_DEBUG
#endif
#ifdef LOG_INFO
#undef LOG_INFO
#endif
#ifdef LOG_WARNING
#undef LOG_WARNING
#endif
#ifdef LOG_ERROR
#undef LOG_ERROR
#endif
#ifdef LOG_CRITICAL
#undef LOG_CRITICAL
#endif

#include "Loger/Logger.h"
#include "Storage/MinioPlugin.h"
#include "TableInfoCache.h"

#include <drogon/drogon.h>
#include <drogon/orm/DbClient.h>
#include <drogon/orm/Exception.h>
#include <drogon/utils/Utilities.h>
#include <json/reader.h>
#include <json/writer.h>

#include <algorithm>
#include <cctype>
#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

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

struct ErrorItem
{
    int64_t rowId{};
    std::string dbName;
    std::string code;
    std::string message;
};

bool isSafeIdentifier(const std::string &s)
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

std::string quoteIdent(const std::string &s)
{
    // Мы заранее проверяем isSafeIdentifier, поэтому экранирование не требуется.
    return "\"" + s + "\"";
}

std::string basenameFromKey(const std::string &objectKey)
{
    const auto pos = objectKey.find_last_of('/');
    std::string name = (pos == std::string::npos) ? objectKey : objectKey.substr(pos + 1);
    // Минимальная санитаризация для заголовка Content-Disposition.
    name.erase(std::remove(name.begin(), name.end(), '\r'), name.end());
    name.erase(std::remove(name.begin(), name.end(), '\n'), name.end());
    for (auto &ch : name)
    {
        if (ch == '"')
            ch = '_';
    }
    if (name.empty())
        name = "file";
    return name;
}

std::string buildInList(size_t n, size_t firstIndex = 1)
{
    // "$1,$2,..."
    std::string out;
    out.reserve(n * 3);
    for (size_t i = 0; i < n; ++i)
    {
        if (i)
            out += ",";
        out += "$" + std::to_string(firstIndex + i);
    }
    return out;
}

// Сформировать одну бинарную часть multipart/mixed.
void appendBinaryPart(std::string &body,
                      const std::string &boundary,
                      int64_t rowId,
                      const std::string &dbName,
                      const std::string &mime,
                      const std::string &filename,
                      const std::string &linkName,
                      const std::string &linkUrl,
                      const std::vector<uint8_t> &bytes)
{
    body += "--";
    body += boundary;
    body += "\r\n";

    body += "Content-Type: ";
    body += (mime.empty() ? "application/octet-stream" : mime);
    body += "\r\n";

    body += "Content-Disposition: attachment; filename=\"";
    body += filename;
    body += "\"\r\n";

    body += "X-Row-Id: ";
    body += std::to_string(rowId);
    body += "\r\n";

    body += "X-Db-Name: ";
    body += dbName;
    body += "\r\n";

    if (!linkName.empty())
    {
        body += "X-Link-Name: ";
        body += linkName;
        body += "\r\n";
    }
    if (!linkUrl.empty())
    {
        body += "X-Link-Url: ";
        body += linkUrl;
        body += "\r\n";
    }

    body += "\r\n";
    if (!bytes.empty())
    {
        body.append(reinterpret_cast<const char *>(bytes.data()), bytes.size());
    }
    body += "\r\n";
}

void appendJsonPart(std::string &body, const std::string &boundary, const Json::Value &json)
{
    Json::StreamWriterBuilder w;
    w["indentation"] = "";
    const std::string payload = Json::writeString(w, json);

    body += "--";
    body += boundary;
    body += "\r\n";
    body += "Content-Type: application/json; charset=utf-8\r\n";
    body += "Content-Disposition: inline\r\n";
    body += "\r\n";
    body += payload;
    body += "\r\n";
}

} // namespace

drogon::Task<drogon::HttpResponsePtr> TableImageSender::getTableImages(drogon::HttpRequestPtr req)
{
    using namespace drogon;
    using namespace drogon::orm;

    const std::string peerIp = req ? req->getPeerAddr().toIp() : std::string();

    // 1) Auth (token header)
    const std::string token = req->getHeader("token");
    TokenValidator validator;
    const auto status = co_await validator.check(token, req->getPeerAddr().toIp());
    if (status != TokenValidator::Status::Ok)
    {
        const auto httpCode = TokenValidator::toHttpCode(status);
        const std::string msg = TokenValidator::toError(status);
        const std::string code = (httpCode == k401Unauthorized) ? "unauthorized" : "internal";
        LOG_WARNING(std::string("TableImageSender: auth failed from ") + peerIp + " code=" + code + " message=" + msg);
        co_return makeJsonResponse(makeErrorObj(code, msg), httpCode);
    }

    // 2) Parse JSON body
    Json::Value rootReq;
    {
        const std::string body(req->body());
        if (body.empty())
        {
            LOG_WARNING(std::string("TableImageSender: empty body from ") + peerIp);
            co_return makeJsonResponse(makeErrorObj("bad_request", "Empty request body"), k400BadRequest);
        }
        Json::Reader reader;
        if (!reader.parse(body, rootReq) || !rootReq.isObject())
        {
            LOG_WARNING(std::string("TableImageSender: invalid JSON body from ") + peerIp);
            co_return makeJsonResponse(makeErrorObj("bad_request", "Invalid JSON body"), k400BadRequest);
        }
    }

    if (!rootReq.isMember("nodeId") || !rootReq["nodeId"].isInt())
    {
        LOG_WARNING(std::string("TableImageSender: missing/invalid nodeId from ") + peerIp);
        co_return makeJsonResponse(makeErrorObj("bad_request", "Missing or invalid nodeId"), k400BadRequest);
    }
    if (!rootReq.isMember("small") || !rootReq["small"].isBool())
    {
        LOG_WARNING(std::string("TableImageSender: missing/invalid small from ") + peerIp);
        co_return makeJsonResponse(makeErrorObj("bad_request", "Missing or invalid small"), k400BadRequest);
    }
    if (!rootReq.isMember("ids") || !rootReq["ids"].isArray())
    {
        LOG_WARNING(std::string("TableImageSender: missing/invalid ids from ") + peerIp);
        co_return makeJsonResponse(makeErrorObj("bad_request", "Missing or invalid ids"), k400BadRequest);
    }

    const int nodeId = rootReq["nodeId"].asInt(); // 1-based
    const bool small = rootReq["small"].asBool();
    if (nodeId <= 0 || static_cast<size_t>(nodeId) > kTableNames.size())
    {
        Json::Value details;
        details["expected_range"] =
            "1.." + std::to_string(kTableNames.size() == 0 ? 0 : kTableNames.size());
        LOG_WARNING(std::string("TableImageSender: invalid nodeId from ") + peerIp + " nodeId=" + std::to_string(nodeId));
        co_return makeJsonResponse(makeErrorObj("bad_request", "Invalid nodeId", details), k400BadRequest);
    }

    std::vector<int64_t> rowIds;
    rowIds.reserve(rootReq["ids"].size());
    for (const auto &v : rootReq["ids"])
    {
        if (!v.isInt64() && !v.isUInt64() && !v.isInt())
        {
            LOG_WARNING(std::string("TableImageSender: ids not integer array from ") + peerIp);
            co_return makeJsonResponse(makeErrorObj("bad_request", "ids must be integer array"), k400BadRequest);
        }
        const int64_t id = v.asInt64();
        if (id <= 0)
        {
            LOG_WARNING(std::string("TableImageSender: invalid rowId from ") + peerIp + " rowId=" + std::to_string(id));
            co_return makeJsonResponse(makeErrorObj("bad_request", "rowId must be > 0"), k400BadRequest);
        }
        rowIds.push_back(id);
    }
    if (rowIds.empty())
    {
        LOG_WARNING(std::string("TableImageSender: ids empty from ") + peerIp);
        co_return makeJsonResponse(makeErrorObj("bad_request", "ids is empty"), k400BadRequest);
    }
    std::sort(rowIds.begin(), rowIds.end());
    rowIds.erase(std::unique(rowIds.begin(), rowIds.end()), rowIds.end());

    const std::string baseTable = kTableNames[static_cast<size_t>(nodeId - 1)];
    auto itImages = kTableMinioBySlot.find(baseTable);
    if (itImages == kTableMinioBySlot.end())
    {
        Json::Value details;
        details["table"] = baseTable;
        LOG_WARNING(std::string("TableImageSender: mapping not found baseTable=") + baseTable);
        co_return makeJsonResponse(makeErrorObj("bad_request", "Images table mapping not found", details), k400BadRequest);
    }
    const std::string imagesTable = itImages->second;
    if (!isSafeIdentifier(baseTable) || !isSafeIdentifier(imagesTable))
    {
        LOG_ERROR(std::string("TableImageSender: unsafe identifiers baseTable=") + baseTable + " imagesTable=" + imagesTable);
        co_return makeJsonResponse(makeErrorObj("internal", "Unsafe table identifier"), k500InternalServerError);
    }

    // 3) Resolve image_* columns via TableInfoCache
    std::vector<std::string> imageCols;
    try
    {
        auto cache = app().getPlugin<TableInfoCache>();
        if (!cache)
        {
            LOG_ERROR("TableImageSender: TableInfoCache is not initialized");
            co_return makeJsonResponse(makeErrorObj("internal", "TableInfoCache is not initialized"), k500InternalServerError);
        }
        auto colsPtr = co_await cache->getColumns(baseTable);
        if (!colsPtr || !colsPtr->isArray())
        {
            LOG_ERROR(std::string("TableImageSender: invalid columns from TableInfoCache table=") + baseTable);
            co_return makeJsonResponse(makeErrorObj("internal", "TableInfoCache returned invalid columns"), k500InternalServerError);
        }
        for (const auto &c : *colsPtr)
        {
            if (!c.isObject() || !c.isMember("name") || !c["name"].isString())
                continue;
            const std::string name = c["name"].asString();
            if (name.rfind("image_", 0) == 0 && isSafeIdentifier(name))
            {
                imageCols.push_back(name);
            }
        }
    }
    catch (const std::exception &)
    {
        LOG_ERROR(std::string("TableImageSender: exception while loading columns table=") + baseTable);
        co_return makeJsonResponse(makeErrorObj("internal", "Failed to load table columns"), k500InternalServerError);
    }

    // Если в таблице нет image_* колонок — это не ошибка: просто вернём multipart с пустым errors.
    // (клиент может вызывать эндпоинт для любых таблиц по nodeId)

    // 4) Query baseTable: id + imageCols
    std::unordered_map<int64_t, std::unordered_map<std::string, int64_t>> imageIdByRowAndSlot;
    std::unordered_set<int64_t> existingRows;
    std::unordered_set<int64_t> imageIdsSet;
    std::vector<ErrorItem> errors;

    try
    {
        auto dbClient = app().getDbClient("default");
        std::string sql = "SELECT " + quoteIdent("id");
        for (const auto &col : imageCols)
        {
            sql += ", " + quoteIdent(col);
        }
        sql += " FROM " + quoteIdent("public") + "." + quoteIdent(baseTable) +
               " WHERE " + quoteIdent("id") + " IN (" + buildInList(rowIds.size(), 1) + ")";

        auto binder = (*dbClient << sql);
        for (const auto id : rowIds)
        {
            binder << id;
        }
        const auto result = co_await drogon::orm::internal::SqlAwaiter(std::move(binder));
        for (const auto &r : result)
        {
            const int64_t rid = r["id"].as<int64_t>();
            existingRows.insert(rid);
            for (const auto &col : imageCols)
            {
                const auto f = r[col];
                if (!f.isNull())
                {
                    const int64_t imageId = f.as<int64_t>();
                    imageIdByRowAndSlot[rid][col] = imageId;
                    imageIdsSet.insert(imageId);
                }
            }
        }
    }
    catch (const DrogonDbException &)
    {
        LOG_ERROR(std::string("TableImageSender: db error while querying base table=") + baseTable);
        co_return makeJsonResponse(makeErrorObj("internal", "db error"), k500InternalServerError);
    }

    // rowId не найден в базе -> ошибка-элемент
    for (const auto rid : rowIds)
    {
        if (existingRows.find(rid) == existingRows.end())
        {
            LOG_WARNING(std::string("TableImageSender: row not found table=") + baseTable + " rowId=" + std::to_string(rid));
            errors.push_back(ErrorItem{rid, "", "not_found", "Row not found"});
        }
    }

    // 5) Query imagesTable metadata
    struct ImageMeta
    {
        int64_t id{};
        std::string slot;
        std::string bigObjectKey;
        std::string bigMime;
        std::string smallObjectKey;
        std::string smallMime;
        std::string linkName;
        std::string linkUrl;
    };
    std::unordered_map<int64_t, ImageMeta> metaById;
    if (!imageIdsSet.empty())
    {
        std::vector<int64_t> imageIds(imageIdsSet.begin(), imageIdsSet.end());
        std::sort(imageIds.begin(), imageIds.end());

        try
        {
            auto dbClient = app().getDbClient("default");
            const std::string sql =
                "SELECT id, slot, big_object_key, big_mime_type, small_object_key, small_mime_type, link_name, link_url "
                "FROM " +
                quoteIdent("public") + "." + quoteIdent(imagesTable) +
                " WHERE id IN (" + buildInList(imageIds.size(), 1) + ")";

            auto binder = (*dbClient << sql);
            for (const auto id : imageIds)
            {
                binder << id;
            }
            const auto result = co_await drogon::orm::internal::SqlAwaiter(std::move(binder));
            for (const auto &r : result)
            {
                ImageMeta m;
                m.id = r["id"].as<int64_t>();
                if (!r["slot"].isNull())
                    m.slot = r["slot"].as<std::string>();
                if (!r["big_object_key"].isNull())
                    m.bigObjectKey = r["big_object_key"].as<std::string>();
                if (!r["big_mime_type"].isNull())
                    m.bigMime = r["big_mime_type"].as<std::string>();
                if (!r["small_object_key"].isNull())
                    m.smallObjectKey = r["small_object_key"].as<std::string>();
                if (!r["small_mime_type"].isNull())
                    m.smallMime = r["small_mime_type"].as<std::string>();
                if (!r["link_name"].isNull())
                    m.linkName = r["link_name"].as<std::string>();
                if (!r["link_url"].isNull())
                    m.linkUrl = r["link_url"].as<std::string>();
                metaById.emplace(m.id, std::move(m));
            }
        }
        catch (const DrogonDbException &)
        {
            LOG_ERROR(std::string("TableImageSender: db error while querying images table=") + imagesTable);
            co_return makeJsonResponse(makeErrorObj("internal", "db error"), k500InternalServerError);
        }
    }

    // 6) MinIO fetch + multipart build
    auto minioPlugin = app().getPlugin<MinioPlugin>();
    if (!minioPlugin)
    {
        LOG_ERROR("TableImageSender: MinioPlugin is not initialized");
        co_return makeJsonResponse(makeErrorObj("internal", "MinioPlugin is not initialized"), k500InternalServerError);
    }
    MinioClient &minio = minioPlugin->client();
    const auto &cfg = minioPlugin->minioConfig();
    const std::string bucket = cfg.bucket;

    const std::string boundary = "boundary_" + drogon::utils::getUuid(true);
    std::string multipartBody;
    multipartBody.reserve(1024);

    for (const auto &rowKv : imageIdByRowAndSlot)
    {
        const int64_t rid = rowKv.first;
        for (const auto &slotKv : rowKv.second)
        {
            const std::string &dbName = slotKv.first; // image_* column name == slot
            const int64_t imageId = slotKv.second;

            auto it = metaById.find(imageId);
            if (it == metaById.end())
            {
                LOG_WARNING(std::string("TableImageSender: image meta not found imagesTable=") + imagesTable +
                            " imageId=" + std::to_string(imageId) + " rowId=" + std::to_string(rid) + " dbName=" + dbName);
                errors.push_back(ErrorItem{rid, dbName, "missing_meta", "Image metadata row not found"});
                continue;
            }

            const ImageMeta &m = it->second;
            // Консистентность: slot в *_images должен совпадать с именем image_* колонки
            if (!m.slot.empty() && m.slot != dbName)
            {
                LOG_WARNING(std::string("TableImageSender: slot mismatch rowId=") + std::to_string(rid) +
                            " dbName=" + dbName + " meta.slot=" + m.slot);
                errors.push_back(ErrorItem{rid, dbName, "slot_mismatch", "Image slot mismatch"});
                continue;
            }

            std::string objectKey;
            std::string mime;
            if (small)
            {
                if (m.smallObjectKey.empty())
                {
                    // Вы выбрали: ошибка-элемент, без бинарной части
                    LOG_WARNING(std::string("TableImageSender: missing small_object_key rowId=") + std::to_string(rid) +
                                " dbName=" + dbName + " imageId=" + std::to_string(imageId));
                    errors.push_back(ErrorItem{rid, dbName, "missing_small", "small_object_key is null"});
                    continue;
                }
                objectKey = m.smallObjectKey;
                mime = m.smallMime;
            }
            else
            {
                if (m.bigObjectKey.empty())
                {
                    LOG_WARNING(std::string("TableImageSender: missing big_object_key rowId=") + std::to_string(rid) +
                                " dbName=" + dbName + " imageId=" + std::to_string(imageId));
                    errors.push_back(ErrorItem{rid, dbName, "missing_big", "big_object_key is null"});
                    continue;
                }
                objectKey = m.bigObjectKey;
                mime = m.bigMime;
            }

            std::vector<uint8_t> bytes;
            std::string mimeFromMinio;
            const bool ok = minio.getObject(bucket, objectKey, bytes, &mimeFromMinio);
            if (!ok)
            {
                LOG_ERROR(std::string("TableImageSender: MinIO getObject failed bucket=") + bucket +
                          " key=" + objectKey + " err=" + minio.lastError());
                errors.push_back(ErrorItem{rid, dbName, "storage_error", "Failed to download object from storage"});
                continue;
            }
            if (mime.empty() && !mimeFromMinio.empty())
            {
                mime = mimeFromMinio;
            }

            const std::string filename = basenameFromKey(objectKey);
            appendBinaryPart(multipartBody,
                             boundary,
                             rid,
                             dbName,
                             mime,
                             filename,
                             m.linkName,
                             m.linkUrl,
                             bytes);
        }
    }

    // Финальная JSON-часть с errors
    Json::Value okJson(Json::objectValue);
    okJson["ok"] = true;
    Json::Value errArr(Json::arrayValue);
    for (const auto &e : errors)
    {
        Json::Value item(Json::objectValue);
        item["rowId"] = static_cast<Json::Int64>(e.rowId);
        if (!e.dbName.empty())
            item["dbName"] = e.dbName;
        item["code"] = e.code;
        item["message"] = e.message;
        errArr.append(std::move(item));
    }
    okJson["errors"] = std::move(errArr);
    appendJsonPart(multipartBody, boundary, okJson);

    multipartBody += "--";
    multipartBody += boundary;
    multipartBody += "--\r\n";

    auto resp = HttpResponse::newHttpResponse();
    resp->setStatusCode(k200OK);
    resp->setContentTypeString("multipart/mixed; boundary=" + boundary);
    resp->setBody(std::move(multipartBody));
    co_return resp;
}
