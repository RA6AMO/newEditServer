#pragma once

#include "Lan/RowDelete/RowDeletePlanner.h"
#include "Lan/RowDelete/RowDeleteTypes.h"

#include <drogon/utils/coroutine.h>
#include <json/json.h>

#include <exception>
#include <memory>
#include <string>

class RowDeleteError : public std::runtime_error
{
public:
    RowDeleteError(std::string code,
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

class RowDeleteService
{
public:
    RowDeleteService();

    /// Пример использования (без эндпоинта):
    /// RowDeleteService service;
    /// RowDeleteRequest req;
    /// req.table = "milling_tool_catalog";
    /// req.rowId = 123;
    /// auto result = co_await service.deleteRow(req);
    drogon::Task<DeleteResult> deleteRow(const RowDeleteRequest &request);

private:
    std::shared_ptr<RowDeletePlannerRegistry> registry_;
};
