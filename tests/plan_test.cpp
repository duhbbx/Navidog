#include "navidog/estimated_plan.hpp"
#include "navidog/types.hpp"
#include <QtTest>
using namespace navidog;
QueryResult document(const QString &text) {
  QueryResult r;
  r.columns = {"plan"};
  r.rows = {{text}};
  return r;
}
class PlanTest : public QObject {
  Q_OBJECT
private slots:
  void postgresHierarchyAndUnknownValues();
  void mysqlNestedLoopAndCostStrings();
  void tidbTabularPlanPreservesHierarchy();
  void vendorJsonVariantsAndOceanBaseUnits();
  void requestAndOriginalRowsRemainInspectable();
  void sqlServerXmlPreservesHierarchyAndRejectsEntities();
  void oracleAndSqliteRowsRejectCyclesAndDuplicateIds();
  void truncationAndBudgetsNeverProduceCompletePlans();
};
#ifndef Q_MOC_RUN
void PlanTest::postgresHierarchyAndUnknownValues() {
  auto r = decodeEstimatedPlan(
      DatabaseKind::PostgreSQL,
      document(
          R"PLAN([{"Plan":{"Node Type":"Nested Loop","Plan Rows":10,"Startup Cost":0.3,"Total Cost":42,"Plans":[{"Node Type":"Seq Scan","Schema":"public","Relation Name":"a","Alias":"x","Plan Rows":100,"Plan Width":8,"Filter":"(id > 1)"},{"Node Type":"Index Scan","Relation Name":"b","Index Name":"b_id_idx","Plan Rows":1}]}}])PLAN"));
  QVERIFY2(r.complete, qPrintable(r.warning));
  QCOMPARE(r.nodes.size(), 3);
  QCOMPARE(r.roots, QVector<int>{0});
  QCOMPARE(r.nodes[0].children, QVector<int>({1, 2}));
  QCOMPARE(*r.nodes[0].totalCost, 42.);
  QCOMPARE(r.nodes[1].object, QString("public.a"));
  QVERIFY(!r.nodes[2].startupCost);
  QVERIFY(estimatedPlanJson(r).contains("server estimates"));
  QVERIFY(!decodeEstimatedPlan(DatabaseKind::PostgreSQL,
                               document("[{\"Plan\":{}}]"))
               .complete);
}
void PlanTest::tidbTabularPlanPreservesHierarchy() {
  QueryResult result;
  result.columns = QStringList{"id", "estRows", "task", "access object", "operator info"};
  result.rows = {{"HashJoin_1", "2.00", "root", "", ""},
                 {QString::fromUtf8("├─TableReader_2(Build)"), "1.00", "root", "", ""},
                 {QString::fromUtf8("│ └─TableFullScan_3"), "5.00", "cop[tikv]", "table:a", ""},
                 {QString::fromUtf8("└─TableReader_4(Probe)"), "8.00", "root", "", ""},
                 {QString::fromUtf8("  └─TableFullScan_5"), "8.00", "cop[tikv]", "table:b", ""}};
  const auto plan = decodeEstimatedPlan(DatabaseKind::TiDB, result);
  QVERIFY2(plan.complete, qPrintable(plan.warning));
  QCOMPARE(plan.format, QString("tidb-plan-rows"));
  QCOMPARE(plan.roots, (QVector<int>{0}));
  QCOMPARE(plan.nodes[0].children, (QVector<int>{1, 3}));
  QCOMPARE(plan.nodes[2].operation, QString("TableFullScan_3"));
  QCOMPARE(plan.nodes[4].object, QString("table:b"));
  QVERIFY(!plan.nodes[0].totalCost);
  QVERIFY(plan.raw.contains("TableFullScan_3"));
  result.rows.removeAt(1);
  QVERIFY(!decodeEstimatedPlan(DatabaseKind::TiDB, result).complete);
}
void PlanTest::mysqlNestedLoopAndCostStrings() {
  auto r = decodeEstimatedPlan(
      DatabaseKind::MySQL,
      document(
          R"PLAN({"query_block":{"select_id":1,"cost_info":{"query_cost":"123.45"},"ordering_operation":{"using_filesort":true,"nested_loop":[{"table":{"table_name":"a","access_type":"ALL","rows_examined_per_scan":100}},{"table":{"table_name":"b","access_type":"ref","key":"b_idx","rows_produced_per_join":10,"cost_info":{"prefix_cost":"120.50"}}}]}}})PLAN"));
  QVERIFY2(r.complete, qPrintable(r.warning));
  QCOMPARE(r.nodes.size(), 5);
  QCOMPARE(*r.nodes[0].totalCost, 123.45);
  QCOMPARE(r.nodes[4].index, QString("b_idx"));
  QCOMPARE(*r.nodes[4].rows, 10.);
  QCOMPARE(r.nodes[2].children, QVector<int>({3, 4}));
}
void PlanTest::vendorJsonVariantsAndOceanBaseUnits() {
  auto mysql = decodeEstimatedPlan(DatabaseKind::MySQL, document(
      R"({"operation":"Sort","estimated_rows":3,"estimated_total_cost":8.5,"inputs":[{"operation":"Index range scan","table_name":"items","index_name":"price_idx","alias":"i","estimated_rows":3}]})"));
  QVERIFY2(mysql.complete, qPrintable(mysql.warning));
  QCOMPARE(mysql.nodes.size(), 2);
  QCOMPARE(mysql.nodes[1].index, QString("price_idx"));
  QCOMPARE(mysql.nodes[1].alias, QString("i"));
  QCOMPARE(*mysql.nodes[0].totalCost, 8.5);
  auto maria = decodeEstimatedPlan(DatabaseKind::MariaDB, document(
      R"({"query_block":{"select_id":1,"cost":12.5,"read_sorted_file":{"filesort":{"sort_key":"a.id","temporary_table":{"nested_loop":[{"table":{"table_name":"a","rows":9,"cost":5}},{"block-nl-join":{"table":{"table_name":"b","rows":4,"cost":7}}}]}}}}})"));
  QVERIFY2(maria.complete, qPrintable(maria.warning));
  QCOMPARE(maria.format, QString("mariadb-json"));
  QCOMPARE(*maria.nodes[0].totalCost, 12.5);
  QCOMPARE(maria.nodes.last().object, QString("b"));
  QCOMPARE(*maria.nodes.last().rows, 4.);
  auto ocean = decodeEstimatedPlan(DatabaseKind::OceanBase, document(
      R"PLAN({"ID":0,"OPERATOR":"HASH JOIN ","NAME":"","EST.ROWS":3,"COST":90,"EST.TIME(us)":8,"CHILD_10":{"ID":2,"OPERATOR":"TABLE FULL SCAN","NAME":"RIGHT_T","EST.ROWS":4,"EST.TIME(us)":4},"CHILD_2":{"ID":1,"OPERATOR":"TABLE FULL SCAN","NAME":"LEFT_T","EST.ROWS":3,"EST.TIME(us)":3}})PLAN"));
  QVERIFY2(ocean.complete, qPrintable(ocean.warning));
  QCOMPARE(ocean.format, QString("oceanbase-json"));
  QCOMPARE(ocean.nodes.size(), 3);
  QCOMPARE(ocean.nodes[1].object, QString("LEFT_T"));
  QCOMPARE(ocean.nodes[2].object, QString("RIGHT_T"));
  QCOMPARE(*ocean.nodes[0].totalCost, 90.);
  QCOMPARE(*ocean.nodes[0].estimatedTimeUs, 8.);
  QVERIFY(!ocean.nodes[1].totalCost);
  QVERIFY(estimatedPlanJson(ocean).contains("estimatedTimeUs"));
  QVERIFY(!decodeEstimatedPlan(DatabaseKind::OceanBase, document(
      R"({"ID":0,"OPERATOR":"HASH JOIN","CHILD_1":{"ID":0,"OPERATOR":"SCAN"}})")).complete);
  QVERIFY(!decodeEstimatedPlan(DatabaseKind::OceanBase, document(
      R"({"OPERATOR":"SCAN","CHILD_0":{}})")).complete);
  auto pg = decodeEstimatedPlan(DatabaseKind::PostgreSQL, document(
      R"([{"Plan":{"Node Type":"Result","Schema":"public","Plan Rows":1}}])"));
  QVERIFY(pg.complete);
  QVERIFY(pg.nodes[0].object.isEmpty());
}
void PlanTest::requestAndOriginalRowsRemainInspectable() {
  QueryResult rows;
  rows.columns = {"id", "parent", "notused", "detail"};
  rows.rows = {{2, 0, QVariant{}, "SCAN synthetic_table"}};
  auto plan = decodeEstimatedPlan(DatabaseKind::SQLite, rows);
  QVERIFY(plan.complete);
  QVERIFY(plan.raw.contains("synthetic_table"));
  QVERIFY(plan.raw.contains("null"));
  plan.request.issued = plan.request.received = true;
  plan.request.commands = {"EXPLAIN QUERY PLAN SELECT * FROM synthetic_table"};
  plan.request.database = "temporary.db";
  auto json = estimatedPlanJson(plan);
  QVERIFY(json.contains("EXPLAIN QUERY PLAN"));
  QVERIFY(json.contains("isolatedSession"));
  rows.truncated = true;
  auto truncated = decodeEstimatedPlan(DatabaseKind::SQLite, rows);
  QVERIFY(!truncated.complete);
  QVERIFY(truncated.nodes.isEmpty());
  QVERIFY(truncated.raw.contains("synthetic_table"));
  auto pgRows = document(R"([{"Plan":{"Node Type":"Result"}}])");
  pgRows.cellTruncated = true;
  auto pg = decodeEstimatedPlan(DatabaseKind::PostgreSQL, pgRows);
  QVERIFY(!pg.complete);
  QVERIFY(!pg.raw.isEmpty());
}
void PlanTest::sqlServerXmlPreservesHierarchyAndRejectsEntities() {
  const auto xml = QString(
      R"PLAN(<ShowPlanXML xmlns="http://schemas.microsoft.com/sqlserver/2004/07/showplan"><BatchSequence><Batch><Statements><StmtSimple><QueryPlan><RelOp NodeId="0" PhysicalOp="Nested Loops" EstimateRows="2.5" EstimatedTotalSubtreeCost="1.2"><NestedLoops><RelOp NodeId="1" PhysicalOp="Index Seek" EstimateRows="2"><IndexScan><Object Schema="[dbo]" Table="[a]" Index="[PK_a]" Alias="[x]"/><Predicate><ScalarOperator ScalarString="[x].[id]=(1)"/></Predicate></IndexScan></RelOp></NestedLoops></RelOp></QueryPlan></StmtSimple></Statements></Batch></BatchSequence></ShowPlanXML>)PLAN");
  auto r = decodeEstimatedPlan(DatabaseKind::SQLServer, document(xml));
  QVERIFY2(r.complete, qPrintable(r.warning));
  QCOMPARE(r.nodes.size(), 2);
  QCOMPARE(*r.nodes[0].rows, 2.5);
  QCOMPARE(r.nodes[1].object, QString("[dbo].[a]"));
  QCOMPARE(r.nodes[0].children, QVector<int>{1});
  auto duplicate = xml;
  duplicate.replace("NodeId=\"1\"", "NodeId=\"0\"");
  QVERIFY(!decodeEstimatedPlan(DatabaseKind::SQLServer, document(duplicate))
               .complete);
  auto repeated = xml;
  const auto begin = repeated.indexOf("<QueryPlan>"),
             end = repeated.indexOf("</QueryPlan>") +
                   QString("</QueryPlan>").size();
  const auto second = repeated.mid(begin, end - begin);
  repeated.insert(end, second);
  auto separate =
      decodeEstimatedPlan(DatabaseKind::SQLServer, document(repeated));
  QVERIFY2(separate.complete, qPrintable(separate.warning));
  QCOMPARE(separate.roots.size(), 2);
  auto foreignNamespace = xml;
  foreignNamespace.replace("<RelOp NodeId=\"1\"", "<RelOp xmlns=\"urn:unrelated\" NodeId=\"1\"");
  QVERIFY(!decodeEstimatedPlan(DatabaseKind::SQLServer, document(foreignNamespace)).complete);
  auto malicious = xml;
  malicious.prepend(
      "<!DOCTYPE ShowPlanXML [<!ENTITY x SYSTEM 'file:///etc/passwd'>]>");
  r = decodeEstimatedPlan(DatabaseKind::SQLServer, document(malicious));
  QVERIFY(!r.complete);
  QVERIFY(r.warning.contains("DTD"));
  QVERIFY(r.nodes.isEmpty());
  QVERIFY(
      !decodeEstimatedPlan(
           DatabaseKind::SQLServer,
           document("<ShowPlanXML><RelOp PhysicalOp='Scan'/></ShowPlanXML>"))
           .complete);
}
void PlanTest::oracleAndSqliteRowsRejectCyclesAndDuplicateIds() {
  QueryResult oracle;
  oracle.columns = {"ID",      "PARENT_ID",    "OPERATION",
                    "OPTIONS", "OBJECT_OWNER", "OBJECT_NAME",
                    "COST",    "CARDINALITY",  "BYTES"};
  oracle.rows = {{0, QVariant{}, "SELECT STATEMENT", "", "", "", 10, 20, 160},
                 {1, 0, "TABLE ACCESS", "FULL", "OWNED", "A", 10, 20, 160}};
  auto r = decodeEstimatedPlan(DatabaseKind::Oracle, oracle);
  QVERIFY2(r.complete, qPrintable(r.warning));
  QCOMPARE(r.nodes.size(), 2);
  QCOMPARE(r.nodes[1].object, QString("OWNED.A"));
  QCOMPARE(*r.nodes[1].rowBytes, 8.);
  auto orphan = oracle;
  orphan.rows[1][1] = 99;
  QVERIFY(!decodeEstimatedPlan(DatabaseKind::Oracle, orphan).complete);
  auto self = oracle;
  self.rows[0][1] = 0;
  QVERIFY(!decodeEstimatedPlan(DatabaseKind::Oracle, self).complete);
  QueryResult sqlite;
  sqlite.columns = {"id", "parent", "notused", "detail"};
  sqlite.rows = {{2, 0, 0, "SCAN a"}, {3, 2, 0, "SEARCH b USING INDEX b_id"}};
  r = decodeEstimatedPlan(DatabaseKind::SQLite, sqlite);
  QVERIFY2(r.complete, qPrintable(r.warning));
  QCOMPARE(r.nodes[0].children, QVector<int>{1});
  auto selfSqlite = sqlite;
  selfSqlite.rows = {{1, 1, 0, "a"}};
  QVERIFY(!decodeEstimatedPlan(DatabaseKind::SQLite, selfSqlite).complete);
  selfSqlite.rows = {{0, 0, 0, "constant"}};
  QVERIFY(decodeEstimatedPlan(DatabaseKind::SQLite, selfSqlite).complete);
  selfSqlite.rows = {{2, 99, 0, "orphan"}};
  QVERIFY(!decodeEstimatedPlan(DatabaseKind::SQLite, selfSqlite).complete);
  sqlite.rows = {{1, 2, 0, "a"}, {2, 1, 0, "b"}};
  r = decodeEstimatedPlan(DatabaseKind::SQLite, sqlite);
  QVERIFY(!r.complete);
  QVERIFY(r.warning.contains("循环"));
  sqlite.rows = {{1, 0, 0, "a"}, {1, 0, 0, "b"}};
  QVERIFY(!decodeEstimatedPlan(DatabaseKind::SQLite, sqlite).complete);
}
void PlanTest::truncationAndBudgetsNeverProduceCompletePlans() {
  auto input = document(
      R"PLAN([{"Plan":{"Node Type":"Seq Scan","Plans":[{"Node Type":"Seq Scan"}]}}])PLAN");
  PlanParseLimits limits;
  limits.maxNodes = 1;
  auto r = decodeEstimatedPlan(DatabaseKind::PostgreSQL, input, limits);
  QVERIFY(!r.complete);
  QVERIFY(r.nodes.isEmpty());
  QVERIFY(!r.raw.isEmpty());
  limits.maxNodes = 10;
  limits.maxDepth = 1;
  input.rows[0][0] = QString(
      R"PLAN([{"Plan":{"Node Type":"Result","Plans":[{"Node Type":"Result","Plans":[{"Node Type":"Result"}]}]}}])PLAN");
  QVERIFY(
      !decodeEstimatedPlan(DatabaseKind::PostgreSQL, input, limits).complete);
  input.cellTruncated = true;
  QVERIFY(!decodeEstimatedPlan(DatabaseKind::PostgreSQL, input).complete);
  input.cellTruncated = false;
  input.rows[0][0] = QString(2048, 'x');
  limits.maxBytes = 1024;
  r = decodeEstimatedPlan(DatabaseKind::PostgreSQL, input, limits);
  QVERIFY(!r.complete);
  QVERIFY(r.raw.isEmpty());
}
#endif
QTEST_GUILESS_MAIN(PlanTest)
#include "plan_test.moc"
