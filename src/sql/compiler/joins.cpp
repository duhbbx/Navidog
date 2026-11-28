#include "parser.hpp"
#include <algorithm>

namespace navidog::sql::compiler {
RelationId Parser::relation(Statement &s, Relation r) {
  if (syntaxNodes_ + context_.ast.size() + context_.ast.queryCount() >=
      context_.options.limits.nodes)
    fail("relation node limit exceeded", DiagnosticCode::ResourceLimit);
  ++syntaxNodes_;
  s.relations.push_back(std::move(r));
  return static_cast<RelationId>(s.relations.size());
}
bool Parser::derivedTableAhead() {
  if (context_.dialect == Dialect::SQLServer && at("VALUES", 1))
    return true;
  if (!queryAhead(1))
    return false;
  if (!at("(", 1))
    return true;

  // A joined table group may start with a parenthesized SELECT as its
  // first table. Only tokens following its closing parenthesis can
  // distinguish that group from a parenthesized set operand.
  std::size_t prefix = 1;
  while (at("(", prefix))
    ++prefix;
  std::size_t depth = prefix;
  for (std::size_t ahead = prefix; cursor_ + ahead < tokens_.size(); ++ahead) {
    if (ahead % 256 == 0)
      checkStop(context_);
    if (at("(", ahead)) {
      ++depth;
    } else if (at(")", ahead)) {
      if (--depth == 0)
        return true;
      if (depth < prefix) {
        prefix = depth;
        const auto next = ahead + 1;
        if (!at(")", next) && !at("UNION", next) && !at("EXCEPT", next) &&
            !at("INTERSECT", next) && !at("MINUS", next) && !at("ORDER", next) &&
            !at("LIMIT", next) && !at("OFFSET", next) && !at("FETCH", next))
          return false;
      }
    }
  }
  return true; // Let the query parser diagnose missing closing tokens.
}
PostgresRowFunction Parser::postgresRowFunctionAhead() const {
  if (context_.dialect!=Dialect::PostgreSQL) return PostgresRowFunction::None;
  const auto name=at("PG_CATALOG") && at(".",1)?2u:0u;
  if (at("(",name+1)) {
    if (at("UNNEST",name)) return PostgresRowFunction::Unnest;
    if (at("GENERATE_SERIES",name)) return PostgresRowFunction::GenerateSeries;
  }
  return PostgresRowFunction::None;
}
TableSource Parser::postgresFunctionSource(PostgresRowFunction kind) {
  const auto start=current().range.begin;
  const auto call=primary();
  const auto &expression=context_.ast.expression(call);
  const auto location=expression.range;
  const bool unnest=kind==PostgresRowFunction::Unnest;
  const auto count=expression.operands.size();
  if (expression.kind!=ExprKind::Call || expression.distinct || count<(unnest?1u:2u) ||
      count>(unnest?1u:4u) || std::any_of(expression.operands.begin(),expression.operands.end(),[&](auto operand) {
        return context_.ast.expression(operand).kind==ExprKind::Star;
      }))
    fail(unnest?"UNNEST relation requires one array expression":"GENERATE_SERIES relation requires two to four arguments",
         DiagnosticCode::UnsupportedSyntax);
  TableSource source;
  source.postgresFunction=kind;
  if (at("WITH") && at("ORDINALITY",1)) {
    take();
    const auto ordinal=take().range;
    source.ordinalityColumn=NamePart{context_.ast.intern("ordinality"),false,ordinal};
  }
  source.alias=alias(true);
  source.inferredFunctionAlias=!source.alias;
  const NamePart defaultName{context_.ast.intern(unnest?"unnest":"generate_series"),false,location};
  auto field=source.alias.value_or(defaultName);
  if (source.alias && eat("(")) {
    field=namePart();
    source.functionColumnAliases=1;
    if (source.ordinalityColumn && eat(",")) {
      source.ordinalityColumn=namePart();
      source.functionColumnAliases=2;
    }
    expect(")");
  }
  if (!source.alias) source.alias=defaultName;
  Statement rows;
  rows.range=location;
  rows.projections={{call,field}};
  source.query=storeQuery(std::move(rows));
  source.range={start,previous().range.end};
  return source;
}
RelationId Parser::tableFactor(Statement &s) {
  if (at("(") && !derivedTableAhead()) {
    if (depth_ >= context_.options.limits.nesting)
      fail("join nesting limit exceeded", DiagnosticCode::ResourceLimit);
    struct Guard {
      std::size_t &d;
      Guard(std::size_t &n) : d(n) { ++d; }
      ~Guard() { --d; }
    } guard(depth_);
    take();
    const auto root = joinedTable(s);
    expect(")");
    if (at("AS") || nameToken())
      fail("aliases on joined table groups require a separate namespace",
           DiagnosticCode::UnsupportedSyntax);
    return root;
  }
  const bool json = at("JSON_TABLE") && at("(", 1);
  if (json && (context_.dialect != Dialect::MySQL || s.kind != StatementKind::Select))
    fail("JSON_TABLE relation is supported in MySQL SELECT queries",
         DiagnosticCode::UnsupportedSyntax);
  const auto function=postgresRowFunctionAhead();
  const bool postgresFunction=function!=PostgresRowFunction::None;
  if (postgresFunction && s.kind!=StatementKind::Select)
    fail("PostgreSQL set-returning functions in data modification require row-source lowering",
         DiagnosticCode::UnsupportedSyntax);
  auto source=json?jsonTableSource():postgresFunction?postgresFunctionSource(function):table(JoinKind::First);
  Relation r;
  r.table = s.tables.size();
  r.range = source.range;
  s.tables.push_back(std::move(source));
  return relation(s, std::move(r));
}
RelationId Parser::joinedTable(Statement &s) {
  auto left = tableFactor(s);
  while (true) {
    const auto begin = s.relations[left - 1].range.begin;
    JoinKind kind;
    bool apply = false;
    bool fixedOrder = false;
    bool natural = eat("NATURAL");
    if (natural && !dmlGrammar().naturalUsing)
      fail("NATURAL JOIN is unavailable in this dialect",
           DiagnosticCode::UnsupportedSyntax);
    if (eat("STRAIGHT_JOIN")) {
      if (!dmlGrammar().mysql || natural)
        fail("STRAIGHT_JOIN requires a non-NATURAL MySQL-family join", DiagnosticCode::UnsupportedSyntax);
      kind = JoinKind::Inner;
      fixedOrder = true;
    } else if (eat("JOIN"))
      kind = JoinKind::Inner;
    else if (eat("INNER")) {
      if (natural && context_.dialect==Dialect::TiDB)
        fail("TiDB NATURAL JOIN does not accept an explicit INNER keyword");
      expect("JOIN");
      kind = JoinKind::Inner;
    } else if (eat("CROSS")) {
      if (natural)
        fail("NATURAL CROSS JOIN is not supported");
      apply = eat("APPLY");
      if (!apply) expect("JOIN");
      kind = JoinKind::Cross;
    } else if (eat("OUTER")) {
      expect("APPLY");
      apply = true;
      kind = JoinKind::Left;
    } else if (eat("LEFT")) {
      eat("OUTER");
      expect("JOIN");
      kind = JoinKind::Left;
    } else if (eat("RIGHT")) {
      eat("OUTER");
      expect("JOIN");
      kind = JoinKind::Right;
    } else if (eat("FULL")) {
      if (!fullJoin())
        fail("FULL JOIN is unavailable in this dialect",
             DiagnosticCode::UnsupportedSyntax);
      eat("OUTER");
      expect("JOIN");
      kind = JoinKind::Full;
    } else {
      if (natural)
        fail("expected JOIN after NATURAL");
      break;
    }
    const bool lateralKeyword = context_.dialect == Dialect::PostgreSQL && eat("LATERAL");
    const bool lateral = apply || lateralKeyword;
    if (apply && context_.dialect != Dialect::SQLServer && context_.dialect != Dialect::Oracle)
      fail("APPLY is unavailable in this dialect", DiagnosticCode::UnsupportedSyntax);
    if (lateral && (s.kind != StatementKind::Select || natural ||
        (!(at("(") && derivedTableAhead()) && postgresRowFunctionAhead()==PostgresRowFunction::None) ||
        (kind != JoinKind::Cross && kind != JoinKind::Inner && kind != JoinKind::Left)))
      fail("lateral joins currently require a derived query and a CROSS, INNER or LEFT join",
           DiagnosticCode::UnsupportedSyntax);
    Relation r;
    r.left = left;
    r.right = tableFactor(s);
    const auto &rightRelation = s.relations[r.right - 1];
    const bool postgresFunction=!rightRelation.left && s.tables[rightRelation.table].postgresFunction!=PostgresRowFunction::None;
    const bool postgresLateral=lateralKeyword || postgresFunction;
    r.implicitLateral = !rightRelation.left && s.tables[rightRelation.table].jsonTable != nullptr;
    if (r.implicitLateral && (natural ||
        (kind != JoinKind::Cross && kind != JoinKind::Inner && kind != JoinKind::Left)))
      fail("JSON_TABLE requires a CROSS, INNER or LEFT join to the preceding relation",
           DiagnosticCode::UnsupportedSyntax);
    if (postgresFunction && (natural || (kind!=JoinKind::Cross && kind!=JoinKind::Inner && kind!=JoinKind::Left)))
      fail("PostgreSQL set-returning function requires CROSS, INNER or LEFT join in this subset",DiagnosticCode::UnsupportedSyntax);
    r.join = kind;
    r.natural = natural;
    r.lateral = lateral || r.implicitLateral || postgresFunction;
    r.explicitLateral = lateralKeyword;
    r.fixedOrder = fixedOrder;
    if (eat("ON")) {
      if (apply || natural || (kind == JoinKind::Cross && !dmlGrammar().mysql))
        fail("ON is invalid on this join");
      r.condition = expression();
      requireScalar(r.condition);
      if (postgresLateral) {
        const auto &on = context_.ast.expression(r.condition);
        if (on.kind != ExprKind::Boolean || asciiUpper(on.value) != "TRUE")
          fail("lateral joins with a nontrivial ON condition require dependent predicate lowering",
               DiagnosticCode::UnsupportedSyntax);
      }
    } else if (eat("USING")) {
      if (postgresLateral)
        fail("lateral joins with USING require correlated column coalescing", DiagnosticCode::UnsupportedSyntax);
      if (r.implicitLateral)
        fail("JSON_TABLE JOIN USING requires implicit lateral column coalescing", DiagnosticCode::UnsupportedSyntax);
      if (apply || natural || fixedOrder || kind == JoinKind::Cross || !dmlGrammar().naturalUsing)
        fail("USING is invalid on this join");
      expect("(");
      do {
        r.usingColumns.push_back(namePart());
      } while (eat(","));
      expect(")");
    } else if (!apply && !natural && kind != JoinKind::Cross &&
               !(dmlGrammar().mysql && kind == JoinKind::Inner))
      fail("expected ON or USING join condition");
    r.range = {begin, previous().range.end};
    if (s.relations[r.right - 1].left == 0) {
      auto &leaf = s.tables[s.relations[r.right - 1].table];
      if (r.lateral) leaf.lateralLeft = left;
      leaf.join = r.join;
      leaf.condition = r.condition;
    }
    left = relation(s, std::move(r));
  }
  return left;
}
void Parser::fromClause(Statement &s) {
  auto left = joinedTable(s);
  while (eat(",")) {
    Relation r;
    r.left = left;
    r.right = joinedTable(s);
    r.join = JoinKind::Cross;
    r.comma = true;
    r.range = {s.relations[left - 1].range.begin, previous().range.end};
    if (!s.relations[r.right - 1].left) {
      auto &leaf = s.tables[s.relations[r.right - 1].table];
      leaf.join = JoinKind::Cross;
      if (leaf.jsonTable) {
        r.lateral = r.implicitLateral = true;
        leaf.lateralLeft = left;
      }
      if (leaf.postgresFunction!=PostgresRowFunction::None) {
        r.lateral=true;
        leaf.lateralLeft=left;
      }
    }
    left = relation(s, std::move(r));
  }
  s.from = left;
}
} // namespace navidog::sql::compiler
