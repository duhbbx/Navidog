#include "frontends.hpp"
#include "../postgresql_keywords.hpp"
#include <algorithm>
#include <array>
#include <charconv>
#include <set>
namespace navidog::sql::compiler {
int PostgreSQLParser::optionInteger(int max) {
  const bool negative = eat("-");
  if (!negative) eat("+");
  const auto &token = current();
  if (token.kind != TokenKind::Number) fail("expected an integer PostgreSQL option value");
  const auto *end = token.value.data() + token.value.size();
  int value = 0;
  const auto parsed = std::from_chars(token.value.data(), end, value);
  if (parsed.ec != std::errc{} || parsed.ptr != end || value > max || (negative && value != 0))
    fail("PostgreSQL option integer is outside its accepted range", DiagnosticCode::InvalidQueryShape);
  take();
  return value;
}
bool PostgreSQLParser::optionBoolean() {
  if (at(",") || at(")")) return true;
  if (current().kind == TokenKind::Number || at("+") || at("-")) return optionInteger(1) != 0;
  const auto word = asciiUpper(current().value);
  if ((current().kind != TokenKind::Word && current().kind != TokenKind::String &&
       current().kind != TokenKind::Identifier) ||
      (word != "TRUE" && word != "ON" && word != "FALSE" && word != "OFF"))
    fail("expected TRUE, FALSE, ON, OFF, 0, or 1 for this PostgreSQL option");
  take();
  return word == "TRUE" || word == "ON";
}
Statement Parser::copyStatement() {
  fail("COPY requires the PostgreSQL frontend", DiagnosticCode::UnsupportedSyntax);
}
Statement PostgreSQLParser::copyStatement() {
  expect("COPY");
  Statement result;
  result.kind = StatementKind::Copy;
  auto copy = std::make_shared<CopySyntax>();
  const auto columns = [&] {
    std::vector<NamePart> list;
    do {
      syntaxNode();
      list.push_back(namePart());
    } while (eat(","));
    expect(")");
    return list;
  };
  if (eat("(")) {
    result.source = nestedQuery();
    expect(")");
  } else {
    TableSource target;
    target.range.begin = current().range.begin;
    target.name = qualifiedName();
    target.range.end = previous().range.end;
    if (target.name.size() > 3) fail("COPY table name has too many qualifiers");
    result.target = std::move(target);
    if (eat("(")) copy->columns = columns();
  }
  if (eat("FROM")) {
    if (result.source) fail("COPY FROM requires a table destination");
  } else {
    expect("TO");
    copy->direction = CopyDirection::To;
  }
  if (current().kind == TokenKind::String || at("PROGRAM"))
    fail("COPY syntax support covers client STDIN and STDOUT transfers", DiagnosticCode::UnsupportedSyntax);
  expect(copy->direction == CopyDirection::From ? "STDIN" : "STDOUT");
  const bool with = eat("WITH");
  if (with || at("(")) {
    expect("(");
    std::set<std::string> seen;
    const auto text = [&](bool nameAllowed) {
      const auto &arg = current();
      if (arg.kind != TokenKind::String && (!nameAllowed || (arg.kind != TokenKind::Word && arg.kind != TokenKind::Identifier)))
        fail("expected a COPY option value");
      const auto token = take();
      return token.kind == TokenKind::Word ? canonicalIdentifier(Dialect::PostgreSQL, token.value, false) : token.value;
    };
    do {
      syntaxNode();
      if (current().kind != TokenKind::Word && current().kind != TokenKind::Identifier)
        fail("expected a COPY option");
      if (current().kind == TokenKind::Identifier &&
          current().value != canonicalIdentifier(Dialect::PostgreSQL, current().value, false))
        fail("unrecognized PostgreSQL COPY option", DiagnosticCode::InvalidQueryShape);
      const auto name = asciiUpper(current().value);
      if (!seen.insert(name).second) fail("COPY option appears more than once", DiagnosticCode::InvalidQueryShape);
      take();
      if (name == "FORMAT") {
        const auto format = text(true);
        if (format == "text") copy->format = CopyFormat::Text;
        else if (format == "csv") copy->format = CopyFormat::Csv;
        else if (format == "binary") copy->format = CopyFormat::Binary;
        else fail("unknown PostgreSQL COPY format", DiagnosticCode::InvalidQueryShape);
      } else if (name == "FREEZE") copy->freeze = optionBoolean();
      else if (name == "HEADER") {
        if (at("MATCH")) { take(); copy->header = CopyHeader::Match; }
        else copy->header = optionBoolean() ? CopyHeader::Enabled : CopyHeader::Disabled;
      } else if (name == "DELIMITER") copy->delimiter = text(false);
      else if (name == "NULL") copy->nullMarker = text(false);
      else if (name == "QUOTE") copy->quote = text(false);
      else if (name == "ESCAPE") copy->escape = text(false);
      else if (name == "ENCODING") copy->encoding = text(true);
      else if (name == "FORCE_QUOTE" || name == "FORCE_NOT_NULL" || name == "FORCE_NULL") {
        if (name == "FORCE_QUOTE" && eat("*")) copy->forceQuoteAll = true;
        else {
          expect("(");
          auto names = columns();
          if (name == "FORCE_QUOTE") copy->forceQuote = std::move(names);
          else if (name == "FORCE_NOT_NULL") copy->forceNotNull = std::move(names);
          else copy->forceNull = std::move(names);
        }
      } else fail("COPY option is not supported by the PostgreSQL 15 frontend", DiagnosticCode::UnsupportedSyntax);
    } while (eat(","));
    expect(")");
  }
  if (eat("WHERE")) {
    if (copy->direction == CopyDirection::To) fail("COPY WHERE requires COPY FROM");
    result.where = expression();
    requireScalar(result.where);
  }
  result.copy = std::move(copy);
  return result;
}
std::optional<std::string> copyOptionError(const ModelCopy &options) {
  const bool csv = options.format == CopyFormat::Csv, binary = options.format == CopyFormat::Binary;
  const bool from = options.direction == CopyDirection::From;
  const bool forceQuote = options.forceQuoteAll || !options.forceQuote.empty();
  const bool forceInput = !options.forceNotNull.empty() || !options.forceNull.empty();
  if (options.header == CopyHeader::Match && !from) return "COPY HEADER MATCH requires COPY FROM";
  if (binary && (options.delimiter || options.nullMarker ||
      (options.header && options.header != CopyHeader::Disabled)))
    return "binary COPY does not allow DELIMITER, NULL, or an enabled HEADER";
  if (!csv && (options.quote || options.escape || forceQuote || forceInput))
    return "COPY QUOTE, ESCAPE and FORCE options require CSV format";
  if (forceQuote && from) return "COPY FORCE_QUOTE requires COPY TO";
  if (forceInput && !from) return "COPY FORCE_NULL and FORCE_NOT_NULL require COPY FROM";
  const auto delimiter = options.delimiter.value_or(csv ? "," : "\t");
  const auto quote = options.quote.value_or("\"");
  const auto oneCharacter = [](const std::string &value) {
    return std::count_if(value.begin(), value.end(), [](unsigned char byte) { return (byte & 0xc0) != 0x80; }) == 1;
  };
  const auto endOfLine = [](const auto &value) { return value.find_first_of("\r\n") != std::string::npos; };
  if (!oneCharacter(delimiter) || endOfLine(delimiter))
    return "COPY delimiter must be one character other than a line break";
  if (!csv && delimiter.size() == 1 && std::string_view("\\.abcdefghijklmnopqrstuvwxyz0123456789").find(delimiter[0]) != std::string_view::npos)
    return "COPY text delimiter conflicts with backslash escaping";
  if (csv && (!oneCharacter(quote) || !oneCharacter(options.escape.value_or(quote)) || delimiter == quote))
    return "COPY CSV quote and escape must each be one character, and quote must differ from delimiter";
  if (options.nullMarker && (endOfLine(*options.nullMarker) || options.nullMarker->find(delimiter) != std::string::npos ||
      (csv && options.nullMarker->find(quote) != std::string::npos)))
    return "COPY NULL marker cannot contain a delimiter, quote, or line break";
  return {};
}
Statement Parser::vacuumStatement() {
  fail("VACUUM syntax is only modeled for PostgreSQL", DiagnosticCode::UnsupportedSyntax);
}
Statement PostgreSQLParser::vacuumStatement() {
  expect("VACUUM");
  Statement statement;
  statement.kind = StatementKind::Vacuum;
  auto vacuum = std::make_shared<VacuumSyntax>();
  using Kind = VacuumOptionKind;
  if (eat("(")) {
    do {
      syntaxNode();
      if (current().kind != TokenKind::Word && current().kind != TokenKind::Identifier)
        fail("expected a VACUUM option");
      const auto begin = current().range.begin;
      if (current().kind == TokenKind::Identifier &&
          current().value != canonicalIdentifier(Dialect::PostgreSQL, current().value, false))
        fail("unrecognized PostgreSQL VACUUM option", DiagnosticCode::InvalidQueryShape);
      const auto option = asciiUpper(current().value);
      constexpr std::array kinds{Kind::Full, Kind::Freeze, Kind::Verbose, Kind::Analyze,
          Kind::DisablePageSkipping, Kind::SkipLocked, Kind::IndexCleanup, Kind::ProcessToast,
          Kind::Truncate, Kind::Parallel};
      const auto kind = std::find_if(kinds.begin(), kinds.end(), [&](auto k) {
        return option == vacuumOptionName(k);
      });
      if (kind == kinds.end()) {
        const bool newer = option == "PROCESS_MAIN" || option == "BUFFER_USAGE_LIMIT" ||
            option == "SKIP_DATABASE_STATS" || option == "ONLY_DATABASE_STATS";
        fail("unrecognized PostgreSQL 15 VACUUM option", newer ? DiagnosticCode::UnsupportedSyntax
                                                               : DiagnosticCode::InvalidQueryShape);
      }
      take();
      VacuumOption out{*kind, {}, {begin, 0}};
      if (*kind == Kind::Parallel) {
        out.argument = optionInteger(1024);
      } else if (!at(",") && !at(")")) {
        const auto value = asciiUpper(current().value);
        if (*kind == Kind::IndexCleanup && value == "AUTO" &&
            (current().kind == TokenKind::Word || current().kind == TokenKind::String ||
             current().kind == TokenKind::Identifier)) {
          take();
          out.argument = VacuumIndexCleanup::Auto;
        } else {
          const auto enabled = optionBoolean();
          if (*kind == Kind::IndexCleanup)
            out.argument = enabled ? VacuumIndexCleanup::Enabled : VacuumIndexCleanup::Disabled;
          else out.argument = enabled;
        }
      }
      out.origin.end = previous().range.end;
      vacuum->options.push_back(out);
    } while (eat(","));
    expect(")");
  } else {
    for (auto kind : {Kind::Full, Kind::Freeze, Kind::Verbose, Kind::Analyze}) {
      if (eat(vacuumOptionName(kind)) || (kind == Kind::Analyze && eat("ANALYSE")))
        vacuum->options.push_back({kind, {}, previous().range});
    }
  }
  if (!at(";") && current().kind != TokenKind::End) {
    do {
      syntaxNode();
      const auto begin = current().range.begin;
      if (at("ONLY")) fail("VACUUM ONLY requires PostgreSQL syntax newer than version 15", DiagnosticCode::UnsupportedSyntax);
      auto &target = vacuum->targets.emplace_back();
      target.name = qualifiedName();
      if (at("*")) fail("explicit VACUUM inheritance requires PostgreSQL syntax newer than version 15", DiagnosticCode::UnsupportedSyntax);
      if (target.name.size() > 3) fail("VACUUM table name has too many qualifiers");
      if (eat("(")) {
        do {
          syntaxNode();
          target.columns.push_back(namePart());
        } while (eat(","));
        expect(")");
      }
      target.origin = {begin, previous().range.end};
    } while (eat(","));
  }
  statement.vacuum = std::move(vacuum);
  return statement;
}
bool PostgreSQLParser::nameToken() const {
  if (current().kind==TokenKind::Identifier) return true;
  if (current().kind!=TokenKind::Word) return false;
  const auto kind=postgresKeywordClass(asciiUpper(current().value));
  return kind==PostgreSqlKeywordClass::Ordinary || kind==PostgreSqlKeywordClass::ColumnName;
}
bool PostgreSQLParser::functionNameToken() const {
  if (current().kind==TokenKind::Identifier) return true;
  if (current().kind!=TokenKind::Word) return false;
  const auto kind=postgresKeywordClass(asciiUpper(current().value));
  return kind==PostgreSqlKeywordClass::Ordinary || kind==PostgreSqlKeywordClass::TypeFunction;
}
bool PostgreSQLParser::typeNameToken() const {
  if (functionNameToken()) return true;
  if (current().kind!=TokenKind::Word) return false;
  static const std::set<std::string> systemTypes={
      "BIGINT","BIT","BOOLEAN","CHAR","CHARACTER","DEC","DECIMAL","FLOAT","INT",
      "INTEGER","INTERVAL","JSON","NCHAR","NUMERIC","REAL","SMALLINT","TIME","TIMESTAMP","VARCHAR"};
  return systemTypes.count(asciiUpper(current().value));
}
bool PostgreSQLParser::bareProjectionAliasToken() const {
  if (current().kind==TokenKind::Identifier) return true;
  if (current().kind!=TokenKind::Word) return false;
  // PostgreSQL kwlist.h labels these AS_LABEL. Other words, including many
  // reserved expression keywords, are legal BareColLabel after a projection.
  return !postgresAliasNeedsAs(asciiUpper(current().value));
}
Parser::DdlGrammar PostgreSQLParser::ddlGrammar() const {
  DdlGrammar g;
  g.postgres = true;
  return g;
}
Parser::DmlGrammar PostgreSQLParser::dmlGrammar() const {
  DmlGrammar g;
  g.updateFrom = true;
  g.deleteUsing = true;
  g.conflict = true;
  g.returning = true;
  return g;
}
ExprId PostgreSQLParser::postfix(ExprId base) {
  for (;;) {
    const auto &input=context_.ast.expression(base);
    const auto begin=input.range.begin;
    if (eat("::")) {
      base=castType(base,begin);
      continue;
    }
    if (!at("[")) return base;
    if (input.kind!=ExprKind::Name && input.kind!=ExprKind::Parameter &&
        input.kind!=ExprKind::Subquery && previous().range.end<=input.range.end)
      fail("a PostgreSQL subscript on this expression requires parentheses");
    Expr access{ExprKind::Subscript,{begin,begin},{},{},{base}};
    while (eat("[")) {
      if (access.value.size()>=context_.options.limits.nesting)
        fail("subscript nesting limit exceeded",DiagnosticCode::ResourceLimit);
      const bool lower=!at(":");
      if (lower) access.operands.push_back(expression());
      auto axis=SubscriptKind::Index;
      if (eat(":")) {
        const bool upper=!at("]");
        if (upper) access.operands.push_back(expression());
        axis=lower?(upper?SubscriptKind::Range:SubscriptKind::Lower):
                   (upper?SubscriptKind::Upper:SubscriptKind::Full);
      }
      access.value.push_back(static_cast<char>(axis));
      expect("]");
    }
    access.range.end=previous().range.end;
    base=node(std::move(access));
  }
}
bool PostgreSQLParser::paginationPrimary(std::size_t begin,ExprId id) const {
  const auto symbol=[&](std::size_t i,std::string_view value) {
    return i<cursor_ && tokens_[i].kind==TokenKind::Symbol && tokens_[i].value==value;
  };
  std::size_t parentheses=0,brackets=0;
  bool enclosed=false,outerClosed=false;
  for (auto i=begin;i<cursor_;++i) {
    if (symbol(i,"(")) ++parentheses;
    else if (symbol(i,")")) {
      if (parentheses) --parentheses;
      if (!parentheses && symbol(begin,"(") && !outerClosed) {
        enclosed=i==cursor_-1;
        outerClosed=true;
      }
    } else if (symbol(i,"[")) ++brackets;
    else if (symbol(i,"]") && brackets) --brackets;
    else if (!parentheses && !brackets && symbol(i,"::")) return false;
  }
  if (enclosed) return true;
  const auto &e=context_.ast.expression(id);
  switch (e.kind) {
  case ExprKind::Binary:case ExprKind::LikeEscape:case ExprKind::Between:
  case ExprKind::In:case ExprKind::IsNull:case ExprKind::TruthTest:case ExprKind::Quantified:
    return false;
  case ExprKind::Unary:
    return cursor_==begin+2 && (symbol(begin,"+") || symbol(begin,"-")) &&
        tokens_[begin+1].kind==TokenKind::Number;
  case ExprKind::Cast:
    return tokens_[begin].kind==TokenKind::Word && asciiUpper(tokens_[begin].value)=="CAST";
  default:return true;
  }
}
ExprId PostgreSQLParser::paginationValue(bool restricted) {
  const auto begin=cursor_;
  const auto value=expression();
  requireScalar(value);
  if (at(",")) fail("PostgreSQL pagination uses OFFSET rather than a comma-separated count");
  if (restricted && !paginationPrimary(begin,value))
    fail("FETCH and OFFSET ROWS require a primary expression, parentheses or a signed numeric literal");
  return value;
}
void PostgreSQLParser::pagination(SelectStatement &s) {
  const auto limit = [&] {
    if (!eat("LIMIT")) return false;
    if (!eat("ALL")) s.limit.count = paginationValue();
    s.limit.syntax = LimitSyntax::LimitOffset;
    return true;
  };
  const auto offset = [&] {
    if (!eat("OFFSET")) return;
    const auto begin=cursor_;
    s.limit.offset = paginationValue();
    if ((at("ROW") || at("ROWS")) && !paginationPrimary(begin,s.limit.offset))
      fail("OFFSET ROWS requires a primary or parenthesized expression");
    if (!eat("ROW")) eat("ROWS");
    if (s.limit.syntax == LimitSyntax::None) s.limit.syntax = LimitSyntax::LimitOffset;
  };
  if (limit()) {
    offset();
    return;
  }
  offset();
  if (limit()) return;
  if (eat("FETCH")) {
    if (!eat("FIRST")) expect("NEXT");
    const bool omitted=(at("ROW") || at("ROWS")) && (at("ONLY",1) || at("WITH",1));
    s.limit.count=omitted?node({ExprKind::Number,current().range,"1",{}, {}}):paginationValue(true);
    if (!eat("ROW")) expect("ROWS");
    if (eat("WITH")) {expect("TIES");s.limit.withTies=true;}
    else expect("ONLY");
    s.limit.syntax=LimitSyntax::Fetch;
    if (!s.limit.offset) offset();
  }
}
std::pair<std::string, int> PostgreSQLParser::binaryOperator() const {
  if (at("ILIKE"))
    return {"ILIKE", 60};
  if (at("||"))
    return {"CONCAT", 70};
  if (at("%"))
    return {"%", 90};
  return Parser::binaryOperator();
}
int PostgreSQLParser::precedence(std::string_view op) const {
  if (op == "BETWEEN" || op == "IN" || op == "LIKE" || op == "ILIKE")
    return 60;
  return Parser::precedence(op);
}
} // namespace navidog::sql::compiler
