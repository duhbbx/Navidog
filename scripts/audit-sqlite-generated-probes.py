#!/usr/bin/env python3
"""Compile independently authored SQLite combinations using EXPLAIN in memory.

No downloaded SQL is an input: every statement must match the local independent
generator's reviewed SQL and digest. EXPLAIN emits VDBE instructions without
executing the query body. Success covers prepare/binding, not runtime coercion.
Each case gets a fresh connection; extensions remain disabled.
"""
from __future__ import annotations

import argparse
from collections import Counter
from contextlib import closing
import importlib.util
import json
from pathlib import Path
import sqlite3


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--input',type=Path,default=Path('.cache/sql-grammar-20261127/generated-sqlite.jsonl'))
    parser.add_argument('--local-report',type=Path)
    parser.add_argument('--output',type=Path,default=Path('.cache/sql-grammar-20261127/generated-sqlite-native.jsonl'))
    args = parser.parse_args()
    spec = importlib.util.spec_from_file_location('independent_generator',Path(__file__).with_name('generate-sql-grammar-probes.py'))
    generator = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(generator)
    allowed = {row['sql_sha256']:row['sql'] for row in generator.sqlite_pagination_cases()}
    rows = list(map(json.loads,args.input.read_text().splitlines()))
    if any(row.get('generator')!='navidog-independent-sqlite-pagination-v1' or
           row.get('dialect')!='sqlite' or allowed.get(row.get('sql_sha256'))!=row.get('sql') for row in rows):
        parser.error('only exact locally authored SQLite generator cases are accepted')
    local = {}
    if args.local_report:
        local = {row['sql_sha256']:row for row in map(json.loads,args.local_report.read_text().splitlines())
                 if row['dialect']=='sqlite'}
    with closing(sqlite3.connect(':memory:')) as connection:
        source_id = connection.execute('SELECT sqlite_source_id()').fetchone()[0]
    identity = dict(version=sqlite3.sqlite_version,source_id=source_id,mode='EXPLAIN prepare only')
    counters = Counter()
    args.output.parent.mkdir(parents=True,exist_ok=True)
    with args.output.open('w') as output:
        for row in rows:
            result = dict(row,native_parser=identity)
            with closing(sqlite3.connect(':memory:')) as connection:
                try:
                    if hasattr(connection,'enable_load_extension'):
                        connection.enable_load_extension(False)
                    bytecode = connection.execute('EXPLAIN '+row['sql']).fetchall()
                    native = dict(accepted=True,instructions=len(bytecode))
                except sqlite3.Error as error:
                    native = dict(accepted=False,error=str(error)[:2000],
                        code=getattr(error,'sqlite_errorcode',None),name=getattr(error,'sqlite_errorname',None))
            result['native_result'] = native
            counters['native-prepare-accepted' if native['accepted'] else 'native-prepare-rejected'] += 1
            own = local.get(row['sql_sha256'])
            if own:
                result['local_status'] = own['local_status']
                if own.get('local_ok') and not native['accepted']:
                    result['native_rejected_local_accepted'] = True
                    counters['native-rejected-local-accepted'] += 1
            output.write(json.dumps(result,ensure_ascii=False)+'\n')
    print(json.dumps(dict(cases=len(rows),native=identity,summary=dict(counters),output=str(args.output))))


if __name__=='__main__':
    main()
