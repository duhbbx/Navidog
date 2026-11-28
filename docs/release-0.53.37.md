# Navidog 0.53.37

- MySQL、MariaDB 和 SQLite 触发器中的 `NEW.`、`OLD.` 使用触发表的字段。INSERT 事件提供 NEW，DELETE 提供 OLD，UPDATE 提供两者。MySQL 系数据库只在 BEFORE 触发器的赋值左侧提供可写的 NEW 字段。
- 复合触发器的补全作用域覆盖内部语句分号、CASE 表达式和 MySQL 客户端分隔符。SQLite 的 WHEN 条件也可使用行字段。
- PostgreSQL 规划器为 `ANY`、`SOME`、`ALL` 的六种标准整数比较推导硬范围。规则按量词使用已验证的端点或离散值，并区分 NULL 数组、空数组和 NULL 元素。隔离 PostgreSQL 对照了行数、比较边界和生成 SQL。

[MySQL 触发器](https://dev.mysql.com/doc/refman/8.0/en/trigger-syntax.html) · [SQLite CREATE TRIGGER](https://www.sqlite.org/lang_createtrigger.html) · [PostgreSQL 数组比较](https://www.postgresql.org/docs/15/functions-comparisons.html)
