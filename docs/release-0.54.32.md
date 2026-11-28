# Navidog 0.54.32

- Create views, functions, and procedures from their navigation folders or from the **New object** menu on a supported database or schema. PostgreSQL and Oracle also expose materialized-view folders and creation templates.
- **Properties** provides object identity, database, schema, directory attributes, and a separate read-only SQL tab with formatting and copying. Routine properties include arguments and available server metadata.
- Object deletion shows the SQL and requires the full object name. PostgreSQL rechecks routine identity and its overload signature before executing the confirmed statement. Cascade actions are exposed where the dialect supports them, with a separate confirmation for dependent objects.
- Oracle can compile stored functions, procedures, triggers, views, and materialized views from the navigation menu. Navidog reads the resulting validity status and reports server diagnostics.
- Structural changes refresh the affected navigation catalogs. Object editors with local drafts retain their contents.
- Shift selects a range within a navigation folder. Command on macOS and Control on Windows and Linux toggle individual objects in that folder. Right-click a selection to generate or export SQL and delete the selected objects in one operation.
- Rename supported objects with a SQL preview. Availability follows the database and object type. PostgreSQL can rename a specific function or procedure overload.
