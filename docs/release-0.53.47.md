# Navidog 0.53.47

- SQL Server `MERGE` 支持 `OUTPUT`、`OUTPUT INTO` 和第二个返回给客户端的 `OUTPUT`。输出列表可补全 `$action`、`INSERTED.`、`DELETED.` 和来源表字段。
- `$action` 在 AST 和 SQL IR 中有独立节点，类型为 `nvarchar(10)`。输出中的虚拟行及可能缺失的来源行按可空类型处理。
- 编辑尚未输入分号的 `MERGE` 仍可补全，实际语句校验保留分号要求。已通过本地编译器、补全和原方言 SQL 往返回归。SQL Server 实库验证仍待补。

[SQL Server OUTPUT](https://learn.microsoft.com/en-us/sql/t-sql/queries/output-clause-transact-sql?view=sql-server-ver16)
