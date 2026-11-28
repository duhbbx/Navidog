# Navidog 0.44.0：带类型字面量与日期转换

在 0.43 类型池的基础上，增加专用 TypedLiteral AST / 统一模型节点，保存结构化源类型、解码值和原文范围。CAST / TypedLiteral 的源类型在模型拷贝间不可变共享，过程源码位置重映射使用新副本；优化器及过程累计内存预算计入类型语法载荷。

## 前端、上下文与格式化

- MySQL-family 识别 DATE / TIME / TIMESTAMP 字符串字面量；Oracle 识别 DATE / TIMESTAMP；PostgreSQL 识别通用 `type 'value'`，包括类型参数、多词类型、时区、限定 / 引号类型名和 dollar-quoted 值。
- PostgreSQL 数组常量必须使用 CAST / `::`，不把 `integer[] '{1,2}'` 错当合法类型字面量。PG WITH LOCAL TIME ZONE、Oracle WITHOUT TIME ZONE、DATE 的精度参数等非法类型组合会报错。
- 独立类型推导保留字面量的类型参数和时区事实；未知自定义输入函数保持未知。MySQL TIMESTAMP 字面量产生 DATETIME 的事实区别于 TIMESTAMP 存储列。Oracle 的类型名称 DATE 仍有时间部分，不能把它的通用值域当作其他厂商的纯日期。
- AST / 模型 JSON 增加 typedLiteral / literalType，现有解析视图可查看。原方言规范生成不拼接未经验证的 SQL 片段。格式化保留值和注释，支持关键字大小写、各预设的幂等性与源码映射。
- 表达式补全提供 DATE / TIME / TIMESTAMP 有效值模板和载荷选区，按方言屏蔽不可用语法。SQL Server / SQLite 不接受 ANSI 日期字面量语法。
- MySQL 支持函数调用后的字符串列别名（含 AS），不将 `DATE('…') 'label'` 误识别为带类型字面量；补入保留关键字 TIME 的函数调用入口。

类型语法解析不代替服务器的全部输入转换函数：DateStyle / NLS / SQL mode、自定义类型输入函数、所有无效日期和时间值仍未完整本地验证。未对任意文字执行服务器转换，也不假设类型字面量绝对非空或不会报错。

## 转换

跨库仅放行已覆盖的 Gregorian ISO DATE 字面量子集：精确 YYYY-MM-DD、真实日历日期、1000..9999 年，不带类型参数、限定 / 引号名称或时区。MySQL / PG / Oracle 目标保留 DATE 字面量；SQL Server 2008+ 目标生成 `CAST('YYYYMMDD' AS DATE)`，避免依赖月日显示顺序。新增表达式计入目标节点限额，失败不发布部分 SQL。

已覆盖的明确 SYS.DUAL 单行查询映射继续生效；未限定 DUAL 是一个可由用户映射的对象名，不擅自假设所有同名表都恰有一行。

时间戳 / 时区 / 区间、自定义类型、非 ISO 或特殊日期（如 infinity）、未证明的日期加减，以及混合字符串与日期的转换会给出显式诊断。对象可以尚未部署，转换不会要求先建源表。本增量不是完整的日期运算、隐式类型转换或跨库函数映射。

## 验证

编译器专项增加类型字面量往返、源码销毁后的模型独立性、非法方言、字符串别名、日期转换、时区 / 日历 / 混合类型拒绝和目标节点限额。格式化语料纳入日期 / 时间 / 精度 / 时区 / 区间与不同大小写预设；内存预算用例包含结构化类型载荷。

临时 PostgreSQL 15 和 MySQL 实际比较原 SQL 与规范生成 SQL 的结果，包括秒精度、时区、numeric 负标度、varchar 截断、区间与字符串别名；实际执行不同源方言转入 PG / MySQL 的 DATE 查询。全部测试只用项目自建临时实例，不连接用户业务库。Oracle / SQL Server 本轮为离线语法 / 转换验证，尚无本版真实服务器验收。

Calcite 全量移植仍未完成。本轮是独立 C++ 的类型 / 前端 / 目标生成增量，不增加已移植规则数量，不宣称兼容 Calcite Java API。

参考：[PostgreSQL 类型常量](https://www.postgresql.org/docs/15/sql-syntax-lexical.html#SQL-SYNTAX-CONSTANTS-GENERIC)、[MySQL 时间字面量](https://dev.mysql.com/doc/refman/8.4/en/date-and-time-literals.html)、[SQL Server DATE 格式](https://learn.microsoft.com/en-us/sql/t-sql/data-types/date-transact-sql)。

最终完整 13 组 CTest 通过（43.08 秒）；ASan 七组核心 / SQL 测试通过（12.56 秒），三个重点 GUI 用例通过（含初始化 / 收尾共五项，1.647 秒）。Win7 x64 构建及 10 个 PE 依赖审计通过，包 SHA256：`ea25c9f0ddac09a1213929c1703871c46ce84be545d4ab34dae7e14a3e05cf88`。macOS 签名安装包已生成；观察到用户正在现有 0.43 窗口执行查询，本阶段没有退出或覆盖其草稿，安装暂留到后续完整更新。
