# Navidog 0.53.8

- 新建和编辑 OceanBase 连接时，常规页可选择 **MySQL 模式** 或 **Oracle 模式**。旧连接继续使用 MySQL 模式。新建 Oracle 模式连接默认端口为 `2881`，用户名填写 `用户@租户`，经 ODP 指定集群时可填写 `用户@租户#集群`。
- Apple Silicon 版本内置官方 OceanBase Connector/C。连接后检查服务端是否为 Oracle 租户。SQL 编辑器按 Oracle 语法处理，目录按 Schema 展开，执行计划采用 OceanBase 的 JSON 结果。
- 新版本保留独立 Oracle 数据库的 OCI 选项。OceanBase 表格预览使用查询标签，数据修改可由 SQL 执行。
- 本机社区版用于验证 Connector/C 可加载、普通 MySQL 连接可用，以及错选租户模式时报错。社区版只提供 MySQL 租户，Oracle 模式的正向服务端验收需使用已有 Oracle 租户。

macOS 源码构建可运行 `bash scripts/setup-oceanbase-macos.sh`，然后重新配置 CMake。其他平台可以在配置时提供与目标 ABI 匹配的 Connector/C 开发包目录 `NAVIDOG_OCEANBASE_CLIENT_ROOT`。

[OceanBase Connector/C](https://github.com/oceanbase/obconnector-c) · [OceanBase 租户创建限制](https://docs.oceanbase.com/obd/deploy-oceanbase-cluster-ui) · [Oracle 模式 EXPLAIN](https://docs.oceanbase.com/oceanbase-database/V4.4.2-explain-oracle-compatible-mode)
