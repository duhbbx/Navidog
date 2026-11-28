#!/usr/bin/env python3
"""Propose exact review entries for independently authored PG window probes.

This is deliberately restricted to tests/sql/syntax/postgresql/window-roundtrip:
only matching authored SQL, identical generated SQL, passed owned-server results
and unchanged native hashes can produce a proposal. It expands just the native
root SELECT's named-window definitions and requires the entire resulting native
tree to equal the generated tree. It never writes the tracked review ledger.
"""
from __future__ import annotations

import argparse
import copy
import hashlib
import json
from pathlib import Path
import subprocess
import sys

NONDEFAULT_FRAME=0x00001 # Pinned PostgreSQL parsenodes.h FRAMEOPTION_NONDEFAULT.

def digest(tree):
    return hashlib.sha256(json.dumps(tree,sort_keys=True,separators=(',',':'),ensure_ascii=False).encode()).hexdigest()


def expand_root_windows(tree):
    result=copy.deepcopy(tree)
    if len(result.get('stmts',[]))!=1:raise ValueError('window proof requires exactly one statement')
    select=result['stmts'][0]['stmt']['SelectStmt']
    definitions={value['WindowDef']['name']:value['WindowDef'] for value in select['windowClause']}
    if len(definitions)!=len(select['windowClause']):raise ValueError('duplicate named window')
    fields={'name','refname','partitionClause','orderClause','frameOptions','startOffset','endOffset'}
    resolved,active,used={},set(),set()
    def inherit(child,parent):
        if set(child)-fields:raise ValueError('unexpected native window field')
        if child.get('partitionClause') or (parent.get('orderClause') and child.get('orderClause')) or parent['frameOptions']&NONDEFAULT_FRAME:
            raise ValueError('invalid native window-copy relationship')
        value=copy.deepcopy(parent)
        for field in ('orderClause','frameOptions','startOffset','endOffset'):
            if field in child:value[field]=copy.deepcopy(child[field])
        return value
    def resolve(name):
        used.add(name)
        if name in resolved:return copy.deepcopy(resolved[name])
        if name in active or name not in definitions:raise ValueError('cyclic or absent window reference')
        active.add(name)
        child=definitions[name]
        if set(child)-fields:raise ValueError('unexpected native window field')
        if child.get('refname'):
            value=inherit(child,resolve(child['refname']))
        else:
            value=copy.deepcopy(child)
        value.pop('name',None);value.pop('refname',None)
        active.remove(name);resolved[name]=value
        return copy.deepcopy(value)
    def visit(value):
        if isinstance(value,dict):
            if 'FuncCall' in value and 'over' in value['FuncCall']:
                over=value['FuncCall']['over']
                if over.get('name'):
                    if set(over)-{'name','frameOptions'}:raise ValueError('direct named window unexpectedly has modifiers')
                    value['FuncCall']['over']=resolve(over['name'])
                elif over.get('refname'):
                    value['FuncCall']['over']=inherit(over,resolve(over['refname']))
                    value['FuncCall']['over'].pop('name',None);value['FuncCall']['over'].pop('refname',None)
            for child in value.values():visit(child)
        elif isinstance(value,list):
            for child in value:visit(child)
    visit(select['targetList'])
    if used!=set(definitions):raise ValueError('proof does not admit unused named definitions')
    select.pop('windowClause')
    return result


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--input',type=Path,default=Path('.cache/postgresql-roundtrip-final25.jsonl'))
    parser.add_argument('--execution',type=Path,default=Path('.cache/grammar-native-pg-window-final.json'))
    parser.add_argument('--output',type=Path,default=Path('.cache/postgresql-window-review-proposal.json'))
    parser.add_argument('--python',default=sys.executable)
    args=parser.parse_args()
    authored={row['id']:row for row in json.loads(Path('tests/sql/corpus/native-probes.json').read_text())['cases']
              if row['engine']=='postgresql' and row['id'].startswith('window-roundtrip-')}
    execution=json.loads(args.execution.read_text())
    if not execution.get('isolated') or execution['engine']!='postgresql':raise ValueError('execution evidence must use an owned PostgreSQL server')
    if execution.get('source_sha256')!=hashlib.sha256(Path('tests/sql/corpus/native-probes.json').read_bytes()).hexdigest():
        raise ValueError('execution evidence uses a different authored probe manifest')
    executed={row['id']:row for row in execution['cases']}
    candidates=[row for row in map(json.loads,args.input.read_text().splitlines())
                if row.get('path')=='tests/sql/syntax/postgresql/window-roundtrip.sqltest' and row.get('native_ast_equal') is False]
    command=[args.python,'-I','-B',str(Path('scripts/sql-grammar/postgresql-native/probe.py').resolve())]
    identity=json.loads(subprocess.run(command+['--version'],text=True,capture_output=True,check=True,timeout=10).stdout)
    entries=[]
    for row in candidates:
        if row['native_parser']['native_sha256']!=identity['native_sha256'] or \
                hashlib.sha256(row['sql'].encode()).hexdigest()!=row['sql_sha256'] or \
                row['native_original'].get('fingerprint_algorithm')!='postgresql-raw-tree-parseloc-v4' or \
                row['native_generated'].get('fingerprint_algorithm')!='postgresql-raw-tree-parseloc-v4':
            raise ValueError('input report native identity, algorithm or source hash mismatch')
        case=authored[row['name']]
        proof=executed[row['name']]
        if row['sql'].strip()!=case['sql'].strip() or not proof.get('passed') or not proof.get('round_trip_passed') or \
                proof['generated_sql']!=row['generated_sql'] or proof['sql']!=case['sql']:
            raise ValueError('authored SQL or passed execution/generation evidence mismatch')
        requests=[dict(index=i,sql=sql,fingerprint=True,include_normalized_ast=True)
                  for i,sql in enumerate((row['sql'],row['generated_sql']))]
        run=subprocess.run(command,input=''.join(json.dumps(value)+'\n' for value in requests),
                           text=True,capture_output=True,check=True,timeout=20)
        replies=list(map(json.loads,run.stdout.splitlines()))
        if len(replies)!=2:raise ValueError('native proof response count mismatch')
        for index,reply in enumerate(replies):
            expected=row['native_original' if index==0 else 'native_generated']
            if reply.get('index')!=index or not reply.get('accepted') or reply.get('harness_error') or reply.get('ast_sha256')!=expected['ast_sha256']:
                raise ValueError('native AST identity changed')
        expanded=expand_root_windows(replies[0]['normalized_ast'])
        if digest(expanded)!=replies[1]['ast_sha256']:
            raise ValueError('named-window substitution did not preserve the entire native tree')
        entries.append(dict(case=row['name'],source_path=row['path'],source_sql_sha256=row['sql_sha256'],
            original_ast_sha256=replies[0]['ast_sha256'],generated_ast_sha256=replies[1]['ast_sha256'],
            reason='This exact authored named-window reference/inheritance is expanded to the same native partition/order/frame specification; every other normalized native tree field is identical.',
            evidence=[f"Owned {execution['version']} probe {row['name']} matched original/generated ordered rows with peers, NULL and separated numeric keys.",
                      'Independent native-root WINDOW substitution produced the exact generated raw-tree hash.'],
            native_execution_sql_sha256=hashlib.sha256(proof['sql'].encode()).hexdigest(),
            generated_sql_sha256=hashlib.sha256(row['generated_sql'].encode()).hexdigest()))
    if len(entries)!=len(authored) or {row['case'] for row in entries}!=set(authored):
        raise ValueError('not every authored window probe was verified exactly once')
    proposal=dict(schema_version=1,native_sha256=identity['native_sha256'],fingerprint_algorithm='postgresql-raw-tree-parseloc-v4',
                  note='Exact-case review proposal only; the tracked ledger is not modified.',entries=entries)
    args.output.parent.mkdir(parents=True,exist_ok=True)
    args.output.write_text(json.dumps(proposal,indent=2)+'\n')
    print(json.dumps(dict(verified=len(entries),output=str(args.output))))


if __name__=='__main__':main()
