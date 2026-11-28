#include <algorithm>
#include <QtTest>
#include <QTemporaryDir>
#include <QProcess>
#include <QStandardPaths>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QUrl>
#include <QUrlQuery>
#include <set>
#include "navidog/database.hpp"
#include "navidog/database_tools.hpp"
#include "navidog/federated_query.hpp"
#include "navidog/catalog.hpp"
#include "navidog/query_service.hpp"
#include "navidog/query_plan.hpp"
#include "navidog/sql.hpp"
#include "navidog/sql/compiler/compiler.hpp"
#include "navidog/sql/compiler/conversion.hpp"
#include "navidog/sql/compiler/formatter.hpp"
#include "navidog/sql/compiler/script_context.hpp"
#include "navidog/metadata_cache.hpp"
#include "navidog/sql/optimizer/optimizer.hpp"
#include "navidog/sql/optimizer/interpreter.hpp"
#include "navidog/sql/optimizer/plan_sql.hpp"
#include "navidog/sql/optimizer/rel_metadata.hpp"
#include "navidog/sql/optimizer/materialized_view.hpp"
#include "navidog/source_design.hpp"
#include "navidog/er_model.hpp"

// Own temporary cluster, Unix socket only, no access to existing servers or credentials.
class PostgresTest : public QObject {
    Q_OBJECT
    QTemporaryDir dir_;
    QString pgCtl_;
    navidog::ConnectionProfile profile_;
    bool started_=false;
    bool command(const QString& binary,const QStringList& arguments) {
        QProcess process; process.start(binary,arguments);
        if(!process.waitForFinished(10000)) { process.kill(); process.waitForFinished(); return false; }
        if(process.exitCode()!=0) qWarning().noquote()<<process.readAllStandardError();
        return process.exitStatus()==QProcess::NormalExit && process.exitCode()==0;
    }
private slots:
    void initTestCase() {
        if(!navidog::availableDrivers().contains(navidog::DatabaseKind::PostgreSQL)) QSKIP("libpq not compiled");
        QString prefix=qEnvironmentVariable("NAVIDOG_TEST_PG_BIN");
        const auto initdb=prefix.isEmpty() ? QStandardPaths::findExecutable("initdb") : prefix+"/initdb";
        pgCtl_=prefix.isEmpty() ? QStandardPaths::findExecutable("pg_ctl") : prefix+"/pg_ctl";
        if(initdb.isEmpty() || pgCtl_.isEmpty()) QSKIP("Set NAVIDOG_TEST_PG_BIN to run an isolated PostgreSQL integration test");
        QVERIFY(dir_.isValid());
        QVERIFY(command(initdb,{"-D",dir_.filePath("data"),"-A","trust","-U","navidog_test","--no-locale","--encoding=UTF8"}));
        QVERIFY(command(pgCtl_,{"-D",dir_.filePath("data"),"-l",dir_.filePath("server.log"),"-o","-c listen_addresses='' -c unix_socket_directories='"+dir_.path()+"' -p 55438","-w","start"}));
        started_=true; profile_.id="owned-pg-test"; profile_.name="Owned temporary PG"; profile_.driver=navidog::DatabaseKind::PostgreSQL;
        profile_.host=dir_.path(); profile_.port=55438; profile_.database="postgres"; profile_.user="navidog_test"; profile_.tls=false;
    }
    void erReverseAndDiffUseNativeCatalog() { try {
        using namespace navidog;
        auto db = connectDatabase(profile_);
        const auto cancel = cancellation();
        const auto run = [&](const QString& statement) {
            auto r = db->execute(statement,{},cancel);
            if (!r.error.isEmpty()) throw std::runtime_error(r.error.toStdString());
        };
        run("CREATE SCHEMA er_owned");
        run("CREATE TABLE er_owned.customer (id INT PRIMARY KEY, alias TEXT UNIQUE)");
        run("CREATE TABLE er_owned.purchase (id INT PRIMARY KEY, customer_id INT REFERENCES er_owned.customer(id), total INT CHECK(total>=0))");
        run("CREATE INDEX er_total ON er_owned.purchase(total DESC) WHERE total>1");
        run("CREATE VIEW er_owned.orders AS SELECT * FROM er_owned.purchase");
        er::ImportOptions options; options.schema = "er_owned";
        auto project = er::reverse(*db,profile_,options,cancel).project;
        QCOMPARE(project.entities.size(),3);
        const auto graph = er::makeGraph(project);
        QCOMPARE(graph.edges.size(),1);
        auto& purchase = project.entities[graph.edges.front().from].table;
        TableColumn column; column.name="new_text";column.type="TEXT";
        purchase.columns.push_back(column);
        auto changes = er::ddl(project,true);
        QCOMPARE(changes.size(),1);
        run(changes.front());
        const auto create = er::ddl(project,false);
        QVERIFY(std::any_of(create.begin(),create.end(),[](const auto& sql){return sql.startsWith("CREATE VIEW");}));
        QVERIFY(std::any_of(create.begin(),create.end(),[](const auto& sql){return sql.startsWith("CREATE INDEX");}));
        run("DROP SCHEMA er_owned CASCADE");
    } catch (const std::exception& e) { QFAIL(e.what()); } }
    void searchesCachedDefinitionsAcrossPostgresSchemas() { try {
        using namespace navidog;
        auto db = connectDatabase(profile_);
        const auto token = cancellation();
        const auto run = [&](const QString& sql) {
            const auto result = db->execute(sql, {}, token);
            if (!result.error.isEmpty()) throw std::runtime_error(result.error.toStdString());
        };
        run("CREATE SCHEMA owned_discovery_schema");
        const QString marker = "owned_postgres_source_discovery";
        run("CREATE MATERIALIZED VIEW owned_discovery_schema.fixture AS SELECT '" + marker + "' AS typed");
        run("CREATE FUNCTION owned_discovery_schema.fixture_routine() RETURNS TEXT LANGUAGE SQL AS $$ SELECT '" + marker + "' $$");
        run("CREATE PROCEDURE owned_discovery_schema.fixture_proc() LANGUAGE plpgsql AS $$ BEGIN RAISE NOTICE '" + marker + "'; END; $$");
        const auto cache = dir_.filePath("dictionary-search.sqlite");
        DatabaseSearchRequest request{{}, marker, DatabaseSearchScope::ObjectDefinitions, DatabaseSearchMode::Contains};
        const auto all = searchDatabaseObjects(profile_, profile_.database, request, cache, true, token);
        QVERIFY2(all.hits.size() == 3, qPrintable(all.warnings.join('\n')));
        for (const auto& result : all.hits) QCOMPARE(result.node.schema, QString("owned_discovery_schema"));
        request.schema = "public";
        const auto filtered = searchDatabaseObjects(profile_, {}, request, cache, false, token);
        QCOMPARE(filtered.cachedScopes, 1);
        QVERIFY(filtered.hits.isEmpty());
        run("DROP SCHEMA owned_discovery_schema CASCADE");
    } catch (const std::exception& e) { QFAIL(e.what()); } }

    void convertedMySqlRoutinesExecuteWithBoundScopes() {try {
        using namespace navidog::sql::compiler;auto db=navidog::connectDatabase(profile_);auto token=navidog::cancellation();
        auto run=[&](const QString& sql){auto r=db->execute(sql,{},token);if(!r.error.isEmpty())throw std::runtime_error(r.error.toStdString());return r;};
        auto result=convertSql(Dialect::MySQL,Dialect::PostgreSQL,"CREATE FUNCTION conversion_add(p INTEGER) RETURNS INTEGER DETERMINISTIC SQL SECURITY INVOKER BEGIN DECLARE v INTEGER DEFAULT p; BEGIN DECLARE v INTEGER DEFAULT 99; SET v=v+1; END; IF v>1 THEN SET v=v+2; ELSEIF v=1 THEN SET v=10; ELSE SET v=0; END IF; RETURN v; END;");
        QVERIFY2(result.ok(),result.diagnostics.empty()?"no result":result.diagnostics.front().message.c_str());run(QString::fromStdString(result.sql));
        QCOMPARE(run("SELECT conversion_add(p=>5)").rows[0][0].toInt(),7);QCOMPARE(run("SELECT conversion_add(0)").rows[0][0].toInt(),0);QCOMPARE(run("SELECT conversion_add(1)").rows[0][0].toInt(),10);QVERIFY(run("SELECT conversion_add(NULL)").rows[0][0].toInt()==0);
        run("CREATE TABLE conversion_audit(n INTEGER)");result=convertSql(Dialect::MySQL,Dialect::PostgreSQL,"CREATE PROCEDURE conversion_write(IN p INTEGER) SQL SECURITY INVOKER BEGIN DECLARE v INTEGER DEFAULT p; WHILE v<3 DO SET v=v+1; END WHILE; INSERT INTO conversion_audit(n) VALUES(v); END;");QVERIFY2(result.ok(),result.diagnostics.empty()?"no result":result.diagnostics.front().message.c_str());run(QString::fromStdString(result.sql));run("CALL conversion_write(1)");QCOMPARE(run("SELECT n FROM conversion_audit").rows[0][0].toInt(),3);
        run("DROP PROCEDURE conversion_write(INTEGER)");run("DROP FUNCTION conversion_add(INTEGER)");run("DROP TABLE conversion_audit");
    }catch(const std::exception& e){QFAIL(e.what());}}
    void exceptionHandlerPreservesSubtransactionAndScope() {try {
        using namespace navidog;
        using namespace navidog::sql::compiler;
        auto db=connectDatabase(profile_);
        auto token=cancellation();
        const auto run=[&](const QString &sql) {
            const auto result=db->execute(sql,{},token);
            if (!result.error.isEmpty()) throw std::runtime_error(result.error.toStdString());
            return result;
        };
        run("CREATE TABLE public.exc_owned(n INT)");
        const auto source="CREATE FUNCTION handle_exc_owned(p INT) RETURNS TEXT LANGUAGE plpgsql AS $$ "
            "DECLARE v INT := 0; err_state TEXT; err_msg TEXT; BEGIN INSERT INTO public.exc_owned VALUES (1); "
            "<<failed_step>> BEGIN INSERT INTO public.exc_owned VALUES (2); v := 7; v := 10/p; "
            "EXCEPTION WHEN SQLSTATE '22012' OR numeric_value_out_of_range THEN "
            "GET STACKED DIAGNOSTICS err_state = RETURNED_SQLSTATE, err_msg := MESSAGE_TEXT; "
            "IF failed_step.SQLSTATE = err_state AND SQLERRM = err_msg AND v = 7 THEN RETURN 'caught'; END IF; RAISE; END; "
            "RETURN 'ok'; END; $$;";
        const auto converted=convertSql(Dialect::PostgreSQL,Dialect::PostgreSQL,source);
        QVERIFY2(converted.ok(),converted.diagnostics.empty()?"no SQL":converted.diagnostics.front().message.c_str());
        run(QString::fromStdString(converted.sql));
        QCOMPARE(run("SELECT handle_exc_owned(0)").rows.at(0).at(0).toString(),QString("caught"));
        QCOMPARE(run("SELECT sum(n) FROM public.exc_owned").rows.at(0).at(0).toInt(),1);
        QCOMPARE(run("SELECT handle_exc_owned(2)").rows.at(0).at(0).toString(),QString("ok"));
        QCOMPARE(run("SELECT sum(n) FROM public.exc_owned").rows.at(0).at(0).toInt(),4);
        const auto current=convertSql(Dialect::PostgreSQL,Dialect::PostgreSQL,
            "CREATE FUNCTION exc_count_owned() RETURNS BIGINT LANGUAGE plpgsql AS $$ "
            "<<outer_scope>> DECLARE count_seen BIGINT; BEGIN INSERT INTO public.exc_owned VALUES (3),(4); "
            "BEGIN GET CURRENT DIAGNOSTICS outer_scope.count_seen = ROW_COUNT; END; "
            "RETURN count_seen; END outer_scope; $$;");
        QVERIFY2(current.ok(),current.diagnostics.empty()?"no SQL":current.diagnostics.front().message.c_str());
        run(QString::fromStdString(current.sql));
        QCOMPARE(run("SELECT exc_count_owned()").rows.at(0).at(0).toLongLong(),qint64(2));
        run("DROP FUNCTION exc_count_owned()");
        run("DROP FUNCTION handle_exc_owned(INT)");
        run("DROP TABLE public.exc_owned");
    }catch(const std::exception& e){QFAIL(e.what());}}
    void numericForKeepsRangeAndLoopScope() {try {
        using namespace navidog;
        using namespace navidog::sql::compiler;
        auto db=connectDatabase(profile_);
        auto token=cancellation();
        const auto source="CREATE FUNCTION numeric_for_pg_owned() RETURNS INTEGER LANGUAGE plpgsql AS $$ "
            "DECLARE i INTEGER:=9; v INTEGER:=0; BEGIN <<outer_loop>> "
            "FOR i IN REVERSE i..1 BY 2 LOOP CONTINUE outer_loop WHEN i=5; "
            "v:=v*10+outer_loop.i; END LOOP outer_loop; RETURN v*10+i; END; $$;";
        const auto generated=convertSql(Dialect::PostgreSQL,Dialect::PostgreSQL,source);
        QVERIFY2(generated.ok(),generated.diagnostics.empty()?"no SQL":generated.diagnostics.front().message.c_str());
        const auto stored=db->execute(QString::fromStdString(generated.sql),{},token);
        QVERIFY2(stored.error.isEmpty(),qPrintable(stored.error));
        const auto rows=db->execute("SELECT numeric_for_pg_owned()",{},token);
        QVERIFY2(rows.error.isEmpty(),qPrintable(rows.error));
        QCOMPARE(rows.rows.at(0).at(0).toInt(),97319);
        db->execute("DROP FUNCTION numeric_for_pg_owned()",{},token);
    }catch(const std::exception& e){QFAIL(e.what());}}
    void positionalArgumentsAndPerformPreserveRoutineScope() {try {
        using namespace navidog;
        using namespace navidog::sql::compiler;
        auto db=connectDatabase(profile_);
        auto token=cancellation();
        const auto run=[&](const std::string &sql) {
            const auto result=db->execute(QString::fromStdString(sql),{},token);
            if (!result.error.isEmpty()) throw std::runtime_error(result.error.toStdString());
            return result;
        };
        const std::string source="CREATE OR REPLACE FUNCTION routine_scope_owned(p INT) RETURNS INT LANGUAGE plpgsql AS $$ "
            "<<outer_block>> DECLARE v INT := 7; BEGIN DECLARE v INT := v + 1; BEGIN "
            "outer_block.v := $1; PERFORM $1 WHERE $1 > 0; IF FOUND THEN RETURN outer_block.v * 100 + v; END IF; "
            "RETURN v; END; END outer_block; $$;";
        run(source);
        QCOMPARE(run("SELECT routine_scope_owned(5),routine_scope_owned(0)").rows.at(0),
                 (QVector<QVariant>{508,8}));
        const auto result=convertSql(Dialect::PostgreSQL,Dialect::PostgreSQL,source);
        QVERIFY2(result.ok(),result.diagnostics.empty()?"no routine SQL":result.diagnostics.front().message.c_str());
        run(result.sql);
        QCOMPARE(run("SELECT routine_scope_owned(5),routine_scope_owned(0)").rows.at(0),
                 (QVector<QVariant>{508,8}));
        run("DROP FUNCTION routine_scope_owned(INT)");
    }catch(const std::exception& e){QFAIL(e.what());}}
    void routineSelectIntoPreservesPostgresCardinality() {try {
        using namespace navidog;
        using namespace navidog::sql::compiler;
        auto db=connectDatabase(profile_);
        auto token=cancellation();
        const auto run=[&](const QString &sql) {
            const auto result=db->execute(sql,{},token);
            if (!result.error.isEmpty()) throw std::runtime_error(result.error.toStdString());
            return result;
        };
        run("CREATE TABLE public.into_rows_pg_owned(n INT)");
        run("INSERT INTO public.into_rows_pg_owned VALUES (3),(7)");
        for (const auto &[name, strict] : std::vector<std::pair<std::string,bool>>{
                 {"read_into_normal_owned",false},{"read_into_strict_owned",true}}) {
            const auto sql="CREATE FUNCTION " + name + "(p INT) RETURNS INT LANGUAGE plpgsql AS $$ "
                "DECLARE v INT := 99; BEGIN SELECT n INTO " + (strict ? "STRICT " : "") +
                "v FROM public.into_rows_pg_owned WHERE n<" + name + ".p ORDER BY n; RETURN v; END; $$;";
            const auto converted=convertSql(Dialect::PostgreSQL,Dialect::PostgreSQL,sql);
            QVERIFY2(converted.ok(),converted.diagnostics.empty()?"no SQL":converted.diagnostics.front().message.c_str());
            run(QString::fromStdString(converted.sql));
            QCOMPARE(run(QString::fromStdString("SELECT " + name + "(5)")).rows.at(0).at(0).toInt(),3);
            if (strict) {
                const auto error=[&](int threshold) {
                    try {
                        return db->execute(QString::fromStdString("SELECT "+name+"("+std::to_string(threshold)+")"),{},token).error;
                    } catch (const std::exception &e) { return QString::fromUtf8(e.what()); }
                };
                QVERIFY(error(2).contains("no rows"));
                QVERIFY(error(10).contains("more than one row"));
            } else {
                const auto zero=run(QString::fromStdString("SELECT " + name + "(2)"));
                const auto many=run(QString::fromStdString("SELECT " + name + "(10)"));
                QVERIFY(zero.error.isEmpty() && zero.rows.at(0).at(0).isNull());
                QVERIFY(many.error.isEmpty());
                QCOMPARE(many.rows.at(0).at(0).toInt(),3);
            }
        }
        const auto found=convertSql(Dialect::PostgreSQL,Dialect::PostgreSQL,
            "CREATE FUNCTION into_found_pg_owned(p INT) RETURNS BOOLEAN LANGUAGE plpgsql AS $$ "
            "DECLARE v INT; BEGIN SELECT n INTO v FROM public.into_rows_pg_owned "
            "WHERE n=into_found_pg_owned.p; RETURN into_found_pg_owned.FOUND; END; $$;");
        QVERIFY2(found.ok(),found.diagnostics.empty()?"no SQL":found.diagnostics.front().message.c_str());
        run(QString::fromStdString(found.sql));
        QCOMPARE(run("SELECT into_found_pg_owned(2)::int").rows.at(0).at(0).toInt(),0);
        QCOMPARE(run("SELECT into_found_pg_owned(3)::int").rows.at(0).at(0).toInt(),1);
        const auto fromMySql=convertSql(Dialect::MySQL,Dialect::PostgreSQL,
            "CREATE FUNCTION converted_into_pg_owned(p INT) RETURNS INT DETERMINISTIC "
            "BEGIN DECLARE v INT; SELECT p INTO v; RETURN v; END;");
        QVERIFY2(fromMySql.ok(),fromMySql.diagnostics.empty()?"no SQL":fromMySql.diagnostics.front().message.c_str());
        run(QString::fromStdString(fromMySql.sql));
        QCOMPARE(run("SELECT converted_into_pg_owned(8)").rows.at(0).at(0).toInt(),8);
        QVERIFY(run("SELECT converted_into_pg_owned(NULL)").rows.at(0).at(0).isNull());
        run("DROP FUNCTION read_into_normal_owned(INT),read_into_strict_owned(INT),"
            "converted_into_pg_owned(INT),into_found_pg_owned(INT)");
        run("DROP TABLE public.into_rows_pg_owned");
    }catch(const std::exception& e){QFAIL(e.what());}}
    void convertedIterateSkipsThePostTestForThatIteration() {try {
        using namespace navidog;
        using namespace navidog::sql::compiler;
        auto db=connectDatabase(profile_);
        auto token=cancellation();
        const auto translated=convertSql(Dialect::MySQL,Dialect::PostgreSQL,
            "CREATE FUNCTION iterate_owned() RETURNS INT DETERMINISTIC BEGIN "
            "DECLARE i INT DEFAULT 0; DECLARE hits INT DEFAULT 0; again: REPEAT "
            "SET i=i+1; IF i=1 THEN ITERATE again; END IF; SET hits=hits+1; "
            "UNTIL i>=1 END REPEAT again; RETURN i*10+hits; END;");
        QVERIFY2(translated.ok(),translated.diagnostics.empty()?"no output":translated.diagnostics.front().message.c_str());
        const auto created=db->executeDefinition(QString::fromStdString(translated.sql),{},token);
        QVERIFY2(created.error.isEmpty(),qPrintable(created.error));
        const auto rows=db->execute("SELECT iterate_owned()",{},token);
        QVERIFY2(rows.error.isEmpty(),qPrintable(rows.error));
        QCOMPARE(rows.rows.at(0).at(0).toInt(),21);
    }catch(const std::exception &e){QFAIL(e.what());}}
    void convertedRepeatPreservesPostTestAndNullCondition() {try {
        using namespace navidog;
        using namespace navidog::sql::compiler;
        auto db=connectDatabase(profile_);
        auto token=cancellation();
        const auto translated=convertSql(Dialect::MySQL,Dialect::PostgreSQL,
            "CREATE FUNCTION repeat_owned(p INT) RETURNS INT DETERMINISTIC BEGIN "
            "DECLARE v INT DEFAULT 0; REPEAT SET v=v+1; UNTIL v=3 OR v>=p END REPEAT; RETURN v; END;");
        QVERIFY2(translated.ok(),translated.diagnostics.empty()?"no output":translated.diagnostics.front().message.c_str());
        const auto created=db->executeDefinition(QString::fromStdString(translated.sql),{},token);
        QVERIFY2(created.error.isEmpty(),qPrintable(created.error));
        const auto rows=db->execute("SELECT repeat_owned(0),repeat_owned(2),repeat_owned(NULL)",{},token);
        QVERIFY2(rows.error.isEmpty(),qPrintable(rows.error));
        QCOMPARE(rows.rows.at(0).at(0).toInt(),1);
        QCOMPARE(rows.rows.at(0).at(1).toInt(),2);
        QCOMPARE(rows.rows.at(0).at(2).toInt(),3);
    }catch(const std::exception &e){QFAIL(e.what());}}
    void convertedCasePreservesNullAndBranchBinding() {try {
        using namespace navidog;
        using namespace navidog::sql::compiler;
        auto db=connectDatabase(profile_);
        auto token=cancellation();
        const auto source="CREATE FUNCTION case_conversion_owned(p INTEGER) RETURNS INTEGER "
                          "DETERMINISTIC SQL SECURITY INVOKER BEGIN DECLARE v INT DEFAULT p; "
                          "CASE v WHEN 1 THEN SET v=8; WHEN NULL THEN SET v=9; "
                          "ELSE CASE WHEN v IS NULL THEN SET v=2; ELSE SET v=0; END CASE; "
                          "END CASE; RETURN v; END;";
        const auto converted=convertSql(Dialect::MySQL,Dialect::PostgreSQL,source);
        QVERIFY2(converted.ok(),converted.diagnostics.empty()?"no SQL":converted.diagnostics.front().message.c_str());
        const auto created=db->executeDefinition(QString::fromStdString(converted.sql),{},token);
        QVERIFY2(created.error.isEmpty(),qPrintable(created.error));
        const auto outcome=db->execute("SELECT case_conversion_owned(1), case_conversion_owned(NULL), case_conversion_owned(3)",{},token);
        QVERIFY2(outcome.error.isEmpty(),qPrintable(outcome.error));
        QCOMPARE(outcome.rows[0][0].toInt(),8);
        QCOMPARE(outcome.rows[0][1].toInt(),2);
        QCOMPARE(outcome.rows[0][2].toInt(),0);
    }catch(const std::exception& e){QFAIL(e.what());}}
    void generatedTransactionCommandsPreserveSavepoints() {try {
        using namespace navidog;
        using namespace navidog::sql::compiler;
        auto db=connectDatabase(profile_);
        auto token=cancellation();
        ScriptContext context(Dialect::PostgreSQL,nullptr);
        for (const auto sql : {"BEGIN ISOLATION LEVEL READ COMMITTED, READ WRITE;",
                               "CREATE TEMP TABLE transaction_codegen_owned(id INT);",
                               "INSERT INTO transaction_codegen_owned VALUES(1);",
                               "SAVEPOINT \"Checkpoint\";",
                               "INSERT INTO transaction_codegen_owned VALUES(2);",
                               "CREATE TEMP TABLE transaction_undone_owned(id INT);",
                               "ROLLBACK TO \"Checkpoint\";",
                               "RELEASE \"Checkpoint\";",
                               "COMMIT AND NO CHAIN;"}) {
            const auto compiled=compile(Dialect::PostgreSQL,sql,{},&context);
            QVERIFY2(compiled->ok(),compiled->diagnostics.empty()?"invalid SQL":compiled->diagnostics.front().message.c_str());
            for (const auto root : compiled->model.roots)
                QVERIFY(!context.observe(compiled->model,root));
            const auto generated=generateSql(compiled->model,Dialect::PostgreSQL);
            QVERIFY(generated.ok());
            const auto result=db->execute(QString::fromStdString(generated.sql),{},token);
            QVERIFY2(result.error.isEmpty(),qPrintable(result.error));
        }
        const auto result=db->execute("SELECT id FROM transaction_codegen_owned",{},token);
        QVERIFY2(result.error.isEmpty(),qPrintable(result.error));
        QCOMPARE(result.rows.size(),1);
        QCOMPARE(result.rows.at(0).at(0).toInt(),1);
        QVERIFY(context.findTable({},{"transaction_codegen_owned"}));
        QVERIFY(!context.findTable({},{"transaction_undone_owned"}));
        const auto discarded=db->execute("SELECT to_regclass('pg_temp.transaction_undone_owned')",{},token);
        QVERIFY2(discarded.error.isEmpty(),qPrintable(discarded.error));
        QVERIFY(discarded.rows.at(0).at(0).isNull());
    }catch(const std::exception &e){QFAIL(e.what());}}
    void inheritedScansPreserveOnlyScope() {try {
        using namespace navidog;
        using namespace navidog::sql::compiler;
        auto db=connectDatabase(profile_);
        auto token=cancellation();
        auto run=[&](const QString &sql) {
            auto outcome=db->execute(sql,{},token);
            if (!outcome.error.isEmpty()) throw std::runtime_error(outcome.error.toStdString());
            return outcome;
        };
        run("CREATE TEMP TABLE inheritance_parent_owned(id INT PRIMARY KEY, n INT)");
        run("CREATE TEMP TABLE inheritance_child_owned() INHERITS (inheritance_parent_owned)");
        run("INSERT INTO inheritance_parent_owned VALUES(1,10)");
        run("INSERT INTO inheritance_child_owned VALUES(1,20)");
        for (const auto outer : {false,true}) {
            const auto source=std::string("SELECT p.id,p.n FROM ")+
                (outer?"inheritance_parent_owned *":"ONLY (inheritance_parent_owned)")+" AS p ORDER BY p.n;";
            auto compiled=compile(Dialect::PostgreSQL,source);
            QVERIFY2(compiled->ok(),compiled->diagnostics.empty()?"invalid SQL":compiled->diagnostics.front().message.c_str());
            const auto canonical=generateSql(compiled->model,Dialect::PostgreSQL);
            QVERIFY(canonical.ok());
            const auto original=run(QString::fromStdString(source));
            const auto generated=run(QString::fromStdString(canonical.sql));
            QCOMPARE(generated.rows,original.rows);
            QCOMPARE(generated.rows.size(),outer?2:1);
        }
        auto update=compile(Dialect::PostgreSQL,"UPDATE ONLY inheritance_parent_owned SET n=11 WHERE id=1;");
        QVERIFY(update->ok());
        auto canonical=generateSql(update->model,Dialect::PostgreSQL);
        QVERIFY(canonical.ok());
        run(QString::fromStdString(canonical.sql));
        const auto rows=run("SELECT n FROM inheritance_parent_owned ORDER BY n").rows;
        QCOMPARE(rows.at(0).at(0).toInt(),11);
        QCOMPARE(rows.at(1).at(0).toInt(),20);
    }catch(const std::exception& e){QFAIL(e.what());}}
    void setQueriesAndNamedWindowsMatchCanonicalSql() {try {
        using namespace navidog;using namespace navidog::sql::compiler;auto db=connectDatabase(profile_);auto token=cancellation();
        auto setup=db->execute("CREATE TEMPORARY TABLE table_query_owned(id INTEGER)",{},token);
        QVERIFY2(setup.error.isEmpty(),qPrintable(setup.error));
        setup=db->execute("INSERT INTO table_query_owned VALUES(2),(1),(3)",{},token);
        QVERIFY2(setup.error.isEmpty(),qPrintable(setup.error));
        for(const auto &sql:{"SELECT 1 AS id UNION ALL SELECT 2 INTERSECT SELECT 2 ORDER BY id;","(SELECT 1 AS id UNION SELECT 2) INTERSECT SELECT 2;","SELECT 1 AS id EXCEPT(SELECT 1 UNION ALL SELECT 2);","WITH q AS(SELECT 1 AS id) SELECT id FROM q UNION ALL SELECT id+1 FROM q ORDER BY id;","WITH RECURSIVE seq(n) AS (SELECT 1 UNION ALL SELECT n+1 FROM seq WHERE n<6) SELECT n FROM seq ORDER BY n;","TABLE table_query_owned ORDER BY id LIMIT 2;", "VALUES (1, 'one'), (2, 'two') ORDER BY column1 DESC;", "TABLE table_query_owned UNION ALL TABLE table_query_owned ORDER BY id;","TABLE ONLY (table_query_owned) ORDER BY id;","SELECT x,SUM(x) OVER w AS total FROM(SELECT 1 AS x UNION ALL SELECT 2) q WINDOW w AS(ORDER BY x ROWS UNBOUNDED PRECEDING) ORDER BY x;"}) {
          auto dialect=profile_.driver==DatabaseKind::PostgreSQL?Dialect::PostgreSQL:Dialect::MySQL;auto c=compile(dialect,sql);QVERIFY2(c->ok(),c->diagnostics.empty()?"no model":c->diagnostics.front().message.c_str());auto generated=generateSql(c->model,dialect);QVERIFY2(generated.ok(),generated.diagnostics.empty()?"no SQL":generated.diagnostics.front().message.c_str());
          auto original=db->execute(sql,{},token);QVERIFY2(original.error.isEmpty(),qPrintable(original.error));auto actual=db->execute(QString::fromStdString(generated.sql),{},token);QVERIFY2(actual.error.isEmpty(),qPrintable(actual.error+"\n"+QString::fromStdString(generated.sql)));QCOMPARE(actual.rows,original.rows);
        }
        for(const auto &offset:{"1.0","1.5","1e0","-1"}) {
          auto sql=QString("SELECT SUM(1) OVER(ROWS %1 PRECEDING)").arg(offset);bool accepted=false;try{accepted=db->execute(sql,{},token).error.isEmpty();}catch(const std::exception&){}
          auto c=compile(profile_.driver==DatabaseKind::PostgreSQL?Dialect::PostgreSQL:Dialect::MySQL,sql.toStdString());
          QVERIFY2(!accepted || c->ok(),qPrintable("native accepts offset "+QString(offset)+" but local checker rejects it"));
        }
    }catch(const std::exception& e){QFAIL(e.what());}}
    void convertedApplyPreservesCorrelatedRowsAndNullExtension() {try {
        using namespace navidog;
        using namespace navidog::sql::compiler;
        auto db=connectDatabase(profile_);
        auto token=cancellation();
        for (const auto &join : {"CROSS APPLY", "OUTER APPLY"}) {
            const auto source=std::string("SELECT l.id, r.n FROM (SELECT 1 AS id UNION ALL SELECT 2) l ")+
                join+" (SELECT l.id+10 AS n WHERE l.id=1) r ORDER BY l.id;";
            auto converted=convertSql(Dialect::SQLServer,Dialect::PostgreSQL,source);
            QVERIFY2(converted.ok(),converted.diagnostics.empty()?"no SQL":converted.diagnostics.front().message.c_str());
            const auto outcome=db->execute(QString::fromStdString(converted.sql),{},token);
            QVERIFY2(outcome.error.isEmpty(),qPrintable(outcome.error));
            const bool outer=std::string(join)=="OUTER APPLY";
            QCOMPARE(outcome.rows.size(),outer?2:1);
            QCOMPARE(outcome.rows[0][0].toInt(),1);
            QCOMPARE(outcome.rows[0][1].toInt(),11);
            if (outer) {
                QCOMPARE(outcome.rows[1][0].toInt(),2);
                QVERIFY(outcome.rows[1][1].isNull());
            }
        }
    }catch(const std::exception& e){QFAIL(e.what());}}
    void positionalAndDistinctOrderRulesMatchNativePg() {try {
        using namespace navidog;using namespace navidog::sql::compiler;auto db=connectDatabase(profile_);auto token=cancellation();
        auto created=db->execute("CREATE TEMP TABLE positional_owned(id INTEGER,n INTEGER,x TEXT)",{},token);QVERIFY(created.error.isEmpty());
        for(const auto &constant:{"0","1","2","3","4","-1","+1","1.0","1.5","'x'","TRUE","NULL"})
          for(const auto &source:{QString("SELECT id,n FROM positional_owned ORDER BY %1;").arg(constant),QString("SELECT COUNT(*) FROM positional_owned GROUP BY %1;").arg(constant)}) {
            auto c=compile(Dialect::PostgreSQL,source.toStdString());bool accepted=false;
            try{accepted=db->execute(source,{},token).error.isEmpty();}catch(const std::exception&){}
            QVERIFY2(c->ok()==accepted,qPrintable(source));
          }
        for(const auto &source:{"SELECT DISTINCT id FROM positional_owned ORDER BY n;","SELECT DISTINCT id FROM positional_owned ORDER BY id+1;",
            "SELECT DISTINCT id AS n FROM positional_owned ORDER BY n;","SELECT DISTINCT id+01 FROM positional_owned ORDER BY id+1;",
            "SELECT DISTINCT lower(x) FROM positional_owned ORDER BY pg_catalog.lower(x);",
            "SELECT DISTINCT id::integer FROM positional_owned ORDER BY id::int;",
            "SELECT DISTINCT (SELECT 1) FROM positional_owned ORDER BY (SELECT 1);",
            "SELECT DISTINCT id FROM positional_owned ORDER BY CAST(id AS INTEGER);",
            "SELECT DISTINCT n+1.0 FROM positional_owned ORDER BY n+1.00;"}) {
          auto c=compile(Dialect::PostgreSQL,source);bool accepted=false;try{accepted=db->execute(source,{},token).error.isEmpty();}catch(const std::exception&){}
          QVERIFY2(c->ok()==accepted,source);
        }
    }catch(const std::exception &e){QFAIL(e.what());}}
    void contextualIdentifiersMatchNativePg() {try {
        using namespace navidog;
        using namespace navidog::sql::compiler;
        auto db=connectDatabase(profile_);
        auto token=cancellation();
        const auto run=[&](const QString &sql) {
            auto result=db->execute(sql,{},token);
            if (!result.error.isEmpty()) throw std::runtime_error(result.error.toStdString());
            return result;
        };
        const QString create="CREATE TEMP TABLE key (key INT PRIMARY KEY, value INT, schema INT, view INT, "
                             "index INT, sequence INT, database INT, type INT, comment INT);";
        const auto definition=compile(Dialect::PostgreSQL,create.toStdString());
        QVERIFY2(definition->ok(),definition->diagnostics.empty()?"bad DDL":definition->diagnostics.front().message.c_str());
        run(create);
        run("INSERT INTO key VALUES (1,2,3,4,5,6,7,8,9)");
        run("INSERT INTO key (key,value) VALUES (1,11) ON CONFLICT (key) DO UPDATE SET value=excluded.value");
        for (const QString &source : {
                QString("SELECT key,value,schema,view,index,sequence,database,type,comment FROM key ORDER BY key;"),
                QString("SELECT value AS key FROM key ORDER BY key;"),
                QString("SELECT key AS key FROM key WHERE key=1 GROUP BY key ORDER BY key;"),
                QString("UPDATE key SET value=11 WHERE key=1 RETURNING key,value;")}) {
            const auto compiled=compile(Dialect::PostgreSQL,source.toStdString());
            QVERIFY2(compiled->ok(),compiled->diagnostics.empty()?"parse failed":compiled->diagnostics.front().message.c_str());
            const auto generated=generateSql(compiled->model,Dialect::PostgreSQL);
            QVERIFY2(generated.ok(),generated.diagnostics.empty()?"no SQL":generated.diagnostics.front().message.c_str());
            const auto expected=run(source),actual=run(QString::fromStdString(generated.sql));
            QCOMPARE(expected.columns,actual.columns);QCOMPARE(expected.rows,actual.rows);
        }
        QVERIFY(!compile(Dialect::PostgreSQL,"SELECT 1 FROM WHERE;")->ok());
        QVERIFY(!compile(Dialect::MySQL,"SELECT key FROM rows_owned;")->ok());
    }catch(const std::exception &e){QFAIL(e.what());}}
    void groupingSemanticRulesMatchNativePg() {try {
        using namespace navidog;using namespace navidog::sql::compiler;
        auto db=connectDatabase(profile_);auto token=cancellation();
        const std::string declarations="CREATE TEMP TABLE grouping_owned(id INTEGER PRIMARY KEY,n INTEGER); CREATE TEMP TABLE grouping_deferred(id INTEGER PRIMARY KEY DEFERRABLE,n INTEGER);";
        for(const auto &span:sql::splitStatements(declarations,sql::Dialect::PostgreSQL)){auto created=db->execute(QString::fromStdString(declarations.substr(span.begin,span.end-span.begin)),{},token);QVERIFY2(created.error.isEmpty(),qPrintable(created.error));}
        for(const auto &item:std::vector<std::pair<std::string,bool>>{
            {"SELECT id,n,COUNT(*) FROM grouping_owned GROUP BY id;",true},
            {"SELECT id,COUNT(*) FROM grouping_owned;",false},
            {"SELECT n,COUNT(*) FROM grouping_owned GROUP BY id;",true},
            {"SELECT id,n,COUNT(*) FROM grouping_deferred GROUP BY id;",false},
            {"WITH q AS(SELECT id,n FROM grouping_owned) SELECT id,n,COUNT(*) FROM q GROUP BY id;",false},
            {"SELECT SUM(SUM(n)) OVER() FROM grouping_owned;",true},
            {"SELECT COUNT(*) OVER() FROM grouping_owned ORDER BY SUM(n);",true},
            {"SELECT SUM(n) FROM grouping_owned GROUP BY 1;",false},
            {"SELECT (1=1)=TRUE,('x' NOT ILIKE 'X')=FALSE,(NULL IS DISTINCT FROM NULL)=FALSE;",true}}) {
          auto parsed=compile(Dialect::PostgreSQL,declarations+item.first);QCOMPARE(parsed->ok(),item.second);
          bool accepted=false;try{accepted=db->execute(QString::fromStdString(item.first),{},token).error.isEmpty();}catch(const std::exception&){}
          QVERIFY2(accepted==item.second,item.first.c_str());
          if(accepted) {
            auto single=compile(Dialect::PostgreSQL,item.first);QVERIFY(single->ok());auto emitted=generateSql(single->model,Dialect::PostgreSQL);QVERIFY(emitted.ok());
            auto original=db->execute(QString::fromStdString(item.first),{},token);auto actual=db->execute(QString::fromStdString(emitted.sql),{},token);QVERIFY(actual.error.isEmpty());QCOMPARE(actual.rows,original.rows);
          }
        }
    }catch(const std::exception &e){QFAIL(e.what());}}
    void mergeIrMatchesNativePgAndRollback() {try {
      using namespace navidog;using namespace navidog::sql::compiler;auto db=connectDatabase(profile_);auto token=cancellation();
      auto run=[&](const QString &text){auto r=db->execute(text,{},token);if(!r.error.isEmpty())throw std::runtime_error(r.error.toStdString());return r;};
      run("CREATE TABLE merge_target_owned(id INTEGER PRIMARY KEY,n INTEGER)");run("CREATE TABLE merge_source_owned(id INTEGER,n INTEGER)");run("INSERT INTO merge_source_owned VALUES(1,11),(2,-1),(4,40)");
      auto reset=[&]{run("TRUNCATE merge_target_owned");run("INSERT INTO merge_target_owned VALUES(1,10),(2,20),(3,30)");};reset();
      const std::string text="MERGE INTO merge_target_owned AS t USING merge_source_owned AS s ON t.id=s.id WHEN MATCHED AND s.n<0 THEN DELETE WHEN MATCHED THEN UPDATE SET n=s.n WHEN NOT MATCHED THEN INSERT(id,n) VALUES(s.id,s.n);";
      auto c=compile(Dialect::PostgreSQL,text);QVERIFY2(c->ok(),c->diagnostics.empty()?"no MERGE model":c->diagnostics.front().message.c_str());auto generated=generateSql(c->model,Dialect::PostgreSQL);QVERIFY(generated.ok());
      namespace opt=navidog::sql::optimizer;
      const MetadataCatalog catalog(std::make_shared<const MetadataSnapshot>(
          collectMetadata(*db,profile_,"public",token)));
      const NameResolutionContext names{profile_.database.toStdString(),"public",Dialect::PostgreSQL};
      const auto bound=compile(Dialect::PostgreSQL,text,{},&catalog,names);
      QVERIFY(bound->ok());
      const auto planned=opt::optimize(bound->model);
      QVERIFY2(planned.ok(),planned.diagnostics.empty()?"no MERGE plan":planned.diagnostics.front().message.c_str());
      const auto &modify=planned.logical.nodes.at(planned.logical.root);
      QCOMPARE(modify.kind,opt::RelKind::Modify);
      const auto &candidate=planned.logical.nodes.at(modify.inputs.front());
      QCOMPARE(candidate.kind,opt::RelKind::Join);
      QCOMPARE(candidate.join,JoinKind::Right);
      QVERIFY(candidate.fixedOrder && !candidate.predicates.empty());
      const auto report=opt::dumpOptimization(planned);
      QVERIFY(report.find("\"joinKind\":\"right\"")!=std::string::npos);
      const auto removal=report.find("\"action\":\"delete\""), update=report.find("\"action\":\"update\""),
                 insert=report.find("\"action\":\"insert\"");
      QVERIFY(removal<update && update<insert && insert!=std::string::npos);
      QVERIFY(!opt::prepareLocalQuery(bound->model).ok());
      QVERIFY(!opt::generatePlanSql(planned.logical,planned.logical.root).ok());
      const auto sourceOnly=compile(Dialect::SQLServer,
          "MERGE INTO dbo.target_rows t USING dbo.source_rows s ON t.id=s.id "
          "WHEN MATCHED THEN UPDATE SET n=s.n WHEN NOT MATCHED BY TARGET THEN INSERT(id,n) VALUES(s.id,s.n) "
          "WHEN NOT MATCHED BY SOURCE THEN DELETE;");
      QVERIFY2(sourceOnly->ok(),sourceOnly->diagnostics.empty()?"no SQL Server MERGE model":
               sourceOnly->diagnostics.front().message.c_str());
      const auto both=opt::optimize(sourceOnly->model);
      QVERIFY2(both.ok(),both.diagnostics.empty()?"no SQL Server MERGE plan":
               both.diagnostics.front().message.c_str());
      QCOMPARE(both.logical.nodes.at(both.logical.nodes.at(both.logical.root).inputs.front()).join,JoinKind::Full);
      run(QString::fromStdString(text));auto expected=run("SELECT id,n FROM merge_target_owned ORDER BY id").rows;QCOMPARE(expected.size(),3);QCOMPARE(expected[0][1].toInt(),11);QCOMPARE(expected[2][0].toInt(),4);
      reset();run(QString::fromStdString(generated.sql));QCOMPARE(run("SELECT id,n FROM merge_target_owned ORDER BY id").rows,expected);
      reset();auto initial=run("SELECT id,n FROM merge_target_owned ORDER BY id").rows;run("BEGIN");run(QString::fromStdString(generated.sql));run("ROLLBACK");QCOMPARE(run("SELECT id,n FROM merge_target_owned ORDER BY id").rows,initial);
      run("DROP TABLE merge_source_owned");run("DROP TABLE merge_target_owned");
    }catch(const std::exception& e){QFAIL(e.what());}}
    void aggregateFiltersMatchNativePgAndGeneratedSql() {try {
      using namespace navidog;using namespace navidog::sql::compiler;auto db=connectDatabase(profile_);auto token=cancellation();
      for(const auto &text:{"WITH t(x) AS(SELECT 1 UNION ALL SELECT 2 UNION ALL SELECT NULL) SELECT COUNT(*) FILTER(WHERE x>1),SUM(x) FILTER(WHERE x IS NOT NULL) FROM t;","WITH t(x) AS(SELECT 1 UNION ALL SELECT 2) SELECT x,SUM(x) FILTER(WHERE x>1) OVER w FROM t WINDOW w AS(ORDER BY x) ORDER BY x;"}) {
        auto c=compile(Dialect::PostgreSQL,text);QVERIFY2(c->ok(),c->diagnostics.empty()?"no model":c->diagnostics.front().message.c_str());auto generated=generateSql(c->model,Dialect::PostgreSQL);QVERIFY(generated.ok());
        auto expected=db->execute(text,{},token);QVERIFY2(expected.error.isEmpty(),qPrintable(expected.error));auto actual=db->execute(QString::fromStdString(generated.sql),{},token);QVERIFY2(actual.error.isEmpty(),qPrintable(actual.error));QCOMPARE(actual.rows,expected.rows);
      }
    }catch(const std::exception& e){QFAIL(e.what());}}
    void outputAliasesRespectNativeScopeRules() {try {
      using namespace navidog;using namespace navidog::sql::compiler;
      auto db=connectDatabase(profile_);auto token=cancellation();
      auto run=[&](const QString &text){auto r=db->execute(text,{},token);if(!r.error.isEmpty())throw std::runtime_error(r.error.toStdString());return r;};
      run("CREATE TABLE alias_scope_owned(x INTEGER,g INTEGER)");
      run("INSERT INTO alias_scope_owned VALUES(1,10),(2,10),(3,20)");
      auto snapshot=collectMetadata(*db,profile_,"public",token);
      MetadataCatalog catalog(std::make_shared<const MetadataSnapshot>(snapshot));
      NameResolutionContext names{profile_.database.toStdString(),"public",Dialect::PostgreSQL};
      for(const auto &text:{"SELECT x+1 AS z FROM alias_scope_owned GROUP BY z ORDER BY z;","SELECT SUM(x) AS total FROM alias_scope_owned HAVING SUM(x)>1;"}) {
        auto c=compile(Dialect::PostgreSQL,text,{},&catalog,names);QVERIFY2(c->ok(),c->diagnostics.empty()?"no model":c->diagnostics.front().message.c_str());
        auto sql=generateSql(c->model,Dialect::PostgreSQL);QVERIFY2(sql.ok(),"no generated alias SQL");QCOMPARE(run(QString::fromStdString(sql.sql)).rows,run(text).rows);
      }

      run("DROP TABLE alias_scope_owned");
    }catch(const std::exception& e){QFAIL(e.what());}}
    void plannerCatalogStatisticsUseNativeRowsColumnsAndIndexes() {try {
        using namespace navidog;auto token=cancellation();auto db=connectDatabase(profile_);
        auto run=[&](const QString &sql){auto r=db->execute(sql,{},token);if(!r.error.isEmpty())throw std::runtime_error(r.error.toStdString());return r;};
        run("CREATE TABLE planner_stats_owned(id INTEGER PRIMARY KEY,n INTEGER)");run("INSERT INTO planner_stats_owned VALUES(1,1),(2,1),(3,2),(4,NULL),(5,NULL)");
        run("ANALYZE planner_stats_owned");
        auto stats=collectPlannerStatistics(*db,profile_,"public",token);QVERIFY2(stats.complete,qPrintable(stats.warning));
        const auto found=std::find_if(stats.tables.begin(),stats.tables.end(),[](const auto &t){return t.name=="planner_stats_owned";});QVERIFY(found!=stats.tables.end());QVERIFY(found->rows>=0);QCOMPARE(found->source,sql::optimizer::StatisticsSource::ServerCatalog);QVERIFY(!found->indexes.empty());
        QVERIFY(std::any_of(found->indexes.begin(),found->indexes.end(),[](const auto &i){return i.unique && i.columns==std::vector<std::string>{"id"};}));
        QVERIFY(std::any_of(found->columns.begin(),found->columns.end(),[](const auto &c){return c.name=="id" && c.distinct.has_value();}));
        QCOMPARE(run("SELECT COUNT(*) FROM planner_stats_owned").rows[0][0].toInt(),5);run("DROP TABLE planner_stats_owned");
    }catch(const std::exception& e){QFAIL(e.what());}}
    void estimatedJsonPlanDoesNotExecuteDelete() {try {
        using namespace navidog;auto token=cancellation();auto db=connectDatabase(profile_);
        auto run=[&](const QString &sql){auto r=db->execute(sql,{},token);if(!r.error.isEmpty())throw std::runtime_error(r.error.toStdString());return r;};
        run("CREATE TABLE estimated_plan_owned(id INTEGER PRIMARY KEY)");run("INSERT INTO estimated_plan_owned VALUES(1),(2)");
        PlanRequestTrace trace;auto raw=readQueryPlan(profile_,"DELETE FROM estimated_plan_owned WHERE id=1",{},token,{},"public",&trace);QVERIFY(trace.issued && trace.received);QVERIFY(trace.commands.front().startsWith("EXPLAIN (FORMAT JSON"));QVERIFY(trace.commands.front().contains("ANALYZE FALSE"));QVERIFY(!trace.serverVersion.isEmpty());
        QVERIFY2(raw.error.isEmpty(),qPrintable(raw.error));auto plan=decodeEstimatedPlan(profile_.driver,raw);QVERIFY2(plan.complete,qPrintable(plan.warning));QVERIFY(!plan.nodes.isEmpty());
        QCOMPARE(run("SELECT COUNT(*) FROM estimated_plan_owned").rows[0][0].toInt(),2);run("DROP TABLE estimated_plan_owned");
    }catch(const std::exception& e){QFAIL(e.what());}}
    void compilerAnalysisAndServerExecutionAgreeOnNestedSelects() {
        using namespace navidog;QueryService service;QSignalSpy done(&service,&QueryService::completed);QueryExecutionOptions options;options.sessionId="owned-pg-compiler";
        auto run=[&](QString sql){if(!service.run(profile_,sql,{},options)||!done.wait(3000))throw std::runtime_error("owned PG compiler query timeout");return qvariant_cast<QueryOutcome>(done.takeFirst()[0]);};
        auto result=run("WITH q AS(SELECT 7 AS n) SELECT CASE WHEN n=7 THEN n+1 ELSE 0 END, (SELECT max(x.n) FROM q x WHERE x.n=q.n) FROM q");
        QVERIFY2(result.result.error.isEmpty(),qPrintable(result.result.error));QCOMPARE(result.result.rows[0][0].toInt(),8);QCOMPARE(result.result.rows[0][1].toInt(),7);
        QCOMPARE(result.analysis.statements[0].state,SqlAnalysisState::Parsed);QCOMPARE(result.analysis.statements[0].queries,3);
        result=run("WITH q AS(SELECT 7 AS n) SELECT row_number() OVER w FROM q WINDOW w AS(ORDER BY n)");
        QVERIFY2(result.result.error.isEmpty(),qPrintable(result.result.error));QCOMPARE(result.result.rows[0][0].toInt(),1);QCOMPARE(result.analysis.statements[0].state,SqlAnalysisState::Parsed);
        result=run("SELECT $body$原文;/*literal*/$body$");QCOMPARE(result.result.rows[0][0].toString(),QString("原文;/*literal*/"));
    }
    void vacuumCompileFormatAndExecuteOwnedPostgres() {try {
        using namespace navidog;
        using namespace navidog::sql::compiler;
        auto db=connectDatabase(profile_);
        auto token=cancellation();
        const auto run=[&](const QString &sql) {
            auto result=db->execute(sql,{},token);
            if (!result.error.isEmpty()) throw std::runtime_error(result.error.toStdString());
            return result;
        };
        run("CREATE TABLE public.vacuum_owned (id INT, \"Mixed Case\" TEXT)");
        run("CREATE TABLE public.vacuum_second_owned (id INT)");
        run("INSERT INTO public.vacuum_owned VALUES(1,'retained'),(2,'deleted')");
        run("DELETE FROM public.vacuum_owned WHERE id=2");
        const MetadataCatalog catalog(std::make_shared<const MetadataSnapshot>(collectMetadata(*db,profile_,"public",token)));
        const NameResolutionContext scope{profile_.database.toStdString(),"public",Dialect::PostgreSQL};
        for (const auto *source : {
             "vacuum analyse public.vacuum_owned(id);",
             "vacuum (analyze on, index_cleanup 'auto', parallel +0, skip_locked off) "
             "public.vacuum_owned(\"Mixed Case\"), public.vacuum_second_owned;",
             "vacuum (full, full false, parallel 2, truncate 0, freeze 1) public.vacuum_owned;"}) {
            const auto compiled=compile(scope.dialect,source,{},&catalog,scope);
            QVERIFY2(compiled->ok(),compiled->diagnostics.empty()?source:compiled->diagnostics.front().message.c_str());
            const auto &model=compiled->model.front();
            QVERIFY(model.kind==StatementKind::Vacuum && model.vacuum);
            QVERIFY(model.transactionEffect==TransactionEffect::ForbiddenInTransaction);
            const auto same=convertSql(scope.dialect,scope.dialect,source);
            QVERIFY2(same.ok(),same.diagnostics.empty()?source:same.diagnostics.front().message.c_str());
            QVERIFY(!convertSql(scope.dialect,Dialect::MySQL,source).ok());
            const auto formatted=formatSql(scope.dialect,source);
            QVERIFY(formatted.ok() && formatted.sql.find("VACUUM")!=std::string::npos);
            if (std::string_view(source).find("vacuum (")==0) QVERIFY(formatted.sql.find("VACUUM (")==0);
            for (const auto &sql : {std::string(source),same.sql,formatted.sql}) run(QString::fromStdString(sql));
        }
        for (const auto *source : {
             "VACUUM (FULL, PARALLEL 1) public.vacuum_owned;",
             "VACUUM (FULL, DISABLE_PAGE_SKIPPING) public.vacuum_owned;",
             "VACUUM (FULL, PROCESS_TOAST FALSE) public.vacuum_owned;",
             "VACUUM (ANALYZE FALSE) public.vacuum_owned(id);",
             "VACUUM (PARALLEL 1025) public.vacuum_owned;"}) {
            QVERIFY(!compile(scope.dialect,source,{},&catalog,scope)->ok());
            bool nativeRejected=false;
            try { run(QString::fromUtf8(source)); } catch (const std::exception &) { nativeRejected=true; }
            QVERIFY2(nativeRejected,source);
        }
        QueryService service;
        QSignalSpy completed(&service,&QueryService::completed);
        QueryExecutionOptions options;options.sessionId="owned-pg-vacuum";
        QVERIFY(service.run(profile_,"VACUUM (ANALYZE) public.vacuum_owned",{},options));
        QTRY_COMPARE_WITH_TIMEOUT(completed.size(),1,3000);
        const auto result=qvariant_cast<QueryOutcome>(completed.takeFirst().front());
        QVERIFY2(result.result.error.isEmpty(),qPrintable(result.result.error));
        QCOMPARE(result.analysis.statements.front().state,SqlAnalysisState::Parsed);
        run("BEGIN");
        bool inTransactionRejected=false;
        try { run("VACUUM public.vacuum_owned"); } catch (const std::exception &) { inTransactionRejected=true; }
        run("ROLLBACK");
        QVERIFY(inTransactionRejected);
        QCOMPARE(run("SELECT id FROM public.vacuum_owned").rows.front().front().toInt(),1);
        run("DROP TABLE public.vacuum_owned,public.vacuum_second_owned");
    }catch(const std::exception &e){QFAIL(e.what());}}
    void copyClientStreamGrammarMatchesOwnedPostgres() {try {
        using namespace navidog;
        using namespace navidog::sql::compiler;
        auto db=connectDatabase(profile_);
        auto token=cancellation();
        const auto run=[&](const QString &sql) {
            auto result=db->execute(sql,{},token);
            if (!result.error.isEmpty()) throw std::runtime_error(result.error.toStdString());
            return result;
        };
        const auto psql=[&](const std::string &script) {
            QProcess process;
            const auto client=QFileInfo(pgCtl_).path()+"/psql";
            process.start(client,{"-X","-q","-v","ON_ERROR_STOP=1","-h",profile_.host,"-p",
                QString::number(profile_.port),"-U",profile_.user,"-d",profile_.database,"-f","-"});
            if (!process.waitForStarted(3000)) throw std::runtime_error("owned PostgreSQL psql could not start");
            process.write(QByteArray::fromStdString(script));
            process.closeWriteChannel();
            if (!process.waitForFinished(5000)) { process.kill(); process.waitForFinished(); throw std::runtime_error("owned COPY timed out"); }
            if (process.exitCode()!=0 || process.exitStatus()!=QProcess::NormalExit)
                throw std::runtime_error(process.readAllStandardError().toStdString());
            return process.readAllStandardOutput();
        };
        run("CREATE TABLE public.copy_stream_owned(id INT,label TEXT)");
        const MetadataCatalog catalog(std::make_shared<const MetadataSnapshot>(collectMetadata(*db,profile_,"public",token)));
        const NameResolutionContext context{profile_.database.toStdString(),"public",Dialect::PostgreSQL};
        const std::string input="copy public.copy_stream_owned (id,label) from stdin with "
            "(format csv, header match, delimiter '|', force_null (label)) where id>=2;";
        const auto compiled=compile(context.dialect,input,{},&catalog,context);
        QVERIFY2(compiled->ok(),compiled->diagnostics.empty()?input.c_str():compiled->diagnostics.front().message.c_str());
        const auto &model=compiled->model.front();
        QVERIFY(model.kind==StatementKind::Copy && model.copy && model.target && model.where);
        QVERIFY(model.copy->header==CopyHeader::Match && model.copy->forceNull.size()==1);
        const auto &left=model.expressions.at(model.expressions.at(model.where).operands.front());
        QVERIFY(left.binding && left.binding->scope==model.id && left.binding->column==0);
        const auto formatted=formatSql(context.dialect,input), formattedAgain=formatSql(context.dialect,formatted.sql);
        QVERIFY(formatted.ok() && formatted.syntaxSupported && formattedAgain.ok());
        QCOMPARE(formatted.sql,formattedAgain.sql);
        const auto generated=convertSql(context.dialect,context.dialect,input);
        QVERIFY2(generated.ok(),generated.diagnostics.empty()?input.c_str():generated.diagnostics.front().message.c_str());
        QVERIFY(!convertSql(context.dialect,Dialect::SQLServer,input).ok());
        for (const auto &header : {input,formatted.sql,generated.sql}) {
            run("TRUNCATE TABLE public.copy_stream_owned");
            psql(header+"\nid|label\n1|skipped\n3|kept\n4|\"\"\n\\.\n");
            const auto rows=run("SELECT id,label FROM public.copy_stream_owned ORDER BY id").rows;
            QCOMPARE(rows.size(),2);
            QCOMPARE(rows.at(0).at(0).toInt(),3);
            QCOMPARE(rows.at(0).at(1).toString(),QString("kept"));
            QCOMPARE(rows.at(1).at(0).toInt(),4);
            QVERIFY(!rows.at(1).at(1).isValid());
        }
        const std::string output="COPY (SELECT id,label FROM public.copy_stream_owned ORDER BY id) TO STDOUT "
                                 "WITH (FORMAT csv, FORCE_QUOTE (label), NULL 'none');";
        const auto exported=convertSql(context.dialect,context.dialect,output);
        QVERIFY2(exported.ok(),exported.diagnostics.empty()?output.c_str():exported.diagnostics.front().message.c_str());
        QCOMPARE(psql(output),psql(exported.sql));
        QCOMPARE(psql(exported.sql),QByteArray("3,\"kept\"\n4,none\n"));
        for (const auto *invalid : {"COPY public.copy_stream_owned TO STDOUT (FORMAT binary, HEADER true);",
             "COPY public.copy_stream_owned TO STDOUT (FORMAT csv, HEADER MATCH);",
             "COPY public.copy_stream_owned (id) FROM STDIN (FORMAT csv, FORCE_NULL (label));"}) {
            QVERIFY2(!compile(context.dialect,invalid,{},&catalog,context)->ok(),invalid);
            bool rejected=false;
            try {psql(invalid);} catch (const std::exception &) {rejected=true;}
            QVERIFY2(rejected,invalid);
        }
        run("DROP TABLE public.copy_stream_owned");
    }catch(const std::exception &e){QFAIL(e.what());}}
    void metadataBulkCollectionAndRoutineValidation(){try{
        using namespace navidog;using namespace navidog::sql::compiler;auto db=connectDatabase(profile_);auto token=cancellation();
        auto run=[&](const QString& sql){auto result=db->execute(sql,{},token);if(!result.error.isEmpty())throw std::runtime_error(result.error.toStdString());};
        run("CREATE TABLE public.metadata_pg_owned(id int,note text)");
        run("CREATE FUNCTION public.metadata_sum(a int,b int DEFAULT 2) RETURNS int LANGUAGE sql AS $$ SELECT a+b $$");
        run("CREATE FUNCTION public.metadata_sum(a text) RETURNS text LANGUAGE sql AS $$ SELECT a $$");
        run("CREATE PROCEDURE public.metadata_proc(IN a int,OUT b int) LANGUAGE plpgsql AS $$ BEGIN b:=a+1;END $$");
        auto snapshot=collectMetadata(*db,profile_,"public",token);QVERIFY(snapshot.tablesComplete);QVERIFY(snapshot.columnsComplete);QVERIFY(snapshot.routinesComplete);
        MetadataCatalog catalog(std::make_shared<const MetadataSnapshot>(snapshot));NameResolutionContext context{profile_.database.toStdString(),"public",Dialect::PostgreSQL};
        auto routines=catalog.findRoutines(context,{"public","metadata_sum"},false);QCOMPARE(routines.size(),std::size_t(2));
        for(const auto& sql:{"SELECT id,note FROM public.metadata_pg_owned", "SELECT public.metadata_sum(1)","SELECT public.metadata_sum(1,2)","CALL public.metadata_proc(1,NULL)"}){auto c=compile(Dialect::PostgreSQL,sql,{},&catalog,context);QVERIFY2(c->ok(),c->diagnostics.empty()?"missing diagnostic":c->diagnostics[0].message.c_str());}
        const auto arity=compile(Dialect::PostgreSQL,"SELECT public.metadata_sum(1,2), public.metadata_sum(1);",{},&catalog,context);
        QVERIFY(arity->ok());
        const auto &root=arity->model.front();
        const auto &exact=arity->model.types.get(root.expressions[root.projections[0].expression].sqlType);
        QCOMPARE(exact.family,TypeFamily::Integer);QCOMPARE(exact.bits,std::optional<int>(32));
        const auto &ambiguous=root.expressions[root.projections[1].expression];
        QVERIFY(!ambiguous.routineType);
        QCOMPARE(arity->model.types.get(ambiguous.sqlType).family,TypeFamily::Unknown);
        auto invalid=compile(Dialect::PostgreSQL,"SELECT public.metadata_sum()",{},&catalog,context);QVERIFY(!invalid->ok());QCOMPARE(invalid->diagnostics[0].code,DiagnosticCode::RoutineArgumentCount);
        invalid=compile(Dialect::PostgreSQL,"SELECT absent FROM public.metadata_pg_owned",{},&catalog,context);QVERIFY(!invalid->ok());QCOMPARE(invalid->diagnostics[0].code,DiagnosticCode::UnknownColumn);
        invalid=compile(Dialect::PostgreSQL,"CALL public.metadata_proc(1)",{},&catalog,context);QVERIFY(!invalid->ok());
        run("DROP PROCEDURE public.metadata_proc(int)");run("DROP FUNCTION public.metadata_sum(int,int)");run("DROP FUNCTION public.metadata_sum(text)");run("DROP TABLE public.metadata_pg_owned");
    }catch(const std::exception& e){QFAIL(e.what());}}
    void typedLiteralCanonicalSqlMatchesServer() {try {
      using namespace navidog;using namespace navidog::sql::compiler;
      auto db=connectDatabase(profile_);auto token=cancellation();
      auto run=[&](const QString &sql){auto r=db->execute(sql,{},token);if(!r.error.isEmpty())throw std::runtime_error(r.error.toStdString());return r;};
      const auto dialect=Dialect::PostgreSQL;
      for(const auto &sql:{"SELECT CAST('1 day 12:34:56.1234' AS INTERVAL DAY TO SECOND(3)), CAST('1-2' AS INTERVAL YEAR TO MONTH)","SELECT INTERVAL '1-2' YEAR TO MONTH,INTERVAL '2 03:04:05.6789' DAY TO SECOND(3),INTERVAL(3) '1.234567 second'","SELECT EXTRACT(YEAR FROM DATE '2024-02-29'),EXTRACT('epoch' FROM TIMESTAMP '2024-02-29 12:34:56'),DATE '2024-02-29'+INTERVAL '1 month'","SELECT EXTRACT(SECOND FROM INTERVAL '1 day 12:34:56.12345')","SELECT DATE '2024-02-29', TIME '12:34:56.123456', TIMESTAMP '2024-02-29 12:34:56.123456'","SELECT TIMESTAMP(3) WITH TIME ZONE '2024-02-29 12:34:56.123456+08', NUMERIC(6,-2) '12345', VARCHAR(3) 'abcdef', INTERVAL '1 day'","SELECT DOUBLE PRECISION '1.25', TEXT $$a'b$$"}) {
        auto c=compile(dialect,sql);QVERIFY2(c->ok(),c->diagnostics.empty()?"no model":c->diagnostics.front().message.c_str());
        auto generated=generateSql(c->model,dialect);QVERIFY2(generated.ok(),generated.diagnostics.empty()?"no SQL":generated.diagnostics.front().message.c_str());
        QCOMPARE(run(QString::fromStdString(generated.sql)).rows,run(sql).rows);
      }
      for(const auto &source:{Dialect::MySQL,Dialect::PostgreSQL,Dialect::Oracle}) {
        const auto sql=std::string("SELECT DATE '2024-02-29'")+(source==Dialect::Oracle?" FROM SYS.DUAL":"");
        auto conversion=convertSql(source,dialect,sql);QVERIFY2(conversion.ok(),conversion.diagnostics.empty()?"no SQL":conversion.diagnostics.front().message.c_str());
        QCOMPARE(run(QString::fromStdString(conversion.sql)).rows,run("SELECT DATE '2024-02-29'").rows);
      }
    }catch(const std::exception &e){QFAIL(e.what());}}
    void qualifiedNativeTypeFactsMatchServer() {try {
      using namespace navidog;using namespace navidog::sql::compiler;
      auto db=connectDatabase(profile_);auto token=cancellation();
      const auto sql="SELECT CAST(42 AS PG_CATALOG.INT4),CAST(42 AS pg_catalog.\"int8\"),CAST('abcdef' AS pg_catalog.bpchar),CAST(1 AS float4),CAST(1 AS float8)";
      auto c=compile(Dialect::PostgreSQL,sql);QVERIFY(c->ok());auto generated=generateSql(c->model,Dialect::PostgreSQL);QVERIFY(generated.ok());
      auto original=db->execute(sql,{},token),actual=db->execute(QString::fromStdString(generated.sql),{},token);QVERIFY2(original.error.isEmpty(),qPrintable(original.error));QVERIFY2(actual.error.isEmpty(),qPrintable(actual.error));QCOMPARE(actual.rows,original.rows);QCOMPARE(actual.rows[0][2].toString(),QString("abcdef"));
      for(const auto &type:{"pg_catalog.bool(1)","pg_catalog.float4(3)","float4(3)","float8(3)"}) {
        const auto bad=QString("SELECT CAST(NULL AS %1)").arg(type);bool rejected=false;try{rejected=!db->execute(bad,{},token).error.isEmpty();}catch(const std::exception &){rejected=true;}QVERIFY2(rejected,qPrintable(bad));QVERIFY(!compile(Dialect::PostgreSQL,bad.toStdString())->ok());
      }
    }catch(const std::exception &e){QFAIL(e.what());}}
    void windowExclusionsMatchServerPeers() {try {
      using namespace navidog;using namespace navidog::sql::compiler;
      auto db=connectDatabase(profile_);auto token=cancellation();const auto dialect=Dialect::PostgreSQL;
      for(const auto &mode:{QString("NO OTHERS"),QString("CURRENT ROW"),QString("GROUP"),QString("TIES")}) {
        const auto sql=QString("WITH t(x) AS(SELECT 1 UNION ALL SELECT 1 UNION ALL SELECT 2) SELECT x,COUNT(*) OVER(ORDER BY x ROWS BETWEEN UNBOUNDED PRECEDING AND UNBOUNDED FOLLOWING EXCLUDE %1) AS n FROM t ORDER BY x").arg(mode);
        auto original=db->execute(sql,{},token);QVERIFY2(original.error.isEmpty(),qPrintable(original.error));QCOMPARE(original.rows.size(),3);
        const int first=mode=="NO OTHERS"?3:mode=="GROUP"?1:2,last=mode=="CURRENT ROW"?2:mode=="GROUP"?2:3;
        QCOMPARE(original.rows[0][1].toInt(),first);QCOMPARE(original.rows[1][1].toInt(),first);QCOMPARE(original.rows[2][1].toInt(),last);
        auto c=compile(dialect,sql.toStdString());QVERIFY2(c->ok(),c->diagnostics.empty()?"no model":c->diagnostics.front().message.c_str());
        auto generated=generateSql(c->model,dialect);QVERIFY(generated.ok());auto actual=db->execute(QString::fromStdString(generated.sql),{},token);QVERIFY2(actual.error.isEmpty(),qPrintable(actual.error));QCOMPARE(actual.rows,original.rows);
      }
      for(const auto &bad:{"SELECT 1 WINDOW w AS(ROWS INTERVAL '1' DAY PRECEDING)","SELECT 1 WINDOW w AS(ROWS SUM(1) PRECEDING)","WITH t(n) AS(SELECT 1) SELECT 1 FROM t WINDOW w AS(ROWS n PRECEDING)"}) {
        bool rejected=false;try{rejected=!db->execute(bad,{},token).error.isEmpty();}catch(const std::exception &){rejected=true;}QVERIFY2(rejected,bad);QVERIFY(!compile(dialect,bad)->ok());
      }
    }catch(const std::exception &e){QFAIL(e.what());}}
    void windowExtremaMatchNativeMovingFrames() {try {
        using namespace navidog;
        using namespace navidog::sql::compiler;
        using namespace navidog::sql::optimizer;
        auto db=connectDatabase(profile_);
        const auto token=cancellation();
        const auto run=[&](const std::string &sql) {
            const auto result=db->execute(QString::fromStdString(sql),{},token);
            if (!result.error.isEmpty()) throw std::runtime_error(result.error.toStdString());
            return result;
        };
        const std::string base="WITH t(id,k,v) AS (VALUES (1,1,CAST(-9223372036854775808 AS bigint)), "
            "(2,1,NULL::bigint),(3,2,8::bigint),(4,3,9223372036854775807::bigint),(5,4,3::bigint)) "
            "SELECT id,MIN(v) FILTER (WHERE id<>3) OVER w AS lo,MAX(v) OVER w AS hi FROM t WINDOW w AS (";
        for (const auto *frame:{
            "ORDER BY k,id ROWS BETWEEN 1 PRECEDING AND 2 FOLLOWING EXCLUDE CURRENT ROW",
            "ORDER BY k GROUPS BETWEEN 1 PRECEDING AND 1 FOLLOWING EXCLUDE TIES",
            "ORDER BY k RANGE BETWEEN 1 PRECEDING AND CURRENT ROW EXCLUDE GROUP",
            "ORDER BY k DESC RANGE BETWEEN CURRENT ROW AND 2 FOLLOWING"}) {
            const auto sql=base+frame+") ORDER BY id";
            const auto compiled=compile(Dialect::PostgreSQL,sql);
            QVERIFY2(compiled->ok(),compiled->diagnostics.empty()?sql.c_str():compiled->diagnostics.front().message.c_str());
            const auto native=run(sql);
            const auto actual=interpret(compiled->model);
            QVERIFY2(actual.ok(),actual.diagnostics.empty()?sql.c_str():actual.diagnostics.front().message.c_str());
            std::vector<LocalRow> rows;
            for (const auto &source:native.rows) {
                LocalRow row;
                for (const auto &field:source)
                    row.push_back(field.isNull()?LocalValue{}:LocalValue{std::int64_t(field.toLongLong())});
                rows.push_back(std::move(row));
            }
            QVERIFY(actual.rows==rows);
            const auto plan=optimize(compiled->model);
            QVERIFY(plan.ok());
            QVERIFY(std::any_of(plan.best.nodes.begin(),plan.best.nodes.end(),[](const auto &node) {
                return node.kind==PhysicalKind::Window && node.description.find("2 frame extrema")!=std::string::npos;
            }));
            const auto generated=generatePlanSql(plan.logical,plan.heuristicRoot);
            QVERIFY2(generated.ok(),generated.diagnostics.empty()?sql.c_str():generated.diagnostics.front().message.c_str());
            const auto emitted=run(generated.sql);
            QCOMPARE(emitted.rows,native.rows);
            QCOMPARE(emitted.columnTypes,native.columnTypes);
        }
    }catch(const std::exception &e){QFAIL(e.what());}}
    void windowOffsetsAreEvaluatedPerNativeRow() {try {
        using namespace navidog;
        using namespace navidog::sql::compiler;
        using namespace navidog::sql::optimizer;
        auto db=connectDatabase(profile_);
        const auto token=cancellation();
        const std::string input="WITH t(id,g,v,d,fallback) AS (VALUES (1,1,10,1,-1),(2,1,20,0,-2),"
            "(3,1,NULL,-1,-3),(4,1,40,NULL,-4),(5,1,50,6,-5),(6,1,NULL,0,-6), (7,2,70,1,-7)) ";
        const std::string window=" FROM t WINDOW w AS (PARTITION BY g ORDER BY id) ORDER BY id";
        const auto compare=[&](const Interpretation &actual,const QueryResult &native) {
            QVERIFY2(native.error.isEmpty(),qPrintable(native.error));
            QVERIFY2(actual.ok(),actual.diagnostics.empty()?"no local window result":actual.diagnostics.front().message.c_str());
            std::vector<LocalRow> expected;
            for (const auto &row:native.rows) {
                LocalRow values;
                for (const auto &cell:row)
                    values.push_back(cell.isNull()?LocalValue{}:LocalValue{std::int64_t(cell.toLongLong())});
                expected.push_back(std::move(values));
            }
            QVERIFY(actual.rows==expected);
        };
        const auto sql=input+"SELECT id,LAG(v,d,fallback) OVER w,LEAD(v,d,fallback) OVER w"+window;
        const auto compiled=compile(Dialect::PostgreSQL,sql);
        QVERIFY2(compiled->ok(),compiled->diagnostics.empty()?sql.c_str():compiled->diagnostics.front().message.c_str());
        compare(interpret(compiled->model),db->execute(QString::fromStdString(sql),{},token));
        const auto boundSql=input+"SELECT id,LEAD(v,$1::integer,fallback) OVER w"+window;
        const auto bound=compile(Dialect::PostgreSQL,boundSql);
        QVERIFY(bound->ok());
        const auto prepared=prepareLocalQuery(bound->model);
        QVERIFY2(prepared.ok(),prepared.diagnostics.empty()?boundSql.c_str():prepared.diagnostics.front().message.c_str());
        for (const auto &offset:QVector<QVariant>{-1,0,QVariant{},std::numeric_limits<int>::min()}) {
            const LocalValue value=offset.isNull()?LocalValue{}:LocalValue{std::int64_t(offset.toInt())};
            compare(executeLocalQuery(*prepared.query,{value}),db->executeBound(QString::fromStdString(boundSql),{offset},{},token));
        }
    }catch(const std::exception &e){QFAIL(e.what());}}
    void dynamicFrameValueAndTileMatchPostgres(){try {
        using namespace navidog;
        using namespace navidog::sql::compiler;
        using namespace navidog::sql::optimizer;
        auto db=connectDatabase(profile_);
        const auto token=cancellation();
        const std::string input="WITH t(id,g,v,idx,n) AS (VALUES (1,1,10,1,NULL), (2,1,20,2,3), "
            "(3,1,NULL,NULL,-4), (4,1,40,2147483647,NULL), (5,1,50,1,99), "
            "(6,2,60,2,NULL), (7,2,70,1,NULL), (8,3,NULL,1,9999999)) ";
        const std::string window=" FROM t WINDOW w AS (PARTITION BY g ORDER BY id) ORDER BY id";
        const auto compare=[&](const Interpretation &local,const QueryResult &native) {
            QVERIFY2(native.error.isEmpty(),qPrintable(native.error));
            QVERIFY2(local.ok(),local.diagnostics.empty()?"no local window result":local.diagnostics.front().message.c_str());
            QCOMPARE(native.columnTypes,QStringList({"integer","integer","integer"}));
            std::vector<LocalRow> expected;
            for (const auto &row:native.rows) {
                LocalRow converted;
                for (const auto &cell:row)
                    converted.push_back(cell.isNull()?LocalValue{}:LocalValue{std::int64_t(cell.toLongLong())});
                expected.push_back(std::move(converted));
            }
            QVERIFY(local.rows==expected);
        };
        for (const auto *projection:{
             "SELECT id,NTH_VALUE(v,idx) OVER w,NTILE(n) OVER w",
             "SELECT id,NTH_VALUE(v,idx) OVER (w ROWS BETWEEN 1 PRECEDING AND 1 FOLLOWING EXCLUDE CURRENT ROW),NTILE(n) OVER w",
             "SELECT id,NTH_VALUE(v,idx) OVER (w GROUPS BETWEEN UNBOUNDED PRECEDING AND UNBOUNDED FOLLOWING EXCLUDE GROUP),NTILE(n) OVER w"}) {
            const auto sql=input+projection+window;
            const auto parsed=compile(Dialect::PostgreSQL,sql);
            QVERIFY2(parsed->ok(),parsed->diagnostics.empty()?sql.c_str():parsed->diagnostics.front().message.c_str());
            compare(interpret(parsed->model),db->execute(QString::fromStdString(sql),{},token));
        }
        const auto bindSql=input+"SELECT id,NTH_VALUE(v,$1) OVER w,NTILE($2) OVER w"+window;
        const auto parsed=compile(Dialect::PostgreSQL,bindSql);
        QVERIFY(parsed->ok());
        const auto prepared=prepareLocalQuery(parsed->model);
        QVERIFY2(prepared.ok(),prepared.diagnostics.empty()?bindSql.c_str():prepared.diagnostics.front().message.c_str());
        const auto &params=localParameters(*prepared.query);
        QCOMPARE(params.size(),std::size_t(2));
        QCOMPARE(params[0].type.bits,32);
        QCOMPARE(params[1].type.bits,32);
        for (const auto &n:QVector<QVariant>{2,QVariant{},std::numeric_limits<int>::max()}) {
            const LocalValue parameter=n.isNull()?LocalValue{}:LocalValue{std::int64_t(n.toInt())};
            compare(executeLocalQuery(*prepared.query,{parameter,parameter}),
                    db->executeBound(QString::fromStdString(bindSql),{n,n},{},token));
        }
        for (const QVector<QVariant> &args:{QVector<QVariant>{1,0},QVector<QVariant>{-1,1}}) {
            bool nativeFailed=false;
            try {
                nativeFailed=!db->executeBound(QString::fromStdString(bindSql),args,{},token).error.isEmpty();
            } catch (const std::exception &) { nativeFailed=true; }
            QVERIFY(nativeFailed);
            const auto local=executeLocalQuery(*prepared.query,
                {std::int64_t(args[0].toInt()),std::int64_t(args[1].toInt())});
            QVERIFY(!local.ok() && local.rows.empty() && local.diagnostics.front().code==DiagnosticCode::InvalidQueryShape);
        }
        for (const auto *sql:{"SELECT NTILE(2::bigint) OVER ()",
                              "SELECT NTH_VALUE(9,true) OVER ()"}) {
            const auto invalid=compile(Dialect::PostgreSQL,sql);
            QVERIFY(!invalid->ok() && invalid->diagnostics.front().code==DiagnosticCode::InvalidQueryShape);
        }
    }catch(const std::exception &e){QFAIL(e.what());}}
    void calciteUnpivotMatchesNativePostgresRows(){try {
        using namespace navidog;
        using namespace navidog::sql::compiler;
        using namespace navidog::sql::optimizer;
        auto db=connectDatabase(profile_);
        const auto token=cancellation();
        LanguageOptions options;
        options.calciteExtensions=true;
        const std::string base="WITH src(id,q1,q2) AS (VALUES (1,5,NULL::bigint), (2,NULL,8::bigint), "
            "(3,CAST(-9223372036854775808 AS bigint),9223372036854775807::bigint)) "
            "SELECT u.id,u.label,u.total FROM src UNPIVOT ";
        const auto compare=[&](const std::string &sql,const QStringList &columns,const QStringList &types) {
            const auto c=compile(Dialect::PostgreSQL,sql,options);
            QVERIFY2(c->ok(),c->diagnostics.empty()?sql.c_str():c->diagnostics.front().message.c_str());
            const auto generated=generateSql(c->model,Dialect::PostgreSQL);
            QVERIFY2(generated.ok(),generated.diagnostics.empty()?sql.c_str():generated.diagnostics.front().message.c_str());
            const auto server=db->execute(QString::fromStdString(generated.sql),{},token);
            QVERIFY2(server.error.isEmpty(),qPrintable(server.error));
            QCOMPARE(server.columns,columns);
            QCOMPARE(server.columnTypes,types);
            const auto plan=optimize(c->model);
            QVERIFY2(plan.ok(),plan.diagnostics.empty()?sql.c_str():plan.diagnostics.front().message.c_str());
            const auto planned=generatePlanSql(plan.logical,plan.heuristicRoot?plan.heuristicRoot:plan.logical.root);
            QVERIFY2(planned.ok(),planned.diagnostics.empty()?sql.c_str():planned.diagnostics.front().message.c_str());
            const auto rewritten=db->execute(QString::fromStdString(planned.sql),{},token);
            QVERIFY2(rewritten.error.isEmpty(),qPrintable(rewritten.error));
            QCOMPARE(rewritten.columns,server.columns);
            QCOMPARE(rewritten.columnTypes,server.columnTypes);
            const auto local=interpret(c->model);
            QVERIFY2(local.ok(),local.diagnostics.empty()?sql.c_str():local.diagnostics.front().message.c_str());
            const auto rows=[&](const QueryResult &result) {
                std::multiset<LocalRow> converted;
                for (const auto &row:result.rows) {
                    LocalRow values;
                    for (qsizetype i=0;i<row.size();++i) {
                        if (row[i].isNull()) values.emplace_back();
                        else if (types[i]=="text") values.emplace_back(LocalText(row[i].toString().toStdString()));
                        else values.emplace_back(std::int64_t(row[i].toLongLong()));
                    }
                    converted.insert(std::move(values));
                }
                return converted;
            };
            const auto expected=rows(server);
            QVERIFY(expected==rows(rewritten));
            QVERIFY((expected==std::multiset<LocalRow>{local.rows.begin(),local.rows.end()}));
        };
        for (const auto *clause:{
             "(total FOR label IN (q1,q2)) u",
             "INCLUDE NULLS (total FOR label IN (q1 AS '',q2 AS NULL)) u",
             "EXCLUDE NULLS (total FOR label IN (q1 AS 'same',q2 AS 'same')) u"})
            compare(base+clause,{"id","label","total"},{"integer","text","bigint"});
        const std::string tupleBase="WITH src(id,q1,v1,q2,v2) AS (VALUES (1,1,NULL::bigint,NULL::int,8::bigint), "
            "(2,NULL,NULL::bigint,NULL,NULL::bigint),(3,NULL,7::bigint,4,NULL::bigint)) "
            "SELECT u.* FROM src UNPIVOT ";
        const std::string tuple="((total,units) FOR (label,period) IN ((q1,v1),(q2,v2) AS (NULL,''))) u";
        for (const auto *mode:{"EXCLUDE NULLS ","INCLUDE NULLS "})
            compare(tupleBase+mode+tuple,{"id","label","period","total","units"},
                    {"integer","text","text","integer","bigint"});
        compare(tupleBase+tuple+" WHERE u.id>=2 AND u.total IS NOT NULL ORDER BY u.id LIMIT 1",
                {"id","label","period","total","units"},{"integer","text","text","integer","bigint"});
        QVERIFY(db->execute("CREATE TABLE unpivot_native_owned (id integer PRIMARY KEY, a smallint,b bigint)",
                            {},token).error.isEmpty());
        QVERIFY(db->execute("INSERT INTO unpivot_native_owned VALUES (1,2,4),(2,NULL,8)",
                            {},token).error.isEmpty());
        auto snapshot=collectMetadata(*db,profile_,"public",token);
        MetadataCatalog catalog(std::make_shared<const MetadataSnapshot>(snapshot));
        const auto baseTable=compile(Dialect::PostgreSQL,
            R"(SELECT u.id,u.label,u.total FROM public.unpivot_native_owned
               UNPIVOT (total FOR label IN (a AS 'one''\two',b AS NULL)) u
               WHERE u.id>=1 ORDER BY u.id,u.total)",
            options,&catalog,{profile_.database.toStdString(),"public",Dialect::PostgreSQL});
        QVERIFY2(baseTable->ok(),baseTable->diagnostics.empty()?"base UNPIVOT compile failed":
                 baseTable->diagnostics.front().message.c_str());
        const auto scan=optimize(baseTable->model);
        QVERIFY(scan.ok());
        const auto scanSql=generatePlanSql(scan.logical,scan.heuristicRoot?scan.heuristicRoot:scan.logical.root);
        QVERIFY2(scanSql.ok(),scanSql.diagnostics.empty()?"base UNPIVOT render failed":
                 scanSql.diagnostics.front().message.c_str());
        const auto nativeScan=db->execute(QString::fromStdString(scanSql.sql),{},token);
        QVERIFY2(nativeScan.error.isEmpty(),qPrintable(nativeScan.error));
        QCOMPARE(nativeScan.columnTypes,QStringList({"integer","text","bigint"}));
        QCOMPARE(nativeScan.rows.size(),3);
        QCOMPARE(nativeScan.rows[0],QVector<QVariant>({1,QStringLiteral("one'\\two"),QVariant::fromValue<qlonglong>(2)}));
        QVERIFY(nativeScan.rows[1][1].isNull());
        QVERIFY(nativeScan.rows[2][1].isNull());
        const std::vector<FederatedTableBinding> sources{{profile_.database.toStdString(),"public",
            "unpivot_native_owned",DatabaseKind::PostgreSQL,db.get(),"public","unpivot_native_owned"}};
        const auto local=executeFederatedQuery(baseTable->model,sources,token);
        QVERIFY2(local.ok(),local.diagnostics.empty()?"no local UNPIVOT":
                 local.diagnostics.front().message.c_str());
        std::vector<LocalRow> expected;
        for (const auto &row:nativeScan.rows)
            expected.push_back({std::int64_t(row[0].toLongLong()),
                row[1].isNull()?LocalValue{}:LocalValue{LocalText(row[1].toString().toStdString())},
                std::int64_t(row[2].toLongLong())});
        QVERIFY(local.rows==expected);
        GenerationOptions limited;
        limited.maxBytes=128;
        const auto refused=generatePlanSql(scan.logical,scan.logical.root,limited);
        QVERIFY(!refused.ok() && refused.sql.empty() && refused.diagnostics.front().code==DiagnosticCode::ResourceLimit);
    }catch(const std::exception &e){QFAIL(e.what());}}
    void calcitePivotEmitsNativeFilteredAggregates(){try {
        using namespace navidog;
        using namespace navidog::sql::compiler;
        using namespace navidog::sql::optimizer;
        auto db=connectDatabase(profile_);
        const auto token=cancellation();
        LanguageOptions options;
        options.calciteExtensions=true;
        const auto compare=[&](const std::string &pivot,const QString &equivalent,
                               const QStringList &types) {
            QVERIFY(!compile(Dialect::PostgreSQL,pivot)->ok());
            const auto compiled=compile(Dialect::PostgreSQL,pivot,options);
            QVERIFY2(compiled->ok(),compiled->diagnostics.empty()?pivot.c_str():
                     compiled->diagnostics.front().message.c_str());
            const auto plan=optimize(compiled->model);
            QVERIFY2(plan.ok(),plan.diagnostics.empty()?pivot.c_str():plan.diagnostics.front().message.c_str());
            QCOMPARE(std::count_if(plan.logical.nodes.begin(),plan.logical.nodes.end(),
                                   [](const auto &n){return n.kind==RelKind::Pivot;})>0,true);
            const auto root=plan.heuristicRoot?plan.heuristicRoot:plan.logical.root;
            const auto reference=db->execute(equivalent,{},token);
            QVERIFY2(reference.error.isEmpty(),qPrintable(reference.error));
            QCOMPARE(reference.columnTypes,types);
            const auto local=interpret(compiled->model);
            QVERIFY2(local.ok(),local.diagnostics.empty()?pivot.c_str():
                     local.diagnostics.front().message.c_str());
            std::vector<LocalRow> expected;
            for (const auto &row:reference.rows) {
                LocalRow values;
                for (qsizetype i=0;i<row.size();++i) {
                    if (row[i].isNull()) values.emplace_back();
                    else if (types[i]=="numeric") {
                        const auto numeric=LocalNumeric::parse(row[i].toString().toStdString());
                        QVERIFY(numeric.has_value());
                        values.push_back(*numeric);
                    } else values.emplace_back(std::int64_t(row[i].toLongLong()));
                }
                expected.push_back(std::move(values));
            }
            QVERIFY(local.rows==expected);
            for (const auto &out:{generateSql(compiled->model,Dialect::PostgreSQL),
                                  generatePlanSql(plan.logical,root)}) {
                QVERIFY2(out.ok(),out.diagnostics.empty()?pivot.c_str():out.diagnostics.front().message.c_str());
                QVERIFY(out.sql.find("FILTER (WHERE")!=std::string::npos);
                const auto generated=db->execute(QString::fromStdString(out.sql),{},token);
                QVERIFY2(generated.error.isEmpty(),qPrintable(generated.error));
                QCOMPARE(generated.columns,reference.columns);
                QCOMPARE(generated.columnTypes,reference.columnTypes);
                QCOMPARE(generated.rows,reference.rows);
            }
        };
        const std::string input="WITH src(g,k,v) AS (VALUES (1,1,5::smallint),(1,1,NULL::smallint),"
            "(1,2,7::smallint),(2,3,99::smallint),(NULL,1,2::smallint),(NULL,NULL,42::smallint)) ";
        compare(input+"SELECT p.* FROM src PIVOT (SUM(v) AS total FOR (k) IN (1 q1,2 q2,NULL empty)) p "
                      "ORDER BY g NULLS FIRST",
                QString::fromStdString(input)+"SELECT g,sum(v) FILTER (WHERE k=1) q1_total,"
                    "sum(v) FILTER (WHERE k=2) q2_total,sum(v) FILTER (WHERE k=NULL) empty_total "
                    "FROM src GROUP BY g ORDER BY g NULLS FIRST",
                {"integer","bigint","bigint","bigint"});
        compare(input+"SELECT g,q1 FROM src PIVOT (MIN(v) FOR k IN (1 q1,2 q2)) p "
                      "WHERE g IS NOT NULL ORDER BY g",
                QString::fromStdString(input)+"SELECT g,min(v) FILTER (WHERE k=1) q1 FROM src "
                    "WHERE g IS NOT NULL GROUP BY g ORDER BY g",
                {"integer","smallint"});
        const std::string empty="WITH src(k) AS (SELECT 1 WHERE FALSE) ";
        compare(empty+"SELECT p.* FROM src PIVOT (COUNT(*) AS n FOR k IN (1,-2 AS other)) p",
                QString::fromStdString(empty)+
                    "SELECT count(*) FILTER (WHERE k=1) \"1_n\", count(*) FILTER (WHERE k=-2) other_n FROM src",
                {"bigint","bigint"});
        compare("SELECT p.* FROM (SELECT 2 AS k) PIVOT (COUNT(*) FOR k IN (2 AS matched)) p",
                "SELECT count(*) AS matched FROM (SELECT 2 AS k) s WHERE k=2",
                {"bigint"});
        const std::string exact="WITH src(k,v) AS (VALUES (1,9223372036854775807::bigint),"
            "(1,9223372036854775806::bigint),(2,NULL::bigint)) ";
        compare(exact+"SELECT p.* FROM src PIVOT (SUM(v) FOR k IN (1 a,1 b,2 c)) p",
                QString::fromStdString(exact)+"SELECT sum(v) FILTER (WHERE k=1) a,"
                    "sum(v) FILTER (WHERE k=1) b,sum(v) FILTER (WHERE k=2) c FROM src",
                {"numeric","numeric","numeric"});
        compare(exact+"SELECT p.* FROM src PIVOT (AVG(v) FOR k IN (1 a,2 b)) p",
                QString::fromStdString(exact)+"SELECT avg(v) FILTER (WHERE k=1) a,"
                    "avg(v) FILTER (WHERE k=2) b FROM src",{"numeric","numeric"});
        compare(exact+"SELECT p.* FROM src PIVOT (COUNT(v) FOR k IN (1 a,2 b)) p",
                QString::fromStdString(exact)+"SELECT count(v) FILTER (WHERE k=1) a,"
                    "count(v) FILTER (WHERE k=2) b FROM src",{"bigint","bigint"});
        const std::string composite="WITH src(g,y,q,v) AS (VALUES (1,2025,1,5),(1,2025,2,7),"
            "(1,2024,1,4),(2,2026,1,22),(3,2025,NULL,17),(NULL,2025,1,23)) ";
        compare(composite+"SELECT p.* FROM src PIVOT (SUM(v) AS total FOR (y,q) IN "
                 "((2025,1) a,(2025,1) replica,(2025,2),(2024,1) previous_year,(2025,NULL) absent)) p "
                 "ORDER BY g NULLS FIRST",
                QString::fromStdString(composite)+"SELECT g,sum(v) FILTER (WHERE y=2025 AND q=1) a_total,"
                  "sum(v) FILTER (WHERE y=2025 AND q=1) replica_total,"
                  "sum(v) FILTER (WHERE y=2025 AND q=2) \"2025_2_total\","
                  "sum(v) FILTER (WHERE y=2024 AND q=1) previous_year_total,sum(v) FILTER (WHERE y=2025 AND q=NULL) absent_total "
                  "FROM src GROUP BY g ORDER BY g NULLS FIRST",
                {"integer","bigint","bigint","bigint","bigint","bigint"});
        for (const auto *bad:{"SELECT p.* FROM (SELECT 1 a,2 b) PIVOT (COUNT(*) FOR (a,b) IN (1 one)) p",
                              "SELECT p.* FROM (SELECT 1 a,2 b) PIVOT (COUNT(*) FOR (a,a) IN ((1,1) one)) p"}) {
            const auto rejected=compile(Dialect::PostgreSQL,bad,options);
            QVERIFY2(!rejected->ok(),bad);
        }
        const auto incompatible=compile(Dialect::PostgreSQL,
            "SELECT p.* FROM (SELECT 'text'::text AS k) PIVOT (COUNT(*) FOR k IN (2 AS matched)) p",options);
        QVERIFY(!incompatible->ok());
        QCOMPARE(incompatible->diagnostics.front().code,DiagnosticCode::InvalidQueryShape);
    }catch(const std::exception &e){QFAIL(e.what());}}
    void calciteUnpivotPushesOnlyRetainedPredicates(){try {
        using namespace navidog;
        using namespace navidog::sql::compiler;
        using namespace navidog::sql::optimizer;
        auto db=connectDatabase(profile_);
        const auto token=cancellation();
        LanguageOptions options;
        options.calciteExtensions=true;
        const std::string input="SELECT u.id,u.label,u.amount FROM "
            "(VALUES (1,5,8),(2,NULL,10),(3,11,12),(NULL,15,17),(4,7,NULL)) src(id,a,b) "
            "UNPIVOT (amount FOR label IN (a,b)) u ";
        const auto test=[&](std::string predicate,bool changes,bool residual) {
            const auto sql=input+predicate+" ORDER BY u.id NULLS FIRST,u.amount";
            const auto compiled=compile(Dialect::PostgreSQL,sql,options);
            QVERIFY2(compiled->ok(),compiled->diagnostics.empty()?sql.c_str():
                     compiled->diagnostics.front().message.c_str());
            const auto plan=optimize(compiled->model);
            QVERIFY2(plan.ok(),plan.diagnostics.empty()?sql.c_str():plan.diagnostics.front().message.c_str());
            const auto rewrite=std::find_if(plan.rules.begin(),plan.rules.end(),[](const auto &event) {
                return event.rule=="HepFilterUnpivotTranspose";
            });
            QCOMPARE(rewrite!=plan.rules.end(),changes);
            if (changes) {
                const auto &node=plan.logical.nodes.at(rewrite->after);
                QCOMPARE(node.kind,residual?RelKind::Filter:RelKind::Unpivot);
                if (residual) QCOMPARE(node.predicates.size(),std::size_t(1));
                const auto &unpivot=residual?plan.logical.nodes.at(node.inputs.front()):node;
                const auto &before=plan.logical.nodes.at(unpivot.inputs.front());
                QCOMPARE(before.kind,RelKind::Filter);
                QCOMPARE(before.predicates.size(),std::size_t(1));
            }
            const auto original=generatePlanSql(plan.logical,plan.logical.root);
            const auto updated=generatePlanSql(plan.logical,plan.heuristicRoot);
            QVERIFY2(original.ok(),original.diagnostics.empty()?"missing base SQL":original.diagnostics.front().message.c_str());
            QVERIFY2(updated.ok(),updated.diagnostics.empty()?"missing plan SQL":updated.diagnostics.front().message.c_str());
            const auto expected=db->execute(QString::fromStdString(original.sql),{},token);
            const auto actual=db->execute(QString::fromStdString(updated.sql),{},token);
            QVERIFY2(expected.error.isEmpty(),qPrintable(expected.error));
            QVERIFY2(actual.error.isEmpty(),qPrintable(actual.error));
            QCOMPARE(actual.columns,expected.columns);
            QCOMPARE(actual.columnTypes,expected.columnTypes);
            QCOMPARE(actual.rows,expected.rows);
        };
        test("WHERE u.id>=2",true,false);
        test("WHERE u.id IS NULL AND u.amount>15",true,true);
        test("WHERE u.amount>9",false,false);
        test("WHERE u.label='b'",false,false);
    }catch(const std::exception &e){QFAIL(e.what());}}
    void federatedCalcitePivotRunsWithRegisteredNativeTable(){try {
        using namespace navidog;
        using namespace navidog::sql::compiler;
        using namespace navidog::sql::optimizer;
        auto db=connectDatabase(profile_);
        const auto token=cancellation();
        QVERIFY(db->execute("CREATE TABLE pivot_federated_owned(g integer,y integer,q integer,v integer)",
                            {},token).error.isEmpty());
        QVERIFY(db->execute("INSERT INTO pivot_federated_owned VALUES "
                            "(1,2025,1,5),(1,2025,1,NULL),(1,2025,2,9),(2,2024,1,7),(3,2025,NULL,13)",
                            {},token).error.isEmpty());
        auto snapshot=collectMetadata(*db,profile_,"public",token);
        MetadataCatalog catalog(std::make_shared<const MetadataSnapshot>(snapshot));
        LanguageOptions options;
        options.calciteExtensions=true;
        const std::string shape=" PIVOT (COUNT(v) FOR (y,q) IN ((2025,1) a,(2025,2) b)) p WHERE g>=1 ORDER BY g";
        const auto compiled=compile(Dialect::PostgreSQL,
            "SELECT p.* FROM public.pivot_federated_owned"+shape,options,&catalog,
            {profile_.database.toStdString(),"public",Dialect::PostgreSQL});
        QVERIFY2(compiled->ok(),compiled->diagnostics.empty()?"no Calcite PIVOT":
                 compiled->diagnostics.front().message.c_str());
        QVERIFY(!interpret(compiled->model).ok());
        const std::vector<FederatedTableBinding> sources{{profile_.database.toStdString(),"public",
            "pivot_federated_owned",DatabaseKind::PostgreSQL,db.get(),"public","pivot_federated_owned"}};
        const auto local=executeFederatedQuery(compiled->model,sources,token);
        QVERIFY2(local.ok(),local.diagnostics.empty()?"no local PIVOT":local.diagnostics.front().message.c_str());
        const auto reference=db->execute("SELECT g,count(v) FILTER (WHERE y=2025 AND q=1) a,"
            "count(v) FILTER (WHERE y=2025 AND q=2) b FROM pivot_federated_owned WHERE g>=1 GROUP BY g ORDER BY g",
            {},token);
        QVERIFY2(reference.error.isEmpty(),qPrintable(reference.error));
        std::vector<LocalRow> expected;
        for (const auto &row:reference.rows)
            expected.push_back({std::int64_t(row[0].toLongLong()),std::int64_t(row[1].toLongLong()),
                                std::int64_t(row[2].toLongLong())});
        QVERIFY(local.rows==expected);

        QUrl endpoint("postgresql:///"+profile_.database);
        QUrlQuery endpointParameters;
        endpointParameters.addQueryItem("host",profile_.host);
        endpointParameters.addQueryItem("port",QString::number(profile_.port));
        endpointParameters.addQueryItem("user",profile_.user);
        endpoint.setQuery(endpointParameters);
        QString command=QCoreApplication::applicationDirPath()+"/navidog-federate";
#ifdef Q_OS_WIN
        command+=".exe";
#endif
        QProcess cli;
        cli.start(command,{"--postgres","sample="+endpoint.toString(QUrl::FullyEncoded),"--table",
            "sample.public.pivot_federated_owned","--calcite-extensions","-"});
        QVERIFY(cli.waitForStarted(3000));
        cli.write(QByteArray::fromStdString("SELECT p.* FROM sample.public.pivot_federated_owned"+shape));
        cli.closeWriteChannel();
        QVERIFY(cli.waitForFinished(8000));
        QVERIFY2(cli.exitCode()==0,cli.readAllStandardError().constData());
        const auto result=QJsonDocument::fromJson(cli.readAllStandardOutput()).object();
        QVERIFY(result["ok"].toBool());
        QCOMPARE(result["columns"].toArray(),QJsonArray({"g","a","b"}));
        QCOMPARE(result["rows"].toArray().size(),qsizetype(3));
        QCOMPARE(result["rows"].toArray().at(0).toArray(),QJsonArray({1,1,1}));
    }catch(const std::exception &e){QFAIL(e.what());}}
    void aggregateDomainsMatchServer() {try {
      using namespace navidog;using namespace navidog::sql::compiler;
      auto db=connectDatabase(profile_);auto token=cancellation();
      auto run=[&](const QString &sql){auto r=db->execute(sql,{},token);if(!r.error.isEmpty())throw std::runtime_error(r.error.toStdString());return r;};
      const auto actual=run("SELECT pg_typeof(SUM(CAST(1 AS SMALLINT)))::text,pg_typeof(SUM(CAST(1 AS BIGINT)))::text,pg_typeof(AVG(CAST(1 AS INTEGER)))::text,pg_typeof(AVG(CAST(1 AS REAL)))::text,pg_typeof(SUM(CAST(1 AS REAL)))::text,pg_typeof(SUM(INTERVAL '1' DAY))::text");
      QCOMPARE(actual.rows[0],QVector<QVariant>({"bigint","numeric","numeric","double precision","real","interval"}));
      const auto sql="SELECT SUM(CAST(1 AS SMALLINT)),SUM(CAST(1 AS BIGINT)),AVG(CAST(1 AS INTEGER)),AVG(CAST(1 AS REAL)),SUM(CAST(1 AS REAL)),SUM(INTERVAL '1' DAY)";
      auto c=compile(Dialect::PostgreSQL,sql);QVERIFY(c->ok());auto generated=generateSql(c->model,Dialect::PostgreSQL);QVERIFY(generated.ok());QCOMPARE(run(QString::fromStdString(generated.sql)).rows,run(sql).rows);
      for(const auto &bad:{"SELECT SUM(CAST('x' AS TEXT))","SELECT AVG(DATE '2024-01-01')","SELECT SUM(TRUE)","SELECT SUM(CAST(NULL AS INTEGER[]))"}) {
        bool rejected=false;try{rejected=!db->execute(bad,{},token).error.isEmpty();}catch(const std::exception &){rejected=true;}
        QVERIFY2(rejected,bad);QVERIFY(!compile(Dialect::PostgreSQL,bad)->ok());
      }
    }catch(const std::exception &e){QFAIL(e.what());}}
    void exactCastConversionMatchesServer() {try {
      using namespace navidog;using namespace navidog::sql::compiler;
      auto db=connectDatabase(profile_);auto token=cancellation();
      auto run=[&](const QString &sql){auto r=db->execute(sql,{},token);if(!r.error.isEmpty())throw std::runtime_error(r.error.toStdString());return r;};
      auto converted=convertSql(Dialect::MySQL,Dialect::PostgreSQL,"SELECT CAST(42 AS SIGNED),CAST(-9223372036854775808 AS SIGNED)");QVERIFY2(converted.ok(),converted.diagnostics.empty()?"no SQL":converted.diagnostics.front().message.c_str());
      QCOMPARE(run(QString::fromStdString(converted.sql)).rows,run("SELECT CAST(42 AS BIGINT),CAST(-9223372036854775808 AS BIGINT)").rows);
      const auto rounding=run("SELECT CAST(1.5 AS INTEGER),CAST(-1.5 AS INTEGER)");QCOMPARE(rounding.rows[0][0].toInt(),2);QCOMPARE(rounding.rows[0][1].toInt(),-2);
      QVERIFY(!convertSql(Dialect::PostgreSQL,Dialect::SQLServer,"SELECT CAST(1.5 AS INTEGER)").ok());
    }catch(const std::exception &e){QFAIL(e.what());}}
    void nativeNonNullMetadataDrivesProvedFilters() {try {
      using namespace navidog;using namespace navidog::sql::compiler;namespace opt=navidog::sql::optimizer;
      auto db=connectDatabase(profile_);auto token=cancellation();
      auto run=[&](const QString &sql){auto r=db->execute(sql,{},token);if(!r.error.isEmpty())throw std::runtime_error(r.error.toStdString());return r;};
      run("CREATE TABLE proof_native_owned(id INTEGER NOT NULL,n INTEGER)");run("INSERT INTO proof_native_owned VALUES(1,NULL),(2,3)");
      auto snapshot=collectMetadata(*db,profile_,"public",token);MetadataCatalog catalog(std::make_shared<const MetadataSnapshot>(snapshot));
      auto fired=[](const opt::OptimizationResult &r){return std::any_of(r.rules.begin(),r.rules.end(),[](const auto &rule){return rule.rule=="HepFilterNullability";});};
      for(const auto &sql:{"SELECT id FROM proof_native_owned WHERE id IS NULL", "SELECT id FROM proof_native_owned WHERE id IS NOT NULL"}) {
        auto c=compile(Dialect::PostgreSQL,sql,{},&catalog,{profile_.database.toStdString(),"public",Dialect::PostgreSQL});QVERIFY(c->ok());
        auto planned=opt::optimize(c->model);QVERIFY(planned.ok());QVERIFY(fired(planned));
      }
      QCOMPARE(run("SELECT id FROM proof_native_owned WHERE id IS NULL").rows.size(),0);QCOMPARE(run("SELECT id FROM proof_native_owned WHERE id IS NOT NULL").rows.size(),2);
      auto nullable=compile(Dialect::PostgreSQL,"SELECT id FROM proof_native_owned WHERE n IS NULL",{},&catalog,{profile_.database.toStdString(),"public",Dialect::PostgreSQL});QVERIFY(nullable->ok());auto kept=opt::optimize(nullable->model);QVERIFY(kept.ok());QVERIFY(!fired(kept));QCOMPARE(run("SELECT id FROM proof_native_owned WHERE n IS NULL").rows.size(),1);
      run("DROP TABLE proof_native_owned");
    }catch(const std::exception &e){QFAIL(e.what());}}
    void convertedSqlServerConstructorPreservesIntegerRows() {try {
        using namespace navidog;
        using namespace navidog::sql::compiler;
        auto db = connectDatabase(profile_);
        auto token = cancellation();
        const auto translated = convertSql(Dialect::SQLServer, Dialect::PostgreSQL,
            "SELECT d.id,d.n FROM (VALUES (1,NULL),(2,-3)) AS d(id,n) ORDER BY d.id;");
        QVERIFY2(translated.ok(),translated.diagnostics.empty()?"no translation":translated.diagnostics.front().message.c_str());
        const auto result = db->execute(QString::fromStdString(translated.sql), {}, token);
        QVERIFY2(result.error.isEmpty(),qPrintable(result.error));
        QCOMPARE(result.rows.size(),2);
        QCOMPARE(result.rows.at(0).at(0).toInt(),1);
        QVERIFY(result.rows.at(0).at(1).isNull());
        QCOMPARE(result.rows.at(1).at(0).toInt(),2);
        QCOMPARE(result.rows.at(1).at(1).toInt(),-3);
    }catch(const std::exception &e){QFAIL(e.what());}}
    void localInterpreterMatchesServerRows() {try {
        using namespace navidog;
        using namespace navidog::sql::compiler;
        using namespace navidog::sql::optimizer;
        auto db = connectDatabase(profile_);
        auto token = cancellation();
        for (auto source : {
                "SELECT 1+2 AS n, TRUE AND NULL AS u, FALSE AND NULL AS f, TRUE OR NULL AS t;",
                "WITH t(x) AS (VALUES (3),(1),(1),(NULL),(2)) SELECT DISTINCT x,x+1 AS successor FROM t "
                "WHERE x>1 OR x IS NULL ORDER BY successor NULLS LAST LIMIT 3 OFFSET 1;",
                "WITH l(x) AS (VALUES (1),(1),(1),(2),(NULL),(NULL)), r(x) AS (VALUES (1),(2),(NULL)) "
                "SELECT x FROM l EXCEPT ALL SELECT x FROM r ORDER BY 1 NULLS FIRST;",
                "WITH l(x) AS (VALUES (1),(1),(1),(2),(NULL),(NULL)), r(x) AS (VALUES (1),(2),(NULL)) "
                "SELECT x FROM l INTERSECT ALL SELECT x FROM r ORDER BY 1 DESC NULLS LAST;",
                "SELECT d.x,d.keep FROM (SELECT 1 AS previous,2 AS keep) AS d(x);",
                "SELECT d.x FROM (VALUES (2),(1),(NULL)) AS d(x) ORDER BY d.x;",
                "WITH s(a) AS (SELECT 1 AS old,2 AS retained) SELECT s.a,s.retained FROM s;",
                "WITH RECURSIVE seq(k) AS (SELECT 1 AS seed_value,9 AS retained UNION ALL "
                "SELECT k+1,retained FROM seq WHERE k<3) SELECT k,retained FROM seq ORDER BY k;",
                "WITH RECURSIVE a(v) AS (SELECT (SELECT k FROM b) AS v), b(k) AS (SELECT 7) SELECT v FROM a;",
                "WITH RECURSIVE a(v) AS (WITH b(v) AS (SELECT 4) SELECT v FROM b), "
                "b(v) AS (SELECT v FROM a) SELECT v FROM b;",
                "WITH RECURSIVE a(v) AS (WITH b(v) AS (SELECT v FROM b) SELECT v FROM b), "
                "b(v) AS (SELECT 7) SELECT v FROM a;",
                "WITH RECURSIVE seq(k) AS (SELECT k FROM b UNION ALL SELECT k+1 FROM seq WHERE k<3), "
                "b(k) AS (VALUES (1)) SELECT k FROM seq ORDER BY k;",
                "SELECT sub.select FROM (SELECT 1 AS select) sub;",
                "SELECT (1=1) IS NOT FALSE, TRUE IS FALSE, NULL IS UNKNOWN, NULL IS NOT TRUE, "
                "2 BETWEEN SYMMETRIC 3 AND 1, 0 NOT BETWEEN SYMMETRIC 3 AND 1, "
                "-1 BETWEEN 1 AND NULL, 2 BETWEEN 1 AND NULL, "
                "3 IN (1,NULL), 1 IN (NULL,1), 3 NOT IN (1,NULL);",
                "WITH l(k) AS (VALUES (1),(2),(3),(NULL)), r(k) AS (VALUES (2),(NULL),(2)) "
                "SELECT k,k IN(SELECT k FROM r),k NOT IN(SELECT k FROM r),"
                "NULL NOT IN(SELECT k FROM r WHERE FALSE) FROM l "
                "WHERE EXISTS(SELECT 1 FROM r WHERE k=2) ORDER BY k NULLS LAST;",
                "SELECT (SELECT (SELECT 4)+1), (SELECT 1 WHERE FALSE);",
                "WITH t(n) AS (VALUES (2),(1)) SELECT CASE WHEN FALSE THEN (SELECT n FROM t) ELSE 8 END;",
                "WITH t(x) AS (VALUES (1),(1),(2),(2),(3)) "
                "SELECT x FROM t ORDER BY x OFFSET 1 ROW FETCH NEXT 2 ROWS WITH TIES;",
                "WITH t(x) AS (VALUES (2),(NULL),(NULL),(1)) "
                "SELECT x FROM t ORDER BY x NULLS FIRST FETCH FIRST ROW WITH TIES OFFSET 0;",
                "WITH l(id) AS (VALUES (1),(2),(NULL)), r(id) AS (VALUES (2),(3),(NULL)) "
                "SELECT l.id,r.id FROM l FULL JOIN r ON l.id=r.id ORDER BY 1,2;",
                "WITH l(id,n) AS (VALUES (1,0),(2,8),(3,8),(4,NULL)), "
                "r(id,n) AS (VALUES (1,9),(2,9),(2,10),(3,NULL),(4,9)) "
                "SELECT l.id,r.n FROM l LEFT JOIN r ON l.id=r.id AND l.n>7 AND r.n>7 ORDER BY 1,2;",
                "WITH RECURSIVE seq(n) AS (SELECT 1 UNION ALL SELECT n+1 FROM seq WHERE n<4) "
                "SELECT n FROM seq ORDER BY n DESC;",
                "WITH RECURSIVE seq(n,k) AS (VALUES (1,7),(1,NULL),(1,NULL) UNION SELECT 3-n,k FROM seq) "
                "SELECT n,k FROM seq ORDER BY n,k NULLS LAST;",
                "SELECT CASE WHEN FALSE THEN 2147483647+1 WHEN NULL THEN 9 ELSE 7 END, "
                "CASE NULL WHEN NULL THEN 1 ELSE 2 END, CASE 2 WHEN 1 THEN 9 END;",
                "WITH t(k,n) AS (VALUES (1,10),(1,10),(1,20),(1,NULL),(2,NULL),(NULL,99)) "
                "SELECT k,COUNT(*),COUNT(n),COUNT(DISTINCT n),SUM(n) FILTER (WHERE n>10),MIN(n),MAX(n) "
                "FROM t GROUP BY k HAVING COUNT(*)>=1 ORDER BY 1;",
                "WITH t(a,b,n) AS (VALUES (1,10,5),(1,10,7),(1,NULL,3),(2,20,2)) "
                "SELECT a,b,GROUPING(a,b) AS mask,COUNT(*),SUM(n) FROM t "
                "GROUP BY GROUPING SETS((a,b),(a),(),()) ORDER BY mask,a,b NULLS LAST;",
                "WITH t(k) AS (SELECT 1 WHERE FALSE) SELECT k,GROUPING(k),COUNT(*) "
                "FROM t GROUP BY GROUPING SETS((k),(),()) ORDER BY 1;",
                "WITH t(k) AS (VALUES (1),(1),(2)) SELECT k,GROUPING(k) AS g,COUNT(*),"
                "RANK() OVER(ORDER BY COUNT(*) DESC) AS place FROM t GROUP BY ROLLUP(k) "
                "HAVING GROUPING(k)>0 OR COUNT(*)>1 ORDER BY place;",
                "WITH t(k) AS (SELECT 1 WHERE FALSE) SELECT COUNT(*),SUM(k),MIN(k),MAX(k) FROM t;",
                "WITH t(team,id,n) AS (VALUES (1,11,5),(1,10,5),(1,12,2),(2,20,NULL),(2,21,7),(NULL,99,9)) "
                "SELECT team,id,ROW_NUMBER() OVER(PARTITION BY team ORDER BY n DESC NULLS LAST,id) AS pos, "
                "RANK() OVER w AS sparse,DENSE_RANK() OVER w AS dense FROM t "
                "WINDOW w AS(PARTITION BY team ORDER BY n DESC NULLS LAST) ORDER BY team NULLS LAST,pos;",
                "WITH d(k,n) AS (VALUES (1,3),(1,4),(2,7),(3,2),(4,NULL)) "
                "SELECT k,SUM(n) AS total,RANK() OVER(ORDER BY SUM(n) DESC NULLS LAST) AS place "
                "FROM d GROUP BY k HAVING COUNT(*)>0 ORDER BY place,k;",
                "WITH t(i,k,n) AS (VALUES (1,1,5),(2,1,NULL),(3,2,4),(4,3,6),(5,3,2)) "
                "SELECT i,SUM(n) OVER (ORDER BY k),COUNT(*) OVER (ORDER BY i ROWS BETWEEN 1 PRECEDING AND 1 FOLLOWING), "
                "COUNT(n) FILTER (WHERE n>4) OVER (ORDER BY k GROUPS BETWEEN 1 PRECEDING AND CURRENT ROW), "
                "SUM(n) OVER (ORDER BY i ROWS BETWEEN 1 FOLLOWING AND 2 FOLLOWING),COUNT(n) OVER() FROM t ORDER BY i;",
                "WITH t(i,k,n) AS (VALUES (1,1,5),(2,1,7),(3,2,4),(4,NULL,NULL)) SELECT i,"
                "COUNT(*) OVER (ORDER BY k NULLS LAST ROWS BETWEEN UNBOUNDED PRECEDING AND UNBOUNDED FOLLOWING EXCLUDE CURRENT ROW),"
                "SUM(n) OVER (ORDER BY k NULLS LAST ROWS BETWEEN UNBOUNDED PRECEDING AND UNBOUNDED FOLLOWING EXCLUDE GROUP),"
                "SUM(n) OVER (ORDER BY k NULLS LAST ROWS BETWEEN UNBOUNDED PRECEDING AND UNBOUNDED FOLLOWING EXCLUDE TIES),"
                "SUM(n) OVER (ORDER BY k NULLS LAST RANGE UNBOUNDED PRECEDING EXCLUDE GROUP) FROM t ORDER BY i;",
                "WITH t(i,k,n) AS (VALUES (1,NULL,9),(2,-3,2),(3,-2,3),(4,-2,5),(5,0,7),(6,2,11),(7,NULL,13)) "
                "SELECT i,SUM(n) OVER (ORDER BY k NULLS LAST RANGE BETWEEN 1 PRECEDING AND 1 FOLLOWING),"
                "SUM(n) OVER (ORDER BY k DESC NULLS FIRST RANGE BETWEEN 2 PRECEDING AND 0 FOLLOWING EXCLUDE TIES),"
                "SUM(n) OVER (ORDER BY k NULLS FIRST RANGE BETWEEN UNBOUNDED PRECEDING AND 1 PRECEDING) "
                "FROM t ORDER BY i;",
                "WITH t(k) AS (VALUES (-2147483647-1),(-1),(0),(2147483647)) SELECT k,"
                "COUNT(*) OVER(ORDER BY k DESC RANGE BETWEEN 2147483647 PRECEDING AND 2147483647 FOLLOWING) "
                "FROM t ORDER BY k;",
                "WITH t(i,k,n) AS (VALUES (1,1,NULL),(2,1,7),(3,2,4),(4,3,6),(5,3,2)) "
                "SELECT i,NTILE(3) OVER(ORDER BY i),NTILE(9) OVER(PARTITION BY k ORDER BY i),"
                "FIRST_VALUE(n) OVER(ORDER BY i ROWS BETWEEN 1 PRECEDING AND CURRENT ROW EXCLUDE CURRENT ROW),"
                "LAST_VALUE(n) OVER(ORDER BY i ROWS BETWEEN 1 PRECEDING AND 1 FOLLOWING EXCLUDE CURRENT ROW),"
                "NTH_VALUE(n,2) OVER(ORDER BY i RANGE BETWEEN 1 PRECEDING AND 1 FOLLOWING EXCLUDE CURRENT ROW) "
                "FROM t ORDER BY i;",
                "WITH t(i,g,n,fallback) AS (VALUES (1,1,2,101),(2,1,NULL,102),(3,1,7,103),(4,2,9,104)) "
                "SELECT i,LAG(n,1,fallback) OVER w,LEAD(n,2,fallback) OVER w,LAG(n,0) OVER w,LEAD(n) OVER w "
                "FROM t WINDOW w AS (PARTITION BY g ORDER BY i) ORDER BY i;",
                "SELECT -9223372036854775808,-4294967296*2147483648,2147483648+1,"
                "-7/3,-7%3,-9223372036854775808 % -1,-(-2147483648),-(-(-9223372036854775808));",
                "WITH t(k,n) AS (VALUES (1,5),(1,6),(2,7),(3,NULL)) "
                "SELECT k,SUM(n),SUM(SUM(n)) OVER(ORDER BY k),COUNT(*) FILTER (WHERE k<2) OVER() "
                "FROM t GROUP BY k ORDER BY k;"}) {
            const auto compiled = compile(Dialect::PostgreSQL, source);
            QVERIFY2(compiled->ok(),compiled->diagnostics.empty()?"invalid fixture":compiled->diagnostics.front().message.c_str());
            const auto local = interpret(compiled->model);
            QVERIFY2(local.ok(),local.diagnostics.empty()?"local result unavailable":local.diagnostics.front().message.c_str());
            const auto native = db->execute(source, {}, token);
            QVERIFY2(native.error.isEmpty(),qPrintable(native.error));
            QCOMPARE(local.rows.size(),std::size_t(native.rows.size()));
            QCOMPARE(local.columns.size(),std::size_t(native.columns.size()));
            for (std::size_t i = 0; i < local.rows.size(); ++i) {
                for (std::size_t j = 0; j < local.columns.size(); ++j) {
                    const auto &actual = native.rows.at(i).at(j);
                    const auto &expected = local.rows.at(i).at(j);
                    if (const auto integer = std::get_if<std::int64_t>(&expected))
                        QCOMPARE(actual.toLongLong(),qint64(*integer));
                    else if (const auto boolean = std::get_if<bool>(&expected))
                        QCOMPARE(actual.toString(),QString(*boolean?"t":"f"));
                    else if (const auto numeric=std::get_if<navidog::sql::optimizer::LocalNumeric>(&expected))
                        QCOMPARE(actual.toString(),QString::fromStdString(numeric->decimal()));
                    else QVERIFY(actual.isNull());
                }
            }
        }
    }catch(const std::exception &e){QFAIL(e.what());}}
    void outerJoinConditionPushdownPreservesNullExtension() {try {
        using namespace navidog;
        using namespace navidog::sql::compiler;
        namespace opt=navidog::sql::optimizer;
        auto db=connectDatabase(profile_);
        auto token=cancellation();
        const auto run=[&](const QString &sql) {
            const auto result=db->execute(sql,{},token);
            if (!result.error.isEmpty()) throw std::runtime_error(result.error.toStdString());
            return result;
        };
        run("CREATE TABLE public.on_left_owned(id INT, n INT)");
        run("CREATE TABLE public.on_right_owned(id INT, n INT)");
        run("INSERT INTO public.on_left_owned VALUES(1,0),(2,8),(3,8),(4,NULL)");
        run("INSERT INTO public.on_right_owned VALUES(1,9),(2,9),(2,10),(3,NULL),(4,9)");
        const auto snapshot=collectMetadata(*db,profile_,"public",token);
        const MetadataCatalog catalog(std::make_shared<const MetadataSnapshot>(snapshot));
        const NameResolutionContext context{profile_.database.toStdString(),"public",Dialect::PostgreSQL};
        const auto source="SELECT a.id,b.n FROM public.on_left_owned a LEFT JOIN public.on_right_owned b "
                          "ON a.id=b.id AND a.n>7 AND b.n>7 ORDER BY a.id,b.n;";
        const auto compiled=compile(Dialect::PostgreSQL,source,{},&catalog,context);
        QVERIFY2(compiled->ok(),compiled->diagnostics.empty()?"invalid SQL":compiled->diagnostics.front().message.c_str());
        const auto planned=opt::optimize(compiled->model);
        QVERIFY(planned.ok());
        QVERIFY(std::any_of(planned.rules.begin(),planned.rules.end(),
            [](const auto &rule) { return rule.rule=="JoinConditionPushDown"; }));
        const auto original=run(source);
        const auto pushed=run("SELECT a.id,b.n FROM public.on_left_owned a LEFT JOIN "
                             "(SELECT * FROM public.on_right_owned WHERE n>7) b ON a.id=b.id AND a.n>7 "
                             "ORDER BY a.id,b.n;");
        QCOMPARE(original.rows,pushed.rows);
        QCOMPARE(pushed.rows.size(),5);
        QVERIFY(pushed.rows.at(0).at(1).isNull());
        QCOMPARE(pushed.rows.at(1).at(1).toInt(),9);
        QCOMPARE(pushed.rows.at(2).at(1).toInt(),10);
        QVERIFY(pushed.rows.at(4).at(1).isNull());
        run("DROP TABLE public.on_left_owned, public.on_right_owned");
    }catch(const std::exception &e){QFAIL(e.what());}}
    void aggregateFilterPushdownPreservesGroupedRows() {try {
        using namespace navidog;
        using namespace navidog::sql::compiler;
        namespace opt=navidog::sql::optimizer;
        auto db=connectDatabase(profile_);
        auto token=cancellation();
        const auto run=[&](const QString &sql) {
            const auto result=db->execute(sql,{},token);
            if (!result.error.isEmpty()) throw std::runtime_error(result.error.toStdString());
            return result;
        };
        run("CREATE TABLE public.agg_filter_owned(n INT)");
        run("INSERT INTO public.agg_filter_owned VALUES(NULL),(NULL),(1),(1),(2),(3),(3)");
        const auto snapshot=collectMetadata(*db,profile_,"public",token);
        const MetadataCatalog catalog(std::make_shared<const MetadataSnapshot>(snapshot));
        const NameResolutionContext context{profile_.database.toStdString(),"public",Dialect::PostgreSQL};
        const auto source="SELECT n,count(*) FROM public.agg_filter_owned GROUP BY n HAVING n>1 ORDER BY 1;";
        const auto compiled=compile(Dialect::PostgreSQL,source,{},&catalog,context);
        QVERIFY2(compiled->ok(),compiled->diagnostics.empty()?"invalid SQL":compiled->diagnostics.front().message.c_str());
        const auto planned=opt::optimize(compiled->model);
        QVERIFY(planned.ok());
        opt::RelMetadataQuery metadata(planned.logical);
        const auto *groupColumn=metadata.columnOrigins(planned.logical.root,0);
        const auto *countColumn=metadata.columnOrigins(planned.logical.root,1);
        QVERIFY(groupColumn && groupColumn->complete && groupColumn->origins.size()==1 &&
                groupColumn->origins.front().schema=="public" && groupColumn->origins.front().table=="agg_filter_owned" &&
                groupColumn->origins.front().column=="n" && !groupColumn->origins.front().derived);
        QVERIFY(countColumn && countColumn->complete && countColumn->origins.empty());
        QVERIFY(std::any_of(planned.rules.begin(),planned.rules.end(),
            [](const auto &rule) { return rule.rule=="HepFilterAggregateTranspose"; }));
        const auto original=run(source);
        const auto pushed=run("SELECT n,count(*) FROM public.agg_filter_owned WHERE n>1 GROUP BY n ORDER BY 1;");
        QCOMPARE(original.rows,pushed.rows);
        QCOMPARE(pushed.rows.size(),2);
        QCOMPARE(pushed.rows.at(0).at(1).toInt(),1);
        QCOMPARE(pushed.rows.at(1).at(1).toInt(),2);
        run("DROP TABLE public.agg_filter_owned");
    }catch(const std::exception &e){QFAIL(e.what());}}
    void setFilterPushdownPreservesMultisetRows() {try {
        using namespace navidog;
        using namespace navidog::sql::compiler;
        namespace opt=navidog::sql::optimizer;
        auto db=connectDatabase(profile_);
        auto token=cancellation();
        const auto run=[&](const QString &sql) {
            const auto result=db->execute(sql,{},token);
            if (!result.error.isEmpty()) throw std::runtime_error(result.error.toStdString());
            return result;
        };
        run("CREATE TABLE public.set_left_owned(n INT)");
        run("CREATE TABLE public.set_right_owned(n INT)");
        run("INSERT INTO public.set_left_owned VALUES(NULL),(NULL),(1),(1),(2),(3),(3)");
        run("INSERT INTO public.set_right_owned VALUES(NULL),(1),(2),(2),(4)");
        const auto snapshot=collectMetadata(*db,profile_,"public",token);
        const MetadataCatalog catalog(std::make_shared<const MetadataSnapshot>(snapshot));
        const NameResolutionContext context{profile_.database.toStdString(),"public",Dialect::PostgreSQL};
        for (const auto &[operation, rows] :
             std::vector<std::pair<std::string,int>>{{"UNION ALL",6},{"INTERSECT ALL",1},{"EXCEPT ALL",2}}) {
            const auto source="SELECT s.k FROM (SELECT n AS left_name FROM public.set_left_owned " +
                operation + " SELECT n AS right_name FROM public.set_right_owned) s(k) WHERE s.k>1 ORDER BY 1;";
            const auto compiled=compile(Dialect::PostgreSQL,source,{},&catalog,context);
            QVERIFY2(compiled->ok(),compiled->diagnostics.empty()?"invalid SQL":compiled->diagnostics.front().message.c_str());
            const auto planned=opt::optimize(compiled->model);
            QVERIFY(planned.ok());
            QVERIFY(std::any_of(planned.rules.begin(),planned.rules.end(),
                [](const auto &rule) { return rule.rule=="HepFilterSetOpTranspose"; }));
            const auto rewritten="SELECT n FROM public.set_left_owned WHERE n>1 " + operation +
                " SELECT n FROM public.set_right_owned WHERE n>1 ORDER BY 1;";
            const auto original=run(QString::fromStdString(source));
            const auto pushed=run(QString::fromStdString(rewritten));
            QCOMPARE(original.rows,pushed.rows);
            QCOMPARE(pushed.rows.size(),rows);
        }
        opt::MemoryStatistics stats;
        opt::TableStatistics leftStats;
        leftStats.database=profile_.database.toStdString();
        leftStats.schema="public";
        leftStats.name="set_left_owned";
        leftStats.rows=100000;
        auto rightStats=leftStats;
        rightStats.name="set_right_owned";
        rightStats.rows=100;
        stats.tables={leftStats,rightStats};
        opt::PlannerOptions options;
        options.weights.memory=.1;
        const std::string intersection="SELECT n AS kept FROM public.set_left_owned "
            "INTERSECT ALL SELECT n FROM public.set_right_owned ORDER BY 1;";
        const auto query=compile(Dialect::PostgreSQL,intersection,{},&catalog,context);
        QVERIFY(query->ok());
        const auto costed=opt::optimize(query->model,&stats,options);
        QVERIFY(costed.ok());
        QVERIFY(std::any_of(costed.rules.begin(),costed.rules.end(),[](const auto &rule) {
            return rule.rule=="IntersectReorder";
        }));
        const auto expected=run(QString::fromStdString(intersection));
        const auto swapped=run("SELECT n AS kept FROM public.set_right_owned "
            "INTERSECT ALL SELECT n FROM public.set_left_owned ORDER BY 1;");
        QCOMPARE(swapped.rows,expected.rows);
        QCOMPARE(swapped.columns,expected.columns);
        QCOMPARE(swapped.columnTypes,expected.columnTypes);

        const std::string aliasFilter="SELECT s.k FROM (SELECT n AS base_name FROM public.set_left_owned) "
            "s(k) WHERE s.k>1 AND s.k IS NOT NULL ORDER BY s.k;";
        const auto declared=compile(Dialect::PostgreSQL,aliasFilter,{},&catalog,context);
        QVERIFY(declared->ok());
        const auto projected=opt::optimize(declared->model);
        QVERIFY(projected.ok());
        QVERIFY(std::any_of(projected.rules.begin(),projected.rules.end(),[](const auto &rule) {
            return rule.rule=="HepFilterProjectTranspose";
        }));
        const auto early=run("SELECT n AS k FROM public.set_left_owned WHERE n>1 AND n IS NOT NULL ORDER BY k;");
        const auto original=run(QString::fromStdString(aliasFilter));
        QCOMPARE(early.rows,original.rows);
        QCOMPARE(early.columns,original.columns);
        run("DROP TABLE public.set_left_owned, public.set_right_owned");
    }catch(const std::exception &e){QFAIL(e.what());}}
    void directProjectionsComposeAcrossAliasesInOwnedPostgres(){try {
        using namespace navidog;
        using namespace navidog::sql::compiler;
        namespace opt=navidog::sql::optimizer;
        auto db=connectDatabase(profile_);
        const auto token=cancellation();
        const auto run=[&](const std::string &sql) {
            auto result=db->execute(QString::fromStdString(sql),{},token);
            if (!result.error.isEmpty()) throw std::runtime_error(result.error.toStdString());
            return result;
        };
        run("CREATE TABLE public.owned_project_merge(id bigint, counter smallint, memo text)");
        run("INSERT INTO public.owned_project_merge VALUES "
            "(3,7,'entry'),(3,7,'repeat'),(5,NULL,''),(NULL,11,'excluded')");
        auto snapshot=std::make_shared<const MetadataSnapshot>(collectMetadata(*db,profile_,"public",token));
        MetadataCatalog catalog(snapshot);
        const NameResolutionContext names{profile_.database.toStdString(),"public",Dialect::PostgreSQL};
        opt::PlannerOptions options;
        options.heuristic.rules={opt::HepRule::ProjectMerge};
        const auto rule=[](const auto &plan) {
            return std::find_if(plan.rules.begin(),plan.rules.end(),[](const auto &event){
                return event.rule=="HepProjectMerge";
            });
        };
        const std::string sql="SELECT p.counted AS amount,p.item_key AS chosen,p.item_key AS repeated "
            "FROM (SELECT id AS item_id,counter,memo FROM public.owned_project_merge WHERE id IS NOT NULL) "
            "AS p(item_key,counted,hidden)";
        const auto parsed=compile(Dialect::PostgreSQL,sql,{},&catalog,names);
        QVERIFY2(parsed->ok(),parsed->diagnostics.empty()?"no model":parsed->diagnostics.front().message.c_str());
        const auto plan=opt::optimize(parsed->model,nullptr,options);
        QVERIFY2(plan.ok(),plan.diagnostics.empty()?"no plan":plan.diagnostics.front().message.c_str());
        const auto event=rule(plan);
        QVERIFY(event!=plan.rules.end());
        const auto &project=plan.logical.nodes.at(event->after);
        QCOMPARE(project.kind,opt::RelKind::Project);
        QCOMPARE(project.keys.size(),std::size_t(3));
        QCOMPARE(plan.logical.nodes.at(project.inputs.front()).kind,opt::RelKind::Filter);
        auto rewritten=plan.logical.program;
        rewritten.roots={project.query};
        auto &query=rewritten.queries.at(project.query-1);
        query.projections.clear();
        for (std::size_t i=0;i<project.keys.size();++i)
            query.projections.push_back({project.keys[i].expression,project.outputs[i].name});
        const auto output=generateSql(rewritten,Dialect::PostgreSQL);
        QVERIFY2(output.ok(),output.diagnostics.empty()?"no SQL":output.diagnostics.front().message.c_str());
        const auto describe=[&](std::string text) {
            if (text.back()==';') text.pop_back();
            return run("SELECT * FROM ("+text+") AS out ORDER BY amount NULLS FIRST,chosen");
        };
        const auto original=describe(sql);
        const auto merged=describe(output.sql);
        QCOMPARE(merged.rows,original.rows);
        QCOMPARE(merged.columns,original.columns);
        QCOMPARE(merged.columnTypes,original.columnTypes);
        for (const auto *barrier:{
            "SELECT p.id FROM (SELECT id,random() AS used FROM public.owned_project_merge) p",
            "SELECT p.id FROM (SELECT id,counter FROM public.owned_project_merge LIMIT 2) p",
            "WITH p AS MATERIALIZED (SELECT id,counter FROM public.owned_project_merge) SELECT id FROM p"}) {
            const auto model=compile(Dialect::PostgreSQL,barrier,{},&catalog,names);
            QVERIFY(model->ok());
            const auto unchanged=opt::optimize(model->model,nullptr,options);
            QVERIFY(unchanged.ok());
            QVERIFY(rule(unchanged)==unchanged.rules.end());
        }
    }catch(const std::exception &e){QFAIL(e.what());}}
    void filterCanPrecedeAnUnboundedSortInOwnedPostgres(){try {
        using namespace navidog;
        using namespace navidog::sql::compiler;
        namespace opt=navidog::sql::optimizer;
        auto db=connectDatabase(profile_);
        const auto token=cancellation();
        const auto run=[&](const std::string &sql) {
            auto result=db->execute(QString::fromStdString(sql),{},token);
            if (!result.error.isEmpty()) throw std::runtime_error(result.error.toStdString());
            return result;
        };
        run("CREATE TABLE public.owned_filter_sort(k int, score bigint)");
        run("INSERT INTO public.owned_filter_sort VALUES (1,8),(3,8),(3,8),(2,NULL),(5,9),(NULL,100),(4,-1)");
        auto snapshot=std::make_shared<const MetadataSnapshot>(collectMetadata(*db,profile_,"public",token));
        MetadataCatalog catalog(snapshot);
        const NameResolutionContext names{profile_.database.toStdString(),"public",Dialect::PostgreSQL};
        opt::PlannerOptions options;
        options.heuristic.rules={opt::HepRule::FilterSortTranspose};
        const auto rule=[](const auto &plan) {
            return std::find_if(plan.rules.begin(),plan.rules.end(),[](const auto &event){
                return event.rule=="HepFilterSortTranspose";
            });
        };
        const std::string select="SELECT v.x,v.y FROM (SELECT k AS x,score AS y FROM public.owned_filter_sort ORDER BY ";
        const std::string order="y DESC NULLS LAST,x ASC NULLS FIRST";
        const std::string predicate=") AS v WHERE v.x>1;";
        const auto sql=select+order+predicate;
        const auto query=compile(Dialect::PostgreSQL,sql,{},&catalog,names);
        QVERIFY2(query->ok(),query->diagnostics.empty()?"no model":query->diagnostics.front().message.c_str());
        const auto plan=opt::optimize(query->model,nullptr,options);
        QVERIFY(plan.ok());
        const auto selected=rule(plan);
        QVERIFY(selected!=plan.rules.end());
        const auto &wrapped=plan.logical.nodes.at(selected->after);
        QCOMPARE(wrapped.kind,opt::RelKind::Subquery);
        const auto &sort=plan.logical.nodes.at(wrapped.inputs.front());
        QCOMPARE(sort.kind,opt::RelKind::Sort);
        QVERIFY(sort.descending==std::vector<bool>({true,false}));
        QVERIFY(sort.nullOrdering==std::vector<std::string>({"LAST","FIRST"}));
        const auto &project=plan.logical.nodes.at(sort.inputs.front());
        QCOMPARE(project.kind,opt::RelKind::Project);
        const auto &filter=plan.logical.nodes.at(project.inputs.front());
        QCOMPARE(filter.kind,opt::RelKind::Filter);
        QCOMPARE(filter.predicates.size(),std::size_t(1));
        auto ref=filter.predicates.front();
        auto rewritten=plan.logical.program;
        rewritten.roots={ref.query};
        rewritten.queries.at(ref.query-1).where=ref.expression;
        const auto emitted=generateSql(rewritten,Dialect::PostgreSQL);
        QVERIFY2(emitted.ok(),emitted.diagnostics.empty()?"no SQL":emitted.diagnostics.front().message.c_str());
        const auto original=run(sql);
        const auto actual=run(emitted.sql);
        QCOMPARE(actual.rows,original.rows);
        QCOMPARE(actual.columns,original.columns);
        QCOMPARE(actual.columnTypes,original.columnTypes);
        for (const auto &barrier:{select+order+" LIMIT 2"+predicate,select+"random()"+predicate}) {
            const auto model=compile(Dialect::PostgreSQL,barrier,{},&catalog,names);
            QVERIFY(model->ok());
            const auto unchanged=opt::optimize(model->model,nullptr,options);
            QVERIFY(unchanged.ok());
            QVERIFY(rule(unchanged)==unchanged.rules.end());
        }
    }catch(const std::exception &e){QFAIL(e.what());}}
    void modifyingCteReturningMatchesOwnedPostgres() {try {
        using namespace navidog;
        using namespace navidog::sql::compiler;
        namespace opt=navidog::sql::optimizer;
        auto db=connectDatabase(profile_);
        const auto token=cancellation();
        const auto run=[&](const std::string &sql) {
            const auto result=db->execute(QString::fromStdString(sql),{},token);
            if (!result.error.isEmpty()) throw std::runtime_error(result.error.toStdString());
            return result;
        };
        run("CREATE TABLE public.cte_rollback_owned(id integer PRIMARY KEY,score smallint)");
        run("CREATE TABLE public.cte_source_owned(id integer PRIMARY KEY,score smallint)");
        run("INSERT INTO public.cte_rollback_owned VALUES (1,4),(2,6)");
        run("INSERT INTO public.cte_source_owned VALUES (1,7)");
        const auto snapshot=std::make_shared<const MetadataSnapshot>(collectMetadata(*db,profile_,"public",token));
        const MetadataCatalog catalog(snapshot);
        const NameResolutionContext names{profile_.database.toStdString(),"public",Dialect::PostgreSQL};
        const auto inTransaction=[&](const std::string &sql,bool emptyAfter) {
            run("BEGIN");
            try {
                const auto result=run(sql);
                if (emptyAfter && run("SELECT id FROM public.cte_rollback_owned ORDER BY id").rows.size()!=1)
                    throw std::runtime_error("unreferenced writable CTE did not delete its row");
                run("ROLLBACK");
                return result;
            } catch (...) {
                run("ROLLBACK");
                throw;
            }
        };
        const auto &table="public.cte_rollback_owned";
        const std::vector<std::string> examples{
            std::string("WITH ins(new_id,score) AS (INSERT INTO ")+table+
                " VALUES (3,12) RETURNING id,score) SELECT new_id,score FROM ins;",
            std::string("WITH changed AS (UPDATE ")+table+" AS t SET score=s.score "
                "FROM public.cte_source_owned s WHERE t.id=s.id RETURNING t.*) "
                "SELECT id,score FROM changed;",
            std::string("WITH gone AS (DELETE FROM ")+table+
                " WHERE id=1 RETURNING id,CAST(score AS bigint) AS score), "
                "seen AS (SELECT id,score FROM gone) SELECT id,score FROM seen;",
            std::string("WITH discard AS (DELETE FROM ")+table+" WHERE id=1) SELECT 5 AS marker;",
            std::string("WITH changed(target_id,target_score,source_id,source_score) AS (UPDATE ")+table+
                " AS t SET score=s.score FROM public.cte_source_owned s WHERE t.id=s.id RETURNING *) "
                "SELECT target_id,source_score FROM changed;"};
        for (std::size_t i=0;i<examples.size();++i) {
            const auto &sql=examples[i];
            const auto compiled=compile(Dialect::PostgreSQL,sql,{},&catalog,names);
            QVERIFY2(compiled->ok(),compiled->diagnostics.empty()?sql.c_str():compiled->diagnostics.front().message.c_str());
            QVERIFY(compiled->model.front().ctes.front().query);
            const auto &cte=compiled->model.front().ctes.front();
            QVERIFY(cte.definition && cte.definition->arityKnown);
            QCOMPARE(cte.definition->columns.size(),i==3?std::size_t(0):i==4?std::size_t(4):std::size_t(2));
            if (i!=3) {
                const auto &q=compiled->model.front();
                const auto &first=compiled->model.types.get(q.expressions[q.projections[0].expression].sqlType);
                const auto &score=compiled->model.types.get(q.expressions[q.projections[1].expression].sqlType);
                QCOMPARE(first.bits.value_or(0),32);
                QCOMPARE(score.bits.value_or(0),i==2?64:16);
            }
            const auto emitted=generateSql(compiled->model,Dialect::PostgreSQL);
            QVERIFY2(emitted.ok(),emitted.diagnostics.empty()?"no SQL":emitted.diagnostics.front().message.c_str());
            QVERIFY(!generateSql(compiled->model,Dialect::MySQL).ok());
            QVERIFY(!opt::optimize(compiled->model).ok());
            QVERIFY(!opt::prepareLocalQuery(compiled->model).ok());
            const auto source=inTransaction(sql,i==3);
            const auto rewritten=inTransaction(emitted.sql,i==3);
            QCOMPARE(rewritten.rows,source.rows);
            QCOMPARE(rewritten.columns,source.columns);
            QCOMPARE(rewritten.columnTypes,source.columnTypes);
        }
        for (const auto &invalid:{
                std::string("WITH gone AS (DELETE FROM ")+table+
                    " WHERE id=1) SELECT * FROM gone;",
                std::string("SELECT * FROM (WITH gone AS (DELETE FROM ")+table+
                    " WHERE id=1 RETURNING id) SELECT * FROM gone) AS nested_cte;"}) {
            const auto compiled=compile(Dialect::PostgreSQL,invalid,{},&catalog,names);
            QVERIFY(compiled->syntaxValid && !compiled->ok());
            QVERIFY(std::any_of(compiled->diagnostics.begin(),compiled->diagnostics.end(),[](const auto &entry){
                return entry.code==DiagnosticCode::InvalidQueryShape;
            }));
            bool rejected=false;
            try { run(invalid); } catch (const std::exception &) { rejected=true; }
            QVERIFY(rejected);
        }
        QCOMPARE(run("SELECT id FROM public.cte_rollback_owned").rows.size(),2);
    }catch(const std::exception &e){QFAIL(e.what());}}
    void prunedAndFoldedValuesRetainPostgresTypes() {try {
        using namespace navidog;
        using namespace navidog::sql::compiler;
        namespace opt=navidog::sql::optimizer;
        auto db=connectDatabase(profile_);
        const auto token=cancellation();
        const auto run=[&](const std::string &sql) {
            const auto result=db->execute(QString::fromStdString(sql),{},token);
            if (!result.error.isEmpty()) throw std::runtime_error(result.error.toStdString());
            return result;
        };
        run("CREATE TABLE public.typed_empty_owned(id integer PRIMARY KEY,score smallint,amount numeric(12,2),flag boolean)");
        run("INSERT INTO public.typed_empty_owned VALUES (1,2,3.50,true),(2,NULL,NULL,NULL)");
        const auto snapshot=std::make_shared<const MetadataSnapshot>(collectMetadata(*db,profile_,"public",token));
        const MetadataCatalog catalog(snapshot);
        const NameResolutionContext names{profile_.database.toStdString(),"public",Dialect::PostgreSQL};
        for (const auto *sql:{
                 "SELECT id,score,amount,flag FROM public.typed_empty_owned WHERE id IS NULL;",
                 "SELECT id,score,amount,flag FROM public.typed_empty_owned ORDER BY id LIMIT 0;",
                 "SELECT COUNT(*) AS n,MAX(score) AS high FROM public.typed_empty_owned WHERE id IS NULL;",
                 "SELECT n FROM (VALUES (1),(NULL)) AS r(n) WHERE n IS NULL;"}) {
            const auto parsed=compile(Dialect::PostgreSQL,sql,{},&catalog,names);
            QVERIFY2(parsed->ok(),parsed->diagnostics.empty()?sql:parsed->diagnostics.front().message.c_str());
            const auto plan=opt::optimize(parsed->model);
            QVERIFY2(plan.ok(),plan.diagnostics.empty()?sql:plan.diagnostics.front().message.c_str());
            QVERIFY(plan.heuristicRoot!=plan.logical.root);
            const auto generated=opt::generatePlanSql(plan.logical,plan.heuristicRoot);
            QVERIFY2(generated.ok(),generated.diagnostics.empty()?sql:generated.diagnostics.front().message.c_str());
            QVERIFY(generated.sql.find("CAST(NULL AS ")!=std::string::npos);
            const auto expected=run(sql), actual=run(generated.sql);
            QCOMPARE(actual.rows,expected.rows);
            QCOMPARE(actual.columns,expected.columns);
            QCOMPARE(actual.columnTypes,expected.columnTypes);
        }
    }catch(const std::exception &e){QFAIL(e.what());}}
    void joinIntegerDomainsRespectNullExtension() {try {
        using namespace navidog;
        using namespace navidog::sql::compiler;
        namespace opt=navidog::sql::optimizer;
        auto db=connectDatabase(profile_);
        const auto token=cancellation();
        const auto run=[&](const std::string &sql) {
            const auto result=db->execute(QString::fromStdString(sql),{},token);
            if (!result.error.isEmpty()) throw std::runtime_error(result.error.toStdString());
            return result;
        };
        run("CREATE TABLE public.join_domains_owned(id int PRIMARY KEY,n int)");
        run("INSERT INTO public.join_domains_owned VALUES (1,NULL),(2,1),(3,2),(4,3),(5,NULL),(6,3)");
        const auto snapshot=std::make_shared<const MetadataSnapshot>(collectMetadata(*db,profile_,"public",token));
        MetadataCatalog catalog(snapshot);
        const NameResolutionContext names{profile_.database.toStdString(),"public",Dialect::PostgreSQL};
        const std::string table="public.join_domains_owned";
        const auto left="(SELECT id,n FROM "+table+" WHERE n IN (1,3)) a";
        const auto right="(SELECT id,n FROM "+table+" WHERE n=2) b";
        const auto join=" FROM "+left+" JOIN "+table+" b ON a.n=b.n";
        for (const auto &[sql,expected]:std::vector<std::pair<std::string,int>>{
                 {"SELECT a.id,b.id AS other"+join+" WHERE b.n=2 ORDER BY 1,2",0},
                 {"SELECT a.id,b.id AS other FROM "+left+" JOIN "+right+
                  " ON a.n IS NOT DISTINCT FROM b.n ORDER BY 1,2",0},
                 {"SELECT a.id,b.id AS other FROM "+table+" a JOIN "+table+
                  " b ON a.n=b.n WHERE b.n IS NULL ORDER BY 1,2",0},
                 {"SELECT a.id,b.id AS other FROM "+table+" a JOIN "+table+
                  " b ON a.n IS NOT DISTINCT FROM b.n WHERE b.n IS NULL ORDER BY 1,2",4},
                 {"SELECT a.id,b.id AS other FROM "+left+" LEFT JOIN "+right+" ON a.n=b.n WHERE b.n IS NULL ORDER BY 1,2",3},
                 {"SELECT a.id,b.id AS other FROM "+left+" LEFT JOIN "+right+" ON a.n=b.n WHERE a.n=2 ORDER BY 1,2",0},
                 {"SELECT a.id,b.id AS other FROM "+left+" FULL JOIN "+right+" ON a.n=b.n WHERE a.n IS NULL ORDER BY 1,2",1}}) {
            const auto parsed=compile(Dialect::PostgreSQL,sql,{},&catalog,names);
            QVERIFY2(parsed->ok(),parsed->diagnostics.empty()?sql.c_str():parsed->diagnostics.front().message.c_str());
            const auto plan=opt::optimize(parsed->model);
            QVERIFY2(plan.ok(),plan.diagnostics.empty()?sql.c_str():plan.diagnostics.front().message.c_str());
            opt::RelMetadataQuery metadata(plan.logical);
            const auto *facts=metadata.properties(plan.logical.root);
            QVERIFY2(facts,metadata.message().c_str());
            QCOMPARE(facts->rows.maximum==0,expected==0);
            const auto emitted=opt::generatePlanSql(plan.logical,plan.heuristicRoot);
            QVERIFY2(emitted.ok(),emitted.diagnostics.empty()?sql.c_str():emitted.diagnostics.front().message.c_str());
            const auto native=run(sql);
            const auto generated=run(emitted.sql);
            QCOMPARE(native.rows.size(),expected);
            QCOMPARE(generated.rows,native.rows);
            QCOMPARE(generated.columns,native.columns);
            QCOMPARE(generated.columnTypes,native.columnTypes);
        }
        run("DROP TABLE public.join_domains_owned");
    }catch(const std::exception &e){QFAIL(e.what());}}
    void sameSourceSetFiltersRespectNullsAndRowIdentity() {try {
        using namespace navidog;
        using namespace navidog::sql::compiler;
        namespace opt=navidog::sql::optimizer;
        auto db=connectDatabase(profile_);
        const auto token=cancellation();
        const auto run=[&](const std::string &sql) {
            const auto result=db->execute(QString::fromStdString(sql),{},token);
            if (!result.error.isEmpty()) throw std::runtime_error(result.error.toStdString());
            return result;
        };
        run("CREATE TABLE public.set_filter_proof_owned(id int PRIMARY KEY,n int,flag int)");
        run("INSERT INTO public.set_filter_proof_owned VALUES "
            "(1,NULL,1),(2,NULL,2),(3,1,1),(4,1,2),(5,2,NULL),(6,2,1),(7,3,3)");
        run("CREATE TABLE public.set_filter_rls_owned(n int)");
        run("ALTER TABLE public.set_filter_rls_owned ENABLE ROW LEVEL SECURITY");
        const auto snapshot=std::make_shared<const MetadataSnapshot>(collectMetadata(*db,profile_,"public",token));
        const MetadataCatalog catalog(snapshot);
        const NameResolutionContext names{profile_.database.toStdString(),"public",Dialect::PostgreSQL};
        QVERIFY(catalog.findTable(names,{"public","set_filter_proof_owned"})->rowSecurityDisabledKnown);
        QVERIFY(!catalog.findTable(names,{"public","set_filter_rls_owned"})->rowSecurityDisabledKnown);
        const auto plan=[&](const std::string &sql) {
            const auto parsed=compile(Dialect::PostgreSQL,sql,{},&catalog,names);
            if (!parsed->ok()) throw std::runtime_error(parsed->diagnostics.front().message);
            auto result=opt::optimize(parsed->model);
            if (!result.ok()) throw std::runtime_error(result.diagnostics.front().message);
            return result;
        };
        const auto fired=[](const opt::OptimizationResult &result) {
            return std::any_of(result.rules.begin(),result.rules.end(),[](const auto &event) {
                return event.rule=="HepSetOpToFilter";
            });
        };
        const std::string left="SELECT l.n AS k FROM public.set_filter_proof_owned l WHERE l.n IS NULL OR l.n>=1";
        const std::string right="SELECT r.n AS right_name FROM public.set_filter_proof_owned r WHERE r.n<3";
        const std::string simpleLeft="SELECT l.n AS k FROM public.set_filter_proof_owned l WHERE l.n IS NOT NULL";
        for (const auto &[source,expectedCount]:std::vector<std::pair<std::string,int>>{
                 {simpleLeft+" UNION "+right,3},
                 {simpleLeft+" INTERSECT ALL "+right,4},
                 {"SELECT l.n AS k FROM public.set_filter_proof_owned l WHERE l.n IS NULL EXCEPT ALL "+right,2},
                 {simpleLeft+" EXCEPT "+right,1},
                 {"SELECT id AS k FROM public.set_filter_proof_owned WHERE n>=1 EXCEPT ALL "
                  "SELECT id FROM public.set_filter_proof_owned WHERE flag=1",3}}) {
            const auto sorted=source+" ORDER BY 1 NULLS FIRST";
            const auto optimized=plan(sorted);
            QVERIFY2(fired(optimized),sorted.c_str());
            const auto emitted=opt::generatePlanSql(optimized.logical,optimized.heuristicRoot);
            QVERIFY2(emitted.ok(),emitted.diagnostics.empty()?sorted.c_str():emitted.diagnostics.front().message.c_str());
            QCOMPARE(emitted.sql.find("set_filter_proof_owned"),emitted.sql.rfind("set_filter_proof_owned"));
            const auto before=run(sorted),after=run(emitted.sql);
            QCOMPARE(after.rows,before.rows);
            QCOMPARE(after.columns,before.columns);
            QCOMPARE(after.columnTypes,before.columnTypes);
            QCOMPARE(after.rows.size(),expectedCount);
        }
        for (const auto &unsafe:std::vector<std::string>{
                 left+" UNION "+right,
                 simpleLeft+" UNION ALL "+right,
                 "SELECT n FROM public.set_filter_proof_owned WHERE flag=1 EXCEPT "
                 "SELECT n FROM public.set_filter_proof_owned WHERE flag=2",
                 "SELECT n FROM public.set_filter_rls_owned WHERE n>0 UNION "
                 "SELECT n FROM public.set_filter_rls_owned WHERE n<3"}) {
            QVERIFY2(!fired(plan(unsafe)),unsafe.c_str());
        }
        const std::string table=" FROM public.set_filter_proof_owned";
        const auto low="SELECT n AS k"+table+" WHERE n=1";
        const auto high="SELECT n AS right_name"+table+" WHERE n=3";
        const auto wide="SELECT n::bigint AS right_name"+table+" WHERE n=3";
        const auto nullable="SELECT n AS right_name"+table+" WHERE n IS NULL";
        for (const auto &[source,empty]:std::vector<std::pair<std::string,bool>>{
                 {"SELECT k FROM ("+low+" UNION ALL "+high+") s WHERE k=2",true},
                 {"SELECT k FROM ("+low+" UNION "+wide+") s WHERE k=2",true},
                 {low+" INTERSECT ALL "+high,true},
                 {"SELECT k FROM ("+low+" EXCEPT ALL "+nullable+") s WHERE k=2",true},
                 {"SELECT k FROM ("+low+" UNION ALL "+nullable+") s WHERE k IS NULL",false}}) {
            const auto original=run(source);
            QCOMPARE(original.rows.isEmpty(),empty);
            const auto optimized=plan(source);
            opt::RelMetadataQuery metadata(optimized.logical);
            const auto *facts=metadata.properties(optimized.logical.root);
            QVERIFY2(facts,metadata.message().c_str());
            QCOMPARE(facts->rows.maximum==0,empty);
            const auto emitted=opt::generatePlanSql(optimized.logical,optimized.heuristicRoot);
            QVERIFY2(emitted.ok(),emitted.diagnostics.empty()?source.c_str():emitted.diagnostics.front().message.c_str());
            const auto generated=run(emitted.sql);
            QCOMPARE(generated.rows,original.rows);
            QCOMPARE(generated.columnTypes,original.columnTypes);
        }
        run("DROP TABLE public.set_filter_proof_owned,public.set_filter_rls_owned");
    }catch(const std::exception &e){QFAIL(e.what());}}
    void calcPlanCombinesVerifiedScalarOperations() {try {
        using namespace navidog;
        using namespace navidog::sql::compiler;
        using namespace navidog::sql::optimizer;
        auto db=connectDatabase(profile_);
        const auto token=cancellation();
        const auto run=[&](const std::string &sql) {
            const auto result=db->execute(QString::fromStdString(sql),{},token);
            if (!result.error.isEmpty()) throw std::runtime_error(result.error.toStdString());
            return result;
        };
        run("CREATE TABLE public.calc_plan_owned(id int,payload bigint,hidden text)");
        run("INSERT INTO public.calc_plan_owned VALUES (1,9,''),(2,NULL,''),(2,7,''),(2,7,''),(3,8,'')");
        const auto snapshot=std::make_shared<const MetadataSnapshot>(collectMetadata(*db,profile_,"public",token));
        const MetadataCatalog catalog(snapshot);
        const NameResolutionContext names{profile_.database.toStdString(),"public",Dialect::PostgreSQL};
        const std::string source="SELECT payload AS amount,id AS item,id AS duplicate_item "
            "FROM public.calc_plan_owned WHERE id>=2 AND payload IS NOT NULL ORDER BY 1,2,3";
        const auto compiled=compile(names.dialect,source,{},&catalog,names);
        QVERIFY2(compiled->ok(),compiled->diagnostics.empty()?"compile":compiled->diagnostics.front().message.c_str());
        auto plan=optimize(compiled->model);
        QVERIFY2(plan.ok(),plan.diagnostics.empty()?"no plan":plan.diagnostics.front().message.c_str());
        const auto fused=std::find_if(plan.rules.begin(),plan.rules.end(),[](const auto &entry) {
            return entry.rule=="ProjectCalcMerge";
        });
        QVERIFY(fused!=plan.rules.end());
        QCOMPARE(plan.logical.nodes.at(fused->after).kind,RelKind::Calc);
        for (auto &node:plan.logical.nodes)
            std::replace(node.inputs.begin(),node.inputs.end(),fused->before,fused->after);
        const auto emitted=generatePlanSql(plan.logical,plan.logical.root);
        QVERIFY2(emitted.ok(),emitted.diagnostics.empty()?"no SQL":emitted.diagnostics.front().message.c_str());
        QVERIFY(emitted.sql.find("hidden")==std::string::npos);
        const auto before=run(source),after=run(emitted.sql);
        QCOMPARE(after.rows,before.rows);
        QCOMPARE(after.columns,before.columns);
        QCOMPARE(after.columnTypes,before.columnTypes);
        const auto effectful=compile(names.dialect,
            "SELECT random() AS random_value FROM public.calc_plan_owned WHERE id>1",{},&catalog,names);
        QVERIFY(effectful->ok());
        const auto rejected=optimize(effectful->model);
        QVERIFY(rejected.ok() && std::none_of(rejected.rules.begin(),rejected.rules.end(),[](const auto &entry) {
            return entry.rule=="ProjectCalcMerge";
        }));
        const std::string decimal="SELECT id,payload FROM public.calc_plan_owned "
            "WHERE CAST(id AS numeric(10,0))>=2.000 ORDER BY 1,2 NULLS FIRST";
        const auto decimalQuery=compile(names.dialect,decimal,{},&catalog,names);
        QVERIFY(decimalQuery->ok());
        auto normalized=optimize(decimalQuery->model);
        QVERIFY2(normalized.ok(),normalized.diagnostics.empty()?"no plan":normalized.diagnostics.front().message.c_str());
        const auto reduced=std::find_if(normalized.rules.begin(),normalized.rules.end(),[](const auto &entry) {
            return entry.rule=="ReduceDecimals";
        });
        QVERIFY(reduced!=normalized.rules.end());
        for (auto &node:normalized.logical.nodes)
            std::replace(node.inputs.begin(),node.inputs.end(),reduced->before,reduced->after);
        const auto integerSql=generatePlanSql(normalized.logical,normalized.logical.root);
        QVERIFY2(integerSql.ok(),integerSql.diagnostics.empty()?"no SQL":integerSql.diagnostics.front().message.c_str());
        QVERIFY(integerSql.sql.find("numeric")==std::string::npos);
        const auto numericRows=run(decimal),integerRows=run(integerSql.sql);
        QCOMPARE(integerRows.rows,numericRows.rows);
        QCOMPARE(integerRows.columns,numericRows.columns);
        QCOMPARE(integerRows.columnTypes,numericRows.columnTypes);
        for (const auto *condition:{"CAST(id AS numeric(1,0))>=2.0","CAST(id AS numeric(10,0))>=2.5"}) {
            const auto risky=compile(names.dialect,std::string("SELECT id FROM public.calc_plan_owned WHERE ")+condition,
                                     {},&catalog,names);
            QVERIFY(risky->ok());
            const auto retained=optimize(risky->model);
            QVERIFY(retained.ok() && std::none_of(retained.rules.begin(),retained.rules.end(),[](const auto &entry) {
                return entry.rule=="ReduceDecimals";
            }));
        }
        run("DROP TABLE public.calc_plan_owned");
    }catch(const std::exception &e){QFAIL(e.what());}}
    void integerSeriesMatchesOwnedPostgres() {try {
        using namespace navidog;
        using namespace navidog::sql::compiler;
        using namespace navidog::sql::optimizer;
        auto db=connectDatabase(profile_);
        const auto token=cancellation();
        const auto run=[&](const std::string &sql) {
            const auto result=db->execute(QString::fromStdString(sql),{},token);
            if (!result.error.isEmpty()) throw std::runtime_error(result.error.toStdString());
            return result;
        };
        const auto dialect=Dialect::PostgreSQL;
        for (const auto &[sql,count]:std::vector<std::pair<std::string,std::optional<std::size_t>>>{
                 {"SELECT g.v,g.n FROM pg_catalog.generate_series(2,8,3) WITH ORDINALITY AS g(v,n) ORDER BY n",3},
                 {"SELECT g.v,g.n FROM pg_catalog.generate_series(7,2,-2) WITH ORDINALITY AS g(v,n) ORDER BY n",3},
                 {"SELECT g FROM pg_catalog.generate_series(1,3,-1) g",0},
                 {"SELECT g FROM pg_catalog.generate_series(NULL,3,0) g",0},
                 {"SELECT g.v,g.n FROM pg_catalog.generate_series(9223372036854775806::bigint, "
                  "9223372036854775807::bigint,2) WITH ORDINALITY AS g(v,n)",1},
                 {"SELECT g.v,g.n FROM pg_catalog.generate_series(0::bigint,(-9223372036854775808)::bigint, "
                  "(-9223372036854775808)::bigint) WITH ORDINALITY AS g(v,n) ORDER BY n",2},
                 {"SELECT a.id,g.v,g.n FROM (VALUES (1),(2),(3)) a(id) LEFT JOIN LATERAL "
                  "pg_catalog.generate_series(2,a.id) WITH ORDINALITY AS g(v,n) ON TRUE ORDER BY a.id,g.n",{}},
                 {"SELECT g FROM pg_catalog.generate_series(1,(SELECT 2)) AS g ORDER BY g",{}}}) {
            const auto compiled=compile(dialect,sql);
            QVERIFY2(compiled->ok(),compiled->diagnostics.empty()?sql.c_str():compiled->diagnostics.front().message.c_str());
            const auto native=run(sql);
            const auto normalized=convertSql(dialect,dialect,sql);
            QVERIFY2(normalized.ok(),normalized.diagnostics.empty()?sql.c_str():normalized.diagnostics.front().message.c_str());
            const auto canonical=run(normalized.sql);
            QCOMPARE(canonical.rows,native.rows);
            QCOMPARE(canonical.columns,native.columns);
            QCOMPARE(canonical.columnTypes,native.columnTypes);
            const auto plan=optimize(compiled->model);
            QVERIFY2(plan.ok(),plan.diagnostics.empty()?sql.c_str():plan.diagnostics.front().message.c_str());
            const auto emitted=generatePlanSql(plan.logical,plan.heuristicRoot);
            QVERIFY2(emitted.ok(),emitted.diagnostics.empty()?sql.c_str():emitted.diagnostics.front().message.c_str());
            const auto generated=run(emitted.sql);
            QCOMPARE(generated.rows,native.rows);
            QCOMPARE(generated.columns,native.columns);
            QCOMPARE(generated.columnTypes,native.columnTypes);
            const auto local=interpret(compiled->model);
            QVERIFY2(local.ok(),local.diagnostics.empty()?sql.c_str():local.diagnostics.front().message.c_str());
            QCOMPARE(local.rows.size(),static_cast<std::size_t>(native.rows.size()));
            for (std::size_t row=0;row<local.rows.size();++row)
                for (std::size_t column=0;column<local.rows[row].size();++column) {
                    const auto &value=native.rows[row][column];
                    if (value.isNull()) QVERIFY(std::holds_alternative<std::monostate>(local.rows[row][column]));
                    else QCOMPARE(std::get<std::int64_t>(local.rows[row][column]),value.toLongLong());
                }
            if (count) {
                RelMetadataQuery properties(plan.logical);
                const auto *facts=properties.properties(plan.logical.root);
                QVERIFY2(facts,properties.message().c_str());
                QCOMPARE(facts->rows.minimum,static_cast<std::uint64_t>(*count));
                QVERIFY(facts->rows.maximum && *facts->rows.maximum==*count);
                QVERIFY(std::any_of(plan.best.nodes.begin(),plan.best.nodes.end(),[&](const auto &node) {
                    return node.kind==PhysicalKind::Series && node.rows==*count && !node.assumedStatistics;
                }));
            }
        }
        for (const auto *sql:{
                 "SELECT v,n FROM pg_catalog.generate_series(1.1,3.3,1.1) WITH ORDINALITY AS s(v,n) ORDER BY n",
                 "SELECT v FROM pg_catalog.generate_series(TIMESTAMP '2024-02-28',TIMESTAMP '2024-03-01',"
                 "INTERVAL '1 day') AS s(v) ORDER BY v"}) {
            const auto parsed=compile(dialect,sql);
            QVERIFY(parsed->ok());
            const auto canonical=convertSql(dialect,dialect,sql);
            QVERIFY2(canonical.ok(),canonical.diagnostics.empty()?sql:canonical.diagnostics.front().message.c_str());
            const auto original=run(sql);
            const auto normalized=run(canonical.sql);
            QCOMPARE(normalized.rows,original.rows);
            QCOMPARE(normalized.columnTypes,original.columnTypes);
            QVERIFY(!interpret(parsed->model).ok());
        }
        const auto zero=compile(dialect,"SELECT g FROM pg_catalog.generate_series(1,3,0) AS g");
        QVERIFY(zero->ok());
        const auto error=interpret(zero->model);
        QVERIFY(!error.ok() && error.rows.empty());
        QVERIFY(error.diagnostics.front().message.find("step size cannot equal zero")!=std::string::npos);
        try {
            run("SELECT g FROM pg_catalog.generate_series(1,3,0) AS g");
            QFAIL("PostgreSQL accepted zero series step size");
        } catch (const std::exception &failure) {
            QVERIFY2(std::string_view(failure.what()).find("step size cannot equal zero")!=std::string_view::npos,
                     failure.what());
        }
        const auto large=compile(dialect,"SELECT g FROM pg_catalog.generate_series(-9223372036854775808,9223372036854775807) g");
        QVERIFY(large->ok());
        InterpreterLimits limits;
        limits.maxRows=8;
        const auto capped=interpret(large->model,limits);
        QVERIFY(!capped.ok() && capped.rows.empty() && capped.diagnostics.front().code==DiagnosticCode::ResourceLimit);
        const auto parameter=compile(dialect,"SELECT g FROM pg_catalog.generate_series($1,3) g ORDER BY g");
        QVERIFY(parameter->ok());
        const auto prepared=prepareLocalQuery(parameter->model);
        QVERIFY(prepared.ok());
        const auto two=executeLocalQuery(*prepared.query,{std::int64_t{2}});
        QVERIFY(two.ok());
        QCOMPARE(two.rows.size(),std::size_t{2});
        QCOMPARE(std::get<std::int64_t>(two.rows.front().front()),std::int64_t{2});
    }catch(const std::exception &e){QFAIL(e.what());}}
    void scalarArrayComparisonsMatchOwnedPostgres() {try {
        using namespace navidog;
        using namespace navidog::sql::compiler;
        using namespace navidog::sql::optimizer;
        auto db=connectDatabase(profile_);
        const auto token=cancellation();
        const auto run=[&](const std::string &sql) {
            const auto result=db->execute(QString::fromStdString(sql),{},token);
            if (!result.error.isEmpty()) throw std::runtime_error(result.error.toStdString());
            return result;
        };
        const auto dialect=Dialect::PostgreSQL;
        for (const auto &[sql,local]:std::vector<std::pair<std::string,bool>>{
                 {"SELECT n,n = ANY(ARRAY[1,3,NULL]) AS any_equal,n <> ALL(ARRAY[1,3,NULL]) AS all_unequal,"
                  "n > SOME(ARRAY[[1,NULL],[2,3]]) AS some_larger,n >= ALL(ARRAY[1,2,3]) AS all_at_least "
                  "FROM (VALUES (0),(1),(2),(3),(4),(NULL)) s(n) ORDER BY n NULLS LAST",true},
                 {"SELECT NULL::int <> ALL(ARRAY[]::integer[]) AS empty_all,"
                  "NULL::int = ANY(ARRAY[]::integer[]) AS empty_any,1 = ANY(NULL::int[]) AS null_any,"
                  "2.5 > ALL(ARRAY[1,2]) AS rounded",true},
                 {"SELECT n,2 = ANY(a) AS hit FROM (VALUES (1,ARRAY[1,2]),(2,NULL::int[]),(3,ARRAY[9])) s(n,a) ORDER BY n",false},
                 {"SELECT 2 = ANY(ARRAY(SELECT n FROM (VALUES (2),(4)) s(n))) AS collected,"
                  "'a' = ANY(ARRAY['a','z']) AS text_hit",false}}) {
            const auto parsed=compile(dialect,sql);
            QVERIFY2(parsed->ok(),parsed->diagnostics.empty()?sql.c_str():parsed->diagnostics.front().message.c_str());
            const auto original=run(sql);
            const auto normalized=convertSql(dialect,dialect,sql);
            QVERIFY2(normalized.ok(),normalized.diagnostics.empty()?sql.c_str():normalized.diagnostics.front().message.c_str());
            const auto canonical=run(normalized.sql);
            QCOMPARE(canonical.rows,original.rows);
            QCOMPARE(canonical.columnTypes,original.columnTypes);
            const auto plan=optimize(parsed->model);
            QVERIFY(plan.ok());
            const auto generated=generatePlanSql(plan.logical,plan.heuristicRoot);
            QVERIFY2(generated.ok(),generated.diagnostics.empty()?sql.c_str():generated.diagnostics.front().message.c_str());
            const auto emitted=run(generated.sql);
            QCOMPARE(emitted.rows,original.rows);
            QCOMPARE(emitted.columns,original.columns);
            QCOMPARE(emitted.columnTypes,original.columnTypes);
            if (!local) continue;
            const auto expected=interpret(parsed->model);
            QVERIFY2(expected.ok(),expected.diagnostics.empty()?sql.c_str():expected.diagnostics.front().message.c_str());
            QCOMPARE(expected.rows.size(),static_cast<std::size_t>(original.rows.size()));
            for (std::size_t row=0;row<expected.rows.size();++row)
                for (std::size_t column=0;column<expected.rows[row].size();++column) {
                    const auto &value=original.rows[row][column];
                    if (value.isNull()) QVERIFY(std::holds_alternative<std::monostate>(expected.rows[row][column]));
                    else if (std::holds_alternative<bool>(expected.rows[row][column])) {
                        const auto nativeBool=value.toString();
                        QVERIFY(nativeBool==QStringLiteral("t") || nativeBool==QStringLiteral("f"));
                        QCOMPARE(std::get<bool>(expected.rows[row][column]),nativeBool==QStringLiteral("t"));
                    }
                    else QCOMPARE(std::get<std::int64_t>(expected.rows[row][column]),value.toLongLong());
                }
        }
        const auto parameter=compile(dialect,"SELECT $1 = ANY(ARRAY[1,4,NULL]) AS candidate");
        QVERIFY(parameter->ok());
        const auto prepared=prepareLocalQuery(parameter->model);
        QVERIFY(prepared.ok());
        const auto match=executeLocalQuery(*prepared.query,{std::int64_t{4}});
        QVERIFY(match.ok() && std::get<bool>(match.rows.front().front()));
        run("CREATE TABLE public.array_compare_owned(id int PRIMARY KEY,n int)");
        run("INSERT INTO public.array_compare_owned VALUES (1,NULL),(3,2),(5,4),(8,NULL),(9,6)");
        const auto snapshot=std::make_shared<const MetadataSnapshot>(collectMetadata(*db,profile_,"public",token));
        const MetadataCatalog catalog(snapshot);
        const NameResolutionContext names{profile_.database.toStdString(),"public",dialect};
        for (const auto &[predicate,max]:std::vector<std::pair<std::string,std::optional<std::uint64_t>>>{
                 {"id = ANY(ARRAY[1,3,3,NULL,9])",3},
                 {"id = SOME(ARRAY[[1,3],[NULL,9]]) AND id BETWEEN 2 AND 8",1},
                 {"id <> ALL(ARRAY[2,5,7]) AND id BETWEEN 3 AND 8",4},
                 {"id > SOME(ARRAY[1,3,NULL]) AND id<4",2},
                 {"id >= ALL(ARRAY[1,3,3]) AND id<=7",5},
                 {"id < ALL(ARRAY[1,3]) AND id>=-2",3},
                 {"id = ALL(ARRAY[3,3])",1},
                 {"id <> ANY(ARRAY[3,3,NULL]) AND id BETWEEN 1 AND 5",4},
                 {"id <> ANY(ARRAY[3,4,NULL]) AND id BETWEEN 1 AND 5",5},
                 {"id = ALL(ARRAY[1,9])",0},
                 {"id >= ALL(ARRAY[1,NULL])",0},
                 {"id < ANY(ARRAY[-9223372036854775808]::bigint[])",0},
                 {"id <> ALL(ARRAY[3,NULL])",0},
                 {"id = ANY(NULL::int[])",0},
                 {"n <> ALL(ARRAY[]::int[]) AND n IS NULL",{}}}) {
            const auto sql="SELECT id,n FROM public.array_compare_owned WHERE "+predicate+" ORDER BY id";
            const auto parsed=compile(dialect,sql,{},&catalog,names);
            QVERIFY2(parsed->ok(),parsed->diagnostics.empty()?sql.c_str():parsed->diagnostics.front().message.c_str());
            const auto plan=optimize(parsed->model);
            QVERIFY(plan.ok());
            RelMetadataQuery metadata(plan.logical);
            const auto *facts=metadata.properties(plan.logical.root);
            QVERIFY2(facts,metadata.message().c_str());
            QCOMPARE(facts->rows.maximum,max);
            const auto original=run(sql);
            if (max) QVERIFY(static_cast<std::uint64_t>(original.rows.size())<=*max);
            else QCOMPARE(original.rows.size(),2);
            const auto generated=generatePlanSql(plan.logical,plan.heuristicRoot);
            QVERIFY2(generated.ok(),generated.diagnostics.empty()?sql.c_str():generated.diagnostics.front().message.c_str());
            const auto emitted=run(generated.sql);
            QCOMPARE(emitted.rows,original.rows);
            QCOMPARE(emitted.columnTypes,original.columnTypes);
        }
        run("DROP TABLE public.array_compare_owned");
    }catch(const std::exception &e){QFAIL(e.what());}}
    void subscriptedWritesMatchOwnedPostgres() {try {
        using namespace navidog;
        using namespace navidog::sql::compiler;
        auto db=connectDatabase(profile_);
        const auto token=cancellation();
        const auto run=[&](const std::string &sql) {
            const auto result=db->execute(QString::fromStdString(sql),{},token);
            if (!result.error.isEmpty()) throw std::runtime_error(result.error.toStdString());
            return result;
        };
        const std::string table="public.subscript_write_owned";
        run("CREATE TABLE "+table+" (id int PRIMARY KEY, values_array int[], other_array int[], payload jsonb)");
        const auto reset=[&] {
            run("TRUNCATE "+table);
            run("INSERT INTO "+table+" VALUES(1,ARRAY[1,2,3],ARRAY[4,5],'{\"a\":{\"x\":2}}')");
        };
        reset();
        const auto snapshot=std::make_shared<const MetadataSnapshot>(collectMetadata(*db,profile_,"public",token));
        const MetadataCatalog catalog(snapshot);
        const auto dialect=Dialect::PostgreSQL;
        const NameResolutionContext names{profile_.database.toStdString(),"public",dialect};
        for (const auto &statement:std::vector<std::string>{
                 "UPDATE "+table+" SET values_array[1]=values_array[2], values_array[2]=values_array[1] RETURNING values_array",
                 "UPDATE "+table+" SET values_array[1]=90, values_array[1]=91 RETURNING values_array",
                 "UPDATE "+table+" SET (values_array[2],other_array[1])=(12,30) RETURNING values_array,other_array",
                 "UPDATE "+table+" AS t SET values_array[d.pos]=d.amount FROM (VALUES (1,100)) d(pos,amount) "
                 "WHERE t.id=1 RETURNING values_array",
                 "UPDATE "+table+" SET values_array[2:3]=ARRAY[20,30], payload['a']['x']='10' RETURNING values_array,payload",
                 "INSERT INTO "+table+" AS t VALUES(1,ARRAY[9],ARRAY[4],'{}') "
                 "ON CONFLICT(id) DO UPDATE SET values_array[t.id]=excluded.values_array[1] RETURNING values_array",
                 "MERGE INTO "+table+" AS t USING (VALUES (1,2,42)) AS d(id,pos,amount) ON t.id=d.id "
                 "WHEN MATCHED THEN UPDATE SET values_array[d.pos]=d.amount"}) {
            const auto compiled=compile(dialect,statement,{},&catalog,names);
            QVERIFY2(compiled->ok(),compiled->diagnostics.empty()?statement.c_str():compiled->diagnostics.front().message.c_str());
            const auto generated=generateSql(compiled->model,dialect);
            QVERIFY2(generated.ok(),generated.diagnostics.empty()?statement.c_str():generated.diagnostics.front().message.c_str());
            reset();
            const auto native=run(statement);
            const auto expected=run("SELECT * FROM "+table);
            reset();
            const auto emitted=run(generated.sql);
            QCOMPARE(emitted.rows,native.rows);
            QCOMPARE(emitted.columns,native.columns);
            QCOMPARE(emitted.columnTypes,native.columnTypes);
            const auto actual=run("SELECT * FROM "+table);
            QCOMPARE(actual.rows,expected.rows);
        }
        for (const auto &invalid:std::vector<std::string>{
                 "UPDATE "+table+" SET id[1]=42",
                 "UPDATE "+table+" SET values_array[1]=DEFAULT",
                 "UPDATE "+table+" SET values_array[1]=3,values_array=ARRAY[4]",
                 "UPDATE "+table+" SET values_array[without_a_binding]=42"}) {
            const auto parsed=compile(dialect,invalid,{},&catalog,names);
            QVERIFY2(parsed->syntaxValid && !parsed->ok(),invalid.c_str());
            try {
                run(invalid);
                QFAIL("PostgreSQL accepted an invalid subscripted update");
            } catch (const std::exception &error) {
                QVERIFY(!std::string_view(error.what()).empty());
            }
        }
        const auto parameters=compile(dialect,"UPDATE "+table+" SET values_array[$1]=$2",{},&catalog,names);
        QVERIFY(parameters->ok());
        std::size_t resolved=0;
        for (const auto &expression:parameters->model.front().expressions)
            if (expression.kind==ExprKind::Parameter) {
                const auto &type=parameters->model.types.get(expression.sqlType);
                QVERIFY(type.family==TypeFamily::Integer && type.bits==32 && type.arrayDimensions==0);
                ++resolved;
            }
        QCOMPARE(resolved,std::size_t{2});
        run("DROP TABLE "+table);
    }catch(const std::exception &e){QFAIL(e.what());}}
    void arrayAndJsonSubscriptMatchesOwnedPostgres() {try {
        using namespace navidog;
        using namespace navidog::sql::compiler;
        using namespace navidog::sql::optimizer;
        auto db=connectDatabase(profile_);
        const auto token=cancellation();
        const auto run=[&](const std::string &sql) {
            const auto result=db->execute(QString::fromStdString(sql),{},token);
            if (!result.error.isEmpty()) throw std::runtime_error(result.error.toStdString());
            return result;
        };
        const auto dialect=Dialect::PostgreSQL;
        for (const auto &[sql,local]:std::vector<std::pair<std::string,bool>>{
                 {"SELECT i,(ARRAY[[1,NULL],[3,4]])[i][2] AS cell FROM (VALUES (0),(1),(2),(3),(NULL)) s(i) "
                  "ORDER BY i NULLS LAST",true},
                 {"SELECT ('[-2:0]={10,20,NULL}'::int[])[-2] AS edge, "
                  "('[-2:0]={10,20,NULL}'::int[])[0] AS nullable, "
                  "(ARRAY[[1,2],[3,4]])[2] AS bad_dim,(ARRAY[10,20])[1.5] AS rounded",true},
                 {"SELECT (ARRAY[[1,2],[3,4]])[:2][1] AS mixed, "
                  "(ARRAY[1,2,3])[:2] AS upper_bound,(ARRAY[1,2,3])[2:] AS lower_bound, "
                  "(ARRAY[1,2,3])[:] AS full_slice,(ARRAY[1,2,3])[NULL::int:] AS null_slice",false},
                 {"SELECT ('{\"data\":[1,2,3]}'::jsonb)['data'][-1] AS payload, "
                  "('{\"data\":[]}'::jsonb)['data'][0] AS missing",false},
                 {"SELECT (ARRAY(SELECT v FROM (VALUES (1),(2)) s(v) ORDER BY v DESC))[2] AS collected",false}}) {
            const auto compiled=compile(dialect,sql);
            QVERIFY2(compiled->ok(),compiled->diagnostics.empty()?sql.c_str():compiled->diagnostics.front().message.c_str());
            const auto native=run(sql);
            const auto normalized=convertSql(dialect,dialect,sql);
            QVERIFY2(normalized.ok(),normalized.diagnostics.empty()?sql.c_str():normalized.diagnostics.front().message.c_str());
            const auto roundTrip=run(normalized.sql);
            QCOMPARE(roundTrip.rows,native.rows);
            QCOMPARE(roundTrip.columnTypes,native.columnTypes);
            const auto plan=optimize(compiled->model);
            QVERIFY(plan.ok());
            const auto emitted=generatePlanSql(plan.logical,plan.heuristicRoot);
            QVERIFY2(emitted.ok(),emitted.diagnostics.empty()?sql.c_str():emitted.diagnostics.front().message.c_str());
            const auto selected=run(emitted.sql);
            QCOMPARE(selected.rows,native.rows);
            QCOMPARE(selected.columns,native.columns);
            QCOMPARE(selected.columnTypes,native.columnTypes);
            if (!local) continue;
            const auto evaluated=interpret(compiled->model);
            QVERIFY2(evaluated.ok(),evaluated.diagnostics.empty()?sql.c_str():evaluated.diagnostics.front().message.c_str());
            QCOMPARE(evaluated.rows.size(),static_cast<std::size_t>(native.rows.size()));
            for (std::size_t row=0;row<evaluated.rows.size();++row)
                for (std::size_t column=0;column<evaluated.rows[row].size();++column) {
                    const auto &value=native.rows[row][column];
                    if (value.isNull()) QVERIFY(std::holds_alternative<std::monostate>(evaluated.rows[row][column]));
                    else QCOMPARE(std::get<std::int64_t>(evaluated.rows[row][column]),value.toLongLong());
                }
        }
        const auto overflow=compile(dialect,"SELECT (ARRAY[1])[2147483648]");
        QVERIFY(overflow->ok());
        const auto localError=interpret(overflow->model);
        QVERIFY(!localError.ok() && localError.rows.empty());
        try {
            run("SELECT (ARRAY[1])[2147483648]");
            QFAIL("PostgreSQL accepted a subscript outside integer range");
        } catch (const std::exception &error) {
            QVERIFY2(std::string_view(error.what()).find("integer out of range")!=std::string_view::npos,error.what());
        }
        const auto parameter=compile(dialect,"SELECT (ARRAY[1,2])[$1] AS item");
        QVERIFY(parameter->ok());
        const auto local=prepareLocalQuery(parameter->model);
        QVERIFY(local.ok());
        const auto item=executeLocalQuery(*local.query,{std::int64_t{2}});
        QVERIFY(item.ok() && item.rows.size()==1);
        QCOMPARE(std::get<std::int64_t>(item.rows.front().front()),std::int64_t{2});
        const auto missing=executeLocalQuery(*local.query,{std::monostate{}});
        QVERIFY(missing.ok() && missing.rows.size()==1);
        QVERIFY(std::holds_alternative<std::monostate>(missing.rows.front().front()));
    }catch(const std::exception &e){QFAIL(e.what());}}
    void nativeArrayExpansionDecorrelatesAndFiltersBeforeExpansion() {try {
        using namespace navidog;
        using namespace navidog::sql::compiler;
        using namespace navidog::sql::optimizer;
        auto db=connectDatabase(profile_);
        const auto token=cancellation();
        const auto run=[&](const std::string &sql) {
            const auto output=db->execute(QString::fromStdString(sql),{},token);
            if (!output.error.isEmpty()) throw std::runtime_error(output.error.toStdString());
            return output;
        };
        run("CREATE TABLE public.unnest_rule_owned(id int PRIMARY KEY, items integer[])");
        run("INSERT INTO public.unnest_rule_owned VALUES (1,ARRAY[2,2,NULL]),(2,'{}'),(3,NULL),(4,ARRAY[-1,5])");
        const auto snapshot=std::make_shared<const MetadataSnapshot>(collectMetadata(*db,profile_,"public",token));
        const MetadataCatalog catalog(snapshot);
        const NameResolutionContext names{profile_.database.toStdString(),"public",Dialect::PostgreSQL};
        const std::string source=" FROM public.unnest_rule_owned AS a "
            "CROSS JOIN LATERAL (SELECT pg_catalog.unnest(a.items) AS element) AS u";
        const auto verify=[&](const std::string &sql,std::string_view rule,bool applies) {
            const auto compiled=compile(names.dialect,sql,{},&catalog,names);
            QVERIFY2(compiled->ok(),compiled->diagnostics.empty()?sql.c_str():compiled->diagnostics.front().message.c_str());
            PlannerOptions options;
            options.heuristic.rules.clear();
            auto plan=optimize(compiled->model,nullptr,options);
            QVERIFY2(plan.ok(),plan.diagnostics.empty()?sql.c_str():plan.diagnostics.front().message.c_str());
            const auto expected=run(sql);
            const auto sourceSql=generateSql(compiled->model,names.dialect);
            QVERIFY2(sourceSql.ok(),sourceSql.diagnostics.empty()?sql.c_str():sourceSql.diagnostics.front().message.c_str());
            const auto regenerated=run(sourceSql.sql);
            QCOMPARE(regenerated.rows,expected.rows);
            QCOMPARE(regenerated.columns,expected.columns);
            QCOMPARE(regenerated.columnTypes,expected.columnTypes);
            const auto equal=[&](RelId root) {
                const auto emitted=generatePlanSql(plan.logical,root);
                QVERIFY2(emitted.ok(),emitted.diagnostics.empty()?sql.c_str():emitted.diagnostics.front().message.c_str());
                const auto actual=run(emitted.sql);
                QCOMPARE(actual.rows,expected.rows);
                QCOMPARE(actual.columns,expected.columns);
                QCOMPARE(actual.columnTypes,expected.columnTypes);
            };
            equal(plan.logical.root);
            const auto event=std::find_if(plan.rules.begin(),plan.rules.end(),[&](const auto &entry) {
                return entry.rule==rule;
            });
            QCOMPARE(event!=plan.rules.end(),applies);
            if (event!=plan.rules.end()) {
                for (auto &node:plan.logical.nodes)
                    std::replace(node.inputs.begin(),node.inputs.end(),event->before,event->after);
                equal(plan.logical.root==event->before?event->after:plan.logical.root);
            }
        };
        verify("SELECT u.element"+source+" ORDER BY 1 NULLS FIRST","UnnestDecorrelate",true);
        verify("SELECT a.id,u.element"+source+" WHERE a.id>=2 ORDER BY 1,2 NULLS FIRST",
               "FilterTableFunctionTranspose",true);
        verify("SELECT a.id,u.element"+source+" WHERE u.element>=2 ORDER BY 1,2 NULLS FIRST",
               "FilterTableFunctionTranspose",false);
        verify("SELECT u.element FROM public.unnest_rule_owned a "
               "CROSS JOIN LATERAL pg_catalog.unnest(a.items) AS u(element) ORDER BY 1 NULLS FIRST",
               "UnnestDecorrelate",true);
        verify("SELECT u.element FROM public.unnest_rule_owned a, "
               "pg_catalog.unnest(a.items) AS u(element) ORDER BY 1 NULLS FIRST",
               "UnnestDecorrelate",true);
        verify("SELECT u.u FROM public.unnest_rule_owned a "
               "CROSS JOIN LATERAL pg_catalog.unnest(a.items) AS u ORDER BY 1 NULLS FIRST",
               "UnnestDecorrelate",true);
        verify("SELECT u.u FROM pg_catalog.unnest(CAST('{1,2}' AS integer[])) AS u ORDER BY 1",
               "UnnestDecorrelate",false);
        const std::string withOrdinality=" FROM public.unnest_rule_owned a "
            "CROSS JOIN LATERAL pg_catalog.unnest(a.items) WITH ORDINALITY AS u(element, pos)";
        verify("SELECT a.id,u.element,u.pos"+withOrdinality+" ORDER BY a.id,u.pos",
               "UnnestDecorrelate",true);
        verify("SELECT a.id,u.element,u.pos"+withOrdinality+" WHERE a.id>=2 ORDER BY a.id,u.pos",
               "FilterTableFunctionTranspose",true);
        verify("SELECT a.id,u.element,u.pos"+withOrdinality+" WHERE u.pos>=2 ORDER BY a.id,u.pos",
               "FilterTableFunctionTranspose",false);
        verify("SELECT a.id,u.element,u.ordinality FROM public.unnest_rule_owned a "
               "LEFT JOIN LATERAL pg_catalog.unnest(a.items) WITH ORDINALITY AS u(element) "
               "ON TRUE ORDER BY a.id,u.ordinality","UnnestDecorrelate",false);
        verify("SELECT u.u,u.ordinality FROM pg_catalog.unnest(CAST('[0:2]={1,1,NULL}' AS integer[])) "
               "WITH ORDINALITY AS u ORDER BY u.ordinality","UnnestDecorrelate",false);
        verify("SELECT unnest,ordinality FROM pg_catalog.unnest(CAST('{2,3}' AS smallint[])) "
               "WITH ORDINALITY ORDER BY ordinality","UnnestDecorrelate",false);
        verify("SELECT a.id,u.item,u.pos FROM public.unnest_rule_owned a "
               "CROSS JOIN LATERAL pg_catalog.unnest(ARRAY[a.id,NULL]) WITH ORDINALITY AS u(item,pos) "
               "ORDER BY a.id,u.pos","UnnestDecorrelate",false);
        verify("SELECT a.id,u.item,u.pos FROM public.unnest_rule_owned a CROSS JOIN LATERAL pg_catalog.unnest("
               "ARRAY(SELECT v FROM (VALUES (1),(2),(3)) s(v) WHERE v<=a.id ORDER BY v DESC)) "
               "WITH ORDINALITY AS u(item,pos) ORDER BY a.id,u.pos","UnnestDecorrelate",false);
        for (const auto *sql:{
            "SELECT u.item,u.pos FROM pg_catalog.unnest(CAST('[-2:1]={ -3, -3, null, \" +5 \"}' AS integer[])) "
              "WITH ORDINALITY AS u(item,pos) ORDER BY u.pos",
            "SELECT u.u FROM pg_catalog.unnest(CAST('{}' AS integer[])) AS u",
            "SELECT u.u,u.ordinality FROM pg_catalog.unnest(CAST(NULL AS smallint[])) WITH ORDINALITY AS u",
            "SELECT u.v FROM pg_catalog.unnest(CAST('{-9223372036854775808,9223372036854775807}' AS bigint[])) "
              "AS u(v) ORDER BY u.v",
            "SELECT v,n FROM pg_catalog.unnest(ARRAY[-8,NULL,2147483648]) WITH ORDINALITY AS u(v,n) ORDER BY n",
            "SELECT v FROM pg_catalog.unnest(ARRAY[]::smallint[]) AS u(v)",
            "SELECT v,n FROM pg_catalog.unnest(ARRAY[[1,2],[NULL,-3]]) WITH ORDINALITY AS u(v,n) ORDER BY n",
            "SELECT v,n FROM pg_catalog.unnest(ARRAY[[[1,2],[3,4]],[[5,NULL],[7,8]]]::smallint[]) "
              "WITH ORDINALITY AS u(v,n) ORDER BY n",
            "SELECT v FROM pg_catalog.unnest(ARRAY[[],[]]::integer[]) AS u(v)",
            "SELECT v,n FROM pg_catalog.unnest(ARRAY(SELECT v FROM (VALUES (1),(2),(NULL::int)) s(v) "
              "ORDER BY v DESC NULLS LAST)) WITH ORDINALITY AS u(v,n) ORDER BY n",
            "SELECT v,n FROM pg_catalog.unnest(ARRAY(VALUES (7),(2))) WITH ORDINALITY AS u(v,n) ORDER BY n",
            "SELECT v FROM pg_catalog.unnest(ARRAY(VALUES (7) EXCEPT VALUES (7))) AS u(v)",
            "SELECT v FROM pg_catalog.unnest(ARRAY[-9223372036854775808,9223372036854775807]) AS u(v) ORDER BY v",
            "SELECT v,n FROM pg_catalog.unnest(ARRAY[-32768,32767]::smallint[]) WITH ORDINALITY AS u(v,n) ORDER BY n",
            "SELECT u.element,u.pos FROM pg_catalog.unnest(CAST('{32767,-32768}' AS smallint[])) "
              "WITH ORDINALITY AS u(element,pos) ORDER BY u.pos"}) {
            const auto compiled=compile(names.dialect,sql,{},&catalog,names);
            QVERIFY2(compiled->ok(),compiled->diagnostics.empty()?sql:compiled->diagnostics.front().message.c_str());
            const auto native=run(sql);
            const auto local=interpret(compiled->model);
            QVERIFY2(local.ok(),local.diagnostics.empty()?sql:local.diagnostics.front().message.c_str());
            QCOMPARE(local.rows.size(),static_cast<std::size_t>(native.rows.size()));
            QCOMPARE(local.columns.size(),static_cast<std::size_t>(native.columns.size()));
            for (std::size_t column=0;column<local.columns.size();++column)
                QCOMPARE(QString::fromStdString(local.columns[column]),native.columns[column]);
            for (std::size_t row=0;row<local.rows.size();++row)
                for (std::size_t column=0;column<local.rows[row].size();++column) {
                    const auto &value=native.rows[row][column];
                    if (value.isNull()) QVERIFY(std::holds_alternative<std::monostate>(local.rows[row][column]));
                    else QCOMPARE(std::get<std::int64_t>(local.rows[row][column]),value.toLongLong());
                }
            const auto cost=optimize(compiled->model);
            QVERIFY(cost.ok());
            const auto generated=generatePlanSql(cost.logical,cost.heuristicRoot);
            QVERIFY2(generated.ok(),generated.diagnostics.empty()?sql:generated.diagnostics.front().message.c_str());
            const auto emitted=run(generated.sql);
            QCOMPARE(emitted.rows,native.rows);
            QCOMPARE(emitted.columnTypes,native.columnTypes);
            const auto arraySource=std::string_view(sql).find("ARRAY(")==std::string_view::npos
                ?"verified PostgreSQL array literal":"PostgreSQL ARRAY query row estimate";
            const bool exactEstimate=std::string_view(sql).find("EXCEPT")==std::string_view::npos;
            QVERIFY(std::any_of(cost.best.nodes.begin(),cost.best.nodes.end(),[&](const auto &node) {
                return node.kind==PhysicalKind::Unnest && (!exactEstimate || node.rows==local.rows.size()) &&
                    node.description.find(arraySource)!=std::string::npos;
            }));
            RelMetadataQuery metadata(cost.logical);
            const auto *facts=metadata.properties(cost.logical.root);
            QVERIFY2(facts,metadata.message().c_str());
            QCOMPARE(facts->rows.minimum,static_cast<std::uint64_t>(local.rows.size()));
            if (exactEstimate) QVERIFY(facts->rows.maximum && *facts->rows.maximum==local.rows.size());
        }
        const auto capped=compile(names.dialect,
            "SELECT u.v FROM pg_catalog.unnest(CAST('{1,2,3}' AS smallint[])) AS u(v)");
        QVERIFY(capped->ok());
        InterpreterLimits small;
        small.maxRows=2;
        const auto exceeded=interpret(capped->model,small);
        QVERIFY(!exceeded.ok() && exceeded.rows.empty() &&
                exceeded.diagnostics.front().code==DiagnosticCode::ResourceLimit);
        const auto constructed=compile(names.dialect,"SELECT v FROM pg_catalog.unnest(ARRAY[1,2,3]) AS u(v)");
        QVERIFY(constructed->ok());
        const auto constructedLimit=interpret(constructed->model,small);
        QVERIFY(!constructedLimit.ok() && constructedLimit.rows.empty() &&
                constructedLimit.diagnostics.front().code==DiagnosticCode::ResourceLimit);
        const auto collected=compile(names.dialect,"SELECT v FROM pg_catalog.unnest(ARRAY(VALUES (1),(2),(3))) AS u(v)");
        QVERIFY(collected->ok());
        const auto collectedLimit=interpret(collected->model,small);
        QVERIFY(!collectedLimit.ok() && collectedLimit.rows.empty() &&
                collectedLimit.diagnostics.front().code==DiagnosticCode::ResourceLimit);
        const auto ragged=compile(names.dialect,"SELECT ARRAY[[1],[2,3]] AS ragged");
        QVERIFY(ragged->syntaxValid && !ragged->ok());
        try {
            run("SELECT ARRAY[[1],[2,3]] AS ragged");
            QFAIL("PostgreSQL accepted ragged array");
        } catch (const std::exception &error) {
            QVERIFY2(std::string(error.what()).find("matching dimensions")!=std::string::npos,error.what());
        }
        const auto bad=compile(names.dialect,
            "SELECT u.v FROM pg_catalog.unnest(CAST('{32768}' AS smallint[])) AS u(v)");
        QVERIFY(bad->ok());
        const auto overflow=interpret(bad->model);
        QVERIFY(!overflow.ok() && overflow.rows.empty());
        const auto keyQuery=compile(names.dialect,"SELECT u.ordinality FROM "
            "pg_catalog.unnest(CAST('{2,3}' AS smallint[])) WITH ORDINALITY AS u",{},&catalog,names);
        QVERIFY(keyQuery->ok());
        const auto keyPlan=optimize(keyQuery->model);
        QVERIFY(keyPlan.ok());
        RelMetadataQuery ordinalMetadata(keyPlan.logical);
        const auto *ordinalFacts=ordinalMetadata.properties(keyPlan.logical.root);
        QVERIFY2(ordinalFacts,ordinalMetadata.message().c_str());
        QVERIFY(coversNonNullUniqueKey(*ordinalFacts,keyPlan.logical.nodes.at(keyPlan.logical.root).outputs));
        QVERIFY(!compile(names.dialect,"SELECT * FROM pg_catalog.unnest(CAST('{1}' AS integer[])) "
                         "WITH ORDINALITY AS u(val,pos,extra)",{},&catalog,names)->ok());
        QVERIFY(!compile(names.dialect,"SELECT * FROM pg_catalog.unnest(CAST('{x}' AS text[])) AS u",
                         {},&catalog,names)->ok());
        run("DROP TABLE public.unnest_rule_owned");
    }catch(const std::exception &e){QFAIL(e.what());}}
    void nativeBernoulliPreservesSamplingAndTypedResults() {try {
        using namespace navidog;
        using namespace navidog::sql::compiler;
        using namespace navidog::sql::optimizer;
        auto db=connectDatabase(profile_);
        const auto token=cancellation();
        const auto run=[&](const std::string &sql) {
            const auto result=db->execute(QString::fromStdString(sql),{},token);
            if (!result.error.isEmpty()) throw std::runtime_error(result.error.toStdString());
            return result;
        };
        run("CREATE TABLE public.sample_rule_owned(id int PRIMARY KEY,name text)");
        run("INSERT INTO public.sample_rule_owned SELECT i,i::text FROM generate_series(1,150) AS i");
        const auto snapshot=std::make_shared<const MetadataSnapshot>(collectMetadata(*db,profile_,"public",token));
        const MetadataCatalog catalog(snapshot);
        const NameResolutionContext names{profile_.database.toStdString(),"public",Dialect::PostgreSQL};
        for (const std::string rate:{"0","31.125","100"}) {
            const auto sql="SELECT t.id,t.name FROM public.sample_rule_owned AS t TABLESAMPLE BERNOULLI ("+
                rate+") REPEATABLE (7) WHERE t.id>=5 ORDER BY 1";
            const auto parsed=compile(Dialect::PostgreSQL,sql,{},&catalog,names);
            QVERIFY2(parsed->ok(),parsed->diagnostics.empty()?sql.c_str():parsed->diagnostics.front().message.c_str());
            const auto rows=run(sql);
            const auto result=optimize(parsed->model);
            QVERIFY2(result.ok(),result.diagnostics.empty()?sql.c_str():result.diagnostics.front().message.c_str());
            auto verify=[&](RelId before=0,RelId after=0) {
                auto logical=result.logical;
                auto root=logical.root;
                if (root==before) root=after;
                if (before) for (auto &node:logical.nodes)
                    if (&node!=&logical.nodes[after])
                        std::replace(node.inputs.begin(),node.inputs.end(),before,after);
                const auto emitted=generatePlanSql(logical,root);
                QVERIFY2(emitted.ok(),emitted.diagnostics.empty()?sql.c_str():emitted.diagnostics.front().message.c_str());
                if (!before) QVERIFY(emitted.sql.find("BERNOULLI ("+rate+") REPEATABLE (7)")!=std::string::npos);
                const auto actual=run(emitted.sql);
                QCOMPARE(actual.rows,rows.rows);
                QCOMPARE(actual.columns,rows.columns);
                QCOMPARE(actual.columnTypes,rows.columnTypes);
            };
            verify();
            for (const std::string rule:{"SampleToFilter","FilterSampleTranspose"}) {
                const auto event=std::find_if(result.rules.begin(),result.rules.end(),[&](const auto &candidate) {
                    return candidate.rule==rule;
                });
                QCOMPARE(event!=result.rules.end(),rate!="31.125");
                if (event!=result.rules.end()) verify(event->before,event->after);
            }
        }
        const auto tooLarge=compile(Dialect::PostgreSQL,
            "SELECT * FROM public.sample_rule_owned TABLESAMPLE BERNOULLI (100.001)");
        QVERIFY(!tooLarge->ok());
        const auto cte=compile(Dialect::PostgreSQL,
            "WITH t AS (SELECT 1) SELECT * FROM t TABLESAMPLE BERNOULLI (100)");
        QVERIFY(!cte->ok());
        run("DROP TABLE public.sample_rule_owned");
    }catch(const std::exception &e){QFAIL(e.what());}}
    void calciteWithinDistinctPreservesNullableTuplesAndEmptyGroups() {try {
        using namespace navidog;
        using namespace navidog::sql::compiler;
        using namespace navidog::sql::optimizer;
        auto db=connectDatabase(profile_);
        const auto token=cancellation();
        const auto run=[&](const std::string &sql) {
            auto result=db->execute(QString::fromStdString(sql),{},token);
            if (!result.error.isEmpty()) throw std::runtime_error(result.error.toStdString());
            return result;
        };
        run("CREATE TABLE public.within_distinct_owned(g int,k int,tag smallint)");
        run("INSERT INTO public.within_distinct_owned VALUES (1,1,1),(1,1,1),(1,1,2),(1,NULL,1),"
            "(1,NULL,NULL),(1,NULL,NULL),(2,NULL,NULL),(2,NULL,NULL),(NULL,8,8)");
        const auto snapshot=std::make_shared<const MetadataSnapshot>(collectMetadata(*db,profile_,"public",token));
        const MetadataCatalog catalog(snapshot);
        const NameResolutionContext names{profile_.database.toStdString(),"public",Dialect::PostgreSQL};
        LanguageOptions enabled;
        enabled.calciteExtensions=true;
        const std::string from=" FROM public.within_distinct_owned";
        const auto verify=[&](const std::string &sql,const std::string &oracle) {
            const auto compiled=compile(names.dialect,sql,enabled,&catalog,names);
            QVERIFY2(compiled->ok(),compiled->diagnostics.empty()?sql.c_str():compiled->diagnostics.front().message.c_str());
            const auto emitted=generateSql(compiled->model,names.dialect);
            QVERIFY2(emitted.ok(),emitted.diagnostics.empty()?sql.c_str():emitted.diagnostics.front().message.c_str());
            QVERIFY(emitted.sql.find("ROW(")!=std::string::npos);
            const auto reparsed=compile(names.dialect,emitted.sql,{},&catalog,names);
            QVERIFY2(reparsed->ok(),reparsed->diagnostics.empty()?emitted.sql.c_str():reparsed->diagnostics.front().message.c_str());
            const auto result=optimize(compiled->model);
            QVERIFY(result.ok());
            const auto planned=generatePlanSql(result.logical,result.logical.root);
            QVERIFY2(planned.ok(),planned.diagnostics.empty()?sql.c_str():planned.diagnostics.front().message.c_str());
            const auto expected=run(oracle);
            for (const auto &output:{run(emitted.sql),run(planned.sql)}) {
                QCOMPARE(output.rows,expected.rows);
                QCOMPARE(output.columns,expected.columns);
                QCOMPARE(output.columnTypes,expected.columnTypes);
            }
        };
        verify("SELECT g,COUNT(*) WITHIN DISTINCT (k,tag) AS n"+from+" GROUP BY g ORDER BY 1 NULLS FIRST",
               "SELECT g,COUNT(*) AS n FROM (SELECT DISTINCT g,k,tag"+from+") t GROUP BY g ORDER BY 1 NULLS FIRST");
        verify("SELECT COUNT(*) WITHIN DISTINCT (k) FILTER (WHERE tag=1) AS n"+from,
               "SELECT COUNT(*) AS n FROM (SELECT DISTINCT k"+from+" WHERE tag=1) t");
        verify("SELECT COUNT(*) WITHIN DISTINCT (k) AS n"+from+" WHERE g>9",
               "SELECT COUNT(*) AS n FROM (SELECT DISTINCT k"+from+" WHERE g>9) t");
        QVERIFY(!compile(names.dialect,"SELECT COUNT(*) WITHIN DISTINCT (k)"+from,{},&catalog,names)->ok());
        QVERIFY(!compile(names.dialect,"SELECT COUNT(*) WITHIN DISTINCT (CAST(k AS text))"+from,enabled,&catalog,names)->ok());
        QVERIFY(!compile(names.dialect,"SELECT SUM(g) WITHIN DISTINCT (k)"+from,enabled,&catalog,names)->ok());
        run("DROP TABLE public.within_distinct_owned");
    }catch(const std::exception &e){QFAIL(e.what());}}
    void aggregateRulesBindColumnsAndKeepEmptyGroupSemantics() {try {
        using namespace navidog;
        using namespace navidog::sql::compiler;
        using namespace navidog::sql::optimizer;
        auto db=connectDatabase(profile_);
        const auto token=cancellation();
        const auto run=[&](const std::string &sql) {
            const auto result=db->execute(QString::fromStdString(sql),{},token);
            if (!result.error.isEmpty()) throw std::runtime_error(result.error.toStdString());
            return result;
        };
        run("CREATE TABLE public.agg_rules_owned(k int,v bigint,flag int,payload text)");
        run("INSERT INTO public.agg_rules_owned VALUES (NULL,8,NULL,'a'),(NULL,NULL,1,'b'),"
            "(1,9007199254740993,1,'c'),(1,4,2,'d'),(2,NULL,0,'e'),(2,5,1,'f')");
        run("CREATE TABLE public.agg_rules_join_owned(k int, group_key int)");
        run("INSERT INTO public.agg_rules_join_owned VALUES (1,1),(1,1),(2,NULL),(NULL,9),(8,8)");
        const auto snapshot=std::make_shared<const MetadataSnapshot>(collectMetadata(*db,profile_,"public",token));
        const MetadataCatalog catalog(snapshot);
        const NameResolutionContext names{profile_.database.toStdString(),"public",Dialect::PostgreSQL};
        const auto plan=[&](const std::string &sql) {
            const auto parsed=compile(Dialect::PostgreSQL,sql,{},&catalog,names);
            if (!parsed->ok()) throw std::runtime_error(parsed->diagnostics.front().message);
            auto result=optimize(parsed->model);
            if (!result.ok()) throw std::runtime_error(result.diagnostics.front().message);
            return result;
        };
        const auto find=[](const OptimizationResult &result,std::string_view rule) {
            return std::find_if(result.rules.begin(),result.rules.end(),[&](const auto &event) {
                return event.rule==rule;
            });
        };
        const auto verify=[&](const std::string &sql,std::string_view rule,std::string_view evidence) {
            auto result=plan(sql);
            const auto event=find(result,rule);
            QVERIFY2(event!=result.rules.end(),sql.c_str());
            auto root=result.heuristicRoot;
            if (!event->heuristic) {
                root=result.logical.root;
                if (root==event->before) root=event->after;
                for (auto &node:result.logical.nodes)
                    std::replace(node.inputs.begin(),node.inputs.end(),event->before,event->after);
            }
            const auto emitted=generatePlanSql(result.logical,root);
            QVERIFY2(emitted.ok(),emitted.diagnostics.empty()?sql.c_str():emitted.diagnostics.front().message.c_str());
            QVERIFY2(emitted.sql.find(evidence)!=std::string::npos,emitted.sql.c_str());
            QVERIFY(emitted.sql.find("payload")==std::string::npos);
            const auto before=run(sql),after=run(emitted.sql);
            QCOMPARE(after.rows,before.rows);
            QCOMPARE(after.columns,before.columns);
            QCOMPARE(after.columnTypes,before.columnTypes);
        };
        verify("SELECT k AS key, MAX(k) AS same, MIN(v) AS smallest "
               "FROM public.agg_rules_owned GROUP BY k ORDER BY 1 NULLS FIRST",
               "HepAggregateReduceFunctionsOnGroupKeys","GROUP BY");
        verify("SELECT k AS key, MIN(v) AS smallest FROM public.agg_rules_owned GROUP BY k ORDER BY 1 NULLS FIRST",
               "HepAggregateExtractProject","GROUP BY");
        for (auto limit:{"1","99"})
            verify("SELECT COUNT(*) AS rows,COUNT(v) AS present,MIN(v) AS low,SUM(v) AS total "
                   "FROM public.agg_rules_owned WHERE flag >= "+std::string(limit)+" AND v IS NOT NULL",
                   "HepAggregateFilterToFilteredAggregate","FILTER (WHERE");
        verify("SELECT t.bucket AS key,MIN(t.amount) AS low,COUNT(DISTINCT t.amount) AS n "
               "FROM (SELECT v AS amount,k AS bucket FROM public.agg_rules_owned WHERE flag IS NOT NULL) t "
               "GROUP BY t.bucket ORDER BY 1 NULLS FIRST",
               "HepAggregateProjectMerge","GROUP BY");
        verify("SELECT k AS key, MIN(v) AS low FROM public.agg_rules_owned WHERE k>=1 "
               "GROUP BY k ORDER BY 1 NULLS FIRST","AggregateFilterTranspose","HAVING ");
        verify("SELECT k AS key,COUNT(DISTINCT v) AS n,SUM(DISTINCT v) AS amount "
               "FROM public.agg_rules_owned GROUP BY k ORDER BY 1 NULLS FIRST",
               "HepAggregateExpandDistinct","SELECT DISTINCT");
        verify("SELECT COUNT(DISTINCT v) AS n,MIN(DISTINCT v) AS low "
               "FROM public.agg_rules_owned WHERE flag>=99",
               "HepAggregateExpandDistinct","SELECT DISTINCT");
        run("INSERT INTO public.agg_rules_owned VALUES (1,4,3,'duplicate'),(1,4,NULL,'again'),(3,0,1,'z')");
        verify("SELECT k AS key,COUNT(DISTINCT v) FILTER (WHERE k>1) AS n,"
               "SUM(DISTINCT v) FILTER (WHERE v>1) AS amount "
               "FROM public.agg_rules_owned GROUP BY k ORDER BY 1 NULLS FIRST",
               "HepAggregateExpandDistinct","FILTER (WHERE");
        verify("SELECT COUNT(DISTINCT v) FILTER (WHERE FALSE) AS n,"
               "MIN(DISTINCT v) FILTER (WHERE v IS NOT NULL) AS low FROM public.agg_rules_owned WHERE flag>=99",
               "HepAggregateExpandDistinct","SELECT DISTINCT");
        for (const auto *condition : {"flag=1", "random()<0.5"}) {
            const auto restricted=plan("SELECT COUNT(DISTINCT v) FILTER (WHERE " +
                std::string(condition) + ") FROM public.agg_rules_owned");
            QVERIFY(find(restricted,"HepAggregateExpandDistinct")==restricted.rules.end());
        }
        verify("SELECT a.low FROM (SELECT k,MIN(v) AS low,MAX(flag) AS unused "
               "FROM public.agg_rules_owned GROUP BY k) a ORDER BY 1 NULLS FIRST",
               "HepProjectAggregateMerge","GROUP BY");
        verify("SELECT b.group_key,MIN(a.v) AS amount FROM public.agg_rules_owned a "
               "JOIN public.agg_rules_join_owned b ON a.k=b.k GROUP BY b.group_key ORDER BY 1 NULLS FIRST",
               "HepAggregateJoinTranspose","min(");
        for (const auto *bound:{"1","99"}) {
            verify("SELECT 7 AS bucket,COUNT(*) AS n,MIN(v) AS amount "
                   "FROM public.agg_rules_owned WHERE flag>="+std::string(bound)+" GROUP BY bucket",
                   "AggregateProjectConstantToDummyJoin","VALUES");
            verify("SELECT k,7 AS bucket,MIN(v) AS amount "
                   "FROM public.agg_rules_owned WHERE flag>="+std::string(bound)+
                   " GROUP BY k,bucket ORDER BY 1 NULLS FIRST",
                   "AggregateProjectConstantToDummyJoin","VALUES");
        }
        const auto hidden=plan("SELECT k,MIN(v) FROM public.agg_rules_owned WHERE flag>0 GROUP BY k");
        QVERIFY(find(hidden,"AggregateFilterTranspose")==hidden.rules.end());
        QVERIFY(find(hidden,"HepAggregateFilterToFilteredAggregate")==hidden.rules.end());
        const auto computed=plan("SELECT CAST(k AS integer),MIN(v) FROM public.agg_rules_owned "
                                 "WHERE k>=1 GROUP BY k");
        QVERIFY(find(computed,"AggregateFilterTranspose")==computed.rules.end());
        const auto rollup=plan("SELECT k,MAX(k) FROM public.agg_rules_owned GROUP BY ROLLUP(k)");
        QVERIFY(find(rollup,"HepAggregateReduceFunctionsOnGroupKeys")==rollup.rules.end());
        const auto differing=plan("SELECT COUNT(DISTINCT flag),SUM(DISTINCT v) FROM public.agg_rules_owned");
        QVERIFY(find(differing,"HepAggregateExpandDistinct")==differing.rules.end());
        const auto mixed=plan("SELECT COUNT(DISTINCT flag),SUM(v) FROM public.agg_rules_owned");
        QVERIFY(find(mixed,"HepAggregateExpandDistinct")==mixed.rules.end());
        run("DROP TABLE public.agg_rules_owned,public.agg_rules_join_owned");
    }catch(const std::exception &e){QFAIL(e.what());}}
    void relationalWindowSqlPreservesFramesAndNamedPartitions() {try {
        using namespace navidog;
        using namespace navidog::sql::compiler;
        using namespace navidog::sql::optimizer;
        auto db=connectDatabase(profile_);
        const auto token=cancellation();
        const auto run=[&](const std::string &sql) {
            const auto result=db->execute(QString::fromStdString(sql),{},token);
            if (!result.error.isEmpty()) throw std::runtime_error(result.error.toStdString());
            return result;
        };
        run("CREATE TABLE public.emit_window_owned(id integer PRIMARY KEY,g integer,v integer,unused text)");
        run("INSERT INTO public.emit_window_owned VALUES (1,1,2,'a'),(2,1,2,'b'),(3,1,5,'c'),"
            "(4,1,NULL,'d'),(5,NULL,3,'e'),(6,NULL,3,'f'),(7,NULL,9,'g')");
        const MetadataCatalog catalog(std::make_shared<const MetadataSnapshot>(
            collectMetadata(*db,profile_,"public",token)));
        const NameResolutionContext names{profile_.database.toStdString(),"public",Dialect::PostgreSQL};
        const std::string from=" FROM public.emit_window_owned t";
        const std::string sharedOrder="SELECT id,SUM(id) OVER (PARTITION BY g ORDER BY id "
            "ROWS BETWEEN 1 PRECEDING AND CURRENT ROW) AS total,"
            "COUNT(*) OVER (PARTITION BY g ORDER BY id ROWS BETWEEN CURRENT ROW AND 1 FOLLOWING) AS peers"+
            from+" ORDER BY 1";
        for (const auto &sql:{
                 "SELECT t.id,ROW_NUMBER() OVER ordered AS seq,LAG(t.v,1,-1) OVER ordered AS preceding,"
                     "LEAD(t.v,1,-1) OVER ordered AS following"+from+
                     " WINDOW partitioned AS (PARTITION BY t.g), ordered AS (partitioned ORDER BY t.id)"
                     " ORDER BY 1",
                 "SELECT t.id,RANK() OVER (PARTITION BY g ORDER BY v DESC NULLS FIRST) AS score_rank,"
                     "SUM(v) FILTER (WHERE v>2) OVER (PARTITION BY g ORDER BY v DESC NULLS FIRST "
                     "GROUPS BETWEEN 1 PRECEDING AND CURRENT ROW EXCLUDE TIES) AS running"+from+" ORDER BY 1",
                 "SELECT t.id,AVG(v) OVER (PARTITION BY g ORDER BY id ROWS BETWEEN 1 PRECEDING AND 1 FOLLOWING) AS mean,"
                     "COUNT(*) OVER (PARTITION BY g ORDER BY id RANGE BETWEEN 1 PRECEDING AND 1 FOLLOWING "
                     "EXCLUDE CURRENT ROW) AS neighbors"+from+" ORDER BY 1",
                 "SELECT t.id,FIRST_VALUE(v) OVER (PARTITION BY g ORDER BY id "
                     "ROWS BETWEEN 1 PRECEDING AND CURRENT ROW) AS first,"
                     "DENSE_RANK() OVER (PARTITION BY g ORDER BY v) AS dense"+from+
                     " WHERE id>=2 ORDER BY 1",
                 "SELECT g AS bucket,SUM(v) AS group_sum,AVG(v) AS group_mean,"
                     "RANK() OVER (ORDER BY SUM(v) DESC) AS pos"+from+
                     " GROUP BY 1 HAVING COUNT(*)>=2 ORDER BY 4,1",
                 "SELECT g AS bucket,GROUPING(g) AS is_total,SUM(v) AS n,"
                     "SUM(SUM(v)) OVER(ORDER BY GROUPING(g),g ROWS UNBOUNDED PRECEDING) AS running"+from+
                     " GROUP BY GROUPING SETS ((g),()) ORDER BY 2,1",
                 sharedOrder}) {
            const auto parsed=compile(Dialect::PostgreSQL,sql,{},&catalog,names);
            QVERIFY2(parsed->ok(),parsed->diagnostics.empty()?sql.c_str():parsed->diagnostics.front().message.c_str());
            const auto plan=optimize(parsed->model);
            QVERIFY2(plan.ok(),(sql+" | "+(plan.diagnostics.empty()?"no plan":plan.diagnostics.front().message)).c_str());
            const auto expected=run(sql);
            if (sql==sharedOrder) {
                const auto proven=std::find_if(plan.rules.begin(),plan.rules.end(),[](const auto &event) {
                    return event.rule=="HepProjectOverSumToSum0";
                });
                QVERIFY(proven!=plan.rules.end());
                const auto &window=plan.logical.nodes.at(proven->after);
                QCOMPARE(window.windowGroups.size(),std::size_t(1));
                QCOMPARE(window.windowGroups.front().functions.size(),std::size_t(2));
                QCOMPARE(window.nonNullWindowSums.size(),std::size_t(1));
                const std::vector<FederatedTableBinding> sources{
                    {names.database,"public","emit_window_owned",DatabaseKind::PostgreSQL,
                     db.get(),"public","emit_window_owned"}};
                const auto local=executeFederatedQuery(parsed->model,sources,token);
                QVERIFY2(local.ok(),local.diagnostics.empty()?"no local rows":local.diagnostics.front().message.c_str());
                QCOMPARE(local.rows.size(),static_cast<std::size_t>(expected.rows.size()));
                for (std::size_t i=0;i<local.rows.size();++i)
                    for (std::size_t j=0;j<local.rows[i].size();++j)
                        QCOMPARE(std::get<std::int64_t>(local.rows[i][j]),expected.rows.at(i).at(j).toLongLong());
            } else {
                QVERIFY(std::none_of(plan.rules.begin(),plan.rules.end(),[](const auto &event) {
                    return event.rule=="HepProjectOverSumToSum0";
                }));
            }
            for (const auto root:{plan.logical.root,plan.heuristicRoot}) {
                const auto generated=generatePlanSql(plan.logical,root);
                QVERIFY2(generated.ok(),generated.diagnostics.empty()?sql.c_str():generated.diagnostics.front().message.c_str());
                QVERIFY(generated.sql.find(" OVER (")!=std::string::npos);
                QVERIFY(generated.sql.find("unused")==std::string::npos);
                const auto actual=run(generated.sql);
                QCOMPARE(actual.rows,expected.rows);
                QCOMPARE(actual.columns,expected.columns);
                QCOMPARE(actual.columnTypes,expected.columnTypes);
            }
        }
    }catch(const std::exception &e){QFAIL(e.what());}}
    void windowFilterMovesOnlyWholeCommonPartitions() {try {
        using namespace navidog;
        using namespace navidog::sql::compiler;
        using namespace navidog::sql::optimizer;
        auto db=connectDatabase(profile_);
        const auto token=cancellation();
        const auto run=[&](const std::string &sql) {
            const auto result=db->execute(QString::fromStdString(sql),{},token);
            if (!result.error.isEmpty()) throw std::runtime_error(result.error.toStdString());
            return result;
        };
        run("CREATE TABLE public.window_filter_owned(id integer PRIMARY KEY,k integer,v integer,unused text)");
        run("INSERT INTO public.window_filter_owned(id,k,v) VALUES (1,9,7),(2,9,7),(3,9,9),(4,NULL,3),(5,NULL,10)");
        run("INSERT INTO public.window_filter_owned(id,k,v) SELECT i,10+i%17,i%3 FROM generate_series(100,4096) i");
        const MetadataCatalog catalog(std::make_shared<const MetadataSnapshot>(
            collectMetadata(*db,profile_,"public",token)));
        const NameResolutionContext names{profile_.database.toStdString(),"public",Dialect::PostgreSQL};
        const std::vector<FederatedTableBinding> sources{
            {names.database,"public","window_filter_owned",DatabaseKind::PostgreSQL,
             db.get(),"public","window_filter_owned"}};
        const std::string select="SELECT s.id,s.grp,s.pos,s.ranking FROM "
            "(SELECT id,k AS grp,ROW_NUMBER() OVER (PARTITION BY k ORDER BY id) AS pos,";
        const std::string from=" FROM public.window_filter_owned) s WHERE ";
        const std::string rank="RANK() OVER (PARTITION BY k ORDER BY v DESC) AS ranking";
        for (const std::string predicate:{"s.grp=9 AND s.pos<=2","s.grp IS NULL","s.grp=3999"}) {
            const auto sql=select+rank+from+predicate+" ORDER BY 1";
            const auto parsed=compile(Dialect::PostgreSQL,sql,{},&catalog,names);
            QVERIFY2(parsed->ok(),parsed->diagnostics.empty()?sql.c_str():parsed->diagnostics.front().message.c_str());
            const auto plan=optimize(parsed->model);
            QVERIFY2(plan.ok(),plan.diagnostics.empty()?sql.c_str():plan.diagnostics.front().message.c_str());
            QVERIFY(std::any_of(plan.rules.begin(),plan.rules.end(),[](const RuleEvent &entry){
                return entry.rule=="HepFilterWindowTranspose";
            }));
            QVERIFY(std::any_of(plan.rules.begin(),plan.rules.end(),[](const RuleEvent &entry){
                return entry.rule=="HepProjectWindowTranspose";
            }));
            const auto expected=run(sql);
            const auto emitted=generatePlanSql(plan.logical,plan.heuristicRoot);
            QVERIFY2(emitted.ok(),emitted.diagnostics.empty()?sql.c_str():emitted.diagnostics.front().message.c_str());
            const auto actual=run(emitted.sql);
            QCOMPARE(actual.rows,expected.rows);
            QCOMPARE(actual.columns,expected.columns);
            QCOMPARE(actual.columnTypes,expected.columnTypes);
            InterpreterLimits limits;
            limits.maxRows=3;
            const auto local=executeFederatedQuery(parsed->model,sources,token,limits);
            QVERIFY2(local.ok(),local.diagnostics.empty()?sql.c_str():local.diagnostics.front().message.c_str());
            std::vector<LocalRow> rows;
            for (const auto &row:expected.rows) {
                LocalRow values;
                for (const auto &value:row)
                    values.push_back(value.isNull()?LocalValue{}:LocalValue{std::int64_t(value.toLongLong())});
                rows.push_back(std::move(values));
            }
            QVERIFY(rows==local.rows);
        }
        for (const auto &sql:{
                 select+"RANK() OVER (ORDER BY v) AS ranking"+from+"s.grp=9",
                 select+"RANK() OVER (PARTITION BY v ORDER BY id) AS ranking"+from+"s.grp=9",
                 select+rank+from+"s.id<3",
                 "SELECT s.grp,s.pos FROM (SELECT k AS grp,ROW_NUMBER() OVER (PARTITION BY k ORDER BY v) AS pos"
                     +from+"s.grp=9"}) {
            const auto parsed=compile(Dialect::PostgreSQL,sql,{},&catalog,names);
            QVERIFY(parsed->ok());
            const auto plan=optimize(parsed->model);
            QVERIFY(plan.ok());
            QVERIFY(std::none_of(plan.rules.begin(),plan.rules.end(),[](const RuleEvent &entry) {
                return entry.rule=="HepFilterWindowTranspose";
            }));
        }
    }catch(const std::exception &e){QFAIL(e.what());}}
    void relationalLateralSqlPreservesScopedColumns() {try {
        using namespace navidog;
        using namespace navidog::sql::compiler;
        using namespace navidog::sql::optimizer;
        auto db=connectDatabase(profile_);
        const auto token=cancellation();
        const auto run=[&](const std::string &sql) {
            const auto result=db->execute(QString::fromStdString(sql),{},token);
            if (!result.error.isEmpty()) throw std::runtime_error(result.error.toStdString());
            return result;
        };
        run("CREATE TABLE public.emit_lateral_left_owned(id integer,ref integer,private_text text)");
        run("CREATE TABLE public.emit_lateral_right_owned(id integer,v bigint,private_text text)");
        run("INSERT INTO public.emit_lateral_left_owned VALUES (1,2,'a'),(2,1,'b'),(3,9,'c'),(4,NULL,'d')");
        run("INSERT INTO public.emit_lateral_right_owned VALUES (1,10,'x'),(2,50,'y'),(2,30,'z'),(2,NULL,'m')");
        const MetadataCatalog catalog(std::make_shared<const MetadataSnapshot>(
            collectMetadata(*db,profile_,"public",token)));
        const NameResolutionContext names{profile_.database.toStdString(),"public",Dialect::PostgreSQL};
        const std::string left=" FROM public.emit_lateral_left_owned l";
        const std::string right=" FROM public.emit_lateral_right_owned r";
        const std::string child="(SELECT r.v"+right+" WHERE r.id=l.ref ORDER BY r.v NULLS LAST LIMIT 1) q";
        const std::vector<FederatedTableBinding> sources{
            {names.database,"public","emit_lateral_left_owned",DatabaseKind::PostgreSQL,
             db.get(),"public","emit_lateral_left_owned"},
            {names.database,"public","emit_lateral_right_owned",DatabaseKind::PostgreSQL,
             db.get(),"public","emit_lateral_right_owned"}};
        const auto verifyLocal=[&](const Interpretation &local,const QueryResult &expected) {
            QVERIFY2(local.ok(),local.diagnostics.empty()?"no local rows":local.diagnostics.front().message.c_str());
            std::vector<LocalRow> rows;
            QStringList columns;
            for (const auto &name:local.columns) columns<<QString::fromStdString(name);
            for (const auto &row:expected.rows) {
                LocalRow values;
                for (const auto &value:row)
                    values.push_back(value.isNull()?LocalValue{}:LocalValue{std::int64_t(value.toLongLong())});
                rows.push_back(std::move(values));
            }
            QVERIFY(rows==local.rows);
            QCOMPARE(columns,expected.columns);
        };
        for (const auto &sql:{
                 "SELECT l.id,q.v"+left+" CROSS JOIN LATERAL "+child+" ORDER BY 1",
                 "SELECT l.id,q.v"+left+" INNER JOIN LATERAL "+child+" ON TRUE ORDER BY 1",
                 "SELECT l.id,q.v"+left+" LEFT JOIN LATERAL "+child+" ON TRUE ORDER BY 1",
                 "SELECT l.id,q.n"+left+" LEFT JOIN LATERAL "
                       "(SELECT l.ref+100 AS n WHERE l.id=2) q ON TRUE ORDER BY 1",
                 "SELECT q.n,r.v,l.id"+left+" CROSS JOIN LATERAL (SELECT l.ref+1 AS n) q "
                       "LEFT JOIN LATERAL (SELECT a.v FROM public.emit_lateral_right_owned a "
                       "WHERE a.id=q.n OR a.id=l.ref ORDER BY a.v LIMIT 1) r ON TRUE ORDER BY 3",
                 "SELECT l.id,q.v"+left+" LEFT JOIN LATERAL "
                       "(SELECT z.v"+right+" LEFT JOIN LATERAL (SELECT r.v+CAST(l.ref AS bigint) AS v) z ON TRUE "
                       "WHERE r.id=l.ref ORDER BY z.v LIMIT 1) q ON TRUE ORDER BY 1",
                 "SELECT l.id,q.nested_ref"+left+" LEFT JOIN LATERAL "
                       "(SELECT (SELECT l.ref+100) AS nested_ref, l.id AS unused_id) q ON TRUE ORDER BY 1"}) {
            const auto parsed=compile(Dialect::PostgreSQL,sql,{},&catalog,names);
            QVERIFY2(parsed->ok(),parsed->diagnostics.empty()?sql.c_str():parsed->diagnostics.front().message.c_str());
            const auto plan=optimize(parsed->model);
            QVERIFY2(plan.ok(),plan.diagnostics.empty()?sql.c_str():plan.diagnostics.front().message.c_str());
            const auto expected=run(sql);
            verifyLocal(executeFederatedQuery(parsed->model,sources,token),expected);
            if (sql.find("nested_ref")!=std::string::npos) {
                const auto narrowed=std::find_if(plan.rules.begin(),plan.rules.end(),[](const RuleEvent &entry) {
                    return entry.rule=="HepProjectCorrelateTranspose";
                });
                QVERIFY(narrowed!=plan.rules.end());
                const auto &result=plan.logical.nodes.at(narrowed->after);
                const auto &lateral=plan.logical.nodes.at(result.inputs.front());
                const auto &leftInput=plan.logical.nodes.at(lateral.inputs[0]);
                const auto &rightInput=plan.logical.nodes.at(lateral.inputs[1]);
                QCOMPARE(leftInput.outputs.size(),std::size_t(2));
                QCOMPARE(leftInput.outputs[1].name,std::string("ref"));
                QCOMPARE(rightInput.outputs.size(),std::size_t(1));
                QCOMPARE(rightInput.outputs[0].name,std::string("nested_ref"));
            }
            for (const auto root:{plan.logical.root,plan.heuristicRoot}) {
                const auto emitted=generatePlanSql(plan.logical,root);
                QVERIFY2(emitted.ok(),emitted.diagnostics.empty()?sql.c_str():emitted.diagnostics.front().message.c_str());
                QVERIFY(emitted.sql.find(" JOIN LATERAL ")!=std::string::npos);
                QVERIFY(emitted.sql.find("private_text")==std::string::npos);
                const auto actual=run(emitted.sql);
                QCOMPARE(actual.rows,expected.rows);
                QCOMPARE(actual.columns,expected.columns);
                QCOMPARE(actual.columnTypes,expected.columnTypes);
            }
        }
        run("INSERT INTO public.emit_lateral_right_owned "
            "SELECT i, i::bigint, 'spare' FROM generate_series(100,4096) i");
        const std::string sparse="SELECT l.id,q.v"+left+" LEFT JOIN LATERAL "+child+" ON TRUE ORDER BY 1";
        const auto parsed=compile(Dialect::PostgreSQL,sparse,{},&catalog,names);
        QVERIFY(parsed->ok());
        const auto prepared=prepareLocalQuery(parsed->model);
        QVERIFY(prepared.ok());
        InterpreterLimits limits;
        limits.maxRows=4;
        verifyLocal(executeFederatedQuery(*prepared.query,{},sources,token,limits),run(sparse));

        const auto transposed="SELECT l.id,q.v"+left+" LEFT JOIN LATERAL "+child+
            " ON TRUE WHERE l.id>=3 AND q.v IS NULL ORDER BY 1";
        const auto filtered=compile(Dialect::PostgreSQL,transposed,{},&catalog,names);
        QVERIFY(filtered->ok());
        const auto plan=optimize(filtered->model);
        QVERIFY(plan.ok());
        const auto rule=std::find_if(plan.rules.begin(),plan.rules.end(),[](const RuleEvent &entry) {
            return entry.rule=="HepFilterCorrelate";
        });
        QVERIFY(rule!=plan.rules.end());
        const auto &residual=plan.logical.nodes.at(rule->after);
        QCOMPARE(residual.kind,RelKind::Filter);
        QCOMPARE(residual.predicates.size(),std::size_t(1));
        const auto &lateral=plan.logical.nodes.at(residual.inputs.front());
        QCOMPARE(lateral.kind,RelKind::Correlate);
        QCOMPARE(plan.logical.nodes.at(lateral.inputs.front()).kind,RelKind::Filter);
        const auto emitted=generatePlanSql(plan.logical,plan.heuristicRoot);
        QVERIFY2(emitted.ok(),emitted.diagnostics.empty()?"no SQL":emitted.diagnostics.front().message.c_str());
        const auto original=run(transposed), rewritten=run(emitted.sql);
        QCOMPARE(rewritten.rows,original.rows);
        QCOMPARE(rewritten.columns,original.columns);
        QCOMPARE(rewritten.columnTypes,original.columnTypes);
        limits.maxRows=2;
        verifyLocal(executeFederatedQuery(filtered->model,sources,token,limits),original);

        for (const auto &unsafe:{
                 "SELECT l.id,q.v"+left+" LEFT JOIN LATERAL "+child+
                     " ON TRUE WHERE q.v IS NULL",
                 "SELECT l.id,q.v"+left+" LEFT JOIN LATERAL "
                     "(SELECT 100/l.ref AS v WHERE l.id=1) q ON TRUE WHERE l.id>=3"}) {
            const auto checked=compile(Dialect::PostgreSQL,unsafe,{},&catalog,names);
            QVERIFY(checked->ok());
            const auto untouched=optimize(checked->model);
            QVERIFY(untouched.ok());
            QVERIFY(std::none_of(untouched.rules.begin(),untouched.rules.end(),[](const RuleEvent &entry) {
                return entry.rule=="HepFilterCorrelate";
            }));
        }
    }catch(const std::exception &e){QFAIL(e.what());}}
    void relationalSubquerySqlPreservesMembershipAndNulls() {try {
        using namespace navidog;
        using namespace navidog::sql::compiler;
        namespace opt=navidog::sql::optimizer;
        auto db=connectDatabase(profile_);
        const auto token=cancellation();
        const auto run=[&](const std::string &sql) {
            const auto result=db->execute(QString::fromStdString(sql),{},token);
            if (!result.error.isEmpty()) throw std::runtime_error(result.error.toStdString());
            return result;
        };
        run("CREATE TABLE public.emit_members_left_owned(id integer NOT NULL,ref integer)");
        run("CREATE TABLE public.emit_members_right_owned(id integer)");
        run("CREATE TABLE public.emit_members_key_owned(id integer PRIMARY KEY)");
        run("INSERT INTO public.emit_members_left_owned VALUES (1,2),(1,2),(3,9),(4,NULL)");
        run("INSERT INTO public.emit_members_right_owned VALUES (2),(2),(NULL)");
        run("INSERT INTO public.emit_members_key_owned VALUES (1),(2)");
        const auto snapshot=std::make_shared<const MetadataSnapshot>(collectMetadata(*db,profile_,"public",token));
        const MetadataCatalog catalog(snapshot);
        const NameResolutionContext names{profile_.database.toStdString(),"public",Dialect::PostgreSQL};
        const auto planFor=[&](const std::string &sql) {
            const auto parsed=compile(Dialect::PostgreSQL,sql,{},&catalog,names);
            if (!parsed->ok()) throw std::runtime_error(parsed->diagnostics.front().message);
            const auto plan=opt::optimize(parsed->model);
            if (!plan.ok()) throw std::runtime_error(plan.diagnostics.front().message);
            return plan;
        };
        const std::string source="SELECT l.id,l.ref FROM public.emit_members_left_owned l WHERE ";
        const std::string order=" ORDER BY 1,2;";
        for (const auto &sql:{
                 std::string("SELECT (SELECT (SELECT 41)+1) AS answer, (SELECT 1 WHERE FALSE) AS missing;"),
                 source+"l.ref IN (SELECT r.id FROM public.emit_members_right_owned r)"+order,
                 source+"l.ref NOT IN (SELECT r.id FROM public.emit_members_right_owned r)"+order,
                 source+"l.id NOT IN (SELECT c.id FROM public.emit_members_key_owned c)"+order,
                 source+"EXISTS (SELECT 1 FROM public.emit_members_right_owned WHERE id=2)"+order}) {
            const auto plan=planFor(sql);
            const auto expected=run(sql);
            for (const auto root:{plan.logical.root,plan.heuristicRoot}) {
                const auto emitted=opt::generatePlanSql(plan.logical,root);
                QVERIFY2(emitted.ok(),emitted.diagnostics.empty()?sql.c_str():emitted.diagnostics.front().message.c_str());
                const auto actual=run(emitted.sql);
                QCOMPARE(actual.rows,expected.rows);
                QCOMPARE(actual.columns,expected.columns);
                QCOMPARE(actual.columnTypes,expected.columnTypes);
            }
        }
        for (const std::string negation:{"","NOT "}) {
            const auto sql=source+negation+
                "EXISTS (SELECT 1 FROM public.emit_members_right_owned r WHERE r.id=l.ref)"+order;
            const auto plan=planFor(sql);
            QVERIFY(plan.heuristicRoot!=plan.logical.root);
            const auto base=opt::generatePlanSql(plan.logical,plan.logical.root);
            QVERIFY2(base.ok(),base.diagnostics.empty()?sql.c_str():base.diagnostics.front().message.c_str());
            const auto emitted=opt::generatePlanSql(plan.logical,plan.heuristicRoot);
            QVERIFY2(emitted.ok(),emitted.diagnostics.empty()?sql.c_str():emitted.diagnostics.front().message.c_str());
            const auto expected=run(sql), actual=run(emitted.sql);
            const auto baseline=run(base.sql);
            QCOMPARE(baseline.rows,expected.rows);
            QCOMPARE(baseline.columns,expected.columns);
            QCOMPARE(baseline.columnTypes,expected.columnTypes);
            QCOMPARE(actual.rows,expected.rows);
            QCOMPARE(actual.columns,expected.columns);
            QCOMPARE(actual.columnTypes,expected.columnTypes);
        }
    }catch(const std::exception &e){QFAIL(e.what());}}
    void correlatedScalarQueriesUseCurrentRowsAndSqlTruth() {try {
        using namespace navidog;
        using namespace navidog::sql::compiler;
        using namespace navidog::sql::optimizer;
        auto db=connectDatabase(profile_);
        const auto token=cancellation();
        const auto run=[&](const std::string &sql) {
            const auto result=db->execute(QString::fromStdString(sql),{},token);
            if (!result.error.isEmpty()) throw std::runtime_error(result.error.toStdString());
            return result;
        };
        run("CREATE TABLE public.correlated_scalar_left_owned(id integer,ref integer)");
        run("CREATE TABLE public.correlated_scalar_right_owned(k integer,owner integer)");
        run("INSERT INTO public.correlated_scalar_left_owned VALUES (1,2),(2,3),(3,9),(4,NULL)");
        run("INSERT INTO public.correlated_scalar_right_owned VALUES "
            "(2,1),(2,1),(NULL,1),(9,2),(NULL,2),(NULL,4)");
        run("INSERT INTO public.correlated_scalar_right_owned "
            "SELECT i,i FROM generate_series(100,8192) i");
        const MetadataCatalog catalog(std::make_shared<const MetadataSnapshot>(
            collectMetadata(*db,profile_,"public",token)));
        const NameResolutionContext names{profile_.database.toStdString(),"public",Dialect::PostgreSQL};
        const std::vector<FederatedTableBinding> sources{
            {names.database,"public","correlated_scalar_left_owned",DatabaseKind::PostgreSQL,
             db.get(),"public","correlated_scalar_left_owned"},
            {names.database,"public","correlated_scalar_right_owned",DatabaseKind::PostgreSQL,
             db.get(),"public","correlated_scalar_right_owned"}};
        const std::string fromL=" FROM public.correlated_scalar_left_owned l";
        const std::string fromR=" FROM public.correlated_scalar_right_owned r WHERE r.owner=l.id";
        const auto compileQuery=[&](const std::string &sql) {
            auto compiled=compile(Dialect::PostgreSQL,sql,{},&catalog,names);
            if (!compiled->ok()) throw std::runtime_error(compiled->diagnostics.front().message);
            return compiled;
        };
        InterpreterLimits limits;
        limits.maxRows=4;
        const auto verify=[&](const std::string &sql) {
            const auto parsed=compileQuery(sql);
            const auto result=executeFederatedQuery(parsed->model,sources,token,limits);
            QVERIFY2(result.ok(),result.diagnostics.empty()?sql.c_str():result.diagnostics.front().message.c_str());
            const auto plan=optimize(parsed->model);
            QVERIFY(plan.ok());
            const auto generated=generatePlanSql(plan.logical,plan.logical.root);
            QVERIFY2(generated.ok(),generated.diagnostics.empty()?sql.c_str():generated.diagnostics.front().message.c_str());
            const auto expected=run(sql);
            const auto rendered=run(generated.sql);
            QCOMPARE(rendered.rows,expected.rows);
            QCOMPARE(rendered.columns,expected.columns);
            QCOMPARE(rendered.columnTypes,expected.columnTypes);
            std::vector<LocalRow> rows;
            for (const auto &row:expected.rows) {
                LocalRow values;
                for (qsizetype i=0;i<row.size();++i) {
                    if (row[i].isNull()) values.emplace_back();
                    else if (expected.columnTypes[i]=="boolean") values.emplace_back(row[i].toString()=="t");
                    else values.emplace_back(std::int64_t(row[i].toLongLong()));
                }
                rows.push_back(std::move(values));
            }
            QVERIFY(rows==result.rows);
        };
        verify("SELECT l.id,(SELECT COUNT(*)"+fromR+") AS n,"
               "(SELECT (SELECT l.ref+1)) AS layered,"
               "EXISTS (SELECT 1"+fromR+" AND r.k IS NOT NULL) AS found"+fromL+" ORDER BY 1");
        verify("SELECT l.id,l.ref IN (SELECT r.k"+fromR+") AS hit,"
               "l.ref NOT IN (SELECT r.k"+fromR+") AS miss,"
               "l.ref=ANY (SELECT r.k"+fromR+") AS any_hit,"
               "l.ref<>ALL (SELECT r.k"+fromR+") AS all_miss"+fromL+" ORDER BY 1");
        verify("SELECT l.id,(SELECT (SELECT r.k+l.ref)"+fromR+
               " AND r.k=9) AS two_scopes"+fromL+" ORDER BY 1");
        const auto duplicate=compileQuery("SELECT l.id,(SELECT r.k"+fromR+") AS m"+fromL);
        const auto rejected=executeFederatedQuery(duplicate->model,sources,token,limits);
        QVERIFY(!rejected.ok() && rejected.rows.empty());
        QCOMPARE(rejected.diagnostics.front().code,DiagnosticCode::InvalidQueryShape);
    }catch(const std::exception &e){QFAIL(e.what());}}
    void logicalPlanSqlMatchesOwnedPostgres() {try {
        using namespace navidog;
        using namespace navidog::sql::compiler;
        namespace opt=navidog::sql::optimizer;
        auto db=connectDatabase(profile_);
        const auto token=cancellation();
        const auto run=[&](const std::string &sql) {
            const auto result=db->execute(QString::fromStdString(sql),{},token);
            if (!result.error.isEmpty()) throw std::runtime_error(result.error.toStdString());
            return result;
        };
        run("CREATE TABLE public.emit_left_owned(id integer PRIMARY KEY,score integer,ref integer,private_text text)");
        run("CREATE TABLE public.emit_right_owned(id integer PRIMARY KEY,payload bigint,private_text text)");
        run("INSERT INTO public.emit_left_owned VALUES "
            "(1,9,1,'p'),(2,9,1,'p'),(3,NULL,2,'p'),(4,7,7,'p'),(5,7,NULL,'p'),(6,7,3,'p')");
        run("INSERT INTO public.emit_right_owned VALUES (1,11000000000,'s'),(2,22,'s'),(3,NULL,'s')");
        run("CREATE ROLE emit_plan_reader");
        run("GRANT SELECT (id,score,ref) ON public.emit_left_owned TO emit_plan_reader");
        run("GRANT SELECT (id,payload) ON public.emit_right_owned TO emit_plan_reader");
        const auto snapshot=collectMetadata(*db,profile_,"public",token);
        const MetadataCatalog catalog(std::make_shared<const MetadataSnapshot>(snapshot));
        const NameResolutionContext names{profile_.database.toStdString(),"public",Dialect::PostgreSQL};
        const auto planFor=[&](const std::string &text) {
            const auto compiled=compile(Dialect::PostgreSQL,text,{},&catalog,names);
            if (!compiled->ok()) throw std::runtime_error(compiled->diagnostics.front().message);
            auto plan=opt::optimize(compiled->model);
            if (!plan.ok()) throw std::runtime_error(plan.diagnostics.front().message);
            return plan;
        };
        run("SET ROLE emit_plan_reader");
        for (const auto *text:{
                 "SELECT 10000000000 AS \"序号\", NULL AS absent, '中文' AS msg;",
                 "VALUES (1,2),(2,NULL),(2,NULL);",
                 "SELECT id AS \"ref\",CASE WHEN score IS NULL THEN 100 ELSE score END AS rank "
                 "FROM public.emit_left_owned WHERE ref IN (1,2,7) ORDER BY rank DESC,1;",
                 "SELECT l.score AS rank,l.id AS seq,r.payload AS value FROM public.emit_left_owned l "
                 "LEFT JOIN public.emit_right_owned r ON l.ref=r.id ORDER BY rank DESC NULLS LAST,2 LIMIT 3 OFFSET 1;",
                 "SELECT l.score,l.id,r.payload FROM public.emit_right_owned r RIGHT JOIN public.emit_left_owned l "
                 "ON l.ref=r.id ORDER BY l.score DESC NULLS FIRST,l.id LIMIT 2 OFFSET 1;",
                 "SELECT score,COUNT(*) FILTER (WHERE ref IS NOT NULL) AS n,MAX(id) AS last_id "
                 "FROM public.emit_left_owned GROUP BY score HAVING COUNT(*)>1 ORDER BY score;",
                 "SELECT COUNT(*) AS n,MIN(score) AS lo FROM public.emit_left_owned WHERE id=99;",
                 "SELECT d.key FROM (SELECT id AS key,score FROM public.emit_left_owned) AS d ORDER BY d.key;",
                 "SELECT * FROM (SELECT id,score FROM public.emit_left_owned) AS d ORDER BY 1;",
                 "SELECT d.key FROM (SELECT DISTINCT a.key,a.payload FROM "
                 "(VALUES (1,2),(1,3),(1,2),(2,NULL)) AS a(key,payload)) AS d ORDER BY 1;",
                 "SELECT r.key FROM (SELECT id AS key,score AS payload FROM public.emit_left_owned "
                 "UNION ALL SELECT id,payload FROM public.emit_right_owned) AS r ORDER BY 1;",
                 "SELECT r.key FROM ((VALUES (1,2),(1,3),(1,2),(2,NULL)) "
                 "INTERSECT ALL (VALUES (1,2),(1,3),(2,NULL))) AS r(key,payload) ORDER BY 1;",
                 "SELECT r.key FROM ((VALUES (1,2),(1,3),(1,2),(2,NULL)) "
                 "EXCEPT ALL (VALUES (1,2),(2,NULL))) AS r(key,payload) ORDER BY 1;",
                 "WITH source_cte AS MATERIALIZED (SELECT id,score,ref FROM public.emit_left_owned),"
                 "links AS NOT MATERIALIZED (SELECT id,ref FROM source_cte WHERE ref IS NOT NULL) "
                 "SELECT s.id,s.score,t.ref FROM source_cte s JOIN links t ON s.id=t.id ORDER BY s.id;",
                 "WITH ids(k) AS (VALUES (3),(2),(2)), step_cte AS (SELECT k FROM ids WHERE k>1) "
                 "SELECT l.k,r.k AS peer FROM step_cte l JOIN ids r ON l.k=r.k ORDER BY 1,2;",
                 "WITH RECURSIVE seq(n) AS (SELECT 1 UNION ALL SELECT n+1 FROM seq WHERE n<4) "
                 "SELECT n AS num FROM seq ORDER BY 1;",
                 "WITH RECURSIVE seq(n,k) AS (VALUES (1,7),(1,NULL),(1,NULL) "
                 "UNION SELECT 3-n,k FROM seq) SELECT n,k FROM seq ORDER BY 1,2 NULLS LAST;",
                 "WITH RECURSIVE seq(n) AS (SELECT k FROM seed_data UNION ALL "
                 "SELECT n+1 FROM seq WHERE n<3), seed_data(k) AS (VALUES (1)), "
                 "tails(k) AS (SELECT n FROM seq WHERE n>1) SELECT s.n,t.k FROM seq s "
                 "JOIN tails t ON s.n=t.k ORDER BY 1;",
                 "WITH RECURSIVE seq(n) AS (SELECT 1 AS n WHERE FALSE UNION ALL "
                 "SELECT n+1 FROM seq WHERE n<4) SELECT COUNT(*) AS n FROM seq;",
                 "WITH RECURSIVE seq(n) AS (SELECT 1 UNION ALL "
                 "SELECT s.n+1 FROM seq s JOIN public.emit_left_owned e ON e.id=s.n WHERE s.n<3) "
                 "SELECT n FROM seq ORDER BY 1;"}) {
            const auto plan=planFor(text);
            const auto expected=run(text);
            for (auto root:{plan.logical.root,plan.heuristicRoot}) {
                const auto emitted=opt::generatePlanSql(plan.logical,root);
                QVERIFY2(emitted.ok(),emitted.diagnostics.empty()?"empty SQL":emitted.diagnostics.front().message.c_str());
                QVERIFY(emitted.sql.find("private_text")==std::string::npos);
                if (std::string_view(text).find("AS MATERIALIZED")!=std::string_view::npos) {
                    QVERIFY(emitted.sql.find("AS MATERIALIZED")!=std::string::npos);
                    QVERIFY(emitted.sql.find("AS NOT MATERIALIZED")!=std::string::npos);
                }
                const auto reparsed=compile(Dialect::PostgreSQL,emitted.sql,{},&catalog,names);
                QVERIFY2(reparsed->ok(),reparsed->diagnostics.empty()?"no parse":reparsed->diagnostics.front().message.c_str());
                const auto actual=run(emitted.sql);
                QCOMPARE(actual.rows,expected.rows);
                QCOMPARE(actual.columns,expected.columns);
                QCOMPARE(actual.columnTypes,expected.columnTypes);
            }
        }
        run("RESET ROLE");
        const auto unknown=planFor("SELECT NULL AS v UNION ALL SELECT 1 AS v;");
        const auto unsafeUnion=opt::generatePlanSql(unknown.logical,unknown.logical.root);
        QVERIFY(!unsafeUnion.ok() && unsafeUnion.sql.empty());
        const auto aggregate=planFor("SELECT STDDEV(score) AS n FROM public.emit_left_owned;");
        const auto unsupported=opt::generatePlanSql(aggregate.logical,aggregate.logical.root);
        QVERIFY(!unsupported.ok() && unsupported.sql.empty());
        const auto plan=planFor("SELECT id FROM public.emit_left_owned ORDER BY id LIMIT 1;");
        GenerationOptions bounds;
        bounds.maxBytes=30;
        const auto oversized=opt::generatePlanSql(plan.logical,plan.logical.root,bounds);
        QVERIFY(!oversized.ok() && oversized.sql.empty());
        QCOMPARE(oversized.diagnostics.front().code,DiagnosticCode::ResourceLimit);
        bounds=GenerationOptions{};
        bounds.shouldStop=[] {return true;};
        const auto stopped=opt::generatePlanSql(plan.logical,plan.logical.root,bounds);
        QVERIFY(!stopped.ok() && stopped.sql.empty());
        QCOMPARE(stopped.diagnostics.front().code,DiagnosticCode::Stopped);
    }catch(const std::exception &e){QFAIL(e.what());}}
    void projectedTableScanKeepsOrdinalLayoutAndHeapCost() {try {
        using namespace navidog;
        using namespace navidog::sql::compiler;
        namespace opt=navidog::sql::optimizer;
        auto db=connectDatabase(profile_);
        const auto token=cancellation();
        const auto run=[&](const std::string &sql) {
            const auto result=db->execute(QString::fromStdString(sql),{},token);
            if (!result.error.isEmpty()) throw std::runtime_error(result.error.toStdString());
            return result;
        };
        run("CREATE TABLE public.scan_columns_owned(k integer,wide text,v bigint,payload text)");
        run("INSERT INTO public.scan_columns_owned VALUES (1,'long',42,'p'),(2,'long',NULL,'p')");
        const auto snapshot=std::make_shared<const MetadataSnapshot>(collectMetadata(*db,profile_,"public",token));
        const MetadataCatalog catalog(snapshot);
        const NameResolutionContext names{profile_.database.toStdString(),"public",Dialect::PostgreSQL};
        opt::MemoryStatistics stats;
        opt::TableStatistics estimate;
        estimate.database=names.database;
        estimate.schema="public";
        estimate.name="scan_columns_owned";
        estimate.rows=1000;
        estimate.averageRowBytes=256;
        estimate.columns={{"k",1000,0,4},{"v",1000,0,8}};
        stats.tables={estimate};
        const std::string sql="SELECT v AS value,k AS key,v AS copy_value FROM public.scan_columns_owned;";
        const auto parsed=compile(Dialect::PostgreSQL,sql,{},&catalog,names);
        QVERIFY(parsed->ok());
        const auto plan=opt::optimize(parsed->model,&stats);
        QVERIFY2(plan.ok(),plan.diagnostics.empty()?"no plan":plan.diagnostics.front().message.c_str());
        const auto &fused=plan.best.nodes.at(plan.best.root);
        QCOMPARE(fused.kind,opt::PhysicalKind::TableScan);
        QCOMPARE(fused.scanOrdinals,(std::vector<std::size_t>{2,0,2}));
        QCOMPARE(fused.rowBytes,20.0);
        const auto raw=std::find_if(plan.best.nodes.begin(),plan.best.nodes.end(),[&](const auto &node) {
            return node.kind==opt::PhysicalKind::TableScan && node.object==fused.object && node.scanOrdinals.empty();
        });
        QVERIFY(raw!=plan.best.nodes.end());
        QCOMPARE(raw->rowBytes,256.0);
        QCOMPARE(fused.self.io,raw->self.io);
        QVERIFY(opt::dumpOptimization(plan).find("\"scanOrdinals\":[2,0,2]")!=std::string::npos);
        const auto sqlPlan=opt::generatePlanSql(plan.logical,fused.logical);
        QVERIFY(sqlPlan.ok());
        QVERIFY(sqlPlan.sql.find("\"wide\"")==std::string::npos);
        const auto native=run(sql), emitted=run(sqlPlan.sql);
        QCOMPARE(emitted.rows,native.rows);
        QCOMPARE(emitted.columns,native.columns);
        QCOMPARE(emitted.columnTypes,native.columnTypes);
        const auto computed=compile(Dialect::PostgreSQL,
            "SELECT v+1 AS next_value FROM public.scan_columns_owned;",{},&catalog,names);
        QVERIFY(computed->ok());
        const auto scalar=opt::optimize(computed->model,&stats);
        QVERIFY(scalar.ok());
        QCOMPARE(scalar.best.nodes.at(scalar.best.root).kind,opt::PhysicalKind::Project);
        QVERIFY(scalar.best.nodes.at(scalar.best.root).scanOrdinals.empty());
        stats.tables.front().indexes.push_back({"scan_columns_key_idx",{"k"},false,true});
        for (const auto &item:std::vector<std::pair<std::string,std::string>>{
                 {"SELECT v+1 AS value FROM public.scan_columns_owned WHERE k=1 ORDER BY 1",
                  "HepProjectFilterTranspose"},
                 {"SELECT v AS value,RANK() OVER (PARTITION BY k ORDER BY v NULLS FIRST) AS pos "
                  "FROM public.scan_columns_owned ORDER BY 1","HepProjectWindowTranspose"}}) {
            const auto source=compile(Dialect::PostgreSQL,item.first,{},&catalog,names);
            QVERIFY(source->ok());
            const auto reduced=opt::optimize(source->model,&stats);
            QVERIFY(reduced.ok());
            const auto event=std::find_if(reduced.rules.begin(),reduced.rules.end(),
                [&](const auto &rule){return rule.rule==item.second;});
            QVERIFY(event!=reduced.rules.end());
            const auto &outer=reduced.logical.nodes.at(event->after);
            const auto &middle=reduced.logical.nodes.at(outer.inputs.front());
            const auto &narrow=reduced.logical.nodes.at(middle.inputs.front());
            QCOMPARE(narrow.kind,opt::RelKind::Project);
            QCOMPARE(narrow.outputs.size(),std::size_t(2));
            QCOMPARE(narrow.outputs[0].name,std::string("k"));
            QCOMPARE(narrow.outputs[1].name,std::string("v"));
            if (middle.kind==opt::RelKind::Filter)
                QVERIFY(std::any_of(reduced.best.nodes.begin(),reduced.best.nodes.end(),[](const auto &candidate) {
                    return candidate.kind==opt::PhysicalKind::IndexScan && !candidate.predicates.empty() &&
                           candidate.scanOrdinals==std::vector<std::size_t>{0,2};
                }));
            const auto generated=opt::generatePlanSql(reduced.logical,reduced.heuristicRoot);
            QVERIFY2(generated.ok(),generated.diagnostics.empty()?item.first.c_str():generated.diagnostics.front().message.c_str());
            const auto expected=run(item.first), actual=run(generated.sql);
            QCOMPARE(actual.rows,expected.rows);
            QCOMPARE(actual.columns,expected.columns);
            QCOMPARE(actual.columnTypes,expected.columnTypes);
        }
    }catch(const std::exception &e){QFAIL(e.what());}}
    void joinRuleAlternativesPreserveRowsAndOrder() {try {
        using namespace navidog;
        using namespace navidog::sql::compiler;
        using namespace navidog::sql::optimizer;
        auto db=connectDatabase(profile_);
        const auto token=cancellation();
        const auto run=[&](const std::string &sql) {
            const auto result=db->execute(QString::fromStdString(sql),{},token);
            if (!result.error.isEmpty()) throw std::runtime_error(result.error.toStdString());
            return result;
        };
        run("CREATE TABLE public.join_rule_left_owned(k int,v bigint)");
        run("CREATE TABLE public.join_rule_right_owned(k int,v bigint)");
        run("CREATE TABLE public.join_rule_middle_owned(k int PRIMARY KEY, detail int)");
        run("INSERT INTO public.join_rule_left_owned VALUES (NULL,1),(1,2),(1,2),(2,3),(3,4)");
        run("INSERT INTO public.join_rule_right_owned VALUES "
            "(1,NULL),(1,7),(2,8),(2,9),(NULL,10),(3,4),(NULL,1),(1,2)");
        run("INSERT INTO public.join_rule_middle_owned VALUES (1,1),(3,2)");
        const auto snapshot=std::make_shared<const MetadataSnapshot>(collectMetadata(*db,profile_,"public",token));
        const MetadataCatalog catalog(snapshot);
        const NameResolutionContext names{profile_.database.toStdString(),"public",Dialect::PostgreSQL};
        const auto plan=[&](const std::string &sql) {
            const auto parsed=compile(Dialect::PostgreSQL,sql,{},&catalog,names);
            if (!parsed->ok()) throw std::runtime_error(parsed->diagnostics.front().message);
            auto result=optimize(parsed->model);
            if (!result.ok()) throw std::runtime_error(result.diagnostics.front().message);
            return result;
        };
        const std::string select="SELECT a.k,l.v AS right_value FROM public.join_rule_left_owned a ";
        const std::string tail=" JOIN public.join_rule_right_owned l ON a.k=l.k "
                               "ORDER BY a.k DESC NULLS FIRST,right_value NULLS LAST";
        const auto verify=[&](const std::string &source,const std::string &rule,std::string_view expectedSql) {
            auto alternative=plan(source);
            const auto candidate=std::find_if(alternative.rules.begin(),alternative.rules.end(),
                [&](const auto &event){return event.rule==rule;});
            QVERIFY2(candidate!=alternative.rules.end(),source.c_str());
            for (auto &node:alternative.logical.nodes)
                std::replace(node.inputs.begin(),node.inputs.end(),candidate->before,candidate->after);
            const auto emitted=generatePlanSql(alternative.logical,alternative.logical.root);
            QVERIFY2(emitted.ok(),emitted.diagnostics.empty()?source.c_str():emitted.diagnostics.front().message.c_str());
            QVERIFY(emitted.sql.find(expectedSql)!=std::string::npos);
            if (rule=="HepProjectJoinJoinRemove" || rule=="HepAggregateJoinJoinRemove")
                QVERIFY(emitted.sql.find("join_rule_middle_owned")==std::string::npos);
            const auto before=run(source),after=run(emitted.sql);
            QCOMPARE(after.rows,before.rows);
            QCOMPARE(after.columns,before.columns);
            QCOMPARE(after.columnTypes,before.columnTypes);
            if (rule=="SortJoinCopy") {
                const auto &sort=alternative.logical.nodes.at(candidate->after);
                const auto &input=alternative.logical.nodes.at(sort.inputs.front());
                const auto &join=input.kind==RelKind::Project
                    ? alternative.logical.nodes.at(input.inputs.front()) : input;
                QCOMPARE(sort.kind,RelKind::Sort);
                QCOMPARE(join.kind,RelKind::Join);
                for (auto child:join.inputs) {
                    const auto &local=alternative.logical.nodes.at(child);
                    QCOMPARE(local.kind,RelKind::Sort);
                    QCOMPARE(local.keys.size(),std::size_t(1));
                    QVERIFY(!local.rowLimit && !local.rowOffset);
                }
            }
        };
        verify(select+"INNER"+tail,"JoinExtractFilter","CROSS JOIN");
        verify(select+"LEFT"+tail+" LIMIT 5 OFFSET 1","SortJoinCopy","NULLS FIRST");
        verify(select+"INNER"+tail,"JoinToCorrelate","JOIN LATERAL");
        verify(select+"LEFT"+tail,"JoinToCorrelate","LEFT JOIN LATERAL");
        verify(select+"INNER JOIN public.join_rule_right_owned l ON CAST(a.k AS bigint)=l.v "
               "ORDER BY 1,2 NULLS FIRST","JoinPushExpressions","CAST(");
        verify("SELECT a.k,l.v FROM public.join_rule_left_owned a CROSS JOIN LATERAL "
               "(SELECT rhs.v FROM public.join_rule_right_owned rhs WHERE a.v=CAST(rhs.k AS bigint)) l "
               "ORDER BY 1,2 NULLS FIRST","FilterFlattenCorrelatedCondition","CAST(");
        verify(select+"FULL"+tail,"FullToLeftAndRightJoin"," UNION ALL ");
        const std::string both=" JOIN public.join_rule_right_owned l ";
        const std::string order=" ORDER BY 1 NULLS FIRST,2 NULLS FIRST";
        verify(select+"INNER"+both+"ON a.k=l.k OR a.v=l.v"+order,
               "JoinExpandOrToUnion","IS NOT TRUE");
        verify(select+"LEFT"+both+"ON a.k IS NOT DISTINCT FROM l.k"+order,
               "JoinConditionExpandIsNotDistinctFrom","IS TRUE");
        verify(select+"CROSS"+both+"WHERE a.k IS NOT DISTINCT FROM l.k"+order,
               "FilterRemoveIsNotDistinctFrom","IS TRUE");
        verify(select+"INNER"+both+"ON a.k=l.k "
               "WHERE (a.k>=1 AND l.v>=2) OR (a.k IS NULL AND l.k>=1)"+order,
               "ExpandDisjunctionForTable"," OR ");
        verify(select+"INNER"+both+"ON a.k=l.k "
               "JOIN public.join_rule_right_owned m ON l.k=m.k "
               "WHERE (a.k>=1 AND m.v>=2) OR (l.v=4 AND m.k>0)"+order,
               "ExpandDisjunctionForJoinInputs"," OR ");
        const auto outer=plan(select+"LEFT"+tail);
        QVERIFY(std::none_of(outer.rules.begin(),outer.rules.end(),[](const auto &event) {
            return event.rule=="JoinExtractFilter";
        }));
        verify("SELECT a.key,l.v FROM (SELECT k AS key FROM public.join_rule_left_owned) a "
               "JOIN public.join_rule_right_owned l ON a.key=l.k"+order,
               "JoinProjectTranspose"," INNER JOIN ");
        verify(select+"CROSS JOIN public.join_rule_middle_owned mid "
               "JOIN public.join_rule_right_owned l ON a.k=l.k AND mid.k=l.k"+order,
               "JoinPushThroughJoin"," INNER JOIN ");
        const std::string chain="FROM public.join_rule_left_owned a "
                                "LEFT JOIN public.join_rule_middle_owned mid ON a.k=mid.k "
                                "LEFT JOIN public.join_rule_right_owned l ON a.k=l.k";
        verify("SELECT a.k,l.v "+chain+order,"HepProjectJoinJoinRemove"," LEFT JOIN ");
        verify("SELECT a.k,COUNT(*) AS n "+chain+" GROUP BY a.k"+order,"HepAggregateJoinJoinRemove","GROUP BY ");
        const std::string uniqueJoin="FROM public.join_rule_left_owned a "
            "INNER JOIN public.join_rule_middle_owned mid ON a.k=mid.k";
        verify("SELECT a.k "+uniqueJoin+" ORDER BY 1 NULLS FIRST","SemiJoin","EXISTS (SELECT 1");
        verify("SELECT a.k,mid.detail "+uniqueJoin+order,"JoinAddRedundantSemiJoin","EXISTS (SELECT 1");
        const auto narrow=plan("SELECT a.k "+uniqueJoin+" ORDER BY 1 NULLS FIRST");
        QVERIFY(std::any_of(narrow.rules.begin(),narrow.rules.end(),[](const auto &event) {
            return event.rule=="SemiJoinProjectTranspose";
        }));
        const auto joined=plan("SELECT a.k,l.v FROM public.join_rule_left_owned a "
            "JOIN public.join_rule_right_owned l ON a.k=l.k "
            "JOIN public.join_rule_middle_owned mid ON a.k=mid.k"+order);
        QVERIFY(std::any_of(joined.rules.begin(),joined.rules.end(),[](const auto &event) {
            return event.rule=="SemiJoinJoinTranspose";
        }));
        for (const auto &test:std::array<std::string,2>{"IS TRUE","IS NOT TRUE"})
            verify("SELECT a.k FROM public.join_rule_left_owned a "
                   "WHERE (a.k IN (SELECT k FROM public.join_rule_right_owned)) "+test+
                   " ORDER BY 1 NULLS FIRST","HepMarkToSemiOrAntiJoin","EXISTS (SELECT 1");
        const auto referenced=plan("SELECT a.k,mid.detail "+chain+order);
        QVERIFY(std::none_of(referenced.rules.begin(),referenced.rules.end(),[](const auto &event) {
            return event.rule=="HepProjectJoinJoinRemove";
        }));
        run("DROP TABLE public.join_rule_left_owned,public.join_rule_right_owned,public.join_rule_middle_owned");
    }catch(const std::exception &e){QFAIL(e.what());}}
    void multiJoinSearchKeepsHyperedgePredicatesAndOutputIdentity() {try {
        using namespace navidog;
        using namespace navidog::sql::compiler;
        using namespace navidog::sql::optimizer;
        auto db=connectDatabase(profile_);
        const auto token=cancellation();
        const auto run=[&](const std::string &sql) {
            const auto result=db->execute(QString::fromStdString(sql),{},token);
            if (!result.error.isEmpty()) throw std::runtime_error(result.error.toStdString());
            return result;
        };
        const std::array<std::string,4> tables{"g1_owned","g2_owned","g3_owned","g4_owned"};
        for (std::size_t i=0; i<tables.size(); ++i) {
            run("CREATE TABLE public."+tables[i]+"(k int,label bigint,payload text)");
            run("INSERT INTO public."+tables[i]+"(k,label) SELECT k,label+"+
                std::to_string(10*i)+" FROM (VALUES(NULL,1),(1,2),(1,2),(2,4),(3,5)) v(k,label)");
        }
        const auto snapshot=std::make_shared<const MetadataSnapshot>(collectMetadata(*db,profile_,"public",token));
        const MetadataCatalog catalog(snapshot);
        const NameResolutionContext names{profile_.database.toStdString(),"public",Dialect::PostgreSQL};
        MemoryStatistics stats;
        for (std::size_t i=0; i<tables.size(); ++i) {
            TableStatistics estimate;
            estimate.database=names.database;
            estimate.schema=names.schema;
            estimate.name=tables[i];
            estimate.rows=std::array{200000.,80000.,50.,5.}[i];
            estimate.columns={{"k",estimate.rows,.1,4.},{"label",estimate.rows,.0,8.}};
            stats.tables.push_back(std::move(estimate));
        }
        const std::string query="SELECT a.label AS a_val,b.label AS b_val,c.label AS c_val,d.label AS d_val "
            "FROM public.g1_owned a JOIN public.g2_owned b ON a.k=b.k "
            "JOIN public.g3_owned c ON (c.k=a.k OR c.k=b.k) "
            "JOIN public.g4_owned d ON c.k=d.k WHERE d.k>=2 "
            "ORDER BY 1,2,3,4";
        const auto compiled=compile(names.dialect,query,{},&catalog,names);
        QVERIFY2(compiled->ok(),compiled->diagnostics.empty()?"compile":compiled->diagnostics.front().message.c_str());
        const auto hasGraph=[](const auto &event) {
            return event.rule=="DphypJoinReorder" || event.rule=="MultiJoinLeftDeep" ||
                   event.rule=="MultiJoinProjectTranspose";
        };
        const auto result=optimize(compiled->model,&stats);
        QVERIFY2(result.ok(),result.diagnostics.empty()?"no plan":result.diagnostics.front().message.c_str());
        QVERIFY(std::any_of(result.rules.begin(),result.rules.end(),hasGraph));
        const auto baseline=run(query);
        QVERIFY(!baseline.rows.empty());
        bool mergedFilter=false, trimmedInputs=false;
        for (const auto &event:result.rules) {
            if (!hasGraph(event)) continue;
            auto alternative=result.logical;
            for (auto &node:alternative.nodes)
                std::replace(node.inputs.begin(),node.inputs.end(),event.before,event.after);
            const auto rendered=generatePlanSql(alternative,alternative.root);
            QVERIFY2(rendered.ok(),rendered.diagnostics.empty()?"no SQL":rendered.diagnostics.front().message.c_str());
            QVERIFY(rendered.sql.find(" OR ")!=std::string::npos);
            const auto actual=run(rendered.sql);
            QCOMPARE(actual.rows,baseline.rows);
            QCOMPARE(actual.columns,baseline.columns);
            QCOMPARE(actual.columnTypes,baseline.columnTypes);
            mergedFilter|=result.logical.nodes.at(event.before).kind==RelKind::Filter;
            if (event.rule=="MultiJoinProjectTranspose") {
                trimmedInputs=true;
                QVERIFY(rendered.sql.find("payload")==std::string::npos);
            }
        }
        QVERIFY(mergedFilter);
        QVERIFY(trimmedInputs);
        PlannerOptions disabled;
        disabled.joinAssociation=false;
        const auto original=optimize(compiled->model,&stats,disabled);
        QVERIFY(original.ok() && std::none_of(original.rules.begin(),original.rules.end(),hasGraph));
        run("DELETE FROM public.g2_owned WHERE k=3");
        const std::string outerQuery="SELECT a.label,b.label,c.label,d.label "
            "FROM public.g1_owned a LEFT JOIN public.g2_owned b ON a.k=b.k "
            "JOIN public.g3_owned c ON a.k=c.k JOIN public.g4_owned d ON c.k=d.k "
            "ORDER BY 1,2 NULLS FIRST,3,4";
        const auto nested=compile(names.dialect,outerQuery,{},&catalog,names);
        QVERIFY(nested->ok());
        auto preserved=optimize(nested->model,&stats);
        QVERIFY(preserved.ok());
        const auto edge=std::find_if(preserved.rules.begin(),preserved.rules.end(),[&](const auto &event) {
            return hasGraph(event) && event.reason.find("外连接作为一个完整输入")!=std::string::npos;
        });
        QVERIFY(edge!=preserved.rules.end());
        for (auto &node:preserved.logical.nodes)
            std::replace(node.inputs.begin(),node.inputs.end(),edge->before,edge->after);
        const auto rewritten=generatePlanSql(preserved.logical,preserved.logical.root);
        QVERIFY2(rewritten.ok(),rewritten.diagnostics.empty()?"no SQL":rewritten.diagnostics.front().message.c_str());
        QVERIFY(rewritten.sql.find("LEFT JOIN")!=std::string::npos);
        const auto outerRows=run(outerQuery);
        QCOMPARE(run(rewritten.sql).rows,outerRows.rows);
        QVERIFY(std::any_of(outerRows.rows.begin(),outerRows.rows.end(),[](const auto &row){return row[1].isNull();}));
        run("DROP TABLE public.g1_owned,public.g2_owned,public.g3_owned,public.g4_owned");
    }catch(const std::exception &e){QFAIL(e.what());}}
    void memoMergeJoinUsesVerifiedOrderedInputs() {try {
        using namespace navidog;
        using namespace navidog::sql::compiler;
        namespace opt=navidog::sql::optimizer;
        auto db=connectDatabase(profile_);
        const auto token=cancellation();
        const auto run=[&](const std::string &sql) {
            const auto result=db->execute(QString::fromStdString(sql),{},token);
            if (!result.error.isEmpty()) throw std::runtime_error(result.error.toStdString());
            return result;
        };
        run("CREATE TABLE public.merge_left_owned(bucket int,id bigint)");
        run("CREATE TABLE public.merge_right_owned(bucket int,rid bigint)");
        run("CREATE INDEX merge_left_keys ON public.merge_left_owned(bucket,id)");
        run("CREATE INDEX merge_right_keys ON public.merge_right_owned(bucket,rid)");
        run("INSERT INTO public.merge_left_owned VALUES (1,2),(1,2),(1,3),(2,NULL),(NULL,2)");
        run("INSERT INTO public.merge_right_owned VALUES (1,2),(1,2),(2,3),(NULL,2)");
        const auto snapshot=std::make_shared<const MetadataSnapshot>(collectMetadata(*db,profile_,"public",token));
        const MetadataCatalog catalog(snapshot);
        const NameResolutionContext names{profile_.database.toStdString(),"public",Dialect::PostgreSQL};
        opt::MemoryStatistics stats;
        opt::TableStatistics entry;
        entry.database=names.database;
        entry.schema="public";
        entry.name="merge_left_owned";
        entry.rows=50000;
        entry.averageRowBytes=16;
        entry.columns={{"bucket",500,0.03,4},{"id",7500,0.02,8}};
        entry.indexes={{"merge_left_keys",{"bucket","id"},false,true,2}};
        stats.tables.push_back(entry);
        entry.name="merge_right_owned";
        entry.columns[1].name="rid";
        entry.indexes={{"merge_right_keys",{"bucket","rid"},false,true,2}};
        stats.tables.push_back(entry);
        opt::PlannerOptions options;
        options.weights.memory=2;
        options.heuristic.enabled=false;
        const auto planned=[&](const std::string &sql) {
            const auto parsed=compile(Dialect::PostgreSQL,sql,{},&catalog,names);
            if (!parsed->ok()) throw std::runtime_error(parsed->diagnostics.front().message);
            const auto plan=opt::optimize(parsed->model,&stats,options);
            if (!plan.ok()) throw std::runtime_error(plan.diagnostics.front().message);
            return plan;
        };
        const auto merge=[&](const opt::PhysicalPlan &plan) -> const opt::PhysicalNode * {
            std::vector<opt::PlanId> work{plan.root};
            while (!work.empty()) {
                const auto id=work.back();
                work.pop_back();
                const auto &node=plan.nodes.at(id);
                if (node.kind==opt::PhysicalKind::MergeJoin) return &node;
                work.insert(work.end(),node.inputs.begin(),node.inputs.end());
            }
            return nullptr;
        };
        const std::string lhs="SELECT l.bucket AS a,l.id,r.bucket AS b,r.rid FROM public.merge_left_owned l ";
        const std::string rhs=" JOIN public.merge_right_owned r ON l.id=r.rid AND l.bucket=r.bucket";
        for (const auto &item:{std::pair<std::string,bool>{"INNER",false},{"LEFT",false},{"RIGHT",true}}) {
            const auto sql=lhs+item.first+rhs+(item.second?" ORDER BY b,rid;":" ORDER BY a,id;");
            const auto plan=planned(sql);
            const auto *node=merge(plan.best);
            QVERIFY2(node,opt::dumpOptimization(plan).c_str());
            QCOMPARE(node->ordering.size(),std::size_t(2));
            QCOMPARE(node->ordering[0].name,std::string("bucket"));
            QCOMPARE(node->ordering[1].name,item.second?std::string("rid"):std::string("id"));
            QVERIFY(node->self.memory<1024);
            for (const auto child:node->inputs) {
                const auto &input=plan.best.nodes.at(child);
                QCOMPARE(input.kind,opt::PhysicalKind::IndexScan);
                QCOMPARE(input.ordering.size(),std::size_t(2));
                QCOMPARE(input.ordering[0].name,std::string("bucket"));
            }
            const auto source=run(sql);
            const auto emitted=opt::generatePlanSql(plan.logical,node->logical);
            QVERIFY2(emitted.ok(),emitted.diagnostics.empty()?"empty SQL":emitted.diagnostics.front().message.c_str());
            const auto rewritten=opt::generatePlanSql(plan.logical,plan.logical.root);
            QVERIFY(rewritten.ok());
            const auto output=run(rewritten.sql);
            QCOMPARE(output.rows,source.rows);
            QCOMPARE(output.columnTypes,source.columnTypes);
        }
        for (const auto &sql:{lhs+"FULL"+rhs+" ORDER BY a,id;",
                              lhs+"INNER"+rhs+" AND random()>0.5 ORDER BY a,id;"}) {
            const auto plan=planned(sql);
            QVERIFY(!merge(plan.best));
        }
    }catch(const std::exception &e){QFAIL(e.what());}}
    void joinInputProjectionPreservesRowsAndReducesWidth() {try {
        using namespace navidog;
        using namespace navidog::sql::compiler;
        namespace opt=navidog::sql::optimizer;
        auto db=connectDatabase(profile_);
        const auto token=cancellation();
        const auto run=[&](const std::string &sql) {
            const auto result=db->execute(QString::fromStdString(sql),{},token);
            if (!result.error.isEmpty()) throw std::runtime_error(result.error.toStdString());
            return result;
        };
        run("CREATE TABLE public.trim_left_owned(id int,ref int,unused1 text,unused2 text)");
        run("CREATE TABLE public.trim_right_owned(id int,payload bigint,unused1 text,unused2 text)");
        run("INSERT INTO public.trim_left_owned VALUES (1,2,'a','a'),(1,2,'b','b'),(3,NULL,'c','c'),(4,9,'d','d')");
        run("INSERT INTO public.trim_right_owned VALUES (2,5,'x','x'),(2,5,'y','y'),(2,6,'z','z'),(NULL,6,'n','n')");
        const auto snapshot=std::make_shared<const MetadataSnapshot>(collectMetadata(*db,profile_,"public",token));
        const MetadataCatalog catalog(snapshot);
        const NameResolutionContext names{profile_.database.toStdString(),"public",Dialect::PostgreSQL};
        opt::MemoryStatistics stats;
        opt::TableStatistics table;
        table.database=names.database;
        table.schema="public";
        table.name="trim_left_owned";
        table.rows=10000;
        table.averageRowBytes=2048;
        table.columns={{"id",10000,0,4},{"ref",2500,0.1,4}};
        stats.tables.push_back(table);
        table.name="trim_right_owned";
        table.columns={{"id",2500,0.1,4},{"payload",2000,0,8}};
        stats.tables.push_back(table);
        opt::PlannerOptions options;
        options.heuristic.rules={opt::HepRule::JoinDeriveNotNull,opt::HepRule::ProjectJoinTranspose};
        options.weights.memory=1;
        const auto planned=[&](const std::string &sql) {
            const auto compiled=compile(Dialect::PostgreSQL,sql,{},&catalog,names);
            if (!compiled->ok()) throw std::runtime_error(compiled->diagnostics.front().message);
            const auto plan=opt::optimize(compiled->model,&stats,options);
            if (!plan.ok()) throw std::runtime_error(plan.diagnostics.front().message);
            return plan;
        };
        const auto rule=[](const opt::OptimizationResult &plan) {
            return std::find_if(plan.rules.begin(),plan.rules.end(),
                [](const auto &event){return event.rule=="HepProjectJoinTranspose";});
        };
        const std::string projection="SELECT l.id AS key,r.payload AS val FROM public.trim_left_owned l ";
        const std::string rhs=" JOIN public.trim_right_owned r ";
        for (const std::string join:{"INNER","LEFT","RIGHT","FULL"}) {
            const auto sql=projection+join+rhs+"ON l.ref=r.id ORDER BY 1,2;";
            const auto plan=planned(sql);
            QVERIFY2(rule(plan)!=plan.rules.end(),opt::dumpOptimization(plan).c_str());
            const auto &top=plan.logical.nodes.at(rule(plan)->after);
            const auto &trimmed=plan.logical.nodes.at(top.inputs.front());
            QVERIFY(trimmed.kind==opt::RelKind::Join && trimmed.outputs.size()==4);
            for (auto id:trimmed.inputs) {
                const auto &child=plan.logical.nodes.at(id);
                QCOMPARE(child.kind,opt::RelKind::Project);
                QCOMPARE(child.outputs.size(),std::size_t(2));
            }
            QVERIFY(plan.best.nodes.at(plan.best.root).total.score(options.weights) <
                    plan.baseline.nodes.at(plan.baseline.root).total.score(options.weights));
            const auto emitted=opt::generatePlanSql(plan.logical,plan.heuristicRoot);
            QVERIFY2(emitted.ok(),emitted.diagnostics.empty()?"no SQL":emitted.diagnostics.front().message.c_str());
            QVERIFY(emitted.sql.find("unused")==std::string::npos);
            const auto input=run(sql), output=run(emitted.sql);
            QCOMPARE(output.rows,input.rows);
            QCOMPARE(output.columns,input.columns);
            QCOMPARE(output.columnTypes,input.columnTypes);
        }
        for (const auto &sql:{
                 projection+"LEFT"+rhs+"ON l.ref=r.id ORDER BY r.unused1;",
                 projection+"INNER"+rhs+"USING(id) ORDER BY 1,2;",
                 projection+"INNER"+rhs+"ON l.ref=r.id AND random()>0 ORDER BY 1,2;",
                 projection+"LEFT"+rhs+"ON l.ref=r.id ORDER BY 1,2 LIMIT 1;"}) {
            const auto plan=planned(sql);
            QVERIFY2(rule(plan)==plan.rules.end(),sql.c_str());
        }
    }catch(const std::exception &e){QFAIL(e.what());}}
    void outerJoinTopNPreservesOwnedPostgresPages() {try {
        using namespace navidog;
        using namespace navidog::sql::compiler;
        namespace opt=navidog::sql::optimizer;
        auto db=connectDatabase(profile_);
        const auto token=cancellation();
        const auto run=[&](const std::string &sql) {
            const auto result=db->execute(QString::fromStdString(sql),{},token);
            if (!result.error.isEmpty()) throw std::runtime_error(result.error.toStdString());
            return result;
        };
        run("CREATE TABLE public.topn_keep_owned(id integer PRIMARY KEY,score integer,ref integer)");
        run("CREATE TABLE public.topn_match_owned(id integer PRIMARY KEY,payload bigint)");
        run("CREATE TABLE public.topn_duplicate_owned(id integer,payload bigint)");
        run("INSERT INTO public.topn_keep_owned VALUES "
            "(1,9,1),(2,9,1),(3,NULL,2),(4,7,7),(5,7,NULL),(6,7,3),(7,2,1),(8,NULL,NULL)");
        run("INSERT INTO public.topn_match_owned VALUES (1,11),(2,22),(3,NULL)");
        run("INSERT INTO public.topn_duplicate_owned VALUES (1,11),(1,12),(2,22)");
        const auto snapshot=collectMetadata(*db,profile_,"public",token);
        const MetadataCatalog catalog(std::make_shared<const MetadataSnapshot>(snapshot));
        const NameResolutionContext context{profile_.database.toStdString(),"public",Dialect::PostgreSQL};
        opt::PlannerOptions options;
        options.heuristic.rules={opt::HepRule::JoinDeriveNotNull,opt::HepRule::SortJoinTranspose};
        const auto planned=[&](const std::string &sql,const opt::PlannerOptions &settings) {
            const auto compiled=compile(Dialect::PostgreSQL,sql,{},&catalog,context);
            if (!compiled->ok()) throw std::runtime_error(compiled->diagnostics.front().message);
            const auto plan=opt::optimize(compiled->model,nullptr,settings);
            if (!plan.ok()) throw std::runtime_error(plan.diagnostics.front().message);
            return plan;
        };
        const auto fired=[](const opt::OptimizationResult &plan) {
            return std::find_if(plan.rules.begin(),plan.rules.end(),
                [](const auto &event){return event.rule=="HepSortJoinTranspose";});
        };
        const std::string select="SELECT k.score AS ranking,k.id AS seq,m.payload AS value FROM ";
        const std::string kept="public.topn_keep_owned k";
        const std::string matched="public.topn_match_owned m";
        const std::string predicate=" ON k.ref=m.id";
        const std::string order=" ORDER BY ranking DESC NULLS LAST,2 DESC";
        const std::string page=" LIMIT 3 OFFSET 2";
        for (const bool left : {true,false}) {
            const std::string join=left?kept+" LEFT JOIN "+matched:matched+" RIGHT JOIN "+kept;
            const auto sql=select+join+predicate+order+page;
            const auto plan=planned(sql,options);
            const auto event=fired(plan);
            QVERIFY2(event!=plan.rules.end(),opt::dumpOptimization(plan).c_str());
            QCOMPARE(std::count_if(plan.rules.begin(),plan.rules.end(),
                [](const auto &entry){return entry.rule=="HepSortJoinTranspose";}),1);
            const auto &outer=plan.logical.nodes.at(event->after);
            QVERIFY(outer.kind==opt::RelKind::Limit && outer.rowLimit==3 && outer.rowOffset==2);
            const auto &sort=plan.logical.nodes.at(outer.inputs.at(0));
            const auto &project=plan.logical.nodes.at(sort.inputs.at(0));
            const auto &joined=plan.logical.nodes.at(project.inputs.at(0));
            QVERIFY(joined.join==(left?JoinKind::Left:JoinKind::Right));
            const auto &cap=plan.logical.nodes.at(joined.inputs.at(left?0:1));
            QVERIFY(cap.kind==opt::RelKind::Limit && cap.rowLimit==5 && cap.rowOffset==0);
            const auto &local=plan.logical.nodes.at(cap.inputs.at(0));
            QVERIFY(local.kind==opt::RelKind::Sort && local.keys.size()==2);
            QVERIFY(local.descending==sort.descending && local.nullOrdering==sort.nullOrdering);
            for (const auto ref:local.keys) {
                const auto &key=plan.logical.program.at(ref.query-1).expressions.at(ref.expression);
                QVERIFY(!key.outputColumn && key.binding);
                QCOMPARE(key.name.front(),std::string("k"));
            }
            const auto original=run(sql);
            const auto inner="(SELECT * FROM public.topn_keep_owned ORDER BY score DESC NULLS LAST,id DESC LIMIT "+
                std::to_string(static_cast<int>(*cap.rowLimit))+") k";
            const auto replacement=left?inner+" LEFT JOIN "+matched:matched+" RIGHT JOIN "+inner;
            const auto rewritten=run(select+replacement+predicate+order+page);
            QCOMPARE(rewritten.rows,original.rows);
            QCOMPARE(rewritten.columns,original.columns);
            QCOMPARE(rewritten.columnTypes,original.columnTypes);
            const auto defaults=planned(sql,{});
            QVERIFY2(fired(defaults)!=defaults.rules.end(),opt::dumpOptimization(defaults).c_str());
        }
        const auto join=kept+" LEFT JOIN "+matched+predicate;
        for (const auto &sql : {
                select+join+" ORDER BY ranking"+page,
                select+join+" ORDER BY value,seq"+page,
                select+join+order+" LIMIT $1 OFFSET 2",
                select+join+order+" OFFSET 2 FETCH FIRST 3 ROWS WITH TIES",
                select+join+" WHERE m.payload IS NOT NULL"+order+page,
                select+kept+" LEFT JOIN public.topn_duplicate_owned m"+predicate+order+page,
                select+kept+" FULL JOIN "+matched+predicate+order+page,
                select+kept+" LEFT JOIN "+matched+" ON k.ref=m.id AND random()>0"+order+page}) {
            const auto plan=planned(sql,options);
            QVERIFY2(fired(plan)==plan.rules.end(),sql.c_str());
        }
    }catch(const std::exception &e){QFAIL(e.what());}}
    void unionSharedConstantsPreserveTypesAndMultiplicity() {try {
        using namespace navidog;
        using namespace navidog::sql::compiler;
        namespace opt=navidog::sql::optimizer;
        auto db=connectDatabase(profile_);
        const auto token=cancellation();
        const auto run=[&](const std::string &sql) {
            const auto result=db->execute(QString::fromStdString(sql),{},token);
            if (!result.error.isEmpty()) throw std::runtime_error(result.error.toStdString());
            return result;
        };
        run("CREATE TABLE public.fixed_union_left_owned(id integer)");
        run("CREATE TABLE public.fixed_union_right_owned(id integer)");
        run("INSERT INTO public.fixed_union_left_owned VALUES (1),(1),(NULL)");
        run("INSERT INTO public.fixed_union_right_owned VALUES (1),(3),(NULL)");
        const auto snapshot=std::make_shared<const MetadataSnapshot>(collectMetadata(*db,profile_,"public",token));
        const MetadataCatalog catalog(snapshot);
        const NameResolutionContext names{profile_.database.toStdString(),"public",Dialect::PostgreSQL};
        opt::PlannerOptions options;
        options.heuristic.rules={opt::HepRule::UnionPullUpConstants};
        const auto planFor=[&](const std::string &sql) {
            const auto parsed=compile(Dialect::PostgreSQL,sql,{},&catalog,names);
            if (!parsed->ok()) throw std::runtime_error(parsed->diagnostics.front().message);
            const auto plan=opt::optimize(parsed->model,nullptr,options);
            if (!plan.ok()) throw std::runtime_error(plan.diagnostics.front().message);
            return plan;
        };
        const auto event=[](const opt::OptimizationResult &plan) {
            return std::find_if(plan.rules.begin(),plan.rules.end(),
                [](const auto &rule){return rule.rule=="HepUnionPullUpConstants";});
        };
        const std::string l=" FROM public.fixed_union_left_owned ";
        const std::string r=" FROM public.fixed_union_right_owned ";
        const auto first="SELECT 7 AS marker,id,CAST(NULL AS bigint) AS gap"+l;
        const auto second="SELECT 7,id,CAST(NULL AS bigint)"+r;
        for (const auto &sql:{first+"UNION ALL "+second+"ORDER BY 2;",
                 "SELECT 7 AS marker,8 AS value"+l+"UNION ALL SELECT 7,8"+r+"ORDER BY 1,2;"}) {
            const auto plan=planFor(sql);
            QVERIFY2(event(plan)!=plan.rules.end(),opt::dumpOptimization(plan).c_str());
            const auto &project=plan.logical.nodes.at(event(plan)->after);
            const auto &united=plan.logical.nodes.at(project.inputs.at(0));
            QCOMPARE(united.kind,opt::RelKind::SetOperation);
            QCOMPARE(united.outputs.size(),std::size_t(1));
            for (auto id:united.inputs) QCOMPARE(plan.logical.nodes.at(id).keys.size(),std::size_t(1));
            const auto emitted=opt::generatePlanSql(plan.logical,plan.heuristicRoot);
            QVERIFY2(emitted.ok(),emitted.diagnostics.empty()?"no SQL":emitted.diagnostics.front().message.c_str());
            const auto expected=run(sql), actual=run(emitted.sql);
            QCOMPARE(actual.rows,expected.rows);
            QCOMPARE(actual.rows.size(),6);
            QCOMPARE(actual.columns,expected.columns);
            QCOMPARE(actual.columnTypes,expected.columnTypes);
        }
        for (const auto &sql:{first+"UNION "+second,
                 "SELECT 7 AS marker,id"+l+"UNION ALL SELECT 8,id"+r,
                 "SELECT 7 AS marker,id"+l+"UNION ALL SELECT CAST(7 AS bigint),id"+r,
                 "SELECT 7 AS marker,random()::integer AS v"+l+"UNION ALL SELECT 7,id"+r}) {
            const auto plan=planFor(sql);
            QVERIFY2(event(plan)==plan.rules.end(),sql.c_str());
        }
    }catch(const std::exception &e){QFAIL(e.what());}}
    void unionProjectionKeepsAliasesAndDuplicateRows() {try {
        using namespace navidog;
        using namespace navidog::sql::compiler;
        namespace opt=navidog::sql::optimizer;
        auto db=connectDatabase(profile_);
        const auto token=cancellation();
        const auto run=[&](const std::string &sql) {
            const auto result=db->execute(QString::fromStdString(sql),{},token);
            if (!result.error.isEmpty()) throw std::runtime_error(result.error.toStdString());
            return result;
        };
        run("CREATE TABLE public.project_set_left_owned(a int,b bigint,c int,d smallint,x int)");
        run("CREATE TABLE public.project_set_right_owned(k int,v bigint,c int,d smallint,x int)");
        run("INSERT INTO public.project_set_left_owned VALUES (1,2,4,7,99),(1,2,5,7,99),(2,NULL,6,8,98)");
        run("INSERT INTO public.project_set_right_owned VALUES (1,2,6,7,97),(2,NULL,8,12,96),(NULL,7,4,1,95)");
        const auto snapshot=std::make_shared<const MetadataSnapshot>(collectMetadata(*db,profile_,"public",token));
        const MetadataCatalog catalog(snapshot);
        const NameResolutionContext names{profile_.database.toStdString(),"public",Dialect::PostgreSQL};
        opt::PlannerOptions options;
        options.heuristic.rules={opt::HepRule::ProjectSetOpTranspose};
        const auto model=[&](const std::string &integ,const opt::PlannerOptions &settings) {
            const auto parsed=compile(Dialect::PostgreSQL,integ,{},&catalog,names);
            if (!parsed->ok()) throw std::runtime_error(parsed->diagnostics.front().message);
            const auto plan=opt::optimize(parsed->model,nullptr,settings);
            if (!plan.ok()) throw std::runtime_error(plan.diagnostics.front().message);
            return plan;
        };
        const auto rule=[](const opt::OptimizationResult &plan) {
            return std::find_if(plan.rules.begin(),plan.rules.end(),
                [](const auto &event){return event.rule=="HepProjectSetOpTranspose";});
        };
        const std::string prefix="SELECT s.amount AS val,s.key AS id,s.amount AS again FROM ";
        const std::string left="(SELECT a AS key,b AS amount,c,d,x FROM public.project_set_left_owned ";
        const std::string right=" SELECT k,v,c,d,x FROM public.project_set_right_owned";
        const std::string tail=") AS s ORDER BY 1 NULLS FIRST,2,3;";
        const auto sql=prefix+left+"UNION ALL"+right+tail;
        for (const auto &settings:{options,opt::PlannerOptions{}}) {
            const auto plan=model(sql,settings);
            const auto event=rule(plan);
            QVERIFY2(event!=plan.rules.end(),opt::dumpOptimization(plan).c_str());
            const auto &rename=plan.logical.nodes.at(event->after);
            const auto &united=plan.logical.nodes.at(rename.inputs.at(0));
            QVERIFY(united.kind==opt::RelKind::SetOperation && united.outputs.size()==3);
            for(const auto branch:united.inputs)
                QCOMPARE(plan.logical.nodes.at(branch).keys.size(),std::size_t(3));
            const auto emitted=opt::generatePlanSql(plan.logical,plan.heuristicRoot);
            QVERIFY2(emitted.ok(),emitted.diagnostics.empty()?"no SQL":emitted.diagnostics.front().message.c_str());
            const auto expected=run(sql);
            const auto actual=run(emitted.sql);
            QCOMPARE(actual.rows,expected.rows);
            QCOMPARE(actual.columns,expected.columns);
            QCOMPARE(actual.columnTypes,expected.columnTypes);
            QCOMPARE(actual.rows.size(),6);
            QVERIFY(!actual.rows.front()[0].isValid());
        }
        for (const auto &text:{
                prefix+left+"UNION"+right+tail,
                prefix+left+"EXCEPT ALL"+right+tail,
                prefix+left+"UNION ALL"+right+" LIMIT 2"+tail,
                "SELECT s.amount+1 AS val FROM "+left+"UNION ALL"+right+") AS s ORDER BY 1;",
                prefix+left+"UNION ALL SELECT k,v,random()::int,d,x FROM public.project_set_right_owned"+tail}) {
            const auto plan=model(text,options);
            QVERIFY2(rule(plan)==plan.rules.end(),text.c_str());
        }
    }catch(const std::exception &e){QFAIL(e.what());}}
    void unionTopNPreservesOrderedPage() {try {
        using namespace navidog;
        using namespace navidog::sql::compiler;
        namespace opt=navidog::sql::optimizer;
        auto db=connectDatabase(profile_);
        auto token=cancellation();
        const auto run=[&](const QString &sql) {
            const auto result=db->execute(sql,{},token);
            if (!result.error.isEmpty()) throw std::runtime_error(result.error.toStdString());
            return result;
        };
        run("CREATE TABLE public.page_left_owned(n INT,id INT)");
        run("CREATE TABLE public.page_right_owned(v INT,seq INT)");
        run("INSERT INTO public.page_left_owned VALUES(NULL,10),(7,3),(7,3),(7,4),(5,6),(3,9),(1,11)");
        run("INSERT INTO public.page_right_owned VALUES(NULL,10),(7,3),(7,7),(6,5),(4,8),(2,12)");
        const auto snapshot=collectMetadata(*db,profile_,"public",token);
        const MetadataCatalog catalog(std::make_shared<const MetadataSnapshot>(snapshot));
        const NameResolutionContext context{profile_.database.toStdString(),"public",Dialect::PostgreSQL};
        const std::string left="SELECT n AS score,id FROM public.page_left_owned";
        const std::string right="SELECT v AS score,seq AS id FROM public.page_right_owned";
        const std::string order=" ORDER BY 1 DESC NULLS LAST, id";
        const auto source=left+" UNION ALL "+right+order+" LIMIT 4 OFFSET 2";
        const auto planned=[&](const std::string &sql) {
            const auto compiled=compile(Dialect::PostgreSQL,sql,{},&catalog,context);
            if (!compiled->ok()) throw std::runtime_error(compiled->diagnostics.front().message);
            const auto result=opt::optimize(compiled->model);
            if (!result.ok()) throw std::runtime_error(result.diagnostics.front().message);
            return result;
        };
        const auto plan=planned(source);
        const auto event=std::find_if(plan.rules.begin(),plan.rules.end(),
            [](const auto &rule) { return rule.rule=="HepSortUnionTranspose"; });
        QVERIFY(event!=plan.rules.end());
        const auto &limit=plan.logical.nodes.at(event->after);
        const auto &sort=plan.logical.nodes.at(limit.inputs.at(0));
        const auto &set=plan.logical.nodes.at(sort.inputs.at(0));
        QVERIFY(limit.rowLimit==4 && limit.rowOffset==2);
        QCOMPARE(set.inputs.size(),2);
        for (const auto arm : set.inputs) {
            const auto &cap=plan.logical.nodes.at(arm);
            QVERIFY(cap.kind==opt::RelKind::Limit && cap.rowLimit==6 && cap.rowOffset==0);
            const auto &orderBy=plan.logical.nodes.at(cap.inputs.at(0));
            QVERIFY(orderBy.kind==opt::RelKind::Sort && orderBy.descending==sort.descending &&
                    orderBy.nullOrdering==sort.nullOrdering);
        }
        const auto rewritten="("+left+order+" LIMIT 6) UNION ALL ("+right+order+" LIMIT 6)"+order+" LIMIT 4 OFFSET 2";
        QCOMPARE(run(QString::fromStdString(source)).rows,run(QString::fromStdString(rewritten)).rows);
        opt::MemoryStatistics estimates;
        for (const auto &[name, columns] : std::vector<std::pair<std::string,std::vector<std::string>>>{
                 {"page_left_owned",{"n","id"}},{"page_right_owned",{"v","seq"}}}) {
            opt::TableStatistics table;
            table.database=profile_.database.toStdString();
            table.schema="public";
            table.name=name;
            table.rows=100000;
            table.averageRowBytes=16;
            table.indexes.push_back({name+"_order",columns,false,true,100});
            estimates.tables.push_back(std::move(table));
        }
        const auto mergeCount = [&](const std::string &sql) {
            const auto compiled=compile(Dialect::PostgreSQL,sql,{},&catalog,context);
            if (!compiled->ok()) throw std::runtime_error(compiled->diagnostics.front().message);
            const auto candidate=opt::optimize(compiled->model,&estimates);
            if (!candidate.ok()) throw std::runtime_error(candidate.diagnostics.front().message);
            std::vector<opt::PlanId> pending{candidate.best.root};
            int merges=0;
            while (!pending.empty()) {
                const auto id=pending.back();
                pending.pop_back();
                const auto &node=candidate.best.nodes.at(id);
                if (node.kind==opt::PhysicalKind::MergeUnion) {
                    if (node.ordering.size()!=2 || node.inputs.size()!=2 ||
                        !std::all_of(node.inputs.begin(),node.inputs.end(),[&](auto child) {
                            return candidate.best.nodes.at(child).ordering.size()==2;
                        })) throw std::runtime_error("Merge Union lost an input ordering");
                    ++merges;
                }
                pending.insert(pending.end(),node.inputs.begin(),node.inputs.end());
            }
            return merges;
        };
        QCOMPARE(mergeCount(left+" UNION ALL "+right+" ORDER BY score,id"),1);
        QCOMPARE(mergeCount(left+" UNION "+right+" ORDER BY score,id"),1);
        QCOMPARE(mergeCount(left+" UNION ALL "+right+order),0);
        QCOMPARE(mergeCount(left+" UNION ALL SELECT CAST(v AS BIGINT),seq FROM public.page_right_owned ORDER BY score,id"),0);
        for (const auto &unsafe : {left+" UNION "+right+order+" LIMIT 4 OFFSET 2",
                                   left+" UNION ALL "+right+" ORDER BY score LIMIT 4 OFFSET 2",
                                   left+" UNION ALL "+right+order+" LIMIT 0"}) {
            const auto rejected=planned(unsafe);
            QVERIFY(std::none_of(rejected.rules.begin(),rejected.rules.end(),
                [](const auto &rule) { return rule.rule=="HepSortUnionTranspose"; }));
        }
        const auto verifyPruning = [&](const std::string &original, const std::string &survivor) {
            const auto pruned=planned(original);
            const auto rename=std::find_if(pruned.rules.begin(),pruned.rules.end(),[&](const auto &rule) {
                return rule.rule=="HepPruneEmpty" && pruned.logical.nodes.at(rule.after).kind==opt::RelKind::Rename;
            });
            QVERIFY(rename!=pruned.rules.end());
            const auto &output=pruned.logical.nodes.at(rename->after).outputs;
            QVERIFY(output.size()==2 && output[0].name=="score" && output[1].name=="id");
            const auto rows=run(QString::fromStdString(original+order));
            const auto remaining=run(QString::fromStdString(survivor+order));
            QCOMPARE(rows.rows,remaining.rows);
            QCOMPARE(rows.columns,remaining.columns);
        };
        verifyPruning("("+left+" LIMIT 0) UNION ALL SELECT v,seq FROM public.page_right_owned",right);
        verifyPruning(left+" EXCEPT ALL ("+right+" LIMIT 0)",left);
        const std::string shared="SELECT n AS score,id FROM public.page_left_owned WHERE id>=2 "
            "UNION ALL SELECT n AS renamed,id FROM public.page_left_owned WHERE id>=2";
        const auto reused=planned(shared+order);
        const auto common=std::find_if(reused.rules.begin(),reused.rules.end(),[](const auto &rule) {
            return rule.rule=="CombineSimpleEquivalence";
        });
        QVERIFY(common!=reused.rules.end());
        QVERIFY(std::any_of(reused.sharedRelations.begin(),reused.sharedRelations.end(),[](const auto &part) {
            return part.uses==2;
        }));
        auto sharedPlan=reused.logical;
        for (auto &node:sharedPlan.nodes)
            std::replace(node.inputs.begin(),node.inputs.end(),common->before,common->after);
        const auto sharedSql=opt::generatePlanSql(sharedPlan,sharedPlan.root);
        QVERIFY2(sharedSql.ok(),sharedSql.diagnostics.empty()?"no SQL":sharedSql.diagnostics.front().message.c_str());
        const auto materialization=sharedSql.sql.find(" AS MATERIALIZED ");
        QVERIFY(materialization!=std::string::npos);
        QCOMPARE(materialization,sharedSql.sql.rfind(" AS MATERIALIZED "));
        const auto beforeShared=run(QString::fromStdString(shared+order));
        const auto afterShared=run(QString::fromStdString(sharedSql.sql));
        QCOMPARE(afterShared.rows,beforeShared.rows);
        QCOMPARE(afterShared.columns,beforeShared.columns);
        QCOMPARE(afterShared.columnTypes,beforeShared.columnTypes);
        const auto different=planned(shared.substr(0,shared.size()-1)+"3"+order);
        QVERIFY(different.sharedRelations.empty());
        for (const auto &source:std::vector<std::string>{
                 "("+left+" LIMIT 0) UNION SELECT v,seq FROM public.page_right_owned",
                 left+" EXCEPT ("+right+" LIMIT 0)"}) {
            const auto pruned=planned(source+order);
            QVERIFY(std::any_of(pruned.rules.begin(),pruned.rules.end(),[&](const auto &event) {
                const auto &rewritten=pruned.logical.nodes.at(event.after);
                return event.rule=="HepPruneEmpty" && rewritten.kind==opt::RelKind::Aggregate &&
                       rewritten.distinct;
            }));
            const auto emitted=opt::generatePlanSql(pruned.logical,pruned.heuristicRoot);
            QVERIFY2(emitted.ok(),emitted.diagnostics.empty()?source.c_str():emitted.diagnostics.front().message.c_str());
            QVERIFY(emitted.sql.find("SELECT DISTINCT")!=std::string::npos);
            const auto before=run(QString::fromStdString(source+order)),after=run(QString::fromStdString(emitted.sql));
            QCOMPARE(after.rows,before.rows);
            QCOMPARE(after.columns,before.columns);
            QCOMPARE(after.columnTypes,before.columnTypes);
        }
        const auto distinct=planned(left+" UNION "+right+order);
        QVERIFY(std::any_of(distinct.rules.begin(),distinct.rules.end(),[](const auto &rule) {
            return rule.rule=="UnionToDistinct";
        }));
        const auto nativeDistinct=run(QString::fromStdString(left+" UNION "+right+order));
        const auto explicitDistinct=run(QString::fromStdString(
            "SELECT DISTINCT * FROM ("+left+" UNION ALL "+right+") AS u"+order));
        QCOMPARE(nativeDistinct.rows,explicitDistinct.rows);
        QCOMPARE(nativeDistinct.columns,explicitDistinct.columns);
        QCOMPARE(nativeDistinct.columnTypes,explicitDistinct.columnTypes);
        QCOMPARE(nativeDistinct.rows.size(),qsizetype(10));
        const auto chain="("+left+" UNION ALL "+right+") UNION ALL "+left;
        const auto flat=planned(chain+order);
        const auto combined=std::find_if(flat.rules.begin(),flat.rules.end(),
            [](const auto &rule) { return rule.rule=="UnionMerge"; });
        QVERIFY(combined!=flat.rules.end());
        const auto &flattened=flat.logical.nodes.at(combined->after);
        QVERIFY(flattened.inputs.size()==3 && flattened.outputs[0].name=="score");
        const auto sourceRows=run(QString::fromStdString(chain+order));
        const auto flatRows=run(QString::fromStdString(left+" UNION ALL "+right+" UNION ALL "+left+order));
        QCOMPARE(sourceRows.rows,flatRows.rows);
        QCOMPARE(sourceRows.columns,flatRows.columns);
        QCOMPARE(flatRows.rows.size(),qsizetype(20));
        run("ALTER TABLE public.page_right_owned ALTER COLUMN v TYPE BIGINT");
        const auto changed=collectMetadata(*db,profile_,"public",token);
        const MetadataCatalog wider(std::make_shared<const MetadataSnapshot>(changed));
        const auto mixed=left+" UNION ALL "+right+" ORDER BY score,id;";
        const auto unionQuery=compile(Dialect::PostgreSQL,mixed,{},&wider,context);
        QVERIFY(unionQuery->ok());
        const auto coercion=opt::optimize(unionQuery->model,&estimates);
        QVERIFY(coercion.ok());
        QVERIFY(std::any_of(coercion.rules.begin(),coercion.rules.end(),[](const auto &rule) {
            return rule.rule=="CoerceInputs";
        }));
        std::vector<opt::PlanId> pending{coercion.best.root};
        bool mergedWidths=false;
        while(!pending.empty()) {
            const auto node=pending.back();
            pending.pop_back();
            const auto &candidate=coercion.best.nodes.at(node);
            mergedWidths |= candidate.kind==opt::PhysicalKind::MergeUnion;
            pending.insert(pending.end(),candidate.inputs.begin(),candidate.inputs.end());
        }
        QVERIFY(mergedWidths);
        const auto implicit=run(QString::fromStdString(mixed));
        const auto explicitCast=run(QString::fromStdString(
            "SELECT CAST(n AS BIGINT) AS score,id FROM public.page_left_owned UNION ALL "+right+" ORDER BY score,id;"));
        QCOMPARE(implicit.rows,explicitCast.rows);
        QCOMPARE(implicit.columns,explicitCast.columns);
        QCOMPARE(implicit.columnTypes,explicitCast.columnTypes);
        run("DROP TABLE public.page_left_owned,public.page_right_owned");
    }catch(const std::exception &e){QFAIL(e.what());}}
    void setMergePreservesMultiplicityTypesAndDifferenceOrder() {try {
        using namespace navidog;
        using namespace navidog::sql::compiler;
        using namespace navidog::sql::optimizer;
        auto db=connectDatabase(profile_);
        const auto token=cancellation();
        const auto run=[&](const std::string &sql) {
            const auto result=db->execute(QString::fromStdString(sql),{},token);
            if (!result.error.isEmpty()) throw std::runtime_error(result.error.toStdString());
            return result;
        };
        run("CREATE TABLE public.merge_sets_a_owned(k smallint,v bigint)");
        run("CREATE TABLE public.merge_sets_b_owned(k smallint,v bigint)");
        run("CREATE TABLE public.merge_sets_c_owned(k smallint,v bigint)");
        run("INSERT INTO public.merge_sets_a_owned VALUES (1,0),(1,0),(1,0),(2,4),(3,NULL),(3,NULL),(3,2),(NULL,9)");
        run("INSERT INTO public.merge_sets_b_owned VALUES (1,0),(1,0),(2,4),(3,NULL),(3,2),(NULL,9)");
        run("INSERT INTO public.merge_sets_c_owned VALUES (1,0),(3,NULL),(NULL,9),(NULL,9),(4,8)");
        const MetadataCatalog catalog(std::make_shared<const MetadataSnapshot>(
            collectMetadata(*db,profile_,"public",token)));
        const NameResolutionContext names{profile_.database.toStdString(),"public",Dialect::PostgreSQL};
        const auto planned=[&](const std::string &sql,PlannerOptions options={}) {
            const auto parsed=compile(Dialect::PostgreSQL,sql,{},&catalog,names);
            if (!parsed->ok()) throw std::runtime_error(parsed->diagnostics.front().message);
            auto result=optimize(parsed->model,nullptr,options);
            if (!result.ok()) throw std::runtime_error(result.diagnostics.front().message);
            return result;
        };
        const std::string a="SELECT k AS first_key,v AS first_value FROM public.merge_sets_a_owned";
        const std::string b="SELECT k AS second_key,v FROM public.merge_sets_b_owned";
        const std::string c="SELECT k AS third_key,v FROM public.merge_sets_c_owned";
        const auto verify=[&](const std::string &sql,const std::string &rule) {
            const auto plan=planned(sql);
            const auto candidate=std::find_if(plan.rules.begin(),plan.rules.end(),[&](const RuleEvent &entry) {
                return entry.rule==rule && plan.logical.nodes.at(entry.after).inputs.size()==3;
            });
            QVERIFY2(candidate!=plan.rules.end(),sql.c_str());
            const auto generated=generatePlanSql(plan.logical,candidate->after);
            QVERIFY2(generated.ok(),generated.diagnostics.empty()?sql.c_str():generated.diagnostics.front().message.c_str());
            const std::string order=") AS ordered_rows ORDER BY 1 NULLS FIRST,2 NULLS FIRST";
            const auto baseline=run("SELECT * FROM ("+sql+order);
            auto rewritten=generated.sql;
            if (!rewritten.empty() && rewritten.back()==';') rewritten.pop_back();
            const auto flattened=run("SELECT * FROM ("+rewritten+order);
            QCOMPARE(flattened.rows,baseline.rows);
            QCOMPARE(flattened.columns,baseline.columns);
            QCOMPARE(flattened.columnTypes,baseline.columnTypes);
        };
        verify("("+a+" UNION "+b+") UNION "+c,"UnionMerge");
        verify("("+a+" INTERSECT ALL "+b+") INTERSECT ALL "+c,"IntersectMerge");
        verify(a+" INTERSECT ALL ("+b+" INTERSECT ALL "+c+")","IntersectMerge");
        verify("("+a+" INTERSECT "+b+") INTERSECT "+c,"IntersectMerge");
        verify("("+a+" EXCEPT ALL "+b+") EXCEPT ALL "+c,"ExceptMerge");
        verify("("+a+" EXCEPT "+b+") EXCEPT "+c,"ExceptMerge");
        const auto noExceptMerge=[&](const std::string &sql,PlannerOptions options={}) {
            const auto plan=planned(sql,options);
            QVERIFY(std::none_of(plan.rules.begin(),plan.rules.end(),[](const RuleEvent &entry) {
                return entry.rule=="ExceptMerge";
            }));
        };
        noExceptMerge(a+" EXCEPT ALL ("+b+" EXCEPT ALL "+c+")");
        noExceptMerge("("+a+" EXCEPT ALL "+b+") EXCEPT "+c);
        noExceptMerge("("+a+" EXCEPT ALL "+b+" LIMIT 2) EXCEPT ALL "+c);
        noExceptMerge("("+a+" EXCEPT ALL "+b+") EXCEPT ALL "
                      "SELECT k,CAST(v AS integer) FROM public.merge_sets_c_owned");
        PlannerOptions disabled;
        disabled.unionMerge=false;
        noExceptMerge("("+a+" EXCEPT ALL "+b+") EXCEPT ALL "+c,disabled);

        const std::array rules{"IntersectToSemiJoin","IntersectToDistinct","MinusToAntiJoin","MinusToDistinct",
                               "IntersectToExists"};
        const std::string order=") AS ordered_rows ORDER BY 1 NULLS FIRST,2 NULLS FIRST";
        for (std::size_t i=0;i<rules.size();++i) {
            const std::string first=i<2 || i==4?" INTERSECT ":" EXCEPT ";
            const auto sql="("+a+first+b+")"+first+c;
            const auto plan=planned(sql);
            const auto candidate=std::find_if(plan.rules.begin(),plan.rules.end(),[&](const RuleEvent &entry) {
                return entry.rule==rules[i] && plan.logical.nodes[entry.after].inputs.size()==3;
            });
            QVERIFY2(candidate!=plan.rules.end(),rules[i]);
            const auto generated=generatePlanSql(plan.logical,candidate->after);
            QVERIFY2(generated.ok(),generated.diagnostics.empty()?sql.c_str():generated.diagnostics.front().message.c_str());
            QVERIFY(generated.sql.find(i%2?"COUNT(*) FILTER":" IS NOT DISTINCT FROM ")!=std::string::npos);
            if (i==4) {
                QCOMPARE(plan.logical.nodes.at(candidate->after).setAlgorithm,SetAlgorithm::CorrelatedExists);
                QVERIFY(generated.sql.find("EXISTS (SELECT 1")!=std::string::npos);
            }
            auto rewritten=generated.sql;
            if (!rewritten.empty() && rewritten.back()==';') rewritten.pop_back();
            const auto expected=run("SELECT * FROM ("+sql+order);
            const auto actual=run("SELECT * FROM ("+rewritten+order);
            QCOMPARE(actual.rows,expected.rows);
            QCOMPARE(actual.columns,expected.columns);
            QCOMPARE(actual.columnTypes,expected.columnTypes);
        }
        const auto narrowSql="SELECT s.first_key FROM ("+a+" INTERSECT "+b+") s ORDER BY 1";
        for (std::size_t i=0;i<2;++i) {
            auto plan=planned(narrowSql);
            const auto candidate=std::find_if(plan.rules.begin(),plan.rules.end(),[&](const RuleEvent &entry) {
                return entry.rule==rules[i];
            });
            QVERIFY(candidate!=plan.rules.end());
            for (auto &node:plan.logical.nodes)
                std::replace(node.inputs.begin(),node.inputs.end(),candidate->before,candidate->after);
            const auto generated=generatePlanSql(plan.logical,plan.logical.root);
            QVERIFY2(generated.ok(),generated.diagnostics.empty()?narrowSql.c_str():generated.diagnostics.front().message.c_str());
            QVERIFY(generated.sql.find(i?"COUNT(*) FILTER":" IS NOT DISTINCT FROM ")!=std::string::npos);
            const auto expected=run(narrowSql), actual=run(generated.sql);
            QCOMPARE(actual.rows,expected.rows);
            QCOMPARE(actual.columns,expected.columns);
            QCOMPARE(actual.columnTypes,expected.columnTypes);
        }
        const auto noAlternatives=[&](const OptimizationResult &plan) {
            QVERIFY(std::none_of(plan.rules.begin(),plan.rules.end(),[&](const RuleEvent &entry) {
                return std::any_of(rules.begin(),rules.end(),[&](auto name) { return entry.rule==name; });
            }));
        };
        noAlternatives(planned(a+" INTERSECT ALL "+b));
        noAlternatives(planned(a+" EXCEPT ALL "+b));
        disabled.alternateSetAlgorithms=false;
        noAlternatives(planned(a+" INTERSECT "+b,disabled));

        const auto unionFrom="("+a+" UNION ALL "+b+") AS s";
        const std::string scanFrom="public.merge_sets_c_owned AS m";
        const std::string on=" ON s.first_key=m.k AND s.first_value=m.v";
        const std::string select="SELECT s.first_key,s.first_value,m.k AS match_key,m.v AS match_value FROM ";
        const std::string sorting=" ORDER BY 1 NULLS FIRST,2 NULLS FIRST,3 NULLS FIRST,4 NULLS FIRST";
        for (const auto &join:{unionFrom+" INNER JOIN "+scanFrom,unionFrom+" LEFT JOIN "+scanFrom,
                               scanFrom+" RIGHT JOIN "+unionFrom}) {
            const auto sql=select+join+on+sorting;
            auto plan=planned(sql);
            const auto candidate=std::find_if(plan.rules.begin(),plan.rules.end(),[](const RuleEvent &entry) {
                return entry.rule=="JoinUnionTranspose";
            });
            QVERIFY2(candidate!=plan.rules.end(),sql.c_str());
            const auto &replacement=plan.logical.nodes.at(candidate->after);
            QCOMPARE(replacement.kind,RelKind::SetOperation);
            QVERIFY(replacement.inputs.size()==2 && !replacement.distinct);
            for (auto &node:plan.logical.nodes)
                std::replace(node.inputs.begin(),node.inputs.end(),candidate->before,candidate->after);
            const auto generated=generatePlanSql(plan.logical,plan.logical.root);
            QVERIFY2(generated.ok(),generated.diagnostics.empty()?sql.c_str():generated.diagnostics.front().message.c_str());
            const auto expected=run(sql), actual=run(generated.sql);
            QCOMPARE(actual.rows,expected.rows);
            QCOMPARE(actual.columns,expected.columns);
            QCOMPARE(actual.columnTypes,expected.columnTypes);
        }
        for (const auto &join:{unionFrom+" FULL JOIN "+scanFrom,scanFrom+" LEFT JOIN "+unionFrom,
                               unionFrom+" RIGHT JOIN "+scanFrom,
                               "("+a+" UNION "+b+") AS s INNER JOIN "+scanFrom}) {
            const auto untouched=planned(select+join+on);
            QVERIFY(std::none_of(untouched.rules.begin(),untouched.rules.end(),[](const RuleEvent &entry) {
                return entry.rule=="JoinUnionTranspose";
            }));
        }
    }catch(const std::exception &e){QFAIL(e.what());}}
    void duplicateInsensitiveGroupsCanRemoveOuterMatches() {try {
        using namespace navidog;
        using namespace navidog::sql::compiler;
        using namespace navidog::sql::optimizer;
        auto db=connectDatabase(profile_);
        const auto token=cancellation();
        const auto run=[&](const std::string &sql) {
            const auto result=db->execute(QString::fromStdString(sql),{},token);
            if (!result.error.isEmpty()) throw std::runtime_error(result.error.toStdString());
            return result;
        };
        run("CREATE TABLE public.aggregate_join_keep_owned(k integer,g integer,v integer)");
        run("CREATE TABLE public.aggregate_join_drop_owned(k integer)");
        run("CREATE TABLE public.aggregate_join_empty_owned(k integer,g integer,v integer)");
        run("INSERT INTO public.aggregate_join_keep_owned VALUES "
            "(1,10,4),(2,10,4),(3,10,2),(3,11,NULL),(NULL,NULL,8)");
        run("INSERT INTO public.aggregate_join_drop_owned VALUES (1),(1),(2),(3),(3),(3),(NULL)");
        const MetadataCatalog catalog(std::make_shared<const MetadataSnapshot>(
            collectMetadata(*db,profile_,"public",token)));
        const NameResolutionContext names{profile_.database.toStdString(),"public",Dialect::PostgreSQL};
        const auto planned=[&](const std::string &sql) {
            const auto parsed=compile(Dialect::PostgreSQL,sql,{},&catalog,names);
            if (!parsed->ok()) throw std::runtime_error(parsed->diagnostics.front().message);
            auto plan=optimize(parsed->model);
            if (!plan.ok()) throw std::runtime_error(plan.diagnostics.front().message);
            return plan;
        };
        const std::string keep="public.aggregate_join_keep_owned AS l";
        const std::string drop="public.aggregate_join_drop_owned AS r";
        const std::string on=" ON l.k=r.k";
        const auto left=" FROM "+keep+" LEFT JOIN "+drop+on;
        const auto right=" FROM "+drop+" RIGHT JOIN "+keep+on;
        const std::string aggregates="SELECT l.g AS grp,MIN(l.v) AS low,MAX(l.v) AS high,"
                                     "COUNT(DISTINCT l.v) AS n,SUM(DISTINCT l.v) AS total";
        for (const auto &sql:{
                 aggregates+left+" GROUP BY 1 ORDER BY 1",
                 aggregates+right+" GROUP BY 1 ORDER BY 1",
                 "SELECT DISTINCT l.g AS grp"+left+" ORDER BY 1",
                 "SELECT l.g AS grp"+right+" GROUP BY 1 ORDER BY 1",
                 "SELECT MIN(l.v) AS empty_min,COUNT(DISTINCT l.v) AS n FROM "
                     "public.aggregate_join_empty_owned AS l LEFT JOIN "+drop+on}) {
            const auto plan=planned(sql);
            QVERIFY2(std::any_of(plan.rules.begin(),plan.rules.end(),[](const RuleEvent &entry) {
                return entry.rule=="HepAggregateJoinRemove";
            }),sql.c_str());
            const auto emitted=generatePlanSql(plan.logical,plan.heuristicRoot);
            QVERIFY2(emitted.ok(),emitted.diagnostics.empty()?sql.c_str():emitted.diagnostics.front().message.c_str());
            QVERIFY(emitted.sql.find("aggregate_join_drop_owned")==std::string::npos);
            const auto baseline=run(sql), rewritten=run(emitted.sql);
            QCOMPARE(rewritten.rows,baseline.rows);
            QCOMPARE(rewritten.columns,baseline.columns);
            QCOMPARE(rewritten.columnTypes,baseline.columnTypes);
        }
        for (const auto &sql:{"SELECT SUM(l.v) AS total"+left,
                              "SELECT COUNT(*) AS total"+right,
                              "SELECT MIN(r.k) AS low"+left,
                              "SELECT DISTINCT l.g AS grp,r.k AS other_key"+left,
                              "SELECT l.g AS grp,MAX(l.v) AS m FROM "+keep+" LEFT JOIN "+drop+
                                  " ON l.k=r.k OR l.k=7 GROUP BY 1"}) {
            const auto plan=planned(sql);
            QVERIFY(std::none_of(plan.rules.begin(),plan.rules.end(),[](const RuleEvent &entry) {
                return entry.rule=="HepAggregateJoinRemove";
            }));
        }
    }catch(const std::exception &e){QFAIL(e.what());}}
    void materializationRewritePreservesNativeResult() {try {
        using namespace navidog;
        using namespace navidog::sql::compiler;
        namespace opt=navidog::sql::optimizer;
        auto db=connectDatabase(profile_);
        auto token=cancellation();
        const auto run=[&](const QString &sql) {
            const auto result=db->execute(sql,{},token);
            if (!result.error.isEmpty()) throw std::runtime_error(result.error.toStdString());
            return result;
        };
        run("CREATE TABLE public.mv_source_owned(bucket INT, n INT)");
        run("INSERT INTO public.mv_source_owned VALUES (4,5),(5,1),(5,1),(6,NULL),(7,3),(8,4),(NULL,6)");
        const auto definition="CREATE MATERIALIZED VIEW public.mv_snapshot_owned (saved_n,saved_bucket) AS "
            "SELECT v.n,v.bucket FROM public.mv_source_owned v WHERE 5<=v.bucket AND v.bucket<9;";
        run(definition);
        const auto narrowDefinition="CREATE MATERIALIZED VIEW public.mv_snapshot_narrow_owned AS "
            "SELECT n,bucket FROM public.mv_source_owned WHERE bucket>=7 AND bucket<9";
        run(narrowDefinition);
        const auto snapshot=collectMetadata(*db,profile_,"public",token);
        const MetadataCatalog catalog(std::make_shared<const MetadataSnapshot>(snapshot));
        const NameResolutionContext context{profile_.database.toStdString(),"public",Dialect::PostgreSQL};
        const auto stored=catalog.findTable(context,{"public","mv_snapshot_owned"});
        QVERIFY(stored.has_value());
        const std::string scanView="SELECT saved_n AS n FROM public.mv_snapshot_owned ORDER BY 1 NULLS FIRST";
        const auto directScan=compile(context.dialect,scanView,{},&catalog,context);
        QVERIFY(directScan->ok());
        const auto scanned=opt::optimize(directScan->model);
        QVERIFY(scanned.ok());
        QVERIFY(std::any_of(scanned.logical.nodes.begin(),scanned.logical.nodes.end(),[](const auto &node) {
            return node.kind==opt::RelKind::Scan && node.scanSource==opt::ScanSource::MaterializedView;
        }));
        const auto sqlScan=opt::generatePlanSql(scanned.logical,scanned.logical.root);
        QVERIFY2(sqlScan.ok(),sqlScan.diagnostics.empty()?"missing scan SQL":sqlScan.diagnostics.front().message.c_str());
        QCOMPARE(run(QString::fromStdString(sqlScan.sql)).rows,run(QString::fromStdString(scanView)).rows);
        const auto compiledDefinition=compile(Dialect::PostgreSQL,definition,{},&catalog,context);
        QVERIFY2(compiledDefinition->ok(),compiledDefinition->diagnostics.empty()?"bad fixture":compiledDefinition->diagnostics.front().message.c_str());
        const std::string query="SELECT n,bucket AS category FROM public.mv_source_owned "
            "WHERE bucket>=5 AND bucket<8";
        const auto compiledQuery=compile(Dialect::PostgreSQL,query,{},&catalog,context);
        QVERIFY2(compiledQuery->ok(),compiledQuery->diagnostics.empty()?"bad fixture":compiledQuery->diagnostics.front().message.c_str());
        const auto rewritten=opt::rewriteMaterializedView(compiledQuery->model,compiledDefinition->model,*stored,
            opt::MaterializationSnapshot::Equivalent);
        QVERIFY2(rewritten.ok(),rewritten.diagnostics.empty()?"no rewrite":rewritten.diagnostics.front().message.c_str());
        const auto original=run(QString::fromStdString(query+" ORDER BY 2,1 NULLS LAST;"));
        const auto rewrittenPage=rewritten.sql.substr(0,rewritten.sql.rfind(';'))+" ORDER BY 2,1 NULLS LAST;";
        const auto result=run(QString::fromStdString(rewrittenPage));
        QCOMPARE(result.rows,original.rows);
        QCOMPARE(result.columns,original.columns);
        QCOMPARE(result.rows.size(),qsizetype(4));
        const std::string filteredScan="SELECT * FROM public.mv_source_owned WHERE bucket>=5 AND bucket<8";
        const auto scan=compile(context.dialect,filteredScan,{},&catalog,context);
        const auto compiledNarrow=compile(context.dialect,narrowDefinition,{},&catalog,context);
        const auto narrow=catalog.findTable(context,{"public","mv_snapshot_narrow_owned"});
        QVERIFY(scan->ok() && compiledNarrow->ok() && narrow);
        const auto candidates=opt::rewriteMaterializedViews(scan->model,{
            {&compiledNarrow->model,&*narrow,opt::MaterializationSnapshot::Equivalent},
            {&compiledDefinition->model,&*stored,opt::MaterializationSnapshot::Equivalent}},
            {},opt::MaterializationCoverage::AllowPartial);
        QVERIFY2(candidates.candidates.size()==2,candidates.diagnostics.empty()?"expected two rewrites":
                 candidates.diagnostics.front().message.c_str());
        QCOMPARE(candidates.candidates.front().registration,std::size_t(1));
        QCOMPARE(candidates.candidates.front().pattern,opt::MaterializationPattern::FilteredScan);
        const auto scanRows=run(QString::fromStdString(filteredScan+" ORDER BY 1,2 NULLS LAST"));
        for (const auto &candidate:candidates.candidates) {
            const auto observed=run(QString::fromStdString(
                candidate.sql.substr(0,candidate.sql.rfind(';'))+" ORDER BY 1,2 NULLS LAST"));
            QCOMPARE(observed.rows,scanRows.rows);
            QCOMPARE(observed.columns,scanRows.columns);
            QCOMPARE(observed.columnTypes,scanRows.columnTypes);
        }
        for (const auto &overlap : {
                std::string("SELECT m.n AS value,m.bucket FROM public.mv_source_owned m WHERE m.bucket<8"),
                std::string("SELECT m.n AS value,m.bucket FROM public.mv_source_owned m")}) {
            const auto candidate=compile(Dialect::PostgreSQL,overlap,{},&catalog,context);
            QVERIFY(candidate->ok());
            const auto compensated=opt::rewriteMaterializedView(
                candidate->model,compiledDefinition->model,*stored,opt::MaterializationSnapshot::Equivalent,
                {},opt::MaterializationCoverage::AllowPartial);
            QVERIFY2(compensated.ok(),compensated.diagnostics.empty()?
                "no partial rewrite":compensated.diagnostics.front().message.c_str());
            QVERIFY(compensated.sql.find("IS NOT TRUE")!=std::string::npos);
            const std::string order=" ORDER BY 2 NULLS LAST,1 NULLS LAST;";
            const auto nativeRows=run(QString::fromStdString(overlap+order));
            const auto mixedRows=run(QString::fromStdString(
                compensated.sql.substr(0,compensated.sql.rfind(';'))+order));
            QCOMPARE(mixedRows.rows,nativeRows.rows);
            QCOMPARE(mixedRows.columns,nativeRows.columns);
            QCOMPARE(mixedRows.columnTypes,nativeRows.columnTypes);
            QCOMPARE(mixedRows.rows.size(),overlap.find("WHERE")==std::string::npos?qsizetype(7):qsizetype(5));
        }
        run("DROP MATERIALIZED VIEW public.mv_snapshot_owned,public.mv_snapshot_narrow_owned");
        run("DROP TABLE public.mv_source_owned");
    }catch(const std::exception &e){QFAIL(e.what());}}
    void literalUnionFoldMatchesOwnedPostgres() {try {
        using namespace navidog;
        using namespace navidog::sql::compiler;
        using namespace navidog::sql::optimizer;
        auto db=connectDatabase(profile_);
        auto token=cancellation();
        const auto run=[&](const std::string &sql) {
            auto output=db->execute(QString::fromStdString(sql),{},token);
            if (!output.error.isEmpty()) throw std::runtime_error(output.error.toStdString());
            return output;
        };
        for (const auto &[source, count] : std::vector<std::pair<std::string,std::size_t>>{
                {"VALUES (1,2),(NULL,2),(-3,4) UNION ALL VALUES (1,2),(NULL,2),(5,6)",6},
                {"VALUES (1,2),(NULL,2),(-3,4) UNION VALUES (1,2),(NULL,2),(5,6)",4},
                {"VALUES (2147483648,1),(NULL,2) UNION ALL VALUES (2147483649,1),(NULL,2)",4}}) {
            const auto compiled=compile(Dialect::PostgreSQL,source);
            QVERIFY(compiled->ok());
            const auto plan=optimize(compiled->model);
            QVERIFY(plan.ok());
            QCOMPARE(plan.best.nodes[plan.best.root].kind,PhysicalKind::Values);
            const auto &folded=plan.logical.nodes.at(plan.best.nodes[plan.best.root].logical);
            QCOMPARE(folded.valueCount,count);
            QCOMPARE(folded.keys.size(),count*2);
            QVERIFY(std::any_of(plan.rules.begin(),plan.rules.end(),[](const auto &e){return e.rule=="UnionToValues";}));
            std::string replacement="VALUES ";
            for (std::size_t i=0;i<folded.keys.size();++i) {
                const auto &ref=folded.keys[i];
                const auto &expression=plan.logical.program.queries.at(ref.query-1).expressions.at(ref.expression);
                const auto range=expression.origin;
                replacement+=i==0 ? "(" : i%2==0 ? ",( " : ", ";
                replacement+=source.substr(range.begin,range.end-range.begin);
                if (i%2==1) replacement+=")";
            }
            const std::string sort=" ORDER BY 1 NULLS FIRST,2;";
            const auto original=run(source+sort), rewritten=run(replacement+sort);
            QCOMPARE(rewritten.rows,original.rows);
            QCOMPARE(rewritten.columns,original.columns);
            QCOMPARE(rewritten.columnTypes,original.columnTypes);
        }
        for (const auto *source : {
                "VALUES (1) UNION ALL VALUES (2147483648);",
                "VALUES (1 + 2) UNION ALL VALUES (3);",
                "VALUES ('abc') UNION VALUES ('ABC');",
                "VALUES (1),(2) INTERSECT VALUES (2);"}) {
            const auto compiled=compile(Dialect::PostgreSQL,source);
            QVERIFY(compiled->ok());
            const auto planned=optimize(compiled->model);
            QVERIFY(planned.ok());
            QVERIFY(std::none_of(planned.rules.begin(),planned.rules.end(),[](const auto &e){return e.rule=="UnionToValues";}));
        }
    }catch(const std::exception &e){QFAIL(e.what());}}
    void materializedAggregationRollupMatchesNativeRows() {try {
        using namespace navidog;
        using namespace navidog::sql::compiler;
        namespace opt=navidog::sql::optimizer;
        auto db=connectDatabase(profile_);
        auto token=cancellation();
        const auto run=[&](const QString &sql) {
            const auto result=db->execute(sql,{},token);
            if (!result.error.isEmpty()) throw std::runtime_error(result.error.toStdString());
            return result;
        };
        run("CREATE TABLE public.mv_agg_source_owned(region INT,shop INT,amount INT, wide BIGINT)");
        run("INSERT INTO public.mv_agg_source_owned VALUES "
            "(1,1,5,9223372036854775807),(1,1,5,9223372036854775807),(1,2,NULL,NULL),"
            "(1,2,3,-22),(2,1,NULL,NULL),(NULL,1,9,100),(1,NULL,400,-1000)");
        const auto ddl="CREATE MATERIALIZED VIEW public.mv_agg_owned (area,store_id,cnt,lowest,highest,nonnull,wide_sum) AS "
            "SELECT region,shop,count(*),min(amount),max(amount),count(amount),sum(wide) FROM public.mv_agg_source_owned "
            "WHERE shop IS NOT NULL GROUP BY region,shop;";
        run(ddl);
        const auto snapshot=collectMetadata(*db,profile_,"public",token);
        const MetadataCatalog catalog(std::make_shared<const MetadataSnapshot>(snapshot));
        const NameResolutionContext names{profile_.database.toStdString(),"public",Dialect::PostgreSQL};
        const auto stored=catalog.findTable(names,{"public","mv_agg_owned"});
        QVERIFY(stored.has_value());
        const auto compiledDefinition=compile(names.dialect,ddl,{},&catalog,names);
        QVERIFY2(compiledDefinition->ok(),compiledDefinition->diagnostics.empty()?
                 "invalid view":compiledDefinition->diagnostics.front().message.c_str());
        const auto rewrite=[&](const std::string &sql, opt::MaterializationCoverage coverage=opt::MaterializationCoverage::FullOnly) {
            const auto compiled=compile(names.dialect,sql,{},&catalog,names);
            if (!compiled->ok()) throw std::runtime_error(compiled->diagnostics.front().message);
            return opt::rewriteMaterializedView(compiled->model,compiledDefinition->model,*stored,
                opt::MaterializationSnapshot::Equivalent,{},coverage);
        };
        for (const auto &[source, order, cardinality] : std::vector<std::tuple<std::string,std::string,qsizetype>>{
            {"SELECT region AS location,COUNT(*),MIN(amount) AS lo,MAX(amount) AS hi,COUNT(amount),SUM(wide) "
             "FROM public.mv_agg_source_owned WHERE shop IS NOT NULL GROUP BY region",
             " ORDER BY 1 NULLS LAST;",3},
            {"SELECT region,COUNT(*) FROM public.mv_agg_source_owned WHERE shop IS NOT NULL AND region>=1 GROUP BY region",
             " ORDER BY 1;",2},
            {"SELECT COUNT(*) AS n, MIN(amount), MAX(amount), COUNT(amount), SUM(wide) FROM public.mv_agg_source_owned "
             "WHERE shop IS NOT NULL AND region=999",";",1}}) {
            const auto generated=rewrite(source);
            QVERIFY2(generated.ok(),generated.diagnostics.empty()?
                     "no aggregate rewrite":generated.diagnostics.front().message.c_str());
            const auto rewritten=run(QString::fromStdString(generated.sql.substr(0,generated.sql.rfind(';'))+order));
            const auto original=run(QString::fromStdString(source+order));
            QCOMPARE(rewritten.rows,original.rows);
            QCOMPARE(rewritten.columns,original.columns);
            QCOMPARE(rewritten.columnTypes,original.columnTypes);
            QCOMPARE(rewritten.rows.size(),cardinality);
        }
        for (const auto &[source, order, cardinality] : std::vector<std::tuple<std::string,std::string,qsizetype>>{
            {"SELECT region AS location,region AS location,COUNT(*),MIN(amount),MAX(amount),COUNT(amount),SUM(wide) "
             "FROM public.mv_agg_source_owned GROUP BY region", " ORDER BY 1 NULLS LAST;",3},
            {"SELECT COUNT(*),SUM(wide) FROM public.mv_agg_source_owned GROUP BY region,shop",
             " ORDER BY 1,2 NULLS LAST;",5},
            {"SELECT region AS location FROM public.mv_agg_source_owned WHERE region>=1 GROUP BY region",
             " ORDER BY 1;",2},
            {"SELECT COUNT(*) AS n, MIN(amount), MAX(amount), COUNT(amount), SUM(wide) FROM public.mv_agg_source_owned "
             "WHERE region=999",";",1},
            {"SELECT COUNT(*),COUNT(amount),SUM(wide) FROM public.mv_agg_source_owned", ";",1}}) {
            QVERIFY(!rewrite(source).ok());
            const auto generated=rewrite(source,opt::MaterializationCoverage::AllowPartial);
            QVERIFY2(generated.ok(),generated.diagnostics.empty()?
                     "no aggregate compensation":generated.diagnostics.front().message.c_str());
            QVERIFY(generated.sql.find("UNION ALL")!=std::string::npos);
            QVERIFY(generated.sql.find("IS NOT TRUE")!=std::string::npos);
            const auto rewritten=run(QString::fromStdString(generated.sql.substr(0,generated.sql.rfind(';'))+order));
            const auto original=run(QString::fromStdString(source+order));
            QCOMPARE(rewritten.rows,original.rows);
            QCOMPARE(rewritten.columns,original.columns);
            QCOMPARE(rewritten.columnTypes,original.columnTypes);
            QCOMPARE(rewritten.rows.size(),cardinality);
        }
        for (const auto *sql : {
                "SELECT COUNT(*) FROM public.mv_agg_source_owned WHERE shop IS NULL;",
                "SELECT COUNT(*) FROM public.mv_agg_source_owned WHERE shop IS NOT NULL AND amount>=3;",
                "SELECT COUNT(*) FROM public.mv_agg_source_owned WHERE amount>=3;",
                "SELECT COUNT(DISTINCT amount) FROM public.mv_agg_source_owned WHERE shop IS NOT NULL;",
                "SELECT SUM(amount) FROM public.mv_agg_source_owned WHERE shop IS NOT NULL;",
                "SELECT amount,COUNT(*) FROM public.mv_agg_source_owned WHERE shop IS NOT NULL GROUP BY amount;",
                "SELECT region FROM public.mv_agg_source_owned WHERE shop IS NOT NULL;"}) {
            const auto refused=rewrite(sql,opt::MaterializationCoverage::AllowPartial);
            QVERIFY2(!refused.ok() && refused.sql.empty(),sql);
        }
        run("DROP MATERIALIZED VIEW public.mv_agg_owned");
        run("DROP TABLE public.mv_agg_source_owned");
    }catch(const std::exception &e){QFAIL(e.what());}}
    void materializedInnerJoinPreservesRowsAndColumnOwnership() {try {
        using namespace navidog;
        using namespace navidog::sql::compiler;
        namespace opt=navidog::sql::optimizer;
        auto db=connectDatabase(profile_);
        const auto token=cancellation();
        const auto run=[&](const QString &sql) {
            const auto result=db->execute(sql,{},token);
            if (!result.error.isEmpty()) throw std::runtime_error(result.error.toStdString());
            return result;
        };
        run("CREATE TABLE public.mv_join_left_owned(k INT, shard INT, amount INT)");
        run("CREATE TABLE public.mv_join_right_owned(k INT, shard INT, amount INT)");
        run("INSERT INTO public.mv_join_left_owned VALUES (1,1,12),(1,1,12),(1,2,25),"
            "(1,1,50),(2,NULL,22),(NULL,2,27),(8,2,NULL)");
        run("INSERT INTO public.mv_join_right_owned VALUES (1,1,8),(1,1,6),(1,2,9),"
            "(1,2,1),(2,NULL,11),(NULL,2,14),(8,2,20)");
        const auto ddl="CREATE MATERIALIZED VIEW public.mv_inner_owned (stored_left,stored_right) AS "
            "SELECT a.amount,b.amount FROM public.mv_join_left_owned a JOIN public.mv_join_right_owned b "
            "ON a.k=b.k AND a.shard=b.shard WHERE b.amount>=5;";
        run(ddl);
        const auto snapshot=collectMetadata(*db,profile_,"public",token);
        const MetadataCatalog catalog(std::make_shared<const MetadataSnapshot>(snapshot));
        const NameResolutionContext scope{profile_.database.toStdString(),"public",Dialect::PostgreSQL};
        const auto storage=catalog.findTable(scope,{"public","mv_inner_owned"});
        QVERIFY(storage.has_value());
        const auto definition=compile(scope.dialect,ddl,{},&catalog,scope);
        QVERIFY(definition->ok());
        const auto rewrite=[&](const std::string &sql) {
            const auto query=compile(scope.dialect,sql,{},&catalog,scope);
            if (!query->ok()) throw std::runtime_error(query->diagnostics.front().message);
            return opt::rewriteMaterializedView(query->model,definition->model,*storage,
                opt::MaterializationSnapshot::Equivalent,{},opt::MaterializationCoverage::AllowPartial);
        };
        const std::string start="SELECT x.amount AS original_left,y.amount AS original_right "
            "FROM public.mv_join_left_owned x ";
        const std::string relation="JOIN public.mv_join_right_owned y ";
        const std::string swapped="SELECT x.amount AS original_left,y.amount AS original_right "
            "FROM public.mv_join_right_owned y JOIN public.mv_join_left_owned x ";
        const std::string conditions="ON y.shard=x.shard AND y.k=x.k WHERE y.amount>=7 AND x.amount<50";
        const auto joined=compile(scope.dialect,start+relation+conditions,{},&catalog,scope);
        QVERIFY(joined->ok());
        const auto candidates=opt::rewriteMaterializedViews(joined->model,{
            {&definition->model,&*storage,opt::MaterializationSnapshot::Equivalent}});
        QVERIFY(candidates.candidates.size()==1 &&
                candidates.candidates.front().pattern==opt::MaterializationPattern::ProjectJoin);
        for (const auto &valid : {start+relation+conditions, swapped+conditions}) {
            const auto generated=rewrite(valid);
            QVERIFY2(generated.ok(),generated.diagnostics.empty()?
                "join rewrite unavailable":generated.diagnostics.front().message.c_str());
            QVERIFY(generated.sql.find("stored_left")!=std::string::npos &&
                    generated.sql.find("stored_right")!=std::string::npos && generated.sql.find("JOIN")==std::string::npos);
            const std::string order=" ORDER BY 1,2";
            const auto original=run(QString::fromStdString(valid+order));
            const auto rewritten=run(QString::fromStdString(generated.sql.substr(0,generated.sql.rfind(';'))+order));
            QCOMPARE(rewritten.rows,original.rows);
            QCOMPARE(rewritten.columns,original.columns);
            QCOMPARE(rewritten.columnTypes,original.columnTypes);
            QCOMPARE(rewritten.rows.size(),qsizetype(3));
        }
        for(const auto& partial:{
                start+relation+"ON y.shard=x.shard AND y.k=x.k WHERE x.amount>=5",
                swapped+"ON y.shard=x.shard AND y.k=x.k WHERE x.amount>=7"}) {
            const auto suggested=rewrite(partial);
            QVERIFY2(suggested.ok(),suggested.diagnostics.empty()?"no compensation":suggested.diagnostics.front().message.c_str());
            QVERIFY(suggested.sql.find("IS NOT TRUE")!=std::string::npos);
            const std::string order=" ORDER BY 1,2";
            const auto original=run(QString::fromStdString(partial+order));
            const auto actual=run(QString::fromStdString(suggested.sql.substr(0,suggested.sql.rfind(';'))+order));
            QCOMPARE(actual.rows,original.rows);
            QCOMPARE(actual.columns,original.columns);
            QCOMPARE(actual.columnTypes,original.columnTypes);
        }
        for (const auto &unsafe : {
                start+"LEFT "+relation+"ON y.shard=x.shard AND y.k=x.k WHERE y.amount>=7",
                start+relation+"ON y.k=x.k WHERE y.amount>=7",
                swapped+"ON y.shard=x.k AND y.k=x.shard WHERE y.amount>=7",
                start+relation+"ON y.shard=x.shard AND y.k=x.k WHERE y.amount>=7 AND x.k=1"}) {
            const auto refused=rewrite(unsafe);
            QVERIFY(!refused.ok() && refused.sql.empty());
        }
        run("DROP MATERIALIZED VIEW public.mv_inner_owned");
        run("DROP TABLE public.mv_join_left_owned,public.mv_join_right_owned");
    }catch(const std::exception &e){QFAIL(e.what());}}
    void starAggregateTilesPreserveRowsAndMeasureTypes(){try {
        using namespace navidog;
        using namespace navidog::sql::compiler;
        namespace opt=navidog::sql::optimizer;
        auto db=connectDatabase(profile_);
        const auto token=cancellation();
        const auto run=[&](const std::string &sql) {
            const auto output=db->execute(QString::fromStdString(sql),{},token);
            if (!output.error.isEmpty()) throw std::runtime_error(output.error.toStdString());
            return output;
        };
        run("CREATE TABLE public.star_fact_owned(store_id integer,product_id integer,amount bigint)");
        run("CREATE TABLE public.star_store_owned(id integer PRIMARY KEY,region integer)");
        run("CREATE TABLE public.star_product_owned(id integer PRIMARY KEY,category integer)");
        run("INSERT INTO public.star_store_owned VALUES (1,1),(2,NULL),(3,1)");
        run("INSERT INTO public.star_product_owned VALUES (1,5),(2,7),(3,NULL)");
        run("INSERT INTO public.star_fact_owned VALUES (1,1,9223372036854775807),"
            "(1,1,9223372036854775807),(2,2,NULL),(3,3,1),(NULL,1,100),(1,9,99)");
        const std::string definition="CREATE MATERIALIZED VIEW public.star_tile_owned AS "
            "SELECT s.region,p.category,COUNT(*) AS n,SUM(f.amount) AS amount "
            "FROM public.star_fact_owned f JOIN public.star_store_owned s ON f.store_id=s.id "
            "JOIN public.star_product_owned p ON f.product_id=p.id GROUP BY s.region,p.category";
        run(definition);
        const auto snapshot=std::make_shared<const MetadataSnapshot>(collectMetadata(*db,profile_,"public",token));
        const MetadataCatalog catalog(snapshot);
        const NameResolutionContext names{profile_.database.toStdString(),"public",Dialect::PostgreSQL};
        const auto view=compile(names.dialect,definition,{},&catalog,names);
        QVERIFY2(view->ok(),view->diagnostics.empty()?"no view":view->diagnostics.front().message.c_str());
        const auto storage=catalog.findTable(names,{"public","star_tile_owned"});
        QVERIFY(storage);
        const std::string from=" FROM public.star_product_owned p JOIN public.star_fact_owned f ON p.id=f.product_id "
            "JOIN public.star_store_owned s ON s.id=f.store_id";
        const auto verify=[&](const std::string &query,const std::string &order,
                              opt::MaterializationPattern pattern=opt::MaterializationPattern::StarAggregate) {
            const auto parsed=compile(names.dialect,query,{},&catalog,names);
            QVERIFY2(parsed->ok(),parsed->diagnostics.empty()?query.c_str():parsed->diagnostics.front().message.c_str());
            const auto search=opt::rewriteMaterializedViews(parsed->model,{
                {&view->model,&*storage,opt::MaterializationSnapshot::Equivalent}});
            QVERIFY2(!search.candidates.empty(),search.diagnostics.empty()?query.c_str():search.diagnostics.front().message.c_str());
            const auto &candidate=search.candidates.front();
            QCOMPARE(candidate.pattern,pattern);
            QVERIFY(candidate.sql.find("star_tile_owned")!=std::string::npos);
            QVERIFY(candidate.sql.find(" JOIN ")==std::string::npos);
            const auto seen=run(candidate.sql.substr(0,candidate.sql.rfind(';'))+order);
            const auto expected=run(query+order);
            QCOMPARE(seen.rows,expected.rows);
            QCOMPARE(seen.columns,expected.columns);
            QCOMPARE(seen.columnTypes,expected.columnTypes);
        };
        verify("SELECT s.region AS area,COUNT(*) AS n,SUM(f.amount) AS amount"+from+" GROUP BY s.region",
               " ORDER BY 1 NULLS LAST");
        verify("SELECT s.region AS area,p.category AS cat,COUNT(*) AS n,SUM(f.amount) AS amount"+from+
               " GROUP BY s.region,p.category"," ORDER BY 1 NULLS LAST,2 NULLS FIRST");
        verify("SELECT COUNT(*) AS n,SUM(f.amount) AS amount"+from+" WHERE s.region>=99","");
        verify("SELECT t.area,COUNT(*) AS n,SUM(t.total) AS amount FROM "
               "(SELECT s.region AS area,f.amount AS total"+from+") t "
               "GROUP BY t.area"," ORDER BY 1 NULLS LAST",opt::MaterializationPattern::StarProjectAggregate);
        const auto distinct=compile(names.dialect,
            "SELECT COUNT(*) FROM (SELECT DISTINCT s.region"+from+") t",{},&catalog,names);
        QVERIFY(distinct->ok());
        QVERIFY(opt::rewriteMaterializedViews(distinct->model,{
            {&view->model,&*storage,opt::MaterializationSnapshot::Equivalent}}).candidates.empty());
        const auto changed=compile(names.dialect,"SELECT COUNT(*)"+
            from.substr(0,from.rfind("f.store_id"))+"f.product_id",{},&catalog,names);
        QVERIFY(changed->ok());
        QVERIFY(opt::rewriteMaterializedViews(changed->model,{
            {&view->model,&*storage,opt::MaterializationSnapshot::Equivalent}}).candidates.empty());
        run("DROP MATERIALIZED VIEW public.star_tile_owned");
        run("DROP TABLE public.star_fact_owned,public.star_store_owned,public.star_product_owned");
    }catch(const std::exception &e){QFAIL(e.what());}}
    void joinedMaterializedAggregatesRecombineUncoveredRowsInOwnedPostgres(){try {
        using namespace navidog;
        using namespace navidog::sql::compiler;
        namespace opt=navidog::sql::optimizer;
        auto db=connectDatabase(profile_);
        auto token=cancellation();
        const auto run=[&](const std::string& sql) {
            auto result=db->execute(QString::fromStdString(sql),{},token);
            if(!result.error.isEmpty())throw std::runtime_error(result.error.toStdString());
            return result;
        };
        run("CREATE TABLE public.owned_mv_stores(id integer, region integer, active integer)");
        run("CREATE TABLE public.owned_mv_sales(store_id integer, channel integer, amount integer, wide bigint)");
        run("INSERT INTO public.owned_mv_stores VALUES (1,1,1),(1,1,1),(2,1,NULL),(3,2,0),(4,NULL,1),(5,3,1)");
        run("INSERT INTO public.owned_mv_sales VALUES (1,1,5,9223372036854775807),(1,1,5,9223372036854775807),"
            "(1,NULL,NULL,NULL),(2,1,10,20),(3,2,3,-12),(4,1,NULL,NULL),(9,1,5,100)");
        const std::string join=" FROM public.owned_mv_stores c JOIN public.owned_mv_sales o ON c.id=o.store_id";
        const std::string aggregate="COUNT(*),COUNT(o.amount),MIN(o.amount),MAX(o.amount),SUM(o.wide)";
        const std::string groupedDdl="CREATE MATERIALIZED VIEW public.owned_mv_grouped "
            "(saved_region,saved_channel,row_ct,val_ct,lo,hi,exact_total) AS SELECT c.region,o.channel,"+
            aggregate+join+" WHERE c.active>=1 GROUP BY c.region,o.channel;";
        const std::string globalDdl="CREATE MATERIALIZED VIEW public.owned_mv_global "
            "(row_ct,val_ct,lo,hi,exact_total) AS SELECT "+aggregate+join+" WHERE c.active>=1;";
        run(groupedDdl);
        run(globalDdl);
        auto snapshot=std::make_shared<const MetadataSnapshot>(collectMetadata(*db,profile_,"public",token));
        MetadataCatalog catalog(snapshot);
        const NameResolutionContext names{profile_.database.toStdString(),"public",Dialect::PostgreSQL};
        const auto compileSql=[&](const std::string& sql) {
            auto parsed=compile(Dialect::PostgreSQL,sql,{},&catalog,names);
            if(!parsed->ok())throw std::runtime_error(parsed->diagnostics.front().message);
            return parsed;
        };
        auto grouped=compileSql(groupedDdl);
        auto global=compileSql(globalDdl);
        auto groupedStore=catalog.findTable(names,{"public","owned_mv_grouped"});
        auto globalStore=catalog.findTable(names,{"public","owned_mv_global"});
        QVERIFY(groupedStore && globalStore);
        // The query and the stored definition deliberately use opposite join orders.
        const std::string source=" FROM public.owned_mv_sales o JOIN public.owned_mv_stores c ON o.store_id=c.id";
        const std::string projection="SELECT c.region AS area,"+aggregate+source;
        const auto check=[&](const std::string& query,bool partial,bool withGrouping,
                             const Compilation& view,const navidog::sql::compiler::TableDefinition& store) {
            auto compiled=compileSql(query);
            const auto rewritten=opt::rewriteMaterializedView(compiled->model,view.model,store,
                opt::MaterializationSnapshot::Equivalent,{},partial?opt::MaterializationCoverage::AllowPartial:
                                                                    opt::MaterializationCoverage::FullOnly);
            QVERIFY2(rewritten.ok(),rewritten.diagnostics.empty()?"no rewrite":rewritten.diagnostics.front().message.c_str());
            QCOMPARE(rewritten.sql.find("UNION ALL")!=std::string::npos,partial);
            if(partial)QVERIFY(rewritten.sql.find("IS NOT TRUE")!=std::string::npos);
            const std::string ordered=withGrouping?" ORDER BY 1 NULLS FIRST":"";
            const auto expected=run(query+ordered);
            const auto actual=run(rewritten.sql.substr(0,rewritten.sql.rfind(';'))+ordered);
            QCOMPARE(actual.rows,expected.rows);
            QCOMPARE(actual.columns,expected.columns);
            QCOMPARE(actual.columnTypes,expected.columnTypes);
        };
        check(projection+" WHERE c.active>=1 AND c.region>=1 GROUP BY c.region",false,true,*grouped,*groupedStore);
        check(projection+" GROUP BY c.region",true,true,*grouped,*groupedStore);
        check("SELECT "+aggregate+source+" WHERE c.region=999",true,false,*grouped,*groupedStore);
        check("SELECT "+aggregate+source+" WHERE c.active>=1",false,false,*global,*globalStore);
        check("SELECT "+aggregate+source,true,false,*global,*globalStore);
        for(const auto& invalid:std::vector<std::string>{
            projection+" WHERE o.amount>=0 GROUP BY c.region",
            "SELECT COUNT(DISTINCT o.amount)"+source,
            "SELECT COUNT(*) FROM public.owned_mv_sales o LEFT JOIN public.owned_mv_stores c ON o.store_id=c.id",
            "SELECT COUNT(*) FROM public.owned_mv_sales o JOIN public.owned_mv_stores c ON o.store_id=c.region"}) {
            auto query=compileSql(invalid);
            QVERIFY(!opt::rewriteMaterializedView(query->model,grouped->model,*groupedStore,
                opt::MaterializationSnapshot::Equivalent,{},opt::MaterializationCoverage::AllowPartial).ok());
        }
    }catch(const std::exception &e){QFAIL(e.what());}}
    void federatedJoinReadsExplicitPostgresAndSQLiteSessions() {try {
        using namespace navidog;
        using namespace navidog::sql::compiler;
        using namespace navidog::sql::optimizer;
        QTemporaryDir sqliteFiles;
        QVERIFY(sqliteFiles.isValid());
        ConnectionProfile sqliteProfile;
        sqliteProfile.driver=DatabaseKind::SQLite;
        sqliteProfile.database=sqliteFiles.filePath("federation.sqlite");
        {
            QFile file(sqliteProfile.database);
            QVERIFY(file.open(QIODevice::WriteOnly));
        }
        auto pg=connectDatabase(profile_);
        auto sqlite=connectDatabase(sqliteProfile);
        auto token=cancellation();
        const auto run=[&](Database &connection, const QString &sql) {
            const auto result=connection.execute(sql,{},token);
            if (!result.error.isEmpty()) throw std::runtime_error(result.error.toStdString());
        };
        run(*pg,"CREATE TABLE public.federation_pg_owned (k INT, enabled BOOLEAN, unused TEXT)");
        run(*pg,QString::fromUtf8("INSERT INTO public.federation_pg_owned VALUES (1,TRUE,'武汉'),(2,FALSE,NULL),(NULL,TRUE,'')"));
        run(*sqlite,"CREATE TABLE federation_sqlite_owned(k INTEGER,v INTEGER,unused TEXT)");
        run(*sqlite,"INSERT INTO federation_sqlite_owned VALUES"
                    "(1,4,'a'),(1,4,'b'),(1,8,'c'),(NULL,7,'d'||char(0)||'返')");
        const auto integer=std::make_shared<const SqlType>(*catalogTypeFromText(Dialect::PostgreSQL,"INTEGER"));
        const auto flag=std::make_shared<const SqlType>(*catalogTypeFromText(Dialect::PostgreSQL,"BOOLEAN"));
        const auto text=std::make_shared<const SqlType>(*catalogTypeFromText(Dialect::PostgreSQL,"TEXT"));
        navidog::sql::compiler::TableDefinition west;
        west.database="pg_source";west.schema="demo";west.name="shared";
        west.columns={{"k",ValueType::Integer,integer},{"enabled",ValueType::Boolean,flag},
                      {"unused",ValueType::String,text}};
        auto east=west;
        east.database="sqlite_source";
        const auto sqliteInteger=std::make_shared<const SqlType>(*catalogTypeFromText(Dialect::PostgreSQL,"BIGINT"));
        east.columns={{"k",ValueType::Integer,sqliteInteger},{"v",ValueType::Integer,sqliteInteger},
                      {"unused",ValueType::String,text}};
        MemoryCatalog catalog;
        catalog.tables={west,east};
        const NameResolutionContext scope{"pg_source","demo",Dialect::PostgreSQL};
        const std::vector<FederatedTableBinding> bindings{
            {"pg_source","demo","shared",DatabaseKind::PostgreSQL,pg.get(),"public","federation_pg_owned"},
            {"sqlite_source","demo","shared",DatabaseKind::SQLite,sqlite.get(),"main","federation_sqlite_owned"}};
        const auto query=compile(scope.dialect,
            "SELECT p.k,p.enabled,s.v FROM pg_source.demo.shared p LEFT JOIN sqlite_source.demo.shared s "
            "ON p.k=s.k ORDER BY 1,3;",{},&catalog,scope);
        QVERIFY2(query->ok(),query->diagnostics.empty()?"bad fixture":query->diagnostics.front().message.c_str());
        const auto result=executeFederatedQuery(query->model,bindings,token);
        QVERIFY2(result.ok(),result.diagnostics.empty()?"no rows":result.diagnostics.front().message.c_str());
        const auto number=[](std::int64_t value){return LocalValue{value};};
        const LocalValue null;
        const std::vector<LocalRow> expected{{number(1),true,number(4)},{number(1),true,number(4)},
            {number(1),true,number(8)},{number(2),false,null},{null,true,null}};
        QVERIFY(result.rows==expected);
        QUrl endpoint("postgresql:///"+profile_.database);
        QUrlQuery params;
        params.addQueryItem("host",profile_.host);
        params.addQueryItem("port",QString::number(profile_.port));
        params.addQueryItem("user",profile_.user);
        endpoint.setQuery(params);
        QString command=QCoreApplication::applicationDirPath()+"/navidog-federate";
#ifdef Q_OS_WIN
        command += ".exe";
#endif
        QProcess cli;
        cli.start(command,{"--postgres","pg_source="+endpoint.toString(QUrl::FullyEncoded),
                           "--sqlite","sqlite_source="+sqliteProfile.database,
                           "--table","pg_source.public.federation_pg_owned",
                           "--table","sqlite_source.main.federation_sqlite_owned","-"});
        QVERIFY2(cli.waitForStarted(5000),qPrintable(cli.errorString()));
        cli.write("SELECT p.k,p.enabled,s.v,p.unused,s.unused FROM pg_source.public.federation_pg_owned p "
                  "LEFT JOIN sqlite_source.main.federation_sqlite_owned s ON p.k=s.k ORDER BY 1,3;");
        cli.closeWriteChannel();
        QVERIFY(cli.waitForFinished(10000));
        const auto cliError=cli.readAllStandardError();
        const auto cliOutput=cli.readAllStandardOutput();
        QVERIFY2(cli.exitStatus()==QProcess::NormalExit && cli.exitCode()==0,
                 (cliError+cliOutput).constData());
        const auto decoded=QJsonDocument::fromJson(cliOutput).object();
        QCOMPARE(decoded["rows"],QJsonDocument::fromJson(
            "[[1,true,4,\"武汉\",\"a\"],[1,true,4,\"武汉\",\"b\"],[1,true,8,\"武汉\",\"c\"],"
            "[2,false,null,null,null],[null,true,null,\"\",null]]").array());
        InterpreterLimits bounded;bounded.maxRows=3;
        const auto overflow=executeFederatedQuery(query->model,bindings,token,bounded);
        QVERIFY(!overflow.ok() && overflow.rows.empty() && overflow.diagnostics.front().code==DiagnosticCode::ResourceLimit);
        auto duplicate=bindings;duplicate.push_back(duplicate.front());
        const auto ambiguous=executeFederatedQuery(query->model,duplicate,token);
        QVERIFY(!ambiguous.ok() && ambiguous.rows.empty() && ambiguous.diagnostics.front().code==DiagnosticCode::InvalidQueryShape);
        bounded.maxRows=1;
        const auto pushed=compile(scope.dialect,
            "SELECT k FROM pg_source.demo.shared WHERE 2>k AND k IS NOT NULL;",{},&catalog,scope);
        QVERIFY(pushed->ok());
        const auto filtered=executeFederatedQuery(pushed->model,bindings,token,bounded);
        const std::vector<LocalRow> firstKey{{number(1)}};
        QVERIFY2(filtered.ok(),filtered.diagnostics.empty()?"no result":filtered.diagnostics.front().message.c_str());
        QVERIFY(filtered.rows==firstKey);
        bounded.maxRows=2;
        for (const auto &[sql, expected] : std::vector<std::pair<std::string,std::int64_t>>{
                {"SELECT k AS result FROM pg_source.demo.shared ORDER BY 1 DESC NULLS FIRST LIMIT 1 OFFSET 1;",2},
                {"SELECT k AS result FROM pg_source.demo.shared WHERE k<3 AND k IS NOT NULL "
                 "ORDER BY result DESC NULLS LAST LIMIT 1 OFFSET 1;",1}}) {
            const auto page=compile(scope.dialect,sql,{},&catalog,scope);
            QVERIFY(page->ok());
            const auto result=executeFederatedQuery(page->model,bindings,token,bounded);
            QVERIFY2(result.ok(),result.diagnostics.empty()?"no page":result.diagnostics.front().message.c_str());
            QCOMPARE(result.columns,std::vector<std::string>{"result"});
            const std::vector<LocalRow> expectedPage{{number(expected)}};
            QVERIFY(result.rows==expectedPage);
        }
        bounded.maxRows=1;
        for (const auto *sql : {
                "SELECT k FROM pg_source.demo.shared ORDER BY 1 FETCH FIRST 1 ROW WITH TIES;",
                "SELECT k,enabled FROM pg_source.demo.shared ORDER BY 1 LIMIT 1;",
                "SELECT k FROM pg_source.demo.shared WHERE k=1 OR k IS NULL ORDER BY 1 LIMIT 1;"}) {
            const auto page=compile(scope.dialect,sql,{},&catalog,scope);
            QVERIFY(page->ok());
            const auto result=executeFederatedQuery(page->model,bindings,token,bounded);
            QVERIFY2(!result.ok() && result.rows.empty() && result.diagnostics.front().code==DiagnosticCode::ResourceLimit,sql);
        }
        const auto count=compile(scope.dialect,"SELECT COUNT(*) FROM pg_source.demo.shared;",{},&catalog,scope);
        QVERIFY(count->ok());
        const auto counted=executeFederatedQuery(count->model,bindings,token);
        const std::vector<LocalRow> expectedCount{{number(3)}};
        QVERIFY2(counted.ok(),counted.diagnostics.empty()?"no result":counted.diagnostics.front().message.c_str());
        QVERIFY(counted.rows==expectedCount);
        const auto star=compile(scope.dialect,"SELECT * FROM pg_source.demo.shared ORDER BY k;",{},&catalog,scope);
        QVERIFY(star->ok());
        const auto projectedText=executeFederatedQuery(star->model,bindings,token);
        const std::vector<LocalRow> expectedText{{number(1),true,LocalText("武汉")},
            {number(2),false,null},{null,true,LocalText("")}};
        QVERIFY2(projectedText.ok(),projectedText.diagnostics.empty()?"no text":projectedText.diagnostics.front().message.c_str());
        QVERIFY(projectedText.rows==expectedText);
        const auto embedded=compile(scope.dialect,"SELECT unused FROM sqlite_source.demo.shared WHERE k IS NULL;",{},&catalog,scope);
        QVERIFY(embedded->ok());
        const auto sqliteText=executeFederatedQuery(embedded->model,bindings,token);
        const std::vector<LocalRow> expectedEmbedded{{LocalText(std::string("d\0返",5))}};
        QVERIFY2(sqliteText.ok(),sqliteText.diagnostics.empty()?"no text":sqliteText.diagnostics.front().message.c_str());
        QVERIFY(sqliteText.rows==expectedEmbedded);
        QVERIFY(dumpInterpretation(sqliteText).find("d\\u0000返")!=std::string::npos);
        const auto textCount=compile(scope.dialect,
            "SELECT COUNT(unused) FROM pg_source.demo.shared WHERE unused IS NOT NULL;",{},&catalog,scope);
        QVERIFY(textCount->ok());
        const auto countedText=executeFederatedQuery(textCount->model,bindings,token);
        QVERIFY2(countedText.ok(),countedText.diagnostics.empty()?"no count":countedText.diagnostics.front().message.c_str());
        QVERIFY(countedText.rows==std::vector<LocalRow>{{number(2)}});
        for (const auto *sql : {
                "SELECT unused FROM pg_source.demo.shared WHERE unused=unused;",
                "SELECT unused FROM pg_source.demo.shared ORDER BY unused;",
                "SELECT DISTINCT unused FROM pg_source.demo.shared;",
                "SELECT COUNT(*) FROM pg_source.demo.shared GROUP BY unused;",
                "SELECT COUNT(DISTINCT unused) FROM pg_source.demo.shared;"}) {
            const auto comparison=compile(scope.dialect,sql,{},&catalog,scope);
            QVERIFY2(comparison->ok(),sql);
            const auto rejected=executeFederatedQuery(comparison->model,bindings,token);
            QVERIFY2(!rejected.ok() && rejected.rows.empty() &&
                     rejected.diagnostics.front().message.find("verified collation")!=std::string::npos,sql);
        }
        run(*sqlite,"INSERT INTO federation_sqlite_owned VALUES(NULL,8,CAST(X'ff' AS TEXT));");
        const auto invalidText=executeFederatedQuery(embedded->model,bindings,token);
        QVERIFY(!invalidText.ok() && invalidText.rows.empty());
        QVERIFY(invalidText.diagnostics.front().message.find("invalid client encoding")!=std::string::npos);
        run(*sqlite,"UPDATE federation_sqlite_owned SET unused=X'ff' WHERE k IS NULL AND v=8;");
        const auto invalidStorage=executeFederatedQuery(embedded->model,bindings,token);
        QVERIFY(!invalidStorage.ok() && invalidStorage.rows.empty());
        QVERIFY(invalidStorage.diagnostics.front().message.find("text storage value")!=std::string::npos);
        run(*sqlite,"UPDATE federation_sqlite_owned SET unused=printf('%.*c',4097,'z') WHERE k IS NULL AND v=8;");
        const auto tooLong=executeFederatedQuery(embedded->model,bindings,token);
        QVERIFY(!tooLong.ok() && tooLong.rows.empty() && tooLong.diagnostics.front().code==DiagnosticCode::ResourceLimit);
        run(*sqlite,"DELETE FROM federation_sqlite_owned WHERE k IS NULL AND v=8;");
        const auto local=compile(scope.dialect,
            "SELECT k FROM sqlite_source.demo.shared WHERE k IS NULL;",{},&catalog,scope);
        QVERIFY(local->ok());
        const auto unsupportedPush=executeFederatedQuery(local->model,bindings,token,bounded);
        QVERIFY(!unsupportedPush.ok() && unsupportedPush.rows.empty() &&
                unsupportedPush.diagnostics.front().code==DiagnosticCode::ResourceLimit);
        run(*pg,"INSERT INTO public.federation_pg_owned SELECT i,true,'unused' FROM generate_series(10,409) i");
        run(*sqlite,"WITH RECURSIVE n(k) AS (VALUES(10) UNION ALL SELECT k+1 FROM n WHERE k<409) "
                    "INSERT INTO federation_sqlite_owned SELECT k,k*2,'unused' FROM n");
        InterpreterLimits indexedBudget;
        indexedBudget.maxSteps=60000;
        const auto joined=executeFederatedQuery(query->model,bindings,token,indexedBudget);
        QVERIFY2(joined.ok(),joined.diagnostics.empty()?"no data":joined.diagnostics.front().message.c_str());
        QCOMPARE(joined.rows.size(),std::size_t(405));
        const LocalRow lastMatch{number(409),true,number(818)};
        QVERIFY(joined.rows.at(joined.rows.size()-2)==lastMatch);
        run(*pg,"DROP TABLE public.federation_pg_owned");
    }catch(const std::exception &e){QFAIL(e.what());}}
    void federatedLookupKeysBoundPostgresReadFromSqlite() {try {
        using namespace navidog;
        using namespace navidog::sql::compiler;
        using namespace navidog::sql::optimizer;
        auto pg=connectDatabase(profile_);
        ConnectionProfile sqliteProfile;
        sqliteProfile.driver=DatabaseKind::SQLite;
        sqliteProfile.database=dir_.filePath("federation-key-lookup.sqlite");
        QFile file(sqliteProfile.database);QVERIFY(file.open(QIODevice::WriteOnly));file.close();
        auto sqlite=connectDatabase(sqliteProfile);
        auto token=cancellation();
        const auto run=[&](Database &db, const QString &sql) {
            auto result=db.execute(sql,{},token);
            if (!result.error.isEmpty()) throw std::runtime_error(result.error.toStdString());
            return result;
        };
        run(*pg,"CREATE TABLE public.federation_key_lookup_owned(k INT,payload INT)");
        run(*pg,"INSERT INTO public.federation_key_lookup_owned SELECT i,i*10 FROM generate_series(1,4096) AS i");
        run(*pg,"INSERT INTO public.federation_key_lookup_owned VALUES (2,21),(NULL,99)");
        run(*sqlite,"CREATE TABLE source_keys(id INTEGER,k INTEGER)");
        run(*sqlite,"INSERT INTO source_keys VALUES(1,2),(2,2),(3,NULL),(4,2147483648),(5,-3)");
        const auto integer=std::make_shared<const SqlType>(*catalogTypeFromText(Dialect::PostgreSQL,"INTEGER"));
        const auto bigint=std::make_shared<const SqlType>(*catalogTypeFromText(Dialect::PostgreSQL,"BIGINT"));
        navidog::sql::compiler::TableDefinition input;
        input.database="sqlite_source";input.schema="data";input.name="keys";
        input.columns={{"id",ValueType::Integer,bigint},{"k",ValueType::Integer,bigint}};
        auto target=input;
        target.database="pg_source";target.name="events";
        target.columns={{"k",ValueType::Integer,integer},{"payload",ValueType::Integer,integer}};
        MemoryCatalog catalog;catalog.tables={input,target};
        const NameResolutionContext scope{"sqlite_source","data",Dialect::PostgreSQL};
        const std::vector<FederatedTableBinding> bindings{
            {"sqlite_source","data","keys",DatabaseKind::SQLite,sqlite.get(),"main","source_keys"},
            {"pg_source","data","events",DatabaseKind::PostgreSQL,pg.get(),"public","federation_key_lookup_owned"}};
        InterpreterLimits limits;limits.maxRows=12;
        for (const auto mode : {"INNER","LEFT","FULL"}) {
            const auto sort=" ORDER BY l.id,r.payload";
            const auto sql=std::string("SELECT l.id,r.payload FROM sqlite_source.data.keys l ")+mode+
                " JOIN pg_source.data.events r ON r.k=l.k"+sort;
            const auto compiled=compile(scope.dialect,sql,{},&catalog,scope);
            QVERIFY2(compiled->ok(),compiled->diagnostics.empty()?sql.c_str():compiled->diagnostics.front().message.c_str());
            auto result=executeFederatedQuery(compiled->model,bindings,token,limits);
            if (std::string_view(mode)=="FULL") {
                QVERIFY(!result.ok() && result.rows.empty());
                QCOMPARE(result.diagnostics.front().code,DiagnosticCode::ResourceLimit);
                continue;
            }
            QVERIFY2(result.ok(),result.diagnostics.empty()?sql.c_str():result.diagnostics.front().message.c_str());
            const auto reference=run(*pg,"SELECT l.id,r.payload FROM (VALUES "
                "(1,2::bigint),(2,2),(3,NULL),(4,2147483648),(5,-3)) l(id,k) "+QString::fromLatin1(mode)+
                " JOIN public.federation_key_lookup_owned r ON r.k=l.k"+sort);
            QCOMPARE(result.rows.size(),std::size_t(reference.rows.size()));
            for (std::size_t i=0;i<result.rows.size();++i) {
                QCOMPARE(std::get<std::int64_t>(result.rows[i][0]),reference.rows[i][0].toLongLong());
                const auto &expected=reference.rows[i][1];
                if (!expected.isValid()) QVERIFY(std::holds_alternative<std::monostate>(result.rows[i][1]));
                else QCOMPARE(std::get<std::int64_t>(result.rows[i][1]),expected.toLongLong());
            }
        }
        run(*pg,"DROP TABLE public.federation_key_lookup_owned");
    }catch(const std::exception &e){QFAIL(e.what());}}
    void federatedTupleLookupPreservesMixedWidthsAndNulls() {try {
        using namespace navidog;
        using namespace navidog::sql::compiler;
        using namespace navidog::sql::optimizer;
        auto pg=connectDatabase(profile_);
        ConnectionProfile sqliteProfile;
        sqliteProfile.driver=DatabaseKind::SQLite;
        sqliteProfile.database=dir_.filePath("federation-tuple-key-lookup.sqlite");
        QFile file(sqliteProfile.database);
        QVERIFY(file.open(QIODevice::WriteOnly));
        file.close();
        auto sqlite=connectDatabase(sqliteProfile);
        auto token=cancellation();
        const auto run=[&](Database &db,const QString& sql) {
            auto result=db.execute(sql,{},token);
            if(!result.error.isEmpty())throw std::runtime_error(result.error.toStdString());
            return result;
        };
        run(*pg,"CREATE TABLE public.owned_federated_tuples(k SMALLINT,v BIGINT,payload INT)");
        run(*pg,"INSERT INTO public.owned_federated_tuples "
                "SELECT (i%100)::smallint,1099511627776::bigint+i,i FROM generate_series(1,8192) AS i");
        run(*pg,"INSERT INTO public.owned_federated_tuples VALUES "
                "(2,1099511627778,20002),(7,9223372036854775806,90000),(NULL,1099511627778,50000)");
        run(*sqlite,"CREATE TABLE source_tuple_keys(id INTEGER,k INTEGER,v INTEGER)");
        const QString values=
            "(1,2,1099511627778),(2,2,1099511627778),(3,5,1099511627781),"
            "(4,NULL,1099511627778),(5,32768,1099511627778),(6,2,NULL),"
            "(7,7,9223372036854775806),(8,2,1099511627781)";
        run(*sqlite,"INSERT INTO source_tuple_keys VALUES "+values);
        const auto type=[](const char* name) {
            return std::make_shared<const SqlType>(*catalogTypeFromText(Dialect::PostgreSQL,name));
        };
        navidog::sql::compiler::TableDefinition left;
        left.database="sqlite_tuple";left.schema="src";left.name="keys";
        left.columns={{"id",ValueType::Integer,type("BIGINT")},
                      {"k",ValueType::Integer,type("BIGINT")},
                      {"v",ValueType::Integer,type("BIGINT")}};
        auto right=left;
        right.database="pg_tuple";right.name="events";
        right.columns={{"k",ValueType::Integer,type("SMALLINT")},
                       {"v",ValueType::Integer,type("BIGINT")},
                       {"payload",ValueType::Integer,type("INTEGER")}};
        MemoryCatalog catalog;
        catalog.tables={left,right};
        const NameResolutionContext names{"sqlite_tuple","src",Dialect::PostgreSQL};
        const std::vector<FederatedTableBinding> bindings{
            {"sqlite_tuple","src","keys",DatabaseKind::SQLite,sqlite.get(),"main","source_tuple_keys"},
            {"pg_tuple","src","events",DatabaseKind::PostgreSQL,pg.get(),"public","owned_federated_tuples"}};
        InterpreterLimits limits;
        limits.maxRows=12;
        const auto execute=[&](const QString& join,const QString& source) {
            const auto condition=" ON r.k=l.k AND l.v=r.v ORDER BY l.id,r.payload";
            const auto query="SELECT l.id,r.payload FROM sqlite_tuple.src.keys l "+join+
                " JOIN pg_tuple.src.events r"+condition;
            const auto compiled=compile(names.dialect,query.toStdString(),{},&catalog,names);
            QVERIFY2(compiled->ok(),compiled->diagnostics.empty()?"no model":compiled->diagnostics.front().message.c_str());
            const auto result=executeFederatedQuery(compiled->model,bindings,token,limits);
            if(join=="FULL") {
                QVERIFY(!result.ok() && result.rows.empty());
                QCOMPARE(result.diagnostics.front().code,DiagnosticCode::ResourceLimit);
                return;
            }
            QVERIFY2(result.ok(),result.diagnostics.empty()?"no federated rows":result.diagnostics.front().message.c_str());
            const auto expected=run(*pg,"SELECT l.id,r.payload FROM (VALUES "+source+") l(id,k,v) "+
                join+" JOIN public.owned_federated_tuples r"+condition);
            QCOMPARE(result.rows.size(),std::size_t(expected.rows.size()));
            for(std::size_t i=0;i<result.rows.size();++i) {
                QCOMPARE(std::get<std::int64_t>(result.rows[i][0]),expected.rows[i][0].toLongLong());
                const auto& value=expected.rows[i][1];
                if(!value.isValid())QVERIFY(std::holds_alternative<std::monostate>(result.rows[i][1]));
                else QCOMPARE(std::get<std::int64_t>(result.rows[i][1]),value.toLongLong());
            }
        };
        for(const auto join:{"INNER","LEFT","FULL"})execute(join,values);
        run(*sqlite,"DELETE FROM source_tuple_keys");
        const QString impossible="(1,32768,1099511627778),(2,2,NULL)";
        run(*sqlite,"INSERT INTO source_tuple_keys VALUES "+impossible);
        execute("INNER",impossible);
        execute("LEFT",impossible);
    }catch(const std::exception& e){QFAIL(e.what());}}
    void federatedJoinPushdownRespectsOuterRows() {try {
        using namespace navidog;
        using namespace navidog::sql::compiler;
        using namespace navidog::sql::optimizer;
        auto db=connectDatabase(profile_);
        const auto token=cancellation();
        const auto run=[&](const QString &sql) {
            auto result=db->execute(sql,{},token);
            if (!result.error.isEmpty()) throw std::runtime_error(result.error.toStdString());
            return result;
        };
        run("CREATE TABLE public.federated_filter_left_owned(k INT,v INT)");
        run("CREATE TABLE public.federated_filter_right_owned(k INT,v INT)");
        run("INSERT INTO public.federated_filter_left_owned SELECT i,i*10+1 FROM generate_series(1,500) i");
        run("INSERT INTO public.federated_filter_right_owned SELECT i,i*10 FROM generate_series(1,500) i");
        const auto snapshot=collectMetadata(*db,profile_,"public",token);
        const MetadataCatalog nativeCatalog(std::make_shared<const MetadataSnapshot>(snapshot));
        const NameResolutionContext nativeScope{profile_.database.toStdString(),"public",Dialect::PostgreSQL};
        auto left=nativeCatalog.findTable(nativeScope,{"public","federated_filter_left_owned"});
        auto right=nativeCatalog.findTable(nativeScope,{"public","federated_filter_right_owned"});
        QVERIFY(left && right);
        left->database="source_left";left->schema="dataset";left->name="shared";
        right->database="source_right";right->schema="dataset";right->name="shared";
        MemoryCatalog catalog;catalog.tables={*left,*right};
        const NameResolutionContext scope{"source_left","dataset",Dialect::PostgreSQL};
        const std::vector<FederatedTableBinding> sources{
            {"source_left","dataset","shared",DatabaseKind::PostgreSQL,db.get(),"public","federated_filter_left_owned"},
            {"source_right","dataset","shared",DatabaseKind::PostgreSQL,db.get(),"public","federated_filter_right_owned"}};
        const auto check=[&](const QString &join, const QString &suffix, bool pushed) {
            const auto sql="SELECT l.k AS left_key,r.v AS right_value FROM source_left.dataset.shared l "+join+
                " JOIN source_right.dataset.shared r "+suffix+";";
            const auto q=compile(scope.dialect,sql.toStdString(),{},&catalog,scope);
            QVERIFY2(q->ok(),q->diagnostics.empty()?"bad join":q->diagnostics.front().message.c_str());
            InterpreterLimits bounded;bounded.maxRows=1;
            auto result=executeFederatedQuery(q->model,sources,token,bounded);
            if (!pushed) {
                QVERIFY2(!result.ok() && result.rows.empty() && result.diagnostics.front().code==DiagnosticCode::ResourceLimit,qPrintable(sql));
                result=executeFederatedQuery(q->model,sources,token);
            }
            QVERIFY2(result.ok(),result.diagnostics.empty()?"no join rows":result.diagnostics.front().message.c_str());
            auto nativeSql=sql;
            nativeSql.replace("source_left.dataset.shared","public.federated_filter_left_owned");
            nativeSql.replace("source_right.dataset.shared","public.federated_filter_right_owned");
            const auto reference=run(nativeSql);
            QCOMPARE(result.rows.size(),std::size_t(reference.rows.size()));
            for (std::size_t i=0;i<result.rows.size();++i) for (std::size_t col=0;col<2;++col) {
                const auto &cell=reference.rows.at(i).at(col);
                if (!cell.isValid()) QVERIFY(std::holds_alternative<std::monostate>(result.rows[i][col]));
                else QCOMPARE(std::get<std::int64_t>(result.rows[i][col]),cell.toLongLong());
            }
        };
        check("INNER","ON l.k=r.k WHERE l.k=2 AND r.v=20",true);
        check("INNER","ON l.k=r.k WHERE 2=l.k",true);
        check("INNER","ON r.k=l.k AND r.k=2",true);
        check("LEFT","ON l.k=r.k AND r.v=20 AND l.v=999 WHERE l.k=2",true);
        check("RIGHT","ON l.k=r.k AND l.v=21 AND r.v=999 WHERE r.k=2",true);
        check("RIGHT","ON l.k=r.k WHERE r.k=2",true);
        check("LEFT","ON l.k=r.k AND r.k=2 WHERE l.v=31",true);
        check("LEFT","ON l.k=r.k AND r.v=20 WHERE l.k=2 AND r.v IS NULL",true);
        check("LEFT","ON l.k=r.k WHERE l.k=2 AND r.v IS NULL",true);
        check("LEFT","ON l.k=r.k WHERE l.v=21 AND r.v IS NULL",true);
        check("FULL","ON l.k=r.k WHERE l.k=2 AND r.v=20",false);
        run("TRUNCATE public.federated_filter_left_owned,public.federated_filter_right_owned");
        run("INSERT INTO public.federated_filter_left_owned SELECT NULL,11 FROM generate_series(1,512)");
        run("INSERT INTO public.federated_filter_right_owned SELECT NULL,22 FROM generate_series(1,512)");
        run("INSERT INTO public.federated_filter_left_owned VALUES(2,21)");
        run("INSERT INTO public.federated_filter_right_owned VALUES(2,20)");
        check("INNER","ON r.k=l.k ORDER BY 1,2",true);
        check("LEFT","ON l.k=r.k ORDER BY 1,2",false);
        check("RIGHT","ON l.k=r.k ORDER BY 1,2",false);
        for (const auto &[mode, filteredLeft, filteredRight] :
             std::vector<std::tuple<std::string,bool,bool>>{{"INNER",true,true},{"LEFT",false,true},
                                                            {"RIGHT",true,false},{"FULL",false,false}}) {
            const auto source="SELECT l.k AS lk,r.k AS rk FROM source_left.dataset.shared l "+mode+
                " JOIN source_right.dataset.shared r ON r.k=l.k;";
            const auto compiled=compile(scope.dialect,source,{},&catalog,scope);
            QVERIFY(compiled->ok());
            const auto plan=optimize(compiled->model);
            QVERIFY(plan.ok());
            const auto rule=std::find_if(plan.rules.begin(),plan.rules.end(),
                [](const auto &event){return event.rule=="HepJoinDeriveNotNull";});
            QCOMPARE(rule!=plan.rules.end(),filteredLeft||filteredRight);
            if (rule!=plan.rules.end()) {
                const auto &join=plan.logical.nodes.at(rule->after);
                QCOMPARE(plan.logical.nodes.at(join.inputs[0]).kind==RelKind::Filter,filteredLeft);
                QCOMPARE(plan.logical.nodes.at(join.inputs[1]).kind==RelKind::Filter,filteredRight);
            }
        }
        run("DROP TABLE public.federated_filter_left_owned,public.federated_filter_right_owned");
    }catch(const std::exception &e){QFAIL(e.what());}}
    void extremumPrefersBoundedNativeOrderedReads() {try {
        using namespace navidog;
        using namespace navidog::sql::compiler;
        using namespace navidog::sql::optimizer;
        auto db=connectDatabase(profile_);
        const auto token=cancellation();
        const auto run=[&](const QString &sql) {
            auto result=db->execute(sql,{},token);
            if (!result.error.isEmpty()) throw std::runtime_error(result.error.toStdString());
            return result;
        };
        run("CREATE TABLE public.extremum_owned(n BIGINT,extra TEXT)");
        run("CREATE INDEX extremum_index_owned ON public.extremum_owned(n)");
        run("INSERT INTO public.extremum_owned SELECT CASE WHEN i%2=0 THEN NULL ELSE i::bigint*10000000000 END,'x' "
            "FROM generate_series(-2048,2048) i");
        run("ANALYZE public.extremum_owned");
        const auto snapshot=collectMetadata(*db,profile_,"public",token);
        const MetadataCatalog catalog(std::make_shared<const MetadataSnapshot>(snapshot));
        const NameResolutionContext scope{profile_.database.toStdString(),"public",Dialect::PostgreSQL};
        const std::vector<FederatedTableBinding> sources{{scope.database,"public","extremum_owned",
            DatabaseKind::PostgreSQL,db.get(),"public","extremum_owned"}};
        InterpreterLimits bounded;bounded.maxRows=1;
        const auto statistics=collectPlannerStatistics(*db,profile_,"public",token);
        QVERIFY2(statistics.complete,qPrintable(statistics.warning));
        MemoryStatistics stats;stats.tables=statistics.tables;
        const auto check=[&](const char *function, bool full, bool checkIndex) {
            const std::string sql=std::string("SELECT ")+function+"(n) AS result FROM public.extremum_owned;";
            const auto compiled=compile(scope.dialect,sql,{},&catalog,scope);
            QVERIFY2(compiled->ok(),compiled->diagnostics.empty()?"no query":compiled->diagnostics.front().message.c_str());
            const auto local=executeFederatedQuery(compiled->model,sources,token,bounded);
            QVERIFY2(local.ok(),local.diagnostics.empty()?"no extremum":local.diagnostics.front().message.c_str());
            const auto native=run(QString::fromStdString(sql));
            QCOMPARE(local.columns,std::vector<std::string>{"result"});
            QCOMPARE(local.rows.size(),std::size_t(1));
            QCOMPARE(native.columnTypes,QStringList{"bigint"});
            if (full)
                QCOMPARE(std::get<std::int64_t>(local.rows.front().front()),native.rows.front().front().toLongLong());
            else
                QVERIFY(std::holds_alternative<std::monostate>(local.rows.front().front()) &&
                        !native.rows.front().front().isValid());
            if (!checkIndex) return;
            const auto plan=optimize(compiled->model,&stats);
            QVERIFY2(plan.ok(),plan.diagnostics.empty()?"no extremum plan":plan.diagnostics.front().message.c_str());
            QVERIFY(std::any_of(plan.rules.begin(),plan.rules.end(),
                [](const auto &event){return event.rule=="AggregateMinMaxToLimit";}));
            std::vector<PlanId> reachable{plan.best.root};
            bool orderedIndex=false;
            while (!reachable.empty()) {
                const auto &node=plan.best.nodes.at(reachable.back());
                reachable.pop_back();
                orderedIndex |= node.kind==PhysicalKind::IndexScan && node.index=="extremum_index_owned" &&
                    node.description.find(std::string(function)=="MIN"?"forward ordered":"backward ordered")!=std::string::npos;
                reachable.insert(reachable.end(),node.inputs.begin(),node.inputs.end());
            }
            QVERIFY2(orderedIndex,dumpOptimization(plan).c_str());
            QVERIFY(plan.best.nodes.at(plan.best.root).total.score()<plan.baseline.nodes.at(plan.baseline.root).total.score());
        };
        check("MIN",true,true);check("MAX",true,true);
        for (const auto *sql : {"SELECT MIN(DISTINCT n) FROM public.extremum_owned;",
                                "SELECT MIN(n),MAX(n) FROM public.extremum_owned;",
                                "SELECT MAX(n) FROM public.extremum_owned WHERE n<0;"}) {
            const auto compiled=compile(scope.dialect,sql,{},&catalog,scope);
            QVERIFY(compiled->ok());
            const auto result=executeFederatedQuery(compiled->model,sources,token,bounded);
            QVERIFY(!result.ok() && result.rows.empty() && result.diagnostics.front().code==DiagnosticCode::ResourceLimit);
            const auto planned=optimize(compiled->model,&stats);
            QVERIFY(planned.ok());
            QVERIFY(std::none_of(planned.rules.begin(),planned.rules.end(),
                [](const auto &event){return event.rule=="AggregateMinMaxToLimit";}));
        }
        run("DELETE FROM public.extremum_owned WHERE n IS NOT NULL");
        check("MIN",false,false);check("MAX",false,false);
        run("TRUNCATE public.extremum_owned");
        check("MIN",false,false);check("MAX",false,false);
        run("DROP TABLE public.extremum_owned");
    }catch(const std::exception &e){QFAIL(e.what());}}
    void emptyAggregateFoldingPreservesResultTypes() {try {
        using namespace navidog;
        using namespace navidog::sql::compiler;
        namespace opt=navidog::sql::optimizer;
        auto db=connectDatabase(profile_);
        const auto token=cancellation();
        const auto run=[&](const QString &sql) {
            auto result=db->execute(sql,{},token);
            if (!result.error.isEmpty()) throw std::runtime_error(result.error.toStdString());
            return result;
        };
        run("CREATE TABLE public.empty_agg_owned(n BIGINT,k INT)");
        run("INSERT INTO public.empty_agg_owned VALUES (3,4),(8,2),(NULL,NULL)");
        const auto snapshot=collectMetadata(*db,profile_,"public",token);
        const MetadataCatalog catalog(std::make_shared<const MetadataSnapshot>(snapshot));
        const NameResolutionContext scope{profile_.database.toStdString(),"public",Dialect::PostgreSQL};
        const std::string sql="SELECT COUNT(*) AS row_count, COUNT(n) AS value_count, MIN(n) AS low, "
            "MAX(k) AS high,SUM(n) AS total FROM public.empty_agg_owned WHERE FALSE;";
        const auto compiled=compile(scope.dialect,sql,{},&catalog,scope);
        QVERIFY(compiled->ok());
        const auto planned=opt::optimize(compiled->model);
        QVERIFY2(planned.ok(),planned.diagnostics.empty()?"no plan":planned.diagnostics.front().message.c_str());
        const auto event=std::find_if(planned.rules.begin(),planned.rules.end(),[](const auto &e){return e.rule=="AggregateValues";});
        QVERIFY2(event!=planned.rules.end(),opt::dumpOptimization(planned).c_str());
        const auto &folded=planned.logical.nodes.at(event->after);
        QCOMPARE(folded.kind,opt::RelKind::Values);QCOMPARE(folded.valueCount,std::size_t(1));
        QCOMPARE(folded.keys.size(),std::size_t(5));
        const auto native=run(QString::fromStdString(sql));
        const auto equivalent=run("SELECT 0::bigint AS row_count,0::bigint AS value_count,NULL::bigint AS low,"
                                  "NULL::int AS high,NULL::numeric AS total;");
        QCOMPARE(equivalent.rows,native.rows);QCOMPARE(equivalent.columns,native.columns);QCOMPARE(equivalent.columnTypes,native.columnTypes);
        for (std::size_t i=0;i<folded.keys.size();++i) {
            const auto &expr=planned.logical.program.front().expressions.at(folded.keys[i].expression);
            QCOMPARE(expr.kind,i<2?ExprKind::Number:ExprKind::Null);
            const auto expected=catalogTypeFromText(scope.dialect,native.columnTypes.at(i).toStdString());
            QVERIFY(expected);
            const auto &type=planned.logical.program.types.get(expr.sqlType);
            QCOMPARE(type.family,expected->family);QCOMPARE(type.bits,expected->bits);
        }
        const std::string distinct="SELECT DISTINCT v.x,v.y FROM (VALUES (1,NULL),(1,NULL),(1,1),(-2,9),(-2,9)) AS v(x,y) ORDER BY 1,2;";
        const auto distinctModel=compile(scope.dialect,distinct);
        QVERIFY(distinctModel->ok());
        const auto deduplicated=opt::optimize(distinctModel->model);
        QVERIFY(deduplicated.ok());
        const auto selected=std::find_if(deduplicated.rules.begin(),deduplicated.rules.end(),
            [](const auto &e){return e.rule=="AggregateValues";});
        QVERIFY(selected!=deduplicated.rules.end());
        QCOMPARE(deduplicated.logical.nodes.at(selected->after).valueCount,std::size_t(3));
        const auto comparable=run("SELECT v.x,v.y FROM (VALUES (1,NULL),(1,1),(-2,9)) v(x,y) ORDER BY 1,2;");
        const auto source=run(QString::fromStdString(distinct));
        QCOMPARE(comparable.rows,source.rows);QCOMPARE(comparable.columnTypes,source.columnTypes);
        run("DROP TABLE public.empty_agg_owned");
    }catch(const std::exception &e){QFAIL(e.what());}}
    void primaryKeyProofsRespectInheritanceAndCacheVerification() {try {
        using namespace navidog;
        using namespace navidog::sql::compiler;
        namespace opt=navidog::sql::optimizer;
        auto db=connectDatabase(profile_);
        auto token=cancellation();
        auto run=[&](const QString &sql) {
            const auto result=db->execute(sql,{},token);
            if (!result.error.isEmpty()) throw std::runtime_error(result.error.toStdString());
        };
        run("CREATE TABLE public.key_parent_owned(id INT,n INT,PRIMARY KEY(n,id))");
        run("CREATE TABLE public.key_child_owned() INHERITS(public.key_parent_owned)");
        run("CREATE TABLE public.key_base_owned(id INT PRIMARY KEY,n INT)");
        run("CREATE TABLE public.key_deferred_owned(id INT PRIMARY KEY DEFERRABLE INITIALLY DEFERRED)");
        run("CREATE TABLE public.key_nullable_owned(id INT)");
        run("INSERT INTO public.key_base_owned VALUES(1,10),(2,10)");
        run("INSERT INTO public.key_parent_owned VALUES(1,10)");
        run("INSERT INTO public.key_child_owned VALUES(1,10)");
        run("INSERT INTO public.key_nullable_owned VALUES(1),(1),(NULL)");
        const auto snapshot=collectMetadata(*db,profile_,"public",token);
        const NameResolutionContext context{profile_.database.toStdString(),"public",Dialect::PostgreSQL};
        const auto confirm=[&](const MetadataCatalog &catalog) {
            const auto parent=catalog.findTable(context,{"public","key_parent_owned"});
            QVERIFY(parent && parent->primaryKeyKnown && !parent->noDescendantsKnown);
            const std::vector<std::string> expected{"n","id"};
            QVERIFY(parent->primaryKey==expected);
            const auto base=catalog.findTable(context,{"public","key_base_owned"});
            QVERIFY(base && base->primaryKeyKnown && base->noDescendantsKnown);
            const auto deferred=catalog.findTable(context,{"public","key_deferred_owned"});
            QVERIFY(deferred && !deferred->primaryKeyKnown);
            for (const bool only : {false,true}) {
                const auto sql=std::string("SELECT DISTINCT n,id FROM ")+(only?"ONLY ":"")+"public.key_parent_owned;";
                auto compiled=compile(Dialect::PostgreSQL,sql,{},&catalog,context);
                QVERIFY(compiled->ok());
                const auto planned=opt::optimize(compiled->model);
                QVERIFY(planned.ok());
                QCOMPARE(std::any_of(planned.rules.begin(),planned.rules.end(),
                    [](const auto &rule) { return rule.rule=="HepDistinctRemove"; }),only);
                const auto outer=std::string("SELECT a.id FROM public.key_base_owned a LEFT JOIN ")+
                    (only?"ONLY ":"")+"public.key_parent_owned b ON a.id=b.id AND a.n=b.n;";
                auto joined=compile(Dialect::PostgreSQL,outer,{},&catalog,context);
                QVERIFY(joined->ok());
                const auto reduced=opt::optimize(joined->model);
                QVERIFY(reduced.ok());
                QCOMPARE(std::any_of(reduced.rules.begin(),reduced.rules.end(),
                    [](const auto &rule) { return rule.rule=="HepRemoveUnusedOuterJoin"; }),only);
                const auto result=db->execute(QString::fromStdString(outer),{},token);
                QVERIFY2(result.error.isEmpty(),qPrintable(result.error));
                QCOMPARE(result.rows.size(),only?2:3);
            }
            const auto membership=[&](const char *predicate, bool expectedRule, int expectedRows) {
                const auto source=std::string("SELECT id FROM public.key_base_owned WHERE id ")+predicate+";";
                const auto compiled=compile(Dialect::PostgreSQL,source,{},&catalog,context);
                QVERIFY(compiled->ok());
                const auto planned=opt::optimize(compiled->model);
                QVERIFY(planned.ok());
                QCOMPARE(std::any_of(planned.rules.begin(),planned.rules.end(),
                    [](const auto &rule) { return rule.rule=="HepRewriteInSubquery"; }),expectedRule);
                const auto result=db->execute(QString::fromStdString(source),{},token);
                QVERIFY2(result.error.isEmpty(),qPrintable(result.error));
                QCOMPARE(result.rows.size(),expectedRows);
            };
            membership("IN (SELECT id FROM public.key_nullable_owned)",true,1);
            membership("NOT IN (SELECT id FROM public.key_nullable_owned)",false,0);
            membership("NOT IN (SELECT id FROM ONLY public.key_parent_owned)",true,1);
        };
        confirm(MetadataCatalog(std::make_shared<const MetadataSnapshot>(snapshot)));
        MetadataStore cache(dir_.filePath("key-proofs.sqlite"));
        cache.replace(snapshot);
        auto disk=cache.load(snapshot.key);
        QVERIFY(disk && !disk->verified);
        QVERIFY(!MetadataCatalog(disk).findTable(context,{"public","key_parent_owned"}));
        auto verified=std::make_shared<MetadataSnapshot>(*disk);
        verified->verified=true;
        confirm(MetadataCatalog(verified));
        run("DROP TABLE public.key_child_owned,public.key_parent_owned,public.key_base_owned,public.key_deferred_owned,public.key_nullable_owned");
    }catch(const std::exception &e){QFAIL(e.what());}}
    void localAveragesAndNumericScalesMatchPostgres() {try {
        using namespace navidog;
        using namespace navidog::sql::compiler;
        using namespace navidog::sql::optimizer;
        auto db=connectDatabase(profile_);
        const auto token=cancellation();
        const std::string values="WITH t(g,i,v) AS (VALUES "
            "(1,1,1.20),(1,2,4.0005),(1,3,2),(1,4,NULL),(1,5,-2.0),"
            "(2,6,-1.40),(2,7,-3.20),(2,8,-3.20),(3,9,NULL)) ";
        for (const auto &sql:{
                 values+"SELECT g,SUM(v) AS sum_value,AVG(v) AS average,AVG(DISTINCT v) AS unique_avg "
                        "FROM t GROUP BY g ORDER BY 1;",
                 values+"SELECT i,SUM(v) OVER (ORDER BY i ROWS BETWEEN 1 PRECEDING AND CURRENT ROW) AS sum_value, "
                        "AVG(v) OVER (ORDER BY i ROWS BETWEEN 1 PRECEDING AND 1 FOLLOWING EXCLUDE CURRENT ROW) "
                        "AS average FROM t ORDER BY 1;",
                 values+"SELECT SUM(v) AS sum_value,AVG(v) AS average FROM t WHERE FALSE;",
                 std::string("SELECT 1.200+2.034 AS total, -(2.00-1.5) AS debt, CAST(1.25 AS numeric) AS n;"),
                 std::string("WITH t(n) AS (VALUES (CAST(9223372036854775807 AS bigint)),"
                             "(9223372036854775807),(9223372036854775806),(NULL)) "
                             "SELECT AVG(n) AS average,AVG(DISTINCT n) AS unique_avg FROM t;"),
                 std::string("WITH t(n) AS (VALUES (1),(0),(0)) SELECT AVG(n) AS positive,AVG(-n) AS negative FROM t;"),
                 std::string("WITH t(n) AS (VALUES (1.00),(-1.00)) SELECT AVG(n) AS empty_sum FROM t;"),
                 std::string("SELECT CAST(1.20 AS numeric) AS n INTERSECT ALL SELECT 1.2 AS n;")}) {
            const auto compiled=compile(Dialect::PostgreSQL,sql);
            QVERIFY2(compiled->ok(),compiled->diagnostics.empty()?sql.c_str():compiled->diagnostics.front().message.c_str());
            const auto local=interpret(compiled->model);
            QVERIFY2(local.ok(),local.diagnostics.empty()?sql.c_str():local.diagnostics.front().message.c_str());
            const auto native=db->execute(QString::fromStdString(sql),{},token);
            QVERIFY2(native.error.isEmpty(),qPrintable(native.error));
            QCOMPARE(local.rows.size(),std::size_t(native.rows.size()));
            for (std::size_t row=0;row<local.rows.size();++row) {
                QCOMPARE(local.rows[row].size(),std::size_t(native.rows[int(row)].size()));
                for (std::size_t col=0;col<local.rows[row].size();++col) {
                    const auto &value=local.rows[row][col];
                    const auto &expected=native.rows[int(row)][int(col)];
                    if (expected.isNull()) QVERIFY(std::holds_alternative<std::monostate>(value));
                    else if (const auto *number=std::get_if<LocalNumeric>(&value))
                        QCOMPARE(QString::fromStdString(number->decimal()),expected.toString());
                    else if (const auto *integer=std::get_if<std::int64_t>(&value))
                        QCOMPARE(*integer,expected.toLongLong());
                    else QFAIL(sql.c_str());
                }
            }
        }
        const auto tooFine=compile(Dialect::PostgreSQL,
            "SELECT AVG(n) FROM (VALUES (0.00000000000000000000001),(0.0)) AS x(n);");
        const auto result=interpret(tooFine->model);
        QVERIFY(!result.ok() && result.rows.empty());
        QCOMPARE(result.diagnostics.front().code,DiagnosticCode::ResourceLimit);
    }catch(const std::exception &e){QFAIL(e.what());}}
    void localIntegralNumericCoercionsMatchPostgres() {try {
        using namespace navidog;
        using namespace navidog::sql::compiler;
        using namespace navidog::sql::optimizer;
        auto db=connectDatabase(profile_);
        const auto token=cancellation();
        for (const auto *sql:{
                 "SELECT 18446744073709551614+1 AS total,-(18446744073709551614-4) AS debt;",
                 "WITH t(n) AS (VALUES (1),(18446744073709551614),(-18446744073709551614),(NULL)) "
                 "SELECT SUM(n) AS total,MIN(n) AS least,MAX(n) AS greatest FROM t;",
                 "WITH t(n) AS (VALUES (1),(18446744073709551614),(1),(NULL)) "
                 "SELECT DISTINCT n FROM t WHERE n IN (1,18446744073709551614) ORDER BY n;",
                 "SELECT 18446744073709551614 AS n UNION ALL SELECT 1 AS n ORDER BY 1;",
                 "SELECT CAST(42 AS numeric) AS n INTERSECT ALL SELECT 42 AS n;",
                 "SELECT 1 AS n EXCEPT SELECT 18446744073709551614 AS n;",
                 "SELECT (SELECT 18446744073709551614-3) AS n;",
                 "SELECT 1.25::numeric(4,2) AS n;"}) {
            const auto model=compile(Dialect::PostgreSQL,sql);
            QVERIFY2(model->ok(),model->diagnostics.empty()?sql:model->diagnostics.front().message.c_str());
            const auto result=interpret(model->model);
            QVERIFY2(result.ok(),result.diagnostics.empty()?sql:result.diagnostics.front().message.c_str());
            const auto expected=db->execute(QString::fromUtf8(sql),{},token);
            QVERIFY2(expected.error.isEmpty(),qPrintable(expected.error));
            QCOMPARE(result.rows.size(),std::size_t(expected.rows.size()));
            for (std::size_t row=0;row<result.rows.size();++row) {
                QCOMPARE(result.rows[row].size(),std::size_t(expected.rows[int(row)].size()));
                for (std::size_t col=0;col<result.rows[row].size();++col) {
                    QVERIFY(expected.columnTypes[int(col)].startsWith("numeric"));
                    const auto *value=std::get_if<LocalNumeric>(&result.rows[row][col]);
                    QVERIFY(value!=nullptr);
                    QCOMPARE(QString::fromStdString(value->decimal()),expected.rows[int(row)][int(col)].toString());
                }
            }
        }
        const auto oversized=compile(Dialect::PostgreSQL,"SELECT 999999999999999999999999999+1;");
        const auto result=interpret(oversized->model);
        QVERIFY(!result.ok() && result.rows.empty());
        QCOMPARE(result.diagnostics.front().code,DiagnosticCode::ResourceLimit);
    }catch(const std::exception &e){QFAIL(e.what());}}
    void localBigintSumsPreserveExactNumericResults() {try {
        using namespace navidog;
        using namespace navidog::sql::compiler;
        using namespace navidog::sql::optimizer;
        auto db=connectDatabase(profile_);
        const auto token=cancellation();
        const std::string data="WITH t(g,i,v) AS (VALUES "
            "(1,1,CAST(9223372036854775807 AS bigint)),(1,2,9223372036854775807),"
            "(1,3,-9223372036854775808),(2,4,-9223372036854775808),(2,5,-9223372036854775808),"
            "(3,6,NULL),(4,7,9223372036854775807),(4,8,-9223372036854775808),(4,9,1)) ";
        for (const auto &tail:std::vector<std::string>{
                 "SELECT g,SUM(v) AS exact_sum,SUM(DISTINCT v) AS distinct_sum FROM t GROUP BY g ORDER BY g;",
                 "SELECT SUM(v) AS exact_sum FROM t WHERE g=99;",
                 "SELECT g,SUM(v) AS exact_sum FROM t GROUP BY g HAVING SUM(v)>=0 ORDER BY 2,1;",
                 "SELECT g,CASE WHEN SUM(v)>0 THEN SUM(v) ELSE 0 END AS exact_sum FROM t GROUP BY g ORDER BY 1;",
                 "SELECT CAST(7 AS numeric) AS constant,SUM(v) AS exact_sum FROM t;",
                 "SELECT g,SUM(v) AS exact_sum FROM t GROUP BY GROUPING SETS ((g),()) ORDER BY 1;",
                 "SELECT SUM(v) AS exact_sum FROM t GROUP BY g INTERSECT ALL "
                 "SELECT SUM(v) AS exact_sum FROM t GROUP BY g ORDER BY 1;",
                 "SELECT i,SUM(v) OVER(ORDER BY i ROWS BETWEEN 1 PRECEDING AND 1 FOLLOWING "
                 "EXCLUDE CURRENT ROW) AS exact_sum FROM t ORDER BY i;",
                 "SELECT i,SUM(v) FILTER(WHERE i<>2) OVER(ORDER BY i ROWS 2 PRECEDING) AS exact_sum "
                 "FROM t ORDER BY i;"}) {
            const auto sql=data+tail;
            const auto compiled=compile(Dialect::PostgreSQL,sql);
            QVERIFY2(compiled->ok(),compiled->diagnostics.empty()?"no model":compiled->diagnostics.front().message.c_str());
            const auto local=interpret(compiled->model);
            QVERIFY2(local.ok(),local.diagnostics.empty()?"no local result":local.diagnostics.front().message.c_str());
            const auto expected=db->execute(QString::fromStdString(sql),{},token);
            QVERIFY2(expected.error.isEmpty(),qPrintable(expected.error));
            QCOMPARE(local.rows.size(),std::size_t(expected.rows.size()));
            for (std::size_t row=0;row<local.rows.size();++row) {
                QCOMPARE(local.rows[row].size(),std::size_t(expected.rows[int(row)].size()));
                for (std::size_t col=0;col<local.rows[row].size();++col) {
                    const auto &native=expected.rows[int(row)][int(col)];
                    const auto &value=local.rows[row][col];
                    if (native.isNull()) QVERIFY(std::holds_alternative<std::monostate>(value));
                    else if (expected.columnTypes[int(col)]==QStringLiteral("numeric")) {
                        const auto *numeric=std::get_if<LocalNumeric>(&value);
                        QVERIFY(numeric!=nullptr);
                        QCOMPARE(QString::fromStdString(numeric->decimal()),native.toString());
                    } else {
                        const auto *integer=std::get_if<std::int64_t>(&value);
                        QVERIFY(integer!=nullptr);
                        QCOMPARE(*integer,native.toLongLong());
                    }
                }
            }
        }
        const auto statement=compile(Dialect::PostgreSQL,data+"SELECT SUM(v) AS total FROM t WHERE g=2;");
        const auto result=interpret(statement->model);
        QVERIFY(result.ok());
        QVERIFY(dumpInterpretation(result).find("\"-18446744073709551616\"")!=std::string::npos);
    }catch(const std::exception &e){QFAIL(e.what());}}
    void bigintAverageSplitsIntoNativeTypedStates() {try {
        using namespace navidog;
        using namespace navidog::sql::compiler;
        namespace opt=navidog::sql::optimizer;
        auto db=connectDatabase(profile_);
        const auto token=cancellation();
        const auto run=[&](const std::string &sql) {
            const auto result=db->execute(QString::fromStdString(sql),{},token);
            if (!result.error.isEmpty()) throw std::runtime_error(result.error.toStdString());
            return result;
        };
        run("CREATE TABLE public.avg_states_owned(g integer,v bigint,small_value integer)");
        run("INSERT INTO public.avg_states_owned VALUES "
            "(1,9223372036854775807,1),(1,9223372036854775806,1),(1,NULL,1),"
            "(2,-9223372036854775808,1),(2,-9223372036854775808,1),"
            "(3,1,1),(3,0,1),(3,0,1),(3,NULL,1),(4,NULL,1),(4,NULL,1)");
        const auto snapshot=std::make_shared<const MetadataSnapshot>(collectMetadata(*db,profile_,"public",token));
        const MetadataCatalog catalog(snapshot);
        const NameResolutionContext names{profile_.database.toStdString(),"public",Dialect::PostgreSQL};
        opt::PlannerOptions options;
        options.heuristic.rules={opt::HepRule::AggregateReduceFunctions};
        const auto planFor=[&](const std::string &sql) {
            const auto model=compile(Dialect::PostgreSQL,sql,{},&catalog,names);
            if (!model->ok()) throw std::runtime_error(model->diagnostics.front().message);
            const auto plan=opt::optimize(model->model,nullptr,options);
            if (!plan.ok()) throw std::runtime_error(plan.diagnostics.front().message);
            return plan;
        };
        const auto rule=[](const opt::OptimizationResult &plan) {
            return std::find_if(plan.rules.begin(),plan.rules.end(),[](const auto &event) {
                return event.rule=="HepAggregateReduceFunctions";
            });
        };
        const std::string table=" FROM public.avg_states_owned";
        for (const auto &sql:{
                 "SELECT g,AVG(v) AS mean_value,SUM(v) AS total,COUNT(v) AS n"+table+" GROUP BY g ORDER BY 1;",
                 "SELECT g,AVG(DISTINCT v) AS mean_value"+table+" GROUP BY g ORDER BY 1;",
                 "SELECT AVG(v) AS mean_value"+table+" WHERE g=99;",
                 "SELECT AVG(v) AS mean_value"+table+" WHERE g=3;"}) {
            const auto plan=planFor(sql);
            const auto event=rule(plan);
            QVERIFY2(event!=plan.rules.end(),opt::dumpOptimization(plan).c_str());
            const auto &project=plan.logical.nodes.at(event->after);
            if (project.keys.size()==4) {
                const auto &expression=plan.logical.program.at(project.query-1).expressions.at(project.keys[1].expression);
                QCOMPARE(expression.operands,(std::vector<ExprId>{project.keys[2].expression,project.keys[3].expression}));
            }
            const auto emitted=opt::generatePlanSql(plan.logical,plan.heuristicRoot);
            QVERIFY2(emitted.ok(),emitted.diagnostics.empty()?"no SQL":emitted.diagnostics.front().message.c_str());
            QVERIFY(emitted.sql.find("numeric_div")!=std::string::npos);
            const auto original=run(sql), replacement=run(emitted.sql);
            QCOMPARE(replacement.rows,original.rows);
            QCOMPARE(replacement.columns,original.columns);
            QCOMPARE(replacement.columnTypes,original.columnTypes);
        }
        for (const auto &sql:{
                "SELECT AVG(small_value) AS mean_value"+table,
                "SELECT AVG(v) FILTER (WHERE g>1) AS mean_value"+table,
                "SELECT AVG(v) AS mean_value"+table+" HAVING COUNT(*)>0",
                "SELECT g,AVG(v) AS mean_value"+table+" GROUP BY GROUPING SETS ((g),())"}) {
            const auto plan=planFor(sql);
            QVERIFY2(rule(plan)==plan.rules.end(),sql.c_str());
        }
    }catch(const std::exception &e){QFAIL(e.what());}}
    void constantGroupingKeysPreserveTypedOutputs() {try {
        using namespace navidog;
        using namespace navidog::sql::compiler;
        namespace opt=navidog::sql::optimizer;
        auto db=connectDatabase(profile_);
        const auto token=cancellation();
        const auto run=[&](const std::string &sql) {
            const auto result=db->execute(QString::fromStdString(sql),{},token);
            if (!result.error.isEmpty()) throw std::runtime_error(result.error.toStdString());
            return result;
        };
        run("CREATE TABLE public.fixed_group_owned(k smallint,region bigint,category integer,v integer)");
        run("INSERT INTO public.fixed_group_owned VALUES "
            "(-2,10,1,5),(-2,10,1,9),(-2,10,2,NULL),(-2,11,2,7),"
            "(NULL,10,1,1),(NULL,11,1,7),(NULL,11,NULL,NULL)");
        const auto snapshot=std::make_shared<const MetadataSnapshot>(collectMetadata(*db,profile_,"public",token));
        MetadataCatalog catalog(snapshot);
        const NameResolutionContext names{profile_.database.toStdString(),"public",Dialect::PostgreSQL};
        opt::PlannerOptions options;
        options.heuristic.rules={opt::HepRule::AggregateProjectPullUpConstants};
        const auto planned=[&](const std::string &sql) {
            const auto parsed=compile(Dialect::PostgreSQL,sql,{},&catalog,names);
            if (!parsed->ok()) throw std::runtime_error(parsed->diagnostics.front().message);
            const auto plan=opt::optimize(parsed->model,nullptr,options);
            if (!plan.ok()) throw std::runtime_error(plan.diagnostics.front().message);
            return plan;
        };
        const auto rule=[](const opt::OptimizationResult &plan) {
            return std::find_if(plan.rules.begin(),plan.rules.end(),
                [](const auto &entry){return entry.rule=="HepAggregateProjectPullUpConstants";});
        };
        const std::string a="SELECT k AS tenant,category,COUNT(*) AS n,MIN(v) AS lo ";
        const std::string b="SELECT k AS tenant,region,COUNT(DISTINCT v) AS n ";
        const std::string table="FROM public.fixed_group_owned ";
        const std::string category=" GROUP BY k,category ORDER BY 1,2";
        const std::string region=" GROUP BY k,region ORDER BY 1,2";
        for (const auto &sql:{a+table+"WHERE -2=k"+category,a+table+"WHERE k IS NULL"+category,
                              b+table+"WHERE k=-2 AND region=10"+region,
                              b+table+"WHERE k=-2 AND region=8"+region}) {
            const auto plan=planned(sql);
            const auto fired=rule(plan);
            QVERIFY2(fired!=plan.rules.end(),opt::dumpOptimization(plan).c_str());
            const auto &project=plan.logical.nodes.at(fired->after);
            const auto &group=plan.logical.nodes.at(project.inputs.at(0));
            QCOMPARE(group.kind,opt::RelKind::Aggregate);
            QCOMPARE(group.keys.size(),std::size_t(1));
            const auto generated=opt::generatePlanSql(plan.logical,plan.heuristicRoot);
            QVERIFY2(generated.ok(),generated.diagnostics.empty()?"no SQL":generated.diagnostics.front().message.c_str());
            QVERIFY(generated.sql.find("CAST(")!=std::string::npos);
            const auto expected=run(sql);
            const auto actual=run(generated.sql);
            QCOMPARE(actual.rows,expected.rows);
            QCOMPARE(actual.columns,expected.columns);
            QCOMPARE(actual.columnTypes,expected.columnTypes);
            if (sql==b+table+"WHERE k=-2 AND region=8"+region) QVERIFY(actual.rows.empty());
        }
        for (const auto &sql:{a+table+"WHERE k=32768"+category,
                              a+table+"WHERE k=-2 OR k IS NULL"+category,
                              a+table+"WHERE k=-2 GROUP BY k,category HAVING COUNT(*)>1",
                              a+table+"WHERE k=-2 GROUP BY GROUPING SETS ((k,category),(k))",
                              "SELECT k,COUNT(*) AS n "+table+"WHERE k=-2 GROUP BY k"}) {
            const auto plan=planned(sql);
            QVERIFY2(rule(plan)==plan.rules.end(),sql.c_str());
        }
    }catch(const std::exception &e){QFAIL(e.what());}}
    void dependentGroupKeysMatchOwnedPostgres() {try {
      using namespace navidog;
      using namespace navidog::sql::compiler;
      namespace opt=navidog::sql::optimizer;
      auto db=connectDatabase(profile_);
      auto token=cancellation();
      const auto run=[&](const QString &sql) {
        auto result=db->execute(sql,{},token);
        if (!result.error.isEmpty()) throw std::runtime_error(result.error.toStdString());
        return result;
      };
      run("CREATE TABLE public.aggregate_keys_owned(p INT,q INT,n INT, PRIMARY KEY(p,q))");
      run("CREATE TABLE public.aggregate_deferrable_owned(p INT PRIMARY KEY DEFERRABLE,n INT)");
      run("INSERT INTO public.aggregate_keys_owned VALUES (1,1,NULL),(1,2,9),(2,1,5),(2,2,5)");
      const auto check=[&](const QString &sql, const QString &rewrite, const CatalogSnapshot &catalog, bool match) {
        const NameResolutionContext context{profile_.database.toStdString(),"public",Dialect::PostgreSQL};
        const auto compiled=compile(context.dialect,sql.toStdString(),{},&catalog,context);
        QVERIFY2(compiled->ok(),compiled->diagnostics.empty()?qPrintable(sql):compiled->diagnostics.front().message.c_str());
        opt::PlannerOptions options;
        auto &rules=options.heuristic.rules;
        rules.erase(std::remove(rules.begin(),rules.end(),opt::HepRule::AggregateRemove),rules.end());
        const auto planned=opt::optimize(compiled->model,nullptr,std::move(options));
        QVERIFY2(planned.ok(),planned.diagnostics.empty()?qPrintable(sql):planned.diagnostics.front().message.c_str());
        const auto event=std::find_if(planned.rules.begin(),planned.rules.end(),
            [](const auto &rule){return rule.rule=="HepAggregateDuplicateKeys";});
        QCOMPARE(event!=planned.rules.end(),match);
        if (!match) return;
        const auto &before=planned.logical.nodes.at(event->before), &after=planned.logical.nodes.at(event->after);
        QCOMPARE(before.keys.size(),std::size_t(3));
        QCOMPARE(after.keys.size(),std::size_t(2));
        QCOMPARE(after.outputs.size(),before.outputs.size());
        QCOMPARE(planned.best.nodes[planned.best.root].rows,planned.baseline.nodes[planned.baseline.root].rows);
        QVERIFY(planned.best.nodes[planned.best.root].total.cpu<planned.baseline.nodes[planned.baseline.root].total.cpu);
        const auto input=run(sql), output=run(rewrite);
        QCOMPARE(input.columns,output.columns);
        QCOMPARE(input.columnTypes,output.columnTypes);
        QCOMPARE(input.rows,output.rows);
      };
      const auto snapshot=[&]{
        return MetadataCatalog(std::make_shared<const MetadataSnapshot>(collectMetadata(*db,profile_,"public",token)));
      };
      const auto catalog=snapshot();
      const QString prefix="SELECT p AS first_key,q,COUNT(*) AS total FROM public.aggregate_keys_owned ";
      check(prefix+"GROUP BY p,n,q ORDER BY first_key,q",prefix+"GROUP BY p,q ORDER BY first_key,q",catalog,true);
      check(prefix+"WHERE p=1 GROUP BY p,q,n ORDER BY first_key,q",
            prefix+"WHERE p=1 GROUP BY p,q ORDER BY first_key,q",catalog,true);
      check("SELECT p,COUNT(*) FROM public.aggregate_keys_owned GROUP BY p,n",{},catalog,false);
      check("SELECT p,q,n,COUNT(*) FROM public.aggregate_keys_owned GROUP BY p,q,n",{},catalog,false);
      check(prefix+"GROUP BY p,q,n ORDER BY n",{},catalog,false);
      check(prefix+"GROUP BY GROUPING SETS ((p,q,n),(p,q))",{},catalog,false);
      check("SELECT p,COUNT(*) FROM public.aggregate_deferrable_owned GROUP BY p,n",{},catalog,false);
      run("CREATE TABLE public.aggregate_keys_child_owned() INHERITS(public.aggregate_keys_owned)");
      run("INSERT INTO public.aggregate_keys_child_owned VALUES (1,1,8)");
      const auto inherited=snapshot();
      check(prefix+"GROUP BY p,q,n ORDER BY first_key,q",{},inherited,false);
      const QString only="SELECT p AS first_key,q,COUNT(*) AS total FROM ONLY public.aggregate_keys_owned ";
      check(only+"GROUP BY p,q,n ORDER BY first_key,q",only+"GROUP BY p,q ORDER BY first_key,q",inherited,true);
      run("DROP TABLE public.aggregate_keys_child_owned,public.aggregate_keys_owned,public.aggregate_deferrable_owned");
    }catch(const std::exception &e){QFAIL(e.what());}}
    void singletonAggregateRemovalMatchesOwnedPostgres() {try {
      using namespace navidog;
      using namespace navidog::sql::compiler;
      namespace opt=navidog::sql::optimizer;
      auto db=connectDatabase(profile_);
      auto token=cancellation();
      const auto run=[&](const QString &sql) {
        auto result=db->execute(sql,{},token);
        if (!result.error.isEmpty()) throw std::runtime_error(result.error.toStdString());
        return result;
      };
      run("CREATE TABLE public.singleton_group_owned(p INT,q INT,v BIGINT,n SMALLINT,PRIMARY KEY(p,q))");
      run("INSERT INTO public.singleton_group_owned VALUES (1,1,NULL,NULL),(1,2,9223372036854775807,9),(2,1,-3,5)");
      const MetadataCatalog catalog(std::make_shared<const MetadataSnapshot>(collectMetadata(*db,profile_,"public",token)));
      const NameResolutionContext context{profile_.database.toStdString(),"public",Dialect::PostgreSQL};
      const auto check=[&](const QString &sql, bool allow) {
        const auto compiled=compile(context.dialect,sql.toStdString(),{},&catalog,context);
        QVERIFY2(compiled->ok(),compiled->diagnostics.empty()?qPrintable(sql):compiled->diagnostics.front().message.c_str());
        const auto planned=opt::optimize(compiled->model);
        QVERIFY2(planned.ok(),planned.diagnostics.empty()?qPrintable(sql):planned.diagnostics.front().message.c_str());
        const auto found=std::find_if(planned.rules.begin(),planned.rules.end(),
            [](const auto &rule){return rule.rule=="HepAggregateRemove";});
        QCOMPARE(found!=planned.rules.end(),allow);
        if (!allow) return;
        const auto &project=planned.logical.nodes.at(found->after);
        QCOMPARE(project.kind,opt::RelKind::Project);
        const auto &input=planned.logical.nodes.at(project.inputs.front());
        QVERIFY(input.kind==opt::RelKind::Scan || input.kind==opt::RelKind::Filter);
        auto generatedModel=planned.logical.program;
        auto &root=generatedModel.queries.at(project.query-1);
        root.groupBy.clear();
        QCOMPARE(project.keys.size(),root.projections.size());
        for (std::size_t i=0;i<root.projections.size();++i) {
          const auto &initial=compiled->model.front();
          const auto expected=compiled->model.types.get(initial.expressions[initial.projections[i].expression].sqlType);
          const auto actual=generatedModel.types.get(root.expressions[project.keys[i].expression].sqlType);
          QVERIFY(actual==expected);
          root.projections[i].expression=project.keys[i].expression;
          if (root.projections[i].alias.empty() && !initial.expressions[initial.projections[i].expression].name.empty())
            root.projections[i].alias=initial.expressions[initial.projections[i].expression].name.back();
        }
        const auto generated=generateSql(generatedModel,Dialect::PostgreSQL);
        QVERIFY2(generated.ok(),generated.diagnostics.empty()?qPrintable(sql):generated.diagnostics.front().message.c_str());
        const auto original=run(sql), nativeRewrite=run(QString::fromStdString(generated.sql));
        QCOMPARE(nativeRewrite.rows,original.rows);
        QCOMPARE(nativeRewrite.columns,original.columns);
        QCOMPARE(nativeRewrite.columnTypes,original.columnTypes);
      };
      const QString select="SELECT p,q,COUNT(*) AS a,COUNT(n) AS b,COUNT(p) AS c,MIN(n) AS low,MAX(v) AS hi,"
                           "SUM(n) AS sum_n,SUM(v) AS sum_v FROM public.singleton_group_owned ";
      check(select+"GROUP BY p,q ORDER BY 1,2",true);
      check(select+"WHERE p=99 GROUP BY p,q ORDER BY 1,2",true);
      check("SELECT p,q FROM public.singleton_group_owned GROUP BY p,q ORDER BY p,q",true);
      check("SELECT p,COUNT(*) FROM public.singleton_group_owned GROUP BY p",false);
      check("SELECT p,COUNT(DISTINCT n) FROM public.singleton_group_owned GROUP BY p,q",false);
      check("SELECT p,AVG(n) FROM public.singleton_group_owned GROUP BY p,q",false);
      check("SELECT p,COUNT(*) FROM public.singleton_group_owned GROUP BY p,q HAVING COUNT(*)=1",false);
      check("SELECT p,COUNT(*) FROM public.singleton_group_owned GROUP BY p,q ORDER BY COUNT(*)",false);
      run("DROP TABLE public.singleton_group_owned");
    }catch(const std::exception &e){QFAIL(e.what());}}
    void sortProofRulesMatchOwnedPostgres() {try {
      using namespace navidog;using namespace navidog::sql::compiler;namespace opt=navidog::sql::optimizer;
      auto db=connectDatabase(profile_);auto token=cancellation();
      auto run=[&](const QString &sql){auto r=db->execute(sql,{},token);if(!r.error.isEmpty())throw std::runtime_error(r.error.toStdString());return r;};
      run("CREATE TABLE public.sort_rules_owned(id integer,n integer)");
      run("INSERT INTO public.sort_rules_owned VALUES(NULL,2),(1,2),(NULL,1),(1,1),(2,1)");
      auto metadata=collectMetadata(*db,profile_,"public",token);MetadataCatalog catalog(std::make_shared<const MetadataSnapshot>(metadata));
      const std::string sql="SELECT id AS a,id AS b,n FROM public.sort_rules_owned ORDER BY a ASC NULLS LAST,b DESC NULLS FIRST,n;";
      auto c=compile(Dialect::PostgreSQL,sql,{},&catalog,{profile_.database.toStdString(),"public",Dialect::PostgreSQL});QVERIFY(c->ok());
      auto planned=opt::optimize(c->model);QVERIFY(planned.ok());
      QVERIFY(std::any_of(planned.rules.begin(),planned.rules.end(),[](const auto &rule){return rule.rule=="HepSortDuplicateKeys";}));
      const auto original=run(QString::fromStdString(sql)), reduced=run("SELECT id AS a,id AS b,n FROM public.sort_rules_owned ORDER BY a ASC NULLS LAST,n");
      QCOMPARE(original.columns,reduced.columns);QCOMPARE(original.rows,reduced.rows);
      auto constant=compile(Dialect::PostgreSQL,"SELECT 1 AS n ORDER BY n LIMIT 10;");QVERIFY(constant->ok());auto one=opt::optimize(constant->model);QVERIFY(one.ok());
      QVERIFY(std::any_of(one.rules.begin(),one.rules.end(),[](const auto &rule){return rule.rule=="HepSortLimitRemove";}));
      QCOMPARE(run("SELECT 1 AS n ORDER BY n LIMIT 10").rows,run("SELECT 1 AS n").rows);
      QCOMPARE(run("SELECT 1 AS n LIMIT 1 OFFSET 1").rows.size(),0);
      const NameResolutionContext context{profile_.database.toStdString(),"public",Dialect::PostgreSQL};
      for (const auto &[source,rewrite,expectedKeys] : std::vector<std::tuple<QString,QString,std::size_t>>{
        {"SELECT id AS fixed_key,n,7 AS bucket FROM public.sort_rules_owned WHERE 1=id "
         "ORDER BY 3 DESC,fixed_key NULLS FIRST,2 DESC FETCH FIRST 1 ROW WITH TIES;",
         "SELECT id AS fixed_key,n,7 AS bucket FROM public.sort_rules_owned WHERE 1=id "
         "ORDER BY 2 DESC FETCH FIRST 1 ROW WITH TIES;",1},
        {"SELECT id AS fixed_key,n FROM public.sort_rules_owned WHERE id IS NULL ORDER BY fixed_key,n;",
         "SELECT id AS fixed_key,n FROM public.sort_rules_owned WHERE id IS NULL ORDER BY n;",1},
        {"SELECT id,7 AS bucket FROM public.sort_rules_owned ORDER BY bucket FETCH FIRST 1 ROW WITH TIES;",
         "SELECT id,7 AS bucket FROM public.sort_rules_owned;",1},
        {"SELECT id FROM public.sort_rules_owned WHERE id=2 ORDER BY id;",
         "SELECT id FROM public.sort_rules_owned WHERE id=2;",0}}) {
          const auto compiled=compile(context.dialect,source.toStdString(),{},&catalog,context);
          QVERIFY2(compiled->ok(),qPrintable(source));
          const auto result=opt::optimize(compiled->model);
          QVERIFY(result.ok());
          const auto found=std::find_if(result.rules.begin(),result.rules.end(),
              [](const auto &rule){return rule.rule=="HepSortConstantKeys";});
          if (source.contains("ORDER BY bucket FETCH")) {
              QCOMPARE(found,result.rules.end());
          } else {
              QVERIFY2(found!=result.rules.end(),qPrintable(source));
              const auto &shape=result.logical.nodes.at(found->after);
              QCOMPARE(shape.kind==opt::RelKind::Sort ? shape.keys.size() : 0,expectedKeys);
              if (source.contains("ORDER BY 3")) {
                  QVERIFY(shape.descending[0]);
                  QVERIFY(shape.nullOrdering[0].empty());
              }
          }
          auto a=run(source), b=run(rewrite);
          QCOMPARE(a.columnTypes,b.columnTypes);
          if (source.contains("ORDER BY bucket FETCH")) {
              const auto byId=[](const auto &a,const auto &b){return a.front().toString()<b.front().toString();};
              std::sort(a.rows.begin(),a.rows.end(),byId);
              std::sort(b.rows.begin(),b.rows.end(),byId);
          }
          QCOMPARE(a.rows,b.rows);
      }
      for (const auto *sql : {
          "SELECT id,n FROM public.sort_rules_owned WHERE id=1 OR id=2 ORDER BY id;",
          "SELECT id,n FROM public.sort_rules_owned ORDER BY 2;",
          "SELECT id,random() AS volatile_value FROM public.sort_rules_owned ORDER BY volatile_value;"}) {
          const auto compiled=compile(context.dialect,sql,{},&catalog,context);
          QVERIFY(compiled->ok());
          const auto planned=opt::optimize(compiled->model);
          QVERIFY(planned.ok());
          QVERIFY(std::none_of(planned.rules.begin(),planned.rules.end(),
              [](const auto &rule){return rule.rule=="HepSortConstantKeys";}));
      }
      run("DROP TABLE public.sort_rules_owned");
    }catch(const std::exception &e){QFAIL(e.what());}}
    void numericLiteralAndCommonTypesMatchPostgres() {try {
        using namespace navidog;
        using namespace navidog::sql::compiler;
        auto db=connectDatabase(profile_);
        auto token=cancellation();
        for (const auto sql : {
                "SELECT 2147483647,2147483648,-2147483648,-(-2147483648),"
                "9223372036854775807,9223372036854775808,-9223372036854775808,1e2,1.25;",
                "SELECT 1+2,CAST(1 AS SMALLINT)/CAST(2 AS SMALLINT),"
                "CAST(1 AS SMALLINT)*CAST(2 AS BIGINT),CAST(3 AS BIGINT)%1;",
                "SELECT CASE WHEN TRUE THEN 1 ELSE 2147483648 END,COALESCE(NULL,1,2147483648),"
                "GREATEST(1,1.25),LEAST(1,CAST(2 AS REAL)),"
                "CASE WHEN TRUE THEN CAST(1 AS NUMERIC) ELSE CAST(2 AS DOUBLE PRECISION) END;",
                "VALUES (1),(2147483648),(NULL);",
                "SELECT 1 UNION ALL SELECT 2.5;",
                "VALUES (1),('2'),(NULL);",
                "SELECT NULL UNION ALL SELECT 'value';",
                "SELECT CASE WHEN TRUE THEN '2' ELSE 1 END,COALESCE(NULL,'value'),"
                "GREATEST('2',1),NULLIF(NULL,1),NULLIF(1,2.2),NULLIF(1,2147483648),"
                "NULLIF(1::REAL,2::DOUBLE PRECISION);",
                "SELECT CAST(1 AS REAL) UNION ALL SELECT 2::NUMERIC;",
                "WITH RECURSIVE r(n) AS (SELECT 2147483648 UNION ALL SELECT 1 FROM r WHERE n>1) SELECT n FROM r;",
                "WITH RECURSIVE r(n) AS (SELECT 9223372036854775808 UNION ALL SELECT 1.5 FROM r WHERE n>2) SELECT n FROM r;",
                "WITH RECURSIVE r(n) AS (SELECT CAST(1 AS NUMERIC(4,1)) UNION ALL SELECT CAST(n+1.0 AS NUMERIC(4,1)) FROM r WHERE n<2) SELECT n FROM r;"}) {
            const auto local=compile(Dialect::PostgreSQL,sql);
            QVERIFY2(local->ok(),local->diagnostics.empty()?"bad fixture":local->diagnostics.front().message.c_str());
            const auto native=db->execute(sql,{},token);
            QVERIFY2(native.error.isEmpty(),qPrintable(native.error));
            const auto &query=local->model.front();
            QCOMPARE(query.projections.size(),std::size_t(native.columnTypes.size()));
            for(std::size_t i=0;i<query.projections.size();++i) {
                const auto inferred=local->model.types.get(query.expressions.at(query.projections[i].expression).sqlType);
                const auto expected=catalogTypeFromText(Dialect::PostgreSQL,native.columnTypes.at(i).toStdString());
                QVERIFY2(expected && inferred.family==expected->family && inferred.bits==expected->bits,sql);
            }
        }
        const auto serverError=[&](const char *sql) -> QString {
            try { return db->execute(sql,{},token).error; }
            catch (const std::runtime_error &e) { return QString::fromUtf8(e.what()); }
        };
        for (const auto sql : {
                "WITH RECURSIVE r(n) AS (SELECT 1 UNION ALL SELECT n+2147483648 FROM r WHERE n<2) SELECT n FROM r;",
                "WITH RECURSIVE r(n) AS (VALUES (1) UNION ALL SELECT n+1.5 FROM r WHERE n<2) SELECT n FROM r;",
                "WITH RECURSIVE r(n) AS (SELECT CAST(1 AS NUMERIC(4,1)) UNION ALL SELECT n+1.0 FROM r WHERE n<2) SELECT n FROM r;"}) {
            const auto local=compile(Dialect::PostgreSQL,sql);
            QVERIFY2(local->syntaxValid && !local->ok(),sql);
            const auto error=serverError(sql);
            QVERIFY2(error.contains("non-recursive term"),qPrintable(error));
        }
        for (const auto *sql : {
                "SELECT NULL UNION SELECT NULL UNION SELECT 1;",
                "SELECT CASE WHEN TRUE THEN 1 ELSE 'x'::TEXT END;",
                "SELECT COALESCE(1,'x'::TEXT);",
                "SELECT GREATEST(TRUE,1);"}) {
            const auto local=compile(Dialect::PostgreSQL,sql);
            QVERIFY2(local->syntaxValid && !local->ok(),sql);
            QVERIFY2(serverError(sql).contains("cannot be matched"),sql);
        }
        const auto ambiguous="SELECT 9%-2;";
        QVERIFY(!compile(Dialect::PostgreSQL,ambiguous)->ok());
        const auto error=serverError(ambiguous);
        QVERIFY2(error.contains("%-"),qPrintable(error));
    }catch(const std::exception &e){QFAIL(e.what());}}
    void preparedParameterTypesMatchOwnedPostgres() {try {
        using namespace navidog;
        using namespace navidog::sql::compiler;
        auto db=connectDatabase(profile_);
        const auto token=cancellation();
        const std::string query="SELECT $1::BIGINT,COALESCE($2,1),CASE WHEN $3 THEN $2 ELSE 0 END,"
            "(SELECT $1 WHERE $4=2) LIMIT $5;";
        const auto local=compile(Dialect::PostgreSQL,query);
        QVERIFY(local->ok());
        auto prepared=db->execute(QString::fromStdString("PREPARE owned_parameters AS "+query),{},token);
        QVERIFY2(prepared.error.isEmpty(),qPrintable(prepared.error));
        const auto types=db->execute("SELECT p.name,t.name::text,t.pos "
            "FROM pg_prepared_statements p CROSS JOIN LATERAL "
            "unnest(p.parameter_types) WITH ORDINALITY AS t(name,pos) "
            "WHERE p.name='owned_parameters' ORDER BY t.pos;"
            ,{},token);
        QVERIFY2(types.error.isEmpty(),qPrintable(types.error));
        QCOMPARE(types.rows.size(),qsizetype(5));
        std::map<std::string,SqlType> server;
        for (const auto &entry:types.rows) {
            const auto type=catalogTypeFromText(Dialect::PostgreSQL,entry.at(1).toString().toStdString());
            QVERIFY(type.has_value());
            server.emplace("$"+entry.at(2).toString().toStdString(),*type);
        }
        for (const auto &q:local->model.queries)
            for (const auto &expression:q.expressions) {
                if (expression.kind!=ExprKind::Parameter) continue;
                const auto &expected=server.at(expression.value);
                const auto &actual=local->model.types.get(expression.sqlType);
                QCOMPARE(actual.family,expected.family);
                QCOMPARE(actual.bits,expected.bits);
            }
        const auto released=db->execute("DEALLOCATE owned_parameters;",{},token);
        QVERIFY2(released.error.isEmpty(),qPrintable(released.error));
    }catch(const std::exception &e){QFAIL(e.what());}}
    void dmlTargetParameterTypesMatchOwnedPostgres() {try {
        using namespace navidog;
        using namespace navidog::sql::compiler;
        auto db=connectDatabase(profile_);
        const auto token=cancellation();
        const auto run=[&](const QString &sql) {
            auto result=db->execute(sql,{},token);
            if (!result.error.isEmpty()) throw std::runtime_error(result.error.toStdString());
            return result;
        };
        run("CREATE TABLE public.bind_dml_owned(id INT PRIMARY KEY, qty BIGINT, title VARCHAR(12), tags TEXT[], "
            "total NUMERIC(8,3), enabled BOOLEAN)");
        const auto snapshot=collectMetadata(*db,profile_,"public",token);
        const MetadataCatalog catalog(std::make_shared<const MetadataSnapshot>(snapshot));
        const NameResolutionContext scope{profile_.database.toStdString(),"public",Dialect::PostgreSQL};
        for (const auto *sql : {
                "INSERT INTO public.bind_dml_owned(title,id,qty,tags,total,enabled) VALUES ($1,$2,$3,$4,$5,$6);",
                "INSERT INTO public.bind_dml_owned VALUES ($1,$2,$3,DEFAULT,DEFAULT,DEFAULT);",
                "INSERT INTO public.bind_dml_owned(id,qty,total) SELECT $1,$2,$3;",
                "UPDATE public.bind_dml_owned SET (title,qty)=($1,$2), tags=$3 WHERE id=$4 AND NOT $5 "
                "RETURNING $2+1::BIGINT AS next_value;",
                "INSERT INTO public.bind_dml_owned(id) VALUES ($1) ON CONFLICT(id) DO UPDATE SET title=$2 WHERE $3;",
                "MERGE INTO public.bind_dml_owned t USING (SELECT 1 id) s ON t.id=s.id "
                "WHEN MATCHED THEN UPDATE SET qty=$1 WHEN NOT MATCHED THEN INSERT(id,title) VALUES ($2,$3);"}) {
            const auto parsed=compile(scope.dialect,sql,{},&catalog,scope);
            QVERIFY2(parsed->ok(),parsed->diagnostics.empty()?"no DML model":parsed->diagnostics.front().message.c_str());
            run(QString("PREPARE owned_bound_dml AS ")+sql);
            const auto types=run("SELECT t.name::text,t.pos FROM pg_prepared_statements p CROSS JOIN LATERAL "
                "unnest(p.parameter_types) WITH ORDINALITY AS t(name,pos) WHERE p.name='owned_bound_dml' ORDER BY t.pos;");
            std::map<std::string,SqlType> native;
            for (const auto &row : types.rows) {
                const auto type=catalogTypeFromText(scope.dialect,row.at(0).toString().toStdString());
                QVERIFY(type);
                native.emplace("$"+row.at(1).toString().toStdString(),*type);
            }
            std::set<std::string> seen;
            for (const auto &q : parsed->model.queries) for (const auto &e : q.expressions) {
                if (e.kind!=ExprKind::Parameter) continue;
                const auto &type=parsed->model.types.get(e.sqlType);
                const auto &expected=native.at(e.value);
                QVERIFY2(type.family==expected.family,sql);
                QCOMPARE(type.bits,expected.bits);QCOMPARE(type.arrayDimensions,expected.arrayDimensions);
                QVERIFY(!type.precision && !type.length);
                seen.insert(e.value);
            }
            QCOMPARE(seen.size(),native.size());
            if (!parsed->model.front().returning.empty()) {
                const auto &root=parsed->model.front();
                const auto &type=parsed->model.types.get(root.expressions.at(root.returning.front().expression).sqlType);
                QCOMPARE(type.family,TypeFamily::Integer);QCOMPARE(type.bits,std::optional<int>(64));
                const auto nativeResult=run("EXECUTE owned_bound_dml('test',1,ARRAY['x'],1,false);");
                QCOMPARE(nativeResult.columnTypes,QStringList{"bigint"});
            }
            run("DEALLOCATE owned_bound_dml");
        }
        const auto ambiguous=compile(scope.dialect,
            "UPDATE public.bind_dml_owned SET qty=$1 RETURNING $1+1;",{},&catalog,scope);
        QVERIFY(ambiguous->ok());
        for (const auto &e : ambiguous->model.front().expressions)
            if (e.kind==ExprKind::Parameter)
                QCOMPARE(ambiguous->model.types.get(e.sqlType).family,TypeFamily::Unknown);
        run("DROP TABLE public.bind_dml_owned");
    }catch(const std::exception &e){QFAIL(e.what());}}
    void intervalTypeContextsMatchOwnedPostgres() {try {
      using namespace navidog;using namespace navidog::sql::compiler;
      auto db=connectDatabase(profile_);auto token=cancellation();
      auto run=[&](const QString &sql){auto r=db->execute(sql,{},token);if(!r.error.isEmpty())throw std::runtime_error(r.error.toStdString());return r;};
      auto definition=[&](const QString &sql){auto r=db->executeDefinition(sql,{},token);if(!r.error.isEmpty())throw std::runtime_error(r.error.toStdString());};
      run("CREATE TABLE public.interval_types_owned(a interval year to month,b interval day to second(3))");
      auto metadata=collectMetadata(*db,profile_,"public",token);MetadataCatalog catalog(std::make_shared<const MetadataSnapshot>(metadata));
      auto c=compile(Dialect::PostgreSQL,"SELECT a,b FROM public.interval_types_owned;",{},&catalog,{profile_.database.toStdString(),"public",Dialect::PostgreSQL});QVERIFY(c->ok());
      const auto &q=c->model.front();const auto a=c->model.types.get(q.expressions[q.projections[0].expression].sqlType),b=c->model.types.get(q.expressions[q.projections[1].expression].sqlType);
      QCOMPARE(a.intervalLeading,IntervalField::Year);QCOMPARE(a.intervalTrailing,IntervalField::Month);QCOMPARE(b.precision,std::optional<int>(3));
      definition("CREATE FUNCTION public.interval_signature_owned(p interval day to second(3)) RETURNS interval hour to second(2) LANGUAGE plpgsql AS $$ BEGIN RETURN p; END; $$;");
      QCOMPARE(run("SELECT public.interval_signature_owned(INTERVAL '1-2' YEAR TO MONTH)::text").rows[0][0].toString(),QString("1 year 2 mons"));
      definition("CREATE FUNCTION public.interval_local_owned(p interval) RETURNS interval LANGUAGE plpgsql AS $$ DECLARE v interval day to second(1):=p; BEGIN RETURN v; END; $$;");
      QCOMPARE(run("SELECT EXTRACT(SECOND FROM public.interval_local_owned(INTERVAL '12.3456 second'))::text").rows[0][0].toString(),QString("12.300000"));
      run("DROP FUNCTION public.interval_signature_owned(interval)");run("DROP FUNCTION public.interval_local_owned(interval)");run("DROP TABLE public.interval_types_owned");
    }catch(const std::exception &e){QFAIL(e.what());}}
    void nativeTypeFactsAndIgnoredRoutineTypmods() {try {
      using namespace navidog;using namespace navidog::sql::compiler;
      auto db=connectDatabase(profile_);auto token=cancellation();
      auto run=[&](const QString &sql){auto r=db->execute(sql,{},token);if(!r.error.isEmpty())throw std::runtime_error(r.error.toStdString());return r;};
      auto definition=[&](const QString &sql){auto r=db->executeDefinition(sql,{},token);if(!r.error.isEmpty())throw std::runtime_error(r.error.toStdString());};
      run("CREATE TABLE public.owned_native_types(amount numeric(12,3),label varchar(30),moment timestamp(3) with time zone,items integer[])");
      auto snapshot=collectMetadata(*db,profile_,"public",token);QVERIFY(!snapshot.typeFacts.empty());
      MetadataCatalog catalog(std::make_shared<const MetadataSnapshot>(snapshot));
      NameResolutionContext names{profile_.database.toStdString(),"public",Dialect::PostgreSQL};
      auto parsed=compile(Dialect::PostgreSQL,"SELECT amount,label,moment,items FROM public.owned_native_types;",{},&catalog,names);
      QVERIFY2(parsed->ok(),parsed->diagnostics.empty()?"no model":parsed->diagnostics.front().message.c_str());
      const auto &q=parsed->model.front();auto type=[&](int i){return parsed->model.types.get(q.expressions[q.projections[i].expression].sqlType);};
      QCOMPARE(type(0).precision,std::optional<int>(12));QCOMPARE(type(0).scale,std::optional<int>(3));
      QCOMPARE(type(1).length,std::optional<std::uint64_t>(30));QCOMPARE(type(2).timeZone,TimeZoneKind::With);QCOMPARE(type(3).arrayDimensions,1);
      const QString function="CREATE FUNCTION public.owned_ignored_typmod(p numeric(6,2)) RETURNS numeric(8,3) LANGUAGE plpgsql AS $$ BEGIN RETURN p; END; $$;";
      definition(function);QCOMPARE(run("SELECT public.owned_ignored_typmod(123456789.12345)::text").rows[0][0].toString(),QString("123456789.12345"));
      auto routine=compileRoutine(Dialect::PostgreSQL,function.toStdString());QVERIFY(routine.ok());
      bool parameter=false;for(const auto &node:routine.model.nodes)for(const auto &query:node.code.queries)for(const auto &expression:query.expressions)if(expression.localSymbol) {
        const auto &t=node.code.types.get(expression.sqlType);QVERIFY(!t.precision && !t.scale);parameter=true;
      }QVERIFY(parameter);
      const std::string invalidSignature="CREATE FUNCTION public.owned_ignored_invalid_typmod(p numeric(0)) RETURNS numeric(0) LANGUAGE plpgsql AS $$ BEGIN RETURN p; END; $$;";
      bool accepted=false;try{definition(QString::fromStdString(invalidSignature));accepted=true;}catch(const std::exception &){}
      QVERIFY(!accepted);QVERIFY(!compileRoutine(Dialect::PostgreSQL,invalidSignature).ok());
      definition("CREATE FUNCTION public.owned_local_typmod(p numeric) RETURNS numeric LANGUAGE plpgsql AS $$ DECLARE v numeric(8,3):=p; BEGIN RETURN v; END; $$;");
      QCOMPARE(run("SELECT public.owned_local_typmod(12.3456)::text").rows[0][0].toString(),QString("12.346"));
      auto routineSnapshot=collectMetadata(*db,profile_,"public",token);
      MetadataCatalog routines(std::make_shared<const MetadataSnapshot>(routineSnapshot));
      const QString caller="CREATE FUNCTION public.owned_typed_caller(p numeric) RETURNS numeric LANGUAGE plpgsql AS $$ "
                           "DECLARE v numeric:=p; BEGIN RETURN public.owned_ignored_typmod(v); END; $$;";
      definition(caller);
      const auto callerModel=compileRoutine(Dialect::PostgreSQL,caller.toStdString(),{},&routines,names);
      QVERIFY2(callerModel.ok(),callerModel.diagnostics.empty()?"no routine IR":callerModel.diagnostics.front().message.c_str());
      bool sawCall=false,sawLocal=false;
      for (const auto &node : callerModel.model.nodes) for (const auto &block : node.code.queries)
          for (const auto &expr : block.expressions) {
              sawLocal |= expr.localSymbol!=0;
              if (expr.kind!=ExprKind::Call || expr.name.empty() || expr.name.back()!="owned_ignored_typmod") continue;
              const auto &type=node.code.types.get(expr.sqlType);
              QCOMPARE(type.family,TypeFamily::Decimal);
              QVERIFY(!type.precision && !type.scale);
              sawCall=true;
          }
      QVERIFY(sawCall && sawLocal);
      QCOMPARE(run("SELECT public.owned_typed_caller(123456.12345)::text").rows[0][0].toString(),QString("123456.12345"));
      run("DROP FUNCTION public.owned_typed_caller(numeric)");
      run("DROP FUNCTION public.owned_local_typmod(numeric)");run("DROP FUNCTION public.owned_ignored_typmod(numeric)");run("DROP TABLE public.owned_native_types");
    }catch(const std::exception &e){QFAIL(e.what());}}
    void caseAggregatesLowerToNativeFiltersAndPreservePostgresTypes(){try {
      using namespace navidog;
      using namespace navidog::sql::compiler;
      namespace opt=navidog::sql::optimizer;
      auto db=connectDatabase(profile_);
      auto token=cancellation();
      const auto run=[&](const std::string& sql) {
          auto result=db->execute(QString::fromStdString(sql),{},token);
          if(!result.error.isEmpty())throw std::runtime_error(result.error.toStdString());
          return result;
      };
      run("CREATE TABLE public.owned_case_filter(g integer, p integer, v bigint)");
      run("INSERT INTO public.owned_case_filter VALUES "
          "(1,2,9223372036854775807),(1,3,9223372036854775807),(1,1,NULL),"
          "(1,-1,1),(1,NULL,1),(2,NULL,1),(2,0,NULL)");
      auto snapshot=std::make_shared<const MetadataSnapshot>(collectMetadata(*db,profile_,"public",token));
      MetadataCatalog catalog(snapshot);
      const NameResolutionContext names{profile_.database.toStdString(),"public",Dialect::PostgreSQL};
      opt::PlannerOptions options;
      options.heuristic.rules={opt::HepRule::AggregateCaseToFilter};
      const auto projections=
          "COUNT(CASE WHEN p > 0 THEN 1 END) AS matched,"
          "COUNT(CASE WHEN p > 0 THEN v ELSE NULL END) AS present,"
          "MIN(CASE WHEN p > 0 THEN v END) AS smallest,"
          "MAX(CASE WHEN p IS NOT NULL THEN v END) AS largest,"
          "SUM(CASE WHEN p > 0 THEN v END) AS total";
      const auto verify=[&](const std::string& original) {
          const auto compiled=compile(Dialect::PostgreSQL,original,{},&catalog,names);
          QVERIFY2(compiled->ok(),compiled->diagnostics.empty()?"no model":compiled->diagnostics.front().message.c_str());
          const auto plan=opt::optimize(compiled->model,nullptr,options);
          QVERIFY2(plan.ok(),plan.diagnostics.empty()?"no plan":plan.diagnostics.front().message.c_str());
          const auto event=std::find_if(plan.rules.begin(),plan.rules.end(),
              [](const auto& item){return item.rule=="HepAggregateCaseToFilter";});
          QVERIFY(event!=plan.rules.end());
          auto program=plan.logical.program;
          const auto& projection=plan.logical.nodes.at(event->after);
          auto& query=program.queries.at(projection.query-1);
          for(std::size_t i=0;i<query.projections.size();++i)
              query.projections[i].expression=projection.keys[i].expression;
          auto generated=generateSql(program,Dialect::PostgreSQL);
          QVERIFY2(generated.ok(),generated.diagnostics.empty()?"no SQL":generated.diagnostics.front().message.c_str());
          const auto sql=QString::fromStdString(generated.sql);
          QVERIFY2(sql.contains("COUNT(*) FILTER",Qt::CaseInsensitive),qPrintable(sql));
          QVERIFY2(!sql.contains("CASE WHEN ",Qt::CaseInsensitive),qPrintable(sql));
          QCOMPARE(run(generated.sql).rows,run(original).rows);
          const auto types=[&](std::string sql) {
              if(sql.back()==';')sql.pop_back();
              return run("SELECT pg_typeof(matched)::text,pg_typeof(present)::text,"
                         "pg_typeof(smallest)::text,pg_typeof(largest)::text,pg_typeof(total)::text"
                         " FROM ("+sql+") AS p").rows;
          };
          QCOMPARE(types(generated.sql),types(original));
      };
      verify(std::string("SELECT g,")+projections+" FROM public.owned_case_filter GROUP BY g ORDER BY g;");
      verify(std::string("SELECT ")+projections+" FROM public.owned_case_filter WHERE p=999;");
      auto unsafe=compile(Dialect::PostgreSQL,
          "SELECT SUM(CASE WHEN p > 0 THEN v ELSE 0 END) FROM public.owned_case_filter;",{},&catalog,names);
      QVERIFY(unsafe->ok());
      auto base=opt::optimize(unsafe->model,nullptr,options);
      QVERIFY(base.ok());
      QVERIFY(std::none_of(base.rules.begin(),base.rules.end(),[](const auto& item){
          return item.rule=="HepAggregateCaseToFilter";
      }));
    } catch(const std::exception& e){QFAIL(e.what());}}
    void literalValuesReductionMatchesOwnedPostgres(){try {
      using namespace navidog;
      using namespace navidog::sql::compiler;
      namespace opt=navidog::sql::optimizer;
      auto db=connectDatabase(profile_);
      auto token=cancellation();
      const auto run=[&](const std::string &sql) {
          auto result=db->execute(QString::fromStdString(sql),{},token);
          if (!result.error.isEmpty()) throw std::runtime_error(result.error.toStdString());
          return result;
      };
      opt::PlannerOptions options;
      options.heuristic.rules={opt::HepRule::ValuesReduce};
      const auto verify=[&](const std::string &predicate,std::size_t count) {
          const std::string sql="SELECT flag AS kept,a AS amount,a AS repeated FROM "
            "(VALUES (1,TRUE),(2,FALSE),(NULL,TRUE),(2,FALSE),(-3,NULL),(5,TRUE)) "
            "AS v(a,flag) WHERE "+predicate;
          const auto parsed=compile(Dialect::PostgreSQL,sql);
          QVERIFY2(parsed->ok(),parsed->diagnostics.empty()?"no model":parsed->diagnostics.front().message.c_str());
          const auto plan=opt::optimize(parsed->model,nullptr,options);
          QVERIFY2(plan.ok(),plan.diagnostics.empty()?"no plan":plan.diagnostics.front().message.c_str());
          std::vector<const opt::RuleEvent *> reductions;
          for (const auto &event:plan.rules)
              if (event.rule=="HepValuesReduce") reductions.push_back(&event);
          QCOMPARE(reductions.size(),count?std::size_t(2):std::size_t(1));
          const auto &values=plan.logical.nodes.at(reductions.back()->after);
          QCOMPARE(values.kind,opt::RelKind::Values);
          QCOMPARE(values.valueCount,count);
          const auto baseline=static_cast<std::size_t>(std::count_if(plan.logical.nodes.begin(),plan.logical.nodes.end(),
            [](const auto &node){return node.kind==opt::RelKind::Filter;}));
          QVERIFY(baseline>0);
          auto replacement=std::string("SELECT NULL::boolean AS kept,NULL::integer AS amount,NULL::integer AS repeated WHERE FALSE");
          if (count) {
              QCOMPARE(values.outputs.size(),std::size_t(3));
              QCOMPARE(QString::fromStdString(values.outputs[0].name),QString("kept"));
              QCOMPARE(QString::fromStdString(values.outputs[1].name),QString("amount"));
              QCOMPARE(QString::fromStdString(values.outputs[2].name),QString("repeated"));
              replacement="SELECT * FROM (VALUES ";
              for (std::size_t row=0;row<count;++row) {
                  if (row) replacement+=",";
                  replacement+="(";
                  for (std::size_t column=0;column<3;++column) {
                      if (column) replacement+=",";
                      const auto ref=values.keys.at(row*3+column);
                      const auto &expression=plan.logical.program.queries.at(ref.query-1).expressions.at(ref.expression);
                      replacement+=sql.substr(expression.origin.begin,expression.origin.end-expression.origin.begin);
                      replacement+=column?"::integer":"::boolean";
                  }
                  replacement+=")";
              }
              replacement+=") AS r(kept,amount,repeated)";
          }
          const auto describe=[&](const std::string &query) {
              return run("SELECT q.*,pg_typeof(kept)::text,pg_typeof(amount)::text,"
                         "pg_typeof(repeated)::text FROM ("+query+") q ORDER BY 2,1,3");
          };
          const auto original=describe(sql);
          const auto reduced=describe(replacement);
          QCOMPARE(reduced.rows,original.rows);
          QCOMPARE(reduced.columns,original.columns);
          QCOMPARE(reduced.columnTypes,original.columnTypes);
      };
      verify("a<>1 AND (flag=FALSE OR flag IS NULL)",3);
      verify("a IS NULL",1);
      verify("a=999",0);
      verify("a IN (2,NULL)",2);
      verify("a NOT IN (2,NULL)",0);
      verify("a BETWEEN -3 AND 2 AND flag IS NOT TRUE",3);
      verify("flag IS UNKNOWN",1);
      for (const auto *barrier:{
          "SELECT a FROM (VALUES (1),(2147483648)) v(a) WHERE a>0",
          "SELECT a FROM (VALUES (1),(2)) v(a) WHERE random()>0.1",
          "WITH v(a) AS MATERIALIZED (VALUES (1),(2)) SELECT a FROM v WHERE a=1"}) {
          const auto model=compile(Dialect::PostgreSQL,barrier);
          QVERIFY(model->ok());
          const auto plan=opt::optimize(model->model,nullptr,options);
          QVERIFY(plan.ok());
          QVERIFY(std::none_of(plan.rules.begin(),plan.rules.end(),[](const auto &event){
              return event.rule=="HepValuesReduce";
          }));
      }
    } catch(const std::exception &e){QFAIL(e.what());}}
    void federatedScanPushesExactWidthsAndNullTests() {try {
        using namespace navidog;
        using namespace navidog::sql::compiler;
        using namespace navidog::sql::optimizer;
        auto db=connectDatabase(profile_);
        const auto token=cancellation();
        const auto run=[&](const QString &sql) {
            const auto result=db->execute(sql,{},token);
            if (!result.error.isEmpty()) throw std::runtime_error(result.error.toStdString());
            return result;
        };
        run("CREATE TABLE public.push_wide_owned(k smallint,v bigint,note text)");
        run("INSERT INTO public.push_wide_owned SELECT i::smallint,i::bigint,'present' FROM generate_series(1,2048) i");
        run("INSERT INTO public.push_wide_owned VALUES "
            "(-32768,-9223372036854775808,NULL),(32767,9223372036854775807,'present'),(NULL,NULL,'present')");
        navidog::sql::compiler::TableDefinition source;
        source.database="owned";source.schema="public";source.name="wide";
        const auto type=[](const char *name) {
            return std::make_shared<const SqlType>(*catalogTypeFromText(Dialect::PostgreSQL,name));
        };
        source.columns={{"k",ValueType::Integer,type("smallint")},
                        {"v",ValueType::Integer,type("bigint")},
                        {"note",ValueType::String,type("text")}};
        MemoryCatalog catalog;
        catalog.tables={source};
        const NameResolutionContext names{"owned","public",Dialect::PostgreSQL};
        const std::vector<FederatedTableBinding> bindings{
            {"owned","public","wide",DatabaseKind::PostgreSQL,db.get(),"public","push_wide_owned"}};
        const auto compileQuery=[&](const std::string &text) {
            auto compiled=compile(Dialect::PostgreSQL,text,{},&catalog,names);
            if (!compiled->ok()) throw std::runtime_error(compiled->diagnostics.front().message);
            return compiled;
        };
        InterpreterLimits limits;
        limits.maxRows=1;
        const auto number=[](std::int64_t value){return LocalValue{value};};
        const LocalValue null;
        for (const auto &item:std::vector<std::pair<std::string,LocalValue>>{
                 {"SELECT v FROM owned.public.wide WHERE -9223372036854775808>=v;",
                   number(std::numeric_limits<std::int64_t>::min())},
                 {"SELECT k FROM owned.public.wide WHERE k < -32767;",number(-32768)},
                 {"SELECT v FROM owned.public.wide WHERE v>9223372036854775806 AND v IS NOT NULL;",
                   number(std::numeric_limits<std::int64_t>::max())},
                 {"SELECT v FROM owned.public.wide WHERE v = ANY(ARRAY[-9223372036854775808,NULL,-9223372036854775808]);",
                   number(std::numeric_limits<std::int64_t>::min())},
                 {"SELECT k FROM owned.public.wide WHERE k = ANY(ARRAY[-32768,-32768,99999,NULL]);",number(-32768)},
                 {"SELECT k FROM owned.public.wide WHERE k IN (32767,-500000,32767,NULL);",number(32767)},
                 {"SELECT k FROM owned.public.wide WHERE k IN (1,2,3,NULL) AND k < 2;",number(1)},
                 {"SELECT k FROM owned.public.wide WHERE k IN (1,2,NULL) AND k = ANY(ARRAY[2,3,NULL]);",number(2)},
                 {"SELECT k FROM owned.public.wide WHERE k IN (1,2,3) AND v IN (2);",number(2)},
                 {"SELECT k FROM owned.public.wide WHERE k IN (1,2) AND v IS NOT NULL AND v > 1;",number(2)},
                 {"SELECT k FROM owned.public.wide WHERE k IN (1,2,3,NULL) ORDER BY k DESC LIMIT 1;",number(3)},
                 {"SELECT k FROM owned.public.wide WHERE k=-32768 OR k IN(-32768,99999) OR k=NULL;",number(-32768)},
                 {"SELECT k FROM owned.public.wide WHERE (k=1 OR k=2) AND v>1;",number(2)},
                 {"SELECT k FROM owned.public.wide WHERE k=1 OR k=ANY(ARRAY[2,3,NULL]) ORDER BY k DESC LIMIT 1;",number(3)},
                 {"SELECT k FROM owned.public.wide WHERE k IN (1,2,3) AND k = ANY(ARRAY[2,3]) "
                  "ORDER BY k ASC NULLS FIRST LIMIT 1;",number(2)},
                 {"SELECT k FROM owned.public.wide WHERE k IS NULL;",null},
                 {"SELECT note FROM owned.public.wide WHERE note IS NULL;",null}}) {
            const auto compiled=compileQuery(item.first);
            const auto result=executeFederatedQuery(compiled->model,bindings,token,limits);
            QVERIFY2(result.ok(),(item.first+" "+(result.diagnostics.empty()?"no rows":
                                                  result.diagnostics.front().message)).c_str());
            QVERIFY(result.rows==std::vector<LocalRow>{{item.second}});
        }
        for (const auto *sql:{"SELECT k FROM owned.public.wide WHERE k = ANY(ARRAY[]::int[]);",
                              "SELECT k FROM owned.public.wide WHERE k = ANY(NULL::int[]);",
                              "SELECT k FROM owned.public.wide WHERE k IN (1,2,NULL) AND k IN (3,4,NULL);"}) {
            const auto compiled=compileQuery(sql);
            const auto empty=executeFederatedQuery(compiled->model,bindings,token,limits);
            QVERIFY2(empty.ok(),empty.diagnostics.empty()?sql:empty.diagnostics.front().message.c_str());
            QVERIFY(empty.rows.empty());
        }
        const auto parsed=compileQuery("SELECT v FROM owned.public.wide WHERE v=$1;");
        const auto prepared=prepareLocalQuery(parsed->model);
        QVERIFY(prepared.ok());
        const auto max=number(std::numeric_limits<std::int64_t>::max());
        const auto result=executeFederatedQuery(*prepared.query,{max},bindings,token,limits);
        QVERIFY2(result.ok(),result.diagnostics.empty()?"no bigint row":result.diagnostics.front().message.c_str());
        QVERIFY(result.rows==std::vector<LocalRow>{{max}});
        const std::string preparedMembers="SELECT k FROM owned.public.wide "
            "WHERE k IN ($1,$2,NULL) ORDER BY k DESC LIMIT 1;";
        const std::string nativeMembers="SELECT k FROM public.push_wide_owned "
            "WHERE k IN ($1,$2,NULL) ORDER BY k DESC LIMIT 1;";
        run(QString::fromStdString("PREPARE owned_member_bindings AS "+nativeMembers));
        const auto nativeParameters=run("SELECT parameter_types::text FROM pg_prepared_statements "
                                        "WHERE name='owned_member_bindings'");
        QCOMPARE(nativeParameters.rows.front().front().toString(),QString("{smallint,smallint}"));
        const auto memberQuery=prepareLocalQuery(compileQuery(preparedMembers)->model);
        QVERIFY2(memberQuery.ok(),memberQuery.diagnostics.empty()?"no member query":memberQuery.diagnostics.front().message.c_str());
        QVERIFY(localParameters(*memberQuery.query).size()==2 &&
                localParameters(*memberQuery.query)[0].type.bits==16 && localParameters(*memberQuery.query)[1].type.bits==16);
        for (const auto &values:std::vector<LocalRow>{{number(1),number(3)},
                                                     {null,number(-32768)},{null,null}}) {
            QVector<QVariant> args;
            for (const auto &value:values) {
                const auto *integer=std::get_if<std::int64_t>(&value);
                args.push_back(integer?QVariant::fromValue<qlonglong>(*integer):QVariant{});
            }
            const auto expected=db->executeBound(QString::fromStdString(nativeMembers),args,{},token);
            QVERIFY2(expected.error.isEmpty(),qPrintable(expected.error));
            const auto page=executeFederatedQuery(*memberQuery.query,values,bindings,token,limits);
            QVERIFY2(page.ok(),page.diagnostics.empty()?"no membership page":page.diagnostics.front().message.c_str());
            QCOMPARE(page.rows.size(),std::size_t(expected.rows.size()));
            if (!page.rows.empty())
                QCOMPARE(std::get<std::int64_t>(page.rows.front().front()),expected.rows.front().front().toLongLong());
        }
        const auto mixed=compileQuery("SELECT k FROM owned.public.wide WHERE k IN (1,2) AND v IN (1,2) "
                                      "ORDER BY k DESC LIMIT 1;");
        const auto incomplete=executeFederatedQuery(mixed->model,bindings,token,limits);
        QVERIFY(!incomplete.ok() && incomplete.rows.empty());
        QCOMPARE(incomplete.diagnostics.front().code,DiagnosticCode::ResourceLimit);
        const auto disjuncts=compileQuery("SELECT k FROM owned.public.wide WHERE k IS NULL OR note IS NULL;");
        const auto refused=executeFederatedQuery(disjuncts->model,bindings,token,limits);
        QVERIFY(!refused.ok() && refused.rows.empty());
        QCOMPARE(refused.diagnostics.front().code,DiagnosticCode::ResourceLimit);
        const auto different=compileQuery("SELECT k FROM owned.public.wide WHERE k=1 OR v=2;");
        const auto preserved=executeFederatedQuery(different->model,bindings,token,limits);
        QVERIFY(!preserved.ok() && preserved.rows.empty());
        QCOMPARE(preserved.diagnostics.front().code,DiagnosticCode::ResourceLimit);
    }catch(const std::exception &e){QFAIL(e.what());}}
    void federatedNumericRetainsTypmodScaleAndExactAggregates() {try {
        using namespace navidog;
        using namespace navidog::sql::compiler;
        using namespace navidog::sql::optimizer;
        auto db=connectDatabase(profile_);
        const auto token=cancellation();
        const auto run=[&](const std::string &sql) {
            const auto result=db->execute(QString::fromStdString(sql),{},token);
            if (!result.error.isEmpty()) throw std::runtime_error(result.error.toStdString());
            return result;
        };
        run("CREATE TABLE public.decimal_scan_owned(id integer,amount numeric(25,4),"
            "rounded numeric(4,-2),variable numeric)");
        run("INSERT INTO public.decimal_scan_owned VALUES "
            "(1,9007199254740993.0001,1250,0.00100),"
            "(2,9007199254740993.0002,-1250,0.00000100000),"
            "(3,NULL,NULL,NULL),(4,-1.0000,1499,-13.400000)");
        const MetadataCatalog catalog(std::make_shared<const MetadataSnapshot>(
            collectMetadata(*db,profile_,"public",token)));
        const NameResolutionContext names{profile_.database.toStdString(),"public",Dialect::PostgreSQL};
        const std::vector<FederatedTableBinding> sources{
            {names.database,"public","decimal_scan_owned",DatabaseKind::PostgreSQL,
             db.get(),"public","decimal_scan_owned"}};
        const auto compileQuery=[&](const std::string &sql) {
            auto compiled=compile(Dialect::PostgreSQL,sql,{},&catalog,names);
            if (!compiled->ok()) throw std::runtime_error(compiled->diagnostics.front().message);
            return compiled;
        };
        const std::string from=" FROM public.decimal_scan_owned";
        const auto projection=run("SELECT amount,rounded"+from+" ORDER BY id");
        QCOMPARE(projection.columnTypes,(QStringList{"numeric(25,4)","numeric(4,-2)"}));
        for (const auto &sql:{
                 "SELECT id,amount,rounded,variable,amount::numeric AS unbounded"+from+" ORDER BY id",
                 "SELECT amount+0.0001 AS adjusted,variable-0.00000001 AS net"+from+
                     " WHERE amount>9007199254740993.0001 ORDER BY 1",
                 "SELECT SUM(amount) AS total,AVG(amount) AS mean,MIN(rounded) AS minimum,"
                     "AVG(variable) AS variable_mean,MAX(amount) AS maximum"+from,
                 "SELECT id,SUM(amount) OVER (ORDER BY id ROWS BETWEEN 1 PRECEDING AND CURRENT ROW) AS running,"
                     "AVG(variable) OVER (ORDER BY id) AS running_mean"+from+" ORDER BY 1",
                 "SELECT rounded,COUNT(*) AS n,SUM(amount) AS total"+from+" GROUP BY rounded ORDER BY 1",
                 "SELECT id,amount*1.250 AS product,(variable * -0.001)::numeric(14,9) AS typed,"
                     "amount::numeric(23,2) AS cents, rounded::smallint AS as_integer"+from+" ORDER BY id",
                 "SELECT id,amount/2.50 AS portion,amount % 2.50 AS residue,"
                     "rounded / -7 AS signed_portion"+from+" ORDER BY id",
                 std::string("SELECT 9.995::numeric(5,2) AS cents, (-9.995)::numeric(5,2) AS negative, "
                             "0.000995::numeric(3,5) AS tiny, 1499.9::numeric(4,-3) AS rounded_down, "
                             "(-1500)::numeric(4,-3) AS rounded_up, (-32767.5)::smallint AS min_int, "
                             "(-0.0049)::numeric(5,2) AS zeroish, NULL::numeric(4,2) AS empty, "
                             "(1000000000.000000001 * -1000000000.000000001) AS carry"),
                 std::string("SELECT 1.0/7 AS repeating, -5.00/3 AS rounded_negative,"
                             "0.0001/3.0 AS small, 0.00/7.0 AS zero,"
                             "18446744073709551614/3.1415 AS large,"
                             "(-1000000000000000000000000.0)%3.14159 AS negative_remainder,"
                             "123.4500%(-2.25) AS remainder,1.0/NULL::numeric AS missing"),
                 std::string("SELECT 1.20e-2 AS small,12.00E1 AS padded,-1.2e+14 AS shifted, "
                             "0.000e2 AS zero,1.234e-5+2.34e-6 AS exact_sum")}) {
            const auto result=executeFederatedQuery(compileQuery(sql)->model,sources,token);
            QVERIFY2(result.ok(),result.diagnostics.empty()?sql.c_str():result.diagnostics.front().message.c_str());
            const auto expected=run(sql);
            QCOMPARE(result.rows.size(),static_cast<std::size_t>(expected.rows.size()));
            for (std::size_t i=0;i<result.rows.size();++i)
                for (std::size_t j=0;j<result.rows[i].size();++j) {
                    const auto &actual=result.rows[i][j];
                    const auto &cell=expected.rows[i][j];
                    if (cell.isNull()) QVERIFY(std::holds_alternative<std::monostate>(actual));
                    else if (expected.columnTypes[j].startsWith("numeric"))
                        QCOMPARE(QString::fromStdString(std::get<LocalNumeric>(actual).decimal()),cell.toString());
                    else QCOMPARE(static_cast<qint64>(std::get<std::int64_t>(actual)),cell.toLongLong());
                }
        }
        for (const std::string sql:{
                 "SELECT 9.995::numeric(3,2) AS cents",
                 "SELECT (-32768.5)::smallint AS below_min",
                 "SELECT 9223372036854775807.5::bigint AS above_max",
                 "SELECT 0.009999::numeric(3,5) AS too_big",
                 "SELECT 1.0 / 0.0 AS zero_divisor",
                 "SELECT 5.0 % 0.0 AS zero_divisor"}) {
            const auto result=executeFederatedQuery(compileQuery(sql)->model,sources,token);
            QVERIFY(!result.ok() && result.rows.empty());
            QCOMPARE(result.diagnostics.front().code,DiagnosticCode::InvalidQueryShape);
            bool failed=false;
            try { run(sql); } catch (const std::exception &) { failed=true; }
            QVERIFY(failed);
        }
        for (const std::string sql:{
                 "SELECT 1.0/10000000000000.0 AS tiny",
                 "SELECT 999999999999999999999999999 / 0.01 AS huge"}) {
            const auto result=executeFederatedQuery(compileQuery(sql)->model,sources,token);
            QVERIFY(!result.ok() && result.rows.empty());
            QCOMPARE(result.diagnostics.front().code,DiagnosticCode::ResourceLimit);
            QCOMPARE(run(sql).rows.size(),qsizetype(1));
        }
        run("INSERT INTO public.decimal_scan_owned(id,amount) "
            "SELECT i,i::numeric FROM generate_series(100,4096) i");
        const std::string narrowSql="SELECT amount"+from+
            " WHERE amount>$1 AND amount<=9.0071992547409930002e15";
        const auto narrow=prepareLocalQuery(compileQuery(narrowSql)->model);
        QVERIFY2(narrow.ok(),narrow.diagnostics.empty()?"no numeric scan plan":narrow.diagnostics.front().message.c_str());
        const auto threshold=parseLocalBinding("9.0071992547409930001e15");
        QVERIFY(threshold && std::holds_alternative<LocalNumeric>(*threshold));
        QVERIFY(!parseLocalBinding("1e+-2") && !parseLocalBinding("+-42"));
        InterpreterLimits limits;
        limits.maxRows=1;
        const auto sparse=executeFederatedQuery(*narrow.query,{*threshold},sources,token,limits);
        QVERIFY2(sparse.ok(),sparse.diagnostics.empty()?"no sparse decimal result":sparse.diagnostics.front().message.c_str());
        QCOMPARE(sparse.rows.size(),std::size_t(1));
        const auto expected=db->executeBound(QString::fromStdString(narrowSql),{QString("9007199254740993.0001")},{},token);
        QVERIFY2(expected.error.isEmpty(),qPrintable(expected.error));
        QCOMPARE(expected.rows.size(),qsizetype(1));
        QCOMPARE(QString::fromStdString(std::get<LocalNumeric>(sparse.rows.front().front()).decimal()),
                 expected.rows.front().front().toString());
        const auto nullThreshold=executeFederatedQuery(*narrow.query,{LocalValue{}},sources,token,limits);
        QVERIFY(nullThreshold.ok() && nullThreshold.rows.empty());
        const auto stable=compileQuery("SELECT amount"+from);
        run("ALTER TABLE public.decimal_scan_owned ALTER COLUMN amount TYPE numeric(26,4)");
        const auto stale=executeFederatedQuery(stable->model,sources,token);
        QVERIFY(!stale.ok() && stale.rows.empty());
        QCOMPARE(stale.diagnostics.front().code,DiagnosticCode::InvalidQueryShape);
        const auto special=compileQuery("SELECT variable"+from+" WHERE id=1");
        run("UPDATE public.decimal_scan_owned SET variable='NaN' WHERE id=1");
        const auto nan=executeFederatedQuery(special->model,sources,token);
        QVERIFY(!nan.ok() && nan.rows.empty());
        QCOMPARE(nan.diagnostics.front().code,DiagnosticCode::UnsupportedSyntax);
    }catch(const std::exception &e){QFAIL(e.what());}}
    void federatedMembershipUsesSparseKeysAndKeepsNullSemantics() {try {
        using namespace navidog;
        using namespace navidog::sql::compiler;
        using namespace navidog::sql::optimizer;
        auto db=connectDatabase(profile_);
        const auto token=cancellation();
        const auto run=[&](const std::string &sql) {
            const auto result=db->execute(QString::fromStdString(sql),{},token);
            if (!result.error.isEmpty()) throw std::runtime_error(result.error.toStdString());
            return result;
        };
        run("CREATE TABLE public.local_members_left_owned(id integer,ref integer)");
        run("CREATE TABLE public.local_members_right_owned(id integer)");
        run("INSERT INTO public.local_members_left_owned VALUES (1,2),(1,2),(3,900000),(4,NULL)");
        run("INSERT INTO public.local_members_right_owned SELECT i FROM generate_series(1,4096) i");
        run("INSERT INTO public.local_members_right_owned VALUES (2),(NULL)");
        const auto snapshot=std::make_shared<const MetadataSnapshot>(collectMetadata(*db,profile_,"public",token));
        const MetadataCatalog catalog(snapshot);
        const NameResolutionContext names{profile_.database.toStdString(),"public",Dialect::PostgreSQL};
        const std::vector<FederatedTableBinding> sources{
            {names.database,"public","local_members_left_owned",DatabaseKind::PostgreSQL,
             db.get(),"public","local_members_left_owned"},
            {names.database,"public","local_members_right_owned",DatabaseKind::PostgreSQL,
             db.get(),"public","local_members_right_owned"}};
        const auto compileQuery=[&](const std::string &sql) {
            auto parsed=compile(Dialect::PostgreSQL,sql,{},&catalog,names);
            if (!parsed->ok()) throw std::runtime_error(parsed->diagnostics.front().message);
            return parsed;
        };
        const std::string from=" FROM public.local_members_left_owned l WHERE ";
        const std::string right=" FROM public.local_members_right_owned r";
        const std::string exists="EXISTS (SELECT 1"+right+" WHERE r.id=l.ref)";
        const std::string order=" ORDER BY 1,2";
        const auto verify=[&](const Interpretation &result,const QueryResult &native) {
            QVERIFY2(result.ok(),result.diagnostics.empty()?"no local rows":result.diagnostics.front().message.c_str());
            std::vector<LocalRow> expected;
            for (const auto &row:native.rows) {
                LocalRow values;
                for (const auto &value:row)
                    values.push_back(value.isNull()?LocalValue{}:LocalValue{std::int64_t(value.toLongLong())});
                expected.push_back(std::move(values));
            }
            QVERIFY(expected==result.rows);
        };
        InterpreterLimits limits;
        limits.maxRows=4;
        for (const auto &predicate:{exists,"NOT "+exists,"l.ref IN (SELECT r.id"+right+")"}) {
            const auto sql="SELECT l.id,l.ref"+from+predicate+order;
            verify(executeFederatedQuery(compileQuery(sql)->model,sources,token,limits),run(sql));
        }
        limits.maxRows=2;
        for (const auto &predicate:{"l.id=1 AND "+exists,
                                    "l.id>=3 AND NOT "+exists,
                                    "l.id=1 AND l.ref IN (SELECT r.id"+right+")"}) {
            const auto sql="SELECT l.id,l.ref"+from+predicate+order;
            const auto parsed=compileQuery(sql);
            const auto baseline=run(sql);
            verify(executeFederatedQuery(parsed->model,sources,token,limits),baseline);
            auto plan=optimize(parsed->model);
            QVERIFY(plan.ok());
            const auto event=std::find_if(plan.rules.begin(),plan.rules.end(),[](const RuleEvent &entry) {
                return entry.rule=="SemiJoinFilterTranspose";
            });
            if (predicate.find("NOT EXISTS")!=std::string::npos) {
                QVERIFY(event==plan.rules.end());
                continue;
            }
            QVERIFY(event!=plan.rules.end());
            for (auto &node:plan.logical.nodes)
                std::replace(node.inputs.begin(),node.inputs.end(),event->before,event->after);
            const auto generated=generatePlanSql(plan.logical,plan.heuristicRoot);
            QVERIFY2(generated.ok(),generated.diagnostics.empty()?sql.c_str():generated.diagnostics.front().message.c_str());
            const auto actual=run(generated.sql);
            QCOMPARE(actual.rows,baseline.rows);
            QCOMPARE(actual.columns,baseline.columns);
            QCOMPARE(actual.columnTypes,baseline.columnTypes);
        }
        limits.maxRows=4;
        const auto preparedSql="SELECT l.id,$1::smallint AS marker"+from+exists+order;
        const auto prepared=prepareLocalQuery(compileQuery(preparedSql)->model);
        QVERIFY2(prepared.ok(),prepared.diagnostics.empty()?"no prepared query":prepared.diagnostics.front().message.c_str());
        for (const int marker:{3,-5}) {
            const auto expected=db->executeBound(QString::fromStdString(preparedSql),{marker},{},token);
            QVERIFY2(expected.error.isEmpty(),qPrintable(expected.error));
            verify(executeFederatedQuery(*prepared.query,{std::int64_t(marker)},sources,token,limits),expected);
        }
        const auto notInSql="SELECT l.id,l.ref"+from+"l.ref NOT IN (SELECT r.id"+right+")"+order;
        verify(executeFederatedQuery(compileQuery(notInSql)->model,sources,token),run(notInSql));

        run("CREATE TABLE public.local_members_not_null_left_owned(id integer,ref integer NOT NULL)");
        run("CREATE TABLE public.local_members_not_null_right_owned(id integer NOT NULL)");
        run("INSERT INTO public.local_members_not_null_left_owned VALUES (1,2),(3,9)");
        run("INSERT INTO public.local_members_not_null_right_owned VALUES (2)");
        const MetadataCatalog initialCatalog(std::make_shared<const MetadataSnapshot>(
            collectMetadata(*db,profile_,"public",token)));
        const std::string changingSql=
            "SELECT l.id,l.ref FROM public.local_members_not_null_left_owned l "
            "WHERE l.ref NOT IN (SELECT r.id FROM public.local_members_not_null_right_owned r)"
            +order;
        const auto original=compile(Dialect::PostgreSQL,changingSql,{},&initialCatalog,names);
        QVERIFY(original->ok());
        const auto planned=optimize(original->model);
        QVERIFY(planned.ok());
        const auto portable=generatePlanSql(planned.logical,planned.heuristicRoot);
        QVERIFY(portable.ok());
        QVERIFY(portable.sql.find(" NOT IN ")!=std::string::npos);
        const auto changed=prepareLocalQuery(original->model);
        QVERIFY(changed.ok());
        const std::vector<FederatedTableBinding> changingSources{
            {names.database,"public","local_members_not_null_left_owned",DatabaseKind::PostgreSQL,
             db.get(),"public","local_members_not_null_left_owned"},
            {names.database,"public","local_members_not_null_right_owned",DatabaseKind::PostgreSQL,
             db.get(),"public","local_members_not_null_right_owned"}};
        verify(executeFederatedQuery(*changed.query,{},changingSources,token),run(changingSql));
        run("ALTER TABLE public.local_members_not_null_left_owned ALTER COLUMN ref DROP NOT NULL");
        run("ALTER TABLE public.local_members_not_null_right_owned ALTER COLUMN id DROP NOT NULL");
        run("INSERT INTO public.local_members_not_null_left_owned VALUES (4,NULL)");
        run("INSERT INTO public.local_members_not_null_right_owned VALUES (NULL)");
        verify(executeFederatedQuery(*changed.query,{},changingSources,token),run(changingSql));
        QCOMPARE(run(portable.sql).rows,run(changingSql).rows);
        run("TRUNCATE public.local_members_not_null_right_owned");
        verify(executeFederatedQuery(*changed.query,{},changingSources,token),run(changingSql));
        QCOMPARE(run(portable.sql).rows,run(changingSql).rows);
    }catch(const std::exception &e){QFAIL(e.what());}}
    void preparedFederatedPagesUseCurrentBindings() {try {
        using namespace navidog;
        using namespace navidog::sql::compiler;
        using namespace navidog::sql::optimizer;
        auto db=connectDatabase(profile_);
        const auto token=cancellation();
        const auto run=[&](const QString &sql) {
            const auto result=db->execute(sql,{},token);
            if (!result.error.isEmpty()) throw std::runtime_error(result.error.toStdString());
            return result;
        };
        run("CREATE TABLE public.prepare_federated_owned(k integer,v integer)");
        run("INSERT INTO public.prepare_federated_owned "
            "SELECT i,i*2 FROM generate_series(-4096,4096) i");
        run("INSERT INTO public.prepare_federated_owned VALUES(-5,-10000),(-5,-10000),(NULL,7),(-5,NULL)");
        navidog::sql::compiler::TableDefinition table;
        table.database="owned";table.schema="public";table.name="events";
        const auto type=std::make_shared<const SqlType>(*catalogTypeFromText(Dialect::PostgreSQL,"integer"));
        table.columns={{"k",ValueType::Integer,type},{"v",ValueType::Integer,type}};
        MemoryCatalog catalog;
        catalog.tables={table};
        const NameResolutionContext names{"owned","public",Dialect::PostgreSQL};
        const auto prepare=[&](const std::string &text) {
            const auto parsed=compile(Dialect::PostgreSQL,text,{},&catalog,names);
            if (!parsed->ok()) throw std::runtime_error(parsed->diagnostics.front().message);
            const auto prepared=prepareLocalQuery(parsed->model);
            if (!prepared.ok()) throw std::runtime_error(prepared.diagnostics.front().message);
            return prepared.query;
        };
        const std::string tail=" WHERE $1>k AND k IS NOT NULL ORDER BY result DESC NULLS LAST,2 LIMIT $2 OFFSET $3";
        const auto query=prepare("SELECT k AS result,v FROM owned.public.events"+tail);
        const QString native="SELECT k AS result,v FROM public.prepare_federated_owned"+QString::fromStdString(tail);
        const std::vector<FederatedTableBinding> sources{
            {"owned","public","events",DatabaseKind::PostgreSQL,db.get(),"public","prepare_federated_owned"}};
        InterpreterLimits budget;
        budget.maxRows=3;
        const auto number=[](std::int64_t n){return LocalValue{n};};
        const LocalValue null;
        const auto verify=[&](LocalRow bindings,QVector<QVariant> nativeArgs) {
            const auto expected=db->executeBound(native,nativeArgs,{},token);
            QVERIFY2(expected.error.isEmpty(),qPrintable(expected.error));
            const auto result=executeFederatedQuery(*query,bindings,sources,token,budget);
            QVERIFY2(result.ok(),result.diagnostics.empty()?"no rows":result.diagnostics.front().message.c_str());
            std::vector<LocalRow> rows;
            for (const auto &row:expected.rows) {
                LocalRow values;
                for (const auto &cell:row) values.push_back(cell.isNull()?null:number(cell.toLongLong()));
                rows.push_back(std::move(values));
            }
            QCOMPARE(result.columns,(std::vector<std::string>{"result","v"}));
            QVERIFY(result.rows==rows);
        };
        verify({number(-3),number(2),number(1)},{-3,2,1});
        verify({number(-4),number(2),null},{-4,2,QVariant{}});
        verify({number(123),number(1),number(2)},{123,1,2});
        verify({number(123),number(0),null},{123,0,QVariant{}});
        const auto ambiguous=prepare(
            "SELECT k,v FROM owned.public.events WHERE k<$1 ORDER BY k DESC LIMIT $2;");
        const auto rejected=executeFederatedQuery(*ambiguous,{number(-4),number(1)},sources,token,budget);
        QVERIFY(!rejected.ok() && rejected.rows.empty());
        QCOMPARE(rejected.diagnostics.front().code,DiagnosticCode::ResourceLimit);
        const auto unlimited=executeFederatedQuery(*query,{number(-4),null,null},sources,token,budget);
        QVERIFY(!unlimited.ok() && unlimited.rows.empty());
        QCOMPARE(unlimited.diagnostics.front().code,DiagnosticCode::ResourceLimit);

        QUrl endpoint("postgresql:///"+profile_.database);
        QUrlQuery parts;
        parts.addQueryItem("host",profile_.host);
        parts.addQueryItem("port",QString::number(profile_.port));
        parts.addQueryItem("user",profile_.user);
        endpoint.setQuery(parts);
        QString program=QCoreApplication::applicationDirPath()+"/navidog-federate";
#ifdef Q_OS_WIN
        program += ".exe";
#endif
        QProcess cli;
        cli.start(program,{"--postgres","owned="+endpoint.toString(QUrl::FullyEncoded),
                           "--table","owned.public.prepare_federated_owned",
                           "--bind","-3","--bind","2","--bind","1","-"});
        QVERIFY2(cli.waitForStarted(5000),qPrintable(cli.errorString()));
        cli.write(QByteArray::fromStdString("SELECT k AS result,v FROM owned.public.prepare_federated_owned"+tail));
        cli.closeWriteChannel();
        QVERIFY(cli.waitForFinished(10000));
        const auto error=cli.readAllStandardError();
        const auto output=cli.readAllStandardOutput();
        QVERIFY2(cli.exitStatus()==QProcess::NormalExit && cli.exitCode()==0,(error+output).constData());
        QCOMPARE(QJsonDocument::fromJson(output).object().value("rows").toArray(),
                 QJsonDocument::fromJson("[[-5,-10000],[-5,-10000]]").array());
    }catch(const std::exception &e){QFAIL(e.what());}}
    void preparedLocalBindingsMatchOwnedPostgres() {try {
      using namespace navidog;
      using namespace navidog::sql::compiler;
      using namespace navidog::sql::optimizer;
      auto db=connectDatabase(profile_);
      const auto token=cancellation();
      const std::string sql="WITH own(k) AS (VALUES (1),(2),(2),(3),(NULL)) "
          "SELECT k, $1::smallint AS n, k+$1::smallint AS adjusted "
          "FROM own WHERE k>=$2 AND $3 ORDER BY k";
      const auto parsed=compile(Dialect::PostgreSQL,sql);
      QVERIFY2(parsed->ok(),parsed->diagnostics.empty()?"no model":parsed->diagnostics.front().message.c_str());
      const auto prepared=prepareLocalQuery(parsed->model);
      QVERIFY2(prepared.ok(),prepared.diagnostics.empty()?"no local query":prepared.diagnostics.front().message.c_str());
      const auto &parameters=localParameters(*prepared.query);
      QCOMPARE(parameters.size(),std::size_t(3));
      QCOMPARE(parameters[0].type.bits.value_or(0),16);
      QCOMPARE(parameters[1].type.bits.value_or(0),32);
      QCOMPARE(parameters[2].type.family,TypeFamily::Boolean);
      const LocalValue null;
      const auto number=[](std::int64_t n) { return LocalValue{n}; };
      const auto verify=[&](const PreparedLocalQuery &query,const std::string &text,
                              LocalRow bindings,QVector<QVariant> args) {
          const auto native=db->executeBound(QString::fromStdString(text),args,{},token);
          QVERIFY2(native.error.isEmpty(),qPrintable(native.error));
          const auto local=executeLocalQuery(query,bindings);
          QVERIFY2(local.ok(),local.diagnostics.empty()?"no result":local.diagnostics.front().message.c_str());
          std::vector<LocalRow> expected;
          for (const auto &row:native.rows) {
              LocalRow values;
              for (int i=0;i<row.size();++i) {
                  const auto &value=row[i];
                  if (value.isNull()) values.push_back(null);
                  else if (native.columnTypes.value(i)==QStringLiteral("boolean")) {
                      QVERIFY(value.toString()==QStringLiteral("t") || value.toString()==QStringLiteral("f"));
                      values.emplace_back(value.toString()==QStringLiteral("t"));
                  } else values.push_back(number(value.toLongLong()));
              }
              expected.push_back(std::move(values));
          }
          QVERIFY(local.rows==expected);
          QStringList columns;
          for (const auto &column:local.columns) columns<<QString::fromStdString(column);
          QCOMPARE(columns,native.columns);
      };
      verify(*prepared.query,sql,{number(-3),number(2),true},{-3,2,true});
      verify(*prepared.query,sql,{null,number(3),true},{QVariant{},3,true});
      verify(*prepared.query,sql,{number(2),number(2),null},{2,2,QVariant{}});
      const auto refused=executeLocalQuery(*prepared.query,{number(32768),number(2),true});
      QVERIFY(!refused.ok());
      QVERIFY(refused.rows.empty());
      bool serverRejected=false;
      try {
          const auto result=db->executeBound(QString::fromStdString(sql),{32768,2,true},{},token);
          serverRejected=!result.error.isEmpty();
      } catch(const std::exception &) { serverRejected=true; }
      QVERIFY(serverRejected);

      const std::string pages="WITH own(k) AS (VALUES (1),(2),(2),(3)) "
          "SELECT k,$1::boolean AS flag FROM own ORDER BY k LIMIT $2 OFFSET $3";
      const auto pageModel=compile(Dialect::PostgreSQL,pages);
      QVERIFY(pageModel->ok());
      const auto page=prepareLocalQuery(pageModel->model);
      QVERIFY2(page.ok(),page.diagnostics.empty()?"no page":page.diagnostics.front().message.c_str());
      verify(*page.query,pages,{true,number(2),number(1)},{true,2,1});
      verify(*page.query,pages,{false,null,number(1)},{false,QVariant{},1});
      verify(*page.query,pages,{null,number(1),null},{QVariant{},1,QVariant{}});
      const auto largest=std::numeric_limits<qint64>::max();
      verify(*page.query,pages,{true,number(largest),number(largest)},{true,largest,largest});

      const std::string decimalSql="SELECT $1+0.0001 AS shifted,$1::numeric(25,4) AS cents,"
                                   "$2*1.25 AS product,$2::numeric(18,3) AS fixed";
      const auto decimalModel=compile(Dialect::PostgreSQL,decimalSql);
      QVERIFY(decimalModel->ok());
      const auto decimalPlan=prepareLocalQuery(decimalModel->model);
      QVERIFY2(decimalPlan.ok(),decimalPlan.diagnostics.empty()?"no numeric plan":
               decimalPlan.diagnostics.front().message.c_str());
      QCOMPARE(localParameters(*decimalPlan.query).size(),std::size_t(2));
      for (const auto &parameter:localParameters(*decimalPlan.query)) {
          QCOMPARE(parameter.type.family,TypeFamily::Decimal);
          QVERIFY(!parameter.type.precision && !parameter.type.scale);
      }
      for (const auto &input:std::vector<std::pair<const char *,const char *>>{
               {"9007199254740993.123465","0.00031"},{"-1250","-0.50"},{"null","12345.6789"}}) {
          LocalRow bindings;
          QVector<QVariant> nativeArgs;
          for (const auto *value:{input.first,input.second}) {
              const auto binding=parseLocalBinding(value);
              QVERIFY(binding);
              bindings.push_back(*binding);
              nativeArgs.push_back(std::string_view(value)=="null"?QVariant{}:QVariant(value));
          }
          const auto native=db->executeBound(QString::fromStdString(decimalSql),nativeArgs,{},token);
          QVERIFY2(native.error.isEmpty(),qPrintable(native.error));
          const auto local=executeLocalQuery(*decimalPlan.query,bindings);
          QVERIFY2(local.ok(),local.diagnostics.empty()?"no numeric rows":local.diagnostics.front().message.c_str());
          QCOMPARE(local.rows.size(),std::size_t(1));
          for (std::size_t i=0;i<local.rows.front().size();++i) {
              const auto &expected=native.rows.front().at(i);
              const auto &actual=local.rows.front().at(i);
              if (expected.isNull()) QVERIFY(std::holds_alternative<std::monostate>(actual));
              else QCOMPARE(QString::fromStdString(std::get<LocalNumeric>(actual).decimal()),expected.toString());
          }
      }
      const auto wrongType=executeLocalQuery(*decimalPlan.query,{true,number(1)});
      QVERIFY(!wrongType.ok() && wrongType.rows.empty());
    }catch(const std::exception &e){QFAIL(e.what());}}
    void filterNonNullProofSurvivesDisjunctionInOwnedPostgres() {try {
      using namespace navidog;
      using namespace navidog::sql::compiler;
      namespace opt=navidog::sql::optimizer;
      auto db=connectDatabase(profile_);
      const auto token=cancellation();
      const auto run=[&](const std::string &sql) {
          auto result=db->execute(QString::fromStdString(sql),{},token);
          if (!result.error.isEmpty()) throw std::runtime_error(result.error.toStdString());
          return result;
      };
      run("CREATE TABLE public.owned_filter_nullable(id bigint, flag integer)");
      run("INSERT INTO public.owned_filter_nullable VALUES(NULL,9),(NULL,NULL),(-4,2),(1,NULL),(1,0),(2,9)");
      auto snapshot=std::make_shared<const MetadataSnapshot>(collectMetadata(*db,profile_,"public",token));
      MetadataCatalog catalog(snapshot);
      const NameResolutionContext names{profile_.database.toStdString(),"public",Dialect::PostgreSQL};
      opt::PlannerOptions options;
      options.heuristic.rules={opt::HepRule::FilterNullability};
      const auto verify=[&](const std::string &condition, bool redundant, int rows, const std::string &group = {}) {
        const auto source="SELECT s.k FROM (SELECT id AS k FROM public.owned_filter_nullable "
            "WHERE "+condition+group+") s WHERE s.k IS NOT NULL ORDER BY 1";
        const auto compiled=compile(Dialect::PostgreSQL,source,{},&catalog,names);
        QVERIFY2(compiled->ok(),compiled->diagnostics.empty()?"no model":compiled->diagnostics.front().message.c_str());
        const auto plan=opt::optimize(compiled->model,nullptr,options);
        QVERIFY(plan.ok());
        QCOMPARE(std::any_of(plan.rules.begin(),plan.rules.end(),[](const auto &event) {
          return event.rule=="HepFilterNullability";
        }),redundant);
        const auto output=opt::generatePlanSql(plan.logical,plan.heuristicRoot);
        QVERIFY2(output.ok(),output.diagnostics.empty()?"no SQL":output.diagnostics.front().message.c_str());
        const auto original=run(source);
        QCOMPARE(original.rows.size(),rows);
        QCOMPARE(run(output.sql).rows,original.rows);
      };
      verify("id>0 OR id<0",true,4);
      verify("id>0 OR flag>0",false,4);
      verify("id>0 OR id IS NULL",false,3);
      verify("id>0",true,2," GROUP BY id");
      verify("id>0",false,2," GROUP BY GROUPING SETS ((id),())");
    }catch(const std::exception &e){QFAIL(e.what());}}
    void setOperationsKeepProvenKeysInOwnedPostgres() {try {
      using namespace navidog;
      using namespace navidog::sql::compiler;
      namespace opt=navidog::sql::optimizer;
      auto db=connectDatabase(profile_);
      const auto token=cancellation();
      const auto run=[&](const std::string &sql) {
          auto result=db->execute(QString::fromStdString(sql),{},token);
          if (!result.error.isEmpty()) throw std::runtime_error(result.error.toStdString());
          return result;
      };
      run("CREATE TABLE public.owned_set_key(id bigint PRIMARY KEY, payload int)");
      run("CREATE TABLE public.owned_set_bag(id bigint, payload int)");
      run("INSERT INTO public.owned_set_key VALUES(1,NULL),(2,9),(9007199254740992,5),(9007199254740993,5)");
      run("INSERT INTO public.owned_set_bag VALUES(1,NULL),(1,NULL),(3,9),(NULL,0)");
      auto snapshot=std::make_shared<const MetadataSnapshot>(collectMetadata(*db,profile_,"public",token));
      MetadataCatalog catalog(snapshot);
      const NameResolutionContext names{profile_.database.toStdString(),"public",Dialect::PostgreSQL};
      const auto verify=[&](const std::string &sql, bool unique, int rows) {
        const auto result=run(sql);
        QCOMPARE(result.rows.size(),rows);
        const auto parsed=compile(Dialect::PostgreSQL,sql,{},&catalog,names);
        QVERIFY2(parsed->ok(),parsed->diagnostics.empty()?"no model":parsed->diagnostics.front().message.c_str());
        const auto plan=opt::optimize(parsed->model);
        QVERIFY(plan.ok());
        opt::RelMetadataQuery metadata(plan.logical);
        const auto *proof=metadata.properties(plan.logical.root);
        QVERIFY(proof);
        QCOMPARE(!proof->uniqueKeys.empty(),unique);
        QCOMPARE(!proof->nonNullColumns.empty(),unique);
        if (unique) {
          QCOMPARE(proof->uniqueKeys.front().size(),std::size_t(1));
          QCOMPARE(proof->uniqueKeys.front().front().name,std::string("k"));
        }
        std::set<QString> keys;
        for (const auto &row:result.rows) keys.insert(row.front().toString());
        if (unique) QCOMPARE(keys.size(),std::size_t(rows));
        else QVERIFY(keys.size()<std::size_t(rows));
      };
      verify("SELECT id AS k,payload FROM public.owned_set_key "
             "EXCEPT ALL SELECT id,payload FROM public.owned_set_bag ORDER BY 1",true,3);
      verify("SELECT id AS k,payload FROM public.owned_set_bag "
             "INTERSECT ALL SELECT id,payload FROM public.owned_set_key ORDER BY 1",true,1);
      verify("SELECT id AS k,payload FROM public.owned_set_key "
             "UNION ALL SELECT id,payload FROM public.owned_set_bag ORDER BY 1",false,8);
      verify("SELECT id AS k,payload FROM public.owned_set_key "
             "EXCEPT ALL SELECT CAST(id AS double precision),payload FROM public.owned_set_bag ORDER BY 1",false,3);
      const std::string selectDistinct="SELECT DISTINCT s.k,s.payload FROM ("
          "SELECT id AS k,payload FROM public.owned_set_key EXCEPT ALL "
          "SELECT id,payload FROM public.owned_set_bag) s ORDER BY 1";
      const auto parsed=compile(Dialect::PostgreSQL,selectDistinct,{},&catalog,names);
      QVERIFY(parsed->ok());
      opt::PlannerOptions options;
      options.heuristic.rules={opt::HepRule::DistinctRemove};
      const auto plan=opt::optimize(parsed->model,nullptr,options);
      QVERIFY(plan.ok());
      QVERIFY(std::any_of(plan.rules.begin(),plan.rules.end(),[](const auto &event) {
          return event.rule=="HepDistinctRemove";
      }));
      const auto generated=opt::generatePlanSql(plan.logical,plan.heuristicRoot);
      QVERIFY2(generated.ok(),generated.diagnostics.empty()?"no SQL":generated.diagnostics.front().message.c_str());
      QCOMPARE(run(generated.sql).rows,run(selectDistinct).rows);
      const std::string grouped="SELECT DISTINCT q.k FROM (SELECT id AS k,COUNT(*) AS n "
          "FROM public.owned_set_key GROUP BY k,payload) q ORDER BY 1";
      const auto groupedSource=compile(Dialect::PostgreSQL,grouped,{},&catalog,names);
      QVERIFY(groupedSource->ok());
      const auto groupedPlan=opt::optimize(groupedSource->model,nullptr,options);
      QVERIFY(groupedPlan.ok());
      QVERIFY(std::any_of(groupedPlan.rules.begin(),groupedPlan.rules.end(),[](const auto &event) {
          return event.rule=="HepDistinctRemove";
      }));
      const auto groupedSql=opt::generatePlanSql(groupedPlan.logical,groupedPlan.heuristicRoot);
      QVERIFY2(groupedSql.ok(),groupedSql.diagnostics.empty()?"no SQL":groupedSql.diagnostics.front().message.c_str());
      QCOMPARE(run(groupedSql.sql).rows,run(grouped).rows);
      for (const auto &[source, unique] : std::vector<std::pair<std::string,bool>>{
            {"SELECT payload,MIN(id) AS edge FROM public.owned_set_key GROUP BY payload",true},
            {"SELECT payload,MAX(id) AS edge FROM public.owned_set_key GROUP BY payload HAVING COUNT(*)>0",true},
            {"SELECT id,MIN(payload) AS edge FROM public.owned_set_key GROUP BY id",false},
            {"SELECT payload,MIN(id) AS edge FROM public.owned_set_key GROUP BY GROUPING SETS ((payload),())",false}}) {
          const auto distinct="SELECT DISTINCT q.edge FROM ("+source+") q ORDER BY 1";
          const auto parsed=compile(Dialect::PostgreSQL,distinct,{},&catalog,names);
          QVERIFY(parsed->ok());
          const auto plan=opt::optimize(parsed->model,nullptr,options);
          QVERIFY(plan.ok());
          QCOMPARE(std::any_of(plan.rules.begin(),plan.rules.end(),[](const auto &event) {
            return event.rule=="HepDistinctRemove";
          }),unique);
          const auto output=opt::generatePlanSql(plan.logical,plan.heuristicRoot);
          QVERIFY2(output.ok(),output.diagnostics.empty()?"no SQL":output.diagnostics.front().message.c_str());
          QCOMPARE(run(output.sql).rows,run(distinct).rows);
      }
      for (const auto &[condition, one] : std::vector<std::pair<std::string,bool>>{
          {"id=2",true},{"id=2 OR id=1",false},{"id>0",false},
          {"2 IS NOT DISTINCT FROM id",true},{"id IS DISTINCT FROM 2",false},
          {"(id=2 AND payload>0) OR (02=id AND payload IS NULL)",true},
          {"(id=2 AND id=1) OR id=2",true},
          {"id>1 AND 3>id",true},{"id BETWEEN 2 AND 2",true},
          {"id BETWEEN 1 AND 2",false},{"id BETWEEN SYMMETRIC 2 AND 1",false},
          {"id<3 AND 1<=id",false},{"id>=9007199254740992 AND id<9007199254740993",true},
          {"(id>1 AND 3>id) OR (2<=id AND 2>=id)",true},
          {"id<=-9223372036854775808",true}}) {
          const auto queried="SELECT DISTINCT payload FROM public.owned_set_key WHERE "+condition+" ORDER BY 1";
          const auto parsed=compile(Dialect::PostgreSQL,queried,{},&catalog,names);
          QVERIFY(parsed->ok());
          const auto plan=opt::optimize(parsed->model,nullptr,options);
          QVERIFY(plan.ok());
          opt::RelMetadataQuery facts(plan.logical);
          const auto *bound=facts.properties(plan.logical.root);
          QVERIFY(bound);
          QCOMPARE(bound->rows.maximum==1,one);
          QCOMPARE(std::any_of(plan.rules.begin(),plan.rules.end(),[](const auto &event) {
            return event.rule=="HepDistinctRemove";
          }),one);
          const auto output=opt::generatePlanSql(plan.logical,plan.heuristicRoot);
          QVERIFY2(output.ok(),output.diagnostics.empty()?"no SQL":output.diagnostics.front().message.c_str());
          QCOMPARE(run(output.sql).rows,run(queried).rows);
      }
      for (const auto &[clause,upper]:std::vector<std::pair<std::string,std::optional<std::uint64_t>>>{
          {"public.owned_set_key WHERE id BETWEEN 1 AND 3",3},
          {"public.owned_set_key WHERE id BETWEEN 1 AND 2 OR id=4",3},
          {"public.owned_set_key WHERE id IN (1,9007199254740992,9007199254740992,NULL)",2},
          {"public.owned_set_key WHERE id NOT IN (1,3) AND id BETWEEN 1 AND 4",2},
          {"public.owned_set_key WHERE id NOT IN (1,NULL) OR id=2",1},
          {"public.owned_set_key WHERE id IN (-9223372036854775808,9223372036854775807)",2},
          {"public.owned_set_key WHERE id IN (1,3) AND id BETWEEN 5 AND 8",0},
          {"(SELECT id FROM public.owned_set_key WHERE id IN (1,3)) AS shifted(id) WHERE id BETWEEN 5 AND 8",0},
          {"(SELECT id AS k FROM public.owned_set_key WHERE id IN (1,2,4,9007199254740992) "
           "ORDER BY k LIMIT 10) sampled(id) WHERE id>1",3},
          {"(SELECT id+1 AS id FROM public.owned_set_key WHERE id IN (1,3)) shifted WHERE id IN (2,4)",2},
          {"public.owned_set_key WHERE id BETWEEN 9007199254740992 AND 9007199254740993",2},
          {"public.owned_set_key WHERE id BETWEEN -9223372036854775808 AND 9223372036854775807",{}},
          {"public.owned_set_key WHERE id IS NULL OR id BETWEEN 1 AND 3",3},
          {"public.owned_set_bag WHERE id BETWEEN 1 AND 2",{}},
          {"public.owned_set_bag WHERE id IS NULL OR id>=3",{}},
          {"public.owned_set_bag WHERE id IS NULL AND id>0",0},
          {"public.owned_set_bag WHERE id IS NULL AND id IS NOT NULL",0},
          {"public.owned_set_bag WHERE id IS NOT DISTINCT FROM NULL OR id IS NOT DISTINCT FROM 1",{}},
          {"public.owned_set_bag WHERE id IS NOT DISTINCT FROM NULL AND NULL IS DISTINCT FROM id",0}}) {
        const auto source="SELECT id FROM "+clause+" ORDER BY id";
        const auto parsed=compile(Dialect::PostgreSQL,source,{},&catalog,names);
        QVERIFY(parsed->ok());
        const auto actual=run(source);
        const auto plan=opt::optimize(parsed->model);
        QVERIFY(plan.ok());
        opt::RelMetadataQuery metadata(plan.logical);
        const auto *property=metadata.properties(plan.logical.root);
        QVERIFY(property);
        QCOMPARE(property->rows.maximum,upper);
        if (upper) {
          QVERIFY(std::uint64_t(actual.rows.size())<=*upper);
          QVERIFY(plan.best.nodes.at(plan.best.root).rows<=*upper);
        }
        const auto generated=opt::generatePlanSql(plan.logical,plan.heuristicRoot);
        QVERIFY2(generated.ok(),generated.diagnostics.empty()?source.c_str():generated.diagnostics.front().message.c_str());
        QCOMPARE(run(generated.sql).rows,actual.rows);
      }
      for (const auto *predicate:{"id=1 AND id=2","id>1 AND id<2","id BETWEEN 2 AND 1",
                                  "id IS NOT DISTINCT FROM NULL",
                                  "id>9223372036854775807","id < -9223372036854775808"}) {
        const auto impossible=std::string("SELECT payload FROM public.owned_set_key WHERE ")+predicate;
        const auto denied=compile(Dialect::PostgreSQL,impossible,{},&catalog,names);
        QVERIFY(denied->ok());
        const auto empty=opt::optimize(denied->model,nullptr,options);
        QVERIFY(empty.ok());
        opt::RelMetadataQuery emptyFacts(empty.logical);
        QVERIFY(emptyFacts.properties(empty.logical.root));
        QCOMPARE(emptyFacts.properties(empty.logical.root)->rows.maximum,std::optional<std::uint64_t>{0});
        QVERIFY(run(impossible).rows.empty());
      }
    }catch(const std::exception &e){QFAIL(e.what());}}
    void secondaryIntegerKeysAreProofsOnlyWhenVerified() {try {
        using namespace navidog;
        using namespace navidog::sql::compiler;
        using namespace navidog::sql::optimizer;
        auto db=connectDatabase(profile_);
        const auto token=cancellation();
        const auto run=[&](const std::string &sql) {
            auto result=db->execute(QString::fromStdString(sql),{},token);
            if (!result.error.isEmpty()) throw std::runtime_error(result.error.toStdString());
            return result;
        };
        run("CREATE TABLE public.owned_secondary_key(tenant int NOT NULL,code bigint NOT NULL,payload int)");
        run("CREATE UNIQUE INDEX owned_secondary_include ON public.owned_secondary_key(tenant,code) INCLUDE(payload)");
        run("INSERT INTO public.owned_secondary_key VALUES (1,10,30),(1,11,30),(2,10,31)");
        run("CREATE TABLE public.owned_secondary_nullable(x int,other int NOT NULL)");
        run("CREATE UNIQUE INDEX owned_secondary_nullable_index ON public.owned_secondary_nullable(x,other)");
        run("INSERT INTO public.owned_secondary_nullable VALUES (NULL,1),(NULL,1)");
        run("CREATE TABLE public.owned_secondary_outer(id int PRIMARY KEY,x int)");
        run("INSERT INTO public.owned_secondary_outer VALUES (1,NULL),(2,10)");
        run("CREATE TABLE public.owned_secondary_rls(id int PRIMARY KEY)");
        run("ALTER TABLE public.owned_secondary_rls ENABLE ROW LEVEL SECURITY");
        run("CREATE TABLE public.owned_secondary_partial(x int NOT NULL)");
        run("CREATE UNIQUE INDEX owned_secondary_partial_index ON public.owned_secondary_partial(x) WHERE x>0");
        run("INSERT INTO public.owned_secondary_partial VALUES (-1),(-1)");
        run("CREATE TABLE public.owned_secondary_expression(x int NOT NULL)");
        run("CREATE UNIQUE INDEX owned_secondary_expression_index ON public.owned_secondary_expression((x+1))");
        run("CREATE TABLE public.owned_secondary_deferred(x int NOT NULL,UNIQUE(x) DEFERRABLE INITIALLY DEFERRED)");
        run("CREATE TABLE public.owned_secondary_parent(x int NOT NULL)");
        run("CREATE UNIQUE INDEX owned_secondary_parent_index ON public.owned_secondary_parent(x)");
        run("CREATE TABLE public.owned_secondary_child(extra int) INHERITS (public.owned_secondary_parent)");
        run("INSERT INTO public.owned_secondary_parent VALUES (1)");
        run("INSERT INTO public.owned_secondary_child VALUES (1,7)");
        auto snapshot=std::make_shared<const MetadataSnapshot>(collectMetadata(*db,profile_,"public",token));
        MetadataCatalog catalog(snapshot);
        const NameResolutionContext names{profile_.database.toStdString(),"public",Dialect::PostgreSQL};
        const auto table=catalog.findTable(names,{"public","owned_secondary_key"});
        QVERIFY2(table.has_value(),qPrintable(snapshot->statisticsWarning));
        const std::vector<std::vector<std::string>> key{{"tenant","code"}};
        QCOMPARE(table->strictUniqueKeys,key);
        QVERIFY(!table->primaryKeyKnown);
        for (const auto *name:{"owned_secondary_nullable","owned_secondary_partial",
                               "owned_secondary_expression","owned_secondary_deferred"}) {
            const auto unsupported=catalog.findTable(names,{"public",name});
            QVERIFY(unsupported && unsupported->strictUniqueKeys.empty());
        }
        PlannerOptions options;
        options.heuristic.rules={HepRule::DistinctRemove};
        const auto verify=[&](const std::string &sql,bool removes,std::optional<std::uint64_t> maxRows) {
            const auto model=compile(names.dialect,sql,{},&catalog,names);
            QVERIFY2(model->ok(),model->diagnostics.empty()?sql.c_str():model->diagnostics.front().message.c_str());
            const auto plan=optimize(model->model,nullptr,options);
            QVERIFY(plan.ok());
            RelMetadataQuery metadata(plan.logical);
            const auto *facts=metadata.properties(plan.logical.root);
            QVERIFY(facts);
            QCOMPARE(facts->rows.maximum,maxRows);
            QCOMPARE(std::any_of(plan.rules.begin(),plan.rules.end(),[](const auto &rule) {
                return rule.rule=="HepDistinctRemove";
            }),removes);
            const auto generated=generatePlanSql(plan.logical,plan.heuristicRoot);
            QVERIFY2(generated.ok(),generated.diagnostics.empty()?sql.c_str():generated.diagnostics.front().message.c_str());
            QCOMPARE(run(generated.sql).rows,run(sql).rows);
        };
        verify("SELECT DISTINCT tenant,code FROM public.owned_secondary_key ORDER BY 1,2",true,{});
        verify("SELECT DISTINCT payload FROM public.owned_secondary_key WHERE tenant=1 AND code=10",true,1);
        verify("SELECT payload FROM public.owned_secondary_key WHERE tenant=1 AND code BETWEEN 10 AND 11 ORDER BY 1",false,2);
        verify("SELECT DISTINCT x FROM public.owned_secondary_parent ORDER BY x",false,{});
        verify("SELECT DISTINCT x FROM ONLY public.owned_secondary_parent ORDER BY x",true,{});
        verify("SELECT DISTINCT a.tenant,a.code FROM (SELECT tenant,code FROM public.owned_secondary_key LIMIT 7) a "
               "LEFT JOIN public.owned_secondary_key b ON a.tenant IS NOT DISTINCT FROM b.tenant "
               "AND a.code IS NOT DISTINCT FROM b.code ORDER BY 1,2",true,7);
        verify("SELECT DISTINCT a.id FROM public.owned_secondary_outer a "
               "LEFT JOIN public.owned_secondary_nullable b ON a.x IS NOT DISTINCT FROM b.x ORDER BY 1",false,{});
        QCOMPARE(run("SELECT a.id FROM public.owned_secondary_outer a LEFT JOIN public.owned_secondary_nullable b "
                     "ON a.x IS NOT DISTINCT FROM b.x WHERE a.id=1").rows.size(),2);
        const auto verifyRemovable=[&](const std::string &sql,bool removes) {
            PlannerOptions choices;
            choices.heuristic.rules={HepRule::RemoveUnusedOuterJoin};
            const auto model=compile(names.dialect,sql,{},&catalog,names);
            QVERIFY(model->ok());
            const auto plan=optimize(model->model,nullptr,choices);
            QVERIFY(plan.ok());
            QCOMPARE(std::any_of(plan.rules.begin(),plan.rules.end(),[](const auto &rule) {
                return rule.rule=="HepRemoveUnusedOuterJoin";
            }),removes);
            const auto generated=generatePlanSql(plan.logical,plan.heuristicRoot);
            QVERIFY2(generated.ok(),generated.diagnostics.empty()?sql.c_str():generated.diagnostics.front().message.c_str());
            const auto values=[&](const std::string &query) {
                std::vector<int> output;
                for (const auto &row:run(query).rows) output.push_back(row.front().toInt());
                std::sort(output.begin(),output.end());
                return output;
            };
            QCOMPARE(values(generated.sql),values(sql));
        };
        verifyRemovable("SELECT a.tenant FROM public.owned_secondary_key a LEFT JOIN public.owned_secondary_key b "
                        "ON b.tenant IS NOT DISTINCT FROM a.tenant AND b.code IS NOT DISTINCT FROM a.code",true);
        verifyRemovable("SELECT a.id FROM public.owned_secondary_outer a LEFT JOIN public.owned_secondary_nullable b "
                        "ON b.x IS NOT DISTINCT FROM a.x",false);
        verifyRemovable("SELECT a.id FROM public.owned_secondary_outer a LEFT JOIN public.owned_secondary_rls b "
                        "ON b.id IS NOT DISTINCT FROM a.x",false);
        try {
            run("SELECT payload FROM public.owned_secondary_key GROUP BY tenant,code");
            QFAIL("PostgreSQL accepted GROUP BY via a secondary unique index");
        } catch (const std::exception &error) {
            QVERIFY2(std::string(error.what()).find("GROUP BY")!=std::string::npos,error.what());
        }
        QTemporaryDir owned;
        QVERIFY(owned.isValid());
        MetadataStore disk(owned.filePath("catalog.sqlite"));
        disk.replace(*snapshot);
        const auto loaded=disk.load(snapshot->key);
        QVERIFY(loaded && !loaded->verified);
        const auto saved=std::find_if(loaded->objects.begin(),loaded->objects.end(),[](const auto &entry) {
            return entry.kind=="table" && entry.schema=="public" && entry.name=="owned_secondary_key";
        });
        QVERIFY(saved!=loaded->objects.end());
        const QVector<QStringList> savedKeys{QStringList{"tenant","code"}};
        QCOMPARE(saved->strictUniqueKeys,savedKeys);
        MetadataCatalog cached(loaded);
        const auto untrusted=cached.findTable(names,{"public","owned_secondary_key"});
        QVERIFY(!untrusted);
        for (const auto *name:{"owned_secondary_child","owned_secondary_parent","owned_secondary_deferred","owned_secondary_outer",
                               "owned_secondary_rls",
                               "owned_secondary_expression","owned_secondary_partial","owned_secondary_nullable",
                               "owned_secondary_key"})
            run(std::string("DROP TABLE public.")+name);
    }catch(const std::exception &e){QFAIL(e.what());}}
    void groupingSetsExpandToTypedUnionBranchesInOwnedPostgres() {try {
      using namespace navidog;
      using namespace navidog::sql::compiler;
      namespace opt=navidog::sql::optimizer;
      auto db=connectDatabase(profile_);
      const auto token=cancellation();
      const auto run=[&](const std::string &sql) {
          auto result=db->execute(QString::fromStdString(sql),{},token);
          if (!result.error.isEmpty()) throw std::runtime_error(result.error.toStdString());
          return result;
      };
      run("CREATE TABLE public.owned_group_set_union(a smallint,b bigint,v bigint,flag boolean)");
      run("INSERT INTO public.owned_group_set_union VALUES "
          "(1,2,9223372036854775807,TRUE),(1,2,9223372036854775807,TRUE),"
          "(1,NULL,NULL,FALSE),(NULL,2,4,NULL),(NULL,NULL,3,TRUE),(2,2,NULL,FALSE)");
      auto snapshot=std::make_shared<const MetadataSnapshot>(collectMetadata(*db,profile_,"public",token));
      MetadataCatalog catalog(snapshot);
      const NameResolutionContext names{profile_.database.toStdString(),"public",Dialect::PostgreSQL};
      opt::PlannerOptions options;
      options.heuristic.rules={opt::HepRule::AggregateGroupingSetsToUnion};
      const auto findRule=[](const auto &plan) {
          return std::find_if(plan.rules.begin(),plan.rules.end(),[](const auto &event) {
              return event.rule=="HepAggregateGroupingSetsToUnion";
          });
      };
      const std::string measures="SELECT a,b,GROUPING(a,b) AS mask,GROUPING(b,a) AS opposite,"
          "COUNT(*) AS n,COUNT(DISTINCT v) AS unique_n,MIN(v) AS lo,SUM(v) AS total "
          "FROM public.owned_group_set_union";
      const std::string sets=" GROUP BY GROUPING SETS ((a,b),(a),(b),(),())";
      const auto verify=[&](bool empty, const std::string &having = {}, bool useDefault = false) {
          const auto sql=measures+(empty?" WHERE FALSE":"")+sets+having+" ORDER BY 3,1,2";
          const auto parsed=compile(Dialect::PostgreSQL,sql,{},&catalog,names);
          QVERIFY2(parsed->ok(),parsed->diagnostics.empty()?"no model":parsed->diagnostics.front().message.c_str());
          const auto plan=opt::optimize(parsed->model,nullptr,useDefault ? opt::PlannerOptions{} : options);
          QVERIFY2(plan.ok(),plan.diagnostics.empty()?"no plan":plan.diagnostics.front().message.c_str());
          const auto event=findRule(plan);
          QVERIFY(event!=plan.rules.end());
          const auto &united=plan.logical.nodes.at(event->after);
          QCOMPARE(united.kind,opt::RelKind::SetOperation);
          QCOMPARE(united.setKind,SetKind::Union);
          QVERIFY(!united.distinct);
          QCOMPARE(united.inputs.size(),std::size_t(5));

          const auto lhs=run(sql);
          for (const auto root:{plan.logical.root,plan.heuristicRoot}) {
              const auto generated=opt::generatePlanSql(plan.logical,root);
              QVERIFY2(generated.ok(),generated.diagnostics.empty()?"no SQL":generated.diagnostics.front().message.c_str());
              const auto reparsed=compile(Dialect::PostgreSQL,generated.sql,{},&catalog,names);
              QVERIFY2(reparsed->ok(),reparsed->diagnostics.empty()?"no model":reparsed->diagnostics.front().message.c_str());
              const auto rhs=run(generated.sql);
              QCOMPARE(rhs.rows,lhs.rows);
              QCOMPARE(rhs.columns,lhs.columns);
              QCOMPARE(rhs.columnTypes,lhs.columnTypes);
          }
          if (empty && having.empty()) {
              QCOMPARE(lhs.rows.size(),2);
              QCOMPARE(lhs.rows[0][2].toInt(),3);
              QCOMPARE(lhs.rows[0][4].toInt(),0);
          }
      };
      verify(false);
      verify(true);
      verify(false, " HAVING (GROUPING(a,b)=3 AND COUNT(*)>0) OR (GROUPING(a,b)=1 AND a IS NULL)");
      verify(true, " HAVING GROUPING(a,b)=3 AND COUNT(*)=0");
      verify(false, " HAVING b IS NOT NULL OR (b IS NULL AND GROUPING(b)=1)");
      verify(false, " HAVING a>1", true);
      for (bool empty : {false,true}) {
          const std::string sql="SELECT flag,GROUPING(flag) AS mask,COUNT(*) AS n,AVG(v) AS average "
              "FROM public.owned_group_set_union "+std::string(empty?"WHERE FALSE ":"")+
              "GROUP BY GROUPING SETS ((flag),(),()) HAVING flag IS TRUE OR GROUPING(flag)=1 ORDER BY 2,1";
          const auto compiled=compile(Dialect::PostgreSQL,sql,{},&catalog,names);
          QVERIFY2(compiled->ok(),compiled->diagnostics.empty()?"no model":compiled->diagnostics.front().message.c_str());
          const auto plan=opt::optimize(compiled->model,nullptr,options);
          QVERIFY(plan.ok());
          QVERIFY(findRule(plan)!=plan.rules.end());
          const auto generated=opt::generatePlanSql(plan.logical,plan.heuristicRoot);
          QVERIFY2(generated.ok(),generated.diagnostics.empty()?"no SQL":generated.diagnostics.front().message.c_str());
          const auto original=run(sql), rewritten=run(generated.sql);
          QCOMPARE(rewritten.rows,original.rows);
          QCOMPARE(rewritten.columns,original.columns);
          QCOMPARE(rewritten.columnTypes,original.columnTypes);
          QCOMPARE(original.rows.size(),empty?2:3);
      }
      for (const auto &barrier:{measures+" WHERE random()>0.5"+sets,
                                measures+sets+" HAVING random()>0.5",
                                measures+sets+" HAVING COALESCE(a,0)>0"}) {
          const auto parsed=compile(Dialect::PostgreSQL,barrier,{},&catalog,names);
          QVERIFY(parsed->ok());
          const auto plan=opt::optimize(parsed->model,nullptr,options);
          QVERIFY(plan.ok());
          QVERIFY(findRule(plan)==plan.rules.end());
      }
    }catch(const std::exception &e){QFAIL(e.what());}}
    void groupedExtremaCanPrecedeUnionAllInOwnedPostgres(){try {
      using namespace navidog;
      using namespace navidog::sql::compiler;
      namespace opt=navidog::sql::optimizer;
      auto db=connectDatabase(profile_);
      const auto token=cancellation();
      const auto run=[&](const std::string &sql) {
          auto result=db->execute(QString::fromStdString(sql),{},token);
          if (!result.error.isEmpty()) throw std::runtime_error(result.error.toStdString());
          return result;
      };
      run("CREATE TABLE public.owned_extreme_west(k integer,lo bigint,hi smallint)");
      run("CREATE TABLE public.owned_extreme_east(g integer,p bigint,q smallint)");
      run("INSERT INTO public.owned_extreme_west VALUES "
          "(1,17,NULL),(1,-9223372036854775808,7),(NULL,NULL,4),(2,18,7),(2,18,7),(3,NULL,NULL)");
      run("INSERT INTO public.owned_extreme_east VALUES "
          "(1,-3,2),(NULL,15,9),(2,20,1),(7,NULL,NULL),(3,NULL,NULL)");
      auto snapshot=std::make_shared<const MetadataSnapshot>(collectMetadata(*db,profile_,"public",token));
      MetadataCatalog catalog(snapshot);
      const NameResolutionContext names{profile_.database.toStdString(),"public",Dialect::PostgreSQL};
      opt::PlannerOptions options;
      options.heuristic.rules={opt::HepRule::AggregateUnionTranspose};
      const auto findRule=[](const auto &plan) {
          return std::find_if(plan.rules.begin(),plan.rules.end(),[](const auto &event){
            return event.rule=="HepAggregateUnionTranspose";
          });
      };
      const auto verify=[&](bool grouped) {
          const auto fields=grouped?std::string("id,")+ "MIN(low_value) AS low,MAX(high_value) AS high":
              "MIN(low_value) AS low,MAX(high_value) AS high";
          const std::string prefix="SELECT "+fields+" FROM (";
          const std::string source=grouped?
              "SELECT k,lo,hi FROM public.owned_extreme_west UNION ALL "
              "SELECT g,p,q FROM public.owned_extreme_east":
              "SELECT lo,hi FROM public.owned_extreme_west UNION ALL "
              "SELECT p,q FROM public.owned_extreme_east";
          const std::string suffix=grouped?") AS u(id,low_value,high_value) GROUP BY id ORDER BY id NULLS FIRST":
              ") AS u(low_value,high_value)";
          const auto sql=prefix+source+suffix;
          const auto parsed=compile(Dialect::PostgreSQL,sql,{},&catalog,names);
          QVERIFY2(parsed->ok(),parsed->diagnostics.empty()?"no model":parsed->diagnostics.front().message.c_str());
          const auto plan=opt::optimize(parsed->model,nullptr,options);
          QVERIFY2(plan.ok(),plan.diagnostics.empty()?"no plan":plan.diagnostics.front().message.c_str());
          const auto event=findRule(plan);
          QVERIFY(event!=plan.rules.end());
          const auto &project=plan.logical.nodes.at(event->after);
          const auto &final=plan.logical.nodes.at(project.inputs.front());
          const auto &united=plan.logical.nodes.at(final.inputs.front());
          QCOMPARE(united.kind,opt::RelKind::SetOperation);
          QCOMPARE(united.inputs.size(),std::size_t(2));
          QVERIFY(!united.distinct);
          std::string partialSql;
          for (const auto id:united.inputs) {
              const auto &output=plan.logical.nodes.at(id);
              const auto &partial=plan.logical.nodes.at(output.inputs.front());
              const auto &wrapped=plan.logical.nodes.at(partial.inputs.front());
              const auto &arm=plan.logical.nodes.at(wrapped.inputs.front());
              QCOMPARE(partial.kind,opt::RelKind::Aggregate);
              QCOMPARE(partial.keys.size(),grouped?std::size_t(1):std::size_t(0));
              QCOMPARE(output.outputs.size(),grouped?std::size_t(3):std::size_t(2));
              auto sourceSql=plan.logical.program;
              sourceSql.roots={arm.query};
              auto generated=generateSql(sourceSql,Dialect::PostgreSQL);
              QVERIFY2(generated.ok(),generated.diagnostics.empty()?"no SQL":generated.diagnostics.front().message.c_str());
              if (generated.sql.back()==';') generated.sql.pop_back();
              if (!partialSql.empty()) partialSql+=" UNION ALL ";
              partialSql+="SELECT ";
              for (std::size_t i=0;i<output.keys.size();++i) {
                  if (i) partialSql+=",";
                  const auto ref=output.keys.at(i);
                  const auto &owner=plan.logical.program.queries.at(ref.query-1);
                  const auto &expr=owner.expressions.at(ref.expression);
                  if (expr.kind==ExprKind::Call)
                      partialSql+=expr.name.back()+"("+owner.expressions.at(expr.operands.front()).name.back()+")";
                  else
                      partialSql+=expr.name.back();
                  partialSql+=" AS "+output.outputs.at(i).name;
              }
              partialSql+=" FROM ("+generated.sql+") AS branch";
              partialSql+=grouped?"(id,low_value,high_value) GROUP BY id":"(low_value,high_value)";
          }
          const auto original=run(sql);
          const auto alternative=run(prefix+partialSql+suffix);
          QCOMPARE(alternative.rows,original.rows);
          QCOMPARE(alternative.columns,original.columns);
          QCOMPARE(alternative.columnTypes,original.columnTypes);
      };
      verify(true);
      verify(false);
      run("TRUNCATE public.owned_extreme_east");
      verify(true);
      verify(false);
      run("TRUNCATE public.owned_extreme_west");
      verify(false);
      for (const auto *barrier:{
          "SELECT SUM(x) FROM (SELECT lo FROM public.owned_extreme_west UNION ALL SELECT p FROM public.owned_extreme_east) s(x)",
          "SELECT MIN(x) FROM (SELECT lo FROM public.owned_extreme_west UNION SELECT p FROM public.owned_extreme_east) s(x)",
          "SELECT MIN(x),MAX(x) FROM (SELECT lo FROM public.owned_extreme_west UNION ALL SELECT p FROM public.owned_extreme_east) s(x)"}) {
          const auto query=compile(Dialect::PostgreSQL,barrier,{},&catalog,names);
          QVERIFY(query->ok());
          const auto plan=opt::optimize(query->model,nullptr,options);
          QVERIFY(plan.ok());
          QVERIFY(findRule(plan)==plan.rules.end());
      }
    } catch(const std::exception &e){QFAIL(e.what());}}
    void nestedExtremumAggregatesMergeAcrossIntegerRollupsInOwnedPostgres(){try {
      using namespace navidog;
      using namespace navidog::sql::compiler;
      namespace opt=navidog::sql::optimizer;
      auto db=connectDatabase(profile_);
      auto token=cancellation();
      const auto run=[&](const std::string& sql) {
          auto result=db->execute(QString::fromStdString(sql),{},token);
          if(!result.error.isEmpty())throw std::runtime_error(result.error.toStdString());
          return result;
      };
      run("CREATE TABLE public.owned_aggregate_merge(a integer,b integer,v bigint,w smallint)");
      run("INSERT INTO public.owned_aggregate_merge VALUES "
          "(1,1,NULL,NULL),(1,2,9223372036854775807,1),(1,2,-5,NULL),"
          "(1,NULL,6,9),(2,2,NULL,NULL),(NULL,1,17,4)");
      auto snapshot=std::make_shared<const MetadataSnapshot>(collectMetadata(*db,profile_,"public",token));
      MetadataCatalog catalog(snapshot);
      const NameResolutionContext names{profile_.database.toStdString(),"public",Dialect::PostgreSQL};
      opt::PlannerOptions options;
      options.heuristic.rules={opt::HepRule::AggregateMerge};
      const auto rule=[](const auto& plan){
          return std::find_if(plan.rules.begin(),plan.rules.end(),
              [](const auto& event){return event.rule=="HepAggregateMerge";});
      };
      const auto verify=[&](const std::string& query,bool grouped) {
          const auto parsed=compile(Dialect::PostgreSQL,query,{},&catalog,names);
          QVERIFY2(parsed->ok(),parsed->diagnostics.empty()?"no model":parsed->diagnostics.front().message.c_str());
          const auto plan=opt::optimize(parsed->model,nullptr,options);
          QVERIFY2(plan.ok(),plan.diagnostics.empty()?"no plan":plan.diagnostics.front().message.c_str());
          const auto event=rule(plan);
          QVERIFY(event!=plan.rules.end());
          const auto& merged=plan.logical.nodes.at(event->after);
          QCOMPARE(merged.kind,opt::RelKind::Project);
          const auto& flattened=plan.logical.nodes.at(merged.inputs.front());
          QCOMPARE(flattened.kind,opt::RelKind::Aggregate);
          QCOMPARE(flattened.keys.size(),grouped?std::size_t(1):std::size_t(0));
          auto replacement=plan.logical.program;
          replacement.roots={merged.query};
          auto& queryBlock=replacement.queries.at(merged.query-1);
          queryBlock.projections.clear();
          queryBlock.groupBy.clear();
          for(std::size_t i=0;i<merged.keys.size();++i)
              queryBlock.projections.push_back({merged.keys[i].expression,merged.outputs[i].name});
          for(const auto& key:flattened.keys)queryBlock.groupBy.push_back(key.expression);
          auto generated=generateSql(replacement,Dialect::PostgreSQL);
          QVERIFY2(generated.ok(),generated.diagnostics.empty()?"no SQL":generated.diagnostics.front().message.c_str());
          auto inspect=[&](std::string sql) {
              if(sql.back()==';')sql.pop_back();
              return run("SELECT q.*,pg_typeof(lo)::text,pg_typeof(hi)::text FROM ("+sql+") AS q"+
                         (grouped?" ORDER BY 1 NULLS FIRST":"")).rows;
          };
          QCOMPARE(inspect(generated.sql),inspect(query));
      };
      const std::string base="SELECT a,b,MIN(v) AS lo,MAX(w) AS hi FROM public.owned_aggregate_merge";
      verify("SELECT MIN(s.lo) AS lo,MAX(s.hi) AS hi FROM ("+base+" GROUP BY a,b) AS s;",false);
      verify("SELECT s.a,MIN(s.lo) AS lo,MAX(s.hi) AS hi FROM ("+base+" GROUP BY a,b) AS s GROUP BY s.a;",true);
      verify("SELECT MIN(s.lo) AS lo,MAX(s.hi) AS hi FROM ("+base+" WHERE a=999 GROUP BY a,b) AS s;",false);
      for(const auto& barrier:{
          "SELECT MAX(s.lo) FROM ("+base+" GROUP BY a,b) AS s;",
          "SELECT MIN(s.lo) FROM ("+base+" GROUP BY a,b HAVING COUNT(*)>0) AS s;",
          "SELECT MIN(s.lo) FROM ("+base+" GROUP BY a,b LIMIT 1) AS s;"}) {
          const auto parsed=compile(Dialect::PostgreSQL,barrier,{},&catalog,names);
          QVERIFY(parsed->ok());
          const auto plan=opt::optimize(parsed->model,nullptr,options);
          QVERIFY(plan.ok());
          QVERIFY(rule(plan)==plan.rules.end());
      }
    } catch(const std::exception& e){QFAIL(e.what());}}
    void unionDistinctEliminatesBranchDistinctInOwnedPostgres(){try {
      using namespace navidog;
      using namespace navidog::sql::compiler;
      namespace opt=navidog::sql::optimizer;
      auto db=connectDatabase(profile_);
      auto token=cancellation();
      const auto run=[&](const std::string& sql) {
          auto result=db->execute(QString::fromStdString(sql),{},token);
          if(!result.error.isEmpty())throw std::runtime_error(result.error.toStdString());
          return result;
      };
      run("CREATE TABLE public.owned_union_distinct_l(a integer,b bigint)");
      run("CREATE TABLE public.owned_union_distinct_r(a integer,b bigint)");
      run("INSERT INTO public.owned_union_distinct_l VALUES (1,2),(1,2),(1,NULL),(NULL,2),(NULL,NULL)");
      run("INSERT INTO public.owned_union_distinct_r VALUES (1,2),(1,3),(1,NULL),(NULL,2),(NULL,NULL),(NULL,NULL)");
      auto snapshot=std::make_shared<const MetadataSnapshot>(collectMetadata(*db,profile_,"public",token));
      MetadataCatalog catalog(snapshot);
      const NameResolutionContext names{profile_.database.toStdString(),"public",Dialect::PostgreSQL};
      const std::string left="SELECT DISTINCT a AS k,b AS v FROM public.owned_union_distinct_l";
      const std::string right="SELECT DISTINCT a,b FROM public.owned_union_distinct_r";
      const auto findRule=[](const auto& plan) {
          return std::find_if(plan.rules.begin(),plan.rules.end(),
              [](const auto& item){return item.rule=="AggregateUnionAggregate";});
      };
      const auto query=left+" UNION "+right+" ORDER BY k NULLS FIRST, v NULLS FIRST;";
      const auto compiled=compile(Dialect::PostgreSQL,query,{},&catalog,names);
      QVERIFY2(compiled->ok(),compiled->diagnostics.empty()?"no model":compiled->diagnostics.front().message.c_str());
      const auto plan=opt::optimize(compiled->model);
      QVERIFY2(plan.ok(),plan.diagnostics.empty()?"no plan":plan.diagnostics.front().message.c_str());
      const auto event=findRule(plan);
      QVERIFY(event!=plan.rules.end());
      auto replacement=plan.logical.program;
      const auto &unified=plan.logical.nodes.at(event->after);
      QVERIFY(unified.distinct);
      QCOMPARE(unified.inputs.size(),std::size_t(2));
      for(auto ref:unified.inputs) {
          const auto& branch=plan.logical.nodes.at(ref);
          QCOMPARE(branch.kind,opt::RelKind::Project);
          replacement.queries.at(branch.query-1).distinct=false;
      }
      const auto rewritten=generateSql(replacement,Dialect::PostgreSQL);
      QVERIFY2(rewritten.ok(),rewritten.diagnostics.empty()?"no SQL":rewritten.diagnostics.front().message.c_str());
      QCOMPARE(run(rewritten.sql).rows,run(query).rows);
      for(const auto& barrier:{left+" UNION ALL "+right+";", "("+left+" LIMIT 2) UNION ("+right+" LIMIT 2);"}) {
          const auto parsed=compile(Dialect::PostgreSQL,barrier,{},&catalog,names);
          QVERIFY(parsed->ok());
          const auto base=opt::optimize(parsed->model);
          QVERIFY(base.ok());
          QVERIFY(findRule(base)==base.rules.end());
      }
    } catch(const std::exception& e){QFAIL(e.what());}}
    void groupingSetsCanonicalSqlMatchesOwnedPostgres(){try{
      using namespace navidog;using namespace navidog::sql::compiler;
      auto db=connectDatabase(profile_);auto token=cancellation();
      auto run=[&](const QString &text){auto r=db->execute(text,{},token);if(!r.error.isEmpty())throw std::runtime_error(r.error.toStdString());return r;};
      run("CREATE TABLE public.owned_grouping_sets(a int,b int,n int)");
      run("INSERT INTO public.owned_grouping_sets VALUES(1,1,2),(1,2,3),(2,1,4),(NULL,1,5)");
      for(auto groups:{"GROUPING SETS((a,b),(a),())","ROLLUP(a,b)","CUBE(a,b)","GROUPING SETS(ROLLUP(a,b),())","a,ROLLUP(b)"}) {
        auto source=std::string("SELECT a,b,COUNT(*),SUM(n),GROUPING(a,b) AS mask FROM public.owned_grouping_sets GROUP BY ")+groups+" ORDER BY mask,a NULLS FIRST,b NULLS FIRST;";
        auto parsed=compile(Dialect::PostgreSQL,source);QVERIFY2(parsed->ok(),parsed->diagnostics.empty()?"no parse":parsed->diagnostics.front().message.c_str());
        auto generated=generateSql(parsed->model,Dialect::PostgreSQL);QVERIFY2(generated.ok(),generated.diagnostics.empty()?"no SQL":generated.diagnostics.front().message.c_str());
        const auto actual=run(QString::fromStdString(source));
        QCOMPARE(run(QString::fromStdString(generated.sql)).rows,actual.rows);
        auto optimized=navidog::sql::optimizer::optimize(parsed->model);
        QVERIFY2(optimized.ok(),optimized.diagnostics.empty()?"no local grouping plan":optimized.diagnostics.front().message.c_str());
        navidog::sql::optimizer::RelMetadataQuery proof(optimized.logical);
        const auto *bounds=proof.properties(optimized.logical.root);QVERIFY2(bounds,proof.message().c_str());
        QVERIFY(std::uint64_t(actual.rows.size())>=bounds->rows.minimum);
        if(bounds->rows.maximum)QVERIFY(std::uint64_t(actual.rows.size())<=*bounds->rows.maximum);
      }
      const std::string emptyTotals="SELECT COUNT(*) FROM public.owned_grouping_sets WHERE FALSE GROUP BY GROUPING SETS((),());";
      QCOMPARE(run(QString::fromStdString(emptyTotals)).rows.size(),2);
      auto emptyParsed=compile(Dialect::PostgreSQL,emptyTotals);QVERIFY(emptyParsed->ok());
      auto emptyPlan=navidog::sql::optimizer::optimize(emptyParsed->model);QVERIFY(emptyPlan.ok());
      QCOMPARE(emptyPlan.best.nodes[emptyPlan.best.root].rows,2.);
      navidog::sql::optimizer::RelMetadataQuery emptyProof(emptyPlan.logical);
      const auto *emptyBounds=emptyProof.properties(emptyPlan.logical.root);QVERIFY(emptyBounds);
      QCOMPARE(emptyBounds->rows.minimum,std::uint64_t(2));QCOMPARE(*emptyBounds->rows.maximum,std::uint64_t(2));
      auto mysql=convertSql(Dialect::MySQL,Dialect::PostgreSQL,"SELECT a,b,COUNT(*) FROM public.owned_grouping_sets GROUP BY a,b WITH ROLLUP;");QVERIFY2(mysql.ok(),mysql.diagnostics.empty()?"no conversion":mysql.diagnostics.front().message.c_str());
      QCOMPARE(run(QString::fromStdString(mysql.sql)).rows.size(),8);
      run("CREATE INDEX owned_grouping_idx ON public.owned_grouping_sets(a)");
      run("CREATE SEQUENCE public.owned_grouping_seq");run("CREATE TYPE public.owned_grouping_type AS ENUM('a','b')");
      run("CREATE MATERIALIZED VIEW public.owned_grouping_mat AS SELECT a FROM public.owned_grouping_sets");
      auto snapshot=collectMetadata(*db,profile_,"public",token);QVERIFY2(snapshot.objectWarning.isEmpty(),qPrintable(snapshot.objectWarning));
      MetadataCatalog catalog(std::make_shared<const MetadataSnapshot>(snapshot));NameResolutionContext context{profile_.database.toStdString(),"public",Dialect::PostgreSQL};
      auto includes=[&](CatalogObjectKind kind,const std::string &name){auto rows=catalog.listObjects(context,kind,4096);return std::any_of(rows.begin(),rows.end(),[&](const auto &o){return o.name==name;});};
      QVERIFY(includes(CatalogObjectKind::Index,"owned_grouping_idx"));QVERIFY(includes(CatalogObjectKind::Sequence,"owned_grouping_seq"));QVERIFY(includes(CatalogObjectKind::Type,"owned_grouping_type"));
      auto mat=catalog.findTable(context,{"owned_grouping_mat"});QVERIFY(mat);QCOMPARE(mat->kind,ObjectKind::MaterializedView);
      run("DROP MATERIALIZED VIEW public.owned_grouping_mat");run("DROP TYPE public.owned_grouping_type");run("DROP SEQUENCE public.owned_grouping_seq");run("DROP TABLE public.owned_grouping_sets");
    }catch(const std::exception &e){QFAIL(e.what());}}
    void routineCategoriesAreCollectedFromOwnedPostgres(){try{
      using namespace navidog;using namespace navidog::sql::compiler;
      auto db=connectDatabase(profile_);auto token=cancellation();
      auto run=[&](const QString &sql){auto r=db->execute(sql,{},token);if(!r.error.isEmpty())throw std::runtime_error(r.error.toStdString());};
      run("CREATE SCHEMA owned_routine_categories");
      run("CREATE FUNCTION owned_routine_categories.accum(s int,n int) RETURNS int LANGUAGE sql IMMUTABLE AS $$ SELECT s+n $$");
      run("CREATE AGGREGATE owned_routine_categories.total(int)(SFUNC=owned_routine_categories.accum,STYPE=int,INITCOND='0')");
      run("CREATE FUNCTION owned_routine_categories.series() RETURNS SETOF int LANGUAGE sql STABLE AS $$ SELECT generate_series(1,2) $$");
      auto snapshot=collectMetadata(*db,profile_,"owned_routine_categories",token);
      MetadataCatalog catalog(std::make_shared<const MetadataSnapshot>(snapshot));
      NameResolutionContext context{profile_.database.toStdString(),"owned_routine_categories",Dialect::PostgreSQL};
      auto total=catalog.findRoutines(context,{"total"},false);QCOMPARE(total.size(),std::size_t(1));QCOMPARE(total.front().kind,RoutineKind::Aggregate);
      auto accum=catalog.findRoutines(context,{"accum"},false);QCOMPARE(accum.size(),std::size_t(1));QCOMPARE(accum.front().kind,RoutineKind::Scalar);QCOMPARE(accum.front().volatility,FunctionVolatility::Immutable);
      auto series=catalog.findRoutines(context,{"series"},false);QCOMPARE(series.size(),std::size_t(1));QCOMPARE(series.front().kind,RoutineKind::TableFunction);QCOMPARE(series.front().volatility,FunctionVolatility::Stable);
      auto c=compile(Dialect::PostgreSQL,"SELECT total(1);",{},&catalog,context);QVERIFY2(c->ok(),c->diagnostics.empty()?"no statement":c->diagnostics.front().message.c_str());
      auto cost=sql::optimizer::optimize(c->model);QVERIFY2(cost.ok(),cost.diagnostics.empty()?"no plan":cost.diagnostics.front().message.c_str());QCOMPARE(cost.best.nodes[cost.best.root].rows,1.);
      QVERIFY(!compile(Dialect::PostgreSQL,"SELECT 1 WHERE total(1)>1;",{},&catalog,context)->ok());
      c=compile(Dialect::PostgreSQL,"SELECT series();",{},&catalog,context);QVERIFY(c->ok());QVERIFY(!sql::optimizer::optimize(c->model).ok());
      run("DROP SCHEMA owned_routine_categories CASCADE");
    }catch(const std::exception &e){QFAIL(e.what());}}
    void orderedPercentilesMatchOwnedPostgres() {try {
        using namespace navidog;
        using namespace navidog::sql::compiler;
        using namespace navidog::sql::optimizer;
        auto db=connectDatabase(profile_);
        const auto token=cancellation();
        const auto run=[&](const std::string &sql) {
            const auto result=db->execute(QString::fromStdString(sql),{},token);
            if (!result.error.isEmpty()) throw std::runtime_error(result.error.toStdString());
            return result;
        };
        const std::string source=" FROM (VALUES (1,1::smallint),(1,3::smallint),(1,6::smallint),"
            "(1,6::smallint),(1,8::smallint),(1,NULL::smallint),(2,NULL::smallint)) s(g,v)";
        const std::vector<std::pair<std::string,bool>> statements{
            {"SELECT g, percentile_disc(0) WITHIN GROUP (ORDER BY v DESC) AS maximum, "
             "percentile_disc(0.4) WITHIN GROUP (ORDER BY v) AS lower, "
             "percentile_disc(1) WITHIN GROUP (ORDER BY v DESC) FILTER (WHERE v>3) AS filtered, "
             "percentile_disc(NULL) WITHIN GROUP (ORDER BY v) AS unknown"+source+" GROUP BY g ORDER BY g",true},
            {"SELECT percentile_disc(0.5) WITHIN GROUP (ORDER BY v) AS no_input"+source+" WHERE false",true},
            {"SELECT percentile_disc(ARRAY[0,1,NULL]) WITHIN GROUP (ORDER BY v) AS discrete, "
             "percentile_cont(0.5) WITHIN GROUP (ORDER BY v) AS numeric_median, "
             "percentile_cont(ARRAY[0.25,0.75]) WITHIN GROUP (ORDER BY v * INTERVAL '1 day') AS durations"+source,false},
            {"SELECT percentile_disc(0.5) WITHIN GROUP (ORDER BY ARRAY[v]) AS collection"+source+" WHERE v IS NOT NULL",false},
            {"SELECT percentile_disc(p) WITHIN GROUP (ORDER BY v) AS grouped "
             "FROM (VALUES (0.25,1),(0.25,8),(0.75,4),(0.75,9)) s(p,v) GROUP BY p ORDER BY p",false}};
        for (const auto &[sql,checkLocal]:statements) {
            const auto parsed=compile(Dialect::PostgreSQL,sql);
            QVERIFY2(parsed->ok(),parsed->diagnostics.empty()?sql.c_str():parsed->diagnostics.front().message.c_str());
            const auto native=run(sql);
            const auto generated=generateSql(parsed->model,Dialect::PostgreSQL);
            QVERIFY2(generated.ok(),generated.diagnostics.empty()?sql.c_str():generated.diagnostics.front().message.c_str());
            const auto emitted=run(generated.sql);
            QCOMPARE(emitted.columns,native.columns);
            QCOMPARE(emitted.columnTypes,native.columnTypes);
            QCOMPARE(emitted.rows,native.rows);
            if (!checkLocal) continue;
            const auto local=interpret(parsed->model);
            QVERIFY2(local.ok(),local.diagnostics.empty()?sql.c_str():local.diagnostics.front().message.c_str());
            std::vector<LocalRow> expected;
            for (const auto &row:native.rows) {
                LocalRow values;
                for (const auto &value:row)
                    values.push_back(value.isNull()?LocalValue{}:LocalValue{std::int64_t(value.toLongLong())});
                expected.push_back(std::move(values));
            }
            QVERIFY(expected==local.rows);
        }
        const std::string invalid="SELECT percentile_disc(-0.5) WITHIN GROUP(ORDER BY v)"+source+" WHERE false";
        const auto parsed=compile(Dialect::PostgreSQL,invalid);
        QVERIFY(parsed->ok());
        std::string nativeError;
        try {
            nativeError=db->execute(QString::fromStdString(invalid),{},token).error.toStdString();
        } catch(const std::exception &error) {
            nativeError=error.what();
        }
        QVERIFY(nativeError.find("not between 0 and 1")!=std::string::npos);
        const auto localError=interpret(parsed->model);
        QVERIFY(!localError.ok() && localError.rows.empty());
        QCOMPARE(localError.diagnostics.front().code,DiagnosticCode::InvalidQueryShape);
    }catch(const std::exception &e){QFAIL(e.what());}}
    void orderedModeMatchesOwnedPostgres() {try {
        using namespace navidog;
        using namespace navidog::sql::compiler;
        using namespace navidog::sql::optimizer;
        auto db=connectDatabase(profile_);
        const auto token=cancellation();
        const auto run=[&](const std::string &sql) {
            const auto result=db->execute(QString::fromStdString(sql),{},token);
            if (!result.error.isEmpty()) throw std::runtime_error(result.error.toStdString());
            return result;
        };
        for (const auto *sql:{
            "SELECT g,mode() WITHIN GROUP (ORDER BY v) AS low_mode, "
            "pg_catalog.mode(*) WITHIN GROUP (ORDER BY v DESC NULLS FIRST) AS high_mode, "
            "mode() WITHIN GROUP (ORDER BY v DESC) FILTER (WHERE g=1 AND v<5) AS filtered "
            "FROM (VALUES (1,5::smallint),(1,2::smallint),(1,5::smallint),(1,2::smallint),"
            "(2,NULL::smallint),(2,8::smallint),(2,3::smallint),(3,NULL::smallint)) t(g,v) "
            "GROUP BY g ORDER BY g",
            "SELECT mode() WITHIN GROUP (ORDER BY v) AS empty_mode "
            "FROM (VALUES (7::smallint)) t(v) WHERE false",
            "SELECT mode() WITHIN GROUP (ORDER BY v ASC) AS mode_value "
            "FROM (VALUES ((-9223372036854775808)::bigint),(9223372036854775807),(NULL)) t(v)"}) {
            const auto parsed=compile(Dialect::PostgreSQL,sql);
            QVERIFY2(parsed->ok(),parsed->diagnostics.empty()?sql:parsed->diagnostics.front().message.c_str());
            const auto native=run(sql);
            const auto generated=generateSql(parsed->model,Dialect::PostgreSQL);
            QVERIFY2(generated.ok(),generated.diagnostics.empty()?sql:generated.diagnostics.front().message.c_str());
            const auto emitted=run(generated.sql);
            QCOMPARE(emitted.columns,native.columns);
            QCOMPARE(emitted.columnTypes,native.columnTypes);
            QCOMPARE(emitted.rows,native.rows);
            const auto local=interpret(parsed->model);
            QVERIFY2(local.ok(),local.diagnostics.empty()?sql:local.diagnostics.front().message.c_str());
            std::vector<LocalRow> expected;
            for (const auto &row:native.rows) {
                LocalRow values;
                for (const auto &value:row)
                    values.push_back(value.isNull()?LocalValue{}:LocalValue{std::int64_t(value.toLongLong())});
                expected.push_back(std::move(values));
            }
            QVERIFY(expected==local.rows);
        }
    }catch(const std::exception &e){QFAIL(e.what());}}
    void guardedAlterKeepsExistingColumnsInOwnedPostgres() {try {
        using namespace navidog;
        using namespace navidog::sql::compiler;
        auto db=connectDatabase(profile_);
        const auto token=cancellation();
        const auto run=[&](const std::string &sql) {
            const auto result=db->execute(QString::fromStdString(sql),{},token);
            if (!result.error.isEmpty()) throw std::runtime_error(result.error.toStdString());
            return result;
        };
        const auto alter=[&](const std::string &sql) {
            const auto result=convertSql(Dialect::PostgreSQL,Dialect::PostgreSQL,sql);
            if (!result.ok()) throw std::runtime_error(result.diagnostics.front().message);
            run(result.sql);
        };
        run("CREATE TEMP TABLE owned_guarded_alter (id INT, description TEXT)");
        run("INSERT INTO owned_guarded_alter VALUES (7,'before')");
        alter("ALTER TABLE IF EXISTS owned_missing_alter ADD COLUMN id INTEGER");
        alter("ALTER TABLE IF EXISTS owned_guarded_alter ADD IF NOT EXISTS id TEXT, "
              "ADD COLUMN IF NOT EXISTS status TEXT DEFAULT 'new', DROP IF EXISTS absent");
        alter("ALTER TABLE owned_guarded_alter RENAME description TO title");
        alter("ALTER TABLE owned_guarded_alter ALTER id SET DATA TYPE BIGINT USING id::BIGINT + 1");
        const auto result=run("SELECT pg_typeof(id)::text,id,title,status FROM owned_guarded_alter");
        QCOMPARE(result.rows.size(),1);
        QCOMPARE(result.rows[0][0].toString(),QString("bigint"));
        QCOMPARE(result.rows[0][1].toInt(),8);
        QCOMPARE(result.rows[0][2].toString(),QString("before"));
        QCOMPARE(result.rows[0][3].toString(),QString("new"));
    }catch(const std::exception &e){QFAIL(e.what());}}
    void generatedSqlMatchesOwnedPostgres(){try{
        using namespace navidog;using namespace navidog::sql::compiler;auto db=connectDatabase(profile_);auto token=cancellation();
        auto run=[&](const std::string& sql){auto c=compile(Dialect::PostgreSQL,sql);if(!c->ok())throw std::runtime_error(c->diagnostics.front().message);auto generated=generateSql(c->model,Dialect::PostgreSQL);if(!generated.ok())throw std::runtime_error(generated.diagnostics.front().message);auto result=db->execute(QString::fromStdString(generated.sql),{},token);if(!result.error.isEmpty())throw std::runtime_error(result.error.toStdString());return result;};
        run("CREATE TABLE gen_items(id INTEGER PRIMARY KEY,title VARCHAR(60) NOT NULL,n DECIMAL(10,2) DEFAULT 0,CHECK(n>=0))");
        run("INSERT INTO gen_items(id,title) VALUES(1,'one'),(2,'two')");run("UPDATE gen_items SET n=id+1 WHERE id=1");
        run("CREATE VIEW gen_view AS SELECT id,title,n FROM gen_items");run("CREATE INDEX gen_ix ON gen_items(title)");
        auto r=run("SELECT id,n FROM gen_view WHERE id=1");QCOMPARE(r.rows[0][1].toDouble(),2.0);run("DROP VIEW gen_view");
        run("DROP INDEX gen_ix");
        run("ALTER TABLE gen_items ADD extra INTEGER DEFAULT 5");r=run("SELECT extra FROM gen_items ORDER BY id");QCOMPARE(r.rows[0][0].toInt(),5);
        auto start=db->execute("START TRANSACTION",{},token);QVERIFY(start.error.isEmpty());run("TRUNCATE TABLE gen_items");auto rollback=db->execute("ROLLBACK",{},token);QVERIFY(rollback.error.isEmpty());
        r=run("SELECT count(*) FROM gen_items");QCOMPARE(r.rows[0][0].toInt(),2);
        run("DROP TABLE gen_items");
    }catch(const std::exception& e){QFAIL(e.what());}}
    void assignmentEvaluationOrderIsVerifiedAndUnsafeTranslationRefused() {try {
        using namespace navidog;using namespace navidog::sql::compiler;auto db=connectDatabase(profile_);auto token=cancellation();
        auto run=[&](const QString &sql){auto r=db->execute(sql,{},token);if(!r.error.isEmpty())throw std::runtime_error(r.error.toStdString());return r;};
        run("CREATE TEMPORARY TABLE assignment_owned(a INTEGER,b INTEGER)");run("INSERT INTO assignment_owned VALUES(1,0)");
        const auto statement="UPDATE assignment_owned SET a=a+1,b=a;";
        auto translated=convertSql(Dialect::PostgreSQL,Dialect::MySQL,statement);QVERIFY(!translated.ok() && translated.sql.empty());
        run(statement);auto row=run("SELECT a,b FROM assignment_owned");QCOMPARE(row.rows[0][0].toInt(),2);QCOMPARE(row.rows[0][1].toInt(),1);
        run("INSERT INTO assignment_owned VALUES(3,4)");
        auto having=run("SELECT 1 FROM assignment_owned HAVING TRUE");QCOMPARE(having.rows.size(),1);
    }catch(const std::exception &e){QFAIL(e.what());}}
    void dmlCompilerMatchesNativePgExecution(){
        using namespace navidog;QueryService service;QSignalSpy done(&service,&QueryService::completed);QueryExecutionOptions options;options.sessionId="owned-pg-dml";
        auto run=[&](QString sql){if(!service.run(profile_,sql,{},options)||!done.wait(3000))throw std::runtime_error("owned PG DML timeout");return qvariant_cast<QueryOutcome>(done.takeFirst()[0]);};
        auto r=run("CREATE TEMP TABLE dml_items(id int primary key,title text);CREATE TEMP TABLE dml_other(id int primary key,title text)");QVERIFY(r.result.error.isEmpty());
        for(auto sql:{"INSERT INTO dml_other VALUES(1,'one'),(2,'two')", "INSERT INTO dml_items SELECT id,title FROM dml_other RETURNING id", "UPDATE dml_items i SET title=o.title FROM dml_other o WHERE i.id=o.id RETURNING i.id", "INSERT INTO dml_items VALUES(1,'changed') ON CONFLICT(id) DO UPDATE SET title=excluded.title RETURNING title", "DELETE FROM dml_items i USING dml_other o WHERE i.id=o.id AND o.id=2 RETURNING i.id", "SELECT id FROM dml_items NATURAL JOIN dml_other"}) {
            r=run(sql);QVERIFY2(r.result.error.isEmpty(),qPrintable(r.result.error));QCOMPARE(r.analysis.statements[0].state,SqlAnalysisState::Parsed);QVERIFY(!r.analysis.statements[0].unifiedAst.isEmpty());
        }
        r=run("SELECT id,title FROM dml_items");QCOMPARE(r.result.rows.size(),1);QCOMPARE(r.result.rows[0][0].toInt(),1);QCOMPARE(r.result.rows[0][1].toString(),QString("changed"));
    }
    void nativeQueriesCatalogAndOverloads() {
        auto db=navidog::connectDatabase(profile_); auto token=navidog::cancellation();
        auto run=[&](QString sql){auto r=db->execute(sql,{},token); QVERIFY2(r.error.isEmpty(),qPrintable(r.error));};
        run("CREATE SCHEMA \"目录.name'\\\\\"");
        const QString schema="目录.name'\\\\";
        auto q=navidog::quoteIdentifier(schema,profile_.driver);
        run("CREATE TABLE "+q+".\"odd.table\"(id integer PRIMARY KEY, note text)");
        run("CREATE FUNCTION "+q+".f(input_value integer) RETURNS integer LANGUAGE sql AS $$ SELECT input_value + 1 $$");
        run("CREATE FUNCTION "+q+".f(input_value text) RETURNS text LANGUAGE sql AS $$ SELECT input_value $$");
        run("INSERT INTO "+q+".\"odd.table\" VALUES (1,NULL),(2,'你好')");
        navidog::QueryLimits limits; limits.maxRows=1; auto cap=db->execute("SELECT * FROM "+q+".\"odd.table\" ORDER BY id",limits,token);
        QCOMPARE(cap.rows.size(),1); QVERIFY(cap.truncated); QVERIFY(!cap.rows[0][1].isValid());
        auto next=db->execute("SELECT 'after cap' AS status",{},token); QCOMPARE(next.rows[0][0].toString(),QString("after cap"));
        navidog::CatalogNode root; auto databases=navidog::loadCatalog(*db,profile_,root,token); QVERIFY(!databases.nodes.isEmpty());
        navidog::CatalogNode n; n.kind="schema"; n.schema=schema; n.database="postgres";
        auto folders=navidog::loadCatalog(*db,profile_,n,token); QCOMPARE(folders.nodes.size(),4);
        auto tables=navidog::loadCatalog(*db,profile_,folders.nodes[0],token); QCOMPARE(tables.nodes.size(),1); QCOMPARE(tables.nodes[0].name,QString("odd.table"));
        auto children=navidog::loadCatalog(*db,profile_,tables.nodes[0],token);
        auto columns=navidog::loadCatalog(*db,profile_,children.nodes[0],token); QCOMPARE(columns.nodes.size(),2);
        auto indexes=navidog::loadCatalog(*db,profile_,children.nodes[1],token); QCOMPARE(indexes.nodes.size(),1);
        auto functions=navidog::loadCatalog(*db,profile_,folders.nodes[2],token); QCOMPARE(functions.nodes.size(),2); QVERIFY(functions.nodes[0].key!=functions.nodes[1].key);
        for(auto f:functions.nodes) {
            auto defs=db->execute(navidog::definitionSql(profile_,f),{},token); QVERIFY(defs.rows[0][0].toString().contains("FUNCTION"));
            auto paramsFolder=navidog::loadCatalog(*db,profile_,f,token); auto params=navidog::loadCatalog(*db,profile_,paramsFolder.nodes[0],token);
            QCOMPARE(params.nodes.size(),1); QCOMPARE(params.nodes[0].name,QString("input_value"));
        }
        run("BEGIN"); run("INSERT INTO "+q+".\"odd.table\" VALUES (3,'rollback')"); run("ROLLBACK");
        auto count=db->execute("SELECT count(*) FROM "+q+".\"odd.table\"",{},token); QCOMPARE(count.rows[0][0].toInt(),2);
    }
    void queryTypesAreNativeAndAvailableWithoutRows(){
        auto db=navidog::connectDatabase(profile_);auto result=db->execute("SELECT CAST(NULL AS varchar(13)),CAST(NULL AS integer),CAST(NULL AS jsonb) WHERE false",{},navidog::cancellation());QCOMPARE(result.rows.size(),0);QCOMPARE(result.columnTypes,QStringList({"varchar(13)","integer","jsonb"}));
    }
    void cancellationAndDatabaseContext() {
        auto db=navidog::connectDatabase(profile_); db->execute("CREATE DATABASE navidog_other",{},navidog::cancellation()); db.reset();
        navidog::QueryService service; QSignalSpy result(&service,&navidog::QueryService::completed), catalog(&service,&navidog::QueryService::catalogCompleted);
        navidog::CatalogNode n; n.kind="database"; n.name="navidog_other"; n.database="navidog_other";
        QVERIFY(service.catalog(profile_,n,"other-database")); QTRY_COMPARE_WITH_TIMEOUT(catalog.count(),1,5000); QVERIFY(qvariant_cast<navidog::CatalogResult>(catalog[0][1]).error.isEmpty());
        auto other=profile_; other.database="navidog_other";
        QVERIFY(service.run(other,"SELECT current_database()")); QTRY_COMPARE_WITH_TIMEOUT(result.count(),1,5000);
        QCOMPARE(qvariant_cast<navidog::QueryOutcome>(result.takeFirst()[0]).result.rows[0][0].toString(),QString("navidog_other"));
        QVERIFY(service.run(profile_,"SELECT pg_sleep(20)")); QTimer::singleShot(50,&service,&navidog::QueryService::cancel);
        QTRY_COMPARE_WITH_TIMEOUT(result.count(),1,5000); QVERIFY(qvariant_cast<navidog::QueryOutcome>(result.takeFirst()[0]).result.cancelled);
        QVERIFY(service.run(profile_,"SELECT 9")); QTRY_COMPARE_WITH_TIMEOUT(result.count(),1,5000); QCOMPARE(qvariant_cast<navidog::QueryOutcome>(result.takeFirst()[0]).result.rows[0][0].toInt(),9);
    }
    void namespaceDropUsesMaintenanceConnectionAndRespectsDependencies() {
        using namespace navidog;
        auto db=connectDatabase(profile_);
        auto token=cancellation();
        const QString name="Owned Drop Database";
        auto setup=db->execute("CREATE DATABASE "+quoteIdentifier(name,profile_.driver),{},token);
        QVERIFY2(setup.error.isEmpty(),qPrintable(setup.error));
        QueryService service;
        QSignalSpy done(&service,&QueryService::namespaceCompleted);
        auto target=profile_;
        target.database=name;
        QVERIFY(service.dropNamespace(target,NamespaceKind::Database,name,"owned-db-drop"));
        QTRY_COMPARE_WITH_TIMEOUT(done.count(),1,3000);
        const auto removed=done.takeFirst().at(1).value<NamespaceResult>();
        QVERIFY2(removed.applied,qPrintable(removed.error));
        QCOMPARE(db->execute("SELECT COUNT(*) FROM pg_database WHERE datname='Owned Drop Database'",{},token)
                 .rows.front().front().toInt(),0);

        setup=db->execute("CREATE SCHEMA owned_drop_schema",{},token);
        QVERIFY2(setup.error.isEmpty(),qPrintable(setup.error));
        setup=db->execute("CREATE TABLE owned_drop_schema.keep_if_nonempty(id integer)",{},token);
        QVERIFY2(setup.error.isEmpty(),qPrintable(setup.error));
        QVERIFY(service.dropNamespace(profile_,NamespaceKind::Schema,"owned_drop_schema","owned-nonempty"));
        QTRY_COMPARE_WITH_TIMEOUT(done.count(),1,3000);
        QVERIFY(!done.takeFirst().at(1).value<NamespaceResult>().applied);
        setup=db->execute("DROP TABLE owned_drop_schema.keep_if_nonempty",{},token);
        QVERIFY2(setup.error.isEmpty(),qPrintable(setup.error));
        QVERIFY(service.dropNamespace(profile_,NamespaceKind::Schema,"owned_drop_schema","owned-empty"));
        QTRY_COMPARE_WITH_TIMEOUT(done.count(),1,3000);
        const auto dropped=done.takeFirst().at(1).value<NamespaceResult>();
        QVERIFY2(dropped.applied,qPrintable(dropped.error));
    }
    void namespaceCreationUsesSeparateSessionsAndCorrectDatabase() {
        using namespace navidog;QueryService service;QSignalSpy queries(&service,&QueryService::completed),done(&service,&QueryService::namespaceCompleted),options(&service,&QueryService::namespaceOptionsCompleted),refreshed(&service,&QueryService::catalogCompleted);
        QVERIFY(service.run(profile_,"BEGIN"));QTRY_COMPARE_WITH_TIMEOUT(queries.count(),1,3000);QVERIFY(qvariant_cast<QueryOutcome>(queries.takeFirst()[0]).result.error.isEmpty());
        QVERIFY(service.run(profile_,"CREATE TABLE public.namespace_uncommitted(id integer)"));QTRY_COMPARE_WITH_TIMEOUT(queries.count(),1,3000);QVERIFY(qvariant_cast<QueryOutcome>(queries.takeFirst()[0]).result.error.isEmpty());
        NamespaceDraft draft;draft.name="namespace_owned";
        QVERIFY(service.createNamespace(profile_,draft,"new-db"));QTRY_COMPARE_WITH_TIMEOUT(done.count(),1,5000);auto result=qvariant_cast<NamespaceResult>(done.takeFirst()[1]);QVERIFY2(result.applied,qPrintable(result.error));QVERIFY(service.isConnected(profile_.id));
        CatalogNode refresh;refresh.kind="connection";
        QVERIFY(service.catalog(profile_,refresh,"isolated-refresh",true));QTRY_COMPARE_WITH_TIMEOUT(refreshed.count(),1,3000);auto refreshedResult=qvariant_cast<CatalogResult>(refreshed.takeFirst()[1]);QVERIFY(refreshedResult.error.isEmpty());QVERIFY(std::any_of(refreshedResult.nodes.begin(),refreshedResult.nodes.end(),[](auto n){return n.name=="namespace_owned";}));
        // A failed post-DDL refresh cannot discard the query session/transaction.
        auto invalid=profile_;invalid.database="not_an_owned_database";
        QVERIFY(service.catalog(invalid,refresh,"failed-isolated-refresh",true));QTRY_COMPARE_WITH_TIMEOUT(refreshed.count(),1,3000);QVERIFY(!qvariant_cast<CatalogResult>(refreshed.takeFirst()[1]).error.isEmpty());QVERIFY(service.isConnected(profile_.id));
        // CREATE DATABASE would fail inside the query's transaction. Rolling that
        // transaction back must still remove its uncommitted table.
        QVERIFY(service.run(profile_,"ROLLBACK"));QTRY_COMPARE_WITH_TIMEOUT(queries.count(),1,3000);queries.clear();auto db=connectDatabase(profile_);auto absent=db->execute("SELECT to_regclass('public.namespace_uncommitted') IS NULL",{},cancellation());QCOMPARE(absent.rows[0][0].toString(),QString("t"));
        auto target=profile_;target.database=draft.name;draft.kind=NamespaceKind::Schema;draft.name="schema\"; SELECT 9; --";draft.owner="navidog_test";
        QVERIFY(service.namespaceOptions(target,draft.kind,"owners"));QTRY_COMPARE_WITH_TIMEOUT(options.count(),1,3000);auto choices=qvariant_cast<NamespaceOptions>(options.takeFirst()[1]);QVERIFY2(choices.error.isEmpty(),qPrintable(choices.error));QVERIFY(choices.owners.contains("navidog_test"));
        QVERIFY(service.createNamespace(target,draft,"new-schema"));QTRY_COMPARE_WITH_TIMEOUT(done.count(),1,3000);result=qvariant_cast<NamespaceResult>(done.takeFirst()[1]);QVERIFY2(result.applied,qPrintable(result.error));
        auto created=connectDatabase(target);CatalogNode node;node.kind="database";node.database=target.database;auto catalog=loadCatalog(*created,target,node,cancellation());QVERIFY(catalog.error.isEmpty());QVERIFY(std::any_of(catalog.nodes.begin(),catalog.nodes.end(),[&](auto n){return n.schema==draft.name;}));
        auto original=loadCatalog(*db,profile_,node,cancellation());QVERIFY(std::none_of(original.nodes.begin(),original.nodes.end(),[&](auto n){return n.schema==draft.name;}));
        QVERIFY(service.createNamespace(target,draft,"duplicate-schema"));QTRY_COMPARE_WITH_TIMEOUT(done.count(),1,3000);result=qvariant_cast<NamespaceResult>(done.takeFirst()[1]);QVERIFY(!result.applied);QVERIFY(!result.error.isEmpty());
        auto readonly=target;readonly.readOnly=true;draft.name="must_not_create";result=createNamespace(readonly,draft,cancellation());QVERIFY(!result.error.isEmpty());
        created.reset();QVERIFY(db->execute("DROP DATABASE namespace_owned",{},cancellation()).error.isEmpty());
    }
    void tableTasksCopyIdentityGrantGenerateExportAndProtectTransactions() {
        using namespace navidog;auto db=connectDatabase(profile_);auto token=cancellation();auto run=[&](QString sql){auto result=db->execute(sql,{},token);QVERIFY2(result.error.isEmpty(),qPrintable(result.error));};
        run("CREATE TABLE public.pg_actions_owned(id integer GENERATED ALWAYS AS IDENTITY PRIMARY KEY,label text NOT NULL UNIQUE,n integer NOT NULL,calculated integer GENERATED ALWAYS AS (n*2) STORED)");
        TableTask task;task.node.kind="table";task.node.database=profile_.database;task.node.schema="public";task.node.object="pg_actions_owned";task.action=TableAction::GenerateData;task.maxRows=4;task.generationRules={{"label",QJsonObject{{"kind","series"},{"value","item_"}}},{"n",QJsonObject{{"kind","sequence"},{"value","20"}}}};
        auto generated=runTableTask(profile_,task,token);QVERIFY2(generated.applied,qPrintable(generated.error));QCOMPARE(generated.rows,4);
        QueryService service;QSignalSpy query(&service,&QueryService::completed),completed(&service,&QueryService::tableTaskCompleted);QVERIFY(service.run(profile_,"BEGIN"));QTRY_COMPARE_WITH_TIMEOUT(query.count(),1,3000);query.clear();QVERIFY(service.run(profile_,"CREATE TABLE public.pg_task_uncommitted(id integer)"));QTRY_COMPARE_WITH_TIMEOUT(query.count(),1,3000);query.clear();
        task.action=TableAction::CopyData;task.targetName="pg_actions_copy";QVERIFY(service.tableTask(profile_,task,"owned-copy"));QTRY_COMPARE_WITH_TIMEOUT(completed.count(),1,5000);auto copied=qvariant_cast<TableTaskResult>(completed.takeFirst()[1]);QVERIFY2(copied.applied,qPrintable(copied.error));QCOMPARE(copied.rows,4);
        run("INSERT INTO public.pg_actions_copy(label,n) VALUES('new',99)");auto identity=db->execute("SELECT id FROM public.pg_actions_copy WHERE label='new'",{},token);QCOMPARE(identity.rows[0][0].toInt(),5);
        QVERIFY(service.run(profile_,"ROLLBACK"));QTRY_COMPARE_WITH_TIMEOUT(query.count(),1,3000);QCOMPARE(db->execute("SELECT to_regclass('public.pg_task_uncommitted') IS NULL",{},token).rows[0][0].toString(),QString("t"));
        run("CREATE ROLE owned_reader NOLOGIN");task.action=TableAction::GrantPrivileges;task.grantee="owned_reader";task.privileges={"SELECT"};auto grant=runTableTask(profile_,task,token);QVERIFY2(grant.applied,qPrintable(grant.error));QCOMPARE(db->execute("SELECT has_table_privilege('owned_reader','public.pg_actions_owned','SELECT')",{},token).rows[0][0].toString(),QString("t"));
        task.revoke=true;QVERIFY(runTableTask(profile_,task,token).applied);QCOMPARE(db->execute("SELECT has_table_privilege('owned_reader','public.pg_actions_owned','SELECT')",{},token).rows[0][0].toString(),QString("f"));
        task.action=TableAction::ReverseModel;auto model=runTableTask(profile_,task,token);QVERIFY2(model.applied,qPrintable(model.error));QCOMPARE(model.document["columns"].toArray().size(),4);
        task.action=TableAction::BuildBI;task.dimension="label";auto bi=runTableTask(profile_,task,token);QVERIFY2(bi.applied,qPrintable(bi.error));QCOMPARE(bi.rows,4);
        task.action=TableAction::ExportSql;task.path=dir_.filePath("pg-actions.sql");task.withData=true;auto dumped=runTableTask(profile_,task,token);QVERIFY2(dumped.applied,qPrintable(dumped.error));
        QFile script(task.path);QVERIFY(script.open(QIODevice::ReadOnly));auto sql=script.readAll();QVERIFY(sql.contains("CREATE TABLE public.pg_actions_owned"));QVERIFY(sql.contains("INSERT INTO public.pg_actions_owned"));QVERIFY(!sql.contains("CREATE ROLE"));
        run("CREATE DATABASE owned_dump_restore");auto psql=QFileInfo(pgCtl_).dir().filePath("psql");QVERIFY(command(psql,{"-X","-v","ON_ERROR_STOP=1","-h",profile_.host,"-p",QString::number(profile_.port),"-U",profile_.user,"-d","owned_dump_restore","-f",task.path}));auto restored=profile_;restored.database="owned_dump_restore";auto restoredDb=connectDatabase(restored);QCOMPARE(restoredDb->execute("SELECT count(*) FROM public.pg_actions_owned",{},token).rows[0][0].toInt(),4);restoredDb.reset();run("DROP DATABASE owned_dump_restore");
    }
    void postgresDesignerRoundTripsIndexesForeignKeysChecksCommentsAndTriggers() {
        using namespace navidog;auto db=connectDatabase(profile_);auto token=cancellation();auto run=[&](QString text){auto r=db->execute(text,{},token);if(!r.error.isEmpty())throw std::runtime_error(r.error.toStdString());};
        run("CREATE TABLE public.design_parent(id integer PRIMARY KEY)");run("INSERT INTO public.design_parent VALUES(1)");run("CREATE TABLE public.design_audit(note text)");run("CREATE FUNCTION public.design_log_fn() RETURNS trigger LANGUAGE plpgsql AS $$ BEGIN INSERT INTO public.design_audit VALUES('logged'); RETURN NEW; END $$");
        TableDefinition empty,after;after.node.kind="table";after.node.schema="public";after.node.object="design_objects";TableColumn id;id.name="id";id.type="integer";id.primaryKey=1;id.nullable=false;id.comment="编号'\\注释";after.columns<<id;TableColumn value;value.name="parent_id";value.type="integer";after.columns<<value;after.comment="表'\\注释";
        TableObject fk;fk.kind=TableObjectKind::ForeignKey;fk.name="design_fk";fk.columns={"parent_id"};fk.referenceSchema="public";fk.referenceTable="design_parent";fk.referenceColumns={"id"};fk.onDelete="CASCADE";after.objects<<fk;
        TableObject index;index.name="design_index";index.expression="parent_id DESC";index.predicate="parent_id IS NOT NULL";index.comment="索引说明";after.objects<<index;
        TableObject check;check.kind=TableObjectKind::Check;check.name="design_check";check.expression="id>0";after.objects<<check;
        TableObject trigger;trigger.kind=TableObjectKind::Trigger;trigger.name="design_trigger";trigger.sql="CREATE TRIGGER design_trigger AFTER INSERT ON public.design_objects FOR EACH ROW EXECUTE FUNCTION public.design_log_fn()";after.objects<<trigger;
        auto r=applyTableChanges(profile_,empty,after,token);QVERIFY2(r.error.isEmpty(),qPrintable(r.error));QCOMPARE(r.definition.objects.size(),4);QCOMPARE(r.definition.comment,after.comment);QCOMPARE(r.definition.columns[0].comment,id.comment);
        run("INSERT INTO public.design_objects VALUES(1,1)");QCOMPARE(db->execute("SELECT count(*) FROM public.design_audit",{},token).rows[0][0].toInt(),1);
        auto before=r.definition;after=before;for(auto& o:after.objects)if(o.kind==TableObjectKind::Index){o.name="design_index_renamed";o.predicate="parent_id>0";}after.comment="updated";r=applyTableChanges(profile_,before,after,token);QVERIFY2(r.error.isEmpty(),qPrintable(r.error));QCOMPARE(r.definition.comment,QString("updated"));
        before=r.definition;after=before;for(auto& o:after.objects)if(o.kind==TableObjectKind::Check)o.expression="id<0";r=applyTableChanges(profile_,before,after,token);QVERIFY(!r.error.isEmpty());QVERIFY(r.rolledBack);QVERIFY(sameTableStructure(before,readTableDefinition(*db,profile_,before.node,token)));
        after=before;for(auto& o:after.objects)if(o.kind==TableObjectKind::Trigger)o.enabled=false;r=applyTableChanges(profile_,before,after,token);QVERIFY2(r.error.isEmpty(),qPrintable(r.error));run("INSERT INTO public.design_objects VALUES(2,1)");QCOMPARE(db->execute("SELECT count(*) FROM public.design_audit",{},token).rows[0][0].toInt(),1);
        before=r.definition;after=before;after.objects.clear();r=applyTableChanges(profile_,before,after,token);QVERIFY2(r.error.isEmpty(),qPrintable(r.error));QVERIFY(r.definition.objects.isEmpty());
    }
    void tableDesignerPostgresCreatesAltersAndRollsBack() {
        auto cancel=navidog::cancellation();navidog::TableDefinition empty,after;after.node.kind="table";after.node.schema="public";after.node.object=after.node.name="navidog_designed";
        navidog::TableColumn id;id.name="id";id.type="integer";id.primaryKey=1;id.nullable=false;navidog::TableColumn note;note.name="note";note.type="varchar(20)";note.defaultExpression="'seed'";after.columns={id,note};
        auto created=navidog::applyTableChanges(profile_,empty,after,cancel);QVERIFY2(created.error.isEmpty(),qPrintable(created.error));QCOMPARE(created.definition.columns.size(),2);QVERIFY(created.definition.columns[1].nullable);QCOMPARE(created.definition.columns[0].primaryKey,1);
        auto baseline=created.definition,changed=baseline;changed.columns[1].name="caption";changed.columns[1].type="varchar(100)";changed.columns[1].nullable=false;changed.columns[1].defaultExpression="'new'";
        auto saved=navidog::applyTableChanges(profile_,baseline,changed,cancel);QVERIFY2(saved.error.isEmpty(),qPrintable(saved.error));QCOMPARE(saved.definition.columns[1].name,QString("caption"));QVERIFY(!saved.definition.columns[1].nullable);QVERIFY(saved.definition.columns[1].type.contains("100"));
        baseline=saved.definition;changed=baseline;changed.columns.removeAt(1);navidog::TableColumn broken;broken.name="broken";broken.type="nonexistent_type";changed.columns<<broken;
        auto failed=navidog::applyTableChanges(profile_,baseline,changed,cancel);QVERIFY(!failed.error.isEmpty());QVERIFY(failed.rolledBack);QVERIFY(!failed.partial);
        auto db=navidog::connectDatabase(profile_);auto actual=navidog::readTableDefinition(*db,profile_,baseline.node,cancel);QCOMPARE(actual.columns,baseline.columns);
    }
    void dataEditorPostgresBoundValuesVersionsAndRollback() {
        using namespace navidog;auto db=connectDatabase(profile_);auto token=cancellation();
        db->execute("CREATE TABLE public.data_owned(id integer GENERATED BY DEFAULT AS IDENTITY PRIMARY KEY,label text UNIQUE NOT NULL DEFAULT 'new',note text,payload bytea,doc jsonb)",{},token);
        auto blob=QByteArray::fromHex("0001ff");auto insert=db->executeBound("INSERT INTO public.data_owned(label,note,payload,doc) VALUES($1,$2,$3,$4)",{QString("'); DROP TABLE public.data_owned; --"),QVariant{},blob,QString("{\"x\":1}")},{},token);QCOMPARE(insert.affectedRows,1);
        for(int i=0;i<5;++i)db->executeBound("INSERT INTO public.data_owned(label,note) VALUES($1,$2)",{QString("row%1").arg(i),QString("")},{},token);
        DataPageRequest request;request.node.kind="table";request.node.schema="public";request.node.object="data_owned";request.pageSize=2;auto page=readDataPage(profile_,request,token);QVERIFY2(page.error.isEmpty(),qPrintable(page.error));QVERIFY(page.editable);QCOMPARE(page.result.columns.size(),5);QCOMPARE(page.result.rows[0][3].toByteArray(),blob);QVERIFY(!page.result.rows[0][1].toString().isEmpty());QVERIFY(!page.versions[0].isEmpty());QVERIFY(!page.result.rows[0][2].isValid());QVERIFY(page.result.rows[1][2].isValid());
        auto next=request;next.afterKey={page.result.rows.last()[0]};next.offset=2;auto second=readDataPage(profile_,next,token);QVERIFY(second.error.isEmpty());QVERIFY(second.keyset);QCOMPARE(second.result.rows[0][0].toInt(),3);
        auto filter=request;filter.filterColumn=1;filter.filter=DataFilter::Contains;filter.filterValue="row";auto filtered=readDataPage(profile_,filter,token);QVERIFY2(filtered.error.isEmpty(),qPrintable(filtered.error));QCOMPARE(filtered.result.rows.size(),2);
        DataRowChange change;change.state=DataRowState::Modified;change.original=change.values=page.result.rows[0];change.version=page.versions[0];change.values[2]="";change.values[3]=QByteArray("",0);change.touched={2,3};auto committed=commitDataChanges(profile_,page.schema,{change},token);QVERIFY2(committed.error.isEmpty(),qPrintable(committed.error));QCOMPARE(committed.updated,1);
        auto actual=db->execute("SELECT note,payload FROM public.data_owned WHERE id=1",{},token);QVERIFY(actual.rows[0][0].isValid());QCOMPARE(actual.rows[0][0].toString(),QString(""));QCOMPARE(actual.rows[0][1].toByteArray(),QByteArray("",0));
        page=readDataPage(profile_,request,token);change.original=change.values=page.result.rows[0];change.version=page.versions[0];change.values[2]="stale";change.touched={2};db->execute("UPDATE public.data_owned SET doc='{\"x\":2}' WHERE id=1",{},token);committed=commitDataChanges(profile_,page.schema,{change},token);QVERIFY(committed.error.contains("其他会话"));QVERIFY(committed.rolledBack);
        page=readDataPage(profile_,request,token);change.original=change.values=page.result.rows[0];change.version=page.versions[0];change.values[2]="must rollback";
        DataRowChange bad;bad.state=DataRowState::Inserted;bad.values.resize(5);bad.values[1]=page.result.rows[0][1];bad.touched={1};committed=commitDataChanges(profile_,page.schema,{change,bad},token);QVERIFY(!committed.error.isEmpty());QVERIFY(committed.rolledBack);actual=db->execute("SELECT note FROM public.data_owned WHERE id=1",{},token);QCOMPARE(actual.rows[0][0].toString(),QString(""));
        DataRowChange remove;remove.state=DataRowState::Deleted;remove.original=remove.values=page.result.rows[1];remove.version=page.versions[1];DataRowChange fresh;fresh.state=DataRowState::Inserted;fresh.values.resize(5);fresh.values[1]="inserted safely";fresh.touched={1};committed=commitDataChanges(profile_,page.schema,{remove,fresh},token);QVERIFY2(committed.error.isEmpty(),qPrintable(committed.error));QCOMPARE(committed.deleted,1);QCOMPARE(committed.inserted,1);
    }
    void explainPreservesOriginalTransactionsAndNeverRunsDelete(){try{
        using namespace navidog;auto db=connectDatabase(profile_);auto token=cancellation();db->setAutoCommit(true);db->execute("CREATE TABLE plan_owned(ID integer PRIMARY KEY)",{},token);db->execute("INSERT INTO plan_owned VALUES(1)",{},token);
        QueryService service;QSignalSpy done(&service,&QueryService::completed);QueryExecutionOptions options;options.sessionId="owned-plan-transaction";options.autoCommit=false;auto run=[&](QString text){if(!service.run(profile_,text,{},options) || !done.wait(5000))throw std::runtime_error("plan test timed out");return qvariant_cast<QueryOutcome>(done.takeFirst()[0]);};
        auto result=run("INSERT INTO plan_owned VALUES(2)");QVERIFY2(result.result.error.isEmpty(),qPrintable(result.result.error));QCOMPARE(result.transactionState,TransactionState::Active);options.explain=true;result=run("DELETE FROM plan_owned WHERE ID=1");QVERIFY(result.explained);QVERIFY2(result.result.error.isEmpty(),qPrintable(result.result.error));QVERIFY(!result.result.rows.isEmpty());QCOMPARE(result.transactionState,TransactionState::Active);QCOMPARE(db->execute("SELECT count(*) FROM plan_owned",{},token).rows[0][0].toInt(),1);
        options.explain=false;options.transaction=TransactionAction::Rollback;result=run({});QCOMPARE(result.transactionState,TransactionState::Idle);QCOMPARE(db->execute("SELECT count(*) FROM plan_owned",{},token).rows[0][0].toInt(),1);
    }catch(const std::exception& e){QFAIL(e.what());}}
    void querySchemaContextResolvesUnqualifiedNamesAndProtectsTransactions(){try{
        using namespace navidog;auto db=connectDatabase(profile_);auto token=cancellation();auto setup=db->execute("CREATE SCHEMA query_ctx_a",{},token);QVERIFY2(setup.error.isEmpty(),qPrintable(setup.error));setup=db->execute("CREATE SCHEMA query_ctx_odd",{},token);
        QVERIFY2(setup.error.isEmpty(),qPrintable(setup.error));
        for(auto sql:{"CREATE TABLE query_ctx_a.context_probe(n int)","INSERT INTO query_ctx_a.context_probe VALUES(41)","CREATE TABLE query_ctx_odd.context_probe(n int)","INSERT INTO query_ctx_odd.context_probe VALUES(43)"}){setup=db->execute(sql,{},token);QVERIFY2(setup.error.isEmpty(),qPrintable(setup.error));}
        QueryService service;QSignalSpy done(&service,&QueryService::completed);QueryExecutionOptions options;options.sessionId="owned-schema-context";options.schema="query_ctx_a";auto run=[&](QString sql){if(!service.run(profile_,sql,{},options) || !done.wait(5000))throw std::runtime_error("context query timeout");return qvariant_cast<QueryOutcome>(done.takeFirst()[0]);};
        auto result=run("SELECT n FROM context_probe");QVERIFY2(result.result.error.isEmpty(),qPrintable(result.result.error));QCOMPARE(result.result.rows[0][0].toInt(),41);options.autoCommit=false;result=run("INSERT INTO context_probe VALUES(47)");QCOMPARE(result.transactionState,TransactionState::Active);options.schema="query_ctx_odd";result=run("SELECT n FROM context_probe");QVERIFY(result.result.error.contains("先提交"));QCOMPARE(result.transactionState,TransactionState::Active);options.schema="query_ctx_a";options.transaction=TransactionAction::Rollback;result=run({});QCOMPARE(result.transactionState,TransactionState::Idle);
        options.transaction=TransactionAction::None;options.autoCommit=true;options.schema="query_ctx_odd";result=run("SELECT n FROM context_probe");QVERIFY2(result.result.error.isEmpty(),qPrintable(result.result.error));QCOMPARE(result.result.rows[0][0].toInt(),43);options.explain=true;result=run("SELECT n FROM context_probe");QVERIFY2(result.result.error.isEmpty(),qPrintable(result.result.error));QVERIFY(!result.result.rows.isEmpty());options.explain=false;options.schema="missing_owned_schema";result=run("SELECT 99");QVERIFY(!result.result.error.isEmpty());QCOMPARE(result.executedStatements,0);options.schema.clear();result=run("SELECT current_schema()");QVERIFY2(result.result.error.isEmpty(),qPrintable(result.result.error));QCOMPARE(result.result.rows[0][0].toString(),QString("public"));
    }catch(const std::exception& e){QFAIL(e.what());}}
    void parallelQuerySessionsDoNotCancelEachOther(){
        using namespace navidog;QueryService service;QSignalSpy done(&service,&QueryService::completed);QueryExecutionOptions first,second;first.sessionId="owned-pg-slow";second.sessionId="owned-pg-other";
        QVERIFY(service.run(profile_,"SELECT pg_sleep(20)",{},first));QVERIFY(service.run(profile_,"SELECT pg_sleep(0.3), 41",{},second));
        QTimer::singleShot(50,&service,[&]{service.cancelQuery(profile_.id,profile_.database,first.sessionId);});QTRY_COMPARE_WITH_TIMEOUT(done.count(),2,4000);bool cancelled=false,successful=false;for(auto event:done){auto result=qvariant_cast<QueryOutcome>(event[0]);if(result.sessionId==first.sessionId){QVERIFY(result.result.cancelled);QVERIFY(result.sessionClosed);cancelled=true;}else{QVERIFY2(result.result.error.isEmpty(),qPrintable(result.result.error));QVERIFY(!result.result.cancelled);QCOMPARE(result.result.rows[0][1].toInt(),41);successful=true;}}QVERIFY(cancelled && successful);QCOMPARE(service.sessionCount(),1);
    }
    void querySessionsPreserveFailedTransactionsUntilRollback(){
        using namespace navidog;QueryService service;QSignalSpy done(&service,&QueryService::completed);QueryExecutionOptions options;options.sessionId="owned-pg-query";options.autoCommit=false;auto run=[&](QString sql){if(!service.run(profile_,sql,{},options))throw std::runtime_error("busy");if(!done.wait(3000))throw std::runtime_error("timeout");return qvariant_cast<QueryOutcome>(done.takeFirst()[0]);};
        auto result=run("CREATE TEMP TABLE owned_tx(n int); INSERT INTO owned_tx VALUES(1)");QVERIFY2(result.result.error.isEmpty(),qPrintable(result.result.error));QCOMPARE(result.transactionState,TransactionState::Active);result=run("SELECT 1/0");QVERIFY(!result.result.error.isEmpty());QCOMPARE(result.transactionState,TransactionState::Failed);QVERIFY(!result.sessionClosed);QCOMPARE(service.sessionCount(),1);
        options.transaction=TransactionAction::Commit;result=run({});QVERIFY(result.result.error.contains("回滚"));QCOMPARE(result.transactionState,TransactionState::Failed);options.transaction=TransactionAction::Rollback;result=run({});QCOMPARE(result.transactionState,TransactionState::Idle);options.transaction=TransactionAction::None;options.autoCommit=true;result=run("SELECT 17");QCOMPARE(result.result.rows[0][0].toInt(),17);QCOMPARE(service.sessionCount(),1);
    }
    void sourceDesignerPreservesViewOptionsAndRoutineIdentity(){
        using namespace navidog;auto db=connectDatabase(profile_);auto token=cancellation();CatalogNode node;node.kind="function";node.schema="public";node.database=profile_.database;node.object=node.name="source_owned";SourceDefinition empty;auto result=applySourceChanges(profile_,empty,node,sourceTemplate(profile_,node),false,token);QVERIFY2(result.applied,qPrintable(result.error));QVERIFY2(result.refreshError.isEmpty(),qPrintable(result.refreshError));QVERIFY(!result.definition.node.key.isEmpty());auto before=result.definition;auto sql=before.sql;sql.replace("RETURN p_value;","RETURN p_value+1;");result=applySourceChanges(profile_,before,before.node,sql,false,token);QVERIFY2(result.applied,qPrintable(result.error));QCOMPARE(db->execute("SELECT public.source_owned(8)",{},token).rows[0][0].toInt(),9);
        before=result.definition;sql=before.sql;sql.replace("p_value integer","p_value bigint");result=applySourceChanges(profile_,before,before.node,sql,false,token);QVERIFY(!result.applied);QVERIFY(result.rolledBack);QVERIFY(result.error.contains("签名"));QCOMPARE(db->execute("SELECT count(*) FROM pg_proc WHERE proname='source_owned'",{},token).rows[0][0].toInt(),1);
        node.kind="view";node.object=node.name="source_view_owned";node.key.clear();db->execute("CREATE VIEW public.source_view_owned WITH (security_barrier=true) AS SELECT 1 AS id",{},token);before=readSourceDefinition(*db,profile_,node,token);QVERIFY(before.sql.contains("security_barrier=true"));sql=before.sql;sql.replace("SELECT 1 AS id","SELECT 2 AS id");result=applySourceChanges(profile_,before,node,sql,false,token);QVERIFY2(result.applied,qPrintable(result.error));QVERIFY(result.definition.sql.contains("security_barrier=true"));QCOMPARE(db->execute("SELECT id FROM public.source_view_owned",{},token).rows[0][0].toInt(),2);
    }
    void cleanupTestCase() { if(started_) QVERIFY(command(pgCtl_,{"-D",dir_.filePath("data"),"-m","immediate","-w","stop"})); }
};
QTEST_GUILESS_MAIN(PostgresTest)
#include "postgres_test.moc"
