#include "navidog/database_types.hpp"
#include <QRegularExpression>
#include <array>
namespace navidog {
namespace {
using K=DatabaseKind; using V=DatabaseVersion;
constexpr VersionInfo versions[]{
    {V::H2_2,K::H2,"h2_2","H2 2.x"},
    {V::Kingbase8,K::Kingbase,"kingbase8","KingbaseES V8"},
    {V::Kingbase9,K::Kingbase,"kingbase9","KingbaseES V9"},
    {V::UxDb2,K::UxDb,"uxdb2","优炫 UXDB 2.x"},
    {V::Redis6,K::Redis,"redis6","Redis 6.x"},
    {V::Redis7,K::Redis,"redis7","Redis 7.x"},
    {V::Redis8,K::Redis,"redis8","Redis 8.x"},
    {V::PolarMySql5,K::PolarDbMySql,"polar-mysql57","PolarDB MySQL 5.7"},
    {V::PolarMySql8,K::PolarDbMySql,"polar-mysql80","PolarDB MySQL 8.0"},
    {V::PolarPostgres11,K::PolarDbPostgres,"polar-postgres11","PolarDB PostgreSQL 11"},
    {V::PolarPostgres14,K::PolarDbPostgres,"polar-postgres14","PolarDB PostgreSQL 14"},
    {V::PolarPostgres15,K::PolarDbPostgres,"polar-postgres15","PolarDB PostgreSQL 15"},
    {V::PolarPostgres16,K::PolarDbPostgres,"polar-postgres16","PolarDB PostgreSQL 16"},
    {V::PolarPostgres17,K::PolarDbPostgres,"polar-postgres17","PolarDB PostgreSQL 17"},
    {V::GaussDb2,K::GaussDb,"gaussdb2","GaussDB 2.x"},
    {V::Db2_11,K::Db2,"db2_11","Db2 11.x"},
    {V::Db2_12,K::Db2,"db2_12","Db2 12.x"},

    {V::Dameng8,K::Dameng,"dm8","达梦 DM8"},
    {V::SQLite3,K::SQLite,"sqlite3","SQLite 3"},
    {V::MySQL5_7,K::MySQL,"mysql57","MySQL 5.7"},{V::MySQL8_0,K::MySQL,"mysql80","MySQL 8.0"},
    {V::MySQL8_4,K::MySQL,"mysql84","MySQL 8.4"},{V::MySQL9,K::MySQL,"mysql9","MySQL 9.x"},
    {V::MariaDB10_6,K::MariaDB,"mariadb106","MariaDB 10.6"},{V::MariaDB10_11,K::MariaDB,"mariadb1011","MariaDB 10.11"},
    {V::MariaDB11_4,K::MariaDB,"mariadb114","MariaDB 11.4"},{V::MariaDB11_8,K::MariaDB,"mariadb118","MariaDB 11.8"},{V::MariaDB12,K::MariaDB,"mariadb12","MariaDB 12.x"},
    {V::PostgreSQL12,K::PostgreSQL,"postgres12","PostgreSQL 12"},{V::PostgreSQL13,K::PostgreSQL,"postgres13","PostgreSQL 13"},
    {V::PostgreSQL14,K::PostgreSQL,"postgres14","PostgreSQL 14"},{V::PostgreSQL15,K::PostgreSQL,"postgres15","PostgreSQL 15"},
    {V::PostgreSQL16,K::PostgreSQL,"postgres16","PostgreSQL 16"},{V::PostgreSQL17,K::PostgreSQL,"postgres17","PostgreSQL 17"},
    {V::PostgreSQL18,K::PostgreSQL,"postgres18","PostgreSQL 18"},
    {V::Oracle11,K::Oracle,"oracle11","Oracle 11"},{V::Oracle12,K::Oracle,"oracle12","Oracle 12"},
    {V::Oracle18,K::Oracle,"oracle18","Oracle 18"},{V::Oracle19,K::Oracle,"oracle19","Oracle 19"},
    {V::Oracle21,K::Oracle,"oracle21","Oracle 21"},{V::Oracle23,K::Oracle,"oracle23","Oracle 23"},{V::Oracle26,K::Oracle,"oracle26","Oracle 26"},
    {V::TiDB6,K::TiDB,"tidb6","TiDB 6.x"},{V::TiDB7,K::TiDB,"tidb7","TiDB 7.x"},
    {V::TiDB8,K::TiDB,"tidb8","TiDB 8.x"},{V::TiDB9,K::TiDB,"tidb9","TiDB 9.x"},
    {V::OceanBase3,K::OceanBase,"oceanbase3","OceanBase 3.x"},{V::OceanBase4,K::OceanBase,"oceanbase4","OceanBase 4.x"},
    {V::OceanBase5,K::OceanBase,"oceanbase5","OceanBase 5.x"},
    {V::SQLServer2008,K::SQLServer,"sqlserver2008","SQL Server 2008"},{V::SQLServer2012,K::SQLServer,"sqlserver2012","SQL Server 2012"},
    {V::SQLServer2014,K::SQLServer,"sqlserver2014","SQL Server 2014"},{V::SQLServer2016,K::SQLServer,"sqlserver2016","SQL Server 2016"},
    {V::SQLServer2017,K::SQLServer,"sqlserver2017","SQL Server 2017"},{V::SQLServer2019,K::SQLServer,"sqlserver2019","SQL Server 2019"},
    {V::SQLServer2022,K::SQLServer,"sqlserver2022","SQL Server 2022"},{V::SQLServer2025,K::SQLServer,"sqlserver2025","SQL Server 2025"}};
constexpr std::array kinds{K::SQLite,K::MySQL,K::PostgreSQL,K::Oracle,K::OceanBase,K::SQLServer,K::MariaDB,K::TiDB,K::OceanBaseOracle,K::Dameng,K::H2,K::Kingbase,K::UxDb,K::Redis,K::PolarDbMySql,K::PolarDbPostgres,K::GaussDb,K::Db2};
}
QString databaseKindKey(K kind) {
    switch(kind) {
    case K::H2:return "h2";
    case K::Kingbase:return "kingbase";
    case K::UxDb:return "uxdb";
    case K::Redis:return "redis";
    case K::PolarDbMySql:return "polardb-mysql";
    case K::PolarDbPostgres:return "polardb-postgres";
    case K::GaussDb:return "gaussdb";
    case K::Db2:return "db2";
    case K::SQLite:return "sqlite"; case K::MySQL:return "mysql"; case K::PostgreSQL:return "postgres";
    case K::Oracle:return "oracle"; case K::OceanBase:return "oceanbase"; case K::OceanBaseOracle:return "oceanbase-oracle"; case K::SQLServer:return "sqlserver";
    case K::Dameng:return "dameng"; case K::MariaDB:return "mariadb";case K::TiDB:return "tidb";case K::Unknown:return "unknown";
    }
    return "unknown";
}
QString databaseKindName(K kind) {
    switch(kind) {
    case K::H2:return "H2";
    case K::Kingbase:return "金仓 KingbaseES";
    case K::UxDb:return "优炫 UXDB";
    case K::Redis:return "Redis";
    case K::PolarDbMySql:return "PolarDB MySQL";
    case K::PolarDbPostgres:return "PolarDB PostgreSQL";
    case K::GaussDb:return "华为 GaussDB";
    case K::Db2:return "IBM Db2";
    case K::SQLite:return "SQLite"; case K::MySQL:return "MySQL"; case K::PostgreSQL:return "PostgreSQL";
    case K::Oracle:return "Oracle"; case K::OceanBase:return "OceanBase"; case K::OceanBaseOracle:return "OceanBase（Oracle 模式）"; case K::SQLServer:return "SQL Server";
    case K::Dameng:return "达梦 DM8"; case K::MariaDB:return "MariaDB";case K::TiDB:return "TiDB";case K::Unknown:return "未知数据库";
    }
    return "未知数据库";
}
K databaseKindFromKey(const QString& key) { for(auto k:kinds) if(databaseKindKey(k)==key) return k; return K::Unknown; }
QString versionKey(V version) { for(const auto& v:versions) if(v.version==version) return v.key; return version==V::Auto ? "auto" : "unknown"; }
QString versionName(V version) { for(const auto& v:versions) if(v.version==version) return v.name; return version==V::Auto ? "自动检测" : "未知版本"; }
V versionFromKey(const QString& key) { for(const auto& v:versions) if(key==v.key) return v.version; return key=="auto" || key.isEmpty() ? V::Auto : V::Unknown; }
QVector<VersionInfo> versionsFor(K kind) {
    QVector<VersionInfo> out;
    const auto family=isOceanBase(kind)?K::OceanBase:kind;
    for(auto v:versions) if(v.kind==family) {
        v.kind=kind;
        out<<v;
    }
    return out;
}
bool versionBelongsTo(V version,K kind) { if(isOceanBase(kind))kind=K::OceanBase; if(version==V::Auto || version==V::Unknown)return true; for(auto v:versions) if(v.version==version) return v.kind==kind; return false; }
V detectVersion(K kind,const QString& raw) {
    if(raw.contains("MariaDB",Qt::CaseInsensitive) && kind!=K::MariaDB)return V::Unknown;
    auto source=raw;
    if(kind==K::TiDB) {
        const auto version=QRegularExpression(R"(TiDB[- ]*v?(\d+)\.(\d+))",QRegularExpression::CaseInsensitiveOption).match(raw);
        if(!version.hasMatch())return V::Unknown;
        source=version.captured(1)+"."+version.captured(2);
    }
    if(kind!=K::TiDB && raw.contains("-TiDB-",Qt::CaseInsensitive))return V::Unknown;
    if(kind==K::MariaDB){auto maria=QRegularExpression("(\\d+)\\.(\\d+)(?:\\.\\d+)?-MariaDB",QRegularExpression::CaseInsensitiveOption).match(raw);if(maria.hasMatch())source=maria.captured(1)+"."+maria.captured(2);}
    if(isOceanBase(kind)) {
        auto match=QRegularExpression("OceanBase.*?[vV]?(\\d+)\\.(\\d+)",QRegularExpression::CaseInsensitiveOption).match(raw);
        if(match.hasMatch()) source=match.captured(1)+"."+match.captured(2);
    }
    auto match=QRegularExpression("(?:^|[^0-9])(\\d+)\\.(\\d+)(?:\\.|$|[^0-9])").match(source);
    if(!match.hasMatch()) return V::Unknown;
    int major=match.captured(1).toInt(), minor=match.captured(2).toInt();
    switch(kind) {
    case K::H2:return major==2 ? V::H2_2 : V::Unknown;
    case K::Kingbase:return major==8 ? V::Kingbase8 : major==9 ? V::Kingbase9 : V::Unknown;
    case K::UxDb:return major==2 ? V::UxDb2 : V::Unknown;
    case K::Redis:return major==6 ? V::Redis6 : major==7 ? V::Redis7 : major==8 ? V::Redis8 : V::Unknown;
    case K::PolarDbMySql:return major==5 && minor==7 ? V::PolarMySql5 : major==8 ? V::PolarMySql8 : V::Unknown;
    case K::PolarDbPostgres:
        for(const auto& version:versionsFor(kind))
            if(QString(version.key)=="polar-postgres"+QString::number(major))return version.version;
        return V::Unknown;
    case K::GaussDb:return major==2 ? V::GaussDb2 : V::Unknown;
    case K::Db2:return major==11 ? V::Db2_11 : major==12 ? V::Db2_12 : V::Unknown;
    case K::Dameng:return major==8 ? V::Dameng8 : V::Unknown;
    case K::SQLite:return major==3 ? V::SQLite3 : V::Unknown;
    case K::MySQL:
        if(major==5 && minor==7)return V::MySQL5_7;
        if(major==8 && minor==0)return V::MySQL8_0;
        if(major==8 && minor==4)return V::MySQL8_4;
        return major==9 ? V::MySQL9 : V::Unknown;
    case K::MariaDB:
        if(major==10 && minor==6)return V::MariaDB10_6;if(major==10 && minor==11)return V::MariaDB10_11;
        if(major==11 && minor==4)return V::MariaDB11_4;if(major==11 && minor==8)return V::MariaDB11_8;
        return major==12?V::MariaDB12:V::Unknown;
    case K::TiDB:
        for(auto v:versionsFor(kind)) if(QString(v.key)=="tidb"+QString::number(major))return v.version;
        break;
    case K::PostgreSQL:
        for(auto v:versionsFor(kind)) if(QString(v.key)=="postgres"+QString::number(major))return v.version;
        break;
    case K::Oracle:
        for(auto v:versionsFor(kind)) if(QString(v.key)=="oracle"+QString::number(major))return v.version;
        break;
    case K::OceanBase: case K::OceanBaseOracle:
        return major==3 ? V::OceanBase3 : major==4 ? V::OceanBase4 : major==5 ? V::OceanBase5 : V::Unknown;
    case K::SQLServer:
        switch(major) { case 10:return V::SQLServer2008;case 11:return V::SQLServer2012;case 12:return V::SQLServer2014;case 13:return V::SQLServer2016;case 14:return V::SQLServer2017;case 15:return V::SQLServer2019;case 16:return V::SQLServer2022;case 17:return V::SQLServer2025;default:break; }
        break;
    case K::Unknown:break;
    }
    return V::Unknown;
}
}
