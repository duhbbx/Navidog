#include <QtTest>
#include <QTemporaryDir>
#include <QProcess>
#include <QTcpServer>
#include <QFile>
#include <QUuid>
#include <QDir>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QProcessEnvironment>
#include <algorithm>
#include <limits>
#include "navidog/ssh_tunnel.hpp"
#include "navidog/federated_query.hpp"
#include "navidog/http_tunnel.hpp"
#include "navidog/data_edit.hpp"
#include "navidog/query_service.hpp"
#include "navidog/query_plan.hpp"
#include "navidog/sql.hpp"
#include "navidog/sql/compiler/compiler.hpp"
#include "navidog/sql/optimizer/optimizer.hpp"
#include "navidog/sql/compiler/conversion.hpp"
#include "navidog/sql/compiler/formatter.hpp"
#include "navidog/sql/compiler/script_context.hpp"
#include "navidog/metadata_cache.hpp"
#include "navidog/source_design.hpp"
#include "navidog/table_design.hpp"
#include "navidog/namespace_design.hpp"
#include "navidog/er_model.hpp"
#include "navidog/database_tools.hpp"
class MySqlSchemaTest:public QObject {
    Q_OBJECT
    QTemporaryDir dir_;
    QProcess server_;
    navidog::ConnectionProfile p_;
    QString serverBinary_,container_;
private slots:
    void initTestCase(){
#ifdef NAVIDOG_MARIADB_TEST
        using namespace navidog;const auto image=qEnvironmentVariable("NAVIDOG_TEST_MARIADB_IMAGE");if(image.isEmpty() || !availableDrivers().contains(DatabaseKind::MariaDB))QSKIP("Set NAVIDOG_TEST_MARIADB_IMAGE to a disposable official MariaDB image");
        QVERIFY(dir_.isValid());container_="navidog-owned-mariadb-"+QUuid::createUuid().toString(QUuid::Id128).left(12);p_.id=container_;p_.driver=DatabaseKind::MariaDB;p_.host="127.0.0.1";p_.user="root";p_.tls=false;p_.password="N9_"+QUuid::createUuid().toString(QUuid::Id128);p_.advanced.connectTimeout=2;
        QTcpServer reserve;QVERIFY(reserve.listen(QHostAddress::LocalHost,0));p_.port=reserve.serverPort();reserve.close();
        server_.start("docker",{"run","--rm","-i","--name",container_,"--label","app.navidog.test=true","--memory=1g","--cpus=2","-p","127.0.0.1:"+QString::number(p_.port)+":3306","--entrypoint","/bin/bash",image,"-c","IFS= read -r MARIADB_ROOT_PASSWORD; export MARIADB_ROOT_PASSWORD; export MARIADB_ROOT_HOST=%; exec docker-entrypoint.sh mariadbd --innodb-buffer-pool-size=32M --skip-log-bin"});QVERIFY(server_.waitForStarted(5000));server_.write(p_.password.toUtf8()+"\n");server_.closeWriteChannel();
        QElapsedTimer elapsed;elapsed.start();bool ready=false;QString error;
        while(elapsed.elapsed()<90000 && server_.state()!=QProcess::NotRunning){try{auto db=connectDatabase(p_);auto result=db->execute("CREATE DATABASE navidog_owned CHARACTER SET utf8mb4",{},cancellation());if(result.error.isEmpty()){ready=true;break;}error=result.error;}catch(const std::exception& e){error=QString::fromUtf8(e.what());}QTest::qWait(200);}
        if(!ready){auto log=QString::fromUtf8(server_.readAllStandardOutput()+server_.readAllStandardError());log.replace(p_.password,"[redacted]");error.replace(p_.password,"[redacted]");qWarning().noquote()<<error<<log.right(3000);}QVERIFY(ready);p_.database="navidog_owned";qInfo()<<"Owned MariaDB fixture ready";
#else
        serverBinary_=qEnvironmentVariable("NAVIDOG_TEST_MYSQLD");if(serverBinary_.isEmpty() || !navidog::availableDrivers().contains(navidog::DatabaseKind::MySQL))QSKIP("Set NAVIDOG_TEST_MYSQLD for a disposable MySQL instance");
        QVERIFY(dir_.isValid());QProcess initialize;initialize.start(serverBinary_,{"--no-defaults","--initialize-insecure","--datadir="+dir_.filePath("data"),"--log-error="+dir_.filePath("init.log")});QVERIFY(initialize.waitForFinished(30000));QCOMPARE(initialize.exitCode(),0);
        QTcpServer reserve;QVERIFY(reserve.listen(QHostAddress::LocalHost,0));auto port=reserve.serverPort();reserve.close();
        server_.start(serverBinary_,{"--no-defaults","--datadir="+dir_.filePath("data"),"--socket="+dir_.filePath("mysql.sock"),"--port="+QString::number(port),"--bind-address=127.0.0.1","--mysqlx=0","--pid-file="+dir_.filePath("mysql.pid"),"--log-error="+dir_.filePath("server.log"),"--skip-log-bin","--innodb-buffer-pool-size=32M"});QVERIFY(server_.waitForStarted());
        p_.driver=navidog::DatabaseKind::MySQL;p_.host="127.0.0.1";p_.port=port;p_.user="root";p_.tls=false;p_.credentialSource=navidog::CredentialSource::Passwordless;bool ready=false;
        for(int i=0;i<100;++i){try{auto db=navidog::connectDatabase(p_);auto r=db->execute("CREATE DATABASE navidog_owned CHARACTER SET utf8mb4",{},navidog::cancellation());if(r.error.isEmpty()){ready=true;break;}}catch(...){}QTest::qWait(100);}
        if(!ready){QFile log(dir_.filePath("server.log"));if(log.open(QIODevice::ReadOnly))qWarning().noquote()<<log.readAll();}QVERIFY(ready);p_.database="navidog_owned";
#endif
    }
    void erReverseAndDiffUseNativeCatalog() { try {
        using namespace navidog;
        auto db = connectDatabase(p_);
        const auto cancel = cancellation();
        const auto run = [&](const QString& statement) {
            auto r = db->execute(statement,{},cancel);
            if (!r.error.isEmpty()) throw std::runtime_error(r.error.toStdString());
        };
        run("CREATE TABLE er_customer (id INT PRIMARY KEY, alias VARCHAR(42) UNIQUE)");
        run("CREATE TABLE er_purchase (id INT PRIMARY KEY, customer_id INT, total INT CHECK(total>=0), "
            "CONSTRAINT er_owner FOREIGN KEY(customer_id) REFERENCES er_customer(id), INDEX er_value(total))");
        run("CREATE VIEW er_overview AS SELECT id,total FROM er_purchase");
        er::ImportOptions options; options.nameContains = "er_";
        auto project = er::reverse(*db,p_,options,cancel).project;
        QCOMPARE(project.entities.size(),3);
        const auto graph = er::makeGraph(project);
        QCOMPARE(graph.edges.size(),1);
        auto& purchase = project.entities[graph.edges.front().from].table;
        TableColumn column; column.name="new_text";column.type="VARCHAR(42)";
        purchase.columns.push_back(column);
        auto changes = er::ddl(project,true);
        QCOMPARE(changes.size(),1);
        run(changes.front());
        const auto create = er::ddl(project,false);
        QVERIFY(std::any_of(create.begin(),create.end(),[](const auto& sql){return sql.startsWith("CREATE VIEW");}));
        QVERIFY(std::any_of(create.begin(),create.end(),[](const auto& sql){return sql.startsWith("CREATE INDEX");}));
        run("DROP VIEW er_overview");run("DROP TABLE er_purchase");run("DROP TABLE er_customer");
    } catch (const std::exception& e) { QFAIL(e.what()); } }
    void databaseToolsUseMySqlCatalogAndLiteralSearch() { try {
        using namespace navidog;
        const auto token = cancellation();
        auto db = connectDatabase(p_);
        const auto create = db->execute("CREATE TABLE search_menu_owned (name VARCHAR(60) COMMENT '商品名称', "
                                        "note VARCHAR(60)) COMMENT='商品主表'", {}, token);
        QVERIFY2(create.error.isEmpty(), qPrintable(create.error));
        const auto insert = db->execute("INSERT INTO search_menu_owned VALUES "
                                        "('one%_token','%_token'),('no_match','ordinary')", {}, token);
        QVERIFY2(insert.error.isEmpty(), qPrintable(insert.error));
        const auto result = searchDatabase(p_, {{}, "%_token", DatabaseSearchScope::Data,
                                               DatabaseSearchMode::Contains}, token);
        QCOMPARE(result.hits.size(), 1);
        QCOMPARE(result.hits.front().node.object, QString("search_menu_owned"));
        QCOMPARE(result.hits.front().matches, qint64(1));
        QCOMPARE(db->execute(result.hits.front().previewSql, {}, token).rows.size(), 1);
        const auto dictionary = loadDatabaseDictionary(p_, {}, token);
        const auto table = std::find_if(dictionary.tables.cbegin(), dictionary.tables.cend(),
            [](const auto& value) { return value.name.endsWith(".`search_menu_owned`"); });
        QVERIFY(table != dictionary.tables.cend());
        QCOMPARE(table->comment, QString("商品主表"));
        QCOMPARE(table->columns.value(0).comment, QString("商品名称"));
        const auto html = databaseDictionaryHtml(dictionary, token);
        QVERIFY(html.contains("商品主表"));
        QVERIFY(html.contains("商品名称"));

        CatalogNode node;
        node.kind = "database";
        node.name = node.database = p_.database;
        auto properties = loadDatabaseProperties(p_, node, token);
        QCOMPARE(properties.charset, QString("utf8mb4"));
        QVERIFY(!properties.collation.isEmpty());
        properties.collation = "utf8mb4_bin";
        applyDatabaseProperties(p_, node, properties, token);
        QCOMPARE(loadDatabaseProperties(p_, node, token).collation, QString("utf8mb4_bin"));
        QVERIFY(db->execute("DROP TABLE search_menu_owned", {}, token).error.isEmpty());
    } catch (const std::exception& e) { QFAIL(e.what()); } }
    void findsAndCachesDefinitionsInAnotherDatabase() { try {
        using namespace navidog;
        auto profile = p_;
        profile.id = "owned-cross-database-search";
        auto db = connectDatabase(profile);
        const auto token = cancellation();
        const auto execute = [&](const QString& sql) {
            const auto result = db->execute(sql, {}, token);
            if (!result.error.isEmpty()) throw std::runtime_error(result.error.toStdString());
        };
        execute("CREATE DATABASE owned_definition_search");
        const QString marker = "navidog_definition_phrase_024";
        execute("CREATE VIEW owned_definition_search.fixture_view AS SELECT '" + marker + "' AS label");
        execute("CREATE FUNCTION owned_definition_search.fixture_function() RETURNS VARCHAR(60) DETERMINISTIC RETURN '" + marker + "'");
        execute("CREATE PROCEDURE owned_definition_search.fixture_proc() SELECT '" + marker + "'");
        QTemporaryDir cache;
        QVERIFY(cache.isValid());
        const auto path = cache.filePath("search.sqlite");
        const DatabaseSearchRequest request{{}, marker, DatabaseSearchScope::ObjectDefinitions,
                                            DatabaseSearchMode::Contains};
        const auto result = searchDatabaseObjects(profile, {}, request, path, true, token);
        QVERIFY2(result.hits.size() == 3, qPrintable(result.warnings.join('\n')));
        QSet<QString> kinds;
        for (const auto& hit : result.hits) {
            QCOMPARE(hit.node.database, QString("owned_definition_search"));
            QVERIFY(hit.definition.contains(marker));
            kinds.insert(hit.node.kind);
        }
        QCOMPARE(kinds, (QSet<QString>{"view", "function", "procedure"}));
        const auto outside = searchDatabaseObjects(profile, profile.database, request, path, false, token);
        QVERIFY(outside.hits.isEmpty());
        const auto cached = searchDatabaseObjects(profile, "owned_definition_search", request, path, false, token);
        QCOMPARE(cached.cachedScopes, 1);
        QCOMPARE(cached.hits.size(), 3);
        execute("DROP DATABASE owned_definition_search");
    } catch (const std::exception& e) { QFAIL(e.what()); } }

    void nativeFederationPushesSignedIntegerFilters() {try {
        using namespace navidog;
        using namespace navidog::sql::compiler;
        using namespace navidog::sql::optimizer;
        auto db=connectDatabase(p_);
        const auto token=cancellation();
        const auto run=[&](const QString &sql) {
            const auto result=db->execute(sql,{},token);
            if (!result.error.isEmpty()) throw std::runtime_error(result.error.toStdString());
        };
        run("CREATE TABLE owned_filter_wide(k SMALLINT,v BIGINT,u BIGINT UNSIGNED,INDEX(k),INDEX(v))");
        const QString digits="(SELECT 0 AS n UNION ALL SELECT 1 UNION ALL SELECT 2 UNION ALL SELECT 3 "
                             "UNION ALL SELECT 4 UNION ALL SELECT 5 UNION ALL SELECT 6 UNION ALL SELECT 7 "
                             "UNION ALL SELECT 8 UNION ALL SELECT 9)";
        run(QStringLiteral("INSERT INTO owned_filter_wide(k,v,u) "
            "SELECT n,n,0 FROM (SELECT 1+a.n+10*b.n+100*c.n+1000*d.n AS n "
            "FROM %1 a CROSS JOIN %1 b CROSS JOIN %1 c CROSS JOIN %1 d WHERE d.n<3) numbers").arg(digits));
        run("INSERT INTO owned_filter_wide VALUES "
            "(-32768,-9223372036854775808,1),(32767,9223372036854775807,2),(NULL,NULL,3),(2,8000000000,4)");
        run("CREATE TABLE owned_filter_narrow(k SMALLINT,v BIGINT)");
        run("INSERT INTO owned_filter_narrow VALUES(2,8000000000)");
        const auto type=[](const char *name) {
            return std::make_shared<const SqlType>(*catalogTypeFromText(Dialect::PostgreSQL,name));
        };
        navidog::sql::compiler::TableDefinition wide;
        wide.database="mysql_owned";wide.schema="local";wide.name="wide";
        wide.columns={{"k",ValueType::Integer,type("smallint")},{"v",ValueType::Integer,type("bigint")},
                      {"u",ValueType::Integer,type("bigint")}};
        auto narrow=wide;
        narrow.name="narrow";
        narrow.columns.pop_back();
        MemoryCatalog catalog;
        catalog.tables={wide,narrow};
        const NameResolutionContext names{"mysql_owned","local",Dialect::PostgreSQL};
        const std::vector<FederatedTableBinding> tables{
            {names.database,names.schema,"wide",p_.driver,db.get(),p_.database,"owned_filter_wide"},
            {names.database,names.schema,"narrow",p_.driver,db.get(),p_.database,"owned_filter_narrow"}};
        const auto query=[&](const std::string &sql) {
            auto compiled=compile(Dialect::PostgreSQL,sql,{},&catalog,names);
            if (!compiled->ok()) throw std::runtime_error(compiled->diagnostics.front().message);
            return compiled;
        };
        InterpreterLimits limits;
        limits.maxRows=1;
        const auto number=[](std::int64_t value){ return LocalValue{value}; };
        const LocalValue null;
        for (const auto &[sql,value]:std::vector<std::pair<std::string,LocalValue>>{
                 {"SELECT v FROM wide WHERE v=-9223372036854775808;",number(std::numeric_limits<std::int64_t>::min())},
                 {"SELECT v FROM wide WHERE v>=9223372036854775807;",number(std::numeric_limits<std::int64_t>::max())},
                 {"SELECT k FROM wide WHERE k>32766 AND k<32768;",number(32767)},
                 {"SELECT k FROM wide WHERE k IS NULL;",null},
                 {"SELECT k FROM wide WHERE k IN (1,3,NULL) AND k<2;",number(1)},
                 {"SELECT k FROM wide WHERE k=-32768 OR k IN(-32768,99999,NULL);",number(-32768)},
                 {"SELECT v FROM wide ORDER BY v ASC LIMIT 1;",number(std::numeric_limits<std::int64_t>::min())},
                 {"SELECT v FROM wide ORDER BY v DESC LIMIT 1;",null},
                 {"SELECT v FROM wide ORDER BY v DESC NULLS LAST LIMIT 1;",number(std::numeric_limits<std::int64_t>::max())},
                 {"SELECT k FROM wide ORDER BY k ASC NULLS FIRST LIMIT 1;",null},
                 {"SELECT v FROM wide WHERE k>=2900 ORDER BY v DESC NULLS LAST LIMIT 1;",number(std::numeric_limits<std::int64_t>::max())},
                 {"SELECT v FROM wide WHERE k IN (1,2000,3000,NULL) ORDER BY v DESC LIMIT 1;",number(3000)},
                 {"SELECT MIN(v) FROM wide;",number(std::numeric_limits<std::int64_t>::min())},
                 {"SELECT MAX(v) FROM wide;",number(std::numeric_limits<std::int64_t>::max())}}) {
            const auto result=executeFederatedQuery(query(sql)->model,tables,token,limits);
            QVERIFY2(result.ok(),(sql+" "+(result.diagnostics.empty()?"no rows":result.diagnostics.front().message)).c_str());
            QVERIFY(result.rows==std::vector<LocalRow>{{value}});
        }
        const auto empty=executeFederatedQuery(query("SELECT k FROM wide WHERE k>=32768")->model,tables,token,limits);
        QVERIFY(empty.ok() && empty.rows.empty());
        const auto join=executeFederatedQuery(query("SELECT r.v FROM narrow l JOIN wide r ON "
                "l.k=r.k AND l.v=r.v")->model,tables,token,limits);
        QVERIFY2(join.ok(),join.diagnostics.empty()?"no joined row":join.diagnostics.front().message.c_str());
        QVERIFY(join.rows==std::vector<LocalRow>{{number(8000000000LL)}});
        const auto unverified=executeFederatedQuery(query("SELECT u FROM wide WHERE u=77")->model,tables,token,limits);
        QVERIFY(!unverified.ok() && unverified.rows.empty());
        QCOMPARE(unverified.diagnostics.front().code,DiagnosticCode::UnsupportedSyntax);
        const auto twoKeys=executeFederatedQuery(query("SELECT k,v FROM wide ORDER BY k NULLS LAST, v DESC LIMIT 1")->model,
                                                 tables,token,limits);
        QVERIFY2(twoKeys.ok(),twoKeys.diagnostics.empty()?"no ordered result":twoKeys.diagnostics.front().message.c_str());
        QVERIFY((twoKeys.rows==std::vector<LocalRow>{{number(-32768),number(std::numeric_limits<std::int64_t>::min())}}));
        for (const auto *sql:{"SELECT k,v FROM wide ORDER BY k LIMIT 1",
                              "SELECT v FROM wide WHERE k=1 OR v=3000 ORDER BY v LIMIT 1"}) {
            const auto fallback=executeFederatedQuery(query(sql)->model,tables,token,limits);
            QVERIFY(!fallback.ok() && fallback.rows.empty());
            QCOMPARE(fallback.diagnostics.front().code,DiagnosticCode::ResourceLimit);
        }
        const auto prep=prepareLocalQuery(query("SELECT v FROM wide WHERE k IN ($1,$2) ORDER BY v DESC LIMIT 1")->model);
        QVERIFY2(prep.ok(),prep.diagnostics.empty()?"no prepared page":prep.diagnostics.front().message.c_str());
        for (const auto &args:std::vector<LocalRow>{{number(1),number(2999)},{number(2),null}}) {
            const auto fetched=executeFederatedQuery(*prep.query,args,tables,token,limits);
            QVERIFY2(fetched.ok(),fetched.diagnostics.empty()?"no prepared page result":fetched.diagnostics.front().message.c_str());
            const auto expected=std::get<std::int64_t>(args.front())==1?2999LL:8000000000LL;
            QVERIFY(fetched.rows==std::vector<LocalRow>{{number(expected)}});
        }
        limits.maxRows=2;
        const auto next=executeFederatedQuery(query("SELECT v FROM wide WHERE k>0 "
                  "ORDER BY v DESC NULLS LAST LIMIT 1 OFFSET 1")->model,tables,token,limits);
        QVERIFY2(next.ok(),next.diagnostics.empty()?"no second page":next.diagnostics.front().message.c_str());
        QVERIFY(next.rows==std::vector<LocalRow>{{number(8000000000LL)}});
        run("DROP TABLE owned_filter_narrow");
        run("DROP TABLE owned_filter_wide");
    }catch(const std::exception &e){QFAIL(e.what());}}
    void federatesSignedMySqlIntegersWithSQLite() {try {
        using namespace navidog;
        using namespace navidog::sql::compiler;
        using namespace navidog::sql::optimizer;
        if (p_.driver != DatabaseKind::MySQL) QSKIP("MySQL-specific native type mapping");
        auto mysql=connectDatabase(p_);
        const auto token=cancellation();
        const auto run=[&](Database &db, const QString &sql) {
            const auto value=db.execute(sql,{},token);
            if (!value.error.isEmpty()) throw std::runtime_error(value.error.toStdString());
        };
        run(*mysql,"CREATE TABLE federated_owned(k MEDIUMINT,v TINYINT,u BIGINT UNSIGNED)");
        run(*mysql,"INSERT INTO federated_owned VALUES "
                   "(-8388608,-128,18446744073709551615),(1,127,4),(2,NULL,5),(NULL,0,0)");
        ConnectionProfile file;
        file.database=dir_.filePath("federated_owned.sqlite");
        {
            QFile output(file.database);
            QVERIFY(output.open(QIODevice::WriteOnly));
        }
        auto sqlite=connectDatabase(file);
        run(*sqlite,"CREATE TABLE values_owned(k INTEGER,w INTEGER)");
        run(*sqlite,"INSERT INTO values_owned VALUES(1,9),(1,9),(2,2147483648),(NULL,1)");
        const auto integer=[](const char *name) {
            return std::make_shared<const SqlType>(*catalogTypeFromText(Dialect::PostgreSQL,name));
        };
        navidog::sql::compiler::TableDefinition native;
        native.database="mysql_owned";native.schema="local";native.name="rows";
        native.columns={{"k",ValueType::Integer,integer("INTEGER")},{"v",ValueType::Integer,integer("SMALLINT")},
                        {"u",ValueType::Integer,integer("BIGINT")}};
        auto local=native;
        local.database="sqlite_owned";
        local.columns={{"k",ValueType::Integer,integer("BIGINT")},{"w",ValueType::Integer,integer("BIGINT")}};
        MemoryCatalog catalog;
        catalog.tables={native,local};
        const std::vector<FederatedTableBinding> tables{
            {"mysql_owned","local","rows",DatabaseKind::MySQL,mysql.get(),p_.database,"federated_owned"},
            {"sqlite_owned","local","rows",DatabaseKind::SQLite,sqlite.get(),"main","values_owned"}};
        const auto execute=[&](const char *sql) {
            const auto query=compile(Dialect::PostgreSQL,sql,{},&catalog);
            if (!query->ok()) throw std::runtime_error(query->diagnostics.front().message);
            return executeFederatedQuery(query->model,tables,token);
        };
        const auto result=execute("SELECT m.k,m.v,s.w FROM mysql_owned.local.\"rows\" m "
            "LEFT JOIN sqlite_owned.local.\"rows\" s ON m.k=s.k ORDER BY 1,3;");
        QVERIFY2(result.ok(),result.diagnostics.empty()?"no rows":result.diagnostics.front().message.c_str());
        const auto number=[](std::int64_t value){return LocalValue{value};};
        const LocalValue null;
        const std::vector<LocalRow> expected{
            {number(-8388608),number(-128),null},{number(1),number(127),number(9)},
            {number(1),number(127),number(9)},{number(2),null,number(2147483648LL)},
            {null,number(0),null}};
        QVERIFY(result.rows==expected);
        QProcess cli;
        QString command=QCoreApplication::applicationDirPath()+"/navidog-federate";
#ifdef Q_OS_WIN
        command += ".exe";
#endif
        const auto uri="remote=mysql://"+p_.user+"@"+p_.host+":"+
            QString::number(p_.port)+"/"+p_.database+"?sslmode=disable";
        cli.start(command,{"--mysql",uri,"--sqlite","local="+file.database,
                           "--table","remote."+p_.database+".federated_owned","--table","local.main.values_owned","-"});
        QVERIFY2(cli.waitForStarted(5000),qPrintable(cli.errorString()));
        cli.write(("SELECT m.k,m.v,s.w FROM remote."+p_.database+".federated_owned m "
                   "LEFT JOIN local.main.values_owned s ON m.k=s.k ORDER BY 1,3;").toUtf8());
        cli.closeWriteChannel();
        QVERIFY(cli.waitForFinished(10000));
        const auto stdoutText=cli.readAllStandardOutput();
        const auto stderrText=cli.readAllStandardError();
        QVERIFY2(cli.exitStatus()==QProcess::NormalExit && cli.exitCode()==0,
                 (stderrText+stdoutText).constData());
        const auto json=QJsonDocument::fromJson(stdoutText).object();
        QCOMPARE(json["rows"],QJsonDocument::fromJson(
            "[[-8388608,-128,null],[1,127,9],[1,127,9],[2,null,2147483648],[null,0,null]]").array());
        const auto unsignedField=execute("SELECT u FROM mysql_owned.local.\"rows\";");
        QVERIFY(!unsignedField.ok() && unsignedField.rows.empty() &&
                unsignedField.diagnostics.front().code==DiagnosticCode::UnsupportedSyntax);
        run(*mysql,"DROP TABLE federated_owned");
    }catch(const std::exception &e){QFAIL(e.what());}}
    void convertedPgFunctionExecutesWithLocalVariables() {try {
        using namespace navidog::sql::compiler;auto db=navidog::connectDatabase(p_);auto token=navidog::cancellation();
        auto run=[&](const QString& sql){auto r=db->execute(sql,{},token);if(!r.error.isEmpty())throw std::runtime_error(r.error.toStdString());return r;};
        auto result=convertSql(Dialect::PostgreSQL,Dialect::MySQL,"CREATE FUNCTION conversion_add(p INTEGER) RETURNS INTEGER LANGUAGE plpgsql IMMUTABLE AS $$ DECLARE v INTEGER:=p; BEGIN IF v>1 THEN v:=v+2; ELSIF v=1 THEN v:=10; ELSE v:=0; END IF; RETURN v; END; $$;");
        QVERIFY2(result.ok(),result.diagnostics.empty()?"no result":result.diagnostics.front().message.c_str());auto serverSql=QString::fromStdString(result.sql);serverSql=serverSql.mid(serverSql.indexOf('\n')+1);serverSql=serverSql.left(serverSql.lastIndexOf("$$"));auto created=db->executeDefinition(serverSql,{},token);QVERIFY2(created.error.isEmpty(),qPrintable(created.error));
        QCOMPARE(run("SELECT conversion_add(5)").rows[0][0].toInt(),7);QCOMPARE(run("SELECT conversion_add(0)").rows[0][0].toInt(),0);QCOMPARE(run("SELECT conversion_add(1)").rows[0][0].toInt(),10);run("DROP FUNCTION conversion_add");
    }catch(const std::exception& e){QFAIL(e.what());}}
    void convertedLoopJumpsReachTheirLabeledTargets() {try {
        using namespace navidog;
        using namespace navidog::sql::compiler;
        auto db=connectDatabase(p_);
        auto token=cancellation();
        const auto translated=convertSql(Dialect::PostgreSQL,Dialect::MySQL,
            "CREATE FUNCTION labeled_loop_owned(p INT) RETURNS INT LANGUAGE plpgsql IMMUTABLE AS $$ "
            "BEGIN <<outside>> LOOP p:=p+1; <<inside>> LOOP EXIT outside WHEN p>=4; "
            "CONTINUE outside WHEN p<2; EXIT inside; END LOOP inside; "
            "EXIT WHEN p>=3; END LOOP outside; RETURN p; END; $$;");
        QVERIFY2(translated.ok(),translated.diagnostics.empty()?"no output":translated.diagnostics.front().message.c_str());
        auto definition=QString::fromStdString(translated.sql);
        definition=definition.mid(definition.indexOf('\n')+1);
        definition=definition.left(definition.lastIndexOf("$$"));
        const auto created=db->executeDefinition(definition,{},token);
        QVERIFY2(created.error.isEmpty(),qPrintable(created.error));
        const auto rows=db->execute("SELECT labeled_loop_owned(0),labeled_loop_owned(2),labeled_loop_owned(3)",{},token);
        QVERIFY2(rows.error.isEmpty(),qPrintable(rows.error));
        QCOMPARE(rows.rows.at(0).at(0).toInt(),3);
        QCOMPARE(rows.rows.at(0).at(1).toInt(),3);
        QCOMPARE(rows.rows.at(0).at(2).toInt(),4);
    }catch(const std::exception &e){QFAIL(e.what());}}
    void convertedLoopPreservesPostTestAndNullCondition() {try {
        using namespace navidog;
        using namespace navidog::sql::compiler;
        auto db=connectDatabase(p_);
        auto token=cancellation();
        const auto translated=convertSql(Dialect::PostgreSQL,Dialect::MySQL,
            "CREATE FUNCTION loop_owned(p INTEGER) RETURNS INTEGER LANGUAGE plpgsql IMMUTABLE AS $$ "
            "DECLARE v INT := 0; BEGIN LOOP v:=v+1; EXIT WHEN v=3 OR v>=p; END LOOP; RETURN v; END; $$;");
        QVERIFY2(translated.ok(),translated.diagnostics.empty()?"no output":translated.diagnostics.front().message.c_str());
        auto definition=QString::fromStdString(translated.sql);
        definition=definition.mid(definition.indexOf('\n')+1);
        definition=definition.left(definition.lastIndexOf("$$"));
        const auto created=db->executeDefinition(definition,{},token);
        QVERIFY2(created.error.isEmpty(),qPrintable(created.error));
        const auto rows=db->execute("SELECT loop_owned(0),loop_owned(2),loop_owned(NULL)",{},token);
        QVERIFY2(rows.error.isEmpty(),qPrintable(rows.error));
        QCOMPARE(rows.rows.at(0).at(0).toInt(),1);
        QCOMPARE(rows.rows.at(0).at(1).toInt(),2);
        QCOMPARE(rows.rows.at(0).at(2).toInt(),3);
    }catch(const std::exception &e){QFAIL(e.what());}}
    void convertedCasePreservesNullAndBranchBinding() {try {
        using namespace navidog;
        using namespace navidog::sql::compiler;
        auto db=connectDatabase(p_);
        auto token=cancellation();
        const auto source="CREATE FUNCTION case_conversion_owned(p INTEGER) RETURNS INTEGER LANGUAGE plpgsql IMMUTABLE "
                          "AS $$ DECLARE v INT := p; BEGIN CASE v WHEN 1 THEN v:=8; WHEN NULL THEN v:=9; "
                          "ELSE CASE WHEN v IS NULL THEN v:=2; ELSE v:=0; END CASE; "
                          "END CASE; RETURN v; END; $$;";
        const auto converted=convertSql(Dialect::PostgreSQL,Dialect::MySQL,source);
        QVERIFY2(converted.ok(),converted.diagnostics.empty()?"no SQL":converted.diagnostics.front().message.c_str());
        auto definition=QString::fromStdString(converted.sql);
        definition=definition.mid(definition.indexOf('\n')+1);
        definition=definition.left(definition.lastIndexOf("$$"));
        const auto created=db->executeDefinition(definition,{},token);
        QVERIFY2(created.error.isEmpty(),qPrintable(created.error));
        const auto outcome=db->execute("SELECT case_conversion_owned(1), case_conversion_owned(NULL), case_conversion_owned(3)",{},token);
        QVERIFY2(outcome.error.isEmpty(),qPrintable(outcome.error));
        QCOMPARE(outcome.rows[0][0].toInt(),8);
        QCOMPARE(outcome.rows[0][1].toInt(),2);
        QCOMPARE(outcome.rows[0][2].toInt(),0);
    }catch(const std::exception& e){QFAIL(e.what());}}
    void mysqlHandlerModesPreserveNativeControlFlow() {try {
        using namespace navidog;
        using namespace navidog::sql::compiler;
        if (p_.driver != DatabaseKind::MySQL) QSKIP("MySQL 9.5 handler fixture");
        auto db=connectDatabase(p_);
        const auto token=cancellation();
        const auto run=[&](const QString &sql, bool definition=false) {
            auto result=definition ? db->executeDefinition(sql,{},token) : db->execute(sql,{},token);
            if (!result.error.isEmpty()) throw std::runtime_error(result.error.toStdString());
            return result;
        };
        run("CREATE TABLE handler_rows_owned(id INT PRIMARY KEY)");
        run("INSERT INTO handler_rows_owned VALUES (1)");
        const QString source="CREATE PROCEDURE handler_test_owned() BEGIN "
            "DECLARE n INT DEFAULT 1; DECLARE duplicate_value CONDITION FOR 1062; "
            "DECLARE CONTINUE HANDLER FOR duplicate_value BEGIN SET n=n+10; END; "
            "INSERT INTO handler_rows_owned VALUES(1); SET n=n+1; "
            "BEGIN DECLARE EXIT HANDLER FOR SQLSTATE '23000' SET n=n+20; "
            "INSERT INTO handler_rows_owned VALUES(1); SET n=99; END; "
            "SET @handler_out_owned=n+1; SELECT n AS output_value; END;";
        run(source,true);
        const auto native=run("CALL handler_test_owned()");
        QCOMPARE(native.rows.front().front().toInt(),32);
        QCOMPARE(run("SELECT @handler_out_owned").rows.front().front().toInt(),33);
        run("DROP PROCEDURE handler_test_owned");
        const auto generated=convertSql(Dialect::MySQL,Dialect::MySQL,source.toStdString());
        QVERIFY2(generated.ok(),generated.diagnostics.empty()?"no routine":generated.diagnostics.front().message.c_str());
        auto definition=QString::fromStdString(generated.sql);
        definition=definition.mid(definition.indexOf('\n')+1);
        definition=definition.left(definition.lastIndexOf("$$"));
        run(definition,true);
        const auto converted=run("CALL handler_test_owned()");
        QCOMPARE(converted.rows,native.rows);
        QCOMPARE(converted.columns,native.columns);
        QCOMPARE(run("SELECT @handler_out_owned").rows.front().front().toInt(),33);
        run("DROP PROCEDURE handler_test_owned");
        run("DROP TABLE handler_rows_owned");
    }catch(const std::exception &e){QFAIL(e.what());}}
    void generatedPartitionClausesPreserveNativeLayout() {try {
        using namespace navidog;
        using namespace navidog::sql::compiler;
        if (p_.driver != DatabaseKind::MySQL) QSKIP("MySQL partition fixture");
        auto db=connectDatabase(p_);
        const auto token=cancellation();
        const auto run=[&](const QString &sql) {
            auto result=db->execute(sql,{},token);
            if (!result.error.isEmpty()) throw std::runtime_error(result.error.toStdString());
            return result;
        };
        for (const auto &[name,ddl,method,count] : std::vector<std::tuple<QString,QString,QString,int>>{
            {"hash_layout_owned","CREATE TABLE hash_layout_owned (id INT PRIMARY KEY, note INT) "
                "PARTITION BY HASH(id) PARTITIONS 4;","HASH",4},
            {"key_layout_owned","CREATE TABLE key_layout_owned (id INT, note INT, PRIMARY KEY(id,note)) "
                "PARTITION BY LINEAR KEY ALGORITHM=2 (id,note) PARTITIONS 3;","LINEAR KEY",3},
            {"implicit_key_owned","CREATE TABLE implicit_key_owned (id INT PRIMARY KEY, note INT) "
                "PARTITION BY KEY () PARTITIONS 2;","KEY",2}}) {
            const auto generated=convertSql(Dialect::MySQL,Dialect::MySQL,ddl.toStdString());
            QVERIFY2(generated.ok(),generated.diagnostics.empty()?"no DDL":generated.diagnostics.front().message.c_str());
            run(QString::fromStdString(generated.sql));
            const auto layout=run("SELECT PARTITION_METHOD, PARTITION_NAME FROM information_schema.PARTITIONS "
                                  "WHERE TABLE_SCHEMA=DATABASE() AND TABLE_NAME='"+name+"' ORDER BY PARTITION_ORDINAL_POSITION");
            QCOMPARE(layout.rows.size(),count);
            for (const auto &row : layout.rows) QCOMPARE(row[0].toString(),method);
            run("INSERT INTO "+name+" VALUES (-1,10),(0,20),(2,30)");
            QCOMPARE(run("SELECT SUM(note) FROM "+name).rows.front().front().toInt(),60);
            run("DROP TABLE "+name);
        }
    }catch(const std::exception &e){QFAIL(e.what());}}
    void convertedCtesPreserveOutputAndDependencyOrder() {try {
        using namespace navidog;
        using namespace navidog::sql::compiler;
        auto db=connectDatabase(p_);
        auto token=cancellation();
        const auto result=convertSql(Dialect::PostgreSQL,Dialect::MySQL,
            "WITH q(a) AS (SELECT 1 AS previous, 2 AS retained) SELECT a,retained FROM q;");
        QVERIFY2(result.ok(),result.diagnostics.empty()?"no SQL":result.diagnostics.front().message.c_str());
        const auto rows=db->execute(QString::fromStdString(result.sql),{},token);
        QVERIFY2(rows.error.isEmpty(),qPrintable(rows.error));
        QCOMPARE(rows.rows.size(),1);
        QCOMPARE(rows.rows.at(0).at(0).toInt(),1);
        QCOMPARE(rows.rows.at(0).at(1).toInt(),2);
        const auto forward=convertSql(Dialect::PostgreSQL,Dialect::MySQL,
            "WITH RECURSIVE a AS (SELECT n FROM b), b AS (SELECT 7 AS n) SELECT n FROM a;");
        QVERIFY2(forward.ok(),forward.diagnostics.empty()?"no SQL":forward.diagnostics.front().message.c_str());
        const auto later=db->execute(QString::fromStdString(forward.sql),{},token);
        QVERIFY2(later.error.isEmpty(),qPrintable(later.error));
        QCOMPARE(later.rows.at(0).at(0).toInt(),7);
    }catch(const std::exception& e){QFAIL(e.what());}}
    void viewOptionsSurviveNativeGeneration() {try {
        using namespace navidog;
        using namespace navidog::sql::compiler;
        const auto dialect=p_.driver==DatabaseKind::MariaDB?Dialect::MariaDB:Dialect::MySQL;
        auto db=connectDatabase(p_);
        const auto token=cancellation();
        const auto run=[&](const QString &sql) {
            const auto result=db->execute(sql,{},token);
            if (!result.error.isEmpty()) throw std::runtime_error(result.error.toStdString());
            return result;
        };
        run("CREATE TABLE owned_view_rows(id int PRIMARY KEY)");
        run("INSERT INTO owned_view_rows VALUES (10),(20),(30)");
        const std::string sql="CREATE OR REPLACE ALGORITHM=MERGE DEFINER=CURRENT_USER() "
            "SQL SECURITY INVOKER VIEW owned_view_options (value_id) "
            "AS SELECT id FROM owned_view_rows WHERE id>10";
        const auto canonical=convertSql(dialect,dialect,sql);
        QVERIFY2(canonical.ok(),canonical.diagnostics.empty()?"no SQL":canonical.diagnostics.front().message.c_str());
        const auto formatted=formatSql(dialect,canonical.sql);
        QVERIFY(formatted.ok());
        const auto expected=run("SELECT id AS value_id FROM owned_view_rows WHERE id>10 ORDER BY id");
        for (const auto &definition : {canonical.sql,formatted.sql}) {
            const auto created=db->executeDefinition(QString::fromStdString(definition),{},token);
            QVERIFY2(created.error.isEmpty(),qPrintable(created.error));
            const auto saved=run("SHOW CREATE VIEW owned_view_options").rows.front().at(1).toString().toStdString();
            const auto parsed=compile(dialect,saved);
            QVERIFY2(parsed->ok(),parsed->diagnostics.empty()?"no AST":parsed->diagnostics.front().message.c_str());
            const auto &header=*parsed->model.front().ddl->mysqlViewOptions;
            QCOMPARE(header.algorithm,MySqlViewAlgorithm::Merge);
            QCOMPARE(header.security,MySqlViewSecurity::Invoker);
            QVERIFY(header.definer && !header.definer->user.empty() && !header.definer->host.empty());
            QCOMPARE(run("SELECT value_id FROM owned_view_options ORDER BY value_id").rows,expected.rows);
        }
        for (const auto &source : {
                "CREATE VIEW owned_view_bad AS SELECT @value AS n",
                "CREATE VIEW owned_view_bad AS SELECT @@version AS n",
                "CREATE VIEW owned_view_bad AS SELECT x FROM (SELECT @value AS x) d",
                "CREATE VIEW owned_view_bad AS SELECT 1 AS n, 2 AS N",
                "CREATE VIEW owned_view_bad AS SELECT ? AS n",
                "CREATE VIEW owned_view_bad (a,b) AS SELECT 1"}) {
            const auto local=compile(dialect,source);
            QVERIFY2(local->syntaxValid && !local->ok(),source);
            QString rejection;
            try {
                rejection=db->executeDefinition(QString::fromUtf8(source),{},token).error;
            } catch (const std::exception &error) {
                rejection=QString::fromUtf8(error.what());
            }
            QVERIFY2(!rejection.isEmpty(),source);
            QCOMPARE(run("SELECT 1").rows.front().front().toInt(),1);
        }
        run("DROP VIEW owned_view_options");
        run("DROP TABLE owned_view_rows");
    }catch(const std::exception &e){QFAIL(e.what());}}
    void temporaryViewAndGeneratedKeyChecksMatchServer() {try {
        using namespace navidog;
        using namespace navidog::sql::compiler;
        const auto dialect=p_.driver==DatabaseKind::MariaDB?Dialect::MariaDB:Dialect::MySQL;
        auto db=connectDatabase(p_);
        const auto token=cancellation();
        const auto run=[&](const std::string &sql) {
            return db->execute(QString::fromStdString(sql),{},token);
        };
        const auto rejection=[&](const std::string &sql) {
            try { return run(sql).error; }
            catch (const std::exception &error) { return QString::fromUtf8(error.what()); }
        };
        const std::string temporary="CREATE TEMPORARY TABLE owned_view_temp(id INT)";
        MemoryCatalog empty;
        ScriptContext declared(dialect,&empty);
        const auto observe=[&](const std::string &sql) {
            const auto model=compile(dialect,sql);
            if (!model->ok() || model->model.roots.size()!=1)
                throw std::runtime_error("could not parse test relation: "+sql);
            if (const auto error=declared.observe(model->model,model->model.roots.front()))
                throw std::runtime_error(error->message);
        };
        observe(temporary);
        QVERIFY(run(temporary).error.isEmpty());
        for (const std::string &view:{"CREATE VIEW owned_invalid_view AS SELECT id FROM owned_view_temp",
                                      "CREATE VIEW owned_invalid_view AS SELECT id FROM (SELECT id FROM owned_view_temp) d"}) {
            const auto parsed=compile(dialect,view,{},&declared);
            QVERIFY(parsed->syntaxValid && !parsed->ok());
            const auto marker=std::find_if(parsed->diagnostics.begin(),parsed->diagnostics.end(),[](const auto &entry) {
                return entry.code==DiagnosticCode::InvalidQueryShape && entry.message.find("temporary tables")!=std::string::npos;
            });
            QVERIFY(marker!=parsed->diagnostics.end());
            QCOMPARE(view.substr(marker->range.begin,marker->range.end-marker->range.begin),std::string("owned_view_temp"));
            QVERIFY2(!rejection(view).isEmpty(),view.c_str());
        }
        const std::string carrier="CREATE TABLE owned_view_carrier LIKE owned_view_temp";
        observe(carrier);
        QVERIFY(run(carrier).error.isEmpty());
        const std::string validView="CREATE VIEW owned_carrier_view AS SELECT id FROM owned_view_carrier";
        QVERIFY(compile(dialect,validView,{},&declared)->ok());
        QVERIFY(run(validView).error.isEmpty());
        for (const std::string &create:{"CREATE TABLE owned_auto_key (id INT AUTO_INCREMENT, payload INT)",
                                        "CREATE TABLE owned_auto_key (id INT AUTO_INCREMENT PRIMARY KEY, seq INT AUTO_INCREMENT UNIQUE)",
                                        "CREATE TABLE owned_auto_key (id INT AUTO_INCREMENT PRIMARY KEY DEFAULT 5)"}) {
            const auto parsed=compile(dialect,create);
            QVERIFY2(parsed->syntaxValid && !parsed->ok(),create.c_str());
            QVERIFY2(!rejection(create).isEmpty(),create.c_str());
        }
        const std::string withIndex="CREATE TABLE owned_auto_key (id INT AUTO_INCREMENT, value_col INT, KEY indexed_id (id))";
        QVERIFY(compile(dialect,withIndex)->ok());
        QVERIFY(run(withIndex).error.isEmpty());
        for (const auto &drop:{"DROP TABLE owned_auto_key","DROP VIEW owned_carrier_view",
                              "DROP TABLE owned_view_carrier","DROP TEMPORARY TABLE owned_view_temp"})
            QVERIFY(run(drop).error.isEmpty());
    }catch(const std::exception &e){QFAIL(e.what());}}
    void loadableFunctionHeadersReachNativeLibraryLookup() {try {
        using namespace navidog;
        using namespace navidog::sql::compiler;
        const auto dialect=p_.driver==DatabaseKind::MariaDB?Dialect::MariaDB:Dialect::MySQL;
        auto db=connectDatabase(p_);
        const auto token=cancellation();
        const auto library="navidog_missing_"+QUuid::createUuid().toString(QUuid::Id128).toStdString()+".so";
        std::vector<std::string> definitions={
            "CREATE FUNCTION owned_loader RETURNS INT SONAME '"+library+"'",
            "CREATE AGGREGATE FUNCTION IF NOT EXISTS owned_loader RETURNS DECIMAL SONAME '"+library+"'"};
        if (dialect==Dialect::MariaDB)
            definitions.push_back("CREATE OR REPLACE FUNCTION owned_loader RETURNS REAL SONAME '"+library+"'");
        for (const auto &sql:definitions) {
            const auto model=compile(dialect,sql);
            QVERIFY(model->ok() && model->model.front().ddl->mysqlLoadableFunction);
            const auto canonical=convertSql(dialect,dialect,sql);
            QVERIFY(canonical.ok());
            for (const auto &candidate:{sql,canonical.sql}) {
                QString rejection;
                try { rejection=db->execute(QString::fromStdString(candidate),{},token).error; }
                catch (const std::exception &error) { rejection=QString::fromUtf8(error.what()); }
                QVERIFY2(rejection.contains(QString::fromStdString(library)) &&
                         rejection.contains("shared library",Qt::CaseInsensitive),qPrintable(rejection));
            }
        }
    }catch(const std::exception &e){QFAIL(e.what());}}
    void routineHeaderCharacteristicsRoundTrip() {try {
        using namespace navidog;
        using namespace navidog::sql::compiler;
        const auto dialect=p_.driver==DatabaseKind::MariaDB?Dialect::MariaDB:Dialect::MySQL;
        auto db=connectDatabase(p_);
        const auto token=cancellation();
        const auto run=[&](const QString &sql) {
            const auto result=db->execute(sql,{},token);
            if (!result.error.isEmpty()) throw std::runtime_error(result.error.toStdString());
            return result;
        };
        for (const auto rawBackslash : {false,true}) {
            run(rawBackslash?"SET SESSION sql_mode = 'NO_BACKSLASH_ESCAPES'":"SET SESSION sql_mode = ''");
            const auto literal=rawBackslash?R"('it''s \ a comment')":R"('it''s \\ a comment')";
            const auto source="CREATE PROCEDURE owned_header_options() LANGUAGE SQL COMMENT " +
                std::string(literal) + " SELECT 42 AS result;";
            ConversionOptions options;
            options.language.mysqlNoBackslashEscapes=rawBackslash;
            const auto canonical=convertSql(dialect,dialect,source,options);
            QVERIFY2(canonical.ok(),canonical.diagnostics.empty()?"no SQL":canonical.diagnostics.front().message.c_str());
            const auto parts=sql::splitStatements(canonical.sql,sql::Dialect::MySQL);
            QCOMPARE(parts.size(),std::size_t(1));
            const auto definition=sql::statementSource(canonical.sql,parts.front(),sql::Dialect::MySQL);
            const auto result=db->executeDefinition(QString::fromUtf8(definition.data(),int(definition.size())),{},token);
            QVERIFY2(result.error.isEmpty(),qPrintable(result.error));
            const auto saved=run("SELECT ROUTINE_COMMENT FROM information_schema.ROUTINES "
                                 "WHERE ROUTINE_SCHEMA=DATABASE() AND ROUTINE_NAME='owned_header_options'");
            QCOMPARE(saved.rows.size(),1);
            QCOMPARE(saved.rows.front().front().toString(),QString::fromUtf8(R"(it's \ a comment)"));
            QCOMPARE(run("CALL owned_header_options()").rows.front().front().toInt(),42);
            run("DROP PROCEDURE owned_header_options");
        }
    }catch(const std::exception &e){QFAIL(e.what());}}
    void guardedAlterKeepsExistingColumnsInOwnedMariaDB() {try {
        using namespace navidog;
        using namespace navidog::sql::compiler;
        if (p_.driver!=DatabaseKind::MariaDB) QSKIP("MariaDB-specific column guards");
        auto db=connectDatabase(p_);
        const auto token=cancellation();
        const auto run=[&](const std::string &sql) {
            const auto result=db->execute(QString::fromStdString(sql),{},token);
            if (!result.error.isEmpty()) throw std::runtime_error(result.error.toStdString());
            return result;
        };
        const auto alter=[&](const std::string &sql) {
            const auto result=convertSql(Dialect::MariaDB,Dialect::MariaDB,sql);
            if (!result.ok()) throw std::runtime_error(result.diagnostics.front().message);
            run(result.sql);
        };
        run("CREATE TEMPORARY TABLE owned_guarded_alter (id INT, remove_me TEXT)");
        run("INSERT INTO owned_guarded_alter VALUES (7,'before')");
        alter("ALTER TABLE IF EXISTS owned_missing_alter ADD COLUMN id INTEGER");
        alter("ALTER TABLE IF EXISTS owned_guarded_alter ADD COLUMN IF NOT EXISTS id TEXT, "
              "ADD COLUMN IF NOT EXISTS status VARCHAR(10) DEFAULT 'new', "
              "DROP COLUMN IF EXISTS absent, DROP COLUMN IF EXISTS remove_me");
        const auto columns=run("SHOW COLUMNS FROM owned_guarded_alter");
        QCOMPARE(columns.rows.size(),2);
        QVERIFY(columns.rows[0][1].toString().startsWith("int"));
        const auto result=run("SELECT id,status FROM owned_guarded_alter");
        QCOMPARE(result.rows.size(),1);
        QCOMPARE(result.rows[0][0].toInt(),7);
        QCOMPARE(result.rows[0][1].toString(),QString("new"));
    }catch(const std::exception &e){QFAIL(e.what());}}
    void guardedRoutineCreationPreservesExistingDefinition() {try {
        using namespace navidog;
        using namespace navidog::sql::compiler;
        const auto dialect=p_.driver==DatabaseKind::MariaDB?Dialect::MariaDB:Dialect::MySQL;
        auto db=connectDatabase(p_);
        const auto token=cancellation();
        const auto execute=[&](const QString &statement) {
            const auto result=db->execute(statement,{},token);
            if (!result.error.isEmpty()) throw std::runtime_error(result.error.toStdString());
            return result;
        };
        const auto create=[&](const std::string &source) {
            const auto emitted=convertSql(dialect,dialect,source);
            if (!emitted.ok()) throw std::runtime_error(emitted.diagnostics.front().message);
            const auto parts=sql::splitStatements(emitted.sql,sql::Dialect::MySQL);
            if (parts.size()!=1) throw std::runtime_error("invalid emitted routine definition");
            const auto text=sql::statementSource(emitted.sql,parts.front(),sql::Dialect::MySQL);
            const auto result=db->executeDefinition(QString::fromUtf8(text.data(),int(text.size())),{},token);
            if (!result.error.isEmpty()) throw std::runtime_error(result.error.toStdString());
        };
        create("CREATE PROCEDURE IF NOT EXISTS owned_guarded_proc(IN n INT) SELECT n+1;");
        create("CREATE PROCEDURE IF NOT EXISTS owned_guarded_proc() SELECT 100;");
        QCOMPARE(execute("CALL owned_guarded_proc(9)").rows.front().front().toInt(),10);
        create("CREATE FUNCTION IF NOT EXISTS owned_guarded_f(n INT) RETURNS INT DETERMINISTIC RETURN n+2;");
        create("CREATE FUNCTION IF NOT EXISTS owned_guarded_f() RETURNS INT DETERMINISTIC RETURN 100;");
        QCOMPARE(execute("SELECT owned_guarded_f(8)").rows.front().front().toInt(),10);
        execute("DROP PROCEDURE owned_guarded_proc");
        execute("DROP FUNCTION owned_guarded_f");
    }catch(const std::exception &e){QFAIL(e.what());}}
    void routineSignalPreservesConditionAndResignal() {try {
        using namespace navidog;
        using namespace navidog::sql::compiler;
        const auto dialect=p_.driver==DatabaseKind::MariaDB?Dialect::MariaDB:Dialect::MySQL;
        auto db=connectDatabase(p_);
        const auto token=cancellation();
        const auto execute=[&](const QString &statement) { return db->execute(statement,{},token); };
        const auto create=[&](const std::string &input) {
            const auto routine=compileRoutine(dialect,input);
            QVERIFY2(routine.ok(),routine.diagnostics.empty()?"no routine":routine.diagnostics.front().message.c_str());
            const auto result=convertSql(dialect,dialect,input);
            QVERIFY2(result.ok(),result.diagnostics.empty()?"no SQL":result.diagnostics.front().message.c_str());
            const auto parts=sql::splitStatements(result.sql,sql::Dialect::MySQL);
            QCOMPARE(parts.size(),std::size_t(1));
            const auto definition=sql::statementSource(result.sql,parts.front(),sql::Dialect::MySQL);
            QVERIFY(compileRoutine(dialect,std::string(definition)).ok());
            const auto created=db->executeDefinition(QString::fromUtf8(definition.data(),int(definition.size())),{},token);
            QVERIFY2(created.error.isEmpty(),qPrintable(created.error));
        };
        const auto checkSignal=[&](const QString &sql,const QString &expected,int code) {
            QString message;
            try { message=execute(sql).error; }
            catch(const std::exception &driverError) { message=QString::fromUtf8(driverError.what()); }
            QVERIFY2(message.contains(expected),qPrintable(message));
            const auto diagnostics=execute("SHOW ERRORS");
            QVERIFY(std::any_of(diagnostics.rows.begin(),diagnostics.rows.end(),[&](const auto &row) {
                return row.size()>=3 && row[1].toInt()==code && row[2].toString().contains(expected);
            }));
        };
        create("CREATE PROCEDURE owned_signal(IN p_msg VARCHAR(50)) BEGIN "
               "DECLARE bad_value CONDITION FOR SQLSTATE VALUE '45001'; "
               "DECLARE local_msg VARCHAR(50) DEFAULT p_msg; "
               "SIGNAL bad_value SET MESSAGE_TEXT=local_msg, MYSQL_ERRNO=31081; END;");
        checkSignal("CALL owned_signal('owned signal message')","owned signal message",31081);
        create("CREATE PROCEDURE owned_resignal(IN p_msg VARCHAR(50)) BEGIN "
               "DECLARE EXIT HANDLER FOR SQLSTATE '45001' "
               "BEGIN RESIGNAL SET MESSAGE_TEXT=p_msg, MYSQL_ERRNO=31082; END; "
               "SIGNAL SQLSTATE '45001' SET MESSAGE_TEXT='original message'; END;");
        checkSignal("CALL owned_resignal('owned resignal message')","owned resignal message",31082);
        create("CREATE PROCEDURE owned_resignal_unchanged() BEGIN "
               "DECLARE EXIT HANDLER FOR SQLEXCEPTION RESIGNAL; "
               "SIGNAL SQLSTATE '45000' SET MESSAGE_TEXT='owned unchanged message'; END;");
        checkSignal("CALL owned_resignal_unchanged()","owned unchanged message",1644);
        QVERIFY(execute("DROP PROCEDURE owned_signal").error.isEmpty());
        QVERIFY(execute("DROP PROCEDURE owned_resignal").error.isEmpty());
        QVERIFY(execute("DROP PROCEDURE owned_resignal_unchanged").error.isEmpty());
    }catch(const std::exception &e){QFAIL(e.what());}}
    void routineReadsNativeDiagnostics() {try {
        using namespace navidog;
        using namespace navidog::sql::compiler;
        const bool maria=p_.driver==DatabaseKind::MariaDB;
        const auto dialect=maria?Dialect::MariaDB:Dialect::MySQL;
        auto db=connectDatabase(p_);
        const auto token=cancellation();
        const auto execute=[&](const QString &statement) {
            const auto result=db->execute(statement,{},token);
            if (!result.error.isEmpty()) throw std::runtime_error(result.error.toStdString());
            return result;
        };
        execute("CREATE TABLE owned_diag_data (id INT PRIMARY KEY, v INT)");
        execute("INSERT INTO owned_diag_data VALUES (1,0),(2,0),(3,0)");
        const std::string area=maria?"CURRENT":"STACKED";
        const std::string sql="CREATE PROCEDURE owned_read_diag(IN p_condition INT) BEGIN "
            "DECLARE saved_state CHAR(5); DECLARE saved_text VARCHAR(100); DECLARE changed INT; "
            "DECLARE CONTINUE HANDLER FOR SQLSTATE '45001' BEGIN GET "+area+" DIAGNOSTICS "
            "CONDITION p_condition saved_state=RETURNED_SQLSTATE,saved_text=MESSAGE_TEXT,@owned_errno=MYSQL_ERRNO; END; "
            "SIGNAL SQLSTATE '45001' SET MYSQL_ERRNO=32001,MESSAGE_TEXT='owned diagnostic'; "
            "UPDATE owned_diag_data SET v=v+1 WHERE id<=2; GET DIAGNOSTICS changed=ROW_COUNT; "
            "SELECT saved_state,saved_text,@owned_errno,changed; END;";
        const auto compiled=compileRoutine(dialect,sql);
        QVERIFY2(compiled.ok(),compiled.diagnostics.empty()?"no routine":compiled.diagnostics.front().message.c_str());
        const auto generated=convertSql(dialect,dialect,sql);
        QVERIFY2(generated.ok(),generated.diagnostics.empty()?"no SQL":generated.diagnostics.front().message.c_str());
        const auto parts=sql::splitStatements(generated.sql,sql::Dialect::MySQL);
        QCOMPARE(parts.size(),std::size_t(1));
        const auto definition=sql::statementSource(generated.sql,parts.front(),sql::Dialect::MySQL);
        const auto created=db->executeDefinition(QString::fromUtf8(definition.data(),int(definition.size())),{},token);
        QVERIFY2(created.error.isEmpty(),qPrintable(created.error));
        const auto result=execute("CALL owned_read_diag(1)");
        QCOMPARE(result.rows.size(),1);
        const auto &row=result.rows.front();
        QCOMPARE(row.size(),4);
        QCOMPARE(row[0].toString(),QString("45001"));
        QCOMPARE(row[1].toString(),QString("owned diagnostic"));
        QCOMPARE(row[2].toInt(),32001);
        QCOMPARE(row[3].toInt(),2);
        execute("DROP PROCEDURE owned_read_diag");
        execute("DROP TABLE owned_diag_data");
    }catch(const std::exception &e){QFAIL(e.what());}}
    void procedurePreparedStatementsKeepNativeSessionBinding() {try {
        using namespace navidog;
        using namespace navidog::sql::compiler;
        const auto dialect=p_.driver==DatabaseKind::MariaDB?Dialect::MariaDB:Dialect::MySQL;
        auto db=connectDatabase(p_);
        const auto token=cancellation();
        const auto execute=[&](const QString &statement) {
            const auto result=db->execute(statement,{},token);
            if (!result.error.isEmpty()) throw std::runtime_error(result.error.toStdString());
            return result;
        };
        const auto create=[&](const std::string &sql) {
            const auto generated=convertSql(dialect,dialect,sql);
            QVERIFY2(generated.ok(),generated.diagnostics.empty()?"no SQL":generated.diagnostics.front().message.c_str());
            const auto parts=sql::splitStatements(generated.sql,sql::Dialect::MySQL);
            QCOMPARE(parts.size(),std::size_t(1));
            const auto definition=sql::statementSource(generated.sql,parts.front(),sql::Dialect::MySQL);
            const auto created=db->executeDefinition(QString::fromUtf8(definition.data(),int(definition.size())),{},token);
            QVERIFY2(created.error.isEmpty(),qPrintable(created.error));
        };
        create("CREATE PROCEDURE owned_prepared(IN source_num INT,OUT result_num INT) BEGIN "
               "SET @owned_input=source_num; SET @owned_sql='SELECT ?+2 INTO @owned_output'; "
               "PREPARE owned_handle FROM @owned_sql; EXECUTE owned_handle USING @owned_input; "
               "DROP PREPARE owned_handle; SET result_num=@owned_output; END;");
        for (int input : {2,7}) {
            execute(QString("CALL owned_prepared(%1,@owned_result)").arg(input));
            QCOMPARE(execute("SELECT @owned_result").rows.front().front().toInt(),input+2);
        }
        QString deallocated;
        try { deallocated=db->execute("EXECUTE owned_handle USING @owned_input",{},token).error; }
        catch (const std::exception &failure) { deallocated=QString::fromUtf8(failure.what()); }
        QVERIFY2(deallocated.contains("Unknown prepared statement"),qPrintable(deallocated));
        QString badFunction;
        try {
            badFunction=db->executeDefinition("CREATE FUNCTION owned_bad_commit() RETURNS INT MODIFIES SQL DATA "
                            "BEGIN COMMIT; RETURN 1; END",{},token).error;
        } catch (const std::exception &failure) { badFunction=QString::fromUtf8(failure.what()); }
        QVERIFY2(badFunction.contains("not allowed",Qt::CaseInsensitive),qPrintable(badFunction));
        execute("DROP PROCEDURE owned_prepared");
    }catch(const std::exception &e){QFAIL(e.what());}}
    void inlineIndexesSurviveNativeTableCreation() {try {
        using namespace navidog;
        using namespace navidog::sql::compiler;
        const auto dialect=p_.driver==DatabaseKind::MariaDB?Dialect::MariaDB:Dialect::MySQL;
        auto db=connectDatabase(p_);
        const auto token=cancellation();
        const auto execute=[&](const QString &statement) {
            const auto result=db->execute(statement,{},token);
            if (!result.error.isEmpty()) throw std::runtime_error(result.error.toStdString());
            return result;
        };
        const auto create=[&](const std::string &statement) {
            const auto emitted=convertSql(dialect,dialect,statement);
            QVERIFY2(emitted.ok(),emitted.diagnostics.empty()?"no SQL":emitted.diagnostics.front().message.c_str());
            const auto result=db->executeDefinition(QString::fromStdString(emitted.sql),{},token);
            QVERIFY2(result.error.isEmpty(),qPrintable(result.error));
        };
        create("CREATE TABLE owned_inline_keys (id INT PRIMARY KEY, nick VARCHAR(30), "
               "INDEX by_prefix (nick(4),id DESC) USING BTREE, KEY (id), UNIQUE INDEX unique_name (nick));");
        const auto indexes=execute("SHOW INDEX FROM owned_inline_keys");
        const auto key=[&](const QString &name,int sequence)->const QVector<QVariant> * {
            const auto entry=std::find_if(indexes.rows.begin(),indexes.rows.end(),[&](const auto &row) {
                return row.size()>=8 && row[2].toString()==name && row[3].toInt()==sequence;
            });
            return entry==indexes.rows.end()?nullptr:&*entry;
        };
        const auto *prefix=key("by_prefix",1);
        const auto *descending=key("by_prefix",2);
        const auto *unique=key("unique_name",1);
        QVERIFY(prefix && descending && unique && key("id",1));
        QCOMPARE((*prefix)[4].toString(),QString("nick"));
        QCOMPARE((*prefix)[7].toInt(),4);
        QCOMPARE((*descending)[5].toString(),QString("D"));
        QCOMPARE((*unique)[1].toInt(),0);
        const auto native=execute("SHOW CREATE TABLE owned_inline_keys");
        QVERIFY(compile(dialect,native.rows.front()[1].toString().toStdString())->ok());
        create("CREATE TABLE owned_index_ctas (KEY by_projection(projected)) AS SELECT 10 AS projected;");
        QCOMPARE(execute("SELECT projected FROM owned_index_ctas").rows.front().front().toInt(),10);
        QCOMPARE(execute("SHOW INDEX FROM owned_index_ctas").rows.front()[2].toString(),QString("by_projection"));
        execute("DROP TABLE owned_index_ctas");
        execute("DROP TABLE owned_inline_keys");
    }catch(const std::exception &e){QFAIL(e.what());}}
    void routineBlockLabelLeavesCorrectScope() {try {
        using namespace navidog;
        using namespace navidog::sql::compiler;
        const auto dialect=p_.driver==DatabaseKind::MariaDB?Dialect::MariaDB:Dialect::MySQL;
        auto db=connectDatabase(p_);
        const auto token=cancellation();
        const auto execute=[&](const QString &statement) {
            const auto result=db->execute(statement,{},token);
            if (!result.error.isEmpty()) throw std::runtime_error(result.error.toStdString());
            return result;
        };
        const std::string sql="CREATE PROCEDURE owned_labels(IN p_mode INT, OUT p_value INT) "
            "root_block: BEGIN SET p_value=1; "
            "IF p_mode=0 THEN LEAVE root_block; END IF; "
            "outer_loop: LOOP "
            "nested_block: BEGIN "
            "SET p_value=p_value+1; "
            "IF p_value=2 THEN ITERATE outer_loop; END IF; "
            "IF p_mode=1 THEN LEAVE nested_block; END IF; "
            "LEAVE outer_loop; "
            "END nested_block; "
            "SET p_value=p_value+10; LEAVE outer_loop; END LOOP outer_loop; "
            "SET p_value=p_value+100; END root_block;";
        const auto compiled=compileRoutine(dialect,sql);
        QVERIFY2(compiled.ok(),compiled.diagnostics.empty()?"no routine":compiled.diagnostics.front().message.c_str());
        QVERIFY(std::count_if(compiled.ast.nodes.begin(),compiled.ast.nodes.end(),
                [](const auto &node) { return node.blockTarget!=0; })==2);
        const auto output=convertSql(dialect,dialect,sql);
        QVERIFY2(output.ok(),output.diagnostics.empty()?"no SQL":output.diagnostics.front().message.c_str());
        const auto statements=sql::splitStatements(output.sql,sql::Dialect::MySQL);
        QCOMPARE(statements.size(),std::size_t(1));
        const auto definition=sql::statementSource(output.sql,statements.front(),sql::Dialect::MySQL);
        const auto created=db->executeDefinition(QString::fromUtf8(definition.data(),int(definition.size())),{},token);
        QVERIFY2(created.error.isEmpty(),qPrintable(created.error));
        for (const auto &[mode,value]:std::vector<std::pair<int,int>>{{0,1},{1,113},{2,103}}) {
            execute(QString("CALL owned_labels(%1,@owned_label_result)").arg(mode));
            QCOMPARE(execute("SELECT @owned_label_result").rows.front().front().toInt(),value);
        }
        execute("DROP PROCEDURE owned_labels");
    }catch(const std::exception &e){QFAIL(e.what());}}
    void routineDefinerSurvivesNativeGeneration() {try {
        using namespace navidog;
        using namespace navidog::sql::compiler;
        auto database=connectDatabase(p_);
        const auto token=cancellation();
        const auto dialect=p_.driver==DatabaseKind::MariaDB?Dialect::MariaDB:Dialect::MySQL;
        const std::string input="CREATE DEFINER=`root`@`localhost` FUNCTION owned_definer(p_value INT) "
            "RETURNS INT DETERMINISTIC BEGIN RETURN p_value + 1; END";
        const auto routine=compileRoutine(dialect,input);
        QVERIFY2(routine.ok(),routine.diagnostics.empty()?"no routine":routine.diagnostics.front().message.c_str());
        QVERIFY(routine.ast.definer && !routine.ast.definer->currentUser);
        QCOMPARE(routine.ast.definer->user,std::string("root"));
        QCOMPARE(routine.ast.definer->host,std::string("localhost"));
        const auto generated=convertSql(dialect,dialect,input+"; SELECT owned_definer(41);");
        QVERIFY2(generated.ok(),generated.diagnostics.empty()?"no SQL":generated.diagnostics.front().message.c_str());
        QVERIFY(generated.sql.find("DEFINER = `root`@`localhost`")!=std::string::npos);
        const auto statements=sql::splitStatements(generated.sql,sql::Dialect::MySQL);
        QCOMPARE(statements.size(),2u);
        QVERIFY(statements.front().compound);
        QVERIFY(!statements.back().compound);
        const auto definition=sql::statementSource(generated.sql,statements.front(),sql::Dialect::MySQL);
        const auto query=sql::statementSource(generated.sql,statements.back(),sql::Dialect::MySQL);
        const auto created=database->executeDefinition(QString::fromUtf8(definition.data(),int(definition.size())),{},token);
        QVERIFY2(created.error.isEmpty(),qPrintable(created.error));
        const auto selected=database->execute(QString::fromUtf8(query.data(),int(query.size())),{},token);
        QVERIFY2(selected.error.isEmpty(),qPrintable(selected.error));
        QCOMPARE(selected.rows.size(),1);
        QCOMPARE(selected.rows.front().front().toInt(),42);
        const auto current=compileRoutine(dialect,"CREATE DEFINER=CURRENT_USER() PROCEDURE owned_current() BEGIN SELECT 1; END");
        QVERIFY(current.ok() && current.ast.definer && current.ast.definer->currentUser);
        const auto removed=database->execute("DROP FUNCTION owned_definer",{},token);
        QVERIFY2(removed.error.isEmpty(),qPrintable(removed.error));
    }catch(const std::exception &e){QFAIL(e.what());}}
    void routineSelectIntoPreservesMySqlCardinality() {try {
        using namespace navidog;
        using namespace navidog::sql::compiler;
        auto db=connectDatabase(p_);
        auto token=cancellation();
        const auto run=[&](const QString &sql) {
            const auto result=db->execute(sql,{},token);
            if (!result.error.isEmpty()) throw std::runtime_error(result.error.toStdString());
            return result;
        };
        const auto create=[&](const ConversionResult &r) {
            if (!r.ok()) throw std::runtime_error(r.diagnostics.front().message);
            auto definition=QString::fromStdString(r.sql);
            definition=definition.mid(definition.indexOf('\n')+1);
            definition=definition.left(definition.lastIndexOf("$$"));
            const auto result=db->executeDefinition(definition,{},token);
            if (!result.error.isEmpty()) throw std::runtime_error(result.error.toStdString());
        };
        run("CREATE TABLE into_rows_my_owned(n INT)");
        run("INSERT INTO into_rows_my_owned VALUES (3),(7)");
        create(convertSql(Dialect::MySQL,Dialect::MySQL,
            "CREATE FUNCTION read_into_my_owned(p INT) RETURNS INT DETERMINISTIC "
            "BEGIN DECLARE v INT DEFAULT 99; SELECT n FROM into_rows_my_owned WHERE n<p INTO v; RETURN v; END;"));
        QCOMPARE(run("SELECT read_into_my_owned(2)").rows.at(0).at(0).toInt(),99);
        QCOMPARE(run("SELECT read_into_my_owned(5)").rows.at(0).at(0).toInt(),3);
        bool multipleRejected = false;
        try { multipleRejected = !db->execute("SELECT read_into_my_owned(10)",{},token).error.isEmpty(); }
        catch (const std::exception &) { multipleRejected = true; }
        QVERIFY(multipleRejected);
        create(convertSql(Dialect::PostgreSQL,Dialect::MySQL,
            "CREATE FUNCTION converted_into_my_owned(p INTEGER) RETURNS INTEGER LANGUAGE plpgsql IMMUTABLE "
            "AS $$ DECLARE v INTEGER; BEGIN SELECT p INTO STRICT v; RETURN v; END; $$;"));
        QCOMPARE(run("SELECT converted_into_my_owned(8)").rows.at(0).at(0).toInt(),8);
        QVERIFY(run("SELECT converted_into_my_owned(NULL)").rows.at(0).at(0).isNull());
        run("DROP FUNCTION read_into_my_owned");
        run("DROP FUNCTION converted_into_my_owned");
        run("DROP TABLE into_rows_my_owned");
    }catch(const std::exception& e){QFAIL(e.what());}}
    void truthTestsBindToMySqlSemantics() {try {
        using namespace navidog;
        using namespace navidog::sql::compiler;
        auto db=connectDatabase(p_);
        auto token=cancellation();
        const auto tested=compile(Dialect::MySQL,
            "SELECT 2 IS TRUE, 0 IS FALSE, 0 IS NOT UNKNOWN, NULL IS UNKNOWN, NULL IS NOT TRUE;");
        QVERIFY(tested->ok());
        const auto sameDialect=generateSql(tested->model,Dialect::MySQL);
        QVERIFY(sameDialect.ok());
        const auto native=db->execute(QString::fromStdString(sameDialect.sql),{},token);
        QVERIFY2(native.error.isEmpty(),qPrintable(native.error));
        for (const auto &cell : native.rows.at(0)) QCOMPARE(cell.toInt(),1);
        const auto converted=convertSql(Dialect::PostgreSQL,Dialect::MySQL,
            "SELECT (1=1) IS NOT FALSE, NULL IS UNKNOWN, 2 BETWEEN SYMMETRIC 3 AND 1;");
        QVERIFY(converted.ok());
        const auto rows=db->execute(QString::fromStdString(converted.sql),{},token);
        QVERIFY2(rows.error.isEmpty(),qPrintable(rows.error));
        for (const auto &cell : rows.rows.at(0)) QCOMPARE(cell.toInt(),1);
    }catch(const std::exception& e){QFAIL(e.what());}}
    void generatedTransactionCommandsPreserveSavepoints() {try {
        using namespace navidog;
        using namespace navidog::sql::compiler;
        auto db=connectDatabase(p_);
        auto token=cancellation();
        const auto dialect=p_.driver==DatabaseKind::MariaDB?Dialect::MariaDB:Dialect::MySQL;
        for (const auto sql : {"CREATE TEMPORARY TABLE transaction_codegen_owned(id INT) ENGINE=InnoDB;",
                               "START TRANSACTION READ WRITE;",
                               "INSERT INTO transaction_codegen_owned VALUES(1);",
                               "SAVEPOINT p1;",
                               "INSERT INTO transaction_codegen_owned VALUES(2);",
                               "ROLLBACK TO p1;",
                               "RELEASE SAVEPOINT p1;",
                               "COMMIT AND NO CHAIN NO RELEASE;"}) {
            const auto compiled=compile(dialect,sql);
            QVERIFY2(compiled->ok(),compiled->diagnostics.empty()?"invalid SQL":compiled->diagnostics.front().message.c_str());
            const auto generated=generateSql(compiled->model,dialect);
            QVERIFY(generated.ok());
            const auto result=db->execute(QString::fromStdString(generated.sql),{},token);
            QVERIFY2(result.error.isEmpty(),qPrintable(result.error));
        }
        const auto result=db->execute("SELECT id FROM transaction_codegen_owned",{},token);
        QVERIFY2(result.error.isEmpty(),qPrintable(result.error));
        QCOMPARE(result.rows.size(),1);
        QCOMPARE(result.rows.at(0).at(0).toInt(),1);
    }catch(const std::exception &e){QFAIL(e.what());}}
    void generatedJsonTableKeepsNestedAndOuterRows() {try {
        using namespace navidog;
        using namespace navidog::sql::compiler;
        if (p_.driver!=DatabaseKind::MySQL) QSKIP("MySQL-specific JSON_TABLE grammar");
        auto db=connectDatabase(p_);
        auto token=cancellation();
        auto run=[&](const QString &sql) {
            auto result=db->execute(sql,{},token);
            if (!result.error.isEmpty()) throw std::runtime_error(result.error.toStdString());
            return result;
        };
        run("CREATE TEMPORARY TABLE json_table_owned(id INT,doc JSON)");
        run(R"(INSERT INTO json_table_owned VALUES
             (1,'[{"kind":1,"items":[10,20]},{"items":[]}]'),
             (2,'[]'),(3,'[{"kind":2,"items":[30]}]'))");
        const auto source=R"(SELECT t.id,j.ord,j.val,j.present,j.item
          FROM json_table_owned t LEFT JOIN JSON_TABLE(t.doc,'$[*]' COLUMNS(
            ord FOR ORDINALITY, val INT PATH '$.kind' DEFAULT '7' ON EMPTY NULL ON ERROR,
            present INT EXISTS PATH '$.kind', NESTED PATH '$.items[*]' COLUMNS(item INT PATH '$')
          )) AS j ON j.val<>2 ORDER BY t.id,j.ord,j.item;)";
        const auto compiled=compile(Dialect::MySQL,source);
        QVERIFY2(compiled->ok(),compiled->diagnostics.empty()?"invalid SQL":compiled->diagnostics.front().message.c_str());
        const auto generated=generateSql(compiled->model,Dialect::MySQL);
        QVERIFY(generated.ok());
        const auto original=run(QString::fromUtf8(source));
        const auto canonical=run(QString::fromStdString(generated.sql));
        QCOMPARE(original.rows,canonical.rows);
        QCOMPARE(canonical.rows.size(),5);
        QCOMPARE(canonical.rows.at(2).at(2).toInt(),7);
        QVERIFY(canonical.rows.at(2).at(4).isNull());
        QVERIFY(canonical.rows.at(3).at(1).isNull());
        QVERIFY(canonical.rows.at(4).at(1).isNull());
    }catch(const std::exception &e){QFAIL(e.what());}}
    void generatedDefaultExpressionsKeepMySqlSyntax() {try {
        using namespace navidog;
        using namespace navidog::sql::compiler;
        if (p_.driver!=DatabaseKind::MySQL) QSKIP("MySQL-specific default expression grammar");
        auto db=connectDatabase(p_);
        auto token=cancellation();
        auto compiled=compile(Dialect::MySQL,"CREATE TEMPORARY TABLE default_expr_owned "
            "(b BLOB DEFAULT ('abc'),d DATE DEFAULT (CURRENT_DATE), doc JSON DEFAULT NULL);");
        QVERIFY(compiled->ok());
        auto canonical=generateSql(compiled->model,Dialect::MySQL);
        QVERIFY(canonical.ok());
        auto created=db->execute(QString::fromStdString(canonical.sql),{},token);
        QVERIFY2(created.error.isEmpty(),qPrintable(created.error));
        auto inserted=db->execute("INSERT INTO default_expr_owned () VALUES()",{},token);
        QVERIFY2(inserted.error.isEmpty(),qPrintable(inserted.error));
        const auto result=db->execute("SELECT HEX(b),d=CURRENT_DATE,doc IS NULL FROM default_expr_owned",{},token);
        QVERIFY2(result.error.isEmpty(),qPrintable(result.error));
        QCOMPARE(result.rows.at(0).at(0).toString(),QString("616263"));
        QCOMPARE(result.rows.at(0).at(1).toInt(),1);
        QCOMPARE(result.rows.at(0).at(2).toInt(),1);
    }catch(const std::exception &e){QFAIL(e.what());}}
    void generatedPreparedStatementsPreserveSessionAndArguments() {try {
        using namespace navidog;
        using namespace navidog::sql::compiler;
        auto db=connectDatabase(p_);
        auto token=cancellation();
        auto value=db->execute("SET @nvd_prepared_arg=7",{},token);
        QVERIFY2(value.error.isEmpty(),qPrintable(value.error));
        const auto dialect=p_.driver==DatabaseKind::MariaDB?Dialect::MariaDB:Dialect::MySQL;
        for (const auto source : {"PREPARE prepared_owned FROM 'SELECT ? + 5';",
                                  "EXECUTE prepared_owned USING @nvd_prepared_arg;",
                                  "DEALLOCATE PREPARE prepared_owned;"}) {
            auto compiled=compile(dialect,source);
            QVERIFY2(compiled->ok(),compiled->diagnostics.empty()?"invalid SQL":compiled->diagnostics.front().message.c_str());
            auto generated=generateSql(compiled->model,dialect);
            QVERIFY(generated.ok());
            auto result=db->execute(QString::fromStdString(generated.sql),{},token);
            QVERIFY2(result.error.isEmpty(),qPrintable(result.error));
            if (compiled->model.front().prepared->action==PreparedAction::Execute) {
                QCOMPARE(result.rows.size(),1);
                QCOMPARE(result.rows.at(0).at(0).toInt(),12);
            }
        }
    }catch(const std::exception &e){QFAIL(e.what());}}
    void generatedDerivedColumnAliasesPreserveOrder() {try {
        using namespace navidog;
        using namespace navidog::sql::compiler;
        if (p_.driver != DatabaseKind::MySQL) QSKIP("MySQL derived alias grammar baseline");
        auto db = connectDatabase(p_);
        auto token = cancellation();
        const auto source = "SELECT d.x,d.y FROM (SELECT 7 AS previous,9 AS keep) AS d(x,y);";
        const auto compiled = compile(Dialect::MySQL, source);
        QVERIFY(compiled->ok());
        const auto canonical = generateSql(compiled->model, Dialect::MySQL);
        QVERIFY(canonical.ok());
        const auto before = db->execute(source, {}, token);
        const auto after = db->execute(QString::fromStdString(canonical.sql), {}, token);
        QVERIFY2(before.error.isEmpty(),qPrintable(before.error));
        QVERIFY2(after.error.isEmpty(),qPrintable(after.error));
        QCOMPARE(before.columns,after.columns);
        QCOMPARE(before.rows,after.rows);
        QCOMPARE(after.rows.at(0).at(0).toInt(),7);
        QCOMPARE(after.rows.at(0).at(1).toInt(),9);
    }catch(const std::exception &e){QFAIL(e.what());}}
    void generatedRoleControlsPreserveSessionAndAccountEffects() {try {
        using namespace navidog;
        using namespace navidog::sql::compiler;
        if (p_.driver != DatabaseKind::MySQL) QSKIP("MySQL-specific role controls");
        auto db = connectDatabase(p_);
        auto token = cancellation();
        const auto run = [&](const QString &sql) {
            auto result = db->execute(sql, {}, token);
            if (!result.error.isEmpty()) throw std::runtime_error(result.error.toStdString());
            return result;
        };
        const auto generated = [&](const char *sql) {
            const auto compilation = compile(Dialect::MySQL, sql);
            if (!compilation->ok()) throw std::runtime_error(compilation->diagnostics.front().message);
            const auto canonical = generateSql(compilation->model, Dialect::MySQL);
            if (!canonical.ok()) throw std::runtime_error(canonical.diagnostics.front().message);
            run(QString::fromStdString(canonical.sql));
        };
        run("CREATE ROLE 'role_owned'");
        run("CREATE USER 'role_user_owned'@'localhost'");
        run("GRANT 'role_owned' TO 'role_user_owned'@'localhost'");
        run("CREATE TEMPORARY TABLE role_test_owned(id INT) ENGINE=InnoDB");

        run("START TRANSACTION");
        run("INSERT INTO role_test_owned VALUES(1)");
        generated("SET ROLE NONE;");
        run("ROLLBACK");
        QCOMPARE(run("SELECT CURRENT_ROLE()").rows.at(0).at(0).toString(), QString("NONE"));
        QCOMPARE(run("SELECT COUNT(*) FROM role_test_owned").rows.at(0).at(0).toInt(), 0);

        run("START TRANSACTION");
        run("INSERT INTO role_test_owned VALUES(2)");
        generated("SET DEFAULT ROLE ALL TO 'role_user_owned'@'localhost';");
        run("ROLLBACK");
        QCOMPARE(run("SELECT COUNT(*) FROM role_test_owned").rows.at(0).at(0).toInt(), 1);
        QCOMPARE(run("SELECT DEFAULT_ROLE_USER FROM mysql.default_roles WHERE USER='role_user_owned'")
                     .rows.at(0).at(0).toString(), QString("role_owned"));
        run("DROP USER 'role_user_owned'@'localhost'");
        run("DROP ROLE 'role_owned'");
    }catch(const std::exception &e){QFAIL(e.what());}}
    void generatedStraightJoinPreservesRows() {try {
        using namespace navidog;
        using namespace navidog::sql::compiler;
        auto db=connectDatabase(p_);
        auto token=cancellation();
        auto run=[&](const QString &sql) {
            auto result=db->execute(sql,{},token);
            if (!result.error.isEmpty()) throw std::runtime_error(result.error.toStdString());
            return result;
        };
        const auto dialect=p_.driver==DatabaseKind::MariaDB?Dialect::MariaDB:Dialect::MySQL;
        run("CREATE TEMPORARY TABLE join_left_owned(id INT)");
        run("CREATE TEMPORARY TABLE join_right_owned(id INT,v INT)");
        run("INSERT INTO join_left_owned VALUES(1),(2)");
        run("INSERT INTO join_right_owned VALUES(1,3),(1,4)");
        for (auto sql : {
            "SELECT a.id,b.v FROM join_left_owned a STRAIGHT_JOIN join_right_owned b ON a.id=b.id ORDER BY b.v;",
            "SELECT STRAIGHT_JOIN a.id,b.v FROM join_left_owned a JOIN join_right_owned b ON a.id=b.id ORDER BY b.v;"}) {
            const auto compiled=compile(dialect,sql);
            QVERIFY(compiled->ok());
            const auto generated=generateSql(compiled->model,dialect);
            QVERIFY(generated.ok());
            const auto original=run(QString::fromUtf8(sql));
            const auto canonical=run(QString::fromStdString(generated.sql));
            QCOMPARE(original.rows,canonical.rows);
            QCOMPARE(canonical.rows.size(),2);
        }
    }catch(const std::exception &e){QFAIL(e.what());}}
    void replaceAndInsertDefaultsRoundTrip() {try {
        using namespace navidog;
        using namespace navidog::sql::compiler;
        auto db=connectDatabase(p_);
        auto token=cancellation();
        auto run=[&](const QString &sql) {
            auto result=db->execute(sql,{},token);
            if (!result.error.isEmpty())
                throw std::runtime_error(result.error.toStdString());
            return result;
        };
        const auto dialect=p_.driver==DatabaseKind::MariaDB?Dialect::MariaDB:Dialect::MySQL;
        run("CREATE TEMPORARY TABLE replace_owned(id INT AUTO_INCREMENT PRIMARY KEY, n INT DEFAULT 37)");
        for (const auto sql : {"INSERT INTO replace_owned () VALUES();",
                               "INSERT INTO replace_owned () VALUES(4,40);",
                               "INSERT INTO replace_owned () SELECT 5,50;"}) {
            auto compiled=compile(dialect,sql);
            QVERIFY2(compiled->ok(),compiled->diagnostics.empty()?"invalid SQL":compiled->diagnostics.front().message.c_str());
            auto generated=generateSql(compiled->model,dialect);
            QVERIFY2(generated.ok(),generated.diagnostics.empty()?"no SQL":generated.diagnostics.front().message.c_str());
            run(QString::fromStdString(generated.sql));
        }
        auto state=run("SELECT id,n FROM replace_owned ORDER BY id");
        QCOMPARE(state.rows.size(),3);
        QCOMPARE(state.rows.at(0).at(1).toInt(),37);
        QCOMPARE(state.rows.at(1).at(1).toInt(),40);
        QCOMPARE(state.rows.at(2).at(1).toInt(),50);
        auto replaced=compile(dialect,"REPLACE INTO replace_owned(id) VALUES(4)");
        QVERIFY(replaced->ok());
        auto generated=generateSql(replaced->model,dialect);
        QVERIFY(generated.ok());
        run(QString::fromStdString(generated.sql));
        QCOMPARE(run("SELECT n FROM replace_owned WHERE id=4").rows.at(0).at(0).toInt(),37);
        if (dialect==Dialect::MySQL) {
            const auto upsert=compile(dialect,
                "INSERT INTO replace_owned(id,n) VALUES(4,12) AS incoming(k,v) "
                "ON DUPLICATE KEY UPDATE n=incoming.v+replace_owned.n;");
            QVERIFY2(upsert->ok(),upsert->diagnostics.empty()?"invalid SQL":upsert->diagnostics.front().message.c_str());
            const auto canonical=generateSql(upsert->model,dialect);
            QVERIFY(canonical.ok());
            run(QString::fromStdString(canonical.sql));
            QCOMPARE(run("SELECT n FROM replace_owned WHERE id=4").rows.at(0).at(0).toInt(),49);
        }
    }catch(const std::exception& e){QFAIL(e.what());}}
    void setQueriesAndNamedWindowsMatchCanonicalSql() {try {
        using namespace navidog;using namespace navidog::sql::compiler;auto db=connectDatabase(p_);auto token=cancellation();
        auto setup=db->execute("CREATE TEMPORARY TABLE table_query_owned(id INTEGER)",{},token);
        QVERIFY2(setup.error.isEmpty(),qPrintable(setup.error));
        setup=db->execute("INSERT INTO table_query_owned VALUES(2),(1),(3)",{},token);
        QVERIFY2(setup.error.isEmpty(),qPrintable(setup.error));
        setup=db->execute("CREATE TEMPORARY TABLE table_query_other(id INTEGER)",{},token);
        QVERIFY2(setup.error.isEmpty(),qPrintable(setup.error));
        setup=db->execute("INSERT INTO table_query_other VALUES(4)",{},token);
        QVERIFY2(setup.error.isEmpty(),qPrintable(setup.error));
        for(const auto &sql:{"SELECT 1 AS id UNION ALL SELECT 2 INTERSECT SELECT 2 ORDER BY id;","(SELECT 1 AS id UNION SELECT 2) INTERSECT SELECT 2;","SELECT 1 AS id EXCEPT(SELECT 1 UNION ALL SELECT 2);","WITH q AS(SELECT 1 AS id) SELECT id FROM q UNION ALL SELECT id+1 FROM q ORDER BY id;","WITH RECURSIVE seq(n) AS (SELECT 1 UNION ALL SELECT n+1 FROM seq WHERE n<6) SELECT n FROM seq ORDER BY n;","TABLE table_query_owned ORDER BY id LIMIT 2;", "VALUES ROW(1, 'one'), ROW(2, 'two') ORDER BY column_0 DESC;", "TABLE table_query_owned UNION ALL TABLE table_query_other ORDER BY id;","SELECT x,SUM(x) OVER w AS total FROM(SELECT 1 AS x UNION ALL SELECT 2) q WINDOW w AS(ORDER BY x ROWS UNBOUNDED PRECEDING) ORDER BY x;"}) {
          if (p_.driver != DatabaseKind::MySQL && (QString(sql).startsWith("TABLE ") || QString(sql).startsWith("VALUES "))) continue;
          auto dialect=p_.driver==DatabaseKind::PostgreSQL?Dialect::PostgreSQL:Dialect::MySQL;auto c=compile(dialect,sql);QVERIFY2(c->ok(),c->diagnostics.empty()?"no model":c->diagnostics.front().message.c_str());auto generated=generateSql(c->model,dialect);QVERIFY2(generated.ok(),generated.diagnostics.empty()?"no SQL":generated.diagnostics.front().message.c_str());
          auto original=db->execute(sql,{},token);QVERIFY2(original.error.isEmpty(),qPrintable(original.error));auto actual=db->execute(QString::fromStdString(generated.sql),{},token);QVERIFY2(actual.error.isEmpty(),qPrintable(actual.error+"\n"+QString::fromStdString(generated.sql)));QCOMPARE(actual.rows,original.rows);
        }
        for(const auto &offset:{"1.0","1.5","1e0","-1"}) {
          auto sql=QString("SELECT SUM(1) OVER(ROWS %1 PRECEDING)").arg(offset);bool accepted=false;try{accepted=db->execute(sql,{},token).error.isEmpty();}catch(const std::exception&){}
          auto c=compile(p_.driver==DatabaseKind::PostgreSQL?Dialect::PostgreSQL:Dialect::MySQL,sql.toStdString());
          QVERIFY2(!accepted || c->ok(),qPrintable("native accepts offset "+QString(offset)+" but local checker rejects it"));
        }
    }catch(const std::exception& e){QFAIL(e.what());}}
    void outputAliasesRespectNativeScopeRules() {try {
      using namespace navidog;using namespace navidog::sql::compiler;
      auto db=connectDatabase(p_);auto token=cancellation();
      auto run=[&](const QString &text){auto r=db->execute(text,{},token);if(!r.error.isEmpty())throw std::runtime_error(r.error.toStdString());return r;};
      run("CREATE TABLE alias_scope_owned(x INTEGER,g INTEGER)");
      run("INSERT INTO alias_scope_owned VALUES(1,10),(2,10),(3,20)");
      auto snapshot=collectMetadata(*db,p_,{},token);
      MetadataCatalog catalog(std::make_shared<const MetadataSnapshot>(snapshot));
      NameResolutionContext names{p_.database.toStdString(),{},Dialect::MySQL};
      for(const auto &text:{"SELECT x+1 AS z FROM alias_scope_owned GROUP BY z ORDER BY z;","SELECT SUM(x) AS total FROM alias_scope_owned HAVING total>1;"}) {
        auto c=compile(Dialect::MySQL,text,{},&catalog,names);QVERIFY2(c->ok(),c->diagnostics.empty()?"no model":c->diagnostics.front().message.c_str());
        auto sql=generateSql(c->model,Dialect::MySQL);QVERIFY2(sql.ok(),"no generated alias SQL");QCOMPARE(run(QString::fromStdString(sql.sql)).rows,run(text).rows);
      }
      auto ungrouped=compile(Dialect::MySQL,"SELECT SUM(x) AS g FROM alias_scope_owned HAVING g>5;",{},&catalog,names);
      QVERIFY(ungrouped->ok());const auto &uq=ungrouped->model[0];QVERIFY(uq.expressions.at(uq.expressions.at(uq.having).operands[0]).outputColumn==0);
      QCOMPARE(run("SELECT SUM(x) AS g FROM alias_scope_owned HAVING g>5").rows[0][0].toInt(),6);
      auto grouped=compile(Dialect::MySQL,"SELECT SUM(x) AS g FROM alias_scope_owned GROUP BY g HAVING g=10;",{},&catalog,names);
      QVERIFY(grouped->ok());const auto &gq=grouped->model[0];QVERIFY(!gq.expressions.at(gq.expressions.at(gq.having).operands[0]).outputColumn);
      QCOMPARE(run("SELECT SUM(x) AS g FROM alias_scope_owned GROUP BY g HAVING g=10").rows[0][0].toInt(),3);
      run("DROP TABLE alias_scope_owned");
    }catch(const std::exception& e){QFAIL(e.what());}}
    void plannerCatalogStatisticsUseNativeRowsColumnsAndIndexes() {try {
        using namespace navidog;auto token=cancellation();auto db=connectDatabase(p_);
        auto run=[&](const QString &sql){auto r=db->execute(sql,{},token);if(!r.error.isEmpty())throw std::runtime_error(r.error.toStdString());return r;};
        run("CREATE TABLE planner_stats_owned(id INTEGER PRIMARY KEY,n INTEGER)");run("INSERT INTO planner_stats_owned VALUES(1,1),(2,1),(3,2),(4,NULL),(5,NULL)");
        run("ANALYZE TABLE planner_stats_owned");
        auto stats=collectPlannerStatistics(*db,p_,{},token);QVERIFY2(stats.complete,qPrintable(stats.warning));
        const auto found=std::find_if(stats.tables.begin(),stats.tables.end(),[](const auto &t){return t.name=="planner_stats_owned";});QVERIFY(found!=stats.tables.end());QVERIFY(found->rows>=0);QCOMPARE(found->source,sql::optimizer::StatisticsSource::ServerCatalog);QVERIFY(!found->indexes.empty());
        QVERIFY(std::any_of(found->indexes.begin(),found->indexes.end(),[](const auto &i){return i.unique && i.columns==std::vector<std::string>{"id"};}));
        QVERIFY(std::any_of(found->columns.begin(),found->columns.end(),[](const auto &c){return c.name=="id" && c.distinct.has_value();}));
        QCOMPARE(run("SELECT COUNT(*) FROM planner_stats_owned").rows[0][0].toInt(),5);run("DROP TABLE planner_stats_owned");
    }catch(const std::exception& e){QFAIL(e.what());}}
    void typedLiteralCanonicalSqlMatchesServer() {try {
      using namespace navidog;using namespace navidog::sql::compiler;
      auto db=connectDatabase(p_);auto token=cancellation();
      auto run=[&](const QString &sql){auto r=db->execute(sql,{},token);if(!r.error.isEmpty())throw std::runtime_error(r.error.toStdString());return r;};
      const auto dialect=Dialect::MySQL;
      for(const auto &sql:{"SELECT DATE '2024-01-31'+INTERVAL 1 MONTH,DATE_ADD(DATE '2024-01-31',INTERVAL (1+2) DAY),DATE_SUB(DATE '2024-01-01',INTERVAL '1:02:03' HOUR_SECOND)","SELECT EXTRACT(DAY_HOUR FROM TIMESTAMP '2024-02-29 12:34:56'),EXTRACT(MICROSECOND FROM TIME '12:34:56.123456'),INTERVAL(23,1,15,17,30)","SELECT DATE '2024-02-29', TIME '12:34:56.123456', TIMESTAMP '2024-02-29 12:34:56.123456'","SELECT DATE('2024-01-01') 'label', TIME('12:34:56') AS 'time alias'"}) {
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
    void temporalRangeFrameMatchesServer() {try {
      using namespace navidog;using namespace navidog::sql::compiler;
      if (p_.driver!=DatabaseKind::MySQL) QSKIP("MySQL temporal RANGE frame grammar");
      auto db=connectDatabase(p_);auto token=cancellation();
      const auto sql="WITH t(d,n) AS(SELECT DATE '2024-01-01',1 UNION ALL SELECT DATE '2024-01-02',2 UNION ALL SELECT DATE '2024-01-04',4) SELECT d,SUM(n) OVER(ORDER BY d RANGE BETWEEN INTERVAL 1 DAY PRECEDING AND CURRENT ROW) AS s FROM t ORDER BY d";
      auto c=compile(Dialect::MySQL,sql);QVERIFY2(c->ok(),c->diagnostics.empty()?"no model":c->diagnostics.front().message.c_str());
      auto generated=generateSql(c->model,Dialect::MySQL);QVERIFY(generated.ok());auto original=db->execute(sql,{},token),actual=db->execute(QString::fromStdString(generated.sql),{},token);
      QVERIFY2(original.error.isEmpty(),qPrintable(original.error));QVERIFY2(actual.error.isEmpty(),qPrintable(actual.error));QCOMPARE(actual.rows,original.rows);QCOMPARE(actual.rows.size(),3);QCOMPARE(actual.rows[0][1].toInt(),1);QCOMPARE(actual.rows[1][1].toInt(),3);QCOMPARE(actual.rows[2][1].toInt(),4);
      const auto unused="SELECT 1 WINDOW w AS(ORDER BY DATE '2024-01-01' RANGE INTERVAL 1 DAY PRECEDING)";
      QVERIFY(compile(Dialect::MySQL,unused)->ok());auto unusedResult=db->execute(unused,{},token);QVERIFY2(unusedResult.error.isEmpty(),qPrintable(unusedResult.error));
      for(const auto &bad:{"SELECT COUNT(*) OVER(ORDER BY DATE '2024-01-01' RANGE 1 PRECEDING)","SELECT COUNT(*) OVER(ORDER BY 1 RANGE INTERVAL 1 DAY PRECEDING)"}) {
        bool rejected=false;try{rejected=!db->execute(bad,{},token).error.isEmpty();}catch(const std::exception &){rejected=true;}QVERIFY2(rejected,bad);QVERIFY(!compile(Dialect::MySQL,bad)->ok());
      }
    }catch(const std::exception &e){QFAIL(e.what());}}
    void aggregateDomainsMatchServer() {try {
      using namespace navidog;using namespace navidog::sql::compiler;
      auto db=connectDatabase(p_);auto token=cancellation();
      const auto sql="SELECT SUM(CAST(1 AS SIGNED)),AVG(CAST(1 AS DECIMAL(8,3))),SUM(CAST(1 AS DOUBLE))";
      auto c=compile(Dialect::MySQL,sql);QVERIFY(c->ok());auto generated=generateSql(c->model,Dialect::MySQL);QVERIFY(generated.ok());
      auto actual=db->execute(sql,{},token);QVERIFY2(actual.error.isEmpty(),qPrintable(actual.error));
      QCOMPARE(actual.columnTypes,QStringList({"DECIMAL","DECIMAL","DOUBLE"}));
      auto rewritten=db->execute(QString::fromStdString(generated.sql),{},token);QVERIFY(rewritten.error.isEmpty());QCOMPARE(rewritten.rows,actual.rows);
    }catch(const std::exception &e){QFAIL(e.what());}}
    void integerDivisionAndInfixModuloMatchNativeMySqlFamily(){try {
      using namespace navidog;
      using namespace navidog::sql::compiler;
      const auto dialect=p_.driver==DatabaseKind::MariaDB?Dialect::MariaDB:Dialect::MySQL;
      auto db=connectDatabase(p_);
      const auto token=cancellation();
      const auto run=[&](const QString &sql) {
        const auto result=db->execute(sql,{},token);
        if (!result.error.isEmpty()) throw std::runtime_error(result.error.toStdString());
        return result;
      };
      const std::string sql="select 10+15 div 2,21 mod 4*2,9.9 div 2.1,-21 mod 4,1 div 0,"
          "9223372036854775807 div 2;";
      const auto parsed=compile(dialect,sql);
      QVERIFY2(parsed->ok(),parsed->diagnostics.empty()?"no model":parsed->diagnostics.front().message.c_str());
      const auto output=generateSql(parsed->model,dialect);
      QVERIFY2(output.ok(),output.diagnostics.empty()?"no SQL":output.diagnostics.front().message.c_str());
      const auto original=run(QString::fromStdString(sql));
      QCOMPARE(run(QString::fromStdString(output.sql)).rows,original.rows);
      QCOMPARE(original.rows.size(),1);
      const auto &row=original.rows.front();
      QCOMPARE(row[0].toLongLong(),17LL);
      QCOMPARE(row[1].toLongLong(),2LL);
      QCOMPARE(row[2].toLongLong(),4LL);
      QCOMPARE(row[3].toLongLong(),-1LL);
      QVERIFY(row[4].isNull());
      QCOMPARE(row[5].toLongLong(),4611686018427387903LL);
      const auto formatted=formatSql(dialect,sql);
      QVERIFY(formatted.ok());
      QVERIFY(formatted.sql.find(" DIV ")!=std::string::npos);
      QVERIFY(formatted.sql.find(" MOD ")!=std::string::npos);
      QCOMPARE(run(QString::fromStdString(formatted.sql)).rows,original.rows);
    }catch(const std::exception &e){QFAIL(e.what());}}
    void numericTypeDerivationMatchesServer() {try {
      using namespace navidog;
      using namespace navidog::sql::compiler;
      if (p_.driver!=DatabaseKind::MySQL) QSKIP("MySQL numeric metadata and session options");
      auto db=connectDatabase(p_);
      const auto token=cancellation();
      const auto run=[&](const QString &sql) {
        auto result=db->execute(sql,{},token);
        if (!result.error.isEmpty()) throw std::runtime_error(result.error.toStdString());
        return result;
      };
      constexpr auto dialect=Dialect::MySQL;
      run("SET SESSION div_precision_increment=9");
      run("SET SESSION sql_mode='NO_UNSIGNED_SUBTRACTION'");
      run("CREATE TABLE numeric_owned(a DECIMAL(8,3), b DECIMAL(5,2), i INT,"
          "u BIGINT UNSIGNED, f FLOAT, g DECIMAL(60,30), h DECIMAL(60,20))");
      run("INSERT INTO numeric_owned VALUES (10,2,3,7,1,2,3)");
      MemoryCatalog catalog;
      sql::compiler::TableDefinition table;
      table.database=p_.database.toStdString();
      table.name="numeric_owned";
      for (const auto &row:run("SHOW COLUMNS FROM numeric_owned").rows) {
        const auto type=catalogTypeFromText(dialect,row.at(1).toString().toStdString());
        QVERIFY(type.has_value());
        table.columns.push_back({row.at(0).toString().toStdString(),valueCategory(*type),
          std::make_shared<const SqlType>(*type)});
      }
      catalog.tables.push_back(std::move(table));
      LanguageOptions options;
      options.mysqlDivisionPrecisionIncrement=9;
      options.mysqlNoUnsignedSubtraction=true;
      const auto compare=[&](const QString &name,const QString &sql) {
        const auto result=compile(dialect,sql.toStdString(),options,&catalog,
                                  {p_.database.toStdString(),{},dialect,{}});
        QVERIFY2(result->ok(),result->diagnostics.empty()?"no model":result->diagnostics.front().message.c_str());
        const auto response=run(sql);
        run("CREATE TEMPORARY TABLE "+name+" AS "+sql);
        const auto columns=run("SHOW COLUMNS FROM "+name).rows;
        const auto &root=result->model.front();
        QCOMPARE(static_cast<std::size_t>(columns.size()),root.projections.size());
        for (qsizetype i=0;i<columns.size();++i) {
          const auto &row=columns.at(i);
          const auto &expected=result->model.types.get(root.expressions.at(
              root.projections.at(static_cast<std::size_t>(i)).expression).sqlType);
          const auto field=expected.family==TypeFamily::Decimal
              ?row.at(1).toString():response.columnTypes.at(i);
          const auto actual=catalogTypeFromText(dialect,field.toStdString());
          const auto label=row.at(0).toString()+" "+field;
          QVERIFY2(actual && actual->family==expected.family,qPrintable(label));
          if (expected.bits) QVERIFY2(actual->bits==expected.bits,qPrintable(label));
          if (expected.unsignedInteger) QVERIFY2(actual->unsignedInteger==expected.unsignedInteger,qPrintable(label));
          if (expected.precision) QVERIFY2(actual->precision==expected.precision,qPrintable(label));
          if (expected.scale) QVERIFY2(actual->scale==expected.scale,qPrintable(label));
        }
      };
      compare("numeric_literals","SELECT 2147483647 AS v1,2147483648 AS v2,9223372036854775808 AS v3,"
          "18446744073709551616 AS v4,-2147483648 AS v5,-9223372036854775808 AS v6,"
          "-9223372036854775809 AS v7,.05 AS v8,000.0020 AS v9,-.0020 AS v10,2.34E0 AS v11");
      compare("numeric_expressions","SELECT a+b AS v1,a-b AS v2,a*b AS v3,a/b AS v4,(a/b)/b AS v5,"
          "a+i AS v6,a+000001 AS v7,u-i AS v8,a/u AS v9,g*h AS v10,f*a AS v11 FROM numeric_owned");
      compare("numeric_aggregates","SELECT SUM(a) AS v1,SUM(i) AS v2,AVG(a) AS v3,AVG(u) AS v4 "
          "FROM numeric_owned");
    }catch(const std::exception &e){QFAIL(e.what());}}
    void castNativeSyntaxAndExactConversionsMatchServer() {try {
      using namespace navidog;using namespace navidog::sql::compiler;
      if (p_.driver!=DatabaseKind::MySQL) QSKIP("MySQL native CAST grammar");
      auto db=connectDatabase(p_);auto token=cancellation();
      auto run=[&](const QString &sql){auto r=db->execute(sql,{},token);if(!r.error.isEmpty())throw std::runtime_error(r.error.toStdString());return r;};
      for(const auto &target:{"INTEGER","BIGINT","VARCHAR(20)","BOOLEAN","TIMESTAMP"}) {
        const auto sql=QString("SELECT CAST(1 AS %1)").arg(target);bool rejected=false;
        try{rejected=!db->execute(sql,{},token).error.isEmpty();}catch(const std::exception &){rejected=true;}
        QVERIFY2(rejected,qPrintable(sql));QVERIFY(!compile(Dialect::MySQL,sql.toStdString())->ok());
      }
      for(const auto &sql:{"SELECT CAST('2024' AS YEAR)","SELECT CAST(-1 AS SIGNED INTEGER)","SELECT CAST(NULL AS CHAR(40))"}) {
        auto c=compile(Dialect::MySQL,sql);QVERIFY(c->ok());auto generated=generateSql(c->model,Dialect::MySQL);QVERIFY(generated.ok());
        QCOMPARE(run(QString::fromStdString(generated.sql)).rows,run(sql).rows);
      }
      auto converted=convertSql(Dialect::PostgreSQL,Dialect::MySQL,"SELECT CAST(42 AS BIGINT),CAST(-9223372036854775808 AS BIGINT)");QVERIFY2(converted.ok(),converted.diagnostics.empty()?"no SQL":converted.diagnostics.front().message.c_str());
      QCOMPARE(run(QString::fromStdString(converted.sql)).rows,run("SELECT CAST(42 AS SIGNED),CAST(-9223372036854775808 AS SIGNED)").rows);
    }catch(const std::exception &e){QFAIL(e.what());}}
    void mariaDbCastAndRangeRespectTheirNativeDialect() {try {
      using namespace navidog;
      using namespace navidog::sql::compiler;
      if (p_.driver!=DatabaseKind::MariaDB) QSKIP("MariaDB-specific CAST and RANGE frame grammar");
      constexpr auto dialect=Dialect::MariaDB;
      auto db=connectDatabase(p_);
      const auto token=cancellation();
      const auto run=[&](const QString &sql) {
        const auto result=db->execute(sql,{},token);
        if (!result.error.isEmpty()) throw std::runtime_error(result.error.toStdString());
        return result;
      };
      for (const auto *sql:{
          "SELECT CAST(9223372036854775807 AS INTEGER), CAST(42 AS INT), CAST('abc' AS VARCHAR(20))",
          "SELECT SUM(1) OVER(ORDER BY 1 RANGE 2 PRECEDING)",
          "SELECT SUM(1) OVER(ORDER BY DATE '2024-01-01' RANGE UNBOUNDED PRECEDING)"}) {
        const auto parsed=compile(dialect,sql);
        QVERIFY2(parsed->ok(),parsed->diagnostics.empty()?"no model":parsed->diagnostics.front().message.c_str());
        const auto generated=generateSql(parsed->model,dialect);
        QVERIFY2(generated.ok(),generated.diagnostics.empty()?"no SQL":generated.diagnostics.front().message.c_str());
        QCOMPARE(run(QString::fromStdString(generated.sql)).rows,run(sql).rows);
      }
      for (const auto *sql:{
          "SELECT CAST(42 AS BIGINT)", "SELECT CAST(42 AS YEAR)",
          "SELECT SUM(1) OVER(ORDER BY DATE '2024-01-01' RANGE INTERVAL 1 DAY PRECEDING)",
          "SELECT SUM(1) OVER(ORDER BY DATE '2024-01-01' RANGE 2 PRECEDING)",
          "SELECT SUM(1) OVER w WINDOW w AS(ORDER BY DATE '2024-01-01' RANGE INTERVAL 1 DAY PRECEDING)"}) {
        bool rejected=false;
        try { run(sql); } catch (const std::runtime_error &) { rejected=true; }
        QVERIFY2(rejected,sql);
        QVERIFY2(!compile(dialect,sql)->ok(),sql);
      }
      auto converted=convertSql(Dialect::MySQL,dialect,
          "SELECT SUM(1) OVER(ORDER BY DATE '2024-01-01' RANGE INTERVAL 1 DAY PRECEDING)");
      QVERIFY(!converted.ok());
    }catch(const std::exception &e){QFAIL(e.what());}}
    void nativeNonNullMetadataDrivesProvedFilters() {try {
      using namespace navidog;using namespace navidog::sql::compiler;namespace opt=navidog::sql::optimizer;
      auto db=connectDatabase(p_);auto token=cancellation();
      auto run=[&](const QString &sql){auto r=db->execute(sql,{},token);if(!r.error.isEmpty())throw std::runtime_error(r.error.toStdString());return r;};
      run("CREATE TABLE proof_native_owned(id INTEGER NOT NULL,n INTEGER)");run("INSERT INTO proof_native_owned VALUES(1,NULL),(2,3)");
      auto snapshot=collectMetadata(*db,p_,{},token);MetadataCatalog catalog(std::make_shared<const MetadataSnapshot>(snapshot));
      auto fired=[](const opt::OptimizationResult &r){return std::any_of(r.rules.begin(),r.rules.end(),[](const auto &rule){return rule.rule=="HepFilterNullability";});};
      for(const auto &sql:{"SELECT id FROM proof_native_owned WHERE id IS NULL", "SELECT id FROM proof_native_owned WHERE id IS NOT NULL"}) {
        auto c=compile(Dialect::MySQL,sql,{},&catalog,{p_.database.toStdString(),"",Dialect::MySQL});QVERIFY(c->ok());
        auto planned=opt::optimize(c->model);QVERIFY(planned.ok());QVERIFY(fired(planned));
      }
      QCOMPARE(run("SELECT id FROM proof_native_owned WHERE id IS NULL").rows.size(),0);QCOMPARE(run("SELECT id FROM proof_native_owned WHERE id IS NOT NULL").rows.size(),2);
      auto nullable=compile(Dialect::MySQL,"SELECT id FROM proof_native_owned WHERE n IS NULL",{},&catalog,{p_.database.toStdString(),"",Dialect::MySQL});QVERIFY(nullable->ok());auto kept=opt::optimize(nullable->model);QVERIFY(kept.ok());QVERIFY(!fired(kept));QCOMPARE(run("SELECT id FROM proof_native_owned WHERE n IS NULL").rows.size(),1);
      run("DROP TABLE proof_native_owned");
    }catch(const std::exception &e){QFAIL(e.what());}}
    void temporalInvalidFormsMatchServer() {try {
      using namespace navidog;using namespace navidog::sql::compiler;
      auto db=connectDatabase(p_);auto token=cancellation();
      for(const auto &sql:{"SELECT INTERVAL 1 DAY", "SELECT DATE_ADD(CURRENT_DATE,1)","SELECT NOW(9)","SELECT CURRENT_DATE(3)"}) {
        bool rejected=false;try{auto actual=db->execute(sql,{},token);rejected=!actual.error.isEmpty();}catch(const std::exception &){rejected=true;}
        QVERIFY2(rejected,sql);
        auto local=compile(Dialect::MySQL,sql);QVERIFY2(!local->ok(),sql);
      }
    }catch(const std::exception &e){QFAIL(e.what());}}
    void nativeTypeFactsMatchColumnDefinitionsAndCasts() {try {
      using namespace navidog;using namespace navidog::sql::compiler;
      auto db=connectDatabase(p_);auto token=cancellation();auto dialect=p_.driver==DatabaseKind::MariaDB?Dialect::MariaDB:Dialect::MySQL;
      auto run=[&](const QString &sql){auto r=db->execute(sql,{},token);if(!r.error.isEmpty())throw std::runtime_error(r.error.toStdString());return r;};
      run("CREATE TABLE native_types_owned(amount DECIMAL(12,3),label VARCHAR(30),counter BIGINT UNSIGNED)");
      auto snapshot=collectMetadata(*db,p_,{},token);QVERIFY(!snapshot.typeFacts.empty());
      MetadataCatalog catalog(std::make_shared<const MetadataSnapshot>(snapshot));NameResolutionContext names{p_.database.toStdString(),{},dialect};
      auto parsed=compile(dialect,"SELECT amount,label,counter FROM native_types_owned;",{},&catalog,names);
      QVERIFY2(parsed->ok(),parsed->diagnostics.empty()?"no model":parsed->diagnostics.front().message.c_str());
      const auto &q=parsed->model.front();auto type=[&](int i){return parsed->model.types.get(q.expressions[q.projections[i].expression].sqlType);};
      QCOMPARE(type(0).precision,std::optional<int>(12));QCOMPARE(type(0).scale,std::optional<int>(3));QCOMPARE(type(1).length,std::optional<std::uint64_t>(30));QCOMPARE(type(2).bits,std::optional<int>(64));QVERIFY(type(2).unsignedInteger==true);
      for(const auto &sql:{"SELECT CAST(-1 AS SIGNED INTEGER)","SELECT CAST(-1 AS UNSIGNED)","SELECT CAST(12.3456 AS DECIMAL(8,3))"}) {
        auto source=compile(dialect,sql);QVERIFY(source->ok());auto generated=generateSql(source->model,dialect);QVERIFY(generated.ok());
        QCOMPARE(run(QString::fromStdString(generated.sql)).rows,run(sql).rows);
      }
      run("DROP TABLE native_types_owned");
    }catch(const std::exception &e){QFAIL(e.what());}}
    void inspectionParserAndCanonicalSqlMatchServer() { try {
        using namespace navidog; using namespace navidog::sql::compiler;
        auto db=connectDatabase(p_);auto token=cancellation();
        auto run=[&](const QString &sql){auto r=db->execute(sql,{},token);if(!r.error.isEmpty())throw std::runtime_error(r.error.toStdString());return r;};
        run("CREATE TABLE inspection_owned(id INTEGER PRIMARY KEY, note VARCHAR(30))");
        run("CREATE VIEW inspection_view AS SELECT id FROM inspection_owned");
        const auto dialect=p_.driver==DatabaseKind::MariaDB?Dialect::MariaDB:Dialect::MySQL;
        for(const auto &sql:{"SHOW TABLES LIKE 'inspection_%';", "SHOW FULL TABLES WHERE Table_type='VIEW';",
                           "SHOW COLUMNS FROM inspection_owned;", "SHOW FULL FIELDS IN inspection_owned WHERE Field='id';",
                           "SHOW INDEXES IN inspection_owned WHERE Non_unique=0;", "SHOW CREATE TABLE inspection_owned;",
                           "SHOW CREATE VIEW inspection_view;", "SHOW GLOBAL VARIABLES LIKE 'max_connections';",
                           "DESCRIBE inspection_owned id;", "DESC inspection_owned 'n%';"}) {
            auto parsed=compile(dialect,sql);QVERIFY2(parsed->ok(),parsed->diagnostics.empty()?"no model":parsed->diagnostics.front().message.c_str());
            auto generated=generateSql(parsed->model,dialect);QVERIFY2(generated.ok(),generated.diagnostics.empty()?"no SQL":generated.diagnostics.front().message.c_str());
            auto original=run(sql), actual=run(QString::fromStdString(generated.sql));
            QCOMPARE(actual.columns,original.columns);QCOMPARE(actual.rows,original.rows);
        }
        auto metadata=collectMetadata(*db,p_,{},token);
        MetadataCatalog catalog(std::make_shared<const MetadataSnapshot>(metadata));
        NameResolutionContext names{p_.database.toStdString(),{},dialect};
        QVERIFY(compile(dialect,"SHOW COLUMNS FROM inspection_owned WHERE field='id';",{},&catalog,names)->ok());
        auto missing=compile(dialect,"SHOW INDEX FROM inspection_missing;",{},&catalog,names);
        QVERIFY(!missing->ok());QCOMPARE(missing->diagnostics.front().code,DiagnosticCode::UnknownTable);
        run("DROP VIEW inspection_view");run("DROP TABLE inspection_owned");
    } catch(const std::exception &e){QFAIL(e.what());} }
    void estimatedJsonPlanDoesNotExecuteDelete() {try {
        using namespace navidog;auto token=cancellation();auto db=connectDatabase(p_);
        auto run=[&](const QString &sql){auto r=db->execute(sql,{},token);if(!r.error.isEmpty())throw std::runtime_error(r.error.toStdString());return r;};
        run("CREATE TABLE estimated_plan_owned(id INTEGER PRIMARY KEY)");run("INSERT INTO estimated_plan_owned VALUES(1),(2)");
        PlanRequestTrace trace;auto raw=readQueryPlan(p_,"DELETE FROM estimated_plan_owned WHERE id=1",{},token,{}, {},&trace);QVERIFY(trace.issued && trace.received);QVERIFY(trace.commands.front().startsWith("EXPLAIN FORMAT=JSON DELETE"));
        QVERIFY2(raw.error.isEmpty(),qPrintable(raw.error));auto plan=decodeEstimatedPlan(p_.driver,raw);QVERIFY2(plan.complete,qPrintable(plan.warning+"\n"+QString::fromUtf8(plan.raw)));QVERIFY(!plan.nodes.isEmpty());
        QCOMPARE(run("SELECT COUNT(*) FROM estimated_plan_owned").rows[0][0].toInt(),2);run("DROP TABLE estimated_plan_owned");
    }catch(const std::exception& e){QFAIL(e.what());}}
    void compilerAnalysisKeepsNativeSqlAndFallbackExecution(){try{
        using namespace navidog;QueryService service;QSignalSpy done(&service,&QueryService::completed);QueryExecutionOptions options;options.sessionId="owned-mysql-compiler";
        auto run=[&](QString sql){if(!service.run(p_,sql,{},options)||!done.wait(3000))throw std::runtime_error("owned MySQL compiler query timeout");return qvariant_cast<QueryOutcome>(done.takeFirst()[0]);};
        auto result=run("WITH q AS(SELECT 7 AS n) SELECT CASE WHEN n=7 THEN n+1 ELSE 0 END, (SELECT max(x.n) FROM q x WHERE x.n=q.n) FROM q");
        QVERIFY2(result.result.error.isEmpty(),qPrintable(result.result.error));QCOMPARE(result.result.rows[0][0].toInt(),8);QCOMPARE(result.result.rows[0][1].toInt(),7);
        QCOMPARE(result.analysis.statements[0].state,SqlAnalysisState::Parsed);QCOMPARE(result.analysis.statements[0].queries,3);
        result=run("WITH q AS(SELECT 7 AS n) SELECT row_number() OVER w FROM q WINDOW w AS(ORDER BY n)");
        QVERIFY2(result.result.error.isEmpty(),qPrintable(result.result.error));QCOMPARE(result.result.rows[0][0].toInt(),1);QCOMPARE(result.analysis.statements[0].state,SqlAnalysisState::Parsed);
        result=run("SELECT '/*literal*/;unchanged'");QCOMPARE(result.result.rows[0][0].toString(),QString("/*literal*/;unchanged"));
    }catch(const std::exception& e){QFAIL(e.what());}}
    void metadataBulkCollectionAndRoutineValidation(){try{
        using namespace navidog;using namespace navidog::sql::compiler;auto db=connectDatabase(p_);auto token=cancellation();auto run=[&](const QString& sql){auto r=db->execute(sql,{},token);if(!r.error.isEmpty())throw std::runtime_error(r.error.toStdString());};
        run("CREATE TABLE metadata_mysql_owned(Id int,note varchar(30))");run("CREATE FUNCTION metadata_calc(a int,b int) RETURNS int DETERMINISTIC RETURN a+b");run("CREATE PROCEDURE metadata_proc(IN a int,OUT b int) SET b=a+1");
        auto snapshot=collectMetadata(*db,p_,{},token);QVERIFY(snapshot.tablesComplete);QVERIFY(snapshot.columnsComplete);QVERIFY(snapshot.routinesComplete);MetadataCatalog catalog(std::make_shared<const MetadataSnapshot>(snapshot));NameResolutionContext context{p_.database.toStdString(),{},Dialect::MySQL};
        auto c=compile(Dialect::MySQL,"SELECT id,note FROM metadata_mysql_owned",{},&catalog,context);QVERIFY2(c->ok(),c->diagnostics.empty()?"missing diagnostic":c->diagnostics[0].message.c_str());
        c=compile(Dialect::MySQL,"SELECT metadata_calc(1,2)",{},&catalog,context);QVERIFY(c->ok());c=compile(Dialect::MySQL,"SELECT metadata_calc(1)",{},&catalog,context);QVERIFY(!c->ok());QCOMPARE(c->diagnostics[0].code,DiagnosticCode::RoutineArgumentCount);
        c=compile(Dialect::MySQL,"CALL metadata_proc(1,@out)",{},&catalog,context);QVERIFY2(c->ok(),c->diagnostics.empty()?"missing diagnostic":c->diagnostics[0].message.c_str());c=compile(Dialect::MySQL,"CALL metadata_proc(1)",{},&catalog,context);QVERIFY(!c->ok());
        run("DROP PROCEDURE metadata_proc");run("DROP FUNCTION metadata_calc");run("DROP TABLE metadata_mysql_owned");
    }catch(const std::exception& e){QFAIL(e.what());}}
    void triggerDefinitionsAndRowScopesMatchOwnedMysql(){try{
        using namespace navidog;
        using namespace navidog::sql::compiler;
        if(p_.driver!=DatabaseKind::MySQL)QSKIP("This test uses the owned MySQL server");
        auto db=connectDatabase(p_);
        auto token=cancellation();
        const auto run=[&](const QString& sql) {
            const auto result=db->execute(sql,{},token);
            if(!result.error.isEmpty())throw std::runtime_error(result.error.toStdString());
            return result;
        };
        run("CREATE TABLE owned_trigger_target(id INT PRIMARY KEY, qty INT)");
        run("CREATE TABLE owned_trigger_log(id INT, qty INT, old_qty INT)");
        const std::string clamp=
            "CREATE DEFINER = CURRENT_USER() TRIGGER owned_trigger_clamp BEFORE INSERT "
            "ON owned_trigger_target FOR EACH ROW "
            "SET NEW.qty = CASE WHEN NEW.qty < 0 THEN 0 ELSE NEW.qty END;";
        const auto in=compile(Dialect::MySQL,clamp);
        QVERIFY2(in->ok(),in->diagnostics.empty()?"no model":in->diagnostics.front().message.c_str());
        const auto normalized=generateSql(in->model,Dialect::MySQL);
        QVERIFY2(normalized.ok(),normalized.diagnostics.empty()?"no SQL":normalized.diagnostics.front().message.c_str());
        auto definition=db->executeDefinition(QString::fromStdString(normalized.sql),{},token);
        QVERIFY2(definition.error.isEmpty(),qPrintable(definition.error));

        auto snapshot=std::make_shared<const MetadataSnapshot>(collectMetadata(*db,p_,{},token));
        MetadataCatalog catalog(snapshot);
        const NameResolutionContext names{p_.database.toStdString(),{},Dialect::MySQL};
        const std::string compound=
            "DELIMITER $$\nCREATE TRIGGER owned_trigger_log_insert BEFORE INSERT ON owned_trigger_target "
            "FOR EACH ROW FOLLOWS owned_trigger_clamp BEGIN\n"
            "SET NEW.qty = NEW.qty + 1;\n"
            "INSERT INTO owned_trigger_log VALUES (NEW.id, NEW.qty, NULL);\nEND$$\nDELIMITER ;\n";
        const auto checked=checkSqlScript(Dialect::MySQL,compound,{},&catalog,names);
        QVERIFY2(checked.ok(),checked.diagnostics.empty()?"no trigger script":checked.diagnostics.front().message.c_str());
        QCOMPARE(checked.statements,std::size_t(1));
        auto emitted=convertSql(Dialect::MySQL,Dialect::MySQL,compound);
        QVERIFY2(emitted.ok(),emitted.diagnostics.empty()?"no emitted trigger":emitted.diagnostics.front().message.c_str());
        auto formatted=formatSql(Dialect::MySQL,emitted.sql);
        QVERIFY2(formatted.ok(),formatted.diagnostics.empty()?"no formatted trigger":formatted.diagnostics.front().message.c_str());
        QueryService service;
        QSignalSpy done(&service,&QueryService::completed);
        QueryExecutionOptions options;
        options.sessionId="owned-trigger-run";
        const auto script=QString::fromStdString(formatted.sql+"\nINSERT INTO owned_trigger_target VALUES (1,-50),(2,8);");
        QVERIFY(service.run(p_,script,{},options));
        QTRY_COMPARE_WITH_TIMEOUT(done.count(),1,5000);
        const auto outcome=done.takeFirst().front().value<QueryOutcome>();
        QVERIFY2(outcome.result.error.isEmpty(),qPrintable(outcome.result.error));
        QCOMPARE(outcome.executedStatements,2);
        QCOMPARE(outcome.analysis.statements.front().kind,QString("TRIGGER"));
        QCOMPARE(outcome.analysis.statements.front().state,SqlAnalysisState::Parsed);
        QCOMPARE(run("SELECT id,qty FROM owned_trigger_target ORDER BY id").rows,
                 run("SELECT id,qty FROM owned_trigger_log ORDER BY id").rows);
        const auto actual=run("SELECT qty FROM owned_trigger_target ORDER BY id").rows;
        QCOMPARE(actual[0][0].toInt(),1);
        QCOMPARE(actual[1][0].toInt(),9);
        const auto native=run("SHOW CREATE TRIGGER owned_trigger_clamp").rows.front()[2].toString();
        auto fromServer=compile(Dialect::MySQL,native.toStdString());
        QVERIFY2(fromServer->ok(),fromServer->diagnostics.empty()?"no server trigger AST":fromServer->diagnostics.front().message.c_str());
        auto invalid=compile(Dialect::MySQL,
            "CREATE TRIGGER invalid_owned BEFORE INSERT ON owned_trigger_target FOR EACH ROW SET NEW.unknown_qty=0;",
            {},&catalog,names);
        QVERIFY(std::any_of(invalid->diagnostics.begin(),invalid->diagnostics.end(),[](const auto& entry) {
            return entry.code==DiagnosticCode::UnknownColumn;
        }));
        run("DROP TRIGGER owned_trigger_log_insert");
        run("DROP TRIGGER owned_trigger_clamp");
        run("DROP TABLE owned_trigger_target, owned_trigger_log");
    }catch(const std::exception& e){QFAIL(e.what());}}
    void filteredCountsConvertedFromPostgresKeepValuesAndWidthInMysql(){try{
        using namespace navidog;
        using namespace navidog::sql::compiler;
        if(p_.driver!=DatabaseKind::MySQL)QSKIP("This lowering is verified for the MySQL frontend");
        auto db=connectDatabase(p_);
        auto token=cancellation();
        const auto run=[&](const QString& sql) {
            const auto result=db->execute(sql,{},token);
            if(!result.error.isEmpty())throw std::runtime_error(result.error.toStdString());
            return result;
        };
        run("CREATE TABLE owned_count_filter(p INT, v BIGINT)");
        run("INSERT INTO owned_count_filter VALUES (1,7),(1,NULL),(NULL,9),(NULL,NULL),(0,10)");
        const std::string source=
            "SELECT COUNT(*) FILTER (WHERE p IS NOT NULL) AS rows_matched, "
            "COUNT(v) FILTER (WHERE p IS NULL) AS null_keys, "
            "COUNT(v) FILTER (WHERE p IS NOT NULL) AS values_matched FROM owned_count_filter";
        const auto generate=[&](const std::string& text){
            auto result=convertSql(Dialect::PostgreSQL,Dialect::MySQL,text);
            if(!result.ok())throw std::runtime_error(result.diagnostics.front().message);
            return QString::fromStdString(result.sql);
        };
        const auto generated=generate(source);
        const auto actual=run(generated).rows.front();
        QCOMPARE(actual.size(),3);
        QCOMPARE(actual[0].toLongLong(),3);
        QCOMPARE(actual[1].toLongLong(),1);
        QCOMPARE(actual[2].toLongLong(),2);
        const auto empty=run(generate(source+" WHERE 1=0")).rows.front();
        for(const auto& cell:empty)QCOMPARE(cell.toLongLong(),0);
        run("CREATE TEMPORARY TABLE temp_owned_count_filter AS "+generated);
        const auto fields=run("SHOW COLUMNS FROM temp_owned_count_filter").rows;
        QCOMPARE(fields.size(),3);
        for(const auto& field:fields)QCOMPARE(field[1].toString(),QString("bigint"));
    }catch(const std::exception& e){QFAIL(e.what());}}
    void generatedSqlMatchesOwnedMysql(){try{
        using namespace navidog;using namespace navidog::sql::compiler;auto db=connectDatabase(p_);auto token=cancellation();
        auto run=[&](const std::string& sql){auto c=compile(Dialect::MySQL,sql);if(!c->ok())throw std::runtime_error(c->diagnostics.front().message);auto generated=generateSql(c->model,Dialect::MySQL);if(!generated.ok())throw std::runtime_error(generated.diagnostics.front().message);auto result=db->execute(QString::fromStdString(generated.sql),{},token);if(!result.error.isEmpty())throw std::runtime_error(result.error.toStdString());return result;};
        run("CREATE TABLE gen_items(id INTEGER PRIMARY KEY,title VARCHAR(60) NOT NULL,n DECIMAL(10,2) DEFAULT 0,CHECK(n>=0))");
        run("INSERT INTO gen_items(id,title) VALUES(1,'one'),(2,'two')");run("UPDATE gen_items SET n=id+1 WHERE id=1");
        run("CREATE VIEW gen_view AS SELECT id,title,n FROM gen_items");run("CREATE INDEX gen_ix ON gen_items(title)");
        auto r=run("SELECT id,n FROM gen_view WHERE id=1");QCOMPARE(r.rows[0][1].toDouble(),2.0);run("DROP VIEW gen_view");
        run("DROP INDEX gen_ix ON gen_items");
        run("ALTER TABLE gen_items ADD extra INTEGER DEFAULT 5");r=run("SELECT extra FROM gen_items ORDER BY id");QCOMPARE(r.rows[0][0].toInt(),5);
        auto start=db->execute("START TRANSACTION",{},token);QVERIFY(start.error.isEmpty());run("TRUNCATE TABLE gen_items");auto rollback=db->execute("ROLLBACK",{},token);QVERIFY(rollback.error.isEmpty());
        r=run("SELECT count(*) FROM gen_items");QCOMPARE(r.rows[0][0].toInt(),0);
        run("DROP TABLE gen_items");
    }catch(const std::exception& e){QFAIL(e.what());}}
    void assignmentEvaluationOrderIsVerifiedAndUnsafeTranslationRefused() {try {
        using namespace navidog;using namespace navidog::sql::compiler;auto db=connectDatabase(p_);auto token=cancellation();
        auto run=[&](const QString &sql){auto r=db->execute(sql,{},token);if(!r.error.isEmpty())throw std::runtime_error(r.error.toStdString());return r;};
        run("CREATE TEMPORARY TABLE assignment_owned(a INTEGER,b INTEGER)");run("INSERT INTO assignment_owned VALUES(1,0)");
        const auto statement="UPDATE assignment_owned SET a=a+1,b=a;";
        auto translated=convertSql(Dialect::MySQL,Dialect::PostgreSQL,statement);QVERIFY(!translated.ok() && translated.sql.empty());
        run(statement);auto row=run("SELECT a,b FROM assignment_owned");QCOMPARE(row.rows[0][0].toInt(),2);QCOMPARE(row.rows[0][1].toInt(),2);
        run("INSERT INTO assignment_owned VALUES(3,4)");
        auto having=run("SELECT 1 FROM assignment_owned HAVING TRUE");QCOMPARE(having.rows.size(),2);
    }catch(const std::exception &e){QFAIL(e.what());}}
    void dmlCompilerMatchesNativeMysqlExecution(){try{
        using namespace navidog;QueryService service;QSignalSpy done(&service,&QueryService::completed);QueryExecutionOptions options;options.sessionId="owned-mysql-dml";
        auto run=[&](QString sql){if(!service.run(p_,sql,{},options)||!done.wait(3000))throw std::runtime_error("owned MySQL DML timeout");return qvariant_cast<QueryOutcome>(done.takeFirst()[0]);};
        auto r=run("CREATE TEMPORARY TABLE dml_items(id int primary key,title varchar(32));CREATE TEMPORARY TABLE dml_other(id int primary key,title varchar(32))");QVERIFY(r.result.error.isEmpty());
        for(auto sql:{"INSERT INTO dml_other VALUES(1,'one'),(2,'two')", "INSERT INTO dml_items SELECT id,title FROM dml_other", "UPDATE dml_items i JOIN dml_other o ON i.id=o.id SET i.title=o.title", "INSERT INTO dml_items VALUES(1,'changed') ON DUPLICATE KEY UPDATE title=VALUES(title)", "DELETE i FROM dml_items i JOIN dml_other o ON i.id=o.id WHERE o.id=2", "SELECT id FROM dml_items NATURAL JOIN dml_other"}) {
            r=run(sql);QVERIFY2(r.result.error.isEmpty(),qPrintable(r.result.error));QCOMPARE(r.analysis.statements[0].state,SqlAnalysisState::Parsed);QVERIFY(!r.analysis.statements[0].unifiedAst.isEmpty());
        }
        r=run("SELECT id,title FROM dml_items");QCOMPARE(r.result.rows.size(),1);QCOMPARE(r.result.rows[0][0].toInt(),1);QCOMPARE(r.result.rows[0][1].toString(),QString("changed"));
    }catch(const std::exception& e){QFAIL(e.what());}}
    void queryTypesAreNativeAndAvailableWithoutRows(){try{
        auto db=navidog::connectDatabase(p_);auto result=db->execute("SELECT CAST(NULL AS DECIMAL(10,2)),CAST(42 AS UNSIGNED),CAST(NULL AS CHAR) WHERE 0",{},navidog::cancellation());QVERIFY(result.rows.isEmpty());QCOMPARE(result.columnTypes.size(),3);QCOMPARE(result.columnTypes[0],QString("DECIMAL"));QVERIFY(result.columnTypes[1].contains("UNSIGNED"));QVERIFY(result.columnTypes[2]=="VARCHAR" || result.columnTypes[2]=="CHAR");
        result=db->executeBound("SELECT CAST(? AS UNSIGNED)",{42},{},navidog::cancellation());QCOMPARE(result.rows[0][0].toInt(),42);QVERIFY(result.columnTypes[0].contains("UNSIGNED"));
    }catch(const std::exception& e){QFAIL(e.what());}}
    void defaultsCompositeKeysAndCheckDropPreserveData(){try{
        using namespace navidog;auto db=connectDatabase(p_);auto token=cancellation();db->setAutoCommit(true);db->execute("CREATE TABLE defaults_owned(a INT NOT NULL,b INT NOT NULL,label VARCHAR(40) DEFAULT 'a''b',computed_default VARCHAR(10) DEFAULT (concat('x','y')),PRIMARY KEY(b,a),CONSTRAINT defaults_check_owned CHECK(a>0)) ENGINE=InnoDB",{},token);
        CatalogNode node;node.kind="table";node.database=p_.database;node.object=node.name="defaults_owned";auto before=readTableDefinition(*db,p_,node,token);auto keys=primaryKeyColumns(before);QCOMPARE(keys,QVector<int>({1,0}));auto after=before;after.columns[2].type="VARCHAR(80)";after.columns[3].type="VARCHAR(20)";after.objects.erase(std::remove_if(after.objects.begin(),after.objects.end(),[](const auto& o){return o.kind==TableObjectKind::Check;}),after.objects.end());
        auto result=applyTableChanges(p_,before,after,token);QVERIFY2(result.error.isEmpty(),qPrintable(result.error+" | owned default: "+before.columns[3].defaultExpression));QVERIFY2(result.refreshError.isEmpty(),qPrintable(result.refreshError));db->execute("INSERT INTO defaults_owned(a,b) VALUES(1,2)",{},token);auto values=db->execute("SELECT label,computed_default FROM defaults_owned",{},token);QCOMPARE(values.rows[0][0].toString(),QString("a'b"));QCOMPARE(values.rows[0][1].toString(),QString("xy"));QCOMPARE(primaryKeyColumns(result.definition),QVector<int>({1,0}));
    }catch(const std::exception& e){QFAIL(e.what());}}
    void databaseCreationAndViewFunctionDesignUseNativeMetadata(){try{
        using namespace navidog;auto token=cancellation();auto options=loadNamespaceOptions(p_,NamespaceKind::Database,token);QVERIFY2(options.error.isEmpty(),qPrintable(options.error));QVERIFY(options.charsets.contains("utf8mb4"));NamespaceDraft draft;draft.name="namespace_owned";draft.charset="utf8mb4";auto created=createNamespace(p_,draft,token);QVERIFY2(created.applied,qPrintable(created.error));auto other=p_;other.database=draft.name;auto fresh=connectDatabase(other);QCOMPARE(fresh->execute("SELECT DATABASE()",{},token).rows[0][0].toString(),draft.name);fresh.reset();
        const auto removed=dropNamespace(other,NamespaceKind::Database,draft.name,token);
        QVERIFY2(removed.applied,qPrintable(removed.error));
        auto db=connectDatabase(p_);
        auto count=db->execute("SELECT COUNT(*) FROM information_schema.schemata WHERE schema_name='namespace_owned'",{},token);
        QVERIFY2(count.error.isEmpty(),qPrintable(count.error));
        QCOMPARE(count.rows.front().front().toInt(),0);
        SourceDefinition empty;CatalogNode node;node.kind="view";node.database=p_.database;node.object=node.name="view_designer_owned";auto result=applySourceChanges(p_,empty,node,sourceTemplate(p_,node),false,token);QVERIFY2(result.applied,qPrintable(result.error));auto before=result.definition;auto sql=before.sql;sql.replace("SELECT 1 AS", "SELECT 7 AS",Qt::CaseInsensitive);result=applySourceChanges(p_,before,node,sql,false,token);QVERIFY2(result.applied,qPrintable(result.error));QCOMPARE(db->execute("SELECT * FROM view_designer_owned",{},token).rows[0][0].toInt(),7);
        node.kind="function";node.object=node.name="function_designer_owned";result=applySourceChanges(p_,empty,node,sourceTemplate(p_,node),false,token);QVERIFY2(result.applied,qPrintable(result.error));QCOMPARE(db->execute("SELECT function_designer_owned(6)",{},token).rows[0][0].toInt(),6);
#ifdef NAVIDOG_MARIADB_TEST
        QCOMPARE(detectVersion(p_.driver,db->serverVersion()),DatabaseVersion::MariaDB11_4);
#endif
    }catch(const std::exception& e){QFAIL(e.what());}}
    void explainPreservesOriginalTransactionsAndNeverRunsDelete(){try{
        using namespace navidog;auto db=connectDatabase(p_);auto token=cancellation();db->setAutoCommit(true);db->execute("CREATE TABLE plan_owned(ID int PRIMARY KEY)",{},token);db->execute("INSERT INTO plan_owned VALUES(1)",{},token);
        QueryService service;QSignalSpy done(&service,&QueryService::completed);QueryExecutionOptions options;options.sessionId="owned-plan-transaction";options.autoCommit=false;auto run=[&](QString text){if(!service.run(p_,text,{},options) || !done.wait(5000))throw std::runtime_error("plan test timed out");return qvariant_cast<QueryOutcome>(done.takeFirst()[0]);};
        auto result=run("INSERT INTO plan_owned VALUES(2)");QVERIFY2(result.result.error.isEmpty(),qPrintable(result.result.error));QCOMPARE(result.transactionState,TransactionState::Active);options.explain=true;result=run("DELETE FROM plan_owned WHERE ID=1");QVERIFY(result.explained);QVERIFY2(result.result.error.isEmpty(),qPrintable(result.result.error));QVERIFY(!result.result.rows.isEmpty());QCOMPARE(result.transactionState,TransactionState::Active);QCOMPARE(db->execute("SELECT count(*) FROM plan_owned",{},token).rows[0][0].toInt(),1);
        options.explain=false;options.transaction=TransactionAction::Rollback;result=run({});QCOMPARE(result.transactionState,TransactionState::Idle);QCOMPARE(db->execute("SELECT count(*) FROM plan_owned",{},token).rows[0][0].toInt(),1);
    }catch(const std::exception& e){QFAIL(e.what());}}
    void parallelQuerySessionsKeepStopsAndTransactionsSeparate(){
        using namespace navidog;QueryService service;QSignalSpy done(&service,&QueryService::completed);QueryExecutionOptions slow,other;slow.sessionId="owned-mysql-slow";other.sessionId="owned-mysql-other";
        QVERIFY(service.run(p_,"SELECT SLEEP(20)",{},slow));QVERIFY(service.run(p_,"SELECT SLEEP(0.3), 43",{},other));QTimer::singleShot(50,&service,[&]{service.cancelQuery(p_.id,p_.database,slow.sessionId);});
        QTRY_COMPARE_WITH_TIMEOUT(done.count(),2,4000);bool stopped=false,success=false;for(auto event:done){auto result=qvariant_cast<QueryOutcome>(event[0]);if(result.sessionId==slow.sessionId){QVERIFY(result.result.cancelled);QVERIFY(result.sessionClosed);stopped=true;}else{QVERIFY2(result.result.error.isEmpty(),qPrintable(result.result.error));QVERIFY(!result.result.cancelled);QCOMPARE(result.result.rows[0][1].toInt(),43);success=true;}}QVERIFY(stopped && success);QCOMPARE(service.sessionCount(),1);
    }
    void fullDesignerRoundTrip(){
        using namespace navidog;auto db=connectDatabase(p_);auto cancel=cancellation();auto run=[&](QString text){auto r=db->executeDefinition(text,{},cancel);if(!r.error.isEmpty())throw std::runtime_error(r.error.toStdString());};
        run("CREATE TABLE parent_owned(id INT PRIMARY KEY)");run("INSERT INTO parent_owned VALUES(1)");run("CREATE TABLE audit_owned(note TEXT)");
        TableDefinition empty,d;d.node.kind="table";d.node.database=p_.database;d.node.object="design_owned";d.engine="InnoDB";d.charset="utf8mb4";d.collation="utf8mb4_bin";d.comment="表'\\注释";
        TableColumn id;id.name="id";id.type="INT";id.primaryKey=1;id.nullable=false;id.autoIncrement=true;id.comment="编号'\\注释";d.columns<<id;TableColumn parent;parent.name="parent_id";parent.type="INT";d.columns<<parent;
        TableObject fk;fk.kind=TableObjectKind::ForeignKey;fk.name="fk_owned";fk.columns={"parent_id"};fk.referenceSchema=p_.database;fk.referenceTable="parent_owned";fk.referenceColumns={"id"};d.objects<<fk;
        TableObject idx;idx.name="idx_owned";idx.expression="parent_id";idx.comment="索引'\\注释";d.objects<<idx;
        TableObject check;check.kind=TableObjectKind::Check;check.name="check_owned";check.expression="parent_id>=0";d.objects<<check;
        TableObject trigger;trigger.kind=TableObjectKind::Trigger;trigger.name="trigger_owned";trigger.sql="CREATE TRIGGER navidog_owned.trigger_owned AFTER INSERT ON navidog_owned.design_owned FOR EACH ROW BEGIN INSERT INTO navidog_owned.audit_owned VALUES('ok'); END";d.objects<<trigger;
        auto r=applyTableChanges(p_,empty,d,cancel);QVERIFY2(r.error.isEmpty(),qPrintable(r.error));QCOMPARE(r.definition.comment,d.comment);QCOMPARE(r.definition.columns[0].comment,id.comment);QCOMPARE(r.definition.objects.size(),4);QVERIFY(r.definition.columns[0].autoIncrement);
        run("INSERT INTO design_owned(parent_id) VALUES(1)");QCOMPARE(db->execute("SELECT count(*) FROM audit_owned",{},cancel).rows[0][0].toInt(),1);
        auto before=r.definition,after=before;after.columns[1].name="parent_ref";after.columns[0].comment="新注释";for(auto& o:after.objects){if(o.kind==TableObjectKind::ForeignKey)o.columns={"parent_ref"};if(o.kind==TableObjectKind::Index){o.expression="parent_ref";o.name="idx_renamed_owned";}}
        r=applyTableChanges(p_,before,after,cancel);QVERIFY2(r.error.isEmpty(),qPrintable(r.error));QCOMPARE(r.definition.columns[1].name,QString("parent_ref"));QCOMPARE(r.definition.columns[0].comment,QString("新注释"));
        auto actual=readTableDefinition(*db,p_,r.definition.node,cancel);QVERIFY(sameTableStructure(actual,r.definition));
        before=r.definition;after=before;after.objects.clear();r=applyTableChanges(p_,before,after,cancel);QVERIFY2(r.error.isEmpty(),qPrintable(r.error));QVERIFY(r.definition.objects.isEmpty());
    }
    void nativeBoundEditingAndConflicts(){
        using namespace navidog;auto db=connectDatabase(p_);auto token=cancellation();db->execute("CREATE TABLE edit_owned(id INT AUTO_INCREMENT PRIMARY KEY,label VARCHAR(200) UNIQUE NOT NULL,note TEXT,payload BLOB) ENGINE=InnoDB",{},token);auto text=QString("'); DROP DATABASE navidog_owned; --");auto blob=QByteArray::fromHex("0001ff");auto r=db->executeBound("INSERT INTO edit_owned(label,note,payload) VALUES(?1,?2,?3)",{text,QVariant{},blob},{},token);QCOMPARE(r.affectedRows,1);r=db->executeBound("SELECT label,note,payload FROM edit_owned WHERE id=?1",{1},{},token);QCOMPARE(r.rows[0][0].toString(),text);QVERIFY(!r.rows[0][1].isValid());QCOMPARE(r.rows[0][2].toByteArray(),blob);
        DataPageRequest request;request.node.kind="table";request.node.database=p_.database;request.node.object="edit_owned";auto page=readDataPage(p_,request,token);QVERIFY2(page.error.isEmpty(),qPrintable(page.error));QVERIFY(page.editable);DataRowChange update;update.state=DataRowState::Modified;update.original=update.values=page.result.rows[0];update.values[1]="updated";update.touched={1};auto commit=commitDataChanges(p_,page.schema,{update},token);QVERIFY2(commit.error.isEmpty(),qPrintable(commit.error));QCOMPARE(commit.updated,1);
        page=readDataPage(p_,request,token);update.original=update.values=page.result.rows[0];update.values[1]="stale";db->execute("UPDATE edit_owned SET note='external' WHERE id=1",{},token);commit=commitDataChanges(p_,page.schema,{update},token);QVERIFY(commit.rolledBack);QVERIFY(commit.error.contains("其他会话"));
        page=readDataPage(p_,request,token);update.original=update.values=page.result.rows[0];update.values[1]="should rollback";DataRowChange insert;insert.state=DataRowState::Inserted;insert.values.resize(4);insert.values[0]=1;insert.values[1]="duplicate";insert.touched={0,1};commit=commitDataChanges(p_,page.schema,{update,insert},token);QVERIFY(commit.rolledBack);QVERIFY(!commit.error.isEmpty());r=db->execute("SELECT label FROM edit_owned WHERE id=1",{},token);QCOMPARE(r.rows[0][0].toString(),QString("updated"));
    }
    void nativeProcedureResultsAndInterruptibleQuery(){
        using namespace navidog;auto p=p_;p.id="owned-mysql-query";QueryService service;QSignalSpy done(&service,&QueryService::completed);QString script="DELIMITER $$\nCREATE PROCEDURE navidog_owned.multi_owned() BEGIN SELECT 11 AS first; SELECT 22 AS second; END$$\nDELIMITER ;\nCALL navidog_owned.multi_owned();";QVERIFY(service.run(p,script));QVERIFY(done.wait(5000));auto outcome=qvariant_cast<QueryOutcome>(done.takeFirst()[0]);QVERIFY2(outcome.result.error.isEmpty(),qPrintable(outcome.result.error));QCOMPARE(outcome.executedStatements,2);QVERIFY(outcome.results.size()>=3);QCOMPARE(outcome.results[1].rows[0][0].toInt(),11);QCOMPARE(outcome.results[2].rows[0][0].toInt(),22);
        QVERIFY(service.run(p,"SELECT 33"));QVERIFY(done.wait(3000));QCOMPARE(qvariant_cast<QueryOutcome>(done.takeFirst()[0]).result.rows[0][0].toInt(),33);
        QVERIFY(service.run(p,"SELECT SLEEP(30)"));QTimer::singleShot(40,&service,&QueryService::cancel);QVERIFY(done.wait(2500));outcome=qvariant_cast<QueryOutcome>(done.takeFirst()[0]);QVERIFY(outcome.result.cancelled);QVERIFY(outcome.sessionClosed);QCOMPARE(service.sessionCount(),0);
    }
    void routineDesignerAcceptsClientDelimitedFunction() {
        using namespace navidog;
        auto db = connectDatabase(p_);
        auto cancel = cancellation();
        auto result = db->execute("CREATE TABLE designer_activity_owned(id INT)", {}, cancel);
        QVERIFY2(result.error.isEmpty(), qPrintable(result.error));
        result = db->execute("INSERT INTO designer_activity_owned VALUES (2),(3)", {}, cancel);
        QVERIFY2(result.error.isEmpty(), qPrintable(result.error));
        CatalogNode node;
        node.database = p_.database;
        node.kind = "function";
        node.object = node.name = "designer_quantity_owned";
        const QString script =
            "-- 中文\n  DELIMITER $$\n"
            "CREATE FUNCTION pasted_name(_id INT) RETURNS INT READS SQL DATA\n"
            "BEGIN\n  IF _id > 0 THEN\n"
            "    RETURN (SELECT SUM(id) + LENGTH('$$') - 2 "
            "FROM designer_activity_owned WHERE id = _id);\n"
            "  ELSE\n    RETURN (SELECT SUM(id) FROM designer_activity_owned);\n"
            "  END IF;\nEND$$";
        const auto sql = rewriteSourceTarget(script, p_, node);
        QVERIFY(sql.startsWith("-- 中文\n  DELIMITER $$"));
        QVERIFY(sql.endsWith("END$$"));
        QVERIFY(sql.contains("LENGTH('$$')"));
        auto saved = applySourceChanges(p_, {}, node, sql + "\nDELIMITER ;\nSELECT 1;", false, cancel);
        QVERIFY(!saved.applied);
        QVERIFY2(saved.error.contains("一次只保存一个"), qPrintable(saved.error));
        saved = applySourceChanges(p_, {}, node, sql, false, cancel);
        QVERIFY2(saved.applied, qPrintable(saved.error));
        QVERIFY2(saved.refreshError.isEmpty(), qPrintable(saved.refreshError));
        result = db->execute("SELECT designer_quantity_owned(2), designer_quantity_owned(0)", {}, cancel);
        QVERIFY2(result.error.isEmpty(), qPrintable(result.error));
        QCOMPARE(result.rows.at(0).at(0).toInt(), 2);
        QCOMPARE(result.rows.at(0).at(1).toInt(), 5);
    }
    void cursorRoutineRoundTripsAndUsesNativeExecution() {
        using namespace navidog;
        namespace sc = navidog::sql::compiler;
        auto db = connectDatabase(p_);
        auto token = cancellation();
        const auto dialect = p_.driver == DatabaseKind::MariaDB ? sc::Dialect::MariaDB : sc::Dialect::MySQL;
        const auto dialectView = navidog::sql::Dialect::MySQL;
        auto exec = [&](const QString& text) {
            return db->execute(text, {}, token);
        };
        exec("CREATE TABLE cursor_source_owned(id INT PRIMARY KEY, amount INT)");
        exec("INSERT INTO cursor_source_owned VALUES (1,2),(2,3),(3,5)");
        const std::string source = R"SQL(
CREATE PROCEDURE cursor_sum_owned(IN low_id INT, OUT total INT) READS SQL DATA
BEGIN
  DECLARE item_id, item_amount INT;
  DECLARE done INT DEFAULT 0;
  DECLARE rows_cursor CURSOR FOR
    SELECT id, amount FROM cursor_source_owned WHERE id >= low_id ORDER BY id;
  DECLARE CONTINUE HANDLER FOR NOT FOUND SET done = 1;
  SET total = 0;
  OPEN rows_cursor;
  sum_rows: LOOP
    FETCH NEXT FROM rows_cursor INTO item_id, item_amount;
    IF done THEN LEAVE sum_rows; END IF;
    SET total = total + item_amount;
  END LOOP;
  CLOSE rows_cursor;
END;
)SQL";
        auto compile = sc::compileRoutine(dialect, source);
        QVERIFY2(compile.ok(), compile.diagnostics.empty() ? "" : compile.diagnostics.front().message.c_str());
        QVERIFY(compile.runtimeDiagnostics.empty());
        const auto generated = sc::convertSql(dialect, dialect, source);
        QVERIFY2(generated.ok(), generated.diagnostics.empty() ? "" : generated.diagnostics.front().message.c_str());
        const auto spans = navidog::sql::splitStatements(generated.sql, dialectView);
        QCOMPARE(spans.size(), std::size_t(1));
        const auto definition = navidog::sql::statementSource(generated.sql, spans.front(), dialectView);
        for (const auto& body : {source, std::string(definition)}) {
            const auto created = db->executeDefinition(QString::fromStdString(body), {}, token);
            QVERIFY2(created.error.isEmpty(), qPrintable(created.error));
            exec("CALL cursor_sum_owned(2,@cursor_total)");
            QCOMPARE(exec("SELECT @cursor_total").rows.at(0).at(0).toInt(), 8);
            exec("CALL cursor_sum_owned(9,@cursor_total)");
            QCOMPARE(exec("SELECT @cursor_total").rows.at(0).at(0).toInt(), 0);
            exec("DROP PROCEDURE cursor_sum_owned");
        }
        const std::vector<std::pair<std::string, std::string>> failures = {
            {"OPEN c; FETCH c INTO x;", "Incorrect number of FETCH variables"},
            {"FETCH c INTO x, y;", "Cursor is not open"},
            {"CLOSE c;", "Cursor is not open"},
            {"OPEN c; OPEN c;", "Cursor is already open"}
        };
        for (const auto& [body, reason] : failures) {
            const std::string definition = "CREATE PROCEDURE cursor_failure_owned() BEGIN "
                "DECLARE x,y INT; DECLARE c CURSOR FOR SELECT 1,2; " + body + " END;";
            const auto checked = sc::compileRoutine(dialect, definition);
            QVERIFY(checked.ok());
            QCOMPARE(checked.runtimeDiagnostics.size(), std::size_t(1));
            const auto created = db->executeDefinition(QString::fromStdString(definition), {}, token);
            QVERIFY2(created.error.isEmpty(), qPrintable(created.error));
            QString error;
            try {
                error = exec("CALL cursor_failure_owned()").error;
            } catch (const std::exception& e) {
                error = QString::fromUtf8(e.what());
            }
            QVERIFY2(error.contains(QString::fromStdString(reason), Qt::CaseInsensitive), qPrintable(error));
            exec("DROP PROCEDURE cursor_failure_owned");
        }
    }
    void routineDesignerRecreatesRestoresAndPreservesGrants(){
        using namespace navidog;auto db=connectDatabase(p_);auto token=cancellation();CatalogNode node;node.kind="procedure";node.database=p_.database;node.object=node.name="source_owned";SourceDefinition empty;auto result=applySourceChanges(p_,empty,node,sourceTemplate(p_,node),false,token);QVERIFY2(result.applied,qPrintable(result.error));QVERIFY2(result.refreshError.isEmpty(),qPrintable(result.refreshError));QCOMPARE(result.definition.parameters.size(),1);
        auto password=QUuid::createUuid().toString(QUuid::Id128);db->execute("CREATE USER 'owned_source_grantee'@'localhost' IDENTIFIED BY '"+password+"'",{},token);db->execute("GRANT EXECUTE ON PROCEDURE navidog_owned.source_owned TO 'owned_source_grantee'@'localhost'",{},token);auto before=result.definition;auto sql=before.sql;sql.replace("SELECT p_value AS result","SELECT p_value+1 AS result");result=applySourceChanges(p_,before,node,sql,false,token);QVERIFY2(result.applied,qPrintable(result.error));auto r=db->execute("CALL navidog_owned.source_owned(8)",{},token);QCOMPARE(r.rows[0][0].toInt(),9);auto grants=db->execute("SELECT Proc_priv FROM mysql.procs_priv WHERE Db='navidog_owned' AND Routine_name='source_owned' AND User='owned_source_grantee'",{},token);QCOMPARE(grants.rows.size(),1);QVERIFY(grants.rows[0][0].toString().contains("Execute",Qt::CaseInsensitive));
        before=result.definition;sql=before.sql;sql.replace("SELECT p_value+1 AS result","SELECT FROM missing");result=applySourceChanges(p_,before,node,sql,false,token);QVERIFY(!result.applied);QVERIFY2(result.restored,qPrintable(result.warning+result.error));r=db->execute("CALL navidog_owned.source_owned(8)",{},token);QCOMPARE(r.rows[0][0].toInt(),9);result=applySourceChanges(p_,before,node,{},true,token);QVERIFY2(result.deleted,qPrintable(result.error));
    }
    void sshNativeRoundTripAndHostVerification(){
        using namespace navidog;if(!sshAvailable())QSKIP("SSH adapter unavailable");auto python=qEnvironmentVariable("NAVIDOG_TEST_SSH_PYTHON");if(python.isEmpty())QSKIP("Set NAVIDOG_TEST_SSH_PYTHON to a Python with Paramiko");
        QProcess ssh;auto env=QProcessEnvironment::systemEnvironment();auto password=QUuid::createUuid().toString();env.insert("NAVIDOG_TEST_SSH_PASSWORD",password);ssh.setProcessEnvironment(env);ssh.start(python,{QString(NAVIDOG_SOURCE_DIR)+"/tests/fixtures/ssh_forward_server.py",dir_.path(),QString::number(p_.port)});QVERIFY(ssh.waitForStarted());QVERIFY(ssh.waitForReadyRead(10000));auto fixture=QJsonDocument::fromJson(ssh.readLine()).object();QVERIFY(fixture.contains("hostKey"));auto p=p_;p.ssh.enabled=true;p.ssh.host="127.0.0.1";p.ssh.port=fixture["port"].toInt();p.ssh.user="owned";p.ssh.password=password;p.ssh.knownHostsFile=dir_.filePath("owned-empty-known-hosts");
        bool unknown=false;try{auto db=connectDatabase(p);}catch(const SshHostKeyError& e){unknown=true;QVERIFY(!e.mismatch);QVERIFY(e.fingerprint.startsWith("SHA256:"));QCOMPARE(e.publicKey,fixture["hostKey"].toString());}QVERIFY(unknown);
        p.ssh.hostKey=fixture["hostKey"].toString();{auto db=connectDatabase(p);auto r=db->execute("SELECT 42, '隧道'",{},cancellation());QCOMPARE(r.rows.size(),1);QCOMPARE(r.rows[0][0].toInt(),42);QCOMPARE(r.rows[0][1].toString(),QString("隧道"));}
        p.ssh.authentication=SshAuthentication::PublicKey;p.ssh.keyFile=fixture["keyFile"].toString();p.ssh.password.clear();{auto db=connectDatabase(p);auto r=db->execute("SELECT 43",{},cancellation());QCOMPARE(r.rows[0][0].toInt(),43);}
        p.ssh.hostKey="ssh-rsa changed-owned-key";bool mismatch=false;try{auto db=connectDatabase(p);}catch(const SshHostKeyError& e){mismatch=e.mismatch;}QVERIFY(mismatch);ssh.terminate();QVERIFY(ssh.waitForFinished(5000));
    }
    void httpNativePdoRoundTrip(){
        using namespace navidog;auto php=qEnvironmentVariable("NAVIDOG_TEST_PHP");if(php.isEmpty())QSKIP("Set NAVIDOG_TEST_PHP for disposable PDO bridge test");
        auto path=dir_.filePath("navidog-tunnel.php");QFile script(path);QVERIFY(script.open(QIODevice::WriteOnly));script.write(httpTunnelScript());script.close();QTcpServer reserve;QVERIFY(reserve.listen(QHostAddress::LocalHost,0));auto port=reserve.serverPort();reserve.close();
        QProcess server;auto env=QProcessEnvironment::systemEnvironment();auto password=QUuid::createUuid().toString();env.insert("NAVIDOG_TUNNEL_USER","owned");env.insert("NAVIDOG_TUNNEL_PASSWORD",password);env.insert("NAVIDOG_DB_HOSTS","127.0.0.1");server.setProcessEnvironment(env);server.start(php,{"-S","127.0.0.1:"+QString::number(port),"-t",dir_.path()});QVERIFY(server.waitForStarted());QTest::qWait(150);
        auto p=p_;p.http.enabled=true;p.http.url="http://127.0.0.1:"+QString::number(port)+"/navidog-tunnel.php";p.http.authentication=true;p.http.user="owned";p.http.password=password;p.http.base64=true;
        {auto db=connectDatabase(p);auto r=db->execute("SELECT 42, '桥接'",{},cancellation());QVERIFY(r.error.isEmpty());QCOMPARE(r.rows.size(),1);QCOMPARE(r.rows[0][0].toInt(),42);QCOMPARE(r.rows[0][1].toString(),QString("桥接"));auto parameter=QString("'); DROP DATABASE navidog_owned; --");r=db->executeBound("SELECT ?1, ?2",{parameter,QVariant{}},{},cancellation());QCOMPARE(r.rows[0][0].toString(),parameter);QVERIFY(!r.rows[0][1].isValid());QVERIFY_THROWS_EXCEPTION(std::runtime_error,db->execute("BEGIN",{},cancellation()));QueryLimits limits;limits.maxRows=2;r=db->execute("SELECT 1 UNION ALL SELECT 2 UNION ALL SELECT 3",limits,cancellation());QCOMPARE(r.rows.size(),2);QVERIFY(r.truncated);}
        p.http.password="wrong";QVERIFY_THROWS_EXCEPTION(std::runtime_error,connectDatabase(p));server.terminate();QVERIFY(server.waitForFinished(5000));
    }
    void cleanupTestCase(){if(!container_.isEmpty()){QProcess remove;remove.start("docker",{"rm","-f",container_});if(!remove.waitForFinished(15000)){remove.kill();remove.waitForFinished(1000);}}if(server_.state()!=QProcess::NotRunning){server_.terminate();if(!server_.waitForFinished(5000)){server_.kill();server_.waitForFinished();}}p_.password.fill(QChar(0));}
};
QTEST_GUILESS_MAIN(MySqlSchemaTest)
#include "mysql_schema_test.moc"
