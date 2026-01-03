#pragma once

#include <drogon/plugins/Plugin.h>
#include <json/json.h>

#include <memory>
#include <string>

#include "Storage/MinioClient.h"

/// Drogon-плагин, который создаёт один MinioClient на всё приложение
/// и отдаёт его другим компонентам через app().getPlugin<MinioPlugin>().
class MinioPlugin : public drogon::Plugin<MinioPlugin>
{
public:
    void initAndStart(const Json::Value &config) override;
    void shutdown() override;

    /// Доступ к клиенту (инициализируется в initAndStart).
    MinioClient &client();

    /// Текущая конфигурация клиента.
    const MinioClient::Config &minioConfig() const;

private:
    std::unique_ptr<MinioClient> client_;
    MinioClient::Config cfg_;
};

