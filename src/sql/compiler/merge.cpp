#include "parser.hpp"
#include <array>
#include <set>
namespace navidog::sql::compiler {
Statement Parser::merge() {
  const auto grammar = ddlGrammar();
  if (!grammar.postgres && !grammar.oracle && !grammar.sqlserver)
    fail("MERGE is unavailable in this dialect baseline");
  Statement s;
  s.kind = StatementKind::Merge;
  expect("MERGE");
  if (grammar.sqlserver) {
    dmlTop(s);
    eat("INTO");
  } else
    expect("INTO");
  s.target = table(JoinKind::First);
  if (s.target->query)
    fail("MERGE target must be a named relation");
  expect("USING");
  s.from = tableFactor(s);
  if (s.relations.at(s.from - 1).left)
    fail("MERGE joined sources require an explicit source subquery",
         DiagnosticCode::UnsupportedSyntax);
  expect("ON");
  if (grammar.oracle) {
    expect("(");
    s.mergeOn = expression();
    expect(")");
  } else
    s.mergeOn = expression();
  requireScalar(s.mergeOn);
  std::array<bool, 3> exhausted{};
  std::array<std::size_t, 3> counts{};
  std::array<MergeAction, 3> firstAction{};
  std::array<bool, 3> firstConditional{};
  while (eat("WHEN")) {
    if (s.mergeClauses.size() >= 256)
      fail("MERGE clause count exceeds budget", DiagnosticCode::ResourceLimit);
    MergeClause clause;
    clause.origin.begin = previous().range.begin;
    if (eat("NOT")) {
      expect("MATCHED");
      clause.match = MergeMatch::NotMatchedTarget;
      if (eat("BY")) {
        if (!grammar.sqlserver)
          fail("BY SOURCE/TARGET requires a newer PostgreSQL baseline or SQL "
               "Server",
               DiagnosticCode::UnsupportedSyntax);
        if (eat("SOURCE"))
          clause.match = MergeMatch::NotMatchedSource;
        else
          expect("TARGET");
      }
    } else
      expect("MATCHED");
    const auto category = std::size_t(clause.match);
    if (eat("AND")) {
      if (grammar.oracle)
        fail(
            "Oracle MERGE puts branch predicates after UPDATE/INSERT as WHERE");
      clause.condition = expression();
      requireScalar(clause.condition);
    }
    expect("THEN");
    if (eat("UPDATE")) {
      if (clause.match == MergeMatch::NotMatchedTarget)
        fail("unmatched target branch cannot UPDATE");
      clause.action = MergeAction::Update;
      expect("SET");
      assignments(clause.assignments, !grammar.postgres);
      if (grammar.oracle) {
        for (const auto &a : clause.assignments)
          if (a.targets.size() != 1)
            fail("Oracle MERGE tuple assignments are not covered",
                 DiagnosticCode::UnsupportedSyntax);
        if (eat("WHERE")) {
          clause.condition = expression();
          requireScalar(clause.condition);
        }
        if (eat("DELETE")) {
          expect("WHERE");
          clause.deleteWhere = expression();
          requireScalar(clause.deleteWhere);
        }
      }
    } else if (eat("DELETE")) {
      if (grammar.oracle || clause.match == MergeMatch::NotMatchedTarget)
        fail("DELETE is invalid for this MERGE branch/dialect");
      clause.action = MergeAction::Delete;
    } else if (eat("INSERT")) {
      if (clause.match != MergeMatch::NotMatchedTarget)
        fail("INSERT requires WHEN NOT MATCHED [BY TARGET]");
      clause.action = MergeAction::Insert;
      if (eat("(")) {
        std::set<std::string> seen;
        do {
          auto name = qualifiedName();
          if (name.size() > 1) {
            if (!grammar.oracle || name.size() != 2)
              fail("MERGE INSERT columns must be unqualified");
            const auto &target = *s.target;
            const auto prefix = canonicalIdentifier(
                context_.dialect, context_.ast.identifier(name[0].identifier),
                name[0].quoted);
            const auto &symbol =
                target.alias ? *target.alias : target.name.back();
            if (prefix !=
                canonicalIdentifier(context_.dialect,
                                    context_.ast.identifier(symbol.identifier),
                                    symbol.quoted))
              fail("MERGE INSERT column qualifier must name its target");
          }
          auto col = name.back();
          auto key = canonicalIdentifier(
              context_.dialect, context_.ast.identifier(col.identifier),
              col.quoted);
          if (!seen.insert(key).second)
            fail("duplicate MERGE INSERT column");
          clause.columns.push_back(col);
        } while (eat(","));
        expect(")");
      }
      if (eat("DEFAULT")) {
        if (grammar.oracle)
          fail("Oracle MERGE requires an explicit VALUES list");
        expect("VALUES");
        clause.defaultValues = true;
      } else {
        expect("VALUES");
        expect("(");
        do {
          clause.values.push_back(assignmentValue());
        } while (eat(","));
        expect(")");
        if (!clause.columns.empty() &&
            clause.columns.size() != clause.values.size())
          fail("MERGE INSERT column/value counts differ");
      }
      if (grammar.oracle && eat("WHERE")) {
        clause.condition = expression();
        requireScalar(clause.condition);
      }
    } else if (eat("DO")) {
      if (!grammar.postgres)
        fail("MERGE DO NOTHING is only enabled for PostgreSQL");
      expect("NOTHING");
      clause.action = MergeAction::Nothing;
    } else
      fail("expected MERGE UPDATE, DELETE, INSERT or DO NOTHING");
    if (grammar.postgres && exhausted[category])
      fail("unreachable MERGE branch follows an unconditional branch",
           DiagnosticCode::InvalidQueryShape);
    if (!clause.condition)
      exhausted[category] = true;
    if (!counts[category]) {
      firstAction[category] = clause.action;
      firstConditional[category] = clause.condition != 0;
    }
    ++counts[category];
    if (grammar.oracle && counts[category] > 1)
      fail("Oracle MERGE accepts one branch of each kind");
    if (grammar.sqlserver) {
      if (counts[category] >
          (clause.match == MergeMatch::NotMatchedTarget ? 1u : 2u))
        fail("too many SQL Server MERGE branches");
      if (counts[category] == 2 && (!firstConditional[category] ||
                                    firstAction[category] == clause.action))
        fail("two SQL Server MERGE branches need an initial predicate and "
             "different UPDATE/DELETE actions");
    }
    clause.origin.end = previous().range.end;
    s.mergeClauses.push_back(std::move(clause));
  }
  if (s.mergeClauses.empty())
    fail("MERGE requires at least one WHEN branch");
  if (grammar.sqlserver) returning(s);
  if (at("OUTPUT") || at("RETURNING"))
    fail("MERGE output clauses require additional pseudo-column/version rules",
         DiagnosticCode::UnsupportedSyntax);
  if (grammar.sqlserver && !at(";"))
    fail("SQL Server MERGE requires a terminating semicolon");
  return s;
}
} // namespace navidog::sql::compiler
