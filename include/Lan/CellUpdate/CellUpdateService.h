#pragma once

#include "Lan/CellUpdate/CellUpdateController.h"
#include "Lan/CellUpdate/CellUpdateErrors.h"
#include "Lan/CellUpdate/CellUpdatePlanner.h"
#include "Lan/RowAdd/RowWriteTypes.h"

#include <drogon/utils/coroutine.h>
#include <json/json.h>

#include <exception>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

class CellUpdateService
{
public:
    CellUpdateService();

    drogon::Task<WriteResult> update(const CellUpdateController::ParsedRequest &parsed);

private:
    struct UploadedObject
    {
        std::string bucket;
        std::string objectKey;
    };

    std::shared_ptr<CellUpdatePlannerRegistry> registry_;

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
