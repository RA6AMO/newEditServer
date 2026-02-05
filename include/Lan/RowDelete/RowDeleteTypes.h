#pragma once

#include <drogon/HttpTypes.h>
#include <drogon/orm/DbClient.h>
#include <drogon/utils/coroutine.h>
#include <json/json.h>

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

struct RowDeleteRequest
{
    std::string table;
    int64_t rowId = 0;
};

struct RowDeleteStorageOp
{
    std::string bucket;
    std::string objectKey;
};

struct RowDeleteDbOp
{
    std::string debugName;
    std::function<drogon::Task<void>(const std::shared_ptr<drogon::orm::Transaction> &)> exec;
};

struct RowDeletePlan
{
    std::vector<RowDeleteDbOp> dbOps;
    std::vector<RowDeleteStorageOp> storageDeletes;
    Json::Value warnings;
    Json::Value debug;
};

struct DeleteResult
{
    int64_t rowId = 0;
    Json::Value warnings;
};

struct DeleteValidationError
{
    std::string code;
    std::string message;
    Json::Value details;
    drogon::HttpStatusCode status = drogon::k400BadRequest;
};
