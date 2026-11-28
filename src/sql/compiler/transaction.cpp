#include "parser.hpp"

namespace navidog::sql::compiler {
bool Parser::transactionAhead() const {
  const bool mssql = context_.dialect == Dialect::SQLServer;
  const bool pg = context_.dialect == Dialect::PostgreSQL;
  const bool sqlite = context_.dialect == Dialect::SQLite;
  return at("COMMIT") || at("ROLLBACK") ||
         (!mssql && at("SAVEPOINT")) || (mssql && at("SAVE")) ||
         ((pg || sqlite || dmlGrammar().mysql) && at("RELEASE")) ||
         ((pg || sqlite) && at("END")) || (pg && at("ABORT")) ||
         ((pg || dmlGrammar().mysql) && at("START")) ||
         (at("BEGIN") && (pg || sqlite || dmlGrammar().mysql ||
                        (mssql && (at("TRAN", 1) || at("TRANSACTION", 1)))));
}

Statement Parser::transactionStatement() {
  Statement statement;
  statement.kind = StatementKind::Transaction;
  auto &control = statement.transaction.emplace();
  const bool mssql = context_.dialect == Dialect::SQLServer;
  const bool pg = context_.dialect == Dialect::PostgreSQL;
  const bool sqlite = context_.dialect == Dialect::SQLite;
  const bool mysql = dmlGrammar().mysql;
  const auto transactionKeyword = [&] {
    if (!eat("TRAN")) expect("TRANSACTION");
  };
  if (at("BEGIN") || at("START")) {
    const bool start = eat("START");
    if (!start) expect("BEGIN");
    control.action = TransactionAction::Begin;
    if (mssql) {
      transactionKeyword();
      if (nameToken()) control.name = namePart();
    } else if (start) {
      expect("TRANSACTION");
    } else if (sqlite) {
      if (eat("DEFERRED")) control.mode = TransactionStartMode::Deferred;
      else if (eat("IMMEDIATE")) control.mode = TransactionStartMode::Immediate;
      else if (eat("EXCLUSIVE")) control.mode = TransactionStartMode::Exclusive;
      eat("TRANSACTION");
    } else if (!eat("WORK") && pg) {
      eat("TRANSACTION");
    }

    if (pg || (mysql && start)) {
      bool comma = false;
      while (true) {
        if (eat("READ")) {
          if (control.access != TransactionAccess::Default)
            fail("transaction access mode specified more than once");
          if (eat("ONLY")) control.access = TransactionAccess::ReadOnly;
          else { expect("WRITE"); control.access = TransactionAccess::ReadWrite; }
        } else if (pg && eat("ISOLATION")) {
          if (control.isolation != TransactionIsolation::Default)
            fail("transaction isolation level specified more than once");
          expect("LEVEL");
          if (eat("SERIALIZABLE")) control.isolation = TransactionIsolation::Serializable;
          else if (eat("REPEATABLE")) {
            expect("READ");
            control.isolation = TransactionIsolation::RepeatableRead;
          } else {
            expect("READ");
            if (eat("COMMITTED")) control.isolation = TransactionIsolation::ReadCommitted;
            else { expect("UNCOMMITTED"); control.isolation = TransactionIsolation::ReadUncommitted; }
          }
        } else if (pg && (at("NOT") || at("DEFERRABLE"))) {
          if (control.deferrable) fail("transaction deferrability specified more than once");
          control.deferrable = !eat("NOT");
          expect("DEFERRABLE");
        } else if (mysql && eat("WITH")) {
          if (control.consistentSnapshot) fail("consistent snapshot specified more than once");
          expect("CONSISTENT");
          expect("SNAPSHOT");
          control.consistentSnapshot = true;
        } else {
          if (comma) fail("expected transaction characteristic after comma");
          break;
        }
        comma = eat(",");
        if (!comma && !pg) break;
      }
    }
    return statement;
  }

  if (at("SAVEPOINT") || (mssql && at("SAVE"))) {
    take();
    if (mssql) transactionKeyword();
    control.action = TransactionAction::Savepoint;
    control.name = namePart();
    return statement;
  }
  if (eat("RELEASE")) {
    if (mysql) expect("SAVEPOINT");
    else eat("SAVEPOINT");
    control.action = TransactionAction::Release;
    control.name = namePart();
    return statement;
  }

  const bool commit = eat("COMMIT") || eat("END");
  if (!commit && !eat("ABORT")) expect("ROLLBACK");
  control.action = commit ? TransactionAction::Commit : TransactionAction::Rollback;
  if (mssql && (at("TRAN") || at("TRANSACTION"))) {
    transactionKeyword();
    if (nameToken()) {
      control.name = namePart();
      if (!commit) control.action = TransactionAction::NamedRollback;
    }
  } else if (sqlite) {
    eat("TRANSACTION");
  } else if (!mssql && !eat("WORK") && pg) {
    eat("TRANSACTION");
  }
  if (!commit && !mssql && eat("TO")) {
    eat("SAVEPOINT");
    control.action = TransactionAction::RollbackTo;
    control.name = namePart();
  } else {
    if ((pg || mysql) && eat("AND")) {
      control.chain = eat("NO") ? TransactionEndOption::No : TransactionEndOption::Yes;
      expect("CHAIN");
    }
    if (mysql && (at("NO") || at("RELEASE"))) {
      control.release = eat("NO") ? TransactionEndOption::No : TransactionEndOption::Yes;
      expect("RELEASE");
      if (control.chain==TransactionEndOption::Yes && control.release==TransactionEndOption::Yes)
        fail("AND CHAIN cannot be combined with RELEASE");
    }
  }
  return statement;
}
} // namespace navidog::sql::compiler
