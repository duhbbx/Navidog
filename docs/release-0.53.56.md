# Navidog 0.53.56

- PostgreSQL 和 SQLite 的本地解释器支持整数窗口 `MIN`、`MAX`。分区内使用区间树查询窗口，支持滑动帧、`FILTER` 和排除项，空帧返回 NULL。
- 窗口成本估算计入区间查询的开销。三千行滑动窗口通过了工作量预算测试。隔离 PostgreSQL 对照了 `ROWS`、`GROUPS` 和 `RANGE` 的结果、bigint 边界、空值及生成 SQL 的返回类型。
- SQL Server 的 `RANGE` 数字偏移给出语法报错，补全也按该限制提供候选。

[PostgreSQL 窗口语法](https://www.postgresql.org/docs/15/sql-expressions.html#SYNTAX-WINDOW-FUNCTIONS) · [SQLite 窗口函数](https://www.sqlite.org/windowfunctions.html) · [SQL Server OVER](https://learn.microsoft.com/en-us/sql/t-sql/queries/select-over-clause-transact-sql?view=sql-server-ver16)
