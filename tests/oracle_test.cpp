#include <algorithm>
#include <QtTest>
#include <QProcess>
#include <QTemporaryDir>
#include <QTcpServer>
#include <QUuid>
#include <QElapsedTimer>
#include <QDir>
#include "navidog/database.hpp"
#include "navidog/query_service.hpp"
#include "navidog/query_plan.hpp"
#include "navidog/source_design.hpp"
#include "navidog/table_design.hpp"
#include "navidog/data_edit.hpp"
#include "navidog/namespace_design.hpp"
#include "navidog/sql/compiler/compiler.hpp"
#include "navidog/sql/optimizer/optimizer.hpp"
// Starts only a disposable, labelled Oracle container on a random loopback port.
// All passwords are generated in memory and passed over process stdin, never saved.
class OracleTest final:public QObject {
    Q_OBJECT
    QProcess server_;
    QString container_,image_,adminPassword_,bootLog_;
    navidog::ConnectionProfile p_;
    bool sqlPlus(const QString& script,int timeout=8000,bool requireReady=false){
        QProcess process;process.start("docker",{"exec","-i",container_,"/bin/bash","-c","exec \"$ORACLE_HOME/bin/sqlplus\" -s / as sysdba"});if(!process.waitForStarted(3000))return false;
        auto bytes=("WHENEVER OSERROR EXIT FAILURE\nWHENEVER SQLERROR EXIT SQL.SQLCODE\nSET ECHO OFF FEEDBACK OFF VERIFY OFF\n"+script+"\nEXIT;\n").toUtf8();process.write(bytes);bytes.fill('\0');process.closeWriteChannel();if(!process.waitForFinished(timeout)){process.kill();process.waitForFinished(1000);return false;}auto output=QString::fromUtf8(process.readAllStandardOutput()+process.readAllStandardError());const bool ok=process.exitStatus()==QProcess::NormalExit && process.exitCode()==0 && (!requireReady || output.contains("NAVIDOG_FIXTURE_READY"));if(!ok && !requireReady){output.replace(adminPassword_,"[redacted]").replace(p_.password,"[redacted]");output.replace(QRegularExpression("(?im)^.*(?:PASSWORD|IDENTIFIED BY).*$"),"[credential output omitted]");if(output.contains("ORA-"))qInfo().noquote()<<output.right(1800);}return ok;
    }
    QString redactedLog(){auto log=bootLog_+QString::fromUtf8(server_.readAllStandardOutput()+server_.readAllStandardError());log.replace(adminPassword_,"[redacted]");log.replace(p_.password,"[redacted]");log.replace(QRegularExpression("(?im)^.*(?:PASSWORD|IDENTIFIED BY).*$"),"[credential output omitted]");return log.right(4000);}
private slots:
    void initTestCase(){
        using namespace navidog;image_=qEnvironmentVariable("NAVIDOG_TEST_ORACLE_IMAGE");if(image_.isEmpty() || !availableDrivers().contains(DatabaseKind::Oracle))QSKIP("Set NAVIDOG_TEST_ORACLE_IMAGE for a disposable official Oracle image");
        auto id=QUuid::createUuid().toString(QUuid::Id128);container_="navidog-owned-oracle-"+id.left(12);adminPassword_="NavidogA9_"+id.left(16);p_.password="NavidogU9_"+QUuid::createUuid().toString(QUuid::Id128).left(16);p_.id=container_;p_.driver=DatabaseKind::Oracle;p_.host="127.0.0.1";p_.user="NAVIDOG_OWNED";p_.database="FREEPDB1";p_.tls=false;p_.oracle.clientDirectory=qEnvironmentVariable("NAVIDOG_TEST_ORACLE_CLIENT_DIR");p_.advanced.connectTimeout=3;p_.advanced.readTimeout=15;
        QTcpServer reserve;QVERIFY(reserve.listen(QHostAddress::LocalHost,0));p_.port=reserve.serverPort();reserve.close();
        server_.start("docker",{"run","--rm","-i","--name",container_,"--label","app.navidog.test=true","--memory=4g","--cpus=2","--shm-size=1g","-p","127.0.0.1:"+QString::number(p_.port)+":1521",image_,"/bin/bash","-c","IFS= read -r ORACLE_PWD; export ORACLE_PWD; exec \"$ORACLE_BASE/$RUN_FILE\""});QVERIFY(server_.waitForStarted(5000));server_.write(adminPassword_.toUtf8()+"\n");server_.closeWriteChannel();
        QElapsedTimer elapsed;elapsed.start();bool ready=false,entrypointReady=false;int announced=0;
        while(elapsed.elapsed()<240000 && server_.state()!=QProcess::NotRunning){
            bootLog_+=QString::fromUtf8(server_.readAllStandardOutput()+server_.readAllStandardError());entrypointReady|=bootLog_.contains("DATABASE IS READY TO USE!");if(bootLog_.size()>32768)bootLog_=bootLog_.right(32768);
            // The PDB can briefly report READ WRITE while the official startup
            // script is still reopening the instance or resetting credentials.
            if(entrypointReady && sqlPlus("ALTER SESSION SET CONTAINER=FREEPDB1;\nSELECT 'NAVIDOG_FIXTURE_READY' FROM v$pdbs WHERE name='FREEPDB1' AND open_mode='READ WRITE';",4000,true)){ready=true;break;}
            if(elapsed.elapsed()/20000>announced){announced=elapsed.elapsed()/20000;qInfo()<<"Waiting for owned Oracle fixture:"<<elapsed.elapsed()/1000<<"s";}QTest::qWait(500);
        }
        if(!ready)qWarning().noquote()<<redactedLog();QVERIFY2(ready,"Owned Oracle container did not become ready");
        QVERIFY2(sqlPlus("ALTER SESSION SET CONTAINER=FREEPDB1;\nCREATE TABLESPACE NAVIDOG_DATA DATAFILE '/opt/oracle/oradata/navidog-owned-data.dbf' SIZE 20M;\nCREATE USER NAVIDOG_OWNED IDENTIFIED BY \""+p_.password+"\";\nGRANT CREATE SESSION, CREATE TABLE, CREATE VIEW, CREATE PROCEDURE, CREATE TRIGGER, CREATE SEQUENCE, CREATE USER, UNLIMITED TABLESPACE TO NAVIDOG_OWNED;\nCREATE USER NAVIDOG_NAMESPACE_ADMIN IDENTIFIED BY \""+adminPassword_+"\";\nGRANT CREATE USER, CREATE SESSION TO NAVIDOG_NAMESPACE_ADMIN WITH ADMIN OPTION;\nGRANT SELECT ON SYS.DBA_USERS TO NAVIDOG_NAMESPACE_ADMIN;\nGRANT SELECT ON SYS.DBA_TS_QUOTAS TO NAVIDOG_NAMESPACE_ADMIN;\nGRANT SELECT ON SYS.DBA_TABLESPACES TO NAVIDOG_NAMESPACE_ADMIN;"),"Owned Oracle bootstrap failed");qInfo()<<"Owned Oracle fixture ready";
    }
    void rowPatternMatchesPreserveNativeRowsAndTypes(){try{
        using namespace navidog;
        using namespace navidog::sql::compiler;
        using namespace navidog::sql::optimizer;
        auto db=connectDatabase(p_);
        auto token=cancellation();
        db->setAutoCommit(true);
        const auto run=[&](const std::string &sql) {
            const auto output=db->execute(QString::fromStdString(sql),{},token);
            if (!output.error.isEmpty()) throw std::runtime_error(output.error.toStdString());
            return output;
        };
        run("CREATE TABLE ROW_PATTERN_OWNED (ID NUMBER(9) PRIMARY KEY, G NUMBER(9), V NUMBER(9))");
        run("INSERT ALL INTO ROW_PATTERN_OWNED VALUES (1,1,7) INTO ROW_PATTERN_OWNED VALUES (2,1,8) "
            "INTO ROW_PATTERN_OWNED VALUES (3,1,NULL) INTO ROW_PATTERN_OWNED VALUES (4,1,9) "
            "INTO ROW_PATTERN_OWNED VALUES (5,1,-1) INTO ROW_PATTERN_OWNED VALUES (6,2,5) "
            "INTO ROW_PATTERN_OWNED VALUES (7,2,7) INTO ROW_PATTERN_OWNED VALUES (8,2,4) "
            "INTO ROW_PATTERN_OWNED VALUES (9,2,-1) SELECT 1 FROM dual");
        NameResolutionContext names{p_.database.toStdString(),p_.user.toStdString(),Dialect::Oracle};
        MemoryCatalog catalog;
        navidog::sql::compiler::TableDefinition table;
        table.database=names.database;
        table.schema=names.schema;
        table.name="ROW_PATTERN_OWNED";
        const auto type=std::make_shared<const SqlType>(categoryType(ValueType::Decimal));
        table.columns={{"ID",ValueType::Decimal,type,true},{"G",ValueType::Decimal,type},{"V",ValueType::Decimal,type}};
        catalog.tables.push_back(std::move(table));
        const std::string query="SELECT m.g,m.n FROM ROW_PATTERN_OWNED MATCH_RECOGNIZE ("
            "PARTITION BY g ORDER BY id MEASURES COUNT(*) AS n ONE ROW PER MATCH "
            "AFTER MATCH SKIP PAST LAST ROW PATTERN (A+) DEFINE A AS A.v > 0) m ORDER BY 1,2";
        const auto model=compile(names.dialect,query,{},&catalog,names);
        QVERIFY2(model->ok(),model->diagnostics.empty()?query.c_str():model->diagnostics.front().message.c_str());
        MemoryStatistics statistics;
        TableStatistics measured;
        measured.database=names.database;
        measured.schema=names.schema;
        measured.name="ROW_PATTERN_OWNED";
        measured.rows=9;
        statistics.tables.push_back(std::move(measured));
        const auto optimized=optimize(model->model,&statistics);
        QVERIFY2(optimized.ok(),optimized.diagnostics.empty()?"missing Match plan":optimized.diagnostics.front().message.c_str());
        QVERIFY(std::any_of(optimized.statistics.begin(),optimized.statistics.end(),[](const auto &entry) {
            return entry.name=="ROW_PATTERN_OWNED" && entry.rows==9;
        }));
        QVERIFY(std::any_of(optimized.best.nodes.begin(),optimized.best.nodes.end(),[](const auto &node) {
            return node.kind==PhysicalKind::Match;
        }));
        const auto generated=generateSql(model->model,names.dialect);
        QVERIFY2(generated.ok(),generated.diagnostics.empty()?query.c_str():generated.diagnostics.front().message.c_str());
        auto emitted=generated.sql;
        if (!emitted.empty() && emitted.back()==';') emitted.pop_back();
        const auto native=run(query),roundTrip=run(emitted);
        QCOMPARE(roundTrip.rows,native.rows);
        QCOMPARE(roundTrip.columns,native.columns);
        QCOMPARE(roundTrip.columnTypes,native.columnTypes);
        QCOMPARE(native.rows.size(),qsizetype(3));
        QCOMPARE(native.rows[0][1].toInt(),1);
        QCOMPARE(native.rows[1][1].toInt(),2);
        QCOMPARE(native.rows[2][1].toInt(),3);
        auto invalid=query;
        invalid.replace(invalid.find("A.v"),3,"B.v");
        QVERIFY(!compile(names.dialect,invalid,{},&catalog,names)->ok());
        run("DROP TABLE ROW_PATTERN_OWNED PURGE");
    }catch(const std::exception &e){QFAIL(e.what());}}
    void querySchemaContextChangesNameResolutionWithoutChangingLogin(){try{
        using namespace navidog;auto db=connectDatabase(p_);auto setup=db->execute("CREATE TABLE QUERY_CTX_TX(n NUMBER)",{},cancellation());QVERIFY2(setup.error.isEmpty(),qPrintable(setup.error));QueryService service;QSignalSpy done(&service,&QueryService::completed);QueryExecutionOptions options;options.sessionId="owned-oracle-context";options.schema="NAVIDOG_NAMESPACE_ADMIN";auto run=[&](QString sql){if(!service.run(p_,sql,{},options) || !done.wait(10000))throw std::runtime_error("Oracle context query timed out");return qvariant_cast<QueryOutcome>(done.takeFirst()[0]);};
        auto result=run("SELECT SYS_CONTEXT('USERENV','CURRENT_SCHEMA'),USER FROM dual");QVERIFY2(result.result.error.isEmpty(),qPrintable(result.result.error));QCOMPARE(result.result.rows[0][0].toString(),QString("NAVIDOG_NAMESPACE_ADMIN"));QCOMPARE(result.result.rows[0][1].toString(),p_.user);options.autoCommit=false;result=run("INSERT INTO NAVIDOG_OWNED.QUERY_CTX_TX VALUES(1)");QVERIFY2(result.result.error.isEmpty(),qPrintable(result.result.error));QCOMPARE(result.transactionState,TransactionState::Active);options.schema=p_.user;result=run("SELECT 1 FROM dual");QVERIFY(result.result.error.contains("先提交"));QCOMPARE(result.transactionState,TransactionState::Active);options.schema="NAVIDOG_NAMESPACE_ADMIN";options.transaction=TransactionAction::Rollback;result=run({});QCOMPARE(result.transactionState,TransactionState::Idle);options.autoCommit=true;options.transaction=TransactionAction::None;options.schema=p_.user;result=run("SELECT SYS_CONTEXT('USERENV','CURRENT_SCHEMA') FROM dual");QVERIFY2(result.result.error.isEmpty(),qPrintable(result.result.error));QCOMPARE(result.result.rows[0][0].toString(),p_.user);options.schema.clear();result=run("SELECT SYS_CONTEXT('USERENV','CURRENT_SCHEMA') FROM dual");QVERIFY2(result.result.error.isEmpty(),qPrintable(result.result.error));QCOMPARE(result.result.rows[0][0].toString(),p_.user);
    }catch(const std::exception& e){QFAIL(e.what());}}
    void createUsersWithQuotasAndReportPartialGrants(){try{
        using namespace navidog;auto admin=p_;admin.id+="-admin";admin.user="NAVIDOG_NAMESPACE_ADMIN";admin.password=adminPassword_;auto token=cancellation();
        auto options=loadNamespaceOptions(admin,NamespaceKind::Schema,token);QVERIFY2(options.error.isEmpty(),qPrintable(options.error));QVERIFY2(options.tablespaces.contains("NAVIDOG_DATA"),qPrintable("Visible permanent tablespaces: "+options.tablespaces.join(", ")));QVERIFY(options.temporaryTablespaces.contains("TEMP"));
        NamespaceDraft d;d.kind=NamespaceKind::Schema;d.name="USER_CREATED_OWNED";d.password="N9_"+QUuid::createUuid().toString(QUuid::Id128).left(16);d.defaultTablespace="NAVIDOG_DATA";d.temporaryTablespace="TEMP";d.quotaMiB=5;
        auto result=createNamespace(admin,d,token);QVERIFY2(result.applied,qPrintable(result.error));QVERIFY(!result.partial);auto user=p_;user.id+="-created";user.user=d.name;user.password=d.password;auto login=connectDatabase(user);QCOMPARE(login->execute("SELECT USER FROM dual",{},token).rows[0][0].toString(),d.name);login.reset();
        auto db=connectDatabase(admin);auto row=db->executeBound("SELECT default_tablespace,temporary_tablespace,account_status FROM dba_users WHERE username=:1",{d.name},{},token).rows[0];QCOMPARE(row[0].toString(),QString("NAVIDOG_DATA"));QCOMPARE(row[1].toString(),QString("TEMP"));QCOMPARE(row[2].toString(),QString("OPEN"));QCOMPARE(db->executeBound("SELECT max_bytes FROM dba_ts_quotas WHERE username=:1 AND tablespace_name='NAVIDOG_DATA'",{d.name},{},token).rows[0][0].toLongLong(),5LL*1024*1024);
        result=createNamespace(admin,d,token);QVERIFY(!result.applied);QVERIFY(!result.partial);QVERIFY(!result.error.contains(d.password));
        d.name="USER_LOCKED_OWNED";d.grantSession=false;d.accountLocked=true;result=createNamespace(admin,d,token);QVERIFY2(result.applied,qPrintable(result.error));QCOMPARE(db->executeBound("SELECT account_status FROM dba_users WHERE username=:1",{d.name},{},token).rows[0][0].toString(),QString("LOCKED"));
        // This fixture may CREATE USER but has no ADMIN OPTION for CREATE SESSION.
        d.name="USER_PARTIAL_OWNED";d.grantSession=true;result=createNamespace(p_,d,token);QVERIFY(!result.applied);QVERIFY(result.partial);QVERIFY(result.error.contains("用户已创建"));QVERIFY(!result.error.contains(d.password));QCOMPARE(db->executeBound("SELECT count(*) FROM all_users WHERE username=:1",{d.name},{},token).rows[0][0].toInt(),1);
    }catch(const std::exception& e){QFAIL(e.what());}}
    void namespaceDropPreservesNonemptyOracleSchemas(){try {
        using namespace navidog;
        QVERIFY(sqlPlus("ALTER SESSION SET CONTAINER=FREEPDB1;\n"
                        "GRANT DROP USER TO NAVIDOG_NAMESPACE_ADMIN;\n"
                        "CREATE USER NAVIDOG_DROP_CASE NO AUTHENTICATION;\n"
                        "CREATE TABLE NAVIDOG_DROP_CASE.retained(id NUMBER);"));
        auto admin=p_;
        admin.id+="-drop";
        admin.user="NAVIDOG_NAMESPACE_ADMIN";
        admin.password=adminPassword_;
        const QString schema="NAVIDOG_DROP_CASE";
        const auto preview=dropNamespaceSql(admin,NamespaceKind::Schema,schema);
        QCOMPARE(preview,QString("DROP USER \"NAVIDOG_DROP_CASE\";"));

        QueryService service;
        QSignalSpy done(&service,&QueryService::namespaceCompleted);
        auto request=[&](const QString &id) {
            if (!service.dropNamespace(admin,NamespaceKind::Schema,schema,id) || !done.wait(10000))
                throw std::runtime_error("Oracle fixture: namespace operation timed out");
            return done.takeFirst().at(1).value<NamespaceResult>();
        };
        const auto refused=request("owned-drop-nonempty");
        QVERIFY(!refused.applied);
        QVERIFY2(refused.error.contains("ORA-01922"),qPrintable(refused.error));
        QVERIFY(sqlPlus("ALTER SESSION SET CONTAINER=FREEPDB1;\n"
                        "SELECT COUNT(*) FROM NAVIDOG_DROP_CASE.retained;\n"
                        "DROP TABLE NAVIDOG_DROP_CASE.retained PURGE;"));
        const auto removed=request("owned-drop-empty");
        QVERIFY2(removed.applied,qPrintable(removed.error));
        const auto token=cancellation();
        auto database=connectDatabase(admin);
        const auto users=database->executeBound("SELECT COUNT(*) FROM all_users WHERE username=:1",
                                                {schema},{},token);
        QVERIFY2(users.error.isEmpty(),qPrintable(users.error));
        QCOMPARE(users.rows.front().front().toInt(),0);
    }catch(const std::exception &e){QFAIL(e.what());}}
    void nativeParametersAndTransactions(){try{
        using namespace navidog;auto db=connectDatabase(p_);auto token=cancellation();auto r=db->executeBound("SELECT :1 AS txt,:2 AS binary_value,:3 AS null_value FROM dual",{QString("中文 ' 参数"),QByteArray::fromHex("0001ff"),QVariant{}},{},token);QCOMPARE(r.rows[0][0].toString(),QString("中文 ' 参数"));QCOMPARE(r.rows[0][1].toByteArray(),QByteArray::fromHex("0001ff"));QVERIFY(!r.rows[0][2].isValid());
        db->execute("CREATE TABLE ROW_OWNED(ID NUMBER PRIMARY KEY,LABEL VARCHAR2(200),PAYLOAD BLOB,NOTE CLOB)",{},token);r=db->executeBound("INSERT INTO ROW_OWNED(ID,LABEL,PAYLOAD,NOTE) VALUES(:1,:2,:3,:4)",{1,QString("typed ' \\ value"),QByteArray::fromHex("00ff"),QString("长文本")},{},token);QCOMPARE(r.affectedRows,1);QCOMPARE(db->transactionState(),TransactionState::Active);db->transaction(TransactionAction::Rollback,token);QCOMPARE(db->execute("SELECT count(*) FROM ROW_OWNED",{},token).rows[0][0].toInt(),0);
        db->setAutoCommit(true);r=db->executeBound("INSERT INTO ROW_OWNED(ID,LABEL) VALUES(:1,:2)",{1,"committed"},{},token);QCOMPARE(r.affectedRows,1);auto other=connectDatabase(p_);QCOMPARE(other->execute("SELECT count(*) FROM ROW_OWNED",{},token).rows[0][0].toInt(),1);
    }catch(const std::exception& e){QFAIL(e.what());}}
    void queryTypesAreNativeAndAvailableWithoutRows(){try{
        auto db=navidog::connectDatabase(p_);auto result=db->execute("SELECT CAST(NULL AS VARCHAR2(13 CHAR)),CAST(NULL AS NUMBER(8,2)),CAST(NULL AS RAW(4)) FROM dual WHERE 1=0",{},navidog::cancellation());QVERIFY(result.rows.isEmpty());QCOMPARE(result.columnTypes,QStringList({"VARCHAR2(13)","NUMBER(8,2)","RAW(4)"}));
    }catch(const std::exception& e){QFAIL(e.what());}}
    void editableDataPagesPreserveDatesLobsAndRollbackConflicts(){try{
        using namespace navidog;auto profile=p_;profile.advanced.initialSql="ALTER SESSION SET NLS_DATE_FORMAT='YYYY'; ALTER SESSION SET NLS_TIMESTAMP_FORMAT='YYYY';";auto db=connectDatabase(profile);auto token=cancellation();db->setAutoCommit(true);
        db->execute("CREATE TABLE DATA_OWNED(ID NUMBER GENERATED ALWAYS AS IDENTITY PRIMARY KEY,LABEL VARCHAR2(100) UNIQUE,NOTE CLOB,PAYLOAD BLOB,HAPPENED DATE,STAMP TIMESTAMP(9),CALCULATED NUMBER GENERATED ALWAYS AS (ID+10) VIRTUAL)",{},token);
        DataPageRequest request;request.node.kind="table";request.node.schema=profile.user;request.node.object=request.node.name="DATA_OWNED";request.pageSize=1;
        auto page=readDataPage(profile,request,token);QVERIFY2(page.error.isEmpty(),qPrintable(page.error));QVERIFY(page.editable);QVERIFY(!page.schema.columns[0].protectedReason.isEmpty());QVERIFY(!page.schema.columns[6].protectedReason.isEmpty());
        DataRowChange insert;insert.state=DataRowState::Inserted;insert.values.resize(7);insert.values[1]="first";insert.values[2]=QString("中文备注");insert.values[3]=QByteArray::fromHex("00ff");insert.values[4]="2024-05-06 12:34:56";insert.values[5]="2024-05-06 12:34:56.123456789";insert.touched={1,2,3,4,5};
        auto saved=commitDataChanges(profile,page.schema,{insert},token);QVERIFY2(saved.error.isEmpty(),qPrintable(saved.error));QCOMPARE(saved.inserted,1);
        page=readDataPage(profile,request,token);QVERIFY2(page.error.isEmpty(),qPrintable(page.error));QCOMPARE(page.result.rows[0][2].toString(),QString("中文备注"));QCOMPARE(page.result.rows[0][3].toByteArray(),QByteArray::fromHex("00ff"));QCOMPARE(page.result.rows[0][4].toString(),QString("2024-05-06 12:34:56"));QCOMPARE(page.result.rows[0][5].toString(),QString("2024-05-06 12:34:56.123456789"));
        auto first=page.result.rows[0];insert.values[1]="second";saved=commitDataChanges(profile,page.schema,{insert},token);QVERIFY2(saved.error.isEmpty(),qPrintable(saved.error));request.afterKey={first[0]};page=readDataPage(profile,request,token);QVERIFY2(page.error.isEmpty(),qPrintable(page.error));QVERIFY(page.keyset);QCOMPARE(page.result.rows[0][1].toString(),QString("second"));
        request.afterKey.clear();request.offset=1;page=readDataPage(profile,request,token);QVERIFY2(page.error.isEmpty(),qPrintable(page.error));QCOMPARE(page.result.columns.size(),7);QCOMPARE(page.result.rows[0][1].toString(),QString("second"));
        request.offset=0;request.filter=DataFilter::Contains;request.filterColumn=1;request.filterValue="fir";page=readDataPage(profile,request,token);QVERIFY2(page.error.isEmpty(),qPrintable(page.error));QCOMPARE(page.result.rows.size(),1);
        request.filter=DataFilter::None;page=readDataPage(profile,request,token);DataRowChange change;change.state=DataRowState::Modified;change.original=change.values=page.result.rows[0];change.values[1]="updated";change.touched={1};saved=commitDataChanges(profile,page.schema,{change},token);QVERIFY2(saved.error.isEmpty(),qPrintable(saved.error));QCOMPARE(saved.updated,1);
        saved=commitDataChanges(profile,page.schema,{change},token);QVERIFY(saved.error.contains("其他会话"));QVERIFY(saved.rolledBack);
        page=readDataPage(profile,request,token);change.original=change.values=page.result.rows[0];change.values[1]="must rollback";insert.values[1]="second";saved=commitDataChanges(profile,page.schema,{change,insert},token);QVERIFY(!saved.error.isEmpty());QVERIFY(saved.rolledBack);page=readDataPage(profile,request,token);QCOMPARE(page.result.rows[0][1].toString(),QString("updated"));
        auto locker=connectDatabase(profile);locker->execute("SELECT ID FROM DATA_OWNED WHERE LABEL='updated' FOR UPDATE",{},token);change.original=change.values=page.result.rows[0];change.values[1]="locked";QElapsedTimer timer;timer.start();saved=commitDataChanges(profile,page.schema,{change},token);QVERIFY(!saved.error.isEmpty());QVERIFY(saved.rolledBack);QVERIFY(timer.elapsed()<3000);locker->transaction(TransactionAction::Rollback,token);
        change.state=DataRowState::Deleted;change.touched.clear();saved=commitDataChanges(profile,page.schema,{change},token);QVERIFY2(saved.error.isEmpty(),qPrintable(saved.error));QCOMPARE(saved.deleted,1);
        DataRowChange defaults;defaults.state=DataRowState::Inserted;defaults.values.resize(7);saved=commitDataChanges(profile,page.schema,{defaults},token);QVERIFY2(saved.error.isEmpty(),qPrintable(saved.error));QCOMPARE(saved.inserted,1);
    }catch(const std::exception& e){QFAIL(e.what());}}
    void tableDesignerAndMetadata(){try{
        using namespace navidog;CatalogNode node;node.kind="table";node.schema=p_.user;node.object=node.name="DESIGN_OWNED";TableDefinition empty,d;d.node=node;TableColumn id;id.name="ID";id.type="NUMBER";id.primaryKey=1;id.nullable=false;d.columns<<id;TableColumn value;value.name="LABEL";value.type="VARCHAR2(50)";value.comment="说明";d.columns<<value;d.comment="自有测试表";TableObject index;index.name="DESIGN_IDX_OWNED";index.expression="LABEL";d.objects<<index;TableObject check;check.kind=TableObjectKind::Check;check.name="DESIGN_CHECK_OWNED";check.expression="ID>0";d.objects<<check;
        auto result=applyTableChanges(p_,empty,d,cancellation());QVERIFY2(result.error.isEmpty(),qPrintable(result.error));QVERIFY2(result.refreshError.isEmpty(),qPrintable(result.refreshError));QCOMPARE(result.definition.columns.size(),2);QCOMPARE(result.definition.comment,d.comment);auto before=result.definition;auto after=before;TableColumn extra;extra.name="NOTE";extra.type="VARCHAR2(100)";after.columns<<extra;result=applyTableChanges(p_,before,after,cancellation());QVERIFY2(result.error.isEmpty(),qPrintable(result.error));QCOMPARE(result.definition.columns.size(),3);
    }catch(const std::exception& e){QFAIL(e.what());}}
    void routineDefinitionCompilationAndImplicitOutputs(){try{
        using namespace navidog;CatalogNode node;node.kind="function";node.schema=p_.user;node.object=node.name="FUNCTION_OWNED";SourceDefinition empty;auto result=applySourceChanges(p_,empty,node,sourceTemplate(p_,node),false,cancellation());QVERIFY2(result.applied,qPrintable(result.error));QVERIFY2(result.refreshError.isEmpty(),qPrintable(result.refreshError));QVERIFY2(result.warning.isEmpty(),qPrintable(result.warning));auto db=connectDatabase(p_);QCOMPARE(db->execute("SELECT FUNCTION_OWNED(8) FROM dual",{},cancellation()).rows[0][0].toInt(),8);
        auto before=result.definition;auto sql=before.sql;sql.replace("RETURN p_value;","RETURN p_value+1;");result=applySourceChanges(p_,before,node,sql,false,cancellation());QVERIFY2(result.applied,qPrintable(result.error));QVERIFY2(result.warning.isEmpty(),qPrintable(result.warning));QCOMPARE(db->execute("SELECT FUNCTION_OWNED(8) FROM dual",{},cancellation()).rows[0][0].toInt(),9);
        node.kind="procedure";node.object=node.name="OUTPUT_OWNED";auto create="CREATE PROCEDURE "+quoteIdentifier(p_.user,p_.driver)+".OUTPUT_OWNED(P_INPUT IN NUMBER,P_OUTPUT OUT NUMBER) AS BEGIN P_OUTPUT:=NVL(P_INPUT,0)+1; END;";result=applySourceChanges(p_,empty,node,create,false,cancellation());QVERIFY2(result.applied,qPrintable(result.error));QVERIFY2(result.warning.isEmpty(),qPrintable(result.warning));auto call=sourceCallSql(p_,result.definition);QueryService service;QSignalSpy done(&service,&QueryService::completed);QVERIFY(service.run(p_,call));QVERIFY(done.wait(5000));auto outcome=qvariant_cast<QueryOutcome>(done.takeFirst()[0]);QVERIFY2(outcome.result.error.isEmpty(),qPrintable(outcome.result.error));QVERIFY(outcome.results.size()>=2);QCOMPARE(outcome.results[1].rows[0][0].toInt(),1);
    }catch(const std::exception& e){QFAIL(e.what());}}
    void explainPreservesOriginalTransactionsAndNeverRunsDelete(){try{
        using namespace navidog;auto db=connectDatabase(p_);auto token=cancellation();db->setAutoCommit(true);db->execute("CREATE TABLE PLAN_OWNED(ID NUMBER PRIMARY KEY)",{},token);db->execute("INSERT INTO PLAN_OWNED VALUES(1)",{},token);
        QueryService service;QSignalSpy done(&service,&QueryService::completed);QueryExecutionOptions options;options.sessionId="owned-plan-transaction";options.autoCommit=false;auto run=[&](QString text){if(!service.run(p_,text,{},options) || !done.wait(5000))throw std::runtime_error("plan test timed out");return qvariant_cast<QueryOutcome>(done.takeFirst()[0]);};
        auto result=run("INSERT INTO PLAN_OWNED VALUES(2)");QVERIFY2(result.result.error.isEmpty(),qPrintable(result.result.error));QCOMPARE(result.transactionState,TransactionState::Active);options.explain=true;result=run("DELETE FROM PLAN_OWNED WHERE ID=1");QVERIFY(result.explained);QVERIFY2(result.result.error.isEmpty(),qPrintable(result.result.error));QVERIFY(!result.result.rows.isEmpty());QCOMPARE(result.transactionState,TransactionState::Active);QCOMPARE(db->execute("SELECT count(*) FROM PLAN_OWNED",{},token).rows[0][0].toInt(),1);
        options.explain=false;options.transaction=TransactionAction::Rollback;result=run({});QCOMPARE(result.transactionState,TransactionState::Idle);QCOMPARE(db->execute("SELECT count(*) FROM PLAN_OWNED",{},token).rows[0][0].toInt(),1);
    }catch(const std::exception& e){QFAIL(e.what());}}
    void parallelQueriesUseThreadOwnedOciSessions(){try{
        using namespace navidog;QueryService service;QSignalSpy done(&service,&QueryService::completed);QueryExecutionOptions first,second;first.sessionId="owned-oracle-slow";second.sessionId="owned-oracle-other";
        QVERIFY(service.run(p_,"BEGIN DBMS_SESSION.SLEEP(20); END;\n/",{},first));QVERIFY(service.run(p_,"BEGIN DBMS_SESSION.SLEEP(0.3); END;\n/\nSELECT 53 FROM dual;",{},second));
        QTimer::singleShot(100,&service,[&]{service.cancelQuery(p_.id,p_.database,first.sessionId);});QTRY_COMPARE_WITH_TIMEOUT(done.count(),2,10000);bool stopped=false,success=false;for(auto event:done){auto result=qvariant_cast<QueryOutcome>(event[0]);if(result.sessionId==first.sessionId){QVERIFY(result.result.cancelled);QVERIFY(result.sessionClosed);stopped=true;}else{QVERIFY2(result.result.error.isEmpty(),qPrintable(result.result.error));QVERIFY(!result.result.cancelled);QCOMPARE(result.result.rows[0][0].toInt(),53);success=true;}}QVERIFY(stopped && success);QCOMPARE(service.sessionCount(),1);
    }catch(const std::exception& e){QFAIL(e.what());}}
    void queryCancellationUsesOciBreak(){try{
        using namespace navidog;QueryService service;QSignalSpy done(&service,&QueryService::completed);
        // Establish the session first, so this exercises OCI cancellation during
        // execution rather than merely cancelling a cold connection attempt.
        QVERIFY(service.run(p_,"SELECT 1 FROM dual"));QVERIFY(done.wait(10000));auto warm=qvariant_cast<QueryOutcome>(done.takeFirst()[0]);QVERIFY2(warm.result.error.isEmpty(),qPrintable(warm.result.error));
        QElapsedTimer elapsed;elapsed.start();QVERIFY(service.run(p_,"BEGIN DBMS_SESSION.SLEEP(30); END;\n/"));QTimer::singleShot(100,&service,&QueryService::cancel);QVERIFY(done.wait(10000));auto outcome=qvariant_cast<QueryOutcome>(done.takeFirst()[0]);QVERIFY(outcome.result.cancelled);QVERIFY(outcome.sessionClosed);QCOMPARE(service.sessionCount(),0);QVERIFY(elapsed.elapsed()<10000);
    }catch(const std::exception& e){QFAIL(e.what());}}
    void cleanupTestCase(){if(container_.isEmpty())return;QProcess remove;remove.start("docker",{"rm","-f",container_});if(!remove.waitForFinished(15000)){remove.kill();remove.waitForFinished(1000);}if(server_.state()!=QProcess::NotRunning && !server_.waitForFinished(3000)){server_.kill();server_.waitForFinished(1000);}adminPassword_.fill(QChar(0));p_.password.fill(QChar(0));}
};
QTEST_GUILESS_MAIN(OracleTest)
#include "oracle_test.moc"
