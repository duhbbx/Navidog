#pragma once
#include <QMetaType>
#include <QString>
#include <QVector>
namespace navidog {
enum class DatabaseKind { Unknown, SQLite, MySQL, PostgreSQL, Oracle, OceanBase, SQLServer, MariaDB, TiDB, OceanBaseOracle, Dameng, H2, Kingbase, UxDb, Redis, PolarDbMySql, PolarDbPostgres, GaussDb, Db2, DuckDb, ClickHouse, OpenGauss, MongoDb, Doris, StarRocks, CockroachDb, YugabyteDb };
constexpr bool isMySqlFamily(DatabaseKind kind){return kind==DatabaseKind::MySQL || kind==DatabaseKind::OceanBase || kind==DatabaseKind::MariaDB || kind==DatabaseKind::TiDB || kind==DatabaseKind::PolarDbMySql;}
constexpr bool isAnalyticalMySql(DatabaseKind kind) {
    return kind == DatabaseKind::Doris || kind == DatabaseKind::StarRocks;
}
constexpr bool isDistributedPostgres(DatabaseKind kind) {
    return kind == DatabaseKind::CockroachDb || kind == DatabaseKind::YugabyteDb;
}
constexpr bool isFileDatabase(DatabaseKind kind) {
    return kind == DatabaseKind::SQLite || kind == DatabaseKind::DuckDb;
}
constexpr bool isCommandDatabase(DatabaseKind kind) {
    return kind == DatabaseKind::Redis || kind == DatabaseKind::MongoDb;
}
constexpr bool usesExtendedCatalog(DatabaseKind kind) {
    return isAnalyticalMySql(kind) || isDistributedPostgres(kind) ||
        kind == DatabaseKind::DuckDb || kind == DatabaseKind::ClickHouse;
}
constexpr bool isOceanBase(DatabaseKind kind) {
    return kind == DatabaseKind::OceanBase || kind == DatabaseKind::OceanBaseOracle;
}
constexpr bool usesOracleSql(DatabaseKind kind) {
    return kind == DatabaseKind::Oracle || kind == DatabaseKind::OceanBaseOracle;
}
constexpr bool usesNativePostgres(DatabaseKind kind) {
    return kind == DatabaseKind::PostgreSQL || kind == DatabaseKind::PolarDbPostgres ||
        isDistributedPostgres(kind);
}
constexpr bool usesPostgresCompatibleSql(DatabaseKind kind) {
    return usesNativePostgres(kind) || kind == DatabaseKind::Kingbase ||
           kind == DatabaseKind::UxDb || kind == DatabaseKind::GaussDb || kind == DatabaseKind::OpenGauss;
}
constexpr bool usesJdbcCatalog(DatabaseKind kind) {
    return kind == DatabaseKind::H2 || kind == DatabaseKind::Kingbase ||
           kind == DatabaseKind::UxDb || kind == DatabaseKind::GaussDb || kind == DatabaseKind::Db2 || kind == DatabaseKind::OpenGauss;
}
constexpr bool usesJdbcDriver(DatabaseKind kind) {
    return usesJdbcCatalog(kind) || kind == DatabaseKind::Dameng;
}
constexpr bool usesSchemaAtConnection(DatabaseKind kind) {
    return usesOracleSql(kind) || kind == DatabaseKind::Dameng || usesJdbcCatalog(kind);
}
// Version families are stable identifiers. Exact patch/build stays in serverVersion.
enum class DatabaseVersion {
    Auto, Unknown, SQLite3,
    MySQL5_7, MySQL8_0, MySQL8_4, MySQL9,
    PostgreSQL12, PostgreSQL13, PostgreSQL14, PostgreSQL15, PostgreSQL16, PostgreSQL17, PostgreSQL18,
    Oracle11, Oracle12, Oracle18, Oracle19, Oracle21, Oracle23, Oracle26,
    OceanBase3, OceanBase4,
    SQLServer2008, SQLServer2012, SQLServer2014, SQLServer2016, SQLServer2017, SQLServer2019, SQLServer2022, SQLServer2025,
    MariaDB10_6, MariaDB10_11, MariaDB11_4, MariaDB11_8, MariaDB12,
    TiDB6, TiDB7, TiDB8, TiDB9, OceanBase5, Dameng8, H2_2, Kingbase8, Kingbase9, UxDb2, Redis6, Redis7, Redis8,
    PolarMySql5, PolarMySql8, PolarPostgres11, PolarPostgres14, PolarPostgres15, PolarPostgres16, PolarPostgres17,
    GaussDb2, Db2_11, Db2_12, DuckDb1, DuckDb2, ClickHouse25, ClickHouse26, OpenGauss5, OpenGauss6, OpenGauss7,
    MongoDb7, MongoDb8, Doris2, Doris3, Doris4, StarRocks3, StarRocks4, Cockroach24, Cockroach25, Cockroach26, Yugabyte2, YugabyteCalendar
};
enum class CredentialSource { Prompt, LocalDocker, Passwordless, SystemStore, LocalEncrypted };
struct VersionInfo { DatabaseVersion version; DatabaseKind kind; const char* key; const char* name; };
QString databaseKindKey(DatabaseKind);
QString databaseKindName(DatabaseKind);
DatabaseKind databaseKindFromKey(const QString&);
QString versionKey(DatabaseVersion);
QString versionName(DatabaseVersion);
DatabaseVersion versionFromKey(const QString&);
DatabaseVersion detectVersion(DatabaseKind,const QString& raw);
QVector<VersionInfo> versionsFor(DatabaseKind);
bool versionBelongsTo(DatabaseVersion,DatabaseKind);
}
Q_DECLARE_METATYPE(navidog::DatabaseKind)
Q_DECLARE_METATYPE(navidog::DatabaseVersion)
