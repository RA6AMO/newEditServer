#pragma once

#include "Lan/RowDelete/SoftDeletePurger.h"

#include <drogon/plugins/Plugin.h>
#include <json/json.h>

#include <memory>

class SoftDeletePurgerPlugin : public drogon::Plugin<SoftDeletePurgerPlugin>
{
public:
    void initAndStart(const Json::Value &config) override;
    void shutdown() override;

    drogon::Task<int> runOnce();

private:
    std::shared_ptr<SoftDeletePurger> purger_;
    trantor::TimerId timerId_{trantor::InvalidTimerId};
};
