#include "navidog/table_actions.hpp"
#include "navidog/database.hpp"
#include "navidog/sql.hpp"
#include <QFile>
#include <QSaveFile>
#include <QRegularExpression>
#include <QSet>
#include <QJsonArray>
#include <limits>
#include <QProcess>
#include <QProcessEnvironment>
#include <QStandardPaths>
#include <QFileInfo>
#include <functional>
#include <algorithm>
#include <cmath>

namespace navidog {
namespace {
using K=DatabaseKind;using A=TableAction;
[[noreturn]] void fail(const QString& s){throw std::runtime_error(s.toStdString());}
bool mysql(K k){return navidog::isMySqlFamily(k);}
QString literal(const QVariant& value,K k) {
    if(!value.isValid())return "NULL";
    if(value.userType()==QMetaType::Int || value.userType()==QMetaType::LongLong || value.userType()==QMetaType::UInt || value.userType()==QMetaType::ULongLong || value.userType()==QMetaType::Bool)return value.userType()==QMetaType::Bool?(value.toBool()?"1":"0"):value.toString();
    if(value.userType()==QMetaType::Double){if(!std::isfinite(value.toDouble()))fail("暂不支持导出非有限浮点值");return QString::number(value.toDouble(),'g',17);}
    const auto bytes=value.userType()==QMetaType::QByteArray?value.toByteArray():value.toString().toUtf8();
    const bool binary=value.userType()==QMetaType::QByteArray;
    if(mysql(k))return binary?(bytes.isEmpty()?QString("X''"):"0x"+QString::fromLatin1(bytes.toHex())):bytes.isEmpty()?"''":"CONVERT(0x"+QString::fromLatin1(bytes.toHex())+" USING utf8mb4)";
    if(binary) {
        auto hex=QString::fromLatin1(bytes.toHex());
        if(navidog::usesNativePostgres(k))return "decode('"+hex+"','hex')";
        if(k==K::SQLServer)return "0x"+hex;
        if(k==K::Oracle){if(bytes.size()>2000)fail("Oracle 二进制单元格超过当前 SQL 字面量导出上限，请使用专用 LOB 工具。");return "HEXTORAW('"+hex+"')";}
        return "X'"+hex+"'";
    }
    if(bytes.contains('\0')){if(k==K::SQLite)return "CAST(X'"+QString::fromLatin1(bytes.toHex())+"' AS TEXT)";fail("该数据库文本不能包含空字符");}
    if(k==K::Oracle && bytes.size()>3000)fail("Oracle 文本超过当前 SQL 字面量长度上限，请使用专用 LOB 工具。");
    auto text=QString::fromUtf8(bytes);text.replace("'","''");if(navidog::usesNativePostgres(k))text.replace("\\","\\\\");
    return (navidog::usesNativePostgres(k)?"E'":k==K::SQLServer?"N'":"'")+text+"'";
}
void validName(const QString& name,K k) {
    if(name.trimmed().isEmpty() || name.contains(QChar::Null))fail("请填写有效表名");
    if((navidog::usesNativePostgres(k)?name.toUtf8().size():name.size())>(navidog::usesNativePostgres(k)?63:k==K::SQLServer?128:k==K::Oracle?128:64))fail("表名超过该数据库长度限制");
    if(mysql(k) && (name.contains('.') || name.contains('/') || name.contains('\\') || name.endsWith(' ')))fail("表名包含不支持的字符");
}
QueryResult checked(Database& db,const QString& sql,const Cancellation& cancel,QueryLimits limits={}) {
    if(cancel->load())fail("操作已停止");auto r=db.execute(sql,limits,cancel);
    if(r.cancelled)fail("操作已停止");if(!r.error.isEmpty())fail(r.error);
    if(r.truncated || r.cellTruncated)fail("结果超过安全读取上限，操作未使用不完整数据");return r;
}
QString rewriteCreate(const QString& ddl,const QString& newName,K k,bool index=false,const QString& newTable={}) {
    auto bytes=ddl.toUtf8();auto lexed=sql::lex(std::string_view(bytes.constData(),bytes.size()),sql::Dialect::SQLite);
    if(!lexed.diagnostics.empty())fail("原始建表 SQL 无法读取");
    auto tokens=lexed.tokens;
    tokens.erase(std::remove_if(tokens.begin(),tokens.end(),[](auto t){return t.kind==sql::TokenKind::Comment;}),tokens.end());
    auto word=[&](int i){return QString::fromUtf8(bytes.mid(tokens.at(i).begin,tokens.at(i).end-tokens.at(i).begin));};
    if(tokens.size()<4 || word(0).compare("CREATE",Qt::CaseInsensitive))fail("不支持此建表定义");
    int pos=1;if(index && word(pos).compare("UNIQUE",Qt::CaseInsensitive)==0)++pos;
    if(word(pos).compare(index?"INDEX":"TABLE",Qt::CaseInsensitive))fail("虚拟表或特殊表请使用 SQL 定义操作");
    ++pos;if(word(pos).compare("IF",Qt::CaseInsensitive)==0)pos+=3;
    if(pos>=int(tokens.size()))fail("建表定义缺少对象名");
    int end=pos;if(end+2<int(tokens.size()) && word(end+1)==".")end+=2;
    auto begin=tokens[pos].begin,finish=tokens[end].end;
    if(!index)return QString::fromUtf8(bytes.left(begin))+quoteIdentifier(newName,k)+QString::fromUtf8(bytes.mid(finish));
    int on=end+1;if(on+2>=int(tokens.size()) || word(on).compare("ON",Qt::CaseInsensitive))fail("索引定义无法读取");
    ++on;int onEnd=on;if(onEnd+2<int(tokens.size()) && word(onEnd+1)==".")onEnd+=2;
    auto suffix=QString::fromUtf8(bytes.mid(tokens[onEnd].end));
    return QString::fromUtf8(bytes.left(begin))+quoteIdentifier(newName,k)+" ON "+quoteIdentifier(newTable,k)+suffix;
}
QStringList definition(Database& db,const ConnectionProfile& p,const CatalogNode& node,const Cancellation& cancel,const QString& destination={}) {
    auto target=node;if(!destination.isEmpty())target.object=target.name=destination;
    if(p.driver==K::SQLite) {
        const auto schema=quoteIdentifier(node.schema.isEmpty()?"main":node.schema,p.driver);
        auto r=checked(db,"SELECT sql FROM "+schema+".sqlite_schema WHERE type='table' AND name="+literal(node.object,p.driver),cancel);
        if(r.rows.size()!=1)fail("未读取到表定义");QStringList statements;
        statements<<rewriteCreate(r.rows[0][0].toString(),target.object,p.driver);
        r=checked(db,"SELECT name,sql FROM "+schema+".sqlite_schema WHERE type='index' AND tbl_name="+literal(node.object,p.driver)+" AND sql IS NOT NULL ORDER BY name",cancel);
        int count=0;for(auto row:r.rows)statements<<rewriteCreate(row[1].toString(),destination.isEmpty()?row[0].toString():destination+"_idx_"+QString::number(++count),p.driver,true,target.object);
        return statements;
    }
    if(mysql(p.driver)) {
        if(!destination.isEmpty())return {"CREATE TABLE "+tableQualifiedName(p,target)+" LIKE "+tableQualifiedName(p,node)};
        auto r=checked(db,"SHOW CREATE TABLE "+tableQualifiedName(p,node),cancel);if(r.rows.isEmpty() || r.rows[0].size()<2)fail("未读取到表定义");return {r.rows[0][1].toString()};
    }
    if(navidog::usesNativePostgres(p.driver) && !destination.isEmpty())return {"CREATE TABLE "+tableQualifiedName(p,target)+" (LIKE "+tableQualifiedName(p,node)+" INCLUDING ALL)"};
    if(p.driver==K::Oracle && destination.isEmpty()){
        QueryLimits limits;limits.maxCellBytes=1024*1024;
        auto r=checked(db,"SELECT DBMS_METADATA.GET_DDL('TABLE',"+literal(node.object,p.driver)+", "+literal(node.schema,p.driver)+") FROM dual",cancel,limits);if(r.rows.isEmpty())fail("未读取到 Oracle 建表定义");QStringList statements{r.rows[0][0].toString().trimmed()};
        r=checked(db,"SELECT DBMS_METADATA.GET_DDL('INDEX',i.index_name,i.owner) FROM all_indexes i WHERE i.table_owner="+literal(node.schema,p.driver)+" AND i.table_name="+literal(node.object,p.driver)+" AND NOT EXISTS (SELECT 1 FROM all_constraints c WHERE c.index_owner=i.owner AND c.index_name=i.index_name)",cancel,limits);for(auto row:r.rows)statements<<row[0].toString().trimmed();return statements;
    }
    // A field-only reconstruction could silently omit constraints, defaults or
    // indexes. Until a complete serializer exists, do not export such a script.
    fail("此引擎的完整结构脚本尚未接通；可使用设计表和数据 CSV 导出。");
}
QByteArray csvValue(const QVariant& v) {
    if(!v.isValid())return "\\N";
    auto bytes=v.userType()==QMetaType::QByteArray?"base64:"+v.toByteArray().toBase64():v.toString().toUtf8();bytes.replace("\"","\"\"");return '"'+bytes+'"';
}
struct CsvField {QByteArray value;bool quoted=false;};
bool csvRow(QFile& file,QVector<CsvField>& row,const Cancellation& cancel) {
    row.clear();CsvField field;bool inQuotes=false,closed=false,started=false;int bytes=0;
    while(!file.atEnd()) {
        if(++bytes>8*1024*1024)fail("CSV 单行超过 8 MB");if((bytes%1024)==0 && cancel->load())fail("操作已停止");
        char c;if(!file.getChar(&c))fail(file.errorString());started=true;
        if(inQuotes){if(c=='"'){if(file.peek(1)=="\""){file.getChar(&c);field.value+='"';}else {inQuotes=false;closed=true;}}else field.value+=c;}
        else if(c==',' || c=='\r' || c=='\n') {
            row<<field;field={};closed=false;if(row.size()>512)fail("CSV 字段超过 512 列");
            if(c!=','){if(c=='\r' && file.peek(1)=="\n")file.getChar(&c);return true;}
        }else if(c=='"' && field.value.isEmpty() && !closed){inQuotes=true;field.quoted=true;}
        else {if(closed || c=='"')fail("CSV 引号或分隔格式无效");field.value+=c;}
        if(field.value.size()>1024*1024)fail("CSV 单元格超过 1 MB");
    }
    if(inQuotes)fail("CSV 引号未闭合");if(!started)return false;row<<field;return true;
}
QString decode(const QByteArray& bytes){auto text=QString::fromUtf8(bytes);if(text.toUtf8()!=bytes)fail("文件必须为有效 UTF-8 编码");return text;}
TableTaskResult postgresDump(const ConnectionProfile& p,const TableTask& task,const Cancellation& cancel) {
    TableTaskResult out;auto db=connectDatabase(p);auto version=db->serverVersion();db.reset();const auto major=version.section('.',0,0).section(' ',0,0);QString executable=QStandardPaths::findExecutable("pg_dump");
#ifdef Q_OS_MACOS
    {auto candidate=QString("/opt/homebrew/opt/postgresql@%1/bin/pg_dump").arg(major);if(QFileInfo(candidate).isExecutable())executable=candidate;}
#endif
    if(executable.isEmpty())fail("请安装 PostgreSQL 官方客户端并将 pg_dump 加入 PATH。");if(task.path.isEmpty())fail("请选择输出文件");
    QSaveFile file(task.path);if(!file.open(QIODevice::WriteOnly))fail(file.errorString());
    auto escape=[](QString s){if(s.contains(QChar::Null))fail("连接字段包含空字符");s.replace("\\","\\\\");s.replace("'","\\'");return "'"+s+"'";};
    auto connection="host="+escape(p.host)+" port="+QString::number(p.port)+" user="+escape(p.user)+" dbname="+escape(p.database)+" sslmode="+(p.tls?"require":"disable")+" connect_timeout=5";
    QStringList args={"--dbname",connection,"--table",tableQualifiedName(p,task.node),"--no-owner","--no-privileges","--column-inserts","--no-password"};if(!task.withData)args<<"--schema-only";
    QProcess process;auto environment=QProcessEnvironment::systemEnvironment();environment.insert("PGPASSWORD",p.password);environment.insert("PGAPPNAME","Navidog table export");process.setProcessEnvironment(environment);process.start(executable,args);if(!process.waitForStarted(5000))fail(process.errorString());QByteArray errors;
    auto drain=[&]{while(process.bytesAvailable()>0){auto bytes=process.read(64*1024);if(file.write(bytes)!=bytes.size()){process.kill();process.waitForFinished(2000);fail(file.errorString());}}auto error=process.readAllStandardError();errors=(errors+error).right(64*1024);};
    while(process.state()!=QProcess::NotRunning){if(cancel->load()){process.kill();process.waitForFinished(2000);out.cancelled=true;fail("导出已停止，原文件未覆盖。");}process.waitForReadyRead(100);drain();}drain();
    if(process.exitStatus()!=QProcess::NormalExit || process.exitCode()!=0)fail(QString::fromUtf8(errors));if(cancel->load())fail("导出已停止");if(!file.commit())fail(file.errorString());out.applied=true;out.report="已使用 PostgreSQL 官方 pg_dump 保存完整表脚本。";return out;
}
QString pageSql(const ConnectionProfile& p,const CatalogNode& n,qint64 offset) {
    auto sql="SELECT * FROM "+tableQualifiedName(p,n);
    if(p.driver==K::SQLServer)return sql+" ORDER BY (SELECT NULL) OFFSET "+QString::number(offset)+" ROWS FETCH NEXT 250 ROWS ONLY";
    if(p.driver==K::Oracle)return sql+" OFFSET "+QString::number(offset)+" ROWS FETCH NEXT 250 ROWS ONLY";
    return sql+" LIMIT 250 OFFSET "+QString::number(offset);
}
}
QString tableActionName(A a) {
    switch(a){case A::Rename:return "重命名表";case A::Drop:return "删除表";case A::DeleteRows:return "清空表";case A::Truncate:return "截断表";case A::CopyStructure:return "复制表（仅结构）";case A::CopyData:return "复制表（结构和数据）";case A::CountRows:return "获取行的总数";case A::Analyze:return "分析表";case A::Check:return "检查表";case A::Optimize:return "优化表";case A::Repair:return "修复表";case A::ExportCsv:return "导出向导";case A::ExportSql:return "转储 SQL 文件";case A::Dictionary:return "数据字典";case A::ImportCsv:return "导入向导";case A::GenerateData:return "数据生成";case A::GrantPrivileges:return "设置权限";case A::TablePrivileges:return "查看权限";case A::ReverseModel:return "逆向表到模型";case A::BuildBI:return "创建 BI 工作区";}return {};
}
bool tableActionWrites(A a){return a==A::Rename || a==A::Drop || a==A::DeleteRows || a==A::Truncate || a==A::CopyData || a==A::CopyStructure || a==A::ImportCsv || a==A::Analyze || a==A::Optimize || a==A::Repair || a==A::GenerateData || a==A::GrantPrivileges;}
bool supportsTableAction(K k,A a) {
    if(k==K::Unknown)return false;
    if(a==A::GrantPrivileges || a==A::TablePrivileges)return k!=K::SQLite;
    if(a==A::CopyData || a==A::CopyStructure)return k==K::SQLite || navidog::usesNativePostgres(k) || mysql(k);
    if(a==A::ExportSql)return k==K::SQLite || navidog::usesNativePostgres(k) || k==K::Oracle || mysql(k);
    if(a==A::Repair)return mysql(k);
    if(a==A::Check)return k==K::SQLite || mysql(k) || k==K::SQLServer;
    if(a==A::Optimize)return k==K::SQLite || navidog::usesNativePostgres(k) || mysql(k) || k==K::SQLServer;
    return true;
}
QString tableTaskSql(const ConnectionProfile& p,const TableTask& task) {
    if(task.node.kind!="table" || task.node.object.isEmpty())fail("请选择具体的表");
    if(!supportsTableAction(p.driver,task.action))fail("此数据库尚未接通该操作");
    if(tableActionWrites(task.action) && p.readOnly)fail("只读连接不能执行此操作");
    if((p.driver==K::SQLite && task.node.object.startsWith("sqlite_",Qt::CaseInsensitive)) || task.node.schema=="information_schema" || task.node.schema=="pg_catalog" || (navidog::usesNativePostgres(p.driver) && task.node.schema.startsWith("pg_")) || (p.driver==K::Oracle && (task.node.schema=="SYS" || task.node.schema=="SYSTEM")) || task.node.schema=="sys" || (mysql(p.driver) && QStringList{"mysql","sys","information_schema","performance_schema","oceanbase"}.contains(task.node.database,Qt::CaseInsensitive))) {if(tableActionWrites(task.action))fail("不能通过此菜单修改系统表");}
    const auto table=tableQualifiedName(p,task.node);const auto k=p.driver;
    switch(task.action) {
    case A::GrantPrivileges:{
        validName(task.grantee,k);static const QSet<QString> allowed={"SELECT","INSERT","UPDATE","DELETE","REFERENCES"};if(task.privileges.isEmpty())fail("请选择权限");for(auto priv:task.privileges)if(!allowed.contains(priv))fail("权限类型无效");
        auto principal=mysql(k)?quoteIdentifier(task.grantee,k)+"@"+quoteIdentifier(task.granteeHost,k):quoteIdentifier(task.grantee,k);
        return QString(task.revoke?"REVOKE ":"GRANT ")+task.privileges.join(", ")+(navidog::usesNativePostgres(k)?" ON TABLE ":" ON ")+table+(task.revoke?" FROM ":" TO ")+principal;
    }
    case A::TablePrivileges:
        if(navidog::usesNativePostgres(k))return "SELECT grantee, privilege_type, is_grantable FROM information_schema.role_table_grants WHERE table_schema="+literal(task.node.schema,k)+" AND table_name="+literal(task.node.object,k)+" ORDER BY grantee,privilege_type";
        if(mysql(k))return "SELECT grantee, privilege_type, is_grantable FROM information_schema.table_privileges WHERE table_schema="+literal(task.node.database,k)+" AND table_name="+literal(task.node.object,k)+" ORDER BY grantee,privilege_type";
        if(k==K::SQLServer)return "SELECT USER_NAME(grantee_principal_id) AS grantee,permission_name,state_desc FROM sys.database_permissions WHERE class=1 AND major_id=OBJECT_ID("+literal(table,k)+") ORDER BY grantee,permission_name";
        return "SELECT grantee, privilege, grantable FROM all_tab_privs WHERE table_schema="+literal(task.node.schema,k)+" AND table_name="+literal(task.node.object,k)+" ORDER BY grantee,privilege";
    case A::BuildBI:{validName(task.dimension,k);auto q=quoteIdentifier(task.dimension,k);auto sql=QString(k==K::SQLServer?"SELECT TOP (20) ":"SELECT ")+q+" AS category, COUNT(*) AS value FROM "+table+" GROUP BY "+q+" ORDER BY value DESC";return sql+(k==K::Oracle?" FETCH FIRST 20 ROWS ONLY":k==K::SQLServer?"":" LIMIT 20");}
    case A::GenerateData:case A::ReverseModel:return {};
    case A::Rename:{validName(task.targetName,k);auto target=task.node;target.object=task.targetName;if(mysql(k))return "RENAME TABLE "+table+" TO "+tableQualifiedName(p,target);if(k==K::SQLServer)return "EXEC sp_rename "+literal(table,k)+", "+literal(task.targetName,k)+", 'OBJECT'";return "ALTER TABLE "+table+" RENAME TO "+quoteIdentifier(task.targetName,k);}
    case A::Drop:return "DROP TABLE "+table;
    case A::DeleteRows:return "DELETE FROM "+table;
    case A::Truncate:return k==K::SQLite?"DELETE FROM "+table:"TRUNCATE TABLE "+table+(navidog::usesNativePostgres(k)?" RESTART IDENTITY":"");
    case A::CountRows:return QString(k==K::SQLServer?"SELECT COUNT_BIG(*) AS row_count FROM ":"SELECT COUNT(*) AS row_count FROM ")+table;
    case A::Analyze:return mysql(k)?"ANALYZE TABLE "+table:k==K::SQLServer?"UPDATE STATISTICS "+table:k==K::Oracle?"CALL DBMS_STATS.GATHER_TABLE_STATS("+literal(task.node.schema,k)+", "+literal(task.node.object,k)+")":"ANALYZE "+table;
    case A::Check:case A::Repair:{
        static const QSet<QString> options={"","QUICK","FAST","MEDIUM","CHANGED","EXTENDED"};if(!options.contains(task.option))fail("维护选项无效");
        if(k==K::SQLite)return "PRAGMA "+quoteIdentifier(task.node.schema.isEmpty()?"main":task.node.schema,k)+".integrity_check("+literal(task.node.object,k)+")";
        if(k==K::SQLServer)return "DBCC CHECKTABLE ("+literal(table,k)+") WITH NO_INFOMSGS";
        return QString(task.action==A::Check?"CHECK TABLE ":"REPAIR TABLE ")+table+(task.option.isEmpty()?QString{}:" "+task.option);
    }
    case A::Optimize:return mysql(k)?"OPTIMIZE TABLE "+table:navidog::usesNativePostgres(k)?"VACUUM (ANALYZE) "+table:k==K::SQLServer?"ALTER INDEX ALL ON "+table+" REBUILD":"VACUUM";
    case A::CopyData:case A::CopyStructure:validName(task.targetName,k);if(task.targetName==task.node.object)fail("目标表名必须不同于原表");return {}; // Must read the original structure first.
    default:return {};
    }
}
TableTaskResult runTableTask(const ConnectionProfile& p,const TableTask& task,const Cancellation& cancel) {
    TableTaskResult out;std::unique_ptr<Database> db;bool transaction=false,created=false,committing=false,irreversibleStarted=false;CatalogNode destination=task.node;destination.object=destination.name=task.targetName;
    const auto a=task.action;
    try {
        auto sql=tableTaskSql(p,task);if(cancel->load())fail("操作已停止");if(a==A::ExportSql && navidog::usesNativePostgres(p.driver))return postgresDump(p,task,cancel);db=connectDatabase(p);
        auto run=[&](const QString& statement){return checked(*db,statement,cancel);};
        auto begin=[&]{if(p.driver==K::Oracle){transaction=true;return;}run(mysql(p.driver)?"START TRANSACTION":p.driver==K::SQLServer?"BEGIN TRANSACTION":"BEGIN");transaction=true;};
        auto commit=[&]{if(transaction){committing=true;run("COMMIT");transaction=false;committing=false;}};
        if(a==A::ReverseModel) {
            auto schema=readTableDefinition(*db,p,task.node,cancel,false);QJsonArray columns,refs,indexes;
            for(auto c:schema.columns)columns<<QJsonObject{{"name",c.name},{"type",c.type},{"pk",c.primaryKey},{"nullable",c.nullable}};
            QString refSql;
            if(p.driver==K::SQLite)refSql="SELECT [table],[from]||' → '||[to] AS detail FROM pragma_foreign_key_list("+literal(task.node.object,p.driver)+", "+literal(task.node.schema.isEmpty()?"main":task.node.schema,p.driver)+")";
            else if(navidog::usesNativePostgres(p.driver))refSql="SELECT n.nspname||'.'||t.relname,pg_get_constraintdef(c.oid) FROM pg_constraint c JOIN pg_class t ON t.oid=c.confrelid JOIN pg_namespace n ON n.oid=t.relnamespace WHERE c.contype='f' AND c.conrelid="+literal(tableQualifiedName(p,task.node),p.driver)+"::regclass";
            else if(mysql(p.driver))refSql="SELECT CONCAT(referenced_table_schema,'.',referenced_table_name),CONCAT(column_name,' → ',referenced_column_name) FROM information_schema.key_column_usage WHERE table_schema="+literal(task.node.database,p.driver)+" AND table_name="+literal(task.node.object,p.driver)+" AND referenced_table_name IS NOT NULL";
            else if(p.driver==K::SQLServer)refSql="SELECT OBJECT_SCHEMA_NAME(referenced_object_id)+'.'+OBJECT_NAME(referenced_object_id),name FROM sys.foreign_keys WHERE parent_object_id=OBJECT_ID("+literal(tableQualifiedName(p,task.node),p.driver)+")";
            else refSql="SELECT r.owner||'.'||r.table_name,f.constraint_name FROM all_constraints f JOIN all_constraints r ON r.owner=f.r_owner AND r.constraint_name=f.r_constraint_name WHERE f.constraint_type='R' AND f.owner="+literal(task.node.schema,p.driver)+" AND f.table_name="+literal(task.node.object,p.driver);
            for(auto row:run(refSql).rows)refs<<QJsonObject{{"table",row[0].toString()},{"detail",row[1].toString()}};
            auto folder=task.node;folder.kind="indexes";auto catalog=loadCatalog(*db,p,folder,cancel);if(!catalog.error.isEmpty() || catalog.truncated)fail("索引元数据未完整读取："+catalog.error);for(auto n:catalog.nodes)indexes<<n.name;
            out.document={{"table",tableQualifiedName(p,task.node)},{"columns",columns},{"references",refs},{"indexes",indexes}};out.applied=true;out.report="已读取表结构、索引及外键关系。";
        }else if(a==A::GenerateData) {
            if(task.maxRows<1 || task.maxRows>1000000)fail("生成行数无效");auto schema=readTableDefinition(*db,p,task.node,cancel,false);
            QSet<QString> known;for(auto c:schema.columns)known.insert(c.name);for(auto it=task.generationRules.begin();it!=task.generationRules.end();++it)if(!known.contains(it.key()))fail("生成规则字段不存在");
            if(mysql(p.driver)){auto engine=run("SELECT engine FROM information_schema.tables WHERE table_schema="+literal(task.node.database,p.driver)+" AND table_name="+literal(task.node.object,p.driver));if(engine.rows.isEmpty() || engine.rows[0][0].toString().compare("InnoDB",Qt::CaseInsensitive))fail("整批数据生成目前要求 InnoDB 事务表");}
            begin();
            for(int row=0;row<task.maxRows;++row){QStringList names,values;
                for(auto c:schema.columns){auto rule=task.generationRules.value(c.name).toObject();auto kind=rule.value("kind").toString("default");if(kind=="default")continue;if(!c.generatedExpression.isEmpty() || c.protectedReason.contains("生成") || c.protectedReason.contains("标识"))fail("生成 / 标识字段须使用默认值："+c.name);names<<quoteIdentifier(c.name,p.driver);QVariant value;auto text=rule.value("value").toString();
                    if(kind=="sequence"){bool ok=false;auto start=text.toLongLong(&ok);if(!ok || start>std::numeric_limits<qlonglong>::max()-row)fail("递增起始值无效或溢出");value=QVariant::fromValue<qlonglong>(start+row);}
                    else if(kind=="series")value=text+QString::number(row+1);else if(kind=="constant")value=text;else if(kind!="null")fail("生成规则无效");values<<literal(value,p.driver);
                }
                QString insert="INSERT INTO "+tableQualifiedName(p,task.node);
                if(names.isEmpty()){if(mysql(p.driver))insert+=" () VALUES ()";else if(p.driver==K::Oracle)fail("Oracle 请选择至少一个可写字段的生成规则");else insert+=" DEFAULT VALUES";}
                else insert+=" ("+names.join(", ")+") VALUES ("+values.join(", ")+")";
                run(insert);++out.rows;
            }
            commit();out.applied=true;out.report=QString("已生成 %1 行；全部写入已提交。").arg(out.rows);
        }else if(a==A::CopyStructure || a==A::CopyData) {
            if(p.driver==K::SQLite || navidog::usesNativePostgres(p.driver))begin();
            const auto scripts=definition(*db,p,task.node,cancel,task.targetName);for(const auto& s:scripts){if(!transaction)irreversibleStarted=true;run(s);created=true;}
            if(a==A::CopyData) {
                auto schema=readTableDefinition(*db,p,task.node,cancel,false);QStringList columns;
                if(navidog::usesNativePostgres(p.driver)) {
                    auto r=run("SELECT column_name FROM information_schema.columns WHERE table_schema="+literal(task.node.schema,p.driver)+" AND table_name="+literal(task.node.object,p.driver)+" AND is_generated='NEVER' ORDER BY ordinal_position");for(auto row:r.rows)columns<<quoteIdentifier(row[0].toString(),p.driver);
                }else if(mysql(p.driver)){
                    auto r=run("SELECT column_name FROM information_schema.columns WHERE table_schema="+literal(task.node.database,p.driver)+" AND table_name="+literal(task.node.object,p.driver)+" AND extra NOT LIKE '%VIRTUAL GENERATED%' AND extra NOT LIKE '%STORED GENERATED%' ORDER BY ordinal_position");for(auto row:r.rows)columns<<quoteIdentifier(row[0].toString(),p.driver);
                }else for(const auto& c:schema.columns)if(c.protectedReason.isEmpty() && c.generatedExpression.isEmpty())columns<<quoteIdentifier(c.name,p.driver);
                if(columns.isEmpty())fail("未读取到可复制字段");
                auto insert="INSERT INTO "+tableQualifiedName(p,destination)+" ("+columns.join(", ")+")"+(navidog::usesNativePostgres(p.driver)?" OVERRIDING SYSTEM VALUE":"")+" SELECT "+columns.join(", ")+" FROM "+tableQualifiedName(p,task.node);out.rows=run(insert).affectedRows;
                if(navidog::usesNativePostgres(p.driver) && out.rows>0) {
                    auto identity=run("SELECT column_name FROM information_schema.columns WHERE table_schema="+literal(task.node.schema,p.driver)+" AND table_name="+literal(task.node.object,p.driver)+" AND is_identity='YES'");
                    for(auto row:identity.rows){auto column=row[0].toString();auto seq="pg_get_serial_sequence("+literal(tableQualifiedName(p,destination),p.driver)+", "+literal(column,p.driver)+")";run("SELECT setval("+seq+", (SELECT CASE WHEN (SELECT seqincrement FROM pg_sequence WHERE seqrelid=("+seq+")::regclass)>0 THEN MAX("+quoteIdentifier(column,p.driver)+") ELSE MIN("+quoteIdentifier(column,p.driver)+") END FROM "+tableQualifiedName(p,destination)+"), true)");}
                }
            }
            commit();out.applied=true;out.report="已复制为 "+task.targetName+QString("，复制 %1 行。").arg(out.rows);
        }else if(a==A::ExportCsv || a==A::ExportSql || a==A::Dictionary) {
            if(task.path.isEmpty())fail("请选择输出文件");
            if(p.driver==K::SQLite){auto source=QFileInfo(p.database).canonicalFilePath();QFileInfo outputInfo(task.path);auto destination=outputInfo.exists()?outputInfo.canonicalFilePath():QFileInfo(outputInfo.absolutePath()).canonicalFilePath()+"/"+outputInfo.fileName();if(destination==source || destination==source+"-wal" || destination==source+"-shm" || destination==source+"-journal")fail("输出文件不能覆盖源数据库或其日志文件。");}
            QSaveFile output(task.path);if(!output.open(QIODevice::WriteOnly))fail(output.errorString());auto write=[&](const QByteArray& bytes){if(cancel->load())fail("操作已停止");if(output.write(bytes)!=bytes.size())fail(output.errorString());};
            if(navidog::usesNativePostgres(p.driver)){run("BEGIN ISOLATION LEVEL REPEATABLE READ READ ONLY");transaction=true;}
            else if(mysql(p.driver)){run("SET TRANSACTION ISOLATION LEVEL REPEATABLE READ");run("START TRANSACTION WITH CONSISTENT SNAPSHOT");transaction=true;}
            else if(p.driver==K::SQLite)begin();
            auto schema=readTableDefinition(*db,p,task.node,cancel,false);
            if(mysql(p.driver))for(auto c:schema.columns)if(QRegularExpression("\\b(tinyblob|mediumblob|longblob|blob|binary|varbinary|bit|geometry|point|linestring|polygon|multipoint|multilinestring|multipolygon|geometrycollection)\\b",QRegularExpression::CaseInsensitiveOption).match(c.type).hasMatch() && a!=A::Dictionary)fail("此驱动的二进制字段导出尚未接通，未写出不完整文件");
            if(a==A::Dictionary) {
                auto escape=[](QString s){s.replace("|","\\|");s.replace('\n'," ");return s;};
                write(("# "+task.node.object+"\n\n| 字段 | 类型 | 允许 NULL | 主键顺序 | 默认值 |\n| --- | --- | --- | --- | --- |\n").toUtf8());
                for(auto c:schema.columns)write(("| "+escape(c.name)+" | "+escape(c.type)+" | "+(c.nullable?"是":"否")+" | "+QString::number(c.primaryKey)+" | "+escape(c.defaultExpression)+" |\n").toUtf8());
            }else {
                if(a==A::ExportSql){write("-- Navidog table export; triggers and grants are not included.\n");for(auto s:definition(*db,p,task.node,cancel))write((s+";\n").toUtf8());}
                qint64 offset=0;bool header=false;
                if(a==A::ExportCsv || task.withData)for(;;) {
                    QueryLimits limits;limits.maxRows=250;limits.maxBytes=16*1024*1024;limits.maxCellBytes=1024*1024;
                    auto page=checked(*db,pageSql(p,task.node,offset),cancel,limits);if(!header && a==A::ExportCsv){QByteArray line;for(auto col:page.columns){if(!line.isEmpty())line+=',';line+=csvValue(col);}write(line+"\r\n");header=true;}
                    QStringList names;for(auto col:page.columns)names<<quoteIdentifier(col,p.driver);
                    for(auto row:page.rows) {
                        QByteArray line;if(a==A::ExportCsv){for(auto v:row){if(!line.isEmpty())line+=',';line+=csvValue(v);}line+="\r\n";}
                        else {QStringList values,insertNames;for(int col=0;col<row.size();++col){auto c=std::find_if(schema.columns.begin(),schema.columns.end(),[&](const auto& c){return c.name==page.columns[col];});if(c!=schema.columns.end() && (!c->generatedExpression.isEmpty() || c->protectedReason.contains("生成")))continue;values<<literal(row[col],p.driver);insertNames<<names[col];}if(values.isEmpty())fail("没有可写入脚本的字段");line=("INSERT INTO "+tableQualifiedName(p,task.node)+" ("+insertNames.join(", ")+") VALUES ("+values.join(", ")+");\n").toUtf8();}
                        write(line);++out.rows;
                    }
                    offset+=page.rows.size();if(page.rows.size()<250)break;
                }
            }
            commit();if(!output.commit())fail(output.errorString());out.report=QString("已保存 %1 行到 %2").arg(out.rows).arg(task.path);out.applied=true;
        }else if(a==A::ImportCsv) {
            QFile file(task.path);if(!file.open(QIODevice::ReadOnly))fail(file.errorString());if(file.peek(3)==QByteArray::fromHex("efbbbf"))file.read(3);QVector<CsvField> row;if(!csvRow(file,row,cancel))fail("CSV 文件为空");
            auto schema=readTableDefinition(*db,p,task.node,cancel,false);QVector<TableColumn> columns;QSet<QString> seen;QStringList names;
            for(auto f:row) {auto name=decode(f.value);if(seen.contains(name))fail("CSV 表头重复");seen.insert(name);auto found=std::find_if(schema.columns.begin(),schema.columns.end(),[&](auto c){return c.name==name;});if(found==schema.columns.end())fail("CSV 字段不在目标表中："+name);columns<<*found;names<<quoteIdentifier(name,p.driver);}
            if(mysql(p.driver)) {auto engine=run("SELECT engine FROM information_schema.tables WHERE table_schema="+literal(task.node.database,p.driver)+" AND table_name="+literal(task.node.object,p.driver));if(engine.rows.isEmpty() || engine.rows[0][0].toString().compare("InnoDB",Qt::CaseInsensitive))fail("CSV 整批导入目前要求 InnoDB 事务表");}
            if(task.maxRows<1 || task.maxRows>1000000)fail("导入行数上限无效");begin();
            while(csvRow(file,row,cancel)) {
                if(out.rows>=task.maxRows)fail("文件超过指定导入行数上限，整批已停止");if(row.size()!=columns.size())fail(QString("第 %1 行字段数量不匹配").arg(out.rows+2));QStringList values;
                for(int i=0;i<row.size();++i) {
                    QVariant value;
                    if(row[i].quoted || row[i].value!="\\N") {
                        const bool blob=QRegularExpression("\\b(tinyblob|mediumblob|longblob|blob|bytea|binary|varbinary|raw|image|geometry|point|linestring|polygon|multipoint|multilinestring|multipolygon|geometrycollection)\\b",QRegularExpression::CaseInsensitiveOption).match(columns[i].type).hasMatch();
                        if(blob){if(!row[i].value.startsWith("base64:"))fail("二进制字段需要 base64: 编码");auto encoded=row[i].value.mid(7);auto decoded=QByteArray::fromBase64(encoded);if(decoded.toBase64()!=encoded)fail("Base64 内容无效");value=decoded;}
                        else value=decode(row[i].value);
                    }
                    values<<literal(value,p.driver);
                }
                run("INSERT INTO "+tableQualifiedName(p,task.node)+" ("+names.join(", ")+")"+(navidog::usesNativePostgres(p.driver)?" OVERRIDING SYSTEM VALUE":"")+" VALUES ("+values.join(", ")+")");++out.rows;
            }
            commit();out.applied=true;out.report=QString("已追加导入 %1 行").arg(out.rows);
        }else {
            if(a==A::DeleteRows){bool transactional=true;if(mysql(p.driver)){auto engine=run("SELECT engine FROM information_schema.tables WHERE table_schema="+literal(task.node.database,p.driver)+" AND table_name="+literal(task.node.object,p.driver));transactional=!engine.rows.isEmpty() && engine.rows[0][0].toString().compare("InnoDB",Qt::CaseInsensitive)==0;}if(transactional)begin();}
            if(a==A::Truncate && (p.driver==K::SQLite || navidog::usesNativePostgres(p.driver)))begin();
            if(a==A::BuildBI){auto schema=readTableDefinition(*db,p,task.node,cancel,false);if(std::none_of(schema.columns.begin(),schema.columns.end(),[&](auto c){return c.name==task.dimension;}))fail("分组字段不存在");}
            if(tableActionWrites(a) && !transaction)irreversibleStarted=true;
            out.result=run(sql);out.rows=out.result.affectedRows;
            if(mysql(p.driver) && (a==A::Check || a==A::Analyze || a==A::Optimize || a==A::Repair)){int type=out.result.columns.indexOf("Msg_type"),message=out.result.columns.indexOf("Msg_text");if(type>=0 && message>=0)for(auto row:out.result.rows)if(row[type].toString().compare("error",Qt::CaseInsensitive)==0)fail(row[message].toString());}
            if(a==A::Truncate && p.driver==K::SQLite){auto exists=run("SELECT count(*) FROM sqlite_schema WHERE name='sqlite_sequence'");if(exists.rows[0][0].toInt())run("DELETE FROM sqlite_sequence WHERE name="+literal(task.node.object,p.driver));}
            commit();out.applied=true;if(a==A::CountRows && !out.result.rows.isEmpty())out.rows=out.result.rows[0][0].toLongLong();out.report=tableActionName(a)+QString("完成，%1 行。").arg(out.rows);if(a==A::Check || a==A::Analyze || a==A::Optimize || a==A::Repair || a==A::GrantPrivileges)out.report=tableActionName(a)+"完成。请查看数据库返回的信息。";if(a==A::Truncate)out.report="已完成截断操作。";if(a==A::BuildBI){out.rows=out.result.rows.size();out.report=QString("已统计 %1 个分组。").arg(out.rows);}
        }
    }catch(const std::exception& e) {
        out.error=QString::fromUtf8(e.what());out.cancelled=cancel->load();out.commitUnknown=committing || (irreversibleStarted && (out.cancelled || QRegularExpression("connection|connect|closed|lost|transport|network|ORA-031|DPI-1080",QRegularExpression::CaseInsensitiveOption).match(out.error).hasMatch()));
        if(transaction && db){try{checked(*db,"ROLLBACK",cancellation());out.rolledBack=!committing;}catch(...){db.reset();out.rolledBack=!committing;}}
        if(created && !out.rolledBack && db){try{checked(*db,"DROP TABLE "+tableQualifiedName(p,destination),cancellation());created=false;}catch(...){}}
        out.partial=(created || (irreversibleStarted && a==A::DeleteRows)) && !out.rolledBack;
    }
    return out;
}
}
