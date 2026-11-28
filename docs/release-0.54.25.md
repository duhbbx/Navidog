# Navidog 0.54.25

- Added DuckDB, ClickHouse, MongoDB, openGauss, Apache Doris, StarRocks, CockroachDB and YugabyteDB YSQL connection types, version families and connection tests.
- Added database-specific object browsing and context actions for queries, data previews, compatible object definitions and refresh. MongoDB exposes databases, collections and indexes through its native commands.
- Added SQL editor dialect selection, diagnostics and completion for the new SQL engines. The local compiler represents DuckDB and ClickHouse `QUALIFY` and ClickHouse `PREWHERE` in its AST and shared model. MongoDB supports Extended JSON commands, local syntax validation and command and option completion.
- DuckDB, ClickHouse and MongoDB are optional native build targets. Doris and StarRocks use the MySQL wire protocol. CockroachDB and YugabyteDB use libpq. openGauss uses its official JDBC driver and managed Java runtime, both available through Driver Management.

Vendor-specific SQL coverage is incremental. Extended catalog actions focus on inspection and queries. The local optimizer and SQL translator do not rewrite `PREWHERE` or `QUALIFY`.
