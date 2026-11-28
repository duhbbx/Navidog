#!/usr/bin/env python3
"""Run a small offline coverage audit against Navidog's compiler CLI.

Statement rows exercise the SQL parser and validator. Routine rows exercise
the procedural parser, validator, and same-dialect generator together.
The SQL is authored here. No ANTLR runtime or database connection is needed.
"""
import argparse
import json
from pathlib import Path
import subprocess


# Reference vocabulary: grammars-v4, commit de38599628d221099c046e92962f70204d852b54.
# These examples are deliberate probes, not a generated grammar test suite.
CASES = [
    ("mysql", "SHOW TABLES", "SHOW FULL TABLES;", False),
    ("mysql", "window function", "SELECT ROW_NUMBER() OVER (ORDER BY id) FROM t;", False),
    ("mysql", "partitioned table",
     "CREATE TABLE t(id INT) PARTITION BY HASH(id) PARTITIONS 4;", False),
    ("mysql", "trigger",
     "CREATE TRIGGER tr BEFORE INSERT ON t FOR EACH ROW SET NEW.id = 1;", False),
    ("mysql", "routine WHILE",
     "CREATE PROCEDURE p() BEGIN DECLARE n INT DEFAULT 0; "
     "WHILE n < 2 DO SET n = n + 1; END WHILE; END;", True),
    ("mysql", "routine handler",
     "CREATE PROCEDURE p() BEGIN DECLARE CONTINUE HANDLER FOR SQLEXCEPTION SET @x = 1; "
     "SELECT 1; END;", True),
    ("mysql", "routine REPEAT",
     "CREATE PROCEDURE p() BEGIN DECLARE n INT DEFAULT 0; REPEAT SET n = n + 1; "
     "UNTIL n = 2 END REPEAT; END;", True),
    ("mysql", "routine SELECT INTO",
     "CREATE PROCEDURE p() BEGIN DECLARE v INT; SELECT 1 INTO v; END;", True),
    ("mysql", "routine DEFINER",
     "CREATE DEFINER=`owner`@`localhost` FUNCTION f(p INT) RETURNS INT "
     "DETERMINISTIC BEGIN RETURN p + 1; END;", True),

    ("postgresql", "RETURNING", "INSERT INTO t(id) VALUES (1) RETURNING id;", False),
    ("postgresql", "FETCH WITH TIES",
     "SELECT id FROM t ORDER BY id OFFSET 3 ROWS FETCH NEXT 2 ROWS WITH TIES;", False),
    ("postgresql", "window EXCLUDE",
     "SELECT COUNT(*) OVER (ROWS UNBOUNDED PRECEDING EXCLUDE CURRENT ROW) FROM t;", False),
    ("postgresql", "recursive CTE",
     "WITH RECURSIVE c(n) AS (SELECT 1 UNION ALL SELECT n + 1 FROM c WHERE n < 3) "
     "SELECT n FROM c;", False),
    ("postgresql", "CTE forward reference",
     "WITH RECURSIVE a AS (SELECT n FROM b), b AS (SELECT 7 AS n) SELECT n FROM a;", False),
    ("postgresql", "COPY", "COPY t FROM STDIN WITH (FORMAT csv);", False),
    ("postgresql", "VACUUM", "VACUUM (ANALYZE) t;", False),
    ("postgresql", "PL/pgSQL local declaration",
     "CREATE FUNCTION f(p integer) RETURNS integer LANGUAGE plpgsql "
     "AS $$ DECLARE v integer := p; BEGIN RETURN v; END; $$;", True),
    ("postgresql", "PL/pgSQL LOOP / EXIT WHEN",
     "CREATE FUNCTION f(p integer) RETURNS integer LANGUAGE plpgsql "
     "AS $$ BEGIN LOOP p:=p+1; EXIT WHEN p>2; END LOOP; RETURN p; END; $$;", True),
    ("postgresql", "PL/pgSQL numeric FOR",
     "CREATE PROCEDURE p() LANGUAGE plpgsql AS $$ BEGIN "
     "FOR i IN REVERSE 5..1 BY 2 LOOP NULL; END LOOP; END; $$;", True),
    ("postgresql", "PL/pgSQL SELECT INTO STRICT",
     "CREATE FUNCTION f() RETURNS INT LANGUAGE plpgsql AS $$ DECLARE v INT; "
     "BEGIN SELECT 1 INTO STRICT v; RETURN v; END; $$;", True),
    ("postgresql", "PL/pgSQL exception handler",
     "CREATE FUNCTION f() RETURNS integer LANGUAGE plpgsql "
     "AS $$ BEGIN RETURN 1; EXCEPTION WHEN OTHERS THEN RETURN 0; END; $$;", True),
    ("postgresql", "PL/pgSQL stacked diagnostics",
     "CREATE FUNCTION f() RETURNS text LANGUAGE plpgsql AS $$ "
     "DECLARE err text; BEGIN RETURN 'ok'; EXCEPTION WHEN OTHERS THEN "
     "GET STACKED DIAGNOSTICS err = MESSAGE_TEXT; RETURN err; END; $$;", True),

    ("oracle", "MERGE",
     "MERGE INTO t USING s ON (t.id = s.id) WHEN MATCHED THEN UPDATE SET t.n = s.n;", False),
    ("oracle", "FETCH WITH TIES",
     "SELECT id FROM t ORDER BY id FETCH FIRST 2 ROWS WITH TIES;", False),
    ("oracle", "PIVOT",
     "SELECT * FROM t PIVOT (SUM(amount) FOR state IN ('A' AS a));", False),
    ("oracle", "package specification",
     "CREATE PACKAGE pkg AS PROCEDURE p; END pkg;", False),
    ("oracle", "PL/SQL local declaration",
     "CREATE OR REPLACE FUNCTION f(p IN NUMBER) RETURN NUMBER AS "
     "v NUMBER := p; BEGIN RETURN v; END;", True),
    ("oracle", "PL/SQL SELECT INTO",
     "CREATE FUNCTION f RETURN NUMBER AS v NUMBER; BEGIN "
     "SELECT 1 INTO v FROM SYS.DUAL; RETURN v; END;", True),
    ("oracle", "PL/SQL numeric FOR",
     "CREATE PROCEDURE p AS BEGIN FOR i IN 1..3 LOOP NULL; END LOOP; END;", True),
    ("oracle", "PL/SQL exception handler",
     "CREATE PROCEDURE p AS BEGIN NULL; EXCEPTION WHEN OTHERS THEN NULL; END;", True),

    ("sqlserver", "OUTPUT", "INSERT INTO dbo.t(id) OUTPUT inserted.id VALUES (1);", False),
    ("sqlserver", "TOP WITH TIES", "SELECT TOP (2) WITH TIES id FROM dbo.t ORDER BY id;", False),
    ("sqlserver", "CROSS APPLY",
     "SELECT t.id FROM dbo.t AS t CROSS APPLY (SELECT t.id AS n) AS d;", False),
    ("sqlserver", "CROSS APPLY VALUES",
     "SELECT d.n FROM dbo.t AS t CROSS APPLY (VALUES (t.id), (t.id+1)) AS d(n);", False),
    ("sqlserver", "PIVOT",
     "SELECT * FROM dbo.t PIVOT (SUM(amount) FOR state IN ([A])) AS p;", False),
    ("sqlserver", "CREATE OR ALTER / NOCOUNT",
     "CREATE OR ALTER PROC dbo.p AS BEGIN SET NOCOUNT ON; "
     "DECLARE @n int = 1; SELECT @n; END;", True),
    ("sqlserver", "TRY/CATCH",
     "CREATE PROC dbo.p AS BEGIN BEGIN TRY SELECT 1; END TRY "
     "BEGIN CATCH THROW; END CATCH; END;", True),

    ("sqlite", "RETURNING", "INSERT INTO t(id) VALUES (1) RETURNING id;", False),
    ("sqlite", "PRAGMA", "PRAGMA table_info(t);", False),
    ("sqlite", "virtual table", "CREATE VIRTUAL TABLE t USING fts5(content);", False),
    ("sqlite", "trigger", "CREATE TRIGGER tr AFTER INSERT ON t BEGIN SELECT NEW.id; END;", False),
]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("compiler", nargs="?", default="build/bin/navidog-sql")
    args = parser.parse_args()
    compiler = str(Path(args.compiler).resolve())
    print("dialect\tfeature\tstage\tstatus\tdiagnostic")
    for dialect, feature, sql, routine in CASES:
        command = [compiler, "--dialect", dialect, "--ast"]
        if routine:
            command.extend(["--convert", dialect])
        run = subprocess.run([*command, "-"], input=sql, text=True,
                             capture_output=True, timeout=10, check=False)
        if run.returncode not in (0, 1):
            raise SystemExit(run.stderr)
        if routine:
            stage = "routine round trip"
            status = "accepted" if run.returncode == 0 else "gap"
        else:
            stage = "statement"
            result = json.loads(run.stdout)
            status = ("accepted" if result["ok"] else
                      "semantic gap" if result["syntaxValid"] else "syntax gap")
        diagnostic = next(iter(run.stderr.splitlines()), "")
        print(f"{dialect}\t{feature}\t{stage}\t{status}\t{diagnostic}")


if __name__ == "__main__":
    main()
