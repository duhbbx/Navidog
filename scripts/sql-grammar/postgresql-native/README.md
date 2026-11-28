# PostgreSQL raw parser probe

This optional offline tool uses the pinned pglast 8.9 wheel, which packages
libpg_query's PostgreSQL 18.6 raw parser. The adapter is GPL-3.0-or-later and
stays entirely in the ignored audit cache; it is not a Navidog dependency.
Both artifact/native-library hashes and source revisions are in the source
lock. The current reviewed artifact is CPython 3.15 on macOS ARM64; a different
Python/OS needs a separately reviewed matching wheel and lock entry.

From the repository root:

```sh
python3 scripts/collect-sql-native-parsers.py --engine postgresql
python3 -I -B scripts/sql-grammar/postgresql-native/probe.py --version
python3 scripts/audit-sql-postgresql-native.py --fixtures \
  --output .cache/postgresql-native-audit.jsonl
```

The probe verifies its installed Python and native files before importing.
JSON requests contain `index` and `sql`; responses report raw-parser acceptance
or its error. SQL is never executed. Raw parsing does not bind names, validate
server settings/types, or validate a routine body stored in a string literal.
