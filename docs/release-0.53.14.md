# Navidog 0.53.14

- MySQL 与 MariaDB 视图定义会报告用户变量、系统变量和参数，检查可确定的重复列名及显式列清单的数量。列清单可以为查询结果重新命名。子查询内的变量会定位到使用处。
- PostgreSQL 普通分组的原生 `MIN` 或 `MAX` 作用在已验证的单列非空整数唯一键上时，其结果也可以证明唯一。规划器据此去掉外层冗余的 `DISTINCT`。实库对照覆盖 `HAVING`、非唯一输入和分组集合。

[MySQL CREATE VIEW](https://dev.mysql.com/doc/refman/8.4/en/create-view.html) · [Calcite 唯一键元数据](https://calcite.apache.org/javadocAggregate/org/apache/calcite/rel/metadata/RelMdUniqueKeys.html)
