# Navidog 0.53.82

- 「连接」「新建查询」「表」「视图」「函数」「存储过程」打开对应操作。「查询」打开按连接、数据库、Schema 保存的查询库，可保存、导入、打开、重命名和移除 SQL 文件。
- 「用户」读取 MySQL、MariaDB、PostgreSQL、Oracle 和 SQL Server 的账号或角色，可新建角色，Oracle 可新建用户 / Schema。「事件」读取 MySQL、MariaDB 的数据库事件和 Oracle 调度任务，支持创建、查看和编辑 MySQL、MariaDB 事件定义，以及启用、停用和删除。可见范围与可执行操作取决于版本和权限。
- 「备份」支持 SQLite 快照、MySQL SQL 备份和 PostgreSQL 归档。SQLite 可恢复为新文件，PostgreSQL 可在单事务中恢复到空数据库。「自动运行」可保存一项 SQLite 定时备份，在 Navidog 运行期间执行。
- 「模型」支持新建、打开和逆向导入 ER 模型。「BI」可从当前缓存结果生成条形图并导出 PNG，也可对所选表按字段分组。

## English

- Added a saved-query library scoped by connection, database, and schema, with SQL import, opening, rename, and removal.
- Added account and role listings, role creation, and Oracle user creation. MySQL and MariaDB event lists support creation, definition inspection and editing, enable, disable, and drop. Oracle scheduler jobs are viewable.
- Added SQLite snapshots, MySQL SQL dumps, and PostgreSQL archives. SQLite restores to a new file, and PostgreSQL restores to an empty database in one transaction. SQLite backups can run on a schedule while Navidog is open.
- Connected ER modeling actions and BI charts to the main toolbar. Charts can use cached results and export PNG.
