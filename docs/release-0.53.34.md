# Navidog 0.53.34

- PostgreSQL 查询支持 `FROM generate_series(…)`、LATERAL 和 `WITH ORDINALITY`。补全可在表引用中提示该函数，在调用后提示序号关键字，并识别输出值与序号的别名。
- 已确认来自 `pg_catalog` 的整数或 bigint 重载支持本地关系规划与执行。常量参数提供准确行数和唯一性证明，动态参数使用保守估算。本地执行检查零步长、空值、方向、整数端点和输出预算。
- 隔离 PostgreSQL 对照了升降序、跨边界步长、标量子查询、侧向连接和序号，以及生成 SQL 的输出和类型。numeric 和 timestamp 重载保留原生 SQL，依数据库执行。

[PostgreSQL 集合返回函数](https://www.postgresql.org/docs/15/functions-srf.html)
