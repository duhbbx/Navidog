# Navidog 0.53.55

- 八种方言的 `OVER (...)` 按当前语法补全 `PARTITION BY`、`ORDER BY` 和窗口帧边界。`NULLS FIRST/LAST`、`GROUPS` 和 `EXCLUDE` 依据当前方言基线显示。
- 编辑未闭合的窗口表达式时，补全会保留原始列、表别名和嵌套查询作用域。分析用的补括号只存在于内部语句副本。
- 在桌面编辑器键入窗口起始括号可直接触发提示。普通函数调用的括号仍遵循已有输入行为。

[PostgreSQL 窗口语法](https://www.postgresql.org/docs/15/sql-expressions.html#SYNTAX-WINDOW-FUNCTIONS) · [SQL Server OVER](https://learn.microsoft.com/en-us/sql/t-sql/queries/select-over-clause-transact-sql?view=sql-server-ver16) · [SQLite 窗口函数](https://www.sqlite.org/windowfunctions.html)
