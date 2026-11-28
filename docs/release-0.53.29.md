# Navidog 0.53.29

- PostgreSQL 的 `ARRAY[...]` 进入语法树、统一模型、类型推导、补全、格式化和同方言 SQL 生成。显式数组转换可提供元素类型，编辑器能指出裸空数组缺少类型及不兼容的元素类型。
- PostgreSQL 本地规划和执行可展开一维有符号整数字面量数组，保留 `NULL`、重复值、`bigint` 极值和 `WITH ORDINALITY` 序号。已验证的字面量计入硬行数和物理成本，仍受本地行数与内存预算限制。
- 隔离 PostgreSQL 核对了数组展开的结果、类型和生成 SQL，也验证了引用外层字段的数组表达式。嵌套方括号语法与通用数组本地执行仍待扩展。

[PostgreSQL 数组构造器](https://www.postgresql.org/docs/15/sql-expressions.html#SQL-SYNTAX-ARRAY-CONSTRUCTORS) · [PostgreSQL 数组函数](https://www.postgresql.org/docs/15/functions-array.html)
