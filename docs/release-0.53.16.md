# Navidog 0.53.16

- MySQL 和 MariaDB 支持带 `IF NOT EXISTS` 的函数及存储过程声明。编辑器分步提示，原方言生成保留声明，脚本分析保留已知的原例程签名。MySQL 需要 8.0.29 或更高版本。
- PostgreSQL 本地规划可从各 OR 分支共有的整数等值条件恢复唯一键和行数上界。AND 分支内的冲突等值可证明零行，执行原查询时仍保留已有的求值和错误边界。
- 隔离数据库对照覆盖重复创建、不同的例程签名、OR 的不同等值以及冲突条件。

[MySQL 8.0.29 发布说明](https://dev.mysql.com/doc/relnotes/mysql/8.0/en/news-8-0-29.html) · [MariaDB CREATE PROCEDURE](https://mariadb.com/docs/server/server-usage/stored-routines/stored-procedures/create-procedure) · [Calcite 谓词元数据](https://calcite.apache.org/javadocAggregate/org/apache/calcite/rel/metadata/RelMdPredicates.html)
