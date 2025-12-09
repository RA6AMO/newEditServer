#pragma once

#include <drogon/HttpController.h>
#include <drogon/drogon.h>
#include <json/json.h>
#include <argon2.h>

#define PAPER_SALT "kfg425kgfetvcfd56"

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
    METHOD_LIST_END

    /// Основной метод авторизации.
    void login(const drogon::HttpRequestPtr &req, std::function<void(const drogon::HttpResponsePtr &)> &&callback);

    drogon::Task<drogon::HttpResponsePtr> registerUser(drogon::HttpRequestPtr req);
private:
    std::unordered_map<std::string, std::string> tokens;

    bool inDatabase(const std::string &login, const std::string &password);
};


