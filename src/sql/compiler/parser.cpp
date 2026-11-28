#include "parser.hpp"
#include "postgresql_keywords.hpp"
#include "sqlserver_syntax.hpp"
#include "navidog/sql/compiler/operator_catalog.hpp"
#include <algorithm>
#include <charconv>
#include <set>
#include <unordered_set>

namespace navidog::sql::compiler {
namespace {
bool reserved(std::string_view value) {
  static const std::unordered_set<std::string> words = {"SELECT",
                                                        "FROM",
                                                        "WHERE",
                                                        "GROUP",
                                                        "BY",
                                                        "HAVING",
                                                        "ORDER",
                                                        "LIMIT",
                                                        "OFFSET",
                                                        "FETCH",
                                                        "FIRST",
                                                        "NEXT",
                                                        "ROW",
                                                        "ROWS",
                                                        "ONLY",
                                                        "TOP",
                                                        "PERCENT",
                                                        "WITH",
                                                        "TIES",
                                                        "DISTINCT",
                                                        "ALL",
                                                        "AS",
                                                        "JOIN",
                                                        "INNER",
                                                        "LEFT",
                                                        "RIGHT",
                                                        "FULL",
                                                        "OUTER",
                                                        "CROSS",
                                                        "ON",
                                                        "USING",
                                                        "NATURAL",
                                                        "AND",
                                                        "OR",
                                                        "XOR",
                                                        "NOT",
                                                        "IN",
                                                        "IS",
                                                        "NULL",
                                                        "TRUE",
                                                        "FALSE",
                                                        "BETWEEN",
                                                        "LIKE",
                                                        "ILIKE",
                                                        "ASC",
                                                        "DESC",
                                                        "NULLS",
                                                        "LAST",
                                                        "UNION",
                                                        "INTERSECT",
                                                        "EXCEPT",
                                                        "MINUS",
                                                        "OVER",
                                                        "PARTITION",
                                                        "WINDOW",
                                                        "QUALIFY",
                                                        "FOR",
                                                        "UPDATE",
                                                        "INTO",
                                                        "RETURNING",
                                                        "CONNECT",
                                                        "START",
                                                        "MODEL",
                                                        "CASE",
                                                        "WHEN",
                                                        "THEN",
                                                        "ELSE",
                                                        "END",
                                                        "EXISTS",
                                                        "LATERAL",
                                                        "COLLATE",
                                                        "ESCAPE",
                                                        "DIV",
                                                        "MOD",
                                                        "REGEXP",
                                                        "RLIKE",
                                                        "MATCH",
                                                        "GLOB",
                                                        "SIMILAR",
                                                        "ANY",
                                                        "SOME",
                                                        "FILTER",
                                                        "WITHIN",
                                                        "VALUES",
                                                        "INSERT",
                                                        "DELETE",
                                                        "CREATE",
                                                        "DROP",
                                                        "ALTER",
                                                        "CAST",
                                                        "TABLE",
                                                        "DATABASE",
                                                        "SCHEMA",
                                                        "ISNULL",
                                                        "NOTNULL",
                                                        "AT",
                                                        "TIME",
                                                        "ZONE",
                                                        "OPERATOR",
                                                        "SQL_CALC_FOUND_ROWS",
                                                        "STRAIGHT_JOIN",
                                                        "SQL_SMALL_RESULT",
                                                        "SQL_BIG_RESULT",
                                                        "SQL_BUFFER_RESULT",
                                                        "HIGH_PRIORITY",
                                                        "LOW_PRIORITY",
                                                        "SQL_NO_CACHE",
                                                        "SQL_CACHE",
                                                        "PRIOR",
                                                        "NOCYCLE",
                                                        "LOCK",
                                                        "SHARE",
                                                        "SKIP",
                                                        "INSERT",
                                                        "DELETE",
                                                        "SET",
                                                        "VALUES",
                                                        "VALUE",
                                                        "DEFAULT",
                                                        "RETURNING",
                                                        "OUTPUT",
                                                        "CONFLICT",
                                                        "DO",
                                                        "NOTHING",
                                                        "DUPLICATE",
                                                        "KEY",
                                                        "CONSTRAINT",
                                                        "IGNORE",
                                                        "LOW_PRIORITY",
                                                        "HIGH_PRIORITY",
                                                        "QUICK",
                                                        "RECURSIVE",
                                                        "ANY",
                                                        "SOME",
                                                        "TRUNCATE",
                                                        "RENAME",
                                                        "COMMENT",
                                                        "VIEW",
                                                        "INDEX",
                                                        "SEQUENCE",
                                                        "PRIMARY",
                                                        "UNIQUE",
                                                        "FOREIGN",
                                                        "REFERENCES",
                                                        "CHECK",
                                                        "ADD",
                                                        "COLUMN",
                                                        "TO",
                                                        "TYPE",
                                                        "CASCADE",
                                                        "RESTRICT",
                                                        "CONSTRAINTS",
                                                        "ENGINE",
                                                        "CHARSET",
                                                        "TABLESPACE",
                                                        "INCLUDE",
                                                        "OPTION",
                                                        "PURGE",
                                                        "IDENTITY",
                                                        "GENERATED",
                                                        "ALWAYS",
                                                        "STORED",
                                                        "VIRTUAL",
                                                        "AUTO_INCREMENT",
                                                        "PERSISTED",
                                                        "INCREMENT",
                                                        "MINVALUE",
                                                        "MAXVALUE",
                                                        "CACHE",
                                                        "CYCLE"};
  return words.count(asciiUpper(value)) != 0;
}
struct DepthGuard {
  std::size_t &depth;
  explicit DepthGuard(std::size_t &d) : depth(d) { ++depth; }
  ~DepthGuard() { --depth; }
};
} // namespace
Parser::Parser(Compilation &c) : context_(c) {
  for (const auto &token : c.tokens)
    if (token.kind != TokenKind::Comment)
      tokens_.push_back(token);
}
void Parser::setTypeTokenStream(const std::vector<Token> &tokens) {
  if(tokens.empty() || tokens.back().kind!=TokenKind::End || tokens.size()-1>context_.options.limits.tokens)
    throw std::invalid_argument("invalid type token stream");
  tokens_.clear();for(const auto &token:tokens)if(token.kind!=TokenKind::Comment)tokens_.push_back(token);
  cursor_=0;
}
BasicTypeSpec<QualifiedName> Parser::parseTypeAt(std::size_t byteOffset) {
  checkStop(context_);
  cursor_=std::size_t(std::lower_bound(tokens_.begin(),tokens_.end(),byteOffset,
    [](const Token &token,std::size_t offset){return token.range.begin<offset;})-tokens_.begin());
  if(cursor_>=tokens_.size())cursor_=tokens_.size()-1;
  return typeSpec();
}
const Token &Parser::current() const { return tokens_.at(cursor_); }
const Token &Parser::previous() const {
  return tokens_.at(cursor_ ? cursor_ - 1 : 0);
}
bool Parser::at(std::string_view value, std::size_t ahead) const {
  const auto &t = tokens_[std::min(cursor_ + ahead, tokens_.size() - 1)];
  return t.kind == TokenKind::Word
             ? asciiUpper(t.value) == value
             : t.kind == TokenKind::Symbol && t.value == value;
}
bool Parser::eat(std::string_view value) {
  if (!at(value))
    return false;
  take();
  return true;
}
Token Parser::take() {
  if ((cursor_ & 255u) == 0)
    checkStop(context_);
  auto t = current();
  if (t.kind != TokenKind::End)
    ++cursor_;
  return t;
}
void Parser::expect(std::string_view value) {
  if (!eat(value))
    fail("expected '" + std::string(value) + "'");
}
[[noreturn]] void Parser::fail(std::string message, DiagnosticCode code) {
  context_.report(code, current().range, std::move(message));
  throw ParseFailure{};
}
bool Parser::nameToken() const {
  return current().kind == TokenKind::Identifier ||
         (current().kind == TokenKind::Word &&
          !(dmlGrammar().mysql && at("XOR")) &&
          (!reserved(current().value) ||
           contextualIdentifier(current().value) ||
           (dmlGrammar().mysql && at("VALUE")) ||
           (context_.dialect == Dialect::SQLite && (at("TRUE") || at("FALSE")))));
}
NamePart Parser::namePart(bool qualified) {
  // MySQL qualified identifiers and PostgreSQL attr_name admit keywords after
  // a dot. Keep other dialects conservative until their name rules are modeled.
  const bool qualifiedKeyword =
      qualified && current().kind == TokenKind::Word &&
      (dmlGrammar().mysql || context_.dialect == Dialect::PostgreSQL);
  if (!nameToken() && !qualifiedKeyword) {
    if (qualified && context_.dialect==Dialect::SQLServer && (at("IDENTITYCOL") || at("ROWGUIDCOL")))
      fail("SQL Server implicit identity/rowguid column references require attribute binding",DiagnosticCode::UnsupportedSyntax);
    fail("expected identifier (quote reserved names)");
  }
  auto token = take();
  context_.identifierRanges.push_back(token.range);
  return {context_.ast.intern(token.value), token.kind == TokenKind::Identifier,
          token.range};
}
QualifiedName Parser::qualifiedName(bool allowStar) {
  QualifiedName name{namePart()};
  while (eat(".")) {
    if (name.size() >= 4)
      fail("at most four name components are supported",
           DiagnosticCode::UnsupportedSyntax);
    if (allowStar && eat("*")) {
      name.push_back({context_.ast.intern("*"), false, previous().range});
      break;
    }
    name.push_back(namePart(true));
  }
  return name;
}
void Parser::groupingClause(Statement &s) {
  using Group=std::vector<ExprId>;using Sets=std::vector<Group>;
  const auto maxSets=std::min<std::size_t>(4096,context_.options.limits.groupingSets);
  const auto maxRefs=std::min<std::size_t>(65536,context_.options.limits.groupingReferences);
  bool advanced=false;
  const bool family=isMySqlDialect(context_.dialect);

  auto bound=[&](const Sets &sets){
    checkStop(context_);std::size_t refs=0;
    if(sets.size()>maxSets)fail("grouping set expansion limit exceeded",DiagnosticCode::ResourceLimit);
    for(const auto &set:sets){if(set.size()>maxRefs-refs)fail("grouping reference limit exceeded",DiagnosticCode::ResourceLimit);refs+=set.size();}
  };
  auto product=[&](const Sets &left,const Sets &right){
    if(!right.empty() && left.size()>maxSets/right.size())fail("grouping set product limit exceeded",DiagnosticCode::ResourceLimit);
    Sets out;std::size_t refs=0;
    for(const auto &a:left)for(const auto &b:right) {
      checkStop(context_);
      if(a.size()>maxRefs-refs || b.size()>maxRefs-refs-a.size())fail("grouping reference product limit exceeded",DiagnosticCode::ResourceLimit);
      refs+=a.size()+b.size();Group next=a;next.insert(next.end(),b.begin(),b.end());out.push_back(std::move(next));
    }
    return out;
  };
  std::function<Sets(std::size_t)> element;
  element=[&](std::size_t depth)->Sets {
    if(depth>=context_.options.limits.nesting)fail("grouping nesting limit exceeded",DiagnosticCode::ResourceLimit);
    checkStop(context_);
    if(at("GROUPING") && at("SETS",1)) {
      if(family)fail("GROUPING SETS is unavailable in the MySQL-family grammar baseline",DiagnosticCode::UnsupportedSyntax);
      advanced=true;take();take();expect("(");Sets out;
      if(at(")"))fail("GROUPING SETS requires at least one grouping set");
      do {auto child=element(depth+1);if(child.size()>maxSets-out.size())fail("grouping set list limit exceeded",DiagnosticCode::ResourceLimit);out.insert(out.end(),child.begin(),child.end());bound(out);}while(eat(","));
      expect(")");return out;
    }
    if((at("ROLLUP") || at("CUBE")) && at("(",1)) {
      if(family && at("CUBE"))fail("CUBE is unavailable in the MySQL-family grammar baseline",DiagnosticCode::UnsupportedSyntax);
      advanced=true;const bool cube=eat("CUBE");if(!cube)expect("ROLLUP");expect("(");
      if(at(")"))fail("ROLLUP/CUBE requires a grouping element");
      std::vector<Group> units;
      do {auto child=element(depth+1);if(child.size()!=1)fail("nested expansion inside ROLLUP/CUBE is not supported",DiagnosticCode::UnsupportedSyntax);units.push_back(std::move(child.front()));if(units.size()>maxRefs)fail("grouping element limit exceeded",DiagnosticCode::ResourceLimit);}while(eat(","));
      expect(")");Sets out;
      if(cube) {
        out={{}};
        for(const auto &unit:units){Sets choices{unit,{}};out=product(out,choices);}
      } else {
        Group all;for(const auto &unit:units){if(unit.size()>maxRefs-all.size())fail("ROLLUP reference limit exceeded",DiagnosticCode::ResourceLimit);all.insert(all.end(),unit.begin(),unit.end());}
        out.push_back(all);
        for(auto it=units.rbegin();it!=units.rend();++it){all.resize(all.size()-it->size());out.push_back(all);bound(out);}
      }
      bound(out);return out;
    }
    bool tuple=false;
    if(at("(")) {
      std::size_t level=0;
      for(std::size_t ahead=0;cursor_+ahead<tokens_.size();++ahead) {
        checkStop(context_);
        if(at("(",ahead))++level;
        else if(at(",",ahead) && level==1)tuple=true;
        else if(at(")",ahead) && --level==0) {
          tuple|=ahead==1;
          const auto next=std::min(cursor_+ahead+1,tokens_.size()-1);
          bool boundary=tokens_[next].kind==TokenKind::End;
          for(auto delimiter:{",", ")", ";", "HAVING", "WINDOW", "ORDER", "LIMIT", "OFFSET", "FETCH", "UNION", "EXCEPT", "INTERSECT", "MINUS", "WITH"})
            boundary|=at(delimiter,ahead+1);
          tuple &= boundary;break;
        }
      }
    }
    if(tuple && eat("(")) {
      Group group;
      if(eat(")")){advanced=true;return Sets{{}};}
      do {auto id=expression();requireScalar(id);group.push_back(id);if(group.size()>maxRefs)fail("grouping tuple limit exceeded",DiagnosticCode::ResourceLimit);}while(eat(","));
      expect(")");return Sets{std::move(group)};
    }
    auto id=expression();requireScalar(id);return Sets{{id}};
  };
  Sets expanded{{}};
  do {expanded=product(expanded,element(0));}while(eat(","));
  if(eat("WITH")) {
    expect("ROLLUP");
    if(!family)fail("WITH ROLLUP is a MySQL-family grouping modifier",DiagnosticCode::UnsupportedSyntax);
    if(advanced || expanded.size()!=1 || expanded.front().empty())fail("WITH ROLLUP requires a plain grouping list");
    advanced=true;auto all=expanded.front();expanded.clear();
    for(;;){expanded.push_back(all);if(all.empty())break;all.pop_back();bound(expanded);}
  }
  if(advanced && context_.dialect==Dialect::SQLite)fail("grouping sets are not supported by SQLite",DiagnosticCode::UnsupportedSyntax);
  if(advanced && family) {
    // Canonical MySQL output uses WITH ROLLUP. Other expanded grouping
    // patterns cannot be represented by that syntax without a relational rewrite.
    bool rollup=expanded.size()>1 && expanded.front().size()+1==expanded.size();
    if(rollup)for(std::size_t i=0;i<expanded.size();++i)
      rollup &= expanded[i].size()==expanded.front().size()-i && std::equal(expanded[i].begin(),expanded[i].end(),expanded.front().begin());
    if(!rollup)fail("this MySQL-family baseline supports prefix ROLLUP only",DiagnosticCode::UnsupportedSyntax);
  }
  std::set<ExprId> seen;
  for(const auto &set:expanded)for(auto id:set)if(seen.insert(id).second)s.groupBy.push_back(id);
  if(advanced) {
    std::size_t refs=0;for(const auto &set:expanded)refs+=set.size();
    if(expanded.size()>maxSets-groupingSetCount_ || refs>maxRefs-groupingReferenceCount_)
      fail("cumulative grouping set/reference budget exceeded",DiagnosticCode::ResourceLimit);
    groupingSetCount_+=expanded.size();groupingReferenceCount_+=refs;s.groupingSets=std::move(expanded);
  }
}
void Parser::syntaxNode() {
  if (syntaxNodes_ + context_.ast.size() + context_.ast.queryCount() >=
      context_.options.limits.nodes)
    fail("syntax node limit exceeded", DiagnosticCode::ResourceLimit);
  ++syntaxNodes_;
}
ExprId Parser::node(Expr expr) {
  if (syntaxNodes_ + context_.ast.size() + context_.ast.queryCount() >=
      context_.options.limits.nodes)
    fail("AST node limit exceeded", DiagnosticCode::ResourceLimit);
  for (auto operand : expr.operands)
    if (expr.kind != ExprKind::Call)
      requireScalar(operand);
  return context_.ast.add(std::move(expr));
}
void Parser::requireScalar(ExprId expression) {
  if (context_.ast.expression(expression).kind == ExprKind::Star)
    fail("wildcard is only supported as a projection or a sole function "
         "argument",
         DiagnosticCode::UnsupportedSyntax);
}
std::pair<std::string, int> Parser::binaryOperator() const {
  if (at("OR"))
    return {"OR", precedence("OR")};
  if (at("AND"))
    return {"AND", precedence("AND")};
  for (auto op : {"=", "!=", "<>", "<", ">", "<=", ">=", "LIKE"})
    if (at(op))
      return {std::string(op) == "!=" ? "<>" : op, precedence(op)};
  if (at("+") || at("-"))
    return {current().value, 80};
  if (at("*") || at("/"))
    return {current().value, 90};
  return {"", -1};
}
int Parser::precedence(std::string_view op) const {
  if (op == "OR")
    return 10;
  if (op == "AND")
    return 20;
  if (op == "NOT")
    return 30;
  if (op == "IS" || op == "BETWEEN" || op == "IN" || op == "LIKE" ||
      op == "ILIKE")
    return 40;
  if (op == "UNARY")
    return 110;
  return 50; // comparison
}
int Parser::mysqlExpressionLevel(ExprId id) const {
  // 0=simple_expr, 1=bit_expr, 2=predicate, 3=bool_pri, 4=expr.
  // Parentheses promote any expression to simple_expr. Read previous() before
  // consuming the following operator: grouping parentheses are not AST nodes.
  const auto &e=context_.ast.expression(id);
  if (previous().range.end>e.range.end) return 0;
  if (e.kind==ExprKind::Unary) return e.value=="NOT"?4:0;
  if (e.kind==ExprKind::TruthTest) return context_.dialect==Dialect::MariaDB?3:4;
  if (e.kind==ExprKind::IsNull || e.kind==ExprKind::Quantified) return 3;
  if (e.kind==ExprKind::Between || e.kind==ExprKind::In || e.kind==ExprKind::LikeEscape) return 2;
  if (e.kind!=ExprKind::Binary) return 0;
  if (e.value=="AND" || e.value=="OR" || e.value=="XOR") return 4;
  if (e.value=="=" || e.value=="<>" || e.value=="<" || e.value==">" ||
      e.value=="<=" || e.value==">=" || e.value=="NULL_SAFE_EQUAL") return 3;
  if (e.value=="LIKE" || e.value=="NOT LIKE" || e.value=="REGEXP" || e.value=="NOT REGEXP") return 2;
  return e.value=="CONCAT"?0:1;
}
int Parser::postgresqlNonAssociativeLevel(ExprId id) const {
  const auto &e=context_.ast.expression(id);
  if (previous().range.end>e.range.end) return -1;
  if (e.kind==ExprKind::Between || e.kind==ExprKind::LikeEscape) return 60;
  // IN(list), IS NULL/TRUE and quantified comparisons end in a fixed token
  // rather than a recursive a_expr. PostgreSQL permits chains after them.
  if (e.kind!=ExprKind::Binary) return -1;
  if (e.value=="LIKE" || e.value=="NOT LIKE" || e.value=="ILIKE" || e.value=="NOT ILIKE") return 60;
  if (e.value=="IS DISTINCT FROM" || e.value=="IS NOT DISTINCT FROM") return 40;
  if (e.value=="=" || e.value=="<>" || e.value=="<" || e.value==">" || e.value=="<=" || e.value==">=") return 50;
  return -1;
}
ExprId Parser::expression(int minimum, bool postgresqlRestricted) {
  if (depth_ >= context_.options.limits.nesting)
    fail("expression nesting limit exceeded", DiagnosticCode::ResourceLimit);
  DepthGuard guard(depth_);
  auto begin = current().range.begin;
  ExprId left;
  if (postgresqlRestricted && at("NOT")) fail("NOT requires parentheses in a PostgreSQL BETWEEN lower bound");
  if (at("NOT") && context_.dialect!=Dialect::SQLite && minimum>precedence("NOT"))
    fail("NOT in an arithmetic/comparison operand requires parentheses");
  if (eat("NOT")) {
    auto operand = expression(precedence("NOT"),postgresqlRestricted);
    left = node(
        {ExprKind::Unary, {begin, previous().range.end}, "NOT", {}, {operand}});
  } else if (at("+") || at("-")) {
    auto op = take().value;
    auto operand = expression(precedence("UNARY"),postgresqlRestricted);
    left = node(
        {ExprKind::Unary, {begin, previous().range.end}, op, {}, {operand}});
  } else
    left = postfix(window(primary()));
  while (true) {
    if (depth_==projectionExpressionDepth_ && trailingProjectionAlias()) break;
    // Compact PostgreSQL/SQLite null tests share the IS predicate precedence.
    // Keep this in the precedence loop: parsing it in primary()/postfix()
    // would incorrectly bind it more tightly than arithmetic expressions.
    if ((context_.dialect == Dialect::SQLite || (context_.dialect == Dialect::PostgreSQL && !postgresqlRestricted)) &&
        minimum <= precedence("IS") && (at("ISNULL") || at("NOTNULL") ||
          (context_.dialect == Dialect::SQLite && at("NOT") && at("NULL", 1)))) {
      if (context_.dialect==Dialect::PostgreSQL && postgresqlNonAssociativeLevel(left)==precedence("IS"))
        fail("non-associative PostgreSQL predicates require parentheses");
      const bool negate = !eat("ISNULL");
      if (negate && !eat("NOTNULL")) {
        expect("NOT");
        expect("NULL");
      }
      left = node({ExprKind::IsNull, {begin, previous().range.end},
                   negate ? "IS NOT NULL" : "IS NULL", {}, {left}});
      continue;
    }
    const bool negated =
        at("NOT") &&
        (at("IN", 1) || at("BETWEEN", 1) || at("LIKE", 1) ||
         (dmlGrammar().mysql && (at("REGEXP",1) || at("RLIKE",1))) ||
         (context_.dialect == Dialect::SQLite && at("GLOB",1)) ||
         (context_.dialect == Dialect::PostgreSQL && at("ILIKE", 1)));
    const auto predicate = at("IS") ? "IS"
                           : at("BETWEEN") || (negated && at("BETWEEN", 1))
                               ? "BETWEEN"
                           : at("IN") || (negated && at("IN", 1)) ? "IN"
                           : negated && at("ILIKE", 1)            ? "ILIKE"
                           : negated && at("GLOB", 1)             ? "GLOB"
                                                                  : "LIKE";
    const auto predicatePower = precedence(predicate);
    if (minimum <= predicatePower &&
        (at("IS") || at("BETWEEN") || at("IN") || negated)) {
      if (postgresqlRestricted && (!at("IS") ||
          !(at("DISTINCT",1) || (at("NOT",1) && at("DISTINCT",2)))))
        fail("this predicate requires parentheses in a PostgreSQL BETWEEN lower bound");
      if (context_.dialect==Dialect::PostgreSQL && postgresqlNonAssociativeLevel(left)==predicatePower)
        fail("non-associative PostgreSQL predicates require parentheses");
      if (dmlGrammar().mysql && mysqlExpressionLevel(left)>
          (at("IS")?3:context_.dialect==Dialect::MariaDB?2:1))
        fail("this predicate operand requires parentheses in this dialect");
      if (negated)
        take();
      if (eat("IS")) {
        bool negate = eat("NOT");
        const bool sqlite = context_.dialect == Dialect::SQLite;
        const bool distinctForm = (sqlite || context_.dialect == Dialect::PostgreSQL ||
                                   context_.dialect == Dialect::SQLServer) && eat("DISTINCT");
        if (sqlite || distinctForm) {
          if (distinctForm) expect("FROM");
          auto right = expression(predicatePower + 1,postgresqlRestricted);
          const bool different = distinctForm != negate;
          if (sqlite && context_.ast.expression(right).kind == ExprKind::Null) {
            left = node({ExprKind::IsNull, {begin, previous().range.end},
                         different ? "IS NOT NULL" : "IS NULL", {}, {left}});
          } else {
            left = node({ExprKind::Binary, {begin, previous().range.end},
                         different ? "IS DISTINCT FROM" : "IS NOT DISTINCT FROM", {}, {left, right}});
          }
          continue;
        }
        if ((context_.dialect == Dialect::PostgreSQL || dmlGrammar().mysql) &&
            (at("TRUE") || at("FALSE") || at("UNKNOWN"))) {
          const auto test = asciiUpper(take().value);
          left = node({ExprKind::TruthTest, {begin, previous().range.end},
                       std::string(negate ? "IS NOT " : "IS ") + test, {}, {left}});
          continue;
        }
        expect("NULL");
        left = node({ExprKind::IsNull,
                     {begin, previous().range.end},
                     negate ? "IS NOT NULL" : "IS NULL",
                     {},
                     {left}});
      } else if (eat("BETWEEN")) {
        bool symmetric = false;
        if (context_.dialect == Dialect::PostgreSQL) {
          symmetric = eat("SYMMETRIC");
          if (!symmetric) eat("ASYMMETRIC");
        }
        // SQLite's first BETWEEN bound is a full expression up to AND;
        // unlike the high bound it can contain equal-precedence comparisons.
        const bool strictMysql=dmlGrammar().mysql && context_.dialect!=Dialect::MariaDB;
        const bool pg=context_.dialect==Dialect::PostgreSQL;
        auto low = expression(context_.dialect==Dialect::SQLite || pg?21:strictMysql?61:
            dmlGrammar().mysql?predicatePower:std::max(51, predicatePower + 1),pg);
        expect("AND");
        auto high = expression(dmlGrammar().mysql?predicatePower:std::max(21, predicatePower + 1));
        left = node({ExprKind::Between,
                     {begin, previous().range.end},
                     std::string(negated ? "NOT BETWEEN" : "BETWEEN") + (symmetric ? " SYMMETRIC" : ""),
                     {},
                     {left, low, high}});
      } else if (eat("IN")) {
        expect("(");
        if (queryAhead()) {
          const auto query = nestedQuery();
          checkQuantifiedSubquery(query);
          expect(")");
          Expr e{ExprKind::In,
                 {begin, previous().range.end},
                 negated ? "NOT IN" : "IN",
                 {},
                 {left}};
          e.query = query;
          left = node(std::move(e));
          continue;
        }
        std::vector<ExprId> args{left};
        // SQLite explicitly permits IN () and NOT IN (); the other supported
        // dialects require at least one element. Preserve the empty set in IR
        // so NULL IN () is false rather than nullable.
        if (context_.dialect != Dialect::SQLite || !at(")"))
          do { args.push_back(expression()); } while (eat(","));
        expect(")");
        left = node({ExprKind::In,
                     {begin, previous().range.end},
                     negated ? "NOT IN" : "IN",
                     {},
                     std::move(args)});
      } else {
        if (context_.dialect==Dialect::SQLite && eat("GLOB")) {
          const auto pattern=expression(predicatePower+1);
          if (at("ESCAPE"))
            fail("SQLite GLOB ESCAPE requires a three-argument function model; unsupported here",DiagnosticCode::UnsupportedSyntax);
          left=node({ExprKind::Binary,{begin,previous().range.end},"NOT GLOB",{}, {left,pattern}});
          continue;
        }
        if (dmlGrammar().mysql && (eat("REGEXP") || eat("RLIKE"))) {
          const auto pattern=expression(context_.dialect==Dialect::TiDB?100:predicatePower+1);
          left=node({ExprKind::Binary,{begin,previous().range.end},"NOT REGEXP",{}, {left,pattern}});
          continue;
        }
        const bool insensitive =
            context_.dialect == Dialect::PostgreSQL && eat("ILIKE");
        if (!insensitive)
          expect("LIKE");
        const auto patternMinimum=dmlGrammar().mysql && context_.dialect!=Dialect::MariaDB?100:predicatePower+1;
        auto right = expression(patternMinimum);
        std::vector<ExprId> operands{left, right};
        const bool escaped = eat("ESCAPE");
        if (escaped) operands.push_back(patternEscapeOperand(std::max(51, patternMinimum)));
        left = node({escaped ? ExprKind::LikeEscape : ExprKind::Binary,
                     {begin, previous().range.end},
                     insensitive ? "NOT ILIKE" : "NOT LIKE",
                     {},
                     std::move(operands)});
      }
      continue;
    }
    auto [op, precedence] = binaryOperator();
    if (precedence < minimum || precedence < 0)
      break;
    if (postgresqlRestricted && (op=="LIKE" || op=="ILIKE" || op=="AND" || op=="OR"))
      fail("this operator requires parentheses in a PostgreSQL BETWEEN lower bound");
    if (context_.dialect==Dialect::PostgreSQL && postgresqlNonAssociativeLevel(left)==precedence)
      fail("non-associative PostgreSQL operators require parentheses");
    if (dmlGrammar().mysql && op!="AND" && op!="OR" && op!="XOR") {
      const bool comparison=op=="=" || op=="<>" || op=="<" || op==">" ||
          op=="<=" || op==">=" || op=="NULL_SAFE_EQUAL";
      const bool predicate=op=="LIKE" || op=="REGEXP";
      const auto maximum=comparison?3:predicate && context_.dialect==Dialect::MariaDB?2:op=="CONCAT"?0:1;
      if (mysqlExpressionLevel(left)>maximum)
        fail("this operator operand requires parentheses in this dialect");
    }
    take();
    if ((op == "=" || op == "<>" || op == "<" || op == ">" || op == "<=" ||
         op == ">=") &&
        (at("ANY") || at("ALL") || at("SOME"))) {
      if (postgresqlRestricted)
        fail("quantified comparisons require parentheses in a PostgreSQL BETWEEN lower bound");
      if (context_.dialect == Dialect::SQLite)
        fail("quantified comparison is unavailable in SQLite",
             DiagnosticCode::UnsupportedSyntax);
      auto quantifier = asciiUpper(take().value);
      expect("(");
      QueryId query=0;
      std::vector<ExprId> operands{left};
      if (context_.dialect==Dialect::PostgreSQL && !queryAhead())
        operands.push_back(expression());
      else {
        query=nestedQuery();
        checkQuantifiedSubquery(query);
      }
      expect(")");
      Expr e{ExprKind::Quantified,
             {begin, previous().range.end},
             op + " " + quantifier,
             {},
             std::move(operands)};
      e.query = query;
      left = node(std::move(e));
      continue;
    }
    const auto operandMinimum=dmlGrammar().mysql &&
        ((op=="LIKE" && context_.dialect!=Dialect::MariaDB) ||
         (op=="REGEXP" && context_.dialect==Dialect::TiDB))?100:precedence+1;
    auto right = expression(operandMinimum,postgresqlRestricted);
    if (context_.dialect==Dialect::SQLite && op=="GLOB" && at("ESCAPE"))
      fail("SQLite GLOB ESCAPE requires a three-argument function model; unsupported here",DiagnosticCode::UnsupportedSyntax);
    std::vector<ExprId> operands{left, right};
    const bool escaped = (op == "LIKE" || op == "ILIKE") && eat("ESCAPE");
    if (escaped) operands.push_back(patternEscapeOperand(std::max(51, operandMinimum)));
    left = node({escaped ? ExprKind::LikeEscape : ExprKind::Binary,
                 {begin, previous().range.end},
                 std::move(op),
                 {},
                 std::move(operands)});
  }
  return left;
}
ExprId Parser::functionArgument(const QualifiedName &name,std::size_t index) {
  if (!dmlGrammar().mysql || name.size()!=1 || name.front().quoted) return expression();
  const auto function=asciiUpper(context_.ast.identifier(name.front().identifier));
  const bool offset=(function=="NTILE" && index==0) ||
      ((function=="LAG" || function=="LEAD") && index==1);
  if (context_.dialect==Dialect::MySQL && offset) {
    const auto token=current();
    if (token.kind==TokenKind::Number) {
      std::uint64_t value=0;
      const auto converted=std::from_chars(token.value.data(),token.value.data()+token.value.size(),value);
      if (token.value.empty() || converted.ec!=std::errc{} || converted.ptr!=token.value.data()+token.value.size())
        fail("this MySQL window argument requires an unsigned integer literal, parameter or routine variable");
      return primary();
    }
    if (token.kind==TokenKind::Parameter ||
        (token.kind==TokenKind::Variable && token.value.size()>1 && token.value[1]!='@')) return primary();
    if (nameToken()) {
      if (at("(",1) || at(".",1)) fail("this MySQL window argument requires a bare routine variable");
      const auto begin=cursor_;
      const auto id=primary();
      auto &argument=context_.ast.mutableExpression(id);
      if (cursor_!=begin+1 || argument.kind!=ExprKind::Name || argument.name.size()!=1)
        fail("this MySQL window argument requires a bare routine variable");
      argument.value="MYSQL_ROUTINE_INTEGER";
      return id;
    }
    fail("this MySQL window argument requires an unsigned integer literal, parameter or routine variable");
  }
  if (context_.dialect==Dialect::TiDB && (function=="LAG" || function=="LEAD") && index==1) {
    if (current().kind!=TokenKind::Number && current().kind!=TokenKind::Parameter)
      fail("TiDB LAG/LEAD offsets require a numeric literal or parameter");
    return primary();
  }
  if ((function=="NTH_VALUE" && index==1 && context_.dialect!=Dialect::MariaDB) ||
      (function=="NTILE" && index==0 && context_.dialect==Dialect::TiDB)) return expression(100);
  return expression();
}
ExprId Parser::patternEscapeOperand(int minimum) {
  const bool tidb=context_.dialect==Dialect::TiDB;
  if (tidb && current().kind!=TokenKind::String)
    fail("TiDB ESCAPE requires a string literal");
  const auto id=expression(minimum);
  if (tidb && context_.ast.expression(id).kind!=ExprKind::String)
    fail("TiDB ESCAPE requires a string literal");
  return id;
}
// Look ahead without consuming tokens or creating AST nodes. PostgreSQL's
// type 'text' syntax overlaps function calls with string aliases; other
// frontends only recognize their supported direct temporal prefixes.
bool Parser::typedLiteralAhead() const {
  if (ddlGrammar().postgres) {
    if (!typeNameToken() && !(nameToken() && at(".",1))) return false;
    if (at(".",1) && !nameToken()) return false;
  } else if(!nameToken() && !at("TIME") && !at("TIMESTAMP") && !at("DATE"))return false;
  auto word=[&](std::size_t at){return at<tokens_.size()?asciiUpper(tokens_[at].value):std::string{};};
  const auto base=word(cursor_);
  if(!ddlGrammar().postgres)
    return (base=="DATE" || base=="TIME" || base=="TIMESTAMP") &&
      cursor_+1<tokens_.size() && tokens_[cursor_+1].kind==TokenKind::String;
  auto i=cursor_+1;
  auto identifier=[&](std::size_t index){return index<tokens_.size() &&
    (tokens_[index].kind==TokenKind::Word || tokens_[index].kind==TokenKind::Identifier);};
  while(word(i)=="." && identifier(i+1))i+=2;
  if((base=="DOUBLE" && word(i)=="PRECISION") ||
     ((base=="CHAR" || base=="CHARACTER" || base=="BIT") && word(i)=="VARYING"))++i;
  if(word(i)=="(") {
    ++i;
    // Type arguments are a flat list, never expressions. Let typeSpec report
    // precise errors after the shape has been established.
    while(i<tokens_.size() && tokens_[i].kind!=TokenKind::End && word(i)!=")") {
      if(tokens_[i].kind!=TokenKind::Number && tokens_[i].kind!=TokenKind::String &&
         word(i)!="," && word(i)!="-" && word(i)!="+" && word(i)!="*" && word(i)!="MAX")return false;
      ++i;
    }
    if(word(i)!=")")return false;
    ++i;
  }
  if((base=="TIME" || base=="TIMESTAMP") && (word(i)=="WITH" || word(i)=="WITHOUT")) {
    ++i;if(word(i)=="LOCAL")++i;
    if(word(i)!="TIME" || word(i+1)!="ZONE")return false;
    i+=2;
  }
  while(word(i)=="[" && word(i+1)=="]")i+=2;
  return i<tokens_.size() && tokens_[i].kind==TokenKind::String;
}
ExprId Parser::typedLiteral() {
  const auto begin=current().range.begin;
  const auto base=asciiUpper(current().value);
  if(ddlGrammar().sqlserver || ddlGrammar().sqlite || (ddlGrammar().oracle && base=="TIME"))
    fail("typed temporal literal is unavailable in this dialect",DiagnosticCode::ExpectedToken);
  auto type=typeSpec(true);
  if(current().kind!=TokenKind::String)fail("typed literal requires a string value");
  if(type.arrayDimensions)fail("array typed literals require CAST or :: syntax",DiagnosticCode::ExpectedToken);
  auto value=take();
  Expr result{ExprKind::TypedLiteral,{begin,value.range.end},std::move(value.value),{}, {}};
  result.castType=std::make_shared<const BasicTypeSpec<QualifiedName>>(std::move(type));
  return node(std::move(result));
}
void Parser::checkQuantifiedSubquery(QueryId query) {
  if (dmlGrammar().mysql && context_.ast.query(query).limit.count)
    fail("MySQL-family IN/ALL/ANY/SOME subqueries do not support a direct LIMIT",
         DiagnosticCode::UnsupportedSyntax);
}
ExprId Parser::arrayConstructor(bool named) {
  const auto start=current().range.begin;
  if (depth_>=context_.options.limits.nesting)
    fail("array constructor nesting limit exceeded",DiagnosticCode::ResourceLimit);
  DepthGuard guard(depth_);
  if (named) expect("ARRAY");
  expect("[");
  std::vector<ExprId> elements;
  if (!eat("]")) {
    const bool bracketRows=at("[");
    do {
      if (at("[")!=bracketRows) fail("nested bracket arrays require a list of bracket rows");
      elements.push_back(bracketRows?arrayConstructor(false):expression());
    } while (eat(","));
    expect("]");
  }
  return node({ExprKind::ArrayConstructor,{start,previous().range.end},named?"ARRAY":"[]",{},std::move(elements)});
}
ExprId Parser::primary() {
  auto token = current();
  if ((token.kind==TokenKind::Word || token.kind==TokenKind::Identifier) && at("(",1) &&
      nativeFunctionNeedsAdjacentParen(context_.dialect,token.value)) {
    if (token.kind==TokenKind::Identifier)
      fail("quoted native-function spellings require deployed routine binding",DiagnosticCode::UnsupportedSyntax);
    const auto gap=context_.source.slice({token.range.end,tokens_[cursor_+1].range.begin});
    const bool whitespace=std::all_of(gap.begin(),gap.end(),[](unsigned char ch) {
      return ch==' ' || ch=='\t' || ch=='\n' || ch=='\r' || ch=='\f' || ch=='\v';
    });
    const bool commentTrivia=context_.dialect==Dialect::MariaDB && context_.options.mysqlIgnoreSpace;
    if (!gap.empty() && (!context_.options.mysqlIgnoreSpace || (!whitespace && !commentTrivia)))
      fail("separated native-function names require IGNORE_SPACE or generic routine binding",DiagnosticCode::UnsupportedSyntax);
  }
  if (context_.dialect==Dialect::SQLServer && token.kind==TokenKind::Symbol &&
      !token.value.empty() && token.value.front()=='$' && asciiUpper(token.value)=="$ACTION") {
    if (!mergeOutput_) fail("$action is only available in SQL Server MERGE OUTPUT");
    take();
    return node({ExprKind::MergeAction,token.range,{}, {}, {}});
  }
  if (context_.dialect==Dialect::PostgreSQL && at("ARRAY") && at("[",1))
    return arrayConstructor(true);
  if (context_.dialect==Dialect::PostgreSQL && at("ARRAY") && at("(",1)) {
    take();
    expect("(");
    if (!queryAhead()) fail("ARRAY(...) requires a query");
    const auto query=nestedQuery();
    expect(")");
    Expr value{ExprKind::ArrayConstructor,{token.range.begin,previous().range.end},"ARRAY",{}, {}};
    value.query=query;
    return node(std::move(value));
  }
  if (context_.dialect==Dialect::PostgreSQL && at("ROW") && at("(",1)) {
    take();
    expect("(");
    std::vector<ExprId> fields;
    if (!eat(")")) {
      do { fields.push_back(expression()); } while (eat(","));
      expect(")");
    }
    return node({ExprKind::Row,{token.range.begin,previous().range.end},"ROW",{},std::move(fields)});
  }
  if (eat("CASE"))
    return caseExpression(token.range.begin);
  if (at("EXISTS") && at("(",1)) {
    take();
    expect("(");
    const auto query = nestedQuery();
    expect(")");
    Expr e{ExprKind::Exists,
           {token.range.begin, previous().range.end},
           "",
           {},
           {}};
    e.query = query;
    return node(std::move(e));
  }
  if (eat("(")) {
    if (at("SELECT") || at("WITH")) {
      const auto query = nestedQuery();
      expect(")");
      Expr e{ExprKind::Subquery,
             {token.range.begin, previous().range.end},
             "",
             {},
             {}};
      e.query = query;
      return node(std::move(e));
    }
    auto inner = expression();
    if (eat(",")) {
      std::vector<ExprId> row{inner};
      do {
        row.push_back(expression());
      } while (eat(","));
      expect(")");
      return node({ExprKind::Row,
                   {token.range.begin, previous().range.end},
                   "",
                   {},
                   std::move(row)});
    }
    expect(")");
    return inner;
  }
  if(intervalAhead())return intervalExpression();
  if(at("EXTRACT") && at("(",1))return extractExpression();
  if(typedLiteralAhead())return typedLiteral();
  if (eat("CAST")) {
    expect("(");
    auto base = expression();
    expect("AS");
    return castType(base, token.range.begin, true);
  }
  if (eat("*"))
    return node({ExprKind::Star, token.range, "", {}, {}});
  if (token.kind == TokenKind::Number || token.kind == TokenKind::String ||
      token.kind == TokenKind::Parameter || token.kind == TokenKind::Variable) {
    take();
    auto kind = token.kind == TokenKind::Number     ? ExprKind::Number
                : token.kind == TokenKind::String   ? ExprKind::String
                : token.kind == TokenKind::Variable ? ExprKind::Variable
                                                    : ExprKind::Parameter;
    return node({kind, token.range, std::move(token.value), {}, {}});
  }
  if (eat("NULL"))
    return node({ExprKind::Null, token.range, "", {}, {}});
  if ((at("TRUE") || at("FALSE")) && context_.dialect!=Dialect::SQLServer) {
    if (context_.dialect == Dialect::Oracle &&
        context_.names.environment == SemanticEnvironment::Sql)
      fail("boolean literals require version/type support not enabled in this "
           "frontend",
           DiagnosticCode::UnsupportedSyntax);
    take();
    return node(
        {ExprKind::Boolean, token.range, asciiUpper(token.value), {}, {}});
  }
  if (dmlGrammar().mysql && (at("VALUES") || at("DEFAULT")) && at("(", 1)) {
    auto part = take();
    expect("(");
    auto arg = namePart();
    expect(")");
    auto id = node({ExprKind::Name, arg.range, "", {arg}, {}});
    return node(
        {ExprKind::Call,
         {part.range.begin, previous().range.end},
         "",
         {{context_.ast.intern(asciiUpper(part.value)), false, part.range}},
         {id}});
  }
  if (current().kind == TokenKind::Word && !at("(", 1)) {
    const auto word = asciiUpper(current().value);
    if ((context_.dialect==Dialect::PostgreSQL || context_.dialect==Dialect::SQLServer) &&
        word=="USER") {
      const auto part=take();
      return node({ExprKind::Call,part.range,"KEYWORD",{{context_.ast.intern(word),false,part.range}}, {}});
    }
    if (context_.dialect==Dialect::SQLServer && word=="CURRENT_TIME")
      fail("SQL Server does not have a bare CURRENT_TIME expression in this parser profile");
    if (context_.dialect==Dialect::SQLServer && (word=="IDENTITYCOL" || word=="ROWGUIDCOL"))
      fail("SQL Server implicit identity/rowguid column references require attribute binding",DiagnosticCode::UnsupportedSyntax);
    if (context_.dialect==Dialect::PostgreSQL &&
        (word=="CURRENT_CATALOG" || word=="CURRENT_ROLE" || word=="CURRENT_SCHEMA")) {
      const auto part=take();
      return node({ExprKind::Call,part.range,"KEYWORD",{{context_.ast.intern(word),false,part.range}}, {}});
    }
    // UTC keyword forms permit omitted parentheses. CURDATE/CURTIME/NOW
    // also occur in native productions, but their function-name lexer tokens
    // require parentheses; bare spellings must remain column references.
    if (dmlGrammar().mysql && !at(".",1) &&
        (word=="UTC_DATE" || word=="UTC_TIME" || word=="UTC_TIMESTAMP")) {
      const auto part=take();
      return node({ExprKind::Call,part.range,"",
                   {{context_.ast.intern(word),false,part.range}}, {}});
    }
    const bool ordinarySqlServerTimeName=context_.dialect==Dialect::SQLServer &&
        (word=="LOCALTIME" || word=="LOCALTIMESTAMP");
    if (!ordinarySqlServerTimeName && (word == "CURRENT_TIMESTAMP" || word == "CURRENT_DATE" ||
        word == "CURRENT_TIME" || word == "CURRENT_USER" ||
        word == "SESSION_USER" || word == "SYSTEM_USER" ||
        word == "LOCALTIME" || word == "LOCALTIMESTAMP" ||
        (ddlGrammar().oracle &&
         (word == "SYSDATE" || word == "SYSTIMESTAMP")))) {
      const auto token = take();
      return node({ExprKind::Call,
                   token.range,
                   "KEYWORD",
                   {{context_.ast.intern(word), false, token.range}},
                   {}});
    }
  }
  const bool mysqlTimeCall=dmlGrammar().mysql && at("TIME") && at("(",1);
  const bool postgresFunction=context_.dialect==Dialect::PostgreSQL && functionNameToken() && at("(",1);
  const bool sqlServerKeywordFunction=context_.dialect==Dialect::SQLServer && current().kind==TokenKind::Word &&
      !nameToken() && functionNameToken() && at("(",1);
  const bool postgresSpecial=context_.dialect==Dialect::PostgreSQL && nameToken() &&
      !functionNameToken() && at("(",1) && current().kind==TokenKind::Word;
  if (nameToken() || mysqlTimeCall || postgresFunction || sqlServerKeywordFunction) {
    QualifiedName name;
    if(sqlServerKeywordFunction) {
      const auto part=take();context_.identifierRanges.push_back(part.range);
      name.push_back({context_.ast.intern(part.value),false,part.range});
    }
    else if(mysqlTimeCall) {const auto part=take();name.push_back({context_.ast.intern("TIME"),false,part.range});}
    else if (postgresFunction && !nameToken()) name.push_back(namePart(true));
    else name=qualifiedName(true);
    if (eat("(")) {
      if (postgresSpecial) {
        const auto word=asciiUpper(context_.ast.identifier(name.front().identifier));
        static const std::unordered_set<std::string> calls={"COALESCE","NULLIF","GREATEST","LEAST","GROUPING","SUBSTRING","TRIM"};
        if (!calls.count(word))
          fail("this PostgreSQL keyword function requires a specialized expression grammar",DiagnosticCode::UnsupportedSyntax);
      }
      if (!name.back().quoted &&
          context_.ast.identifier(name.back().identifier) == "*")
        fail("wildcard cannot be called");
      if (context_.dialect==Dialect::SQLServer && name.size()==1 && !name.front().quoted) {
        const auto function=asciiUpper(context_.ast.identifier(name.front().identifier));
        if (function=="JSON_OBJECT" || function=="JSON_ARRAY")
          return sqlServerJsonConstructor(std::move(name),token.range.begin);
        if (function=="CONVERT" || function=="TRY_CONVERT" || function=="TRY_CAST")
          return sqlServerConversion(function,token.range.begin);
        if (function=="JSON_OBJECTAGG" || function=="JSON_ARRAYAGG" || function=="AI_GENERATE_EMBEDDINGS")
          fail("this SQL Server function requires a specialized grammar not implemented yet",DiagnosticCode::UnsupportedSyntax);
      }
      std::vector<ExprId> args;
      const bool distinct = eat("DISTINCT") || (dmlGrammar().mysql && eat("DISTINCTROW"));
      if (distinct && at("ALL")) {
        if (name.size()!=1 || name.front().quoted ||
            !builtinAcceptsDistinctAll(context_.dialect,context_.ast.identifier(name.front().identifier)))
          fail("DISTINCT ALL is unavailable for this function/dialect");
        take();
      }
      const bool all=!distinct && eat("ALL");
      if (sqlServerKeywordFunction && (distinct || all))
        fail("this SQL Server keyword function has no aggregate quantifier production");
      if (all && (at("DISTINCT") || at("DISTINCTROW")))
        fail("function quantifier cannot combine DISTINCT and ALL");
      const auto nativeName=name.size()==1 && !name.front().quoted?
          asciiUpper(context_.ast.identifier(name.front().identifier)):std::string{};
      const bool nativeWindow=dmlGrammar().mysql &&
          (nativeName=="NTILE" || nativeName=="NTH_VALUE" || nativeName=="LAG" || nativeName=="LEAD");
      if (nativeWindow && (distinct || all)) fail("this window function has no aggregate quantifier production");
      if (!eat(")")) {
        args.push_back(functionArgument(name,args.size()));
        while (eat(",")) {
          if (sqlServerKeywordFunction && (nativeName=="CONTAINS" || nativeName=="FREETEXT") && at("LANGUAGE"))
            fail("SQL Server full-text LANGUAGE requires a dedicated clause model",DiagnosticCode::UnsupportedSyntax);
          args.push_back(functionArgument(name,args.size()));
        }
        expect(")");
      }
      if (sqlServerKeywordFunction &&
          ((nativeName=="COALESCE" && args.size()<2) || (nativeName=="NULLIF" && args.size()!=2)))
        fail("SQL Server COALESCE/NULLIF argument list does not match its keyword production");
      if (sqlServerKeywordFunction && (nativeName=="CONTAINS" || nativeName=="FREETEXT")) {
        if (args.size()!=2) fail("SQL Server full-text predicate requires a search expression; LANGUAGE requires a separate clause model");
        const auto &field=context_.ast.expression(args.front());
        if (field.kind==ExprKind::Row || field.kind==ExprKind::Star)
          fail("SQL Server full-text column lists/wildcards require dedicated binding support",DiagnosticCode::UnsupportedSyntax);
        if (field.kind!=ExprKind::Name) fail("SQL Server full-text predicate requires a column reference");
        const auto &search=context_.ast.expression(args[1]);
        if (search.kind!=ExprKind::String &&
            !(search.kind==ExprKind::Parameter && !search.value.empty() && search.value.front()=='@'))
          fail("SQL Server full-text search requires a string literal or local variable");
      }
      if (nativeWindow &&
          ((nativeName=="NTILE" && args.size()!=1) || (nativeName=="NTH_VALUE" && args.size()!=2) ||
           ((nativeName=="LAG" || nativeName=="LEAD") && (args.empty() ||
            (context_.dialect==Dialect::MariaDB && args.size()>2) ||
            (context_.dialect!=Dialect::OceanBase && context_.dialect!=Dialect::MariaDB && args.size()>3)))))
        fail("window argument list does not match this dialect's native production");
      for (auto arg : args)
        if (context_.ast.expression(arg).kind == ExprKind::Star &&
            (distinct || args.size() != 1 ||
             !context_.ast.expression(arg).name.empty()))
          fail("only a bare sole wildcard argument is supported",
               DiagnosticCode::UnsupportedSyntax);
      if (distinct && args.empty())
        fail("DISTINCT requires a function argument");
      if (all) {
        if (args.empty() && context_.dialect!=Dialect::SQLite)
          fail("ALL requires a function argument");
        std::vector<std::string> spelling;
        std::vector<bool> quoted;
        for (const auto &part:name) {
          spelling.push_back(canonicalIdentifier(context_.dialect,context_.ast.identifier(part.identifier),part.quoted));
          quoted.push_back(part.quoted);
        }
        const auto op=builtinOperator(context_.dialect,spelling,quoted,args.size());
        if (dmlGrammar().mysql && context_.dialect!=Dialect::TiDB && op && op->name=="GROUP_CONCAT")
          fail("GROUP_CONCAT has no ALL quantifier production in this dialect");
        if (context_.dialect==Dialect::MariaDB && op &&
            (op->name=="JSON_ARRAYAGG" || op->name=="JSON_OBJECTAGG"))
          fail("MariaDB JSON aggregates do not have an ALL quantifier production");
        if (context_.dialect!=Dialect::PostgreSQL && context_.dialect!=Dialect::SQLite &&
            (!op || op->kind!=RoutineKind::Aggregate))
          fail("ALL on this function requires a dialect-specific signature",DiagnosticCode::UnsupportedSyntax);
        for (const auto arg:args)
          if (context_.ast.expression(arg).kind==ExprKind::Star &&
              (!dmlGrammar().mysql || context_.dialect==Dialect::TiDB || !op || op->name!="COUNT"))
            fail("ALL wildcard is unavailable for this function/dialect");
        // ALL is the default for these calls. Normalize it away rather than
        // turning it into DISTINCT or a spurious identifier operand.
      }
      if (dmlGrammar().mysql && name.size()==1 && !name.front().quoted) {
        const auto function=asciiUpper(context_.ast.identifier(name.front().identifier));
        const bool bit=function=="BIT_AND" || function=="BIT_OR" || function=="BIT_XOR";
        const bool statistics=function=="STD" || function=="STDDEV" || function=="STDDEV_POP" ||
          function=="STDDEV_SAMP" || function=="VARIANCE" || function=="VAR_POP" || function=="VAR_SAMP";
        const bool jsonArray=function=="JSON_ARRAYAGG",jsonObject=function=="JSON_OBJECTAGG";
        if (distinct && !builtinAcceptsDistinct(context_.dialect,function))
          fail("DISTINCT is unavailable for this function in the selected dialect");
        if (((bit || statistics || (jsonArray && context_.dialect!=Dialect::MariaDB)) && args.size()!=1) ||
            (jsonObject && args.size()!=2))
          fail("aggregate argument list does not match its native grammar");
        if (bit || statistics || jsonArray || jsonObject)
          for (const auto arg:args) if (context_.ast.expression(arg).kind==ExprKind::Star)
            fail("this aggregate requires an expression rather than a wildcard");
        const bool precision=(function=="NOW" && context_.dialect!=Dialect::TiDB) || function=="CURTIME" || function=="UTC_TIME" ||
          function=="UTC_TIMESTAMP" || function=="CURRENT_TIME" || function=="CURRENT_TIMESTAMP" ||
          function=="LOCALTIME" || function=="LOCALTIMESTAMP";
        if ((function=="CURDATE" || function=="UTC_DATE" || function=="CURRENT_DATE") &&
            (distinct || !args.empty()))
          fail("date keyword function requires an empty argument list",DiagnosticCode::ExpectedToken);
        if (precision) {
          if (distinct || args.size()>1)
            fail("temporal function permits at most one integer precision",DiagnosticCode::ExpectedToken);
          if (!args.empty()) {
            const auto &argument=context_.ast.expression(args.front());
            if (argument.kind!=ExprKind::Number || argument.value.empty() ||
                argument.value.find_first_not_of("0123456789")!=std::string::npos)
              fail("temporal precision requires an unsigned integer literal",DiagnosticCode::ExpectedToken);
          }
        }
      }
      Expr call{ExprKind::Call,
                {token.range.begin, previous().range.end},
                "",
                std::move(name),
                std::move(args)};
      call.distinct = distinct;
      return node(std::move(call));
    }
    const auto kind = !name.back().quoted && context_.ast.identifier(
                                                 name.back().identifier) == "*"
                          ? ExprKind::Star
                          : ExprKind::Name;
    return node({kind,
                 {token.range.begin, previous().range.end},
                 "",
                 std::move(name),
                 {}});
  }
  if (context_.dialect == Dialect::PostgreSQL && at("`") && at("`", 2) &&
      tokens_[cursor_ + 1].kind == TokenKind::Word &&
      current().range.end == tokens_[cursor_ + 1].range.begin &&
      tokens_[cursor_ + 1].range.end == tokens_[cursor_ + 2].range.begin &&
      (cursor_ + 3 == tokens_.size() - 1 || at("FROM", 3) || at("WHERE", 3) ||
       at("AS", 3) || at(",", 3) || at(")", 3) || at(";", 3)))
    fail("PostgreSQL quoted identifiers require double quotes");
  if (current().kind == TokenKind::End || at(";") || at(")") || at(",") ||
      at("FROM") || at("WHERE") || at("THEN") || at("ELSE") || at("END") ||
      at("GROUP") || at("HAVING") || at("ORDER") || at("ESCAPE") ||
      (context_.dialect==Dialect::SQLite && at("GLOB")) ||
      (dmlGrammar().mysql && (at("XOR") || at("REGEXP") || at("RLIKE"))))
    fail("expected expression");
  if (context_.dialect==Dialect::PostgreSQL && current().kind==TokenKind::Word &&
      (postgresKeywordClass(asciiUpper(current().value))==PostgreSqlKeywordClass::Reserved ||
       postgresKeywordClass(asciiUpper(current().value))==PostgreSqlKeywordClass::TypeFunction))
    fail("this PostgreSQL keyword cannot be used as an unquoted scalar column name");
  fail("expected expression; unsupported or incomplete syntax",
       DiagnosticCode::UnsupportedSyntax);
}
ExprId Parser::castType(ExprId base, std::size_t begin, bool closeParenthesis) {
  BasicTypeSpec<QualifiedName> type;
  if(dmlGrammar().mysql && (at("SIGNED") || at("UNSIGNED"))) {
    const auto token=take();type.origin=token.range;
    type.name.push_back({context_.ast.intern(asciiUpper(token.value)),false,token.range});
    if(eat("INTEGER"))type.origin.end=previous().range.end;
  } else type=typeSpec();
  // Keep the compatibility name field, but the structured syntax owns modifiers,
  // lengths, signed scales and arrays. No SQL fragment is hidden in a string.
  auto name = type.name;
  if (closeParenthesis) expect(")");
  Expr result{ExprKind::Cast,{begin,previous().range.end},{},std::move(name),{base}};
  result.castType=std::make_shared<const BasicTypeSpec<QualifiedName>>(std::move(type));
  return node(std::move(result));
}
ExprId Parser::postfix(ExprId base) { return base; }
ExprId Parser::sqlServerConversion(std::string function, std::size_t begin) {
  BasicTypeSpec<QualifiedName> type;
  std::vector<ExprId> operands;
  if (function=="TRY_CAST") {
    operands.push_back(expression());
    expect("AS");
    type=typeSpec();
  } else {
    type=typeSpec();
    expect(",");
    operands.push_back(expression());
    if (eat(",")) operands.push_back(expression());
  }
  expect(")");
  for (const auto id:operands)
    if (context_.ast.expression(id).kind==ExprKind::Star)
      fail("type conversions require scalar value and style expressions");
  auto name=type.name;
  Expr result{ExprKind::Convert,{begin,previous().range.end},std::move(function),std::move(name),std::move(operands)};
  result.castType=std::make_shared<const BasicTypeSpec<QualifiedName>>(std::move(type));
  return node(std::move(result));
}
ExprId Parser::sqlServerJsonConstructor(QualifiedName name, std::size_t begin) {
  const bool object=asciiUpper(context_.ast.identifier(name.front().identifier))=="JSON_OBJECT";
  JsonConstructorSpec syntax;
  std::vector<ExprId> args;
  const auto policyAhead=[&] {return (at("NULL") || at("ABSENT")) && at("ON",1);};
  if (!at(")") && !policyAhead() && !(at("RETURNING") && at("JSON",1))) {
    do {
      if (at("DISTINCT") || at("ALL")) fail("JSON constructors do not accept function quantifiers");
      if (at(":")) fail("JSON object key is missing");
      args.push_back(expression());
      if (object) {
        expect(":");
        args.push_back(expression());
      }
    } while (eat(","));
  }
  if (policyAhead()) {
    syntax.nullPolicy=eat("NULL")?JsonNullPolicy::NullOnNull:JsonNullPolicy::AbsentOnNull;
    if (syntax.nullPolicy==JsonNullPolicy::AbsentOnNull) expect("ABSENT");
    expect("ON");
    expect("NULL");
  }
  if (eat("RETURNING")) {
    expect("JSON");
    syntax.returningJson=true;
  }
  expect(")");
  if (syntax.returningJson && args.empty() && syntax.nullPolicy==JsonNullPolicy::Default)
    fail("empty JSON constructor with bare RETURNING differs between the pinned source grammar and native parser; use an explicit null policy",DiagnosticCode::UnsupportedSyntax);
  for (const auto arg:args) {
    if (context_.ast.expression(arg).kind==ExprKind::Star)
      fail("JSON constructors require scalar expressions rather than wildcards");
    requireScalar(arg);
  }
  Expr call{ExprKind::Call,{begin,previous().range.end},"",std::move(name),std::move(args)};
  call.jsonConstructor=syntax;
  return node(std::move(call));
}
void Parser::selectPrefix(SelectStatement &) {}
void Parser::pagination(SelectStatement &) {}
bool Parser::trailingProjectionAlias() const {
  if (context_.dialect!=Dialect::PostgreSQL || !bareProjectionAliasToken()) return false;
  if (cursor_+1>=tokens_.size() || tokens_[cursor_+1].kind==TokenKind::End) return true;
  for (const auto boundary:{";",",",")","FROM","WHERE","GROUP","HAVING","WINDOW",
                            "ORDER","LIMIT","OFFSET","FETCH","UNION","INTERSECT","EXCEPT"})
    if (at(boundary,1)) return true;
  return false;
}
std::optional<NamePart> Parser::alias(bool isTable) {
  const auto extension = context_.options.extension;
  if ((extension == SqlExtension::ClickHouse && (at("PREWHERE") || (isTable && (at("FINAL") || at("SAMPLE"))))) ||
      (supportsQualify(extension) && at("QUALIFY")) ||
      (isTable && extension == SqlExtension::CockroachDb && at("AS") && at("OF", 1) &&
       at("SYSTEM", 2))) return {};
  if (isTable && (pivotAhead() || unpivotAhead())) return {};
  auto aliasName=[&]() {
    if(!isTable && (dmlGrammar().mysql || context_.dialect==Dialect::SQLServer) && current().kind==TokenKind::String) {
      const auto token=take();
      if(token.value.empty())fail("empty string aliases cannot be represented by this output model",DiagnosticCode::UnsupportedSyntax);
      return NamePart{context_.ast.intern(token.value),true,token.range};
    }
    return namePart();
  };
  if (eat("AS")) {
    if (isTable && !tableAliasWithAS())
      fail("AS is not allowed for table aliases in this dialect");
    if (!isTable && context_.dialect == Dialect::PostgreSQL)
      return namePart(true);
    return aliasName();
  }
  if (!isTable && context_.dialect==Dialect::PostgreSQL) {
    if (bareProjectionAliasToken()) return namePart(true);
    if (current().kind==TokenKind::Word && cursor_+1<tokens_.size() &&
        (tokens_[cursor_+1].kind==TokenKind::End || at(";",1) || at(")",1) || at(",",1)))
      fail("keyword requires AS for a projection label, or the following clause is incomplete");
    return {};
  }
  if (isTable && context_.dialect==Dialect::PostgreSQL && !nameToken() &&
      current().kind==TokenKind::Word && cursor_+1<tokens_.size() &&
      (tokens_[cursor_+1].kind==TokenKind::End || at(";",1) || at(")",1) || at(",",1)))
    fail("a PostgreSQL table alias requires a permitted identifier or quotes; the following clause may be incomplete");
  if (nameToken() || (!isTable && (dmlGrammar().mysql || context_.dialect==Dialect::SQLServer) && current().kind==TokenKind::String))
    return aliasName();
  return {};
}
ClickHouseSampleRatio Parser::clickHouseSampleRatio() {
  const auto begin = current().range.begin;
  const auto number = [&]() {
    const auto &token = current();
    if (token.kind != TokenKind::Number || token.value.empty() ||
        token.value.front() == '.' || token.value.find_first_not_of("0123456789.eE+-") != std::string::npos)
      fail("SAMPLE requires an unsigned number or a ratio");
    if (token.value.size() > 128)
      fail("SAMPLE number exceeds local size limit", DiagnosticCode::ResourceLimit);
    return take().value;
  };
  ClickHouseSampleRatio ratio;
  ratio.numerator = number();
  if (eat("/")) ratio.denominator = number();
  ratio.origin = {begin, previous().range.end};
  return ratio;
}

TableSource Parser::table(JoinKind kind, bool updateTarget, bool allowDeleteWildcard) {
  auto start = current().range.begin;
  TableSource source;
  source.join = kind;
  if (eat("(")) {
    const bool constructor = context_.dialect == Dialect::SQLServer && at("VALUES");
    if (constructor) {
      Statement values;
      values.range.begin = current().range.begin;
      values.sqlServerValueConstructor = true;
      valueRows(values);
      values.range.end = previous().range.end;
      source.query = storeQuery(std::move(values));
    } else {
      source.query = nestedQuery();
    }
    expect(")");
    source.alias = alias(true);
    const bool aliasAfterTransform=context_.dialect==Dialect::Oracle ||
        (context_.dialect==Dialect::PostgreSQL && context_.options.calciteExtensions);
    if (!source.alias && context_.dialect != Dialect::SQLite && context_.dialect != Dialect::Oracle &&
        context_.dialect != Dialect::PostgreSQL &&
        !(aliasAfterTransform && (pivotAhead() || unpivotAhead())))
      fail("derived tables require an alias in this supported subset");
    if (at("(")) {
      if (!source.alias) fail("derived column alias lists require a relation alias");
      if (!allowsDerivedColumnAliases(context_.dialect))
        fail("derived table column aliases are unavailable for this dialect baseline",
             DiagnosticCode::UnsupportedSyntax);
      take();
      do {
        if (source.columnAliases.size() >= context_.options.limits.columnsPerQuery)
          fail("derived column alias limit exceeded", DiagnosticCode::ResourceLimit);
        source.columnAliases.push_back(namePart());
      } while (eat(","));
      expect(")");
    }
    if (constructor && source.columnAliases.empty())
      fail("SQL Server derived VALUES requires a name for each output column",
           DiagnosticCode::InvalidQueryShape);
    pivotClause(source);
    unpivotClause(source);
    source.range = {start, previous().range.end};
    return source;
  }
  const bool only = context_.dialect == Dialect::PostgreSQL && eat("ONLY");
  const bool parenthesized = only && eat("(");
  source.name = qualifiedName(allowDeleteWildcard);
  if (parenthesized)
    expect(")");
  if (only)
    source.inheritance = TableInheritance::Only;
  else if (context_.dialect == Dialect::PostgreSQL && eat("*"))
    source.inheritance = TableInheritance::IncludeDescendants;
  if (context_.dialect!=Dialect::PostgreSQL || !at("TABLESAMPLE"))
    if (context_.dialect!=Dialect::Oracle || !at("MATCH_RECOGNIZE"))
      if (!updateTarget || context_.dialect!=Dialect::PostgreSQL || !at("SET"))
        source.alias = alias(true);
  if (!updateTarget && context_.options.extension == SqlExtension::ClickHouse) {
    source.finalRead = eat("FINAL");
    if (eat("SAMPLE")) {
      ClickHouseSampling sampling{clickHouseSampleRatio(), {}};
      if (eat("OFFSET")) sampling.offset = clickHouseSampleRatio();
      if (at("+") || at("-") || at("*") || at("/"))
        fail("SAMPLE accepts a numeric literal or ratio, not an arithmetic expression");
      source.clickHouseSampling = std::move(sampling);
    }
  }
  if (context_.dialect==Dialect::PostgreSQL && eat("TABLESAMPLE")) {
    const auto method = eat("SYSTEM") ? TableSamplingMethod::System : TableSamplingMethod::Bernoulli;
    if (method == TableSamplingMethod::Bernoulli && !eat("BERNOULLI"))
      fail("supported TABLESAMPLE methods are BERNOULLI and SYSTEM", DiagnosticCode::UnsupportedSyntax);
    if (method == TableSamplingMethod::System && context_.options.extension != SqlExtension::None)
      fail("SYSTEM sampling is only modeled for PostgreSQL", DiagnosticCode::UnsupportedSyntax);
    expect("(");
    const auto token=current();
    if (token.kind!=TokenKind::Number || token.value.size()>7)
      fail("TABLESAMPLE requires a constant percentage in the range 0 to 100");
    const auto point=token.value.find('.');
    const auto whole=token.value.substr(0,point);
    auto fraction=point==std::string::npos?std::string{}:token.value.substr(point+1);
    if (whole.size()>3 || fraction.size()>3 ||
        whole.find_first_not_of("0123456789")!=std::string::npos ||
        fraction.find_first_not_of("0123456789")!=std::string::npos)
      fail("TABLESAMPLE requires a decimal percentage with at most three fractional digits");
    fraction.resize(3,'0');
    const auto rate=static_cast<std::uint32_t>((whole.empty()?0:std::stoi(whole))*1000+std::stoi(fraction));
    if (rate>TableSampling::full) fail("TABLESAMPLE percentage exceeds 100");
    source.sampling=TableSampling{rate,{},method};
    take();
    expect(")");
    if (eat("REPEATABLE")) {
      expect("(");
      const bool negative=eat("-");
      if (!negative) eat("+");
      if (current().kind!=TokenKind::Number || current().value.size()>9 ||
          current().value.find_first_not_of("0123456789")!=std::string::npos)
        fail("REPEATABLE requires a bounded signed integer seed");
      const auto seed=std::stoll(take().value);
      source.sampling->repeatable=negative?-seed:seed;
      expect(")");
    }
  }
  rowMatchClause(source);
  pivotClause(source);
  unpivotClause(source);
  source.range = {start, previous().range.end};
  return source;
}
std::string tableSamplePercent(std::uint32_t rate) {
  auto whole=std::to_string(rate/1000);
  if (rate%1000) {
    auto fraction=std::to_string(1000+rate%1000).substr(1);
    while (fraction.back()=='0') fraction.pop_back();
    whole+='.'+fraction;
  }
  return whole;
}
void Parser::valueRows(Statement &s) {
  expect("VALUES");
  s.valuesQuery = true;
  do {
    if (context_.dialect == Dialect::MySQL) expect("ROW");
    expect("(");
    std::vector<ExprId> row;
    do {
      const auto cell = expression();
      requireScalar(cell);
      row.push_back(cell);
    } while (eat(","));
    expect(")");
    s.rows.push_back(std::move(row));
  } while (eat(","));
}
QueryId Parser::nestedCteBody() {
  if (context_.dialect!=Dialect::PostgreSQL || (queryAhead() && !at("WITH")))
    return nestedQuery();
  if (depth_>=context_.options.limits.nesting)
    fail("CTE statement nesting limit exceeded",DiagnosticCode::ResourceLimit);
  DepthGuard guard(depth_);
  auto body=statement(true);
  if (body.kind!=StatementKind::Select && body.kind!=StatementKind::Insert &&
      body.kind!=StatementKind::Update && body.kind!=StatementKind::Delete)
    fail("PostgreSQL CTE body requires SELECT, INSERT, UPDATE, or DELETE",
         DiagnosticCode::UnsupportedSyntax);
  return storeQuery(std::move(body));
}
SelectStatement Parser::select(bool nested, bool embedded, bool setOperand) {
  if (depth_ >= context_.options.limits.nesting)
    fail("query nesting limit exceeded", DiagnosticCode::ResourceLimit);
  DepthGuard guard(depth_);
  windowScopes_.push_back(std::make_unique<WindowScope>());
  struct LeaveScope {
    decltype(windowScopes_) &scopes;
    ~LeaveScope() { scopes.pop_back(); }
  } leave{windowScopes_};
  const auto begin = current().range.begin;
  SelectStatement s;
  bool recursive = false;
  auto outerCtes = withClause(recursive);
  const bool parenthesized = eat("(");
  if (parenthesized) {
    s = select(true);
    expect(")");
    auto *scope = windowScopes_.back().get();
    for (const auto &w : s.namedWindows) {
      PendingWindow value;
      value.spec = w.specification;
      value.keys = w.keys;
      scope->definitions.emplace(windowName(w.name), value);
      scope->names.push_back(w.name);
    }
    s.namedWindows.clear();
  } else if (valuesQueryAhead()) {
    valueRows(s);
  } else if (tableQueryAhead()) {
    const auto tableToken = take();
    const bool postgres = context_.dialect == Dialect::PostgreSQL;
    const bool only = postgres && eat("ONLY");
    const bool enclosed = only && eat("(");
    TableSource source;
    source.range.begin = current().range.begin;
    source.name = qualifiedName();
    source.range.end = previous().range.end;
    if (enclosed) expect(")");
    s.tableQuery = only ? TableQuerySyntax::Only : TableQuerySyntax::Table;
    if (postgres && !only && eat("*")) s.tableQuery = TableQuerySyntax::IncludeDescendants;
    Relation leaf;
    leaf.range = source.range;
    s.tables.push_back(std::move(source));
    s.from = relation(s, std::move(leaf));
    s.projections.push_back({node({ExprKind::Star, tableToken.range, {}, {}, {}}), {}});
    if (at("WHERE") || at("GROUP") || at("HAVING") || at("WINDOW") || at("JOIN"))
      fail("TABLE query accepts a relation, ordering, and pagination; use SELECT for this clause");
  } else {
    const auto extension = context_.options.extension;
    const bool fromFirst = extension == SqlExtension::DuckDb && eat("FROM");
    const auto fromRange = previous().range;
    if (fromFirst) fromClause(s);
    if (fromFirst && !at("SELECT")) {
      s.projections.push_back({node({ExprKind::Star, fromRange, {}, {}, {}}), {}});
    } else {
      expect("SELECT");
      s.distinct = eat("DISTINCT");
      if (!s.distinct) eat("ALL");
      s.fixedJoinOrder = dmlGrammar().mysql && eat("STRAIGHT_JOIN");
      selectPrefix(s);
      const bool emptyPostgresTarget=context_.dialect==Dialect::PostgreSQL &&
        extension != SqlExtension::DuckDb && !s.distinct &&
        (at("FROM") || at("WHERE") || at("GROUP") || at("HAVING") || at("WINDOW") ||
         at("ORDER") || at("LIMIT") || at("OFFSET") || at("FETCH") || at("UNION") ||
         at("INTERSECT") || at("EXCEPT") || at(";") || (nested && at(")")) ||
         current().kind==TokenKind::End);
      if (!emptyPostgresTarget) projections(s.projections,true);
    }
    if (fromFirst && at("FROM")) fail("FROM-first query cannot declare a second FROM");
    if (!fromFirst && eat("FROM"))
      fromClause(s);
    if (extension == SqlExtension::CockroachDb && !s.tables.empty() && at("AS") && at("OF", 1) &&
        at("SYSTEM", 2)) {
      take();
      take();
      expect("SYSTEM");
      expect("TIME");
      s.systemTime = expression();
      requireScalar(s.systemTime);
    }
    if (s.tables.empty() && requiresFrom())
      fail("FROM is required by the Oracle 12c/19c grammar baseline");
    if (context_.options.extension == SqlExtension::ClickHouse && eat("PREWHERE")) {
      if (s.tables.empty()) fail("PREWHERE requires FROM");
      s.prewhere = expression();
      requireScalar(s.prewhere);
    }
    if (eat("WHERE")) {
      s.where = expression();
      requireScalar(s.where);
    }
    if (eat("GROUP")) {
      expect("BY");
      s.groupByAll = (extension == SqlExtension::DuckDb ||
                      extension == SqlExtension::ClickHouse) && eat("ALL");
      if (!s.groupByAll) groupingClause(s);
      else if (at(",")) fail("GROUP BY ALL cannot be mixed with explicit grouping keys");
    }
    if (eat("HAVING")) {
      s.having = expression();
      requireScalar(s.having);
    }
    windowClause(s);
    if (supportsQualify(extension) && eat("QUALIFY")) {
      s.qualify = expression();
      requireScalar(s.qualify);
    }
  }
  s.range.begin = begin;
  s.range.end = previous().range.end;
  const bool compound = !setOperand && setPrecedence() != 0;
  if ((setOperand || compound) && s.limit.withTies && s.orderBy.empty() &&
      context_.dialect == Dialect::SQLServer)
    fail("TOP WITH TIES in a set operand requires its own ORDER BY");
  if (compound) {
    resolveWindows(s);
    s = setTail(std::move(s), 0);
  }
  if (!outerCtes.empty()) {
    if (!s.ctes.empty())
      fail("nested WITH scopes on a parenthesized query require a separate "
           "wrapper",
           DiagnosticCode::UnsupportedSyntax);
    s.ctes = std::move(outerCtes);
    s.recursiveWith = recursive;
  }
  if (setOperand) {
    resolveWindows(s);
    return s;
  }
  if (parenthesized && !compound &&
      (!s.orderBy.empty() || s.limit.count || s.limit.offset || !s.rowLocks.empty()) &&
      (at("ORDER") || at("LIMIT") || at("OFFSET") || at("FETCH")))
    fail("outer clauses on an already limited parenthesized query require a "
         "wrapper",
         DiagnosticCode::UnsupportedSyntax);
  orderBy(s);
  pagination(s);
  if (parenthesized && !s.rowLocks.empty() && (at("FOR") || at("LOCK")))
    fail("outer locking on an already locked query requires a separate query block");
  lockingRead(s);
  if (s.setOperation && s.limitBy)
    fail("LIMIT BY on a set operation requires an outer SELECT", DiagnosticCode::UnsupportedSyntax);
  if (s.limit.withTies && s.orderBy.empty() && context_.dialect != Dialect::Oracle)
    fail("WITH TIES requires ORDER BY in this query");
  if (context_.dialect == Dialect::SQLite &&
      (!s.orderBy.empty() || s.limit.count || s.limit.offset)) {
    const auto *last = &s;
    while (last->setOperation) last = &context_.ast.query(last->setOperation->right);
    if (last->valuesQuery)
      fail("SQLite queries ending in VALUES cannot have ORDER BY or LIMIT");
  }
  if (!compound)
    resolveWindows(s);
  s.range.end = previous().range.end;
  if (!embedded)
    endStatement(nested);
  return s;
}
ExprId Parser::limitValue() {
  if (current().kind != TokenKind::Number &&
      current().kind != TokenKind::Parameter)
    fail("expected nonnegative integer or parameter for row limit");
  if (current().kind == TokenKind::Number &&
      current().value.find_first_not_of("0123456789") != std::string::npos)
    fail("row limit must be an integer");
  return primary();
}
void Parser::limitOffset(SelectStatement &s, bool comma, bool all) {
  if (eat("LIMIT")) {
    s.limit.syntax = LimitSyntax::LimitOffset;
    if (!(all && eat("ALL")))
      s.limit.count = limitValue();
    if (comma && eat(",")) {
      s.limit.offset = s.limit.count;
      s.limit.count = limitValue();
      s.limit.syntax = LimitSyntax::CommaLimit;
    } else if (eat("OFFSET"))
      s.limit.offset = limitValue();
    if (context_.options.extension == SqlExtension::ClickHouse && eat("BY")) {
      if (s.limitBy) fail("duplicate LIMIT BY clause");
      GroupedRowLimit grouped;
      grouped.count = s.limit.count;
      grouped.offset = s.limit.offset;
      grouped.all = eat("ALL");
      if (!grouped.all) do {
        if (grouped.keys.size() >= context_.options.limits.columnsPerQuery)
          fail("LIMIT BY expression limit exceeded", DiagnosticCode::ResourceLimit);
        const auto key = expression();
        requireScalar(key);
        grouped.keys.push_back(key);
      } while (eat(","));
      s.limitBy = std::move(grouped);
      s.limit = {};
      if (at("LIMIT")) limitOffset(s, comma, all);
    }
  } else if (all && eat("OFFSET")) {
    s.limit.offset = limitValue();
    if (!eat("ROW"))
      eat("ROWS");
    s.limit.syntax = LimitSyntax::LimitOffset;
    if (eat("LIMIT")) {
      if (!eat("ALL"))
        s.limit.count = limitValue();
    }
  }
}
void Parser::offsetFetch(SelectStatement &s, bool requireOrder) {
  if (eat("OFFSET")) {
    if (requireOrder && s.orderBy.empty())
      fail("OFFSET requires ORDER BY in SQL Server");
    s.limit.offset = limitValue();
    if (!eat("ROW"))
      expect("ROWS");
    s.limit.syntax = LimitSyntax::Fetch;
  }
  if (eat("FETCH")) {
    if (requireOrder && !s.limit.offset)
      fail("FETCH requires OFFSET in SQL Server");
    if (!eat("FIRST"))
      expect("NEXT");
    if (!requireOrder && (at("ROW") || at("ROWS")))
      s.limit.count = node({ExprKind::Number, current().range, "1", {}, {}});
    else
      s.limit.count = limitValue();
    if (!eat("ROW"))
      expect("ROWS");
    if (!requireOrder && eat("WITH")) {
      expect("TIES");
      s.limit.withTies = true;
    } else {
      expect("ONLY");
    }
    s.limit.syntax = LimitSyntax::Fetch;
  }
}
void Parser::parse() {
  std::size_t count = 0;
  bool batchExclusive=false;
  while (current().kind != TokenKind::End) {
    if (eat(";"))
      continue;
    if (++count > context_.options.limits.statements) {
      context_.report(DiagnosticCode::ResourceLimit, current().range,
                      "statement limit exceeded");
      break;
    }
    try {
      if (batchExclusive && at("GO"))
        fail("SQL Server GO client batch boundaries require a separate script model",DiagnosticCode::UnsupportedSyntax);
      if (batchExclusive)
        fail("SQL Server schema/view definition requires a separate batch");
      auto parsed=statement();
      if (context_.dialect==Dialect::SQLServer && parsed.ddl &&
          sqlServerDefinitionRequiresBatch(parsed.kind,parsed.ddl->object)) {
        if (!context_.statements.empty())
          fail("SQL Server schema/view definition must begin a separate batch");
        if (parsed.ddl->object==ObjectKind::Schema &&
            (at("CREATE") || at("GRANT") || at("DENY") || at("REVOKE")))
          fail("SQL Server CREATE SCHEMA elements require a nested definition model",DiagnosticCode::UnsupportedSyntax);
        batchExclusive=true;
      }
      context_.statements.push_back(std::move(parsed));
    } catch (const ParseFailure &) {
      // Recover at statement boundaries; never treat a partial statement as
      // valid.
      while (current().kind != TokenKind::End && !at(";"))
        take();
      if (context_.diagnostics.size() >=
          std::max<std::size_t>(1, context_.options.limits.diagnostics))
        break;
    }
    eat(";");
  }
}
} // namespace navidog::sql::compiler
