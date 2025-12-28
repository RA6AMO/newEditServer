#pragma once

#include <string>
#include <fstream>
#include <mutex>
#include <source_location>
#include <chrono>
#include <iomanip>
#include <sstream>
#include <filesystem>

/// Уровни логирования от самого детального до критического
enum class LogLevel
{
    TRACE = 0,   ///< Детальная трассировка выполнения (самый низкий приоритет)
    DEBUG,       ///< Отладочная информация
    INFO,        ///< Информационные сообщения
    WARNING,     ///< Предупреждения
    ERROR,       ///< Ошибки
    CRITICAL     ///< Критические ошибки (самый высокий приоритет)
};

/// Потокобезопасный синглтон-логгер для записи в файлы и консоль
///
/// Особенности:
/// - Thread-safe: использует std::mutex для защиты записи
/// - Разделение по файлам: trace.log (TRACE/DEBUG), info.log (INFO/WARNING), error.log (ERROR/CRITICAL)
/// - Ротация по размеру: при превышении лимита (10MB) файлы переименовываются (.log.1, .log.2, ...)
/// - Гибкая настройка: можно отключить вывод в консоль или файл
/// - Автоматическое определение файла/строки через C++20 source_location
///
/// Пример использования:
/// \code
///     // Простое использование через макросы (рекомендуется)
///     LOG_TRACE("Детальная информация: {}", value);
///     LOG_DEBUG("Отладочное сообщение");
///     LOG_INFO("Сервер запущен на порту {}", 8080);
///     LOG_WARNING("Медленный запрос: {}ms", elapsed);
///     LOG_ERROR("Ошибка подключения: {}", error);
///     LOG_CRITICAL("Критическая ошибка!");
///
///     // Настройка вывода
///     auto& log = Logger::instance();
///     log.enableConsole(false);  // Отключить консоль (только файл)
///     log.enableFile(false);      // Отключить файл (только консоль)
///     log.setMinLevel(LogLevel::INFO);  // Игнорировать TRACE и DEBUG
///
///     // Прямой вызов (редко нужно)
///     log.log(LogLevel::ERROR, std::source_location::current(), "Сообщение");
/// \endcode
class Logger
{
public:
    /// Получить единственный экземпляр логгера (Meyers Singleton, thread-safe с C++11)
    /// \return Ссылка на экземпляр логгера
    static Logger& instance();

    /// Записать сообщение в лог
    /// \param level Уровень логирования
    /// \param location Информация о месте вызова (автоматически заполняется макросами)
    /// \param message Текст сообщения
    void log(LogLevel level, const std::source_location& location, const std::string& message);

    /// Удобные методы для каждого уровня (используются макросами)
    void trace(const std::string& message, const std::source_location& location = std::source_location::current());
    void debug(const std::string& message, const std::source_location& location = std::source_location::current());
    void info(const std::string& message, const std::source_location& location = std::source_location::current());
    void warning(const std::string& message, const std::source_location& location = std::source_location::current());
    void error(const std::string& message, const std::source_location& location = std::source_location::current());
    void critical(const std::string& message, const std::source_location& location = std::source_location::current());

    /// Включить/выключить вывод в консоль
    /// \param enable true - выводить в консоль, false - не выводить
    void enableConsole(bool enable);

    /// Включить/выключить вывод в файл
    /// \param enable true - записывать в файл, false - не записывать
    void enableFile(bool enable);

    /// Установить минимальный уровень логирования (сообщения ниже этого уровня игнорируются)
    /// \param level Минимальный уровень (по умолчанию TRACE - логируется всё)
    void setMinLevel(LogLevel level);

    // Запрещаем копирование и присваивание (синглтон)
    Logger(const Logger&) = delete;
    Logger& operator=(const Logger&) = delete;

private:
    /// Приватный конструктор (синглтон)
    Logger();
    
    /// Деструктор
    ~Logger();

    /// Получить имя файла для уровня логирования
    /// \param level Уровень логирования
    /// \return Имя файла (trace.log, info.log или error.log)
    std::string getFileName(LogLevel level) const;

    /// Получить поток для уровня логирования
    /// \param level Уровень логирования
    /// \return Ссылка на соответствующий файловый поток
    std::ofstream& getFileStream(LogLevel level);

    /// Ротация конкретного файла
    /// \param filePath Путь к файлу
    void rotateFile(const std::string& filePath);

    /// Записать сообщение в файл
    /// \param level Уровень логирования
    /// \param formattedMessage Отформатированное сообщение
    void writeToFile(LogLevel level, const std::string& formattedMessage);

    /// Записать сообщение в консоль
    /// \param formattedMessage Отформатированное сообщение
    /// \param level Уровень логирования (для выбора stdout/stderr)
    void writeToConsole(const std::string& formattedMessage, LogLevel level);

    /// Отформатировать сообщение с timestamp, уровнем, файлом и строкой
    /// \param level Уровень логирования
    /// \param location Информация о месте вызова
    /// \param message Текст сообщения
    /// \return Отформатированная строка
    std::string formatMessage(LogLevel level, const std::source_location& location, const std::string& message) const;

    /// Получить строковое представление уровня логирования
    /// \param level Уровень логирования
    /// \return Строка (TRACE, DEBUG, INFO, WARNING, ERROR, CRITICAL)
    std::string levelToString(LogLevel level) const;

    /// Получить текущий размер файла
    /// \param filePath Путь к файлу
    /// \return Размер файла в байтах
    size_t getFileSize(const std::string& filePath) const;

    /// Создать директорию для логов, если её нет
    void ensureLogDirectory() const;

    std::mutex mutex_;                    ///< Мьютекс для потокобезопасности
    std::ofstream traceFile_;             ///< Файл для TRACE и DEBUG
    std::ofstream infoFile_;              ///< Файл для INFO и WARNING
    std::ofstream errorFile_;             ///< Файл для ERROR и CRITICAL
    
    bool consoleEnabled_ = true;          ///< Включен ли вывод в консоль
    bool fileEnabled_ = true;             ///< Включена ли запись в файл
    LogLevel minLevel_ = LogLevel::TRACE; ///< Минимальный уровень логирования
};

/// Макросы для удобного логирования с автоматическим определением файла и строки
///
/// Примеры:
/// \code
///     LOG_TRACE("Обработка запроса ID: {}", requestId);
///     LOG_ERROR("Не удалось подключиться к БД: {}", errorMessage);
///     LOG_INFO("Сервер запущен");
/// \endcode

#define LOG_TRACE(msg) Logger::instance().trace(msg)
#define LOG_DEBUG(msg) Logger::instance().debug(msg)
#define LOG_INFO(msg)  Logger::instance().info(msg)
#define LOG_WARNING(msg) Logger::instance().warning(msg)
#define LOG_ERROR(msg) Logger::instance().error(msg)
#define LOG_CRITICAL(msg) Logger::instance().critical(msg)
