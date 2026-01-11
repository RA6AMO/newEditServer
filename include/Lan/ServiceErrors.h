#pragma once

#include <stdexcept>
#include <string>

/// Ошибка входных данных/контракта (нужно отвечать 400).
struct BadRequestError : public std::runtime_error
{
    explicit BadRequestError(const std::string &msg)
        : std::runtime_error(msg)
    {
    }
};

