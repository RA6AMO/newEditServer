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

drogon::Task<drogon::HttpResponsePtr>
AuthController::login(drogon::HttpRequestPtr req)
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

        // Асинхронный запрос к БД: поиск пользователя по логину.
        using namespace drogon;
        using namespace drogon::orm;

        auto dbClient = app().getDbClient("default");

        try
        {
            // Ищем пользователя в БД.
            auto resultRows = co_await dbClient->execSqlCoro(
                "SELECT id, password_hash FROM users WHERE username = $1",
                login);

            // Если пользователь не найден.
            if (resultRows.empty())
            {
                Json::Value err;
                err["error"] = "invalid login or password";

                auto resp = HttpResponse::newHttpJsonResponse(err);
                resp->setStatusCode(k401Unauthorized);
                co_return resp;
            }

            const int64_t userId = resultRows[0]["id"].as<int64_t>();
            const std::string passwordHash = resultRows[0]["password_hash"].as<std::string>();

            // Проверяем пароль с помощью Argon2id.
            int verifyResult = argon2id_verify(
                passwordHash.c_str(),
                password.data(),
                password.size());

            if (verifyResult != ARGON2_OK)
            {
                Json::Value err;
                err["error"] = "invalid login or password";

                auto resp = HttpResponse::newHttpJsonResponse(err);
                resp->setStatusCode(k401Unauthorized);
                co_return resp;
            }

            // Пароль верный. Генерируем токен и получаем IP клиента.
            const std::string token = generateToken(32);
            auto peerAddr = req->getPeerAddr();
            std::string clientIp = peerAddr.toIp(); // только IP (без порта)

            // Обновляем last_login_at, last_ip и last_token.
            co_await dbClient->execSqlCoro(
                "UPDATE users SET last_login_at = now(), last_ip = $2, last_token = $3 WHERE id = $1",
                userId,
                clientIp,
                token);

            // Сохраняем токен в памяти.
            tokens[token] = clientIp;

            Json::Value body;
            body["token"] = token;

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
                "INSERT INTO users(username, password_hash) "
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

drogon::Task<drogon::HttpResponsePtr>
AuthController::autoConnect(drogon::HttpRequestPtr req)
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

        // Проверяем наличие и тип поля token.
        if (!json.isMember("token") || !json["token"].isString())
        {
            Json::Value err;
            err["error"] = "missing or invalid token";

            auto resp = drogon::HttpResponse::newHttpJsonResponse(err);
            resp->setStatusCode(drogon::k400BadRequest);
            co_return resp;
        }

        const std::string token = json["token"].asString();
        
        // Получаем IP клиента.
        auto peerAddr = req->getPeerAddr();
        std::string clientIp = peerAddr.toIp(); // только IP (без порта)

        // Асинхронный запрос к БД: поиск пользователя по токену.
        using namespace drogon;
        using namespace drogon::orm;

        auto dbClient = app().getDbClient("default");

        try
        {
            // Ищем пользователя в БД по токену.
            auto resultRows = co_await dbClient->execSqlCoro(
                "SELECT id, last_ip FROM users WHERE last_token = $1",
                token);

            // Если пользователь не найден или токен не совпадает.
            if (resultRows.empty())
            {
                Json::Value err;
                err["error"] = "invalid token";

                auto resp = HttpResponse::newHttpJsonResponse(err);
                resp->setStatusCode(k401Unauthorized);
                co_return resp;
            }

            const std::string storedIp = resultRows[0]["last_ip"].as<std::string>();

            // Проверяем, совпадает ли IP.
            if (storedIp != clientIp)
            {
                Json::Value err;
                err["error"] = "ip mismatch";

                auto resp = HttpResponse::newHttpJsonResponse(err);
                resp->setStatusCode(k401Unauthorized);
                co_return resp;
            }

            // IP и токен совпадают. Возвращаем успешный ответ с токеном.
            Json::Value body;
            body["token"] = token;
            body["status"] = "ok";

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
