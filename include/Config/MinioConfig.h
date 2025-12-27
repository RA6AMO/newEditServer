#pragma once

#include "Storage/MinioClient.h"

namespace workshop::config
{
/// Возвращает конфигурацию MinIO, зашитую в код.
/// Важно: реальные значения лежат в .cpp, чтобы не тащить секреты в заголовки.
MinioClient::Config getMinioConfig();
} // namespace workshop::config

