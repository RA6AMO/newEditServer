#pragma once

#include <drogon/HttpController.h>
#include <drogon/drogon.h>
#include <json/json.h>
#include <argon2.h>
#include <string>

#define PAPER_SALT "kfg425kgfetvcfd56"

/// Небольшой помощник для проверки токена:
/// 1) Сначала проверяет токен в `AppCache` (и IP клиента).
/// 2) Если токена нет/протух — проверяет `users.last_token/last_ip` в БД.
/// При успехе (совпадение токен+IP) кладёт токен в кэш.
class TokenValidator
{
public:
    enum class Status
    {
        Ok = 0,
        InvalidToken,
        IpMismatch,
        DbError
    };

    /// Проверить токен для клиента с IP `clientIp`.
    drogon::Task<Status> check(const std::string &token, const std::string &clientIp) const;

    /// Преобразовать статус в понятный текст ошибки (для JSON).
    static const char *toError(Status status);

    /// Преобразовать статус в HTTP код.
    static drogon::HttpStatusCode toHttpCode(Status status);
};

/// Контроллер авторизации для Qt‑клиента.
/// Маршрут: POST /login
/// Ожидает JSON: { "login": "...", "password": "..." }
/// Успех: 200 OK, JSON { "token": "<случайная_строка>" }
/// Ошибка формата: 400 Bad Request.
class AuthController : public drogon::HttpController<AuthController>
{
public:
    METHOD_LIST_BEGIN
    // Обработчик POST /login
    ADD_METHOD_TO(AuthController::login, "/login", drogon::Post);
    ADD_METHOD_TO(AuthController::registerUser, "/register", drogon::Post);
    ADD_METHOD_TO(AuthController::autoConnect, "/autoConnect", drogon::Post);
    METHOD_LIST_END

    /// Основной метод авторизации.
    drogon::Task<drogon::HttpResponsePtr> login(drogon::HttpRequestPtr req);

    drogon::Task<drogon::HttpResponsePtr> registerUser(drogon::HttpRequestPtr req);

    /// Автоматическое подключение по токену.
    drogon::Task<drogon::HttpResponsePtr> autoConnect(drogon::HttpRequestPtr req);
};


