# Navidog 0.54.28

- ClickHouse parses numeric and fractional `SAMPLE` clauses on named tables, including `OFFSET` and combinations with `FINAL`. The AST and IR retain exact literal spellings. Local diagnostics identify a zero denominator or nonzero offset with a zero or full sample.
- PostgreSQL adds `TABLESAMPLE SYSTEM` for constant percentages and optional integer `REPEATABLE` seeds. The compiler and local plan output preserve the choice of block or row sampling.
- Doris and StarRocks parse table-level `ORDER BY` and validate sort columns, key columns and distribution. Doris validates the key prefix and the `UNIQUE KEY` requirement for sort columns. Random distribution is checked against the table key model.
- Contextual completion suggests sampling clauses, CockroachDB historical-read clauses, Doris and StarRocks table options and declared layout columns, and YugabyteDB tablet-split keywords.
