# Navidog 0.53.53

- Oracle 和 SQL Server 的 `PIVOT` 支持聚合函数、参数列、透视轴以及 `FOR`、`IN` 的上下文补全。未闭合的参数或轴能从原始表和派生表识别字段。
- 已知输入与输出形状的静态 `PIVOT` 进入本地关系计划。按隐式分组列、不同值数与 NULL 组估算行数。无分组的 Oracle PIVOT 在空输入上仍输出一行，已对照 Oracle Free。
- 本地计划保留分组列、透视轴与聚合列的血缘。动态 `PIVOT XML`、多聚合 PIVOT、本地执行及跨方言转换仍待实现。

[Oracle SELECT](https://docs.oracle.com/en/database/oracle/oracle-database/19/sqlrf/SELECT.html) · [Microsoft FROM 与 PIVOT](https://learn.microsoft.com/en-us/sql/t-sql/queries/from-transact-sql?view=sql-server-ver16#use-pivot-and-unpivot) · [Calcite RelBuilder](https://calcite.apache.org/javadocAggregate/org/apache/calcite/tools/RelBuilder.html)
