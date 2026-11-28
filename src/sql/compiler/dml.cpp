#include "parser.hpp"
#include <algorithm>
#include <unordered_set>

namespace navidog::sql::compiler {
void Parser::endStatement(bool nested) {
  if (current().kind != TokenKind::End && !at(";") && !(nested && at(")")))
    fail("unsupported or misplaced clause: " + current().value,
         current().kind == TokenKind::Number || current().kind == TokenKind::String
             ? DiagnosticCode::ExpectedToken : DiagnosticCode::UnsupportedSyntax);
}
std::vector<CommonTableExpression> Parser::withClause(bool &recursive) {
  std::vector<CommonTableExpression> result;
  if (!eat("WITH"))
    return result;
  recursive = eat("RECURSIVE");
  if (recursive && context_.dialect != Dialect::PostgreSQL &&
      context_.dialect != Dialect::SQLite && context_.dialect != Dialect::MySQL &&
      context_.dialect != Dialect::MariaDB)
    fail("this dialect has no WITH RECURSIVE support in the local grammar",
         DiagnosticCode::UnsupportedSyntax);
  do {
    CommonTableExpression cte;
    cte.range.begin = current().range.begin;
    cte.name = namePart();
    if (eat("(")) {
      do {
        cte.columns.push_back(namePart());
      } while (eat(","));
      expect(")");
    }
    expect("AS");
    if (context_.dialect == Dialect::PostgreSQL || context_.dialect == Dialect::SQLite) {
      if (eat("MATERIALIZED")) cte.materialization = CteMaterialization::Materialized;
      else if (eat("NOT")) {
        expect("MATERIALIZED");
        cte.materialization = CteMaterialization::NotMaterialized;
      }
    }
    expect("(");
    cte.query = nestedCteBody();
    expect(")");
    cte.range.end = previous().range.end;
    result.push_back(std::move(cte));
  } while (eat(","));
  return result;
}
Statement Parser::statement(bool nested) {
  const auto begin = current().range.begin;
  bool recursive = false;
  auto ctes = withClause(recursive);
  Statement s;
  if (queryAhead())
    s = select(nested);
  else {
    if (!ctes.empty() && !dmlGrammar().leadingWith)
      fail("this dialect does not accept a leading WITH on DML",
           DiagnosticCode::UnsupportedSyntax);
    if (transactionAhead()) {
      if (!ctes.empty()) fail("WITH cannot precede a transaction statement");
      s = transactionStatement();
    } else if (dmlGrammar().mysql && (at("PREPARE") || at("EXECUTE") || at("DEALLOCATE") ||
                                     (at("DROP") && at("PREPARE", 1)))) {
      if (!ctes.empty()) fail("WITH cannot precede a prepared statement control");
      s = preparedStatement();
    } else if (context_.dialect == Dialect::MySQL && at("SET") &&
               (at("ROLE", 1) || (at("DEFAULT", 1) && at("ROLE", 2)))) {
      if (!ctes.empty()) fail("WITH cannot precede a role statement");
      s = roleStatement();
    } else if (at("PRAGMA")) {
      if (!ctes.empty()) fail("WITH cannot precede a PRAGMA statement");
      s = pragmaStatement();
    } else if (at("VACUUM")) {
      if (!ctes.empty()) fail("WITH cannot precede a VACUUM statement");
      s = vacuumStatement();
    } else if (at("COPY")) {
      if (!ctes.empty()) fail("WITH cannot precede a COPY statement");
      s = copyStatement();
    } else if (at("SHOW") || at("DESCRIBE") || at("DESC") ||
               (context_.dialect == Dialect::TiDB && at("ADMIN"))) {
      if (!ctes.empty()) fail("WITH cannot precede an inspection statement");
      s = inspectionStatement();
    } else if (at("CALL"))
      s = callStatement();
    else if (at("MERGE"))
      s=merge();
    else if (at("INSERT"))
      s = insert();
    else if (dmlGrammar().mysql && at("REPLACE"))
      s = insert(true);
    else if (at("UPDATE"))
      s = update();
    else if (at("DELETE"))
      s = deleteStatement();
    else if (at("CREATE") || at("ALTER") || at("DROP") || at("TRUNCATE") ||
             at("RENAME") || at("COMMENT")) {
      if (!ctes.empty())
        fail("WITH is not accepted before this DDL statement");
      s = ddl();
    } else {
      // Recognized statement families outside this frontend stay informational
      // in the editor. An arbitrary word/typo is a syntax error, not a promise
      // of a future grammar extension.
      static const std::unordered_set<std::string> otherStatements = {
          "SHOW",     "DESCRIBE",  "DESC",       "EXPLAIN",  "USE",
          "SET",      "CALL",      "BEGIN",      "START",    "COMMIT",
          "ROLLBACK", "SAVEPOINT", "RELEASE",    "END",      "ABORT",
          "DO",       "MERGE",     "GRANT",      "REVOKE",   "ANALYZE",
          "VACUUM",   "REINDEX",   "CLUSTER",    "COPY",     "IMPORT",
          "EXPORT",   "LOAD",      "UNLOAD",     "LOCK",     "UNLOCK",
          "AUDIT",    "NOAUDIT",   "FLASHBACK",  "PURGE",    "PREPARE",
          "EXECUTE",  "EXEC",      "DEALLOCATE", "DISCARD",  "LISTEN",
          "NOTIFY",   "UNLISTEN",  "DECLARE",    "FETCH",    "CLOSE",
          "MOVE",     "OPEN",      "RESET",      "REASSIGN", "REFRESH",
          "REPAIR",   "OPTIMIZE",  "CHECK",      "CHECKSUM", "FLUSH",
          "KILL",     "INSTALL",   "UNINSTALL",  "BACKUP",   "RESTORE",
          "DBCC",     "PRINT",     "RAISERROR",  "THROW",    "WAITFOR",
          "DENY",     "ENABLE",    "DISABLE",    "SHUTDOWN", "HELP",
          "HANDLER",  "DELIMITER", "SIGNAL",     "RESIGNAL", "GET",
          "IF",       "WHILE",     "LOOP",       "REPEAT",   "RETURN",
          "VALUES",   "TABLE",     "CHECKPOINT", "SECURITY", "BULK",
          "REPLACE",  "GO"};
      if (current().kind == TokenKind::Word &&
          otherStatements.count(asciiUpper(current().value)))
        fail("statement family is not supported by the local parser: " +
                 current().value,
             DiagnosticCode::UnsupportedSyntax);
      fail("unrecognized SQL statement start: " + current().value);
    }
  }
  if (!ctes.empty() && s.kind == StatementKind::Insert && dmlGrammar().mysql)
    fail("MySQL INSERT places WITH before its source SELECT",
         DiagnosticCode::UnsupportedSyntax);
  if (!ctes.empty()) {
    if (!s.ctes.empty())
      fail("nested WITH scopes require a separate wrapper",
           DiagnosticCode::UnsupportedSyntax);
    s.ctes = std::move(ctes);
    s.recursiveWith = recursive;
  }
  s.range = {begin, previous().range.end};
  endStatement(nested);
  return s;
}
Statement Parser::roleStatement() {
  Statement result;
  result.kind = StatementKind::Role;
  auto &command = result.role.emplace();
  expect("SET");
  const bool accountDefault = eat("DEFAULT");
  if (accountDefault) command.scope = RoleScope::AccountDefault;
  expect("ROLE");
  auto accountPart = [&]() {
    if (at("CURRENT_USER") || (!nameToken() && current().kind != TokenKind::String))
      fail("expected a MySQL account name or a quoted account part");
    auto token = take();
    if (token.kind != TokenKind::String)
      context_.identifierRanges.push_back(token.range);
    return token.value;
  };
  auto accounts = [&](std::vector<RoleAccount> &list) {
    do {
      RoleAccount account;
      account.range.begin = current().range.begin;
      account.name = accountPart();
      if (eat("@")) account.host = accountPart();
      account.range.end = previous().range.end;
      list.push_back(std::move(account));
    } while (eat(","));
  };
  if (!accountDefault && eat("DEFAULT")) {
    command.mode = RoleMode::Default;
  } else if (eat("NONE")) {
    command.mode = RoleMode::None;
  } else if (eat("ALL")) {
    command.mode = RoleMode::All;
    if (!accountDefault && eat("EXCEPT")) {
      command.mode = RoleMode::AllExcept;
      accounts(command.roles);
    }
  } else {
    accounts(command.roles);
  }
  if (accountDefault) {
    expect("TO");
    accounts(command.recipients);
  }
  return result;
}
Statement Parser::callStatement() {
  if (context_.dialect == Dialect::TiDB)
    fail("TiDB does not support stored procedure CALL", DiagnosticCode::UnsupportedSyntax);
  if (context_.dialect == Dialect::SQLite ||
      context_.dialect == Dialect::SQLServer)
    fail("CALL is not available in this dialect baseline",
         DiagnosticCode::UnsupportedSyntax);
  Statement s;
  s.kind = StatementKind::Call;
  expect("CALL");
  s.routine = qualifiedName();
  expect("(");
  if (!eat(")")) {
    do {
      s.arguments.push_back(expression());
    } while (eat(","));
    expect(")");
  }
  return s;
}
QueryId Parser::storeQuery(Statement s) {
  if (syntaxNodes_ + context_.ast.size() + context_.ast.queryCount() >=
          context_.options.limits.nodes ||
      context_.ast.queryCount() >= context_.options.limits.queries)
    fail("AST query limit exceeded", DiagnosticCode::ResourceLimit);
  return context_.ast.addQuery(std::move(s));
}
void Parser::projections(std::vector<Projection> &out,bool allowAssignmentAliases) {
  do {
    std::optional<NamePart> assignedAlias;
    if (allowAssignmentAliases && context_.dialect==Dialect::SQLServer && at("=",1)) {
      if (current().kind==TokenKind::Parameter)
        fail("T-SQL SELECT variable assignments require a structured assignment model",DiagnosticCode::UnsupportedSyntax);
      if (current().kind==TokenKind::String) {
        const auto token=take();
        if (token.value.empty()) fail("empty projection aliases cannot be represented by this output model",DiagnosticCode::UnsupportedSyntax);
        assignedAlias=NamePart{context_.ast.intern(token.value),true,token.range};
      } else if (nameToken()) assignedAlias=namePart();
      if (assignedAlias) {
        if (context_.ast.identifier(assignedAlias->identifier).empty())
          fail("empty projection aliases cannot be represented by this output model",DiagnosticCode::UnsupportedSyntax);
        expect("=");
      }
    }
    ExprId e;
    {
      struct RestoreDepth {
        std::size_t &target;
        std::size_t saved;
        ~RestoreDepth() {target=saved;}
      } restore{projectionExpressionDepth_,projectionExpressionDepth_};
      projectionExpressionDepth_=depth_+1;
      e = expression();
    }
    if (assignedAlias && (at("AS") || nameToken() || current().kind==TokenKind::String))
      fail("a T-SQL projection cannot declare both assignment and trailing aliases");
    auto a = assignedAlias?assignedAlias:alias(false);
    if (a && context_.ast.expression(e).kind == ExprKind::Star)
      fail("wildcard cannot have an alias");
    out.push_back({e, a});
  } while (eat(","));
}
ExprId Parser::assignmentValue() {
  if (at("DEFAULT") && !at("(", 1)) {
    if (!dmlGrammar().defaultExpression)
      fail("DEFAULT expressions are not available in this dialect",
           DiagnosticCode::UnsupportedSyntax);
    take();
    return node({ExprKind::Default, previous().range, "", {}, {}});
  }
  auto value = expression();
  requireScalar(value);
  return value;
}
void Parser::assignments(std::vector<Assignment> &out, bool qualified) {
  do {
    Assignment assignment;
    const bool tuple = eat("(");
    do {
      const auto begin = current().range.begin;
      auto name = qualifiedName();
      if (!qualified && name.size() != 1)
        fail("assignment target must be an unqualified column");
      const auto base=node({ExprKind::Name,{begin,previous().range.end},{},std::move(name),{}});
      auto target=base;
      if (context_.dialect==Dialect::PostgreSQL && at("[")) {
        target=postfix(target);
        const auto &subscript=context_.ast.expression(target);
        if (subscript.kind!=ExprKind::Subscript || subscript.operands.front()!=base)
          fail("assignment target cannot contain a type cast");
      }
      assignment.targets.push_back(target);
    } while (tuple && eat(","));
    if (tuple)
      expect(")");
    expect("=");
    if (tuple && at("(") && !at("SELECT", 1) && !at("WITH", 1)) {
      const auto begin = take().range.begin;
      std::vector<ExprId> values;
      do {
        values.push_back(assignmentValue());
      } while (eat(","));
      expect(")");
      if (values.size() != assignment.targets.size())
        fail("row assignment column/value counts differ");
      assignment.value = node({ExprKind::Row,
                               {begin, previous().range.end},
                               "",
                               {},
                               std::move(values)});
    } else
      assignment.value = assignmentValue();
    if (tuple &&
        context_.ast.expression(assignment.value).kind != ExprKind::Row &&
        context_.ast.expression(assignment.value).kind != ExprKind::Subquery)
      fail("row assignment requires a row or SELECT subquery");
    if (tuple && (dmlGrammar().mysql || dmlGrammar().output))
      fail("row assignment is not supported by this dialect",
           DiagnosticCode::UnsupportedSyntax);
    out.push_back(std::move(assignment));
  } while (eat(","));
}
void Parser::dmlTop(Statement &s) {
  if (!dmlGrammar().output || !at("TOP"))
    return;
  expect("TOP");
  expect("(");
  s.limit.count = limitValue();
  expect(")");
  s.limit.syntax = LimitSyntax::Top;
}
void Parser::returning(Statement &s) {
  const auto g = dmlGrammar();
  if (eat(g.output ? "OUTPUT" : "RETURNING")) {
    struct RestoreOutputScope {
      bool &flag, original;
      ~RestoreOutputScope() { flag=original; }
    } restore{mergeOutput_,mergeOutput_};
    mergeOutput_=g.output && s.kind==StatementKind::Merge;
    const auto start=previous().range.begin;
    if (!g.output && !g.returning)
      fail("RETURNING is not available in this dialect",
           DiagnosticCode::UnsupportedSyntax);
    projections(s.returning);
    if (g.output && eat("INTO")) {
      auto &sink=s.outputInto.emplace();
      sink.origin.begin=start;
      sink.projections.swap(s.returning);
      sink.tableOrigin.begin=current().range.begin;
      if (current().kind==TokenKind::Parameter)
        sink.tableVariable=take().value;
      else {
        sink.table=qualifiedName();
        if (sink.table.size()>3)
          fail("OUTPUT INTO requires a local table");
      }
      sink.tableOrigin.end=previous().range.end;
      if (at("(")) columnList(sink.columns);
      sink.origin.end=previous().range.end;
      if (eat("OUTPUT")) projections(s.returning);
    }
    if (g.returningInto) {
      expect("INTO");
      do {
        if (current().kind != TokenKind::Parameter)
          fail("RETURNING INTO requires bind variables");
        s.returningInto.push_back(primary());
      } while (eat(","));
      if (s.returningInto.size() != s.returning.size())
        fail("RETURNING/INTO counts differ");
    }
  }
}
void Parser::orderBy(Statement &s) {
  if (eat("ORDER")) {
    expect("BY");
    do {
      OrderItem order;
      order.expression = expression();
      requireScalar(order.expression);
      order.descending = eat("DESC");
      if (!order.descending)
        order.explicitAscending = eat("ASC");
      if (eat("NULLS")) {
        if (!nullOrdering())
          fail("NULLS FIRST/LAST is not available in this frontend",
               DiagnosticCode::UnsupportedSyntax);
        if (eat("FIRST"))
          order.nulls = "FIRST";
        else {
          expect("LAST");
          order.nulls = "LAST";
        }
      }
      s.orderBy.push_back(std::move(order));
    } while (eat(","));
  }
}
void Parser::dmlTail(Statement &s, bool allowLimit) {
  if (eat("WHERE")) {
    s.where = expression();
    requireScalar(s.where);
  }
  if (dmlGrammar().mysql && allowLimit) {
    orderBy(s);
    if (eat("LIMIT")) {
      s.limit.count = limitValue();
      s.limit.syntax = LimitSyntax::LimitOffset;
    }
  }
  if (!dmlGrammar().output)
    returning(s);
}
void Parser::conflict(Statement &s) {
  if (!eat("ON"))
    return;
  if (dmlGrammar().mysql) {
    if (at("CONFLICT"))
      fail("ON CONFLICT / EXCLUDED is PostgreSQL syntax; MySQL requires ON "
           "DUPLICATE KEY UPDATE");
    expect("DUPLICATE");
    expect("KEY");
    expect("UPDATE");
    s.conflict.action = ConflictClause::Action::DuplicateKey;
    assignments(s.conflict.assignments, true);
    return;
  }
  if (!dmlGrammar().conflict)
    fail("ON CONFLICT is not available in this dialect",
         DiagnosticCode::UnsupportedSyntax);
  expect("CONFLICT");
  if (eat("(")) {
    do {
      s.conflict.targets.push_back(expression());
    } while (eat(","));
    expect(")");
    if (eat("WHERE"))
      s.conflict.targetWhere = expression();
  } else if (eat("ON")) {
    if (context_.dialect != Dialect::PostgreSQL)
      fail("ON CONSTRAINT is PostgreSQL-specific");
    expect("CONSTRAINT");
    s.conflict.constraint = namePart();
  }
  expect("DO");
  if (eat("NOTHING"))
    s.conflict.action = ConflictClause::Action::Nothing;
  else {
    expect("UPDATE");
    expect("SET");
    s.conflict.action = ConflictClause::Action::Update;
    if (context_.dialect == Dialect::PostgreSQL && s.conflict.targets.empty() &&
        !s.conflict.constraint)
      fail("PostgreSQL DO UPDATE requires a conflict target");
    assignments(s.conflict.assignments, false);
    if (eat("WHERE"))
      s.conflict.where = expression();
  }
}
Statement Parser::insert(bool replaceExisting) {
  Statement s;
  s.kind = StatementKind::Insert;
  s.replaceExisting = replaceExisting;
  expect(replaceExisting ? "REPLACE" : "INSERT");
  const auto g = dmlGrammar();
  if (g.mysql) {
    if (eat("LOW_PRIORITY"))
      s.modifiers.push_back("LOW_PRIORITY");
    else if (!replaceExisting && eat("HIGH_PRIORITY"))
      s.modifiers.push_back("HIGH_PRIORITY");
    if (!replaceExisting && eat("IGNORE"))
      s.modifiers.push_back("IGNORE");
  }
  if (context_.dialect == Dialect::SQLite && eat("OR")) {
    if (!at("ROLLBACK") && !at("ABORT") && !at("REPLACE") && !at("FAIL") &&
        !at("IGNORE"))
      fail("expected SQLite conflict algorithm");
    s.modifiers.push_back("OR " + take().value);
  }
  dmlTop(s);
  if (!g.mysql && !g.output)
    expect("INTO");
  else
    eat("INTO");
  TableSource target;
  target.range.begin = current().range.begin;
  target.name = qualifiedName();
  if ((g.conflict || context_.dialect == Dialect::Oracle) && at("AS")) {
    if (!tableAliasWithAS())
      fail("AS is not allowed for Oracle table aliases");
    take();
    target.alias = namePart();
  } else if (context_.dialect == Dialect::Oracle &&
             nameToken())
    target.alias = namePart();
  target.range.end = previous().range.end;
  s.target = target;
  bool explicitColumns = false;
  if (at("(") && !(g.mysql && queryAhead(1))) {
    explicitColumns = true;
    take();
    if (!g.mysql || !eat(")")) {
      do {
        s.columns.push_back(namePart());
      } while (eat(","));
      expect(")");
    }
  }
  if (g.output)
    returning(s);
  if (eat("DEFAULT")) {
    if (!g.defaultValues)
      fail("DEFAULT VALUES is not available in this dialect",
           DiagnosticCode::UnsupportedSyntax);
    expect("VALUES");
    s.insertSource = InsertSource::DefaultValues;
  } else if (eat("VALUES") || (g.mysql && eat("VALUE"))) {
    s.insertSource = InsertSource::Values;
    do {
      expect("(");
      std::vector<ExprId> row;
      if (!eat(")")) {
        do {
          row.push_back(assignmentValue());
        } while (eat(","));
        expect(")");
      }
      if (row.empty() && !g.mysql)
        fail("empty VALUES row is only available in MySQL mode");
      s.rows.push_back(std::move(row));
      if (at(",") && !g.multiValues)
        fail("multi-row VALUES is unavailable in the Oracle 19c baseline",
             DiagnosticCode::UnsupportedSyntax);
    } while (eat(","));
  } else if (g.mysql && eat("SET")) {
    if (explicitColumns) fail("INSERT SET cannot be combined with an explicit column list");
    s.insertSource = InsertSource::Assignments;
    assignments(s.assignments, false);
  } else if (queryAhead()) {
    s.insertSource = InsertSource::Query;
    const bool parens = eat("(");
    s.source = storeQuery(select(parens, true));
    if (parens)
      expect(")");
  } else
    fail("expected VALUES, a query, or DEFAULT VALUES");
  if (g.mysql && !replaceExisting &&
      (s.insertSource == InsertSource::Values || s.insertSource == InsertSource::Assignments) &&
      eat("AS")) {
    if (context_.dialect != Dialect::MySQL)
      fail("VALUES/SET row aliases require the MySQL dialect", DiagnosticCode::UnsupportedSyntax);
    s.conflict.rowAlias = namePart();
    if (eat("(")) {
      do {
        if (s.conflict.columnAliases.size() >= context_.options.limits.columnsPerQuery)
          fail("inserted row column-alias budget exceeded", DiagnosticCode::ResourceLimit);
        s.conflict.columnAliases.push_back(namePart());
      } while (eat(","));
      expect(")");
      const auto width = s.insertSource == InsertSource::Values
                             ? s.rows.front().size() : s.assignments.size();
      if (s.conflict.columnAliases.size() != width)
        fail("inserted row column aliases must match the number of inserted values");
    }
  }
  if (replaceExisting && at("ON"))
    fail("REPLACE cannot have an ON DUPLICATE KEY UPDATE clause");
  if (!replaceExisting)
    conflict(s);
  if (!g.output)
    returning(s);
  return s;
}
Statement Parser::update() {
  Statement s;
  s.kind = StatementKind::Update;
  expect("UPDATE");
  const auto g = dmlGrammar();
  if (g.mysql) {
    if (eat("LOW_PRIORITY"))
      s.modifiers.push_back("LOW_PRIORITY");
    if (eat("IGNORE"))
      s.modifiers.push_back("IGNORE");
  }
  if (context_.dialect == Dialect::SQLite && eat("OR")) {
    if (!at("ROLLBACK") && !at("ABORT") && !at("REPLACE") && !at("FAIL") &&
        !at("IGNORE"))
      fail("expected SQLite conflict algorithm");
    s.modifiers.push_back("OR " + take().value);
  }
  dmlTop(s);
  if (g.mysql) {
    fromClause(s);
    s.target = s.tables.front();
    s.targetInTables = true;
  } else
    s.target = table(JoinKind::First,true);
  expect("SET");
  assignments(s.assignments, g.qualifiedAssignment);
  if (g.output)
    returning(s);
  if (g.updateFrom && eat("FROM"))
    fromClause(s);
  dmlTail(s, s.tables.size() <= 1);
  return s;
}
Statement Parser::deleteStatement() {
  Statement s;
  s.kind = StatementKind::Delete;
  expect("DELETE");
  const auto g = dmlGrammar();
  if (g.mysql) {
    bool low=false,quick=false,ignore=false;
    if (context_.dialect==Dialect::TiDB) {
      low=eat("LOW_PRIORITY");
      quick=eat("QUICK");
      ignore=eat("IGNORE");
    } else {
      // MySQL/MariaDB accept these flags in any order, including duplicates.
      // OceanBase places IGNORE before its repeatable QUICK/LOW_PRIORITY list.
      if (context_.dialect==Dialect::OceanBase) ignore=eat("IGNORE");
      while (at("LOW_PRIORITY") || at("QUICK") ||
             (context_.dialect!=Dialect::OceanBase && at("IGNORE"))) {
        if (eat("LOW_PRIORITY")) low=true;
        else if (eat("QUICK")) quick=true;
        else { expect("IGNORE"); ignore=true; }
      }
    }
    if (low) s.modifiers.push_back("LOW_PRIORITY");
    if (quick) s.modifiers.push_back("QUICK");
    if (ignore) s.modifiers.push_back("IGNORE");
  }
  dmlTop(s);
  if (g.mysql && !at("FROM")) {
    do {
      auto target = qualifiedName(true);
      if (!target.back().quoted &&
          context_.ast.identifier(target.back().identifier) == "*")
        target.pop_back();
      s.deleteTargets.push_back(std::move(target));
    } while (eat(","));
    expect("FROM");
    fromClause(s);
    s.targetInTables = true;
    // Resolve deletion aliases during lowering instead of assuming the first
    // joined table.
    TableSource target;
    target.name = s.deleteTargets.front();
    target.range={target.name.front().range.begin,target.name.back().range.end};
    s.target = target;
  } else {
    if (!g.output)
      expect("FROM");
    else
      eat("FROM");
    if (g.mysql && at("(")) fail("DELETE requires a named target table");
    s.target = table(JoinKind::First,false,g.mysql);
    const bool wildcard=g.mysql && !s.target->name.back().quoted &&
        context_.ast.identifier(s.target->name.back().identifier)=="*";
    if (wildcard) s.target->name.pop_back();
    if (g.mysql && (at(",") || at("USING")) && s.target->alias)
      fail("multi-table DELETE targets name existing aliases; they cannot declare aliases");
    if (wildcard && !at(",") && !at("USING"))
      fail("a DELETE target wildcard requires a multi-table DELETE form");
    if (g.output)
      returning(s);
    if (g.mysql && at(",")) {
      s.deleteTargets.push_back(s.target->name);
      while (eat(",")) {
        auto target = qualifiedName(true);
        if (!target.back().quoted &&
            context_.ast.identifier(target.back().identifier) == "*")
          target.pop_back();
        s.deleteTargets.push_back(std::move(target));
      }
      expect("USING");
      fromClause(s);
      s.targetInTables = true;
    } else if (g.output && eat("FROM"))
      fromClause(s);
    else if (g.deleteUsing && eat("USING"))
      fromClause(s);
    else if (g.mysql && eat("USING")) {
      s.deleteTargets.push_back(s.target->name);
      fromClause(s);
      s.targetInTables = true;
    }
    if (context_.dialect==Dialect::MariaDB && s.deleteTargets.empty() && s.target->alias)
      fail("single-table DELETE aliases are unavailable in the MariaDB 11.4 baseline");
  }
  dmlTail(s, s.deleteTargets.empty());
  return s;
}
} // namespace navidog::sql::compiler
