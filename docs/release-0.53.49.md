# Navidog 0.53.49

- MySQL 和 MariaDB 来源可执行联邦查询中的有符号整数比较、NULL 检查、小键集合和组合连接键。值通过原生驱动绑定，并按有符号整数比较。本地继续检查原始条件。
- 已在隔离 MySQL 和 MariaDB 上分别验证了三千余行数据、单行读取预算、有符号 bigint 的两端、小整数越界和复合连接。无符号字段仍由原生列类型校验拒绝参与该执行路径。
- PostgreSQL 的有序取页和组合筛选通过回归。MySQL 系当前沿用本地排序及分页。

[MySQL 类型转换规则](https://dev.mysql.com/doc/refman/8.4/en/type-conversion.html) · [Calcite 过滤扫描](https://calcite.apache.org/javadocAggregate/org/apache/calcite/rel/rules/FilterTableScanRule.html)
