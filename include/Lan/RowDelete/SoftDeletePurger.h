#pragma once

#include "Lan/RowDelete/RowDeleteService.h"

#include <drogon/utils/coroutine.h>

#include <cstdint>
#include <memory>
#include <string>

struct SoftDeletePurgerConfig
{
    std::string table = "milling_tool_catalog";
    int retentionDays = 30;
    int batchSize = 100;
    bool useAdvisoryLock = true;
    int64_t advisoryLockKey = 739001;
};

class SoftDeletePurger
{
public:
    SoftDeletePurger(SoftDeletePurgerConfig cfg, std::shared_ptr<RowDeleteService> deleteService);

    /// Выполнить один проход purge. Возвращает количество удалённых записей.
    drogon::Task<int> runOnce();

private:
    SoftDeletePurgerConfig cfg_;
    std::shared_ptr<RowDeleteService> deleteService_;
};
