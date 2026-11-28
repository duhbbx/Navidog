#!/usr/bin/env python3
"""Audit SQL Server candidates/fixtures with the verified offline ScriptDOM tool.

The native parser has version-specific grammar and some shape checks. Neither
its acceptance nor its rejection establishes server execution behavior. Fabric
engine tests keep their original labels but are not compared with the default
standalone/All parser profile.
"""
from __future__ import annotations

import argparse
from collections import Counter
import hashlib
import importlib.util
import json
import os
from pathlib import Path
import re
import shutil
import subprocess


def load_script(name):
    path = Path(__file__).with_name(name)
    spec = importlib.util.spec_from_file_location(path.stem.replace('-','_'),path)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def source_profile(name, default):
    if not name:
        return default,True
    exact = re.fullmatch(r'ErrorTest(\d+)',name)
    if exact:
        return int(exact.group(1)),True
    # These are the actual parser sets in the pinned TestUtilities.cs, rather
    # than a guess from the helper name: Until150 excludes TSql150Parser.
    ranges = {
        'ErrorTestAllParsers': (80,170),
        'ErrorTest90AndAbove': (90,170),
        'ErrorTest100AndAbove': (100,170),
        'ErrorTestAllParsersUntil150': (80,140),
        'ErrorTest90andAboveUntil150': (90,140),
    }
    bounds = ranges.get(name)
    if bounds:
        return max(bounds[0],min(default,bounds[1])),True
    # Fabric and unknown helpers do not establish a standalone parser label.
    return default,False


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--dotnet',default=shutil.which('dotnet') or str(Path.home()/'.dotnet/dotnet'))
    parser.add_argument('--probe',type=Path,default=Path('.cache/sqlserver-native-parser/artifacts/bin/SqlServerProbe/release/SqlServerProbe.dll'))
    parser.add_argument('--input',type=Path,default=Path('.cache/sql-grammar-20261127/candidates.jsonl'))
    parser.add_argument('--local-report',type=Path)
    parser.add_argument('--fixtures',action='store_true')
    parser.add_argument('--profile',type=int,default=160,choices=[80,90,100,110,120,130,140,150,160,170,180])
    parser.add_argument('--uniform-profile',action='store_true')
    parser.add_argument('--limit',type=int)
    parser.add_argument('--output',type=Path,default=Path('.cache/sql-grammar-20261127/scriptdom-audit.jsonl'))
    args = parser.parse_args()
    if args.limit is not None and args.limit<1:
        parser.error('limit must be positive')
    command = [str(Path(shutil.which(args.dotnet) or args.dotnet).resolve()),str(args.probe.resolve())]
    environment = dict(os.environ,DOTNET_CLI_HOME=str(Path('.cache/dotnet-home').resolve()),
        DOTNET_CLI_TELEMETRY_OPTOUT='1',DOTNET_NOLOGO='1',DOTNET_PROCESSOR_COUNT='2',
        DOTNET_GCHeapHardLimit='0x20000000')
    version = subprocess.run(command+['--version'],capture_output=True,text=True,check=True,timeout=10,env=environment)
    identity = json.loads(version.stdout)
    expected = json.loads(Path('tests/sql/corpus/reference-sources.json').read_text())['native_parser_tools']['sqlserver']
    if identity.get('assembly_sha256')!=expected['assembly_sha256'] or identity.get('version')!=expected['version']:
        parser.error('native ScriptDOM identity does not match the reviewed source lock')
    rows = [row for row in map(json.loads,args.input.read_text().splitlines()) if row['dialect']=='sqlserver']
    if args.fixtures:
        reader = load_script('audit-sql-antlr.py')
        for row in reader.fixtures(Path('tests/sql/syntax'),'sqlserver'):
            row.update(dialect='sqlserver',authored=True,
                sql_sha256=hashlib.sha256(row['sql'].encode()).hexdigest())
            rows.append(row)
    if args.limit:
        rows = rows[:args.limit]
    local = {}
    if args.local_report:
        local = {row['sql_sha256']:row for row in map(json.loads,args.local_report.read_text().splitlines())
                 if row['dialect']=='sqlserver'}
    counters = Counter()
    args.output.parent.mkdir(parents=True,exist_ok=True)
    with args.output.open('w') as output:
        for begin in range(0,len(rows),64):
            batch = rows[begin:begin+64]
            requests,comparisons = [],[]
            for position,row in enumerate(batch):
                profile,comparable = source_profile(row.get('upstream_parser_set'),args.profile)
                if row.get('authored') and row.get('native_profile'):
                    profile = row['native_profile']
                if args.uniform_profile:
                    comparable = comparable and profile==args.profile
                    profile = args.profile
                requests.append(dict(index=begin+position,sql=row['sql'],version=profile))
                comparisons.append(comparable)
            payload = ''.join(json.dumps(row,ensure_ascii=False)+'\n' for row in requests)
            failure = ''
            responses = []
            try:
                run = subprocess.run(command,input=payload,capture_output=True,text=True,timeout=30,check=True,env=environment)
                responses = [json.loads(line) for line in run.stdout.splitlines()]
                if len(responses)!=len(batch) or any(response.get('index')!=request['index'] or
                    not isinstance(response.get('accepted'),bool) for request,response in zip(requests,responses)):
                    raise ValueError('native ScriptDOM response protocol mismatch')
            except (subprocess.SubprocessError,ValueError) as error:
                failure = str(error)[:1000]
            for position,row in enumerate(batch):
                result = dict(row)
                native = responses[position] if not failure else {'harness_error':True,'error':failure}
                result.update(native_parser=identity,native_result=native)
                if native.get('harness_error'):
                    counters['harness-error'] += 1
                else:
                    accepted = native['accepted']
                    counters['native-accepted' if accepted else 'native-rejected'] += 1
                    want = row.get('upstream_parser_accepts') if comparisons[position] else None
                    if row.get('authored') and comparisons[position]:
                        want = None if row['expected']=='semantic-error' else row['expected']=='valid'
                    if want is not None:
                        review = accepted!=want
                        result['native_expectation_review'] = review
                        counters['native-expectation-review' if review else 'native-expectation-agreement'] += 1
                    own = local.get(row['sql_sha256'])
                    if own:
                        result['local_status'] = own['local_status']
                        result['local_compiler_sha256']=own.get('compiler_sha256')
                        result['local_syntax_valid'] = own.get('local_syntax_valid')
                        result['local_statement_count']=own.get('local_statement_count')
                        if own.get('local_ok') and accepted and own.get('local_statement_count') is not None and own['local_statement_count']!=native.get('statements'):
                            result['native_statement_count_review']=True
                            counters['native-statement-count-review']+=1
                        if own.get('local_ok') and not accepted:
                            result['native_rejected_local_accepted'] = True
                            counters['native-rejected-local-accepted'] += 1
                output.write(json.dumps(result,ensure_ascii=False)+'\n')
    print(json.dumps({'cases':len(rows),'summary':dict(counters),'output':str(args.output)}))
    if counters['harness-error']:
        raise SystemExit(1)


if __name__=='__main__':
    main()
