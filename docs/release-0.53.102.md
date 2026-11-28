# Navidog 0.53.102

- 独立的桌面元数据控制器负责缓存读取、后台刷新、失败重试和 SQL 编辑器更新。
- 查询和对象编辑器共用元数据上下文。移除连接时，一并清理本地缓存和待处理的元数据结果。

## English

- A dedicated desktop metadata controller manages cache reads, background refresh, retries, and SQL editor updates.
- Query and object editors share metadata context handling. Removing a connection clears its local cache and pending metadata results.
