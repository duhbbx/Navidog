# Navidog 0.25：增删改解析与统一 AST

新增 INSERT、INSERT SELECT、UPDATE、DELETE，复用表达式、嵌套 / 关联子查询及 CTE。JOIN 使用显式二叉关系树，增加 USING、NATURAL、嵌套 JOIN 和逗号混用，保留来源关系与作用域。

按方言加入常用扩展：MySQL 的 INSERT SET / IGNORE、ON DUPLICATE KEY UPDATE、JOIN UPDATE / 多目标 DELETE；PG 的 UPDATE FROM、DELETE USING、ON CONFLICT、RETURNING；SQLite 的冲突算法 / UPSERT / RETURNING；Oracle 的 RETURNING INTO / 多列子查询赋值；SQL Server 的 TOP、OUTPUT、UPDATE / DELETE FROM。Oracle 采用 19c 基线，MariaDB / OceanBase 当前仅 MySQL 兼容子集。

查询窗口新增“解析 AST”：不执行 SQL、不连接数据库、不改变已有事务，结果区“语法树”显示统一 Model。可展开节点、复制 JSON，双击含 origin 的节点定位未改变的完整原文；正常执行 SQL 也会更新同一解析结果。解析选区的范围相对于提交文本，避免错误选择整篇编辑器的位置。

JSON 每批最多 2 MiB、可视树最多 2500 节点，有时间 / 取消保护；原有结果与 SQL 草稿继续保留。解析未覆盖的语句正常运行时仍交官方驱动执行，原 SQL 不改写。

范围与边界见 [SQL 编译库](sql-compiler.md)。本版未实现所有版本完整文法、集合 / 窗口 / 递归 / MERGE、跨方言 SQL 生成或新的格式化 / 补全。

语法参考：[PG INSERT](https://www.postgresql.org/docs/current/sql-insert.html)、[MySQL INSERT](https://dev.mysql.com/doc/refman/8.4/en/insert.html)、[Oracle UPDATE](https://docs.oracle.com/en/database/oracle/oracle-database/19/sqlrf/UPDATE.html)、[SQL Server INSERT](https://learn.microsoft.com/en-us/sql/t-sql/statements/insert-transact-sql)、[SQLite INSERT](https://www.sqlite.org/lang_insert.html)。

验证：七套 CTest 回归通过（MariaDB / Oracle 未配置服务端时按设计跳过）；20 组编译器用例含 2000 份畸形输入，ASan / UBSan 通过。隔离 PG / MySQL 验证 DML 的解析与真实执行；SQLite / Qt 测试仅解析不连接、不执行、不改变事务 / 结果集。新增辅助功能激活后的树选择、模型重置及标签关闭回归；最终 UI 套件 47 项通过。Win7 C++17 交叉构建与 10 个 PE 依赖审计通过，未在 Win7 客户机运行验收。

Mac 已安装 0.25，保留正常退出时最新的 9 个原有标签和草稿，配置文件字节一致。原生窗口识别 INSERT SELECT + JOIN / EXISTS、UPDATE、DELETE 并展示统一 AST，状态与会话数确认 SQL 未执行；树展开后的测试页已不保存关闭，原有标签保留。一次 macOS Qt 辅助功能桥崩溃后增加树的选择 / 焦点清理和模型解除关联，已在原生窗口重测关闭通过。当前仅一个实例；旧版包和暂存 bundle 已清理，私有备份保留。
