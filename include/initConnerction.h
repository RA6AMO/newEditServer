#include <drogon/drogon.h>

// Простая инициализация Drogon без конфигурационного файла.
// При необходимости можно заменить на загрузку config.json:
// drogon::app().loadConfigFile("config.json");
void initDrogon()
{
    // Слушать на 0.0.0.0:8080
    drogon::app()
        .addListener("0.0.0.0", 8080)
        .run();
}