#include "Config/MinioConfig.h"

namespace workshop::config
{
MinioClient::Config getMinioConfig()
{
    MinioClient::Config cfg;
    cfg.endpoint = "192.168.56.101:9000";
    cfg.accessKey = "root";
    cfg.secretKey = "root123longpassword";
    cfg.bucket = "fordata";
    cfg.useSSL = false;
    return cfg;
}
} // namespace workshop::config

