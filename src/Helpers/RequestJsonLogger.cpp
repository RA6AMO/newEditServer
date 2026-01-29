#include "Helpers/RequestJsonLogger.h"

#include <array>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <random>
#include <sstream>
#include <string>
#include <utility>

namespace
{
std::string nowIsoUtc()
{
    using namespace std::chrono;

    const auto now = system_clock::now();
    const auto msPart = duration_cast<milliseconds>(now.time_since_epoch()) % 1000;
    const std::time_t tt = system_clock::to_time_t(now);

    std::tm tmUtc{};
    gmtime_r(&tt, &tmUtc);

    std::ostringstream oss;
    oss << std::put_time(&tmUtc, "%Y-%m-%dT%H:%M:%S");
    oss << "." << std::setfill('0') << std::setw(3) << msPart.count() << "Z";
    return oss.str();
}

std::string nowForFilenameUtc()
{
    using namespace std::chrono;

    const auto now = system_clock::now();
    const auto msPart = duration_cast<milliseconds>(now.time_since_epoch()) % 1000;
    const std::time_t tt = system_clock::to_time_t(now);

    std::tm tmUtc{};
    gmtime_r(&tt, &tmUtc);

    // Без ':' для совместимости с разными файловыми системами.
    std::ostringstream oss;
    oss << std::put_time(&tmUtc, "%Y-%m-%dT%H-%M-%S");
    oss << "." << std::setfill('0') << std::setw(3) << msPart.count() << "Z";
    return oss.str();
}

std::string uuidV4()
{
    std::array<unsigned char, 16> bytes{};

    std::random_device rd;
    std::mt19937_64 gen(rd());
    std::uniform_int_distribution<unsigned int> dist(0, 255);
    for (auto &b : bytes)
    {
        b = static_cast<unsigned char>(dist(gen));
    }

    // RFC 4122: version 4 (random)
    bytes[6] = static_cast<unsigned char>((bytes[6] & 0x0F) | 0x40);
    // RFC 4122: variant 1
    bytes[8] = static_cast<unsigned char>((bytes[8] & 0x3F) | 0x80);

    auto hex2 = [](unsigned char v) -> std::string {
        std::ostringstream oss;
        oss << std::hex << std::nouppercase << std::setfill('0') << std::setw(2) << static_cast<int>(v);
        return oss.str();
    };

    std::string out;
    out.reserve(36);
    for (int i = 0; i < 16; ++i)
    {
        if (i == 4 || i == 6 || i == 8 || i == 10)
            out.push_back('-');
        out += hex2(bytes[static_cast<size_t>(i)]);
    }
    return out;
}

bool exceedsPayloadLimit(const Json::Value &payload, std::size_t maxBytes)
{
    if (maxBytes == 0)
        return false;

    Json::StreamWriterBuilder b;
    b["indentation"] = "";
    const std::string s = Json::writeString(b, payload);
    return s.size() > maxBytes;
}

bool exceedsBodyLimit(const drogon::HttpRequestPtr &req, std::size_t maxBytes)
{
    if (maxBytes == 0)
        return false;
    if (!req)
        return false;
    return req->bodyLength() > maxBytes;
}

Json::Value headersToJson(const drogon::HttpRequestPtr &req)
{
    Json::Value headers(Json::objectValue);
    if (!req)
        return headers;
    for (const auto &kv : req->headers())
    {
        headers[kv.first] = kv.second;
    }
    return headers;
}

Json::Value bodyToJson(const drogon::HttpRequestPtr &req,
                       bool skipForMultipart,
                       std::size_t maxBodyBytes)
{
    Json::Value body(Json::objectValue);
    if (!req)
        return body;

    body["lengthBytes"] = static_cast<Json::UInt64>(req->bodyLength());

    if (skipForMultipart && req->contentType() == drogon::CT_MULTIPART_FORM_DATA)
    {
        body["skipped"] = true;
        body["reason"] = "multipart";
        return body;
    }

    if (maxBodyBytes != 0 && req->bodyLength() > maxBodyBytes)
    {
        body["skipped"] = true;
        body["reason"] = "maxBodyBytes_exceeded";
        body["maxBodyBytes"] = static_cast<Json::UInt64>(maxBodyBytes);
        return body;
    }

    const auto sv = req->body();
    body["text"] = std::string(sv.data(), sv.size());
    return body;
}
} // namespace

RequestJsonLogger::RequestJsonLogger() : RequestJsonLogger(Options{}) {}

RequestJsonLogger::RequestJsonLogger(Options opt) : opt_(std::move(opt)) {}

Json::Value RequestJsonLogger::toJson(const RowController::ParsedRequest &parsed)
{
    Json::Value root(Json::objectValue);
    root["timestamp"] = nowIsoUtc();
    root["payload"] = parsed.payload;

    if (true)
    {
        Json::Value arr(Json::arrayValue);
        for (const auto &att : parsed.attachments)
        {
            Json::Value a(Json::objectValue);
            a["id"] = att.id;
            a["dbName"] = att.dbName;
            a["role"] = att.role;
            a["filename"] = att.filename;
            a["mimeType"] = att.mimeType;
            a["sizeBytes"] = static_cast<Json::UInt64>(att.data.size());
            arr.append(std::move(a));
        }
        root["attachments"] = std::move(arr);
    }

    return root;
}

Json::Value RequestJsonLogger::toJson(const drogon::HttpRequestPtr &req)
{
    Json::Value root(Json::objectValue);
    root["timestamp"] = nowIsoUtc();

    if (!req)
    {
        root["error"] = "null_request";
        return root;
    }

    root["method"] = req->getMethodString();
    root["path"] = req->path();
    root["peerIp"] = req->getPeerAddr().toIp();
    root["contentType"] = static_cast<Json::Int>(req->contentType());
    root["query"] = req->query();
    root["headers"] = headersToJson(req);
    root["body"] = bodyToJson(req, true, 0);

    return root;
}

bool RequestJsonLogger::log(const RowController::ParsedRequest &parsed) noexcept
{
    try
    {
        if (exceedsPayloadLimit(parsed.payload, opt_.maxPayloadBytes))
        {
            return false;
        }

        const std::filesystem::path base(opt_.baseDir.empty() ? "./logs/requests" : opt_.baseDir);
        std::filesystem::create_directories(base);

        Json::Value root(Json::objectValue);
        root["timestamp"] = nowIsoUtc();
        root["payload"] = parsed.payload;

        if (opt_.writeAttachmentsMeta)
        {
            Json::Value arr(Json::arrayValue);
            for (const auto &att : parsed.attachments)
            {
                Json::Value a(Json::objectValue);
                a["id"] = att.id;
                a["dbName"] = att.dbName;
                a["role"] = att.role;
                a["filename"] = att.filename;
                a["mimeType"] = att.mimeType;
                a["sizeBytes"] = static_cast<Json::UInt64>(att.data.size());
                arr.append(std::move(a));
            }
            root["attachments"] = std::move(arr);
        }

        const std::string filename = nowForFilenameUtc() + "_" + uuidV4() + ".json";
        const std::filesystem::path fullPath = base / filename;

        Json::StreamWriterBuilder builder;
        builder["indentation"] = "  ";

        std::ofstream out(fullPath, std::ios::out | std::ios::binary);
        if (!out.is_open())
            return false;

        out << Json::writeString(builder, root);
        out.flush();
        return static_cast<bool>(out);
    }
    catch (...)
    {
        return false;
    }
}

bool RequestJsonLogger::log(const drogon::HttpRequestPtr &req) noexcept
{
    try
    {
        if (!req)
            return false;

        // Лимит на body проверяем до сериализации.
        if (exceedsBodyLimit(req, opt_.maxBodyBytes))
        {
            return false;
        }

        const std::filesystem::path base(opt_.baseDir.empty() ? "./logs/requests" : opt_.baseDir);
        std::filesystem::create_directories(base);

        Json::Value root(Json::objectValue);
        root["timestamp"] = nowIsoUtc();
        root["method"] = req->getMethodString();
        root["path"] = req->path();
        root["peerIp"] = req->getPeerAddr().toIp();
        root["contentType"] = static_cast<Json::Int>(req->contentType());

        if (opt_.writeQuery)
        {
            root["query"] = req->query();
        }

        if (opt_.writeHeaders)
        {
            root["headers"] = headersToJson(req);
        }

        if (opt_.writeBody)
        {
            root["body"] = bodyToJson(req, opt_.skipBodyForMultipart, opt_.maxBodyBytes);
        }
        else
        {
            Json::Value body(Json::objectValue);
            body["lengthBytes"] = static_cast<Json::UInt64>(req->bodyLength());
            body["skipped"] = true;
            body["reason"] = "disabled";
            root["body"] = std::move(body);
        }

        const std::string filename = nowForFilenameUtc() + "_" + uuidV4() + ".json";
        const std::filesystem::path fullPath = base / filename;

        Json::StreamWriterBuilder builder;
        builder["indentation"] = "  ";

        std::ofstream out(fullPath, std::ios::out | std::ios::binary);
        if (!out.is_open())
            return false;

        out << Json::writeString(builder, root);
        out.flush();
        return static_cast<bool>(out);
    }
    catch (...)
    {
        return false;
    }
}

