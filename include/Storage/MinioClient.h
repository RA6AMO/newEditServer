#pragma once

#include <string>
#include <vector>
#include <memory>
#include <optional>
#include <mutex>

/// Обёртка над MinIO C++ SDK для загрузки и удаления объектов.
/// Использует minio-cpp SDK (https://github.com/minio/minio-cpp)
class MinioClient
{
public:
    /// Структура для хранения конфигурации MinIO
    struct Config
    {
        std::string endpoint;    // например "localhost:9000"
        std::string accessKey;
        std::string secretKey;
        std::string bucket;      // имя bucket по умолчанию
        bool useSSL = false;     // использовать ли HTTPS
    };

    /// Инициализация клиента с конфигурацией
    explicit MinioClient(const Config &config);
    
    /// Деструктор
    ~MinioClient();

    // Запрещаем копирование (клиент владеет ресурсами)
    MinioClient(const MinioClient &) = delete;
    MinioClient &operator=(const MinioClient &) = delete;

    /// Загрузить объект в MinIO
    /// @param bucket имя bucket (если пустое, используется из config)
    /// @param objectKey ключ объекта (путь)
    /// @param data данные для загрузки
    /// @param contentType MIME-тип (опционально)
    /// @return true при успехе, false при ошибке
    bool putObject(const std::string &bucket,
                   const std::string &objectKey,
                   const std::vector<uint8_t> &data,
                   const std::string &contentType = "");

    /// Загрузить объект в MinIO (перегрузка для string_view)
    bool putObject(const std::string &bucket,
                   const std::string &objectKey,
                   const std::string_view &data,
                   const std::string &contentType = "");

    /// Удалить объект из MinIO
    /// @param bucket имя bucket
    /// @param objectKey ключ объекта
    /// @return true при успехе, false при ошибке
    bool deleteObject(const std::string &bucket, const std::string &objectKey);

    /// Выгрузить (скачать) объект из MinIO
    /// @param bucket имя bucket (если пустое, используется из config)
    /// @param objectKey ключ объекта
    /// @param outData буфер, в который будет записано содержимое объекта (перезаписывается)
    /// @param outContentType опционально: MIME-тип из ответа (если доступен)
    /// @return true при успехе, false при ошибке
    bool getObject(const std::string &bucket,
                   const std::string &objectKey,
                   std::vector<uint8_t> &outData,
                   std::string *outContentType = nullptr);

    /// Получить конфигурацию
    const Config &getConfig() const { return config_; }

    /// Последняя ошибка, полученная от SDK (для диагностики).
    /// Возвращает пустую строку, если последняя операция прошла успешно
    /// или если ошибка не была установлена.
    std::string lastError() const;

private:
    Config config_;
    // PIMPL: скрываем детали реализации minio-cpp
    class Impl;
    std::unique_ptr<Impl> pImpl_;

    void setLastError(std::string err);
    void clearLastError();
    mutable std::mutex lastErrorMutex_;
    std::string lastError_;
};

