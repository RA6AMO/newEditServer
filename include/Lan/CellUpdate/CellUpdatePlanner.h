#pragma once

#include "Lan/CellUpdate/CellUpdateController.h"
#include "Lan/RowAdd/RowWriteTypes.h"
#include "Storage/MinioClient.h"

#include <drogon/utils/coroutine.h>

#include <memory>
#include <optional>
#include <string>
#include <unordered_map>

/// Интерфейс планировщика обновления для конкретной таблицы.
/// Расширение:
/// - Для новых таблиц/типов создавайте отдельные planner-ы.
/// - Регистрируйте их в CellUpdatePlannerRegistry.
class ITableCellUpdatePlanner
{
public:
    virtual ~ITableCellUpdatePlanner() = default;

    /// Валидация payload + attachments.
    virtual drogon::Task<std::optional<ValidationError>>
    validate(const CellUpdateController::ParsedRequest &parsed) const = 0;

    /// Строит план обновления: DB ops + uploads.
    virtual RowWritePlan buildUpdatePlan(int64_t rowId,
                                         const CellUpdateController::ParsedRequest &parsed,
                                         const std::unordered_map<std::string, std::string> &objectKeys,
                                         const MinioClient::Config &minioConfig) const = 0;
};

class CellUpdatePlannerRegistry
{
public:
    void registerPlanner(const std::string &tableName, std::shared_ptr<ITableCellUpdatePlanner> planner);
    std::shared_ptr<ITableCellUpdatePlanner> getPlanner(const std::string &tableName) const;

private:
    std::unordered_map<std::string, std::shared_ptr<ITableCellUpdatePlanner>> planners_;
};

std::shared_ptr<CellUpdatePlannerRegistry> createDefaultCellUpdatePlannerRegistry();
