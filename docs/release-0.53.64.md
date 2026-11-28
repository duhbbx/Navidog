# Navidog 0.53.64

- Calcite 静态 PIVOT 可在本地解释器执行。利用整数轴值定位对应桶，同时保留命中多个同值桶和未命中任何桶的分组。
- 本地执行支持有界派生表或 CTE、整数轴、COUNT、精确整数与十进制 SUM/AVG、MIN/MAX、NULL 分组以及空输入。文本轴需要 PostgreSQL 排序规则，仍可生成原生 SQL。
- 隔离 PostgreSQL 对照了重复桶、没有匹配项的分组、精确大整数和均值，校验超限时不返回部分分组。
