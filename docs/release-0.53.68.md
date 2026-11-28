# Navidog 0.53.68

- 本地执行 `PIVOT` 和 `UNPIVOT` 时可读取显式登记的基表。扫描按已验证的原始表列取数，转列和转行后再按各自的输出列处理筛选。
- 联邦查询入口 `navidog-federate --calcite-extensions` 可对已登记的数据源启用扩展。查询以只读方式读取，仍受行数、内存和时间预算约束。
- 用隔离 PostgreSQL 核对了基表的多轴 PIVOT 和 UNPIVOT，并运行了真实命令行入口。
