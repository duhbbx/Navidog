#pragma once
#include "background_tasks.hpp"
namespace navidog {
enum class ResultExportFormat { Csv, Json, Sql };
QByteArray exportResult(const QueryResult&,ResultExportFormat,DatabaseKind,const QString& tableName={});
TaskResult exportResultFile(const QString& path, const QueryResult&, ResultExportFormat,
                            DatabaseKind, const QString& tableName, const TaskContext&);
}
