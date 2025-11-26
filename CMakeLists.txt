cmake_minimum_required(VERSION 3.20)

project(workshop_server LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_C_STANDARD 17)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# Qt6: автогенерация MOC (только для Q_OBJECT, без UI)
set(CMAKE_AUTOMOC ON)

# Пусть CMake сможет найти Qt, если путь не передан извне
if(WIN32 AND NOT DEFINED CMAKE_PREFIX_PATH)
  set(CMAKE_PREFIX_PATH "C:/Qt/6.9.2/mingw_64")
endif()

## Явная линковка вместо слепого GLOB по всем .a/.dll.a

# Пути к заголовкам сторонних библиотек
set(THIRD_PARTY_INCLUDE_DIR "${CMAKE_SOURCE_DIR}/libs/include")

file(GLOB_RECURSE PROJECT_SOURCES CONFIGURE_DEPENDS
  "${CMAKE_SOURCE_DIR}/src/*.cpp"
  "${CMAKE_SOURCE_DIR}/src/TreeWidgets/*.cpp"
  "${CMAKE_SOURCE_DIR}/src/DB/*.cpp"
  "${CMAKE_SOURCE_DIR}/src/Table/*.cpp"
  "${CMAKE_SOURCE_DIR}/src/workers/*.cpp"
)

# Добавляем заголовочные файлы для правильной работы MOC с Q_OBJECT
file(GLOB_RECURSE PROJECT_HEADERS CONFIGURE_DEPENDS
  "${CMAKE_SOURCE_DIR}/include/*.h"
  "${CMAKE_SOURCE_DIR}/include/TreeWidgets/*.h"
  "${CMAKE_SOURCE_DIR}/include/DB/*.h"
  "${CMAKE_SOURCE_DIR}/include/Table/*.h"
  "${CMAKE_SOURCE_DIR}/include/workers/*.h"
)

add_executable(app ${PROJECT_SOURCES} ${PROJECT_HEADERS})

# Используем найденные папки с заголовками
target_include_directories(app PRIVATE
    "${CMAKE_SOURCE_DIR}/include"
    "${CMAKE_SOURCE_DIR}/include/TreeWidgets"
    "${CMAKE_SOURCE_DIR}/include/DB"
    "${CMAKE_SOURCE_DIR}/include/Table"
    ${THIRD_PARTY_INCLUDE_DIR}

)

# Qt6 модули (только Core и Network для сервера)
set(QT_REQUIRED_COMPONENTS Core Network)
find_package(Qt6 REQUIRED COMPONENTS ${QT_REQUIRED_COMPONENTS})

# Линкуем библиотеки
target_link_libraries(app PRIVATE
  Qt6::Core
  Qt6::Network
)

if(WIN32)
  # Устаревшие системные зависимости нужны только при сборке под Windows
  target_link_libraries(app PRIVATE shell32 advapi32 ole32 uuid winmm ws2_32)
endif()

if(MSVC)
  target_compile_options(app PRIVATE /W4 /permissive-)
else()
  target_compile_options(app PRIVATE -Wall -Wextra -Wpedantic)
endif()
