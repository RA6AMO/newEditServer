#include <drogon/drogon.h>

#define PORT 8080
#define HOST "0.0.0.0"
// Простая инициализация Drogon без конфигурационного файла.
// При необходимости можно заменить на загрузку config.json:
// drogon::app().loadConfigFile("config.json");
void initDrogon()
{
    // Слушать на 0.0.0.0:8080
    drogon::app()
        .addListener(HOST, PORT)
        .run();
}