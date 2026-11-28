#!/usr/bin/env python3
"""Compare pinned, locally generated ANTLR grammars with SQL fixture expectations.

ANTLR is an auxiliary grammar reference, not a server-validity oracle. Results
are JSON for review; a discrepancy is never automatically relabeled or blessed.
"""
from __future__ import annotations

import argparse
import base64
from collections import Counter
import hashlib
import json
from pathlib import Path
import shutil
import subprocess
import re


GRAMMARS = {
    "sqlite": ("sqlite", "SQLiteLexer", "SQLiteParser", "parse"),
    "sqlserver": ("tsql", "TSqlLexer", "TSqlParser", "tsql_file"),
    "postgresql": ("postgresql", "PostgreSQLLexer", "PostgreSQLParser", "root"),
    "oracle": ("plsql", "PlSqlLexer", "PlSqlParser", "sql_script"),
    "mysql": ("mysql/Oracle", "MySQLLexer", "MySQLParser", "queries"),
}


def fixtures(root: Path, dialect: str):
    for path in sorted((root / dialect).glob("*.sqltest")):
        lines = path.read_text(encoding="utf-8-sig").splitlines(keepends=True)
        index = 0
        native_profile = None
        while index < len(lines):
            header = lines[index].strip()
            index += 1
            profile = re.fullmatch(r"# native-profile: (80|90|100|110|120|130|140|150|160|170|180)", header)
            if profile:
                native_profile = int(profile.group(1))
                continue
            if not header.startswith("case "):
                continue
            fields = header.split()
            if len(fields) < 4 or not fields[-1].startswith("<<"):
                raise ValueError(f"invalid case header: {path}:{index}")
            marker = fields[-1][2:]
            body = []
            first_line = index + 1
            while index < len(lines) and lines[index].strip() != marker:
                body.append(lines[index])
                index += 1
            if index == len(lines):
                raise ValueError(f"unterminated case: {path}:{first_line}")
            index += 1
            # Semantic failures should be accepted by a pure grammar parser.
            if fields[2] in ("valid", "syntax-error", "semantic-error"):
                sql = "".join(body)
                # DELIMITER is a MySQL client command, not server SQL grammar.
                # Keep it out of this server-grammar comparison rather than
                # inventing a delimiter rewriter that changes quoted bodies.
                if dialect == "mysql" and re.search(r"^\s*DELIMITER\b", sql, re.M | re.I):
                    continue
                if dialect == "mysql" and not sql.rstrip().endswith(";"):
                    sql += "\n;"
                row = {"path": str(path), "line": first_line, "name": fields[1],
                       "expected": fields[2], "sql": sql}
                if native_profile is not None:
                    row['native_profile'] = native_profile
                yield row


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cache", type=Path, default=Path(".cache/sql-grammar-20261127"))
    parser.add_argument("--fixtures", type=Path, default=Path("tests/sql/syntax"))
    parser.add_argument("--dialect", choices=sorted(GRAMMARS), action="append")
    parser.add_argument("--java", default="java")
    parser.add_argument("--javac", default="javac")
    parser.add_argument("--output", type=Path)
    args = parser.parse_args()
    cache = args.cache.resolve()
    jar = cache / "antlr-4.13.2-complete.jar"
    lock = json.loads(Path("tests/sql/corpus/reference-sources.json").read_text())
    if not jar.is_file() or hashlib.sha256(jar.read_bytes()).hexdigest() != lock["antlr_tool"]["sha256"]:
        parser.error("download the pinned ANTLR jar and verify its SHA-256 first")
    work = cache / "antlr-java"
    work.mkdir(exist_ok=True)
    all_results = []
    for dialect in args.dialect or GRAMMARS:
        folder, lexer, grammar, entry = GRAMMARS[dialect]
        for name in [lexer, grammar]:
            shutil.copy2(cache / "antlr/sql" / folder / (name + ".g4"), work)
        support = list((cache / "antlr/sql" / folder / "Java").glob("*.java"))
        for path in support:
            shutil.copy2(path, work)
        subprocess.run([args.java, "-Xmx512m", "-jar", str(jar), "-Dlanguage=Java",
                        "-no-listener", lexer + ".g4", grammar + ".g4"],
                       cwd=work, check=True, timeout=180, capture_output=True)
        harness = Path(__file__).parent / "sql-grammar/AntlrSqlProbe.java"
        shutil.copy2(harness, work)
        subprocess.run([args.javac, "-J-Xmx512m", "-cp", str(jar),
                        lexer + ".java", grammar + ".java", "AntlrSqlProbe.java",
                        *[path.name for path in support]],
                       cwd=work, check=True, timeout=180, capture_output=True)
        cases = list(fixtures(args.fixtures, dialect))
        payload = "".join(base64.b64encode(case["sql"].encode()).decode() + "\n" for case in cases)
        run = subprocess.run([args.java, "-Xmx512m", "-cp", str(work) + ":" + str(jar),
                              "AntlrSqlProbe", lexer, grammar, entry],
                             input=payload, capture_output=True, text=True, check=True, timeout=180)
        lines = run.stdout.splitlines()
        if len(lines) != len(cases):
            raise RuntimeError("ANTLR batch response count did not match input count")
        for index, (case, line) in enumerate(zip(cases, lines)):
            ordinal, accepted, error = line.split("\t")
            if int(ordinal) != index:
                raise RuntimeError("ANTLR response order did not match input order")
            case["dialect"] = dialect
            case["antlr_accepted"] = accepted == "true"
            case["antlr_diagnostic"] = base64.b64decode(error).decode()
            case["agrees"] = case["antlr_accepted"] == (case["expected"] != "syntax-error")
            all_results.append(case)
    report = {"antlr_commit": lock["sources"]["antlr"]["revision"],
              "summary": dict(Counter("agreement" if r["agrees"] else "review" for r in all_results)),
              "cases": all_results}
    data = json.dumps(report, indent=2, ensure_ascii=False) + "\n"
    if args.output:
        args.output.write_text(data)
        print(json.dumps(report["summary"]))
    else:
        print(data, end="")


if __name__ == "__main__":
    try:
        main()
    except subprocess.CalledProcessError as error:
        detail = error.stderr.decode(errors="replace") if isinstance(error.stderr, bytes) else error.stderr
        raise SystemExit(detail or str(error))
