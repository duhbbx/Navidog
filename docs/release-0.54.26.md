# Navidog 0.54.26

- Server-side estimated plan views now cover DuckDB, ClickHouse, openGauss, MongoDB, Doris, StarRocks, CockroachDB and YugabyteDB YSQL. The viewer preserves the original server response. Doris and StarRocks plans are grouped by fragment.
- The navigation tree now shows native index metadata for the new SQL engines, with constraint details on DuckDB, openGauss, CockroachDB and YugabyteDB. DuckDB tables and views expose their stored DDL.
- Added database and schema management actions where the engine supports them. Structural changes update the loaded folders in the navigation tree.
- Added paged data editing for CockroachDB and YugabyteDB YSQL. Writes require a primary key and check for concurrent changes in a transaction.
- MongoDB collection and index menus expose properties and editable command drafts. Write results report affected documents and surface item-level write errors.
