# Navidog 0.54.27

- DuckDB accepts FROM-first queries and `GROUP BY ALL`. ClickHouse accepts `FINAL`, `GROUP BY ALL` and `LIMIT … BY`, including a separate global limit.
- CockroachDB parses historical reads with `AS OF SYSTEM TIME`. YugabyteDB YSQL parses `SPLIT INTO … TABLETS` on tables.
- Doris and StarRocks table definitions capture native keys, distribution, buckets and properties. Local validation checks that the key and distribution columns exist.
- The SQL editor and command-line compiler apply the selected database's grammar. The editor adds contextual completion for DuckDB FROM-first queries, grouping, ClickHouse final reads and per-group limits.
- AST and IR retain the native clauses. SQL conversion reports an explicit error when the target cannot preserve their behavior.
