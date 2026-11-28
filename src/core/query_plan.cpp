#include "navidog/database_sql.hpp"
#include "navidog/query_plan.hpp"
#include "navidog/query_context.hpp"
#include "navidog/sql.hpp"
#include <QUuid>
#include <QDateTime>
#include <QElapsedTimer>
namespace navidog {
namespace {
[[noreturn]] void fail(const QString &text) {
  throw std::runtime_error(text.toStdString());
}
QueryResult checked(Database &db, const QString &text, QueryLimits limits,
                    const Cancellation &cancel,PlanRequestTrace *trace) {
  if(trace){trace->issued=true;trace->commands.append(text);}
  auto result = db.execute(text, limits, cancel);
  if (result.cancelled)
    return result;
  if (!result.error.isEmpty())
    fail(result.error);
  return result;
}
} // namespace
QueryResult readQueryPlan(const ConnectionProfile &profile, const QString &text,
                          QueryLimits limits, const Cancellation &cancel,
                          const std::function<void()> &connected,
                          const QString &schema,PlanRequestTrace *trace) {
  QElapsedTimer elapsed;elapsed.start();
  if(trace){*trace={};trace->connectionName=profile.name;trace->database=profile.database;trace->schema=schema;trace->requestedAtUtc=QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs);}
  struct Finish {PlanRequestTrace *trace;QElapsedTimer &elapsed;~Finish(){if(trace)trace->elapsedMs=elapsed.elapsed();}} finishTrace{trace,elapsed};
  auto document=[&](QueryResult result){if(trace)trace->received=!result.cancelled && result.error.isEmpty() && !result.rows.isEmpty();return result;};
  // Plan documents are commonly larger than the ordinary cell preview cap.
  limits.maxBytes =
      qBound<qint64>(qint64(1024), limits.maxBytes, qint64(4 * 1024 * 1024));
  limits.maxCellBytes = int(qMin<qint64>(limits.maxBytes, 1024 * 1024));
  limits.maxRows = qBound(1, limits.maxRows, 10000);
  const auto k = profile.driver;
  const auto dialect = scriptDialect(k);
  auto bytes = text.toUtf8();
  if(bytes.size()>1024*1024)fail("执行计划输入超过 1 MiB 上限");
  auto spans = sql::splitStatements(
      std::string_view(bytes.constData(), bytes.size()), dialect);
  if (spans.size() != 1 || spans.front().compound)
    fail("请选中一条查询 / 数据操作语句，再查看执行计划");
  auto span = spans.front();
  const auto original = sql::statementSource(
      std::string_view(bytes.constData(), bytes.size()), span, dialect);
  auto statement = QString::fromUtf8(original.data(), int(original.size()));
  auto tokens = sql::lex(statement.toStdString(), dialect);
  QString first;
  for (auto token : tokens.tokens)
    if (token.kind != sql::TokenKind::Comment &&
        statement.toUtf8().mid(token.begin, token.end - token.begin) != "(") {
      first = QString::fromUtf8(
                  statement.toUtf8().mid(token.begin, token.end - token.begin))
                  .toUpper();
      break;
    }
  if (!QStringList{"SELECT", "WITH", "INSERT", "UPDATE", "DELETE", "MERGE",
                   "REPLACE"}
           .contains(first))
    fail("执行计划仅支持 SELECT / WITH / INSERT / UPDATE / DELETE / MERGE "
         "等数据语句");
  if (cancel->load()) {
    QueryResult result;
    result.cancelled = true;
    return result;
  }
  auto db = connectDatabase(profile);
  if(trace)trace->serverVersion=db->serverVersion().left(256);
  if (connected)
    connected();
  applyQuerySchema(*db, profile, schema, cancel);
  db->setAutoCommit(false);
  if (k == DatabaseKind::Dameng)
    return document(checked(*db, "EXPLAIN FOR " + statement, limits, cancel, trace));
  if (k == DatabaseKind::Oracle) {
    const auto id =
        "NAVIDOG_" + QUuid::createUuid().toString(QUuid::Id128).left(16);
    auto planned = checked(
        *db, "EXPLAIN PLAN SET STATEMENT_ID = '" + id + "' FOR " + statement,
        limits, cancel,trace);
    if (planned.cancelled)
      return planned;
    auto result =
        checked(*db,
                "SELECT ID, PARENT_ID, OPERATION, OPTIONS, OBJECT_OWNER, "
                "OBJECT_NAME, COST, CARDINALITY, BYTES, ACCESS_PREDICATES, "
                "FILTER_PREDICATES FROM PLAN_TABLE WHERE STATEMENT_ID = '" +
                    id + "' ORDER BY ID",
                limits, cancel,trace);
    // EXPLAIN PLAN writes only this isolated session's temporary plan table.
    // Discard its rows without committing or touching the query tab's
    // transaction.
    db->transaction(TransactionAction::Rollback, cancellation());
    return document(std::move(result));
  }
  if (k == DatabaseKind::SQLServer) {
    auto setup = checked(*db, "SET SHOWPLAN_XML ON", limits, cancel,trace);
    if (setup.cancelled)
      return setup;
    auto result = checked(*db, statement, limits, cancel,trace);
    // The session is discarded on every path, including cancel/error, so that
    // SHOWPLAN can never leak into a subsequent user query.
    return document(std::move(result));
  }
  if (k == DatabaseKind::SQLite)
    return document(checked(*db, "EXPLAIN QUERY PLAN " + statement, limits, cancel,trace));
  if (usesNativePostgres(k))
    return document(checked(*db,
                   "EXPLAIN (FORMAT JSON, VERBOSE, COSTS, ANALYZE FALSE) " + statement,
                   limits, cancel,trace));
  if (k == DatabaseKind::TiDB)
    return document(checked(*db, "EXPLAIN FORMAT='row' " + statement, limits, cancel, trace));
  if (navidog::isMySqlFamily(k) || k==DatabaseKind::OceanBaseOracle)
    return document(checked(*db, "EXPLAIN FORMAT=JSON " + statement, limits, cancel,trace));
  fail("当前数据库尚不支持执行计划");
}
} // namespace navidog
