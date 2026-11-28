#include "navidog/namespace_design.hpp"
#include "navidog/database.hpp"
#include "navidog/object_management.hpp"
#include <QFile>
#include <QFileInfo>
#include <QRegularExpression>
#include <sqlite3.h>

namespace navidog {
namespace {
void require(bool condition,const QString& message) { if(!condition)throw std::runtime_error(message.toStdString()); }
void identifier(const QString& name,DatabaseKind kind) {
    require(!name.trimmed().isEmpty() && !name.contains(QChar(0)),"请填写有效名称。");
    const auto max=navidog::usesNativePostgres(kind)?63:kind==DatabaseKind::SQLServer?128:64;
    require((navidog::usesNativePostgres(kind)?name.toUtf8().size():name.size())<=max,QString("名称过长；此数据库允许最多 %1 %2。").arg(max).arg(navidog::usesNativePostgres(kind)?"字节":"字符"));
}
QString option(const QString& value) {
    require(QRegularExpression("^[A-Za-z0-9_]+$").match(value).hasMatch(),"字符集或排序规则名称无效。");return value;
}
QString oracleUserSql(const NamespaceDraft& d,bool revealPassword) {
    // Use the legacy identifier bound so the same dialog also works with 11g.
    for(const auto& name:{d.name,d.defaultTablespace,d.temporaryTablespace})if(!name.isEmpty()) {
        require(!name.trimmed().isEmpty() && name.toUtf8().size()<=30 && !name.contains(QChar(0)) && !name.contains('"'),"Oracle 用户或表空间名称须为 1–30 字节，不能包含双引号。");
        for(auto c:name)require(!c.isSpace() || c==QChar(' '),"Oracle 名称不能包含换行或控制字符。");
        for(auto c:name)require(c.unicode()>=32 && c.unicode()!=127,"Oracle 名称不能包含控制字符。");
    }
    require(!d.name.isEmpty(),"请填写 Oracle 用户 / Schema 名称。");
    require(!d.name.startsWith("C##",Qt::CaseInsensitive),"此窗口创建本地用户，请连接目标 PDB，并使用不以 C## 开头的名称。");
    require(!d.password.isEmpty() && d.password.toUtf8().size()<=30,"Oracle 密码须为 1–30 字节。");
    for(auto c:d.password)require(c.unicode()>=32 && c.unicode()!=127 && c!=QChar('"') && c!=QChar(0x2028) && c!=QChar(0x2029),"Oracle 密码不能包含双引号、换行或控制字符。");
    require(d.quotaMiB>=0 && d.quotaMiB<=1048576,"配额须为 0–1048576 MiB。");
    require(d.quotaMiB==0 || !d.defaultTablespace.isEmpty(),"设置配额前请指定默认表空间。");
    auto q=[](const QString& name){return quoteIdentifier(name,DatabaseKind::Oracle);};
    QString sql="CREATE USER "+q(d.name)+" IDENTIFIED BY \""+(revealPassword?d.password:QString("<password>"))+"\"";
    if(!d.defaultTablespace.isEmpty())sql+=" DEFAULT TABLESPACE "+q(d.defaultTablespace);
    if(!d.temporaryTablespace.isEmpty())sql+=" TEMPORARY TABLESPACE "+q(d.temporaryTablespace);
    if(d.quotaMiB>0)sql+=" QUOTA "+QString::number(d.quotaMiB)+"M ON "+q(d.defaultTablespace);
    sql+=d.accountLocked?" ACCOUNT LOCK":" ACCOUNT UNLOCK";
    return sql;
}
}
bool supportsNamespace(DatabaseKind driver,NamespaceKind kind) {
    if(kind==NamespaceKind::Schema)return usesNativePostgres(driver) || driver==DatabaseKind::OpenGauss ||
        driver==DatabaseKind::DuckDb || driver==DatabaseKind::SQLServer || driver==DatabaseKind::Oracle;
    return driver==DatabaseKind::SQLite || isMySqlFamily(driver) || isAnalyticalMySql(driver) ||
        usesNativePostgres(driver) || driver==DatabaseKind::ClickHouse || driver==DatabaseKind::SQLServer;
}
bool supportsNamespaceDrop(DatabaseKind driver,NamespaceKind kind) {
    return driver!=DatabaseKind::SQLite && supportsNamespace(driver,kind);
}
QString dropNamespaceSql(const ConnectionProfile& profile,NamespaceKind kind,const QString& name,bool cascade) {
    require(!profile.readOnly,"只读连接不能删除数据库或 Schema。");
    require(supportsNamespaceDrop(profile.driver,kind),"此驱动不支持删除该类型对象。");
    identifier(name,profile.driver);
    if(cascade) {
        require(kind==NamespaceKind::Schema && supportsObjectCascade(profile.driver,"schema"),"此数据库不支持级联删除该类命名空间。");
        const auto system=name.toLower();
        require(!(usesNativePostgres(profile.driver) && (system.startsWith("pg_") || system=="information_schema")) &&
                !(profile.driver==DatabaseKind::Oracle && (system=="sys" || system=="system")),
                "此菜单只操作用户 Schema。");
    }
    if(kind==NamespaceKind::Schema && profile.driver!=DatabaseKind::Oracle)
        require(!profile.database.isEmpty(),"请先选择 Schema 所属数据库。");
    const auto object=kind==NamespaceKind::Database?"DATABASE":
        profile.driver==DatabaseKind::Oracle?"USER":"SCHEMA";
    return QString("DROP %1 %2%3;").arg(object,quoteIdentifier(name,profile.driver),cascade?" CASCADE":"");
}
NamespaceResult dropNamespace(const ConnectionProfile& profile,NamespaceKind kind,const QString& name,const Cancellation& cancel,bool cascade) {
    NamespaceResult result;
    try {
        const auto sql=dropNamespaceSql(profile,kind,name,cascade);
        if(cancel->load()){result.cancelled=true;return result;}
        auto administration=profile;
        if(kind==NamespaceKind::Database) {
            if(profile.driver==DatabaseKind::SQLServer)administration.database="master";
            else if(usesNativePostgres(profile.driver) && (profile.database==name || profile.database.isEmpty())) {
                const auto maintenance = profile.driver==DatabaseKind::CockroachDb ? QString("defaultdb") : QString("postgres");
                administration.database = name==maintenance ?
                    (profile.driver==DatabaseKind::CockroachDb ? QString("postgres") : QString("template1")) : maintenance;
            } else if(isMySqlFamily(profile.driver) || isAnalyticalMySql(profile.driver)) administration.database.clear();
            else if(profile.driver==DatabaseKind::ClickHouse && (profile.database==name || profile.database.isEmpty()))
                administration.database = name=="default" ? "system" : "default";
        }
        auto db=connectDatabase(administration);
        if(cancel->load()){result.cancelled=true;return result;}
        const auto executed=db->execute(sql,{},cancel);
        result.error=executed.error;
        result.cancelled=executed.cancelled;
        result.applied=result.error.isEmpty() && !result.cancelled;
    } catch(const std::exception& e){result.error=QString::fromUtf8(e.what());}
    return result;
}
QString namespaceSql(const ConnectionProfile& p,const NamespaceDraft& d) {
    require(!p.readOnly,"只读连接不能创建数据库或 Schema。");
    require(supportsNamespace(p.driver,d.kind),"此驱动不支持直接创建该类型对象。");
    if(p.driver==DatabaseKind::Oracle) {
        auto sql="-- 密码已隐藏；用户名保持输入的大小写。\n"+oracleUserSql(d,false)+";";
        if(d.grantSession)sql+="\nGRANT CREATE SESSION TO "+quoteIdentifier(d.name,p.driver)+";";
        return sql;
    }
    if(p.driver==DatabaseKind::SQLite) {
        require(!d.filePath.trimmed().isEmpty() && QFileInfo(d.filePath).isAbsolute(),"请选择新数据库文件的完整路径。");
        return {}; // File operation, not an SQL statement.
    }
    identifier(d.name,p.driver);if(!d.owner.isEmpty())identifier(d.owner,p.driver);
    auto q=[&](const QString& name){return quoteIdentifier(name,p.driver);};
    if(d.kind==NamespaceKind::Schema) {
        require(!p.database.isEmpty(),"请先选择数据库。");
        if(navidog::usesNativePostgres(p.driver))require(!d.name.startsWith("pg_",Qt::CaseInsensitive),"pg_ 是 PostgreSQL 保留的 Schema 前缀。");
        require(p.driver!=DatabaseKind::DuckDb || d.owner.isEmpty(),"DuckDB Schema 没有所有者选项。");
        return "CREATE SCHEMA "+q(d.name)+(d.owner.isEmpty()?QString{}:" AUTHORIZATION "+q(d.owner))+";";
    }
    QString sql="CREATE DATABASE "+q(d.name);
    if(navidog::usesNativePostgres(p.driver)) {
        if(!d.owner.isEmpty())sql+=" OWNER "+q(d.owner);
    }else if(navidog::isMySqlFamily(p.driver)) {
        require(!d.name.contains('/') && !d.name.contains('\\') && !d.name.contains('.') && !d.name.endsWith(' '),"数据库名称不能包含 /、\\、. 或以空格结尾。");
        if(!d.charset.isEmpty())sql+=" CHARACTER SET "+option(d.charset);
        if(!d.collation.isEmpty())sql+=" COLLATE "+option(d.collation);
    }else if(p.driver==DatabaseKind::SQLServer && !d.collation.isEmpty())sql+=" COLLATE "+option(d.collation);
    return sql+";";
}
NamespaceOptions loadNamespaceOptions(const ConnectionProfile& p,NamespaceKind kind,const Cancellation& cancel) {
    NamespaceOptions options;
    if(p.driver==DatabaseKind::SQLite)return options;
    try {
        require(supportsNamespace(p.driver,kind),"此驱动不支持该操作。");auto db=connectDatabase(p);
        auto read=[&](const QString& sql){QueryLimits limits;limits.maxRows=4096;limits.maxBytes=2*1024*1024;auto r=db->execute(sql,limits,cancel);require(r.error.isEmpty(),r.error);require(!r.cancelled,"选项加载已停止");require(!r.truncated,"选项过多，请直接输入名称");return r;};
        if(navidog::isMySqlFamily(p.driver)) {
            for(const auto& row:read("SHOW CHARACTER SET").rows)options.charsets<<row[0].toString();
            for(const auto& row:read("SHOW COLLATION").rows){options.collations<<row[0].toString();options.collationCharsets<<row[1].toString();}
        }else if(navidog::usesNativePostgres(p.driver) || p.driver==DatabaseKind::OpenGauss) {
            for(const auto& row:read("SELECT rolname FROM pg_roles ORDER BY rolname").rows)options.owners<<row[0].toString();
        }else if(p.driver==DatabaseKind::SQLServer) {
            if(kind==NamespaceKind::Schema)for(const auto& row:read("SELECT name FROM sys.database_principals WHERE type IN ('S','U','G','R') ORDER BY name").rows)options.owners<<row[0].toString();
            else for(const auto& row:read("SELECT name FROM sys.fn_helpcollations() ORDER BY name").rows)options.collations<<row[0].toString();
        }else if(p.driver==DatabaseKind::Oracle) {
            QueryResult tablespaces;
            try{tablespaces=read("SELECT tablespace_name,contents FROM dba_tablespaces WHERE contents IN ('PERMANENT','TEMPORARY') ORDER BY tablespace_name");}
            catch(const std::exception& e){auto error=QString::fromUtf8(e.what());if(!error.contains("ORA-00942") && !error.contains("ORA-01031"))throw;tablespaces=read("SELECT tablespace_name,contents FROM user_tablespaces WHERE contents IN ('PERMANENT','TEMPORARY') ORDER BY tablespace_name");}
            for(const auto& row:tablespaces.rows)
                if(row[1].toString()=="TEMPORARY")options.temporaryTablespaces<<row[0].toString();else options.tablespaces<<row[0].toString();
        }
    }catch(const std::exception& e){options.error=QString::fromUtf8(e.what());}
    return options;
}
NamespaceResult createNamespace(const ConnectionProfile& p,const NamespaceDraft& d,const Cancellation& cancel) {
    NamespaceResult result;
    try {
        const auto sql=namespaceSql(p,d);
        if(cancel->load()){result.cancelled=true;return result;}
        if(p.driver==DatabaseKind::SQLite) {
            // Atomically reserve a new file. Never truncate or replace an existing database.
            QFile reservation(d.filePath);require(reservation.open(QIODevice::WriteOnly|QIODevice::NewOnly),"无法创建新文件（文件可能已存在）："+reservation.errorString());reservation.close();
            sqlite3* raw=nullptr;const auto path=QFile::encodeName(d.filePath);const auto opened=sqlite3_open_v2(path.constData(),&raw,SQLITE_OPEN_READWRITE,nullptr);
            QString error;if(opened!=SQLITE_OK)error=raw?QString::fromUtf8(sqlite3_errmsg(raw)):"SQLite 初始化失败";
            if(error.isEmpty() && sqlite3_exec(raw,"PRAGMA user_version=0;",nullptr,nullptr,nullptr)!=SQLITE_OK)error=QString::fromUtf8(sqlite3_errmsg(raw));
            if(raw)sqlite3_close(raw);
            if(!error.isEmpty()){QFile::remove(d.filePath);throw std::runtime_error(error.toStdString());}
            result.applied=true;
        }else {
            auto db=connectDatabase(p);
            if(cancel->load()){result.cancelled=true;return result;}
            if(p.driver==DatabaseKind::Oracle) {
                // Oracle DDL commits implicitly. Once CREATE succeeds, a failed
                // grant is partial success, never silently undone or retried.
                auto executed=db->execute(oracleUserSql(d,true),{},cancel);
                result.error=executed.error;result.cancelled=executed.cancelled;
                if(!result.error.isEmpty() || result.cancelled){result.error.replace(d.password,"[redacted]");return result;}
                result.partial=true;
                if(d.grantSession) {
                    executed=db->execute("GRANT CREATE SESSION TO "+quoteIdentifier(d.name,p.driver),{},cancel);
                    result.error=executed.error;result.cancelled=executed.cancelled;
                    if(!result.error.isEmpty() || result.cancelled) {
                        result.error="用户已创建，但 CREATE SESSION 授权未完成。请刷新用户列表后处理授权，不要重复创建。\n"+result.error;
                        if(!d.password.isEmpty())result.error.replace(d.password,"[redacted]");
                        return result;
                    }
                }
                result.partial=false;result.applied=true;
            }else {
                const auto executed=db->execute(sql,{},cancel);result.error=executed.error;result.cancelled=executed.cancelled;result.applied=result.error.isEmpty() && !result.cancelled;
            }
        }
    }catch(const std::exception& e){result.error=QString::fromUtf8(e.what());if(result.partial)result.error="用户已创建，后续操作未完成。请刷新核对，不要重复创建。\n"+result.error;}
    if(p.driver==DatabaseKind::Oracle && !d.password.isEmpty())result.error.replace(d.password,"[redacted]");
    return result;
}
}
