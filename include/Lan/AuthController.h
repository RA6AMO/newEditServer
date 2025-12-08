#pragma once

#include <drogon/HttpController.h>
#include <json/json.h>

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
    ADD_METHOD_TO(AuthController::register, "/register", drogon::Post);
    METHOD_LIST_END

    /// Основной метод авторизации.
    void login(const drogon::HttpRequestPtr &req,
               std::function<void(const drogon::HttpResponsePtr &)> &&callback);
    void register(const drogon::HttpRequestPtr &req,
               std::function<void(const drogon::HttpResponsePtr &)> &&callback);
private:
    std::unordered_map<std::string, std::string> tokens;
};


