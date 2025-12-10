#pragma once

#include <drogon/drogon.h>

// Инициализация и запуск Drogon сервера с загрузкой конфигурации из config.json
// 
// Согласно документации Drogon (https://drogonframework.github.io/drogon-docs):
// - loadConfigFile() должен быть ПЕРВЫМ вызовом, до любых других настроек
// - run() блокирует выполнение до остановки сервера
// - Конфигурация загружается из config.json в корне проекта
void initDrogon()
{
    // Загрузка конфигурации из config.json
    // ВАЖНО: loadConfigFile должен быть ПЕРВЫМ вызовом, до любых других настроек!
    // Путь указывается относительно рабочей директории при запуске приложения
    drogon::app().loadConfigFile("config.json");
    
    // Запускаем HTTP-сервер (блокирует выполнение до остановки)
    // Все настройки (порт, адрес, БД и т.д.) берутся из config.json
    drogon::app().run();
}