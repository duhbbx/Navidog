# Navidog 0.53.48

- PostgreSQL 联邦扫描可合并同一整数列上的等值、`IN` 和等值 `ANY` 的 OR 分支。合并会去掉重复值及超出列宽的值，再以绑定参数下推，原谓词仍由本地执行器核对。
- 等值分支可引用已绑定的参数。已验证的 OR 键集合可与 AND 范围条件和排序分页共用。跨列 OR 和无法完整分析的分支沿用原扫描路径。
- 在隔离 PostgreSQL 的两千余行测试表上验证了负值边界、带 NULL 的分支、排序分页及单行读取预算。本地回归也验证了忽略推送提示的来源保持重复行和结果。

[Calcite Sarg](https://calcite.apache.org/javadocAggregate/org/apache/calcite/util/Sarg.html) · [Calcite FilterTableScanRule](https://calcite.apache.org/javadocAggregate/org/apache/calcite/rel/rules/FilterTableScanRule.html)
