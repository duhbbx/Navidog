# Navidog 0.53.62

- 八种 SQL 方言的 `CASE` 表达式按分支提示 `WHEN`、`THEN`、`ELSE`、`END`。未写完的分支仍能读取当前 SELECT 的字段，例程中的标量 CASE 保留本地变量提示。
- PostgreSQL 本地分析切换 Calcite 扩展语法后，编辑器的补全、实时检查、规划共享同一语法设置。补全与检查使用当前连接的本地快照。
- OceanBase Oracle 模式沿用 Oracle 补全方言。回归覆盖嵌套 CASE、BETWEEN 上限、相邻关键字和启用或关闭扩展时的候选。
