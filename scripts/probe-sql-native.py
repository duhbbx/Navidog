#!/usr/bin/env python3
"""Run independently authored probes against disposable native databases.

SQLite uses :memory:. MySQL/PostgreSQL always initialize a new temporary data
directory, disable TCP listening, and connect to that directory's Unix socket.
There is deliberately no host, socket, password, or existing-data option.
The optional PostgreSQL DDL suite uses only its separate authored manifest;
each original/generated statement and owned-catalog inspection rolls back.
Downloaded upstream scripts are never inputs to this tool.
"""
from __future__ import annotations

import argparse
from contextlib import closing, contextmanager
import csv
import hashlib
import io
import json
import os
from pathlib import Path
import re
import shutil
import sqlite3
import subprocess
import tempfile
import time
import uuid


def command(argv, *, sql=None, check=True, timeout=30):
    environment=dict(os.environ)
    for key in ('PGHOSTADDR','PGSERVICE','PGSERVICEFILE','PGPASSWORD','PGPASSFILE','PGOPTIONS','MYSQL_PWD'):
        environment.pop(key,None)
    environment['PGPASSFILE']='/dev/null'
    return subprocess.run([str(arg) for arg in argv], input=sql, text=True,
                          capture_output=True, check=check, timeout=timeout,env=environment)


def postgres_columns(client, sql):
    """Describe only the already selected, independently authored SELECT.

    psql's gdesc uses Parse/Describe without running the query a second time.
    CSV preserves commas/newlines in quoted result labels; tuple-only output
    omits the description table's own header. The server is always disposable.
    """
    if not re.match(r'^\s*SELECT\b',sql,re.I) or len(sql.encode())>8192:
        raise ValueError('column description requires a bounded authored SELECT')
    query=sql.rstrip().removesuffix(';')+'\n\\gdesc\n'
    response=command(client+['--csv'],sql=query,check=False,timeout=5)
    if response.returncode or len(response.stdout.encode())>65536:
        return dict(accepted=False,stderr=response.stderr[:2000])
    columns=list(csv.reader(io.StringIO(response.stdout),strict=True))
    if not columns or len(columns)>128 or any(len(row)!=2 for row in columns):
        raise ValueError('invalid native column-description response')
    return dict(accepted=True,columns=columns)


@contextmanager
def mysql_server(binaries: Path, root: Path, log):
    data, socket = root / "data", root / "mysql.sock"
    server = binaries / "mysqld"
    subprocess.run([str(server), "--no-defaults", "--initialize-insecure",
                    f"--datadir={data}"], stdout=log, stderr=log, check=True, timeout=45)
    process = subprocess.Popen([str(server), "--no-defaults", f"--datadir={data}",
        f"--socket={socket}", f"--pid-file={root / 'mysql.pid'}", "--skip-networking",
        "--mysqlx=OFF", "--innodb-buffer-pool-size=64M", "--max-connections=8",
        "--local-infile=0", "--secure-file-priv=NULL"], stdout=log, stderr=log)
    client = [binaries / "mysql", "--no-defaults", "--protocol=socket", f"--socket={socket}",
              "--user=root", "--batch", "--raw", "--skip-column-names",
              "--default-character-set=utf8mb4"]
    try:
        deadline = time.monotonic() + 20
        while True:
            if process.poll() is not None:
                raise RuntimeError("isolated MySQL exited during startup; see server log")
            if socket.exists() and command(client, sql="SELECT 1;", check=False, timeout=2).returncode == 0:
                break
            if time.monotonic() >= deadline:
                raise RuntimeError("isolated MySQL startup timed out")
            time.sleep(0.1)
        yield client
    finally:
        if process.poll() is None:
            process.terminate()
            try:
                process.wait(timeout=20)
            except subprocess.TimeoutExpired:
                process.kill()
                process.wait(timeout=5)


@contextmanager
def postgres_server(binaries: Path, root: Path, log_path: Path):
    data = root / "data"
    command([binaries / "initdb", "-D", data, "--no-locale", "--encoding=UTF8",
             "--auth=trust", "--username=grammar_probe"])
    # TemporaryDirectory's prefix contains no spaces or shell metacharacters.
    options = f"-k {root} -p 59438 -c listen_addresses='' -c shared_buffers=16MB"
    started = False
    try:
        command([binaries / "pg_ctl", "-D", data, "-l", log_path, "-o", options, "-w", "start"])
        started = True
        yield [binaries / "psql", "-X", "-h", root, "-p", "59438", "-U", "grammar_probe",
               "-d", "postgres", "-A", "-t", "-v", "ON_ERROR_STOP=1", "-v", "VERBOSITY=verbose"]
    finally:
        if started or (data / "postmaster.pid").exists():
            command([binaries / "pg_ctl", "-D", data, "-m", "immediate", "-w", "stop"], check=False)


@contextmanager
def postgres_container(image_label, context, report):
    """Use only an inspected cached image, a fresh tmpfs, and our own socket."""
    context=context or command(['docker','context','show']).stdout.strip()
    identity=json.loads(command(['docker','context','inspect',context]).stdout)[0]
    if not identity['Endpoints']['docker']['Host'].startswith('unix://'):
        raise ValueError('native PostgreSQL containers require a local Unix Docker endpoint')
    docker=['docker','--context',context]
    image=json.loads(command(docker+['image','inspect',image_label]).stdout)[0]
    report['container_image']={key:image.get(key) for key in ['Id','RepoDigests','Architecture','Os']}
    report['container_image']['label']=image_label
    name='nvg-grammar-pg-'+uuid.uuid4().hex[:12]
    container=''
    try:
        container=command(docker+['run','-d','--rm','--pull=never','--name',name,
            '--network','none','--cpus','2','--memory','512m','--pids-limit','96',
            '--tmpfs','/var/lib/postgresql:rw,size=256m,mode=1777',
            '-e','POSTGRES_HOST_AUTH_METHOD=trust','-e','POSTGRES_USER=grammar_probe',
            '-e','POSTGRES_DB=postgres',image['Id'],
            '-c','listen_addresses=','-c','shared_buffers=16MB']).stdout.strip()
        if not re.fullmatch('[0-9a-f]{64}',container):
            raise ValueError('Docker did not return the newly created container ID')
        report['owned_container_id']=container
        client=docker+['exec','-i',container,'psql','-X','-h','/var/run/postgresql',
            '-U','grammar_probe','-d','postgres','-A','-t','-v','ON_ERROR_STOP=1',
            '-v','VERBOSITY=verbose']
        deadline=time.monotonic()+60
        while True:
            process=command(docker+['exec',container,'cat','/proc/1/comm'],check=False,timeout=5)
            if process.returncode==0 and process.stdout.strip()=='postgres':
                ready=command(client,sql='SELECT 1;',check=False,timeout=5)
                if ready.returncode==0:break
            if time.monotonic()>deadline:
                raise TimeoutError('owned PostgreSQL container did not become ready')
            time.sleep(.2)
        yield client
    finally:
        # The UUID name is allocated by this invocation even if run times out.
        target=container if re.fullmatch('[0-9a-f]{64}',container) else name
        command(docker+['rm','-f','-v',target],check=False,timeout=30)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--engine", required=True, choices=["sqlite", "mysql", "postgresql"])
    parser.add_argument('--suite',choices=['baseline','postgresql-ddl'],default='baseline')
    parser.add_argument("--mysql-bin", type=Path, default=Path("/opt/homebrew/opt/mysql/bin"))
    parser.add_argument("--postgres-bin", type=Path, default=Path("/opt/homebrew/opt/postgresql@15/bin"))
    parser.add_argument('--postgres-container-image',help='already-cached PostgreSQL image; no image is pulled by this tool')
    parser.add_argument('--docker-context')
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument('--compiler',type=Path,default=Path('build-grammar-audit/bin/navidog-sql'))
    args = parser.parse_args()
    ddl_suite=args.suite=='postgresql-ddl'
    if ddl_suite and args.engine!='postgresql':
        parser.error('the DDL suite requires an owned PostgreSQL server')
    if args.postgres_container_image and args.engine!='postgresql':
        parser.error('the PostgreSQL container option requires engine postgresql')
    source = Path('tests/sql/corpus/postgresql-ddl-probes.json' if ddl_suite else 'tests/sql/corpus/native-probes.json')
    cases = [row for row in json.loads(source.read_text())["cases"] if row["engine"] == args.engine]
    if not cases:
        parser.error("no authored probes for this engine")
    if any(not re.match(r'^\s*'+('(?:CREATE|ALTER)' if ddl_suite else 'SELECT')+r'\b', row['sql'], re.I) for row in cases):
        parser.error('native probes must match the independently authored suite')
    if any(row.get('compare_columns') and (args.engine!='postgresql' or not row.get('round_trip')) for row in cases):
        parser.error('column comparisons require authored PostgreSQL round-trip probes')
    args.output.parent.mkdir(parents=True, exist_ok=True)
    log_path = args.output.with_suffix(".server.log").resolve()
    report = {"engine": args.engine, "isolated": True, "source": str(source),
              "source_sha256":hashlib.sha256(source.read_bytes()).hexdigest(), "cases": []}
    compiler=None

    def run(client=None):
        if client:
            version_sql = "SELECT VERSION();" if args.engine == "mysql" else "SHOW server_version;"
            report["version"] = command(client, sql=version_sql).stdout.strip()
        else:
            report["version"] = sqlite3.sqlite_version
            with closing(sqlite3.connect(":memory:")) as identity:
                report["source_id"] = identity.execute("SELECT sqlite_source_id()").fetchone()[0]
        for case in cases:
            result = dict(case)
            finish=''
            if client:
                setup = ""
                if "mysql_sql_mode" in case:
                    if not re.fullmatch(r"[A-Z_,]*", case["mysql_sql_mode"]):
                        raise ValueError("invalid authored SQL mode")
                    setup = "SET SESSION sql_mode='" + case["mysql_sql_mode"] + "';\n"
                active_client=client
                if ddl_suite:
                    active_client=client+['-q']
                    setup='BEGIN; CREATE SCHEMA audit_grammar; SET LOCAL search_path=audit_grammar;\n'+case.get('setup_sql','')+'\n'
                    finish='\n'+case.get('inspection_sql','')+'\nROLLBACK;\n'
                response = command(active_client, sql=setup + case["sql"]+finish, check=False, timeout=5)
                result.update(accepted=response.returncode == 0, stdout=response.stdout, stderr=response.stderr)
            else:
                # Separate connections keep schema/settings out of the next probe.
                with closing(sqlite3.connect(":memory:")) as connection:
                    try:
                        values = connection.execute(case["sql"]).fetchall()
                        result.update(accepted=True, rows=[list(row) for row in values], stderr="")
                    except sqlite3.Error as error:
                        result.update(accepted=False, stderr=str(error))
            result["passed"] = result["accepted"] == (case["expect"] == "accept")
            if "error_contains" in case and not result["accepted"]:
                result["passed"] &= case["error_contains"] in result["stderr"]
            if "stdout" in case and result["accepted"]:
                result["passed"] &= result["stdout"] == case["stdout"]
            if "rows" in case and result["accepted"]:
                result["passed"] &= result["rows"] == case["rows"]
            if case.get('round_trip') and result['accepted']:
                if not client or compiler is None:raise ValueError('round-trip native probes require an owned CLI server and frozen compiler')
                generated=command([compiler,'--dialect',args.engine,'--emit',args.engine,'-'],sql=case['sql'],check=False,timeout=5)
                result['generation_diagnostic']=generated.stderr[:2000]
                result['round_trip_passed']=False
                if generated.returncode==0 and re.match(r'^\s*'+('(?:CREATE|ALTER)' if ddl_suite else 'SELECT')+r'\b',generated.stdout,re.I):
                    replay=command(active_client,sql=setup+generated.stdout+finish,check=False,timeout=5)
                    result.update(generated_sql=generated.stdout,generated_stdout=replay.stdout,generated_stderr=replay.stderr[:2000])
                    result['round_trip_passed']=replay.returncode==0 and replay.stdout==result['stdout']
                result['passed'] &= result['round_trip_passed']
                if case.get('compare_columns'):
                    original_columns=postgres_columns(client,case['sql'])
                    generated_columns=postgres_columns(client,generated.stdout) if result['round_trip_passed'] else dict(accepted=False)
                    result.update(original_column_description=original_columns,generated_column_description=generated_columns)
                    result['column_description_passed']=original_columns.get('accepted',False) and generated_columns.get('accepted',False) and original_columns['columns']==generated_columns['columns']
                    if 'columns' in case:
                        result['column_description_passed'] &= original_columns.get('columns')==case['columns']
                    result['passed'] &= result['column_description_passed']
            report["cases"].append(result)

    with tempfile.TemporaryDirectory(prefix="nvg-native-probe-",dir="/tmp") as directory:
        root = Path(directory)
        if any(case.get('round_trip') for case in cases):
            compiler=root/'navidog-sql'
            shutil.copy2(args.compiler.resolve(),compiler)
            report['compiler_sha256']=hashlib.sha256(compiler.read_bytes()).hexdigest()
        if args.engine == "sqlite":
            run()
        elif args.engine == "mysql":
            with log_path.open("w") as log, mysql_server(args.mysql_bin, root, log) as client:
                run(client)
        elif args.postgres_container_image:
            with postgres_container(args.postgres_container_image,args.docker_context,report) as client:
                run(client)
        else:
            with postgres_server(args.postgres_bin, root, log_path) as client:
                run(client)
    args.output.write_text(json.dumps(report, indent=2, ensure_ascii=False) + "\n")
    passed = sum(row["passed"] for row in report["cases"])
    print(f"{args.engine} {report['version']}: {passed}/{len(cases)} native probes passed; {args.output}")
    if passed != len(cases):
        raise SystemExit(1)


if __name__ == "__main__":
    main()
