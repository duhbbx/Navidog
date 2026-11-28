#!/usr/bin/env python3
"""Fetch pinned SQL grammar references into an ignored local cache.

Only downloads public source files; never executes downloaded code or SQL.
The manifest records exact commits, byte counts, and SHA-256 digests. Original
sources and their license files stay outside the shipped application/source.
"""
from __future__ import annotations

import argparse
import base64
from concurrent.futures import ThreadPoolExecutor, as_completed
from datetime import datetime, timezone
import hashlib
import json
from pathlib import Path
import subprocess
from urllib.parse import quote


SOURCES = {
    "antlr": ("antlr/grammars-v4", "de38599628d221099c046e92962f70204d852b54", [
        # grammars-v4 has per-grammar notices, not a repository-wide LICENSE.
        "README.md",
        "sql/mysql/Oracle/MySQLParser.g4", "sql/mysql/Oracle/MySQLLexer.g4",
        "sql/postgresql/PostgreSQLParser.g4",
        "sql/postgresql/PostgreSQLLexer.g4", "sql/plsql/PlSqlParser.g4",
        "sql/plsql/PlSqlLexer.g4", "sql/tsql/TSqlParser.g4",
        "sql/tsql/TSqlLexer.g4", "sql/sqlite/SQLiteParser.g4",
        "sql/sqlite/SQLiteLexer.g4",
        "sql/postgresql/Java/PostgreSQLLexerBase.java",
        "sql/postgresql/Java/PostgreSQLParserBase.java",
        "sql/postgresql/Java/LexerDispatchingErrorListener.java",
        "sql/postgresql/Java/ParserDispatchingErrorListener.java",
        "sql/plsql/Java/PlSqlLexerBase.java", "sql/plsql/Java/PlSqlParserBase.java",
        "sql/mysql/Oracle/Java/MySQLLexerBase.java", "sql/mysql/Oracle/Java/MySQLParserBase.java",
        "sql/mysql/Oracle/Java/SqlMode.java", "sql/mysql/Oracle/Java/SqlModes.java",
    ]),
    "postgresql": ("postgres/postgres", "REL_18_STABLE", [
        "COPYRIGHT", "src/backend/parser/gram.y", "src/include/parser/kwlist.h",
        "src/backend/commands/define.c",
        "src/backend/parser/parse_clause.c", "src/backend/parser/parse_target.c",
        "src/include/nodes/parsenodes.h",
        *[f"src/test/regress/{directory}/{name}.{extension}"
          for directory, extension in [("sql", "sql"), ("expected", "out")]
          for name in ["select", "select_distinct", "select_distinct_on",
                       "select_having", "join", "window", "arrays", "subselect",
                       "with", "insert", "update", "delete", "merge",
                       "create_table", "create_index", "alter_table"]],
    ]),
    "mysql": ("mysql/mysql-server", "8.4", [
        "LICENSE", "sql/sql_yacc.yy", "sql/lex.h", "sql/sql_lex.cc",
        "mysql-test/include/select.inc", "mysql-test/include/with_recursive.inc",
        *[f"mysql-test/t/{name}.test" for name in ["select_all",
          "func_like", "subselect", "window_functions", "with_recursive", "insert",
          "update", "delete", "partition"]],
    ]),
    "mariadb": ("MariaDB/server", "11.4", [
        "COPYING", "sql/sql_yacc.yy", "sql/lex.h", "sql/sql_lex.cc",
        *[f"mysql-test/main/{name}.test" for name in ["select", "func_in",
          "func_like", "subselect", "insert", "update", "delete", "partition"]],
    ]),
    "tidb": ("pingcap/tidb", "release-8.5", [
        "LICENSE", "pkg/parser/parser.y", "pkg/parser/parser_test.go",
        "pkg/parser/lexer_test.go", "pkg/parser/lexer.go", "pkg/parser/misc.go",
        "pkg/parser/go.mod", "pkg/parser/README.md",
        "pkg/expression/builtin_time.go", "pkg/expression/builtin_time_test.go",
    ]),
    "sqlite": ("sqlite/sqlite", "master", [
        "LICENSE.md", "src/parse.y",
        *[f"test/{name}.test" for name in ["select1", "select2", "select3",
          "select4", "select5", "select6", "select7", "select8", "select9",
          "selectA", "window1", "with1", "insert", "update", "delete"]],
    ]),
    "oceanbase": ("oceanbase/oceanbase", "master", [
        "LICENSE", "src/sql/parser/sql_parser_mysql_mode.y",
    ]),
    "sqlserver": ("microsoft/SqlScriptDOM", "main", [
        "LICENSE", "SqlScriptDom/Parser/TSql/TSql160.g", "SqlScriptDom/Parser/TSql/TSql170.g",
        "SqlScriptDom/Parser/TSql/TSql100ParserBaseInternal.cs",
        "SqlScriptDom/Parser/TSql/TSql80ParserBaseInternal.cs",
        "Test/SqlDom/ParserErrorsTests.cs", "Test/SqlDom/TestUtilities.cs",
    ]),
}


def fetch(url: str, max_bytes: int = 12 * 1024 * 1024) -> bytes:
    # A direct retry is useful where the local HTTPS proxy cannot reach GitHub.
    for direct in (False, True):
        command = ["curl", "--fail", "--silent", "--show-error", "--location",
                   "--connect-timeout", "8", "--max-time", "25",
                   "--max-filesize", str(max_bytes)]
        if direct:
            command += ["--noproxy", "*"]
        try:
            result = subprocess.run([*command, url], capture_output=True, timeout=30)
        except subprocess.TimeoutExpired:
            continue
        if result.returncode == 0:
            if len(result.stdout) > max_bytes:
                raise RuntimeError("response exceeds byte limit")
            return result.stdout
        if result.returncode == 22 and b"404" in result.stderr:
            raise FileNotFoundError(url)
    raise RuntimeError("public source download failed: " + url)


def write_manifest(path: Path, manifest) -> None:
    temporary = path.with_name(path.name + '.partial')
    temporary.write_text(json.dumps(manifest,indent=2) + '\n')
    temporary.replace(path)


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cache", type=Path, default=Path(".cache/sql-grammar-20261127"))
    parser.add_argument("--source", action="append", choices=sorted(SOURCES))
    parser.add_argument("--lock", type=Path, default=Path("tests/sql/corpus/reference-sources.json"))
    parser.add_argument("--jobs", type=int, default=4)
    parser.add_argument("--examples", type=int, default=0,
                        help="also collect up to N pinned ANTLR example .sql files per grammar (0..100)")
    args = parser.parse_args()
    if not 1 <= args.jobs <= 8:
        parser.error("--jobs must be between 1 and 8")
    if not 0 <= args.examples <= 100:
        parser.error("--examples must be between 0 and 100")
    args.cache.mkdir(parents=True, exist_ok=True)
    manifest_path = args.cache / "manifest.json"
    manifest = json.loads(manifest_path.read_text()) if manifest_path.exists() else {
        "schema_version": 1, "sources": {}, "files": {},
    }
    manifest["retrieved_at"] = datetime.now(timezone.utc).isoformat()
    locked = json.loads(args.lock.read_text())["sources"] if args.lock.exists() else {}
    tasks = []
    for name in args.source or SOURCES:
        repository, ref, paths = SOURCES[name]
        existing = manifest["sources"].get(name)
        revision = locked.get(name, {}).get("revision",existing["revision"] if existing else ref)
        if len(revision) != 40:
            metadata = json.loads(fetch(f"https://api.github.com/repos/{repository}/commits/{quote(ref, safe='')}"))
            revision = metadata["sha"]
        manifest["sources"][name] = {"repository": repository, "ref": ref, "revision": revision}
        paths = list(paths)
        if name == "antlr" and args.examples:
            for directory in ["sql/mysql/Oracle/examples", "sql/postgresql/examples",
                              "sql/plsql/examples", "sql/tsql/examples", "sql/sqlite/examples"]:
                try:
                    listing = json.loads(fetch(
                        f"https://api.github.com/repos/{repository}/contents/{directory}?ref={revision}"))
                except (FileNotFoundError, RuntimeError):
                    print("example directory unavailable", directory, flush=True)
                    continue
                if not isinstance(listing, list):
                    raise RuntimeError("expected an example directory listing")
                examples = sorted(item["path"] for item in listing
                    if item.get("type") == "file" and str(item.get("name", "")).endswith(".sql") and
                    str(item.get("path", "")).startswith(directory + "/") and
                    ".." not in str(item.get("path", "")).split("/"))
                paths.extend(examples[:args.examples])
        for path in paths:
            tasks.append((name, repository, revision, path))
    # Save revisions before downloading so an interrupted run remains pinned.
    write_manifest(manifest_path,manifest)

    def download(task):
        name, repository, revision, path = task
        destination = args.cache / name / path
        key = name + "/" + path
        previous = manifest["files"].get(key, {})
        url = f"https://raw.githubusercontent.com/{repository}/{revision}/{path}"
        if previous.get('url')==url and destination.is_file() and hashlib.sha256(destination.read_bytes()).hexdigest() == previous.get("sha256"):
            return key, previous
        method = "raw"
        try:
            content = fetch(url)
        except FileNotFoundError:
            return key, {"url": url, "status": "not-found"}
        except RuntimeError:
            # GitHub's separate contents endpoint can remain reachable when
            # the raw CDN is unavailable. It returns the same pinned bytes.
            api = f"https://api.github.com/repos/{repository}/contents/{path}?ref={revision}"
            try:
                metadata = json.loads(fetch(api))
            except FileNotFoundError:
                return key, {"url": url, "status": "not-found"}
            if metadata.get("encoding") != "base64":
                # GitHub contents omits inline data for files larger than 1 MiB.
                blob = metadata.get("git_url", "")
                if (metadata.get("size", 0) > 12 * 1024 * 1024 or
                    not blob.startswith(f"https://api.github.com/repos/{repository}/git/blobs/")):
                    raise RuntimeError("contents endpoint did not provide source bytes: " + url)
                metadata = json.loads(fetch(blob, max_bytes=20 * 1024 * 1024))
            if metadata.get("encoding") != "base64":
                raise RuntimeError("Git blob endpoint did not provide source bytes: " + url)
            content = base64.b64decode(metadata["content"], validate=False)
            method = "github-contents-api"
        destination.parent.mkdir(parents=True, exist_ok=True)
        temporary = destination.with_name(destination.name + ".partial")
        temporary.write_bytes(content)
        temporary.replace(destination)
        return key, {"url": url, "status": "downloaded", "retrieval": method, "bytes": len(content),
                     "sha256": hashlib.sha256(content).hexdigest()}

    failures = []
    with ThreadPoolExecutor(max_workers=args.jobs) as pool:
        futures = {pool.submit(download, task): task for task in tasks}
        for future in as_completed(futures):
            try:
                key, entry = future.result()
                manifest["files"][key] = entry
                print(entry["status"], key, flush=True)
                if entry["status"] == "not-found":
                    failures.append(key)
            except Exception as error:
                task = futures[future]
                failures.append(task[0] + "/" + task[3])
                print("failed", failures[-1], str(error), flush=True)
            write_manifest(manifest_path,manifest)
    if failures:
        raise SystemExit("Unfinished downloads (safe to retry): " + ", ".join(failures))
    print(f"Manifest: {manifest_path}")


if __name__ == "__main__":
    main()
