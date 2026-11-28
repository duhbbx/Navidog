#include "jdbc_bridge.hpp"
#include "navidog/catalog.hpp"
#include "navidog/credentials.hpp"
#include "navidog/metadata_cache.hpp"
#include "navidog/database.hpp"
#include "navidog/query_context.hpp"
#include "navidog/query_plan.hpp"
#include "navidog/table_design.hpp"
#include "navidog/profile_store.hpp"
#include "navidog/ssh_tunnel.hpp"
#include <QJsonDocument>
#include <QTemporaryDir>
#include <QTest>
#include <QUuid>
#include <algorithm>
#include <future>
#include <thread>

using namespace navidog;

class JdbcTest final : public QObject {
    Q_OBJECT
private slots:
    void damengProfile() {
        ConnectionProfile profile;
        profile.driver = DatabaseKind::Dameng;
        profile.version = DatabaseVersion::Dameng8;
        profile.port = 5236;
        profile.jdbcDriverJar = "/drivers/dm.jar";
        auto restored = profile;
        restored.jdbcDriverJar.clear();
        readConnectionOptions(restored, connectionOptionsJson(profile));
        QCOMPARE(restored, profile);
        QCOMPARE(databaseKindFromKey("dameng"), profile.driver);
        QCOMPARE(detectVersion(profile.driver, "8.1.3.162"), profile.version);
        QVERIFY(availableDrivers().contains(profile.driver));
    }

    void damengSslProfileKeepsPasswordEncrypted() {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        ProfileStore store(dir.filePath("workspace.sqlite"));
        ConnectionProfile profile;
        profile.id = "dm";
        profile.name = "DM8";
        profile.driver = DatabaseKind::Dameng;
        profile.port = 5236;
        profile.tls = true;
        profile.ssl.damengKeystoreDirectory = "/fixtures/client";
        profile.ssl.damengKeystorePassword = "synthetic-keystore-pass";
        profile.ssl.saveDamengKeystorePassword = true;
        profile.credentialSource = CredentialSource::LocalEncrypted;
        QVERIFY(!QJsonDocument(connectionOptionsJson(profile)).toJson().contains("synthetic-keystore-pass"));
        store.save({profile});
        auto restored = store.load().at(0);
        QCOMPARE(restored.ssl.damengKeystoreDirectory, profile.ssl.damengKeystoreDirectory);
        QVERIFY(restored.ssl.damengKeystorePassword.isEmpty());
        QVERIFY(restored.ssl.saveDamengKeystorePassword);
        QCOMPARE(resolveCredentials(restored).ssl.damengKeystorePassword, profile.ssl.damengKeystorePassword);
    }

    void damengSsl() {
        const auto directory = qEnvironmentVariable("NAVIDOG_DM_TEST_TLS_DIR");
        if (directory.isEmpty())
            QSKIP("Set NAVIDOG_DM_TEST_TLS_DIR and TLS/localhost plain DM8 fixture ports");
        try {
            ConnectionProfile profile;
            profile.driver = DatabaseKind::Dameng;
            profile.host = "127.0.0.1";
            profile.port = qEnvironmentVariableIntValue("NAVIDOG_DM_TEST_TLS_PORT");
            profile.user = qEnvironmentVariable("NAVIDOG_DM_TEST_USER");
            profile.password = qEnvironmentVariable("NAVIDOG_DM_TEST_PASSWORD");
            profile.jdbcDriverJar = qEnvironmentVariable("NAVIDOG_DM_TEST_JAR");
            profile.ssl.damengKeystoreDirectory = directory;
            QVERIFY(profile.port > 0);
            auto tls = connectDatabase(profile);
            QCOMPARE(tls->execute("SELECT 1 FROM DUAL", {}, cancellation()).rows.at(0).at(0).toInt(), 1);

            auto wrongPassword = profile;
            wrongPassword.ssl.damengKeystorePassword = "wrong-keystore-password";
            QVERIFY_THROWS_EXCEPTION(std::runtime_error, connectDatabase(wrongPassword));

            auto untrusted = profile;
            untrusted.ssl.damengKeystoreDirectory = qEnvironmentVariable("NAVIDOG_DM_TEST_TLS_UNTRUSTED_DIR");
            if (!untrusted.ssl.damengKeystoreDirectory.isEmpty())
                QVERIFY_THROWS_EXCEPTION(std::runtime_error, connectDatabase(untrusted));

            auto noFallback = profile;
            noFallback.port = qEnvironmentVariableIntValue("NAVIDOG_DM_TEST_PORT");
            QVERIFY(noFallback.port > 0);
            try {
                connectDatabase(noFallback);
                QFAIL("DM8 server without SSL was accepted by an SSL profile");
            } catch (const std::runtime_error& error) {
                QVERIFY(QString::fromUtf8(error.what()).contains("ENABLE_ENCRYPT"));
            }

            const int sshPort = qEnvironmentVariableIntValue("NAVIDOG_DM_TEST_SSH_PORT");
            if (sshPort && sshAvailable()) {
                profile.ssh.enabled = true;
                profile.ssh.host = "127.0.0.1";
                profile.ssh.port = sshPort;
                profile.ssh.user = "bridge";
                profile.ssh.password = qEnvironmentVariable("NAVIDOG_DM_TEST_SSH_PASSWORD");
                try {
                    connectDatabase(profile);
                    QFAIL("Unknown SSH host key was accepted");
                } catch (const SshHostKeyError& error) {
                    QCOMPARE(error.publicKey, qEnvironmentVariable("NAVIDOG_DM_TEST_SSH_KEY"));
                    profile.ssh.hostKey = error.publicKey;
                }
                auto tunnel = connectDatabase(profile);
                QCOMPARE(tunnel->execute("SELECT 1 FROM DUAL", {}, cancellation()).rows.at(0).at(0).toInt(), 1);
            }
        } catch (const std::exception& error) {
            QFAIL(error.what());
        }
    }

    void damengIntegration() {
        try {
            const auto jar = qEnvironmentVariable("NAVIDOG_DM_TEST_JAR");
            if (jar.isEmpty())
                QSKIP("Set NAVIDOG_DM_TEST_JAR and the credentials for a disposable loopback DM8 server");
            bool validPort = false;
            const int port = qEnvironmentVariableIntValue("NAVIDOG_DM_TEST_PORT", &validPort);
            QVERIFY(validPort && port > 0 && port <= 65535);
            ConnectionProfile profile;
            profile.driver = DatabaseKind::Dameng;
            profile.host = "127.0.0.1";
            profile.port = port;
            profile.tls = false;
            profile.user = qEnvironmentVariable("NAVIDOG_DM_TEST_USER");
            profile.password = qEnvironmentVariable("NAVIDOG_DM_TEST_PASSWORD");
            profile.jdbcDriverJar = jar;
            QVERIFY(!profile.user.isEmpty() && !profile.password.isEmpty());
            profile.secretsLoaded = true;

            auto db = connectDatabase(profile);
            QCOMPARE(detectVersion(profile.driver, db->serverVersion()), DatabaseVersion::Dameng8);
            auto cancel = cancellation();
            const auto name = "ND_JDBC_" + QUuid::createUuid().toString(QUuid::Id128).left(12).toUpper();
            db->execute("CREATE TABLE " + name +
                        " (ID INT PRIMARY KEY, NAME VARCHAR(100), VALUE DECIMAL(18,4), EXTRA BLOB)", {}, cancel);
            struct Drop {
                Database& db;
                QString name;
                ~Drop() {
                    try { db.execute("DROP TABLE " + name, {}, cancellation()); } catch (...) {}
                }
            } drop{*db, name};

            QCOMPARE(db->executeBound("INSERT INTO " + name + " VALUES (?, ?, ?, ?)",
                {1, QStringLiteral("武汉"), QStringLiteral("12345678901234.5678"), QByteArray::fromHex("0001ff")},
                {}, cancel).affectedRows, 1);
            db->transaction(TransactionAction::Begin, cancel);
            db->executeBound("INSERT INTO " + name + " VALUES (?, ?, ?, ?)",
                {2, QStringLiteral("回滚"), QStringLiteral("0.1"), QVariant{}}, {}, cancel);
            QCOMPARE(db->transactionState(), TransactionState::Active);
            QCOMPARE(db->execute("SELECT COUNT(*) FROM " + name, {}, cancel).rows.at(0).at(0).toString(), "2");

            auto concurrent = std::async(std::launch::async, [&] {
                auto db2 = connectDatabase(profile);
                return db2->execute("SELECT COUNT(*) FROM " + name, {}, cancellation()).rows.at(0).at(0).toString();
            });
            QCOMPARE(concurrent.get(), "1");
            db->transaction(TransactionAction::Rollback, cancel);
            QCOMPARE(db->transactionState(), TransactionState::Idle);
            db->setAutoCommit(true);
            auto row = db->execute("SELECT NAME, VALUE, EXTRA, NULL FROM " + name, {}, cancel).rows.at(0);
            QCOMPARE(row[0].toString(), "武汉");
            QCOMPARE(row[1].toString(), "12345678901234.5678");
            QCOMPARE(row[2].toByteArray(), QByteArray::fromHex("0001ff"));
            QVERIFY(row[3].isNull());

            QueryLimits clipped;
            clipped.maxRows = 1;
            clipped.maxCellBytes = 2;
            auto tiny = db->execute("SELECT NAME, EXTRA FROM " + name, clipped, cancel);
            QVERIFY(tiny.cellTruncated);
            QCOMPARE(tiny.rows.at(0).at(1).toByteArray(), QByteArray::fromHex("0001"));

            auto current = db->execute("SELECT SYS_CONTEXT('USERENV','CURRENT_SCHEMA') FROM dual", {}, cancel);
            const auto schema = current.rows.at(0).at(0).toString();
            applyQuerySchema(*db, profile, schema, cancel);
            QVERIFY(db->tables().contains(name));
            CatalogNode node;
            node.kind = "connection";
            auto find = [](const CatalogResult& result, const QString& key) {
                return std::find_if(result.nodes.cbegin(), result.nodes.cend(),
                    [&](const auto& entry) { return entry.name == key; });
            };
            const auto schemas = loadCatalog(*db, profile, node, cancel);
            QVERIFY(find(schemas, schema) != schemas.nodes.end());
            node = *find(schemas, schema);
            node.kind = "tables";
            const auto tables = loadCatalog(*db, profile, node, cancel);
            QVERIFY(find(tables, name) != tables.nodes.end());
            node = *find(tables, name);
            QCOMPARE(db->execute(previewSql(profile, node), {}, cancel).rows.size(), 1);
            node.kind = "columns";
            const auto columns = loadCatalog(*db, profile, node, cancel);
            QVERIFY(find(columns, "VALUE") != columns.nodes.end());
            const auto snapshot = collectMetadata(*db, profile, schema, cancel);
            QVERIFY(std::any_of(snapshot.objects.cbegin(), snapshot.objects.cend(),
                [&](const auto& object) { return object.name == name && object.columns.size() == 4; }));

            const auto plan = readQueryPlan(profile, "SELECT NAME FROM " + name + " WHERE ID=1",
                                            {}, cancel, {}, schema);
            const auto tree = decodeEstimatedPlan(profile.driver, plan);
            QVERIFY2(tree.complete, qPrintable(tree.warning));
            QVERIFY(!tree.roots.isEmpty());

            auto stopped = cancellation();
            stopped->store(true);
            QVERIFY(db->execute("SELECT 1 FROM DUAL", {}, stopped).cancelled);
            QCOMPARE(db->execute("SELECT 1 FROM DUAL", {}, cancel).rows.size(), 1);
        } catch (const std::exception& error) {
            QFAIL(error.what());
        }
    }

    void h2ConnectionCatalogAndAlter() {
        const auto jar = qEnvironmentVariable("NAVIDOG_JDBC_TEST_H2_JAR");
        if (jar.isEmpty()) QSKIP("Set NAVIDOG_JDBC_TEST_H2_JAR");
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        const auto path = directory.filePath("navidog_fixture");
        const auto token = cancellation();
        try {
            const JdbcConnectionSpec setup{jar, "org.h2.Driver", "jdbc:h2:file:" + path,
                {{"user", "sa"}}, sql::Dialect::SQLite, 5, 5};
            const auto created = connectJdbc(setup)->execute(
                "CREATE TABLE INVENTORY (ID INTEGER PRIMARY KEY, QUANTITY DECIMAL(12,3))", {}, token);
            QVERIFY2(created.error.isEmpty(), qPrintable(created.error));
            ConnectionProfile profile;
            profile.id = "h2-fixture";
            profile.driver = DatabaseKind::H2;
            profile.tls = false;
            profile.h2File = true;
            profile.database = path + ".mv.db";
            profile.jdbcDriverJar = jar;
            profile.user = "sa";
            profile.credentialSource = CredentialSource::Passwordless;
            auto db = connectDatabase(profile);
            const auto schemas = loadCatalog(*db, profile, {}, token);
            QVERIFY(std::any_of(schemas.nodes.begin(), schemas.nodes.end(),
                [](const auto& node) { return node.kind == "schema" && node.name == "PUBLIC"; }));
            const CatalogNode folder{"tables", "表", profile.database, "PUBLIC"};
            const auto tables = loadCatalog(*db, profile, folder, token);
            QCOMPARE(tables.nodes.size(), 1);
            const auto table = tables.nodes.first();
            QCOMPARE(table.name, "INVENTORY");
            auto fields = table;
            fields.kind = "columns";
            QCOMPARE(loadCatalog(*db, profile, fields, token).nodes.size(), 2);
            const auto metadata = collectMetadata(*db, profile, "PUBLIC", token);
            QVERIFY(metadata.tablesComplete && metadata.columnsComplete);
            QVERIFY(std::any_of(metadata.objects.begin(), metadata.objects.end(), [](const auto& object) {
                return object.name == "INVENTORY" && object.columns.size() == 2;
            }));
            auto before = readTableDefinition(*db, profile, table, token);
            QCOMPARE(before.columns.first().primaryKey, 1);
            QCOMPARE(before.columns.at(1).type, "DECIMAL(12,3)");
            // A query session remains open while metadata and table design use separate helpers.
            auto second = connectDatabase(profile);
            QCOMPARE(second->execute("SELECT COUNT(*) FROM INVENTORY", {}, token).rows.first().first().toInt(), 0);
            second.reset();
            auto after = before;
            TableColumn added;
            added.name = "NOTE";
            added.type = "VARCHAR(64)";
            after.columns << added;
            const auto applied = applyTableChanges(profile, before, after, token);
            QVERIFY2(applied.error.isEmpty(), qPrintable(applied.error));
            const auto edited = readTableDefinition(*db, profile, table, token);
            QCOMPARE(edited.columns.size(), 3);
            const auto rows = db->execute(previewSql(profile, table), {}, token);
            QVERIFY2(rows.error.isEmpty(), qPrintable(rows.error));
            QVERIFY(rows.columns.contains("NOTE"));
        } catch (const std::exception& error) {
            QFAIL(error.what());
        }
    }

    void genericH2Integration() {
        try {
            const auto jar = qEnvironmentVariable("NAVIDOG_JDBC_TEST_H2_JAR");
            if (jar.isEmpty())
                QSKIP("Set NAVIDOG_JDBC_TEST_H2_JAR to check a second JDBC vendor using an in-memory database");
            const JdbcConnectionSpec spec{jar, "org.h2.Driver", "jdbc:h2:mem:navidog_bridge_test",
                {{"user", "sa"}}, sql::Dialect::SQLite, 5, 5};
            auto db = connectJdbc(spec);
            auto token = cancellation();
            auto numbered = db->execute("SELECT X FROM SYSTEM_RANGE(1, 10)", {2}, token);
            QCOMPARE(numbered.rows.size(), 2);
            QVERIFY(numbered.truncated);

            auto interrupted = cancellation();
            auto cancelSoon = std::async(std::launch::async, [interrupted] {
                std::this_thread::sleep_for(std::chrono::milliseconds(200));
                interrupted->store(true);
            });
            auto stopped = db->execute(
                "SELECT SUM(MOD(X, 1234567)) FROM SYSTEM_RANGE(1, 2000000000)", {}, interrupted);
            cancelSoon.get();
            QVERIFY(stopped.cancelled);
            QVERIFY(db->healthy());
            QCOMPARE(db->execute("SELECT 42", {}, token).rows.at(0).at(0).toInt(), 42);
        } catch (const std::exception& error) {
            QFAIL(error.what());
        }
    }
};

QTEST_GUILESS_MAIN(JdbcTest)
#include "jdbc_test.moc"
