#!/usr/bin/env python3
"""Compare authored SQLite scalar operator expressions and generated SQL.

Only exact independently generated SQL/hashes are admitted. Connections are new
in-memory databases, extensions are disabled, an authorizer admits SELECT and
LIKE/GLOB only, and VM steps/results are bounded. No upstream SQL is executed.
Result values AND Python storage categories are compared; column labels are not
part of this scalar-expression check. Native failures remain stage-neutral.
"""
from __future__ import annotations

import argparse
from collections import Counter
from contextlib import closing
import hashlib
import importlib.util
import json
from pathlib import Path
import shutil
import sqlite3
import subprocess
import tempfile


def native(sql):
    with closing(sqlite3.connect(':memory:')) as connection:
        if hasattr(connection,'enable_load_extension'):connection.enable_load_extension(False)
        def authorize(action,first,second,database,trigger):
            if action==sqlite3.SQLITE_SELECT:return sqlite3.SQLITE_OK
            if action==sqlite3.SQLITE_FUNCTION and (second or '').lower() in ('like','glob'):
                return sqlite3.SQLITE_OK
            return sqlite3.SQLITE_DENY
        connection.set_authorizer(authorize)
        remaining=100
        def progress():
            nonlocal remaining
            remaining-=1
            return int(remaining<=0)
        connection.set_progress_handler(progress,1000)
        try:
            cursor=connection.execute(sql)
            rows=cursor.fetchmany(3)
            if len(rows)>1 or any(len(row)>1 for row in rows):raise ValueError('scalar probe result exceeded bounds')
            cells=[[(type(value).__name__,value) for value in row] for row in rows]
            return dict(accepted=True,typed_rows=cells)
        except sqlite3.Error as error:
            return dict(accepted=False,error=str(error)[:2000],
                        code=getattr(error,'sqlite_errorcode',None),name=getattr(error,'sqlite_errorname',None))


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--compiler',type=Path,default=Path('build-grammar-audit/bin/navidog-sql'))
    parser.add_argument('--input',type=Path,default=Path('.cache/generated-sqlite-operators.jsonl'))
    parser.add_argument('--output',type=Path,default=Path('.cache/sqlite-operator-roundtrip.jsonl'))
    args=parser.parse_args()
    spec=importlib.util.spec_from_file_location('independent_generator',Path(__file__).with_name('generate-sql-grammar-probes.py'))
    generator=importlib.util.module_from_spec(spec)
    spec.loader.exec_module(generator)
    allowed={row['sql_sha256']:row for row in generator.sqlite_operator_cases()}
    input_bytes=args.input.read_bytes()
    rows=list(map(json.loads,input_bytes.decode().splitlines()))
    if not rows or any(row.get('generator')!='navidog-independent-sqlite-operators-v1' or
                      row.get('dialect')!='sqlite' or
                      allowed.get(row.get('sql_sha256'),{}).get('sql')!=row.get('sql') for row in rows):
        parser.error('only exact locally authored SQLite operator cases are accepted')
    with closing(sqlite3.connect(':memory:')) as connection:
        source_id=connection.execute('SELECT sqlite_source_id()').fetchone()[0]
    identity=dict(version=sqlite3.sqlite_version,source_id=source_id,
                  scope='authored scalar expression values and storage categories; no column label comparison')
    args.output.parent.mkdir(parents=True,exist_ok=True)
    counts=Counter()
    with tempfile.TemporaryDirectory(prefix='nvg-sqlite-compiler-',dir=args.output.parent) as directory, args.output.open('w') as output:
        compiler=Path(directory)/'navidog-sql'
        shutil.copy2(args.compiler.resolve(),compiler)
        compiler_sha=hashlib.sha256(compiler.read_bytes()).hexdigest()
        for index,row in enumerate(rows):
            result=dict(row,native_parser=identity,compiler_sha256=compiler_sha)
            original=native(row['sql'])
            result['native_original']=original
            counts['native-accepted' if original['accepted'] else 'native-rejected']+=1
            try:
                run=subprocess.run([str(compiler),'--dialect','sqlite','--ast','-'],
                    input=row['sql'],capture_output=True,text=True,timeout=5)
                if run.returncode not in (0,1):raise ValueError('compiler exited abnormally')
                own=json.loads(run.stdout)
                result.update(local_ok=own['ok'],local_syntax_valid=own['syntaxValid'],local_diagnostic=run.stderr[:2000])
                if not original['accepted'] and own['ok']:
                    counts['native-rejected-local-accepted-review']+=1
                    result['native_rejected_local_accepted_review']=True
                if own['ok']:
                    generated=subprocess.run([str(compiler),'--dialect','sqlite','--emit','sqlite','-'],
                        input=row['sql'],capture_output=True,text=True,timeout=5)
                    if generated.returncode or not generated.stdout.strip():
                        result['generation_deferred']=generated.stderr[:2000]
                    elif len(generated.stdout.encode())>262144:
                        raise ValueError('generated SQL exceeds probe bounds')
                    else:
                        result['generated_sql']=generated.stdout
                        rewritten=native(generated.stdout)
                        result['native_generated']=rewritten
                        if original['accepted'] and rewritten['accepted']:
                            same=original['typed_rows']==rewritten['typed_rows']
                            result['native_result_equal']=same
                            counts['typed-result-agreement' if same else 'typed-result-review']+=1
                        elif original['accepted']!=rewritten['accepted']:
                            counts['native-acceptance-review']+=1
                elif original['accepted']:
                    counts['native-accepted-local-rejected-review']+=1
            except (subprocess.SubprocessError,ValueError) as error:
                result['harness_error']=str(error)[:1000]
                counts['harness-error']+=1
            output.write(json.dumps(result,ensure_ascii=False)+'\n')
            if (index+1)%200==0:
                output.flush()
                print(json.dumps(dict(completed=index+1,total=len(rows),summary=dict(counts))),flush=True)
    summary=dict(cases=len(rows),summary=dict(counts),native=identity,compiler_sha256=compiler_sha,
                 input_sha256=hashlib.sha256(input_bytes).hexdigest(),output=str(args.output))
    args.output.with_suffix('.summary.json').write_text(json.dumps(summary,indent=2)+'\n')
    print(json.dumps(summary))
    if any(counts[key] for key in ('typed-result-review','native-acceptance-review','harness-error')):raise SystemExit(1)


if __name__=='__main__':main()
