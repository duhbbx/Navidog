# Navidog 0.30：统一设置与窗口函数诊断

全局“设置”弹窗整合常规、SQL 编辑器、查询与结果、SQL 格式化、元数据五类。左侧为设置分类，右侧显示对应表单；SQL 格式化只是其中一类，右侧页签细分基本样式、列表、间距和函数 / 存储过程。入口是应用菜单（macOS）/ 工具菜单“设置…”，以及查询和定义编辑页的“设置”。后两者打开同一个窗口并定位 SQL 格式化。

本轮实际可配置项：启动恢复工作区、编辑器字号 / 缩进宽度 / 自动换行、新查询默认缓存行数与结果布局、自动刷新元数据，以及原有全部 SQL 格式化选项。保存一次原子写入，取消不改变设置。字号 / 缩进 / 换行应用当前和新建编辑器；查询默认项只影响新建页，保留已有页的独立状态。元数据关闭自动刷新后仍可手动同步。旧格式配置在首次读取时兼容，保存后纳入统一设置；连接、TLS / SSH / HTTP 凭据及端点仍属于各自连接，不能混成全局参数。

SQL 新增独立的 Window AST / IR 节点、窗口布局元数据及同方言生成：函数调用 OVER(...)、PARTITION BY、ORDER BY、ROWS / RANGE 框架，支持通用的 UNBOUNDED / CURRENT ROW / PRECEDING / FOLLOWING；GROUPS 仅在支持的方言基线启用。窗口键和帧表达式都是拥有型 IR 的普通表达式子节点，参加字段绑定和资源预算。此基线为 MySQL 8+ 等现代版本；未加入完整数据库版本能力诊断。命名 WINDOW / OVER name、EXCLUDE 和完整窗口语义限制尚未覆盖，仍交给服务器判断。

MySQL 的 ON CONFLICT / EXCLUDED 现在定位为本地方言错误，红色波浪线和悬浮说明提示使用 ON DUPLICATE KEY UPDATE。不会把用户 SQL 自动改写成另一方言，也不会仅因本地未覆盖语法而禁止执行。

修复整批降级：在没有 DDL / 动态上下文变更的 DML 脚本中逐条运行可验证元数据检查，一条语法失败不再抑制后续可解析语句的表 / 字段提示。未同步目录时明确显示“表/字段未校验”。缓存必须当前会话已验证、未失效且未过期；不使用不完整缓存断言对象不存在。语法失败的那条语句仍不保证能发现所有引用的表和字段；自动同步也不会擅自打开未连接的业务库。

测试仅使用临时 SQLite / 隔离 PG 与 MySQL；日志位于 `.cache/sql30-*.log`。覆盖全局设置分类与内部页签、统一原子保存 / 取消、旧设置兼容、编辑器实际应用，以及混合脚本中的红色方言提示和橙色缺失表 / 字段提示。执行分析回归保留命名窗口作为尚未覆盖但允许原文送服务器的样例。Oracle / SQL Server 本轮无实际实例回归；Win7 包仍只含 SQLite，构建不能代替客户机运行验收。

参考：[MySQL 窗口语法](https://dev.mysql.com/doc/refman/8.4/en/window-functions-usage.html)、[MySQL INSERT SELECT](https://dev.mysql.com/doc/refman/8.4/en/insert-select.html)、[MySQL 重复键更新](https://dev.mysql.com/doc/refman/8.4/en/insert-on-duplicate.html)、[PostgreSQL INSERT / ON CONFLICT](https://www.postgresql.org/docs/current/sql-insert.html)。

安装验收：已替换 `/Applications/Navidog.app` 为 0.30.0，单进程运行。全局快捷键 Cmd+, 打开设置的“常规”，编辑器“设置”进入同一个窗口的“SQL 格式化”，右侧复杂对象页签切换通过。独立本地解析测试确认第一条为 ON CONFLICT 方言错误，第二条内联窗口 CTE 为 syntaxValid=true；未执行 SQL、未连接业务库。测试标签已关闭，保存的原有 12 个标签 / 草稿以及连接配置与安装前备份逐字节一致。

最终 7 套 CTest 通过（含隔离 PG / MySQL；MariaDB / Oracle 未启用真实实例的部分自动跳过）。独立编译器 30 组测试，CLT 21 ASan / UBSan 通过。Win7 SQLite 包交叉构建完成，10 个 PE 依赖审计通过；0.30 Win7 客户机运行验证未完成。安装前旧版 0.29 存在一次 Qt macOS accessibilitySelectedChildren 崩溃记录，当前原生验收未复现；不宣称本轮修复该底层问题。
