# Navidog 0.43.0：规范类型池、结构化 CAST 与作用域类型

本版包含 [0.41 诊断与真实计划展示](release-0.41.md)、[0.42 SHOW 与多组聚合](release-0.42.md)，继续加强 Parser → AST → 统一模型 → 语义 → 生成的类型基础。

## 类型上下文

每个 ModelProgram 拥有有界 TypeContext；相同的类型描述在同一池内复用 TypeId。拷贝 / 移动模型后编号有效，不引用已销毁的源码、AST 或目录对象；不同池的编号不可混用。默认最多 8192 个描述，单描述最多 64 KiB、池计费上限 8 MiB，并纳入编译 / 优化 / 过程累计预算。外部列、变量及例程类型用不可变共享描述传递，避免在每个表达式里复制完整类型。

描述区分整数位宽 / 符号、精确 decimal、浮点、粗粒度 numeric / string / bytes、长度、长度单位、精度 / 标度、可空性、时区、数组及未解析的类型名称 / 参数。未知保持未知，粗粒度 Decimal 不冒充精确 decimal。SQLite 的类型亲和性不当作其他关系库的强制精度约束。PostgreSQL 数组维数声明不当作独立类型 / 实际维数约束，语法层仍保存原声明。

IR JSON 新增类型池和表达式的类型 ID；解析视图显示类型池、结构化 CAST、精度 / 标度 / 长度单位等。描述相等是已知事实相等，不证明跨厂商的值域、NaN、舍入、字符集、排序规则、溢出或隐式转换行为等价。

## 类型语法与 CAST

CAST 与 PostgreSQL `::` 保存结构化类型语法，包括参数、负标度、多词类型、时区和数组。原方言规范生成复用同一类型打印器，不再把参数藏在字符串里。过程参数 / 返回值 / 局部变量也复用各方言的类型 Parser；PG dollar-quoted 函数体使用单独词法流并保留原文位置，不为每个类型重造建表语句。

新增常见数字精度 / 标度边界及字符串长度检查。保留声明、CAST、函数签名、目录来源的不同默认规则：例如 SQL Server VARCHAR 的声明默认长度与 CAST 默认长度不同；MySQL CHAR CAST 也不当成固定 CHAR 列或默认一字符截断。Oracle 未知 NLS 长度语义保持未知，SQL Server 的字节 / UTF-16 单位与 PostgreSQL 的字符单位分开。

输入目录的原生类型文本有独立的字节 / 标记限额，必须完整解析为一个类型，不接受默认值、列约束或语句拼接，也不会执行 SQL。

## 语义和上下文传播

类型事实进入名称绑定后的列、CTE / 派生结果、输出别名、标量子查询、参数、局部变量与已解析的例程结果。外连接与小计可能引入 NULL，CASE 缺少 ELSE 保留隐式 NULL；COUNT、EXISTS、COALESCE 等只增加已覆盖的保守事实。重载返回类型不一致时，不猜一个精确结果。SQL Server COUNT 的算术会话设置未知时，不声称绝对非空。

ScriptContext 的 CREATE、CTAS 输出及已覆盖的 ALTER 类型 / 可空性更新同步保留或替换类型事实。过程变量由原符号 ID 和作用域选择，不以全局同名文本替代。

一个已验证的方言差异：PostgreSQL 会校验签名类型修饰的合法性，但不会将合法修饰作为形参 / 返回值的存储约束；局部变量的修饰会生效。签名与局部变量分别推导，并阻止把被忽略的源修饰转成目标数据库的新约束。Oracle 不接受形参 / 返回值的内联长度、精度、标度约束；命名子类型和更复杂类型解析尚未完整支持。参数化 CAST 的跨产品转换继续明确拒绝，直到精度、舍入和值域映射得到验证。

## 后台目录

刷新服务器目录或读取 SQLite 快照时，在工作线程一次性准备原生类型事实；最多尝试 512 种类型并限制计费字节。编辑时复用结果，不为每个字符访问数据库或重解析全部目录类型。类型补充失败只保留未知，已有表 / 列检查不因此失效。磁盘只保存原始类型文本，派生事实按当前编译器重新构建，无新增本地数据库迁移。

目录只返回基础名称时不套用 DDL 默认值，例如 SQL Server 的 `decimal` 不臆造为 decimal(18,0)，`nvarchar` 不臆造为长度 1。目录没有可空性 / 字符集 / 排序规则时保留未知。

## 验证与边界

- 类型专项覆盖池内复用 / 独立拷贝、预算、精度 / 负标度 / 长度单位、结构化 CAST 往返、外连接、CTE、小计、过程作用域、返回重载、脚本内 ALTER、元数据重建与拼接拒绝。
- 项目自建 PostgreSQL 15 实例核对 numeric(12,3)、varchar(30)、带时区 timestamp 和数组目录；实际比较函数签名不截断数值、非法 numeric(0) 被拒绝、局部 numeric 变量发生舍入。
- 项目自建 MySQL 实例核对 decimal、varchar、unsigned bigint 的目录事实，以及 signed / unsigned / decimal CAST 的源 / 生成 SQL 结果。
- 第一轮完整 13 组 CTest 通过（46.22 秒）。长度单位和 CAST 默认值补强后的最终记录在打包后补充。Oracle / SQL Server 对应规则主要是官方文档和离线回归，不代表本版真实服务器验收；Win7 也需单独客户机运行验收。

这仍不是完整 RelDataTypeFactory / coercion / 重载解析。复杂记录、ROW / MAP / MULTISET、全部数组算子、完整数值运算精度 / 值域、字符集 / 排序规则 / 会话配置及所有厂商版本仍有缺口；新事实尚未被任意优化规则当作等价性证明。Calcite 完整库存继续标记未完成。

参考：[Calcite 类型工厂](https://calcite.apache.org/javadocAggregate/org/apache/calcite/rel/type/RelDataTypeFactory.html)、[PostgreSQL 函数类型修饰](https://www.postgresql.org/docs/15/sql-createfunction.html#SQL-CREATEFUNCTION-NOTES)、[Oracle 形参](https://docs.oracle.com/en/database/oracle/oracle-database/19/lnpls/subprogram-parameters.html)、[PostgreSQL 字符类型](https://www.postgresql.org/docs/15/datatype-character.html)、[SQL Server 字符类型](https://learn.microsoft.com/en-us/sql/t-sql/data-types/char-and-varchar-transact-sql)。

最终完整 13 组 CTest 再次通过（46.36 秒）；ASan 七组核心 / 编译 / 类型 / 优化等测试通过（12.48 秒），三个重点 GUI 用例通过（含初始化 / 收尾共五项，1.58 秒）。Win7 x64 构建及 10 个 PE 依赖审计通过；本版客户端包仍仅含 SQLite，未做 Win7 客户机运行验收。

macOS 正常退出前确认三个查询无活动事务，私有备份工作区与密钥后替换签名应用。启动后观察原 13 个标签、0 自动连接 / 0 会话；退出后与启动后的 SQL 指纹一致，只运行一个安装实例。旧版 / 中间包、应用 rollback 和已验证 staging 已清理，SQL 草稿未丢弃。Win7 包 SHA256：`1743a19d98a29cf21968f2faa01b1faee2cfebbd764b36e2620df084b5f5a345`。
