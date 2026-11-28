#include "navidog/result_export.hpp"
#include "navidog/catalog.hpp"
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
}
QByteArray exportResult(const QueryResult& result,ResultExportFormat format,DatabaseKind kind,const QString& tableName){
    if(result.columns.isEmpty())fail("结果没有可导出的列");for(auto row:result.rows)if(row.size()!=result.columns.size())fail("结果列数不一致");
    if(format==ResultExportFormat::Json){QJsonArray columns,rows;for(auto name:result.columns)columns<<name;for(auto row:result.rows){QJsonArray values;for(auto value:row){if(!value.isValid() || value.isNull())values<<QJsonValue(QJsonValue::Null);else if(value.userType()==QMetaType::QByteArray)values<<QJsonObject{{"base64",QString::fromLatin1(value.toByteArray().toBase64())}};else if((value.userType()==QMetaType::LongLong && (value.toLongLong()>9007199254740991LL || value.toLongLong()<-9007199254740991LL)) || value.userType()==QMetaType::ULongLong)values<<QJsonObject{{"integer",value.toString()}};else values<<QJsonValue::fromVariant(value);}rows<<values;}return QJsonDocument(QJsonObject{{"columns",columns},{"rows",rows},{"truncated",result.truncated},{"cellTruncated",result.cellTruncated}}).toJson(QJsonDocument::Indented);}
    QByteArray output;if(format==ResultExportFormat::Csv){auto csv=[](QString s){s.replace('"',"\"\"");return '"'+s+'"';};QStringList headers;for(auto name:result.columns)headers<<csv(name);output+=(headers.join(',')+"\r\n").toUtf8();for(auto row:result.rows){QStringList fields;for(auto value:row)fields<<(!value.isValid() || value.isNull()?QString{}:csv(text(value)));output+=(fields.join(',')+"\r\n").toUtf8();}return output;}
    if(tableName.trimmed().isEmpty())fail("SQL 导出需要目标表名");QStringList columns;for(auto name:result.columns)columns<<quoteIdentifier(name,kind);output="-- Navidog: cached result only; binary values preserved.\n";if(result.truncated || result.cellTruncated)output+="-- WARNING: result/cells are truncated; this is not a full data backup.\n";auto prefix="INSERT INTO "+quoteIdentifier(tableName,kind)+" ("+columns.join(", ")+") VALUES (";for(auto row:result.rows){QStringList values;for(auto value:row)values<<literal(value,kind);output+=(prefix+values.join(", ")+ ");\n").toUtf8();}return output;
}
}
