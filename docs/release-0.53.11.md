# Navidog 0.53.11

- `JOIN ON` 的字段与表别名候选沿用语义绑定器的连接作用域。稍后的连接和独立的逗号连接输入会从当前条件的候选中排除。
- PostgreSQL 的本地关系元数据可从已验证的比较和 `IS NOT NULL` 证明字段非空，并经直接列投影、去重和分组传播。每个 OR 分支都须证明字段非空。分组集合只有在每个分组都包含该键时才保留此证明。
- 隔离 PostgreSQL 对照了原 SQL 与优化后的 SQL，并验证可消除的外层非空条件，以及仍需保留外层条件的 OR 和分组集合。

[Calcite 谓词元数据](https://calcite.apache.org/javadocAggregate/org/apache/calcite/rel/metadata/RelMdPredicates.html)
