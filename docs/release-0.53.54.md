# Navidog 0.53.54

- MySQL 和 MariaDB 来源支持联邦查询的有符号整数排序取页。源库执行完整可下推的筛选和键集合，再按要求的 NULL 顺序取 `LIMIT + OFFSET` 行。本地保留原筛选、排序和分页语义。
- 受支持的整数 `MIN` 和 `MAX` 可读取源库的一行边界值。排序并列可能改变所选可见值、筛选无法完整下推或类型未验证时，沿用本地路径和已有资源限制。
- 隔离 MySQL 和 MariaDB 各用三千余行数据在单行预算下验证了有符号 bigint 两端、NULL、自定义排序、键集合以及绑定参数。偏移分页在两行预算下验证。

[MySQL NULL 排序](https://dev.mysql.com/doc/refman/8.4/en/working-with-null.html) · [MariaDB NULL 排序](https://mariadb.com/docs/server/reference/data-types/null-values#ordering)
