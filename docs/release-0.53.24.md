# Navidog 0.53.24

- PostgreSQL 与 SQL Server 的索引编辑补全识别 `INCLUDE` 和 `WHERE`。在 `INCLUDE` 列表中，候选来自目标表，并跳过已作为直接索引键或包含列的字段。
- PostgreSQL 本地规划可以从 `IS NOT DISTINCT FROM` 和整数常量推导唯一键的行数上限，也可以用与 `NULL` 的安全比较确定非空或互斥条件。隔离 PostgreSQL 核对了生成 SQL 的结果和带空值的分支。
- MySQL 编辑器可指出 `VARCHAR` 缺少长度、存储函数缺少 `RETURN`，以及 `RETURN` 的子查询缺少括号。用隔离 MySQL 核对了回归用例预期，包括 CTE 查找同名基表、限定名列赋值、例程 SQL 与客户端分隔符。

[PostgreSQL 比较函数与运算符](https://www.postgresql.org/docs/15/functions-comparison.html) · [MySQL 存储函数](https://dev.mysql.com/doc/refman/8.4/en/create-procedure.html)
