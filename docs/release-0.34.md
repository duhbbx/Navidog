# Navidog 0.34 — SQL 转换与过程作用域

查询工具栏、查询菜单和视图 / 函数 / 过程定义工具栏新增“转换 SQL…”。弹窗左右显示源与目标，支持源 / 目标方言、单级数据库 / Schema 限定映射、DDL 事务变化审核、诊断定位、统一模型 / 作用域报告，以及复制和导出。输入为当前选区或全文的副本；原草稿不会被覆盖，转换不会执行数据库命令。

独立 C++17 API `convertSql` 支持已有七类方言入口，主要验证 MySQL / PostgreSQL 双向转换。源 Parser → owning Model → 目标能力检查 / lowering → 方言后端 → 目标再次解析。整个脚本原子交付；任何错误、取消、超限或未覆盖构造都会清空目标结果。源文、字符串、引用名称不做关键字替换；客户端 DELIMITER / GO / Oracle 斜杠在保留源字节位置的分析副本处理。

| 类别 | 本版范围 |
| --- | --- |
| 查询 | SELECT、常见子查询 / JOIN / CTE / CASE / 窗口表达式、分页；按目标语法能力及已知语义检查限制 |
| DML | INSERT VALUES / SELECT、单目标简单 UPDATE、DELETE；连接更新 / 删除、多目标、限行 DML、UPSERT / RETURNING 等不做未经证明的重写 |
| DDL | 常用 CREATE / ALTER / DROP TABLE、基础约束 / 索引 / 视图；物理选项、identity、序列默认行为、复杂重命名等有明确诊断 |
| 物化视图 | PG / Oracle 基础原生 CREATE / DROP；目标无原生等价物时拒绝，不降级为普通视图；NO DATA / 刷新属性等有边界 |
| 函数 / 过程 | MySQL、PL/pgSQL、PL/SQL、T-SQL 的受限标量 / 过程子集；参数、局部声明 / 初值、赋值、嵌套块、IF / ELSE、WHILE、RETURN、部分内嵌 SQL |
| 上下文 | 源位置、作用域树、参数 / 变量符号表、最近作用域查找、遮蔽、重名 / 未声明变量诊断、IR 符号 ID；SQL Server 局部变量按过程作用域处理 |

目标变量局部名称按符号去歧义，参数名保留，避免任意改写命名参数调用。跨库 OUT / INOUT 的调用协定、参数默认值、异常、游标、动态 SQL、包、触发器、FOR / LOOP、复杂记录 / 表值返回和完整类型推导仍未覆盖。PG / Oracle 内嵌 SQL 中未经限定的列 / 变量冲突拒绝猜测；可用标签限定变量。Oracle 数值域、空字符串、长度 / 时区等差异不会简单替换类型；因此普通 NUMBER、VARCHAR2 等跨库类型转换常需人工确定政策，不能称为 Oracle 任意过程的完整转换器。

DDL 的隐式提交 / 可回滚差异默认阻止，须显式允许后继续。已知不安全的运算符、未映射函数、NULL 排序、字符串 / 类型 / 对象能力差异给出诊断。库函数、列类型、排序规则、溢出、权限、调用方和服务端版本的全部语义尚未证明；**目标再次解析只证明本地已覆盖语法可接受，不等于任意 SQL 的执行等价保证**。本版是可用的结构化转换首版，未达到四种数据库所有语句精准互转。

真实隔离 PostgreSQL / MySQL 测试覆盖转换后函数 / 过程创建、命名参数、局部变量、嵌套遮蔽、条件、循环和结果值；不使用用户业务库。Oracle / SQL Server 本版只有离线文法和能力检查，未完成服务端验收。Qt 回归覆盖弹窗输出、模型报告、复制、编辑后立即失效和失败原子性。Win7 构建仍只有 SQLite 驱动，离线转换库与桌面共享源码。

命令行：

```sh
navidog-sql --dialect mysql --convert postgresql input.sql
navidog-sql --dialect mysql --convert postgresql --ast input.sql
navidog-sql --dialect mysql --convert postgresql --allow-ddl-transaction-change input.sql
```

架构参考：[PG 块结构与标签](https://www.postgresql.org/docs/15/plpgsql-structure.html)、[MySQL 局部变量优先级](https://dev.mysql.com/doc/refman/8.4/en/stored-program-restrictions.html)、[SQL Server 变量作用域](https://learn.microsoft.com/en-us/sql/t-sql/language-elements/variables-transact-sql)。
