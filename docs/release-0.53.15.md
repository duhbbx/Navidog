# Navidog 0.53.15

- MySQL 与 MariaDB 的函数和存储过程支持 `COMMENT` 与 `LANGUAGE SQL`，原方言生成保留这些声明。例程头补全能提示特性和正文起始关键字，也支持 `SQL SECURITY`、`READS SQL DATA` 等子句的后续词。
- PostgreSQL 的本地关系元数据可把经过验证的完整整数唯一键等值过滤收紧到最多一行。该证明也适用于各成员都被固定的复合键，可消掉对过滤结果的冗余去重。只限定部分键或使用 OR 时保持原来的行数推导。

[MySQL 创建例程](https://dev.mysql.com/doc/refman/8.4/en/create-procedure.html) · [Calcite 唯一键元数据](https://calcite.apache.org/javadocAggregate/org/apache/calcite/rel/metadata/RelMdUniqueKeys.html)
