# Navidog 0.53.10

- `ORDER BY` 提示当前查询的输出别名。集合查询末尾的排序按集合输出列补全。PostgreSQL、MySQL 系列和 SQLite 的 `GROUP BY` 可提示已绑定直接列的别名。已重名的别名和聚合表达式的分组别名会被过滤。
- 在 `INSERT` 列清单、单表 `UPDATE SET` 和 `JOIN USING` 中，输入指定括号或逗号可触发字段补全。普通表达式和函数参数输入保留原来的触发方式。
- PostgreSQL 对共享一个整数参数的去重聚合生成先去重再聚合的计划。可验证的 `FILTER` 谓词只读取保留的键时，继续在各聚合上单独应用。隔离实库验证了重复行、NULL、空输入与过滤无命中的分组。读取其他字段或使用易变表达式的过滤保留原计划。

[Calcite 去重聚合展开规则](https://calcite.apache.org/javadocAggregate/org/apache/calcite/rel/rules/AggregateExpandDistinctAggregatesRule.html)
