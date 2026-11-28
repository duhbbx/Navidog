# Navidog 0.53.27

- PostgreSQL 与 SQLite 的 `INSERT ... ON CONFLICT (...)` 可补全目标表字段。候选使用连接目录或同一脚本的建表定义，并忽略光标两侧已经写出的简单索引字段。`INSERT ... SELECT` 的来源列不会混入候选。
- PostgreSQL 本地规划用不相交的整数范围表示查询条件，可合并范围、求交集和计算字面量 `IN`、`NOT IN` 的取值。完整唯一键上稀疏取值能得到更紧的行数上界，累加时保留 `bigint` 边界精度。最多保留 64 段，超出时使用覆盖全部值的区间。
- 隔离 PostgreSQL 用重复值、`NULL`、正负 `bigint` 极值和互斥范围对照了原始 SQL、生成 SQL 与估计上界。

[PostgreSQL INSERT](https://www.postgresql.org/docs/15/sql-insert.html) · [PostgreSQL IN 与 NOT IN](https://www.postgresql.org/docs/15/functions-comparisons.html) · [Calcite Sarg](https://calcite.apache.org/javadocAggregate/org/apache/calcite/util/Sarg.html)
