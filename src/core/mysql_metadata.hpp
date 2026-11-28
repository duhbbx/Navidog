#pragma once
#include <QString>
#include <QRegularExpression>
namespace navidog::design {
inline QString mysqlMetadataExpression(QString expression,QString& protectedReason){
    // Some MySQL versions escape literal delimiters in INFORMATION_SCHEMA.
    // Only complete literals without inner escapes are decoded; unknown quoting
    // stays protected instead of being reinterpreted during a schema change.
    static const QRegularExpression printedLiteral(R"(\\'([^'\\]*)\\')");
    expression.replace(printedLiteral,"'\\1'");
    if(expression.contains('\\'))protectedReason="默认 / 生成 / 检查表达式含复杂转义，请使用完整 SQL 修改此对象。";
    return expression;
}
}
