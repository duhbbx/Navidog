#pragma once

#include "types.hpp"

namespace navidog {

struct QuerySafetyPolicy {
    bool enabled = true, confirm = true, warnings = true;
    bool previewBeforeRun = true, previewWhileEditing = false;
    bool confirmUnmeasured = false;
    int rowThreshold = 1000, percentThreshold = 90, timeoutMs = 2500;
};

enum class QueryRisk { None, NoFilter, AlwaysTrue, Destructive, Unverified };
enum class ImpactStatus { NotRequested, Pending, Unavailable, Measured };

struct QueryImpact {
    ImpactStatus status = ImpactStatus::NotRequested;
    int matched = 0, total = -1;
    bool atLeast = false;
    QString detail;
};

struct QuerySafetyFinding {
    int index = 0, line = 1, begin = 0, end = 0; // Editor positions in UTF-16.
    QString operation, target;
    QueryRisk risk = QueryRisk::None;
    bool syntaxError = false;
    QueryImpact impact;
    QByteArray table, predicate;
    int predicateOffset = 0; // Relative to the original statement, in UTF-8.
    std::optional<qint64> writeLimit;
    QString matchCommand, totalCommand; // Native command adapters.
};

QVector<QuerySafetyFinding> inspectQuerySafety(DatabaseKind, const QString&,
    const QuerySafetyPolicy&, const Cancellation& = {});
bool mightNeedQuerySafety(DatabaseKind, const QString&);
void measureQueryImpact(const ConnectionProfile&, const QString& schema,
    const QuerySafetyPolicy&, const QVector<BoundQueryStatement>&,
    QVector<QuerySafetyFinding>&, const Cancellation&, const ImpactQueryTrace& = {});
QString queryRiskMessage(QueryRisk);
QString queryImpactMessage(const QueryImpact&);
bool largeQueryImpact(const QueryImpact&, const QuerySafetyPolicy&);
bool requiresQueryConfirmation(const QuerySafetyFinding&, const QuerySafetyPolicy&);
}
