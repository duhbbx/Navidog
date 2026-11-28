#include "parser.hpp"

namespace navidog::sql::compiler {
std::string unpivotDefaultLabel(const std::vector<std::string> &columns) {
  std::string label;
  for (std::size_t i=0;i<columns.size();++i) {
    if (i) label+='_';
    label+=columns[i];
  }
  return label;
}
bool Parser::pivotAhead() const {
  const bool calcite=context_.dialect==Dialect::PostgreSQL && context_.options.calciteExtensions;
  return (calcite || context_.dialect == Dialect::Oracle || context_.dialect == Dialect::SQLServer) &&
      at("PIVOT") && (at("(", 1) || at("XML", 1));
}
void Parser::pivotClause(TableSource &table) {
  if (!pivotAhead()) return;
  const bool oracle = context_.dialect == Dialect::Oracle;
  const bool calcite = context_.dialect == Dialect::PostgreSQL;
  auto pivot = std::make_shared<PivotSyntax>();
  pivot->origin.begin = take().range.begin;
  if (at("XML")) fail("PIVOT XML requires dynamic XML result types", DiagnosticCode::UnsupportedSyntax);
  expect("(");
  if (current().kind != TokenKind::Word)
    fail("expected a native aggregate in PIVOT");
  const auto name = asciiUpper(current().value);
  if (name != "SUM" && name != "AVG" && name != "MIN" && name != "MAX" && name != "COUNT" &&
      !(name == "COUNT_BIG" && !oracle && !calcite))
    fail("PIVOT aggregate is outside the supported native subset", DiagnosticCode::UnsupportedSyntax);
  pivot->aggregate = asciiUpper(take().value);
  expect("(");
  pivot->argumentOrigin.begin = current().range.begin;
  if ((oracle || calcite) && name == "COUNT" && eat("*")) {
    // An empty argument represents COUNT(*).
  } else pivot->argument = qualifiedName();
  pivot->argumentOrigin.end = previous().range.end;
  expect(")");
  if ((oracle || calcite) && (eat("AS") || (nameToken() && !at("FOR"))))
    pivot->aggregateAlias = namePart();
  expect("FOR");
  pivot->axisOrigin.begin = current().range.begin;
  const bool parenthesized=calcite && eat("(");
  do {
    if (pivot->axes.size()>=context_.options.limits.columnsPerQuery)
      fail("PIVOT axis width limit exceeded",DiagnosticCode::ResourceLimit);
    syntaxNode();
    pivot->axes.push_back(namePart());
  } while (parenthesized && eat(","));
  if (parenthesized) expect(")");
  pivot->axisOrigin.end = previous().range.end;
  expect("IN");
  expect("(");
  do {
    if (pivot->buckets.size() >= context_.options.limits.columnsPerQuery)
      fail("PIVOT output column limit exceeded", DiagnosticCode::ResourceLimit);
    BasicPivotBucket<NamePart> bucket;
    bucket.origin.begin = current().range.begin;
    if (oracle || calcite) {
      const bool tuple=calcite && eat("(");
      do {
        if (bucket.values.size()>=context_.options.limits.columnsPerQuery)
          fail("PIVOT bucket width limit exceeded",DiagnosticCode::ResourceLimit);
        const auto id=expression();
        const auto &value=context_.ast.expression(id);
        const bool signedNumber=value.kind==ExprKind::Unary && value.operands.size()==1 &&
            (value.value=="+" || value.value=="-") &&
            context_.ast.expression(value.operands.front()).kind==ExprKind::Number;
        if (!signedNumber && value.kind!=ExprKind::Number && value.kind!=ExprKind::String &&
            value.kind!=ExprKind::Null)
          fail("PIVOT requires static scalar IN values in this subset",DiagnosticCode::UnsupportedSyntax);
        bucket.values.push_back(id);
      } while (tuple && eat(","));
      if (tuple) expect(")");
      if (eat("AS") || nameToken()) bucket.name = namePart();
    } else bucket.name = namePart();
    bucket.origin.end = previous().range.end;
    pivot->buckets.push_back(std::move(bucket));
  } while (eat(","));
  expect(")");
  expect(")");
  pivot->origin.end = previous().range.end;
  if (oracle && table.alias)
    fail("Oracle PIVOT accepts the relation alias after the clause");
  pivot->inputAlias = table.alias;
  pivot->relationAlias = alias(true);
  if (!oracle && !pivot->relationAlias)
    fail("PIVOT needs an output relation alias");
  table.pivot = std::move(pivot);
}
bool Parser::unpivotAhead() const {
  const bool oracle=context_.dialect==Dialect::Oracle;
  const bool calcite=context_.dialect==Dialect::PostgreSQL && context_.options.calciteExtensions;
  if (!oracle && !calcite && context_.dialect!=Dialect::SQLServer) return false;
  return at("UNPIVOT") && (at("(",1) || ((oracle || calcite) && (at("INCLUDE",1) || at("EXCLUDE",1))));
}
void Parser::unpivotClause(TableSource &table) {
  if (!unpivotAhead()) return;
  if (table.pivot || table.rowMatch)
    fail("chaining UNPIVOT with another table transform requires a subquery",DiagnosticCode::UnsupportedSyntax);
  const bool oracle=context_.dialect==Dialect::Oracle;
  const bool calcite=context_.dialect==Dialect::PostgreSQL;
  auto unpivot=std::make_shared<UnpivotSyntax>();
  unpivot->origin.begin=take().range.begin;
  if ((oracle || calcite) && (at("INCLUDE") || at("EXCLUDE"))) {
    unpivot->includeNulls=eat("INCLUDE");
    if (!*unpivot->includeNulls) expect("EXCLUDE");
    expect("NULLS");
  }
  const auto columns=[&] {
    std::vector<NamePart> result;
    const bool tuple=(oracle || calcite) && eat("(");
    do {
      if (result.size()>=context_.options.limits.columnsPerQuery)
        fail("UNPIVOT tuple width limit exceeded",DiagnosticCode::ResourceLimit);
      syntaxNode();
      result.push_back(namePart());
    } while (tuple && eat(","));
    if (tuple) expect(")");
    return result;
  };
  expect("(");
  unpivot->measures=columns();
  expect("FOR");
  unpivot->descriptors=columns();
  expect("IN");
  expect("(");
  do {
    if (unpivot->inputs.size()>=context_.options.limits.columnsPerQuery)
      fail("UNPIVOT input column limit exceeded",DiagnosticCode::ResourceLimit);
    syntaxNode();
    UnpivotSyntax::Input input;
    input.origin.begin=current().range.begin;
    input.columns=columns();
    if ((oracle || calcite) && eat("AS")) {
      const bool tuple=eat("(");
      do {
        if (input.labels.size()>=context_.options.limits.columnsPerQuery)
          fail("UNPIVOT label width limit exceeded",DiagnosticCode::ResourceLimit);
        const auto id=expression();
        const auto &value=context_.ast.expression(id);
        if (calcite && value.kind!=ExprKind::String && value.kind!=ExprKind::Null)
          fail("Calcite UNPIVOT currently requires text or NULL labels",DiagnosticCode::UnsupportedSyntax);
        const bool signedNumber=value.kind==ExprKind::Unary && value.operands.size()==1 &&
            (value.value=="-" || value.value=="+") &&
            context_.ast.expression(value.operands.front()).kind==ExprKind::Number;
        if (!signedNumber && value.kind!=ExprKind::Number && value.kind!=ExprKind::String &&
            value.kind!=ExprKind::Null && value.kind!=ExprKind::TypedLiteral && value.kind!=ExprKind::Interval)
          fail("UNPIVOT labels require static literals",DiagnosticCode::UnsupportedSyntax);
        input.labels.push_back(id);
      } while (tuple && eat(","));
      if (tuple) expect(")");
    }
    input.origin.end=previous().range.end;
    unpivot->inputs.push_back(std::move(input));
  } while (eat(","));
  expect(")");
  expect(")");
  unpivot->origin.end=previous().range.end;
  if (oracle && table.alias)
    fail("Oracle UNPIVOT accepts the relation alias after the clause");
  unpivot->inputAlias=table.alias;
  unpivot->relationAlias=alias(true);
  if (!oracle && !unpivot->relationAlias)
    fail("UNPIVOT needs an output relation alias in this dialect");
  table.unpivot=std::move(unpivot);
}
const char *windowExclusionName(WindowExclusion exclusion) {
  switch(exclusion) {
  case WindowExclusion::None:return "";
  case WindowExclusion::NoOthers:return "NO OTHERS";
  case WindowExclusion::CurrentRow:return "CURRENT ROW";
  case WindowExclusion::Group:return "GROUP";
  case WindowExclusion::Ties:return "TIES";
  }
  return "INVALID";
}
QueryId Parser::nestedQuery() {
  if (!queryAhead())
    fail("expected a query expression", DiagnosticCode::UnsupportedSyntax);
  auto query = select(true);
  return storeQuery(std::move(query));
}

bool Parser::namedWindowsAllowed() const {
  return context_.dialect == Dialect::PostgreSQL ||
         context_.dialect == Dialect::SQLite || dmlGrammar().mysql;
}
std::string Parser::windowName(const NamePart &part) const {
  auto value = canonicalIdentifier(
      context_.dialect, context_.ast.identifier(part.identifier), part.quoted);
  if (dmlGrammar().mysql || context_.dialect == Dialect::SQLite)
    for (auto &c : value)
      if (c >= 'A' && c <= 'Z')
        c += 32;
  return value;
}
Parser::PendingWindow Parser::windowBody() {
  expect("(");
  PendingWindow result;
  auto &spec = result.spec;
  auto &operands = result.keys;
  if (namedWindowsAllowed() && nameToken() && !at("PARTITION") && !at("RANGE") && !at("GROUPS") &&
      !at("ROWS") && !at("EXCLUDE"))
    result.parent = namePart();
  if (eat("PARTITION")) {
    expect("BY");
    do {
      operands.push_back(expression());
      ++spec.partitions;
    } while (eat(","));
  }
  if (eat("ORDER")) {
    expect("BY");
    do {
      operands.push_back(expression());
      WindowOrdering order;
      if (eat("DESC"))
        order.descending = true;
      else
        order.explicitAscending = eat("ASC");
      if (eat("NULLS")) {
        if (!nullOrdering())
          fail("NULLS FIRST/LAST is not supported by this dialect");
        if (eat("FIRST"))
          order.nulls = "FIRST";
        else {
          expect("LAST");
          order.nulls = "LAST";
        }
      }
      spec.ordering.push_back(std::move(order));
    } while (eat(","));
  }
  if (at("ROWS") || at("RANGE") || at("GROUPS")) {
    spec.frameUnit = asciiUpper(take().value);
    if (spec.frameUnit == "GROUPS" &&
        (dmlGrammar().mysql || ddlGrammar().sqlserver || ddlGrammar().oracle))
      fail("GROUPS window frames are not supported in this dialect baseline");
    spec.between = eat("BETWEEN");
    auto bound = [&] {
      if (eat("UNBOUNDED")) {
        if (eat("PRECEDING"))
          return WindowBound::UnboundedPreceding;
        expect("FOLLOWING");
        return WindowBound::UnboundedFollowing;
      }
      if (eat("CURRENT")) {
        expect("ROW");
        return WindowBound::CurrentRow;
      }
      if (ddlGrammar().sqlserver && spec.frameUnit=="RANGE")
        fail("SQL Server RANGE does not allow numeric offsets");
      operands.push_back(expression(61));
      if (eat("PRECEDING"))
        return WindowBound::Preceding;
      expect("FOLLOWING");
      return WindowBound::Following;
    };
    spec.start = bound();
    if (spec.between) {
      expect("AND");
      spec.end = bound();
    }
    if (spec.start == WindowBound::UnboundedFollowing ||
        spec.end == WindowBound::UnboundedPreceding ||
        (!spec.between && spec.start == WindowBound::Following) ||
        (spec.between &&
         static_cast<int>(spec.start) > static_cast<int>(spec.end)))
      fail("invalid window frame bound order");
  }
  if (eat("EXCLUDE")) {
    if(context_.dialect!=Dialect::PostgreSQL && context_.dialect!=Dialect::SQLite)
      fail("EXCLUDE window frames are unavailable in this dialect baseline",DiagnosticCode::UnsupportedSyntax);
    if(spec.frameUnit.empty())fail("EXCLUDE requires an explicit window frame");
    if(eat("NO")){expect("OTHERS");spec.exclusion=WindowExclusion::NoOthers;}
    else if(eat("CURRENT")){expect("ROW");spec.exclusion=WindowExclusion::CurrentRow;}
    else if(eat("GROUP"))spec.exclusion=WindowExclusion::Group;
    else if(eat("TIES"))spec.exclusion=WindowExclusion::Ties;
    else fail("expected NO OTHERS, CURRENT ROW, GROUP or TIES after EXCLUDE");
  }
  expect(")");
  if(context_.dialect==Dialect::PostgreSQL)
    result.source=WindowSource{result.parent?windowName(*result.parent):std::string{},false,result.spec,result.keys.size()};
  return result;
}
bool Parser::deferWindowError(DiagnosticCode code,SourceRange range,std::string message) {
  if(context_.dialect!=Dialect::PostgreSQL) return false;
  if(analysisDiagnostics_.size()<std::max<std::size_t>(1,context_.options.limits.diagnostics))
    analysisDiagnostics_.push_back({code,range,std::move(message)});
  return true;
}
Parser::PendingWindow Parser::inheritWindow(PendingWindow child,
                                            const PendingWindow &parent) {
  const auto location=child.parent?child.parent->range:previous().range;
  if (child.spec.partitions) {
    if(deferWindowError(DiagnosticCode::InvalidQueryShape,location,"a copied window cannot override PARTITION BY")) return parent;
    fail("a copied window cannot override PARTITION BY",
         DiagnosticCode::InvalidQueryShape);
  }
  if (!parent.spec.frameUnit.empty() && deferWindowError(DiagnosticCode::InvalidQueryShape,location,
        "a window with a frame cannot be copied; reference it directly with OVER name")) {
    // Retain a bounded recovery shape so the parser can finish recognizing
    // the statement. Compilation stops before lowering this invalid expansion.
    return parent;
  }
  if (!parent.spec.frameUnit.empty())
    fail("a window with a frame cannot be copied; reference it directly with "
         "OVER name",
         DiagnosticCode::InvalidQueryShape);
  if (!child.spec.ordering.empty() && !parent.spec.ordering.empty()) {
    if(deferWindowError(DiagnosticCode::InvalidQueryShape,location,"a copied window cannot override ORDER BY")) return parent;
    fail("a copied window cannot override ORDER BY",
         DiagnosticCode::InvalidQueryShape);
  }
  PendingWindow out = parent;
  out.parent.reset();
  if (!child.spec.ordering.empty())
    out.spec.ordering = child.spec.ordering;
  out.spec.frameUnit = child.spec.frameUnit;
  out.spec.between = child.spec.between;
  out.spec.start = child.spec.start;
  out.spec.end = child.spec.end;
  out.spec.exclusion = child.spec.exclusion;
  out.keys.insert(out.keys.end(), child.keys.begin(), child.keys.end());
  return out;
}
void Parser::rowMatchClause(TableSource &source) {
  if (context_.dialect!=Dialect::Oracle || !eat("MATCH_RECOGNIZE")) return;
  if (source.alias || source.name.empty())
    fail("MATCH_RECOGNIZE requires a direct named input in this subset",DiagnosticCode::UnsupportedSyntax);
  const auto start=previous().range.begin;
  RowMatchSyntax match;
  expect("(");
  if (eat("PARTITION")) {
    expect("BY");
    match.partition=namePart();
  }
  expect("ORDER");
  expect("BY");
  match.order=namePart();
  match.descending=eat("DESC");
  if (!match.descending) eat("ASC");
  if (eat("NULLS")) {
    if (eat("FIRST")) match.nullOrdering="FIRST";
    else { expect("LAST"); match.nullOrdering="LAST"; }
  }
  expect("MEASURES");
  expect("COUNT");
  expect("(");
  expect("*");
  expect(")");
  expect("AS");
  match.measure=namePart();
  if (eat("ONE")) {
    expect("ROW");
    expect("PER");
    expect("MATCH");
  }
  if (eat("AFTER")) {
    expect("MATCH");
    expect("SKIP");
    expect("PAST");
    expect("LAST");
    expect("ROW");
  }
  expect("PATTERN");
  expect("(");
  match.variable=namePart();
  expect("+");
  expect(")");
  expect("DEFINE");
  const auto definition=namePart();
  expect("AS");
  const auto reference=namePart();
  if (windowName(definition)!=windowName(match.variable) ||
      windowName(reference)!=windowName(match.variable))
    fail("DEFINE must bind the variable declared by the single row pattern");
  expect(".");
  match.testColumn=namePart();
  if (!at("=") && !at("<>") && !at("<") && !at(">") && !at("<=") && !at(">="))
    fail("row pattern DEFINE requires a direct numeric comparison");
  match.comparison=take().value;
  const bool negative=eat("-");
  if (!negative) eat("+");
  if (current().kind!=TokenKind::Number || current().value.size()>18 ||
      current().value.find_first_not_of("0123456789")!=std::string::npos)
    fail("row pattern comparison requires a bounded integer literal");
  const auto magnitude=std::stoll(take().value);
  match.threshold=negative?-magnitude:magnitude;
  expect(")");
  match.origin={start,previous().range.end};
  source.rowMatch=std::move(match);
  source.alias=alias(true);
}
ExprId Parser::window(ExprId base) {
  if ((context_.ast.expression(base).jsonConstructor || context_.ast.expression(base).kind==ExprKind::Convert) &&
      (at("OVER") || at("FILTER") || at("WITHIN")))
    fail("scalar JSON constructors and conversions do not accept aggregate/window clauses");
  const bool withinGroup=at("WITHIN") && at("GROUP",1);
  if (withinGroup) {
    auto call=context_.ast.expression(base);
    const auto isName=[&](const NamePart &part,std::string_view expected) {
      return canonicalIdentifier(context_.dialect,
          std::string(context_.ast.identifier(part.identifier)),part.quoted)==expected;
    };
    if (context_.dialect!=Dialect::PostgreSQL || call.kind!=ExprKind::Call ||
        call.name.empty() || call.name.size()>2 ||
        (call.name.size()==2 && !isName(call.name.front(),"pg_catalog")))
      fail("WITHIN GROUP requires a supported PostgreSQL aggregate",DiagnosticCode::UnsupportedSyntax);
    const auto &name=call.name.back();
    const bool mode=isName(name,"mode");
    if (!mode && !isName(name,"percentile_disc") && !isName(name,"percentile_cont"))
      fail("unknown PostgreSQL ordered-set aggregate in this frontend",DiagnosticCode::UnsupportedSyntax);
    const bool star=call.operands.size()==1 &&
        context_.ast.expression(call.operands.front()).kind==ExprKind::Star &&
        context_.ast.expression(call.operands.front()).name.empty();
    if (call.distinct)
      fail("ordered-set aggregates do not accept DISTINCT");
    if (mode && !call.operands.empty() && !star)
      fail("MODE takes no direct arguments before WITHIN GROUP");
    if (!mode && (call.operands.size()!=1 || star))
      fail("percentile requires one direct fraction argument");
    take();
    take();
    expect("(");
    expect("ORDER");
    expect("BY");
    const auto key=expression();
    requireScalar(key);
    WindowOrdering ordering;
    ordering.descending=eat("DESC");
    if (!ordering.descending) eat("ASC");
    if (eat("NULLS")) {
      if (eat("FIRST")) ordering.nulls="FIRST";
      else { expect("LAST"); ordering.nulls="LAST"; }
    }
    expect(")");
    if (mode) call.operands.clear();
    call.operands.push_back(key);
    call.withinGroup=std::make_shared<const WindowOrdering>(std::move(ordering));
    call.range.end=previous().range.end;
    base=node(std::move(call));
  }
  const bool withinDistinct=context_.dialect==Dialect::PostgreSQL &&
      context_.options.calciteExtensions && at("WITHIN") && at("DISTINCT",1);
  if (withinDistinct) {
    take();
    take();
    auto call=context_.ast.expression(base);
    if (call.kind!=ExprKind::Call || call.distinct || call.name.size()!=1 ||
        call.name.front().quoted ||
        asciiUpper(context_.ast.identifier(call.name.front().identifier))!="COUNT" ||
        call.operands.size()!=1 || context_.ast.expression(call.operands.front()).kind!=ExprKind::Star)
      fail("Calcite WITHIN DISTINCT currently supports COUNT(*)",DiagnosticCode::UnsupportedSyntax);
    expect("(");
    const auto start=current().range.begin;
    std::vector<ExprId> keys;
    do {
      if (keys.size()>=8)
        fail("WITHIN DISTINCT key count exceeds eight",DiagnosticCode::ResourceLimit);
      const auto key=expression();
      const auto kind=context_.ast.expression(key).kind;
      if (kind!=ExprKind::Name && kind!=ExprKind::Number && kind!=ExprKind::Cast)
        fail("WITHIN DISTINCT requires an integer column, literal or cast",DiagnosticCode::UnsupportedSyntax);
      keys.push_back(key);
    } while (eat(","));
    expect(")");
    // A native composite is non-NULL for COUNT even when all its fields
    // are NULL. Therefore a nullable key retains its own distinct bucket.
    const SourceRange range{start,previous().range.end};
    call.operands={node({ExprKind::Row,range,"ROW",{},std::move(keys)})};
    call.name.insert(call.name.begin(),{context_.ast.intern("pg_catalog"),false,call.range});
    call.range.end=range.end;
    call.value="withinDistinct";
    call.distinct=true;
    base=node(std::move(call));
  }
  if (eat("FILTER")) {
    if (context_.dialect != Dialect::PostgreSQL &&
        context_.dialect != Dialect::SQLite)
      fail("FILTER aggregate clauses are unavailable in this dialect baseline");
    if (context_.ast.expression(base).kind != ExprKind::Call)
      fail("FILTER requires an aggregate call");
    expect("(");
    expect("WHERE");
    auto predicate = expression();
    expect(")");
    base =
        node({ExprKind::AggregateFilter,
              {context_.ast.expression(base).range.begin, previous().range.end},
              "",
              {},
              {base, predicate}});
  }
  if (!eat("OVER"))
    return base;
  if (withinGroup)
    fail("OVER is unavailable for this ordered-set aggregate");
  if (withinDistinct)
    fail("WITHIN DISTINCT does not support OVER",DiagnosticCode::UnsupportedSyntax);
  if (context_.ast.expression(base).kind != ExprKind::Call &&
      context_.ast.expression(base).kind != ExprKind::AggregateFilter)
    fail("OVER requires a function call");
  PendingWindow pending;
  bool copy = at("(");
  if (copy)
    pending = windowBody();
  else {
    if (!namedWindowsAllowed())
      fail("named windows require a newer dialect/version baseline",
           DiagnosticCode::UnsupportedSyntax);
    pending.parent = namePart();
    if(context_.dialect==Dialect::PostgreSQL)
      pending.source=WindowSource{windowName(*pending.parent),true,{},0};
  }
  std::vector<ExprId> operands{base};
  operands.insert(operands.end(), pending.keys.begin(), pending.keys.end());
  Expr result{ExprKind::Window,
              {context_.ast.expression(base).range.begin, previous().range.end},
              "",
              {},
              std::move(operands)};
  result.window = pending.spec;
  result.windowSource = pending.source;
  if (pending.parent)
    result.value = "window:" + windowName(*pending.parent);
  const auto id = node(std::move(result));
  if (pending.parent) {
    if (windowScopes_.empty())
      fail("named window reference outside SELECT",
           DiagnosticCode::InvalidQueryShape);
    windowScopes_.back()->references.push_back({id, std::move(pending), copy});
  }
  return id;
}
void Parser::windowClause(Statement &) {
  if (!eat("WINDOW"))
    return;
  if (!namedWindowsAllowed())
    fail("WINDOW clause is not enabled in this dialect/version baseline",
         DiagnosticCode::UnsupportedSyntax);
  auto *scope = windowScopes_.back().get();
  do {
    if (scope->names.size() >= 1024)
      fail("named window count limit exceeded", DiagnosticCode::ResourceLimit);
    auto name = namePart();
    const auto key = windowName(name);
    expect("AS");
    auto definition = windowBody();
    if (definition.parent && !isMySqlDialect(context_.dialect) &&
        !scope->definitions.count(windowName(*definition.parent))) {
      if(!deferWindowError(DiagnosticCode::InvalidQueryShape,definition.parent->range,
                          "copied window must refer to an earlier WINDOW definition"))
        fail("copied window must refer to an earlier WINDOW definition",DiagnosticCode::InvalidQueryShape);
    }
    if (!scope->definitions.emplace(key, std::move(definition)).second) {
      if(deferWindowError(DiagnosticCode::DuplicateAlias,name.range,"duplicate window name: "+key)) continue;
      context_.report(DiagnosticCode::DuplicateAlias, name.range,
                      "duplicate window name: " + key);
      throw ParseFailure{};
    }
    scope->names.push_back(name);
  } while (eat(","));
}
void Parser::resolveWindows(Statement &statement) {
  auto *scope = windowScopes_.back().get();
  std::map<std::string, int> state;
  std::function<PendingWindow(const NamePart &, std::size_t)> resolve;
  auto charge = [&](std::size_t count) {
    if (count > context_.options.limits.nodes - expandedWindowOperands_)
      fail("named window expansion limit exceeded",
           DiagnosticCode::ResourceLimit);
    expandedWindowOperands_ += count;
  };
  resolve = [&](const NamePart &name, std::size_t depth) -> PendingWindow {
    checkStop(context_);
    const auto key = windowName(name);
    auto found = scope->definitions.find(key);
    if (found == scope->definitions.end()) {
      if(deferWindowError(DiagnosticCode::InvalidQueryShape,name.range,"window not found in this SELECT scope: "+key)) return {};
      context_.report(DiagnosticCode::InvalidQueryShape, name.range,
                      "window not found in this SELECT scope: " + key);
      throw ParseFailure{};
    }
    if (depth > context_.options.limits.nesting)
      fail("named window inheritance depth exceeded",
           DiagnosticCode::ResourceLimit);
    if (state[key] == 1) {
      if(deferWindowError(DiagnosticCode::InvalidQueryShape,name.range,"cyclic named window inheritance: "+key)) return {};
      context_.report(DiagnosticCode::InvalidQueryShape, name.range,
                      "cyclic named window inheritance: " + key);
      throw ParseFailure{};
    }
    if (state[key] == 2)
      return found->second;
    state[key] = 1;
    auto value = found->second;
    const auto source=value.source;
    if (value.parent)
      value = inheritWindow(value, resolve(*value.parent, depth + 1));
    value.source=source;
    charge(value.keys.size());
    found->second = value;
    state[key] = 2;
    return value;
  };
  for (const auto &name : scope->names) {
    auto value = resolve(name, 0);
    statement.namedWindows.push_back({name, value.spec, value.keys,value.source});
  }
  for (const auto &ref : scope->references) {
    auto value = resolve(*ref.window.parent, 0);
    if (ref.copy)
      value = inheritWindow(ref.window, value);
    charge(value.keys.size());
    auto &expression = context_.ast.mutableExpression(ref.expression);
    const auto base = expression.operands.front();
    expression.window = value.spec;
    expression.operands = {base};
    expression.operands.insert(expression.operands.end(), value.keys.begin(),
                               value.keys.end());
  }
}

bool Parser::queryAhead(std::size_t ahead) const {
  while (at("(", ahead) && ahead < context_.options.limits.nesting)
    ++ahead;
  return at("SELECT", ahead) || at("WITH", ahead) ||
         (context_.options.extension == SqlExtension::DuckDb && at("FROM", ahead)) || tableQueryAhead(ahead) ||
         valuesQueryAhead(ahead);
}
bool Parser::tableQueryAhead(std::size_t ahead) const {
  return (context_.dialect == Dialect::PostgreSQL || context_.dialect == Dialect::MySQL) &&
         at("TABLE", ahead);
}
bool Parser::valuesQueryAhead(std::size_t ahead) const {
  return (context_.dialect == Dialect::PostgreSQL || context_.dialect == Dialect::SQLite ||
          context_.dialect == Dialect::MySQL) && at("VALUES", ahead);
}
int Parser::setPrecedence() const {
  if (at("INTERSECT"))
    return context_.dialect == Dialect::Oracle ||
                   context_.dialect == Dialect::SQLite
               ? 10
               : 20;
  if (at("UNION") || at("EXCEPT") || at("MINUS"))
    return 10;
  return 0;
}
Statement Parser::setTail(Statement left, int minimum) {
  while (setPrecedence() && setPrecedence() >= minimum) {
    const auto precedence = setPrecedence();
    const auto token = take();
    const auto op = asciiUpper(token.value);
    if (op == "MINUS" && context_.dialect != Dialect::Oracle)
      fail("MINUS set operator is only supported in the Oracle baseline");
    if (op == "EXCEPT" && context_.dialect == Dialect::Oracle)
      fail("Oracle baseline uses MINUS instead of EXCEPT",
           DiagnosticCode::UnsupportedSyntax);
    const auto kind = op == "UNION"       ? SetKind::Union
                      : op == "INTERSECT" ? SetKind::Intersect
                                          : SetKind::Except;
    const bool all = eat("ALL");
    if (at("DISTINCT")) {
      if (context_.dialect == Dialect::Oracle ||
          context_.dialect == Dialect::SQLServer)
        fail("explicit DISTINCT on set operators is not supported in this "
             "baseline");
      take();
    }
    if (all && kind != SetKind::Union &&
        (context_.dialect == Dialect::Oracle ||
         context_.dialect == Dialect::SQLServer ||
         context_.dialect == Dialect::SQLite))
      fail("ALL is not supported for this set operator in the current baseline",
           DiagnosticCode::UnsupportedSyntax);
    Statement right;
    if (eat("(")) {
      if (context_.dialect == Dialect::SQLite)
        fail("SQLite set operands require a SELECT/derived-query wrapper, not "
             "direct parentheses",
             DiagnosticCode::UnsupportedSyntax);
      right = select(true);
      expect(")");
    } else {
      if (!at("SELECT") && !tableQueryAhead() && !valuesQueryAhead())
        fail("expected query operand after set operator");
      right = select(false, true, true);
    }
    if (setPrecedence() > precedence)
      right = setTail(std::move(right), precedence + 1);
    Statement parent;
    parent.kind = StatementKind::Select;
    parent.range = {left.range.begin, right.range.end};
    SetOperation set;
    set.kind = kind;
    set.all = all;
    set.origin = token.range;
    set.left = storeQuery(std::move(left));
    set.right = storeQuery(std::move(right));
    parent.setOperation = set;
    syntaxNode();
    left = std::move(parent);
  }
  return left;
}

ExprId Parser::caseExpression(std::size_t begin) {
  std::vector<ExprId> operands;
  const bool simple = !at("WHEN");
  if (simple)
    operands.push_back(expression());
  expect("WHEN");
  do {
    operands.push_back(expression());
    expect("THEN");
    operands.push_back(expression());
  } while (eat("WHEN"));
  const bool hasElse = eat("ELSE");
  if (hasElse)
    operands.push_back(expression());
  expect("END");
  Expr e{ExprKind::Case,
         {begin, previous().range.end},
         simple ? "simple" : "searched",
         {},
         std::move(operands)};
  e.explicitElse = hasElse;
  return node(std::move(e));
}
} // namespace navidog::sql::compiler
