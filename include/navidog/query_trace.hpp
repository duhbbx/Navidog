#pragma once

#include <QString>
#include <functional>

namespace navidog {

// In-memory display text. Build it at dispatch; driver parameter values remain
// separate and are not interpolated into the SQL shown in the workspace.
inline QString queryTraceText(const QString& sql) {
    constexpr int maxCharacters = 2048, maxLines = 8;
    int end = qMin(maxCharacters, int(sql.size()));
    int lines = 1;
    for (int i = 0; i < end; ++i) {
        const auto c = sql.at(i);
        const bool paragraph = c == '\n' || c == '\r' ||
            c == QChar::ParagraphSeparator || c == QChar::LineSeparator;
        if (paragraph && ++lines > maxLines) {
            end = i;
            break;
        }
    }
    if (end == sql.size()) return sql;
    if (end && sql.at(end - 1).isHighSurrogate()) --end;
    return sql.left(end) + QStringLiteral(" …〔显示已截断〕");
}

struct QueryStatementTrace {
    int index = 0;
    int begin = 0, end = 0; // UTF-16 offsets in the submitted editor text.
    QString sql;
};

enum class ImpactQueryKind { MatchingRows, TotalRows };
using ImpactQueryTrace = std::function<void(int, ImpactQueryKind, QString)>;
}
