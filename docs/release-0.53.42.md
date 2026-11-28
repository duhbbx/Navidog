# Navidog 0.53.42

- SQL Server 的 `OUTPUT` 补全可根据操作提示 `inserted.` 或 `deleted.`，并继续补全该行的实际字段。编辑尚未写到 `VALUES` 的 `INSERT` 时，也能使用这套作用域。
- `UPDATE … OUTPUT`、`DELETE … OUTPUT` 可补全已绑定来源表的列。目标列通过 `INSERTED` 或 `DELETED` 引用。进入 `WHERE` 等后续子句后，输出行候选退出作用域。
- 语义检查覆盖缺少限定符、使用当前操作不可见的行以及误用目标表别名。已通过编译器、编辑器补全和往返回归。服务器执行验证仍待补，`OUTPUT INTO` 和 `MERGE OUTPUT` 仍是独立的语法缺口。

[SQL Server OUTPUT](https://learn.microsoft.com/en-us/sql/t-sql/queries/output-clause-transact-sql?view=sql-server-ver16)
