#!/usr/bin/env python3
"""Extract literal upstream cases for OFFLINE review; never execute their SQL.

No upstream rejection is automatically classified as a syntax error. Test
parsers also reject some semantically invalid SQL, and tests may require data,
functions, special SQL modes, harness directives, or particular engine versions.
Original bytes and derived candidates remain in the ignored source cache.
"""
from __future__ import annotations

import argparse
import ast
from collections import Counter
import hashlib
import json
from pathlib import Path
import re


def balanced(text: str, start: int, go: bool = False) -> int:
    """Find a braced Tcl word or Go composite literal without evaluating it."""
    if text[start] != "{":
        raise ValueError("expected opening brace")
    depth, i = 1, start + 1
    while i < len(text):
        char = text[i]
        if char == "\\":
            i += 2
            continue
        if go and char in ('"', "'", "`"):
            delimiter = char
            i += 1
            while i < len(text):
                if delimiter != "`" and text[i] == "\\":
                    i += 2
                elif text[i] == delimiter:
                    i += 1
                    break
                else:
                    i += 1
            continue
        if go and text.startswith("//", i):
            end = text.find("\n", i)
            i = len(text) if end < 0 else end + 1
            continue
        if go and text.startswith("/*", i):
            end = text.find("*/", i + 2)
            if end < 0:
                raise ValueError("unclosed Go comment")
            i = end + 2
            continue
        if char == "{":
            depth += 1
        elif char == "}":
            depth -= 1
            if not depth:
                return i
        i += 1
    raise ValueError("unclosed brace")


def tidb_cases(text: str):
    entry = re.compile(r'^\s*\{\s*("(?:\\.|[^"\\])*"|`[^`]*`)\s*,\s*(true|false)\s*,', re.M)
    functions = []
    for function in re.finditer(r'\bfunc\s+(Test\w+)\s*\(',text):
        begin = text.index('{',function.end())
        end = balanced(text,begin,go=True)
        functions.append((begin,end,function.group(1),text[begin:end]))
    for table in re.finditer(r'\btable\s*:?=\s*\[\]testCase\s*\{', text):
        start = text.index("{", table.start())
        end = balanced(text, start, go=True)
        owner = next((item for item in functions if item[0]<start<item[1]),None)
        acceptance = bool(owner and re.search(r'\bRunTest\s*\(\s*t\s*,\s*table\s*,',owner[3]))
        for found in entry.finditer(text, start + 1, end):
            literal = found.group(1)
            try:
                sql = literal[1:-1].replace("\r", "") if literal.startswith("`") else ast.literal_eval(literal)
            except (ValueError, SyntaxError):
                continue
            if not re.match(r'\s*(?:SELECT|WITH|CREATE|ALTER|DROP|INSERT|REPLACE|UPDATE|DELETE|SHOW|ADMIN|SET|BEGIN|START|COMMIT|ROLLBACK|EXPLAIN|TRUNCATE|RENAME|GRANT|REVOKE|ANALYZE|PREPARE|EXECUTE|DEALLOCATE|USE|DO|LOCK|UNLOCK|LOAD|FLUSH|KILL|CHECK|RECOVER|SPLIT|BACKUP|RESTORE|IMPORT|EXPORT)\b', sql, re.I):
                continue
            yield {"sql": sql, "line": text.count("\n", 0, found.start(1)) + 1,
                   "upstream_case_flag": found.group(2) == "true",
                   "upstream_test_function": owner[2] if owner else None,
                   "upstream_parser_accepts": (found.group(2) == "true") if acceptance else None}


def sqlite_cases(text: str):
    pattern = re.compile(r'^\s*(do_execsql_test|do_catchsql_test)\s+(\S+)\s+(\{)', re.M)
    for found in pattern.finditer(text):
        start = found.end(3) - 1
        end = balanced(text, start)
        tail = end + 1
        while tail < len(text) and text[tail].isspace():
            tail += 1
        expected = None
        if tail < len(text) and text[tail] == "{":
            expected_end = balanced(text, tail)
            expected = text[tail + 1:expected_end].strip()
        status = re.match(r"^([01])(?:\s|$)", expected or "")
        execution_accepts = True if found.group(1) == "do_execsql_test" else (
            status.group(1) == "0" if status else None)
        # catchsql also catches binding, constraint and runtime failures. Even
        # syntax-looking messages may come from eval() or another SQL body;
        # retain them as review hints, never as outer raw-parser assertions.
        accepts = True if execution_accepts is True else None
        yield {"sql": text[start + 1:end], "line": text.count("\n", 0, start + 1) + 1,
               "upstream_case": found.group(2), "upstream_parser_accepts": accepts,
               "upstream_execution_accepts": execution_accepts,
               "upstream_parse_error_expected": execution_accepts is False and bool(re.search(
                   r'\bsyntax error\b|\bincomplete input\b|\bunrecognized token\b',expected or '',re.I)),
               "requires_harness": "$" in text[start + 1:end],
               "upstream_expected": expected}


def csharp_string(text: str, start: int):
    """Decode a non-interpolated C# string literal, without evaluating C#."""
    verbatim = text.startswith('@"', start)
    if not verbatim and text[start:start + 1] != '"':
        raise ValueError("not a literal C# string")
    i, value = start + (2 if verbatim else 1), []
    escapes = {"0": "\0", "a": "\a", "b": "\b", "f": "\f", "n": "\n", "r": "\r",
               "t": "\t", "v": "\v", "\\": "\\", '"': '"', "'": "'"}
    while i < len(text):
        char = text[i]
        i += 1
        if char == '"':
            if verbatim and text[i:i + 1] == '"':
                value.append('"')
                i += 1
                continue
            return "".join(value), i
        if char == "\\" and not verbatim:
            escape = text[i:i + 1]
            i += 1
            if escape in escapes:
                value.append(escapes[escape])
            elif escape in ("u", "U", "x"):
                count = 8 if escape == "U" else 4
                found = re.match(r"[0-9a-fA-F]{1," + str(count) + "}", text[i:])
                if not found or (escape != "x" and len(found.group()) != count):
                    raise ValueError("invalid C# Unicode escape")
                value.append(chr(int(found.group(), 16)))
                i += len(found.group())
            else:
                raise ValueError("unsupported C# string escape")
        else:
            value.append(char)
    raise ValueError("unclosed C# string")


def csharp_call_end(text: str, start: int):
    depth, i = 1, start + 1
    while i < len(text):
        if text.startswith('//',i):
            end = text.find('\n',i)
            i = len(text) if end < 0 else end + 1
        elif text.startswith('/*',i):
            end = text.find('*/',i + 2)
            if end < 0:
                raise ValueError('unclosed C# comment')
            i = end + 2
        elif text.startswith('@"',i) or text[i] == '"':
            _, i = csharp_string(text,i)
        elif text[i] == "'":
            i += 1
            while i < len(text) and text[i] != "'":
                i += 2 if text[i] == '\\' else 1
            i += 1
        else:
            if text[i] == '(':
                depth += 1
            elif text[i] == ')':
                depth -= 1
                if not depth:
                    return i
            i += 1
    raise ValueError('unclosed C# call')


def csharp_code_mask(text: str):
    mask, i = list(text), 0
    while i < len(text):
        start = i
        if text.startswith('//',i):
            end = text.find('\n',i)
            i = len(text) if end < 0 else end
        elif text.startswith('/*',i):
            end = text.find('*/',i + 2)
            i = len(text) if end < 0 else end + 2
        elif text.startswith('@"',i) or text[i] == '"':
            try:
                _, i = csharp_string(text,i)
            except (ValueError,OverflowError):
                i += 1
        elif text[i] == "'":
            i += 1
            while i < len(text) and text[i] != "'":
                i += 2 if text[i] == '\\' else 1
            i = min(i + 1,len(text))
        else:
            i += 1
            continue
        for position in range(start,i):
            if mask[position] not in '\r\n':
                mask[position] = ' '
    return ''.join(mask)


def sqlserver_cases(text: str):
    pattern = re.compile(r'^\s*ParserTestUtils\.(ErrorTest\w*)\s*\(', re.M)
    for found in pattern.finditer(csharp_code_mask(text)):
        start, i, parts = found.end(), found.end(), []
        while text[i:i + 1].isspace():
            i += 1
        start = i
        try:
            while True:
                part, i = csharp_string(text, i)
                parts.append(part)
                while text[i:i + 1].isspace():
                    i += 1
                if text[i:i + 1] != "+":
                    break
                i += 1
                while text[i:i + 1].isspace():
                    i += 1
            # Skip computed/interpolated/resource expressions conservatively.
            if text[i:i + 1] not in (",", ")"):
                continue
            # Only diagnostics inside this call count as evidence. An
            # ErrorTest name alone does not establish rejection.
            end = csharp_call_end(text,found.end() - 1)
            tail = text[i:end]
            diagnostics = re.findall(r'new\s+ParserErrorInfo\([^;]*?"(SQL\d+)"', tail)
            expected = False if diagnostics else (True if text[i:i + 1] == ")" else None)
            yield {"sql": "".join(parts), "line": text.count("\n", 0, start) + 1,
                   "upstream_parser_accepts": expected, "upstream_parser_set": found.group(1),
                   "upstream_diagnostic_codes": diagnostics,
                   "version_sensitive": True}
        except (ValueError, IndexError, OverflowError):
            continue


def postgresql_cases(text: str):
    """Split regression input lexically; no psql or server execution occurs.

    These are unclassified candidates. Regression scripts deliberately mix
    successful statements with syntax, binding, semantic and runtime failures.
    """
    i, start, active, harness = 0, 0, False, False
    while i < len(text):
        if text.startswith('--',i):
            end = text.find('\n',i + 2)
            i = len(text) if end < 0 else end + 1
            continue
        if text.startswith('/*',i):
            depth, i = 1, i + 2
            while i < len(text) and depth:
                if text.startswith('/*',i):
                    depth, i = depth + 1, i + 2
                elif text.startswith('*/',i):
                    depth, i = depth - 1, i + 2
                else:
                    i += 1
            continue
        if text[i] == '\\' and not text[text.rfind('\n',0,i) + 1:i].strip():
            end = text.find('\n',i)
            end = len(text) if end < 0 else end + 1
            if not active:
                start = end
            else:
                harness = True
            i = end
            continue
        char = text[i]
        if char in ("'", '"'):
            active = True
            escaped = char == "'" and i > 0 and text[i - 1] in 'eE' and (
                i == 1 or not (text[i - 2].isalnum() or text[i - 2] in '_$'))
            i += 1
            while i < len(text):
                if escaped and text[i] == '\\':
                    i += 2
                elif text[i] == char:
                    i += 1
                    if text[i:i + 1] != char:
                        break
                    i += 1
                else:
                    i += 1
            continue
        if char == '$' and (i == 0 or not (text[i - 1].isalnum() or text[i - 1] in '_$')):
            tag = re.match(r'\$(?:[A-Za-z_][A-Za-z_0-9]*)?\$',text[i:])
            if tag:
                active = True
                end = text.find(tag.group(),i + len(tag.group()))
                i = len(text) if end < 0 else end + len(tag.group())
                continue
        if char == ';':
            if active:
                sql = text[start:i + 1]
                probe = re.sub(r'/\*.*?\*/|--[^\n]*',' ',sql,flags=re.S).lstrip()
                copy = bool(re.match(r'COPY\b',probe,re.I) and re.search(r'\bFROM\s+STDIN\b',probe,re.I))
                yield {"sql": sql,"line": text.count('\n',0,start) + 1,
                       "upstream_parser_accepts": None,"upstream_regression": True,
                       "requires_harness": harness or copy}
                if copy:
                    end = re.search(r'(?m)^\\\.[ \t]*\r?$',text[i + 1:])
                    if not end:
                        return
                    i += 1 + end.end()
            i += 1
            start, active, harness = i, False, False
            continue
        if not char.isspace():
            active = True
        i += 1
    if active:
        yield {"sql": text[start:],"line": text.count('\n',0,start) + 1,
               "upstream_parser_accepts": None,"upstream_regression": True,
               "requires_harness": harness}


def postgres_expected_annotations(cases, expected: str):
    """Attach only adjacent, exact echo matches from PostgreSQL expected output."""
    def without_leading_comments(sql):
        i = 0
        while i < len(sql):
            if sql[i].isspace():
                i += 1
            elif sql.startswith('--',i):
                end = sql.find('\n',i)
                i = len(sql) if end < 0 else end + 1
            elif sql.startswith('/*',i):
                depth, i = 1, i + 2
                while i < len(sql) and depth:
                    if sql.startswith('/*',i):
                        depth, i = depth + 1, i + 2
                    elif sql.startswith('*/',i):
                        depth, i = depth - 1, i + 2
                    else:
                        i += 1
            else:
                break
        return sql[i:].rstrip()

    positions, cursor = [], 0
    for case in cases:
        sql = without_leading_comments(case['sql'])
        if not sql or expected.count(sql)!=1:
            positions.append((-1,-1))
            continue
        begin = expected.find(sql,cursor) if sql else -1
        if begin >= 0 and expected[expected.rfind('\n',0,begin) + 1:begin].strip():
            begin = -1
        end = begin + len(sql) if begin >= 0 else -1
        positions.append((begin,end))
        if begin >= 0:
            cursor = end
    for index,case in enumerate(cases):
        begin,end = positions[index]
        following = positions[index + 1][0] if index + 1 < len(cases) else len(expected)
        if begin < 0 or following < 0 or case.get('requires_harness'):
            continue
        output = expected[end:following]
        if re.search(r'(?m)^\\',output):
            continue
        case['upstream_expected_echo_matched'] = True
        if re.search(r'(?m)^\(\d+ rows?\)\s*$',output):
            case['upstream_parser_accepts'] = True
            case['upstream_execution_accepts'] = True
        else:
            error = re.search(r'(?m)^ERROR:\s+([^\n]+)',output)
            if error:
                message = error.group(1)
                case['upstream_execution_accepts'] = False
                case['upstream_error_message'] = message
                if message.startswith('syntax error') and re.search(r'(?m)^LINE \d+:',output):
                    case['upstream_parser_accepts'] = False
                    case['upstream_error_category'] = 'syntax'


def mysqltest_cases(text: str):
    """Read literal mysql-test statements without interpreting its language.

    Includes, eval/let, control flow and embedded files are never evaluated.
    Custom delimiters delimit whole routine bodies, not their inner SQL. Even
    ER_PARSE_ERROR/1064 can come from PREPARE or a called routine's body: error
    codes are review hints and never establish raw parser acceptance here.
    """
    sql_head=re.compile(r'(?:SELECT|WITH|CREATE|ALTER|DROP|INSERT|REPLACE|UPDATE|DELETE|'
        r'SHOW|SET|BEGIN|START|COMMIT|ROLLBACK|EXPLAIN|TRUNCATE|RENAME|GRANT|REVOKE|'
        r'ANALYZE|PREPARE|EXECUTE|DEALLOCATE|USE|DO|LOCK|UNLOCK|LOAD|FLUSH|KILL|'
        r'CHECK|REPAIR|OPTIMIZE|CALL|SAVEPOINT|RELEASE|HANDLER|DESCRIBE|DESC)\b',re.I)
    i,start,delimiter=0,None,';'
    errors,mode,depth=[],None,0
    pending_control=False
    skip_statement,embedded,disabled=False,None,False
    while i<len(text):
        line_start=i==0 or text[i-1]=='\n'
        if line_start and start is None:
            end=text.find('\n',i)
            end=len(text) if end<0 else end+1
            line=text[i:end].strip()
            if embedded:
                if line==embedded:embedded=None
                i=end
                continue
            command=re.sub(r'^--\s*','',line)
            if disabled:
                if command.lower().startswith('enable_parsing'):disabled=False
                i=end
                continue
            directive=re.match(r'(delimiter|error)\s+(.*)',command,re.I)
            if directive:
                value=directive[2].strip()
                if directive[1].lower()=='delimiter':
                    # Bare mysqltest commands end with the OLD delimiter;
                    # --delimiter is a comment command and has no terminator.
                    if not line.startswith('--') and value.endswith(delimiter):value=value[:-len(delimiter)]
                    if value and not any(c.isspace() for c in value):delimiter=value
                else:
                    if value.endswith(delimiter):value=value[:-len(delimiter)]
                    errors=[part.strip().upper() for part in value.split(',')]
                i=end
                continue
            block=re.match(r'(?:write_file|append_file|perl)\b(.*)',command,re.I)
            if block:
                marker=re.search(r'\b(EOF\w*)\s*;?$',block[1])
                embedded=marker[1] if marker else 'EOF'
                errors=[]
                i=end
                continue
            if re.match(r'disable_parsing\b',command,re.I):
                disabled=True
                errors=[]
                i=end
                continue
            if re.match(r'(?:if|while)\s*\(',command,re.I):
                if command.endswith('{'):depth+=1
                else:pending_control=True
                errors=[]
                i=end
                continue
            if line=='{':
                depth+=1
                pending_control=False
                errors=[]
                i=end
                continue
            if line.startswith('}'):
                depth=max(0,depth-1)
                pending_control=False
                errors=[]
                i=end
                continue
            if re.match(r'(?:let|eval|send_eval)\b',command,re.I):
                errors=[]
                if line.startswith('--'):
                    i=end
                    continue
                # Skip to the real SQL delimiter, including multiline strings.
                start=i
                skip_statement=True
            elif not line or line.startswith(('#','--')):
                if re.match(r'(?:source|connection|connect|disconnect)\b',command,re.I):
                    mode=None
                    errors=[]
                i=end
                continue
            elif not sql_head.match(line) and not line.startswith('/*'):
                # Harness commands (connection, echo, replace_result, ...).
                if re.match(r'(?:source|connection|connect|disconnect)\b',line,re.I):mode=None
                errors=[]
                i=end
                continue
        if text.startswith('/*',i):
            end=text.find('*/',i+2)
            if end<0:return
            i=end+2
            continue
        if text[i]=='#' or (text.startswith('--',i) and
                (i+2==len(text) or text[i+2].isspace())):
            end=text.find('\n',i)
            i=len(text) if end<0 else end+1
            continue
        if text[i] in "'\"`":
            if start is None:start=i
            quote=text[i]
            i+=1
            while i<len(text):
                if text[i]=='\\' and 'NO_BACKSLASH_ESCAPES' not in (mode or '').split(','):i+=2
                elif text[i]==quote:
                    i+=1
                    if text[i:i+1]!=quote:break
                    i+=1
                else:i+=1
            continue
        if text.startswith(delimiter,i):
            if start is not None and not skip_statement:
                raw=text[start:i].strip()
                if sql_head.match(raw):
                    sql=raw if raw.endswith(';') else raw+';'
                    dynamic=bool(re.search(r'\$[A-Za-z_0-9{]',raw))
                    yield dict(sql=sql,line=text.count('\n',0,start)+1,
                        upstream_parser_accepts=None,
                        upstream_parse_error_expected=bool(errors) and all(e in ('1064','ER_PARSE_ERROR') for e in errors),
                        upstream_error_codes=errors,upstream_regression=True,
                        upstream_execution_expectation='error' if errors else 'success',
                        upstream_delimiter=delimiter,mysql_sql_mode_known=mode is not None,
                        **({'mysql_sql_mode':mode} if mode is not None else {}),
                        requires_harness=dynamic or depth>0 or pending_control)
                    assignment=re.match(r'SET\s+(?:(?:SESSION|LOCAL)\s+|@@(?:session\.)?)?sql_mode\s*=\s*\'([^\']*)\'\s*;?$',raw,re.I)
                    if assignment and not errors and not depth:
                        mode=','.join(sorted(set(part.strip().upper() for part in assignment[1].split(',') if part.strip())))
                    elif re.search(r'\bsql_mode\b',raw,re.I) and re.match(r'SET\b',raw,re.I):mode=None
            i+=len(delimiter)
            start,errors,skip_statement=None,[],False
            continue
        if start is None and not text[i].isspace():start=i
        i+=1


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cache", type=Path, default=Path(".cache/sql-grammar-20261127"))
    parser.add_argument('--output',type=Path,help='candidate JSONL destination; defaults to candidates.jsonl in the cache')
    args = parser.parse_args()
    manifest = json.loads((args.cache / "manifest.json").read_text())
    rows = []
    seen = set()
    for key, entry in sorted(manifest["files"].items()):
        if entry["status"] != "downloaded":
            continue
        path = args.cache / key
        dialect = key.split("/", 1)[0]
        if key == "tidb/pkg/parser/parser_test.go":
            extractor = tidb_cases
        elif dialect == "sqlite" and path.suffix == ".test":
            extractor = sqlite_cases
        elif key == "sqlserver/Test/SqlDom/ParserErrorsTests.cs":
            extractor = sqlserver_cases
        elif key.startswith("postgresql/src/test/regress/sql/") and path.suffix == ".sql":
            extractor = postgresql_cases
        elif dialect in ('mysql','mariadb') and '/mysql-test/' in key and path.suffix in ('.test','.inc'):
            extractor = mysqltest_cases
        elif key.startswith("antlr/sql/") and "/examples/" in key and path.suffix == ".sql":
            grammar = key.split("/")[2]
            dialect = {"mysql": "mysql", "postgresql": "postgresql", "plsql": "oracle",
                       "tsql": "sqlserver", "sqlite": "sqlite"}.get(grammar)
            if not dialect:
                continue
            def extractor(text):
                yield {"sql": text, "line": 1, "upstream_parser_accepts": None,
                       "upstream_example": True}
        else:
            continue
        content = path.read_bytes()
        if hashlib.sha256(content).hexdigest() != entry["sha256"]:
            raise ValueError("cached source digest mismatch: " + str(path))
        encoding='utf-8'
        try:
            decoded=content.decode(encoding)
        except UnicodeDecodeError:
            if extractor is not mysqltest_cases:raise
            encoding='latin-1'
            decoded=content.decode(encoding)
        cases = list(extractor(decoded))
        if extractor is mysqltest_cases:
            for case in cases:
                case['source_encoding']=encoding
                if encoding!='utf-8' and not case['sql'].isascii():
                    case['requires_harness']=True
                    case['upstream_parser_accepts']=None
        expected_key = key.replace('/sql/','/expected/')[:-4] + '.out' if key.endswith('.sql') else ''
        expected_entry = manifest['files'].get(expected_key)
        if key.startswith('postgresql/src/test/regress/sql/') and expected_entry:
            expected = (args.cache / expected_key).read_bytes()
            if hashlib.sha256(expected).hexdigest() != expected_entry['sha256']:
                raise ValueError('expected output digest mismatch: ' + expected_key)
            postgres_expected_annotations(cases,expected.decode('utf-8'))
            for case in cases:
                case['upstream_expected_url'] = expected_entry['url']
                case['upstream_expected_sha256'] = expected_entry['sha256']
        for case in cases:
            identity = (dialect, case["sql"],case.get('mysql_sql_mode'),
                        case.get('upstream_parser_accepts'),tuple(case.get('upstream_error_codes',[])),
                        case.get('upstream_execution_accepts'),case.get('upstream_expected') if dialect=='sqlite' else None)
            if identity in seen or len(case["sql"]) > 262144:
                continue
            seen.add(identity)
            case.update({"dialect": dialect, "source_url": entry["url"],
                         "source_sha256": entry["sha256"], "source_path": key,
                         "sql_sha256": hashlib.sha256(case["sql"].encode()).hexdigest(),
                         "classification": "unreviewed"})
            rows.append(case)
    destination = args.output or args.cache / "candidates.jsonl"
    destination.parent.mkdir(parents=True,exist_ok=True)
    temporary = destination.with_name(destination.name + '.partial')
    temporary.write_text("".join(json.dumps(row, ensure_ascii=False) + "\n" for row in rows))
    temporary.replace(destination)
    print(json.dumps({"destination": str(destination), "cases": len(rows),
                      "dialects": dict(Counter(row["dialect"] for row in rows)),
                      "upstream_acceptance": dict(Counter(str(row["upstream_parser_accepts"]) for row in rows))}))


if __name__ == "__main__":
    main()
