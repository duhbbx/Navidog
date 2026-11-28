# Navidog 0.53.63

- PostgreSQL 本地分析开启 Calcite 扩展后，可解析静态单轴 `PIVOT`，支持 `COUNT(*)`、`COUNT(列)`、`SUM`、`AVG`、`MIN`、`MAX`。补全对应的聚合、字段和关键字。
- 转换后的 SQL 采用 PostgreSQL 的过滤聚合和隐式分组列。CLI 可用 `--emit postgresql`、`--emit-plan`，桌面端可从“计划 SQL”查看。
- 隔离 PostgreSQL 对照了列名与类型、NULL 分组、没有匹配项的分组、空输入和下游筛选。“Calcite 扩展语法”默认关闭。

[Calcite PIVOT 语法](https://calcite.apache.org/docs/reference.html)
