#include "Storage/MinioClient.h"

#include "Loger/Logger.h"

#include <miniocpp/client.h>
#include <sstream>
#include <iostream>

class MinioClient::Impl
{
public:
    std::unique_ptr<minio::s3::Client> client;
    std::unique_ptr<minio::creds::StaticProvider> credProvider;
};

void MinioClient::setLastError(std::string err)
{
    std::lock_guard<std::mutex> lk(lastErrorMutex_);
    lastError_ = std::move(err);
}

void MinioClient::clearLastError()
{
    std::lock_guard<std::mutex> lk(lastErrorMutex_);
    lastError_.clear();
}

std::string MinioClient::lastError() const
{
    std::lock_guard<std::mutex> lk(lastErrorMutex_);
    return lastError_;
}

MinioClient::MinioClient(const Config &config)
    : config_(config), pImpl_(std::make_unique<Impl>())
{
    if (config_.endpoint.empty() || config_.accessKey.empty() || config_.secretKey.empty())
    {
        throw std::invalid_argument("MinIO config: endpoint, accessKey and secretKey are required");
    }

    // Создаём base URL
    minio::s3::BaseUrl baseUrl(config_.endpoint, config_.useSSL);

    // Создаём провайдера учётных данных
    pImpl_->credProvider = std::make_unique<minio::creds::StaticProvider>(
        config_.accessKey, config_.secretKey);

    // Создаём клиент MinIO
    pImpl_->client = std::make_unique<minio::s3::Client>(baseUrl, pImpl_->credProvider.get());
}

MinioClient::~MinioClient() = default;

bool MinioClient::putObject(const std::string &bucket,
                            const std::string &objectKey,
                            const std::vector<uint8_t> &data,
                            const std::string &contentType)
{
    try
    {
        clearLastError();
        std::string bucketName = bucket.empty() ? config_.bucket : bucket;

        // Создаём поток из данных
        std::string dataStr(data.begin(), data.end());
        std::istringstream stream(dataStr);

        // Формируем аргументы для загрузки
        minio::s3::PutObjectArgs args(stream, static_cast<long>(data.size()), 0);
        args.bucket = bucketName;
        args.object = objectKey;

        if (!contentType.empty())
        {
            args.content_type = contentType;
        }

        // Загружаем объект
        minio::s3::PutObjectResponse resp = pImpl_->client->PutObject(args);

        if (!resp)
        {
            std::ostringstream oss;
            oss << "MinIO putObject failed"
                << " endpoint=" << config_.endpoint
                << " useSSL=" << (config_.useSSL ? "true" : "false")
                << " bucket=" << bucketName
                << " key=" << objectKey
                << " sizeBytes=" << data.size();
            if (!contentType.empty())
            {
                oss << " contentType=" << contentType;
            }
            oss << " error=" << resp.Error().String();
            Logger::instance().error(oss.str());
            setLastError(resp.Error().String());
            return false;
        }

        clearLastError();
        return true;
    }
    catch (const std::exception &e)
    {
        std::ostringstream oss;
        oss << "MinIO putObject exception"
            << " endpoint=" << config_.endpoint
            << " useSSL=" << (config_.useSSL ? "true" : "false")
            << " bucket=" << (bucket.empty() ? config_.bucket : bucket)
            << " key=" << objectKey
            << " sizeBytes=" << data.size()
            << " what=" << e.what();
        Logger::instance().error(oss.str());
        setLastError(e.what());
        return false;
    }
}

bool MinioClient::putObject(const std::string &bucket,
                            const std::string &objectKey,
                            const std::string_view &data,
                            const std::string &contentType)
{
    std::vector<uint8_t> vec(data.begin(), data.end());
    return putObject(bucket, objectKey, vec, contentType);
}

bool MinioClient::deleteObject(const std::string &bucket, const std::string &objectKey)
{
    try
    {
        clearLastError();
        std::string bucketName = bucket.empty() ? config_.bucket : bucket;

        minio::s3::RemoveObjectArgs args;
        args.bucket = bucketName;
        args.object = objectKey;

        minio::s3::RemoveObjectResponse resp = pImpl_->client->RemoveObject(args);

        if (!resp)
        {
            std::ostringstream oss;
            oss << "MinIO deleteObject failed"
                << " endpoint=" << config_.endpoint
                << " useSSL=" << (config_.useSSL ? "true" : "false")
                << " bucket=" << bucketName
                << " key=" << objectKey
                << " error=" << resp.Error().String();
            Logger::instance().error(oss.str());
            setLastError(resp.Error().String());
            return false;
        }

        clearLastError();
        return true;
    }
    catch (const std::exception &e)
    {
        std::ostringstream oss;
        oss << "MinIO deleteObject exception"
            << " endpoint=" << config_.endpoint
            << " useSSL=" << (config_.useSSL ? "true" : "false")
            << " bucket=" << (bucket.empty() ? config_.bucket : bucket)
            << " key=" << objectKey
            << " what=" << e.what();
        Logger::instance().error(oss.str());
        setLastError(e.what());
        return false;
    }
}

bool MinioClient::getObject(const std::string &bucket,
                            const std::string &objectKey,
                            std::vector<uint8_t> &outData,
                            std::string *outContentType)
{
    try
    {
        clearLastError();
        std::string bucketName = bucket.empty() ? config_.bucket : bucket;
        outData.clear();

        minio::s3::GetObjectArgs args;
        args.bucket = bucketName;
        args.object = objectKey;
        args.datafunc = [&outData](minio::http::DataFunctionArgs cbArgs) -> bool {
            const std::string &chunk = cbArgs.datachunk;
            outData.insert(outData.end(), chunk.begin(), chunk.end());
            return true;
        };

        minio::s3::GetObjectResponse resp = pImpl_->client->GetObject(args);

        if (!resp)
        {
            std::ostringstream oss;
            oss << "MinIO getObject failed"
                << " endpoint=" << config_.endpoint
                << " useSSL=" << (config_.useSSL ? "true" : "false")
                << " bucket=" << bucketName
                << " key=" << objectKey
                << " error=" << resp.Error().String();
            Logger::instance().error(oss.str());
            setLastError(resp.Error().String());
            return false;
        }

        if (outContentType)
        {
            *outContentType = resp.headers.GetFront("content-type");
        }

        clearLastError();
        return true;
    }
    catch (const std::exception &e)
    {
        std::ostringstream oss;
        oss << "MinIO getObject exception"
            << " endpoint=" << config_.endpoint
            << " useSSL=" << (config_.useSSL ? "true" : "false")
            << " bucket=" << (bucket.empty() ? config_.bucket : bucket)
            << " key=" << objectKey
            << " what=" << e.what();
        Logger::instance().error(oss.str());
        setLastError(e.what());
        return false;
    }
}
