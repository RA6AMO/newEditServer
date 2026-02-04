#pragma once

#include <drogon/HttpTypes.h>
#include <json/json.h>

#include <stdexcept>
#include <string>

class CellUpdateError : public std::runtime_error
{
public:
    CellUpdateError(std::string code,
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
