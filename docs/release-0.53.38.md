# Navidog 0.53.38

- PostgreSQL 的 `UPDATE`、`ON CONFLICT DO UPDATE` 和 `MERGE` 更新分支支持给数组元素、切片和 JSONB 路径赋值。写入目标保留完整路径，索引与边界可以引用 `FROM` 或 `USING` 来源。
- 补全可在下标右方括号仍未输入时识别已有字段。语义检查区分普通列赋值与局部赋值，允许同列的多次局部修改，并报告同时整列与局部写入、在下标赋值中使用 DEFAULT 等错误。
- 隔离 PostgreSQL 对照了源 SQL 与生成 SQL 的结果和写入后的行，覆盖同列交换、切片、JSONB、冲突更新、MERGE 以及整数绑定参数。

[PostgreSQL UPDATE](https://www.postgresql.org/docs/15/sql-update.html) · [PostgreSQL 数组修改](https://www.postgresql.org/docs/15/arrays.html#ARRAYS-MODIFYING) · [PostgreSQL JSONB 下标](https://www.postgresql.org/docs/15/datatype-json.html#JSONB-SUBSCRIPTING)
