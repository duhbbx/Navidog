#include "navidog/result_export.hpp"
#include "navidog/catalog.hpp"
#include <QBuffer>
#include <QSaveFile>
#include <QJsonDocument>
#include <QJsonArray>
#include <QJsonObject>
#include <cmath>
#include <stdexcept>
namespace navidog {
namespace {
[[noreturn]] void fail(const QString& message){throw std::runtime_error(message.toStdString());}
QString text(const QVariant& value){return value.userType()==QMetaType::QByteArray?QString::fromLatin1(value.toByteArray().toHex()):value.toString();}
QString literal(const QVariant& value,DatabaseKind kind){if(!value.isValid() || value.isNull())return "NULL";if(value.userType()==QMetaType::QByteArray){auto bytes=value.toByteArray();auto hex=QString::fromLatin1(bytes.toHex());if(kind==DatabaseKind::PostgreSQL)return "decode('"+hex+"','hex')";if(kind==DatabaseKind::SQLServer)return "0x"+hex;if(kind==DatabaseKind::Oracle){if(bytes.size()>2000)fail("Oracle SQL 导出二进制单元格超过 2000 字节，请使用 JSON / CSV 或数据库原生导出");return "HEXTORAW('"+hex+"')";}return "X'"+hex+"'";}
    if(value.userType()==QMetaType::Bool)return kind==DatabaseKind::PostgreSQL?(value.toBool()?"TRUE":"FALSE"):(value.toBool()?"1":"0");if(value.userType()==QMetaType::Int || value.userType()==QMetaType::LongLong || value.userType()==QMetaType::UInt || value.userType()==QMetaType::ULongLong)return value.toString();if(value.userType()==QMetaType::Double){if(!std::isfinite(value.toDouble()))fail("SQL 导出不支持非有限浮点数");return QString::number(value.toDouble(),'g',17);}auto s=value.toString();if(s.contains(QChar(0)))fail("SQL 文本中含空字符，请使用二进制 / JSON 导出");if(navidog::isMySqlFamily(kind))return "CONVERT(X'"+QString::fromLatin1(s.toUtf8().toHex())+"' USING utf8mb4)";if(kind==DatabaseKind::Oracle && s.toUtf8().size()>4000)fail("Oracle SQL 导出文本单元格超过 4000 字节，请使用 JSON / CSV 或数据库原生导出");if(kind==DatabaseKind::PostgreSQL)s.replace("\\","\\\\");s.replace("'","''");return (kind==DatabaseKind::PostgreSQL?"E'":kind==DatabaseKind::SQLServer?"N'":"'")+s+"'";}
void writeResult(QIODevice& target, const QueryResult& result, ResultExportFormat format,
                 DatabaseKind kind, const QString& tableName, const TaskContext& context) {
    if (result.columns.isEmpty()) fail("结果没有可导出的列");
    const auto check = [&] { if (context.cancel->load()) fail("导出已停止"); };
    const auto write = [&](const QByteArray& bytes) {
        check();
        if (target.write(bytes) != bytes.size()) fail(target.errorString());
    };
    const auto csv = [](QString value) {
        value.replace('"', "\"\"");
        return '"' + value + '"';
    };
    QString prefix;
    if (format == ResultExportFormat::Json) {
        write("{\"columns\":" + QJsonDocument(QJsonArray::fromStringList(result.columns)).toJson(QJsonDocument::Compact)
              + ",\n\"rows\":[");
    } else if (format == ResultExportFormat::Csv) {
        QStringList headers;
        for (const auto& name : result.columns) headers << csv(name);
        write((headers.join(',') + "\r\n").toUtf8());
    } else {
        if (tableName.trimmed().isEmpty()) fail("SQL 导出需要目标表名");
        QStringList columns;
        for (const auto& name : result.columns) columns << quoteIdentifier(name, kind);
        write("-- Navidog: cached result only; binary values preserved.\n");
        if (result.truncated || result.cellTruncated)
            write("-- WARNING: result/cells are truncated; this is not a full data backup.\n");
        prefix = "INSERT INTO " + quoteIdentifier(tableName, kind) + " (" + columns.join(", ") + ") VALUES (";
    }
    qint64 done = 0;
    if (context.report) context.report({done, result.rows.size(), TaskUnit::Rows, "正在写入文件"});
    for (const auto& row : result.rows) {
        check();
        if (row.size() != result.columns.size()) fail("结果列数不一致");
        if (format == ResultExportFormat::Json) {
            QJsonArray values;
            for (const auto& value : row) {
                if (!value.isValid() || value.isNull()) values << QJsonValue(QJsonValue::Null);
                else if (value.userType() == QMetaType::QByteArray)
                    values << QJsonObject{{"base64", QString::fromLatin1(value.toByteArray().toBase64())}};
                else if ((value.userType() == QMetaType::LongLong &&
                          (value.toLongLong() > 9007199254740991LL || value.toLongLong() < -9007199254740991LL)) ||
                         value.userType() == QMetaType::ULongLong)
                    values << QJsonObject{{"integer", value.toString()}};
                else values << QJsonValue::fromVariant(value);
            }
            if (done) write(",\n");
            write(QJsonDocument(values).toJson(QJsonDocument::Compact));
        } else {
            QStringList fields;
            for (const auto& value : row)
                fields << (format == ResultExportFormat::Sql ? literal(value, kind) :
                           !value.isValid() || value.isNull() ? QString{} : csv(text(value)));
            if (format == ResultExportFormat::Csv) write((fields.join(',') + "\r\n").toUtf8());
            else write((prefix + fields.join(", ") + ");\n").toUtf8());
        }
        ++done;
        if (context.report && (done % 64 == 0 || done == result.rows.size()))
            context.report({done, result.rows.size(), TaskUnit::Rows, "正在写入文件"});
    }
    if (format == ResultExportFormat::Json) {
        write(QByteArray("],\n\"truncated\":") + (result.truncated ? "true" : "false")
              + ",\n\"cellTruncated\":" + (result.cellTruncated ? "true" : "false") + "}\n");
    }
}
}

QByteArray exportResult(const QueryResult& result, ResultExportFormat format, DatabaseKind kind,
                        const QString& tableName) {
    QByteArray bytes;
    QBuffer buffer(&bytes);
    buffer.open(QIODevice::WriteOnly);
    writeResult(buffer, result, format, kind, tableName, {cancellation(), {}});
    return bytes;
}

TaskResult exportResultFile(const QString& path, const QueryResult& result, ResultExportFormat format,
                            DatabaseKind kind, const QString& tableName, const TaskContext& context) {
    QSaveFile file(path);
    if (!file.open(QIODevice::WriteOnly)) fail(file.errorString());
    writeResult(file, result, format, kind, tableName, context);
    if (context.cancel->load()) fail("导出已停止");
    if (!file.commit()) fail(file.errorString());
    TaskResult outcome;
    outcome.detail = QString("已导出 %1 行").arg(result.rows.size());
    outcome.artifacts << path;
    return outcome;
}
}
