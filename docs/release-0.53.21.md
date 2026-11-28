# Navidog 0.53.21

- MySQL 系 `CREATE TABLE` 新增 `INDEX`、`KEY`、`UNIQUE INDEX`、`UNIQUE KEY` 的解析、校验、补全和同方言生成。索引列补全使用本表列定义并跳过已经加入的列。
- 临时 MySQL 与 MariaDB 核对了创建后的唯一性、前缀长度、降序键、自动索引名、`SHOW CREATE TABLE` 往返和 `CREATE TABLE AS SELECT` 的输出列索引。
- Calcite 开发分支的关系元数据通过有确定位置的列改名传递唯一键和非空性。测试覆盖明确别名与重复别名。

[MySQL CREATE TABLE](https://dev.mysql.com/doc/refman/8.4/en/create-table.html) · [Calcite 唯一键元数据](https://calcite.apache.org/javadocAggregate/org/apache/calcite/rel/metadata/RelMdUniqueKeys.html)
