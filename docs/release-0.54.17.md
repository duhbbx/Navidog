# Navidog 0.54.17

- 处理上一版剩余的 200 条 PostgreSQL 原生树结构差异：保留隐式投影名、逗号 FROM、函数别名列表、LATERAL JOIN、命名窗口及继承、显式 ASC、CASE 的省略 ELSE、列属性与表约束顺序；200 条全部恢复原生树一致，不增加豁免规则。
- 修复未使用的 WINDOW 定义漏做分组检查的问题。窗口名缺失、重复、前向引用、循环和继承冲突保留语法有效状态，并在分析阶段报错，阻止无效模型继续生成 SQL。
- 优化器单独输出窗口表达式时展开完整定义，避免引用外层未携带的窗口名。
- 新增 36 条独立语法回归，覆盖源结构、窗口分析边界以及后续语句的语法错误；补充模型所有权、修改后生成、取消和不安全 JOIN 结构保护测试。
- 补充 PostgreSQL 15/18 临时实例中的查询、列描述和 DDL 目录对比。详情及复现命令见 [SQL 文法语料报告](sql-grammar-corpus.md)。

## English

- Resolved all 200 outstanding PostgreSQL raw-tree differences by preserving native source structure, without adding review exemptions.
- Validate grouping in unused WINDOW definitions and report named-window resolution errors during analysis. Invalid definitions cannot enter SQL generation.
- Added 36 independent syntax fixtures, model mutation/ownership checks and disposable PostgreSQL 15/18 result, column-description and DDL catalog probes.
