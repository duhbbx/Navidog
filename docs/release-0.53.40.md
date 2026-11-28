# Navidog 0.53.40

- PostgreSQL、MariaDB 和 TiDB 的修改表语句支持单列 `ADD COLUMN IF NOT EXISTS` 和 `DROP COLUMN IF EXISTS`，也可以补全这些关键字。已有列和已知缺失列按相应条件跳过。选择目标不支持列条件的方言时，SQL 转换会提示具体缺失能力。
- PostgreSQL 和 MariaDB 接受 `ALTER TABLE IF EXISTS`。编辑一条包含多项操作的语句时，后续列补全会参考已完成的操作。
- PostgreSQL 支持省略 `COLUMN` 的列改名和 `SET DATA TYPE … USING …`，保留类型转换表达式。
- 隔离的 PostgreSQL 15 和 MariaDB 11.4 测试执行了生成的语句，并核对列类型、列名和数据。TiDB 覆盖解析、补全、格式化和原方言生成。

[PostgreSQL ALTER TABLE](https://www.postgresql.org/docs/15/sql-altertable.html) · [MariaDB ALTER TABLE](https://mariadb.com/docs/server/reference/sql-statements/data-definition/alter/alter-table) · [TiDB ADD COLUMN](https://docs.pingcap.com/tidb/stable/sql-statement-add-column/) · [TiDB DROP COLUMN](https://docs.pingcap.com/tidb/stable/sql-statement-drop-column/)
