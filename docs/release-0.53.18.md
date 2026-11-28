# Navidog 0.53.18

- MySQL、MariaDB 和 OceanBase 的 MySQL 模式新增例程 `SIGNAL` 与 `RESIGNAL` 的解析、语义诊断、同方言生成和补全。输入时可选择作用域内的 SQLSTATE 条件、消息字段和局部变量。已填写的诊断字段会从候选中移除。
- PostgreSQL 本地解释器可展开单维有符号整数数组常量，规划器按已验证的元素数计算行数。结果保留重复元素、NULL、范围下标和 `WITH ORDINALITY` 的序号语义。
- 临时 MySQL 与 MariaDB 实库对照核对了条件、错误号和消息。临时 PostgreSQL 核对了本地执行结果和规划行数，包括空数组、整数边界和行数限制。

[MySQL SIGNAL](https://dev.mysql.com/doc/refman/8.4/en/signal.html) · [MySQL RESIGNAL](https://dev.mysql.com/doc/refman/8.4/en/resignal.html) · [PostgreSQL 数组格式](https://www.postgresql.org/docs/15/arrays.html#ARRAYS-IO)
