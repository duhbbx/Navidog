#include "navidog/background_tasks.hpp"
#include <QtTest>
#include <algorithm>
#include <QTemporaryDir>
#include <QFile>
#include <QBuffer>
#include <QProcess>
#include <QTimer>
#include <QUuid>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonArray>
#include "navidog/catalog.hpp"
#include "navidog/node_actions.hpp"
#include "navidog/database.hpp"
#include "navidog/database_tools.hpp"
#include "navidog/profile_store.hpp"
#include "navidog/connection_archive.hpp"
#include "navidog/query_service.hpp"
#include "navidog/redis_commands.hpp"
#include "navidog/document_commands.hpp"
#include "navidog/driver_packages.hpp"
#include "navidog/operation_timing.hpp"
#include "navidog/sql.hpp"
#include "navidog/table_design.hpp"
#include "navidog/data_edit.hpp"
#include "navidog/result_export.hpp"
#include "navidog/source_design.hpp"
#include "navidog/statement_templates.hpp"
#include "navidog/script_document.hpp"
#include "navidog/query_plan.hpp"
#include "navidog/table_actions.hpp"
#include "navidog/query_analysis.hpp"
#include "navidog/query_parameters.hpp"
#include "navidog/query_safety.hpp"
#include "navidog/sql/compiler/compiler.hpp"
#include "navidog/sql/compiler/conversion.hpp"
#include "navidog/sql/compiler/formatter.hpp"
#include "navidog/sql/compiler/script_context.hpp"
#include "navidog/sql/optimizer/optimizer.hpp"
#include "navidog/sql/optimizer/interpreter.hpp"
#include "navidog/metadata_cache.hpp"
#include "navidog/namespace_design.hpp"
#include "navidog/credentials.hpp"
#include "navidog/secret_store.hpp"
#include "navidog/local_store.hpp"

class CoreTest : public QObject {
    Q_OBJECT
    QTemporaryDir dir_;
    navidog::ConnectionProfile profile_;
private slots:
    void dangerousSqlRecognizesAllRelationalDriversAndCommandEngines() {
        using namespace navidog;
        using K = DatabaseKind;
        QuerySafetyPolicy policy;
        for (const auto kind : {K::SQLite, K::MySQL, K::MariaDB, K::PostgreSQL, K::Oracle,
                K::OceanBase, K::OceanBaseOracle, K::SQLServer, K::TiDB, K::Dameng, K::H2,
                K::Kingbase, K::UxDb, K::PolarDbMySql, K::PolarDbPostgres, K::GaussDb,
                K::Db2, K::DuckDb, K::ClickHouse, K::OpenGauss, K::Doris,
                K::StarRocks, K::CockroachDb, K::YugabyteDb}) {
            auto findings = inspectQuerySafety(kind,
                "SELECT 'UPDATE t SET n=1';\nTRUNCATE TABLE t;\nDELETE FROM t;\nUPDATE t SET n=1 WHERE (1 = 1 OR n = 0);", policy);
            QCOMPARE(findings.size(), 3);
            QCOMPARE(findings[0].index, 1);
            QCOMPARE(findings[0].line, 2);
            QCOMPARE(findings[0].operation, QString("TRUNCATE"));
            QCOMPARE(findings[1].risk, QueryRisk::NoFilter);
            QCOMPARE(findings[2].risk, QueryRisk::AlwaysTrue);
            QCOMPARE(findings[2].line, 4);
            findings = inspectQuerySafety(kind, "DELETE FROM t WHERE id = 7", policy);
            QCOMPARE(findings.size(), 1);
            QCOMPARE(findings.front().impact.status, ImpactStatus::Pending);
        }
        auto findings = inspectQuerySafety(K::MySQL, "SELECT * FROM t FOR UPDATE;", policy);
        QVERIFY(findings.isEmpty());
        findings = inspectQuerySafety(K::MySQL, "DELETE FROM t WHERE 1", policy);
        QCOMPARE(findings.front().risk, QueryRisk::AlwaysTrue);
        QVERIFY(!inspectQuerySafety(K::MySQL, "/*! DELETE FROM t */ WHERE id=1", policy).isEmpty());
        findings = inspectQuerySafety(K::PostgreSQL, "WITH deleted AS (DELETE FROM t RETURNING id) SELECT * FROM deleted", policy);
        QVERIFY(!findings.isEmpty() && findings.front().risk == QueryRisk::Unverified);
        findings = inspectQuerySafety(K::Redis, "# fixture\n  FLUSHDB", policy);
        QCOMPARE(findings.size(), 1);
        QCOMPARE(findings.front().begin, 12);
        QCOMPARE(findings.front().matchCommand, QString("DBSIZE"));
        findings = inspectQuerySafety(K::MongoDb, R"({"delete":"items","deletes":[{"q":{},"limit":0}]})", policy);
        QCOMPARE(findings.size(), 1);
        QCOMPARE(findings.front().risk, QueryRisk::NoFilter);
        QVERIFY(findings.front().matchCommand.startsWith("{\"count\":\"items\""));
    }

    void queryImpactReadsOwnedRowsAndRebindsOnlyWhereValues() {
        using namespace navidog;
        QTemporaryDir directory;
        ConnectionProfile profile;
        profile.database = ensureDemoDatabase(directory.path());
        auto db = connectDatabase(profile);
        const auto token = cancellation();
        QVERIFY(db->execute("CREATE TABLE owned_impact(id INTEGER PRIMARY KEY, state TEXT)", {}, token).error.isEmpty());
        QVERIFY(db->execute("INSERT INTO owned_impact VALUES (1,'active'),(2,'active'),(3,'active')", {}, token).error.isEmpty());
        QuerySafetyPolicy policy;
        const QString sql = "UPDATE owned_impact SET state = :replacement WHERE ((id >= :lower) AND (state = :filter));";
        const auto bindings = bindQueryParameters(inspectQueryParameters(DatabaseKind::SQLite, sql, {}, {}),
                                                  {QString("changed"), 1, QString("active")});
        auto findings = inspectQuerySafety(profile.driver, sql, policy);
        QCOMPARE(findings.size(), 1);
        QCOMPARE(findings.front().risk, QueryRisk::None);
        QCOMPARE(findings.front().impact.status, ImpactStatus::Pending);
        measureQueryImpact(profile, {}, policy, bindings, findings, token);
        const auto full = findings.front().impact;
        QCOMPARE(full.status, ImpactStatus::Measured);
        QCOMPARE(full.matched, 3);
        QCOMPARE(full.total, 3);
        QVERIFY(requiresQueryConfirmation(findings.front(), policy));
        const auto rows = db->execute("SELECT COUNT(*) FROM owned_impact WHERE state='active'", {}, token).rows;
        QCOMPARE(rows.front().front().toInt(), 3);

        findings = inspectQuerySafety(profile.driver, "DELETE FROM owned_impact WHERE id = 2", policy);
        measureQueryImpact(profile, {}, policy, {}, findings, token);
        QCOMPARE(findings.front().impact.matched, 1);
        QCOMPARE(findings.front().impact.total, 3);
        QVERIFY(!requiresQueryConfirmation(findings.front(), policy));
        policy.rowThreshold = 2;
        findings = inspectQuerySafety(profile.driver, "DELETE FROM owned_impact WHERE id >= 1", policy);
        measureQueryImpact(profile, {}, policy, {}, findings, token);
        QCOMPARE(findings.front().impact.matched, 2);
        QVERIFY(findings.front().impact.atLeast);
        QVERIFY(requiresQueryConfirmation(findings.front(), policy));

        findings = inspectQuerySafety(profile.driver, "DELETE FROM absent_table WHERE id >= 1", policy);
        measureQueryImpact(profile, {}, policy, {}, findings, token);
        QCOMPARE(findings.front().impact.status, ImpactStatus::Unavailable);
        QVERIFY(!requiresQueryConfirmation(findings.front(), policy));
        policy.confirmUnmeasured = true;
        QVERIFY(requiresQueryConfirmation(findings.front(), policy));
        policy.enabled = false;
        QVERIFY(inspectQuerySafety(profile.driver, sql, policy).isEmpty());
    }

    void objectActionsAreGatedByObjectAndDialect() {
        using namespace navidog;
        const auto pg = DatabaseKind::PostgreSQL;
        const auto oracle = DatabaseKind::Oracle;
        QVERIFY(nodeActions("schema", true, pg).contains(NodeAction::NewSourceList));
        QVERIFY(nodeActions("schema", true, pg).contains(NodeAction::DropCascade));
        QVERIFY(nodeActions("materializedViews", true, pg).contains(NodeAction::NewSource));
        QVERIFY(nodeActions("trigger", true, oracle).contains(NodeAction::CompileObject));
        QVERIFY(nodeActions("procedure", true, oracle).contains(NodeAction::ObjectProperties));
        QVERIFY(!nodeActions("procedure", true, oracle).contains(NodeAction::DropCascade));
        QVERIFY(!nodeActions("procedure", true, DatabaseKind::MySQL).contains(NodeAction::CompileObject));
        QVERIFY(!nodeActions("table", true, DatabaseKind::SQLite).contains(NodeAction::DropCascade));
    }

    void batchObjectSqlAndDropsRespectScopeAndRollback() {
        using namespace navidog;
        QTemporaryDir directory;
        ConnectionProfile profile;
        profile.database=ensureDemoDatabase(directory.path());
        auto db=connectDatabase(profile);
        const auto token=cancellation();
        QVERIFY(db->execute("CREATE TABLE batch_a(id INTEGER PRIMARY KEY)",{},token).error.isEmpty());
        QVERIFY(db->execute("CREATE TABLE batch_b(id INTEGER PRIMARY KEY)",{},token).error.isEmpty());
        CatalogNode a{"table","batch_a",profile.database,"main","batch_a"};
        CatalogNode b{"table","batch_b",profile.database,"main","batch_b"};
        CatalogNode column{"column","id",profile.database,"main","batch_a"};
        const auto renamed=changeCatalogObject(profile,column,CatalogObjectChange::Rename,token,"entry_id");
        QVERIFY2(renamed.applied,qPrintable(renamed.error));
        QBuffer output;
        QVERIFY(output.open(QIODevice::WriteOnly));
        writeCatalogBatchSql(profile,{a,b},StatementTemplate::Select,output,{token,{}});
        QVERIFY(output.data().contains("entry_id") && output.data().contains("batch_b"));
        QBuffer exported;
        QVERIFY(exported.open(QIODevice::WriteOnly));
        writeCatalogBatchSql(profile,{a,b},{},exported,{token,{}});
        QVERIFY(exported.data().contains("CREATE TABLE batch") || exported.data().contains("CREATE TABLE \"batch"));
        auto missing=b;
        missing.object=missing.name="absent_owned_batch";
        const auto partial=dropCatalogBatch(profile,{a,missing},false,{token,{}});
        QVERIFY(partial.rolledBack);
        QVERIFY(partial.removed.isEmpty());
        QVERIFY(!partial.error.isEmpty());
        QCOMPARE(db->execute("SELECT name FROM sqlite_schema WHERE name='batch_a'",{},token).rows.size(),1);
        auto foreign=b;
        foreign.schema="other";
        QVERIFY_EXCEPTION_THROWN(catalogBatchDropSql(profile,{a,foreign},false),std::runtime_error);
        const auto removed=dropCatalogBatch(profile,{a,b},false,{token,{}});
        QVERIFY2(removed.error.isEmpty(),qPrintable(removed.error));
        QCOMPARE(removed.removed.size(),2);
        QVERIFY(db->execute("SELECT name FROM sqlite_schema WHERE name IN ('batch_a','batch_b')",{},token).rows.isEmpty());
    }

    void queryParametersRespectDialectAndSourceRanges() {
        using namespace navidog;
        using Type = QueryParameterType;
        auto plan = inspectQueryParameters(DatabaseKind::PostgreSQL,
            R"(SELECT $4::integer, $4, '$5', $$ $6 $$, '{"a":1}'::jsonb ? 'a';)" "\n"
            "PREPARE p AS SELECT $1::int;\nSELECT CAST($8 AS numeric) /* $6 */", {}, {});
        QCOMPARE(plan.parameters.size(), 2);
        QCOMPARE(plan.statements.size(), 2);
        QCOMPARE(plan.statements[1].index, 2);
        QCOMPARE(plan.parameters[0].type, Type::Integer);
        QCOMPARE(plan.parameters[1].type, Type::ExactNumber);
        QCOMPARE(plan.parameters[0].locations.size(), 2);
        QCOMPARE(plan.parameters[1].locations[0].line, 3);
        QVERIFY(plan.statements[0].sql.contains("SELECT $1::integer, $1"));
        QVERIFY(plan.statements[0].sql.contains("jsonb ? 'a'"));
        QCOMPARE(plan.statements[0].parameters.size(), 1);

        plan = inspectQueryParameters(DatabaseKind::SQLite,
            "SELECT :label, ?1, ?47, ?, ?48, '[?]', @label, $var::suffix(text);"
            " SELECT :label", {}, {});
        QCOMPARE(plan.parameters.size(), 5);
        QCOMPARE(plan.parameters[0].locations.size(), 3);
        QCOMPARE(plan.parameters[2].locations.size(), 2);
        QCOMPARE(plan.statements[0].parameters, QVector<int>({0, 0, 1, 2, 2, 3, 4}));

        plan = inspectQueryParameters(DatabaseKind::MySQL,
            "SELECT @session, @@version, '?', CAST(? AS SIGNED); -- ?\nSELECT 2", {}, {});
        QCOMPARE(plan.parameters.size(), 1);
        QCOMPARE(plan.parameters[0].type, Type::Integer);

        plan = inspectQueryParameters(DatabaseKind::Oracle,
            "SELECT q'[ :ignored ]', CAST(:value AS DATE), :VALUE FROM dual;\n"
            "CREATE TRIGGER sample BEFORE INSERT ON t FOR EACH ROW BEGIN :NEW.id := 1; END;\n/", {}, {});
        QCOMPARE(plan.parameters.size(), 1);
        QCOMPARE(plan.parameters[0].locations.size(), 2);
        QCOMPARE(plan.parameters[0].type, Type::Timestamp);

        plan = inspectQueryParameters(DatabaseKind::SQLServer,
            "SELECT @variable, CAST(? AS bigint), [?], N'?'; -- ?", {}, {});
        QCOMPARE(plan.parameters.size(), 1);
        QCOMPARE(plan.parameters[0].type, Type::Integer);

        QVERIFY(!parseQueryParameterValue(Type::Integer, "1.2", false));
        QVERIFY(!parseQueryParameterValue(Type::Date, "2025-02-29", false));
        const auto empty = parseQueryParameterValue(Type::Text, {}, false);
        QVERIFY(empty && !empty->isNull());
        const auto missing = parseQueryParameterValue(Type::Unknown, {}, true);
        QVERIFY(missing && missing->isNull());
        QCOMPARE(parseQueryParameterValue(Type::ExactNumber, "123456789.0123456789", false)->toString(),
                 QString("123456789.0123456789"));
    }

    void queryParametersUseCachedColumnsAndBindInBatch() {
        using namespace navidog;
        auto metadata = std::make_shared<MetadataSnapshot>();
        metadata->driver = DatabaseKind::SQLite;
        metadata->schema = "main";
        metadata->searchPath = QStringList{"main"};
        metadata->verified = true;
        metadata->refreshedMs = QDateTime::currentMSecsSinceEpoch();
        metadata->tablesComplete = metadata->columnsComplete = true;
        MetadataObject object;
        object.name = object.key = "bindings_demo";
        object.kind = "table";
        object.schema = "main";
        object.columns = {{"id", "INTEGER", {}, 1}, {"label", "TEXT", {}, 2}};
        metadata->objects << object;
        prepareMetadataTypeFacts(*metadata);
        const QString sql =
            "CREATE TABLE bindings_demo(id INTEGER, label TEXT); "
            "INSERT INTO bindings_demo(id,label) VALUES(:id,:value); "
            "SELECT id,label,?9 AS empty_value FROM bindings_demo WHERE id=:id;";
        const auto plan = inspectQueryParameters(DatabaseKind::SQLite, sql, {}, "main", metadata);
        QCOMPARE(plan.parameters.size(), 3);
        QCOMPARE(plan.parameters[0].type, QueryParameterType::Integer);
        QCOMPARE(plan.parameters[1].type, QueryParameterType::Text);
        QTemporaryDir directory;
        ConnectionProfile p;
        p.database = ensureDemoDatabase(directory.path());
        QueryExecutionOptions options;
        options.sessionId = "bind-test";
        const QString label = "O'Reilly; 中文";
        options.bindings = bindQueryParameters(plan, {qlonglong(19), label, QVariant{}});
        QueryService service;
        QSignalSpy finished(&service, &QueryService::completed);
        QVERIFY(service.run(p, sql, {}, options));
        QVERIFY(finished.wait(5000));
        const auto result = qvariant_cast<QueryOutcome>(finished.takeFirst().front());
        QVERIFY2(result.result.error.isEmpty(), qPrintable(result.result.error));
        QCOMPARE(result.executedStatements, 3);
        QCOMPARE(result.result.rows.size(), 1);
        QCOMPARE(result.result.rows[0][0].toInt(), 19);
        QCOMPARE(result.result.rows[0][1].toString(), label);
        QVERIFY(result.result.rows[0][2].isNull());
        QVERIFY(!result.statements.join(' ').contains(label));
    }

    void extendedDialectAndDocumentSyntax() {
        namespace sc = navidog::sql::compiler;
        sc::LanguageOptions clickhouse;
        clickhouse.extension = sc::SqlExtension::ClickHouse;
        const auto click = sc::compile(sc::Dialect::MySQL, "SELECT id FROM events PREWHERE id > 1 WHERE id < 10", clickhouse);
        QVERIFY(click->syntaxValid);
        QVERIFY(click->statements.front().prewhere);
        QVERIFY(click->model.queries.front().prewhere);
        QVERIFY(!sc::compile(sc::Dialect::MySQL, "SELECT id FROM events PREWHERE id > 1")->syntaxValid);
        sc::LanguageOptions duckdb;
        duckdb.extension = sc::SqlExtension::DuckDb;
        const auto duck = sc::compile(sc::Dialect::PostgreSQL,
            "SELECT row_number() OVER (ORDER BY id) AS rn FROM events QUALIFY rn = 1", duckdb);
        QVERIFY(duck->syntaxValid);
        QVERIFY(duck->statements.front().qualify);
        QVERIFY(duck->model.queries.front().qualify);
        const auto command = navidog::parseDocumentCommand(navidog::mongoCatalogCommand("find", "orders\"2026"));
        QCOMPARE(command.name, "find");
        QVERIFY2(command.error.isEmpty(), qPrintable(command.error));
        QCOMPARE(command.options.value("find").toString(), "orders\"2026");
        QVERIFY(!navidog::parseDocumentCommand(R"({"find":"orders","filter":[]})").error.isEmpty());
        const auto drop = navidog::parseDocumentCommand(navidog::mongoCommandDraft("dropIndexes","a\"b","x\\y"));
        QVERIFY(drop.error.isEmpty());
        QCOMPARE(drop.name,"dropIndexes");
        QCOMPARE(drop.options.value("dropIndexes").toString(),"a\"b");
        QCOMPARE(drop.options.value("index").toString(),"x\\y");
    }

    void extendedPlanPreservesTreeAndFragments() {
        using namespace navidog;
        QueryResult cockroach;
        cockroach.columns = QStringList{"info"};
        cockroach.rows = {{"• limit"}, {"│ count: 1"}, {"└── • hash join"},
                          {"    ├── • scan"}, {"    └── • scan"}};
        const auto tree = decodeEstimatedPlan(DatabaseKind::CockroachDb,cockroach);
        QVERIFY2(tree.complete,qPrintable(tree.warning));
        QCOMPARE(tree.roots.size(),1);
        QCOMPARE(tree.nodes[0].children.size(),1);
        QCOMPARE(tree.nodes[1].children.size(),2);
        QCOMPARE(tree.nodes[0].properties.value("count"),"1");
        QueryResult doris;
        doris.columns = QStringList{"Explain String"};
        doris.rows = {{"PLAN FRAGMENT 0\n  1:VAGGREGATE\n  0:VOlapScanNode"}};
        const auto fragment = decodeEstimatedPlan(DatabaseKind::Doris,doris);
        QVERIFY2(fragment.complete,qPrintable(fragment.warning));
        QCOMPARE(fragment.nodes[0].children.size(),2);
        QVERIFY(fragment.raw.contains("0:VOlapScanNode"));
        doris.cellTruncated = true;
        QVERIFY(!decodeEstimatedPlan(DatabaseKind::Doris,doris).complete);
    }

    void extendedDriverFixture_data() {
        QTest::addColumn<int>("kindValue");
        QTest::addColumn<QString>("environment");
        using navidog::DatabaseKind;
        QTest::newRow("duckdb") << int(DatabaseKind::DuckDb) << QString("NAVIDOG_TEST_DUCKDB_FILE");
        QTest::newRow("clickhouse") << int(DatabaseKind::ClickHouse) << QString("NAVIDOG_TEST_CLICKHOUSE_PORT");
        QTest::newRow("mongodb") << int(DatabaseKind::MongoDb) << QString("NAVIDOG_TEST_MONGODB_PORT");
        QTest::newRow("cockroachdb") << int(DatabaseKind::CockroachDb) << QString("NAVIDOG_TEST_COCKROACH_PORT");
        QTest::newRow("yugabytedb") << int(DatabaseKind::YugabyteDb) << QString("NAVIDOG_TEST_YUGABYTE_PORT");
        QTest::newRow("doris") << int(DatabaseKind::Doris) << QString("NAVIDOG_TEST_DORIS_PORT");
        QTest::newRow("starrocks") << int(DatabaseKind::StarRocks) << QString("NAVIDOG_TEST_STARROCKS_PORT");
        QTest::newRow("opengauss") << int(DatabaseKind::OpenGauss) << QString("NAVIDOG_TEST_OPENGAUSS_PORT");
    }

    void extendedDriverFixture() {
        using namespace navidog;
        QFETCH(int, kindValue);
        QFETCH(QString, environment);
        const auto endpoint = qEnvironmentVariable(environment.toLatin1().constData());
        if (endpoint.isEmpty()) QSKIP("Set the isolated fixture endpoint to run this integration test");
        ConnectionProfile profile;
        profile.driver = static_cast<DatabaseKind>(kindValue);
        profile.id = "extended-fixture";
        profile.host = "127.0.0.1";
        profile.port = endpoint.toInt();
        profile.credentialSource = CredentialSource::Passwordless;
        profile.tls = false;
        profile.advanced.readTimeout = 10;
        const bool mongo = profile.driver == DatabaseKind::MongoDb;
        const bool analytical = isAnalyticalMySql(profile.driver);
        const bool openGauss = profile.driver == DatabaseKind::OpenGauss;
        if (!mongo) profile.user = profile.driver == DatabaseKind::ClickHouse ? "default" : "root";
        if (profile.driver == DatabaseKind::DuckDb) profile.database = endpoint;
        else if (mongo || analytical) profile.database = "navidog_fixture";
        else if (profile.driver == DatabaseKind::YugabyteDb) profile.database = profile.user = "yugabyte";
        else if (openGauss) {
            profile.database = "postgres";
            profile.user = qEnvironmentVariable("NAVIDOG_TEST_OPENGAUSS_USER");
            profile.password = qEnvironmentVariable("NAVIDOG_TEST_OPENGAUSS_PASSWORD");
            profile.jdbcDriverJar = qEnvironmentVariable("NAVIDOG_TEST_OPENGAUSS_JAR");
            profile.credentialSource = CredentialSource::Prompt;
            profile.secretsLoaded = true;
        }
        else profile.database = profile.driver == DatabaseKind::CockroachDb ? "defaultdb" : "default";
        if (analytical) {
            auto root = profile;
            root.database.clear();
            try {
                auto db = connectDatabase(root);
                const auto created = db->execute("CREATE DATABASE IF NOT EXISTS navidog_fixture", {}, cancellation());
                QVERIFY2(created.error.isEmpty(), qPrintable(created.error));
            } catch (const std::exception& error) {
                QFAIL(error.what());
            }
        }
        QueryService service;
        QSignalSpy tested(&service, &QueryService::connectionTestCompleted);
        QVERIFY(service.testConnection(profile, "fixture"));
        QVERIFY(tested.wait(10000));
        const auto test = tested.takeFirst().at(1).value<QueryOutcome>();
        QVERIFY2(test.result.error.isEmpty(), qPrintable(test.result.error));
        QVERIFY(!test.serverVersion.isEmpty());
        try {
            auto db = connectDatabase(profile);
            const auto token = cancellation();
            if (mongo) {
                const auto inserted = db->execute(R"({"insert":"navidog_check","documents":[{"n":1},{"n":2}]})", {}, token);
                QVERIFY2(inserted.error.isEmpty(), qPrintable(inserted.error));
                QCOMPARE(inserted.affectedRows, 2);
                const auto duplicateId = QUuid::createUuid().toString(QUuid::Id128);
                const auto duplicate = db->execute(QString(
                    "{\"insert\":\"navidog_unique\",\"documents\":[{\"_id\":\"%1\"},{\"_id\":\"%1\"}]}")
                    .arg(duplicateId), {}, token);
                QVERIFY(!duplicate.error.isEmpty());
                QCOMPARE(duplicate.affectedRows, 1);
                CatalogNode database;
                database.kind = "database";
                database.database = profile.database;
                const auto collection = loadCatalog(*db, profile, database, token);
                QVERIFY2(collection.error.isEmpty(), qPrintable(collection.error));
                QVERIFY(std::any_of(collection.nodes.begin(),collection.nodes.end(),
                    [](const auto& node) { return node.kind == "mongoCollection" && node.name == "navidog_check"; }));
                const auto docs = db->execute(R"({"find":"navidog_check","batchSize":1,"limit":2})", {}, token);
                QVERIFY2(docs.error.isEmpty(), qPrintable(docs.error));
                QCOMPARE(docs.rows.size(), 2);
                database.kind = "mongoCollection";
                database.object = "navidog_check";
                const auto indexes = loadCatalog(*db, profile, database, token);
                QVERIFY2(indexes.error.isEmpty(), qPrintable(indexes.error));
                QCOMPARE(indexes.nodes.first().name, "_id_");
                QVERIFY(nodeActions(database.kind, true, profile.driver).contains(NodeAction::Preview));
                const auto collectionInfo = db->execute(definitionSql(profile,database), {}, token);
                QVERIFY2(collectionInfo.error.isEmpty(), qPrintable(collectionInfo.error));
                QCOMPARE(collectionInfo.rows.size(), 1);
                QVERIFY(collectionInfo.rows.first().first().toString().contains("navidog_check"));
            } else {
                const auto row = db->execute("SELECT 42 AS n", {}, token);
                QVERIFY2(row.error.isEmpty(), qPrintable(row.error));
                QCOMPARE(row.rows.at(0).at(0).toInt(), 42);
                const bool clickhouse = profile.driver == DatabaseKind::ClickHouse;
                const bool distributed = isDistributedPostgres(profile.driver);
                const QString tableName = distributed ? "navidog_metadata_check" : "navidog_check";
                QString create = "CREATE TABLE IF NOT EXISTS " + tableName +
                    (distributed ? " (id INTEGER PRIMARY KEY)" : " (id INTEGER)");
                if (clickhouse) create += " ENGINE = Memory";
                if (analytical) create += R"( DUPLICATE KEY(id) DISTRIBUTED BY HASH(id) BUCKETS 1 PROPERTIES ("replication_num"="1"))";
                const auto created = db->execute(create, {}, token);
                QVERIFY2(created.error.isEmpty(), qPrintable(created.error));
                CatalogNode scope;
                scope.database = profile.database;
                scope.schema = profile.driver == DatabaseKind::DuckDb ? "main" : "public";
                if (openGauss) {
                    const auto current = db->execute("SELECT CURRENT_SCHEMA", {}, token);
                    QVERIFY2(current.error.isEmpty(), qPrintable(current.error));
                    scope.schema = current.rows.value(0).value(0).toString();
                    QVERIFY(!scope.schema.isEmpty());
                }
                scope.kind = "tables";
                const auto tables = loadCatalog(*db, profile, scope, token);
                QVERIFY2(tables.error.isEmpty(), qPrintable(tables.error));
                const auto table = std::find_if(tables.nodes.begin(), tables.nodes.end(),
                    [&](const auto& node) { return node.name == tableName; });
                QVERIFY(table != tables.nodes.end());
                const auto actions = nodeActions(table->kind, true, profile.driver);
                QVERIFY(actions.contains(NodeAction::Preview));
                QVERIFY(actions.contains(NodeAction::Definition));
                const auto preview = db->execute(previewSql(profile,*table), {}, token);
                QVERIFY2(preview.error.isEmpty(), qPrintable(preview.error));
                const auto definition = db->execute(definitionSql(profile,*table), {}, token);
                QVERIFY2(definition.error.isEmpty(), qPrintable(definition.error));
                QVERIFY(!definition.rows.isEmpty());
                if (profile.driver == DatabaseKind::DuckDb)
                    QVERIFY(definition.rows.first().first().toString().startsWith("CREATE TABLE"));
                auto columns = *table;
                columns.kind = "columns";
                const auto fields = loadCatalog(*db, profile, columns, token);
                QVERIFY2(fields.error.isEmpty(), qPrintable(fields.error));
                QVERIFY(!fields.nodes.isEmpty());
                QCOMPARE(fields.nodes.first().name, "id");
                auto indexFolder = *table;
                indexFolder.kind = "indexes";
                if (!clickhouse && !analytical && !distributed) {
                    const auto index = db->execute("CREATE INDEX IF NOT EXISTS navidog_idx ON " +
                        tableQualifiedName(profile,*table) + " (id)", {}, token);
                    QVERIFY2(index.error.isEmpty(), qPrintable(index.error));
                }
                const auto indexes = loadCatalog(*db, profile, indexFolder, token);
                QVERIFY2(indexes.error.isEmpty(), qPrintable(indexes.error));
                if (!clickhouse && !analytical) {
                    const auto index = std::find_if(indexes.nodes.cbegin(), indexes.nodes.cend(),
                        [&](const auto& node) { return node.name == (distributed ? tableName + "_pkey" : QString("navidog_idx")); });
                    QVERIFY(index != indexes.nodes.cend());
                    const auto indexDdl = db->execute(definitionSql(profile,*index), {}, token);
                    QVERIFY2(indexDdl.error.isEmpty(), qPrintable(indexDdl.error));
                    QVERIFY(!indexDdl.rows.isEmpty());
                }
                if (supportsNamespace(profile.driver, NamespaceKind::Schema)) {
                    NamespaceDraft draft;
                    draft.kind = NamespaceKind::Schema;
                    draft.name = "navidog_namespace_check";
                    const auto namespaceResult = createNamespace(profile, draft, token);
                    QVERIFY2(namespaceResult.applied, qPrintable(namespaceResult.error));
                    const auto removed = dropNamespace(profile, draft.kind, draft.name, token);
                    QVERIFY2(removed.applied, qPrintable(removed.error));
                } else if (clickhouse || analytical) {
                    NamespaceDraft draft;
                    draft.kind = NamespaceKind::Database;
                    draft.name = "navidog_namespace_check";
                    const auto namespaceResult = createNamespace(profile,draft,token);
                    QVERIFY2(namespaceResult.applied,qPrintable(namespaceResult.error));
                    CatalogNode database;
                    database.kind = "database";
                    database.database = draft.name;
                    const auto properties = db->execute(definitionSql(profile,database),{},token);
                    QVERIFY2(properties.error.isEmpty(),qPrintable(properties.error));
                    QCOMPARE(properties.rows.size(),1);
                    const auto removed = dropNamespace(profile,draft.kind,draft.name,token);
                    QVERIFY2(removed.applied,qPrintable(removed.error));
                }
                if (distributed) {
                    auto constraintsFolder = *table;
                    constraintsFolder.kind = "constraints";
                    const auto constraints = loadCatalog(*db,profile,constraintsFolder,token);
                    QVERIFY2(constraints.error.isEmpty(),qPrintable(constraints.error));
                    QVERIFY(!constraints.nodes.isEmpty());
                    const auto constraint = db->execute(definitionSql(profile,constraints.nodes.first()),{},token);
                    QVERIFY2(constraint.error.isEmpty(),qPrintable(constraint.error));
                    QVERIFY(!constraint.rows.isEmpty());
                    const auto setup = db->execute("CREATE TABLE IF NOT EXISTS navidog_edit_check (id INTEGER PRIMARY KEY, n INTEGER)", {}, token);
                    QVERIFY2(setup.error.isEmpty(), qPrintable(setup.error));
                    QVERIFY(db->execute("INSERT INTO navidog_edit_check VALUES (1, 2) ON CONFLICT (id) DO UPDATE SET n=2", {}, token).error.isEmpty());
                    DataPageRequest request;
                    request.node = *table;
                    request.node.object = request.node.name = "navidog_edit_check";
                    const auto page = readDataPage(profile,request,token);
                    QVERIFY2(page.error.isEmpty() && page.editable, qPrintable(page.error + page.readOnlyReason));
                    DataRowChange edit;
                    edit.state = DataRowState::Modified;
                    edit.values = edit.original = page.result.rows.first();
                    edit.values[1] = 9;
                    edit.touched.insert(1);
                    const auto saved = commitDataChanges(profile,page.schema,{edit},token);
                    QVERIFY2(saved.error.isEmpty(),qPrintable(saved.error));
                    QCOMPARE(saved.updated,1);
                    const auto stale = commitDataChanges(profile,page.schema,{edit},token);
                    QVERIFY(!stale.error.isEmpty());
                    QVERIFY(stale.rolledBack);
                    QVERIFY(db->execute("DROP TABLE navidog_edit_check", {}, token).error.isEmpty());
                }
            }
            const auto catalog = loadCatalog(*db, profile, {}, token);
            QVERIFY2(catalog.error.isEmpty(), qPrintable(catalog.error));
            QVERIFY(!catalog.nodes.isEmpty());
            QSignalSpy completed(&service, &QueryService::completed);
            QVERIFY(service.run(profile, mongo ? mongoCatalogCommand("find", "navidog_check") : "SELECT 42 AS n", {}));
            QVERIFY(completed.wait(10000));
            const auto outcome = completed.takeFirst().at(0).value<QueryOutcome>();
            QVERIFY2(outcome.result.error.isEmpty(), qPrintable(outcome.result.error));
            QVERIFY(!outcome.result.rows.isEmpty());
            QueryExecutionOptions options;
            options.explain = true;
            QVERIFY(service.run(profile, mongo ? mongoCatalogCommand("find", "navidog_check") :
                "SELECT 42 AS n", {}, options));
            QVERIFY(completed.wait(10000));
            const auto explained = completed.takeFirst().at(0).value<QueryOutcome>();
            QVERIFY2(explained.result.error.isEmpty(), qPrintable(explained.result.error));
            QVERIFY(explained.estimatedPlan.request.issued && explained.estimatedPlan.request.received);
            QVERIFY2(explained.estimatedPlan.complete, qPrintable(explained.estimatedPlan.warning +
                "\n" + QString::fromUtf8(explained.estimatedPlan.raw).left(1200)));
        } catch (const std::exception& error) {
            QFAIL(error.what());
        }
    }

    void driverReleaseAssetsRequireExactVersionPlatformAndDigest() {
        using namespace navidog;
        const auto jar = driverDownload(DriverPackage::H2);
        QCOMPARE(jar.url.host(), "repo.maven.apache.org");
        QCOMPARE(jar.sha256.size(), 32);
        const auto archive = driverDownload(DriverPackage::Java);
        if (archive.asset.isEmpty()) return;
        const auto tag = QStringLiteral("v" NAVIDOG_VERSION);
        const auto url = "https://github.com/duhbbx/Navidog/releases/download/" + tag + '/' + archive.asset;
        const auto parse = [&](QString assetUrl, QString digest, qint64 bytes, bool draft = false) {
            const auto item = QJsonObject{{"name",archive.asset},{"state","uploaded"},{"size",double(bytes)},
                                          {"digest",digest},{"browser_download_url",assetUrl}};
            const auto release = QJsonObject{{"tag_name",tag},{"draft",draft},{"assets",QJsonArray{item}}};
            return githubDriverAsset(archive, QJsonDocument(release).toJson(QJsonDocument::Compact));
        };
        const auto hash = "sha256:" + QString(64, '1');
        const auto found = parse(url, hash, 1024);
        QCOMPARE(found.url, QUrl(url));
        QCOMPARE(found.maxBytes, 1024);
        QCOMPARE(found.sha256.size(), 32);
        QVERIFY(parse("https://example.org/package", hash, 1024).url.isEmpty());
        QVERIFY(parse(url, "unchecked", 1024).url.isEmpty());
        QVERIFY(parse(url, hash, archive.maxBytes + 1).url.isEmpty());
        QVERIFY(parse(url, hash, 1024, true).url.isEmpty());
    }

    void redisCommandsAndNativeFixture() {
        using namespace navidog;
        const auto binary = QByteArray::fromHex("00ff0a22205c");
        const auto command = parseRedisCommand("SET key " + quoteRedisArgument(binary));
        QVERIFY2(command.error.isEmpty(), qPrintable(command.error));
        QCOMPARE(command.arguments.at(2), binary);
        QVERIFY(!parseRedisCommand("GET").error.isEmpty());
        QVERIFY(!parseRedisCommand("GET \"unterminated").error.isEmpty());
        QVERIFY(parseRedisCommand("# test comment").arguments.isEmpty());
        const auto port = qEnvironmentVariableIntValue("NAVIDOG_REDIS_TEST_PORT");
        if (!port) return;
        try {
            ConnectionProfile profile;
            profile.driver = DatabaseKind::Redis;
            profile.tls = false;
            profile.id = "redis-fixture";
            profile.host = "127.0.0.1";
            profile.port = port;
            profile.database = "2";
            profile.credentialSource = CredentialSource::Passwordless;
            auto db = connectDatabase(profile);
            const auto token = cancellation();
            QVERIFY(versionBelongsTo(detectVersion(profile.driver, db->serverVersion()), profile.driver));
            const auto key = QByteArray("key ") + binary;
            const auto turnedOn = db->execute("SET " + quoteRedisArgument(key) + " hello", {}, token);
            QVERIFY2(turnedOn.error.isEmpty(), qPrintable(turnedOn.error));
            CatalogNode folder;
            folder.kind = "redisKeys";
            const auto keys = loadCatalog(*db, profile, folder, token);
            QCOMPARE(keys.nodes.size(), 1);
            QCOMPARE(QByteArray::fromBase64(keys.nodes.first().key.toLatin1()), key);
            const auto preview = db->redisPreview(key, {}, token);
            QVERIFY2(preview.error.isEmpty(), qPrintable(preview.error));
            QCOMPARE(preview.rows.first().last().toString(), "hello");
            QVERIFY(db->execute("HGET " + quoteRedisArgument(key) + " field", {}, token).error.contains("WRONGTYPE"));
            QueryService service;
            QSignalSpy completed(&service, &QueryService::completed);
            QVERIFY(service.run(profile, "PING\nGET " + quoteRedisArgument(key), {}));
            QVERIFY(completed.wait(5000));
            const auto outcome = completed.takeFirst().at(0).value<QueryOutcome>();
            QVERIFY2(outcome.result.error.isEmpty(), qPrintable(outcome.result.error));
            QCOMPARE(outcome.results.size(), 2);
            QCOMPARE(outcome.results.first().rows.first().last().toString(), "PONG");
            const QuerySafetyPolicy policy;
            auto reset = inspectQuerySafety(DatabaseKind::Redis, "FLUSHDB", policy);
            QCOMPARE(reset.size(), 1);
            measureQueryImpact(profile, {}, policy, {}, reset, token);
            QCOMPARE(reset.front().impact.status, ImpactStatus::Measured);
            QCOMPARE(reset.front().impact.matched, 1);
            QVERIFY(requiresQueryConfirmation(reset.front(), policy));
            auto all = inspectQuerySafety(DatabaseKind::Redis, "FLUSHALL", policy);
            measureQueryImpact(profile, {}, policy, {}, all, token);
            QCOMPARE(all.front().impact.status, ImpactStatus::Measured);
            QCOMPARE(all.front().impact.matched, 1);
            QCOMPARE(db->execute("GET " + quoteRedisArgument(key), {}, token).rows.front().back().toString(), "hello");
        } catch (const std::exception& error) {
            QFAIL(error.what());
        }
    }

    void databaseSearchMatchesLiteralTextAndCountsEachRecordOnce() {
        using namespace navidog;
        QTemporaryDir directory;
        ConnectionProfile profile;
        profile.name = "owned";
        profile.database = directory.filePath("owned.sqlite");
        const auto cancel = cancellation();
        {
            NamespaceDraft draft;
            draft.filePath = profile.database;
            QVERIFY(createNamespace(profile, draft, cancel).applied);
            auto db = connectDatabase(profile);
            const auto setup = db->execute("CREATE TABLE owned(a TEXT, b TEXT, hidden BLOB)", {}, cancel);
            QVERIFY2(setup.error.isEmpty(), qPrintable(setup.error));
            const auto inserted = db->execute(
                "INSERT INTO owned VALUES ('100%_test', '100%_test', NULL), ('no match', 'other', X'313030255F'), "
                "('100AAAtest', NULL, NULL)", {}, cancel);
            QVERIFY2(inserted.error.isEmpty(), qPrintable(inserted.error));
        }
        DatabaseSearchRequest request;
        request.text = "%_";
        auto result = searchDatabase(profile, request, cancel);
        QCOMPARE(result.scanned, 1);
        QCOMPARE(result.hits.size(), 1);
        QCOMPARE(result.hits.front().node.object, QString("owned"));
        QCOMPARE(result.hits.front().matches, 1);
        QVERIFY(!result.hits.front().previewSql.isEmpty());
        request.scope = DatabaseSearchScope::ObjectNames;
        request.mode = DatabaseSearchMode::Exact;
        request.text = "owned";
        result = searchDatabase(profile, request, cancel);
        QCOMPARE(result.hits.size(), 1);
        QCOMPARE(result.hits.front().matches, 1);
        const auto dictionary = loadDatabaseDictionary(profile, {}, cancel);
        QVERIFY(databaseDictionaryHtml(dictionary, cancel).contains("BLOB"));
        QVERIFY(supportsDatabaseSqlDump(DatabaseKind::SQLite, false));
        QVERIFY(supportsDatabaseSqlDump(DatabaseKind::PostgreSQL, true));
    }

    void objectSearchReusesDefinitionsAndHonorsInvalidation() {
        using namespace navidog;
        QTemporaryDir temp;
        QVERIFY(temp.isValid());
        ConnectionProfile profile;
        profile.id = "owned-view-search";
        profile.database = temp.filePath("fixture.sqlite");
        { QFile file(profile.database); QVERIFY(file.open(QIODevice::WriteOnly)); }
        const auto token = cancellation();
        auto db = connectDatabase(profile);
        QVERIFY(db->execute("CREATE VIEW fixture_view AS SELECT '核心业务标记' AS marker", {}, token).error.isEmpty());
        const auto path = temp.filePath("metadata.sqlite");
        DatabaseSearchRequest request{{}, "核心业务标记", DatabaseSearchScope::ObjectDefinitions,
                                      DatabaseSearchMode::Contains};
        auto live = searchDatabaseObjects(profile, {}, request, path, true, token);
        QCOMPARE(live.refreshedScopes, 1);
        QCOMPARE(live.hits.size(), 1);
        QCOMPARE(live.hits.front().node.object, QString("fixture_view"));
        QVERIFY(live.hits.front().definition.contains("核心业务标记"));

        auto offline = searchDatabaseObjects(profile, {}, request, path, false, token);
        QCOMPARE(offline.cachedScopes, 1);
        QCOMPARE(offline.hits.size(), 1);
        MetadataStore cache(path);
        const auto generation = cache.generation(profile.id);
        auto previous = cache.load(metadataKey(profile, profile.database, {}) + "-search");
        QVERIFY(previous);
        QVERIFY(db->execute("DROP VIEW fixture_view", {}, token).error.isEmpty());
        cache.invalidate(profile.id, profile.database);
        QVERIFY(cache.generation(profile.id) > generation);
        try {
            cache.replace(*previous, generation);
            QFAIL("Stale definitions were written after invalidation");
        } catch (const std::runtime_error&) {}
        QVERIFY(searchDatabaseObjects(profile, {}, request, path, false, token).hits.isEmpty());
        QVERIFY(searchDatabaseObjects(profile, {}, request, path, true, token).hits.isEmpty());
    }

    void operationTimelineRecordsSlowOperationsAndBoundsHistory() {
        using navidog::TimedOperation;
        const auto before = navidog::operationLogSnapshot();
        navidog::recordOperationTiming(TimedOperation::SqlFormat, 99);
        QCOMPARE(navidog::operationLogSnapshot().revision, before.revision);
        QVERIFY(navidog::operationLogSnapshot(before.revision).text.isEmpty());

        for (int index = 0; index <= navidog::operationLogMaxEntries; ++index)
            navidog::recordOperationTiming(TimedOperation::SqlFormat, 20000 + index);
        const auto snapshot = navidog::operationLogSnapshot(before.revision);
        QCOMPARE(snapshot.revision, before.revision + navidog::operationLogMaxEntries + 1);
        const auto rows = snapshot.text.split('\n');
        QCOMPARE(rows.size(), navidog::operationLogMaxEntries + 2);
        QCOMPARE(rows.front(), QByteArray("time_utc\tthread\toperation\telapsed_ms"));
        QVERIFY(rows.at(1).endsWith("\tui\tsql_format\t20001"));
        QVERIFY(rows.at(navidog::operationLogMaxEntries).endsWith(
            "\tui\tsql_format\t" + QByteArray::number(20000 + navidog::operationLogMaxEntries)));
    }

    void federatedCommandReadsOnlyRegisteredSqliteTables() {try {
        using namespace navidog;
        QTemporaryDir owned;
        QVERIFY(owned.isValid());
        const auto east=owned.filePath("east.sqlite"), west=owned.filePath("west.sqlite");
        const auto create=[&](const QString &path, const QString &sql, const QString &rows) {
            QFile file(path);
            if (!file.open(QIODevice::WriteOnly)) throw std::runtime_error("cannot create fixture");
            file.close();
            ConnectionProfile connection;
            connection.database=path;
            auto db=connectDatabase(connection);
            const auto cancel=cancellation();
            for (const auto &statement : {sql, rows}) {
                const auto result=db->execute(statement,{},cancel);
                if (!result.error.isEmpty()) throw std::runtime_error(result.error.toStdString());
            }
        };
        create(east,"CREATE TABLE facts(k INT,active BOOLEAN)",
                    "INSERT INTO facts VALUES(1,1),(2,0),(NULL,1)");
        create(west,"CREATE TABLE facts(k INT,n INT,extra TEXT)",
                    "INSERT INTO facts VALUES(1,2147483648,'x'),(1,2147483648,'y'),(NULL,2,'z')");
        const auto readBytes=[](const QString &path) {
            QFile file(path);
            if (!file.open(QIODevice::ReadOnly)) throw std::runtime_error("cannot read fixture");
            return file.readAll();
        };
        const auto before=std::make_pair(readBytes(east),readBytes(west));
        QString command=QCoreApplication::applicationDirPath()+"/navidog-federate";
#ifdef Q_OS_WIN
        command += ".exe";
#endif
        const auto run=[&](const QByteArray &sql, const QString &table) {
            QProcess process;
            process.start(command,{"--sqlite","east="+east,"--sqlite","west="+west,
                                   "--table","east.main.facts","--table",table,"-"});
            if (!process.waitForStarted(5000)) throw std::runtime_error(process.errorString().toStdString());
            process.write(sql);
            process.closeWriteChannel();
            if (!process.waitForFinished(5000) || process.exitStatus()!=QProcess::NormalExit)
                throw std::runtime_error("federated command did not finish");
            return std::make_pair(process.exitCode(),process.readAllStandardOutput());
        };
        const auto selected=run(
            "SELECT a.k,a.active,b.n+1 AS n FROM east.main.facts a FULL JOIN west.main.facts b "
            "ON a.k=b.k ORDER BY 1 NULLS LAST,3;","west.main.facts");
        QCOMPARE(selected.first,0);
        const auto result=QJsonDocument::fromJson(selected.second).object();
        QVERIFY(result["ok"].toBool());
        QCOMPARE(result["columns"],QJsonValue(QJsonArray{"k","active","n"}));
        QCOMPARE(result["rows"],QJsonDocument::fromJson(
            "[[1,true,2147483649],[1,true,2147483649],[2,false,null],[null,null,3],[null,true,null]]").array());
        const auto writes=run("DELETE FROM east.main.facts;","west.main.facts");
        QVERIFY(writes.first!=0 && writes.second.isEmpty());
        const auto unavailable=run("SELECT * FROM west.main.other;","west.main.facts");
        QVERIFY(unavailable.first!=0 && unavailable.second.isEmpty());
        QCOMPARE(readBytes(east),before.first);
        QCOMPARE(readBytes(west),before.second);
    }catch (const std::exception &e) { QFAIL(e.what()); }}
    void localInterpreterMatchesSQLiteRows() {try {
        using namespace navidog;
        using namespace navidog::sql::compiler;
        using namespace navidog::sql::optimizer;
        auto db = connectDatabase(profile_);
        auto token = cancellation();
        for (const auto source : {
                "WITH t(x) AS (VALUES (3),(1),(NULL)) SELECT x,TRUE AS flag FROM t ORDER BY 1;",
                "WITH t(x) AS (VALUES (3),(1),(NULL),(3)) SELECT DISTINCT x FROM t ORDER BY 1 DESC LIMIT 2;",
                "WITH l(x) AS (VALUES (1),(2),(NULL),(NULL)), r(x) AS (VALUES (2),(NULL)) "
                "SELECT x FROM l EXCEPT SELECT x FROM r ORDER BY 1;",
                "SELECT 2147483647+1 AS wide, TRUE OR NULL AS yes, TRUE AND NULL AS unknown;",
                "WITH l(id) AS (VALUES (1),(2),(NULL)), r(id) AS (VALUES (2),(3),(NULL)) "
                "SELECT l.id,r.id FROM l LEFT JOIN r ON l.id=r.id AND r.id>2 ORDER BY 1,2;",
                "SELECT 2 IS TRUE, 2 IS +TRUE, 2=TRUE, 1 IS NULL < 2, "
                "NULL IS DISTINCT FROM FALSE, 0 IS NOT DISTINCT FROM TRUE, 2==2;",
                "SELECT -1 BETWEEN 1 AND NULL, 2 BETWEEN 1 AND NULL, "
                "3 IN (1,NULL), 1 IN (NULL,1), 3 NOT IN (1,NULL);",
                "WITH l(k) AS (VALUES (1),(2),(3),(NULL)), r(k) AS (VALUES (2),(NULL),(2)) "
                "SELECT k,k IN(SELECT k FROM r),k NOT IN(SELECT k FROM r),"
                "NULL NOT IN(SELECT k FROM r WHERE FALSE) FROM l "
                "WHERE EXISTS(SELECT 1 FROM r WHERE k=2) ORDER BY k NULLS LAST;",
                "SELECT (SELECT (SELECT 4)+1), (SELECT 1 WHERE FALSE);",
                "WITH t(n) AS (VALUES (2),(1)) SELECT (SELECT n FROM t ORDER BY n);",
                "WITH t(\"true\") AS (VALUES (7)) SELECT TRUE, 2 IS TRUE, t.column1 FROM t;",
                "SELECT TRUE, 2 IS TRUE, t.column1 FROM (SELECT 7 AS TRUE) t;",
                "SELECT CASE WHEN 2 THEN 5 ELSE 0 END, CASE 1 WHEN TRUE THEN 6 ELSE 0 END;",
                "WITH t(k,n) AS (VALUES (1,8),(1,8),(1,10),(1,NULL),(2,NULL),(NULL,1)) "
                "SELECT k,COUNT(*),COUNT(n),COUNT(DISTINCT n),SUM(n) FILTER (WHERE n>8),MIN(n),MAX(n) "
                "FROM t GROUP BY k HAVING COUNT(*)>=1 ORDER BY 1;",
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
                "WITH t(i,k,n) AS (VALUES (1,1,NULL),(2,1,7),(3,2,4),(4,3,6),(5,3,2)) "
                "SELECT i,NTILE(3) OVER(ORDER BY i),NTILE(9) OVER(PARTITION BY k ORDER BY i),"
                "FIRST_VALUE(n) OVER(ORDER BY i ROWS BETWEEN 1 PRECEDING AND CURRENT ROW EXCLUDE CURRENT ROW),"
                "LAST_VALUE(n) OVER(ORDER BY i ROWS BETWEEN 1 PRECEDING AND 1 FOLLOWING EXCLUDE CURRENT ROW),"
                "NTH_VALUE(n,2) OVER(ORDER BY i RANGE BETWEEN 1 PRECEDING AND 1 FOLLOWING EXCLUDE CURRENT ROW) "
                "FROM t ORDER BY i;",
                "WITH t(i,g,n,fallback) AS (VALUES (1,1,2,101),(2,1,NULL,102),(3,1,7,103),(4,2,9,104)) "
                "SELECT i,LAG(n,1,fallback) OVER w,LEAD(n,2,fallback) OVER w,LAG(n,0) OVER w,LEAD(n) OVER w "
                "FROM t WINDOW w AS (PARTITION BY g ORDER BY i) ORDER BY i;",
                "WITH t(k,n) AS (VALUES (1,5),(1,6),(2,7),(3,NULL)) "
                "SELECT k,SUM(n),SUM(SUM(n)) OVER(ORDER BY k),COUNT(*) FILTER (WHERE SUM(n)>7) OVER() "
                "FROM t GROUP BY k ORDER BY k;"}) {
            const auto compiled = compile(Dialect::SQLite, source);
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
                    else QVERIFY(actual.isNull() && std::holds_alternative<std::monostate>(expected));
                }
            }
        }
        const auto created = db->execute("CREATE TABLE bool_scope_owned(\"true\" INT);", {}, token);
        QVERIFY2(created.error.isEmpty(),qPrintable(created.error));
        const auto inserted = db->execute("INSERT INTO bool_scope_owned VALUES (7);", {}, token);
        QVERIFY2(inserted.error.isEmpty(),qPrintable(inserted.error));
        const auto snapshot = collectMetadata(*db, profile_, {}, token);
        const MetadataCatalog catalog(std::make_shared<const MetadataSnapshot>(snapshot));
        const NameResolutionContext names{profile_.database.toStdString(), {}, Dialect::SQLite};
        const auto source = "SELECT TRUE, 2 IS TRUE FROM bool_scope_owned;";
        const auto compiled = compile(Dialect::SQLite, source, {}, &catalog, names);
        QVERIFY(compiled->ok());
        const auto canonical = generateSql(compiled->model, Dialect::SQLite);
        QVERIFY(canonical.ok());
        const auto native = db->execute(source, {}, token);
        const auto roundtrip = db->execute(QString::fromStdString(canonical.sql), {}, token);
        QVERIFY2(native.error.isEmpty(),qPrintable(native.error));
        QVERIFY2(roundtrip.error.isEmpty(),qPrintable(roundtrip.error));
        QCOMPARE(native.rows,roundtrip.rows);
        QCOMPARE(roundtrip.rows.at(0).at(0).toInt(),7);
        QCOMPARE(roundtrip.rows.at(0).at(1).toInt(),0);
        const auto dropped = db->execute("DROP TABLE bool_scope_owned;", {}, token);
        QVERIFY2(dropped.error.isEmpty(),qPrintable(dropped.error));
    }catch(const std::exception &e){QFAIL(e.what());}}
    void credentialsAreSessionOnlyAndLegacyFlagsCannotAccessSystemStore(){
        using namespace navidog;QVERIFY(!systemSecretStoreAvailable());ConnectionProfile old;old.id="owned-legacy-secret";old.name="Legacy";old.driver=DatabaseKind::MySQL;old.credentialSource=CredentialSource::SystemStore;old.savePassword=old.ssh.savePassword=old.ssh.savePassphrase=old.http.savePassword=true;
        old.password="owned-runtime-database";old.ssh.password="owned-runtime-ssh";old.ssh.passphrase="owned-runtime-phrase";old.http.password="owned-runtime-http";
        auto runtime=resolveCredentials(old);QCOMPARE(runtime.password,old.password);QCOMPARE(runtime.ssh.password,old.ssh.password);QCOMPARE(runtime.http.password,old.http.password);QVERIFY(!runtime.savePassword && !runtime.ssh.savePassword && !runtime.ssh.savePassphrase && !runtime.http.savePassword);QCOMPARE(runtime.credentialSource,CredentialSource::Prompt);
        ProfileStore store(dir_.filePath("session-only-connections.json"));store.saveWorkspace({{old},{}});auto loaded=store.loadWorkspace().connections;QCOMPARE(loaded.size(),1);QCOMPARE(loaded[0].credentialSource,CredentialSource::Prompt);QVERIFY(loaded[0].password.isEmpty());QVERIFY(!loaded[0].savePassword && !loaded[0].ssh.savePassword && !loaded[0].http.savePassword);
        QFile file(dir_.filePath("session-only-connections.json"));QVERIFY(file.open(QIODevice::ReadOnly));QVERIFY(!file.readAll().contains("owned-runtime-"));
        old.password.clear();QVERIFY_THROWS_EXCEPTION(std::runtime_error,resolveCredentials(old));removeConnectionSecrets(old.id);QVERIFY_THROWS_EXCEPTION(std::runtime_error,loadConnectionSecrets(old));
    }
    void sqliteWorkspaceEncryptsAndAuthenticatesRememberedPasswords(){
        using namespace navidog;QTemporaryDir owned;auto path=localStorePath(owned.path());ProfileStore store(path);ConnectionProfile p;p.id="owned-encrypted";p.name="Owned encryption";p.driver=DatabaseKind::MySQL;p.host="localhost";p.user="owned";p.password="synthetic-only-secret-31";p.savePassword=true;p.credentialSource=CredentialSource::LocalEncrypted;p.secretsLoaded=true;p.credentialStorePath=path;
        store.saveWorkspace({{p},{{"group","Owned group"}}});auto loaded=store.loadWorkspace();QCOMPARE(loaded.connections.size(),1);QCOMPARE(loaded.groups.size(),1);auto restored=loaded.connections.front();QVERIFY(restored.password.isEmpty());auto resolved=resolveCredentials(restored);QCOMPARE(resolved.password,p.password);
        LocalStore db(path);auto document=db.document("connections");QVERIFY(document && !document->contains(p.password.toUtf8()));auto cipher=db.secret(p.id);QVERIFY(cipher && !cipher->contains(p.password.toUtf8()));auto tampered=*cipher;tampered[tampered.size()-1]=char(tampered.back()^1);db.putSecret(p.id,tampered);QVERIFY_THROWS_EXCEPTION(std::runtime_error,resolveCredentials(restored));restored.name="Renamed without loading credentials";store.saveWorkspace({{restored},{}});QCOMPARE(db.secret(p.id).value(),tampered);db.putSecret(p.id,*cipher);
        QVERIFY_THROWS_EXCEPTION(std::runtime_error,db.transaction([&]{db.putDocument("rolled-back","x");throw std::runtime_error("owned rollback");}));QVERIFY(!db.document("rolled-back"));
        restored.savePassword=false;restored.credentialSource=CredentialSource::Prompt;store.saveWorkspace({{restored},{}});QVERIFY(!db.secret(p.id));
    }
    void backgroundMetadataDoesNotOccupyForegroundQueryLane(){
        using namespace navidog;QTemporaryDir owned;QueryService service;service.setMetadataCachePath(localStorePath(owned.path()));QSignalSpy synced(&service,&QueryService::metadataCompleted),queried(&service,&QueryService::completed);QVERIFY(service.metadata(profile_,profile_.database,{},"owned-quiet-refresh",true));QVERIFY(!service.busy());QueryExecutionOptions options;options.sessionId="owned-parallel-query";QVERIFY(service.run(profile_,"SELECT 1",{},options));QTRY_COMPARE_WITH_TIMEOUT(queried.count(),1,4000);QTRY_COMPARE_WITH_TIMEOUT(synced.count(),1,4000);auto result=qvariant_cast<MetadataResult>(synced[0][1]);QVERIFY2(result.error.isEmpty(),qPrintable(result.error));QVERIFY(result.snapshot);
    }
    void slowBackgroundMetadataDoesNotQueueCatalogNavigation(){
        using namespace navidog;QTemporaryDir owned;auto lockedProfile=profile_;lockedProfile.id="owned-locked-metadata";lockedProfile.database=owned.filePath("locked.sqlite");{QFile file(lockedProfile.database);QVERIFY(file.open(QIODevice::WriteOnly));}auto lock=connectDatabase(lockedProfile);auto token=cancellation();QVERIFY(lock->execute("CREATE TABLE owned_lock(id INTEGER)",{},token).error.isEmpty());QVERIFY(lock->execute("BEGIN EXCLUSIVE",{},token).error.isEmpty());
        QueryService service;service.setMetadataCachePath(localStorePath(owned.path()));QSignalSpy synced(&service,&QueryService::metadataCompleted),catalog(&service,&QueryService::catalogCompleted);QVERIFY(service.metadata(lockedProfile,lockedProfile.database,{},"owned-blocked-refresh",true));QTest::qWait(80);QCOMPARE(synced.count(),0);
        CatalogNode root;root.kind="connection";QVERIFY(service.catalog(profile_,root,"owned-independent-navigation"));QTRY_COMPARE_WITH_TIMEOUT(catalog.count(),1,600);QCOMPARE(synced.count(),0);auto result=qvariant_cast<CatalogResult>(catalog[0][1]);QVERIFY2(result.error.isEmpty(),qPrintable(result.error));QVERIFY(!result.nodes.isEmpty());QVERIFY(lock->execute("ROLLBACK",{},cancellation()).error.isEmpty());QTRY_COMPARE_WITH_TIMEOUT(synced.count(),1,3000);auto metadata=qvariant_cast<MetadataResult>(synced[0][1]);QVERIFY2(metadata.error.isEmpty(),qPrintable(metadata.error));QVERIFY(metadata.snapshot);
    }
    void plannerCatalogStatisticsRoundTripWithoutScanningBusinessTables() {
        using namespace navidog;QTemporaryDir owned;ConnectionProfile p;p.database=ensureDemoDatabase(owned.path());auto db=connectDatabase(p);auto token=cancellation();
        auto before=db->execute("SELECT count(*) FROM orders",{},token).rows[0][0].toInt();
        // Fixture-only ANALYZE: the production collector never runs it.
        auto analyzed=db->execute("ANALYZE orders",{},token);QVERIFY(analyzed.error.isEmpty());auto stats=collectPlannerStatistics(*db,p,{},token);QVERIFY(stats.complete);QVERIFY(!stats.tables.empty());
        auto found=std::find_if(stats.tables.begin(),stats.tables.end(),[](const auto &t){return t.name=="orders";});QVERIFY(found!=stats.tables.end());QCOMPARE(found->rows,100000.);QVERIFY(found->rowBytesAssumed);
        auto snapshot=collectMetadata(*db,p,{},token);QVERIFY(snapshot.statistics);MetadataStore store(owned.filePath("cache.sqlite"));store.replace(snapshot);auto loaded=store.load(snapshot.key);QVERIFY(loaded&&loaded->statistics);QCOMPARE(loaded->statistics->tables.size(),snapshot.statistics->tables.size());QVERIFY(!loaded->verified);
        auto copy=plannerTableFromJson(plannerTableJson(*found));QCOMPARE(copy.name,found->name);QCOMPARE(copy.rows,found->rows);QCOMPARE(copy.source,sql::optimizer::StatisticsSource::ServerCatalog);
        QCOMPARE(db->execute("SELECT count(*) FROM orders",{},token).rows[0][0].toInt(),before);
    }
    void metadataTypesArePreparedOnceAndRebuiltFromDisk() {
      using namespace navidog;using namespace navidog::sql::compiler;
      MetadataSnapshot snapshot;snapshot.key="owned-typed-metadata";snapshot.profileId="owned-types";
      snapshot.driver=DatabaseKind::SQLServer;snapshot.database="owned";snapshot.schema="dbo";
      snapshot.searchPath={"dbo"};snapshot.refreshedMs=QDateTime::currentMSecsSinceEpoch();
      snapshot.verified=snapshot.tablesComplete=snapshot.columnsComplete=true;
      MetadataObject table;table.key="owned-table";table.kind="table";table.database="owned";table.schema="dbo";table.name="typed";
      table.columns={{"amount","decimal",{},1},{"label","nvarchar",{},2},{"id","int",{},3}};snapshot.objects.push_back(table);
      MetadataObject function;
      function.key="owned-typed-function";function.kind="function";function.database="owned";
      function.schema="dbo";function.name="identity_value";function.type="bigint";
      function.minArgs=function.maxArgs=1;function.routineKind=RoutineKind::Scalar;
      snapshot.routinesComplete=true;snapshot.objects.push_back(function);
      prepareMetadataTypeFacts(snapshot);QCOMPARE(snapshot.typeFacts.size(),std::size_t(4));
      const auto &decimal=snapshot.typeFacts.at("decimal");QCOMPARE(decimal.family,TypeFamily::Decimal);QVERIFY(!decimal.precision && !decimal.scale);
      QVERIFY(!snapshot.typeFacts.at("nvarchar").length);
      QCOMPARE(*snapshot.typeFacts.at("int").bits,32);
      const auto published=std::make_shared<const MetadataSnapshot>(snapshot);
      MetadataCatalog catalog(published);
      NameResolutionContext context{"owned","dbo",Dialect::SQLServer};
      const auto first=catalog.findTable(context,{"typed"});
      const auto reused=MetadataCatalog(published).findTable(context,{"typed"});
      QVERIFY(first && reused && first->columns.front().sqlType);
      QCOMPARE(first->columns.front().sqlType.get(),reused->columns.front().sqlType.get());
      auto refreshed=std::make_shared<MetadataSnapshot>(*published);
      refreshed->objects[0].name="typed_renamed";
      MetadataCatalog refreshedCatalog(refreshed);
      QVERIFY(!refreshedCatalog.findTable(context,{"typed"}));
      QVERIFY(refreshedCatalog.findTable(context,{"typed_renamed"}));
      QVERIFY(catalog.findTable(context,{"typed"}));
      auto many=std::make_shared<MetadataSnapshot>(*published);
      for (int i=0;i<48;++i) {
        auto candidate=table;
        candidate.name=QString("unrelated_%1").arg(i);
        many->objects.push_back(candidate);
      }
      auto late=table;
      late.name="late_typed";
      many->objects.push_back(late);
      const auto matches=MetadataCatalog(many).matchingRelations(context,1,
          [](const auto &,const auto &,const auto &name) { return name=="late_typed"; });
      QCOMPARE(matches.size(),std::size_t(1));
      QCOMPARE(matches.front().name,std::string("late_typed"));
      auto parsed=compile(Dialect::SQLServer,"SELECT amount,label,id FROM typed;",{},&catalog,context);
      QVERIFY2(parsed->ok(),parsed->diagnostics.empty()?"no model":parsed->diagnostics.front().message.c_str());
      const auto &q=parsed->model.front();const auto &type=parsed->model.types.get(q.expressions[q.projections[0].expression].sqlType);
      QCOMPARE(type.family,TypeFamily::Decimal);QVERIFY(!type.precision);QCOMPARE(type.nullability,Nullability::Unknown);
      auto called=compile(Dialect::SQLServer,"SELECT dbo.identity_value(id) FROM typed;",{},&catalog,context);
      QVERIFY2(called->ok(),called->diagnostics.empty()?"no routine model":called->diagnostics.front().message.c_str());
      const auto &call=called->model.front();
      const auto &returned=called->model.types.get(call.expressions[call.projections.front().expression].sqlType);
      QCOMPARE(returned.family,TypeFamily::Integer);QCOMPARE(returned.bits,std::optional<int>(64));
      QTemporaryDir own;MetadataStore store(own.filePath("types.sqlite"));store.replace(snapshot);
      auto loaded=store.load(snapshot.key);QVERIFY(loaded);QVERIFY(!loaded->verified);QCOMPARE(loaded->typeFacts.size(),std::size_t(4));
      QVERIFY(loaded->typeFacts.at("decimal")==decimal);
      QVERIFY(loaded->typeFacts.at("bigint")==snapshot.typeFacts.at("bigint"));
      QVERIFY(MetadataCatalog(loaded).findRoutines(context,{"dbo","identity_value"},false).empty());
      auto cancelled=cancellation();cancelled->store(true);prepareMetadataTypeFacts(snapshot,cancelled);QVERIFY(snapshot.typeFacts.empty());
    }
    void metadataStoreAndSnapshotLifetime(){
        using namespace navidog;auto db=connectDatabase(profile_);auto token=cancellation();auto r=db->execute("CREATE TABLE metadata_owned(Id INTEGER PRIMARY KEY,note TEXT)",{},token);QVERIFY(r.error.isEmpty());
        db->execute("INSERT INTO metadata_owned VALUES(1,'private-row-never-cache')",{},token);
        auto snapshot=collectMetadata(*db,profile_,{},token);QVERIFY(snapshot.tablesComplete);QVERIFY(snapshot.columnsComplete);QVERIFY(snapshot.routinesComplete);QVERIFY(snapshot.usable());
        auto path=dir_.filePath("metadata/cache.sqlite");MetadataStore store(path);store.replace(snapshot);auto loaded=store.load(snapshot.key);QVERIFY(loaded);QCOMPARE(loaded->objects.size(),snapshot.objects.size());QVERIFY(!loaded->verified);QVERIFY(!loaded->usable());
        MetadataCatalog catalog(std::make_shared<const MetadataSnapshot>(snapshot));sql::compiler::NameResolutionContext context{profile_.database.toStdString(),{},sql::compiler::Dialect::SQLite};
        auto found=catalog.findTable(context,{"main","metadata_owned"});QVERIFY(found);QCOMPARE(found->columns.size(),std::size_t(2));
        auto good=sql::compiler::compile(sql::compiler::Dialect::SQLite,"SELECT id,note FROM metadata_owned",{},&catalog,context);QVERIFY2(good->ok(),good->diagnostics.empty()?"missing diagnostic":good->diagnostics[0].message.c_str());
        for(const auto& text:{"SELECT X.id FROM metadata_owned x","WITH q AS(SELECT id FROM metadata_owned) SELECT ID FROM Q","SELECT ID FROM metadata_owned a JOIN metadata_owned b USING(ID)"}){auto checked=sql::compiler::compile(sql::compiler::Dialect::SQLite,text,{},&catalog,context);QVERIFY2(checked->ok(),checked->diagnostics.empty()?"missing diagnostic":checked->diagnostics[0].message.c_str());}
        auto bad=sql::compiler::compile(sql::compiler::Dialect::SQLite,"SELECT absent FROM metadata_owned",{},&catalog,context);QVERIFY(!bad->ok());QCOMPARE(bad->diagnostics[0].code,sql::compiler::DiagnosticCode::UnknownColumn);
        auto invalid=snapshot;invalid.objects.push_back(invalid.objects[0]);QVERIFY_THROWS_EXCEPTION(std::runtime_error,store.replace(invalid));QCOMPARE(store.load(snapshot.key)->objects.size(),snapshot.objects.size());
        store.invalidate(profile_.id,profile_.database);QVERIFY(store.load(snapshot.key)->invalidated);store.replace(snapshot);store.removeProfile(profile_.id);QVERIFY(!store.load(snapshot.key));
        QFile file(path);QVERIFY(file.open(QIODevice::ReadOnly));auto bytes=file.readAll();QVERIFY(!bytes.contains("private-row-never-cache"));db->execute("DROP TABLE metadata_owned",{},token);
    }
    void otherObjectMetadataRoundTripsWithoutDataReads() {
      using namespace navidog;using namespace navidog::sql::compiler;
      QTemporaryDir owned;auto p=profile_;p.database=owned.filePath("objects.sqlite");
      {QFile file(p.database);QVERIFY(file.open(QIODevice::WriteOnly));}
      auto db=connectDatabase(p);auto token=cancellation();
      QVERIFY(db->execute("CREATE TABLE owned_objects(n int)",{},token).error.isEmpty());
      QVERIFY(db->execute("CREATE INDEX owned_objects_idx ON owned_objects(n)",{},token).error.isEmpty());
      QVERIFY(db->executeDefinition("CREATE TRIGGER owned_objects_trigger AFTER INSERT ON owned_objects BEGIN SELECT 1;END",{},token).error.isEmpty());
      auto snapshot=collectMetadata(*db,p,{},token);QVERIFY2(snapshot.objectWarning.isEmpty(),qPrintable(snapshot.objectWarning));
      MetadataCatalog catalog(std::make_shared<const MetadataSnapshot>(snapshot));NameResolutionContext context{p.database.toStdString(),"main",Dialect::SQLite};
      auto indexes=catalog.listObjects(context,CatalogObjectKind::Index,20);QCOMPARE(indexes.size(),std::size_t(1));QCOMPARE(indexes[0].name,std::string("owned_objects_idx"));
      auto triggers=catalog.listObjects(context,CatalogObjectKind::Trigger,20);QCOMPARE(triggers.size(),std::size_t(1));
      MetadataStore store(owned.filePath("cache.sqlite"));store.replace(snapshot);auto loaded=store.load(snapshot.key);QVERIFY(loaded);
      MetadataCatalog cached(loaded);QCOMPARE(cached.listObjects(context,CatalogObjectKind::Trigger,20).size(),std::size_t(1));
      auto check=compile(Dialect::SQLite,"SELECT n FROM owned_objects",{},&catalog,context);QVERIFY(check->ok());
      QCOMPARE(db->execute("SELECT COUNT(*) FROM owned_objects",{},token).rows[0][0].toInt(),0);
    }
    void routineMetadataPropertiesPersistAndReplace() {
      using namespace navidog;using namespace navidog::sql::compiler;
      QTemporaryDir owned;auto db=connectDatabase(profile_);auto token=cancellation();
      auto snapshot=collectMetadata(*db,profile_,{},token);
      MetadataCatalog catalog(std::make_shared<const MetadataSnapshot>(snapshot));
      NameResolutionContext context{profile_.database.toStdString(),{},Dialect::SQLite};
      auto functions=catalog.findRoutines(context,{"sum"},false);QVERIFY(!functions.empty());
      QCOMPARE(functions.front().kind,RoutineKind::Aggregate);
      functions=catalog.findRoutines(context,{"row_number"},false);QVERIFY(!functions.empty());
      QCOMPARE(functions.front().kind,RoutineKind::Window);
      functions=catalog.findRoutines(context,{"length"},false);QVERIFY(!functions.empty());
      QCOMPARE(functions.front().kind,RoutineKind::Scalar);QCOMPARE(functions.front().volatility,FunctionVolatility::Immutable);
      auto one=compile(Dialect::SQLite,"SELECT max(1);",{},&catalog,context);QVERIFY(one->ok());
      QCOMPARE(one->model[0].expressions[one->model[0].projections[0].expression].routineKind,RoutineKind::Aggregate);
      auto two=compile(Dialect::SQLite,"SELECT max(1,2);",{},&catalog,context);QVERIFY(two->ok());
      QCOMPARE(two->model[0].expressions[two->model[0].projections[0].expression].routineKind,RoutineKind::Scalar);
      auto path=owned.filePath("cache.sqlite");MetadataStore store(path);store.replace(snapshot);
      auto loaded=store.load(snapshot.key);QVERIFY(loaded);
      QCOMPARE(loaded->objects.size(),snapshot.objects.size());
      for(int i=0;i<snapshot.objects.size();++i) {
        auto found=std::find_if(loaded->objects.cbegin(),loaded->objects.cend(),[&](const auto &o){return o.key==snapshot.objects[i].key;});
        QVERIFY(found!=loaded->objects.cend());QCOMPARE(found->routineKind,snapshot.objects[i].routineKind);QCOMPARE(found->volatility,snapshot.objects[i].volatility);
      }
      // A legacy collector has no facts. Replacing that snapshot must also
      // remove the former facts, never attach stale classification to an ID.
      for(auto &o:snapshot.objects){o.routineKind=RoutineKind::Unknown;o.volatility=FunctionVolatility::Unknown;}
      store.replace(snapshot);loaded=store.load(snapshot.key);QVERIFY(loaded);
      for(const auto &o:loaded->objects){QCOMPARE(o.routineKind,RoutineKind::Unknown);QCOMPARE(o.volatility,FunctionVolatility::Unknown);}
      auto p=profile_;p.database=path;auto raw=connectDatabase(p);
      QCOMPARE(raw->execute("PRAGMA user_version",{},token).rows[0][0].toInt(),1);
      QCOMPARE(raw->execute("SELECT count(*) FROM routine_properties",{},token).rows[0][0].toInt(),0);
    }
    void metadataBulkReadsDoNotGrowPerTableAndFutureFormatsStayUntouched(){
        using namespace navidog;auto db=connectDatabase(profile_);auto token=cancellation();
        for(int i=0;i<20;++i)db->execute(QString("CREATE TABLE meta_bulk_%1(n int)").arg(i),{},token);
        struct CountingDb final:Database{Database& delegate;int count=0;CountingDb(Database& value):delegate(value){}QueryResult execute(const QString& sql,QueryLimits limits,const Cancellation& cancel)override{++count;return delegate.execute(sql,limits,cancel);}QStringList tables()override{return delegate.tables();}QString serverVersion()override{return delegate.serverVersion();}} counter(*db);
        auto snapshot=collectMetadata(counter,profile_,{},token);QVERIFY(snapshot.tablesComplete);QCOMPARE(counter.count,5);
        for(int i=0;i<20;++i)db->execute(QString("DROP TABLE meta_bulk_%1").arg(i),{},token);
        auto future=profile_;future.database=dir_.filePath("future-cache.sqlite");{QFile empty(future.database);QVERIFY(empty.open(QIODevice::WriteOnly));}{auto owned=connectDatabase(future);owned->execute("PRAGMA user_version=99",{},token);}QFile file(future.database);QVERIFY(file.open(QIODevice::ReadOnly));auto before=file.readAll();file.close();QVERIFY_THROWS_EXCEPTION(std::runtime_error,MetadataStore{future.database});QVERIFY(file.open(QIODevice::ReadOnly));QCOMPARE(file.readAll(),before);
    }
    void metadataCompletenessExpiryAndIdentityAreConservative(){
        using namespace navidog;using namespace navidog::sql::compiler;auto db=connectDatabase(profile_);auto value=collectMetadata(*db,profile_,{},cancellation());
        auto changed=profile_;changed.name="renamed";changed.groupId="moved";changed.password="runtime-secret";QCOMPARE(metadataKey(changed,profile_.database,{}),value.key);changed.host="elsewhere";QVERIFY(metadataKey(changed,profile_.database,{})!=value.key);
        auto partial=value;partial.tablesComplete=false;partial.columnsComplete=false;MetadataCatalog catalog(std::make_shared<const MetadataSnapshot>(partial));NameResolutionContext context{profile_.database.toStdString(),{},Dialect::SQLite};
        auto c=compile(Dialect::SQLite,"SELECT missing FROM never_cached",{},&catalog,context);QVERIFY(c->ok());
        partial=value;partial.refreshedMs-=MetadataSnapshot::ttlMs+1;MetadataCatalog expired(std::make_shared<const MetadataSnapshot>(partial));QVERIFY(!expired.tableLookupComplete(context,{"absent"}));QVERIFY(!expired.findTable(context,{"orders"}));
        MetadataCatalog fresh(std::make_shared<const MetadataSnapshot>(value));QVERIFY(fresh.tableLookupComplete(context,{"absent"}));QVERIFY(!fresh.tableLookupComplete(context,{"attached","absent"}));
        auto valid=compile(Dialect::SQLite,"SELECT length('hello')",{},&fresh,context);QVERIFY(valid->ok());auto invalid=compile(Dialect::SQLite,"SELECT length()",{},&fresh,context);QVERIFY(!invalid->ok());QCOMPARE(invalid->diagnostics[0].code,DiagnosticCode::RoutineArgumentCount);
    }
    void aggregateFiltersMatchOwnedSqliteAndPostgresConversion() {
      using namespace navidog;using namespace navidog::sql::compiler;auto db=connectDatabase(profile_);auto token=cancellation();
      const std::string text="WITH t(x) AS(SELECT 1 UNION ALL SELECT 2 UNION ALL SELECT NULL) SELECT COUNT(*) FILTER(WHERE x>1),SUM(x) FILTER(WHERE x IS NOT NULL) FROM t;";
      auto converted=convertSql(Dialect::PostgreSQL,Dialect::SQLite,text);QVERIFY2(converted.ok(),converted.diagnostics.empty()?"no conversion":converted.diagnostics.front().message.c_str());
      auto expected=db->execute(QString::fromStdString(text),{},token), actual=db->execute(QString::fromStdString(converted.sql),{},token);QVERIFY2(actual.error.isEmpty(),qPrintable(actual.error));QCOMPARE(actual.rows,expected.rows);QCOMPARE(actual.rows[0][0].toInt(),1);QCOMPARE(actual.rows[0][1].toInt(),3);
    }
    void metadataTypeFamiliesAvoidSubstringGuesses() {
      using namespace navidog;using namespace navidog::sql::compiler;
      auto snapshot=std::make_shared<MetadataSnapshot>();snapshot->driver=DatabaseKind::PostgreSQL;snapshot->database="owned";snapshot->schema="public";snapshot->searchPath={"public"};snapshot->verified=snapshot->tablesComplete=snapshot->columnsComplete=snapshot->routinesComplete=true;snapshot->refreshedMs=QDateTime::currentMSecsSinceEpoch();
      MetadataObject table;table.kind="table";table.database="owned";table.schema="public";table.name="t";
      for(const auto &type:{"point","interval","integer[]","integer","numeric(10,2)","double precision","character varying(30)"})table.columns.push_back({QString("c%1").arg(table.columns.size()),type});
      snapshot->objects.push_back(table);MetadataObject routine;routine.kind="function";routine.database="owned";routine.schema="public";routine.name="calculate";routine.type="integer";routine.minArgs=routine.maxArgs=1;snapshot->objects.push_back(routine);
      MetadataCatalog catalog(snapshot);NameResolutionContext names{"owned","public",Dialect::PostgreSQL};
      auto def=catalog.findTable(names,{"t"});QVERIFY(def);QCOMPARE(def->columns[0].type,ValueType::Unknown);QCOMPARE(def->columns[1].type,ValueType::Unknown);QCOMPARE(def->columns[2].type,ValueType::Unknown);QCOMPARE(def->columns[3].type,ValueType::Integer);QCOMPARE(def->columns[4].type,ValueType::Decimal);QCOMPARE(def->columns[5].type,ValueType::Decimal);QCOMPARE(def->columns[6].type,ValueType::String);
      auto result=compile(Dialect::PostgreSQL,"SELECT public.calculate(1);",{},&catalog,names);QVERIFY(result->ok());const auto &q=result->model[0];QCOMPARE(q.expressions.at(q.projections[0].expression).type,ValueType::Integer);
    }
    void metadataServiceDiskReadsNeverConnectAndDdlInvalidates(){
        using namespace navidog;QueryService service;service.setMetadataCachePath(dir_.filePath("service-metadata/catalog.sqlite"));QSignalSpy ready(&service,&QueryService::metadataCompleted);QSignalSpy invalidated(&service,&QueryService::metadataInvalidated);
        auto unreachable=profile_;unreachable.driver=DatabaseKind::PostgreSQL;unreachable.host="invalid.invalid";unreachable.password.clear();QVERIFY(service.metadata(unreachable,"offline","public","disk",false));QVERIFY(ready.wait(3000));QCOMPARE(service.sessionCount(),0);QVERIFY(!qvariant_cast<MetadataResult>(ready.takeFirst()[1]).snapshot);
        QVERIFY(service.metadata(profile_,profile_.database,{},"refresh",true));QVERIFY(ready.wait(5000));auto result=qvariant_cast<MetadataResult>(ready.takeFirst()[1]);QVERIFY2(result.error.isEmpty(),qPrintable(result.error));QVERIFY(result.snapshot && result.snapshot->usable());
        QSignalSpy done(&service,&QueryService::completed);QVERIFY(service.run(profile_,"CREATE TABLE metadata_service_owned(n)"));QVERIFY(done.wait(3000));QVERIFY(invalidated.count()>=1);auto outcome=qvariant_cast<QueryOutcome>(done.takeFirst()[0]);QVERIFY(outcome.metadataChanged);
        QVERIFY(service.metadata(profile_,profile_.database,{},"disk-after-ddl",false));QVERIFY(ready.wait(3000));result=qvariant_cast<MetadataResult>(ready.takeFirst()[1]);QVERIFY(result.snapshot && result.snapshot->invalidated);QVERIFY(!result.snapshot->usable());
        QVERIFY(service.metadata(profile_,profile_.database,{},"race-refresh",true));service.invalidateMetadata(profile_.id,profile_.database);QVERIFY(ready.wait(5000));result=qvariant_cast<MetadataResult>(ready.takeFirst()[1]);QVERIFY(!result.snapshot);QVERIFY(result.error.contains("已失效"));
        auto db=connectDatabase(profile_);db->execute("DROP TABLE metadata_service_owned",{},cancellation());
    }
    void nativeNonNullFactsRequireCurrentSnapshotAndSurviveCache() {try {
      using namespace navidog;using namespace navidog::sql::compiler;namespace opt=navidog::sql::optimizer;
      QTemporaryDir owned;ConnectionProfile p=profile_;p.id="owned-non-null-cache";p.database=owned.filePath("data.sqlite");{QFile file(p.database);QVERIFY(file.open(QIODevice::WriteOnly));}auto db=connectDatabase(p);auto token=cancellation();
      auto actual=db->execute("CREATE TABLE proof_owned(id INTEGER NOT NULL,n INTEGER)",{},token);QVERIFY(actual.error.isEmpty());
      auto snapshot=collectMetadata(*db,p,"main",token);QVERIFY(snapshot.usable());
      auto find=[&](const MetadataSnapshot &s,const QString &name)->bool{for(const auto &object:s.objects)if(object.name=="proof_owned")for(const auto &column:object.columns)if(column.name==name)return column.nonNullKnown;return false;};
      QVERIFY(find(snapshot,"id"));QVERIFY(!find(snapshot,"n"));
      auto plan=[&](MetadataSnapshotPtr data){MetadataCatalog catalog(data);auto c=compile(Dialect::SQLite,"SELECT id FROM proof_owned WHERE id IS NOT NULL",{},&catalog,{p.database.toStdString(),"main",Dialect::SQLite});if(!c->ok())throw std::runtime_error(c->diagnostics.front().message);return opt::optimize(c->model);};
      auto fired=[](const opt::OptimizationResult &r){return std::any_of(r.rules.begin(),r.rules.end(),[](const auto &rule){return rule.rule=="HepFilterNullability";});};
      auto fresh=plan(std::make_shared<const MetadataSnapshot>(snapshot));QVERIFY(fresh.ok());QVERIFY(fired(fresh));
      auto expired=snapshot;expired.refreshedMs=QDateTime::currentMSecsSinceEpoch()-MetadataSnapshot::ttlMs-1;
      auto stale=plan(std::make_shared<const MetadataSnapshot>(expired));QVERIFY(stale.ok());QVERIFY(!fired(stale));
      const auto cache=owned.filePath("cache.sqlite");
      {MetadataStore store(cache);store.replace(snapshot);auto loaded=store.load(snapshot.key);QVERIFY(loaded && !loaded->verified);QVERIFY(find(*loaded,"id"));auto unverified=plan(loaded);QVERIFY(unverified.ok());QVERIFY(!fired(unverified));}
      // Simulate a v1 cache from before this additive side table existed.
      {auto old=p;old.database=cache;auto legacy=connectDatabase(old);auto removed=legacy->execute("DROP TABLE member_constraints",{},token);QVERIFY(removed.error.isEmpty());}
      {MetadataStore store(cache);auto legacy=store.load(snapshot.key);QVERIFY(legacy && !find(*legacy,"id"));store.replace(snapshot);auto updated=store.load(snapshot.key);QVERIFY(updated && find(*updated,"id"));}
    }catch(const std::exception &e){QFAIL(e.what());}}
    void sqliteNullablePrimaryKeysCannotProveEmptyFilters() {try {
      using namespace navidog;using namespace navidog::sql::compiler;namespace opt=navidog::sql::optimizer;
      auto db=connectDatabase(profile_);auto token=cancellation();
      auto run=[&](const std::string &sql){auto r=db->execute(QString::fromStdString(sql),{},token);if(!r.error.isEmpty())throw std::runtime_error(r.error.toStdString());return r;};
      ScriptContext context(Dialect::SQLite,nullptr,{});
      auto create=[&](const std::string &sql){auto c=compile(Dialect::SQLite,sql,{},&context);QVERIFY2(c->ok(),c->diagnostics.empty()?"no model":c->diagnostics.front().message.c_str());QVERIFY(!context.observe(c->model,c->model.roots.back()));run(sql);};
      auto fired=[](const opt::OptimizationResult &r,const char *name){return std::any_of(r.rules.begin(),r.rules.end(),[&](const auto &rule){return rule.rule==name;});};
      for(const auto &entry:std::vector<std::pair<std::string,std::string>>{{"nullable_text_pk_owned","TEXT"},{"nullable_int_pk_owned","INT"}}) {
        create("CREATE TEMP TABLE "+entry.first+"(id "+entry.second+" PRIMARY KEY)");run("INSERT INTO "+entry.first+" VALUES(NULL),(NULL)");
        const auto sql="SELECT DISTINCT id FROM "+entry.first+" WHERE id IS NULL";
        auto c=compile(Dialect::SQLite,sql,{},&context);QVERIFY(c->ok());auto planned=opt::optimize(c->model);QVERIFY(planned.ok());
        QVERIFY(!fired(planned,"HepFilterNullability"));QVERIFY(!fired(planned,"HepDistinctRemove"));QCOMPARE(run(sql).rows.size(),1);
      }
      create("CREATE TEMP TABLE rowid_pk_owned(id INTEGER PRIMARY KEY)");run("INSERT INTO rowid_pk_owned VALUES(NULL),(NULL)");
      auto c=compile(Dialect::SQLite,"SELECT id FROM rowid_pk_owned WHERE id IS NULL",{},&context);QVERIFY(c->ok());auto planned=opt::optimize(c->model);QVERIFY(planned.ok());QVERIFY(fired(planned,"HepFilterNullability"));QCOMPARE(run("SELECT id FROM rowid_pk_owned WHERE id IS NULL").rows.size(),0);
      create("CREATE TEMP TABLE ctas_null_owned AS SELECT 1 AS id");run("INSERT INTO ctas_null_owned VALUES(NULL)");
      c=compile(Dialect::SQLite,"SELECT id FROM ctas_null_owned WHERE id IS NULL",{},&context);QVERIFY(c->ok());planned=opt::optimize(c->model);QVERIFY(planned.ok());QVERIFY(!fired(planned,"HepFilterNullability"));QCOMPARE(run("SELECT id FROM ctas_null_owned WHERE id IS NULL").rows.size(),1);
    }catch(const std::exception &e){QFAIL(e.what());}}
    void windowExclusionsMatchOwnedSqlitePeers() {try {
      using namespace navidog;using namespace navidog::sql::compiler;
      auto db=connectDatabase(profile_);auto token=cancellation();const auto dialect=Dialect::SQLite;
      for(const auto &mode:{QString("NO OTHERS"),QString("CURRENT ROW"),QString("GROUP"),QString("TIES")}) {
        const auto sql=QString("WITH t(x) AS(SELECT 1 UNION ALL SELECT 1 UNION ALL SELECT 2) SELECT x,COUNT(*) OVER(ORDER BY x ROWS BETWEEN UNBOUNDED PRECEDING AND UNBOUNDED FOLLOWING EXCLUDE %1) AS n FROM t ORDER BY x").arg(mode);
        auto original=db->execute(sql,{},token);QVERIFY2(original.error.isEmpty(),qPrintable(original.error));QCOMPARE(original.rows.size(),3);
        const int first=mode=="NO OTHERS"?3:mode=="GROUP"?1:2,last=mode=="CURRENT ROW"?2:mode=="GROUP"?2:3;
        QCOMPARE(original.rows[0][1].toInt(),first);QCOMPARE(original.rows[1][1].toInt(),first);QCOMPARE(original.rows[2][1].toInt(),last);
        auto c=compile(dialect,sql.toStdString());QVERIFY2(c->ok(),c->diagnostics.empty()?"no model":c->diagnostics.front().message.c_str());
        auto generated=generateSql(c->model,dialect);QVERIFY(generated.ok());auto actual=db->execute(QString::fromStdString(generated.sql),{},token);QVERIFY2(actual.error.isEmpty(),qPrintable(actual.error));QCOMPARE(actual.rows,original.rows);
      }
      auto boolean=compile(dialect,"SELECT COUNT(*) OVER(ROWS TRUE PRECEDING)");QVERIFY(boolean->ok());QVERIFY(db->execute("SELECT COUNT(*) OVER(ROWS TRUE PRECEDING)",{},token).error.isEmpty());
    }catch(const std::exception &e){QFAIL(e.what());}}
    void sqliteAggregateDomainsAndTotalRunOnOwnedDatabase() {try {
      using namespace navidog;using namespace navidog::sql::compiler;
      auto profile=profile_;profile.id="owned-aggregate-domains";profile.database=dir_.filePath("aggregate-owned.sqlite");QFile file(profile.database);QVERIFY(file.open(QIODevice::WriteOnly));file.close();
      auto db=connectDatabase(profile);auto token=cancellation();
      const auto sql="SELECT typeof(AVG(1)),typeof(SUM(1)),typeof(TOTAL(NULL)),TOTAL(NULL),COUNT(*)";
      auto actual=db->execute(sql,{},token);QVERIFY2(actual.error.isEmpty(),qPrintable(actual.error));
      QCOMPARE(actual.rows[0][0].toString(),QString("real"));QCOMPARE(actual.rows[0][1].toString(),QString("integer"));QCOMPARE(actual.rows[0][2].toString(),QString("real"));QCOMPARE(actual.rows[0][3].toDouble(),0.0);
      auto c=compile(Dialect::SQLite,"SELECT AVG(1),SUM(1),TOTAL(NULL),COUNT(*)");QVERIFY(c->ok());auto generated=generateSql(c->model,Dialect::SQLite);QVERIFY(generated.ok());
      auto canonical=db->execute(QString::fromStdString(generated.sql),{},token);QVERIFY(canonical.error.isEmpty());QCOMPARE(canonical.rows[0][2].toDouble(),0.0);
    }catch(const std::exception &e){QFAIL(e.what());}}
    void generatedCanonicalSqlRunsOnOwnedSqlite(){
        using namespace navidog;using namespace navidog::sql::compiler;auto db=connectDatabase(profile_);auto token=cancellation();
        auto run=[&](const std::string& sql){auto c=compile(Dialect::SQLite,sql);if(!c->ok())throw std::runtime_error(c->diagnostics.front().message);auto generated=generateSql(c->model,Dialect::SQLite);if(!generated.ok())throw std::runtime_error(generated.diagnostics.front().message);auto result=db->execute(QString::fromStdString(generated.sql),{},token);if(!result.error.isEmpty())throw std::runtime_error(result.error.toStdString());return result;};
        run("CREATE TEMP TABLE gen_items(id INTEGER PRIMARY KEY,title TEXT NOT NULL,n INTEGER DEFAULT 0,CHECK(n>=0))");
        run("INSERT INTO gen_items(id,title) VALUES(1,'one'),(2,'two')");run("UPDATE gen_items SET n=id+1 WHERE EXISTS(SELECT 1 FROM gen_items x WHERE x.id=gen_items.id)");
        run("CREATE INDEX gen_ix ON gen_items(n DESC) WHERE n>0");run("CREATE TEMP VIEW gen_view AS SELECT id,title,n FROM gen_items");
        auto r=run("SELECT id,title,n FROM gen_view ORDER BY id");QCOMPARE(r.rows.size(),2);QCOMPARE(r.rows[0][2].toInt(),2);
        run("DELETE FROM gen_items WHERE id IN(SELECT id FROM gen_items WHERE id=2)");run("ALTER TABLE gen_items ADD extra TEXT DEFAULT 'default'");
        r=run("SELECT title,extra FROM gen_items");QCOMPARE(r.rows.size(),1);QCOMPARE(r.rows[0][1].toString(),QString("default"));
        run("DROP VIEW gen_view");run("DROP INDEX gen_ix");run("ALTER TABLE gen_items RENAME COLUMN title TO label");run("DROP TABLE gen_items");
    }
    void sqliteVirtualTableArgumentsPreserveModuleInput() {try {
        using namespace navidog;
        using namespace navidog::sql::compiler;
        const std::string source="create virtual table temp.virtual_sql_owned using 'fts5'"
            "(title, /* visible to the formatter */ body   UNINDEXED, prefix  =   '2 3');";
        const auto compiled=compile(Dialect::SQLite,source);
        QVERIFY2(compiled->ok(),compiled->diagnostics.empty()?"no virtual table":compiled->diagnostics.front().message.c_str());
        const auto &model=compiled->model.front();
        QVERIFY(model.ddl && model.ddl->virtualTable && model.ddl->virtualTable->argumentList);
        QCOMPARE(model.ddl->virtualTable->module,std::string("fts5"));
        QCOMPARE(model.ddl->virtualTable->arguments.size(),std::size_t(3));
        QCOMPARE(model.ddl->virtualTable->arguments[1].payload,std::string("body   UNINDEXED"));
        QCOMPARE(model.ddl->virtualTable->arguments[2].payload,std::string("prefix  =   '2 3'"));
        const auto formatted=formatSql(Dialect::SQLite,source);
        QVERIFY2(formatted.ok(),formatted.diagnostics.empty()?"no format":formatted.diagnostics.front().message.c_str());
        QVERIFY(formatted.syntaxSupported && formatted.sql.find("CREATE VIRTUAL TABLE")!=std::string::npos);
        QVERIFY(formatted.sql.find("/* visible to the formatter */")!=std::string::npos);
        QVERIFY(formatted.sql.find("body   UNINDEXED")!=std::string::npos);
        QVERIFY(formatted.sql.find("prefix  =   '2 3'")!=std::string::npos);
        const auto generated=generateSql(compiled->model,Dialect::SQLite);
        QVERIFY2(generated.ok(),generated.diagnostics.empty()?"no SQL":generated.diagnostics.front().message.c_str());
        QVERIFY(!convertSql(Dialect::SQLite,Dialect::PostgreSQL,source).ok());
        auto invalid=compiled->model;
        auto altered=std::make_shared<BasicVirtualTable<std::string>>(*model.ddl->virtualTable);
        altered->arguments.back().payload="broken)";
        invalid.queries.front().ddl->virtualTable=std::move(altered);
        const auto refused=generateSql(invalid,Dialect::SQLite);
        QVERIFY(!refused.ok() && refused.sql.empty());
        const auto nested=compile(Dialect::SQLite,"CREATE VIRTUAL TABLE custom USING module((x, (y)), opt = 'a,b;');");
        QVERIFY(nested->ok());
        QCOMPARE(nested->model.front().ddl->virtualTable->arguments.front().payload,std::string("(x, (y))"));

        auto db=connectDatabase(profile_);
        auto token=cancellation();
        const auto run=[&](const QString &sql) {
            auto result=db->execute(sql,{},token);
            if (!result.error.isEmpty()) throw std::runtime_error(result.error.toStdString());
            return result;
        };
        if (!run("SELECT sqlite_compileoption_used('ENABLE_FTS5')").rows.front().front().toInt()) return;
        for (const auto &creation : {source, formatted.sql, generated.sql}) {
            run(QString::fromStdString(creation));
            run("INSERT INTO temp.virtual_sql_owned(title,body) VALUES ('fast search','other'),('notes','only searchable in body')");
            const auto rows=run("SELECT title FROM temp.virtual_sql_owned WHERE virtual_sql_owned MATCH 'sear*'").rows;
            QCOMPARE(rows.size(),1);
            QCOMPARE(rows.front().front().toString(),QString("fast search"));
            run("DROP TABLE temp.virtual_sql_owned");
        }
    }catch(const std::exception &e){QFAIL(e.what());}}
    void sqliteTriggerRowsBindAndExecuteAsOneStatement() {try {
        using namespace navidog;
        using namespace navidog::sql::compiler;
        auto profile=profile_;
        profile.id="owned-trigger-compiler";
        profile.database=dir_.filePath("trigger-compiler.sqlite");
        QFile file(profile.database);QVERIFY(file.open(QIODevice::WriteOnly));file.close();
        auto db=connectDatabase(profile);
        auto token=cancellation();
        const auto run=[&](const QString &sql) {
            auto result=db->execute(sql,{},token);
            if (!result.error.isEmpty()) throw std::runtime_error(result.error.toStdString());
            return result;
        };
        const std::string inventory="CREATE TABLE inventory(id INTEGER PRIMARY KEY,quantity INT);";
        const std::string events="CREATE TABLE inventory_events(id INT,before_value INT,after_value INT,note TEXT);";
        run(QString::fromStdString(inventory));
        run(QString::fromStdString(events));
        run("INSERT INTO inventory VALUES(1,7)");
        const std::string source="create trigger if not exists audit_qty after update of quantity,ignored_name on inventory "
            "for each row when NEW.quantity<>old.quantity begin "
            "insert into inventory_events values(NEW.id, old.quantity, NEW.quantity, ';');"
            "select case when NEW.quantity>0 then ';' else 'other' end;"
            "update inventory_events set note = note || 'changed' where id=NEW.id; end;";
        const auto compiled=compile(Dialect::SQLite,inventory+events+source);
        QVERIFY2(compiled->ok(),compiled->diagnostics.empty()?"missing trigger model":compiled->diagnostics.front().message.c_str());
        const auto &trigger=compiled->model.queries.at(compiled->model.roots.back()-1);
        QVERIFY(trigger.trigger && trigger.trigger->body.size()==3);
        QCOMPARE(trigger.tables.size(),std::size_t(2));
        QVERIFY(trigger.tables[0].pseudo && trigger.tables[1].pseudo);
        QVERIFY(trigger.transactionEffect==TransactionEffect::TransactionalDdl);
        const auto &insert=compiled->model.queries.at(trigger.trigger->body.front()-1);
        QCOMPARE(insert.scope.parent,std::optional<std::size_t>(trigger.id));
        const auto &oldValue=insert.expressions.at(insert.rows.front().at(1));
        const auto &newValue=insert.expressions.at(insert.rows.front().at(2));
        QVERIFY(oldValue.binding && newValue.binding && oldValue.binding->scope==trigger.id && newValue.binding->scope==trigger.id);
        QCOMPARE(oldValue.binding->table,std::size_t(0));
        QCOMPARE(newValue.binding->table,std::size_t(1));
        const auto formatted=formatSql(Dialect::SQLite,source);
        QVERIFY2(formatted.ok(),formatted.diagnostics.empty()?"no trigger format":formatted.diagnostics.front().message.c_str());
        QVERIFY(formatted.syntaxSupported && formatted.sql.find("\nBEGIN\n")!=std::string::npos);
        const auto converted=convertSql(Dialect::SQLite,Dialect::SQLite,source);
        QVERIFY2(converted.ok(),converted.diagnostics.empty()?"no trigger round trip":converted.diagnostics.front().message.c_str());
        QVERIFY(!convertSql(Dialect::SQLite,Dialect::PostgreSQL,source).ok());
        QVERIFY(convertSql(Dialect::SQLite,Dialect::SQLite,source+" SELECT ';';").ok());
        const auto dropped=convertSql(Dialect::SQLite,Dialect::SQLite,"DROP TRIGGER IF EXISTS audit_qty;");
        QVERIFY(dropped.ok());
        for (const auto &creation : {source,formatted.sql,converted.sql}) {
            const auto created=db->executeDefinition(QString::fromStdString(creation),{},token);
            QVERIFY2(created.error.isEmpty(),qPrintable(created.error));
            run("UPDATE inventory SET quantity=quantity WHERE id=1");
            QCOMPARE(run("SELECT count(*) FROM inventory_events").rows.front().front().toInt(),0);
            run("UPDATE inventory SET quantity=quantity+1 WHERE id=1");
            const auto rows=run("SELECT before_value,after_value,note FROM inventory_events").rows;
            QCOMPARE(rows.size(),1);QCOMPARE(rows[0][0].toInt(),7);QCOMPARE(rows[0][1].toInt(),8);
            QCOMPARE(rows[0][2].toString(),QString(";changed"));
            run(QString::fromStdString(dropped.sql));
            run("UPDATE inventory SET quantity=7");
            run("DELETE FROM inventory_events");
        }
        QueryService service;
        QSignalSpy finished(&service,&QueryService::completed);
        QueryExecutionOptions options;options.sessionId="owned-trigger-query";
        QVERIFY(service.run(profile,QString::fromStdString(source+" UPDATE inventory SET quantity=8; SELECT note FROM inventory_events;"),{},options));
        QTRY_COMPARE_WITH_TIMEOUT(finished.size(),1,3000);
        const auto output=qvariant_cast<QueryOutcome>(finished.takeFirst().front());
        QVERIFY2(output.result.error.isEmpty(),qPrintable(output.result.error));
        QCOMPARE(output.executedStatements,3);
        QCOMPARE(output.analysis.statements.front().state,SqlAnalysisState::Parsed);
        QCOMPARE(output.result.rows.front().front().toString(),QString(";changed"));
        run("ATTACH DATABASE ':memory:' AS attached");
        run("CREATE TABLE attached.inventory(id INT)");
        run("CREATE TABLE attached.changes(id INT)");
        const std::string good="CREATE TRIGGER attached.log_id AFTER INSERT ON attached.inventory "
                               "BEGIN INSERT INTO changes VALUES(NEW.id); END;";
        const auto generated=convertSql(Dialect::SQLite,Dialect::SQLite,good);
        QVERIFY(generated.ok());
        const auto created=db->executeDefinition(QString::fromStdString(generated.sql),{},token);
        QVERIFY2(created.error.isEmpty(),qPrintable(created.error));
        run("INSERT INTO attached.inventory VALUES(42)");
        QCOMPARE(run("SELECT id FROM attached.changes").rows.front().front().toInt(),42);
        const std::string bad="CREATE TRIGGER logs_wrong AFTER INSERT ON attached.inventory BEGIN SELECT NEW.id; END;";
        QVERIFY(!compile(Dialect::SQLite,bad)->ok());
        bool rejected=false;
        try { rejected=!db->executeDefinition(QString::fromStdString(bad),{},token).error.isEmpty(); }
        catch (const std::exception &) { rejected=true; }
        QVERIFY(rejected);
        QVERIFY(!compile(Dialect::SQLite,"CREATE TRIGGER logs_wrong AFTER INSERT ON inventory "
            "BEGIN SELECT id FROM attached.changes; END;")->ok());
    }catch(const std::exception &e){QFAIL(e.what());}}
    void localAstAnalysisDoesNotConnectExecuteOrChangeTransactions(){
        using namespace navidog;QueryService service;QSignalSpy done(&service,&QueryService::completed);QueryExecutionOptions options;options.sessionId="owned-ast-local";options.analyzeOnly=true;
        auto unreachable=profile_;unreachable.driver=DatabaseKind::PostgreSQL;unreachable.host="invalid.invalid";unreachable.database="never_connected";unreachable.password.clear();
        QVERIFY(service.run(unreachable,"INSERT INTO items(id) SELECT id FROM other; UPDATE items SET id=2; DELETE FROM items",{},options));QVERIFY(done.wait(3000));
        auto parsed=qvariant_cast<QueryOutcome>(done.takeFirst()[0]);QVERIFY(parsed.analyzedOnly);QVERIFY(parsed.result.error.isEmpty());QCOMPARE(parsed.attemptedStatements,0);QCOMPARE(parsed.executedStatements,0);QCOMPARE(service.sessionCount(),0);
        QCOMPARE(parsed.analysis.statements.size(),3);QCOMPARE(parsed.analysis.statements[0].kind,QString("INSERT"));QVERIFY(!parsed.analysis.statements[0].unifiedAst.isEmpty());QVERIFY(parsed.analysis.statements[0].generatedSql.contains("INSERT INTO"));QVERIFY(queryAnalysisText(parsed).contains("仅解析 · 未执行"));
        auto run=[&](const QString& sql){if(!service.run(profile_,sql,{},options) || !done.wait(3000))throw std::runtime_error("owned AST transaction test timed out");return qvariant_cast<QueryOutcome>(done.takeFirst()[0]);};
        options.analyzeOnly=false;options.autoCommit=false;auto r=run("CREATE TEMP TABLE ast_owned(n);INSERT INTO ast_owned VALUES(7)");QVERIFY(r.result.error.isEmpty());QCOMPARE(r.transactionState,TransactionState::Active);
        options.analyzeOnly=true;r=run("DELETE FROM ast_owned");QVERIFY(r.result.error.isEmpty());QCOMPARE(r.transactionState,TransactionState::Active);QCOMPARE(r.attemptedStatements,0);
        options.analyzeOnly=false;r=run("SELECT n FROM ast_owned");QVERIFY(r.result.error.isEmpty());QCOMPARE(r.result.rows[0][0].toInt(),7);
        options.transaction=TransactionAction::Rollback;r=run({});QVERIFY(r.result.error.isEmpty());QCOMPARE(r.transactionState,TransactionState::Idle);
    }
    void compilerAnalysisRunsInsideExecutionWithoutChangingSql(){
        using namespace navidog;QueryService service;QSignalSpy done(&service,&QueryService::completed);
        QueryExecutionOptions options;options.sessionId="owned-compiler-pipeline";
        auto run=[&](const QString& sql){
            if(!service.run(profile_,sql,{},options) || !done.wait(3000))throw std::runtime_error("owned compiler pipeline timeout");
            return qvariant_cast<QueryOutcome>(done.takeFirst()[0]);
        };
        auto result=run("CREATE TEMP TABLE compiler_pipeline(n); INSERT INTO compiler_pipeline VALUES(7); "
            "WITH q AS (SELECT n FROM compiler_pipeline) SELECT CASE WHEN n=7 THEN n ELSE 0 END, "
            "(SELECT count(*) FROM compiler_pipeline) AS total FROM q; "
            "SELECT n,sum(n) OVER(ROWS BETWEEN UNBOUNDED PRECEDING AND UNBOUNDED FOLLOWING EXCLUDE NO OTHERS) AS window_total FROM compiler_pipeline; "
            "UPDATE compiler_pipeline SET n=n+1; SELECT n FROM compiler_pipeline; "
            "WITH RECURSIVE owned_seq(n) AS(VALUES(1) UNION ALL SELECT n+1 FROM owned_seq WHERE n<8) SELECT MAX(n) FROM owned_seq;");
        QVERIFY2(result.result.error.isEmpty(),qPrintable(result.result.error));QCOMPARE(result.executedStatements,7);
        QCOMPARE(result.result.rows[0][0].toInt(),8);QCOMPARE(result.results[2].rows[0][0].toInt(),7);
        QCOMPARE(result.results[2].rows[0][1].toInt(),1);QCOMPARE(result.results[3].rows[0][1].toInt(),7);
        QCOMPARE(result.analysis.statements.size(),7);
        QCOMPARE(result.analysis.statements[2].state,SqlAnalysisState::Parsed);
        QCOMPARE(result.analysis.statements[2].queries,3);
        QCOMPARE(result.analysis.statements[3].state,SqlAnalysisState::Parsed);
        QVERIFY(QJsonDocument(result.analysis.statements[3].unifiedAst).toJson().contains("exclusion"));
        QCOMPARE(result.analysis.statements[6].state,SqlAnalysisState::Parsed);
        QVERIFY(QJsonDocument(result.analysis.statements[6].unifiedAst).toJson().contains("workingTableOf"));
        QCOMPARE(result.analysis.statements[1].state,SqlAnalysisState::Parsed);QCOMPARE(result.analysis.statements[4].state,SqlAnalysisState::Parsed);
        QCOMPARE(result.analysis.statements[4].kind,QString("UPDATE"));QVERIFY(!result.analysis.statements[4].unifiedAst.isEmpty());
        auto pragma=run("PRAGMA table_info(compiler_pipeline)");
        QVERIFY(pragma.result.error.isEmpty());QCOMPARE(pragma.executedStatements,1);
        QCOMPARE(pragma.analysis.statements.front().state,SqlAnalysisState::Parsed);
        QCOMPARE(pragma.analysis.statements.front().kind,QString("PRAGMA"));
        QCOMPARE(pragma.result.rows.front().at(1).toString(),QString("n"));
        auto settings=run("PRAGMA temp.cache_size = -23; PRAGMA temp.cache_size;");
        QVERIFY2(settings.result.error.isEmpty(),qPrintable(settings.result.error));
        QCOMPARE(settings.result.rows.front().front().toInt(),-23);
        auto literal=run("SELECT '/*literal*/;中文',length('中文')");
        QVERIFY(literal.result.error.isEmpty());QCOMPARE(literal.result.rows[0][0].toString(),QString("/*literal*/;中文"));
        QCOMPARE(literal.result.rows[0][1].toInt(),2);
        auto failed=run("SELECT 1; SELECT * FROM owned_missing_compiler_table; SELECT 3;");
        QVERIFY(!failed.result.error.isEmpty());QCOMPARE(failed.executedStatements,1);QCOMPARE(failed.failedStatement,1);
        QCOMPARE(failed.analysis.totalStatements,3);QVERIFY(queryAnalysisText(failed).contains("未执行"));
        auto invalid=run("SELECT FROM");QVERIFY(!invalid.result.error.isEmpty());QCOMPARE(invalid.executedStatements,0);QCOMPARE(invalid.attemptedStatements,1);QVERIFY(invalid.result.error.contains("FROM"));
        QCOMPARE(invalid.analysis.statements[0].state,SqlAnalysisState::NotCovered);
        QString many;for(int i=0;i<70;++i)many+=QString("SELECT %1;").arg(i);
        auto bounded=run(many);QVERIFY(bounded.result.error.isEmpty());QCOMPARE(bounded.executedStatements,70);
        QCOMPARE(bounded.result.rows[0][0].toInt(),69);QVERIFY(bounded.analysis.truncated);QCOMPARE(bounded.analysis.statements.size(),64);
    }
    void sqlServerMergeRetainsOriginalTerminatorAcrossAnalysisAndExecutionSlices(){
        using namespace navidog;const std::string source="MERGE INTO t USING s ON t.id=s.id WHEN MATCHED THEN DELETE; SELECT 1;";
        auto spans=sql::splitStatements(source,sql::Dialect::SQLServer);QCOMPARE(spans.size(),std::size_t(2));
        auto text=sql::statementSource(source,spans[0],sql::Dialect::SQLServer);QVERIFY(text.back()==';');
        QCOMPARE(sql::statementSource(source,spans[1],sql::Dialect::SQLServer),std::string_view(" SELECT 1;"));
        auto p=profile_;p.driver=DatabaseKind::SQLServer;
        auto analysis=analyzeQueryScript(p,{},source,spans,cancellation());QCOMPARE(analysis.statements[0].state,SqlAnalysisState::Parsed);
        const std::string absent="MERGE INTO t USING s ON t.id=s.id WHEN MATCHED THEN DELETE";
        auto missing=sql::splitStatements(absent,sql::Dialect::SQLServer);QVERIFY(sql::statementSource(absent,missing[0],sql::Dialect::SQLServer).back()!=';');
        analysis=analyzeQueryScript(p,{},absent,missing,cancellation());QCOMPARE(analysis.statements[0].state,SqlAnalysisState::NotCovered);
        const std::string procedure="CREATE PROC p @p INTEGER AS BEGIN SELECT @p; SELECT @p+1; END;\nGO\nSELECT 1;";
        auto batches=sql::splitStatements(procedure,sql::Dialect::SQLServer);QCOMPARE(batches.size(),std::size_t(2));QVERIFY(batches[0].compound);
    }
    void queryScriptAnalysisCarriesHypotheticalDeclarationsForward(){
        using namespace navidog;auto p=profile_;p.driver=DatabaseKind::PostgreSQL;
        const std::string source="CREATE TABLE analysis_owned(id INTEGER); SELECT missing FROM analysis_owned;";
        auto analysis=analyzeQueryScript(p,"public",source,sql::splitStatements(source,sql::Dialect::PostgreSQL),cancellation());
        QCOMPARE(analysis.statements.size(),2);QCOMPARE(analysis.statements[0].state,SqlAnalysisState::Parsed);
        QCOMPARE(analysis.statements[1].state,SqlAnalysisState::NotCovered);QVERIFY(analysis.statements[1].detail.contains("missing"));
        const std::string functions="CREATE FUNCTION analysis_f(p INTEGER) RETURNS INTEGER LANGUAGE plpgsql AS $$ BEGIN RETURN p; END; $$; SELECT analysis_f();";
        analysis=analyzeQueryScript(p,"public",functions,sql::splitStatements(functions,sql::Dialect::PostgreSQL),cancellation());
        QCOMPARE(analysis.statements.size(),2);QCOMPARE(analysis.statements[0].state,SqlAnalysisState::Parsed);
        QCOMPARE(analysis.statements[1].state,SqlAnalysisState::NotCovered);QVERIFY(analysis.statements[1].detail.contains("argument"));
    }
    void routineQueryAnalysisExposesScopesBindingsAndNestedSqlWithoutConnecting(){
        using namespace navidog;
        const QVector<QPair<DatabaseKind,QString>> cases={
          {DatabaseKind::MySQL,"CREATE FUNCTION ast_owned(p INTEGER) RETURNS INTEGER DETERMINISTIC RETURN p+1;"},
          {DatabaseKind::PostgreSQL,"CREATE FUNCTION ast_owned(p INTEGER) RETURNS INTEGER LANGUAGE plpgsql AS $$ DECLARE v INTEGER:=p; BEGIN RETURN v+1; END; $$;"},
          {DatabaseKind::Oracle,"CREATE FUNCTION ast_owned(p INTEGER) RETURN INTEGER AS v INTEGER:=p; BEGIN RETURN v+1; END;"},
          {DatabaseKind::SQLServer,"CREATE FUNCTION ast_owned(@p INTEGER) RETURNS INTEGER AS BEGIN RETURN @p+1; END;"}};
        for(const auto &sample:cases) {
          auto profile=profile_;profile.driver=sample.first;auto source=sample.second.toStdString();
          auto analysis=analyzeQueryScript(profile,{},source,{{0,source.size(),true}},cancellation());
          QCOMPARE(analysis.statements.size(),1);const auto &item=analysis.statements[0];
          QVERIFY2(item.state==SqlAnalysisState::Parsed,qPrintable(item.detail));QVERIFY(item.queries>0);
          QVERIFY(item.unifiedAst.contains("scopes"));QVERIFY(item.unifiedAst.contains("symbols"));
          const auto nodes=item.unifiedAst["nodes"].toArray();bool code=false;
          for(const auto &node:nodes)if(node.toObject()["code"].isObject()){code=true;QVERIFY(node.toObject()["origin"].isArray());}
          QVERIFY(code);QVERIFY(!item.generatedSql.isEmpty());
        }
    }
    void queryAnalysisUsesDialectContextAndHasBounds(){
        using namespace navidog;auto token=cancellation();
        const QVector<QPair<DatabaseKind,QString>> cases={{DatabaseKind::MySQL,"SELECT `id` FROM `items` LIMIT 1,2"},
            {DatabaseKind::PostgreSQL,"SELECT id::numeric FROM items"}, {DatabaseKind::Oracle,"SELECT q'[value]' FROM dual FETCH FIRST 1 ROW ONLY"},
            {DatabaseKind::SQLServer,"SELECT TOP (2) [id] FROM [items]"}, {DatabaseKind::SQLite,"SELECT 1"},
            {DatabaseKind::MariaDB,"SELECT `id` FROM items LIMIT 2"}, {DatabaseKind::OceanBase,"SELECT `id` FROM items LIMIT 2"}};
        for(const auto& sample:cases){auto p=profile_;p.driver=sample.first;auto bytes=sample.second.toUtf8();
            auto report=analyzeQueryScript(p,"owned_schema",std::string_view(bytes.constData(),bytes.size()),{{0,std::size_t(bytes.size()),false}},token);
            QCOMPARE(report.statements.size(),1);QCOMPARE(report.statements[0].state,SqlAnalysisState::Parsed);}
        std::string source="SELECT 1;\r\nSELECT * FROM";
        auto report=analyzeQueryScript(profile_,{},source,sql::splitStatements(source,sql::Dialect::SQLite),token);
        QCOMPARE(report.statements[1].line,2);QVERIFY(report.statements[1].byteColumn>1);
        token->store(true);auto stopped=analyzeQueryScript(profile_,{},"SELECT 1",{{0,8,false}},token);
        QCOMPARE(stopped.statements[0].state,SqlAnalysisState::Stopped);token->store(false);
        std::string huge(2*1024*1024+1,' ');auto skipped=analyzeQueryScript(profile_,{},huge,{{0,huge.size(),false}},token);
        QVERIFY(skipped.truncated);QCOMPARE(skipped.totalStatements,1);QVERIFY(skipped.statements.isEmpty());
        std::string longStatement(1024*1024+1,' ');auto limited=analyzeQueryScript(profile_,{},longStatement,{{0,longStatement.size(),false}},token);
        QCOMPARE(limited.statements[0].state,SqlAnalysisState::LimitReached);
    }
    void catalogLoadingDoesNotBlockQueryLanesAndCancelIsScoped(){
        using namespace navidog;QueryService service;QSignalSpy queries(&service,&QueryService::completed),catalogs(&service,&QueryService::catalogCompleted);CatalogNode node;node.kind="connection";QVERIFY(service.catalog(profile_,node,"owned-context-list"));QVERIFY(service.catalogBusy());QVERIFY(!service.operationBusy());QueryExecutionOptions options;options.sessionId="owned-context-parallel";QVERIFY(service.run(profile_,"SELECT 53",{},options));service.cancelCatalog();QTRY_COMPARE_WITH_TIMEOUT(catalogs.count(),1,3000);QTRY_COMPARE_WITH_TIMEOUT(queries.count(),1,3000);auto result=qvariant_cast<QueryOutcome>(queries[0][0]);QVERIFY2(result.result.error.isEmpty(),qPrintable(result.result.error));QVERIFY(!result.result.cancelled);QCOMPARE(result.result.rows[0][0].toInt(),53);QVERIFY(!service.catalogBusy());QVERIFY(!service.operationBusy());
    }
    void initTestCase() {
        QVERIFY(dir_.isValid()); profile_.id = "test"; profile_.name = "Test";
        profile_.database = navidog::ensureDemoDatabase(dir_.path());
        QCOMPARE(navidog::ensureDemoDatabase(dir_.path()), profile_.database);
    }
    void connectionOptionsRoundTripExcludesSecrets(){
        using namespace navidog;ConnectionProfile p;p.driver=DatabaseKind::MySQL;p.notes="owned notes";p.password="runtime-db-secret";p.ssh.password="runtime-ssh-secret";p.ssh.passphrase="runtime-key-secret";p.http.password="runtime-http-secret";p.http.proxyPassword="runtime-proxy-secret";p.ssh.hostKey="ssh-ed25519 owned-public-pin";p.advanced.keepAlive=true;p.advanced.connectTimeout=17;p.advanced.maxSessions=3;p.ssl.serverName="db.example";p.savePassword=true;p.ssh.savePassword=true;p.http.base64=true;auto json=connectionOptionsJson(p);auto bytes=QJsonDocument(json).toJson();QVERIFY(!bytes.contains("runtime-"));ConnectionProfile actual;actual.driver=p.driver;readConnectionOptions(actual,json);QCOMPARE(actual.notes,p.notes);QCOMPARE(actual.advanced,p.advanced);QCOMPARE(actual.ssl,p.ssl);QCOMPARE(actual.ssh.hostKey,p.ssh.hostKey);QVERIFY(actual.ssh.password.isEmpty());QVERIFY(actual.savePassword);QVERIFY(actual.http.base64);
        p.ssh.enabled=true;p.http.enabled=true;QVERIFY_THROWS_EXCEPTION(std::runtime_error,validateConnectionOptions(p));
    }
    void sqliteQueryTypesUseDeclarationsEvenForEmptyResults(){
        auto db=navidog::connectDatabase(profile_);auto result=db->execute("SELECT id,order_no FROM orders WHERE 0",{},navidog::cancellation());QVERIFY(result.rows.isEmpty());QCOMPARE(result.columnTypes,QStringList({"INTEGER","TEXT"}));result=db->execute("SELECT 1 AS expression",{},navidog::cancellation());QCOMPARE(result.columnTypes.size(),1);QVERIFY(result.columnTypes[0].isEmpty());
    }
    void sqlDocumentsPreserveEncodingsAndRejectConcurrentFileChanges(){
        using namespace navidog;QTemporaryDir owned;const auto text=QString::fromUtf8("-- 中文与 emoji 😀\nSELECT 'a;b';\n");
        int index=0;for(auto encoding:{ScriptEncoding::Utf8,ScriptEncoding::Utf8Bom,ScriptEncoding::Utf16LE,ScriptEncoding::Utf16BE}){
            ScriptDocument original;original.encoding=encoding;original.crlf=true;auto path=owned.filePath(QString::number(index++)+".sql");auto saved=writeScriptDocument(original,path,text);auto loaded=readScriptDocument(path);QCOMPARE(loaded.text,text);QCOMPARE(loaded.encoding,encoding);QVERIFY(loaded.crlf);QCOMPARE(loaded.fingerprint,saved.fingerprint);
            QFile external(path);QVERIFY(external.open(QIODevice::WriteOnly|QIODevice::Truncate));external.write("SELECT 'external';");external.close();QVERIFY_THROWS_EXCEPTION(ScriptConflict,writeScriptDocument(loaded,path,"SELECT 'mine';"));QCOMPARE(readScriptDocument(path).text,QString("SELECT 'external';"));
            saved=writeScriptDocument(loaded,path,text,true);QCOMPARE(readScriptDocument(path).text,text);QVERIFY(QFile::remove(path));QVERIFY_THROWS_EXCEPTION(ScriptConflict,writeScriptDocument(saved,path,text));
        }
        auto path=owned.filePath("invalid.sql");QFile invalid(path);QVERIFY(invalid.open(QIODevice::WriteOnly));invalid.write(QByteArray::fromHex("c028"));invalid.close();QVERIFY_THROWS_EXCEPTION(std::runtime_error,readScriptDocument(path));
        QVERIFY(invalid.open(QIODevice::WriteOnly|QIODevice::Truncate));invalid.write(QByteArray::fromHex("fffe00d8"));invalid.close();QVERIFY_THROWS_EXCEPTION(std::runtime_error,readScriptDocument(path));
        QVERIFY(invalid.open(QIODevice::WriteOnly|QIODevice::Truncate));invalid.write(QByteArray(4*1024*1024+1,'a'));invalid.close();QVERIFY_THROWS_EXCEPTION(std::runtime_error,readScriptDocument(path));
    }
    void executionPlansDoNotExecuteDmlOrCommitQueryTransactions(){
        using namespace navidog;auto token=cancellation();PlanRequestTrace trace;auto plan=readQueryPlan(profile_,"DELETE FROM orders WHERE id=1",{},token,{}, {},&trace);QVERIFY(trace.issued && trace.received);QVERIFY(trace.commands.front().startsWith("EXPLAIN QUERY PLAN DELETE"));QVERIFY(!trace.serverVersion.isEmpty());QVERIFY2(plan.error.isEmpty(),qPrintable(plan.error));QVERIFY(!plan.rows.isEmpty());auto db=connectDatabase(profile_);QCOMPARE(db->execute("SELECT count(*) FROM orders",{},token).rows[0][0].toInt(),100000);
        QVERIFY_THROWS_EXCEPTION(std::runtime_error,readQueryPlan(profile_,"DELETE FROM orders; DELETE FROM customers",{},token));QVERIFY_THROWS_EXCEPTION(std::runtime_error,readQueryPlan(profile_,"DROP TABLE orders",{},token));
        QueryService service;QSignalSpy done(&service,&QueryService::completed);QueryExecutionOptions options;options.sessionId="owned-plan-tx";options.autoCommit=false;auto run=[&](QString text){if(!service.run(profile_,text,{},options) || !done.wait(3000))throw std::runtime_error("owned plan query failed");return qvariant_cast<QueryOutcome>(done.takeFirst()[0]);};
        auto result=run("CREATE TEMP TABLE plan_uncommitted(n); INSERT INTO plan_uncommitted VALUES(1)");QCOMPARE(result.transactionState,TransactionState::Active);options.explain=true;result=run("SELECT * FROM orders WHERE id=1");QVERIFY(result.explained);QVERIFY2(result.result.error.isEmpty(),qPrintable(result.result.error));QCOMPARE(result.transactionState,TransactionState::Active);options.explain=false;result=run("SELECT n FROM plan_uncommitted");QCOMPARE(result.result.rows[0][0].toInt(),1);options.transaction=TransactionAction::Rollback;result=run({});QCOMPARE(result.transactionState,TransactionState::Idle);
    }
    void generatedStatementDraftsQuoteNamesAndGuardUnfilledWrites(){
        using namespace navidog;auto p=profile_;p.driver=DatabaseKind::PostgreSQL;TableDefinition d;d.node.kind="table";d.node.schema="odd.schema";d.node.object="a\"b";TableColumn id;id.name="id";id.primaryKey=1;id.autoIncrement=true;d.columns<<id;TableColumn text;text.name="display name";d.columns<<text;TableColumn generated;generated.name="computed";generated.generatedExpression="1";d.columns<<generated;
        auto select=statementTemplate(p,d,StatementTemplate::Select);QVERIFY(select.contains("\"odd.schema\".\"a\"\"b\""));
        auto insert=statementTemplate(p,d,StatementTemplate::Insert);QVERIFY(!insert.contains("\"id\""));QVERIFY(!insert.contains("computed"));
        auto update=statementTemplate(p,d,StatementTemplate::Update);QVERIFY(update.contains("WHERE \"id\" = NULL"));QVERIFY(update.contains("SET \"display name\" = NULL"));d.columns[0].primaryKey=0;QVERIFY(statementTemplate(p,d,StatementTemplate::Delete).contains("WHERE 1 = 0"));
    }
    void sessionLimitsCoverDirectConnectionsAndReleaseFailedOpens(){
        using namespace navidog;auto p=profile_;p.id="owned-limit";p.advanced.maxSessions=1;
        auto first=connectDatabase(p);QVERIFY_THROWS_EXCEPTION(std::runtime_error,connectDatabase(p));
        first.reset();auto next=connectDatabase(p);QCOMPARE(next->execute("SELECT 1",{},cancellation()).rows[0][0].toInt(),1);next.reset();
        auto missing=p;missing.database=dir_.filePath("owned-no-file");QVERIFY_THROWS_EXCEPTION(std::runtime_error,connectDatabase(missing));
        next=connectDatabase(p);QVERIFY(next!=nullptr);
    }
    void initialScriptsPreserveSessionAndQuotedDelimiters(){
        using namespace navidog;auto p=profile_;p.id="owned-initial";p.advanced.initialSql="CREATE TEMP TABLE owned_initial(n TEXT); INSERT INTO owned_initial VALUES('a;b');";
        auto db=connectDatabase(p);auto r=db->execute("SELECT n FROM owned_initial",{},cancellation());QCOMPARE(r.rows[0][0].toString(),QString("a;b"));db.reset();
        p.advanced.maxSessions=1;p.advanced.initialSql="SELECT 1; SELECT missing_owned_column;";QVERIFY_THROWS_EXCEPTION(std::runtime_error,connectDatabase(p));
        p.advanced.initialSql.clear();QVERIFY(connectDatabase(p)!=nullptr);
    }
    void oracleDescriptorsCoverSidServiceTnsAndTls(){
        using namespace navidog;ConnectionProfile p;p.driver=DatabaseKind::Oracle;p.host="db.example";p.database="FREEPDB1";p.port=1521;auto text=oracleConnectDescriptor(p);QVERIFY(text.contains("(SERVICE_NAME=FREEPDB1)"));p.oracle.addressKind=OracleAddressKind::SID;QVERIFY(oracleConnectDescriptor(p).contains("(SID=FREEPDB1)"));p.transportHost="127.0.0.1";p.transportPort=41111;QVERIFY(oracleConnectDescriptor(p).contains("(HOST=127.0.0.1)(PORT=41111)"));p.tls=true;p.ssl.verification=SslVerification::Identity;p.ssl.serverDN="CN=db.example";QVERIFY(oracleConnectDescriptor(p).contains("(SSL_SERVER_CERT_DN=\"CN=db.example\")"));p.oracle.addressKind=OracleAddressKind::TNS;p.database="owned_alias";QCOMPARE(oracleConnectDescriptor(p),QString("owned_alias"));p.ssh.enabled=true;QVERIFY_THROWS_EXCEPTION(std::runtime_error,oracleConnectDescriptor(p));p.ssh.enabled=false;p.database="alias)(HOST=evil";QVERIFY_THROWS_EXCEPTION(std::runtime_error,oracleConnectDescriptor(p));
    }
    void scriptsRespectQuotedBodiesAndClientDelimiters(){
        using namespace navidog::sql;auto text=std::string("SELECT ';'; -- ; comment\n SELECT 2;");auto spans=splitStatements(text,Dialect::SQLite);QCOMPARE(spans.size(),std::size_t(2));QCOMPARE(text.substr(spans[1].begin,spans[1].end-spans[1].begin),std::string(" -- ; comment\n SELECT 2"));
        text="CREATE TRIGGER owned AFTER INSERT ON t BEGIN SELECT CASE WHEN 1 THEN 2 END; SELECT 2; END; SELECT 3;";spans=splitStatements(text,Dialect::SQLite);QCOMPARE(spans.size(),std::size_t(2));QVERIFY(spans[0].compound);QVERIFY(!spans[1].compound);
        text="DELIMITER $$\nCREATE PROCEDURE owned() BEGIN SELECT 1; SELECT 'x$$y'; END$$\nDELIMITER ;\nCALL owned();";spans=splitStatements(text,Dialect::MySQL);QCOMPARE(spans.size(),std::size_t(2));QVERIFY(spans[0].compound);QVERIFY(text.substr(spans[0].begin,spans[0].end-spans[0].begin).find("END")!=std::string::npos);
        text="DELIMITER $ND123$\nALTER DEFINER=`owner`@`localhost` EVENT `scheduled` DO BEGIN SELECT 1; SELECT 2; END\n$ND123$\nDELIMITER ;\nSELECT 3;";
        spans=splitStatements(text,Dialect::MySQL);
        QCOMPARE(spans.size(),std::size_t(2));
        QVERIFY(spans[0].compound);
        QVERIFY(!spans[1].compound);
        text="CREATE FUNCTION owned() RETURNS int LANGUAGE plpgsql AS $body$BEGIN RETURN 1; END;$body$; SELECT 2;";QCOMPARE(splitStatements(text,Dialect::PostgreSQL).size(),std::size_t(2));
        text="\nBEGIN NULL; NULL; END;\n/\nSELECT 1 FROM dual;";spans=splitStatements(text,Dialect::Oracle);QCOMPARE(spans.size(),std::size_t(2));QVERIFY(spans[0].compound);
        QVERIFY_THROWS_EXCEPTION(std::runtime_error,splitStatements("CREATE PROCEDURE p() BEGIN SELECT 1; END;",Dialect::MySQL));
        text=std::string(100000,' ')+"SELECT 1; SELECT 2";QCOMPARE(splitStatements(text,Dialect::SQLite).size(),std::size_t(2));
    }
    void queryLanesRunConcurrentlyCancelIndependentlyAndBoundConcurrency(){
        using namespace navidog;QueryService service;QSignalSpy done(&service,&QueryService::completed);QueryExecutionOptions slow,fast;slow.sessionId="owned-slow";fast.sessionId="owned-fast";
        const QString work="WITH RECURSIVE t(x) AS (VALUES(1) UNION ALL SELECT x+1 FROM t WHERE x<1000000000) SELECT sum(x) FROM t";
        QVERIFY(service.run(profile_,work,{},slow));QVERIFY(service.busy());QVERIFY(!service.operationBusy());QTRY_COMPARE_WITH_TIMEOUT(service.sessionCount(),1,3000);QVERIFY(service.run(profile_,"CREATE TEMP TABLE lane_owned(n); INSERT INTO lane_owned VALUES(23); SELECT n FROM lane_owned",{},fast));
        QTRY_COMPARE_WITH_TIMEOUT(done.count(),1,3000);auto quick=qvariant_cast<QueryOutcome>(done.takeFirst()[0]);QCOMPARE(quick.sessionId,fast.sessionId);QVERIFY2(quick.result.error.isEmpty(),qPrintable(quick.result.error));QCOMPARE(quick.result.rows[0][0].toInt(),23);QVERIFY(service.busy());
        service.cancelQuery(profile_.id,profile_.database,slow.sessionId);QTRY_COMPARE_WITH_TIMEOUT(done.count(),1,3000);auto stopped=qvariant_cast<QueryOutcome>(done.takeFirst()[0]);QCOMPARE(stopped.sessionId,slow.sessionId);QVERIFY(stopped.result.cancelled);QVERIFY(!service.busy());
        QVERIFY(service.run(profile_,"SELECT n FROM lane_owned",{},fast));QTRY_COMPARE_WITH_TIMEOUT(done.count(),1,3000);QCOMPARE(qvariant_cast<QueryOutcome>(done.takeFirst()[0]).result.rows[0][0].toInt(),23);
        for(int i=0;i<4;++i){QueryExecutionOptions option;option.sessionId="owned-cap-"+QString::number(i);QVERIFY(service.run(profile_,work,{},option));}
        QueryExecutionOptions fifth;fifth.sessionId="owned-fifth";QVERIFY(!service.run(profile_,"SELECT 1",{},fifth));service.cancel();QTRY_COMPARE_WITH_TIMEOUT(done.count(),4,3000);for(auto event:done)QVERIFY(qvariant_cast<QueryOutcome>(event[0]).result.cancelled);done.clear();
        service.disconnectProfile(profile_.id);QTRY_COMPARE_WITH_TIMEOUT(service.sessionCount(),0,3000);QTRY_VERIFY(!service.busy());
    }
    void queryScriptsKeepIndependentSessionsAndRollbackAfterErrors(){
        using namespace navidog;QueryService service;QSignalSpy done(&service,&QueryService::completed);auto options=QueryExecutionOptions{};options.sessionId="owned-one";auto run=[&](QString sql){bool accepted=service.run(profile_,sql,{},options);if(!accepted)throw std::runtime_error("service busy");if(!done.wait(3000))throw std::runtime_error("timed out");return qvariant_cast<QueryOutcome>(done.takeFirst()[0]);};
        auto result=run("CREATE TEMP TABLE owned_session(n); INSERT INTO owned_session VALUES(7); SELECT n FROM owned_session; SELECT 8;");QVERIFY2(result.result.error.isEmpty(),qPrintable(result.result.error));QCOMPARE(result.executedStatements,4);QCOMPARE(result.results.size(),4);QCOMPARE(result.results[2].rows[0][0].toInt(),7);QCOMPARE(result.results[3].rows[0][0].toInt(),8);
        options.autoCommit=false;result=run("INSERT INTO owned_session VALUES(9)");QCOMPARE(result.transactionState,TransactionState::Active);result=run("bad sql owned");QVERIFY(!result.result.error.isEmpty());QCOMPARE(result.transactionState,TransactionState::Active);QCOMPARE(service.sessionCount(),1);
        options.transaction=TransactionAction::Rollback;result=run({});QVERIFY(result.result.error.isEmpty());QCOMPARE(result.transactionState,TransactionState::Idle);options.transaction=TransactionAction::None;options.autoCommit=true;result=run("SELECT count(*) FROM owned_session");QCOMPARE(result.result.rows[0][0].toInt(),1);
        options.sessionId="owned-two";result=run("SELECT * FROM owned_session");QVERIFY(!result.result.error.isEmpty());QCOMPARE(service.sessionCount(),2);options.sessionId="owned-one";result=run("SELECT count(*) FROM owned_session");QCOMPARE(result.result.rows[0][0].toInt(),1);service.disconnectSession(profile_.id,profile_.database,options.sessionId);QTRY_COMPARE(service.sessionCount(),1);
    }
    void cachedResultExportsPreserveEscapingBinaryAndLargeIntegers(){
        using namespace navidog;QueryResult r;r.columns={"quote","binary","big","empty","null"};r.rows={{QString("x'\\y\nz"),QByteArray::fromHex("00ff"),QVariant::fromValue<qlonglong>(9223372036854775806LL),QString(""),QVariant{}}};auto bytes=exportResult(r,ResultExportFormat::Json,DatabaseKind::SQLite);auto json=QJsonDocument::fromJson(bytes).object();auto row=json["rows"].toArray()[0].toArray();QCOMPARE(row[2].toObject()["integer"].toString(),QString("9223372036854775806"));QCOMPARE(row[1].toObject()["base64"].toString(),QString::fromLatin1(QByteArray::fromHex("00ff").toBase64()));QVERIFY(row[4].isNull());QCOMPARE(row[3].toString(),QString(""));
        QTemporaryDir own;ConnectionProfile p;p.database=ensureDemoDatabase(own.path());auto db=connectDatabase(p);db->execute("CREATE TABLE exported_owned(quote TEXT,binary BLOB,big INTEGER,empty TEXT,null_value TEXT)",{},cancellation());r.columns[4]="null_value";auto sql=exportResult(r,ResultExportFormat::Sql,DatabaseKind::SQLite,"exported_owned");auto actual=db->execute(QString::fromUtf8(sql),{},cancellation());QVERIFY(actual.error.isEmpty());actual=db->execute("SELECT quote,binary,big,empty,null_value FROM exported_owned",{},cancellation());QCOMPARE(actual.rows[0],r.rows[0]);auto mysql=exportResult(r,ResultExportFormat::Sql,DatabaseKind::MySQL,"owned");QVERIFY(mysql.contains("CONVERT(X'"));QVERIFY(mysql.contains("X'00ff'"));
    }
    void sqliteViewDesignerPreservesTriggersAndRollsBackFailures(){
        using namespace navidog;QTemporaryDir own;auto p=profile_;p.database=ensureDemoDatabase(own.path());auto token=cancellation();auto db=connectDatabase(p);CatalogNode node;node.kind="view";node.schema="main";node.object=node.name="view_owned";SourceDefinition empty;auto result=applySourceChanges(p,empty,node,sourceTemplate(p,node),false,token);QVERIFY2(result.applied,qPrintable(result.error));QVERIFY(result.refreshError.isEmpty());QVERIFY(result.definition.sql.contains("CREATE VIEW"));db->executeDefinition("CREATE TRIGGER view_owned_insert INSTEAD OF INSERT ON view_owned BEGIN SELECT NEW.id; END",{},token);
        auto before=readSourceDefinition(*db,p,node,token);auto sql=rewriteSourceTarget("CREATE VIEW old_name AS SELECT 2 AS id",p,node);result=applySourceChanges(p,before,node,sql,false,token);QVERIFY2(result.applied,qPrintable(result.error));QCOMPARE(db->execute("SELECT id FROM view_owned",{},token).rows[0][0].toInt(),2);QCOMPARE(db->execute("SELECT count(*) FROM sqlite_schema WHERE type='trigger' AND name='view_owned_insert'",{},token).rows[0][0].toInt(),1);
        before=result.definition;result=applySourceChanges(p,before,node,"CREATE VIEW view_owned AS SELECT FROM invalid",false,token);QVERIFY(!result.applied);QVERIFY(result.rolledBack);QCOMPARE(db->execute("SELECT id FROM view_owned",{},token).rows[0][0].toInt(),2);result=applySourceChanges(p,before,node,{},true,token);QVERIFY2(result.deleted,qPrintable(result.error));QCOMPARE(db->execute("SELECT count(*) FROM sqlite_schema WHERE name='view_owned'",{},token).rows[0][0].toInt(),0);
    }
    void routineCallScriptsHandleInOutAndOracleImplicitResults(){
        using namespace navidog;ConnectionProfile p;p.driver=DatabaseKind::MySQL;SourceDefinition d;d.node.kind="procedure";d.node.database="owned";d.node.object="call_owned";CatalogNode input;input.name="input";input.detail="IN int";CatalogNode output;output.name="output";output.detail="OUT int";d.parameters={input,output};auto text=sourceCallSql(p,d);QVERIFY(text.contains("SET @p1"));QVERIFY(text.contains("CALL `owned`.`call_owned`(@p1, @p2)"));QVERIFY(text.contains("SELECT @p2 AS `output`"));
        p.driver=DatabaseKind::Oracle;d.node.schema="OWNED";input.detail="IN NUMBER";output.detail="OUT NUMBER";d.parameters={input,output};text=sourceCallSql(p,d);QVERIFY(text.contains("v_p2 NUMBER"));QVERIFY(text.contains("OPEN v_output FOR SELECT v_p2"));QVERIFY(text.contains("DBMS_SQL.RETURN_RESULT(v_output)"));QCOMPARE(sql::splitStatements(text.toStdString(),sql::Dialect::Oracle).size(),std::size_t(1));p.version=DatabaseVersion::Oracle11;QVERIFY_THROWS_EXCEPTION(std::runtime_error,sourceCallSql(p,d));
        p.version=DatabaseVersion::Auto;p.driver=DatabaseKind::PostgreSQL;d.parameters={input,output};text=sourceCallSql(p,d);QVERIFY(text.contains("CALL"));QCOMPARE(text.count("NULL"),2);
    }
    void sampleHas100000Rows() {
        auto db = navidog::connectDatabase(profile_);
        auto result = db->execute("SELECT count(*) FROM orders", {}, navidog::cancellation());
        QVERIFY(result.error.isEmpty()); QCOMPARE(result.rows[0][0].toInt(), 100000);
        QVERIFY(db->tables().contains("customers")); QVERIFY(db->tables().contains("orders"));
    }
    void rowCap() {
        auto db = navidog::connectDatabase(profile_); navidog::QueryLimits limits; limits.maxRows = 20;
        auto r = db->execute("SELECT * FROM orders", limits, navidog::cancellation());
        QCOMPARE(r.rows.size(), 20); QVERIFY(r.truncated); QVERIFY(r.cachedBytes <= limits.maxBytes);
    }
    void sqliteBoundParametersPreserveTypesAndDoNotExecuteValues() {
        auto db=navidog::connectDatabase(profile_);auto token=navidog::cancellation();QString text="'); DROP TABLE orders; --";auto blob=QByteArray::fromHex("0001ff");
        auto r=db->executeBound("SELECT ?1,?2,?3,?4,?5",{text,blob,QVariant{},QString(""),QVariant::fromValue<qlonglong>(9223372036854775806LL)},{},token);
        QCOMPARE(r.rows[0][0].toString(),text);QCOMPARE(r.rows[0][1].toByteArray(),blob);QVERIFY(!r.rows[0][2].isValid());QVERIFY(r.rows[0][3].isValid());QCOMPARE(r.rows[0][4].toLongLong(),9223372036854775806LL);QVERIFY(db->tables().contains("orders"));
        QVERIFY_THROWS_EXCEPTION(std::runtime_error,db->executeBound("SELECT ?1",{}, {},token));
    }
    void sqliteDataEditingPagingCommitConflictAndRollback() {
        QTemporaryDir owned;auto profile=profile_;profile.database=navidog::ensureDemoDatabase(owned.path());
        using namespace navidog;auto db=connectDatabase(profile);auto token=cancellation();
        db->execute("CREATE TABLE data_owned(id INTEGER PRIMARY KEY,label TEXT UNIQUE NOT NULL DEFAULT 'new',note TEXT,payload BLOB)",{},token);
        for(int i=1;i<=6;++i){auto result=db->executeBound("INSERT INTO data_owned(id,label,note,payload) VALUES(?1,?2,?3,?4)",{i,"item-"+QString::number(i),i==1?QVariant{}:QVariant(QString("")),QByteArray::fromHex("00ff")},{},token);QVERIFY(result.error.isEmpty());}
        DataPageRequest request;request.node.kind="table";request.node.schema="main";request.node.name=request.node.object="data_owned";request.pageSize=2;
        auto first=readDataPage(profile,request,token);QVERIFY2(first.error.isEmpty(),qPrintable(first.error));QVERIFY(first.editable);QVERIFY(first.hasMore);QCOMPARE(first.result.rows.size(),2);QCOMPARE(first.result.rows[0][0].toInt(),1);
        auto next=request;next.offset=2;next.afterKey={first.result.rows.last()[0]};auto second=readDataPage(profile,next,token);QVERIFY2(second.error.isEmpty(),qPrintable(second.error));QVERIFY(second.keyset);QCOMPARE(second.result.rows[0][0].toInt(),3);
        auto filter=request;filter.filterColumn=1;filter.filter=DataFilter::Contains;filter.filterValue="item-%";auto escaped=readDataPage(profile,filter,token);QVERIFY(escaped.error.isEmpty());QCOMPARE(escaped.result.rows.size(),0);
        auto sort=request;sort.sortColumn=0;sort.descending=true;auto descending=readDataPage(profile,sort,token);QCOMPARE(descending.result.rows[0][0].toInt(),6);
        DataRowChange update;update.state=DataRowState::Modified;update.original=update.values=first.result.rows[0];update.values[1]="'); DELETE FROM data_owned; --";update.values[2]="";update.values[3]=QByteArray("",0);update.touched={1,2,3};
        DataRowChange remove;remove.state=DataRowState::Deleted;remove.original=remove.values=first.result.rows[1];
        DataRowChange insert;insert.state=DataRowState::Inserted;insert.values.resize(4);insert.values[1]="inserted";insert.values[2]={};insert.touched={1,2};
        auto result=commitDataChanges(profile,first.schema,{update,remove,insert},token);QVERIFY2(result.error.isEmpty(),qPrintable(result.error));QCOMPARE(result.updated,1);QCOMPARE(result.inserted,1);QCOMPARE(result.deleted,1);
        auto actual=db->execute("SELECT label,note,payload FROM data_owned WHERE id=1",{},token);QCOMPARE(actual.rows[0][0].toString(),update.values[1].toString());QVERIFY(actual.rows[0][1].isValid());QCOMPARE(actual.rows[0][1].toString(),QString(""));QCOMPARE(actual.rows[0][2].toByteArray().size(),0);
        first=readDataPage(profile,request,token);update.original=update.values=first.result.rows[0];update.values[1]="stale write";update.touched={1};db->execute("UPDATE data_owned SET note='external' WHERE id=1",{},token);
        result=commitDataChanges(profile,first.schema,{update},token);QVERIFY(result.error.contains("其他会话"));QVERIFY(result.rolledBack);
        first=readDataPage(profile,request,token);update.original=update.values=first.result.rows[0];update.values[1]="should rollback";update.touched={1};insert.values[0]=1;insert.values[1]="duplicate";insert.touched={0,1};
        result=commitDataChanges(profile,first.schema,{update,insert},token);QVERIFY(!result.error.isEmpty());QVERIFY(result.rolledBack);QVERIFY(!result.commitUnknown);actual=db->execute("SELECT label FROM data_owned WHERE id=1",{},token);QCOMPARE(actual.rows[0][0].toString(),first.result.rows[0][1].toString());
        auto readonly=profile;readonly.readOnly=true;QVERIFY(!readDataPage(readonly,request,token).editable);QVERIFY(!commitDataChanges(readonly,first.schema,{update},token).error.isEmpty());
        auto cancelled=cancellation();cancelled->store(true);result=commitDataChanges(profile,first.schema,{update},cancelled);QVERIFY(result.cancelled);QVERIFY(!result.error.isEmpty());
    }
    void dataPagesWithoutKeysViewsAndTruncationAreReadOnly() {
        QTemporaryDir owned;auto profile=profile_;profile.database=navidog::ensureDemoDatabase(owned.path());
        using namespace navidog;auto db=connectDatabase(profile);auto token=cancellation();db->execute("CREATE TABLE no_key_owned(note TEXT)",{},token);db->execute("CREATE VIEW view_owned AS SELECT id,order_no FROM orders",{},token);
        DataPageRequest request;request.node.kind="table";request.node.schema="main";request.node.object="no_key_owned";auto page=readDataPage(profile,request,token);QVERIFY(page.error.isEmpty());QVERIFY(!page.editable);QVERIFY(page.readOnlyReason.contains("主键"));
        request.node.kind="view";request.node.object="view_owned";page=readDataPage(profile,request,token);QVERIFY(page.error.isEmpty());QVERIFY(!page.editable);
        db->execute("CREATE TABLE large_owned(id INTEGER PRIMARY KEY,note TEXT)",{},token);db->execute("INSERT INTO large_owned VALUES(1,printf('%0100000d',1))",{},token);request.node.kind="table";request.node.object="large_owned";page=readDataPage(profile,request,token);QVERIFY(page.result.cellTruncated);QVERIFY(!page.editable);
    }
    void byteAndCellCap() {
        auto db = navidog::connectDatabase(profile_); navidog::QueryLimits limits; limits.maxBytes = 256; limits.maxCellBytes = 16;
        auto r = db->execute("SELECT printf('%010000d', id) FROM orders", limits, navidog::cancellation());
        QVERIFY(r.cellTruncated); QVERIFY(r.truncated); QVERIFY(r.cachedBytes <= limits.maxBytes);
    }
    void nullAndBlob() {
        auto db = navidog::connectDatabase(profile_);
        auto r = db->execute("SELECT NULL, '', X'00FF', 42, 1.25", {}, navidog::cancellation());
        QVERIFY(!r.rows[0][0].isValid()); QVERIFY(r.rows[0][1].isValid()); QCOMPARE(r.rows[0][1].toString(), QString());
        QCOMPARE(r.rows[0][2].toByteArray(), QByteArray::fromHex("00ff")); QCOMPARE(r.rows[0][3].toInt(), 42);
    }
    void realSyntaxErrors() {
        auto db = navidog::connectDatabase(profile_);
        QVERIFY_THROWS_EXCEPTION(std::runtime_error, db->execute("SELEC nope", {}, navidog::cancellation()));
    }
    void connectionDoesNotCreateMissingFile() {
        auto p = profile_; p.database = dir_.filePath("missing.db");
        QVERIFY_THROWS_EXCEPTION(std::runtime_error, navidog::connectDatabase(p));
        QVERIFY(!QFile::exists(p.database));
    }
    void demoDoesNotOverwriteUnknownFile() {
        QTemporaryDir other; QFile file(other.filePath("harbor-demo.db")); QVERIFY(file.open(QIODevice::WriteOnly)); file.write("do not overwrite"); file.close();
        QVERIFY_THROWS_EXCEPTION(std::runtime_error, navidog::ensureDemoDatabase(other.path()));
        QVERIFY(file.open(QIODevice::ReadOnly)); QCOMPARE(file.readAll(), QByteArray("do not overwrite"));
    }
    void profilesNeverSavePasswords() {
        auto p = profile_; p.password = "private-test-secret-123";
        navidog::ProfileStore store(dir_.filePath("profiles.json")); store.save({p});
        QFile file(dir_.filePath("profiles.json")); QVERIFY(file.open(QIODevice::ReadOnly));
        QVERIFY(!file.readAll().contains(p.password.toUtf8()));
        auto loaded = store.load(); QCOMPARE(loaded.size(), 1); QVERIFY(loaded[0].password.isEmpty()); QCOMPARE(loaded[0].database, p.database);
    }

    void connectionArchivePreservesOptionsAndProtectsEveryCredential() {
        using namespace navidog;
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        const auto credential = [] { return QUuid::createUuid().toString(QUuid::WithoutBraces) + " 密码 "; };
        const auto sourceDirectory = directory.filePath("source");
        const auto sourcePath = localStorePath(sourceDirectory);
        ProfileStore source(sourcePath);
        ConnectionProfile ssh;
        ssh.id = "owned-ssh";
        ssh.name = "SSH connection";
        ssh.groupId = "source-group";
        ssh.driver = DatabaseKind::Dameng;
        ssh.host = "example.invalid";
        ssh.port = 5236;
        ssh.user = "owned";
        ssh.database = "owned";
        ssh.notes = "配置测试";
        ssh.jdbcDriverJar = "/fixture/dm.jar";
        ssh.advanced.connectTimeout = 19;
        ssh.advanced.autoConnect = true;
        ssh.advanced.initialSql = "SELECT 1";
        ssh.ssl.verification = SslVerification::Identity;
        ssh.ssl.damengKeystoreDirectory = "/fixture/certs";
        ssh.ssl.damengKeystorePassword = credential();
        ssh.ssl.saveDamengKeystorePassword = true;
        ssh.ssh.enabled = true;
        ssh.ssh.host = "ssh.example.invalid";
        ssh.ssh.port = 2222;
        ssh.ssh.user = "ssh-user";
        ssh.ssh.authentication = SshAuthentication::PasswordAndKey;
        ssh.ssh.keyFile = "/fixture/id_ed25519";
        ssh.ssh.knownHostsFile = "/fixture/known_hosts";
        ssh.ssh.hostKey = "public-key-pin";
        ssh.ssh.password = credential();
        ssh.ssh.passphrase = credential();
        ssh.ssh.savePassword = ssh.ssh.savePassphrase = true;
        ssh.password = credential();
        ssh.savePassword = true;
        ssh.credentialSource = CredentialSource::LocalEncrypted;
        ssh.secretsLoaded = true;
        ssh.credentialStorePath = sourcePath;
        ConnectionProfile http;
        http.id = "owned-http";
        http.name = "HTTP connection";
        http.groupId = ssh.groupId;
        http.driver = DatabaseKind::PostgreSQL;
        http.host = "example.invalid";
        http.user = "tester";
        http.tls = true;
        http.ssl.caFile = "/fixture/ca.pem";
        http.ssl.certificateFile = "/fixture/client.pem";
        http.ssl.keyFile = "/fixture/client-key.pem";
        http.ssl.tlsVersions = "TLSv1.3";
        http.http.enabled = http.http.authentication = http.http.base64 = http.http.proxyEnabled = true;
        http.http.url = "https://example.invalid/tunnel";
        http.http.user = "http-user";
        http.http.password = credential();
        http.http.proxyHost = "proxy.example.invalid";
        http.http.proxyPort = 3128;
        http.http.proxyUser = "proxy-user";
        http.http.proxyPassword = credential();
        const auto passphrase = credential();
        source.saveWorkspace({{ssh, http}, {{ssh.groupId, "Team A"}, {"empty", "Empty group"}}});
        auto archived = source.loadWorkspace();
        QVERIFY(archived.connections[0].password.isEmpty());
        archived.connections[1].http.password = http.http.password;
        archived.connections[1].http.proxyPassword = http.http.proxyPassword;
        const auto without = directory.filePath("connections.json");
        const auto with = directory.filePath("connections.ndconn");
        exportConnectionArchive(without, archived);
        exportConnectionArchive(with, archived, passphrase);
        const auto plain = readConnectionArchiveFile(without);
        const auto encrypted = readConnectionArchiveFile(with);
        QVERIFY(!connectionArchiveIsEncrypted(plain));
        QVERIFY(connectionArchiveIsEncrypted(encrypted));
        for (const auto& value : {ssh.password, ssh.ssh.password, ssh.ssh.passphrase,
                                  ssh.ssl.damengKeystorePassword, http.http.password, http.http.proxyPassword}) {
            QVERIFY(!plain.contains(value.toUtf8()));
            QVERIFY(!encrypted.contains(value.toUtf8()));
        }
        QVERIFY(!QJsonDocument::fromJson(plain).object()["connections"].toArray()[0].toObject().contains("secrets"));
        const auto sanitized = decodeConnectionArchive(plain);
        QCOMPARE(sanitized.groups.size(), 2);
        QCOMPARE(sanitized.connections.size(), 2);
        QVERIFY(sanitized.connections[0].password.isEmpty());
        QVERIFY(!sanitized.connections[0].savePassword);
        QCOMPARE(sanitized.connections[0].advanced, ssh.advanced);
        QCOMPARE(sanitized.connections[0].ssh.knownHostsFile, ssh.ssh.knownHostsFile);
        QCOMPARE(sanitized.connections[0].ssl.damengKeystoreDirectory, ssh.ssl.damengKeystoreDirectory);
        QCOMPARE(sanitized.connections[1].ssl, http.ssl);
        auto imported = decodeConnectionArchive(encrypted, passphrase);
        QCOMPARE(imported.connections[0], ssh);
        QCOMPARE(imported.connections[1].http.password, http.http.password);
        QCOMPARE(imported.connections[1].http.proxyPassword, http.http.proxyPassword);
        QCOMPARE(imported.connections[1].http.proxyPort, http.http.proxyPort);
        QVERIFY(imported.connections[1].http.savePassword);
        QVERIFY_THROWS_EXCEPTION(std::runtime_error, decodeConnectionArchive(encrypted, "incorrect"));
        auto tampered = encrypted;
        tampered.data()[45] ^= 1;
        QVERIFY_THROWS_EXCEPTION(std::runtime_error, decodeConnectionArchive(tampered, passphrase));

        const auto destinationPath = localStorePath(directory.filePath("destination"));
        ProfileStore destination(destinationPath);
        ConnectionProfile existing;
        existing.id = "existing";
        existing.name = ssh.name;
        existing.groupId = "destination-group";
        existing.database = directory.filePath("example.db");
        auto merged = mergeConnectionArchive({{existing}, {{existing.groupId, "team a"}}},
                                             std::move(imported), destinationPath);
        destination.saveWorkspace(merged);
        QCOMPARE(merged.connections[0], existing);
        QCOMPARE(merged.connections[1].name, ssh.name + " (2)");
        QCOMPARE(merged.connections[1].groupId, existing.groupId);
        QVERIFY(merged.connections[1].id != ssh.id);
        QVERIFY(!merged.connections[1].advanced.autoConnect);
        QCOMPARE(merged.groups.size(), 2);
        auto restored = destination.loadWorkspace();
        loadConnectionSecrets(restored.connections[1]);
        loadConnectionSecrets(restored.connections[2]);
        QCOMPARE(restored.connections[1].password, ssh.password);
        QCOMPARE(restored.connections[1].ssh, ssh.ssh);
        QCOMPARE(restored.connections[1].ssl, ssh.ssl);
        QCOMPARE(restored.connections[2].http.proxyPassword, http.http.proxyPassword);
        const auto stored = LocalStore(destinationPath).document("connections");
        QVERIFY(stored && !stored->contains(ssh.password.toUtf8()) && !stored->contains(http.http.proxyPassword.toUtf8()));
    }

    void workspaceMigrationAndEmptyGroups() {
        const auto path=dir_.filePath("legacy.json"); navidog::ProfileStore store(path); store.save({profile_});
        QFile file(path); QVERIFY(file.open(QIODevice::ReadOnly)); auto json=QJsonDocument::fromJson(file.readAll()); file.close();
        QVERIFY(file.open(QIODevice::WriteOnly|QIODevice::Truncate)); file.write(QJsonDocument(json.object()["connections"].toArray()).toJson()); file.close();
        auto workspace=store.loadWorkspace(); QCOMPARE(workspace.connections.size(),1); QVERIFY(workspace.groups.isEmpty());
        workspace.groups={{"dev","Development"},{"empty","Empty group"}}; workspace.connections[0].groupId="dev"; workspace.connections[0].password="secret-unpersisted";
        store.saveWorkspace(workspace); auto restored=store.loadWorkspace(); QCOMPARE(restored.groups.size(),2); QCOMPARE(restored.connections[0].groupId,QString("dev")); QVERIFY(restored.connections[0].password.isEmpty());
        store.save(restored.connections); QCOMPARE(store.loadWorkspace().groups.size(),2);
    }
    void nativeDriverRegistry() {
        auto drivers=navidog::driverRegistry(); QCOMPARE(drivers.size(),26); QVector<navidog::DatabaseKind> ids;
        for(const auto& d:drivers) { QVERIFY(!d.provider.isEmpty()); QVERIFY(!d.status.isEmpty()); ids<<d.id; }
        QVERIFY(ids.contains(navidog::DatabaseKind::Oracle)); QVERIFY(ids.contains(navidog::DatabaseKind::SQLServer)); QVERIFY(ids.contains(navidog::DatabaseKind::OceanBase));
        const auto tidb = std::find_if(drivers.begin(), drivers.end(), [](const auto &driver) {
            return driver.id == navidog::DatabaseKind::TiDB;
        });
        QVERIFY(tidb != drivers.end());
        QCOMPARE(tidb->defaultPort, 4000);
        const auto dameng = std::find_if(drivers.begin(), drivers.end(), [](const auto& driver) {
            return driver.id == navidog::DatabaseKind::Dameng;
        });
        QVERIFY(dameng != drivers.end());
        QCOMPARE(dameng->defaultPort, 5236);
    }
    void sqliteCatalogUsesRealMetadataAndQuotedNames() {
        auto db=navidog::connectDatabase(profile_); auto cancel=navidog::cancellation(); navidog::CatalogNode root;
        auto databases=navidog::loadCatalog(*db,profile_,root,cancel); QCOMPARE(databases.nodes.size(),1);
        auto folders=navidog::loadCatalog(*db,profile_,databases.nodes[0],cancel); QCOMPARE(folders.nodes.size(),3);
        auto tables=navidog::loadCatalog(*db,profile_,folders.nodes[0],cancel); QCOMPARE(tables.nodes.size(),2);
        auto children=navidog::loadCatalog(*db,profile_,tables.nodes[1],cancel); QCOMPARE(children.nodes.size(),5);
        auto columns=navidog::loadCatalog(*db,profile_,children.nodes[0],cancel); QVERIFY(columns.nodes.size()>=6); QCOMPARE(columns.nodes[0].name,QString("id"));
        auto indexes=navidog::loadCatalog(*db,profile_,children.nodes[1],cancel); QVERIFY(indexes.nodes.size()>0);
        auto functions=navidog::loadCatalog(*db,profile_,folders.nodes[2],cancel); QVERIFY(functions.nodes.size()>0);
        db->execute("CREATE TEMP TABLE \"odd.name'\" (\"semi;column\" INTEGER)",{},cancel);
        navidog::CatalogNode n; n.kind="table"; n.object="odd.name'";
        auto preview=db->execute(navidog::previewSql(profile_,n),{},cancel); QCOMPARE(preview.columns[0],QString("semi;column"));
        auto p=profile_; p.driver=navidog::DatabaseKind::SQLServer; n.schema="a.b]"; QCOMPARE(navidog::previewSql(p,n),QString("SELECT TOP (100) * FROM [a.b]]].[odd.name'];"));
    }
    void catalogAsyncAndRenamingKeepsSession() {
        navidog::QueryService service; QSignalSpy queries(&service,&navidog::QueryService::completed), catalogs(&service,&navidog::QueryService::catalogCompleted);
        QVERIFY(service.run(profile_,"CREATE TEMP TABLE rename_test(value)")); QTRY_COMPARE_WITH_TIMEOUT(queries.count(),1,3000); queries.clear();
        auto renamed=profile_; renamed.name="Renamed"; renamed.groupId="some-group";
        QVERIFY(service.run(renamed,"INSERT INTO rename_test VALUES(42)")); QTRY_COMPARE_WITH_TIMEOUT(queries.count(),1,3000);
        QVERIFY(qvariant_cast<navidog::QueryOutcome>(queries.takeFirst()[0]).result.error.isEmpty()); QCOMPARE(service.sessionCount(),1);
        QVERIFY(service.catalog(renamed,{},"request-1")); QVERIFY(!service.run(renamed,"SELECT 1")); QTRY_COMPARE_WITH_TIMEOUT(catalogs.count(),1,3000);
        QCOMPARE(catalogs[0][0].toString(),QString("request-1")); QCOMPARE(qvariant_cast<navidog::CatalogResult>(catalogs[0][1]).nodes.size(),1);
        service.disconnectProfile(profile_.id); QTRY_COMPARE_WITH_TIMEOUT(service.sessionCount(),0,3000);
    }

    void oceanBaseOracleProfilesChooseOracleSql() {
        using namespace navidog;
        using K = DatabaseKind;
        using V = DatabaseVersion;
        QCOMPARE(databaseKindFromKey("oceanbase"), K::OceanBase);
        QCOMPARE(databaseKindFromKey("oceanbase-oracle"), K::OceanBaseOracle);
        QVERIFY(!isMySqlFamily(K::OceanBaseOracle));
        QVERIFY(usesOracleSql(K::OceanBaseOracle));
        QCOMPARE(detectVersion(K::OceanBaseOracle, "OceanBase 4.3.5.5"), V::OceanBase4);
        QVERIFY(versionBelongsTo(V::OceanBase4, K::OceanBaseOracle));
        QVERIFY(!versionBelongsTo(V::Oracle19, K::OceanBaseOracle));
        ConnectionProfile mode;
        mode.id = "ob-oracle-test";
        mode.name = "OceanBase Oracle fixture";
        mode.driver = K::OceanBaseOracle;
        mode.port = 2881;
        mode.version = V::OceanBase4;
        mode.user = "APP@tenant#cluster";
        validateConnectionOptions(mode);
        auto invalid = mode;
        invalid.user = "APP";
        QVERIFY_THROWS_EXCEPTION(std::runtime_error, validateConnectionOptions(invalid));
        invalid = mode;
        invalid.ssl.verification = SslVerification::Identity;
        QVERIFY_THROWS_EXCEPTION(std::runtime_error, validateConnectionOptions(invalid));

        QTemporaryDir temporary;
        ProfileStore store(temporary.filePath("connections.json"));
        store.save({mode});
        const auto recovered=store.load();
        QCOMPARE(recovered.size(),1);
        QCOMPARE(recovered.front().driver,K::OceanBaseOracle);
        QCOMPARE(recovered.front().user,mode.user);
        CatalogNode table;
        table.kind="table";table.schema="APP";table.object="ORDERS";
        QCOMPARE(previewSql(mode, table),QString("SELECT * FROM \"APP\".\"ORDERS\" FETCH FIRST 100 ROWS ONLY"));
        const std::string text="SELECT 1 FROM DUAL FETCH FIRST 1 ROWS ONLY";
        const auto spans=sql::splitStatements(text,sql::Dialect::Oracle);
        const auto result=analyzeQueryScript(mode,"APP",text,spans,cancellation());
        QCOMPARE(result.statements.size(),1);
        QCOMPARE(result.statements.front().state,SqlAnalysisState::Parsed);
        QCOMPARE(result.statements.front().kind,QString("SELECT"));
    }

    void oceanBaseOracleRejectsMySqlTenant() {
        using namespace navidog;
        bool configured=false;
        const auto port=qEnvironmentVariableIntValue("NAVIDOG_TEST_OCEANBASE_MYSQL_PORT",&configured);
        if(!configured)QSKIP("Provide a disposable localhost OceanBase CE test port");
        QVERIFY(port>0 && port<=65535);
        QVERIFY(availableDrivers().contains(DatabaseKind::OceanBaseOracle));
        ConnectionProfile profile;
        profile.id="ob-mode-mismatch-test";
        profile.driver=DatabaseKind::OceanBaseOracle;
        profile.host="127.0.0.1";
        profile.port=port;
        profile.user="root@test";
        profile.tls=false;
        profile.credentialSource=CredentialSource::Passwordless;
        try {
            const auto db=connectDatabase(profile);
            QFAIL("Oracle mode accepted a MySQL tenant");
        } catch(const std::exception& error) {
            QVERIFY2(QString::fromUtf8(error.what()).contains("服务端租户不是 Oracle 模式"),error.what());
        }
        profile.driver=DatabaseKind::OceanBase;
        const auto db=connectDatabase(profile);
        QVERIFY(db->serverVersion().startsWith("4."));
        const auto result=db->execute("SELECT 1",{},cancellation());
        QVERIFY(!result.rows.isEmpty());
        QCOMPARE(result.rows.front().front().toInt(),1);
    }

    void distributedNativeEngines() {
        using namespace navidog;
        using K = DatabaseKind;
        using V = DatabaseVersion;
        namespace sc = navidog::sql::compiler;
        bool checked = false;
        for (const auto kind : {K::TiDB, K::OceanBase}) {
            bool hasPort = false;
            const auto port = qEnvironmentVariableIntValue(
                kind == K::TiDB ? "NAVIDOG_TEST_TIDB_PORT" : "NAVIDOG_TEST_OCEANBASE_PORT", &hasPort);
            if (!hasPort) continue;
            QVERIFY(port > 0 && port < 65536);
            checked = true;
            const bool tidb = kind == K::TiDB;
            const auto dialect = tidb ? sc::Dialect::TiDB : sc::Dialect::OceanBase;
            ConnectionProfile profile;
            profile.id = tidb ? "tidb-native-test" : "oceanbase-native-test";
            profile.driver = kind;
            profile.host = "127.0.0.1";
            profile.port = port;
            profile.user = tidb ? "root" : "root@test";
            profile.database = "navidog_demo";
            profile.tls = false;
            profile.credentialSource = CredentialSource::Passwordless;
            const auto cancel = cancellation();
            try {
                auto db = connectDatabase(profile);
                QCOMPARE(detectVersion(kind, db->serverVersion()), tidb ? V::TiDB8 : V::OceanBase4);
                const auto run = [&](const char *source) {
                    const auto compilation = sc::compile(dialect, source);
                    if (!compilation->ok())
                        throw std::runtime_error(compilation->diagnostics.front().message);
                    const auto command = sc::generateSql(compilation->model, dialect);
                    if (!command.ok()) throw std::runtime_error(command.diagnostics.front().message);
                    auto result = db->execute(QString::fromStdString(command.sql), {}, cancel);
                    if (!result.error.isEmpty()) throw std::runtime_error(result.error.toStdString());
                    return result;
                };
                if (tidb) {
                    run("CREATE TABLE IF NOT EXISTS dialect_probe (id BIGINT PRIMARY KEY CLUSTERED "
                        "AUTO_RANDOM(5,54), item_count INT) PRE_SPLIT_REGIONS=2");
                    const auto raw = run("SHOW CREATE TABLE dialect_probe");
                    QVERIFY(!raw.rows.isEmpty());
                    auto ddl = sc::compile(dialect, raw.rows.front().at(1).toString().toStdString());
                    QVERIFY2(ddl->ok(), ddl->diagnostics.empty() ? "invalid definition" :
                             ddl->diagnostics.front().message.c_str());
                    QVERIFY(ddl->model.front().ddl->definitions.front().autoRandom.has_value());
                    const auto jobs = run("ADMIN SHOW DDL JOBS 10 WHERE TABLE_NAME = 'dialect_probe'");
                    QVERIFY(jobs.columns.contains("JOB_ID"));
                    const auto plan = decodeEstimatedPlan(kind, readQueryPlan(profile,
                        "SELECT * FROM dialect_probe WHERE item_count=4", {}, cancel));
                    QVERIFY2(plan.complete, qPrintable(plan.warning));
                } else {
                    run("CREATE TABLEGROUP IF NOT EXISTS navidog_probe_group SHARDING='NONE'");
                    run("CREATE TABLE IF NOT EXISTS dialect_probe (id INT PRIMARY KEY, item_count INT) "
                        "TABLEGROUP=navidog_probe_group");
                    run("CREATE INDEX dialect_probe_local ON dialect_probe(item_count) LOCAL");
                    const auto result = run("SHOW CREATE TABLEGROUP navidog_probe_group");
                    QVERIFY(!result.rows.isEmpty());
                    run("ALTER TABLE dialect_probe SET TABLEGROUP=''");
                    run("DROP TABLEGROUP navidog_probe_group");
                }
                auto snapshot = std::make_shared<const MetadataSnapshot>(collectMetadata(*db, profile, profile.database, cancel));
                const MetadataCatalog catalog(snapshot);
                const sc::NameResolutionContext names{"navidog_demo", "navidog_demo", dialect};
                const auto table = catalog.findTable(names, {"dialect_probe"});
                QVERIFY(table.has_value());
                QVERIFY(table->complete);
                const auto invalid = sc::compile(dialect, "SELECT missing_column FROM dialect_probe", {},
                                                 &catalog, names);
                QVERIFY(!invalid->ok());
                QVERIFY(std::any_of(invalid->diagnostics.begin(), invalid->diagnostics.end(), [](const auto &error) {
                    return error.code == sc::DiagnosticCode::UnknownColumn;
                }));
                run("DROP TABLE dialect_probe");
            } catch (const std::exception &error) {
                QFAIL(error.what());
            }
        }
        if (!checked) QSKIP("Set a dedicated localhost port for a disposable TiDB or OceanBase server");
    }

    void connectionTestsUseIsolatedSessionsAndFailureDoesNotCreateFiles() {
        navidog::QueryService service;QSignalSpy queries(&service,&navidog::QueryService::completed),tests(&service,&navidog::QueryService::connectionTestCompleted);
        QVERIFY(service.run(profile_,"CREATE TEMP TABLE connection_test_guard(id INTEGER)"));QTRY_COMPARE_WITH_TIMEOUT(queries.count(),1,3000);QVERIFY(service.isConnected(profile_.id));
        QVERIFY(service.testConnection(profile_,"success"));QTRY_COMPARE_WITH_TIMEOUT(tests.count(),1,3000);QVERIFY(qvariant_cast<navidog::QueryOutcome>(tests[0][1]).result.error.isEmpty());QCOMPARE(service.sessionCount(),1);QVERIFY(service.isConnected(profile_.id));
        auto bad=profile_;bad.database=profile_.database+".missing";QVERIFY(service.testConnection(bad,"failure"));QTRY_COMPARE_WITH_TIMEOUT(tests.count(),2,3000);QVERIFY(!qvariant_cast<navidog::QueryOutcome>(tests[1][1]).result.error.isEmpty());QVERIFY(!QFileInfo::exists(bad.database));QCOMPARE(service.sessionCount(),1);
        QVERIFY(service.run(profile_,"SELECT * FROM connection_test_guard"));QTRY_COMPARE_WITH_TIMEOUT(queries.count(),2,3000);QVERIFY(qvariant_cast<navidog::QueryOutcome>(queries[1][0]).result.error.isEmpty());
        service.disconnectProfile(profile_.id);QTRY_VERIFY_WITH_TIMEOUT(!service.isConnected(profile_.id),3000);QCOMPARE(service.sessionCount(),0);
    }
    void databaseEnumsAndVersionDetection() {
        using K=navidog::DatabaseKind;using V=navidog::DatabaseVersion;
        for(auto kind:{K::SQLite,K::MySQL,K::PostgreSQL,K::Oracle,K::OceanBase,K::SQLServer,K::MariaDB,K::TiDB}) {
            QCOMPARE(navidog::databaseKindFromKey(navidog::databaseKindKey(kind)),kind);
            for(auto v:navidog::versionsFor(kind)) {QCOMPARE(navidog::versionFromKey(navidog::versionKey(v.version)),v.version);QVERIFY(navidog::versionBelongsTo(v.version,kind));}
        }
        QCOMPARE(navidog::databaseKindFromKey("mispelled-driver"),K::Unknown);
        QCOMPARE(navidog::detectVersion(K::MySQL,"8.4.9"),V::MySQL8_4);
        QCOMPARE(navidog::detectVersion(K::MySQL,"8.0.46"),V::MySQL8_0);
        QCOMPARE(navidog::detectVersion(K::PostgreSQL,"15.18 (Alpine)"),V::PostgreSQL15);
        QCOMPARE(navidog::detectVersion(K::Oracle,"23.26.1"),V::Oracle23);
        QCOMPARE(navidog::detectVersion(K::OceanBase,"5.7.25-OceanBase_CE-v4.3.5.5"),V::OceanBase4);
        QCOMPARE(navidog::detectVersion(K::TiDB,"8.0.11-TiDB-v8.5.3"),V::TiDB8);
        QCOMPARE(navidog::detectVersion(K::TiDB,"5.7.25-TiDB-v7.5.2"),V::TiDB7);
        QCOMPARE(navidog::detectVersion(K::TiDB,"8.0.46"),V::Unknown);
        QCOMPARE(navidog::detectVersion(K::MySQL,"8.0.11-TiDB-v8.5.3"),V::Unknown);
        QCOMPARE(navidog::detectVersion(K::SQLServer,"16.00.1000"),V::SQLServer2022);
        QCOMPARE(navidog::detectVersion(K::MySQL,"10.11.0-MariaDB"),V::Unknown);
        QCOMPARE(navidog::detectVersion(K::MariaDB,"5.5.5-11.4.11-MariaDB-ubu2404"),V::MariaDB11_4);QCOMPARE(navidog::detectVersion(K::MariaDB,"10.11.9-MariaDB"),V::MariaDB10_11);
        QVERIFY(navidog::isMySqlFamily(K::MariaDB));QVERIFY(!navidog::isMySqlFamily(K::PostgreSQL));QCOMPARE(navidog::quoteIdentifier("a`b",K::MariaDB),QString("`a``b`"));
        QCOMPARE(navidog::detectVersion(K::PostgreSQL,"unknown"),V::Unknown);
        QVERIFY(!navidog::versionBelongsTo(V::MySQL8_4,K::PostgreSQL));
        auto bad=profile_;bad.version=V::MySQL8_4;QVERIFY_THROWS_EXCEPTION(std::runtime_error,navidog::connectDatabase(bad));
    }
    void tableActionsCopyRenameClearAndProtectExistingObjects() {
        using namespace navidog;QTemporaryDir owned;auto p=profile_;p.database=ensureDemoDatabase(owned.path());auto db=connectDatabase(p);auto cancel=cancellation();
        QVERIFY(db->execute("CREATE TABLE actions_owned(id INTEGER PRIMARY KEY AUTOINCREMENT, label TEXT NOT NULL CHECK(length(label)>0), calc INTEGER GENERATED ALWAYS AS (id*2) STORED)",{},cancel).error.isEmpty());
        QVERIFY(db->execute("CREATE UNIQUE INDEX owned_label_idx ON actions_owned(label)",{},cancel).error.isEmpty());db->execute("INSERT INTO actions_owned(label) VALUES ('a'),('b')",{},cancel);
        TableTask task;task.node.kind="table";task.node.schema="main";task.node.object="actions_owned";task.action=TableAction::CopyData;task.targetName="copied\";value";
        auto copied=runTableTask(p,task,cancel);QVERIFY2(copied.applied,qPrintable(copied.error));QCOMPARE(copied.rows,2);auto target=task.node;target.object=task.targetName;auto actual=db->execute("SELECT * FROM "+tableQualifiedName(p,target)+" ORDER BY id",{},cancel);QCOMPARE(actual.rows.size(),2);QCOMPARE(actual.rows[1][2].toInt(),4);
        auto indexes=db->execute("SELECT count(*) FROM sqlite_schema WHERE type='index' AND tbl_name='copied\";value'",{},cancel);QCOMPARE(indexes.rows[0][0].toInt(),1);
        auto duplicate=runTableTask(p,task,cancel);QVERIFY(!duplicate.error.isEmpty());QVERIFY(!duplicate.partial);QCOMPARE(db->execute("SELECT count(*) FROM actions_owned",{},cancel).rows[0][0].toInt(),2);
        task.node=target;task.action=TableAction::Rename;task.targetName="renamed_owned";auto renamed=runTableTask(p,task,cancel);QVERIFY2(renamed.applied,qPrintable(renamed.error));task.node.object=task.targetName;
        task.action=TableAction::CountRows;QCOMPARE(runTableTask(p,task,cancel).rows,2);
        auto readonly=p;readonly.readOnly=true;task.action=TableAction::Drop;QVERIFY(!runTableTask(readonly,task,cancel).error.isEmpty());
        task.action=TableAction::Check;auto check=runTableTask(p,task,cancel);QVERIFY2(check.error.isEmpty(),qPrintable(check.error));QCOMPARE(check.result.rows[0][0].toString(),QString("ok"));
        task.action=TableAction::Truncate;auto cleared=runTableTask(p,task,cancel);QVERIFY2(cleared.applied,qPrintable(cleared.error));db->execute("INSERT INTO renamed_owned(label) VALUES ('fresh')",{},cancel);QCOMPARE(db->execute("SELECT id FROM renamed_owned",{},cancel).rows[0][0].toInt(),1);
        task.action=TableAction::DeleteRows;QVERIFY(runTableTask(p,task,cancel).applied);task.action=TableAction::Drop;QVERIFY(runTableTask(p,task,cancel).applied);QVERIFY(!db->tables().contains("renamed_owned"));
    }
    void tableCsvAndSqlExportsRoundTripAndImportFailureRollsBack() {
        using namespace navidog;QTemporaryDir owned;auto p=profile_;p.database=ensureDemoDatabase(owned.path());auto db=connectDatabase(p);auto token=cancellation();
        db->execute("CREATE TABLE file_owned(id INTEGER PRIMARY KEY,label TEXT,empty TEXT,payload BLOB,calc INTEGER GENERATED ALWAYS AS(id*3) STORED)",{},token);
        auto text=QString("value,\"quoted\"\nnext; -- 中文");auto blob=QByteArray::fromHex("0001ff");db->executeBound("INSERT INTO file_owned(id,label,empty,payload) VALUES(?1,?2,?3,?4)",{1,text,QString(""),blob},{},token);db->executeBound("INSERT INTO file_owned(id,label,empty,payload) VALUES(?1,?2,?3,?4)",{2,QString("\\N"),QVariant{},QByteArray("",0)},{},token);
        TableTask task;task.node.kind="table";task.node.schema="main";task.node.object="file_owned";task.action=TableAction::ExportCsv;task.path=owned.filePath("data.csv");auto exported=runTableTask(p,task,token);QVERIFY2(exported.applied,qPrintable(exported.error));QCOMPARE(exported.rows,2);
        // CSV exposes all columns; generated columns must be removed from the
        // import mapping. Use a matching writable destination fixture here.
        db->execute("CREATE TABLE import_owned(id INTEGER PRIMARY KEY,label TEXT,empty TEXT,payload BLOB,calc INTEGER)",{},token);task.node.object="import_owned";task.action=TableAction::ImportCsv;auto imported=runTableTask(p,task,token);QVERIFY2(imported.applied,qPrintable(imported.error));QCOMPARE(imported.rows,2);
        auto rows=db->execute("SELECT * FROM import_owned ORDER BY id",{},token);QCOMPARE(rows.rows[0][1].toString(),text);QCOMPARE(rows.rows[0][2].toString(),QString(""));QVERIFY(rows.rows[0][2].isValid());QCOMPARE(rows.rows[0][3].toByteArray(),blob);QVERIFY(!rows.rows[1][2].isValid());QCOMPARE(rows.rows[1][1].toString(),QString("\\N"));
        QFile bad(owned.filePath("bad.csv"));QVERIFY(bad.open(QIODevice::WriteOnly));bad.write("id,label\r\n3,first\r\n1,duplicate\r\n");bad.close();task.path=bad.fileName();auto failed=runTableTask(p,task,token);QVERIFY(!failed.error.isEmpty());QVERIFY(failed.rolledBack);QCOMPARE(db->execute("SELECT count(*) FROM import_owned",{},token).rows[0][0].toInt(),2);
        task.node.object="file_owned";task.action=TableAction::ExportSql;task.path=owned.filePath("dump.sql");exported=runTableTask(p,task,token);QVERIFY2(exported.applied,qPrintable(exported.error));QFile dump(task.path);QVERIFY(dump.open(QIODevice::ReadOnly));auto script=dump.readAll();
        NamespaceDraft fresh;fresh.filePath=owned.filePath("restored.sqlite");QVERIFY(createNamespace(p,fresh,token).applied);auto restoredProfile=p;restoredProfile.database=fresh.filePath;auto restored=connectDatabase(restoredProfile);auto lexed=sql::lex(std::string_view(script.constData(),script.size()),sql::Dialect::SQLite);qsizetype start=0;
        for(auto t:lexed.tokens)if(t.kind==sql::TokenKind::Symbol && script.mid(t.begin,t.end-t.begin)==";"){auto statement=QString::fromUtf8(script.mid(start,t.end-start));auto result=restored->execute(statement,{},token);QVERIFY2(result.error.isEmpty(),qPrintable(result.error));start=t.end;}
        auto actual=restored->execute("SELECT * FROM file_owned ORDER BY id",{},token);QCOMPARE(actual.rows,db->execute("SELECT * FROM file_owned ORDER BY id",{},token).rows);
        task.action=TableAction::ExportCsv;task.path=p.database;auto overwrite=runTableTask(p,task,token);QVERIFY(!overwrite.applied);QVERIFY(overwrite.error.contains("不能覆盖"));QCOMPARE(db->execute("SELECT count(*) FROM file_owned",{},token).rows[0][0].toInt(),2);
        task.action=TableAction::Dictionary;task.path=owned.filePath("dictionary.md");QVERIFY(runTableTask(p,task,token).applied);QFile dictionary(task.path);QVERIFY(dictionary.open(QIODevice::ReadOnly));QVERIFY(dictionary.readAll().contains("INTEGER"));
    }
    void tableGenerationModelsAndDistributionUseRealMetadata() {
        using namespace navidog;QTemporaryDir owned;auto p=profile_;p.database=ensureDemoDatabase(owned.path());auto db=connectDatabase(p);auto token=cancellation();
        db->execute("CREATE TABLE parent_owned(id INTEGER PRIMARY KEY)",{},token);db->execute("INSERT INTO parent_owned VALUES(1)",{},token);db->execute("CREATE TABLE generated_owned(id INTEGER PRIMARY KEY AUTOINCREMENT,title TEXT UNIQUE NOT NULL,amount INTEGER NOT NULL,parent_id INTEGER REFERENCES parent_owned(id))",{},token);
        TableTask task;task.node.kind="table";task.node.schema="main";task.node.object="generated_owned";task.action=TableAction::GenerateData;task.maxRows=5;task.generationRules={{"title",QJsonObject{{"kind","series"},{"value","row_"}}},{"amount",QJsonObject{{"kind","sequence"},{"value","10"}}},{"parent_id",QJsonObject{{"kind","constant"},{"value","1"}}}};
        auto made=runTableTask(p,task,token);QVERIFY2(made.applied,qPrintable(made.error));QCOMPARE(made.rows,5);auto duplicate=runTableTask(p,task,token);QVERIFY(!duplicate.applied);QVERIFY(duplicate.rolledBack);QCOMPARE(db->execute("SELECT count(*) FROM generated_owned",{},token).rows[0][0].toInt(),5);
        task.action=TableAction::ReverseModel;auto model=runTableTask(p,task,token);QVERIFY2(model.applied,qPrintable(model.error));QCOMPARE(model.document["columns"].toArray().size(),4);QCOMPARE(model.document["references"].toArray()[0].toObject()["table"].toString(),QString("parent_owned"));
        task.action=TableAction::BuildBI;task.dimension="parent_id";auto chart=runTableTask(p,task,token);QVERIFY2(chart.applied,qPrintable(chart.error));QCOMPARE(chart.result.rows.size(),1);QCOMPARE(chart.result.rows[0][1].toInt(),5);
        task.dimension="unknown' field";QVERIFY(!runTableTask(p,task,token).error.isEmpty());task.action=TableAction::GenerateData;task.generationRules["amount"]=QJsonObject{{"kind","sequence"},{"value","9223372036854775807"}};auto overflow=runTableTask(p,task,token);QVERIFY(!overflow.applied);QVERIFY(overflow.rolledBack);QCOMPARE(db->execute("SELECT count(*) FROM generated_owned",{},token).rows[0][0].toInt(),5);
    }
    void backgroundTasksSerializeResourcesAndCancelIndependently() {
        using namespace navidog;
        BackgroundTasks tasks;
        QTemporaryDir directory;
        const auto output = directory.filePath("reserved.csv");
        const auto first = tasks.submit({"导出", {}, "database-A", output}, [](const TaskContext& task) {
            task.report({3, 10, TaskUnit::Rows, "导出"});
            while (!task.cancel->load()) QThread::msleep(4);
            TaskResult result;
            result.cancelled = true;
            return result;
        });
        QVERIFY(!first.isEmpty());
        const auto next = tasks.submit({"导入", {}, "database-A", {}}, [](const TaskContext&) {
            TaskResult result;
            result.detail = "导入完成";
            return result;
        });
        QCOMPARE(tasks.info(next)->state, TaskState::Queued);
        QString rejection;
        QVERIFY(tasks.submit({"冲突文件", {}, "database-B", output}, [](const TaskContext&) {
            return TaskResult{};
        }, &rejection).isEmpty());
        QVERIFY(!rejection.isEmpty());
        const auto independent = tasks.submit({"独立导出", {}, "database-B", {}}, [](const TaskContext&) {
            return TaskResult{};
        });
        QTRY_COMPARE(tasks.info(first)->progress.done, qint64(3));
        QTRY_COMPARE(tasks.info(independent)->state, TaskState::Succeeded);
        QCOMPARE(tasks.info(next)->state, TaskState::Queued);
        QVERIFY(tasks.cancel(first));
        QTRY_COMPARE(tasks.info(first)->state, TaskState::Cancelled);
        QTRY_COMPARE(tasks.info(next)->state, TaskState::Succeeded);
        QCOMPARE(tasks.info(next)->detail, QString("导入完成"));
        tasks.clearFinished();
        QVERIFY(tasks.history().isEmpty());
    }

    void cancelledFileExportKeepsExistingFile() {
        using namespace navidog;
        QTemporaryDir directory;
        const auto path = directory.filePath("cached.csv");
        QFile file(path);
        QVERIFY(file.open(QIODevice::WriteOnly));
        QCOMPARE(file.write("original"), qint64(8));
        file.close();
        QueryResult data;
        data.columns = {"id"};
        data.rows = {{1}, {2}};
        BackgroundTasks tasks;
        const auto id = tasks.submit({"导出", {}, {}, path}, [data, path](const TaskContext& task) {
            const TaskContext stopped{task.cancel, [&](const TaskProgress& progress) {
                task.report(progress);
                if (progress.done == progress.total) task.cancel->store(true);
            }};
            return exportResultFile(path, data, ResultExportFormat::Csv, DatabaseKind::SQLite, {}, stopped);
        });
        QTRY_COMPARE(tasks.info(id)->state, TaskState::Cancelled);
        QVERIFY(tasks.info(id)->artifacts.isEmpty());
        QVERIFY(file.open(QIODevice::ReadOnly));
        QCOMPARE(file.readAll(), QByteArray("original"));
    }

    void dataGenerationUsesCompositeKeysWeightedFieldsAndCapacity() {
        using namespace navidog;
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        ConnectionProfile profile;
        profile.database = ensureDemoDatabase(directory.path());
        auto db = connectDatabase(profile);
        const auto token = cancellation();
        auto sql = [&](const QString& query) {
            const auto result = db->execute(query, {}, token);
            if (!result.error.isEmpty()) throw std::runtime_error(result.error.toStdString());
            return result;
        };
        sql("CREATE TABLE parent(region INT, tenant TEXT, PRIMARY KEY(region, tenant))");
        sql("INSERT INTO parent VALUES (3, 'east'), (7, 'west')");
        sql("CREATE TABLE child(id INTEGER PRIMARY KEY, region INT NOT NULL, tenant TEXT NOT NULL,"
            "name VARCHAR(20) NOT NULL, age INT NOT NULL CHECK(age BETWEEN 18 AND 80),"
            "created DATETIME NOT NULL, code TEXT UNIQUE NOT NULL, category TEXT NOT NULL,"
            "FOREIGN KEY(region, tenant) REFERENCES parent)");
        sql("CREATE INDEX child_age ON child(age)");
        sql("CREATE TABLE startup_side_effect (id INT)");
        profile.advanced.initialSql = "INSERT INTO startup_side_effect VALUES (1)";
        TableTask task;
        task.action = TableAction::GenerateData;
        task.node.kind = "table";
        task.node.schema = "main";
        task.node.object = "child";
        task.generation.size = "100,000,000,000,000";
        task.generation.seed = 154;
        task.generation.preview = true;
        const auto table = readTableDefinition(*db, profile, task.node, token);
        task.generationRules = suggestedGenerationRules(table, profile.driver);
        QCOMPARE(task.generationRules["region"].toObject()["kind"].toString(), QString("foreign"));
        QCOMPARE(task.generationRules["tenant"].toObject()["kind"].toString(), QString("foreign"));
        task.generationRules["age"] = QJsonObject{{"kind", "number"}, {"value", "18..35@80;36..80@20"}};
        task.generationRules["created"] = QJsonObject{{"kind", "temporal"},
            {"value", "2026-01-01..2026-01-03"}, {"distribution", "0..5@90;6..23@10"}};
        task.generationRules["code"] = QJsonObject{{"kind", "regex"}, {"value", "(?:[A-Z]{3}|华)[0-9]{6}"}};
        task.generationRules["category"] = QJsonObject{{"kind", "choice"}, {"value", "A@10;B@90"}};
        auto readOnly = profile;
        readOnly.readOnly = true;
        const auto estimate = runTableTask(readOnly, task, token);
        QVERIFY2(estimate.error.isEmpty(), qPrintable(estimate.error));
        QCOMPARE(estimate.rows, qint64(100000000000000LL));
        QVERIFY(estimate.report.contains("UTF", Qt::CaseInsensitive));
        QVERIFY(estimate.report.contains("常见索引"));
        QCOMPARE(estimate.result.rows.size(), 12);
        QCOMPARE(sql("SELECT count(*) FROM child").rows.front().front().toInt(), 0);
        QCOMPARE(sql("SELECT count(*) FROM startup_side_effect").rows.front().front().toInt(), 0);
        const int code = estimate.result.columns.indexOf("code");
        QVERIFY(code >= 0);
        const QRegularExpression pattern("^(?:[A-Z]{3}|华)[0-9]{6}$");
        for (const auto& row : estimate.result.rows) QVERIFY(pattern.match(row[code].toString()).hasMatch());

        task.generation.sizeMode = GenerationSizeMode::Capacity;
        task.generation.size = "1 MiB";
        const auto byCapacity = runTableTask(profile, task, token);
        QVERIFY2(byCapacity.error.isEmpty(), qPrintable(byCapacity.error));
        QVERIFY(byCapacity.rows > 5000 && byCapacity.rows < 40000);
        task.generation.sizeMode = GenerationSizeMode::Rows;
        task.generation.size = "1024";
        task.generation.preview = false;
        qint64 completed = 0;
        const auto created = runTableTask(profile, task, token, [&](qint64 done, qint64 total) {
            QCOMPARE(total, qint64(1024));
            completed = done;
        });
        QVERIFY2(created.applied, qPrintable(created.error));
        QCOMPARE(completed, qint64(1024));
        const auto distribution = sql("SELECT count(*), sum(age BETWEEN 18 AND 35), "
                                      "sum(CAST(strftime('%H',created) AS INT)<6), sum(category='B'), "
                                      "count(DISTINCT code), count(DISTINCT region) FROM child").rows.front();
        QCOMPARE(distribution[0].toInt(), 1024);
        QVERIFY(distribution[1].toInt() > 740);
        QVERIFY(distribution[2].toInt() > 850);
        QVERIFY(distribution[3].toInt() > 850);
        QCOMPARE(distribution[4].toInt(), 1024);
        QCOMPARE(distribution[5].toInt(), 2);
        QCOMPARE(sql("SELECT count(*) FROM child JOIN parent USING(region,tenant)").rows.front().front().toInt(), 1024);
    }

    void generationStopsInvalidRulesAndRollsBackBatches() {
        using namespace navidog;
        QTemporaryDir directory;
        ConnectionProfile profile;
        profile.database = ensureDemoDatabase(directory.path());
        auto db = connectDatabase(profile);
        const auto token = cancellation();
        const auto run = [&](const QString& text) { return db->execute(text, {}, token); };
        QVERIFY(run("CREATE TABLE late_failure(id INTEGER PRIMARY KEY, code INTEGER NOT NULL CHECK(code+0 < 300))").error.isEmpty());
        TableTask task;
        task.action = TableAction::GenerateData;
        task.node.kind = "table";
        task.node.schema = "main";
        task.node.object = "late_failure";
        task.generation.size = "400";
        task.generationRules["code"] = QJsonObject{{"kind", "sequence"}, {"value", "1"}};
        const auto failed = runTableTask(profile, task, token);
        QVERIFY(!failed.applied);
        QVERIFY(failed.rolledBack);
        QCOMPARE(failed.rows, qint64(250));
        QCOMPARE(run("SELECT count(*) FROM late_failure").rows.front().front().toInt(), 0);

        QVERIFY(run("CREATE TABLE empty_parent(id INTEGER PRIMARY KEY)").error.isEmpty());
        QVERIFY(run("CREATE TABLE empty_child(id INTEGER PRIMARY KEY, parent INTEGER NOT NULL REFERENCES empty_parent(id),"
                    "label VARCHAR(16) NOT NULL)").error.isEmpty());
        task.node.object = "empty_child";
        task.generationRules = QJsonObject{{"parent", QJsonObject{{"kind", "foreign"}}},
            {"label", QJsonObject{{"kind", "regex"}, {"value", "[A-Z]{4}"}}}};
        task.generation.preview = true;
        const auto noParent = runTableTask(profile, task, token);
        QVERIFY2(noParent.error.isEmpty(), qPrintable(noParent.error));
        QVERIFY(noParent.report.contains("没有可用父键"));
        task.generation.preview = false;
        const auto missing = runTableTask(profile, task, token);
        QVERIFY(!missing.applied);
        QVERIFY(missing.rolledBack);
        QVERIFY(missing.error.contains("没有可用引用键"));
        task.generation.preview = true;
        task.generationRules["label"] = QJsonObject{{"kind", "regex"}, {"value", "(?=A)A"}};
        const auto invalid = runTableTask(profile, task, token);
        QVERIFY(invalid.error.contains("正则"));
    }
    void tablePlansQuoteIdentifiersAndGuardMaintenanceOptions() {
        using namespace navidog;ConnectionProfile p;p.driver=DatabaseKind::MySQL;TableTask task;task.node.kind="table";task.node.database="owned`db";task.node.object="table`name";task.action=TableAction::Rename;task.targetName="target`name";
        QCOMPARE(tableTaskSql(p,task),QString("RENAME TABLE `owned``db`.`table``name` TO `owned``db`.`target``name`"));
        task.action=TableAction::Check;task.option="EXTENDED; DROP TABLE other";QVERIFY_THROWS_EXCEPTION(std::runtime_error,tableTaskSql(p,task));task.option="QUICK";QVERIFY(tableTaskSql(p,task).endsWith(" QUICK"));
        p.driver=DatabaseKind::SQLServer;task.action=TableAction::CountRows;QVERIFY(tableTaskSql(p,task).startsWith("SELECT COUNT_BIG(*)"));
        p.driver=DatabaseKind::MySQL;task.action=TableAction::GrantPrivileges;task.grantee="user`name";task.granteeHost="local`host";task.privileges={"SELECT","UPDATE"};QVERIFY(tableTaskSql(p,task).endsWith("TO `user``name`@`local``host`"));task.privileges={"SELECT; DROP"};QVERIFY_THROWS_EXCEPTION(std::runtime_error,tableTaskSql(p,task));
        p.driver=DatabaseKind::Oracle;task.action=TableAction::DeleteRows;QVERIFY(tableTaskSql(p,task).startsWith("DELETE FROM"));
    }
    void namespacePlansQuoteNamesAndRespectCapabilities() {
        using namespace navidog;ConnectionProfile p;NamespaceDraft d;d.name="new`db";p.driver=DatabaseKind::MySQL;d.charset="utf8mb4";d.collation="utf8mb4_unicode_ci";
        QCOMPARE(namespaceSql(p,d),QString("CREATE DATABASE `new``db` CHARACTER SET utf8mb4 COLLATE utf8mb4_unicode_ci;"));
        d.collation="utf8mb4; DROP DATABASE other";QVERIFY_THROWS_EXCEPTION(std::runtime_error,namespaceSql(p,d));d.collation.clear();d.charset.clear();
        p.driver=DatabaseKind::PostgreSQL;d.name="db\"; SELECT 1; --";d.owner="owner\"name";
        QCOMPARE(namespaceSql(p,d),QString("CREATE DATABASE \"db\"\"; SELECT 1; --\" OWNER \"owner\"\"name\";"));
        d.name=QString(22,QChar(0x4e2d));QVERIFY_THROWS_EXCEPTION(std::runtime_error,namespaceSql(p,d));
        d.kind=NamespaceKind::Schema;d.name="pg_owned";p.database="owned";QVERIFY_THROWS_EXCEPTION(std::runtime_error,namespaceSql(p,d));
        d.name="schema";d.owner="dbo]owner";p.driver=DatabaseKind::SQLServer;
        QCOMPARE(namespaceSql(p,d),QString("CREATE SCHEMA [schema] AUTHORIZATION [dbo]]owner];"));
        p.readOnly=true;QVERIFY_THROWS_EXCEPTION(std::runtime_error,namespaceSql(p,d));p.readOnly=false;
        QVERIFY(!supportsNamespace(DatabaseKind::MySQL,NamespaceKind::Schema));QVERIFY(!supportsNamespace(DatabaseKind::Oracle,NamespaceKind::Database));QVERIFY(supportsNamespace(DatabaseKind::Oracle,NamespaceKind::Schema));QVERIFY(!supportsNamespace(DatabaseKind::SQLite,NamespaceKind::Schema));
        QVERIFY(nodeActions("database",false,DatabaseKind::PostgreSQL).contains(NodeAction::NewSchema));QVERIFY(!nodeActions("database",false,DatabaseKind::MySQL).contains(NodeAction::NewSchema));
        QVERIFY(!nodeActions("database",false,DatabaseKind::PostgreSQL).contains(NodeAction::NewTable));QVERIFY(nodeActions("schema",false,DatabaseKind::PostgreSQL).contains(NodeAction::NewTable));
    }
    void oracleNamespacePreviewNeverContainsCredentials(){
        using namespace navidog;ConnectionProfile p;p.driver=DatabaseKind::Oracle;NamespaceDraft d;d.kind=NamespaceKind::Schema;d.name="OWNED_USER";d.password="N9_"+QUuid::createUuid().toString(QUuid::Id128).left(16);d.defaultTablespace="DATA SPACE";d.temporaryTablespace="TEMP";d.quotaMiB=50;
        auto sql=namespaceSql(p,d);QVERIFY(!sql.contains(d.password));QVERIFY(sql.contains("IDENTIFIED BY \"<password>\""));QVERIFY(sql.contains("QUOTA 50M ON \"DATA SPACE\""));QVERIFY(sql.contains("GRANT CREATE SESSION TO \"OWNED_USER\""));QVERIFY(nodeActions("connection",false,p.driver).contains(NodeAction::NewSchema));
        d.accountLocked=true;d.grantSession=false;sql=namespaceSql(p,d);QVERIFY(sql.contains("ACCOUNT LOCK"));QVERIFY(!sql.contains("GRANT"));d.defaultTablespace.clear();QVERIFY_THROWS_EXCEPTION(std::runtime_error,namespaceSql(p,d));d.quotaMiB=0;d.password+='"';QVERIFY_THROWS_EXCEPTION(std::runtime_error,namespaceSql(p,d));
        d.password="N9_"+QUuid::createUuid().toString(QUuid::Id128).left(16);d.name="C##OWNED";QVERIFY_THROWS_EXCEPTION(std::runtime_error,namespaceSql(p,d));d.name=QString(11,QChar(0x4e2d));QVERIFY_THROWS_EXCEPTION(std::runtime_error,namespaceSql(p,d));
    }
    void sqliteCreationNeverOverwritesFilesAndRunsOffThread() {
        using namespace navidog;QTemporaryDir dir;ConnectionProfile p;NamespaceDraft d;d.name="Owned";d.filePath=dir.filePath("new.sqlite");QueryService service;QSignalSpy done(&service,&QueryService::namespaceCompleted);
        QVERIFY(service.createNamespace(p,d,"create-owned"));QTRY_COMPARE_WITH_TIMEOUT(done.count(),1,3000);auto result=qvariant_cast<NamespaceResult>(done.takeFirst()[1]);QVERIFY2(result.applied,qPrintable(result.error));QCOMPARE(service.sessionCount(),0);
        p.database=d.filePath;auto db=connectDatabase(p);QVERIFY(db->execute("CREATE TABLE marker(id INTEGER)",{},cancellation()).error.isEmpty());db.reset();
        QFile file(d.filePath);QVERIFY(file.open(QIODevice::ReadOnly));auto before=file.readAll();file.close();QVERIFY(before.startsWith("SQLite format 3"));
        result=createNamespace(p,d,cancellation());QVERIFY(!result.error.isEmpty());QVERIFY(!result.applied);QVERIFY(file.open(QIODevice::ReadOnly));QCOMPARE(file.readAll(),before);file.close();
        d.filePath=dir.filePath("cancelled.sqlite");auto cancel=cancellation();cancel->store(true);result=createNamespace(p,d,cancel);QVERIFY(result.cancelled);QVERIFY(!QFile::exists(d.filePath));
        p.readOnly=true;result=createNamespace(p,d,cancellation());QVERIFY(!result.error.isEmpty());QVERIFY(!QFile::exists(d.filePath));
    }
    void contextActionsRespectNodeMeaning() {
        using A=navidog::NodeAction;
        for(const auto& kind:{"table","view","column","index","parameter","variable","function","procedure","database","schema","tables"}) {
            auto actions=navidog::nodeActions(kind); QVERIFY(!actions.contains(A::NewConnection));QVERIFY(!actions.contains(A::DeleteConnection));QVERIFY(!actions.contains(A::RenameConnection));
        }
        QVERIFY(navidog::nodeActions("connection").contains(A::NewConnection));QVERIFY(navidog::nodeActions("group").contains(A::NewConnection));
        QVERIFY(navidog::nodeActions("database",false).contains(A::OpenDatabase));QVERIFY(navidog::nodeActions("database",true).contains(A::CloseDatabase));
    }
    void typedProfilesMigrateAndDoNotStoreDockerPasswords() {
        auto p=profile_;p.driver=navidog::DatabaseKind::MySQL;p.version=navidog::DatabaseVersion::MySQL8_4;p.detectedVersion=p.version;p.serverVersion="8.4.9";
        p.credentialSource=navidog::CredentialSource::LocalDocker;p.credentialContainer="owned-test-container";p.credentialVariable="MYSQL_ROOT_PASSWORD";p.password="never-save-this";
        navidog::ProfileStore store(dir_.filePath("typed.json"));store.save({p});auto loaded=store.load()[0];
        QCOMPARE(loaded.driver,p.driver);QCOMPARE(loaded.version,p.version);QCOMPARE(loaded.serverVersion,p.serverVersion);QCOMPARE(loaded.credentialSource,p.credentialSource);QVERIFY(loaded.password.isEmpty());
        QFile file(dir_.filePath("typed.json"));QVERIFY(file.open(QIODevice::ReadOnly));QVERIFY(!file.readAll().contains(p.password.toUtf8()));
    }
    void tableDesignerSQLiteRoundtripStaleDetectionAndRollback() {
        auto cancel=navidog::cancellation();navidog::TableDefinition empty,after;after.node.kind="table";after.node.schema="main";after.node.object=after.node.name="design.owned";
        navidog::TableColumn id;id.name="id";id.type="INTEGER";id.nullable=false;id.primaryKey=1;
        navidog::TableColumn note;note.name="note";note.type="TEXT";note.defaultExpression="'hello'";after.columns={id,note};
        auto created=navidog::applyTableChanges(profile_,empty,after,cancel);QVERIFY2(created.error.isEmpty(),qPrintable(created.error));QVERIFY(created.definition.exists);QCOMPARE(created.definition.columns.size(),2);
        auto baseline=created.definition;auto changed=baseline;changed.columns[1].name="renamed note";navidog::TableColumn extra;extra.name="added";extra.type="INTEGER";changed.columns<<extra;
        auto saved=navidog::applyTableChanges(profile_,baseline,changed,cancel);QVERIFY2(saved.error.isEmpty(),qPrintable(saved.error));QCOMPARE(saved.appliedStatements,2);QCOMPARE(saved.definition.columns[1].name,QString("renamed note"));
        auto stale=navidog::applyTableChanges(profile_,baseline,changed,cancel);QVERIFY(stale.error.contains("已被其他操作修改"));
        baseline=saved.definition;changed=baseline;changed.columns.removeAt(1);extra.name="bad";extra.type="INTEGER";extra.defaultExpression="not_a_valid_default()";changed.columns<<extra;
        auto rolled=navidog::applyTableChanges(profile_,baseline,changed,cancel);QVERIFY(!rolled.error.isEmpty());QVERIFY(rolled.rolledBack);QVERIFY(!rolled.partial);
        auto db=navidog::connectDatabase(profile_);auto actual=navidog::readTableDefinition(*db,profile_,baseline.node,cancel);QCOMPARE(actual.columns,baseline.columns);
        changed=baseline;changed.columns[1].type="INTEGER";QVERIFY(navidog::tableChangeSql(profile_,baseline,changed).join(' ' ).contains("INSERT INTO"));
        auto readonly=profile_;readonly.readOnly=true;auto denied=navidog::applyTableChanges(readonly,empty,after,cancel);QVERIFY(denied.error.contains("只读"));
    }
    void sqliteDesignerRebuildKeepsDataConstraintsTriggersViewsAndSequence() {
        using namespace navidog;QTemporaryDir owned;auto p=profile_;p.database=ensureDemoDatabase(owned.path());auto db=connectDatabase(p);auto token=cancellation();
        auto run=[&](const QString& text){auto r=db->executeDefinition(text,{},token);if(!r.error.isEmpty())throw std::runtime_error(r.error.toStdString());};
        run("CREATE TABLE parent_design(id INTEGER PRIMARY KEY)");run("INSERT INTO parent_design VALUES(1)");
        run("CREATE TABLE detail_design(id INTEGER PRIMARY KEY AUTOINCREMENT,parent_id INTEGER REFERENCES parent_design(id),code TEXT COLLATE NOCASE UNIQUE,qty INTEGER NOT NULL DEFAULT 1 CHECK(qty>=0),calc INTEGER GENERATED ALWAYS AS(qty*2) STORED)");
        run("CREATE TABLE audit_design(note TEXT)");run("CREATE TRIGGER log_design AFTER UPDATE ON detail_design BEGIN INSERT INTO audit_design VALUES('updated;ok'); INSERT INTO audit_design VALUES('second'); END");
        run("CREATE INDEX qty_design ON detail_design(qty DESC) WHERE qty>0");run("CREATE VIEW detail_view_design AS SELECT id,qty FROM detail_design");run("INSERT INTO detail_design(id,parent_id,code,qty) VALUES(1,1,'Alpha',3),(1000,1,'old',4)");run("DELETE FROM detail_design WHERE id=1000");
        CatalogNode node;node.kind="table";node.schema="main";node.object="detail_design";auto before=readTableDefinition(*db,p,node,token);QCOMPARE(before.objects.size(),5);QVERIFY(before.columns[0].autoIncrement);QCOMPARE(before.columns[4].generatedExpression,QString("qty*2"));
        auto after=before;after.columns[3].type="BIGINT";after.columns[3].defaultExpression="2";auto result=applyTableChanges(p,before,after,token);QVERIFY2(result.error.isEmpty(),qPrintable(result.error));QVERIFY(!result.partial);
        auto row=db->execute("SELECT id,code,qty,calc FROM detail_design",{},token);QCOMPARE(row.rows.size(),1);QCOMPARE(row.rows[0][0].toInt(),1);QCOMPARE(row.rows[0][3].toInt(),6);
        run("UPDATE detail_design SET qty=5 WHERE id=1");QCOMPARE(db->execute("SELECT COUNT(*) FROM audit_design",{},token).rows[0][0].toInt(),2);QCOMPARE(db->execute("SELECT qty FROM detail_view_design",{},token).rows[0][0].toInt(),5);
        run("INSERT INTO detail_design(parent_id,code) VALUES(1,'Beta')");QCOMPARE(db->execute("SELECT id FROM detail_design WHERE code='Beta'",{},token).rows[0][0].toInt(),1001);
        QVERIFY(!db->execute("INSERT INTO detail_design(parent_id,code) VALUES(999,'invalid')",{},token).error.isEmpty());QVERIFY(!db->execute("INSERT INTO detail_design(parent_id,code) VALUES(1,'alpha')",{},token).error.isEmpty());
        before=readTableDefinition(*db,p,node,token);after=before;TableObject check;check.kind=TableObjectKind::Check;check.name="invalid_check_design";check.expression="qty<0";after.objects<<check;result=applyTableChanges(p,before,after,token);QVERIFY(!result.error.isEmpty());QVERIFY(result.rolledBack);QVERIFY(!result.partial);QVERIFY(sameTableStructure(before,readTableDefinition(*db,p,node,token)));
        after=before;after.columns[3].defaultExpression="9";run("CREATE INDEX concurrent_design ON detail_design(code)");result=applyTableChanges(p,before,after,token);QVERIFY(result.error.contains("已被其他操作修改"));QVERIFY(result.rolledBack);
    }
    void sqliteTableObjectsCanBeCreatedEditedDroppedAndRolledBack() {
        using namespace navidog;QTemporaryDir owned;auto p=profile_;p.database=ensureDemoDatabase(owned.path());auto db=connectDatabase(p);auto token=cancellation();TableDefinition empty,after;after.node.kind="table";after.node.schema="main";after.node.object="objects_design";TableColumn c;c.name="id";c.type="INTEGER";c.primaryKey=1;after.columns<<c;c.name="value";c.type="TEXT";c.primaryKey=0;after.columns<<c;
        TableObject idx;idx.name="object_idx_design";idx.expression="\"value\"";idx.unique=true;after.objects<<idx;auto r=applyTableChanges(p,empty,after,token);QVERIFY2(r.error.isEmpty(),qPrintable(r.error));QCOMPARE(r.definition.objects.size(),1);auto before=r.definition;after=before;after.objects[0].name="renamed_idx_design";after.objects[0].unique=false;r=applyTableChanges(p,before,after,token);QVERIFY2(r.error.isEmpty(),qPrintable(r.error));QCOMPARE(r.definition.objects[0].name,QString("renamed_idx_design"));
        before=r.definition;after=before;after.objects[0].expression="missing_column";r=applyTableChanges(p,before,after,token);QVERIFY(!r.error.isEmpty());QVERIFY(r.rolledBack);QVERIFY(sameTableStructure(before,readTableDefinition(*db,p,before.node,token)));
        after=before;after.objects.clear();r=applyTableChanges(p,before,after,token);QVERIFY2(r.error.isEmpty(),qPrintable(r.error));QVERIFY(r.definition.objects.isEmpty());
    }
    void tableDesignerDialectPlansAndValidation() {
        using K=navidog::DatabaseKind;
        for(auto kind:{K::SQLite,K::MySQL,K::PostgreSQL,K::Oracle,K::OceanBase,K::SQLServer}) {
            auto p=profile_;p.driver=kind;navidog::TableDefinition empty,after;after.node.object="odd.table";after.node.schema="owned";after.node.database="db";navidog::TableColumn c;c.name="a]b";c.type=kind==K::Oracle?"NUMBER":"INTEGER";after.columns={c};
            auto sql=navidog::tableChangeSql(p,empty,after);QCOMPARE(sql.size(),1);QVERIFY(sql[0].startsWith("CREATE TABLE "));QVERIFY(sql[0].contains(navidog::quoteIdentifier("odd.table",kind)));
            c.defaultExpression="0); DROP TABLE orders; --";after.columns={c};QVERIFY_THROWS_EXCEPTION(std::runtime_error,navidog::tableChangeSql(p,empty,after));
            c.defaultExpression.clear();c.type="INTEGER); DROP TABLE orders";after.columns={c};QVERIFY_THROWS_EXCEPTION(std::runtime_error,navidog::tableChangeSql(p,empty,after));
        }
        for(auto kind:{K::MySQL,K::OceanBase,K::PostgreSQL,K::Oracle,K::SQLServer}) {
            auto p=profile_;p.driver=kind;navidog::TableDefinition before;before.exists=true;before.node.object="owned";navidog::TableColumn c;c.name=c.originalName="n";c.type="INTEGER";before.columns={c};auto after=before;after.columns[0].type="BIGINT";auto sql=navidog::tableChangeSql(p,before,after);QVERIFY(!sql.isEmpty());QVERIFY(sql.join(' ').contains("BIGINT"));
        }
    }
    void objectPlansUseNativeConstraintAndTriggerForms() {
        using namespace navidog;for(auto kind:{DatabaseKind::Oracle,DatabaseKind::SQLServer,DatabaseKind::PostgreSQL,DatabaseKind::MySQL}){
            ConnectionProfile p;p.driver=kind;TableDefinition before;before.exists=before.detailsLoaded=true;before.node.object="owned_table";before.node.schema="owned_schema";before.node.database="owned_db";TableColumn c;c.originalName=c.name="id";c.type=kind==DatabaseKind::Oracle?"NUMBER":"INT";before.columns<<c;auto after=before;
            TableObject fk;fk.kind=TableObjectKind::ForeignKey;fk.name="fk'owned";fk.columns={"id"};fk.referenceSchema=kind==DatabaseKind::MySQL?"owned_db":"owned_schema";fk.referenceTable="parent";fk.referenceColumns={"id"};fk.onDelete="CASCADE";after.objects<<fk;
            TableObject check;check.kind=TableObjectKind::Check;check.name="check_owned";check.expression="id>0";after.objects<<check;auto plan=tableChangeSql(p,before,after).join(' ');QVERIFY(plan.contains("FOREIGN KEY"));QVERIFY(plan.contains("ON DELETE CASCADE"));QVERIFY(plan.contains("CHECK (id>0)"));if(kind==DatabaseKind::Oracle)QVERIFY(!plan.contains("ON UPDATE"));
            after=before;TableObject trigger;trigger.kind=TableObjectKind::Trigger;trigger.name=trigger.originalName="tr_owned";trigger.sql="CREATE TRIGGER tr_owned ON owned_table";before.objects={trigger};after=before;after.objects[0].enabled=false;
            if(kind!=DatabaseKind::MySQL){auto state=tableChangeSql(p,before,after).join(' ');QVERIFY(state.contains("DISABLE"));QVERIFY(!state.contains("DROP TRIGGER"));QVERIFY(!state.contains("CREATE TRIGGER"));}
        }
        ConnectionProfile p;p.driver=DatabaseKind::SQLServer;TableDefinition before;before.exists=true;before.node.object="t";before.node.schema="dbo";TableColumn c;c.originalName=c.name="old";c.type="INT";c.defaultExpression="(1)";c.defaultConstraint="DF_owned";before.columns={c};auto after=before;after.columns[0].name="new";after.columns[0].defaultExpression="2";auto sql=tableChangeSql(p,before,after).join(' ');QVERIFY(sql.contains("sp_rename"));QVERIFY(sql.contains("DROP CONSTRAINT [DF_owned]"));QVERIFY(sql.contains("ADD DEFAULT 2 FOR [new]"));
    }
    void mysqlTableMetadataKeepsNumericAndExpressionDefaults() {
        class MetadataDb final:public navidog::Database {
        public:
            navidog::QueryResult result;
            navidog::QueryResult execute(const QString& query,navidog::QueryLimits,const navidog::Cancellation&)override{return query.startsWith("SELECT column_name,column_type")?result:navidog::QueryResult{};}
            QStringList tables()override{return {};}
            QString serverVersion()override{return "8.4";}
        } db;
        db.result.rows={{"amount","int","YES","42","","","", ""},{"uuid","varchar(36)","YES","uuid()","","DEFAULT_GENERATED","utf8mb4_bin",""},{"label","varchar(255)","YES","quote'\\text","","","utf8mb4_bin",""}};
        auto p=profile_;p.driver=navidog::DatabaseKind::MySQL;navidog::CatalogNode n;n.kind="table";n.database="owned";n.object="t";
        auto before=navidog::readTableDefinition(db,p,n,navidog::cancellation());QCOMPARE(before.columns[0].defaultExpression,QString("42"));QCOMPARE(before.columns[1].defaultExpression,QString("(uuid())"));QVERIFY(before.columns[2].defaultExpression.startsWith("0x"));
        auto after=before;after.columns[0].type="BIGINT";auto sql=navidog::tableChangeSql(p,before,after).join('\n');QVERIFY(sql.contains("DEFAULT 42"));QVERIFY(!sql.contains("0x3432"));
        after=before;after.columns[1].type="VARCHAR(100)";sql=navidog::tableChangeSql(p,before,after).join('\n');QVERIFY(sql.contains("DEFAULT (uuid())"));QVERIFY(sql.contains("COLLATE `utf8mb4_bin`"));
    }
    void readonlyConnection() {
        auto p = profile_; p.readOnly = true; auto db = navidog::connectDatabase(p);
        auto result = db->execute("DELETE FROM orders WHERE id = 1", {}, navidog::cancellation());
        QVERIFY(!result.error.isEmpty());
    }
    void statementBoundary_data() {
        QTest::addColumn<QString>("sql"); QTest::addColumn<int>("dialect"); QTest::addColumn<bool>("valid");
        using D = navidog::sql::Dialect;
        QTest::newRow("quoted semicolon") << "SELECT ';' AS x; -- trailing" << int(D::SQLite) << true;
        QTest::newRow("script") << "SELECT 1; DELETE FROM orders" << int(D::SQLite) << false;
        QTest::newRow("unclosed") << "SELECT 'hello" << int(D::SQLite) << false;
        QTest::newRow("empty") << "-- only a comment" << int(D::SQLite) << false;
        QTest::newRow("nested PG comments") << "/* a /* b */ c */ SELECT 1" << int(D::PostgreSQL) << true;
        QTest::newRow("dollar string") << "SELECT $tag$a;b$tag$;" << int(D::PostgreSQL) << true;
        QTest::newRow("mysql double quote") << "SELECT \"a;b\";" << int(D::MySQL) << true;
        QTest::newRow("mysql executable comment") << "SELECT /*! 1; SELECT */ 2" << int(D::MySQL) << false;
        QTest::newRow("mysql dash not comment") << "SELECT 1--2; SELECT 3" << int(D::MySQL) << false;
        QTest::newRow("oracle q quote") << "SELECT q'[a';b]' FROM dual" << int(D::Oracle) << true;
        QTest::newRow("sqlserver bracket escape") << "SELECT [a]];b] FROM t" << int(D::SQLServer) << true;
        QTest::newRow("sqlserver nested comment") << "/* a /* b */ c */ SELECT 1" << int(D::SQLServer) << true;
        QTest::newRow("bracket name") << "SELECT [semi;colon] FROM x" << int(D::SQLite) << true;
    }
    void statementBoundary() {
        QFETCH(QString, sql); QFETCH(int, dialect); QFETCH(bool, valid);
        std::string error; auto bytes = sql.toUtf8(); QCOMPARE(navidog::sql::isSingleStatement(bytes.toStdString(), static_cast<navidog::sql::Dialect>(dialect), error), valid);
    }
    void formatterPreservesTokensAndComments() {
        using namespace navidog::sql;
        const std::string original = "select E'a\\'b' as x, '-- do not alter' from t /*+ hint */ where x != 2 -- comment\n order by x";
        auto output = format(original, Dialect::PostgreSQL);
        auto a = lex(original, Dialect::PostgreSQL), b = lex(output, Dialect::PostgreSQL);
        QCOMPARE(a.tokens.size(), b.tokens.size()); QVERIFY(b.diagnostics.empty());
        for (std::size_t i = 0; i < a.tokens.size(); ++i) QCOMPARE(original.substr(a.tokens[i].begin, a.tokens[i].end-a.tokens[i].begin), output.substr(b.tokens[i].begin, b.tokens[i].end-b.tokens[i].begin));
        QCOMPARE(format(output, Dialect::PostgreSQL), output);
    }
    void serviceKeepsSessionAndGuiResponsive() {
        navidog::QueryService service; QSignalSpy spy(&service, &navidog::QueryService::completed);
        QVERIFY(service.run(profile_, "CREATE TEMP TABLE transient_test(n)"));
        QTRY_COMPARE_WITH_TIMEOUT(spy.count(), 1, 3000); QVERIFY(qvariant_cast<navidog::QueryOutcome>(spy.takeFirst()[0]).result.error.isEmpty());
        QVERIFY(service.run(profile_, "INSERT INTO transient_test VALUES(7)"));
        QTRY_COMPARE_WITH_TIMEOUT(spy.count(), 1, 3000); spy.clear();
        QVERIFY(service.run(profile_, "SELECT n FROM transient_test"));
        QTRY_COMPARE_WITH_TIMEOUT(spy.count(), 1, 3000); QCOMPARE(qvariant_cast<navidog::QueryOutcome>(spy.takeFirst()[0]).result.rows[0][0].toInt(), 7);
        int ticks = 0; QTimer heartbeat; heartbeat.setInterval(1); connect(&heartbeat, &QTimer::timeout, [&] { ++ticks; }); heartbeat.start();
        QVERIFY(service.run(profile_, "WITH RECURSIVE x(n) AS (VALUES(1) UNION ALL SELECT n+1 FROM x WHERE n<1000000000) SELECT sum(n) FROM x"));
        QVERIFY(!service.run(profile_, "SELECT 2"));
        QTimer::singleShot(30, &service, &navidog::QueryService::cancel);
        QTRY_COMPARE_WITH_TIMEOUT(spy.count(), 1, 3000);
        auto outcome = qvariant_cast<navidog::QueryOutcome>(spy.takeFirst()[0]); QVERIFY(outcome.result.cancelled); QVERIFY(ticks > 0); QVERIFY(!service.busy());
        QVERIFY(service.run(profile_, "SELECT 9")); QTRY_COMPARE_WITH_TIMEOUT(spy.count(), 1, 3000);
        QCOMPARE(qvariant_cast<navidog::QueryOutcome>(spy.takeFirst()[0]).result.rows[0][0].toInt(), 9);
    }
};
QTEST_GUILESS_MAIN(CoreTest)
#include "core_test.moc"
