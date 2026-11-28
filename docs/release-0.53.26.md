# Navidog 0.53.26

- MySQL 与 MariaDB 的 `CREATE [AGGREGATE] FUNCTION ... SONAME` 有独立的语法树和统一模型。编辑器补全 `RETURNS`、支持的返回类型和 `SONAME`，同方言格式化和 SQL 生成保留函数属性。MariaDB 另支持 `OR REPLACE`。跨库转换需要明确的库文件和部署映射。
- 隔离 MySQL 与 MariaDB 验证了原始 SQL 和生成 SQL 均通过语法检查并到达库文件查找阶段。测试使用专门生成的不存在的库名。
- PostgreSQL 本地规划在完整整数唯一键、非空匹配及求值安全均可证明时，可以移除未被使用的 `LEFT JOIN` 或 `RIGHT JOIN`。隔离 PostgreSQL 比较了 `IS NOT DISTINCT FROM` 下的生成 SQL 和实际结果，覆盖双侧 NULL 重复匹配、继承范围和启用行级安全的表。

[MySQL 可加载函数语法](https://dev.mysql.com/doc/refman/8.4/en/create-function-loadable.html) · [MariaDB 可加载函数语法](https://mariadb.com/docs/server/server-usage/user-defined-functions/create-function-udf) · [Calcite ProjectJoinRemoveRule](https://calcite.apache.org/javadocAggregate/org/apache/calcite/rel/rules/ProjectJoinRemoveRule.html)
