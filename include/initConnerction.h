#include <drogon/drogon.h>

// Инициализация Drogon с загрузкой конфигурации из config.json
// Конфигурация включает настройки сервера и подключения к PostgreSQL
void initDrogon()
{
    // Загружаем конфигурацию из config.json
    // Это автоматически создаст подключения к БД и настроит сервер
    drogon::app().loadConfigFile("/root/workshop-server/config.json");
    
    // Запускаем сервер (блокирующий вызов)
    drogon::app().run();
}