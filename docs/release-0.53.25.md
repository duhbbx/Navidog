# Navidog 0.53.25

- MySQL 与 MariaDB 校验 `CREATE TABLE` 的 `AUTO_INCREMENT`。编辑器可提示缺少支持索引、有多个自增字段或同时设置默认值。引擎对复合索引列顺序的要求由服务端确认。
- MySQL 与 MariaDB 的视图定义使用脚本作用域判断是否引用了已声明的临时表。诊断标记实际引用的表名，在派生查询内也生效。隔离数据库核对了临时表、永久 `LIKE` 副本及自增列的合法和非法写法。
- PostgreSQL 本地规划可识别 `IS NOT DISTINCT FROM` 的完整整数连接键。能够确认每个字段至少有一侧非空时，规划可推导匹配上界并保留外连接的唯一键。隔离 PostgreSQL 对照了复合键、结果行与双侧 NULL 重复匹配。

[MySQL 建表语法](https://dev.mysql.com/doc/refman/8.4/en/create-table.html) · [MySQL 视图语法](https://dev.mysql.com/doc/refman/8.4/en/create-view.html) · [Calcite 唯一键元数据](https://calcite.apache.org/javadocAggregate/org/apache/calcite/rel/metadata/RelMdUniqueKeys.html)
