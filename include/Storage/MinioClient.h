#pragma once

#include <string>
#include <vector>
#include <memory>
#include <optional>

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

    /// Получить конфигурацию
    const Config &getConfig() const { return config_; }

private:
    Config config_;
    // PIMPL: скрываем детали реализации minio-cpp
    class Impl;
    std::unique_ptr<Impl> pImpl_;
};

