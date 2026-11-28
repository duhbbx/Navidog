#!/usr/bin/env python3
"""Offline boundary tests; no network, external database, or upstream code runs."""
import importlib.util
from contextlib import redirect_stdout
import hashlib
import io
import json
from pathlib import Path
import sys
import tempfile
import unittest
from unittest.mock import patch

SOURCE = Path(__file__).resolve().parents[2] / "scripts/extract-sql-grammar-candidates.py"
SPEC = importlib.util.spec_from_file_location("grammar_extraction", SOURCE)
EXTRACT = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(EXTRACT)


class LiteralExtraction(unittest.TestCase):
    def test_postgresql_container_refuses_remote_and_cleans_only_owned_name(self):
        spec=importlib.util.spec_from_file_location('native_container_probe',SOURCE.parent/'probe-sql-native.py')
        probe=importlib.util.module_from_spec(spec)
        spec.loader.exec_module(probe)
        from types import SimpleNamespace
        remote=json.dumps([{'Endpoints':{'docker':{'Host':'tcp://unrelated.example:2376'}}}])
        with patch.object(probe,'command',return_value=SimpleNamespace(stdout=remote)) as command:
            with self.assertRaises(ValueError):
                with probe.postgres_container('postgres:18','remote',{}):pass
            self.assertEqual(command.call_count,1)
        local=json.dumps([{'Endpoints':{'docker':{'Host':'unix:///owned/docker.sock'}}}])
        image=json.dumps([{'Id':'sha256:'+'1'*64,'RepoDigests':[]}])
        replies=[SimpleNamespace(stdout=local),SimpleNamespace(stdout=image),
                 SimpleNamespace(stdout='invalid-container-id'),SimpleNamespace(stdout='')]
        with patch.object(probe,'command',side_effect=replies) as command:
            with self.assertRaises(ValueError):
                with probe.postgres_container('postgres:18','local',{}):pass
            create=command.call_args_list[2].args[0]
            cleanup=command.call_args_list[3].args[0]
            owned=create[create.index('--name')+1]
            self.assertRegex(owned,r'^nvg-grammar-pg-[0-9a-f]{12}$')
            self.assertEqual(cleanup,['docker','--context','local','rm','-f','-v',owned])

    def test_postgresql_column_description_preserves_csv_labels_and_bounds(self):
        spec=importlib.util.spec_from_file_location('native_probe',SOURCE.parent/'probe-sql-native.py')
        probe=importlib.util.module_from_spec(spec)
        spec.loader.exec_module(probe)
        from types import SimpleNamespace
        with patch.object(probe,'command',return_value=SimpleNamespace(returncode=0,stdout='"cost,net",integer\n"two\nlines",text\n',stderr='')) as command:
            result=probe.postgres_columns(['psql','-t'],'SELECT 17;')
            self.assertEqual(result['columns'],[['cost,net','integer'],['two\nlines','text']])
            self.assertTrue(command.call_args.kwargs['sql'].endswith('\n\\gdesc\n'))
        with patch.object(probe,'command') as command:
            for sql in ['DELETE FROM audit_items;', 'SELECT '+('x'*8192)]:
                with self.assertRaises(ValueError):probe.postgres_columns(['psql','-t'],sql)
            command.assert_not_called()
        with patch.object(probe,'command',return_value=SimpleNamespace(returncode=0,stdout='name,type,unexpected\n',stderr='')):
            with self.assertRaises(ValueError):probe.postgres_columns(['psql','-t'],'SELECT 17;')

    def test_candidate_status_uses_diagnostic_codes_not_message_words(self):
        spec=importlib.util.spec_from_file_location('candidate_audit',SOURCE.parent/'audit-sql-grammar-candidates.py')
        audit=importlib.util.module_from_spec(spec)
        spec.loader.exec_module(audit)
        codes=audit.diagnostic_codes()
        unsupported=dict(ok=False,syntaxValid=False,diagnostics=[dict(code=codes['UnsupportedSyntax'],message='requires validation')])
        syntax=dict(ok=False,syntaxValid=False,diagnostics=[dict(code=codes['ExpectedToken'],message='not supported here')])
        self.assertEqual(audit.local_status(unsupported,codes),'unsupported')
        self.assertEqual(audit.local_status(syntax,codes),'syntax-rejection')
        unsupported['syntaxValid']=True
        self.assertEqual(audit.local_status(unsupported,codes),'semantic-rejection')

    def test_sqlite_scalar_probe_authorizer_denies_side_effects_and_extensions(self):
        spec=importlib.util.spec_from_file_location('sqlite_probe',SOURCE.parent/'audit-sqlite-operator-roundtrip.py')
        probe=importlib.util.module_from_spec(spec)
        spec.loader.exec_module(probe)
        self.assertEqual(probe.native('SELECT 17;')['typed_rows'],[[('int',17)]])
        self.assertEqual(probe.native('SELECT 17.0;')['typed_rows'],[[('float',17.0)]])
        for sql in ["SELECT load_extension('untrusted');",'CREATE TABLE forbidden(id);',
                    "ATTACH DATABASE ':memory:' AS forbidden;",'PRAGMA writable_schema=ON;']:
            self.assertFalse(probe.native(sql)['accepted'])

    def test_native_ast_fingerprint_ignores_positions_but_preserves_sql_values(self):
        spec=importlib.util.spec_from_file_location('pg_probe',SOURCE.parent/'sql-grammar/postgresql-native/probe.py')
        probe=importlib.util.module_from_spec(spec)
        spec.loader.exec_module(probe)
        a={'stmts':[{'stmt_location':0,'stmt_len':17,'stmt':{'A_Expr':{'lexpr':{'String':{'sval':'location'}},'location':7,'rexpr_list_start':11}}}]}
        b={'stmts':[{'stmt_location':23,'stmt':{'A_Expr':{'lexpr':{'String':{'sval':'location'}},'location':29,'rexpr_list_end':31}}}]}
        positions={'A_Expr':{'location','rexpr_list_start','rexpr_list_end'}}
        self.assertEqual(probe.ast_fingerprint(a,positions),probe.ast_fingerprint(b,positions))
        b['stmts'][0]['stmt']['A_Expr']['lexpr']['String']['sval']='other value'
        self.assertNotEqual(probe.ast_fingerprint(a,positions),probe.ast_fingerprint(b,positions))
        # A same-named semantic field on a different node must be retained.
        self.assertNotEqual(probe.ast_fingerprint({'Other':{'location':17}},positions),
                            probe.ast_fingerprint({'Other':{'location':23}},positions))
        typed_a={'TypeCast':{'typeName':{'location':17,'names':['integer']}}}
        typed_b={'TypeCast':{'typeName':{'location':23,'names':['integer']}}}
        self.assertEqual(probe.ast_fingerprint(typed_a,{'TypeCast':set(),'TypeName':{'location'}},{'TypeCast':{'typeName':'TypeName'}}),
                         probe.ast_fingerprint(typed_b,{'TypeCast':set(),'TypeName':{'location'}},{'TypeCast':{'typeName':'TypeName'}}))

    def test_mysqltest_delimiters_and_error_codes_are_not_raw_parse_labels(self):
        source='''--echo SELECT fake;
--error ER_PARSE_ERROR
SELECT (;
--error 1064
PREPARE stmt FROM 'SELECT (';
--error ER_BAD_FIELD_ERROR
SELECT absent_column;
delimiter //;
CREATE PROCEDURE authored() BEGIN SELECT 'a;b'; SELECT 23; END//
delimiter ;//
SELECT 'quoted;delimiter';
'''
        rows=list(EXTRACT.mysqltest_cases(source))
        self.assertEqual(len(rows),5)
        self.assertEqual([r['upstream_parse_error_expected'] for r in rows],[True,True,False,False,False])
        self.assertTrue(all(r['upstream_parser_accepts'] is None for r in rows))
        self.assertEqual(rows[0]['line'],3)
        self.assertIn("BEGIN SELECT 'a;b'; SELECT 23; END;",rows[3]['sql'])
        self.assertEqual(rows[3]['upstream_delimiter'],'//')
        self.assertEqual(rows[4]['sql'],"SELECT 'quoted;delimiter';")

    def test_mysqltest_skips_embedded_and_dynamic_harness_content(self):
        source='''--write_file $file EOF
SELECT fake_file;
EOF
perl;
SELECT fake_perl;
EOF
let $query=
SELECT fake_variable;
eval SELECT $query;
--disable_parsing
SELECT fake_disabled;
--enable_parsing
if (1)
{
SELECT 17;
}
SELECT $variable;
SELECT 'literal';
'''
        rows=list(EXTRACT.mysqltest_cases(source))
        self.assertEqual([r['sql'] for r in rows],["SELECT 17;","SELECT $variable;","SELECT 'literal';"])
        self.assertEqual([r['requires_harness'] for r in rows],[True,True,False])

    def test_mysqltest_mode_tracking_and_connection_boundaries(self):
        source="""SET sql_mode='IGNORE_SPACE';
SELECT SUM (ALL 17);
SET sql_mode='NO_BACKSLASH_ESCAPES';
SELECT '\\';
--connection other
SELECT 23;
--delimiter |
CREATE PROCEDURE authored() BEGIN SELECT 17; END|
--delimiter ;
SELECT 29;
"""
        rows=list(EXTRACT.mysqltest_cases(source))
        self.assertEqual(len(rows),7)
        self.assertEqual(rows[1]['mysql_sql_mode'],'IGNORE_SPACE')
        self.assertEqual(rows[3]['mysql_sql_mode'],'NO_BACKSLASH_ESCAPES')
        self.assertFalse(rows[4]['mysql_sql_mode_known'])
        self.assertEqual(rows[5]['upstream_delimiter'],'|')
        self.assertEqual(rows[6]['sql'],'SELECT 29;')

    def test_tidb_audit_preserves_sql_mode_and_separates_observations(self):
        spec=importlib.util.spec_from_file_location('tidb_native_audit',SOURCE.with_name('audit-sql-tidb-native.py'))
        audit=importlib.util.module_from_spec(spec)
        spec.loader.exec_module(audit)
        sql='SELECT SUM (17);'
        sha=hashlib.sha256(sql.encode()).hexdigest()
        rows=[dict(sql=sql,sql_sha256=sha),dict(sql=sql,sql_sha256=sha,mysql_sql_mode='IGNORE_SPACE')]
        requests=[json.loads(line) for batch in audit.batches(rows,64) for _,_,line in batch]
        self.assertEqual([row['mode'] for row in requests],['','IGNORE_SPACE'])
        self.assertNotEqual(audit.case_key(rows[0]),audit.case_key(rows[1]))

    def test_postgres_raw_parser_does_not_claim_literal_body_validation(self):
        spec = importlib.util.spec_from_file_location('pg_native_audit',SOURCE.with_name('audit-sql-postgresql-native.py'))
        audit = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(audit)
        self.assertTrue(audit.raw_body('-- authored comment\n/* outer /* nested */ comment */ CREATE OR REPLACE FUNCTION f() RETURNS INT AS $$ bad body $$ LANGUAGE sql;'))
        self.assertTrue(audit.raw_body('/* comment */ DO $$ body $$;'))
        self.assertFalse(audit.raw_body("SELECT 'CREATE FUNCTION';"))
        self.assertFalse(audit.raw_body('CREATE TABLE function_names(id INT);'))

    def test_sqlserver_source_profiles_and_fixture_version_metadata(self):
        spec = importlib.util.spec_from_file_location('scriptdom_audit',SOURCE.with_name('audit-sql-scriptdom.py'))
        scriptdom = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(scriptdom)
        self.assertEqual(scriptdom.source_profile('ErrorTest130',160),(130,True))
        self.assertEqual(scriptdom.source_profile('ErrorTestAllParsersUntil150',160),(140,True))
        self.assertEqual(scriptdom.source_profile('ErrorTest90andAboveUntil150',160),(140,True))
        self.assertEqual(scriptdom.source_profile('ErrorTestAllParsers',160),(160,True))
        self.assertEqual(scriptdom.source_profile('ErrorTestAllParsers',180),(170,True))
        self.assertEqual(scriptdom.source_profile('ErrorTest90AndAbove',80),(90,True))
        self.assertEqual(scriptdom.source_profile('ErrorTest100AndAbove',90),(100,True))
        self.assertEqual(scriptdom.source_profile('ErrorTestUnknown',160),(160,False))
        self.assertEqual(scriptdom.source_profile('ErrorTestFabricDW',160),(160,False))
        reader = scriptdom.load_script('audit-sql-antlr.py')
        with tempfile.TemporaryDirectory(prefix='nvg-grammar-profiles-') as directory:
            root = Path(directory)
            (root/'sqlserver').mkdir()
            (root/'sqlserver/authored.sqltest').write_text(
                '# native-profile: 170\ncase returning valid <<SQL\n'
                "SELECT JSON_ARRAY(17 RETURNING JSON);\nSQL\n")
            rows = list(reader.fixtures(root,'sqlserver'))
        self.assertEqual(len(rows),1)
        self.assertEqual(rows[0]['native_profile'],170)
        self.assertEqual(rows[0]['line'],3)

    def test_collector_revision_change_invalidates_old_bytes(self):
        spec = importlib.util.spec_from_file_location('grammar_collector',SOURCE.with_name('collect-sql-grammar.py'))
        collector = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(collector)
        calls = []
        def fetch(url):
            calls.append(url)
            return b'first grammar' if 'a'*40 in url else b'second grammar'
        with tempfile.TemporaryDirectory(prefix='nvg-grammar-tools-') as directory:
            root = Path(directory)
            cache, lock = root/'cache',root/'lock.json'
            argv = ['collect-sql-grammar.py','--cache',str(cache),'--lock',str(lock),'--source','sqlite','--jobs','1']
            with patch.object(collector,'SOURCES',{'sqlite':('example/sql','main',['src/parse.y'])}), \
                 patch.object(collector,'fetch',fetch),patch.object(sys,'argv',argv),redirect_stdout(io.StringIO()):
                for revision in ['a'*40,'b'*40,'b'*40]:
                    lock.write_text(json.dumps({'sources':{'sqlite':{'revision':revision}}}))
                    collector.main()
            manifest = json.loads((cache/'manifest.json').read_text())
            self.assertEqual(len(calls),2)
            self.assertEqual(manifest['sources']['sqlite']['revision'],'b'*40)
            entry = manifest['files']['sqlite/src/parse.y']
            self.assertIn('b'*40,entry['url'])
            self.assertEqual(entry['sha256'],hashlib.sha256(b'second grammar').hexdigest())
            self.assertFalse((cache/'manifest.json.partial').exists())

    def test_csharp_literals_and_computed_expressions(self):
        source = r'''
ParserTestUtils.ErrorTest160("SELECT \"name\"" + " FROM items", new ParserErrorInfo(0,"SQL46010"));
ParserTestUtils.ErrorTest160(@"SELECT ""name""
FROM items");
ParserTestUtils.ErrorTest160(sql + " computed", new ParserErrorInfo(0,"SQL46010"));
ParserTestUtils.ErrorTest160($"SELECT {value}", new ParserErrorInfo(0,"SQL46010"));
'''
        cases = list(EXTRACT.sqlserver_cases(source))
        self.assertEqual([row['sql'] for row in cases], ['SELECT "name" FROM items','SELECT "name"\nFROM items'])
        self.assertEqual([row['upstream_parser_accepts'] for row in cases],[False,True])
        self.assertEqual(cases[0]['upstream_diagnostic_codes'],['SQL46010'])
        self.assertTrue(all(row['version_sensitive'] for row in cases))

    def test_csharp_comments_and_diagnostic_scope(self):
        prefixed = '/* unmatched ( in a comment */\nParserTestUtils.ErrorTest160("SELECT 17");'
        self.assertEqual(list(EXTRACT.sqlserver_cases(prefixed))[0]['sql'],'SELECT 17')
        source = '''/*
ParserTestUtils.ErrorTest160("fake commented query");
*/
string documentation = @"
ParserTestUtils.ErrorTest160(""fake string query"");";
ParserTestUtils.ErrorTest160("SELECT 17");
ParserTestUtils.ErrorTest160("SELECT (", new ParserErrorInfo(7,"SQL46010"));
'''
        cases = list(EXTRACT.sqlserver_cases(source))
        self.assertEqual([row['sql'] for row in cases],['SELECT 17','SELECT ('])
        self.assertEqual([row['upstream_parser_accepts'] for row in cases],[True,False])

    def test_postgres_quoted_semicolons_and_nested_comments(self):
        source = """-- leading ; comment
SELECT 'a;b', E'c\\\';d'; /* outer /* ; nested */ end */
SELECT $tag$begin; "quoted"; $tag$, $$other;$$;
SELECT "semi;column";
"""
        cases = list(EXTRACT.postgresql_cases(source))
        self.assertEqual(len(cases),3)
        self.assertIn("E'c\\\';d'",cases[0]['sql'])
        self.assertIn('$tag$begin;',cases[1]['sql'])
        self.assertIn('"semi;column"',cases[2]['sql'])
        self.assertTrue(all(row['upstream_parser_accepts'] is None for row in cases))

    def test_postgres_copy_and_psql_are_not_sql(self):
        source = "\\echo authored test\nCOPY items FROM stdin;\n17;not SQL\n\\.\nSELECT 23;"
        cases = list(EXTRACT.postgresql_cases(source))
        self.assertEqual(len(cases),2)
        self.assertTrue(cases[0]['requires_harness'])
        self.assertFalse(cases[1]['requires_harness'])
        self.assertNotIn('not SQL',cases[1]['sql'])

    def test_postgres_expected_error_stages_and_ambiguous_echoes(self):
        sql = 'SELECT 17;\nSELECT (;\nSELECT missing_column;\nSELECT 23;'
        expected = '''SELECT 17;
 value
-------
 17
(1 row)
SELECT (;
ERROR:  syntax error at or near ";"
LINE 1: SELECT (;
SELECT missing_column;
ERROR:  column "missing_column" does not exist
LINE 1: SELECT missing_column;
SELECT 23;
 23
(1 row)
'''
        cases = list(EXTRACT.postgresql_cases(sql))
        EXTRACT.postgres_expected_annotations(cases,expected)
        # Native error output repeats the SQL in LINE context. Ambiguous
        # echoes remain unclassified instead of guessing which occurrence.
        self.assertIsNone(cases[1]['upstream_parser_accepts'])
        self.assertIsNone(cases[2]['upstream_parser_accepts'])
        self.assertTrue(cases[3]['upstream_parser_accepts'])
        unique_error = list(EXTRACT.postgresql_cases('SELECT (;\nSELECT 23;'))
        EXTRACT.postgres_expected_annotations(unique_error,'SELECT (;\nERROR: syntax error\nLINE 1: ...\nSELECT 23;\n(1 row)\n')
        self.assertFalse(unique_error[0]['upstream_parser_accepts'])

    def test_tidb_labels_are_not_interpreted_as_syntax_stages(self):
        source = r'''func TestAcceptance(t *testing.T) {
table := []testCase{
 {"SELECT 17", true, ""},
 {`SELECT '{literal}'`, false, ""},
 {"SELECT " + computed, false, ""},
}
RunTest(t, table, false)
}'''
        cases = list(EXTRACT.tidb_cases(source))
        self.assertEqual([row['sql'] for row in cases],['SELECT 17',"SELECT '{literal}'"])
        self.assertEqual([row['upstream_parser_accepts'] for row in cases],[True,False])
        self.assertTrue(all('classification' not in row for row in cases))
        custom = 'func TestOptionFlag(t *testing.T) { table := []testCase{\n{"SELECT SQL_NO_CACHE 17", false, ""},\n} }'
        self.assertIsNone(list(EXTRACT.tidb_cases(custom))[0]['upstream_parser_accepts'])

    def test_sqlite_tcl_words_and_runtime_error_labels(self):
        source = '''do_execsql_test sample-1 {SELECT '{x}';} {value}
do_catchsql_test sample-2 {SELECT 17/$value;} {1 {test runtime error}}
do_catchsql_test sample-3 {SELECT 23;} {0 {23}}
do_catchsql_test sample-4 {SELECT eval('SELECT (');} {1 {near "(": syntax error}}
do_catchsql_test sample-5 {INSERT INTO t VALUES(17);} {1 {UNIQUE constraint failed: t.id}}
'''
        cases = list(EXTRACT.sqlite_cases(source))
        self.assertEqual(len(cases),5)
        self.assertEqual(cases[0]['sql'],"SELECT '{x}';")
        self.assertTrue(cases[1]['requires_harness'])
        self.assertIsNone(cases[1]['upstream_parser_accepts'])
        self.assertFalse(cases[1]['upstream_execution_accepts'])
        self.assertFalse(cases[1]['upstream_parse_error_expected'])
        self.assertTrue(cases[2]['upstream_parser_accepts'])
        self.assertIsNone(cases[3]['upstream_parser_accepts'])
        self.assertTrue(cases[3]['upstream_parse_error_expected'])
        self.assertIsNone(cases[4]['upstream_parser_accepts'])
        self.assertFalse(cases[4]['upstream_parse_error_expected'])


if __name__ == '__main__':
    unittest.main()
