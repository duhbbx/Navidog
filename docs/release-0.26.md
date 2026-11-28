# Navidog 0.26：DDL、统一 IR 与规范 SQL

在 0.25 DML / JOIN / 子查询基础上，新增核心 CREATE / ALTER / DROP / TRUNCATE / RENAME / COMMENT，支持表、视图、索引、Schema、数据库与序列的常用语法。各引擎独立前端保留差异，经过 Lowering 得到拥有数据的类型化 Model，包含列类型、约束、默认 / 生成表达式、ALTER 动作、索引与序列选项。

查询窗口点击“解析 AST”，可在“语法树”查看统一 Model，在“规范 SQL”查看从 IR 生成的同方言 SQL。仅解析不会连接或执行；正常运行依然把原文提交给官方驱动。生成内容只读，不会自动替换用户 SQL。

Model 保留源方言、位置、上下文及事务效果；生成器有输出 / 工作量 / 嵌套 / 取消保护，失败不输出半段 SQL。跨方言转换尚未开放，不声称简单归一化就实现语义等价。

验证：七套 CTest 回归通过（未配置 MariaDB / Oracle 服务时相应集成项跳过）；独立编译器 26 组用例含 2000 份混合 DML / DDL 畸形输入、往返、字面量模式、所有权与限额测试。SQLite、隔离 PostgreSQL / MySQL 实际执行生成的表 / 视图 / 索引 / ALTER / DML，并验证 PG TRUNCATE 能回滚、MySQL TRUNCATE 不回滚。Oracle / SQL Server 只有解析 / 生成测试，未连接实例执行。CLT 21 ASan / UBSan 通过；Win7 C++17 交叉构建、10 个 PE 依赖审计通过，当前版本客户机运行待验收。

完整范围和限制见 [SQL 编译库](sql-compiler.md)。本批不含所有厂商对象 / 版本、MERGE、集合 / 窗口、存储过程体、完整语义诊断、基于 AST 的格式化与补全。

参考：[MySQL CREATE TABLE](https://dev.mysql.com/doc/refman/8.4/en/create-table.html)、[PG CREATE TABLE](https://www.postgresql.org/docs/current/sql-createtable.html)、[Oracle CREATE TABLE](https://docs.oracle.com/en/database/oracle/oracle-database/19/sqlrf/CREATE-TABLE.html)、[SQL Server CREATE TABLE](https://learn.microsoft.com/en-us/sql/t-sql/statements/create-table-transact-sql)、[PG TRUNCATE](https://www.postgresql.org/docs/current/sql-truncate.html)、[SQL Server TRUNCATE](https://learn.microsoft.com/en-us/sql/t-sql/statements/truncate-table-transact-sql)。

Mac 已安装 0.26.0，正常退出后恢复最新 5 个工作标签与草稿，连接 / 工作区文件与退出备份字节一致。原生窗口仅解析 4 条 CREATE / INSERT SELECT / ALTER / TRUNCATE，显示统一树及规范 SQL；会话数为 0，未执行这些语句。展开 AST 后测试标签已不保存关闭，原有标签保留，运行实例为 1；旧安装包与暂存目录清理，私有更新备份保留。
