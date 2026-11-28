#!/usr/bin/env python3
"""Group outstanding native PostgreSQL tree differences for manual review.

Reparses stored original/generated SQL with the verified offline probe and
checks both hashes against the existing report. Only source positions are
normalized; no semantic nodes are dropped, no SQL executes, and no review
ledger entry or fixture expectation is created. Group names are triage hints,
not equivalence proofs. Large SQL bodies are explicitly deferred.
"""
from __future__ import annotations

import argparse
from collections import Counter
import hashlib
import json
from pathlib import Path
import subprocess
import sys


def difference_paths(left,right,limit=256):
    pending=[('$',left,right)]
    paths=[]
    while pending and len(paths)<limit:
        path,a,b=pending.pop()
        if type(a) is not type(b):paths.append(path);continue
        if isinstance(a,dict):
            for key in sorted(a.keys()|b.keys(),reverse=True):
                if key not in a or key not in b:paths.append(path+'.'+key)
                else:pending.append((path+'.'+key,a[key],b[key]))
        elif isinstance(a,list):
            if len(a)!=len(b):paths.append(path+'.length')
            for index in reversed(range(min(len(a),len(b)))):
                pending.append((f'{path}[{index}]',a[index],b[index]))
        elif a!=b:paths.append(path)
    return sorted(set(paths[:limit])),bool(pending) or len(paths)>limit


def groups(paths):
    labels=set()
    for path in paths:
        if '.over.' in path or '.windowClause' in path:labels.add('window-definition-structure')
        elif '.ResTarget.name' in path:labels.add('projection-label')
        elif '.fromClause' in path:labels.add('source-and-join-structure')
        elif '.sortby_dir' in path:labels.add('ordering-spelling')
        elif '.constraints' in path:labels.add('constraint-list-structure')
        else:labels.add('other-structural-field')
    return sorted(labels)


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--input',type=Path,default=Path('.cache/postgresql-roundtrip-cached-final23.jsonl'))
    parser.add_argument('--output',type=Path,default=Path('.cache/postgresql-review-groups.json'))
    parser.add_argument('--python',default=sys.executable)
    args=parser.parse_args()
    raw=args.input.read_bytes()
    candidates=[row for row in map(json.loads,raw.decode().splitlines())
                if row.get('native_ast_equal') is False and not row.get('reviewed_representation_difference')]
    command=[args.python,'-I','-B',str(Path('scripts/sql-grammar/postgresql-native/probe.py').resolve())]
    identity=json.loads(subprocess.run(command+['--version'],capture_output=True,text=True,check=True,timeout=10).stdout)
    algorithm='postgresql-raw-tree-parseloc-v4'
    rows=[]
    totals=Counter()
    for start in range(0,len(candidates),4):
        batch=candidates[start:start+4]
        requests=[]
        for offset,row in enumerate(batch):
            if row['native_parser']['native_sha256']!=identity['native_sha256'] or \
                    row['native_original'].get('fingerprint_algorithm')!=algorithm or \
                    row['native_generated'].get('fingerprint_algorithm')!=algorithm:
                raise ValueError('review report native identity/algorithm does not match')
            if hashlib.sha256(row['sql'].encode()).hexdigest()!=row['sql_sha256']:
                raise ValueError('stored source SQL digest mismatch')
            for side,sql in enumerate((row['sql'],row['generated_sql'])):
                if len(sql.encode())<=8192:
                    requests.append(dict(index=offset*2+side,sql=sql,fingerprint=True,include_normalized_ast=True))
        replies={}
        if requests:
            run=subprocess.run(command,input=''.join(json.dumps(row)+'\n' for row in requests),
                               capture_output=True,text=True,check=True,timeout=20)
            decoded=list(map(json.loads,run.stdout.splitlines()))
            if len(decoded)!=len(requests) or any(reply['index']!=request['index'] for reply,request in zip(decoded,requests)):
                raise ValueError('native review response protocol mismatch')
            replies={reply['index']:reply for reply in decoded}
        for offset,row in enumerate(batch):
            result={key:row[key] for key in ('sql_sha256','compiler_sha256')}
            result.update(source_url=row.get('source_url'),source_path=row.get('source_path'),line=row.get('line'),
                          original_ast_sha256=row['native_original']['ast_sha256'],
                          generated_ast_sha256=row['native_generated']['ast_sha256'],
                          generated_sql_sha256=hashlib.sha256(row['generated_sql'].encode()).hexdigest(),
                          review_status='unreviewed')
            original,generated=replies.get(offset*2),replies.get(offset*2+1)
            if not original or not generated:
                result['deferred']='original or generated SQL exceeds bounded tree-output limit'
                totals['large-sql-deferred']+=1
            else:
                for expected,reply in [(row['native_original'],original),(row['native_generated'],generated)]:
                    if reply.get('harness_error') or not reply['accepted'] or reply.get('ast_sha256')!=expected['ast_sha256']:
                        raise ValueError('native tree changed while reproducing review report')
                paths,truncated=difference_paths(original['normalized_ast'],generated['normalized_ast'])
                if not paths:raise ValueError('different native hashes produced no structural difference')
                result.update(difference_paths=paths,paths_truncated=truncated,triage_groups=groups(paths))
                totals.update(result['triage_groups'])
            rows.append(result)
        print(json.dumps(dict(completed=len(rows),total=len(candidates))),flush=True)
    report=dict(schema_version=1,native_parser=identity,fingerprint_algorithm=algorithm,
                input_sha256=hashlib.sha256(raw).hexdigest(),cases=len(rows),
                note='Triage only. Every entry remains unreviewed; no structural field is suppressed and no equivalence is asserted.',
                triage_summary=dict(totals),entries=rows)
    args.output.parent.mkdir(parents=True,exist_ok=True)
    args.output.write_text(json.dumps(report,ensure_ascii=False,indent=2)+'\n')
    print(json.dumps(dict(cases=len(rows),triage_summary=dict(totals),output=str(args.output))))


if __name__=='__main__':main()
