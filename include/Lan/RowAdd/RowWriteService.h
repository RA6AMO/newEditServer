#pragma once

#include "Lan/RowController.h"
#include "Lan/RowWritePlanner.h"
#include "Lan/RowWriteTypes.h"

#include <drogon/utils/coroutine.h>
#include <json/json.h>

#include <exception>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

class RowWriteError : public std::runtime_error
{
public:
    RowWriteError(std::string code,
                  std::string message,
                  drogon::HttpStatusCode status,
                  Json::Value details = Json::nullValue)
        : std::runtime_error(message),
          code_(std::move(code)),
          status_(status),
          details_(std::move(details))
    {
    }

    const std::string &code() const noexcept { return code_; }
    drogon::HttpStatusCode status() const noexcept { return status_; }
    const Json::Value &details() const noexcept { return details_; }

private:
    std::string code_;
    drogon::HttpStatusCode status_;
    Json::Value details_;
};

class RowWriteService
{
public:
    RowWriteService();

    drogon::Task<WriteResult> write(const RowController::ParsedRequest &parsed);

private:
    struct UploadedObject
    {
        std::string bucket;
        std::string objectKey;
    };

    std::shared_ptr<RowWritePlannerRegistry> registry_;

    std::unordered_map<std::string, const AttachmentInput *> buildAttachmentIndex(
        const std::vector<AttachmentInput> &attachments) const;

    std::string buildObjectKey(const std::string &table,
                               int64_t rowId,
                               const AttachmentInput &attachment) const;

    drogon::Task<void> executePlan(const std::shared_ptr<drogon::orm::Transaction> &trans,
                                   MinioClient &minioClient,
                                   const RowWritePlan &plan,
                                   const std::unordered_map<std::string, const AttachmentInput *> &attachmentIndex,
                                   std::vector<UploadedObject> &uploadedObjects);
};

