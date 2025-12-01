#include <iostream>
#include "initConnerction.h"


int main(int argc, char *argv[])
{
    std::cout << "Starting Drogon HTTP server on 0.0.0.0:8080..." << std::endl;

    // Функция не вернётся, пока сервер не будет остановлен.
    initDrogon();

    return 0;
}
