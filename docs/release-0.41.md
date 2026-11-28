# Navidog 0.41.0：清晰诊断与可核对的数据库执行计划

SQL 检查说明改为深琥珀色加粗虚线波浪，保留语法错误的红色波浪；说明位置同时增加浅琥珀背景、行号颜色和左侧圆点。说明与错误保留不同含义，未支持的本地语法不冒充服务器语法错误。

![SQL 检查说明标记](assets/diagnostic-note-0.41.png)

“解释”沿用真实驱动请求，在后台建立隔离会话，与当前标签的事务分开。新增请求记录，展示连接名称、数据库、Schema、服务器版本、请求时间、耗时及实际交给驱动的计划命令。连接初始化和 Schema 设置未列入命令记录；不保存凭据。记录只在当前结果内存中，并随用户显式导出计划 JSON 输出，SQL 本身可能包含用户输入的字面量。

| 数据库 | 实际计划机制 |
| --- | --- |
| PostgreSQL | EXPLAIN JSON，显式 ANALYZE FALSE |
| MySQL / MariaDB | EXPLAIN FORMAT=JSON |
| OceanBase MySQL 模式 | EXPLAIN FORMAT=JSON，独立解码 OPERATOR / CHILD_n 结构 |
| SQL Server | 隔离会话 SET SHOWPLAN_XML ON 后提交所选 SQL，读取估计 XML |
| Oracle | 独立 STATEMENT_ID 的 EXPLAIN PLAN，读取 PLAN_TABLE 后回滚 |
| SQLite | EXPLAIN QUERY PLAN |

执行计划视图增加“树形、表形、原文、计划请求”切换。表形使用虚拟模型，一行对应一个算子，可数值排序、搜索、复制和查看属性。树形与表形最多显示 1000 个算子；完整解码和导出仍有独立预算。原文界面最多预览 256 KiB，完整有界文档可导出。SQLite / Oracle 的原始字段和行也以 JSON 保留；截断文档可以检查，但不会被标记为完整计划。

![执行计划算子表](assets/estimated-plan-table-0.41.png)

新增 MySQL v2、MariaDB filesort / block-nl-join 等结构处理；OceanBase 的 CHILD_n 按数字排列，估计微秒单独显示，不与 COST 混用。SQL Server 结构节点校验命名空间。未知属性仍可在原文检查，不代表所有版本的所有结构已完整覆盖。

本地优化器仍是独立 C++ 成本模型。“解释”是服务端估计计划；二者均不等于实测执行耗时。隔离计划会话看不到标签里的临时表和未提交结构。

验证：完整 12 组 CTest 通过（36.51 秒）；项目自建 MySQL 临时实例 21 项通过，SSH / HTTP 环境用例跳过。包含 SQLite、PG、MySQL 对 DELETE 的实际计划读取及数据保留断言。ASan 核心、计划、完整界面三组通过（55.09 秒）；Win7 x64 交叉构建通过。Oracle / SQL Server / OceanBase / MariaDB 新结构主要依靠离线样例，不能替代对应实际服务器及 Win7 客户机验收。截图为 Qt 测试捕获，并非真实数据库在线截图。

本版没有新增 Calcite 规则，完整移植仍未完成，缺口继续记录在 [功能清单](calcite-port.md) 和固定上游源码库存中。当前发布包已构建；原生安装验证另行记录。
