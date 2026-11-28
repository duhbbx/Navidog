# Navidog 0.53.12

- MySQL 与 MariaDB 的 `CREATE VIEW` 支持 `ALGORITHM`、`DEFINER`、`SQL SECURITY` 的解析和补全。原方言生成保留三个选项，其他方言需要显式映射算法与权限。
- 在临时 MySQL 9.5 实例验证了视图创建、格式化、`SHOW CREATE VIEW` 重新解析和读出结果，同时回归了例程及触发器的 DEFINER。
- PostgreSQL 的分组集合拆分新增布尔分组键和精确的 `AVG(bigint)`。临时 PostgreSQL 核对了数据库输出的值、列名与类型，覆盖真实 NULL、条件过滤和重复总计。

[MySQL CREATE VIEW](https://dev.mysql.com/doc/refman/8.4/en/create-view.html) · [Calcite 分组集合拆分](https://calcite.apache.org/javadocAggregate/org/apache/calcite/rel/rules/AggregateGroupingSetsToUnionRule.html)
