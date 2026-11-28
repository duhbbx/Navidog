# Navidog 0.54.24

- Fixed a Windows SDK macro conflict when building the independently downloadable Redis driver adapter.
- Enabled C++ exception unwinding in modern Windows CI and prevented the macOS Universal build from linking single-architecture Homebrew database clients.
- Java 25 and hiredis components installed by 0.54.23 remain usable without downloading them again.
- See [0.54.23](release-0.54.23.md) for the optional driver and runtime downloads.
