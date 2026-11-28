# Navidog 0.53.20

- MySQL 系例程支持带标签的语句块。光标在 `LEAVE` 后可选当前语句块和循环，`ITERATE` 后可选循环。语义检查区分作用域和处理器边界，生成的 SQL 保留跳转目标。临时 MySQL 和 MariaDB 验证了嵌套跳转和例程输出。
- PostgreSQL 规划器利用完整唯一键上已知的整数范围计算行数上界。复合键按各列的离散值组合数估算，未知键或溢出的范围保守处理。临时 PostgreSQL 和独立规划测试验证了主键、复合键、缺少键列约束和大整数端点。
- PostgreSQL 常量数组的物理计划使用已验证的元素数估计展开成本。计划说明区分已核对的常量数组与按经验值估计的输入。

[MySQL 语句标签](https://dev.mysql.com/doc/refman/8.4/en/statement-labels.html) · [Calcite 关系元数据](https://calcite.apache.org/javadocAggregate/org/apache/calcite/rel/metadata/package-summary.html)
