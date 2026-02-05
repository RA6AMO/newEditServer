#include <drogon/drogon.h>
#include <drogon/utils/coroutine.h>

#include "Lan/RowDelete/SoftDeletePurgerPlugin.h"
#include "Loger/Logger.h"

#include <algorithm>
#include <string>

namespace
{
int clampPositiveInt(int value, int fallback)
{
    if (value <= 0)
    {
        return fallback;
    }
    return value;
}
} // namespace

void SoftDeletePurgerPlugin::initAndStart(const Json::Value &config)
{
    SoftDeletePurgerConfig cfg;
    if (config.isMember("table") && config["table"].isString())
    {
        cfg.table = config["table"].asString();
    }
    if (config.isMember("retention_days") && config["retention_days"].isInt())
    {
        cfg.retentionDays = clampPositiveInt(config["retention_days"].asInt(), cfg.retentionDays);
    }
    if (config.isMember("batch_size") && config["batch_size"].isInt())
    {
        cfg.batchSize = clampPositiveInt(config["batch_size"].asInt(), cfg.batchSize);
    }
    if (config.isMember("use_advisory_lock") && config["use_advisory_lock"].isBool())
    {
        cfg.useAdvisoryLock = config["use_advisory_lock"].asBool();
    }
    if (config.isMember("advisory_lock_key") && config["advisory_lock_key"].isInt64())
    {
        cfg.advisoryLockKey = config["advisory_lock_key"].asInt64();
    }

    int intervalMinutes = 60;
    if (config.isMember("interval_minutes") && config["interval_minutes"].isInt())
    {
        intervalMinutes = clampPositiveInt(config["interval_minutes"].asInt(), intervalMinutes);
    }

    purger_ = std::make_shared<SoftDeletePurger>(cfg, std::make_shared<RowDeleteService>());

    if (intervalMinutes > 0)
    {
        const double intervalSeconds = static_cast<double>(intervalMinutes) * 60.0;
        timerId_ = drogon::app().getLoop()->runEvery(
            intervalSeconds,
            drogon::async_func([this]() -> drogon::Task<void> {
                if (!purger_)
                {
                    co_return;
                }
                try
                {
                    const int purged = co_await purger_->runOnce();
                    if (purged > 0)
                    {
                        Logger::instance().info("SoftDeletePurgerPlugin: purged rows=" + std::to_string(purged));
                    }
                }
                catch (const std::exception &e)
                {
                    Logger::instance().error("SoftDeletePurgerPlugin: runOnce failed: " + std::string(e.what()));
                }
                co_return;
            }));
    }
}

void SoftDeletePurgerPlugin::shutdown()
{
    if (timerId_ != trantor::InvalidTimerId)
    {
        drogon::app().getLoop()->invalidateTimer(timerId_);
        timerId_ = trantor::InvalidTimerId;
    }
    purger_.reset();
}

drogon::Task<int> SoftDeletePurgerPlugin::runOnce()
{
    if (!purger_)
    {
        Logger::instance().error("SoftDeletePurgerPlugin: purger is not initialized");
        co_return 0;
    }
    co_return co_await purger_->runOnce();
}
