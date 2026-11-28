#!/usr/bin/env python3
"""Generate independently authored, bounded SQL grammar combinations.

This writes review inputs only. No SQL is executed, and no generated statement
is presumed valid or invalid. Feed the JSONL to the existing CLI and native
auditors, then review discrepancies before writing regression expectations.
"""
from __future__ import annotations

import argparse
from collections import Counter
import hashlib
import json
from itertools import chain
from pathlib import Path
import re


def sqlserver_cases():
    seen = set()

    def row(family, sql):
        if sql in seen:
            return []
        seen.add(sql)
        return [dict(dialect='sqlserver',family=family,sql=sql,
            sql_sha256=hashlib.sha256(sql.encode()).hexdigest(),
            generator='navidog-independent-sqlserver-v1',native_profile=170,
            upstream_parser_accepts=None,requires_harness=False)]

    values = ['17','NULL','-17','+23',"'audit'",'17+23','ABS(17)',
        'other_id','@value','CAST(17 AS INT)','CAST(CAST(17 AS INT) AS INT)',
        'CONVERT(INT,17)','CONVERT(INT,CONVERT(INT,23))',
        'TRY_CONVERT(INT,17)',"TRY_CAST('17' AS INT)"]
    for value in values:
        for operator in ['=','<>','<','>=','IS DISTINCT FROM','IS NOT DISTINCT FROM']:
            yield from row('filtered-index',
                f'CREATE INDEX audit_idx ON audit_items(item_id) WHERE item_id {operator} {value};')
        for condition in [f'item_id IN ({value})',f'item_id NOT IN ({value})',
            f'(item_id>{value}) AND label IS NOT NULL',f'item_id>{value} OR label IS NULL']:
            yield from row('filtered-index',
                f'CREATE INDEX audit_idx ON audit_items(item_id) WHERE {condition};')
    for function in ['JSON_OBJECT','JSON_ARRAY','[JSON_OBJECT]','dbo.JSON_OBJECT']:
        for body in ['',"'item':17","'item',17",'17','label:item_id',
                     'returning:item_id','returning','NULL']:
            for policy in ['', ' NULL ON NULL',' ABSENT ON NULL',
                           ' NULL ON NULL ABSENT ON NULL']:
                for returning in ['',' RETURNING JSON']:
                    yield from row('json-constructor',
                        f'SELECT {function}({body}{policy}{returning}) FROM audit_items;')
    for function in ['CONVERT','TRY_CONVERT','TRY_CAST']:
        for target in ['INT','DECIMAL(10,2)','NVARCHAR(MAX)','VARCHAR(20)',
                       'dbo.audit_type','INT(10)','VARCHAR(0)',"VARCHAR('x')",'17',"'INT'",'MAX']:
            for value in ['17','NULL',"'audit'",'item_id','*',
                          'CAST(17 AS INT)','CONVERT(INT,17)']:
                if function=='TRY_CAST':
                    yield from row('conversion',
                        f'SELECT {function}({value} AS {target}) FROM audit_items;')
                else:
                    for style in ['',',23',',style_code',',*']:
                        yield from row('conversion',
                            f'SELECT {function}({target},{value}{style}) FROM audit_items;')


    for value in ['0','1','+1','-1','NULL','1.0','1.5','1e0',"'1'",'1+1',
                  '(1+1)','(1)+(1)','9%2','ABS(1)','CAST(1 AS INT)','@take',
                  '@take+1','item_id','(SELECT 1)','(SELECT COUNT(*) FROM audit_items)']:
        for sql in [f'SELECT TOP({value}) 17;',f'SELECT TOP {value} 17;',
                    f'SELECT TOP({value}) WITH TIES item_id FROM audit_items ORDER BY item_id;',
                    f'SELECT 17 ORDER BY 1 OFFSET {value} ROWS;',
                    f'SELECT 17 ORDER BY 1 OFFSET 0 ROWS FETCH NEXT {value} ROWS ONLY;',
                    f'SELECT 17 OFFSET {value} ROWS;',
                    f'UPDATE TOP({value}) audit_items SET item_id=17;',
                    f'DELETE TOP({value}) FROM audit_items;']:
            yield from row('pagination',sql)


def sqlserver_identifier_cases():
    source=json.loads(Path('tests/sql/corpus/sqlserver-keyword-probes.json').read_text())
    words=source['words']
    if words!=sorted(set(words)) or any(not re.fullmatch(r'[A-Z][A-Z_0-9]*',word) for word in words):
        raise ValueError('invalid SQL Server keyword review spellings')
    for word in words:
        for sql in [f'SELECT {word} FROM audit_items;',f'SELECT 17 AS {word};',
                    f'SELECT 17 {word};',f'SELECT audit_items.{word} FROM audit_items;',
                    f'CREATE TABLE audit_items ({word} INT);',f'SELECT {word}(17);',
                    f'CREATE TABLE {word} (item_id INT);',f'SELECT [{word}] FROM audit_items;',
                    f'SELECT 17 AS [{word}];',f'SELECT 17 FROM audit_items AS {word};']:
            yield dict(dialect='sqlserver',family='identifier-contexts',sql=sql,
                sql_sha256=hashlib.sha256(sql.encode()).hexdigest(),
                generator='navidog-independent-sqlserver-keyword-contexts-v1',
                source_url=source['source_url'],source_snapshot_sha256=source['source_snapshot_sha256'],
                upstream_parser_accepts=None,requires_harness=False,native_profile=160)


def postgresql_cases(cache):
    key = 'postgresql/src/include/parser/kwlist.h'
    lock = json.loads(Path('tests/sql/corpus/reference-sources.json').read_text())
    source = lock['cached_file_inventory'][key]
    data = (cache/key).read_bytes()
    if hashlib.sha256(data).hexdigest()!=source['sha256']:
        raise ValueError('PostgreSQL keyword reference does not match the reviewed source lock')
    seen = set()
    for word,category,label in re.findall(
        r'^PG_KEYWORD\("([a-z_]+)",\s*\w+,\s*(\w+),\s*(BARE_LABEL|AS_LABEL)\)',
        data.decode(),re.M):
        for family,sql in [
            ('projection-label',f'SELECT 17 {word};'),
            ('explicit-label',f'SELECT 17 AS {word};'),
            ('expression-name',f'SELECT {word} FROM audit_items;'),
            ('type-literal',f"SELECT {word} 'audit';"),
            ('predicate-position',f'SELECT 17 WHERE 17=17 {word};'),
            ('argument-position',f'SELECT ABS(17 {word});'),
            ('table-label',f'SELECT item_id FROM audit_items {word};')]:
            if sql in seen:
                continue
            seen.add(sql)
            yield dict(dialect='postgresql',family=family,sql=sql,
                sql_sha256=hashlib.sha256(sql.encode()).hexdigest(),
                generator='navidog-independent-postgresql-keywords-v1',
                reference_url=source['url'],reference_sha256=source['sha256'],
                keyword=word,keyword_category=category,keyword_label=label,
                upstream_parser_accepts=None,requires_harness=False)


def postgresql_index_cases():
    keys=['item_id','"ON"','audit_items.item_id','17','NULL','TRUE',
          'item_id+17','(item_id+17)','((item_id))','ABS(item_id)',
          'ABS(item_id)+1','CAST(item_id AS INT)','item_id::INT',
          '(item_id::INT)','COALESCE(item_id,17)','CURRENT_DATE',
          'CASE WHEN item_id>0 THEN 1 ELSE 0 END',
          '(CASE WHEN item_id>0 THEN 1 ELSE 0 END)',
          'item_id[1]','(item_id[1])','ROW(item_id)','(ROW(item_id))',
          '(SELECT 17)','SUM(item_id) FILTER(WHERE item_id>0)',
          '(SUM(item_id) FILTER(WHERE item_id>0))','SUM(item_id)OVER()',
          '(SUM(item_id)OVER())','item_id COLLATE "C"','item_id int4_ops']
    heads=['CREATE INDEX audit_idx','CREATE INDEX','CREATE UNIQUE INDEX',
           'CREATE INDEX IF NOT EXISTS audit_idx','CREATE INDEX IF NOT EXISTS',
           'CREATE INDEX owned.audit_idx']
    for head in heads:
        for key in keys:
            sql=f'{head} ON audit_items({key});'
            yield dict(dialect='postgresql',family='ddl-indexes',sql=sql,
                sql_sha256=hashlib.sha256(sql.encode()).hexdigest(),
                generator='navidog-independent-pg-index-positions-v1',
                upstream_parser_accepts=None,requires_harness=False)


def postgresql_pagination_cases():
    values=['0','1','+1','-1','NULL',"'1'","'audit'",'1.5','TRUE','CURRENT_DATE',
        '(1+1)','1+1','(1)+(1)','(1+1)+0',"CAST('1' AS BIGINT)","'1'::BIGINT",
        "('1'::BIGINT)",'ABS(-1)','(SELECT 1)','item_id','$1','row','rows',
        'CASE WHEN 17=17 THEN 1 ELSE 2 END','ARRAY[1]','(ARRAY[1])[1]',
        'ARRAY[1][1]','COALESCE(NULL,1)','1 ISNULL','(1 ISNULL)','NOT FALSE',
        '(NOT FALSE)','1 BETWEEN 0 AND 2','(1 BETWEEN 0 AND 2)','(1)+1',
        '(1)::BIGINT','((1)+(1))','1=1','(1=1)','1 AND','DEFAULT']
    for value in values:
        for clause in [f'LIMIT {value}',f'OFFSET {value}',f'OFFSET {value} ROWS',
            f'FETCH FIRST {value} ROWS ONLY',f'OFFSET {value} LIMIT 1',
            f'LIMIT 1 OFFSET {value} ROWS',f'OFFSET 0 FETCH NEXT {value} ROWS ONLY',
            f'ORDER BY 1 FETCH FIRST {value} ROWS WITH TIES']:
            sql = f'SELECT 17 {clause};'
            yield dict(dialect='postgresql',family='pagination',sql=sql,
                sql_sha256=hashlib.sha256(sql.encode()).hexdigest(),
                generator='navidog-independent-postgresql-pagination-v1',
                upstream_parser_accepts=None,requires_harness=False)


def sqlite_pagination_cases():
    values=['0','1','-1','+1','NULL','TRUE','FALSE','1.0','1.5','1e0','1e-1',
        "'1'","'1.5'","'audit'",'ABS(-1)','MIN(1,2)','MAX(1,2)',
        'COALESCE(NULL,1)','IFNULL(NULL,1)','1+1','1-1','(1)+(1)','(SELECT 1)',
        'CASE WHEN 17=17 THEN 1 ELSE 2 END','COUNT(*)','ROW_NUMBER() OVER()',
        'item_id','(1=1)','NOT 0','1 IN ()',"'a' LIKE 'a'",'NULL ISNULL',
        "CAST('1' AS INTEGER)",'CAST(NULL AS INTEGER)','1 BETWEEN 0 AND 2','1 AND']
    for value in values:
        for clause in [f'LIMIT {value}',f'LIMIT {value} OFFSET 0',f'LIMIT 1 OFFSET {value}',
                       f'LIMIT {value},2',f'LIMIT 1,{value}',f'LIMIT {value} OFFSET NULL',
                       f'LIMIT NULL,{value}']:
            sql = f'SELECT 17 {clause};'
            yield dict(dialect='sqlite',family='pagination',sql=sql,
                sql_sha256=hashlib.sha256(sql.encode()).hexdigest(),
                generator='navidog-independent-sqlite-pagination-v1',
                upstream_parser_accepts=None,requires_harness=False)


def sqlite_operator_cases():
    seen=set()
    def row(expression):
        sql=f'SELECT {expression};'
        if sql in seen:return []
        seen.add(sql)
        return [dict(dialect='sqlite',family='operator-matrix',sql=sql,
            sql_sha256=hashlib.sha256(sql.encode()).hexdigest(),
            generator='navidog-independent-sqlite-operators-v1',
            upstream_parser_accepts=None,requires_harness=False)]
    operators=['=','==','<>','<','<=','+','-','*','/','%','AND','OR',
               'LIKE','GLOB','||','IS','IS NOT','IS DISTINCT FROM']
    for a,b,c in [('17','23','29'),('0','1','0'),('TRUE','FALSE','TRUE'),
                  ('NULL','NULL','NULL'),("'a'","'b'","'c'")]:
        for first in operators:
            for second in operators:
                yield from row(f'{a} {first} {b} {second} {c}')
    for value in ['17','TRUE','NULL']:
        for first in ['IS NULL','ISNULL','NOTNULL','IS TRUE','IS NOT FALSE',
                      'BETWEEN 0 AND 1','NOT BETWEEN 0 AND 1','IN(0,1)',"LIKE '0'"]:
            for second in ['IS NULL','ISNULL','IS TRUE','BETWEEN 0 AND 1',
                           'IN(0,1)',"LIKE '0'",'=TRUE','<TRUE','AND TRUE','OR FALSE']:
                yield from row(f'{value} {first} {second}')
    for expression in ['NOT 0=1','NOT 0+1','0 BETWEEN 0=0 AND 1',
        '0 BETWEEN 0 AND 1=1','0 BETWEEN 0 AND 1 BETWEEN 0 AND 1',
        "'a' LIKE 'a' ESCAPE '!' LIKE 'a'",'NULL NOT IN () IS TRUE',
        'NULL IS NOT NULL IS NULL','1 IS DISTINCT FROM 2 IS NOT DISTINCT FROM 0']:
        yield from row(expression)


def postgresql_operator_cases():
    seen=set()
    def row(sql):
        if sql in seen:return []
        seen.add(sql)
        return [dict(dialect='postgresql',family='operator-matrix',sql=sql,
            sql_sha256=hashlib.sha256(sql.encode()).hexdigest(),
            generator='navidog-independent-postgresql-operators-v1',
            upstream_parser_accepts=None,requires_harness=False)]
    operators=['=','<>','<','+','-','*','/','%','AND','OR','LIKE','ILIKE','||']
    for a,b,c in [('17','23','29'),('TRUE','FALSE','TRUE'),('NULL','NULL','NULL'),("'a'","'b'","'c'")]:
        for first in operators:
            for second in operators:
                yield from row(f'SELECT {a} {first} {b} {second} {c};')
    for value in ['17','TRUE','NULL']:
        for first in ['IS NULL','ISNULL','NOTNULL','IS TRUE','IS NOT FALSE',
                      'BETWEEN 0 AND 1','NOT BETWEEN 0 AND 1','IN(0,1)',"LIKE '0'"]:
            for second in ['IS NULL','ISNULL','IS TRUE','BETWEEN 0 AND 1','IN(0,1)',
                           "LIKE '0'",'=TRUE','<TRUE','AND TRUE','OR FALSE']:
                yield from row(f'SELECT {value} {first} {second};')
    for expression in ['1 BETWEEN 1=1 AND 2','1 BETWEEN 1 IS NULL AND 2',
        '1 BETWEEN 1 IS DISTINCT FROM 1 AND 2','1 BETWEEN 1 IN(1) AND 2',
        '1 BETWEEN 0 AND 2=TRUE','1 BETWEEN 0 AND 2 BETWEEN 0 AND 1',
        'TRUE IS TRUE IS TRUE','(TRUE IS TRUE) IS TRUE','(1=1)=TRUE',
        '1=(1=1)','1 IN(1) IN(TRUE)','(1 IN(1)) IN(TRUE)',
        "'a' LIKE 'a' ESCAPE '!' LIKE 'a'", "('a' LIKE 'a' ESCAPE '!') IS TRUE",
        '1=ANY(ARRAY[1])=TRUE','1 IS DISTINCT FROM 2 IS DISTINCT FROM 3',
        '1 IS DISTINCT FROM 2 ISNULL','1 ISNULL IS DISTINCT FROM FALSE',
        'TRUE BETWEEN 1=1 AND TRUE','TRUE BETWEEN 1 IS DISTINCT FROM 2 AND TRUE',
        'TRUE BETWEEN (1 IN(1)) AND TRUE','TRUE BETWEEN 1=ANY(ARRAY[1]) AND TRUE',
        'TRUE BETWEEN (1=ANY(ARRAY[1])) AND TRUE',
        'TRUE BETWEEN (TRUE IS NULL) AND TRUE','TRUE BETWEEN TRUE IS NULL AND TRUE',
        'TRUE BETWEEN (FALSE OR TRUE) AND TRUE','TRUE BETWEEN FALSE OR TRUE AND TRUE']:
        yield from row(f'SELECT {expression};')


def mysql_family_cases(dialect,families):
    seen=set()
    def row(family,sql,mode=''):
        key=(sql,mode)
        if key in seen:return []
        seen.add(key)
        return [dict(dialect=dialect,family=family,sql=sql,
            sql_sha256=hashlib.sha256(sql.encode()).hexdigest(),
            case_sha256=hashlib.sha256((sql+'\0'+mode).encode()).hexdigest(),
            generator='navidog-independent-mysql-family-v1',mysql_sql_mode=mode,
            round_trip_compare=True,upstream_parser_accepts=None,requires_harness=False)]
    if 'ddl-types' in families:
        for base in ['TINYINT','SMALLINT','MEDIUMINT','MIDDLEINT','INT','INT1','INT2','INT3','INT4','INT8','INTEGER','BIGINT',
                     'BOOL','BOOLEAN','DECIMAL','NUMERIC','FIXED','FLOAT','REAL','DOUBLE','FLOAT4','FLOAT8','BIT',
                     'CHAR','CHARACTER','NCHAR','VARCHAR','VARCHARACTER','NVARCHAR','BINARY','VARBINARY',
                     'TINYBLOB','BLOB','MEDIUMBLOB','LONGBLOB','TINYTEXT','TEXT','MEDIUMTEXT','LONGTEXT',
                     'ENUM','SET','JSON','LONG','VECTOR','DATE','DATETIME','TIMESTAMP','TIME','YEAR',
                     'BYTES','custom_type','`INT`','dbo.INT','NATIONAL']:
            for arguments in ['', '(1)','(1,2)','(1.0)','(-1)','(+1)',"('item')","('item','other')"]:
                for sql in [f'CREATE TABLE audit_items (item_id {base}{arguments});',
                            f'ALTER TABLE audit_items ADD item_id {base}{arguments};']:
                    yield from row('ddl-types',sql)
    if 'operators' in families:
        suffixes=['IS NULL','IS TRUE','BETWEEN 0 AND 1','IN(0,1)',"LIKE '0'","REGEXP '^0$'"]
        for left in ['0','1','2']:
            for right in ['0','1','2']:
                for operator in ['=','<>','<']:
                    for suffix in suffixes:
                        yield from row('operators',f'SELECT {left}{operator}{right} {suffix};')
        for expression in ['0=0 IS NULL','NULL=0 IS NULL','0=NULL IS NULL',
            '0 IS NULL=0','2=2 IS TRUE','0=2 IS FALSE','0 BETWEEN 0 AND 1=1',
            "0 BETWEEN 0 AND 1 LIKE '1'",'0 BETWEEN 1 AND 0 IS NULL',
            'NOT 0=0 IS NULL','NOT 2=2 IS TRUE','0 XOR 1 OR 0','1 OR 0 XOR 1',
            '1 XOR 1 AND 0','NOT 0 BETWEEN 0 AND 1','0 IS NULL OR 1=1 AND 0=0',
            '0 IN(0) BETWEEN 0 AND 1','0 LIKE 0 LIKE 1','0 REGEXP 0 REGEXP 1',
            '0 IN(0) IN(1)','0 BETWEEN 0 AND 1 BETWEEN 0 AND 1',
            '0 BETWEEN 0 BETWEEN 0 AND 1 AND 1','0 BETWEEN 0 LIKE 0 AND 1',
            '0 BETWEEN 0 AND 1 IN(1)','0 LIKE 0+1','0 LIKE (0+1)',
            '0 NOT LIKE 0+1','0 LIKE 0 ESCAPE 0+1','0 LIKE 0 ESCAPE (0+1)',
            '0 IS TRUE=0','0 IS TRUE IS NULL','0 IS TRUE+1',
            '0 IS NULL+1','0 IN(0)+1','0 LIKE 0+1=1','0=0 IS TRUE=0',
            '(0 IS TRUE)=0','(0 IS TRUE) IS NULL','(0 IN(0))+1',
            '(0 LIKE 0) LIKE 1','0 BETWEEN 0 AND (1 BETWEEN 0 AND 1)',
            '0 BETWEEN 0 AND 1 BETWEEN 0 AND 0','1 BETWEEN 0 AND 1 BETWEEN 0 AND 0',
            '2 BETWEEN 0 AND 1 BETWEEN 0 AND 0','0 LIKE 0 ESCAPE 0 LIKE 0']:
            yield from row('operators',f'SELECT {expression};')
    if 'operator-matrix' in families:
        operators=['=','<>','<','+','-','*','DIV','%','AND','OR','XOR','LIKE','REGEXP']
        for a,b,c in [('0','1','2'),('2','0','1'),('NULL','1','0')]:
            for first in operators:
                for second in operators:
                    yield from row('operator-matrix',f'SELECT {a} {first} {b} {second} {c};')
        for a in ['0','1','NULL']:
            for first in ['IS NULL','IS NOT NULL','IS TRUE','IS NOT FALSE',
                          'BETWEEN 0 AND 1','NOT BETWEEN 0 AND 1','IN(0,1)',
                          'NOT IN(0,1)',"LIKE '0'","NOT LIKE '0'",'REGEXP 0','NOT REGEXP 0']:
                for suffix in ['IS NULL','IS TRUE','BETWEEN 0 AND 1','IN(0,1)',
                               'LIKE 0','REGEXP 0','+1','=0','AND 1','OR 0']:
                    yield from row('operator-matrix',f'SELECT {a} {first} {suffix};')
        for expression in ['0 REGEXP 0+1','0 NOT REGEXP 0+1',
            '0 REGEXP (0+1)','0 NOT REGEXP (0+1)',
            'NOT 0 LIKE 0','0=NOT 0','0=(NOT 0)','-0 LIKE 0','0 LIKE -0',
            '0 LIKE 0 ESCAPE NULL','0 NOT LIKE 0 ESCAPE NULL',
            '0 BETWEEN 0 AND NOT 0','0 BETWEEN NOT 0 AND 1',
            '0 BETWEEN (NOT 0) AND 1','0 BETWEEN 0 AND (NOT 0)',
            '0 BETWEEN 0 AND 1 LIKE 0 ESCAPE 0','0 BETWEEN 0 AND 1 NOT IN(0)',
            '0 BETWEEN 0 AND 1 BETWEEN 0 AND 1 BETWEEN 0 AND 0']:
            yield from row('operator-matrix',f'SELECT {expression};')
    if 'delete-shapes' in families:
        statements=[]
        for target in ['audit_items','audit_items AS item','audit_items item','audit_items.*',
                       'item','item.*','audit_items,audit_other','audit_items item,audit_other']:
            for source in ['audit_items',
                           'audit_items item',
                           'audit_items JOIN audit_other ON audit_items.item_id=audit_other.item_id']:
                statements.append(f'DELETE FROM {target} USING {source} WHERE item_id=17;')
        for target in ['audit_items','audit_items AS item','audit_items item','audit_items.*']:
            statements.append(f'DELETE FROM {target} WHERE item_id=17;')
            statements.append(f'DELETE FROM {target} ORDER BY item_id LIMIT 1;')
        for target in ['item','item.*','item AS other','item other','item,other','item.*,other.*']:
            statements.append(f'DELETE {target} FROM audit_items item JOIN audit_other other ON item.item_id=other.item_id;')
        for modifier in ['LOW_PRIORITY','QUICK','IGNORE','LOW_PRIORITY QUICK IGNORE',
            'QUICK LOW_PRIORITY IGNORE','IGNORE QUICK LOW_PRIORITY','QUICK IGNORE LOW_PRIORITY',
            'IGNORE LOW_PRIORITY QUICK','LOW_PRIORITY IGNORE QUICK','QUICK QUICK',
            'LOW_PRIORITY LOW_PRIORITY','IGNORE IGNORE']:
            statements.append(f'DELETE {modifier} FROM audit_items WHERE item_id=17;')
        for sql in statements:
            for result in row('delete-shapes',sql):
                result['native_explain_only']=True
                yield result
    if 'window-arguments' in families:
        offsets=['0','1','+1','-1','NULL','1.0','1.5','1e0',"'1'","'audit'",'1+1','(1)','(1+1)',
                 'ABS(1)','CAST(1 AS SIGNED)','@step','@@max_allowed_packet','item_id','?',
                 '18446744073709551615','18446744073709551616',
                 '9223372036854775807','9223372036854775808','9223372036854775809',
                 'TRUE','FALSE','CURRENT_DATE','CURRENT_TIMESTAMP','CURRENT_USER']
        for function in ['NTILE','NTH_VALUE','LAG','LEAD']:
            for offset in offsets:
                args=offset if function=='NTILE' else '17,'+offset
                sql=f'SELECT {function}({args}) OVER(ORDER BY item_id) FROM (SELECT 17 AS item_id) AS audit_items;'
                for result in row('window-arguments',sql):
                    result['native_prepare_only']=True
                    yield result
        for call in ['NTILE()','NTILE(1,2)','NTILE(ALL 1)','NTH_VALUE(17)',
                     'NTH_VALUE(17,1,0)','LAG()','LEAD()',
                     'LAG(17,1,0)','LEAD(17,1,1+1)','LAG(17,1,0,0)']:
            for result in row('window-arguments',f'SELECT {call} OVER(ORDER BY ABS(17));'):
                result['native_prepare_only']=True
                yield result
    if 'window-runtime' in families:
        for result in mysql_family_cases(dialect,['window-arguments']):
            if '?' in result['sql']:continue
            result['family']='window-runtime'
            result['native_prepare_only']=False
            yield result
    if 'aggregates' in families:
        functions=['COUNT','SUM','AVG','MIN','MAX','BIT_AND','BIT_OR','BIT_XOR',
            'STD','STDDEV','STDDEV_POP','STDDEV_SAMP','VARIANCE','VAR_POP','VAR_SAMP',
            'JSON_ARRAYAGG','JSON_OBJECTAGG','GROUP_CONCAT','ABS']
        for function in functions:
            args="'item',17" if function=='JSON_OBJECTAGG' else '17'
            for quantifier in ['','ALL ','DISTINCT ','DISTINCT ALL ','DISTINCTROW ','DISTINCTROW ALL ']:
                yield from row('aggregates',f'SELECT {function}({quantifier}{args});')
        for quantifier in ['','ALL ','DISTINCT ','DISTINCT ALL ']:
            yield from row('aggregates',f'SELECT COUNT({quantifier}*);')
    if 'function-spacing' in families:
        for function in ['SUM','COUNT','CURDATE','NOW','JSON_ARRAYAGG','JSON_OBJECTAGG','STD','GROUP_CONCAT','ABS']:
            args='' if function in ('CURDATE','NOW') else "'item',17" if function=='JSON_OBJECTAGG' else '17'
            for gap in ['', ' ', '/* authored */',' /* authored */ ','\n',
                        '-- authored\n','# authored\n','/* first */\n/* second */']:
                for mode in ['','IGNORE_SPACE']:
                    yield from row('function-spacing',f'SELECT {function}{gap}({args}) IS NOT NULL;',mode)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--dialect',choices=['sqlserver','postgresql','sqlite','mysql','mariadb','tidb','oceanbase'],default='sqlserver')
    parser.add_argument('--cache',type=Path,default=Path('.cache/sql-grammar-20261127'))
    parser.add_argument('--family',action='append',
        choices=['filtered-index','json-constructor','conversion','projection-label',
                 'explicit-label','expression-name','type-literal','predicate-position',
                 'argument-position','table-label','pagination','identifier-contexts','operators','operator-matrix','delete-shapes','ddl-types','ddl-indexes','window-arguments','window-runtime','aggregates','function-spacing'])
    parser.add_argument('--limit',type=int)
    parser.add_argument('--output',type=Path)
    args = parser.parse_args()
    if args.limit is not None and not 1 <= args.limit <= 10000:
        parser.error('limit must be 1..10000')
    if args.output is None:
        args.output = args.cache/f'generated-{args.dialect}.jsonl'
    if args.dialect=='sqlserver':
        inputs = chain(sqlserver_cases(),sqlserver_identifier_cases() if args.family and 'identifier-contexts' in args.family else [])
    elif args.dialect=='sqlite':
        inputs = chain(sqlite_pagination_cases() if not args.family or 'pagination' in args.family else [],
                       sqlite_operator_cases() if args.family and 'operator-matrix' in args.family else [])
    elif args.dialect in ('mysql','mariadb','tidb','oceanbase'):
        inputs = mysql_family_cases(args.dialect,args.family or ['operators','aggregates','function-spacing'])
    else:
        keywords = not args.family or bool(set(args.family)&{'projection-label','explicit-label',
            'expression-name','type-literal','predicate-position','argument-position','table-label'})
        pagination = not args.family or 'pagination' in args.family
        inputs = chain(postgresql_cases(args.cache) if keywords else [],
                       postgresql_pagination_cases() if pagination else [],
                       postgresql_operator_cases() if args.family and 'operator-matrix' in args.family else [],
                       postgresql_index_cases() if args.family and 'ddl-indexes' in args.family else [])
    rows = [row for row in inputs if not args.family or row['family'] in args.family]
    if args.limit:
        rows = rows[:args.limit]
    if not rows:
        parser.error('the selected dialect/family has no independently authored probes')
    args.output.parent.mkdir(parents=True,exist_ok=True)
    temporary = args.output.with_name(args.output.name+'.partial')
    with temporary.open('w') as output:
        for row in rows:
            output.write(json.dumps(row,ensure_ascii=False)+'\n')
    temporary.replace(args.output)
    print(json.dumps(dict(cases=len(rows),families=dict(Counter(row['family'] for row in rows)),
        output=str(args.output))))


if __name__=='__main__':
    main()
