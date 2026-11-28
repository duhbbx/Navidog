# Navidog 0.53.33

- PostgreSQL 查询表达式支持多维下标、数组切片和 JSONB 连续路径。类型检查区分元素、切片和 JSONB 的结果类型。下标、切片上下界可补全作用域中的列，编辑时缺少右括号也能读取后面的 FROM。
- PostgreSQL SQL 生成保留连续下标、省略的切片边界和 JSONB 路径。本地执行支持可验证的有符号整数字面数组取值，包括带非 1 起始下标的字符串数组、空数组、NULL 元素、越界和参数化索引。
- 隔离 PostgreSQL 对照了原始、同方言生成和规划 SQL 的结果与类型，也核对了本地整数元素的取值和越界诊断。

[PostgreSQL 数组下标](https://www.postgresql.org/docs/15/arrays.html#ARRAYS-ACCESSING) · [PostgreSQL JSONB 下标](https://www.postgresql.org/docs/15/datatype-json.html#JSONB-SUBSCRIPTING)
