#!/usr/bin/env python3
"""Parse cached upstream candidates offline and report disagreements for review.

The compiler is invoked with --ast only. No database, shell expansion, imported
test harness, or upstream code is executed. Source rejection does NOT establish
that a query is syntactically invalid; review reports retain that distinction.
"""
from __future__ import annotations

import argparse
from collections import Counter
from concurrent.futures import ThreadPoolExecutor
import hashlib
import json
from pathlib import Path
import re
import shutil
import subprocess
import tempfile


def diagnostic_codes():
    """Resolve the CLI's numeric codes from the declared stable C++ enum."""
    header=(Path(__file__).resolve().parents[1]/'include/navidog/sql/compiler/compiler.hpp').read_text()
    match=re.search(r'enum class DiagnosticCode\s*\{([^}]+)\}',header,re.S)
    if not match:raise ValueError('diagnostic enum not found')
    names=[part.strip() for part in match[1].split(',') if part.strip()]
    if any(not re.fullmatch(r'[A-Za-z_]\w*',name) for name in names):
        raise ValueError('diagnostic enum requires explicit numeric mapping review')
    return {name:index for index,name in enumerate(names)}


def local_status(parsed,codes):
    if parsed['ok']:return 'accepted'
    if parsed['syntaxValid']:return 'semantic-rejection'
    diagnostics={row['code'] for row in parsed.get('diagnostics',[])}
    if codes['ResourceLimit'] in diagnostics:return 'resource-limit'
    if codes['Stopped'] in diagnostics:return 'stopped'
    return 'unsupported' if codes['UnsupportedSyntax'] in diagnostics else 'syntax-rejection'


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--compiler", type=Path, default=Path("build-grammar-audit/bin/navidog-sql"))
    parser.add_argument("--input", type=Path, default=Path(".cache/sql-grammar-20261127/candidates.jsonl"))
    parser.add_argument("--output", type=Path, default=Path(".cache/sql-grammar-20261127/candidate-audit.jsonl"))
    parser.add_argument("--dialect", choices=["mysql", "postgresql", "oracle", "sqlserver", "sqlite", "tidb", "mariadb", "oceanbase"])
    parser.add_argument("--limit", type=int)
    parser.add_argument("--jobs", type=int, default=4)
    parser.add_argument('--generate',action='store_true',help='also emit same-dialect SQL for locally accepted inputs; never executes SQL')
    args = parser.parse_args()
    if not 1 <= args.jobs <= 8 or (args.limit is not None and args.limit < 1):
        parser.error("jobs must be 1..8 and limit must be positive")
    compiler = args.compiler.resolve()
    if not compiler.is_file():
        parser.error("build the compiler first")
    input_bytes=args.input.read_bytes()
    input_sha=hashlib.sha256(input_bytes).hexdigest()
    cases = [json.loads(line) for line in input_bytes.decode('utf-8').splitlines() if line.strip()]
    if args.dialect:
        cases = [row for row in cases if row["dialect"] == args.dialect]
    if args.limit:
        cases = cases[:args.limit]
    codes=diagnostic_codes()

    def inspect(case):
        result = dict(case,compiler_sha256=compiler_sha)
        if case.get("requires_harness"):
            result["local_status"] = "harness-dependent"
            return result
        mode=case.get('mysql_sql_mode','')
        if mode not in ('','IGNORE_SPACE'):
            result['local_status']='mode-dependent'
            return result
        try:
            command=[str(compiler), "--dialect", case["dialect"], "--ast", "-"]
            if mode=='IGNORE_SPACE':command.insert(1,'--mysql-ignore-space')
            run = subprocess.run(command,
                                 input=case["sql"], text=True, capture_output=True, timeout=5)
        except subprocess.TimeoutExpired:
            result["local_status"] = "timeout"
            return result
        if run.returncode not in (0, 1):
            result.update(local_status="process-error", local_exit=run.returncode,
                          local_diagnostic=run.stderr[-4000:])
            return result
        try:
            parsed = json.loads(run.stdout)
        except json.JSONDecodeError:
            result.update(local_status="invalid-output", local_exit=run.returncode)
            return result
        result.update(local_ok=parsed["ok"], local_syntax_valid=parsed["syntaxValid"],
                      local_diagnostic=run.stderr[:4000],local_statement_count=len(parsed.get('statements',[])))
        result["local_status"] = local_status(parsed,codes)
        if args.generate and parsed['ok']:
            emission=command.copy()
            position=emission.index('--ast')
            emission[position:position+1]=['--emit',case['dialect']]
            try:
                generated=subprocess.run(emission,input=case['sql'],text=True,capture_output=True,timeout=5)
                if generated.returncode not in (0,1):
                    result['generation_status']='process-error'
                elif generated.returncode or not generated.stdout.strip():
                    result['generation_status']='rejected'
                elif len(generated.stdout.encode())>262144:
                    result['generation_status']='native-input-size-limit'
                else:
                    result['generation_status']='accepted'
                    result['generated_sql']=generated.stdout
                    result['generated_sql_sha256']=hashlib.sha256(generated.stdout.encode()).hexdigest()
                if generated.stderr:result['generation_diagnostic']=generated.stderr[:2000]
            except subprocess.TimeoutExpired:
                result['generation_status']='timeout'
        # This is triage only: no result is promoted directly into .sqltest.
        source = case.get("upstream_parser_accepts")
        result["review"] = ("known-upstream-acceptance-local-rejection" if source is True and not parsed["ok"] else
                            "known-upstream-rejection-local-acceptance" if source is False and parsed["ok"] else
                            "none")
        if case.get("version_sensitive") and result["review"] != "none":
            result["review"] = "version-sensitive-" + result["review"]
        return result

    statuses, reviews, generations = Counter(), Counter(), Counter()
    args.output.parent.mkdir(parents=True,exist_ok=True)
    with tempfile.TemporaryDirectory(prefix='nvg-candidate-compiler-',dir=args.output.parent) as directory, \
         args.output.open("w") as output, ThreadPoolExecutor(max_workers=args.jobs) as pool:
        frozen=Path(directory)/'navidog-sql'
        shutil.copy2(compiler,frozen)
        compiler=frozen
        compiler_sha=hashlib.sha256(frozen.read_bytes()).hexdigest()
        for index, result in enumerate(pool.map(inspect, cases), 1):
            output.write(json.dumps(result, ensure_ascii=False) + "\n")
            statuses[result["local_status"]] += 1
            reviews[result.get("review", "not-compared")] += 1
            if result.get('generation_status'):generations[result['generation_status']]+=1
            if index % 500 == 0:
                output.flush()
                print(f"Parsed {index}/{len(cases)} cached candidates", flush=True)
    summary={"cases": len(cases), "statuses": dict(statuses),
             "reviews": dict(reviews), "output": str(args.output),
             "compiler_sha256":compiler_sha,"input_sha256":input_sha,"generation":dict(generations)}
    summary['diagnostic_codes']=codes
    args.output.with_suffix('.summary.json').write_text(json.dumps(summary,indent=2)+'\n')
    print(json.dumps(summary))
    if any(statuses[key] for key in ["timeout", "process-error", "invalid-output"]) or any(generations[key] for key in ['timeout','process-error']):
        raise SystemExit(1)


if __name__ == "__main__":
    main()
