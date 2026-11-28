# Navidog 0.45.0：区间、时间字段与过程类型上下文

继续 0.44 的类型前端，在表达式和源类型语法中增加不可变 IntervalSpec。起始 / 结束字段、MySQL 单位、起始精度和秒小数精度分别保存；统一 SqlType 使用枚举字段保留区间事实，不把区间起始精度塞进 decimal 的标度。模型拷贝不引用源文本或可变 AST，载荷纳入已有预算。

## 表达式前端

PostgreSQL 支持 INTERVAL 字面量的字段限定、前置 / 末尾秒精度，Oracle 支持年月至日秒的多种字面量简写及两种精度，MySQL-family 支持 INTERVAL 数值表达式及复合单位。MySQL 的 INTERVAL(a,b,…) 标量函数与时间区间语法分开解析，区间操作数只在日期加减及已覆盖的 DATE_ADD / DATE_SUB / ADDDATE / SUBDATE 位置使用。

EXTRACT 是专用语法节点：字段不是列引用，不进入普通函数实参绑定。按方言验证字段列表，Oracle DATE 的 HOUR 等不合法字段有本地错误；SQL Server / SQLite 不放行 EXTRACT 语法。提取结果按已覆盖方言保留 numeric / integer / 字符等保守类型。

原方言生成保留限定和精度；后端会拒绝手工构造的非法区间 / 字段 IR。跨产品区间和 EXTRACT 转换仍明确拒绝，直到日历、单位、精度、返回类型和错误行为映射得到验证。没有通过改函数名来冒充精确转换。

## 类型与过程上下文

区间类型用于已覆盖的 CREATE / ALTER / CAST / 目录类型文本，以及过程参数、返回值和局部变量。PG 区间数组保留元素类型限定和数组语法；Oracle 声明只接受 YEAR TO MONTH 或 DAY TO SECOND，区别于更灵活的字面量简写。

PG 的函数签名会校验类型语法和范围，但忽略区间 typmod；局部变量的字段 / 秒精度约束会生效。两者分别推导，不把被忽略的源签名约束变成目标约束。目录读取与编译仍在原有工作线程 / 本地操作内完成，输入时不访问数据库。

新增常用时间内建的保守类型事实，包括 NOW / CURRENT_TIMESTAMP / CURRENT_TIME / CURRENT_DATE 等的时区差异、已知整数秒精度，以及若干日期 / 区间加减的结果类型。不同区间限制合并时丢弃不再成立的限定。未知用户函数、未建模的 SQL mode / NLS / session、隐式转换、复杂时间算术和完整值校验仍有缺口；类型已知不代表可以任意跨库优化。

## 格式化与补全

时间单位和字段参与关键字大小写及现有预设。格式化保持原值、注释和语句结构；增加区间字面量 / 类型、EXTRACT、日期加减、过程类型等往返和幂等语料。

EXTRACT 的字段位置只提示该方言支持的字段，随后提示 FROM，再回到表达式 / 列作用域。区间的数值后提示单位，TO 后按起始字段筛选合法结束字段。模板插入后选中实际值载荷；补全仍使用已有原生弹层与键盘规则。

## 验证范围

临时 PG / MySQL 比较源 SQL 与规范 SQL 的实际结果，包括区间截断 / 秒精度、日期跨月、复合单位、EXTRACT、INTERVAL 标量函数、目录字段限制和过程局部精度。PG 实测签名区间修饰被忽略、本地变量会舍入，并将观察保留为回归。全部真实执行只使用项目自建临时实例。

Oracle 的本轮语法 / 类型主要来自官方说明及离线回归；本版没有 Oracle / SQL Server 真实服务验收。跨库区间 / EXTRACT 转换仍未实现。这些是独立原生 C++ 前端和模型增量，不是 Calcite 所有时间算子、coercion、Rex 简化器或执行器的全量移植。

参考：[PG 区间类型](https://www.postgresql.org/docs/15/datatype-datetime.html)、[PG 函数类型修饰](https://www.postgresql.org/docs/15/sql-createfunction.html#SQL-CREATEFUNCTION-NOTES)、[MySQL 区间语法](https://dev.mysql.com/doc/refman/8.4/en/expressions.html#temporal-intervals)、[Oracle 区间字面量](https://docs.oracle.com/en/database/oracle/oracle-database/19/sqlrf/Literals.html)、[Oracle 区间类型](https://docs.oracle.com/en/database/oracle/oracle-database/19/sqlrf/Data-Types.html)。

最终完整 13 组 CTest 通过（41.67 秒）；ASan 七组核心 / SQL 测试通过（12.75 秒），三个重点 GUI 用例通过（含初始化 / 收尾共五项，1.641 秒）。Win7 x64 构建和 10 个 PE 依赖审计通过，包 SHA256：`dbfcbdc3aef304f971911594b041a80e675e48fd3ea7b2768ca005eb3221cf04`。本阶段已生成 macOS 签名包，继续开发时保持用户当前安装版和查询会话不动。
