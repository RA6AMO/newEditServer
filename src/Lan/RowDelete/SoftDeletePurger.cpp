#include "Lan/RowDelete/SoftDeletePurger.h"
#include "Lan/allTableList.h"

#include <drogon/drogon.h>

#include "Loger/Logger.h"

#include <cctype>
#include <string>

namespace
{
bool isSafeIdentifier(const std::string &name)
{
    if (name.empty())
    {
        return false;
    }
    for (const char c : name)
    {
        if (!(std::isalnum(static_cast<unsigned char>(c)) || c == '_'))
        {
            return false;
        }
    }
    return true;
}

std::string quoteIdent(const std::string &name)
{
    return "\"" + name + "\"";
}
} // namespace

SoftDeletePurger::SoftDeletePurger(SoftDeletePurgerConfig cfg,
                                   std::shared_ptr<RowDeleteService> deleteService)
    : cfg_(std::move(cfg)),
      deleteService_(std::move(deleteService))
{
}

drogon::Task<int> SoftDeletePurger::runOnce()
{
    if (!deleteService_)
    {
        Logger::instance().error("SoftDeletePurger: RowDeleteService is not initialized");
        co_return 0;
    }

    const std::string baseTable = resolveBaseTable(cfg_.table);
    if (!isSafeIdentifier(baseTable))
    {
        Logger::instance().error("SoftDeletePurger: unsafe table name: " + baseTable);
        co_return 0;
    }

    auto dbClient = drogon::app().getDbClient("default");
    bool locked = false;
    if (cfg_.useAdvisoryLock)
    {
        auto lockRows = co_await dbClient->execSqlCoro(
            "SELECT pg_try_advisory_lock($1) AS locked",
            cfg_.advisoryLockKey);
        if (lockRows.empty() || lockRows[0]["locked"].isNull() || !lockRows[0]["locked"].as<bool>())
        {
            co_return 0;
        }
        locked = true;
    }

    int purged = 0;
    try
    {
        const std::string sql =
            "SELECT id FROM public." + quoteIdent(baseTable) +
            " WHERE is_deleted = TRUE"
            " AND deleted_at IS NOT NULL"
            " AND deleted_at <= now() - ($1::int * interval '1 day')"
            " ORDER BY deleted_at ASC"
            " LIMIT $2";

        auto rows = co_await dbClient->execSqlCoro(sql, cfg_.retentionDays, cfg_.batchSize);
        for (const auto &row : rows)
        {
            if (row["id"].isNull())
            {
                continue;
            }
            const int64_t rowId = row["id"].as<int64_t>();
            RowDeleteRequest req;
            req.table = baseTable;
            req.rowId = rowId;
            try
            {
                co_await deleteService_->deleteRow(req);
                purged++;
            }
            catch (const std::exception &e)
            {
                Logger::instance().error("SoftDeletePurger: hard delete failed: " + std::string(e.what()));
            }
        }
    }
    catch (const std::exception &e)
    {
        Logger::instance().error("SoftDeletePurger: select failed: " + std::string(e.what()));
    }

    if (locked)
    {
        try
        {
            (void)co_await dbClient->execSqlCoro("SELECT pg_advisory_unlock($1)", cfg_.advisoryLockKey);
        }
        catch (const std::exception &e)
        {
            Logger::instance().error("SoftDeletePurger: advisory unlock failed: " + std::string(e.what()));
        }
    }

    co_return purged;
}
