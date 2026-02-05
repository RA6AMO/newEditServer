#pragma once

#include "Lan/RowDelete/RowDeleteTypes.h"
#include "Storage/MinioClient.h"

#include <drogon/utils/coroutine.h>

#include <memory>
#include <optional>
#include <string>
#include <unordered_map>

/// Интерфейс планировщика удаления для конкретной таблицы.
/// Расширение:
/// - Для новой таблицы/логики создайте новый класс, унаследованный от ITableRowDeletePlanner.
/// - Зарегистрируйте его в RowDeletePlannerRegistry (см. createDefaultRowDeletePlannerRegistry()).
/// - Сервис удаления при этом не меняется.
class ITableRowDeletePlanner
{
public:
    virtual ~ITableRowDeletePlanner() = default;

    /// Валидация запроса удаления.
    /// Возвращает ValidationError (код/сообщение/детали/HTTP) или nullopt.
    virtual drogon::Task<std::optional<DeleteValidationError>> validate(const RowDeleteRequest &request) const = 0;

    /// Строит план удаления (DB + storage).
    /// Расширение:
    /// - Добавляйте в plan.dbOps специфичную логику удаления из БД.
    /// - В plan.storageDeletes добавляйте ключи объектов для MinIO.
    virtual drogon::Task<RowDeletePlan> buildDeletePlan(const RowDeleteRequest &request,
                                                        const std::shared_ptr<drogon::orm::Transaction> &trans,
                                                        const MinioClient::Config &minioConfig) const = 0;
};

class RowDeletePlannerRegistry
{
public:
    /// Регистрирует планировщик для конкретного table.
    /// Расширение:
    /// - Для новой таблицы вызовите registerPlanner("table_name", planner).
    void registerPlanner(const std::string &tableName, std::shared_ptr<ITableRowDeletePlanner> planner);
    std::shared_ptr<ITableRowDeletePlanner> getPlanner(const std::string &tableName) const;

private:
    std::unordered_map<std::string, std::shared_ptr<ITableRowDeletePlanner>> planners_;
};

std::shared_ptr<RowDeletePlannerRegistry> createDefaultRowDeletePlannerRegistry();
