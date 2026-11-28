#pragma once
#include "mysql_metadata.hpp"
#include "navidog/table_design.hpp"
#include "navidog/sql.hpp"
#include <QRegularExpression>
#include <QMap>
#include <stdexcept>
#include <algorithm>
namespace navidog::design {
using K=DatabaseKind;
[[noreturn]] inline void fail(const QString& s){throw std::runtime_error(s.toStdString());}
inline bool mysql(K k){return navidog::isMySqlFamily(k);}
inline QString lit(QString s,K k){if(mysql(k))return s.isEmpty()?"''":"CONVERT(0x"+QString::fromLatin1(s.toUtf8().toHex())+" USING utf8mb4)";if(k==K::PostgreSQL)s.replace("\\","\\\\");s.replace("'","''");return (k==K::PostgreSQL?"E'":k==K::SQLServer?"N'":"'")+s+"'";}
inline QString ddlLiteral(QString s,K k){if(mysql(k)){s.replace("'","''");return "'"+s+"'";}return lit(s,k);}
inline sql::Dialect dialect(K k){return mysql(k)?sql::Dialect::MySQL:k==K::PostgreSQL?sql::Dialect::PostgreSQL:k==K::SQLServer?sql::Dialect::SQLServer:k==K::Oracle?sql::Dialect::Oracle:sql::Dialect::SQLite;}
struct Token {QString text;sql::TokenKind kind;int begin,end;};
inline QVector<Token> tokens(const QString& source,K k){auto bytes=source.toUtf8();auto result=sql::lex(std::string_view(bytes.constData(),bytes.size()),dialect(k));if(!result.diagnostics.empty())fail("SQL 表达式未闭合");QVector<Token> out;for(auto t:result.tokens)out<<Token{QString::fromUtf8(bytes.mid(t.begin,t.end-t.begin)),t.kind,int(t.begin),int(t.end)};return out;}
inline QString unquote(QString s){if(s.startsWith('"') || s.startsWith('`') || s.startsWith('[') || s.startsWith('\'')){auto end=s.startsWith('[')?QString("]"):s.left(1);s=s.mid(1,s.size()-2);s.replace(end+end,end);}return s;}
inline QString slice(const QString& s,int start,int end){return QString::fromUtf8(s.toUtf8().mid(start,end-start));}
inline void expression(const QString& value,K k){int depth=0;for(const auto& t:tokens(value,k)){if(t.kind==sql::TokenKind::Comment)fail("表达式不能包含注释");if(t.kind!=sql::TokenKind::Symbol)continue;if(t.text=="(")++depth;if(t.text==")" && --depth<0)fail("表达式括号不匹配");if(t.text==";" || (t.text=="," && depth==0))fail("请填写单个表达式");}if(depth)fail("表达式括号不匹配");}
inline QString renameIdentifiers(QString source,const QMap<QString,QString>& names,K k){auto ts=tokens(source,k);for(int i=ts.size()-1;i>=0;--i){const auto& t=ts[i];if(t.kind!=sql::TokenKind::Word && t.kind!=sql::TokenKind::Identifier)continue;auto name=unquote(t.text);if(!names.contains(name))continue;if(i+1<ts.size() && ts[i+1].text=="(")continue;auto bytes=source.toUtf8();bytes.replace(t.begin,t.end-t.begin,quoteIdentifier(names.value(name),k).toUtf8());source=QString::fromUtf8(bytes);}return source;}
inline QStringList existingColumnOrder(const TableDefinition& before,const TableDefinition& after){QStringList names;for(const auto& old:before.columns){auto current=std::find_if(after.columns.begin(),after.columns.end(),[&](const auto& c){return c.originalName==old.name;});if(current!=after.columns.end())names<<current->name;}for(const auto& c:after.columns)if(c.originalName.isEmpty())names<<c.name;return names;}
inline QStringList columnOrder(const TableDefinition& table){QStringList names;for(const auto& c:table.columns)names<<c.name;return names;}
inline void name(const QString& s){if(s.trimmed().isEmpty() || s.contains(QChar::Null))fail("对象名称不能为空或包含空字符");}
inline QString qualifiedObject(const ConnectionProfile& p,const CatalogNode& node,const QString& object){auto n=node;n.object=object;return tableQualifiedName(p,n);}
inline QueryResult read(Database& db,const QString& query,const Cancellation& cancel){QueryLimits limits;limits.maxRows=10000;limits.maxBytes=16*1024*1024;limits.maxCellBytes=1024*1024;auto r=db.execute(query,limits,cancel);if(r.cancelled)fail("读取已停止");if(!r.error.isEmpty())fail(r.error);if(r.truncated || r.cellTruncated)fail("对象定义超过读取上限，未加载不完整结构");return r;}
}
