#include <iostream>
#include <drogon/drogon.h>
#include <thread>
#include <functional>

int main(int argc, char *argv[])
{


    // Запускаем HTTP-сервер на Drogon в отдельном потоке
    std::thread drogonThread([]() {
        drogon::app()
            .registerHandler(
                "/ping",
                [](const drogon::HttpRequestPtr &,
                   std::function<void(const drogon::HttpResponsePtr &)> callback) {
                    auto resp = drogon::HttpResponse::newHttpResponse();
                    resp->setBody("pong");
                    callback(resp);
                })
            .addListener("0.0.0.0", 8080)
            .setThreadNum(1)
            .run();
    });
    drogonThread.detach();


}

