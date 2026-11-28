# Navidog 0.53.57

- 在 `OVER` 后补全当前 SELECT 的命名窗口，也可插入带括号的分区或排序模板。`OVER (...)` 只提供可复制的窗口，检查已有帧、分区和排序冲突，并按查询块隔离候选。
- PostgreSQL 本地执行的 `LAG` 和 `LEAD` 支持逐行求值的整数偏移和绑定参数。负值反转方向，零读取当前行，NULL 偏移返回 NULL。越过分区边界时读取当前行的默认值。
- 本地测试和隔离 PostgreSQL 对照了分区边界、NULL 值、零和负偏移、整数边界及更换参数后的结果。

[PostgreSQL 窗口函数](https://www.postgresql.org/docs/15/functions-window.html) · [PostgreSQL 窗口定义](https://www.postgresql.org/docs/15/sql-expressions.html#SYNTAX-WINDOW-FUNCTIONS)
