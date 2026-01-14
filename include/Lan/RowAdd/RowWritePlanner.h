#pragma once

#include "Lan/RowController.h"
#include "Lan/RowWriteTypes.h"
#include "Storage/MinioClient.h"

#include <drogon/utils/coroutine.h>

#include <memory>
#include <optional>
#include <string>
#include <unordered_map>

/// Интерфейс планировщика записи для конкретной таблицы.
/// Расширение:
/// - Для новой таблицы/логики создайте новый класс, унаследованный от ITableRowWritePlanner.
/// - Зарегистрируйте его в RowWritePlannerRegistry (см. createDefaultRowWritePlannerRegistry()).
/// - Контроллеры/сервис записи при этом не меняются.
class ITableRowWritePlanner
{
public:
    virtual ~ITableRowWritePlanner() = default;

    /// Валидация payload + attachments.
    /// Возвращает ValidationError (код/сообщение/детали/HTTP) или nullopt.
    /// Расширение:
    /// - Здесь проверяйте типы (types[dbName]), роли и формат attachments.
    /// - Здесь же проверяйте whitelist колонок (например, image_*).
    virtual drogon::Task<std::optional<ValidationError>> validate(const RowController::ParsedRequest &parsed) const = 0;

    /// Создаёт базовую строку и возвращает rowId.
    /// Расширение:
    /// - Можно переиспользовать общую логику вставки (buildInsertCommand).
    /// - Важно: INSERT должен вернуть id (RETURNING id).
    virtual drogon::Task<int64_t> insertBaseRow(const RowController::ParsedRequest &parsed,
                                                const std::shared_ptr<drogon::orm::Transaction> &trans) const = 0;

    /// Строит план записи, который затем выполнит RowWriteService.
    /// Расширение:
    /// - Для новых типов файлов добавляйте UploadOp + DbOp в plan.
    /// - Вся семантика таблицы должна быть здесь, а не в контроллере.
    virtual RowWritePlan buildWritePlan(int64_t rowId,
                                        const RowController::ParsedRequest &parsed,
                                        const std::unordered_map<std::string, std::string> &objectKeys,
                                        const MinioClient::Config &minioConfig) const = 0;
};

class RowWritePlannerRegistry
{
public:
    /// Регистрирует планировщик для конкретного table.
    /// Расширение:
    /// - Для новой таблицы вызовите registerPlanner("table_name", planner).
    /// - Можно иметь несколько planner-ов для разных таблиц/схем.
    void registerPlanner(const std::string &tableName, std::shared_ptr<ITableRowWritePlanner> planner);
    std::shared_ptr<ITableRowWritePlanner> getPlanner(const std::string &tableName) const;

private:
    std::unordered_map<std::string, std::shared_ptr<ITableRowWritePlanner>> planners_;
};

std::shared_ptr<RowWritePlannerRegistry> createDefaultRowWritePlannerRegistry();

