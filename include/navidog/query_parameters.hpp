#pragma once

#include "metadata_cache.hpp"
#include <QVariant>

namespace navidog {
enum class QueryParameterType { Unknown, Text, Integer, ExactNumber, Real, Boolean, Date, Time, Timestamp, Binary };
struct QueryParameterLocation {
    int statement = 0, line = 0, byteColumn = 0;
};
struct QueryParameter {
    QString name;
    QueryParameterType type = QueryParameterType::Unknown;
    QString evidence;
    QVector<QueryParameterLocation> locations;
};
struct ParameterizedStatement {
    int index = 0;
    QString original, sql;
    QVector<int> parameters;
    QVector<BoundQueryMarker> markers;
};
struct QueryParameterPlan {
    QVector<QueryParameter> parameters;
    QVector<ParameterizedStatement> statements;
};
bool mayHaveQueryParameters(DatabaseKind, const QString&);
QueryParameterPlan inspectQueryParameters(DatabaseKind, const QString& sql, const QString& database,
                                          const QString& schema, MetadataSnapshotPtr metadata = {});
QVector<BoundQueryStatement> bindQueryParameters(const QueryParameterPlan&, const QVector<QVariant>&);
bool rewriteQueryPredicate(DatabaseKind, QByteArray& predicate, int originalOffset,
                           const BoundQueryStatement*, QVector<QVariant>&);
QString queryParameterTypeName(QueryParameterType);
std::optional<QVariant> parseQueryParameterValue(QueryParameterType, const QString& text, bool isNull);
}
