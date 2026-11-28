#pragma once
#include "database_types.hpp"
#include <QByteArray>
#include <QMap>
#include <QString>
#include <QStringList>
#include <QVector>
#include <optional>
namespace navidog {
struct EstimatedPlanNode {
  QString operation, object, index, alias;
  std::optional<double> rows, startupCost, totalCost, rowBytes;
  std::optional<double> estimatedTimeUs; // Vendor estimate, distinct from COST and actual duration.
  QMap<QString, QString> properties;
  QVector<int> children;
};
// In-memory evidence of the actual planning requests sent through a driver.
// No credentials/endpoints; SQL is included only for the user's explicit view/export.
struct PlanRequestTrace {
  QString connectionName,database,schema,serverVersion,requestedAtUtc;
  QStringList commands;
  qint64 elapsedMs=0;
  bool issued=false,received=false,isolated=true;
};
struct EstimatedPlan {
  DatabaseKind driver = DatabaseKind::Unknown;
  PlanRequestTrace request;
  QString format, warning;
  QByteArray raw;
  QVector<EstimatedPlanNode> nodes;
  QVector<int> roots;
  bool complete = false;
};
struct PlanParseLimits {
  int maxBytes = 4 * 1024 * 1024, maxNodes = 10000, maxDepth = 128;
};
struct QueryResult;
// Bounded, offline decoder. All COST/row counts are server estimates; never
// substitutes measurements or runs SQL. Raw data is retained for inspection.
EstimatedPlan decodeEstimatedPlan(DatabaseKind, const QueryResult &,
                                  PlanParseLimits = {});
QByteArray estimatedPlanJson(const EstimatedPlan &,
                             int maxBytes = 4 * 1024 * 1024);
} // namespace navidog
