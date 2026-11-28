# Navidog 0.53.13

- 编辑 `CREATE TABLE` 或 `ALTER TABLE` 的外键引用时，`REFERENCES 表名 (` 内提示被引用表的字段。候选优先显示已知主键字段，并省略已经选用的列。桌面测试覆盖左括号触发。
- PostgreSQL 的普通分组包含输入唯一键时，规划器可以保留较小的键。后续投影即使省略其他分组列，也可基于该键去掉冗余的 `DISTINCT`。隔离 PostgreSQL 对照了原查询和改写后的结果。多组分组集合继续保留原来的唯一性判定。

[Calcite 唯一键元数据](https://calcite.apache.org/javadocAggregate/org/apache/calcite/rel/metadata/RelMdUniqueKeys.html)
