# Navidog 0.54.31

- Running SQL with input placeholders opens a parameter dialog that shows each name, its references in the submitted SQL, editable input types, and values including explicit SQL NULL. Cancel closes the dialog before any statement runs.
- The preflight understands MySQL and SQL Server positional `?` markers, PostgreSQL `$n` markers, Oracle `:name` markers, and SQLite positional, numbered, and named markers. JDBC connections use the backend's input syntax and dispatch values through prepared statements. DuckDB supports its positional and named input markers.
- Types are suggested from explicit casts, comparison and assignment contexts, pagination, and available column metadata. Choose an input type when the context leaves it unresolved. Bind values are sent separately from SQL through the driver.
- The same binding flow is available for the current statement, selected SQL, batches, and estimated plans where the driver supports them. Oracle PL/SQL input binds are supported, with output parameters outside this release.
