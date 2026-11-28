# Navidog 0.53.45

- PostgreSQL `PERCENTILE_DISC` 和 `PERCENTILE_CONT` 支持补全、格式化、类型推导与同方言生成。补全会选中示例百分位 `0.5`，便于直接替换。排序表达式继续使用当前查询的字段。
- 分组阶段分别检查百分位参数与逐行排序值。离散百分位保留排序值类型，连续百分位返回双精度数或区间。数组百分位的相应结果类型也进入 SQL IR。
- 本地离散百分位支持数字字面量或 NULL 配合有符号整数排序。测试和隔离 PostgreSQL 对照了重复项、升降序、空分组、过滤与非法百分位。连续插值、数组百分位及按组计算的动态百分位由服务器执行。

[PostgreSQL 有序集聚合](https://www.postgresql.org/docs/15/functions-aggregate.html) · [Calcite 有序集聚合](https://calcite.apache.org/docs/reference.html#ordered-set-aggregate-functions)
