#!/usr/bin/env python3
"""JSON-line PostgreSQL raw-parser probe; no server or SQL execution.

The optional GPL pglast adapter is verified against its pinned wheel and kept
in the ignored audit cache. It is never imported by or linked into Navidog.
"""
import argparse
import hashlib
import json
from pathlib import Path
import sys
import zipfile

ROOT = Path(__file__).resolve().parents[3]


def normalized_ast(tree,position_fields,child_types=None):
    """Remove only schema-declared source-position metadata from a native tree."""
    child_types=child_types or {}
    def normalize(value,node_type=None):
        if isinstance(value,dict):
            return {key:normalize(child,key if key in position_fields else child_types.get(node_type,{}).get(key)) for key,child in value.items()
                    if key not in ('stmt_location','stmt_len') and key not in position_fields.get(node_type,set())}
        if isinstance(value,list):return [normalize(child) for child in value]
        return value
    return normalize(tree)


def ast_fingerprint(tree,position_fields,child_types=None):
    """Hash the native tree after removing only source-position metadata."""
    canonical=json.dumps(normalized_ast(tree,position_fields,child_types),sort_keys=True,separators=(',',':'),ensure_ascii=False)
    return hashlib.sha256(canonical.encode()).hexdigest()


def main():
    argument = argparse.ArgumentParser(description=__doc__)
    argument.add_argument('--cache',type=Path,default=ROOT/'.cache/postgresql-native-parser')
    argument.add_argument('--lock',type=Path,default=ROOT/'tests/sql/corpus/reference-sources.json')
    argument.add_argument('--version',action='store_true')
    args = argument.parse_args()
    source = json.loads(args.lock.read_text())['native_parser_tools']['postgresql']
    wheel = args.cache/source['filename']
    if hashlib.sha256(wheel.read_bytes()).hexdigest()!=source['sha256']:
        argument.error('optional pglast wheel does not match the source lock')
    site = (args.cache/'site-packages').resolve()
    # Verify Python adapter code as well as the native module before importing.
    with zipfile.ZipFile(wheel) as archive:
        for info in archive.infolist():
            if not info.is_dir() and info.filename.startswith('pglast/'):
                if '..' in Path(info.filename).parts:
                    argument.error('invalid adapter archive path')
                expected = hashlib.sha256(archive.read(info)).digest()
                if hashlib.sha256((site/info.filename).read_bytes()).digest()!=expected:
                    argument.error('installed optional adapter differs from its verified wheel')
    library = site/source['native_path']
    if hashlib.sha256(library.read_bytes()).hexdigest()!=source['native_sha256']:
        argument.error('PostgreSQL native library differs from its source lock')
    sys.path.insert(0,str(site))
    import pglast
    from pglast import parser,ast
    # Use the verified adapter's native node schema: array/list ranges are also
    # ParseLoc fields. Never drop a semantic field just because another node
    # happens to use the same field name for a source position.
    positions={name:{field for field,type_info in cls.__slots__.items()
                    if getattr(type_info,'c_type',type_info)=='ParseLoc'}
        for name,cls in vars(ast).items() if isinstance(cls,type) and isinstance(getattr(cls,'__slots__',None),dict)}
    # The adapter abstracts A_Const's native tagged union and omits this raw
    # C-node field; parse_sql_json still includes its ParseLoc location.
    positions.setdefault('A_Const',set()).add('location')
    # Specifically typed pointers (e.g. TypeCast.typeName, FuncCall.over) are
    # JSON objects without a node-tag wrapper. Carry their verified C type.
    child_types={name:{field:getattr(type_info,'c_type',type_info).rstrip('*')
        for field,type_info in cls.__slots__.items()
        if isinstance(getattr(type_info,'c_type',type_info),str) and
           getattr(type_info,'c_type',type_info).rstrip('*') in positions}
        for name,cls in vars(ast).items() if isinstance(cls,type) and isinstance(getattr(cls,'__slots__',None),dict)}
    if not {'location','rexpr_list_start','rexpr_list_end'}<=positions.get('A_Expr',set()) or \
            not {'location','list_start','list_end'}<=positions.get('A_ArrayExpr',set()):
        argument.error('native ParseLoc schema is incomplete; fingerprint comparison would be invalid')
    version = parser.get_postgresql_version()
    if pglast.__version__.lstrip('v')!=source['version'] or list(version)!=source['postgresql_version']:
        argument.error('PostgreSQL native parser runtime identity mismatch')
    if args.version:
        print(json.dumps(dict(adapter='pglast',version=source['version'],
            postgresql_version=version,libpg_query_revision=source['libpg_query_revision'],
            native_sha256=source['native_sha256'])))
        return
    for line in sys.stdin:
        result = dict(index=-1,accepted=False,harness_error=False)
        try:
            if len(line.encode())>2*1024*1024:
                raise ValueError('request exceeds line size limit')
            request = json.loads(line)
            result['index'] = int(request['index'])
            sql = request['sql']
            if not isinstance(sql,str) or len(sql.encode())>262144:
                raise ValueError('SQL exceeds probe limits')
            parsed = json.loads(parser.parse_sql_json(sql))
            result.update(accepted=True,statements=len(parsed.get('stmts',[])))
            if request.get('fingerprint'):
                result.update(ast_sha256=ast_fingerprint(parsed,positions,child_types),fingerprint_algorithm='postgresql-raw-tree-parseloc-v4')
            if request.get('include_ast') and len(sql.encode())<=8192:
                result['ast']=parsed
            if request.get('include_normalized_ast'):
                if len(sql.encode())<=8192:result['normalized_ast']=normalized_ast(parsed,positions,child_types)
                else:result['normalized_ast_omitted']='SQL exceeds 8192 bytes'
        except parser.ParseError as error:
            result.update(error=str(error)[:2048])
            if len(error.args)>1 and isinstance(error.args[1],int):
                result['error_offset'] = error.args[1]
        except Exception as error:
            result.update(harness_error=True,error=f'{type(error).__name__}: {error}'[:2048])
        print(json.dumps(result,ensure_ascii=False))


if __name__=='__main__':
    main()
