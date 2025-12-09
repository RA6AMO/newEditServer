#include "AuthController.h"

#include <drogon/drogon.h>
#include <drogon/orm/DbClient.h>
#include <drogon/orm/Exception.h>
#include <random>
#include <string>

namespace
{
enum InputType
{
    LOG = 0,
    PAS = 1
};

/// Проверка допустимости логина/пароля по тем же правилам, что и в Qt‑клиенте.
bool isValidInput(const std::string &input, InputType type)
{
    if (input.empty())
    {
        return false;
    }

    for (char ch : input)
    {
        if (!((ch >= 'a' && ch <= 'z') ||
              (ch >= 'A' && ch <= 'Z') ||
              (ch >= '0' && ch <= '9')))
        {
            return false;
        }
    }

    if (type == LOG)
    {
        if (input.length() < 3 || input.length() > 32)
        {
            return false;
        }
    }
    else if (type == PAS)
    {
        if (input.length() < 8 || input.length() > 32)
        {
            return false;
        }
    }

    return true;
}

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

        if(!inDatabase(json["login"].asString(), json["password"].asString()))
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
    // TODO: реализовать проверку логина и пароля (сравнение Argon2-хеша) в базе данных.
    return false;
}

drogon::Task<drogon::HttpResponsePtr>
AuthController::registerUser(drogon::HttpRequestPtr req)
{
    try
    {
        // Разбираем JSON‑тело запроса.
        auto jsonPtr = req->getJsonObject();
        if (!jsonPtr)
        {
            Json::Value err;
            err["error"] = "invalid json";

            auto resp = drogon::HttpResponse::newHttpJsonResponse(err);
            resp->setStatusCode(drogon::k400BadRequest);
            co_return resp;
        }

        const auto &json = *jsonPtr;

        // Проверяем наличие и тип полей login / password.
        if (!json.isMember("login") || !json.isMember("password") ||
            !json["login"].isString() || !json["password"].isString())
        {
            Json::Value err;
            err["error"] = "missing or invalid login/password";

            auto resp = drogon::HttpResponse::newHttpJsonResponse(err);
            resp->setStatusCode(drogon::k400BadRequest);
            co_return resp;
        }

        const std::string login = json["login"].asString();
        const std::string password = json["password"].asString();

        // Валидация как в Qt‑клиенте.
        if (!isValidInput(login, InputType::LOG) || !isValidInput(password, InputType::PAS))
        {
            Json::Value err;
            err["error"] = "invalid login or password format";

            auto resp = drogon::HttpResponse::newHttpJsonResponse(err);
            resp->setStatusCode(drogon::k400BadRequest);
            co_return resp;
        }

        // Хешируем пароль с помощью Argon2id.
        const uint32_t t_cost = 2;              // количество итераций
        const uint32_t m_cost = 1 << 16;        // память в KiB (64 МБ)
        const uint32_t parallelism = 1;         // параллелизм
        const std::size_t salt_length = sizeof(PAPER_SALT) - 1; // длина "перца"
        const std::size_t hash_length = 64;     // длина хеша

        const unsigned char *salt =
            reinterpret_cast<const unsigned char *>(PAPER_SALT);

        char encoded[256] = {0};
        int result = argon2id_hash_encoded(
            t_cost,
            m_cost,
            parallelism,
            password.data(),
            password.size(),
            salt,
            salt_length,
            hash_length,
            encoded,
            sizeof(encoded));

        if (result != ARGON2_OK)
        {
            Json::Value err;
            err["error"] = "password hash error";

            auto resp = drogon::HttpResponse::newHttpJsonResponse(err);
            resp->setStatusCode(drogon::k500InternalServerError);
            co_return resp;
        }

        // На этом этапе encoded содержит Argon2id-хеш (включая параметры и соль).
        const std::string passwordHash(encoded);

        // Асинхронный запрос к БД: вставка пользователя и возврат его id.
        using namespace drogon;
        using namespace drogon::orm;

        auto dbClient = app().getDbClient("default");

        try
        {
            auto resultRows = co_await dbClient->execSqlCoro(
                "INSERT INTO users(login, password_hash) "
                "VALUES($1, $2) RETURNING id",
                login,
                passwordHash);

            Json::Value body;
            body["status"] = "ok";

            if (!resultRows.empty())
            {
                body["user_id"] = resultRows[0]["id"].as<int64_t>();
            }

            auto resp = HttpResponse::newHttpJsonResponse(body);
            resp->setStatusCode(k200OK);
            co_return resp;
        }
        catch (const DrogonDbException &)
        {
            Json::Value err;
            err["error"] = "db error";

            auto resp = HttpResponse::newHttpJsonResponse(err);
            resp->setStatusCode(k500InternalServerError);
            co_return resp;
        }
    }
    catch (const std::exception &)
    {
        Json::Value err;
        err["error"] = "internal error";

        auto resp = drogon::HttpResponse::newHttpJsonResponse(err);
        resp->setStatusCode(drogon::k500InternalServerError);
        co_return resp;
    }
}
