#include "Lan/TableImageSender.h"

#include <drogon/drogon.h>
#include <drogon/orm/DbClient.h>
#include <drogon/orm/Exception.h>
#include <drogon/utils/Utilities.h>
#include <json/reader.h>
#include <json/writer.h>

#include "Storage/MinioPlugin.h"
#include "TableInfoCache.h"

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

#include <algorithm>
#include <cctype>
#include <limits>
#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace
{
Json::Value makeErrorMessage(const std::string &message)
{
    Json::Value root;
    root["ok"] = false;
    root["error"]["message"] = message;
    return root;
}

drogon::HttpResponsePtr makeJsonResponse(const Json::Value &body, drogon::HttpStatusCode status)
{
    auto resp = drogon::HttpResponse::newHttpJsonResponse(body);
    resp->setStatusCode(status);
    return resp;
}

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

std::string sanitizeHeaderValue(std::string value)
{
    value.erase(std::remove(value.begin(), value.end(), '\r'), value.end());
    value.erase(std::remove(value.begin(), value.end(), '\n'), value.end());
    return value;
}

std::optional<uint64_t> parseRowId(const Json::Value &val)
{
    if (val.isUInt64())
    {
        return val.asUInt64();
    }
    if (val.isInt64())
    {
        const auto v = val.asInt64();
        if (v > 0)
        {
            return static_cast<uint64_t>(v);
        }
        return std::nullopt;
    }
    if (val.isInt())
    {
        const auto v = val.asInt();
        if (v > 0)
        {
            return static_cast<uint64_t>(v);
        }
        return std::nullopt;
    }
    if (val.isString())
    {
        const std::string s = val.asString();
        if (s.empty())
        {
            return std::nullopt;
        }
        for (const unsigned char c : s)
        {
            if (c < '0' || c > '9')
            {
                return std::nullopt;
            }
        }
        try
        {
            const unsigned long long v = std::stoull(s);
            if (v == 0ULL)
            {
                return std::nullopt;
            }
            return static_cast<uint64_t>(v);
        }
        catch (const std::exception &)
        {
            return std::nullopt;
        }
    }
    return std::nullopt;
}

std::string inferImageMime(const std::string &objectKey)
{
    const auto dot = objectKey.find_last_of('.');
    if (dot == std::string::npos || dot + 1 >= objectKey.size())
    {
        return "image/*";
    }
    std::string ext = objectKey.substr(dot + 1);
    std::transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    if (ext == "jpg" || ext == "jpeg")
        return "image/jpeg";
    if (ext == "png")
        return "image/png";
    if (ext == "webp")
        return "image/webp";
    if (ext == "gif")
        return "image/gif";
    if (ext == "bmp")
        return "image/bmp";
    if (ext == "tif" || ext == "tiff")
        return "image/tiff";
    return "image/*";
}

std::string normalizeImageMime(const std::string &mime, const std::string &objectKey)
{
    if (!mime.empty())
    {
        return mime;
    }
    return inferImageMime(objectKey);
}

// Сформировать одну бинарную часть multipart/mixed.
void appendBinaryPart(std::string &body,
                      const std::string &boundary,
                      int64_t rowId,
                      const std::string &dbName,
                      const std::string &mime,
                      const std::string &filename,
                      const std::string &reason,
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

    if (!reason.empty())
    {
        body += "X-Reason: ";
        body += reason;
        body += "\r\n";
    }

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
        co_return makeJsonResponse(makeErrorMessage(msg), httpCode);
    }

    // 2) Parse JSON body
    Json::Value rootReq;
    {
        const std::string body(req->body());
        if (body.empty())
        {
            LOG_WARNING(std::string("TableImageSender: empty body from ") + peerIp);
            co_return makeJsonResponse(makeErrorMessage("Empty request body"), k400BadRequest);
        }
        Json::Reader reader;
        if (!reader.parse(body, rootReq) || !rootReq.isObject())
        {
            LOG_WARNING(std::string("TableImageSender: invalid JSON body from ") + peerIp);
            co_return makeJsonResponse(makeErrorMessage("Invalid JSON body"), k400BadRequest);
        }
    }

    if (!rootReq.isMember("nodeId") || !rootReq["nodeId"].isInt())
    {
        LOG_WARNING(std::string("TableImageSender: missing/invalid nodeId from ") + peerIp);
        co_return makeJsonResponse(makeErrorMessage("Missing or invalid nodeId"), k400BadRequest);
    }
    if (!rootReq.isMember("small") || !rootReq["small"].isBool())
    {
        LOG_WARNING(std::string("TableImageSender: missing/invalid small from ") + peerIp);
        co_return makeJsonResponse(makeErrorMessage("Missing or invalid small"), k400BadRequest);
    }
    if (!rootReq.isMember("rowId"))
    {
        LOG_WARNING(std::string("TableImageSender: missing rowId from ") + peerIp);
        co_return makeJsonResponse(makeErrorMessage("Missing rowId"), k400BadRequest);
    }
    if (!rootReq.isMember("dbName") || !rootReq["dbName"].isString())
    {
        LOG_WARNING(std::string("TableImageSender: missing/invalid dbName from ") + peerIp);
        co_return makeJsonResponse(makeErrorMessage("Missing or invalid dbName"), k400BadRequest);
    }

    const int nodeId = rootReq["nodeId"].asInt(); // 1-based
    const bool small = rootReq["small"].asBool();
    if (nodeId <= 0)
    {
        LOG_WARNING(std::string("TableImageSender: invalid nodeId from ") + peerIp + " nodeId=" + std::to_string(nodeId));
        co_return makeJsonResponse(makeErrorMessage("Invalid nodeId"), k400BadRequest);
    }

    const auto rowIdOpt = parseRowId(rootReq["rowId"]);
    if (!rowIdOpt.has_value())
    {
        LOG_WARNING(std::string("TableImageSender: invalid rowId from ") + peerIp);
        co_return makeJsonResponse(makeErrorMessage("Invalid rowId"), k400BadRequest);
    }
    const uint64_t rowIdU = *rowIdOpt;
    if (rowIdU > static_cast<uint64_t>(std::numeric_limits<int64_t>::max()))
    {
        LOG_WARNING(std::string("TableImageSender: rowId too large from ") + peerIp + " rowId=" + std::to_string(rowIdU));
        co_return makeJsonResponse(makeErrorMessage("rowId is out of range"), k400BadRequest);
    }
    const int64_t rowId = static_cast<int64_t>(rowIdU);

    const std::string dbName = rootReq["dbName"].asString();
    if (dbName.empty() || dbName.rfind("image_", 0) != 0 || !isSafeIdentifier(dbName))
    {
        LOG_WARNING(std::string("TableImageSender: invalid dbName from ") + peerIp + " dbName=" + dbName);
        co_return makeJsonResponse(makeErrorMessage("Invalid dbName"), k400BadRequest);
    }
    std::string reason;
    if (rootReq.isMember("reason") && rootReq["reason"].isString())
    {
        reason = sanitizeHeaderValue(rootReq["reason"].asString());
    }

    std::string baseTable;
    if (!tryGetTableNameById(nodeId, baseTable))
    {
        LOG_WARNING(std::string("TableImageSender: invalid nodeId from ") + peerIp + " nodeId=" + std::to_string(nodeId));
        co_return makeJsonResponse(makeErrorMessage("Invalid nodeId"), k400BadRequest);
    }
    baseTable = resolveBaseTable(baseTable);
    auto itImages = kTableMinioBySlot.find(baseTable);
    if (itImages == kTableMinioBySlot.end())
    {
        LOG_WARNING(std::string("TableImageSender: mapping not found baseTable=") + baseTable);
        co_return makeJsonResponse(makeErrorMessage("Images table mapping not found"), k400BadRequest);
    }
    const std::string imagesTable = itImages->second;
    if (!isSafeIdentifier(baseTable) || !isSafeIdentifier(imagesTable))
    {
        LOG_ERROR(std::string("TableImageSender: unsafe identifiers baseTable=") + baseTable + " imagesTable=" + imagesTable);
        co_return makeJsonResponse(makeErrorMessage("Unsafe table identifier"), k500InternalServerError);
    }

    // 3) Validate dbName via TableInfoCache
    bool dbNameFound = false;
    try
    {
        auto cache = app().getPlugin<TableInfoCache>();
        if (!cache)
        {
            LOG_ERROR("TableImageSender: TableInfoCache is not initialized");
            co_return makeJsonResponse(makeErrorMessage("TableInfoCache is not initialized"), k500InternalServerError);
        }
        auto colsPtr = co_await cache->getColumns(baseTable);
        if (!colsPtr || !colsPtr->isArray())
        {
            LOG_ERROR(std::string("TableImageSender: invalid columns from TableInfoCache table=") + baseTable);
            co_return makeJsonResponse(makeErrorMessage("TableInfoCache returned invalid columns"), k500InternalServerError);
        }
        for (const auto &c : *colsPtr)
        {
            if (!c.isObject() || !c.isMember("name") || !c["name"].isString())
                continue;
            const std::string name = c["name"].asString();
            if (name.rfind("image_", 0) == 0 && isSafeIdentifier(name))
            {
                if (name == dbName)
                {
                    dbNameFound = true;
                    break;
                }
            }
        }
    }
    catch (const std::exception &)
    {
        LOG_ERROR(std::string("TableImageSender: exception while loading columns table=") + baseTable);
        co_return makeJsonResponse(makeErrorMessage("Failed to load table columns"), k500InternalServerError);
    }
    if (!dbNameFound)
    {
        LOG_WARNING(std::string("TableImageSender: dbName not found in table=") + baseTable + " dbName=" + dbName);
        co_return makeJsonResponse(makeErrorMessage("dbName is not an image column"), k400BadRequest);
    }

    // 4) Query baseTable: id + dbName
    int64_t imageId = 0;
    try
    {
        auto dbClient = app().getDbClient("default");
        std::string sql = "SELECT " + quoteIdent("id") + ", " + quoteIdent(dbName) +
                          " FROM " + quoteIdent("public") + "." + quoteIdent(baseTable) +
                          " WHERE " + quoteIdent("id") + " = $1";
        auto binder = (*dbClient << sql);
        binder << rowId;
        const auto result = co_await drogon::orm::internal::SqlAwaiter(std::move(binder));
        if (result.empty())
        {
            LOG_WARNING(std::string("TableImageSender: row not found table=") + baseTable + " rowId=" + std::to_string(rowId));
            co_return makeJsonResponse(makeErrorMessage("Row not found"), k404NotFound);
        }
        const auto &r = result[0];
        const auto f = r[dbName];
        if (f.isNull())
        {
            LOG_WARNING(std::string("TableImageSender: image id is null rowId=") + std::to_string(rowId) + " dbName=" + dbName);
            co_return makeJsonResponse(makeErrorMessage("Image not found"), k404NotFound);
        }
        imageId = f.as<int64_t>();
        if (imageId <= 0)
        {
            LOG_WARNING(std::string("TableImageSender: invalid image id rowId=") + std::to_string(rowId) + " dbName=" + dbName);
            co_return makeJsonResponse(makeErrorMessage("Image not found"), k404NotFound);
        }
    }
    catch (const DrogonDbException &)
    {
        LOG_ERROR(std::string("TableImageSender: db error while querying base table=") + baseTable);
        co_return makeJsonResponse(makeErrorMessage("db error"), k500InternalServerError);
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
    ImageMeta meta;
    bool metaFound = false;
    try
    {
        auto dbClient = app().getDbClient("default");
        const std::string sql =
            "SELECT id, slot, big_object_key, big_mime_type, small_object_key, small_mime_type, link_name, link_url "
            "FROM " +
            quoteIdent("public") + "." + quoteIdent(imagesTable) +
            " WHERE id = $1";
        auto binder = (*dbClient << sql);
        binder << imageId;
        const auto result = co_await drogon::orm::internal::SqlAwaiter(std::move(binder));
        if (!result.empty())
        {
            const auto &r = result[0];
            meta.id = r["id"].as<int64_t>();
            if (!r["slot"].isNull())
                meta.slot = r["slot"].as<std::string>();
            if (!r["big_object_key"].isNull())
                meta.bigObjectKey = r["big_object_key"].as<std::string>();
            if (!r["big_mime_type"].isNull())
                meta.bigMime = r["big_mime_type"].as<std::string>();
            if (!r["small_object_key"].isNull())
                meta.smallObjectKey = r["small_object_key"].as<std::string>();
            if (!r["small_mime_type"].isNull())
                meta.smallMime = r["small_mime_type"].as<std::string>();
            if (!r["link_name"].isNull())
                meta.linkName = r["link_name"].as<std::string>();
            if (!r["link_url"].isNull())
                meta.linkUrl = r["link_url"].as<std::string>();
            metaFound = true;
        }
    }
    catch (const DrogonDbException &)
    {
        LOG_ERROR(std::string("TableImageSender: db error while querying images table=") + imagesTable);
        co_return makeJsonResponse(makeErrorMessage("db error"), k500InternalServerError);
    }
    if (!metaFound)
    {
        LOG_WARNING(std::string("TableImageSender: image meta not found imagesTable=") + imagesTable +
                    " imageId=" + std::to_string(imageId) + " rowId=" + std::to_string(rowId) + " dbName=" + dbName);
        co_return makeJsonResponse(makeErrorMessage("Image not found"), k404NotFound);
    }
    if (!meta.slot.empty() && meta.slot != dbName)
    {
        LOG_WARNING(std::string("TableImageSender: slot mismatch rowId=") + std::to_string(rowId) +
                    " dbName=" + dbName + " meta.slot=" + meta.slot);
        co_return makeJsonResponse(makeErrorMessage("Image slot mismatch"), k500InternalServerError);
    }

    std::string objectKey;
    std::string mime;
    if (small)
    {
        if (meta.smallObjectKey.empty())
        {
            LOG_WARNING(std::string("TableImageSender: missing small_object_key rowId=") + std::to_string(rowId) +
                        " dbName=" + dbName + " imageId=" + std::to_string(imageId));
            co_return makeJsonResponse(makeErrorMessage("Small image not found"), k404NotFound);
        }
        objectKey = meta.smallObjectKey;
        mime = meta.smallMime;
    }
    else
    {
        if (meta.bigObjectKey.empty())
        {
            LOG_WARNING(std::string("TableImageSender: missing big_object_key rowId=") + std::to_string(rowId) +
                        " dbName=" + dbName + " imageId=" + std::to_string(imageId));
            co_return makeJsonResponse(makeErrorMessage("Image not found"), k404NotFound);
        }
        objectKey = meta.bigObjectKey;
        mime = meta.bigMime;
    }

    // 6) MinIO fetch + multipart build
    auto minioPlugin = app().getPlugin<MinioPlugin>();
    if (!minioPlugin)
    {
        LOG_ERROR("TableImageSender: MinioPlugin is not initialized");
        co_return makeJsonResponse(makeErrorMessage("MinioPlugin is not initialized"), k500InternalServerError);
    }
    MinioClient &minio = minioPlugin->client();
    const auto &cfg = minioPlugin->minioConfig();
    const std::string bucket = cfg.bucket;

    std::vector<uint8_t> bytes;
    std::string mimeFromMinio;
    const bool ok = minio.getObject(bucket, objectKey, bytes, &mimeFromMinio);
    if (!ok)
    {
        LOG_ERROR(std::string("TableImageSender: MinIO getObject failed bucket=") + bucket +
                  " key=" + objectKey + " err=" + minio.lastError());
        co_return makeJsonResponse(makeErrorMessage("Image not found"), k404NotFound);
    }
    if (mime.empty() && !mimeFromMinio.empty())
    {
        mime = mimeFromMinio;
    }
    mime = normalizeImageMime(mime, objectKey);

    const std::string boundary = "boundary_" + drogon::utils::getUuid(true);
    std::string multipartBody;
    multipartBody.reserve(1024);

    const std::string filename = basenameFromKey(objectKey);
    appendBinaryPart(multipartBody,
                     boundary,
                     static_cast<int64_t>(rowId),
                     dbName,
                     mime,
                     filename,
                     reason,
                     meta.linkName,
                     meta.linkUrl,
                     bytes);

    // Финальная JSON-часть: ok=true, errors=[]
    Json::Value okJson(Json::objectValue);
    okJson["ok"] = true;
    okJson["errors"] = Json::Value(Json::arrayValue);
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
