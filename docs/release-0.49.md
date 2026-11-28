# Navidog 0.49.0：聚合结果类型、参数与已知转换差异

共享 resolveAggregateType 提供部分原生聚合参数 / 结果类型事实，供类型推导和转换检查使用。已解析的用户例程不按同名内建函数处理；未知类型 / 参数仍保持未知，尚未实现完整重载选择和 coercion。

- PG：SUM(smallint / integer) → bigint，SUM(bigint / numeric) → 无输入 typmod 的 numeric；AVG(整数 / numeric) → numeric，AVG(real) → double，SUM(real) → real，区间聚合不继承输入限定。COUNT 是 bigint。
- SQL Server：已知整数 SUM / AVG 的提升、decimal SUM(38,s) / AVG(38,max(s,6))、real / float 的提升及 COUNT / COUNT_BIG 位宽。COUNT 可空性仍受会话设置边界影响，不当作无条件非空证明。
- MySQL：精确参数 SUM / AVG 为 decimal 类别，近似参数为 double。未从当前目录中猜测 div_precision_increment / 会话标度或完整累加精度。
- SQLite：AVG 为近似数值结果、SUM 维持运行时数值类别；加入 TOTAL 内建聚合、double 非空结果事实及目录补全。Oracle / MariaDB / OceanBase 只保留已覆盖的保守事实。

已知 PG / SQL Server SUM / AVG 不接受的参数类型给出定位诊断，包括已覆盖的字符串、日期、布尔 / bit、数组反例。没有把此检查称为全量函数类型系统。类型事实经 CTE、窗口及已有上下文传播。

跨库 SUM / AVG 已知结果域、位宽或参数化 decimal 行为不一致时原子失败；即使名字相同也不直接放行。未发现差异不等于已完成所有语义证明：未知输入、所有累加精度、排序规则、SQL 模式、扩展重载仍须后续完善；浮点聚合的执行顺序也不能靠类型描述符保证一致。

说明标记使用 2.6px 深琥珀虚线波浪、浅色背景和行号提示；波纹间距加大，错误红波浪为 2px。保持文字可选和普通编辑行为。

验证：新增临时 PG / MySQL / SQLite 对照已通过（17.68 秒）；完整 13 组 CTest 通过（53.48 秒）。检查 PG 的真实聚合结果类型与拒绝形式，MySQL 返回类型 / 原生生成，SQLite TOTAL(NULL)=0.0。SQL Server 的类型规则根据官方文档和静态样例，本轮没有连接实际服务验收。后续 ASAN / 包记录在下方补充。

参考：[PG 15 聚合](https://www.postgresql.org/docs/15/functions-aggregate.html)、[MySQL 8.4 聚合](https://dev.mysql.com/doc/refman/8.4/en/aggregate-functions.html)、[SQL Server SUM](https://learn.microsoft.com/en-us/sql/t-sql/functions/sum-transact-sql)、[SQL Server AVG](https://learn.microsoft.com/en-us/sql/t-sql/functions/avg-transact-sql)。

ASAN 七组通过（13.33 秒），三项 GUI 场景连同初始化 / 清理五项通过（1.574 秒），已查看 [说明标记实际渲染](assets/diagnostic-note-0.49.png)。Win7 SQLite-only 包十个 PE 依赖审计通过，未进行 Win7 运行验收。Win7 ZIP SHA-256：`aeaed7dc602db6ebed43c18d1b515299f51c6ce431630efb8b84d33cbbea1926`。此中间版未安装，待后续统一更新。
