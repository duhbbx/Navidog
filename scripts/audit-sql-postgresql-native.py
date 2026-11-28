#!/usr/bin/env python3
"""Compare cached/authored PostgreSQL with an optional pinned raw parser.

Only SQL recognition runs. No database, imported upstream tests or SQL execution
is involved. Raw parsing leaves literal routine bodies opaque; those expectations
are retained but are not compared as though their bodies had been validated.
"""
from __future__ import annotations

import argparse
from collections import Counter
import hashlib
import importlib.util
import json
from pathlib import Path
import re
import subprocess
import sys


def raw_body(sql):
    index = 0
    while index<len(sql):
        if sql[index].isspace():
            index += 1
        elif sql.startswith('--',index):
            end = sql.find('\n',index)
            index = len(sql) if end<0 else end+1
        elif sql.startswith('/*',index):
            depth,index = 1,index+2
            while index<len(sql) and depth:
                if sql.startswith('/*',index):
                    depth,index = depth+1,index+2
                elif sql.startswith('*/',index):
                    depth,index = depth-1,index+2
                else:
                    index += 1
        else:
            break
    sql = sql[index:]
    return bool(re.match(r'\s*(?:CREATE\s+(?:OR\s+REPLACE\s+)?(?:FUNCTION|PROCEDURE)\b|DO\b)',sql,re.I))


def batches(rows):
    batch, byte_count = [],0
    for index,row in enumerate(rows):
        request = json.dumps(dict(index=index,sql=row['sql']),ensure_ascii=False)+'\n'
        size = len(request.encode())
        if batch and (len(batch)>=64 or byte_count+size>4*1024*1024):
            yield batch
            batch,byte_count = [],0
        batch.append((index,row,request))
        byte_count += size
    if batch:
        yield batch


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--python',default=sys.executable)
    parser.add_argument('--probe',type=Path,default=Path('scripts/sql-grammar/postgresql-native/probe.py'))
    parser.add_argument('--input',type=Path,default=Path('.cache/sql-grammar-20261127/candidates.jsonl'))
    parser.add_argument('--local-report',type=Path)
    parser.add_argument('--fixtures',action='store_true')
    parser.add_argument('--limit',type=int)
    parser.add_argument('--output',type=Path,default=Path('.cache/sql-grammar-20261127/postgresql-native-audit.jsonl'))
    args = parser.parse_args()
    if args.limit is not None and args.limit<1:
        parser.error('limit must be positive')
    command = [args.python,'-I','-B',str(args.probe.resolve())]
    run = subprocess.run(command+['--version'],text=True,capture_output=True,check=True,timeout=10)
    identity = json.loads(run.stdout)
    lock = json.loads(Path('tests/sql/corpus/reference-sources.json').read_text())['native_parser_tools']['postgresql']
    if identity.get('native_sha256')!=lock['native_sha256'] or identity.get('version')!=lock['version']:
        parser.error('PostgreSQL raw parser identity does not match source lock')
    rows = [row for row in map(json.loads,args.input.read_text().splitlines()) if row['dialect']=='postgresql']
    if args.fixtures:
        spec = importlib.util.spec_from_file_location('grammar_reader',Path(__file__).with_name('audit-sql-antlr.py'))
        reader = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(reader)
        for fixture in reader.fixtures(Path('tests/sql/syntax'),'postgresql'):
            fixture.update(dialect='postgresql',authored=True,
                sql_sha256=hashlib.sha256(fixture['sql'].encode()).hexdigest())
            rows.append(fixture)
    if args.limit:
        rows = rows[:args.limit]
    local = {}
    if args.local_report:
        local = {row['sql_sha256']:row for row in map(json.loads,args.local_report.read_text().splitlines())
                 if row['dialect']=='postgresql'}
    counters = Counter()
    args.output.parent.mkdir(parents=True,exist_ok=True)
    with args.output.open('w') as output:
        for batch in batches(rows):
            # COPY stdin/psql dependency is a collection boundary, not native SQL.
            active = [entry for entry in batch if not entry[1].get('requires_harness')]
            responses,failure = {},''
            if active:
                try:
                    run = subprocess.run(command,input=''.join(entry[2] for entry in active),
                        text=True,capture_output=True,check=True,timeout=20)
                    replies = [json.loads(line) for line in run.stdout.splitlines()]
                    if len(replies)!=len(active) or any(reply.get('index')!=index or
                        not isinstance(reply.get('accepted'),bool) for (index,_,_),reply in zip(active,replies)):
                        raise ValueError('PostgreSQL native probe protocol mismatch')
                    responses = {reply['index']:reply for reply in replies}
                except (subprocess.SubprocessError,ValueError) as error:
                    failure = str(error)[:1000]
            for index,row,_ in batch:
                result = dict(row,native_parser=identity)
                if row.get('requires_harness'):
                    result['native_status'] = 'harness-dependent'
                    counters['harness-dependent'] += 1
                else:
                    native = responses.get(index) if not failure else dict(harness_error=True,error=failure)
                    result['native_result'] = native
                    if native.get('harness_error'):
                        counters['harness-error'] += 1
                    else:
                        accepted = native['accepted']
                        counters['native-accepted' if accepted else 'native-rejected'] += 1
                        want = row.get('upstream_parser_accepts')
                        if row.get('authored'):
                            want = None if row['expected']=='semantic-error' else row['expected']=='valid'
                        if raw_body(row['sql']):
                            want = None
                            result['raw_routine_body_opaque'] = True
                            counters['raw-routine-body-opaque'] += 1
                        if want is not None:
                            review = accepted!=want
                            result['native_expectation_review'] = review
                            counters['native-expectation-review' if review else 'native-expectation-agreement'] += 1
                        own = local.get(row['sql_sha256'])
                        if own:
                            result['local_status'] = own['local_status']
                            result['local_syntax_valid'] = own.get('local_syntax_valid')
                            if own.get('local_ok') and not accepted:
                                result['native_rejected_local_accepted'] = True
                                counters['native-rejected-local-accepted'] += 1
                output.write(json.dumps(result,ensure_ascii=False)+'\n')
    print(json.dumps(dict(cases=len(rows),summary=dict(counters),output=str(args.output))))
    if counters['harness-error']:
        raise SystemExit(1)


if __name__=='__main__':
    main()
