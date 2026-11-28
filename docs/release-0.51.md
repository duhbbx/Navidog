# Navidog 0.51.0：原生类型名称与规范身份修补

本版包含 0.41–0.50 的执行计划、诊断标记、Parser / IR / 类型 / 关系规则、格式化与转换增量，另修复类型名称中的两个上下文问题。

## 明确的 PG 原生类型

TypeResolution 识别明确 pg_catalog 下已覆盖的真实类型名称，包括 int2 / int4 / int8、float4 / float8、numeric、varchar、bpchar、text、bool、bytea、date 和时间类型。按引号规则匹配真实目录拼写，不把 pg_catalog.integer 这种 SQL 别名拼成不存在的目录类型，也不把 app.int4 或不同大小写的带引号命名空间当作原生整数。

原始 AST / Model TypeSpec 继续保留写法，语义类型进入统一 TypeContext；PG 参数 / 返回签名仍遵循忽略 typmod 的已覆盖规则。裸 float4 / float8 识别精度位宽，不接受原生不支持的类型参数。pg_catalog.bpchar 没有 typmod 时不伪造 CHAR(1) 长度；真实 PG 用 abcdef 做往返对照，保持全部字符。

明确原生整数参与已有精确 CAST 子集：pg_catalog.int8 → MySQL SIGNED，pg_catalog.int4 → SQL Server INTEGER；原有范围、舍入与目标能力检查继续生效。数组、其他原生类型、用户类型和不能证明的转换并未因此放开。

## 名称折叠与生成

未知类型的规范身份按方言和每一段引号标志进行名称折叠。PG 中 app.MyType 与 "app"."mytype" 可归于同一符号写法，app."MyType" 仍不同；不会因为原始文本同为 MyType 就把不同引号语义合并。

生成类型名称时，对未加引号的 PG / Oracle 名称先按方言折叠再引用，保留大小写含义；float4 / float8 保持原生语法。只增加名称折叠公共帮助函数，不将本地比较规则冒充数据库排序规则或完整类型解析。

该修补仍缺完整用户类型 / 域 / 搜索路径绑定、所有重载和 coercion。未知类型会保留未知，不能据名称相似推导原生域。

## 验证

针对性原生 PG、类型和转换回归已通过（7.77 秒）；最终全量测试 / ASAN / 跨平台包与安装记录在下方补充。安装时继续保存并核对工作区草稿，测试只使用项目临时数据库。

既有功能和边界见 [0.50](release-0.50.md)、[0.49](release-0.49.md)、[0.48](release-0.48.md)、[Calcite 差距清单](calcite-port.md)。

最终验证：完整 13 组 CTest 通过（44.10 秒）；ASAN 七组通过（13.22 秒），五项 GUI 场景连同初始化 / 清理七项通过（2.225 秒）。独立 SQLite 的 48 组帧 × 五种聚合、三个预设共 144 次执行仍保持生成 / 格式化结果一致与幂等（1.42 秒）。新增不合法原生类型参数也已在临时 PG 拒绝并与本地诊断对照。

已正常保存并退出 0.50，再将 0.51 安装到 `/Applications/Navidog.app` 并启动。15 个标签（包括两个对象定义草稿）恢复；退出后与启动后私人备份的逐项 SQL / 完整状态指纹相同。GUI 显示 8 个连接配置、0 已打开连接和 0 个会话，运行进程仅一个。安装文件签名校验通过，SHA-256：`9100e9f37be150835810ed03070e5b1e94d3dc5823f8c559f2ad0b978cd34dbb`。

已校验最新 ZIP 内容和摘要，清理 0.50 的旧 Win7 包 / 校验文件、回滚 App 和 macOS / Win7 暂存；源码、构建及私人工作区备份保留。Win7 SQLite-only 包十个 PE 依赖审计通过，SHA-256：`9b78bfa034317b18cf8a614adfef82d9b85697101bbf4dfbe70e9be1d9b921e0`。本轮未做 Win7 运行验收，也未连接实际 Oracle / SQL Server 进行新增功能验证。

独立核心构建复核：以 `NAVIDOG_COMPILER_ONLY=ON` 新建 `.cache/compiler-only51`，从头构建 C++17 编译库与 CLI，六组前端 / 类型 / 补全 / 转换 / 优化 / 关系元数据测试全过（0.35 秒）。otool 核对 CLI 只动态依赖 libc++ 与 libSystem，不包含 Qt 或数据库客户端；桌面应用仍使用 Qt 和原生驱动。
