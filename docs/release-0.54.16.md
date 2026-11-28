# Navidog 0.54.16

- 扩展 MySQL、PostgreSQL、SQL Server、Oracle、SQLite、MariaDB、TiDB 和 OceanBase MySQL 模式的独立 SQL 语法回归，累计 2,277 条 `.sqltest`。修正表达式优先级与产生式边界、窗口函数参数、DELETE 目标别名和分页表达式等问题。
- PostgreSQL 补齐关键字上下文、匿名派生表、分页、非结合表达式、零列表和无名索引；SQLite 增加空 IN、空值判断、分页与 GLOB；SQL Server 增强 CONVERT/TRY、JSON 构造、过滤索引、关键字/别名和定义批次；TiDB 单独检查列类型及 DDL 规则。
- SQL 补全会为保留字名称添加正确的分隔符，并按值关键字补全 CURRENT_USER 等表达式。转换至 SQL Server 的定义脚本保留 GO 批次边界；缺少确定映射的跨库语义整体拒绝，不发布半段 SQL。
- 增加固定提交文法、ANTLR `.g4`、公开测试与原生解析器的采集和审计工具。候选预期、语法/绑定/运行错误、准备/执行结果分别记录；上游脚本不执行，原始参考资料和可选解析库不进入应用。
- 来源、复现命令、版本限制和待复核差异见 [SQL 文法语料与差分回归](sql-grammar-corpus.md)。Oracle 与 OceanBase 的本轮结果不宣称经过实库执行；原生树差异也不自动认定为语义等价。

## English

- Expanded independent grammar regressions across all eight SQL frontends to 2,277 `.sqltest` cases, covering expression productions, window arguments, DELETE aliases, pagination and dialect-specific DDL.
- Improved PostgreSQL keyword/derived-table rules, zero-column tables and server-chosen index names, SQLite GLOB and pagination, T-SQL conversions/JSON/index predicates/batches, and TiDB type productions. Completion now delimits reserved identifiers and preserves SQL value-keyword syntax.
- Added pinned reference collection and offline/native differential audits with explicit error-stage and version boundaries. SQL Server definition conversion emits required GO boundaries; uncertain cross-dialect mappings fail atomically.
- Reference SQL is never executed. Optional parser libraries and upstream source caches are not shipped. See the corpus report for reproducible checks and outstanding reviews.
