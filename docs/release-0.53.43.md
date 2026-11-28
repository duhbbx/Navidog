# Navidog 0.53.43

- PostgreSQL 联邦查询支持在整数集合筛选后下推 `ORDER BY` 与分页。来源承诺先完成筛选，再读取所需的有序前缀。顺序可以直接写列名，仍需覆盖全部输出值。
- PostgreSQL `k IN ($1, $2, NULL)` 的参数可由已绑定的整数列推导为 `smallint`、`integer` 或 `bigint`。重复执行时，每次都用新绑定生成候选集合，保留 NULL 语义。
- 用隔离实例核对了服务器参数类型、两千余行数据上的单行读取预算、不同绑定及全部为 NULL 的参数。多个独立列的候选集合目前保留普通分页路径。

[PostgreSQL 数组和行比较](https://www.postgresql.org/docs/15/functions-comparisons.html) · [Calcite FilterTableScanRule](https://calcite.apache.org/javadocAggregate/org/apache/calcite/rel/rules/FilterTableScanRule.html)
