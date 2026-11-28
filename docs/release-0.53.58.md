# Navidog 0.53.58

- CLI 的 `--calcite-extensions` 可解析单度量、单标签列的 `UNPIVOT`。支持派生表、CTE、文本与 NULL 标签，也保留输入字段补全和独立行数估算。
- `--emit postgresql` 可将已知输入形状转成 PostgreSQL 的 `CROSS JOIN LATERAL ... VALUES`。每个分支保留自己的标签和度量，重复标签与 NULL 标签仍可独立输出。
- 本地解释器执行已验证的整数度量，并检查展开后的行数和内存。隔离 PostgreSQL 对照了本地结果、生成 SQL、返回类型和 bigint 两端。普通 PostgreSQL 连接沿用原生语法基线。

[Calcite RelBuilder](https://calcite.apache.org/javadocAggregate/org/apache/calcite/tools/RelBuilder.html) · [PostgreSQL LATERAL](https://www.postgresql.org/docs/15/queries-table-expressions.html#QUERIES-LATERAL)
