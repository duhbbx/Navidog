#include "frontends.hpp"
#include "../sqlserver_keywords.hpp"
namespace navidog::sql::compiler {
bool SQLServerParser::nameToken() const {
  if (current().kind==TokenKind::Word && sqlServerReservedWord(asciiUpper(current().value))) return false;
  return Parser::nameToken();
}
bool SQLServerParser::functionNameToken() const {
  if (current().kind==TokenKind::Word && (at("COALESCE") || at("NULLIF") || at("LEFT") ||
      at("RIGHT") || at("CONTAINS") || at("FREETEXT") || at("CONVERT") || at("TRY_CONVERT"))) return true;
  return Parser::functionNameToken();
}
bool SQLServerParser::contextualIdentifier(std::string_view word) const {
  // RETURNING is an unreserved JSON clause identifier in T-SQL, unlike the
  // PostgreSQL/Oracle DML keyword in the common lexer vocabulary.
  const auto spelling=asciiUpper(std::string(word));
  // TRUE/FALSE are ordinary identifiers in this T-SQL profile, not SQL scalar
  // boolean literals. This also admits them as projection alias names.
  return spelling=="RETURNING" || spelling=="TRUE" || spelling=="FALSE";
}
Parser::DdlGrammar SQLServerParser::ddlGrammar() const {
  DdlGrammar g;
  g.sqlserver = true;
  return g;
}
Parser::DmlGrammar SQLServerParser::dmlGrammar() const {
  DmlGrammar g;
  g.updateFrom = true;
  g.output = true;
  g.qualifiedAssignment = true;
  g.naturalUsing = false;
  return g;
}
void SQLServerParser::selectPrefix(SelectStatement &s) {
  if (!eat("TOP"))
    return;
  bool parenthesized = eat("(");
  if (!parenthesized && current().kind != TokenKind::Number)
    fail("nonconstant TOP requires parentheses");
  // A bare TOP consumes one constant. In TOP 1 +1, +1 is the projection,
  // whereas TOP (1+1) owns the complete parenthesized count expression.
  s.limit.count = parenthesized ? limitValue() : primary();
  if (parenthesized)
    expect(")");
  if (at("PERCENT"))
    fail("TOP PERCENT requires percentage-based row limiting",
         DiagnosticCode::UnsupportedSyntax);
  if (eat("WITH")) {
    expect("TIES");
    s.limit.withTies = true;
  }
  s.limit.syntax = LimitSyntax::Top;
}
ExprId SQLServerParser::limitValue() {
  if (at("ROW") || at("ROWS") || at("ONLY") || at("OFFSET") || at("FETCH") || at(")"))
    fail("expected a SQL Server row-count/offset expression");
  const auto root=expression(51);
  std::vector<ExprId> work{root};
  while (!work.empty()) {
    checkStop(context_);
    const auto &value=context_.ast.expression(work.back());
    work.pop_back();
    const bool scalar=value.kind==ExprKind::Number || value.kind==ExprKind::Parameter;
    const bool sign=value.kind==ExprKind::Unary && (value.value=="+" || value.value=="-");
    const bool arithmetic=value.kind==ExprKind::Binary &&
        (value.value=="+" || value.value=="-" || value.value=="*" || value.value=="/" || value.value=="%");
    if (!scalar && !sign && !arithmetic)
      fail("SQL Server pagination currently covers numeric literals/arithmetic and parameters; other expression scope requires further validation",
           DiagnosticCode::UnsupportedSyntax);
    work.insert(work.end(),value.operands.begin(),value.operands.end());
  }
  return root;
}
void SQLServerParser::pagination(SelectStatement &s) {
  if (s.limit.syntax == LimitSyntax::Top && (at("OFFSET") || at("FETCH")))
    // ScriptDOM recognizes both clauses; SQL Server rejects their combination
    // during analysis (10741). The single-bound owning model cannot retain both.
    fail("combined TOP and OFFSET/FETCH requires separate bounds and is rejected by SQL Server analysis",
         DiagnosticCode::UnsupportedSyntax);
  offsetFetch(s, true);
  if (s.limit.syntax==LimitSyntax::Fetch) {
    std::vector<ExprId> work;
    if (s.limit.count) work.push_back(s.limit.count);
    if (s.limit.offset) work.push_back(s.limit.offset);
    while (!work.empty()) {
      checkStop(context_);
      const auto &value=context_.ast.expression(work.back());
      work.pop_back();
      if (value.kind==ExprKind::Number && value.value.find_first_not_of("0123456789")!=std::string::npos)
        fail("SQL Server OFFSET/FETCH decimal and floating-point coercion requires further validation",DiagnosticCode::UnsupportedSyntax);
      work.insert(work.end(),value.operands.begin(),value.operands.end());
    }
  }
}
std::pair<std::string, int> SQLServerParser::binaryOperator() const {
  if (at("%"))
    return {"%", 90};
  return Parser::binaryOperator();
}
int SQLServerParser::precedence(std::string_view op) const {
  if (op == "BETWEEN" || op == "IN" || op == "LIKE")
    // T-SQL search_condition combines whole predicates with NOT/AND/OR.
    // Giving LIKE the OR binding power would consume AND inside its pattern
    // operand and turn NOT column LIKE pattern into (NOT column) LIKE pattern.
    return 50;
  if (op == "IS")
    return 50;
  if (op == "UNARY")
    return 81;
  return Parser::precedence(op);
}
} // namespace navidog::sql::compiler
