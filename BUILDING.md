# 构建 Navidog

桌面版要求 CMake 3.24 或以上、支持 C++20 的编译器、Qt 6.4 或以上（含 Core5Compat）及 SQLite 3 开发库。Windows 7 / 8 兼容版使用 Qt 5.15.2、MinGW 8.1 x64 和 C++17。

| 目标 | 绿色版 | 安装版 |
| --- | --- | --- |
| macOS Arm、Intel、Universal | ZIP 内为 `Navidog.app` | PKG |
| Windows 7 SP1、8、10、11 x64 | ZIP 内为 `navidog.exe` 和运行库 | Setup EXE |
| Ubuntu 24.04、Debian 13 x64 | TAR.GZ 内运行 `Navidog` | DEB |
| Arch Linux x64 | TAR.GZ 内运行 `Navidog` | `.pkg.tar.zst` |
| 统信 UOS、麒麟 | 在目标发行版打包的 TAR.GZ | 目标发行版的 DEB 或 RPM |
| HarmonyOS GUI | — | Qt 技术预览版生成的 HAP |
| HarmonyOS SQL 工具 | NDK 原生命令行工具 TAR.GZ | — |

从 GitHub 的 **Actions → Desktop builds → Artifacts** 下载桌面包。统信 UOS、麒麟和鸿蒙使用 **Vendor SDK builds**。Windows 和 macOS 绿色版解压后即可启动。Linux 绿色版随附 Qt 和所需的非系统动态库，仍需要同类发行版的系统加载器、glibc 和图形环境。

macOS 构建产物使用临时签名。正式分发时可用 `NAVIDOG_PKG_SIGN_IDENTITY` 指定 Installer 签名身份，并对应用和安装包签名及公证。Windows 10 的 Qt 6 构建以 1809 或以上为目标。Windows 7 / 8 的 PE 审计检查子系统版本和依赖。旧版 Windows 运行需在对应系统上验收。

CI 桌面包内含 SQLite。Linux 构建在依赖可用时同时编译 PostgreSQL、MySQL / MariaDB、Oracle 与 SQL Server 适配器。MySQL 和 MariaDB 连接使用带异步 API 的官方 MySQL 客户端。发行版的 MariaDB Connector/C 兼容包会被跳过，可使用 `NAVIDOG_MYSQL_CLIENT_ROOT` 提供官方 SDK。CI 的 macOS 包启用 Oracle 适配器，CI 的 Windows 10 / 11 包启用 Oracle 与 SQL Server 适配器。CI 的 macOS Universal 包禁用单架构 Homebrew 提供的 SSH 客户端。使用 Oracle 或 SQL Server 仍需在目标机安装对应厂商的客户端。自行构建时可启用更多驱动，并提供与目标架构匹配的客户端 SDK。

## JDBC 和达梦 DM8

在受支持的桌面构建机上，将 `JAVA_HOME` 指向 OpenJDK 25 LTS 的 JDK 目录，并配置 `-DNAVIDOG_WITH_JDBC=ON`。安装包包含 JDBC 桥接 JAR，Java 运行时通过应用的「驱动与运行时」按需获取。macOS Universal 使用当前 CPU 架构的运行时包。Windows 7 / 8、鸿蒙和交叉编译构建关闭 JDBC 桥接。

新建达梦连接时，可下载托管驱动，也可在「高级」选择厂商提供的本地 JAR。首次连接时启动辅助进程，最后一个 JDBC 会话关闭后退出。SQL 查询结果按现有 Navidog 的行数和内存限额读取。

达梦 SSL 页使用包含 `.keystore` 的厂商证书目录和对应密钥库密码。数据库服务器需启用带证书认证的加密模式，`ENABLE_ENCRYPT` 取 `1` 或 `5`。同一连接可以启用 SSH 隧道。

## macOS 本机构建

安装 Xcode 命令行工具、CMake、Ninja 和 Qt。按目标选择 `arm64`、`x86_64` 或 `arm64;x86_64`。Universal 需要所有已启用的预编译依赖同时含两种架构。

```sh
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_PREFIX_PATH=/path/to/Qt/macos \
  -DCMAKE_OSX_ARCHITECTURES='arm64;x86_64'
PATH="/path/to/Qt/macos/bin:$PATH" ./scripts/package-macos.sh
```

## Windows 7 / 8

将 `NAVIDOG_QT_ROOT` 设置为 Qt 5.15.2 的 `mingw81_64`，`NAVIDOG_MINGW_ROOT` 设置为 Qt MinGW 8.1 工具根目录，`NAVIDOG_SQLITE_SOURCE` 设置为 SQLite amalgamation 解压目录。

```bat
scripts\build-windows7.cmd 7
scripts\build-windows7.cmd 8
```

已设置 Qt 5、MinGW 和 SQLite 交叉编译 SDK，并安装 NSIS 的 Mac 可以执行 `./scripts/build-win7-macos.sh 7` 或 `./scripts/build-win7-macos.sh 8`。Windows 10 / 11 可用 Visual Studio 2022 的 x64 生成器，参照 Desktop builds 的 Qt 部署步骤打包。

## Linux 桌面

在目标发行版预装 C++20 编译器、CMake、Ninja、Python 3、Qt 6 Widgets / Network / Test / Core5Compat、SQLite 和 OpenSSL 的开发包。需要联网构建 Oracle 适配器时，CMake 会下载经校验的 ODPI-C 源码。离线或不需要 Oracle 时，可传 `-DNAVIDOG_WITH_ORACLE=OFF`。

```sh
./scripts/build-linux.sh ubuntu
./scripts/build-linux.sh debian
./scripts/build-linux.sh archlinux
./scripts/build-linux.sh uos -DCMAKE_PREFIX_PATH=/path/to/Qt
./scripts/build-linux.sh kylin -DCMAKE_PREFIX_PATH=/path/to/Qt
```

每条命令都在对应发行版上运行。生成 DEB 包需要安装 `dpkg-dev` 和 `file`。UOS 与麒麟的产物绑定构建机的架构、系统库和 Qt ABI。用 GitHub Actions 时，注册带 `uos` 或 `kylin` 标签的 Linux 自托管 Runner，并预装上述工具及本发行版的 DEB 或 RPM 打包工具。

## HarmonyOS

在安装官方 SDK 的工作站上选择图形或命令行构建。GitHub Actions 使用带 `harmonyos` 标签的 Linux 自托管 Runner，并在 Runner 服务环境中设置所需变量。

图形构建使用 [Qt for HarmonyOS 6.12 技术预览](https://doc.qt.io/qt-6/harmonyos.html) 和 HarmonyOS 6.1（API 23）ARM64 SDK。Qt 目标套件需要包含 Widgets、Network 和 Core5Compat。使用目标架构的静态 OpenSSL 和 SQLite amalgamation：

```sh
export OHOS_SDK_ROOT=/path/to/sdk
export NAVIDOG_OHOS_QT_ROOT=/path/to/qt-harmonyos
export NAVIDOG_OHOS_OPENSSL_ROOT=/path/to/openssl-harmonyos
export NAVIDOG_SQLITE_SOURCE=/path/to/sqlite-amalgamation
./scripts/build-harmonyos.sh --gui
```

HAP 要安装到真机时，按 [Qt HAP 签名说明](https://doc.qt.io/qt-6/harmonyos-deploy-qt-tool.html#signing-the-hap) 在本机配置签名。构建并签名后需在 HarmonyOS 设备上验收。CLI 构建只需要官方 NDK：

```sh
export NAVIDOG_OHOS_NATIVE=/path/to/harmonyos/sdk/native
./scripts/build-harmonyos.sh arm64-v8a
```

构建产物为 `navidog-sql`，提供独立的 SQL 解析、转换和分析能力。


## Optional driver and Java packages

JDK 25 builds the JDBC bridge JAR and a platform-specific `jlink` image. The application distributes the bridge JAR. For native builds, `scripts/package-driver-components.py` creates separate runtime and hiredis archives with SHA-256 sidecars. The desktop manager reads the exact version's GitHub Release asset digest before installing. Modern macOS ARM and Intel, Windows x64 and Linux x64 releases provide the optional archives. macOS Universal uses the active CPU slice's package.

`NAVIDOG_BUNDLE_HIREDIS=ON` builds the separate Redis shared library from upstream hiredis 1.3.0. Local builds default to a system hiredis when found. For JDBC development, `NAVIDOG_JAVA` can point to an absolute Java 25 executable. In the local build tree the bridge also uses the generated runtime.

The OpenJDK runtime retains its module license notices. See [OpenJDK 25 update sources](https://github.com/openjdk/jdk25u) and [Azul Zulu license information](https://docs.azul.com/core/tpls/). The Redis asset includes upstream hiredis's COPYING file. The proprietary JDBC binaries are downloaded directly from the applicable vendor distribution and stay outside the repository.
