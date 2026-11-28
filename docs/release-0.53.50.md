# Navidog 0.53.50

- Oracle 和 SQL Server 支持单度量、单标签列的 `UNPIVOT`。解析、统一模型、格式化和同方言 SQL 生成保留原始列、输出别名及 Oracle 的字符标签和空值模式。
- 结果列可用于查询补全和字段检查。已知数据结构时，会校验输入列与类型，识别结果命名冲突，并按空值模式标记度量字段。正在输入子句时会提示 `FOR`、`IN` 和 Oracle 的空值选项。
- 在隔离 Oracle Free 实例对照了原始和生成 SQL 的列顺序、行值与 NULL。SQL Server 通过了本地语法、绑定、类型和生成往返测试。
- 多度量和多标签列、跨方言转换及本地成本规划仍待实现。

[Oracle SELECT](https://docs.oracle.com/en/database/oracle/oracle-database/19/sqlrf/SELECT.html) · [Microsoft UNPIVOT](https://learn.microsoft.com/en-us/sql/t-sql/queries/from-using-pivot-and-unpivot) · [Calcite SqlUnpivot](https://calcite.apache.org/javadocAggregate/org/apache/calcite/sql/SqlUnpivot.html)
