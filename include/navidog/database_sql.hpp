#pragma once

#include "navidog/database_types.hpp"
#include "navidog/sql.hpp"
#include "navidog/sql/compiler/compiler.hpp"

namespace navidog {
inline sql::Dialect scriptDialect(DatabaseKind kind) {
    if (isMySqlFamily(kind) || isAnalyticalMySql(kind) || kind == DatabaseKind::ClickHouse)
        return sql::Dialect::MySQL;
    if (usesPostgresCompatibleSql(kind) || kind == DatabaseKind::H2 || kind == DatabaseKind::DuckDb)
        return sql::Dialect::PostgreSQL;
    if (usesOracleSql(kind) || kind == DatabaseKind::Dameng || kind == DatabaseKind::Db2)
        return sql::Dialect::Oracle;
    if (kind == DatabaseKind::SQLServer)
        return sql::Dialect::SQLServer;
    return sql::Dialect::SQLite;
}
inline sql::compiler::SqlExtension compilerExtensionFor(DatabaseKind kind) {
    using E = sql::compiler::SqlExtension;
    if (kind == DatabaseKind::DuckDb) return E::DuckDb;
    if (kind == DatabaseKind::ClickHouse) return E::ClickHouse;
    if (kind == DatabaseKind::CockroachDb) return E::CockroachDb;
    if (kind == DatabaseKind::Doris) return E::Doris;
    if (kind == DatabaseKind::StarRocks) return E::StarRocks;
    if (kind == DatabaseKind::YugabyteDb) return E::YugabyteDb;
    return E::None;
}
inline std::optional<sql::compiler::Dialect> compilerDialectFor(DatabaseKind kind) {
    using D = sql::compiler::Dialect;
    if (usesPostgresCompatibleSql(kind) || kind == DatabaseKind::H2 || kind == DatabaseKind::Db2 || kind == DatabaseKind::DuckDb)
        return D::PostgreSQL;
    if (kind == DatabaseKind::PolarDbMySql || isAnalyticalMySql(kind) || kind == DatabaseKind::ClickHouse)
        return D::MySQL;
    if (usesOracleSql(kind))
        return D::Oracle;
    return sql::compiler::parseDialect(databaseKindKey(kind).toStdString());
}
}
