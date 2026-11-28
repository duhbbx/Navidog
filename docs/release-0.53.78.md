# Navidog 0.53.78

- Windows 构建同时生成可解压运行的 ZIP 和支持卸载的 Setup EXE。安装器在 CI 中进行静默安装、启动和卸载验证。
- macOS Arm、Intel 和 Universal 构建同时生成包含应用的 ZIP 和安装到「应用程序」的 PKG。
- Linux 构建同时生成包含运行依赖的 TAR.GZ 和发行版安装包。Ubuntu / Debian 使用 DEB，Arch Linux 使用 pacman 安装包，统信 UOS / 麒麟根据构建系统选择 DEB 或 RPM。
- GitHub 仓库公开，Navidog 源码采用 MIT 许可证。
