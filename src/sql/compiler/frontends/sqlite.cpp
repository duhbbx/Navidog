#include "frontends.hpp"
#include <unordered_set>
namespace navidog::sql::compiler {
Statement Parser::pragmaStatement() {
  fail("PRAGMA requires the SQLite frontend", DiagnosticCode::UnsupportedSyntax);
}
Statement Parser::triggerStatement() {
  fail("trigger grammar is unavailable for this dialect", DiagnosticCode::UnsupportedSyntax);
}
Statement SQLiteParser::triggerStatement() {
  Statement result;
  result.kind = StatementKind::Trigger;
  auto trigger = std::make_shared<TriggerSyntax>();
  trigger->drop = eat("DROP");
  if (!trigger->drop) {
    expect("CREATE");
    trigger->temporary = eat("TEMP") || eat("TEMPORARY");
  }
  expect("TRIGGER");
  if (eat("IF")) {
    if (!trigger->drop) expect("NOT");
    expect("EXISTS");
    trigger->ifGuard = true;
  }
  trigger->name = qualifiedName();
  if (trigger->name.size() > 2) fail("SQLite trigger name permits only a database qualifier");
  if (!trigger->drop) {
    if (eat("BEFORE")) trigger->timing = TriggerTiming::Before;
    else if (eat("AFTER")) trigger->timing = TriggerTiming::After;
    else if (eat("INSTEAD")) {
      expect("OF");
      trigger->timing = TriggerTiming::InsteadOf;
    }
    if (eat("INSERT")) trigger->event = TriggerEvent::Insert;
    else if (eat("DELETE")) trigger->event = TriggerEvent::Delete;
    else {
      expect("UPDATE");
      trigger->event = TriggerEvent::Update;
      if (eat("OF")) do {
        syntaxNode();
        trigger->updateColumns.push_back(namePart());
      } while (eat(","));
    }
    expect("ON");
    trigger->target = qualifiedName();
    if (trigger->target.size() > 2) fail("SQLite trigger target permits only a database qualifier");
    trigger->targetOrigin = {trigger->target.front().range.begin, trigger->target.back().range.end};
    if (eat("FOR")) {
      expect("EACH");
      expect("ROW");
      trigger->explicitRow = true;
    }
    if (eat("WHEN")) {
      trigger->when = expression();
      requireScalar(trigger->when);
    }
    expect("BEGIN");
    trigger->compound = true;
    do {
      if (!at("SELECT") && !at("INSERT") && !at("UPDATE") && !at("DELETE"))
        fail("SQLite trigger body requires a SELECT, INSERT, UPDATE or DELETE step");
      if (trigger->body.size() >= context_.options.limits.statements)
        fail("SQLite trigger step limit exceeded", DiagnosticCode::ResourceLimit);
      auto step = statement();
      if (step.target && step.target->name.size() != 1)
        fail("SQLite trigger DML targets must be unqualified", DiagnosticCode::InvalidQueryShape);
      if (!step.returning.empty() || step.insertSource == InsertSource::DefaultValues ||
          ((step.kind == StatementKind::Update || step.kind == StatementKind::Delete) &&
           (!step.orderBy.empty() || step.limit.count || step.limit.offset)))
        fail("SQLite trigger steps do not allow RETURNING, DEFAULT VALUES or UPDATE/DELETE pagination",
             DiagnosticCode::InvalidQueryShape);
      expect(";");
      trigger->body.push_back(context_.ast.addQuery(std::move(step)));
    } while (!eat("END"));
  }
  result.trigger = std::move(trigger);
  return result;
}
void Parser::createVirtualTable(DdlSyntax &) {
  fail("CREATE VIRTUAL TABLE requires the SQLite frontend", DiagnosticCode::UnsupportedSyntax);
}
void SQLiteParser::createVirtualTable(DdlSyntax &ddl) {
  expect("TABLE");
  ddl.object = ObjectKind::Table;
  ifNotExists(ddl);
  ddl.name = qualifiedName();
  if (ddl.name.size() > 2) fail("SQLite virtual table name permits only a database qualifier");
  expect("USING");
  auto module = std::make_shared<BasicVirtualTable<NamePart>>();
  if (current().kind == TokenKind::String) {
    const auto token = take();
    context_.identifierRanges.push_back(token.range);
    module->module = {context_.ast.intern(token.value), true, token.range};
  } else {
    module->module = namePart();
  }
  if (eat("(")) {
    module->argumentList = true;
    if (!eat(")")) {
      do {
        syntaxNode();
        const auto begin = current().range.begin;
        auto end = begin;
        std::size_t depth = 0;
        while (depth || (!at(")") && !at(","))) {
          if (current().kind == TokenKind::End || at(";")) fail("unclosed SQLite module argument list");
          if (at("(")) {
            if (++depth > context_.options.limits.nesting)
              fail("SQLite module argument nesting limit exceeded", DiagnosticCode::ResourceLimit);
          } else if (at(")")) --depth;
          end = take().range.end;
        }
        module->arguments.push_back({context_.source.text().substr(begin,end-begin), {begin,end}});
      } while (eat(","));
      expect(")");
    }
  }
  ddl.virtualTable = std::move(module);
}
std::optional<Diagnostic> validateVirtualTableArgument(std::string_view payload, std::function<bool()> shouldStop) {
  LanguageOptions options;
  options.shouldStop = std::move(shouldStop);
  if (payload.size() > options.limits.sourceBytes)
    return Diagnostic{DiagnosticCode::ResourceLimit, {}, "SQLite module argument byte limit exceeded"};
  Compilation compilation(Dialect::SQLite, std::string(payload), options, "<virtual table module argument>");
  try { lex(compilation, true); } catch (const CompilationAborted &) {}
  if (!compilation.diagnostics.empty()) return compilation.diagnostics.front();
  std::size_t depth = 0;
  for (const auto &token : compilation.tokens) {
    if (options.shouldStop && options.shouldStop())
      return Diagnostic{DiagnosticCode::Stopped, {}, "SQLite module argument validation stopped"};
    if (token.kind == TokenKind::Comment && token.range.end == payload.size() &&
        payload.substr(token.range.begin, 2) == "--")
      return Diagnostic{DiagnosticCode::InvalidQueryShape, {}, "line comment would hide the SQLite module separator"};
    if (token.kind != TokenKind::Symbol) continue;
    if (token.value == "(") {
      if (++depth > options.limits.nesting)
        return Diagnostic{DiagnosticCode::ResourceLimit, {}, "SQLite module argument nesting limit exceeded"};
    } else if (token.value == ")" && depth) --depth;
    else if (token.value == ")" || token.value == ";" || (token.value == "," && !depth))
      return Diagnostic{DiagnosticCode::InvalidQueryShape, {}, "invalid separator or unbalanced SQLite module argument"};
  }
  if (depth) return Diagnostic{DiagnosticCode::InvalidQueryShape, {}, "unclosed SQLite module argument"};
  return {};
}
Statement SQLiteParser::pragmaStatement() {
  expect("PRAGMA");
  Statement statement;
  statement.kind = StatementKind::Pragma;
  auto &pragma = statement.pragma.emplace();
  // These words have no identifier fallback in the PRAGMA name production.
  // ON, DELETE and DEFAULT are additionally allowed as bare argument values.
  const auto pragmaName = [&]() {
    if (current().kind == TokenKind::Identifier || current().kind == TokenKind::String) return true;
    if (current().kind != TokenKind::Word) return false;
    static const std::unordered_set<std::string> reserved{
        "ADD", "ALL", "ALTER", "AND", "AS", "AUTOINCREMENT", "BETWEEN", "CASE", "CHECK",
        "COLLATE", "COMMIT", "CONSTRAINT", "CREATE", "DEFAULT", "DEFERRABLE", "DELETE",
        "DISTINCT", "DROP", "ELSE", "ESCAPE", "EXCEPT", "EXISTS", "FOREIGN", "FROM", "GROUP", "HAVING",
        "IN", "INDEX", "INSERT", "INTERSECT", "INTO", "IS", "ISNULL", "JOIN", "LIMIT", "NOT", "NOTHING",
        "NOTNULL", "NULL", "ON", "OR", "ORDER", "PRIMARY", "REFERENCES", "RETURNING",
        "SELECT", "SET", "TABLE", "THEN", "TO", "TRANSACTION", "UNION", "UNIQUE", "UPDATE", "USING",
        "VALUES", "WHEN", "WHERE"};
    return !reserved.count(asciiUpper(current().value));
  };
  const auto readName = [&]() {
    if (!pragmaName()) fail("expected a SQLite PRAGMA name");
    const auto token = take();
    context_.identifierRanges.push_back(token.range);
    return NamePart{context_.ast.intern(token.value), token.kind != TokenKind::Word, token.range};
  };
  pragma.name.push_back(readName());
  if (eat(".")) pragma.name.push_back(readName());
  if (eat("=")) pragma.style = PragmaArgumentStyle::Assignment;
  else if (eat("(")) pragma.style = PragmaArgumentStyle::Parenthesized;
  else return statement;
  if (current().kind == TokenKind::Number || at("+") || at("-")) {
    const auto begin = current().range.begin;
    std::string sign;
    if (at("+") || at("-")) sign = take().value;
    if (current().kind != TokenKind::Number) fail("expected a signed number in SQLite PRAGMA");
    const auto number = take();
    auto value = node({ExprKind::Number, number.range, number.value, {}, {}});
    if (!sign.empty()) value = node({ExprKind::Unary, {begin,number.range.end}, std::move(sign), {}, {value}});
    pragma.argument = value;
  } else {
    if (!pragmaName() && !at("ON") && !at("DELETE") && !at("DEFAULT"))
      fail("expected a name or signed number in SQLite PRAGMA");
    const auto token = take();
    pragma.argument = node({ExprKind::String, token.range, token.value, {}, {}});
  }
  if (pragma.style == PragmaArgumentStyle::Parenthesized) expect(")");
  return statement;
}
Parser::DdlGrammar SQLiteParser::ddlGrammar() const {
  DdlGrammar g;
  g.sqlite = true;
  return g;
}
Parser::DmlGrammar SQLiteParser::dmlGrammar() const {
  DmlGrammar g;
  g.defaultExpression = false;
  g.updateFrom = true;
  g.conflict = true;
  g.returning = true;
  return g;
}
void SQLiteParser::pagination(SelectStatement &s) {
  if (!eat("LIMIT")) {
    if (at("OFFSET")) fail("SQLite OFFSET requires a preceding LIMIT");
    return;
  }
  const auto value=[&] {
    if (at("ALL")) fail("SQLite LIMIT does not have an ALL keyword form");
    const auto id=expression();
    requireScalar(id);
    return id;
  };
  s.limit.count=value();
  s.limit.syntax=LimitSyntax::LimitOffset;
  if (eat(",")) {
    s.limit.offset=s.limit.count;
    s.limit.count=value();
    s.limit.syntax=LimitSyntax::CommaLimit;
  } else if (eat("OFFSET")) s.limit.offset=value();
  if (at(",") || at("OFFSET")) fail("SQLite pagination contains an extra offset or count");
}
std::pair<std::string, int> SQLiteParser::binaryOperator() const {
  if (at("GLOB"))
    return {"GLOB",60};
  if (at("=="))
    return {"=", precedence("=")};
  if (at("||"))
    return {"CONCAT", 100};
  if (at("%"))
    return {"%", 90};
  return Parser::binaryOperator();
}
int SQLiteParser::precedence(std::string_view op) const {
  if (op == "IS" || op == "=" || op == "!=" || op == "<>" ||
      op == "LIKE" || op == "GLOB" || op == "IN" || op == "BETWEEN")
    return 60;
  if (op == "<" || op == ">" || op == "<=" || op == ">=")
    return 70;
  return Parser::precedence(op);
}
} // namespace navidog::sql::compiler
