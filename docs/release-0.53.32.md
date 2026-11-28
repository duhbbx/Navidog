# Navidog 0.53.32

- `ALTER TABLE` 编辑时按方言提示可用操作。删除、修改或重命名现有列时，候选来自目标表。新列和重命名目标使用声明位置，SQL Server 修改列可直接补全数据类型，Oracle 分组 `MODIFY` 可补全组内各列。
- 同一条 `ALTER TABLE` 中可以用前面已解析的操作更新候选。前面新增的列可用于后续动作，已经删除的列从候选中移除。
- PostgreSQL 本地规划按输出位置传播集合运算的已证明整数范围。`UNION` 合并范围，`INTERSECT` 取交集，`EXCEPT` 保留左侧范围。直接整数宽化保存列范围和非空证明。
- 隔离 PostgreSQL 对照了整数宽化、互斥集合、重复行语义和 NULL 分支，核对本地规划与生成 SQL。

[Calcite 关系谓词推导](https://calcite.apache.org/javadocAggregate/org/apache/calcite/rel/metadata/RelMdPredicates.html) · [PostgreSQL ALTER TABLE](https://www.postgresql.org/docs/15/sql-altertable.html)
