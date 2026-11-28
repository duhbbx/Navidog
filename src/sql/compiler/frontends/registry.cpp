#include "frontends.hpp"
namespace navidog::sql::compiler {
std::unique_ptr<Parser> createParser(Compilation &c) {
    switch (c.dialect) {
    case Dialect::MySQL:
        return std::make_unique<MySQLParser>(c);
    case Dialect::PostgreSQL:
        return std::make_unique<PostgreSQLParser>(c);
    case Dialect::Oracle:
        return std::make_unique<OracleParser>(c);
    case Dialect::SQLite:
        return std::make_unique<SQLiteParser>(c);
    case Dialect::SQLServer:
        return std::make_unique<SQLServerParser>(c);
    case Dialect::MariaDB:
        return std::make_unique<MariaDBParser>(c);
    case Dialect::OceanBase:
        return std::make_unique<OceanBaseParser>(c);
    case Dialect::TiDB:
        return std::make_unique<TiDBParser>(c);
    }
    throw std::invalid_argument("unregistered SQL dialect");
}
} // namespace navidog::sql::compiler
