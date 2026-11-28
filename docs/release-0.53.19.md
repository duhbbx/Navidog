# Navidog 0.53.19

- MySQL、MariaDB 和 OceanBase 的 MySQL 模式支持例程中的 `GET DIAGNOSTICS`。补全区分语句信息、条件信息、目标变量和诊断区域。临时 MySQL 与 MariaDB 核对了 SQLSTATE、消息、错误号及受影响的行数。
- PostgreSQL 本地规划从有符号整数比较和普通 `BETWEEN` 推导闭区间。某个区间缩到一个值时可以固定唯一键的一列，互相冲突的范围可以证明零行。临时 PostgreSQL 对照覆盖反向比较、OR、大整数边界和原 SQL 与改写 SQL 的结果。

[MySQL GET DIAGNOSTICS](https://dev.mysql.com/doc/refman/8.4/en/get-diagnostics.html) · [MariaDB GET DIAGNOSTICS](https://mariadb.com/docs/server/reference/sql-statements/programmatic-compound-statements/programmatic-compound-statements-diagnostics/get-diagnostics) · [Calcite 谓词元数据](https://calcite.apache.org/javadocAggregate/org/apache/calcite/rel/metadata/RelMdPredicates.html)
