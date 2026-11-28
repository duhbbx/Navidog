#include "navidog/query_context.hpp"
#include "navidog/catalog.hpp"
namespace navidog {
void applyQuerySchema(Database& db,const ConnectionProfile& profile,const QString& schema,const Cancellation& token){
    if(schema.isEmpty())return;
    if(schema.size()>512 || schema.contains(QChar(0)))throw std::runtime_error("Schema 名称无效");
    if(profile.http.enabled)throw std::runtime_error("HTTP 隧道不保留会话，不能切换默认 Schema；请使用 Schema 限定名称");
    QString sql;
    if(usesNativePostgres(profile.driver)){
        auto checked=db.executeBound("SELECT 1 FROM pg_namespace WHERE nspname=$1 AND has_schema_privilege(oid,'USAGE')",{schema},{},token);
        if(!checked.error.isEmpty())throw std::runtime_error(checked.error.toStdString());
        if(checked.cancelled || token->load())throw std::runtime_error("Schema 切换已停止");
        if(checked.rows.isEmpty())throw std::runtime_error("Schema 不存在或没有 USAGE 权限");
        sql="SET search_path TO "+quoteIdentifier(schema,profile.driver);
    }else if(profile.driver==DatabaseKind::DuckDb) {
        auto name=schema;
        name.replace("'", "''");
        sql="SET schema = '"+name+"'";
    }else if(usesPostgresCompatibleSql(profile.driver))sql="SET search_path TO "+quoteIdentifier(schema,profile.driver);
    else if(usesJdbcCatalog(profile.driver) || profile.driver==DatabaseKind::Dameng)sql="SET SCHEMA "+quoteIdentifier(schema,profile.driver);
    else if(usesOracleSql(profile.driver))sql="ALTER SESSION SET CURRENT_SCHEMA = "+quoteIdentifier(schema,profile.driver);
    else throw std::runtime_error("此数据库不支持独立的会话 Schema 切换");
    auto result=db.execute(sql,{},token);
    if(!result.error.isEmpty())throw std::runtime_error(result.error.toStdString());
    if(result.cancelled || token->load())throw std::runtime_error("Schema 切换已停止");
}
}
