# Navidog 0.53.35

- 编辑 `CREATE TABLE` 的主键、唯一约束和外键本地列时，候选来自当前表已定义的字段。补全可在括号仍未写完时读取前面已写好的字段，并排除当前列表两侧已选的列。
- PostgreSQL 等值内连接传播已证明的非空性和整数取值范围。两侧范围互斥时可以证明没有匹配。外连接对保留侧传播原约束，补 NULL 的一侧保持保守判断。
- 原生 PostgreSQL 对照了普通等值、NULL 安全等值、左外连接与全外连接的结果。建表约束补全通过了 PostgreSQL、MySQL、Oracle、SQL Server 和 SQLite 的代表用例。

[Calcite 连接谓词推导](https://calcite.apache.org/javadocAggregate/org/apache/calcite/rel/rules/JoinPushTransitivePredicatesRule.html) · [PostgreSQL CREATE TABLE](https://www.postgresql.org/docs/15/sql-createtable.html)
