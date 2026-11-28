# Navidog 0.53.31

- PostgreSQL 支持 `ARRAY(SELECT …)`、`ARRAY(VALUES …)` 等查询数组表达式，检查查询输出列数并推断数组元素类型。补全在数组括号内提示查询起始关键字，子查询中的表和列使用对应的作用域。
- `UNNEST` 展开直接构造的单列有符号整数查询数组时，可按查询计划估算元素数。无外部关联的查询使用可证明的上下界。受支持的本地执行会保留子查询排序、NULL 元素和每次展开的序号，并限制输出行数和内存。
- 隔离 PostgreSQL 对照了有序整数查询、VALUES、空结果及相关查询的原生结果和规划生成 SQL。常量和支持的查询也核对了本地执行输出。

[PostgreSQL 数组构造器](https://www.postgresql.org/docs/15/sql-expressions.html#SQL-SYNTAX-ARRAY-CONSTRUCTORS) · [Calcite Collect](https://calcite.apache.org/javadocAggregate/org/apache/calcite/rel/core/Collect.html)
