# Navidog 0.53.22

- MySQL 与 MariaDB 支持存储过程内的预处理语句。补全可提示过程里已出现的语句名和 `FROM`、`USING`、`DEALLOCATE PREPARE`。在可确认的路径上，编辑器提示参数数量不匹配和执行已释放语句。
- 临时 MySQL 与 MariaDB 核对了会话变量绑定、重复调用、释放语句和存储函数内事务控制的服务端报错。
- PostgreSQL 本地规划可用已确认的整数或布尔列上的 `IS NULL`、非空约束和比较条件收紧行数上界。临时 PostgreSQL 测试覆盖未知空值、可满足的 OR 分支和真正互斥的条件。

[MySQL 预处理语句](https://dev.mysql.com/doc/refman/8.4/en/sql-prepared-statements.html) · [MariaDB PREPARE](https://mariadb.com/docs/server/reference/sql-statements/prepared-statements/prepare-statement) · [Calcite 谓词元数据](https://calcite.apache.org/javadocAggregate/org/apache/calcite/rel/metadata/RelMdPredicates.html)
