# Navidog 0.48.0：CAST 方言语义与空结果归约

## CAST：声明、输入和结果域分别检查

MySQL CAST 的 SIGNED / UNSIGNED 是 64 位结果，不等于列声明里的 INTEGER。前端和后端检查已知无效的声明类型目标及参数 / 修饰形式；编辑器 CAST AS 位置使用独立的目标类型候选，CREATE TABLE 的类型候选不受影响。空间 / YEAR 等原生关键字生成时不误加标识符引号。当前不是覆盖所有 MySQL 版本的完整 CAST 目标文法；未知扩展仍保留为未知事实。

跨库整数 CAST 要求目标结果位宽与符号一致，并且输入有可验证的整数范围：包括已覆盖的常量 / NULL，以及当前类型上下文中已知的整数列。支持 PostgreSQL BIGINT 与 MySQL SIGNED 的已证明子集、PG / SQL Server 相同整数域；对超范围值、带小数的输入、未知列类型、无法证明一致的舍入 / 溢出 / 字符串转换，给出诊断并原子失败。对象不需要先部署，调用方可以提供离线类型目录；没有类型证据与“对象必须存在”是不同条件。

Oracle INTEGER 等是 NUMBER(38,0) 的同义词，不自动缩为其他库的窄整数。原生 DATE 域和 SQL 模式、SQLite DATE 数值 affinity 也不自动等同；0.44 已覆盖的便携 DATE 常量映射保持可用。DECIMAL 参数化 CAST、SQLite / MariaDB / OceanBase 跨产品 CAST 等仍缺精确策略，不能称为任意语句精确转换。

## HepPruneEmpty：只用硬证明

新增可关闭、可追踪、有界的空关系归约。硬行数上界为零且关系没有已知求值影响时，已覆盖的 Filter / Project / Sort / Limit / Window / Aggregate / Join / Set 可归约为零行 Values，保留原输出列身份 / 别名、原始 DAG 和成本基线。一般函数、潜在报错表达式、子查询依赖和统计估计不用于无条件消除。

全局 COUNT 空输入仍有一行，GROUPING SETS 中重复总计仍有对应多行；保守求值判断下未证明安全的聚合表达式保留算子。没有自动产生或执行改写 SQL。它属于 PruneEmptyRules 的受限同类能力，不等于完整规则族，也没有按新增一个 *Rule.java 文件计数。

关系元数据识别行输入后附加的 scalar / IN / EXISTS 子查询依赖，不把依赖当作笛卡尔积；附加普通 Scan 仍为无效图。子查询继续保留可能报错及求值次数屏障。DDL 伪表类型事实与 ScriptContext 对 SQLite 可空主键、INTEGER 行号别名的判断保持一致。

## 验证记录

开发中的 CAST 增量已通过 13 组 CTest（45.03 秒），包括项目临时 PG / MySQL / SQLite。最终包含空关系规则的整体验证在下方补充。MySQL 真实执行核对非法 CAST 目标、YEAR 原生生成和 PostgreSQL BIGINT → SIGNED；PG 核对反向生成与小数转整数的原生舍入。SQL Server / Oracle 本轮没有实际服务验收，不将静态测试冒充服务验证。

参考：[MySQL 8.4 CAST](https://dev.mysql.com/doc/refman/8.4/en/cast-functions.html)、[SQLite CAST affinity](https://www.sqlite.org/lang_expr.html#castexpr)、[SQLite 主键](https://www.sqlite.org/lang_createtable.html)。

最终验证：完整 13 组 CTest 通过（53.20 秒）；ASAN 七组通过（13.21 秒），三项 GUI 场景连同初始化 / 清理五项通过（1.625 秒）。Win7 SQLite-only 包十个 PE 依赖审计通过，未进行 Win7 运行验收。Win7 ZIP SHA-256：`2144fd8f61dac74613017f3480d8f9255eb871764c889c525d52aa30fd426aba`。该中间包已生成，尚未替换仍在使用的 0.43；后续统一更新。
