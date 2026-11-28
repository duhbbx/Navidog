# Navidog 0.50.0：窗口排除模式与类型化帧

## 前端、上下文与 IR

PG / SQLite 的 EXCLUDE NO OTHERS / CURRENT ROW / GROUP / TIES 使用 WindowExclusion 枚举，保存在 AST / 拥有数据的 Model WindowSpec 中；AST / 统一模型报告显示 exclusion。命名窗口继承保留该字段，含帧的父窗口仍不能通过括号复制。后端验证枚举和目标能力，原方言规范生成保留排除模式；手工 IR 的非法枚举也会被拒绝。

四种模式的结果不同，不能统一去掉排除条款。目标没有 EXCLUDE 时，仅将显式 NO OTHERS 归一化成默认行为；其余模式给出诊断，不用不可靠的 COUNT / SUM 相减替代。NULL 排序与原转换边界继续独立生效，跨库转换仍是受限子集。

MySQL 的 RANGE INTERVAL 属于合法区间使用位置；不再一概按独立区间报错。行帧 / 分组帧的明显时间、区间或数组偏移给出诊断；已知 PG / MySQL 数字 ORDER BY 要匹配数值偏移，时间 ORDER BY 要匹配区间。NULL 字面量偏移报错。动态值、负区间文本、所有 coercion / 版本化文法仍未完整覆盖，数据库继续负责执行期检查。

未使用的命名窗口也进行已覆盖的类型和依赖校验；合法 MySQL 时间 RANGE 可被接受，偏移不能引用普通行字段、聚合或窗口函数。PG 的整数偏移允许其本身的 coercion 边界；SQLite TRUE 行偏移作为数值常量保留，不制造布尔误报。

## 格式化、候选和规划边界

原文保持格式化识别排除关键词，多个预设保持稳定；PG / SQLite 的窗口帧末尾和 EXCLUDE 后提供限定候选，支持 NO OTHERS / CURRENT ROW 的续词。候选仍由现有编辑器统一交互，不连接数据库或改变文本。

优化器保留窗口边界，本版没有新增本地窗口执行运行时或自动帧改写。公开 IR 的排除枚举及窗口载荷进入规划器验证 / 内存预算；保留原计划基线。

## 验证

临时 PG、MySQL、SQLite 已对照原始 SQL 与生成 SQL：含重复值的四种排除模式逐行核对，MySQL 三个日期滚动汇总为 1 / 3 / 4，并验证已覆盖的时间 / 数字偏移错误。格式化、AST / IR 所有权、生成后的重解析、非法枚举、参数 / 字节预算及上下文候选加入回归。执行链路中 EXCLUDE 已从 NotCovered 变为 Parsed；未覆盖语法的直通执行回归改用临时 SQLite 递归 CTE，未因此宣称支持递归 CTE 的本地规划。

最终测试、打包和安装记录随后补充。本版仍不是 Calcite 全量移植。

参考：[PG 15 窗口表达式](https://www.postgresql.org/docs/15/sql-expressions.html#SYNTAX-WINDOW-FUNCTIONS)、[SQLite 窗口排除](https://www.sqlite.org/windowfunctions.html#the_exclude_clause)、[MySQL 8.4 窗口帧](https://dev.mysql.com/doc/refman/8.4/en/window-functions-frames.html)。

最终验证：完整 13 组 CTest 全过（43.02 秒）；ASAN 七组全过（13.16 秒），五项 GUI 场景连同初始化 / 清理七项全过（2.251 秒）。独立 SQLite 3.54.1 内存库覆盖 48 种帧 / 排除组合，每种五个聚合，三个格式预设共 144 次执行，规范生成 / 格式化结果一致且格式化幂等（2.99 秒）。这是 SQLite 的结果验证，不能冒充 PG / Oracle / SQL Server 的所有执行兼容性。

安装记录：已正常保存并退出 0.43，将已验证签名的 0.50 安装到 `/Applications/Navidog.app` 并启动。恢复 15 个工作区标签（包括两个未保存函数定义），正常退出后的备份与启动后备份逐项 SQL / 完整状态指纹相同；GUI 显示 8 个连接配置、0 已打开连接、0 个会话。工作区数据库及对应密钥的私人备份做完整性检查，权限分别限制为目录 0700 / 文件 0600，未输出 SQL 或凭据内容。

运行实例检查：仅一个 `/Applications/Navidog.app/Contents/MacOS/navidog` 进程。已安装可执行文件 SHA-256：`dc05a7beb602ff32a0120820ced320a04c43df1c5b70834ecd7cf2c77461ccfa`，与打包后文件一致。验证后已清理 0.43–0.49 的中间 Win7 包、macOS / Win7 展开暂存及 0.43 回滚 App；保留最新发行 ZIP、源码 / 构建目录和私人工作区备份。

Win7 ZIP 校验与十个 PE 依赖审计通过，SHA-256：`32c8b76b0a0c119ebc80deb5c013cf27eabd6fe59efd03b31d8757e07f27938b`。仍为 SQLite-only 构建；本轮未在 Win7 VM / 实机运行，不把交叉编译和依赖审计称为运行验收。
