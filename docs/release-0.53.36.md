# Navidog 0.53.36

- PostgreSQL 的 `ANY`、`SOME` 和 `ALL` 支持数组右操作数，保留与查询右操作数各自的语法和作用域。编辑时可补全数组构造器与表列，并检查已经确定的比较类型。
- 本地执行支持有符号整数字面数组和精确整数、定点数比较。空数组、NULL 数组、NULL 元素及多维数组按原生三值逻辑处理。已绑定的动态数组也可生成原生 PostgreSQL 执行 SQL。
- 规划器对已证明的整数数组 `= ANY`、`= SOME` 和 `<> ALL` 推导范围及行数上界。隔离 PostgreSQL 核对了结果、参数语义、NULL 边界和生成 SQL。

[PostgreSQL 行和数组比较](https://www.postgresql.org/docs/15/functions-comparisons.html) · [Calcite Sarg](https://calcite.apache.org/javadocAggregate/org/apache/calcite/util/Sarg.html)
