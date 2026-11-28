# Navidog 0.53.44

- PostgreSQL 的 `MODE() WITHIN GROUP (ORDER BY …)` 已接入解析、语义、类型推导、格式化和同方言 SQL 生成。`MODE` 补全会插入排序结构并把光标放到字段位置。
- 本地执行器按组计算 `smallint`、`integer` 和 `bigint` 的众数。它跳过 NULL，频次相同时遵循排序方向。也支持聚合 `FILTER`、空输入及有预算的频次统计。
- 隔离 PostgreSQL 实测覆盖升降序并列、无有效值的分组、过滤、空输入和 bigint 两端。规范化 SQL 的列名、类型和结果与原语句一致，本地整数计算结果也一致。

[PostgreSQL 有序集聚合](https://www.postgresql.org/docs/15/functions-aggregate.html) · [Calcite 聚合语法](https://calcite.apache.org/docs/reference.html#aggregate-functions)
