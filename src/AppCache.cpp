#include "AppCache.h"
#include <drogon/drogon.h>

DROGON_PLUGIN(AppCache);

void AppCache::initAndStart(const Json::Value &config)
{
    // Читаем TTL токенов из конфига (по умолчанию 3600 секунд = 1 час)
    if (config.isMember("token_ttl_sec") && config["token_ttl_sec"].isInt())
    {
        tokenTtl_ = std::chrono::seconds(config["token_ttl_sec"].asInt());
    }
}

void AppCache::shutdown()
{
    std::unique_lock lk(mu_);
    tokenByValue_.clear();
}

void AppCache::putToken(std::string token, int64_t userId, std::string clientIp)
{
    TokenInfo info;
    info.userId = userId;
    info.clientIp = std::move(clientIp);
    info.expiresAt = std::chrono::steady_clock::now() + tokenTtl_;

    std::unique_lock lk(mu_);
    tokenByValue_[std::move(token)] = std::move(info);
}

std::optional<AppCache::TokenInfo> AppCache::getToken(const std::string &token) const
{
    const auto now = std::chrono::steady_clock::now();

    // Быстрая проверка с shared_lock (чтение)
    {
        std::shared_lock lk(mu_);
        auto it = tokenByValue_.find(token);
        if (it == tokenByValue_.end())
        {
            return std::nullopt;
        }

        // Если токен не протух, возвращаем его
        if (it->second.expiresAt > now)
        {
            return it->second;
        }
    }

    // Токен протух - удаляем его (lazy eviction)
    // Переходим на unique_lock для записи
    std::unique_lock lk(mu_);
    auto it = tokenByValue_.find(token);
    if (it != tokenByValue_.end() && it->second.expiresAt <= now)
    {
        tokenByValue_.erase(it);
    }

    return std::nullopt;
}

void AppCache::eraseToken(const std::string &token)
{
    std::unique_lock lk(mu_);
    tokenByValue_.erase(token);
}