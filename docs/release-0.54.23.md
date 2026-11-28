# Navidog 0.54.23

- Open **Tools → Drivers and Runtime** or use the corresponding button in the connection editor to manage optional components. Connections use the managed JDBC JAR unless a custom vendor JAR is selected in Advanced.
- Download H2, Dameng, KingbaseES, Huawei GaussDB and IBM Db2 JDBC drivers from Maven Central with pinned SHA-256 checksums. UXDB uses the vendor's download page and a locally selected JAR.
- Download the Java 25 runtime and the Redis native client as separate, architecture-specific release assets on supported macOS, Linux and Windows builds. The installer validates the asset digest published by GitHub and installs the component in the current user's application data. No downloads are initiated at startup.
- Oracle Instant Client and SQL Server ODBC are available through their official vendor installers. Builds that link MySQL and PostgreSQL client libraries continue to use the clients provided at build time.
- The desktop build matrix retains macOS ARM, Intel and Universal, Windows 7 through 11, Ubuntu, Debian and Arch packages. UOS, Kylin and HarmonyOS use the existing dedicated SDK jobs. Windows 7 and 8 keep their legacy build settings.
