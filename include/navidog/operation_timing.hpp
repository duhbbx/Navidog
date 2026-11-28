#pragma once

#include <QByteArray>
#include <QElapsedTimer>
#include <limits>

namespace navidog {
enum class TimedOperation {
    UiEventLoopDelay,
    QueryExecute,
    SqlAnalyze,
    Explain,
    ApplyQueryResults,
    OpenConnection,
    TestConnection,
    LoadCatalog,
    ApplyCatalog,
    LoadMetadataCache,
    RefreshMetadata,
    ReadTableData,
    ApplyTableData,
    SaveTableData,
    ReadTableDefinition,
    SaveTableDefinition,
    ReadSourceDefinition,
    SaveSourceDefinition,
    SqlFormat,
    SqlConvert,
    SqlOptimize,
    ErImport,
    ErLayout,
    ErDisplay,
    ErExport,
    BackupOrRestore,
    ReadSqlFile,
    DisplaySqlFile
};

inline constexpr qint64 operationLogThresholdMs = 100;
inline constexpr int operationLogMaxEntries = 512;
inline constexpr int operationLogMaxFileBytes = 128 * 1024;

void recordOperationTiming(TimedOperation operation, qint64 elapsedMs);

class OperationTimer final {
public:
    explicit OperationTimer(TimedOperation operation) : operation_(operation) { timer_.start(); }
    ~OperationTimer() { recordOperationTiming(operation_, timer_.elapsed()); }

    OperationTimer(const OperationTimer&) = delete;
    OperationTimer& operator=(const OperationTimer&) = delete;

private:
    TimedOperation operation_;
    QElapsedTimer timer_;
};

struct OperationLogSnapshot {
    quint64 revision;
    QByteArray text;
};

OperationLogSnapshot operationLogSnapshot(
    quint64 lastSavedRevision = std::numeric_limits<quint64>::max());
void restoreOperationLog(const QByteArray& report);
}
