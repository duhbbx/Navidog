# Navidog 0.54.37

- Parse and format MySQL locking reads, including FOR UPDATE, FOR SHARE, OF targets, NOWAIT, SKIP LOCKED, and LOCK IN SHARE MODE. The AST and SQL model retain the locking clauses. Conversion and offline optimization reject locking semantics they cannot preserve.
- Remove SQL text padding in queries, routine editors, and read-only previews. Line numbers remain beside editable SQL.
