#pragma once
#include "types.hpp"
namespace navidog {
enum class ResultExportFormat { Csv, Json, Sql };
QByteArray exportResult(const QueryResult&,ResultExportFormat,DatabaseKind,const QString& tableName={});
}
