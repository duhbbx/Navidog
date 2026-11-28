#pragma once

#include "navidog/database_types.hpp"
#include "navidog/sql.hpp"
#include "navidog/sql/compiler/compiler.hpp"

namespace navidog {
inline sql::Dialect scriptDialect(DatabaseKind kind) {
    if (isMySqlFamily(kind))
        return sql::Dialect::MySQL;
    if (usesPostgresCompatibleSql(kind) || kind == DatabaseKind::H2)
        return sql::Dialect::PostgreSQL;
    if (usesOracleSql(kind) || kind == DatabaseKind::Dameng || kind == DatabaseKind::Db2)
        return sql::Dialect::Oracle;
    if (kind == DatabaseKind::SQLServer)
        return sql::Dialect::SQLServer;
    return sql::Dialect::SQLite;
}
inline std::optional<sql::compiler::Dialect> compilerDialectFor(DatabaseKind kind) {
    using D = sql::compiler::Dialect;
    if (usesPostgresCompatibleSql(kind) || kind == DatabaseKind::H2 || kind == DatabaseKind::Db2)
        return D::PostgreSQL;
    if (kind == DatabaseKind::PolarDbMySql)
        return D::MySQL;
    if (usesOracleSql(kind))
        return D::Oracle;
    return sql::compiler::parseDialect(databaseKindKey(kind).toStdString());
}
}
