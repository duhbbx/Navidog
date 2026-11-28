# Navidog 0.53.23

- 独立 `CREATE INDEX` 的索引列位置支持上下文补全。MySQL、PostgreSQL、Oracle 和 SQL Server 的测试覆盖目标表字段、已选字段和 PostgreSQL 的 `USING btree`。同一脚本先声明表时，也可以使用脚本里的字段定义。
- PostgreSQL 本地规划可使用已验证的非空整数 B-tree 唯一索引。它能为完整复合键推导过滤后的最大行数，也能在确认输出唯一时省去 `DISTINCT`。`INCLUDE` 字段不参与唯一键计算。
- 隔离 PostgreSQL 核对了原始 SQL 与生成 SQL 的结果，以及可空、部分、表达式和延迟唯一索引、继承表和未经刷新的磁盘缓存等边界。

[PostgreSQL 索引目录](https://www.postgresql.org/docs/15/catalog-pg-index.html) · [PostgreSQL 操作符类目录](https://www.postgresql.org/docs/15/catalog-pg-opclass.html) · [Calcite 唯一键元数据](https://calcite.apache.org/javadocAggregate/org/apache/calcite/rel/metadata/RelMdUniqueKeys.html)
