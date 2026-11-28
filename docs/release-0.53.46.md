# Navidog 0.53.46

- SQL Server 的 `INSERT`、`UPDATE` 和 `DELETE` 支持 `OUTPUT … INTO` 及随后返回给客户端的第二个 `OUTPUT`。写入列表和返回列表独立保存。目标可写为表、临时表或表变量。
- 普通表或脚本中已声明的临时表可提供目标字段补全。编辑列清单时会排除清单中已有的列。语义检查覆盖重复列、已知的缺失表或列、视图目标及显式清单的数量。
- 已通过本地语法、语义、补全、格式化和生成回归。SQL Server 实库验证、表变量声明与类型检查、目标表触发器及约束资格检查仍由后续覆盖。

[SQL Server OUTPUT](https://learn.microsoft.com/en-us/sql/t-sql/queries/output-clause-transact-sql?view=sql-server-ver16)
