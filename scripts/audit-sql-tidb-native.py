#!/usr/bin/env python3
"""Compare cached/authored TiDB SQL with the pinned native parser, offline.

This launches only the optional parser probe, not a TiDB server or any SQL
execution engine. A native parser rejection is not automatically a Navidog
syntax-stage assertion: parser actions can also check shape and SQL modes.
"""
from __future__ import annotations

import argparse
from collections import Counter
import hashlib
import importlib.util
import json
import os
from pathlib import Path
import subprocess


def case_key(case):
    return case['sql_sha256'],case.get('mysql_sql_mode','')


def batches(rows, size):
    batch, byte_count = [], 0
    for index, case in enumerate(rows):
        line = json.dumps({"index": index,"sql": case['sql'],
            "mode":case.get('mysql_sql_mode','')},ensure_ascii=False) + '\n'
        encoded = len(line.encode())
        if batch and (len(batch) == size or byte_count + encoded > 4 * 1024 * 1024):
            yield batch
            batch, byte_count = [], 0
        batch.append((index,case,line))
        byte_count += encoded
    if batch:
        yield batch


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--probe',type=Path,default=Path('.cache/tidb-native-probe/tidb-probe'))
    parser.add_argument('--input',type=Path,default=Path('.cache/sql-grammar-20261127/candidates.jsonl'))
    parser.add_argument('--local-report',type=Path)
    parser.add_argument('--fixtures',action='store_true')
    parser.add_argument('--limit',type=int)
    parser.add_argument('--batch-size',type=int,default=64)
    parser.add_argument('--output',type=Path,default=Path('.cache/sql-grammar-20261127/tidb-native-audit.jsonl'))
    args = parser.parse_args()
    if not 1 <= args.batch_size <= 256 or (args.limit is not None and args.limit < 1):
        parser.error('batch size must be 1..256 and limit positive')
    probe = args.probe.resolve()
    identity = subprocess.run([str(probe),'--version'],capture_output=True,text=True,check=True,timeout=5)
    identity = json.loads(identity.stdout)
    lock = json.loads(Path('tests/sql/corpus/reference-sources.json').read_text())
    if identity.get('revision') != lock['sources']['tidb']['revision']:
        parser.error('probe revision does not match the pinned native grammar')
    rows = [row for row in map(json.loads,args.input.read_text().splitlines()) if row['dialect']=='tidb']
    if args.fixtures:
        spec = importlib.util.spec_from_file_location('antlr_fixture_reader',Path(__file__).with_name('audit-sql-antlr.py'))
        reader = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(reader)
        for fixture in reader.fixtures(Path('tests/sql/syntax'),'tidb'):
            fixture.update(dialect='tidb',authored=True,
                sql_sha256=hashlib.sha256(fixture['sql'].encode()).hexdigest())
            rows.append(fixture)
    if args.limit:
        rows = rows[:args.limit]
    local = {}
    if args.local_report:
        for row in map(json.loads,args.local_report.read_text().splitlines()):
            if row['dialect']=='tidb':
                local[case_key(row)] = row
    environment = dict(os.environ,GOMEMLIMIT='512MiB',GOMAXPROCS='2')
    args.output.parent.mkdir(parents=True,exist_ok=True)
    counters = Counter()
    with args.output.open('w') as output:
        for batch in batches(rows,args.batch_size):
            responses = []
            harness_error = ''
            try:
                run = subprocess.run([str(probe)],input=''.join(line for _,_,line in batch),
                    capture_output=True,text=True,timeout=20,env=environment,check=True)
                responses = [json.loads(line) for line in run.stdout.splitlines()]
                if len(responses)!=len(batch) or any(response.get('index')!=index or
                    not isinstance(response.get('accepted'),bool)
                    for (index,_,_),response in zip(batch,responses)):
                    raise ValueError('native response protocol mismatch')
            except (subprocess.SubprocessError,ValueError) as error:
                harness_error = str(error)[:1000]
            for position,(_,case,_) in enumerate(batch):
                result = dict(case)
                native = responses[position] if not harness_error else {'harness_error':True,'error':harness_error}
                result['native_parser'] = identity
                result['native_result'] = native
                if native.get('harness_error'):
                    counters['harness-error'] += 1
                else:
                    accepted = native['accepted']
                    counters['native-accepted' if accepted else 'native-rejected'] += 1
                    expected = case.get('upstream_parser_accepts')
                    if case.get('authored'):
                        expected = None if case['expected']=='semantic-error' else case['expected']=='valid'
                    if expected is not None:
                        key = 'native-expectation-agreement' if accepted==expected else 'native-expectation-review'
                        result['native_expectation_review'] = accepted!=expected
                        counters[key] += 1
                    own = local.get(case_key(case))
                    if own:
                        result['local_status'] = own['local_status']
                        result['local_ok']=own.get('local_ok')
                        result['local_compiler_sha256']=own.get('compiler_sha256')
                        result['local_syntax_valid'] = own.get('local_syntax_valid')
                        result['local_statement_count']=own.get('local_statement_count')
                        if own.get('local_ok') and accepted and own.get('local_statement_count') is not None and own['local_statement_count']!=native.get('statements'):
                            result['native_statement_count_review']=True
                            counters['native-statement-count-review']+=1
                        if own.get('local_ok') and not accepted:
                            counters['native-rejected-local-accepted-review']+=1
                            result['native_rejected_local_accepted_review']=True
                        if own.get('local_syntax_valid') is not None and accepted!=own['local_syntax_valid']:
                            counters['native-local-syntax-review'] += 1
                            result['native_local_syntax_review'] = True
                output.write(json.dumps(result,ensure_ascii=False)+'\n')
    print(json.dumps({'cases':len(rows),'summary':dict(counters),'output':str(args.output)}))
    if counters['harness-error']:
        raise SystemExit(1)


if __name__=='__main__':
    main()
