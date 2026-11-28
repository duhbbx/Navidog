#!/usr/bin/env python3
"""Recognize/evaluate authored SELECT probes in disposable local containers.

Only already-cached images are used, through a local Unix Docker context. Every
server has a fresh tmpfs data directory, no host mounts, no published ports and
no external network. Cleanup removes only the newly created container ID.
MariaDB/TiDB observations do not inherit MySQL acceptance expectations.
The delete-shapes suite plans authored DELETE with EXPLAIN in an owned schema;
it never executes those DELETEs or treats a plan as an execution result.
Window-argument probes use PREPARE only, including parameter placeholders.
Server error codes are review hints, not automatic parser-stage labels.
"""
from __future__ import annotations

import argparse
from collections import Counter
from contextlib import contextmanager
import hashlib
import importlib.util
import json
import os
from pathlib import Path
import re
import shutil
import subprocess
import tempfile
import time
import uuid


def run(command,*,sql=None,timeout=30,check=True):
    return subprocess.run(command,input=sql,text=True,capture_output=True,timeout=timeout,check=check)


def native_sql(row,sql):
    if row.get('native_prepare_only'):
        if row.get('native_explain_only'):raise ValueError('probe cannot be both PREPARE and EXPLAIN')
        if 'NO_BACKSLASH_ESCAPES' not in row.get('mysql_sql_mode','').split(','):
            sql=sql.replace('\\','\\\\')
        return "PREPARE navidog_grammar_stmt FROM '"+sql.replace("'","''")+"';"
    return ('EXPLAIN ' if row.get('native_explain_only') else '')+sql


@contextmanager
def server(args,docker,image,client_image):
    name='nvg-grammar-'+args.engine+'-'+uuid.uuid4().hex[:12]
    command=docker+['run','-d','--rm','--pull=never','--name',name,'--network','none',
        '--cpus','2','--memory','1g','--pids-limit','128']
    if args.engine=='tidb':
        command+=['--tmpfs','/tmp:rw,size=512m,mode=1777','-e','GOMEMLIMIT=768MiB',
            '-e','GOMAXPROCS=2',image,'--store=unistore','--path=/tmp/navidog-tidb',
            '--log-file=/tmp/navidog-tidb.log']
    else:
        variable='MARIADB_ALLOW_EMPTY_ROOT_PASSWORD' if args.engine=='mariadb' else 'MYSQL_ALLOW_EMPTY_PASSWORD'
        command+=['--tmpfs','/var/lib/mysql:rw,size=512m', '-e',variable+'=1',image,
            '--skip-networking','--innodb-buffer-pool-size=64M','--max-connections=8',
            '--local-infile=0','--skip-log-bin','--innodb-flush-method=fsync']
        if args.engine=='mysql':
            command+=['--mysqlx=OFF','--secure-file-priv=NULL','--innodb-redo-log-capacity=64M']
    container=''
    try:
        container=run(command,timeout=30).stdout.strip()
        if not re.fullmatch('[0-9a-f]{64}',container):
            raise ValueError('Docker did not return the newly created container ID')
        common=['--user=root','--batch','--raw','--skip-column-names','--default-character-set=utf8mb4']
        if args.engine=='tidb':
            client=docker+['run','--rm','--pull=never','-i','--network','container:'+container,
                '--memory','128m','--pids-limit','32',client_image,'mysql','--no-defaults',
                '--protocol=tcp','--host=127.0.0.1','--port=4000']+common
        else:
            binary='mariadb' if args.engine=='mariadb' else 'mysql'
            client=docker+['exec','-i',container,binary,'--no-defaults','--protocol=socket']+common
        deadline=time.monotonic()+75
        while True:
            state=run(docker+['inspect','--format','{{.State.Running}}',container],check=False)
            if state.returncode or state.stdout.strip()!='true':
                logs=run(docker+['logs','--tail','60',container],check=False)
                raise RuntimeError('owned native server exited: '+logs.stdout[-4000:]+logs.stderr[-4000:])
            if args.engine!='tidb':
                # Official entrypoints start a temporary initialization server
                # before exec-ing the final daemon. Its socket is not readiness.
                process=run(docker+['exec',container,'cat','/proc/1/comm'],check=False)
                expected='mariadbd' if args.engine=='mariadb' else 'mysqld'
                if process.stdout.strip()!=expected:
                    if time.monotonic()>deadline:raise TimeoutError('owned native server initialization timed out')
                    time.sleep(.2)
                    continue
            response=run(client,sql='SELECT VERSION();',timeout=8,check=False)
            if response.returncode==0:
                break
            if time.monotonic()>deadline:
                raise TimeoutError('owned native server startup timed out: '+response.stderr[-2000:])
            time.sleep(.2)
        yield client,container,response.stdout.strip()
    finally:
        if container and re.fullmatch('[0-9a-f]{64}',container):
            run(docker+['rm','-f','-v',container],check=False,timeout=30)
        elif not container:
            # A client-side timeout can follow a successful daemon creation.
            # The UUID name was allocated exclusively by this invocation.
            run(docker+['rm','-f','-v',name],check=False,timeout=30)


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--engine',required=True,choices=['mysql','mariadb','tidb'])
    parser.add_argument('--context')
    parser.add_argument('--image')
    parser.add_argument('--client-image',default='mysql:8.4')
    parser.add_argument('--compiler',type=Path,default=Path('build-grammar-audit/bin/navidog-sql'))
    parser.add_argument('--suite',action='append',choices=['baseline','operators','operator-matrix','delete-shapes','window-arguments','window-runtime','aggregates','function-spacing'])
    parser.add_argument('--output',required=True,type=Path)
    args=parser.parse_args()
    context=args.context or run(['docker','context','show']).stdout.strip()
    identity=json.loads(run(['docker','context','inspect',context]).stdout)[0]
    if not identity['Endpoints']['docker']['Host'].startswith('unix://'):
        parser.error('this probe only uses a local Unix Docker endpoint')
    docker=['docker','--context',context]
    label=args.image or {'mysql':'mysql:8.4','mariadb':'mariadb:11.4','tidb':'pingcap/tidb:v8.5.3'}[args.engine]
    image=json.loads(run(docker+['image','inspect',label]).stdout)[0]
    client_image=json.loads(run(docker+['image','inspect',args.client_image]).stdout)[0] if args.engine=='tidb' else None
    source=Path('tests/sql/corpus/native-probes.json')
    cases=[dict(row) for row in json.loads(source.read_text())['cases'] if row['engine']=='mysql']
    if args.engine!='mysql':
        for row in cases:
            row['source_engine']=row['engine']
            row['engine']=args.engine
            row['source_expectation_not_inherited']=row.pop('expect')
            if 'stage' in row:row['source_mysql_stage']=row.pop('stage')
            row.pop('error_contains',None)
            row.pop('stdout',None)
    extra={
        'mariadb':[
            ('is-precedence','SELECT 0=0 IS NULL;'),
            ('between-precedence','SELECT 2=1 BETWEEN 0 AND 1;'),
            ('json-arrayagg-distinct','SELECT JSON_ARRAYAGG(DISTINCT 17);'),
            ('json-arrayagg-all','SELECT JSON_ARRAYAGG(ALL 17);'),
            ('sum-distinct-all','SELECT SUM(DISTINCT ALL 17);'),
            ('count-distinct-all','SELECT COUNT(DISTINCT ALL 17);'),
            ('abs-distinct','SELECT ABS(DISTINCT 17);')],
        'tidb':[
            ('now-dynamic-precision',"SELECT NOW(1+1),NOW('3'),NOW(NULL);"),
            ('escape-empty',"SELECT 'item' LIKE 'i%' ESCAPE '';"),
            ('escape-multibyte',"SELECT 'item' LIKE 'i%' ESCAPE '§';"),
            ('std-distinct','SELECT STD(DISTINCT ALL 17);'),
            ('natural-inner','SELECT 17 FROM (SELECT 1 AS item_id) a NATURAL INNER JOIN (SELECT 1 AS item_id) b;')],
        'mysql':[]}[args.engine]
    cases += [dict(engine=args.engine,id=name,sql=sql) for name,sql in extra]
    if args.suite and args.suite!=['baseline']:
        if 'baseline' not in args.suite:cases=[]
        spec=importlib.util.spec_from_file_location('independent_generator',Path(__file__).with_name('generate-sql-grammar-probes.py'))
        generator=importlib.util.module_from_spec(spec)
        spec.loader.exec_module(generator)
        for index,row in enumerate(generator.mysql_family_cases(args.engine,args.suite)):
            row.update(engine=args.engine,id='generated-'+str(index))
            cases.append(row)
    if any(not re.match(r'^\s*'+('DELETE' if row.get('native_explain_only') else 'SELECT')+r'\b',row['sql'],re.I) for row in cases):
        parser.error('inputs must be authored SELECT, or EXPLAIN-only authored DELETE')
    args.output.parent.mkdir(parents=True,exist_ok=True)
    report=dict(schema_version=2,engine=args.engine,isolated=True,source=str(source),image=label,
        error_codes_are_stage_hints=True,
        source_sha256=hashlib.sha256(source.read_bytes()).hexdigest(),
        image_id=image['Id'],image_digests=image.get('RepoDigests',[]),cases=[])
    counts=Counter()
    snapshot=tempfile.TemporaryDirectory(prefix='nvg-grammar-compiler-',dir=args.output.parent)
    original_compiler=args.compiler.resolve()
    args.compiler=Path(snapshot.name)/'navidog-sql'
    shutil.copy2(original_compiler,args.compiler)
    report.update(compiler_path=str(original_compiler),
        compiler_sha256=hashlib.sha256(args.compiler.read_bytes()).hexdigest())
    with snapshot,server(args,docker,image['Id'],client_image['Id'] if client_image else '') as (client,container,version), \
         args.output.with_suffix('.cases.jsonl').open('w') as stream:
        report.update(version=version,container_id=container)
        default_mode=run(client,sql='SELECT @@SESSION.sql_mode;').stdout.strip()
        report.update(native_default_sql_mode=default_mode,sql_mode_policy='explicit per case; omitted mode means empty')
        explain_suite=any(row.get('native_explain_only') for row in cases)
        if explain_suite:
            run(client,sql='CREATE DATABASE audit_grammar; USE audit_grammar; '
                'CREATE TABLE audit_items(item_id INT,label VARCHAR(30)); '
                'CREATE TABLE audit_other(item_id INT,label VARCHAR(30));')
            report['dml_validation']='EXPLAIN planning only; authored empty schema; no DELETE executes'
        print(json.dumps(dict(engine=args.engine,version=version,owned_container=container[:12])),flush=True)
        for row in cases:
            mode=row.get('mysql_sql_mode','')
            if not re.fullmatch('[A-Z_,]*',mode):
                raise ValueError('unexpected authored SQL mode')
            setup=("USE audit_grammar;\n" if explain_suite else '')+"SET SESSION sql_mode='"+mode+"';\n"
            operation='prepare' if row.get('native_prepare_only') else 'explain' if row.get('native_explain_only') else ''
            response=run(client,sql=setup+native_sql(row,row['sql']),check=False,timeout=10)
            accepted=response.returncode==0
            result=dict(row,native_accepted=accepted,stdout=response.stdout,stderr=response.stderr[-4000:])
            result['native_case_sha256']=hashlib.sha256((row['sql']+'\0'+mode+'\0'+(operation or 'execute')).encode()).hexdigest()
            if 'expect' in row and row.get('stdout') is not None:result['expected_stdout']=row['stdout']
            match=re.search(r'ERROR (\d+) \(([A-Z0-9]+)\)',response.stderr)
            if match and 2000<=int(match.group(1))<3000:
                raise RuntimeError('native client transport failure is not a SQL rejection: '+response.stderr[-2000:])
            if match:result.update(native_error_code=int(match.group(1)),native_sqlstate=match.group(2))
            counts['native-accepted' if accepted else 'native-rejected']+=1
            if 'expect' in row:
                passed=accepted==(row['expect']=='accept')
                if row.get('stdout') is not None and accepted:passed &= row['stdout']==response.stdout
                if row.get('error_contains') and not accepted:passed &= row['error_contains'] in response.stderr
                result['passed']=passed
                counts['known-expectation-pass' if passed else 'known-expectation-review']+=1
            if mode not in ('','IGNORE_SPACE'):
                result['local_comparison_deferred']='this SQL mode has no matching CLI flag'
            else:
                command=[str(args.compiler.resolve()),'--dialect',args.engine,'--ast','-']
                if mode=='IGNORE_SPACE':command.insert(1,'--mysql-ignore-space')
                own=run(command,sql=row['sql'],check=False,timeout=5)
                if own.returncode not in (0,1):raise RuntimeError('local compiler exited abnormally')
                parsed=json.loads(own.stdout)
                result.update(local_ok=parsed['ok'],local_syntax_valid=parsed['syntaxValid'],local_diagnostic=own.stderr[:2000])
                if accepted and not parsed['ok']:
                    counts['native-accepted-local-rejected-review']+=1
                    result['native_accepted_local_rejected_review']=True
                if parsed['ok'] and not accepted:
                    counts['native-rejected-local-accepted-review']+=1
                    result['native_rejected_local_accepted_review']=True
                    if match and int(match.group(1))==1064:
                        result['native_parse_error_code_local_accepted_review']=True
                        counts['native-1064-local-accepted-review']+=1
                if parsed['ok'] and row.get('round_trip_compare'):
                    emitted=run([str(args.compiler.resolve()),*(['--mysql-ignore-space'] if mode=='IGNORE_SPACE' else []),
                        '--dialect',args.engine,'--emit',args.engine,'-'],sql=row['sql'],check=False,timeout=5)
                    if emitted.returncode or not emitted.stdout.strip():
                        raise RuntimeError('accepted local SQL did not generate a complete query')
                    generated=run(client,sql=setup+native_sql(row,emitted.stdout),check=False,timeout=10)
                    error=re.search(r'ERROR (\d+) \(([A-Z0-9]+)\)',generated.stderr)
                    if error and 2000<=int(error.group(1))<3000:
                        raise RuntimeError('generated-query transport failure is not a SQL result')
                    equivalent=(generated.returncode==0)==accepted and (
                        (bool(operation) or generated.stdout==response.stdout) if accepted else
                        bool(error and match and error.group(1)==match.group(1)))
                    result.update(generated_sql=emitted.stdout,generated_stdout=generated.stdout,
                        generated_stderr=generated.stderr[-2000:])
                    result['round_trip_acceptance_equivalent' if operation else 'round_trip_equivalent']=equivalent
                    counts[(operation+'-acceptance-agreement' if equivalent else operation+'-acceptance-review') if operation else
                           ('round-trip-agreement' if equivalent else 'round-trip-review')]+=1
            report['cases'].append(result)
            stream.write(json.dumps(result,ensure_ascii=False)+'\n')
            stream.flush()
            if len(report['cases'])%25==0:
                print(json.dumps(dict(engine=args.engine,completed=len(report['cases']),total=len(cases),
                    round_trip_reviews=counts['round-trip-review'],parse_error_code_reviews=counts['native-1064-local-accepted-review'])),flush=True)
    report['summary']=dict(counts)
    args.output.write_text(json.dumps(report,indent=2,ensure_ascii=False)+'\n')
    print(json.dumps(dict(engine=args.engine,version=report['version'],summary=dict(counts),output=str(args.output))))
    if any(counts[key] for key in ('known-expectation-review','round-trip-review','explain-acceptance-review',
                                  'prepare-acceptance-review','native-1064-local-accepted-review')):
        raise SystemExit(1)


if __name__=='__main__':
    main()
