# Navidog 0.53.9

- 在 `INSERT` 目标列清单和单表 `UPDATE SET` 的赋值左侧，只提示写入目标的字段，省略已使用的列。能绑定 SQL Server 的更新目标别名。赋值右侧沿用表达式补全。
- `JOIN USING` 在两个连接输入中查找共同列，省略已使用和有歧义的字段。该提示也适用于可解析列结构的 CTE、派生表与链式连接。列补全按完整限定名核对作用域。
- PostgreSQL 的分组集合优化可将整数和布尔条件组成的部分 `HAVING` 谓词放到拆分后的聚合分支中，使用每个分支对应的类型化 NULL 与 `GROUPING` 位掩码。临时数据库对照了原 SQL 与生成 SQL，并覆盖重复总计、空输入、NULL 和默认规则组合。
- PostgreSQL 的 `INTERSECT` 和 `EXCEPT` 可按列位置传播已验证的整数唯一键，`UNION` 可传播所有分支均满足的非空性。允许精确的有符号整数拓宽。隔离实库验证了 ALL 运算的重复行，以及大整数转换为浮点数后可能合并的边界。

[Calcite 分组集合拆分](https://calcite.apache.org/javadocAggregate/org/apache/calcite/rel/rules/AggregateGroupingSetsToUnionRule.html) · [Calcite 唯一键元数据](https://calcite.apache.org/javadocAggregate/org/apache/calcite/rel/metadata/RelMdUniqueKeys.html)
