# Navidog 0.53.17

- PostgreSQL 数组表值函数新增 `WITH ORDINALITY` 的补全、解析和生成。在字段位置补全序号列的默认名称或当前别名。
- 单整数数组的 PostgreSQL 关系计划保留由 1 开始的 `bigint` 序号。交叉侧向展开可以去相关，并只将原输入字段的安全过滤提前。序号在单次展开中唯一。
- 临时 PostgreSQL 核对了原始查询、重新生成的 SQL 和规划后的 SQL，覆盖重复与空元素、左连接补 NULL、非 1 数组下标和序号筛选。

[PostgreSQL 表值函数](https://www.postgresql.org/docs/current/queries-table-expressions.html#QUERIES-TABLEFUNCTIONS) · [Calcite Uncollect](https://calcite.apache.org/javadocAggregate/org/apache/calcite/rel/core/Uncollect.html)
