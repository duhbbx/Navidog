# Navidog 0.53.70

- 表及例程的候选达到上限时，仍优先保留与输入完全匹配的项目。同一名称靠后、位于指定 Schema 或包含大量类似前缀时，也可直接补全。
- 完全匹配与普通补全共用本地名称匹配和排序。已验证表、函数、存储过程的候选上限与排序。

PostgreSQL 的 Calcite 功能可从查询编辑器右键打开“SQL 本地分析 / 优化”，勾选“Calcite 扩展语法”后分析。在“计划 SQL”可以查看和复制可生成的 PostgreSQL 改写结果。

[Calcite 实现范围](calcite-port.md) · [UNPIVOT 过滤下推](release-0.53.69.md) · [大目录按输入补全](release-0.53.67.md)
