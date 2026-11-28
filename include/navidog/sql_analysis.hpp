#pragma once
#include <QStringList>
#include <QVector>
#include <QJsonObject>
#include <QByteArray>

namespace navidog {
enum class SqlAnalysisState { Parsed, NotCovered, LimitReached, Stopped };
struct SqlStatementAnalysis {
    int index = 0; // Same zero-based index as the authoritative execution splitter.
    SqlAnalysisState state = SqlAnalysisState::NotCovered;
    QString kind, detail;
    QString generatedSql,generationDetail;
    QJsonObject unifiedAst; // Owning Model JSON; no driver/session handles.
    int sourceBegin=0;
    bool astTruncated=false;
    QStringList objects;
    int queries = 0, expressions = 0;
    int line = 1, byteColumn = 1;
};
struct SqlExecutionAnalysis {
    QVector<SqlStatementAnalysis> statements;
    int totalStatements = 0;
    QByteArray sourceHash;
    qint64 elapsedMs = 0;
    bool truncated = false;
};
} // namespace navidog
