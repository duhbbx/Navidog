# Navidog 0.53.30

- PostgreSQL 支持 `ARRAY[[...], [...]]` 的语法解析、补全和同方言生成。显式数组转换会为嵌套构造器提供元素类型。已知维度的数组可以在编辑时检查行宽不一致、混合标量与数组，以及超过六维的字面量。
- 多维字面量保留统一的 PostgreSQL 数组类型，并独立计算值的维度。规划和本地执行可按 PostgreSQL 存储顺序展开可证明的矩形整数数组，保留 `NULL`、序号和输出预算。
- 隔离 PostgreSQL 对照了二维、三维、空数组及类型化窄整数的展开结果，并核对了不规则数组的报错。

[PostgreSQL 数组构造器](https://www.postgresql.org/docs/15/sql-expressions.html#SQL-SYNTAX-ARRAY-CONSTRUCTORS) · [PostgreSQL 数组函数](https://www.postgresql.org/docs/15/functions-array.html) · [PostgreSQL 数组维度上限](https://github.com/postgres/postgres/blob/REL_15_STABLE/src/include/utils/array.h)
