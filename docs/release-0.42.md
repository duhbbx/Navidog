# Navidog 0.42.0：MySQL 检视语句与多组聚合成本

本版包含 [0.41 的诊断和数据库执行计划展示](release-0.41.md)，并继续扩展独立编译前端与本地优化器。

## MySQL SHOW / DESCRIBE

MySQL-family 前端新增独立的检视语句 Parser、AST、拥有数据的统一 IR、JSON 展示和同方言 SQL 生成，不把它们伪装成普通 SELECT。

覆盖 SHOW DATABASES / SCHEMAS、TABLES、COLUMNS / FIELDS、INDEX / INDEXES / KEYS、TABLE STATUS、VARIABLES、STATUS、PROCESSLIST，以及 SHOW CREATE 的表、视图、数据库、函数、过程、触发器、事件；另支持 DESCRIBE / DESC 表与可选字段 / 字符串模式。按语句种类检查 FULL、EXTENDED、SESSION / GLOBAL、FROM / IN、LIKE / WHERE 和重复数据库限定。

WHERE 绑定到检视结果的字段，例如 Field、Table_type、Variable_name，不绑定到被查看表的业务列。动态 Tables_in_数据库名 在上下文未知时仍保持未知。已完成的目录快照可检查检视目标表；没有完整目录时不猜测对象不存在。补全提供检视关键字、数据库 / 表 / 对象候选及 WHERE 结果字段。

格式化继续保留原标识符、注释和字面量，新语句关键字进入既有大小写与布局配置。未实现的 SHOW 分支仍有本地检查说明；DESCRIBE 查询计划形式尚未进入该前端。

**检视语句仅支持原方言生成，跨产品转换明确拒绝。** 不用未经证明的信息模式查询替换动态返回列、权限可见性、隐藏对象等语义。MariaDB / OceanBase 的厂商扩展和所有版本差异尚未穷尽。

## GROUPING SETS / ROLLUP / CUBE 的本地规划

关系 Aggregate 节点保存有序分组集合，保留重复集合和显式空集合。Memo 摘要和 JSON 保留集合内容；节点 / 字节 / 引用预算纳入这些负载。本地默认最多 256 个集合和 8192 个分组引用。

每个集合独立估计分组数量后相加；空集合即使输入为空仍贡献一行。多组 Hash Aggregate 的 CPU / 内存按组数及估计状态计费，不用单一 Sort Aggregate 假装实现任意分组集合。基线和候选保持相同语义，统计相关性仍是估计，不能作为重写证明。

关系元数据分别累加各组的硬行数边界。多组结果不凭普通分组键推导唯一性，避免重复总计和小计 NULL 冲突导致错误移除 DISTINCT。HAVING 中的小计 NULL 不套用原表的 NULL 比例作为事实。这里是分组集合关系表示 / 估计的增量，**不是 AggregateGroupingSetsToUnionRule、完整 grouping-id 唯一性推理或执行算子实现**。

## 验证

- 完整 12 组 CTest 通过，49.00 秒，含项目自建临时 PostgreSQL 15 与 MySQL。无自动连接用户业务数据库。
- MySQL 对比检视原 SQL 与生成 SQL 的列名和行值；覆盖表 / 视图 / 字段 / 索引 / 变量 / DESCRIBE，并用实际目录检查缺失目标。
- PostgreSQL 比较规范分组 SQL 的实际结果，并核对本地关系元数据的行数边界；空输入下两个重复总计均保留。单位测试另覆盖 NDV 估计、重复组、HAVING NULL、预算、无效引用和 DISTINCT 保留。
- ASan 七组核心 / 编译 / 转换 / 补全 / 优化 / 元数据 / 计划测试通过，11.69 秒。Win7 x64 交叉构建通过。Qt 泄漏检查关闭；Oracle / SQL Server / OceanBase / MariaDB 对应新增功能的完整真实实例与 Win7 客户机仍待验收。

Calcite 全量仍未完成，类型、Rex、去相关、规则、适配器和执行运行时缺口继续保留在 [清单](calcite-port.md)。当前已构建发布包，原生安装情况另行记录。
