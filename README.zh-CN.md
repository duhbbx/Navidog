# Navidog

<img src="resources/branding/navidog.png" alt="Navidog 应用图标" width="112" height="112">

[English](README.md) · [路线图](ROADMAP.md#中文路线图) · [构建指南](BUILDING.md) · [Apache 2.0 许可证](LICENSE)

Navidog 是使用 C++ 和 Qt Widgets 开发的原生数据库工作台。可以在一个桌面应用里管理数据库连接、编写和执行 SQL、查看执行计划，以及设计关系数据库结构。

## 功能

- **连接与对象管理** — 连接分组，浏览数据库、Schema、表、视图、索引、函数和存储过程。按驱动和构建版本支持 TLS、SSH 和 HTTP 隧道。
- **SQL 工作台** — 独立查询标签页提供补全、格式化、错误提示、事务控制和结果导出。支持使用本地元数据提供对象补全和检查。
- **结构与数据编辑** — 编辑表结构和行数据，预览生成的 DDL，查看和编辑受支持的视图及例程。具体操作随数据库类型而异。
- **SQL 分析与转换** — 查看语法树，在支持的方言之间转换 SQL，审阅转换明细，并对照本地优化建议和数据库返回的执行计划。
- **ER 建模** — 选择 Schema 或表进行逆向建模，编辑关系，生成建库或差异 SQL。支持 PNG、Markdown、Word 和 PDF，Word 与 PDF 支持目录。

## 支持的数据库

| 数据库 | 连接驱动 | 说明 |
| --- | --- | --- |
| SQLite | SQLite C API | 所有桌面构建内置，可直接打开本地数据库文件 |
| H2 | H2 官方 JDBC | 支持打开已有 MVStore 文件或 TCP 连接，浏览 Schema、执行查询和基本表设计 |
| 金仓、优炫、GaussDB、Db2 LUW | 厂商 JDBC | 支持浏览 Schema、执行查询、基本表设计和基于元数据的 SQL 检查与补全 |
| PolarDB MySQL / PostgreSQL | `libmysqlclient` / `libpq` | 原生连接，沿用相应的兼容 SQL 工具 |
| Redis | `hiredis` | 独立实例连接、命令控制台和键值浏览 |
| MySQL | MySQL 官方 `libmysqlclient` | 支持 MySQL 连接和编辑器方言功能，认证与 TLS 能力取决于客户端版本 |
| PostgreSQL | `libpq` | 支持数据库和 Schema 浏览、对象与数据编辑以及服务端执行计划 |
| MariaDB | MySQL `libmysqlclient` | 独立连接类型和 SQL 方言，使用 MySQL 兼容协议 |
| TiDB | MySQL `libmysqlclient` | 独立 TiDB 方言，使用 MySQL 兼容协议 |
| OceanBase | MySQL `libmysqlclient` 或 OceanBase Connector/C | 支持选择 MySQL 或 Oracle 租户模式，Oracle 模式使用 OceanBase 客户端 SDK |
| Oracle Database | ODPI-C / OCI | 目标机需安装相应架构的 Oracle Instant Client |
| 达梦 DM8 | 达梦官方 JDBC | 支持 Schema 浏览、查询、事务和执行计划。使用含 JDBC 的构建，并选择达梦官方驱动 JAR。 |
| Microsoft SQL Server | Microsoft ODBC Driver 18 或 17 | 目标机需安装微软驱动，应用需启用 ODBC 适配器 |

在 **工具 → 数据库驱动状态** 中可以查看当前构建内置的适配器。Windows 7 和 8 兼容包内置 SQLite。具体功能取决于构建选项、数据库版本、权限和方言覆盖范围。

## 下载与构建

Windows、macOS 和受支持的 Linux 桌面目标提供绿色版和安装版，可在 [GitHub Releases](https://github.com/duhbbx/Navidog/releases) 下载。构建记录见 [GitHub Actions](https://github.com/duhbbx/Navidog/actions/workflows/build.yml)，各平台依赖和统信 UOS、麒麟、鸿蒙的构建入口见 [构建指南](BUILDING.md)。

桌面版需要 CMake 3.24+、C++20 编译器、Qt 6.4+（含 Core5Compat）和 SQLite 开发库。

```sh
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build --parallel
QT_QPA_PLATFORM=offscreen ctest --test-dir build --output-on-failure
```

可使用 `-DCMAKE_PREFIX_PATH=/path/to/Qt` 指定 Qt。只构建独立 SQL 工具时，在单独的构建目录中使用 `-DNAVIDOG_COMPILER_ONLY=ON`。

[0.54.40 更新说明](docs/release-0.54.40.md) · [0.54.39](docs/release-0.54.39.md) · [0.54.38](docs/release-0.54.38.md) · [0.54.37](docs/release-0.54.37.md) · [0.54.36](docs/release-0.54.36.md) · [0.54.35](docs/release-0.54.35.md) · [0.54.34](docs/release-0.54.34.md) · [0.54.33](docs/release-0.54.33.md) · [0.54.32](docs/release-0.54.32.md) · [0.54.31](docs/release-0.54.31.md) · [0.54.30](docs/release-0.54.30.md) · [0.54.29](docs/release-0.54.29.md) · [0.54.28](docs/release-0.54.28.md) · [0.54.27](docs/release-0.54.27.md) · [0.54.26](docs/release-0.54.26.md) · [0.54.25](docs/release-0.54.25.md) · [0.54.24](docs/release-0.54.24.md) · [0.54.23](docs/release-0.54.23.md) · [0.54.22](docs/release-0.54.22.md) · [0.54.21](docs/release-0.54.21.md) · [0.54.20](docs/release-0.54.20.md) · [0.54.19](docs/release-0.54.19.md) · [0.54.18](docs/release-0.54.18.md) · [0.54.17](docs/release-0.54.17.md) · [0.54.16](docs/release-0.54.16.md) · [0.54.15](docs/release-0.54.15.md) · [0.54.14](docs/release-0.54.14.md) · [0.54.13](docs/release-0.54.13.md) · [0.54.12](docs/release-0.54.12.md) · [0.53.111](docs/release-0.53.111.md) · [0.53.110](docs/release-0.53.110.md) · [0.53.109](docs/release-0.53.109.md) · [0.53.108](docs/release-0.53.108.md) · [0.53.107](docs/release-0.53.107.md) · [0.53.106](docs/release-0.53.106.md) · [0.53.105](docs/release-0.53.105.md) · [0.53.104](docs/release-0.53.104.md) · [0.53.103](docs/release-0.53.103.md) · [0.53.102](docs/release-0.53.102.md) · [0.53.101](docs/release-0.53.101.md) · [0.53.100](docs/release-0.53.100.md) · [0.53.99](docs/release-0.53.99.md) · [0.53.98](docs/release-0.53.98.md) · [0.53.97](docs/release-0.53.97.md) · [0.53.96](docs/release-0.53.96.md) · [0.53.95](docs/release-0.53.95.md) · [0.53.94](docs/release-0.53.94.md) · [0.53.93](docs/release-0.53.93.md) · [0.53.92](docs/release-0.53.92.md) · [0.53.91](docs/release-0.53.91.md) · [0.53.90](docs/release-0.53.90.md) · [0.53.89](docs/release-0.53.89.md) · [0.53.88](docs/release-0.53.88.md) · [0.53.87](docs/release-0.53.87.md) · [0.53.86](docs/release-0.53.86.md) · [0.53.85](docs/release-0.53.85.md) · [0.53.84](docs/release-0.53.84.md) · [0.53.83](docs/release-0.53.83.md) · [0.53.82](docs/release-0.53.82.md) · [0.53.81](docs/release-0.53.81.md) · [0.53.80](docs/release-0.53.80.md) · [0.53.79](docs/release-0.53.79.md)
