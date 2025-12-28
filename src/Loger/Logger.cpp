#include "Loger/Logger.h"

#include <iostream>
#include <thread>
#include <ctime>

// Конфигурация логгера
#define LOG_DIR "./logs/"                    // Путь к директории с логами
#define LOG_MAX_FILE_SIZE (10 * 1024 * 1024) // Максимальный размер файла: 10 MB
#define LOG_MAX_BACKUP_FILES 5               // Максимальное количество архивных файлов (.log.1 ... .log.5)

Logger::Logger()
{
    ensureLogDirectory();
    
    // Открываем файлы в режиме append (добавление в конец)
    traceFile_.open(std::string(LOG_DIR) + "trace.log", std::ios::app);
    infoFile_.open(std::string(LOG_DIR) + "info.log", std::ios::app);
    errorFile_.open(std::string(LOG_DIR) + "error.log", std::ios::app);
    
    // Проверяем, что файлы открылись успешно
    if (!traceFile_.is_open() || !infoFile_.is_open() || !errorFile_.is_open())
    {
        std::cerr << "ERROR: Failed to open log files!" << std::endl;
    }
}

Logger::~Logger()
{
    std::lock_guard<std::mutex> lock(mutex_);
    
    if (traceFile_.is_open())
        traceFile_.close();
    if (infoFile_.is_open())
        infoFile_.close();
    if (errorFile_.is_open())
        errorFile_.close();
}

Logger& Logger::instance()
{
    static Logger inst;
    return inst;
}

void Logger::log(LogLevel level, const std::source_location& location, const std::string& message)
{
    // Проверяем минимальный уровень
    if (level < minLevel_)
        return;
    
    std::lock_guard<std::mutex> lock(mutex_);
    
    // Форматируем сообщение
    std::string formatted = formatMessage(level, location, message);
    
    // Записываем в файл, если включено
    if (fileEnabled_)
    {
        writeToFile(level, formatted);
    }
    
    // Выводим в консоль, если включено
    if (consoleEnabled_)
    {
        writeToConsole(formatted, level);
    }
}

void Logger::trace(const std::string& message, const std::source_location& location)
{
    log(LogLevel::TRACE, location, message);
}

void Logger::debug(const std::string& message, const std::source_location& location)
{
    log(LogLevel::DEBUG, location, message);
}

void Logger::info(const std::string& message, const std::source_location& location)
{
    log(LogLevel::INFO, location, message);
}

void Logger::warning(const std::string& message, const std::source_location& location)
{
    log(LogLevel::WARNING, location, message);
}

void Logger::error(const std::string& message, const std::source_location& location)
{
    log(LogLevel::ERROR, location, message);
}

void Logger::critical(const std::string& message, const std::source_location& location)
{
    log(LogLevel::CRITICAL, location, message);
}

void Logger::enableConsole(bool enable)
{
    std::lock_guard<std::mutex> lock(mutex_);
    consoleEnabled_ = enable;
}

void Logger::enableFile(bool enable)
{
    std::lock_guard<std::mutex> lock(mutex_);
    fileEnabled_ = enable;
}

void Logger::setMinLevel(LogLevel level)
{
    std::lock_guard<std::mutex> lock(mutex_);
    minLevel_ = level;
}

std::string Logger::getFileName(LogLevel level) const
{
    switch (level)
    {
        case LogLevel::TRACE:
        case LogLevel::DEBUG:
            return std::string(LOG_DIR) + "trace.log";
        case LogLevel::INFO:
        case LogLevel::WARNING:
            return std::string(LOG_DIR) + "info.log";
        case LogLevel::ERROR:
        case LogLevel::CRITICAL:
            return std::string(LOG_DIR) + "error.log";
    }
    return std::string(LOG_DIR) + "error.log"; // fallback
}

std::ofstream& Logger::getFileStream(LogLevel level)
{
    switch (level)
    {
        case LogLevel::TRACE:
        case LogLevel::DEBUG:
            return traceFile_;
        case LogLevel::INFO:
        case LogLevel::WARNING:
            return infoFile_;
        case LogLevel::ERROR:
        case LogLevel::CRITICAL:
            return errorFile_;
    }
    return errorFile_; // fallback
}


void Logger::rotateFile(const std::string& filePath)
{
    // Удаляем самый старый файл, если он существует
    std::string oldestFile = filePath + "." + std::to_string(LOG_MAX_BACKUP_FILES);
    if (std::filesystem::exists(oldestFile))
    {
        std::filesystem::remove(oldestFile);
    }
    
    // Сдвигаем файлы: .log.4 -> .log.5, .log.3 -> .log.4, и т.д.
    for (int i = LOG_MAX_BACKUP_FILES - 1; i >= 1; --i)
    {
        std::string oldFile = filePath + "." + std::to_string(i);
        std::string newFile = filePath + "." + std::to_string(i + 1);
        
        if (std::filesystem::exists(oldFile))
        {
            std::filesystem::rename(oldFile, newFile);
        }
    }
    
    // Переименовываем текущий файл в .log.1
    if (std::filesystem::exists(filePath))
    {
        std::string firstBackup = filePath + ".1";
        std::filesystem::rename(filePath, firstBackup);
    }
}

void Logger::writeToFile(LogLevel level, const std::string& formattedMessage)
{
    std::ofstream& stream = getFileStream(level);
    std::string filePath = getFileName(level);
    
    // Проверяем необходимость ротации перед записью
    // Закрываем файл для ротации (на Linux нельзя переименовать открытый файл)
    if (stream.is_open())
    {
        size_t currentSize = getFileSize(filePath);
        if (currentSize >= LOG_MAX_FILE_SIZE)
        {
            stream.close();
            rotateFile(filePath);
            stream.open(filePath, std::ios::app);
        }
    }
    
    // Записываем сообщение
    if (stream.is_open())
    {
        stream << formattedMessage << std::endl;
        
        // Немедленный flush для критических уровней (чтобы не потерять при крэше)
        if (level == LogLevel::ERROR || level == LogLevel::CRITICAL)
        {
            stream.flush();
        }
    }
}

void Logger::writeToConsole(const std::string& formattedMessage, LogLevel level)
{
    // ERROR и CRITICAL идут в stderr, остальное в stdout
    if (level == LogLevel::ERROR || level == LogLevel::CRITICAL)
    {
        std::cerr << formattedMessage << std::endl;
        std::cerr.flush();
    }
    else
    {
        std::cout << formattedMessage << std::endl;
    }
}

std::string Logger::formatMessage(LogLevel level, const std::source_location& location, const std::string& message) const
{
    // Получаем текущее время
    auto now = std::chrono::system_clock::now();
    auto time_t = std::chrono::system_clock::to_time_t(now);
    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        now.time_since_epoch()) % 1000;
    
    std::tm tm_buf;
    localtime_r(&time_t, &tm_buf);
    
    // Форматируем timestamp: [YYYY-MM-DD HH:MM:SS.mmm]
    std::ostringstream oss;
    oss << std::put_time(&tm_buf, "[%Y-%m-%d %H:%M:%S");
    oss << "." << std::setfill('0') << std::setw(3) << ms.count() << "]";
    
    // Добавляем уровень логирования
    oss << " [" << levelToString(level) << "]";
    
    // Добавляем информацию о потоке
    oss << " [thread:" << std::this_thread::get_id() << "]";
    
    // Добавляем информацию о файле и строке
    std::string fileName = location.file_name();
    // Оставляем только имя файла (без пути)
    size_t lastSlash = fileName.find_last_of("/\\");
    if (lastSlash != std::string::npos)
    {
        fileName = fileName.substr(lastSlash + 1);
    }
    oss << " [" << fileName << ":" << location.line() << "]";
    
    // Добавляем сообщение
    oss << " " << message;
    
    return oss.str();
}

std::string Logger::levelToString(LogLevel level) const
{
    switch (level)
    {
        case LogLevel::TRACE:   return "TRACE";
        case LogLevel::DEBUG:   return "DEBUG";
        case LogLevel::INFO:    return "INFO";
        case LogLevel::WARNING: return "WARNING";
        case LogLevel::ERROR:   return "ERROR";
        case LogLevel::CRITICAL: return "CRITICAL";
    }
    return "UNKNOWN";
}

size_t Logger::getFileSize(const std::string& filePath) const
{
    if (!std::filesystem::exists(filePath))
        return 0;
    
    try
    {
        return std::filesystem::file_size(filePath);
    }
    catch (...)
    {
        return 0;
    }
}

void Logger::ensureLogDirectory() const
{
    std::string dir = LOG_DIR;
    
    // Убираем завершающий слэш для создания директории
    if (!dir.empty() && (dir.back() == '/' || dir.back() == '\\'))
    {
        dir.pop_back();
    }
    
    // Создаём директорию, если её нет
    if (!dir.empty() && !std::filesystem::exists(dir))
    {
        try
        {
            std::filesystem::create_directories(dir);
        }
        catch (const std::exception& e)
        {
            std::cerr << "ERROR: Failed to create log directory '" << dir << "': " << e.what() << std::endl;
        }
    }
}
