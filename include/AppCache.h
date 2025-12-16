#pragma once

#include <drogon/plugins/Plugin.h>
#include <json/json.h>
#include <chrono>
#include <optional>
#include <shared_mutex>
#include <string>
#include <unordered_map>

class AppCache : public drogon::Plugin<AppCache>
{
public:
    /// Структура для хранения информации о токене.
    struct TokenInfo
    {
        std::string clientIp;
        std::chrono::steady_clock::time_point expiresAt;
    };

    /// Инициализация плагина при старте сервера.
    /// Читает настройки из config.json (token_ttl_sec, по умолчанию 3600 секунд).
    void initAndStart(const Json::Value &config) override;

    /// Очистка ресурсов при остановке сервера.
    void shutdown() override;

    /// Сохранить токен в кэш.
    /// @param token Строка токена
    /// @param clientIp IP адрес клиента
    void putToken(std::string token, std::string clientIp);

    /// Получить информацию о токене из кэша.
    /// Автоматически удаляет протухшие токены (lazy eviction).
    /// @param token Строка токена
    /// @return TokenInfo если токен найден и не протух, иначе std::nullopt
    std::optional<TokenInfo> getToken(const std::string &token);

    /// Удалить токен из кэша.
    /// @param token Строка токена
    void eraseToken(const std::string &token);

private:
    mutable std::shared_mutex mu_;
    std::unordered_map<std::string, TokenInfo> tokenByValue_;
    std::chrono::seconds tokenTtl_{3600}; // TTL по умолчанию: 1 час
};