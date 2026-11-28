# Navidog 0.53.7

## TiDB

- 新增 TiDB 连接类型与版本识别。新增连接默认端口为 4000，使用随应用打包的 MySQL 原生客户端，通过 TiDB 的 MySQL 兼容协议连接。查询、格式化、补全、元数据校验、转换和本地优化使用 TiDB 方言。
- 解析 `AUTO_RANDOM`、`PRIMARY KEY … CLUSTERED / NONCLUSTERED`、`SHARD_ROW_ID_BITS`、`PRE_SPLIT_REGIONS`、`AUTO_RANDOM_BASE` 和 `PLACEMENT POLICY`。支持识别 `SHOW CREATE TABLE` 输出中已覆盖的 TiDB 专用 DDL 注释。校验随机 ID 的类型、主键位置、位数和互斥选项，以及预分裂位数。
- 新增 `ADMIN SHOW DDL` 和 `ADMIN SHOW DDL JOBS [数量] [WHERE …]`，并识别 `TIDB_VERSION()`、`TIDB_CURRENT_TSO()`。存储函数、存储过程和触发器显示 TiDB 不支持的提示。
- 执行计划使用服务端 `EXPLAIN FORMAT='row'`，保留原始行、父子算子和估算行数。

## OceanBase MySQL 模式

- 解析 `CREATE / DROP TABLEGROUP`、`SHARDING`、建表归组、`ALTER TABLE … SET TABLEGROUP` 及解除归组。
- 支持独立 `CREATE INDEX … LOCAL / GLOBAL`、`SHOW TABLEGROUPS`、`SHOW CREATE TABLEGROUP`，并识别 `OB_VERSION()`。
- 两种方言的新增 DDL 选项均进入 AST 和 IR。同方言生成保留原选项。跨方言转换遇到需要显式目标映射的物理布局会停止并标出对应选项。

参考语法见 [TiDB 建表](https://docs.pingcap.com/tidb/stable/sql-statement-create-table/)、[TiDB AUTO_RANDOM](https://docs.pingcap.com/tidb/stable/auto-random/)、[OceanBase 表组](https://docs.oceanbase.com/oceanbase-database/V4.4.2-create-tablegroup-mysql-compatible-mode) 和 [OceanBase 索引](https://docs.oceanbase.com/oceanbase-database/V4.4.2-create-indexes-mysql-compatible-mode)。

在隔离的 TiDB 8.5.3 和 OceanBase CE 4.3.5.5 验证了原生连接、所列建表或表组操作，以及用真实表字段元数据校验未知列。TiDB 同时验证了建表定义的往返解析和服务端执行计划。
