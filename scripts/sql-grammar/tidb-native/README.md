# Pinned TiDB parser probe

This optional command recognizes SQL with the public TiDB parser module at the
same commit as `tests/sql/corpus/reference-sources.json`. It does not start or
connect to a database. The upstream `test_driver` package supplies literal AST
nodes; it is not a network driver. Upstream test programs are not executed.

Build with Go 1.27.3 or newer, keeping module/build caches outside shipped code.
Run `tidb-native-probe --version` to verify the embedded module identity. The
normal protocol is one JSON request per line (`index`, `sql`, optional `mode`)
and one JSON recognition result per line. Successful recognition is not proof
that TiDB can bind or execute a statement.
