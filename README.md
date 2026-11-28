# Navidog

<img src="resources/branding/navidog.png" alt="Navidog app icon" width="112" height="112">

[简体中文](README.zh-CN.md) · [Roadmap](ROADMAP.md) · [Build guide](BUILDING.md) · [Apache 2.0](LICENSE)

Navidog is a native database workbench built with C++ and Qt Widgets. Manage connections, edit and run SQL, inspect execution plans, and design relational schemas in one desktop app.

## Features

- **Connections and objects** — Group connections, browse databases and schemas, and open tables, views, indexes, functions, and procedures. Connection options include TLS, SSH and HTTP tunneling when supported by the selected driver and build.
- **SQL workspace** — Work in independent query tabs with completion, formatting, diagnostics, transaction controls, and result export. Local metadata improves completion and object checks in supported database contexts.
- **Schema and data editing** — Edit table structure and rows, preview generated DDL, and work with views and routines. Editing capabilities vary by database.
- **SQL analysis and conversion** — Inspect the syntax tree, convert SQL between supported dialects, review conversion changes, and compare local optimization suggestions with a plan returned by the server.
- **ER modeling** — Reverse-engineer selected schemas or tables, edit relationships, generate CREATE or migration SQL, and export PNG, Markdown, Word, or PDF. Word and PDF include a table of contents.

## Supported databases

| Database | Connection driver | Notes |
| --- | --- | --- |
| SQLite | SQLite C API | Included in all desktop builds. Opens local database files. |
| H2 | Official H2 JDBC | Existing local MVStore files or TCP, schema browsing, query execution and basic table design. |
| KingbaseES, UXDB, GaussDB, Db2 LUW | Vendor JDBC | Schema browsing, query execution, basic table design and metadata-aware SQL editing. |
| PolarDB (MySQL / PostgreSQL) | `libmysqlclient` / `libpq` | Native connections and the respective compatible SQL tooling. |
| Redis | `hiredis` | Standalone connections, command console and key/value browsing. |
| MySQL | MySQL `libmysqlclient` | Supports MySQL connections and editor dialect features. Authentication and TLS options depend on the client version. |
| PostgreSQL | `libpq` | Browse databases and schemas, edit supported objects and data, and inspect server plans. |
| MariaDB | MySQL `libmysqlclient` | MariaDB connection type and SQL dialect use the MySQL-compatible protocol. |
| TiDB | MySQL `libmysqlclient` | Dedicated TiDB dialect with MySQL-compatible connections. |
| OceanBase | MySQL `libmysqlclient` or OceanBase Connector/C | Choose MySQL or Oracle tenant mode. Oracle mode requires the OceanBase client SDK. |
| Oracle Database | ODPI-C / OCI | Requires a compatible Oracle Instant Client on the target machine. |
| Dameng DM8 | Official Dameng JDBC | Requires a JDBC-enabled build and an official driver JAR. Includes schema browsing, queries, transactions and estimated plans. |
| Microsoft SQL Server | Microsoft ODBC Driver 18 or 17 | Requires the Microsoft driver on the target machine and an ODBC-enabled build. |

Check **Tools → Database Driver Status** in Navidog to see which adapters a particular build contains. The Windows 7 and 8 compatibility packages include SQLite. Driver availability, server permissions, and SQL dialect coverage determine which operations are available.

## Downloads and development

Desktop downloads are available in [GitHub Releases](https://github.com/duhbbx/Navidog/releases). Builds provide portable archives and installers for Windows, macOS, and supported Linux distributions. See [GitHub Actions](https://github.com/duhbbx/Navidog/actions/workflows/build.yml) for build artifacts and the [build guide](BUILDING.md) for the platform matrix, driver requirements, and vendor SDK targets.

Build the desktop app with CMake 3.24+, a C++20 compiler, Qt 6.4+ with Core5Compat, and SQLite development libraries.

```sh
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build --parallel
QT_QPA_PLATFORM=offscreen ctest --test-dir build --output-on-failure
```

Use `-DCMAKE_PREFIX_PATH=/path/to/Qt` for a custom Qt installation. To build only the standalone SQL tool, configure a separate build directory with `-DNAVIDOG_COMPILER_ONLY=ON` and run `build/bin/navidog-sql` from that build.

[Latest changes in 0.54.40](docs/release-0.54.40.md) · [0.54.39](docs/release-0.54.39.md) · [0.54.38](docs/release-0.54.38.md) · [0.54.37](docs/release-0.54.37.md) · [0.54.36](docs/release-0.54.36.md) · [0.54.35](docs/release-0.54.35.md) · [0.54.34](docs/release-0.54.34.md) · [0.54.33](docs/release-0.54.33.md) · [0.54.32](docs/release-0.54.32.md) · [0.54.31](docs/release-0.54.31.md) · [0.54.30](docs/release-0.54.30.md) · [0.54.29](docs/release-0.54.29.md) · [0.54.28](docs/release-0.54.28.md) · [0.54.27](docs/release-0.54.27.md) · [0.54.26](docs/release-0.54.26.md) · [0.54.25](docs/release-0.54.25.md) · [0.54.24](docs/release-0.54.24.md) · [0.54.23](docs/release-0.54.23.md) · [0.54.22](docs/release-0.54.22.md) · [0.54.21](docs/release-0.54.21.md) · [0.54.20](docs/release-0.54.20.md) · [0.54.19](docs/release-0.54.19.md) · [0.54.18](docs/release-0.54.18.md) · [0.54.17](docs/release-0.54.17.md) · [0.54.16](docs/release-0.54.16.md) · [0.54.15](docs/release-0.54.15.md) · [0.54.14](docs/release-0.54.14.md) · [0.54.13](docs/release-0.54.13.md) · [0.54.12](docs/release-0.54.12.md) · [0.53.111](docs/release-0.53.111.md) · [0.53.110](docs/release-0.53.110.md) · [0.53.109](docs/release-0.53.109.md) · [0.53.108](docs/release-0.53.108.md) · [0.53.107](docs/release-0.53.107.md) · [0.53.106](docs/release-0.53.106.md) · [0.53.105](docs/release-0.53.105.md) · [0.53.104](docs/release-0.53.104.md) · [0.53.103](docs/release-0.53.103.md) · [0.53.102](docs/release-0.53.102.md) · [0.53.101](docs/release-0.53.101.md) · [0.53.100](docs/release-0.53.100.md) · [0.53.99](docs/release-0.53.99.md) · [0.53.98](docs/release-0.53.98.md) · [0.53.97](docs/release-0.53.97.md) · [0.53.96](docs/release-0.53.96.md) · [0.53.95](docs/release-0.53.95.md) · [0.53.94](docs/release-0.53.94.md) · [0.53.93](docs/release-0.53.93.md) · [0.53.92](docs/release-0.53.92.md) · [0.53.91](docs/release-0.53.91.md) · [0.53.90](docs/release-0.53.90.md) · [0.53.89](docs/release-0.53.89.md) · [0.53.88](docs/release-0.53.88.md) · [0.53.87](docs/release-0.53.87.md) · [0.53.86](docs/release-0.53.86.md) · [0.53.85](docs/release-0.53.85.md) · [0.53.84](docs/release-0.53.84.md) · [0.53.83](docs/release-0.53.83.md) · [0.53.82](docs/release-0.53.82.md) · [0.53.81](docs/release-0.53.81.md) · [0.53.80](docs/release-0.53.80.md) · [0.53.79](docs/release-0.53.79.md)
