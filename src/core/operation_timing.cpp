#include "navidog/operation_timing.hpp"

#include <QCoreApplication>
#include <QDateTime>
#include <QMutex>
#include <QQueue>
#include <QThread>
#include <utility>

namespace navidog {
namespace {
constexpr auto header = "time_utc\tthread\toperation\telapsed_ms";

const char* operationName(TimedOperation operation) {
    switch (operation) {
    case TimedOperation::UiEventLoopDelay: return "ui_event_loop_delay";
    case TimedOperation::QueryExecute: return "query_execute";
    case TimedOperation::SqlAnalyze: return "sql_analyze";
    case TimedOperation::Explain: return "explain";
    case TimedOperation::ApplyQueryResults: return "apply_query_results";
    case TimedOperation::OpenConnection: return "open_connection";
    case TimedOperation::TestConnection: return "test_connection";
    case TimedOperation::LoadCatalog: return "load_catalog";
    case TimedOperation::ApplyCatalog: return "apply_catalog";
    case TimedOperation::LoadMetadataCache: return "load_metadata_cache";
    case TimedOperation::RefreshMetadata: return "refresh_metadata";
    case TimedOperation::ReadTableData: return "read_table_data";
    case TimedOperation::ApplyTableData: return "apply_table_data";
    case TimedOperation::SaveTableData: return "save_table_data";
    case TimedOperation::ReadTableDefinition: return "read_table_definition";
    case TimedOperation::SaveTableDefinition: return "save_table_definition";
    case TimedOperation::ReadSourceDefinition: return "read_source_definition";
    case TimedOperation::SaveSourceDefinition: return "save_source_definition";
    case TimedOperation::SqlFormat: return "sql_format";
    case TimedOperation::SqlConvert: return "sql_convert";
    case TimedOperation::SqlOptimize: return "sql_optimize";
    case TimedOperation::ErImport: return "er_import";
    case TimedOperation::ErLayout: return "er_layout";
    case TimedOperation::ErDisplay: return "er_display";
    case TimedOperation::ErExport: return "er_export";
    case TimedOperation::BackupOrRestore: return "backup_or_restore";
    case TimedOperation::ReadSqlFile: return "read_sql_file";
    case TimedOperation::DisplaySqlFile: return "display_sql_file";
    }
    return "unknown";
}

struct OperationLog {
    QMutex mutex;
    QQueue<QByteArray> entries;
    quint64 revision = 0;
};

OperationLog& operationLog() {
    static OperationLog log;
    return log;
}
}

void recordOperationTiming(TimedOperation operation, qint64 elapsedMs) {
    if (elapsedMs < operationLogThresholdMs)
        return;
    const auto* app = QCoreApplication::instance();
    const bool ui = app && QThread::currentThread() == app->thread();
    const auto time = QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs).toLatin1();
    const auto entry = time + '\t' + (ui ? "ui" : "worker") + '\t' +
                       operationName(operation) + '\t' + QByteArray::number(elapsedMs);
    auto& log = operationLog();
    QMutexLocker lock(&log.mutex);
    if (log.entries.size() == operationLogMaxEntries)
        log.entries.dequeue();
    log.entries.enqueue(entry);
    ++log.revision;
}

OperationLogSnapshot operationLogSnapshot(quint64 lastSavedRevision) {
    auto& log = operationLog();
    QMutexLocker lock(&log.mutex);
    OperationLogSnapshot snapshot{log.revision, {}};
    if (log.revision == lastSavedRevision)
        return snapshot;
    snapshot.text = QByteArray(header) + '\n';
    for (const auto& entry : std::as_const(log.entries))
        snapshot.text += entry + '\n';
    return snapshot;
}

void restoreOperationLog(const QByteArray& report) {
    const auto lines = report.split('\n');
    if (lines.isEmpty() || lines.front() != header)
        return;
    auto& log = operationLog();
    QMutexLocker lock(&log.mutex);
    for (auto index = lines.size() - 1; index > 0 &&
         log.entries.size() < operationLogMaxEntries; --index) {
        const auto& row = lines.at(index);
        if (row.size() <= 128 && row.count('\t') == 3)
            log.entries.prepend(row);
    }
}
}
