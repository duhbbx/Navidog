# Navidog 0.53.28

- PostgreSQL 与 SQLite 的 `ON CONFLICT ... DO UPDATE SET`、MySQL 系的 `ON DUPLICATE KEY UPDATE` 可按目标表补全待赋值字段。候选排除同一赋值列表已经出现的字段，并保留右侧 `excluded` 等表达式的正常补全。
- PostgreSQL 本地规划可将已证明的整数范围经过直接列投影、别名、派生表、排序和分页传给上层过滤。如果内层限制 `id IN (1, 9)`，外层别名上的 `BETWEEN 2 AND 8` 可以证明结果为零行。复合唯一键的各列限制即使出现在不同查询层，也可以合并推导行数上界。
- 隔离 PostgreSQL 对照了分层过滤、分页和表达式列的实际结果。测试同时覆盖集合分支与外连接 NULL 产生的证明边界。

[PostgreSQL INSERT](https://www.postgresql.org/docs/15/sql-insert.html) · [MySQL ON DUPLICATE KEY UPDATE](https://dev.mysql.com/doc/refman/8.4/en/insert-on-duplicate.html) · [Calcite RelMdPredicates](https://calcite.apache.org/javadocAggregate/org/apache/calcite/rel/metadata/RelMdPredicates.html)
