# Navidog 0.53.41

- PostgreSQL 联邦查询可以把小整数集合与等值、范围及 NULL 条件一起交给来源执行。同一列上的多个集合会取交集。对不同列的集合，当前选择候选值最少的一组发送，保留所有原条件供本地校验。
- 含复杂表达式的 AND 条件保留原读取路径。排序分页仍要求来源完整执行所有筛选条件。
- 隔离 PostgreSQL 中超过两千行的测试表用单行读取预算验证了范围和集合并用、同列集合求交、按其他列筛选及空交集。测试也核对了忽略过滤提示的来源仍返回相同结果。

[Calcite FilterTableScanRule](https://calcite.apache.org/javadocAggregate/org/apache/calcite/rel/rules/FilterTableScanRule.html)
