#!/usr/bin/env python3
"""Compare original/generated PostgreSQL native raw trees, without SQL execution.

Inputs are independently authored fixtures and operator combinations. A different
raw tree is a review item, not an automatic declaration of different semantics.
Only source positions are normalized; literal routine bodies remain opaque.
"""
from __future__ import annotations

import argparse
from collections import Counter
from concurrent.futures import ThreadPoolExecutor
import hashlib
import importlib.util
import json
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile


def load(name):
    spec=importlib.util.spec_from_file_location(name.replace('-','_'),Path(__file__).with_name(name+'.py'))
    module=importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--compiler',type=Path,default=Path('build-grammar-audit/bin/navidog-sql'))
    parser.add_argument('--python',default=sys.executable)
    parser.add_argument('--suite',action='append',choices=['fixtures','operators','cached','ddl-probes'])
    parser.add_argument('--input',type=Path,default=Path('.cache/sql-grammar-20261127/candidates.jsonl'))
    parser.add_argument('--reviews',type=Path,default=Path('tests/sql/corpus/postgresql-roundtrip-reviews.json'))
    parser.add_argument('--jobs',type=int,default=4)
    parser.add_argument('--output',type=Path,default=Path('.cache/postgresql-roundtrip.jsonl'))
    args=parser.parse_args()
    if not 1<=args.jobs<=8:parser.error('jobs must be 1..8')
    reference=load('audit-sql-postgresql-native')
    reader=load('audit-sql-antlr')
    generator=load('generate-sql-grammar-probes')
    suites=args.suite or ['fixtures','operators']
    rows=[]
    if 'cached' in suites:
        rows.extend(row for row in map(json.loads,args.input.read_text().splitlines()) if row['dialect']=='postgresql')
    if 'fixtures' in suites:
        for row in reader.fixtures(Path('tests/sql/syntax'),'postgresql'):
            row.update(authored=True,sql_sha256=hashlib.sha256(row['sql'].encode()).hexdigest())
            rows.append(row)
    if 'operators' in suites:rows.extend(generator.postgresql_operator_cases())
    if 'ddl-probes' in suites:
        source=Path('tests/sql/corpus/postgresql-ddl-probes.json')
        for case in json.loads(source.read_text())['cases']:
            expected='valid' if case['expect']=='accept' else 'semantic-error' if case.get('stage')=='semantic' else 'syntax-error'
            rows.append(dict(name=case['id'],path=str(source),sql=case['sql'],expected=expected,
                authored=True,sql_sha256=hashlib.sha256(case['sql'].encode()).hexdigest()))
    command=[args.python,'-I','-B',str(Path('scripts/sql-grammar/postgresql-native/probe.py').resolve())]
    identity=json.loads(subprocess.run(command+['--version'],capture_output=True,text=True,check=True,timeout=10).stdout)
    reviews,ledger={},{}
    if args.reviews.is_file():
        ledger=json.loads(args.reviews.read_text())
        if ledger.get('native_sha256')==identity['native_sha256']:
            for entry in ledger['entries']:
                key=tuple(entry[field] for field in ('source_sql_sha256','original_ast_sha256','generated_ast_sha256'))
                if key in reviews or any(len(value)!=64 or any(c not in '0123456789abcdef' for c in value) for value in key):
                    raise ValueError('invalid or duplicate reviewed native-tree identity')
                reviews[key]=entry
    args.output.parent.mkdir(parents=True,exist_ok=True)
    counts=Counter()
    with tempfile.TemporaryDirectory(prefix='nvg-pg-compiler-',dir=args.output.parent) as directory, \
         args.output.open('w') as output,ThreadPoolExecutor(max_workers=args.jobs) as pool:
        compiler=Path(directory)/'navidog-sql'
        shutil.copy2(args.compiler.resolve(),compiler)
        compiler_sha=hashlib.sha256(compiler.read_bytes()).hexdigest()
        def inspect(row):
            result=dict(row)
            if row.get('requires_harness') or reference.raw_body(row['sql']):
                result['comparison_deferred']='harness or opaque routine body'
                return result
            try:
                own=subprocess.run([str(compiler),'--dialect','postgresql','--ast','-'],
                    input=row['sql'],capture_output=True,text=True,timeout=5)
                if own.returncode not in (0,1):raise ValueError('compiler exited abnormally')
                parsed=json.loads(own.stdout)
                result.update(local_ok=parsed['ok'],local_syntax_valid=parsed['syntaxValid'],local_diagnostic=own.stderr[:2000])
                if parsed['ok']:
                    generated=subprocess.run([str(compiler),'--dialect','postgresql','--emit','postgresql','-'],
                        input=row['sql'],capture_output=True,text=True,timeout=5)
                    if generated.returncode or not generated.stdout.strip():
                        result['generation_deferred']=generated.stderr[:1000]
                    elif len(generated.stdout.encode())>262144:
                        result['generation_deferred']='generated SQL exceeds native probe limit'
                    else:result['generated_sql']=generated.stdout
            except (subprocess.SubprocessError,ValueError) as error:
                result['harness_error']=str(error)[:1000]
            return result
        for offset in range(0,len(rows),32):
            batch=list(pool.map(inspect,rows[offset:offset+32]))
            requests=[]
            for index,row in enumerate(batch):
                if row.get('comparison_deferred') or row.get('harness_error'):continue
                requests.append(dict(sql=row['sql'],request_index=2*index,fingerprint=True))
                if row.get('generated_sql'):
                    requests.append(dict(sql=row['generated_sql'],request_index=2*index+1,fingerprint=True))
            replies={}
            for pack in reference.batches(requests):
                payload=''.join(json.dumps(dict(index=row['request_index'],sql=row['sql'],fingerprint=True),ensure_ascii=False)+'\n'
                    for _,row,_ in pack)
                run=subprocess.run(command,input=payload,text=True,capture_output=True,check=True,timeout=20)
                parsed=[json.loads(line) for line in run.stdout.splitlines()]
                if len(parsed)!=len(pack) or any(reply.get('index')!=row['request_index'] or
                        not isinstance(reply.get('accepted'),bool) for (_,row,_),reply in zip(pack,parsed)):
                    raise ValueError('native round-trip response protocol mismatch')
                replies.update((reply['index'],reply) for reply in parsed)
            for index,row in enumerate(batch):
                row.update(native_parser=identity,compiler_sha256=compiler_sha)
                if row.get('comparison_deferred'):counts['deferred']+=1
                elif row.get('harness_error'):counts['harness-error']+=1
                else:
                    original=replies[2*index]
                    row['native_original']=original
                    if original.get('harness_error'):counts['harness-error']+=1
                    else:
                        counts['native-accepted' if original['accepted'] else 'native-rejected']+=1
                        if not original['accepted'] and row.get('local_syntax_valid'):
                            row['native_rejected_local_syntax_valid']=True
                            counts['native-rejected-local-syntax-valid']+=1
                        generated=replies.get(2*index+1)
                        if generated:
                            row['native_generated']=generated
                            if generated.get('harness_error'):counts['harness-error']+=1
                            elif original['accepted'] and generated['accepted']:
                                same=original['ast_sha256']==generated['ast_sha256']
                                row['native_ast_equal']=same
                                key=(row['sql_sha256'],original['ast_sha256'],generated['ast_sha256'])
                                reviewed=reviews.get(key) if original.get('fingerprint_algorithm')==ledger.get('fingerprint_algorithm') else None
                                if not same and reviewed:
                                    row['reviewed_representation_difference']=reviewed
                                    counts['reviewed-native-tree-difference']+=1
                                else:counts['native-tree-agreement' if same else 'native-tree-review']+=1
                            elif original['accepted']!=generated['accepted']:
                                counts['native-acceptance-review']+=1
                output.write(json.dumps(row,ensure_ascii=False)+'\n')
            output.flush()
            print(json.dumps(dict(completed=min(offset+32,len(rows)),total=len(rows),summary=dict(counts))),flush=True)
    summary=dict(cases=len(rows),summary=dict(counts),output=str(args.output),native_parser=identity,compiler_sha256=compiler_sha)
    args.output.with_suffix('.summary.json').write_text(json.dumps(summary,indent=2)+'\n')
    print(json.dumps(summary))
    if any(counts[key] for key in ['harness-error','native-tree-review','native-acceptance-review','native-rejected-local-syntax-valid']):
        raise SystemExit(1)


if __name__=='__main__':main()
