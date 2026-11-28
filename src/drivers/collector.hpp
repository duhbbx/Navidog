#pragma once
#include "navidog/types.hpp"
#include "navidog/sql.hpp"
#include <algorithm>
#include <stdexcept>
namespace navidog::native {
[[noreturn]] inline void fail(const QString& s) { throw std::runtime_error(s.toUtf8().constData()); }
inline void validate(const QString& sql, sql::Dialect dialect,bool definitionMode=false) {
    auto bytes = sql.toUtf8(); std::string error;
    if (bytes.contains('\0')) fail("SQL 包含空字符");
    if(definitionMode && sql::isCompoundDefinition(bytes.toStdString(),dialect))return;
    if (!sql::isSingleStatement(bytes.toStdString(), dialect, error)) fail(QString::fromStdString(error));
}
struct Collector {
    QueryResult result; QueryLimits limits;
    explicit Collector(QueryLimits l): limits(l) {
        if (l.maxRows < 1 || l.maxRows > 100000 || l.maxBytes < 128 || l.maxCellBytes < 1 || l.maxCellBytes > 1048576) fail("结果缓存限制无效");
    }
    bool add(QVector<QVariant> row) {
        qint64 bytes = 64 + row.size()*48;
        for (const auto& v : row) bytes += v.userType() == QMetaType::QByteArray ? v.toByteArray().size() : v.toString().size()*2;
        if (result.rows.size() >= limits.maxRows || result.cachedBytes + bytes > limits.maxBytes) { result.truncated = true; return false; }
        result.cachedBytes += bytes; result.rows.push_back(std::move(row)); return true;
    }
};
}
