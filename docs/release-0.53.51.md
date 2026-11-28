# Navidog 0.53.51

- Oracle 和 SQL Server 在 `UNPIVOT ... IN (` 中提示原始输入列，支持带引号的列名和部分未闭合的子句。补全会去掉已在列表内的列。
- 已知输入列时，本地成本计划把 `UNPIVOT` 作为独立关系节点。计划保留原始扫描和转行后的输出。行数估计考虑每列的 NULL 比例，已知行数上界随分支数扩张。
- 列血缘记录保留列和度量来自哪些源字段。生成标签没有源列，原表唯一键不会直接传给能扩张行数的结果。
- 本地运行和跨方言转换仍待实现。Oracle、SQL Server 可继续执行原生语句。

[Calcite RelBuilder.unpivot](https://calcite.apache.org/javadocAggregate/org/apache/calcite/tools/RelBuilder.html#unpivot(boolean,java.lang.Iterable,java.lang.Iterable,java.lang.Iterable))
