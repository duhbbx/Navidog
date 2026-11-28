#include "navidog/source_design.hpp"
#include "navidog/table_design.hpp"
#include "navidog/sql.hpp"
#include <QRegularExpression>
#include <QSet>
#include <algorithm>
#include <stdexcept>
namespace navidog {
namespace {
using K=DatabaseKind;
[[noreturn]] void fail(QString message){throw std::runtime_error(message.toStdString());}
bool mysql(K k){return navidog::isMySqlFamily(k);}
sql::Dialect dialect(K k){return mysql(k)?sql::Dialect::MySQL:navidog::usesNativePostgres(k)?sql::Dialect::PostgreSQL:usesOracleSql(k)?sql::Dialect::Oracle:k==K::SQLServer?sql::Dialect::SQLServer:sql::Dialect::SQLite;}
QString literal(QString text,K k){if(mysql(k))return text.isEmpty()?"''":"CONVERT(X'"+QString::fromLatin1(text.toUtf8().toHex())+"' USING utf8mb4)";if(navidog::usesNativePostgres(k))text.replace("\\","\\\\");text.replace("'","''");return (navidog::usesNativePostgres(k)?"E'":k==K::SQLServer?"N'":"'")+text+"'";}
QueryResult checked(Database& db,QString sql,const Cancellation& cancel,QueryLimits limits={}){if(cancel->load())fail("操作已停止");auto result=db.executeDefinition(sql,limits,cancel);if(result.cancelled)fail("操作已停止");if(!result.error.isEmpty())fail(result.error);return result;}
QString qualified(const ConnectionProfile& p,const CatalogNode& n){return tableQualifiedName(p,n);}
sql::StatementSpan definitionSpan(const QByteArray& bytes,K kind){
    const std::string_view source(bytes.constData(),bytes.size());
    if(mysql(kind)){
        const auto parsed=sql::lex(source,sql::Dialect::MySQL);
        const auto first=std::find_if(parsed.tokens.begin(),parsed.tokens.end(),
            [](const auto& token){return token.kind!=sql::TokenKind::Comment;});
        if(first!=parsed.tokens.end() && first->kind==sql::TokenKind::Word &&
           bytes.mid(first->begin,first->end-first->begin).compare("DELIMITER",Qt::CaseInsensitive)==0){
            const auto statements=sql::splitStatements(source,sql::Dialect::MySQL);
            if(statements.size()!=1)fail("对象编辑器一次只保存一个 CREATE 定义，请移除脚本中的其他 SQL");
            return statements.front();
        }
    }
    return {0,source.size(),false};
}
struct Target {QString kind,name,schema;int begin=0,end=0;};
Target target(const QByteArray& bytes,K kind){auto parsed=sql::lex(bytes.toStdString(),dialect(kind));if(!parsed.diagnostics.empty())fail(QString::fromStdString(parsed.diagnostics.front().message));QVector<sql::Token> tokens;for(auto token:parsed.tokens)if(token.kind!=sql::TokenKind::Comment)tokens<<token;if(tokens.isEmpty())fail("请输入对象定义");auto word=[&](int i){return QString::fromUtf8(bytes.mid(tokens[i].begin,tokens[i].end-tokens[i].begin));};auto first=word(0).toUpper();if(first!="CREATE" && !(kind==K::SQLServer && first=="ALTER"))fail("对象编辑器需要完整的 CREATE 定义");
    int index=-1;Target out;for(int i=1;i<qMin(80,int(tokens.size()));++i){if(tokens[i].kind!=sql::TokenKind::Word)continue;auto w=word(i).toUpper();if(QStringList{"TABLE","INDEX","DATABASE","SCHEMA","TRIGGER"}.contains(w))fail("对象定义类型不匹配");if(w=="FUNCTION" || w=="PROCEDURE" || w=="VIEW"){out.kind=w.toLower();index=i+1;break;}}
    if(index<0 || index>=tokens.size())fail("未找到视图、函数或存储过程名称");if(index+2<tokens.size() && word(index).compare("IF",Qt::CaseInsensitive)==0 && word(index+1).compare("NOT",Qt::CaseInsensitive)==0 && word(index+2).compare("EXISTS",Qt::CaseInsensitive)==0)index+=3;
    auto name=[&](int i){if(i>=tokens.size())fail("对象名称不完整");auto token=tokens[i];auto value=word(i);if(token.kind==sql::TokenKind::Identifier){auto quote=value.left(1);value=value.mid(1,value.size()-2);value.replace(quote=="["?"]]":quote+quote,quote=="["?"]":quote);return value;}if(token.kind!=sql::TokenKind::Word)fail("对象名称无效");return navidog::usesNativePostgres(kind)?value.toLower():usesOracleSql(kind)?value.toUpper():value;};
    out.begin=tokens[index].begin;out.name=name(index);out.end=tokens[index].end;if(index+2<tokens.size() && word(index+1)=="."){out.schema=out.name;out.name=name(index+2);out.end=tokens[index+2].end;if(index+3<tokens.size() && word(index+3)==".")fail("请使用当前数据库中的 Schema / 对象名称");}return out;
}
QString createMode(QString sql,K k,bool replace){auto bytes=sql.toUtf8();auto parsed=sql::lex(bytes.toStdString(),dialect(k));QVector<sql::Token> tokens;for(auto t:parsed.tokens)if(t.kind!=sql::TokenKind::Comment)tokens<<t;if(tokens.size()<2)fail("对象定义不完整");auto word=[&](int i){return QString::fromUtf8(bytes.mid(tokens[i].begin,tokens[i].end-tokens[i].begin)).toUpper();};int end=tokens[0].end;if(word(0)=="ALTER")bytes.replace(tokens[0].begin,tokens[0].end-tokens[0].begin,"CREATE");else if(tokens.size()>2 && word(1)=="OR" && (word(2)=="REPLACE" || word(2)=="ALTER")){end=tokens[2].end;bytes.remove(tokens[0].end,end-tokens[0].end);}
    if(replace){if(k==K::SQLServer)bytes.replace(tokens[0].begin,6,"ALTER");else bytes.insert(tokens[0].begin+6," OR REPLACE");}return QString::fromUtf8(bytes);
}
QStringList pgOids(Database& db,const ConnectionProfile& p,const CatalogNode& n,const Cancellation& cancel){auto result=checked(db,"SELECT p.oid::text FROM pg_proc p JOIN pg_namespace s ON s.oid=p.pronamespace WHERE s.nspname="+literal(n.schema,p.driver)+" AND p.proname="+literal(n.object,p.driver)+" AND p.prokind="+literal(n.kind=="procedure"?"p":"f",p.driver)+" ORDER BY p.oid",cancel);QStringList out;for(auto row:result.rows)out<<row[0].toString();return out;}
QStringList mysqlGrants(Database& db,const ConnectionProfile& p,const CatalogNode& n,const Cancellation& cancel){auto result=checked(db,"SELECT Host,User,Proc_priv FROM mysql.procs_priv WHERE Db="+literal(n.database.isEmpty()?p.database:n.database,p.driver)+" AND Routine_name="+literal(n.object,p.driver)+" AND Routine_type="+literal(n.kind.toUpper(),p.driver),cancel);QStringList statements;for(auto row:result.rows){QStringList privileges;bool grant=false;for(auto value:row[2].toString().split(',')){if(value.compare("Execute",Qt::CaseInsensitive)==0)privileges<<"EXECUTE";else if(value.compare("Alter Routine",Qt::CaseInsensitive)==0)privileges<<"ALTER ROUTINE";else if(value.compare("Grant",Qt::CaseInsensitive)==0)grant=true;else fail("例程含无法完整保留的授权，请使用 SQL 管理");}if(privileges.isEmpty())continue;statements<<"GRANT "+privileges.join(", ")+" ON "+n.kind.toUpper()+" "+qualified(p,n)+" TO "+quoteIdentifier(row[1].toString(),p.driver)+"@"+quoteIdentifier(row[0].toString(),p.driver)+(grant?" WITH GRANT OPTION":"");}return statements;}
}
bool supportsSourceDesign(K kind,const QString& object){if(object!="view" && object!="function" && object!="procedure")return false;return kind!=K::Unknown && kind!=K::Dameng && !usesJdbcCatalog(kind) && (kind!=K::SQLite || object=="view");}
QString sourceTemplate(const ConnectionProfile& p,const CatalogNode& n){auto name=qualified(p,n);if(n.kind=="view")return "CREATE VIEW "+name+" AS\nSELECT 1 AS id;";
    if(navidog::usesNativePostgres(p.driver))return n.kind=="function"?"CREATE FUNCTION "+name+"(p_value integer)\nRETURNS integer\nLANGUAGE plpgsql\nAS $body$\nBEGIN\n    RETURN p_value;\nEND;\n$body$;":"CREATE PROCEDURE "+name+"(IN p_value integer)\nLANGUAGE plpgsql\nAS $body$\nBEGIN\n    RAISE NOTICE 'value: %', p_value;\nEND;\n$body$;";
    if(mysql(p.driver))return n.kind=="function"?"CREATE FUNCTION "+name+"(p_value INT)\nRETURNS INT DETERMINISTIC\nBEGIN\n    RETURN p_value;\nEND":"CREATE PROCEDURE "+name+"(IN p_value INT)\nBEGIN\n    SELECT p_value AS result;\nEND";
    if(usesOracleSql(p.driver))return n.kind=="function"?"CREATE FUNCTION "+name+"(p_value IN NUMBER)\nRETURN NUMBER AS\nBEGIN\n    RETURN p_value;\nEND;":"CREATE PROCEDURE "+name+"(p_value IN NUMBER) AS\nBEGIN\n    NULL;\nEND;";
    return n.kind=="function"?"CREATE FUNCTION "+name+"(@p_value int)\nRETURNS int AS\nBEGIN\n    RETURN @p_value;\nEND":"CREATE PROCEDURE "+name+" @p_value int AS\nBEGIN\n    SELECT @p_value AS result;\nEND";
}
QString rewriteSourceTarget(const QString& sql,const ConnectionProfile& p,const CatalogNode& node){
    auto bytes=sql.toUtf8();
    const auto span=definitionSpan(bytes,p.driver);
    const auto found=target(bytes.mid(span.begin,span.end-span.begin),p.driver);
    bytes.replace(span.begin+found.begin,found.end-found.begin,qualified(p,node).toUtf8());
    return QString::fromUtf8(bytes);
}
QString sourceCallSql(const ConnectionProfile& p,const SourceDefinition& definition){
    auto n=definition.node;auto name=qualified(p,n);QStringList args,setup,outputs,declarations,implicit;int count=0;QString returnType;
    auto comment=[](QString value){return value.replace("*/","* /").replace('\n',' ').replace('\r',' ');};
    auto oracleType=[](QString type){if(type=="VARCHAR2" || type=="VARCHAR" || type=="LONG")return QString("VARCHAR2(32767)");if(type=="NVARCHAR2")return QString("NVARCHAR2(16383)");if(type=="CHAR")return QString("CHAR(32767)");if(type=="NCHAR")return QString("NCHAR(16383)");if(type=="RAW" || type=="LONG RAW")return QString("RAW(32767)");if(type=="REF CURSOR")return QString("SYS_REFCURSOR");if(type=="PL/SQL BOOLEAN")return QString("BOOLEAN");if(type.startsWith("PL/SQL"))fail("目录未提供可声明的完整参数类型，请按定义手动编写调用");return type;};
    bool oracleBlock=n.kind=="procedure",hasOutput=false;
    for(auto parameter:definition.parameters){auto detail=parameter.detail;auto split=detail.indexOf(' ');auto mode=split<0?QString("IN"):detail.left(split);auto type=split<0?detail:detail.mid(split+1);if(parameter.name=="RETURN" || mode=="RETURN"){returnType=type;continue;}++count;auto variable=usesOracleSql(p.driver)?"v_p"+QString::number(count):"@p"+QString::number(count);bool output=mode=="OUT" || mode=="INOUT" || mode=="IN/OUT";hasOutput|=output;
        if(usesOracleSql(p.driver)){type=oracleType(type);declarations<<"    "+variable+" "+type+" := NULL; -- "+comment(parameter.name+" "+detail);args<<variable;if(output){oracleBlock=true;if(type=="SYS_REFCURSOR")implicit<<"    DBMS_SQL.RETURN_RESULT("+variable+");";else outputs<<(type=="BOOLEAN"?"CASE WHEN "+variable+" THEN 'TRUE' WHEN NOT "+variable+" THEN 'FALSE' ELSE NULL END":variable)+" AS "+quoteIdentifier(parameter.name,p.driver);}}
        else if(mysql(p.driver) && n.kind=="procedure"){setup<<"SET "+variable+" = NULL; -- "+comment(parameter.name+" "+detail);args<<variable;if(output)outputs<<variable+" AS "+quoteIdentifier(parameter.name,p.driver);}
        else if(p.driver==K::SQLServer && n.kind=="procedure"){if(!QRegularExpression("^@[\\p{L}_][\\p{L}\\p{N}_@$#]*$").match(parameter.name).hasMatch())fail("参数名称需要手动填写");args<<parameter.name+" = NULL";}
        else if(n.kind=="procedure" || mode!="OUT")args<<(navidog::usesNativePostgres(p.driver) && mode=="VARIADIC"?"VARIADIC ":"")+QString("NULL")+(navidog::usesNativePostgres(p.driver)?"::"+type:QString{})+" /* "+comment(parameter.name+" "+detail)+" */";
    }
    if(usesOracleSql(p.driver)){if(n.kind=="function" && oracleType(returnType)=="SYS_REFCURSOR")oracleBlock=true;if(!oracleBlock){QStringList nulls;for(auto parameter:definition.parameters)if(parameter.name!="RETURN" && !parameter.detail.startsWith("OUT"))nulls<<"NULL /* "+comment(parameter.name+" "+parameter.detail)+" */";return "SELECT "+name+"("+nulls.join(", ")+") AS result FROM dual;";}
        if((!outputs.isEmpty() || !implicit.isEmpty() || (n.kind=="function" && oracleType(returnType)=="SYS_REFCURSOR")) && (p.version==DatabaseVersion::Oracle11 || p.detectedVersion==DatabaseVersion::Oracle11))fail("Oracle 11 不支持隐式结果返回，请按定义手动编写 OUT 参数调用");
        if(n.kind=="function"){auto type=oracleType(returnType);if(type.isEmpty())fail("无法读取函数返回类型，请重新读取定义");declarations<<"    v_result "+type+";";if(type=="SYS_REFCURSOR")implicit<<"    DBMS_SQL.RETURN_RESULT(v_result);";else outputs<<"v_result AS result";}
        QString sql=declarations.isEmpty()?QString{}:"DECLARE\n"+declarations.join('\n');if(!outputs.isEmpty())sql+="\n    v_output SYS_REFCURSOR;";sql+=(sql.isEmpty()?"BEGIN\n    ":"\nBEGIN\n    ")+QString(n.kind=="function"?"v_result := ":"")+name+"("+args.join(", ")+");";
        if(!outputs.isEmpty())sql+="\n    OPEN v_output FOR SELECT "+outputs.join(", ")+" FROM dual;\n    DBMS_SQL.RETURN_RESULT(v_output);";if(!implicit.isEmpty())sql+='\n'+implicit.join('\n');return sql+"\nEND;\n/";
    }
    if(p.driver==K::SQLServer && n.kind=="procedure")return "EXEC "+name+(args.isEmpty()?QString{}:" "+args.join(", "))+";";
    auto sql=setup.join('\n');if(!sql.isEmpty())sql+='\n';sql+=(n.kind=="function"?(navidog::usesNativePostgres(p.driver) && hasOutput?"SELECT * FROM ":"SELECT "):"CALL ")+name+"("+args.join(", ")+");";if(!outputs.isEmpty())sql+="\nSELECT "+outputs.join(", ")+";";return sql;
}
QString sourceDropSql(const ConnectionProfile& p,const CatalogNode& n){auto name=qualified(p,n);if(navidog::usesNativePostgres(p.driver) && n.kind!="view"){if(n.key.isEmpty() || !QRegularExpression("^[0-9]+$").match(n.key).hasMatch())fail("删除 PostgreSQL 例程需要精确 OID / 参数签名，请重新读取");name+="("+n.detail+")";}return "DROP "+n.kind.toUpper()+" "+name;}
SourceDefinition readSourceDefinition(Database& db,const ConnectionProfile& p,const CatalogNode& node,const Cancellation& cancel){
    if(!supportsSourceDesign(p.driver,node.kind))fail("此对象不支持定义编辑");SourceDefinition out;out.node=node;QueryLimits limits;limits.maxRows=2;limits.maxCellBytes=1048576;limits.maxBytes=4*1024*1024;auto query=definitionSql(p,node);
    if(navidog::usesNativePostgres(p.driver) && node.kind!="view"){if(!QRegularExpression("^[0-9]+$").match(node.key).hasMatch())fail("请从对象树重新选择对应的例程签名");}
    auto result=checked(db,query,cancel,limits);if(result.truncated || result.cellTruncated)fail("对象定义超过完整读取上限，未载入不完整内容");if(result.rows.size()!=1)fail("对象不存在或无法唯一定位");int column=0;
    if(mysql(p.driver)){column=-1;for(int i=0;i<result.columns.size();++i){auto name=result.columns[i].toLower();if(name=="create "+node.kind)column=i;if(name=="sql_mode")out.sqlMode=result.rows[0][i].toString();}if(column<0)fail("服务器未返回完整 CREATE 定义");}
    out.sql=result.rows[0][column].toString();if(out.sql.trimmed().isEmpty())fail("无权读取完整对象定义，或对象定义已加密");out.exists=true;
    if(navidog::usesNativePostgres(p.driver) && node.kind=="view"){auto options=checked(db,"SELECT option FROM pg_class c JOIN pg_namespace s ON s.oid=c.relnamespace CROSS JOIN LATERAL unnest(c.reloptions) AS option WHERE s.nspname="+literal(node.schema,p.driver)+" AND c.relname="+literal(node.object,p.driver),cancel);QStringList values;for(auto row:options.rows){auto option=row[0].toString();if(!QRegularExpression("^(security_barrier|security_invoker|check_option)=(true|false|local|cascaded)$").match(option).hasMatch())fail("视图含无法完整保留的选项");values<<option;}auto columns=checked(db,"SELECT a.attname FROM pg_attribute a JOIN pg_class c ON c.oid=a.attrelid JOIN pg_namespace s ON s.oid=c.relnamespace WHERE s.nspname="+literal(node.schema,p.driver)+" AND c.relname="+literal(node.object,p.driver)+" AND a.attnum>0 AND NOT a.attisdropped ORDER BY a.attnum",cancel);QStringList names;for(auto row:columns.rows)names<<quoteIdentifier(row[0].toString(),p.driver);out.sql="CREATE OR REPLACE VIEW "+qualified(p,node)+" ("+names.join(", ")+")"+(values.isEmpty()?QString{}:" WITH ("+values.join(", ")+")")+" AS\n"+out.sql;}
    if(node.kind!="view"){auto n=node;n.kind="parameters";auto parameters=loadCatalog(db,p,n,cancel);if(!parameters.error.isEmpty())fail(parameters.error);if(parameters.truncated)fail("例程参数超过读取上限");out.parameters=parameters.nodes;}return out;
}
SourceDesignResult applySourceChanges(const ConnectionProfile& p,const SourceDefinition& before,const CatalogNode& node,const QString& sql,bool remove,const Cancellation& cancel){
    SourceDesignResult out;out.definition=before;std::unique_ptr<Database> db;bool transaction=false,dropped=false,commitAttempted=false,ddlAttempted=false,created=false;QStringList grants;
    try{if(p.readOnly)fail("只读连接不能修改对象");if(p.http.enabled)fail("HTTP 隧道不支持对象设计会话，请使用直连或 SSH");if(!supportsSourceDesign(p.driver,node.kind))fail("不支持此对象类型");if(remove && !before.exists)fail("对象尚未创建");
        QString statement;if(!remove){if(sql.contains(QChar(0)))fail("对象定义包含空字符");const auto bytes=sql.toUtf8();if(bytes.size()>4*1024*1024)fail("定义超过 4 MB");
            const auto span=definitionSpan(bytes,p.driver);
            const auto definition=bytes.mid(span.begin,span.end-span.begin);
            const auto found=target(definition,p.driver);
            auto schema=mysql(p.driver)?(node.database.isEmpty()?p.database:node.database):node.schema;if(found.kind!=node.kind || found.name!=node.object || (!found.schema.isEmpty() && found.schema!=schema))fail("定义中的对象名称 / 类型与当前对象不一致，请在新建对象时填写对应名称");
            const std::string_view view(definition.constData(),definition.size());
            if(!span.compound && !sql::isCompoundDefinition(view,dialect(p.driver))){std::string error;if(!sql::isSingleStatement(view,dialect(p.driver),error))fail(QString::fromStdString(error));}
            statement=createMode(QString::fromUtf8(definition),p.driver,before.exists && p.driver!=K::SQLite && !(mysql(p.driver) && node.kind!="view"));
        }
        db=connectDatabase(p);if(p.driver==K::SQLite || navidog::usesNativePostgres(p.driver) || p.driver==K::SQLServer){checked(*db,p.driver==K::SQLServer?"BEGIN TRANSACTION":p.driver==K::SQLite?"BEGIN IMMEDIATE":"BEGIN",cancel);transaction=true;}
        if(before.exists){auto fresh=readSourceDefinition(*db,p,before.node,cancel);if(fresh.sql!=before.sql || fresh.sqlMode!=before.sqlMode)fail("对象已被其他会话修改，请重新读取后核对");}
        QStringList priorOids;if(navidog::usesNativePostgres(p.driver) && node.kind!="view")priorOids=pgOids(*db,p,node,cancel);
        QStringList triggers;if(p.driver==K::SQLite && before.exists && !remove){auto list=checked(*db,"SELECT sql FROM sqlite_schema WHERE type='trigger' AND tbl_name="+literal(node.object,p.driver)+" AND sql IS NOT NULL ORDER BY name",cancel);for(auto row:list.rows)triggers<<row[0].toString();}
        if(mysql(p.driver) && before.exists && node.kind!="view" && !remove){try{grants=mysqlGrants(*db,p,node,cancel);}catch(const std::exception& e){fail("无法完整读取例程授权，已取消重建以保留原授权："+QString::fromUtf8(e.what()));}checked(*db,"SET SESSION sql_mode="+literal(before.sqlMode,p.driver),cancel);}
        ddlAttempted=true;
        if(remove || (before.exists && (p.driver==K::SQLite || (mysql(p.driver) && node.kind!="view")))){checked(*db,sourceDropSql(p,before.node),cancel);dropped=true;}
        if(!remove){checked(*db,statement,cancel);created=true;for(auto trigger:triggers)checked(*db,trigger,cancel);for(auto grant:grants)checked(*db,grant,cancel);}
        auto actualNode=node;if(navidog::usesNativePostgres(p.driver) && node.kind!="view" && !remove){auto current=pgOids(*db,p,node,cancel);if(before.exists && current!=priorOids)fail("已有例程的参数签名发生变化，请新建重载；本次修改已回滚");if(before.exists)actualNode=before.node;else{for(auto oid:current)if(!priorOids.contains(oid))actualNode.key=oid;if(actualNode.key.isEmpty())fail("未能唯一确认新建例程");auto r=checked(*db,"SELECT pg_get_function_identity_arguments("+actualNode.key+")",cancel);actualNode.detail=r.rows[0][0].toString();}}
        if(transaction){commitAttempted=true;checked(*db,"COMMIT",cancel);transaction=false;}out.applied=true;out.deleted=remove;out.definition.node=actualNode;out.definition.exists=!remove;
        if(!remove){try{out.definition=readSourceDefinition(*db,p,actualNode,cancel);if(usesOracleSql(p.driver)){auto errors=checked(*db,"SELECT line,position,text FROM all_errors WHERE owner="+literal(node.schema,p.driver)+" AND name="+literal(node.object,p.driver)+" AND type="+literal(node.kind.toUpper(),p.driver)+" ORDER BY sequence",cancel);for(auto row:errors.rows)out.warning+=QString("%1:%2 %3\n").arg(row[0].toString(),row[1].toString(),row[2].toString());}}catch(const std::exception& e){out.refreshError=QString::fromUtf8(e.what());out.definition.sql=statement;}}
    }catch(const std::exception& e){out.error=QString::fromUtf8(e.what());out.cancelled=cancel->load();if(transaction && db){try{checked(*db,"ROLLBACK",cancellation());out.rolledBack=true;}catch(...){db.reset();out.rolledBack=!commitAttempted;}}
        if(dropped && !transaction && !out.rolledBack && before.exists && !remove && mysql(p.driver)){try{if(!db || !db->healthy())db=connectDatabase(p);checked(*db,"SET SESSION sql_mode="+literal(before.sqlMode,p.driver),cancellation());if(created)checked(*db,sourceDropSql(p,node),cancellation());checked(*db,before.sql,cancellation());for(auto grant:grants)checked(*db,grant,cancellation());out.restored=true;}catch(const std::exception& restore){out.warning="恢复原例程失败："+QString::fromUtf8(restore.what());out.partial=true;}}
        else out.partial=!out.rolledBack && (dropped || (ddlAttempted && !transaction && (cancel->load() || (db && !db->healthy()))));
    }return out;
}
}
