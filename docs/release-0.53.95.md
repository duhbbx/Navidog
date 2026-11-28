# Navidog 0.53.95

- 「帮助 → 查看运行耗时日志」记录达到 100 毫秒的数据库操作、本地 SQL 工具、ER 导入、布局和导出等任务，并标明界面线程或后台线程。
- 应用在前台时单独记录界面事件循环延迟。日志在后台写入本地，保留最近 512 条。内容仅包括时间、线程类别、操作类型和耗时。

## English

- Help → View Operation Timings records operations taking at least 100 ms, including database work, local SQL tools, and ER import, layout, and export. Each record identifies the UI thread or a worker thread.
- The app logs UI event-loop delays separately while active. A background thread saves the latest 512 entries locally with the timestamp, thread category, operation type, and duration.
