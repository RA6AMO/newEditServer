#pragma once

#include <drogon/HttpTypes.h>
#include <drogon/orm/DbClient.h>
#include <drogon/utils/coroutine.h>
#include <json/json.h>

#include <cstdint>
#include <functional>
#include <string>
#include <unordered_map>
#include <vector>

struct AttachmentInput
{
    std::string id;
    std::string dbName;
    std::string role;
    std::string filename;
    std::string mimeType;
    std::vector<uint8_t> data;
};

struct UploadOp
{
    std::string attachmentId;
    std::string bucket;
    std::string objectKey;
    std::string mimeType;
};

struct DbOp
{
    std::string debugName;
    std::function<drogon::Task<void>(const std::shared_ptr<drogon::orm::Transaction> &)> exec;
};

struct RowWritePlan
{
    std::vector<DbOp> preUploadDbOps;
    std::vector<UploadOp> uploads;
    std::vector<DbOp> postUploadDbOps;
    Json::Value successExtra;
    Json::Value warnings;
    Json::Value debug;
};

struct WriteResult
{
    int64_t rowId = 0;
    Json::Value extra;
};

struct ValidationError
{
    std::string code;
    std::string message;
    Json::Value details;
    drogon::HttpStatusCode status = drogon::k400BadRequest;
};

