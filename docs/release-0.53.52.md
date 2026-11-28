# Navidog 0.53.52

- Oracle `UNPIVOT` 支持多个度量列和标签列，可使用字符、数字、NULL 和日期等常量作标签。AST 和统一模型保留每个输入元组、标签元组及其列位置。
- 补全覆盖多列输入元组和结果字段。编译器校验元组宽度、对应类型以及输出列名，并为标签和各度量分别推导类型及可空性。
- 本地成本和血缘按元组保留每个度量的来源。排除 NULL 只丢弃全部度量为空的元组。缺少相关性统计时，计划会标记多列 NULL 比例为估算。
- 隔离 Oracle Free 实例通过了 13 组原始 SQL 与生成 SQL 对照，覆盖默认标签、重复输入、混合度量和空元组。

[Oracle UNPIVOT 语法](https://docs.oracle.com/en/database/oracle/oracle-database/19/sqlrf/img_text/unpivot_in_clause.html) · [Calcite SqlUnpivot](https://calcite.apache.org/javadocAggregate/org/apache/calcite/sql/SqlUnpivot.html)
