#include "Storage/MinioPlugin.h"

#include "Config/MinioConfig.h"

#include <stdexcept>

namespace
{
MinioClient::Config configFromPluginConfig(const Json::Value &config)
{
    MinioClient::Config cfg;

    // Поддерживаем конфиг в стиле config.json/minio
    // {
    //   "endpoint": "...",
    //   "access_key": "...",
    //   "secret_key": "...",
    //   "bucket": "...",
    //   "use_ssl": false
    // }
    if (config.isObject() && !config.empty())
    {
        cfg.endpoint = config.get("endpoint", "").asString();
        cfg.accessKey = config.get("access_key", "").asString();
        cfg.secretKey = config.get("secret_key", "").asString();
        cfg.bucket = config.get("bucket", "").asString();
        cfg.useSSL = config.get("use_ssl", false).asBool();
        return cfg;
    }

    // Фоллбек на кодовую конфигурацию (исторически так было в RowController)
    return workshop::config::getMinioConfig();
}
} // namespace

void MinioPlugin::initAndStart(const Json::Value &config)
{
    cfg_ = configFromPluginConfig(config);
    client_ = std::make_unique<MinioClient>(cfg_);
}

void MinioPlugin::shutdown()
{
    client_.reset();
}

MinioClient &MinioPlugin::client()
{
    if (!client_)
    {
        throw std::runtime_error("MinioPlugin: MinioClient is not initialized");
    }
    return *client_;
}

const MinioClient::Config &MinioPlugin::minioConfig() const
{
    return cfg_;
}

