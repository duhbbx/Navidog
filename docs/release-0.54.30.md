# Navidog 0.54.30

- Open **Tasks** in the lower-right corner to view queued and active jobs, check progress, cancel work, and open completed exports or their folders. Task history lasts for the current app session.
- Closing an active table import, export, or generation dialog sends it to the background. Reopen the same dialog from **Tasks** to review its inputs and progress. Data dictionary export also preserves its original wizard while running.
- CSV import and export, SQL dumps, query-result exports, database backups and restores, and ER and data dictionary exports use the shared background queue. Jobs report rows, bytes, pages, or items when available.
- File exports stream output and commit only after a successful write. The queue prevents active jobs from writing to the same destination and serializes work that shares a database resource.
