#include "AuthController.h"

#include <drogon/drogon.h>
#include <random>
#include <string>

namespace
{
/// Простая генерация псевдослучайного токена из латинских букв и цифр.
std::string generateToken(std::size_t length = 32)
{
    static const char charset[] =
        "0123456789"
        "abcdefghijklmnopqrstuvwxyz"
        "ABCDEFGHIJKLMNOPQRSTUVWXYZ";

    static thread_local std::mt19937_64 rng{std::random_device{}()};
    std::uniform_int_distribution<std::size_t> dist(0, sizeof(charset) - 2);

    std::string token;
    token.reserve(length);

    for (std::size_t i = 0; i < length; ++i)
    {
        token.push_back(charset[dist(rng)]);
    }

    return token;
}
} // namespace

void AuthController::login(
    const drogon::HttpRequestPtr &req,
    std::function<void(const drogon::HttpResponsePtr &)> &&callback)
{
    try
    {
        // Пытаемся разобрать JSON‑тело запроса.
        auto jsonPtr = req->getJsonObject();
        if (!jsonPtr)
        {
            Json::Value err;
            err["error"] = "invalid json";

            auto resp = drogon::HttpResponse::newHttpJsonResponse(err);
            resp->setStatusCode(drogon::k400BadRequest);
            callback(resp);
            return;
        }

        const auto &json = *jsonPtr;

        // Проверяем наличие обязательных полей.
        if (!json.isMember("login") || !json.isMember("password"))
        {
            Json::Value err;
            err["error"] = "missing login or password";

            auto resp = drogon::HttpResponse::newHttpJsonResponse(err);
            resp->setStatusCode(drogon::k400BadRequest);
            callback(resp);
            return;
        }

        if(!inDatabase(json["login"], json["password"]))
        {
            Json::Value err;
            err["error"] = "invalid login or password";

            auto resp = drogon::HttpResponse::newHttpJsonResponse(err);
            resp->setStatusCode(drogon::k400BadRequest);
            callback(resp);
            return;
        }
        // Заглушка: принимаем любой логин/пароль как валидные.
        const std::string token = generateToken(32);

        // Сохраняем IP клиента вместо логина.
        auto peerAddr = req->getPeerAddr();
        std::string clientIp = peerAddr.toIp(); // только IP (без порта)
        tokens[token] = clientIp;

        Json::Value body;
        body["token"] = token;

        auto resp = drogon::HttpResponse::newHttpJsonResponse(body);
        resp->setStatusCode(drogon::k200OK);
        callback(resp);
    }
    catch (const std::exception &)
    {
        Json::Value err;
        err["error"] = "internal error";

        auto resp = drogon::HttpResponse::newHttpJsonResponse(err);
        resp->setStatusCode(drogon::k500InternalServerError);
        callback(resp);
    }
}

bool AuthController::inDatabase(const std::string &login, const std::string &password)
{
    
}

void AuthController::register(const drogon::HttpRequestPtr &req,
                              std::function<void(const drogon::HttpResponsePtr &)> &&callback)
{
    try
    {
        auto jsonPtr = req->getJsonObject();
    }
}
