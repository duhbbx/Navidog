# Navidog 0.54.20

- Added a shared JDBC bridge that starts when its first connection opens. It keeps JDBC sessions independent and bounds cached result data, including text and binary cells.
- Added Dameng DM8 connections with schema browsing, table and view previews, routine definitions, SQL execution, transactions, and native execution plans using `EXPLAIN FOR`.
- JDBC-enabled packages bundle a trimmed OpenJDK 25 LTS runtime. Choose the official Dameng JDBC JAR under **Connection → Advanced**. Native TLS and Dameng-specific SQL compiler and object designer coverage are pending.
