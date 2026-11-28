#pragma once
#include "database.hpp"
#include "sql/optimizer/optimizer.hpp"
#include <QJsonObject>
namespace navidog {
struct PlannerStatisticsSnapshot {
  std::vector<sql::optimizer::TableStatistics> tables;
  qint64 refreshedMs = 0;
  bool complete = false;
  QString warning;
};
// Bulk catalog estimates only. Does not count user tables, ANALYZE, sample
// rows, read histograms or change statistics. Caller owns this worker's
// connection.
PlannerStatisticsSnapshot collectPlannerStatistics(Database &,
                                                   const ConnectionProfile &,
                                                   const QString &schema,
                                                   const Cancellation &);
QJsonObject plannerTableJson(const sql::optimizer::TableStatistics &);
sql::optimizer::TableStatistics plannerTableFromJson(const QJsonObject &);
} // namespace navidog
