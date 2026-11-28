# Navidog 0.47.0：可信约束、NULL 条件与 Memo 基数

新增 HepFilterNullability：只使用当前目录的显式立即非空保证或合格主键，以及已绑定的标量字段，消除恒真的 IS NOT NULL 条件。单纯字段 NULL 检查被证明不可能成立、且来源是无已知求值影响的纯表时，可使用空关系。含可能报错的其他表达式时保留其求值结构；外连接补 NULL、未知列、数组 / 复合类型、延迟键约束和普通类型推导都不当作该证明。

## 约束事实与类型事实分开

ColumnDefinition.nonNullKnown 表示明确的立即非空保证；SqlType.nullability 仍是保守类型事实，不自动升级为优化证明。NOT NULL / SET NOT NULL / DROP NOT NULL 进入 ScriptContext。CTAS 不把运行时投影的非空性当作新存储列约束，未建模继承规则保持未知；未建模 ALTER 不保留旧主键证明。

SQLite 的普通 TEXT / INT / 复合主键可能含 NULL，本版不再一概作为非空唯一键。只在已覆盖的精确 INTEGER 行号别名或所有键字段有明确 NOT NULL 时使用相应保证；STRICT / WITHOUT ROWID 的前端声明尚未覆盖。普通 TEXT / INT 主键两个 NULL 的实际反例，以及 INTEGER 行号、CTAS 插入 NULL 已加入临时 SQLite 回归。

关系元数据保留 INNER/CROSS 两侧、LEFT 左侧、RIGHT 右侧的非空字段，FULL 两侧都不保留；经投影只传播可证明的直接谱系。模型标为 catalogChecked=false 时，扫描不使用主键 / 非空约束作为硬事实。现有目录层仍会拒绝过期 / 未验证快照；此处是在模型到规划器之间再加一道保护。

## 后台目录与本地缓存

后台批量目录读取补充普通字段的非空事实：PG 使用 attnotnull，MySQL-family 使用 IS_NULLABLE，SQLite 使用 pragma 的 notnull，SQL Server 使用 is_nullable。计算列不参与这类证明；视图字段也不按基础表立即约束使用。Oracle 的延迟 / 状态规则需要更完整字典，暂不从简单 NULLABLE 字样推断该保证。

事实随元数据保存到原缓存 SQLite 的新增 member_constraints 附表，不修改原 members / profiles / 草稿字段。旧缓存没有附表记录时为未知；使用同一 v1 的附加表兼容方式，旧程序可忽略新表。FK 级联随原缓存记录失效 / 替换而清理，不建立第二套无界缓存。磁盘快照加载仍未验证，验证 / TTL / DDL 失效流程保持生效。输入时不额外连库。

## 等价组和物理计划

Memo 为等价组交集保留硬行数上下界，精化估计时不以统计值制造证明；若某条规则与已有硬界限冲突，会停止并诊断。物理 Values / Scan / Project / Sort / Window / Limit 等根据当前输入传递基数，Join / Set 和空输入聚合也保留相应语义。

这修正了“关系规则已证明为空、物理图仍沿用旧估计”的情况。空输入 COUNT 仍输出一行，普通分组输出零行，重复总计集合仍分别输出各自总计；没有把任何 Aggregate 都错误裁成零行。未知形状的限定星号投影不当作恒等投影删去。

## 验证与边界

本轮完整 13 组 CTest 已通过（53.85 秒），真实执行仅用项目临时 PG / MySQL / SQLite。覆盖当前 / 过期 / 未验证目录、旧缓存附表兼容、即时约束 / 软类型分离、外连接、复合类型边界、潜在报错表达式、SQLite NULL 主键、CTAS、聚合和分页中的空基数传递。SQL Server 的目录查询本轮未接真实服务验收，Oracle 不增加相应推断。

该增量扩充 ReduceExpressionsRule 的受限同类能力，库存仍为十一项 partial-analogue；没有把它计作完整 RexSimplify / RelMdPredicates / 约束求解器，也没有自动修改或执行用户 SQL。

参考：[SQLite 主键与 CTAS](https://www.sqlite.org/lang_createtable.html)、[PG pg_attribute](https://www.postgresql.org/docs/15/catalog-pg-attribute.html)、[MySQL COLUMNS](https://dev.mysql.com/doc/refman/8.4/en/information-schema-columns-table.html)、[SQL Server sys.columns](https://learn.microsoft.com/en-us/sql/relational-databases/system-catalog-views/sys-columns-transact-sql)。

最终补充验证：ASAN 七组 13.03 秒，三项 GUI 场景连同初始化 / 清理五项 1.714 秒；Win7 SQLite 包十个 PE 依赖审计通过，未在 Win7 实机运行验收。Win7 ZIP SHA-256：`16bd4c52fd6f364bc4f49c8ee8cea096402d211cf9f6e19a3f806fca2d0126eb`。该中间包没有替换仍在使用的 0.43。
