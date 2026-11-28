# Navidog 0.53.104

- 在「首选项 → 常规」配置导出与下载的默认保存目录。默认使用系统下载目录，可随时恢复默认值。
- 导出默认文件名包含连接、数据库、Schema、用途和本地时间。缺失的数据库或 Schema 会省略，后缀随导出格式选择。
- 查询结果使用执行时的连接上下文，ER 文档和分组 PNG 使用统一的命名格式。

## English

- Set the default export and download folder in General preferences. The system Downloads folder is the default.
- Suggested export names use the connection, database, schema, purpose, and local timestamp. Missing scopes are omitted, and the extension follows the export format.
- Query results keep the connection context used to execute them. ER documents and group PNGs follow the same naming pattern.
