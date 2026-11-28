# Navidog 0.53.39

- PostgreSQL、MySQL、Oracle、SQL Server 和 SQLite 的 `CHECK` 表达式可以在括号未写完时补全当前表字段。补全沿用建表声明或已绑定的修改表元数据。
- PostgreSQL 联邦扫描可把单个 `IN`、`= ANY` 或 `= SOME` 的整数候选值作为绑定参数交给数据源。过滤会去重、检查列宽，并对空结果和本地三值逻辑保留验证。
- 测试使用超过两千行的隔离 PostgreSQL 表。代表查询在单行读取预算内正确匹配 smallint 和 bigint 的边界值，也对照了重复项、NULL 和空数组条件。

[PostgreSQL 数组比较](https://www.postgresql.org/docs/15/functions-comparisons.html) · [Calcite 可过滤表](https://calcite.apache.org/javadocAggregate/org/apache/calcite/schema/FilterableTable.html)
