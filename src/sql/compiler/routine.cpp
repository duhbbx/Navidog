#include "bounded_output.hpp"
#include "conversion_internal.hpp"
#include "parser.hpp"
#include "navidog/sql/compiler/script_context.hpp"
#include <algorithm>
#include <charconv>
#include <map>
#include <set>
#include <sstream>
#include <tuple>

namespace navidog::sql::compiler {
namespace {
struct Failure {};
void recordDiagnostic(RoutineCompilation &out, std::size_t limit, Diagnostic diagnostic) {
  if (out.diagnostics.size() < std::max<std::size_t>(1, limit)) {
    out.diagnostics.push_back(std::move(diagnostic));
    return;
  }
  // Body parsing and binding can discover errors in different source orders.
  auto last = std::max_element(out.diagnostics.rbegin(), out.diagnostics.rend(),
      [](const auto &a, const auto &b) { return a.range.begin < b.range.begin; });
  if (diagnostic.range.begin < last->range.begin)
    *last = std::move(diagnostic);
}
bool mysql(Dialect d) {
  return isMySqlDialect(d);
}
std::string structuralSpelling(const Token &token) {
  if (token.kind == TokenKind::Word)
    return asciiUpper(token.value);
  if (token.kind == TokenKind::Symbol || token.kind == TokenKind::Number)
    return token.value;
  return {};
}
class RoutineParser {
  Compilation lexed;
  std::vector<Token> tokens;
  std::unique_ptr<Parser> typeParser;
  std::size_t at = 0, depth = 0;
  std::size_t handlerDepth = 0;
  std::map<std::pair<ProceduralScopeId, std::string>, ExceptionCondition> mysqlConditions;
  std::map<std::pair<ProceduralScopeId, std::string>, ProceduralCursorId> cursors;
  std::set<std::string> seenPrepared;
  RoutineCompilation out;
  bool completionRecovery = false;
  RoutineCompletionSite *completionSite = nullptr;
  ProceduralScopeId scope = 0;
  struct JumpTarget {
    std::uint32_t id; // Zero for a BEGIN ... END block.
    ProceduralScopeId scope;
    std::string label;
  };
  std::vector<JumpTarget> jumpTargets;
  std::uint32_t lastLoop = 0;
  const Token &token(std::size_t n = 0) const {
    return tokens.at(std::min(at + n, tokens.size() - 1));
  }
  bool is(const std::string &s, std::size_t n = 0) const {
    return structuralSpelling(token(n)) == s;
  }
  Token take() {
    checkStop(lexed);
    auto t = token();
    if (t.kind != TokenKind::End)
      ++at;
    return t;
  }
  bool eat(const std::string &s) {
    if (!is(s))
      return false;
    take();
    return true;
  }
  [[noreturn]] void fail(const std::string &text,
                         DiagnosticCode code = DiagnosticCode::ExpectedToken) {
    failAt(token().range, text, code);
  }
  [[noreturn]] void failAt(SourceRange range, const std::string &text, DiagnosticCode code) {
    recordDiagnostic(out, lexed.options.limits.diagnostics, {code, range, text});
    throw Failure{};
  }
  void need(const std::string &s) {
    if (!eat(s))
      fail("expected '" + s + "' in routine");
  }
  void statementEnd() {
    if (lexed.dialect != Dialect::SQLServer || !is("END")) need(";");
  }
  SourceRange statementExpression() {
    return lexed.dialect == Dialect::SQLServer ? expressionUntil({";", "END"})
                                               : expressionUntil({";"});
  }
  std::string name(bool variable = true) {
    auto t = token();
    if (t.kind != TokenKind::Word && t.kind != TokenKind::Identifier &&
        !(lexed.dialect == Dialect::SQLServer &&
          t.kind == TokenKind::Parameter))
      fail("expected routine identifier");
    take();
    auto value = t.value;
    if (t.kind == TokenKind::Parameter && !value.empty() && value[0] == '@')
      value.erase(0, 1);
    auto key = canonicalIdentifier(lexed.dialect, value,
                                   t.kind == TokenKind::Identifier);
    if (variable &&
        (mysql(lexed.dialect) || lexed.dialect == Dialect::SQLServer))
      for (auto &ch : key)
        if (ch >= 'A' && ch <= 'Z')
          ch += 32;
    return key;
  }
  std::vector<std::string> qualified() {
    std::vector<std::string> n{name(false)};
    while (eat(".")) {
      if (n.size() >= 3)
        fail("routine name has too many qualifiers");
      n.push_back(name(false));
    }
    return n;
  }
  std::uint32_t node(ProceduralAstNode n) {
    if (out.ast.nodes.size() >= lexed.options.limits.nodes)
      fail("procedural node budget exceeded", DiagnosticCode::ResourceLimit);
    out.ast.nodes.push_back(std::move(n));
    return std::uint32_t(out.ast.nodes.size() - 1);
  }
  ProceduralScopeId enter() {
    if (++depth > lexed.options.limits.nesting)
      fail("procedural scope nesting exceeded", DiagnosticCode::ResourceLimit);
    auto id = ProceduralScopeId(out.ast.scopes.size());
    out.ast.scopes.push_back(
        {id, scope, {token().range.begin, token().range.begin}, {}, {}, {}});
    scope = id;
    return id;
  }
  void leave() {
    out.ast.scopes[scope].origin.end = token().range.begin;
    scope = out.ast.scopes[scope].parent;
    --depth;
  }
  ProceduralSymbolId find(const std::string &n) const {
    for (auto s = scope; s; s = out.ast.scopes[s].parent)
      for (auto id : out.ast.scopes[s].symbols)
        if (out.ast.symbols[id].name == n)
          return id;
    return 0;
  }
  ProceduralSymbolId declare(ProceduralSymbol v) {
    for (auto id : out.ast.scopes[scope].symbols)
      if (out.ast.symbols[id].name == v.name)
        fail("duplicate symbol in the same scope: " + v.name,
             DiagnosticCode::DuplicateAlias);
    v.id = std::uint32_t(out.ast.symbols.size());
    v.scope = lexed.dialect == Dialect::SQLServer ? 1 : scope;
    for (auto id : out.ast.scopes[v.scope].symbols)
      if (out.ast.symbols[id].name == v.name)
        fail("duplicate routine variable: " + v.name,
             DiagnosticCode::DuplicateAlias);
    out.ast.symbols.push_back(v);
    out.ast.scopes[v.scope].symbols.push_back(v.id);
    return v.id;
  }
  void declareFound() {
    ProceduralSymbol found;
    found.id = static_cast<ProceduralSymbolId>(out.ast.symbols.size());
    found.scope = scope;
    found.kind = SymbolKind::Implicit;
    found.name = "found";
    found.type.name = {"boolean"};
    found.origin = found.type.origin = {token().range.begin, token().range.begin};
    out.ast.symbols.push_back(found);
    // PostgreSQL adds FOUND after the parameters, with the newest binding
    // visible first even if a parameter has the same name.
    auto &symbols = out.ast.scopes[scope].symbols;
    symbols.insert(symbols.begin(), found.id);
  }
  SourceRange expressionUntil(const std::set<std::string> &stops) {
    const auto begin = token().range.begin;
    std::size_t end = begin;
    int parens = 0, cases = 0;
    while (token().kind != TokenKind::End) {
      const auto word = structuralSpelling(token());
      if (!parens && !cases && stops.count(word))
        break;
      if (word == "(")
        ++parens;
      else if (word == ")") {
        if (!parens)
          break;
        --parens;
      } else if (word == "CASE")
        ++cases;
      else if (word == "END" && cases)
        --cases;
      end = take().range.end;
    }
    if (end == begin)
      fail("expected expression in routine");
    return {begin, end};
  }
  BasicTypeSpec<std::vector<std::string>> type(TypeUse use=TypeUse::Declaration) {
    const auto begin=token().range.begin, diagnostics=lexed.diagnostics.size();
    if(!typeParser){typeParser=createParser(lexed);typeParser->setTypeTokenStream(tokens);}
    try {
      auto syntax=typeParser->parseTypeAt(begin);
      auto result=lowerTypeSpec(lexed,syntax);
      const auto resolved=resolveDeclaredType(result,lexed.dialect,use);
      if(!resolved.error.empty()){out.diagnostics.push_back({DiagnosticCode::InvalidQueryShape,result.origin,resolved.error});throw Failure{};}
      while(token().kind!=TokenKind::End && token().range.begin<result.origin.end)take();
      return result;
    }catch(const ParseFailure &) {
      for(std::size_t i=diagnostics;i<lexed.diagnostics.size();++i)out.diagnostics.push_back(lexed.diagnostics[i]);
      throw Failure{};
    }
  }

  void variableDeclaration(bool hasDeclare) {
    const auto begin = token().range.begin;
    if (hasDeclare)
      need("DECLARE");
    ProceduralSymbol v;
    std::vector<std::string> names{name()};
    while (mysql(lexed.dialect) && eat(",")) {
      if (names.size() >= lexed.options.limits.columnsPerQuery)
        fail("variable declaration exceeds the column limit", DiagnosticCode::ResourceLimit);
      names.push_back(name());
    }
    v.kind = SymbolKind::Variable;
    v.origin = {begin, token().range.begin};
    v.type = type();
    if (eat("DEFAULT") || eat(":=") || eat("="))
      v.initializer = statementExpression();
    v.origin.end = token().range.begin;
    statementEnd();
    for (auto &spelling : names) {
      v.name = std::move(spelling);
      ProceduralAstNode n;
      n.kind = ProceduralKind::Declare;
      n.scope = scope;
      n.target = declare(v);
      n.expression = v.initializer;
      n.origin = {begin, tokens[at - 1].range.end};
      pendingDeclarations.push_back(node(std::move(n)));
    }
  }
  enum class MySqlConditionUse { Declaration, Handler, Signal };
  ExceptionCondition mysqlConditionValue(MySqlConditionUse use) {
    using Kind = ExceptionConditionKind;
    ExceptionCondition condition;
    condition.origin.begin = token().range.begin;
    if (eat("SQLSTATE")) {
      if (use==MySqlConditionUse::Signal) completionKeywords({"VALUE"});
      eat("VALUE");
      if (token().kind != TokenKind::String || token().value.size() != 5 ||
          token().value.compare(0,2,"00") == 0 ||
          token().value.find_first_not_of("0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZ") != std::string::npos)
        fail("MySQL SQLSTATE needs a five-character uppercase error or warning code",
             DiagnosticCode::InvalidQueryShape);
      condition.kind = Kind::SqlState;
      condition.sqlState = take().value;
    } else if (use!=MySqlConditionUse::Signal && token().kind == TokenKind::Number) {
      const auto &spelling = token().value;
      const auto *end = spelling.data() + spelling.size();
      const auto converted = std::from_chars(spelling.data(), end, condition.errorCode);
      if (converted.ec != std::errc{} || converted.ptr != end || !condition.errorCode)
        fail("MySQL handler requires a positive integer error code", DiagnosticCode::InvalidQueryShape);
      condition.kind = Kind::MySqlErrorCode;
      take();
    } else {
      if (use==MySqlConditionUse::Declaration)
        fail("expected a SQLSTATE or MySQL error code after CONDITION FOR");
      if (use==MySqlConditionUse::Handler && eat("SQLWARNING")) condition.kind = Kind::SqlWarning;
      else if (use==MySqlConditionUse::Handler && eat("SQLEXCEPTION")) condition.kind = Kind::SqlException;
      else if (use==MySqlConditionUse::Handler && eat("NOT")) {
        need("FOUND");
        condition.kind = Kind::NotFound;
      } else {
        condition.kind = Kind::Named;
        condition.name = {name()};
      }
    }
    condition.origin.end = tokens[at - 1].range.end;
    return condition;
  }
  std::uint32_t mysqlConditionDeclaration() {
    ProceduralAstNode declaration;
    declaration.origin.begin = take().range.begin;
    declaration.kind = ProceduralKind::MySqlCondition;
    declaration.scope = scope;
    declaration.conditionName = name();
    need("CONDITION");
    need("FOR");
    auto condition = mysqlConditionValue(MySqlConditionUse::Declaration);
    const auto key = std::make_pair(scope, declaration.conditionName);
    if (!mysqlConditions.emplace(key, condition).second)
      failAt({declaration.origin.begin, condition.origin.end},
             "duplicate MySQL condition declaration: " + declaration.conditionName,
             DiagnosticCode::DuplicateAlias);
    declaration.conditions.push_back(std::move(condition));
    statementEnd();
    declaration.origin.end = tokens[at - 1].range.end;
    return node(std::move(declaration));
  }
  using HandlerKey = std::tuple<ExceptionConditionKind, std::uint32_t, std::string>;
  HandlerKey mysqlHandlerKey(const ExceptionCondition &condition) {
    const auto key = [](const ExceptionCondition &c) {
      return HandlerKey{c.kind, c.errorCode, c.sqlState};
    };
    return key(condition.kind == ExceptionConditionKind::Named ? namedMySqlCondition(condition) : condition);
  }
  const ExceptionCondition &namedMySqlCondition(const ExceptionCondition &condition) {
    for (auto block = scope; block; block = out.ast.scopes[block].parent) {
      const auto found = mysqlConditions.find({block, condition.name.front()});
      if (found != mysqlConditions.end()) return found->second;
    }
    failAt(condition.origin, "undeclared MySQL condition: " + condition.name.front(),
           DiagnosticCode::InvalidQueryShape);
  }
  void mysqlSignal(ProceduralAstNode &n) {
    const bool resignal=eat("RESIGNAL");
    if (!resignal) need("SIGNAL");
    n.kind=resignal?ProceduralKind::MySqlResignal:ProceduralKind::MySqlSignal;
    if (atCompletionSite()) {
      completionSite->keywords={"SQLSTATE"};
      if (resignal) completionSite->keywords.push_back("SET");
      std::set<std::string> seen;
      for (auto block=scope;block;block=out.ast.scopes[block].parent) {
        for (auto it=mysqlConditions.lower_bound({block,{}});
             it!=mysqlConditions.end() && it->first.first==block;++it) {
          checkStop(lexed);
          if (seen.insert(it->first.second).second && it->second.kind==ExceptionConditionKind::SqlState &&
              completionSite->conditionNames.size()<1024)
            completionSite->conditionNames.push_back(it->first.second);
        }
      }
    }
    if (!resignal || (!is("SET") && !is(";"))) {
      auto condition=mysqlConditionValue(MySqlConditionUse::Signal);
      if (condition.kind==ExceptionConditionKind::Named &&
          namedMySqlCondition(condition).kind!=ExceptionConditionKind::SqlState)
        failAt(condition.origin,"SIGNAL and RESIGNAL require a condition declared with SQLSTATE",
               DiagnosticCode::InvalidQueryShape);
      n.conditions.push_back(std::move(condition));
      completionKeywords({"SET"});
    }
    if (eat("SET")) {
      std::set<MySqlDiagnosticField> seen;
      do {
        if (atCompletionSite()) {
          completionSite->keywords.clear();
          for (const auto &field:mysqlDiagnosticFields)
            if (field.use==MySqlDiagnosticUse::SignalAndCondition && !seen.count(field.id))
              completionSite->keywords.push_back(field.spelling);
        }
        const auto field=std::find_if(std::begin(mysqlDiagnosticFields),std::end(mysqlDiagnosticFields),
            [&](const auto &candidate) {
              return candidate.use==MySqlDiagnosticUse::SignalAndCondition && is(candidate.spelling);
            });
        if (field==std::end(mysqlDiagnosticFields)) fail("unknown SIGNAL condition information field");
        const auto fieldToken=take();
        if (!seen.insert(field->id).second)
          failAt(fieldToken.range,"duplicate SIGNAL condition information field",DiagnosticCode::InvalidQueryShape);
        need("=");
        if (is("NULL"))
          fail("SIGNAL condition information cannot be NULL",DiagnosticCode::InvalidQueryShape);
        const auto value=mysqlDiagnosticValue("SIGNAL field");
        if (!is(",") && !is(";"))
          fail("SIGNAL requires a simple literal or variable",DiagnosticCode::UnsupportedSyntax);
        ProceduralAstNode item;
        item.kind=ProceduralKind::MySqlSignalValue;
        item.scope=scope;
        item.origin={fieldToken.range.begin,value.range.end};
        item.expression=value.range;
        item.mysqlSignalField=field->id;
        n.children.push_back(node(std::move(item)));
      } while (eat(","));
    }
    statementEnd();
  }
  std::uint32_t mysqlHandlerDeclaration(std::set<HandlerKey> &registered) {
    ProceduralAstNode handler;
    handler.origin.begin = take().range.begin;
    handler.kind = ProceduralKind::MySqlHandler;
    handler.scope = scope;
    if (eat("CONTINUE")) handler.mysqlHandlerMode = MySqlHandlerMode::Continue;
    else if (eat("EXIT")) handler.mysqlHandlerMode = MySqlHandlerMode::Exit;
    else fail("MySQL handlers support CONTINUE or EXIT", DiagnosticCode::UnsupportedSyntax);
    need("HANDLER");
    need("FOR");
    do {
      auto condition = mysqlConditionValue(MySqlConditionUse::Handler);
      if (!registered.insert(mysqlHandlerKey(condition)).second)
        failAt(condition.origin, "duplicate handler condition in the same block", DiagnosticCode::InvalidQueryShape);
      if (registered.size() > 1024)
        failAt(condition.origin, "MySQL handler condition budget exceeded", DiagnosticCode::ResourceLimit);
      handler.conditions.push_back(std::move(condition));
    } while (eat(","));
    // Handler bodies see enclosing declarations. Their jump labels start
    // in a new scope and any labels they declare stay local to the handler.
    auto enclosingTargets=std::move(jumpTargets);
    jumpTargets.clear();
    handler.children.push_back(statement());
    jumpTargets=std::move(enclosingTargets);
    handler.origin.end = tokens[at - 1].range.end;
    return node(std::move(handler));
  }
  std::vector<std::uint32_t> pendingDeclarations;
  void requireUnusedMySqlLabel(const std::string &label,std::size_t begin) {
    if (!label.empty() && std::any_of(jumpTargets.begin(),jumpTargets.end(),[&](const auto &target) {
          return target.label==label;
        }))
      failAt({begin,token().range.begin},"duplicate active MySQL label: "+label,DiagnosticCode::DuplicateAlias);
  }
  void beginLoop(ProceduralAstNode &node) {
    if (mysql(lexed.dialect)) requireUnusedMySqlLabel(node.loopLabel,node.origin.begin);
    node.loop=++lastLoop;
    jumpTargets.push_back({node.loop,node.scope,node.loopLabel});
  }
  void endLoop(const ProceduralAstNode &node, const std::string &keyword) {
    need("END");
    need(keyword);
    if (!is(";")) {
      const auto at = token().range;
      if (node.loopLabel.empty() || name() != node.loopLabel)
        failAt(at, "end loop label does not match its declaration", DiagnosticCode::InvalidQueryShape);
    }
    need(";");
    jumpTargets.pop_back();
  }
  bool mysqlBlockLabelAhead() const {
    return mysql(lexed.dialect) && (token().kind==TokenKind::Word || token().kind==TokenKind::Identifier) &&
        is(":",1) && is("BEGIN",2);
  }
  bool hasLabel() const {
    return lexed.dialect == Dialect::PostgreSQL ? is("<<") : is("<") && is("<", 1);
  }
  std::string readLabel() {
    if (lexed.dialect == Dialect::PostgreSQL) {
      need("<<");
      auto label = name();
      need(">>");
      return label;
    }
    need("<");
    need("<");
    auto label = name();
    need(">");
    need(">");
    return label;
  }
  std::string loopLabel(bool my, bool pl) {
    auto loopAt = [&](std::size_t n) {
      return is("LOOP", n) || is("WHILE", n) || (my && is("REPEAT", n)) || (pl && is("FOR", n));
    };
    if (my && is(":", 1) && loopAt(2)) {
      auto label = name();
      need(":");
      return label;
    }
    if (pl && hasLabel()) {
      const bool pg = lexed.dialect == Dialect::PostgreSQL;
      if ((pg && is(">>", 2) && loopAt(3)) ||
          (!pg && is(">", 3) && is(">", 4) && loopAt(5)))
        return readLabel();
    }
    return {};
  }
  void loopJump(ProceduralAstNode &node, bool my) {
    const auto jumpToken = take();
    const auto jump = structuralSpelling(jumpToken);
    node.kind = jump == "CONTINUE" || jump == "ITERATE" ? ProceduralKind::Continue : ProceduralKind::Exit;
    const auto labelRange = token().range;
    const bool iterate=node.kind==ProceduralKind::Continue;
    if (my && atCompletionSite()) {
      completionSite->labelTarget=true;
      for (auto it=jumpTargets.rbegin();it!=jumpTargets.rend();++it)
        if (!it->label.empty() && (!iterate || it->id))
          completionSite->labels.emplace_back(it->label,it->id!=0);
    }
    std::string label;
    if (my || (!is("WHEN") && !is(";"))) label=name();
    const auto target=std::find_if(jumpTargets.rbegin(),jumpTargets.rend(),[&](const auto &candidate) {
      return (!iterate || candidate.id) && (label.empty() || candidate.label==label);
    });
    if (!label.empty() && !my) {
      const auto stop=target==jumpTargets.rend()?0:target->scope;
      for (auto visible = scope; visible && visible != stop; visible = out.ast.scopes[visible].parent) {
        if (out.ast.scopes[visible].label != label) continue;
        if (node.kind == ProceduralKind::Exit)
          failAt(labelRange, "EXIT to a block label needs a block-control mapping", DiagnosticCode::UnsupportedSyntax);
        failAt(labelRange, "CONTINUE requires a loop label", DiagnosticCode::InvalidQueryShape);
      }
    }
    if (target==jumpTargets.rend()) {
      failAt(label.empty()?jumpToken.range:labelRange,label.empty()
          ?"unlabeled jump requires an enclosing loop"
          :my?jump+" has no matching active label: "+label:"loop label is outside the active scope: "+label,
          DiagnosticCode::InvalidQueryShape);
    }
    if (target->id) node.loop=target->id;
    else node.blockTarget=target->scope;
    node.loopLabel = std::move(label);
    if (!my && eat("WHEN")) node.expression = expressionUntil({";"});
    need(";");
  }
  std::vector<std::uint32_t> list(const std::set<std::string> &ends,
                                  bool ifBranch = false) {
    std::vector<std::uint32_t> result;
    while (token().kind != TokenKind::End) {
      if (completionSite && token().range.begin == completionSite->offset) {
        completionSite->statement = true;
        completionSite->closesIf = ifBranch;
      }
      if (ends.count(structuralSpelling(token())))
        break;
      if (eat(";"))
        continue;
      if (!completionRecovery) {
        result.push_back(statement());
        continue;
      }
      const auto start = at, scopeCount = out.ast.scopes.size(), targetCount = jumpTargets.size();
      try {
        result.push_back(statement());
      } catch (const Failure &) {
        if (out.ast.scopes.size() != scopeCount || jumpTargets.size() != targetCount ||
            out.diagnostics.empty() ||
            out.diagnostics.size() >= std::min<std::size_t>(32, lexed.options.limits.diagnostics) ||
            out.diagnostics.back().code == DiagnosticCode::ResourceLimit)
          throw;
        // Recover only across a flat statement. Leave control markers to
        // the enclosing block and never infer declarations past a broken block.
        at = start;
        static const std::set<std::string> control{
            "BEGIN", "DECLARE", "IF", "THEN", "ELSE", "ELSIF", "ELSEIF",
            "EXCEPTION", "FOR", "WHILE", "LOOP", "REPEAT", "UNTIL", "CASE"};
        while (token().kind != TokenKind::End && !is(";") && !is("END") &&
               !ends.count(structuralSpelling(token()))) {
          if (control.count(structuralSpelling(token())))
            throw;
          take();
        }
        if (!eat(";") && at == start)
          throw;
      }
    }
    return result;
  }
  void exceptionHandlers(ProceduralAstNode &block) {
    const bool pg = lexed.dialect == Dialect::PostgreSQL;
    if (!pg && lexed.dialect != Dialect::Oracle)
      fail("EXCEPTION blocks require a PL/pgSQL or PL/SQL routine", DiagnosticCode::UnsupportedSyntax);
    if (!is("WHEN")) fail("EXCEPTION requires a WHEN handler");
    bool othersSeen = false;
    while (is("WHEN")) {
      if (othersSeen && !pg) fail("Oracle WHEN OTHERS must be the last exception handler");
      ProceduralAstNode handler;
      handler.kind = ProceduralKind::ExceptionHandler;
      handler.origin.begin = take().range.begin;
      do {
        ExceptionCondition condition;
        condition.origin.begin = token().range.begin;
        if (eat("OTHERS")) {
          condition.kind = ExceptionConditionKind::Others;
          othersSeen = true;
        } else if (pg && eat("SQLSTATE")) {
          if (token().kind != TokenKind::String || token().value.size() != 5 ||
              token().value.find_first_not_of("0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZ") != std::string::npos)
            fail("SQLSTATE must be a five-character uppercase code");
          condition.kind = ExceptionConditionKind::SqlState;
          condition.sqlState = take().value;
        } else {
          condition.name = qualified();
          if (pg && condition.name.size() != 1) fail("PL/pgSQL exception condition must be unqualified");
        }
        condition.origin.end = tokens[at - 1].range.end;
        handler.conditions.push_back(std::move(condition));
      } while (eat("OR"));
      if (!pg && othersSeen && handler.conditions.size() != 1)
        fail("Oracle WHEN OTHERS must be a separate exception handler");
      need("THEN");
      handler.scope = enter();
      out.ast.scopes[scope].origin.begin = tokens[at - 1].range.end;
      if (pg) for (const auto name : {"sqlstate", "sqlerrm"}) {
        ProceduralSymbol status;
        status.kind = SymbolKind::ExceptionStatus;
        status.name = name;
        status.type.name = {"text"};
        status.origin = status.type.origin = {handler.origin.begin, handler.origin.begin};
        declare(std::move(status));
      }
      ++handlerDepth;
      handler.children = list({"WHEN", "END"});
      --handlerDepth;
      if (!pg && handler.children.empty()) fail("Oracle exception handler requires a statement");
      handler.origin.end = token().range.begin;
      leave();
      block.handlers.push_back(node(std::move(handler)));
    }
  }
  std::uint32_t tryCatch() {
    const auto begin = token().range.begin;
    if (out.ast.function) fail("TRY/CATCH is unavailable in a T-SQL function", DiagnosticCode::InvalidQueryShape);
    ProceduralAstNode n;
    n.kind = ProceduralKind::TsqlTryCatch;
    n.scope = scope;
    n.origin.begin = begin;
    need("BEGIN");
    need("TRY");
    out.ast.scopes[enter()].origin.begin = tokens[at - 1].range.end;
    n.children = list({"END"});
    leave();
    if (n.children.empty()) fail("BEGIN TRY requires a statement");
    need("END");
    need("TRY");
    need("BEGIN");
    need("CATCH");
    out.ast.scopes[enter()].origin.begin = tokens[at - 1].range.end;
    ++handlerDepth;
    n.otherwise = list({"END"});
    --handlerDepth;
    leave();
    need("END");
    need("CATCH");
    n.hasElse = true;
    n.origin.end = tokens[at - 1].range.end;
    return node(std::move(n));
  }
  std::uint32_t block(bool oracleHeader = false) {
    if (lexed.dialect == Dialect::SQLServer && is("BEGIN") && is("TRY", 1))
      return tryCatch();
    const auto begin=token().range.begin;
    const bool my=mysql(lexed.dialect);
    std::string label;
    if (mysqlBlockLabelAhead()) {
      label=name();
      need(":");
      requireUnusedMySqlLabel(label,begin);
    } else if (hasLabel()) label=readLabel();
    const auto id=enter();
    if (my && !label.empty()) jumpTargets.push_back({0,id,label});
    out.ast.scopes[id].label = label;
    ProceduralAstNode n;
    n.kind = ProceduralKind::Block;
    n.scope = id;
    n.origin.begin = begin;
    auto old = std::move(pendingDeclarations);
    pendingDeclarations.clear();
    if (eat("DECLARE") || oracleHeader) {
      while (!is("BEGIN") && token().kind != TokenKind::End)
        variableDeclaration(false);
    }
    need("BEGIN");
    if (mysql(lexed.dialect)) {
      std::set<HandlerKey> handlerKeys;
      enum class DeclarationPhase { Variables, Cursors, Handlers };
      auto phase = DeclarationPhase::Variables;
      while (is("DECLARE")) {
        if (is("HANDLER", 2)) {
          phase = DeclarationPhase::Handlers;
          const auto handler = mysqlHandlerDeclaration(handlerKeys);
          pendingDeclarations.push_back(handler);
        } else if (is("CURSOR", 2)) {
          if (phase == DeclarationPhase::Handlers)
            fail("MySQL cursor declarations must precede handlers",
                 DiagnosticCode::InvalidQueryShape);
          phase = DeclarationPhase::Cursors;
          pendingDeclarations.push_back(cursorDeclaration());
        } else {
          if (phase != DeclarationPhase::Variables)
            fail("MySQL variable and condition declarations must precede cursors and handlers",
                 DiagnosticCode::InvalidQueryShape);
          if (is("CONDITION", 2)) {
            const auto condition = mysqlConditionDeclaration();
            pendingDeclarations.push_back(condition);
          } else variableDeclaration(true);
        }
      }
    }
    n.children = std::move(pendingDeclarations);
    pendingDeclarations = std::move(old);
    auto body = list({"END", "EXCEPTION"});
    n.children.insert(n.children.end(), body.begin(), body.end());
    if (eat("EXCEPTION")) exceptionHandlers(n);
    need("END");
    if (my) {
      if (token().kind==TokenKind::Word || token().kind==TokenKind::Identifier) {
        const auto endLabel=token().range;
        if (label.empty() || name()!=label)
          failAt(endLabel,"block end label does not match its declaration",DiagnosticCode::InvalidQueryShape);
      }
      if (!label.empty()) jumpTargets.pop_back();
    } else if (!label.empty() && (token().kind==TokenKind::Word || token().kind==TokenKind::Identifier) &&
               canonicalIdentifier(lexed.dialect,token().value,token().kind==TokenKind::Identifier)==label)
      take();
    if (!my && lexed.dialect != Dialect::SQLServer &&
        token().kind == TokenKind::Word && !is("IF") && !is("LOOP") &&
        !is("WHILE") && !is("ELSE") && !is("LANGUAGE"))
      fail("named block endings are not covered",
           DiagnosticCode::UnsupportedSyntax);
    n.origin.end = tokens[at - 1].range.end;
    leave();
    return node(std::move(n));
  }
  void caseStatement(ProceduralAstNode &n) {
    if (lexed.dialect == Dialect::SQLServer)
      fail("T-SQL CASE is an expression, not a procedural statement",
           DiagnosticCode::UnsupportedSyntax);
    n.kind = ProceduralKind::Case;
    const bool simple = !is("WHEN");
    if (simple)
      n.expression = expressionUntil({"WHEN"});
    if (!is("WHEN"))
      fail("procedural CASE requires at least one WHEN");
    do {
      ProceduralAstNode branch;
      branch.kind = ProceduralKind::CaseWhen;
      branch.scope = scope;
      branch.origin.begin = take().range.begin;
      if (simple) {
        do {
          ProceduralAstNode operand;
          operand.kind = ProceduralKind::CaseOperand;
          operand.scope = scope;
          operand.origin = operand.expression = expressionUntil({",", "THEN"});
          branch.caseOperands.push_back(node(std::move(operand)));
          if (!eat(","))
            break;
          if (lexed.dialect != Dialect::PostgreSQL)
            fail("only PL/pgSQL supports several values in one CASE WHEN");
        } while (true);
      } else {
        branch.expression = expressionUntil({"THEN"});
      }
      need("THEN");
      branch.children = list({"WHEN", "ELSE", "END"});
      if (branch.children.empty() && lexed.dialect != Dialect::PostgreSQL)
        fail("procedural CASE WHEN requires a statement");
      branch.origin.end = token().range.begin;
      n.children.push_back(node(std::move(branch)));
    } while (is("WHEN"));
    n.hasElse = eat("ELSE");
    if (n.hasElse) {
      n.otherwise = list({"END"});
      if (n.otherwise.empty() && lexed.dialect != Dialect::PostgreSQL)
        fail("procedural CASE ELSE requires a statement");
    }
    need("END");
    need("CASE");
    need(";");
  }
  void numericFor(ProceduralAstNode &n) {
    n.kind = ProceduralKind::NumericFor;
    ProceduralSymbol index;
    index.kind = SymbolKind::LoopIndex;
    index.origin = token().range;
    index.name = name();
    const bool oracle = lexed.dialect == Dialect::Oracle;
    index.type.name = {oracle ? "PLS_INTEGER" : "integer"};
    index.type.origin = index.origin;
    need("IN");
    n.reverseFor = eat("REVERSE");
    const auto bound = [&](const std::set<std::string> &ends) {
      ProceduralAstNode expression;
      expression.kind = ProceduralKind::RangeBound;
      expression.scope = scope;
      expression.origin = expression.expression = expressionUntil(ends);
      n.rangeBounds.push_back(node(std::move(expression)));
    };
    bound({"..", "LOOP"});
    if (!eat(".."))
      fail("FOR over a cursor or query requires a row-iteration model", DiagnosticCode::UnsupportedSyntax);
    bound({"BY", "LOOP"});
    if (eat("BY")) {
      if (oracle) fail("Oracle numeric FOR does not support a BY step");
      bound({"LOOP"});
    }
    need("LOOP");
    n.scope = enter();
    out.ast.scopes[scope].label = n.loopLabel;
    n.target = declare(std::move(index));
    beginLoop(n);
    n.children = list({"END"});
    if (oracle && n.children.empty()) fail("Oracle numeric FOR requires a body statement");
    endLoop(n, "LOOP");
    leave();
  }
  ProceduralSymbolId scalarTarget(std::string_view action) {
    const auto start = token().range.begin;
    ProceduralSymbolId id = 0;
    const bool positionalParameter = lexed.dialect == Dialect::PostgreSQL &&
        token().kind == TokenKind::Parameter && !token().value.empty() && token().value.front() == '$';
    auto spelling = positionalParameter ? take().value : name();
    if (positionalParameter) {
      std::size_t ordinal = 0;
      const auto end = spelling.data() + spelling.size();
      const auto parsed = std::from_chars(spelling.data() + 1, end, ordinal);
      if (parsed.ec == std::errc{} && parsed.ptr == end && ordinal > 0 && ordinal <= out.ast.parameters.size())
        id = out.ast.parameters[ordinal - 1];
    } else if ((lexed.dialect == Dialect::PostgreSQL || lexed.dialect == Dialect::Oracle) && eat(".")) {
      const auto label = std::move(spelling);
      spelling = name();
      for (auto visible = scope; visible; visible = out.ast.scopes[visible].parent) {
        const auto &block = out.ast.scopes[visible];
        for (auto symbol : block.symbols) {
          const auto &status = out.ast.symbols[symbol];
          if (status.kind == SymbolKind::ExceptionStatus && status.name == spelling &&
              routineSymbolLabel(out.ast, status) == label) { id = symbol; break; }
        }
        if (id) break;
        if (block.label != label && (visible != 1 || out.ast.name.back() != label)) continue;
        for (auto symbol : block.symbols)
          if (out.ast.symbols[symbol].name == spelling) { id = symbol; break; }
        break;
      }
    } else id = find(spelling);
    const SourceRange position{start, tokens[at - 1].range.end};
    if (!id) {
      recordDiagnostic(out, lexed.options.limits.diagnostics,
          {DiagnosticCode::UnknownColumn, position,
           "undeclared procedural variable: " + std::string(lexed.source.slice(position))});
      return 0;
    }
    const auto &symbol = out.ast.symbols[id];
    if (!routineSymbolWritable(lexed.dialect, symbol)) {
      recordDiagnostic(out, lexed.options.limits.diagnostics,
          {DiagnosticCode::InvalidQueryShape, position,
           std::string(action) + " cannot write to read-only variable: " + spelling});
      return 0;
    }
    return id;
  }
  Token mysqlDiagnosticValue(std::string_view owner) {
    if (atCompletionSite()) completionSite->scalarVariable=true;
    const auto kind=token().kind;
    if (kind!=TokenKind::String && kind!=TokenKind::Number && kind!=TokenKind::Variable &&
        kind!=TokenKind::Word && kind!=TokenKind::Identifier)
      fail(std::string(owner)+" requires a simple literal or variable");
    return take();
  }
  void getMySqlDiagnostics(ProceduralAstNode &n) {
    need("GET");
    n.kind=ProceduralKind::MySqlGetDiagnostics;
    auto &read=n.mysqlDiagnostics.emplace();
    const bool stackedAllowed=lexed.dialect!=Dialect::MariaDB;
    if (stackedAllowed) completionKeywords({"DIAGNOSTICS","CURRENT","STACKED"});
    else completionKeywords({"DIAGNOSTICS","CURRENT"});
    if (eat("STACKED")) {
      if (!stackedAllowed)
        failAt(tokens[at-1].range,"MariaDB supports only the current diagnostics area",
               DiagnosticCode::UnsupportedSyntax);
      read.stacked=true;
      completionKeywords({"DIAGNOSTICS"});
    } else if (eat("CURRENT")) completionKeywords({"DIAGNOSTICS"});
    need("DIAGNOSTICS");
    completionKeywords({"CONDITION"});
    const bool condition=eat("CONDITION");
    if (condition) n.expression=mysqlDiagnosticValue("GET DIAGNOSTICS CONDITION").range;
    do {
      const auto start=token().range;
      if (read.targets.size()>=lexed.options.limits.columnsPerQuery)
        fail("diagnostic target count exceeds the column limit",DiagnosticCode::ResourceLimit);
      if (atCompletionSite()) completionSite->variableTarget=true;
      MySqlDiagnosticTarget target{};
      if (token().kind==TokenKind::Variable) {
        const auto session=take();
        if (!isSimpleMySqlUserVariable(session.value))
          failAt(session.range,"GET DIAGNOSTICS requires a local or user variable target",
                 DiagnosticCode::InvalidQueryShape);
        target.destination=session.value;
      } else target.destination=scalarTarget("GET DIAGNOSTICS");
      need("=");
      if (atCompletionSite())
        for (const auto &field:mysqlDiagnosticFields)
          if (field.readable(lexed.dialect,condition)) completionSite->keywords.push_back(field.spelling);
      const auto field=std::find_if(std::begin(mysqlDiagnosticFields),std::end(mysqlDiagnosticFields),
          [&](const auto &candidate) { return candidate.readable(lexed.dialect,condition) && is(candidate.spelling); });
      if (field==std::end(mysqlDiagnosticFields))
        fail("unknown field for this GET DIAGNOSTICS form");
      target.field=field->id;
      target.origin={start.begin,take().range.end};
      read.targets.push_back(std::move(target));
    } while (eat(","));
    statementEnd();
  }
  void getDiagnostics(ProceduralAstNode &n) {
    need("GET");
    n.kind = ProceduralKind::GetDiagnostics;
    auto &read = n.pgDiagnostics.emplace();
    if (eat("STACKED")) {
      read.area = PgDiagnosticArea::Stacked;
      if (!handlerDepth)
        failAt(tokens[at - 1].range, "STACKED DIAGNOSTICS requires an active exception handler",
               DiagnosticCode::InvalidQueryShape);
    } else eat("CURRENT");
    need("DIAGNOSTICS");
    do {
      const auto position = token().range;
      if (read.targets.size() >= lexed.options.limits.columnsPerQuery)
        failAt(position, "diagnostic target count exceeds the column limit", DiagnosticCode::ResourceLimit);
      const auto target = scalarTarget("GET DIAGNOSTICS");
      if (!eat("=")) need(":=");
      const auto fieldToken = token();
      const auto field = std::find_if(std::begin(pgDiagnosticFields), std::end(pgDiagnosticFields),
          [&](const auto &item) { return is(item.spelling); });
      if (field == std::end(pgDiagnosticFields))
        fail("unknown PL/pgSQL diagnostic field");
      if (!field->available(read.area))
        failAt(fieldToken.range, "diagnostic field is unavailable in this area: " + std::string(field->spelling),
               DiagnosticCode::InvalidQueryShape);
      take();
      read.targets.push_back({target, field->id, {position.begin, fieldToken.range.end}});
    } while (eat(","));
    need(";");
  }
  void tsqlThrow(ProceduralAstNode &n) {
    if (out.ast.function)
      fail("THROW is unavailable in a T-SQL function", DiagnosticCode::InvalidQueryShape);
    const auto keyword = take();
    if (is(";") || is("END")) {
      if (!handlerDepth)
        failAt(keyword.range, "parameterless THROW requires an active CATCH block", DiagnosticCode::InvalidQueryShape);
      n.kind = ProceduralKind::Rethrow;
      statementEnd();
      return;
    }
    n.kind = ProceduralKind::TsqlThrow;
    auto &args = n.throwArguments.emplace();
    for (std::size_t i = 0; i < args.size(); ++i) {
      if (i) need(",");
      const auto argument = token();
      auto &arg = args[i];
      arg.origin = argument.range;
      if (argument.kind == TokenKind::Parameter && !argument.value.empty() && argument.value[0] == '@') {
        arg.value = scalarTarget("THROW");
      } else if (i == 1 && argument.kind == TokenKind::String) {
        arg.value = take().value;
        arg.unicode = lexed.source.text()[argument.range.begin] != '\'';
      } else if (i != 1 && argument.kind == TokenKind::Number) {
        std::int64_t number = 0;
        const auto end = argument.value.data() + argument.value.size();
        const auto parsed = std::from_chars(argument.value.data(), end, number);
        const auto low = i == 0 ? 50000 : 0;
        const auto high = i == 0 ? std::int64_t{2147483647} : 255;
        if (parsed.ec != std::errc{} || parsed.ptr != end || number < low || number > high)
          failAt(argument.range, i == 0 ? "THROW error number must be an integer from 50000 through 2147483647"
                                       : "THROW state must be an integer from 0 through 255",
                 DiagnosticCode::InvalidQueryShape);
        take();
        arg.value = number;
      } else {
        fail(i == 1 ? "THROW message requires a string literal or local variable"
                    : "THROW requires an integer literal or local variable");
      }
    }
    statementEnd();
  }
  std::pair<std::size_t, std::size_t> selectIntoTokens(std::size_t begin, std::size_t end) {
    int parens = 0, cases = 0;
    auto select = end;
    auto into = end;
    for (auto i = begin; i < end; ++i) {
      if ((i & 255u) == 0) checkStop(lexed);
      const auto word = structuralSpelling(tokens[i]);
      if (word == "(") ++parens;
      else if (word == ")") --parens;
      else if (word == "CASE") ++cases;
      else if (word == "END" && cases) --cases;
      else if (!parens && !cases) {
        if (select == end && (word == "INSERT" || word == "UPDATE" || word == "DELETE" || word == "MERGE"))
          return {end, end};
        if (word == "SELECT" && select == end) select = i;
        if (select != end && word == "INTO" && i > begin &&
            structuralSpelling(tokens[i - 1]) != "AS" && structuralSpelling(tokens[i - 1]) != ".") {
          if (into != end)
            failAt(tokens[i].range, "SELECT contains several INTO clauses", DiagnosticCode::InvalidQueryShape);
          into = i;
        }
      }
    }
    return {select, into};
  }
  std::uint32_t cursorDeclaration() {
    ProceduralAstNode n;
    n.kind = ProceduralKind::CursorDeclare;
    n.scope = scope;
    n.origin.begin = take().range.begin;
    const auto position = token().range;
    auto spelling = name();
    need("CURSOR");
    need("FOR");
    const auto first = at;
    n.expression = statementExpression();
    if (n.expression.begin == n.expression.end)
      fail("DECLARE CURSOR requires a SELECT query");
    const auto into = selectIntoTokens(first, at).second;
    if (into != at)
      failAt(tokens[into].range, "a MySQL cursor query cannot contain INTO",
             DiagnosticCode::InvalidQueryShape);
    statementEnd();
    const auto id = static_cast<ProceduralCursorId>(out.ast.cursors.size());
    if (!cursors.emplace(std::make_pair(scope, spelling), id).second)
      failAt(position, "duplicate MySQL cursor: " + spelling,
             DiagnosticCode::DuplicateAlias);
    out.ast.cursors.push_back({id, scope, std::move(spelling), position});
    out.ast.scopes[scope].cursors.push_back(id);
    n.cursor = CursorUse{id, position, {}};
    n.origin.end = tokens[at - 1].range.end;
    return node(std::move(n));
  }
  void cursorStatement(ProceduralAstNode &n) {
    const auto operation = structuralSpelling(take());
    n.kind = operation == "OPEN" ? ProceduralKind::CursorOpen
           : operation == "FETCH" ? ProceduralKind::CursorFetch
                                  : ProceduralKind::CursorClose;
    if (n.kind == ProceduralKind::CursorFetch) {
      if (eat("NEXT"))
        need("FROM");
      else
        eat("FROM");
    }
    auto &use = n.cursor.emplace();
    use.origin = token().range;
    if (completionSite && completionSite->offset == use.origin.begin)
      completionSite->cursorName = true;
    const auto spelling = name();
    for (auto visible = scope; visible; visible = out.ast.scopes[visible].parent) {
      const auto found = cursors.find({visible, spelling});
      if (found != cursors.end()) {
        use.cursor = found->second;
        break;
      }
    }
    if (!use.cursor)
      recordDiagnostic(out, lexed.options.limits.diagnostics,
                       {DiagnosticCode::InvalidQueryShape, use.origin,
                        "undeclared MySQL cursor: " + spelling});
    if (n.kind == ProceduralKind::CursorFetch) {
      need("INTO");
      do {
        if (use.targets.size() >= lexed.options.limits.columnsPerQuery)
          fail("FETCH target count exceeds the column limit", DiagnosticCode::ResourceLimit);
        if (completionSite && completionSite->offset == token().range.begin)
          completionSite->variableTarget = true;
        use.targets.push_back(scalarTarget("FETCH INTO"));
      } while (eat(","));
    }
    statementEnd();
  }
  void selectTargets(ProceduralAstNode &n, std::size_t begin, std::size_t end) {
    if (lexed.dialect == Dialect::SQLServer || (!is("SELECT") && !is("WITH"))) return;
    const auto [select, into] = selectIntoTokens(begin, end);
    if (into == end) return;
    at = into + 1;
    auto &assignment = n.selectInto.emplace();
    assignment.origin.begin = tokens[into].range.begin;
    assignment.mode = mysql(lexed.dialect) ? SelectIntoMode::SingleRowOrUnchanged :
        lexed.dialect == Dialect::Oracle ? SelectIntoMode::ExactlyOne : SelectIntoMode::FirstRowOrNull;
    if (lexed.dialect == Dialect::PostgreSQL && eat("STRICT"))
      assignment.mode = SelectIntoMode::ExactlyOne;
    if (mysql(lexed.dialect) && (is("OUTFILE") || is("DUMPFILE") || token().kind == TokenKind::Variable))
      fail("SELECT INTO file or session variable needs its own execution policy", DiagnosticCode::UnsupportedSyntax);
    do {
      if (assignment.targets.size() >= lexed.options.limits.columnsPerQuery)
        fail("SELECT INTO target count exceeds the column limit", DiagnosticCode::ResourceLimit);
      assignment.targets.push_back(scalarTarget("SELECT INTO"));
    } while (eat(","));
    assignment.origin.end = tokens[at - 1].range.end;
    const bool beforeProjection = lexed.dialect == Dialect::PostgreSQL && into == select + 1;
    if (at < end && !beforeProjection && !is("FROM") && !is("WHERE") && !is("GROUP") && !is("HAVING") &&
        !is("WINDOW") && !is("ORDER") && !is("LIMIT") && !is("OFFSET") && !is("FETCH") &&
        !is("FOR") && !is("UNION") && !is("EXCEPT") && !is("INTERSECT") && !is("MINUS"))
      fail("expected SELECT clause after INTO targets");
    at = end;
  }
  void preparedCompletion(std::size_t first,std::size_t end) {
    if (!completionSite) return;
    const auto keyword=structuralSpelling(tokens[first]);
    const bool prepare=keyword=="PREPARE", execute=keyword=="EXECUTE";
    const auto nameAt=first+(prepare || execute?1:2);
    const auto follows=[&](std::size_t i) {
      return i<end && tokens[i].range.begin==completionSite->offset;
    };
    const auto isName=[&](std::size_t i) {
      return i<end && (tokens[i].kind==TokenKind::Word || tokens[i].kind==TokenKind::Identifier);
    };
    if (!(prepare || execute) && follows(first+1)) {
      completionSite->keywords={"PREPARE"};
      return;
    }
    if (!prepare && follows(nameAt)) {
      completionSite->preparedNameTarget=true;
      completionSite->preparedNames.assign(seenPrepared.begin(),seenPrepared.end());
    }
    if ((prepare || execute) && isName(nameAt) && follows(nameAt+1))
      completionSite->keywords={prepare?"FROM":"USING"};
    if (prepare && isName(nameAt) && end==nameAt+3 &&
        structuralSpelling(tokens[nameAt+1])=="FROM" &&
        (tokens[nameAt+2].kind==TokenKind::String || tokens[nameAt+2].kind==TokenKind::Variable) &&
        seenPrepared.size()<1024) {
      const auto &name=tokens[nameAt];
      seenPrepared.insert(canonicalIdentifier(lexed.dialect,name.value,name.kind==TokenKind::Identifier));
    } else if (!prepare && !execute && isName(nameAt) && end==nameAt+1 &&
               structuralSpelling(tokens[nameAt-1])=="PREPARE" && !follows(nameAt)) {
      const auto &name=tokens[nameAt];
      seenPrepared.erase(canonicalIdentifier(lexed.dialect,name.value,name.kind==TokenKind::Identifier));
    }
  }
  std::uint32_t statement() {
    if (++depth > lexed.options.limits.nesting)
      fail("procedural statement nesting exceeded",
           DiagnosticCode::ResourceLimit);
    struct DepthGuard {
      std::size_t &depth;
      ~DepthGuard() { --depth; }
    } guard{depth};
    const auto begin = token().range.begin;
    const bool my = mysql(lexed.dialect);
    const bool plLoop = lexed.dialect == Dialect::PostgreSQL || lexed.dialect == Dialect::Oracle;
    ProceduralAstNode n;
    n.scope = scope;
    n.origin.begin = begin;
    n.loopLabel = loopLabel(my, plLoop);
    if (is("BEGIN") || mysqlBlockLabelAhead() || hasLabel() ||
        (is("DECLARE") && lexed.dialect != Dialect::SQLServer)) {
      auto id = block();
      if (lexed.dialect == Dialect::SQLServer)
        eat(";");
      else
        need(";");
      return id;
    }
    if (lexed.dialect == Dialect::SQLServer && is("DECLARE")) {
      auto old = pendingDeclarations.size();
      variableDeclaration(true);
      auto id = pendingDeclarations.back();
      pendingDeclarations.resize(old);
      return id;
    }
    const bool transaction=my && ((is("START") && is("TRANSACTION",1)) || is("COMMIT") || is("ROLLBACK"));
    if (transaction && out.ast.function)
      fail("transaction control is unavailable in stored functions",DiagnosticCode::InvalidQueryShape);
    const bool cursor = my && (is("OPEN") || is("FETCH") || is("CLOSE"));
    const bool signal = my && (is("SIGNAL") || is("RESIGNAL"));
    const bool preparedDialect=lexed.dialect==Dialect::MySQL || lexed.dialect==Dialect::MariaDB;
    const bool prepared=preparedDialect && (is("PREPARE") || is("EXECUTE") || is("DEALLOCATE") ||
        (is("DROP") && (is("PREPARE",1) || (completionSite && token(1).range.begin==completionSite->offset))));
    if (prepared && out.ast.function)
      fail("dynamic prepared statements are unavailable in stored functions",DiagnosticCode::InvalidQueryShape);
    if ((!plLoop && is("FOR")) || (!plLoop && (is("EXIT") || is("CONTINUE"))) ||
        (!plLoop && !my && is("LOOP")) || (!my && (is("REPEAT") || is("LEAVE") || is("ITERATE"))) ||
        (!prepared && is("EXECUTE")) || is("EXEC") || (!my && (is("OPEN") || is("FETCH") || is("CLOSE"))) ||
        (is("RAISE") && !is(";", 1)) || (!signal && (is("SIGNAL") || is("RESIGNAL"))) ||
        (!transaction && (is("COMMIT") || is("ROLLBACK"))))
      fail("procedural construct " + token().value +
               " requires a dedicated control-flow/exception/transaction "
               "mapping",
           DiagnosticCode::UnsupportedSyntax);
    if (cursor) {
      cursorStatement(n);
    } else if (signal) {
      mysqlSignal(n);
    } else if (my && is("GET")) {
      getMySqlDiagnostics(n);
    } else if (lexed.dialect == Dialect::SQLServer && token().kind == TokenKind::Word &&
        token(1).kind == TokenKind::Word && is("SET") && is("NOCOUNT", 1)) {
      if (out.ast.function)
        fail("SET NOCOUNT is only supported in T-SQL procedures");
      take();
      take();
      if (token().kind != TokenKind::Word || (!is("ON") && !is("OFF")))
        fail("SET NOCOUNT requires ON or OFF");
      n.kind = eat("ON") ? ProceduralKind::SetNoCountOn : ProceduralKind::SetNoCountOff;
      if (n.kind == ProceduralKind::SetNoCountOff)
        need("OFF");
      eat(";");
    } else if (eat("IF")) {
      n.kind = ProceduralKind::If;
      n.expression = expressionUntil(
          {lexed.dialect == Dialect::SQLServer ? "BEGIN" : "THEN"});
      if (lexed.dialect == Dialect::SQLServer) {
        n.children.push_back(block());
        if (eat("ELSE"))
          n.otherwise.push_back(block());
        eat(";");
      } else {
        need("THEN");
        n.children = list({"ELSE", "ELSEIF", "ELSIF", "END"}, true);
        std::vector<ProceduralAstNode> alternatives;
        while (is("ELSIF") || is("ELSEIF")) {
          if ((mysql(lexed.dialect) && !is("ELSEIF")) ||
              (lexed.dialect == Dialect::Oracle && !is("ELSIF")))
            fail("this dialect uses a different ELSIF/ELSEIF spelling");
          if (alternatives.size() >=
              std::min<std::size_t>(256, lexed.options.limits.nesting))
            fail("conditional alternative budget exceeded",
                 DiagnosticCode::ResourceLimit);
          ProceduralAstNode branch;
          branch.kind = ProceduralKind::If;
          branch.scope = scope;
          branch.origin.begin = take().range.begin;
          branch.expression = expressionUntil({"THEN"});
          need("THEN");
          branch.children = list({"ELSE", "ELSIF", "ELSEIF", "END"}, true);
          alternatives.push_back(std::move(branch));
        }
        if (eat("ELSE"))
          n.otherwise = list({"END"}, true);
        need("END");
        need("IF");
        need(";");
        for (auto it = alternatives.rbegin(); it != alternatives.rend(); ++it) {
          it->otherwise = std::move(n.otherwise);
          it->origin.end = tokens[at - 1].range.end;
          n.otherwise = {node(std::move(*it))};
        }
      }
    } else if (eat("CASE")) {
      caseStatement(n);
    } else if (eat("FOR")) {
      numericFor(n);
    } else if (lexed.dialect == Dialect::PostgreSQL && is("GET") &&
               (is("DIAGNOSTICS", 1) || is("CURRENT", 1) || is("STACKED", 1))) {
      getDiagnostics(n);
    } else if (plLoop && eat("RAISE")) {
      if (!handlerDepth) fail("bare RAISE requires an active exception handler", DiagnosticCode::InvalidQueryShape);
      n.kind = ProceduralKind::Rethrow;
      need(";");
    } else if (lexed.dialect == Dialect::SQLServer && is("THROW")) {
      tsqlThrow(n);
    } else if (eat("REPEAT")) {
      n.kind = ProceduralKind::Repeat;
      beginLoop(n);
      n.children = list({"UNTIL", "END"});
      if (n.children.empty()) fail("REPEAT requires at least one body statement");
      need("UNTIL");
      n.expression = expressionUntil({"END"});
      endLoop(n, "REPEAT");
    } else if (eat("LOOP")) {
      n.kind = ProceduralKind::Loop;
      beginLoop(n);
      n.children = list({"END"});
      if (my && n.children.empty()) fail("MySQL LOOP requires a body statement");
      endLoop(n, "LOOP");
    } else if (is("EXIT") || is("CONTINUE") || is("LEAVE") || is("ITERATE")) {
      loopJump(n, my);
    } else if (eat("WHILE")) {
      n.kind = ProceduralKind::While;
      beginLoop(n);
      n.expression =
          expressionUntil({lexed.dialect == Dialect::SQLServer ? "BEGIN"
                           : mysql(lexed.dialect)              ? "DO"
                                                               : "LOOP"});
      if (lexed.dialect == Dialect::SQLServer) {
        n.children.push_back(block());
        eat(";");
        jumpTargets.pop_back();
      } else {
        need(mysql(lexed.dialect) ? "DO" : "LOOP");
        n.children = list({"END"});
        endLoop(n, my ? "WHILE" : "LOOP");
      }
    } else if (eat("RETURN")) {
      n.kind = ProceduralKind::Return;
      if (my && out.ast.function && (is("SELECT") || is("WITH")))
        fail("a query in RETURN must be parenthesized as a scalar subquery");
      if (!is(";") && !(lexed.dialect == Dialect::SQLServer && is("END")))
        n.expression = statementExpression();
      statementEnd();
      if (out.ast.function && n.expression.begin == n.expression.end)
        fail("scalar function RETURN requires a value");
      if (!out.ast.function && n.expression.begin != n.expression.end)
        fail("procedure RETURN values are dialect-specific",
             DiagnosticCode::UnsupportedSyntax);
    } else if (eat("DO")) {
      n.kind = ProceduralKind::Null;
      need("0");
      need(";");
    } else if (eat("NULL")) {
      n.kind = ProceduralKind::Null;
      need(";");
    } else if (lexed.dialect == Dialect::PostgreSQL && eat("PERFORM")) {
      n.kind = ProceduralKind::Sql;
      n.discardRows = true;
      n.expression = statementExpression();
      if (n.expression.begin == n.expression.end) fail("PERFORM requires a query expression");
      statementEnd();
    } else if (is("SELECT") || is("INSERT") || is("UPDATE") || is("DELETE") ||
               (mysql(lexed.dialect) && is("REPLACE")) ||
               is("WITH") || is("CALL") || is("MERGE") || transaction || prepared) {
      n.kind = ProceduralKind::Sql;
      const auto first = at;
      n.expression = statementExpression();
      const auto end = at;
      if (prepared) preparedCompletion(first,end);
      at = first;
      selectTargets(n, first, end);
      at = end;
      statementEnd();
    } else if (my && is("SET") && token(1).kind == TokenKind::Variable) {
      take();
      const auto variable = take();
      if (variable.value.size() < 2 || variable.value[1] == '@')
        failAt(variable.range, "setting a MySQL system variable requires an explicit session policy",
               DiagnosticCode::UnsupportedSyntax);
      n.kind = ProceduralKind::MySqlSessionAssignment;
      n.sessionVariable = variable.value;
      if (!eat(":=")) need("=");
      n.expression = statementExpression();
      statementEnd();
    } else {
      if (!eat("SET") && my)
        fail("expected a MySQL routine statement; use SET for a variable assignment");
      n.target = scalarTarget("assignment");
      if (!eat(":=") && !eat("="))
        fail("expected assignment operator");
      if (mysql(lexed.dialect) && is("DEFAULT") && is(";", 1))
        fail("DEFAULT is unavailable for MySQL local variable assignments");
      n.kind = ProceduralKind::Assign;
      n.expression = statementExpression();
      statementEnd();
    }
    n.origin.end = tokens[at - 1].range.end;
    return node(std::move(n));
  }
  void parameters() {
    bool parens = eat("(");
    if (!parens && (lexed.dialect != Dialect::SQLServer || is("AS")))
      return;
    if (parens && eat(")"))
      return;
    while (token().kind != TokenKind::End) {
      ProceduralSymbol p;
      p.kind = SymbolKind::Parameter;
      p.origin.begin = token().range.begin;
      if (lexed.dialect != Dialect::Oracle) {
        if (eat("INOUT"))
          p.mode = ParameterMode::InOut;
        else if (eat("OUT"))
          p.mode = ParameterMode::Out;
        else
          eat("IN");
      }
      p.name = name();
      if (lexed.dialect == Dialect::Oracle) {
        if (eat("IN")) {
          if (eat("OUT"))
            p.mode = ParameterMode::InOut;
        } else if (eat("OUT"))
          p.mode = ParameterMode::Out;
      }
      p.type = type(TypeUse::Signature);
      if (eat("OUTPUT") || eat("OUT"))
        p.mode = ParameterMode::InOut;
      if (eat("DEFAULT") || eat(":=") || eat("="))
        p.initializer = expressionUntil({",", ")"});
      p.origin.end = token().range.begin;
      out.ast.parameters.push_back(declare(p));
      if (!eat(","))
        break;
    }
    if (parens)
      need(")");
  }
  bool atCompletionSite() const {
    return mysql(lexed.dialect) && completionSite && token().range.begin == completionSite->offset;
  }
  void completionKeywords(std::initializer_list<const char *> words) {
    if (atCompletionSite()) completionSite->keywords.assign(words.begin(), words.end());
  }
  void attributes() {
    while (token().kind != TokenKind::End) {
      if (atCompletionSite()) {
        completionKeywords({"COMMENT", "LANGUAGE SQL", "DETERMINISTIC", "NOT DETERMINISTIC",
                        "SQL SECURITY", "CONTAINS SQL", "NO SQL", "READS SQL DATA",
                        "MODIFIES SQL DATA", "BEGIN"});
        auto &words = completionSite->keywords;
        if (out.ast.function) words.push_back("RETURN");
        else words.insert(words.end(), {"SELECT", "SET", "CALL", "INSERT", "UPDATE", "DELETE"});
      }
      if (mysql(lexed.dialect) && eat("COMMENT")) {
        if (token().kind != TokenKind::String) fail("routine COMMENT requires a string literal");
        out.ast.comment = take().value;
      } else if (mysql(lexed.dialect) && eat("LANGUAGE")) {
        completionKeywords({"SQL"});
        need("SQL");
        out.ast.explicitLanguageSql = true;
      } else if (eat("DETERMINISTIC")) {
        out.ast.deterministic = true;
        out.ast.volatility = "IMMUTABLE";
      } else if (eat("NOT")) {
        completionKeywords({"DETERMINISTIC"});
        need("DETERMINISTIC");
      } else if (eat("SQL")) {
        completionKeywords({"SECURITY"});
        need("SECURITY");
        completionKeywords({"DEFINER", "INVOKER"});
        if (eat("DEFINER"))
          out.ast.securityDefiner = true;
        else {
          need("INVOKER");
          out.ast.securityDefiner = false;
        }
      } else if (eat("AUTHID")) {
        if (eat("CURRENT_USER"))
          out.ast.securityDefiner = false;
        else
          need("DEFINER");
      } else if (eat("SECURITY")) {
        if (eat("DEFINER"))
          out.ast.securityDefiner = true;
        else {
          need("INVOKER");
          out.ast.securityDefiner = false;
        }
      } else if (eat("VOLATILE"))
        out.ast.volatility = "VOLATILE";
      else if (eat("IMMUTABLE")) {
        out.ast.volatility = "IMMUTABLE";
        out.ast.deterministic = true;
      } else if (eat("STABLE"))
        out.ast.volatility = "STABLE";
      else if (is("READS") || is("MODIFIES")) {
        out.ast.dataAccess = is("READS") ? RoutineDataAccess::ReadsSqlData
                                        : RoutineDataAccess::ModifiesSqlData;
        take();
        completionKeywords({"SQL DATA"});
        need("SQL");
        completionKeywords({"DATA"});
        need("DATA");
      } else if (eat("CONTAINS")) {
        completionKeywords({"SQL"});
        need("SQL");
        out.ast.dataAccess = RoutineDataAccess::ContainsSql;
      } else if (eat("NO")) {
        completionKeywords({"SQL"});
        need("SQL");
        out.ast.dataAccess = RoutineDataAccess::NoSql;
      } else
        break;
    }
  }

public:
  RoutineParser(Dialect d, std::string text, LanguageOptions opts, bool recovery = false,
                RoutineCompletionSite *site = nullptr)
      : lexed(d, std::move(text), std::move(opts), "<routine>"),
        completionRecovery(recovery), completionSite(site) {
    out.ast.dialect = d;
    out.ast.securityDefiner = mysql(d) || d == Dialect::Oracle;
    out.ast.scopes.emplace_back();
    out.ast.symbols.emplace_back();
    out.ast.cursors.emplace_back();
    out.ast.nodes.emplace_back();
    lex(lexed, true, true);
    for (const auto &t : lexed.tokens)
      if (t.kind != TokenKind::Comment)
        tokens.push_back(t);
  }
  RoutineCompilation parse() {
    try {
      if (!lexed.diagnostics.empty()) {
        out.diagnostics = lexed.diagnostics;
        return out;
      }
      if (lexed.dialect == Dialect::SQLServer && eat("ALTER")) {
        out.ast.action = RoutineDefinitionAction::Alter;
      } else {
        need("CREATE");
        if (eat("OR")) {
          if (lexed.dialect == Dialect::SQLServer) {
            need("ALTER");
            out.ast.action = RoutineDefinitionAction::CreateOrAlter;
          } else {
            need("REPLACE");
            out.ast.action = RoutineDefinitionAction::CreateOrReplace;
          }
        }
      }
      if (mysql(lexed.dialect) && eat("DEFINER")) {
        need("=");
        auto &owner=out.ast.definer.emplace();
        if (eat("CURRENT_USER")) {
          owner.currentUser=true;
          if (eat("(")) need(")");
        } else {
          const auto accountPart=[&] {
            if (token().kind!=TokenKind::Word && token().kind!=TokenKind::Identifier &&
                token().kind!=TokenKind::String)
              fail("expected a MySQL routine definer account component");
            auto value=take().value;
            if (value.size()>512) fail("routine definer account is too long",DiagnosticCode::ResourceLimit);
            return value;
          };
          owner.user=accountPart();
          owner.host=eat("@")?accountPart():"%";
        }
      }
      if (eat("FUNCTION"))
        out.ast.function = true;
      else if (!(lexed.dialect == Dialect::SQLServer && eat("PROC")))
        need("PROCEDURE");
      if (lexed.dialect == Dialect::MySQL || lexed.dialect == Dialect::MariaDB) {
        completionKeywords({"IF NOT EXISTS"});
        if (eat("IF")) {
          if (out.ast.action != RoutineDefinitionAction::Create)
            fail("routine IF NOT EXISTS requires plain CREATE");
          completionKeywords({"NOT EXISTS"});
          need("NOT");
          completionKeywords({"EXISTS"});
          need("EXISTS");
          out.ast.ifNotExists = true;
        }
      }
      out.ast.name = qualified();
      enter();
      out.ast.scopes[scope].label = out.ast.name.back();
      parameters();
      if (lexed.dialect == Dialect::PostgreSQL) declareFound();
      if (out.ast.function) {
        need(lexed.dialect == Dialect::Oracle ? "RETURN" : "RETURNS");
        out.ast.result = type(TypeUse::Signature);
      }
      attributes();
      if (lexed.dialect == Dialect::PostgreSQL) {
        bool languageBefore = false;
        if (eat("LANGUAGE")) {
          need("PLPGSQL");
          languageBefore = true;
          attributes();
        }
        need("AS");
        auto body = take();
        if (body.kind != TokenKind::String ||
            lexed.source.text()[body.range.begin] != '$')
          fail("PL/pgSQL routine bodies must use dollar quoting",
               DiagnosticCode::UnsupportedSyntax);
        const auto bodyBegin =
            lexed.source.text().find('$', body.range.begin + 1) + 1;
        Compilation inner(lexed.dialect, body.value, lexed.options,
                          "<routine body>");
        lex(inner, true, true);
        if (!inner.diagnostics.empty()) {
          for (auto d : inner.diagnostics) {
            d.range.begin += bodyBegin;
            d.range.end += bodyBegin;
            out.diagnostics.push_back(d);
          }
          throw Failure{};
        }
        auto outerTokens = std::move(tokens);
        auto outerAt = at;
        tokens.clear();
        for (auto t : inner.tokens)
          if (t.kind != TokenKind::Comment) {
            t.range.begin += bodyBegin;
            t.range.end += bodyBegin;
            tokens.push_back(t);
          }
        at = 0;
        if(typeParser)typeParser->setTypeTokenStream(tokens);
        out.ast.body = block();
        need(";");
        if (token().kind != TokenKind::End)
          fail("trailing tokens in routine body");
        tokens = std::move(outerTokens);
        at = outerAt;
        if(typeParser)typeParser->setTypeTokenStream(tokens);
        if (!languageBefore) {
          need("LANGUAGE");
          need("PLPGSQL");
        }
        attributes();
      } else {
        bool oracleHeader = false;
        if (lexed.dialect == Dialect::Oracle) {
          if (!eat("AS") && !eat("IS"))
            fail("expected AS/IS before PL/SQL declarations");
          oracleHeader = !is("BEGIN") && !is("<");
        } else if (lexed.dialect == Dialect::SQLServer)
          need("AS");
        if ((mysql(lexed.dialect) && !is("BEGIN") && !mysqlBlockLabelAhead()) ||
            (lexed.dialect == Dialect::SQLServer && is("BEGIN") && is("TRY", 1))) {
          const auto bodyScope = enter();
          ProceduralAstNode body;
          body.kind = ProceduralKind::Block;
          body.scope = bodyScope;
          body.origin.begin = token().range.begin;
          body.children.push_back(statement());
          body.origin.end = tokens[at - 1].range.end;
          leave();
          out.ast.body = node(std::move(body));
        } else
          out.ast.body = block(oracleHeader);
      }
      eat(";");
      out.ast.origin = {0, at ? tokens[at - 1].range.end : 0};
      leave();
      out.model.syntax = out.ast;
    } catch (const Failure &) {
    } catch (const CompilationAborted &) {
      out.diagnostics.insert(out.diagnostics.end(), lexed.diagnostics.begin(),
                             lexed.diagnostics.end());
    } catch (const std::exception &e) {
      out.diagnostics.push_back(
          {DiagnosticCode::InvalidQueryShape, token().range, e.what()});
    }
    return out;
  }
};
void lower(RoutineCompilation &out, const std::string &source,
           LanguageOptions opts, const CatalogSnapshot *catalog,
           NameResolutionContext names, std::optional<std::size_t> completionAt = {}) {
  if (!completionAt && !out.ast.origin.end)
    return;
  auto report = [&](Diagnostic diagnostic) {
    recordDiagnostic(out, opts.limits.diagnostics, std::move(diagnostic));
  };
  if (!completionAt && out.ast.function && mysql(out.ast.dialect) &&
      std::none_of(out.ast.nodes.begin(),out.ast.nodes.end(),[](const auto &node) {
        return node.kind==ProceduralKind::Return;
      }))
    report({DiagnosticCode::InvalidQueryShape,out.ast.nodes.at(out.ast.body).origin,
            "MySQL stored functions require a RETURN statement"});
  out.model.syntax = out.ast;
  RoutineDefinition declared;
  declared.database = names.database;
  declared.schema = names.schema;
  if (out.ast.name.size() > 1)
    declared.schema = out.ast.name[out.ast.name.size() - 2];
  if (out.ast.name.size() > 2)
    declared.database = out.ast.name[0];
  declared.name = out.ast.name.empty() ? "" : out.ast.name.back();
  declared.procedure = !out.ast.function;
  declared.kind=out.ast.function?RoutineKind::Scalar:RoutineKind::Procedure;
  declared.minArguments = declared.maxArguments = out.ast.parameters.size();
  for (const auto id : out.ast.parameters) {
    const auto &parameter = out.ast.symbols[id];
    if (parameter.initializer.end > parameter.initializer.begin)
      --declared.minArguments;
  }
  declared.resultType =
      out.ast.result.arrayDimensions
          ? ValueType::Unknown
          : typeCategory(out.ast.result.name, out.ast.dialect);
  if(out.ast.function)declared.sqlType=std::make_shared<const SqlType>(resolveDeclaredType(out.ast.result,out.ast.dialect,TypeUse::Signature).type);
  names.routines.push_back(declared);
  bool stableCatalog = true;
  for (const auto &node : out.ast.nodes)
    if (node.kind == ProceduralKind::Sql &&
        node.expression.end > node.expression.begin) {
      const auto text = asciiUpper(
          source.substr(node.expression.begin,
                        std::min<std::size_t>(16, node.expression.end -
                                                      node.expression.begin)));
      for (const auto *barrier :
           {"CREATE", "ALTER", "DROP", "TRUNCATE", "SET ", "USE ", "EXECUTE"})
        if (text.rfind(barrier, 0) == 0)
          stableCatalog = false;
    }
  if (!stableCatalog)
    catalog = nullptr;
  std::size_t loweredBytes = 0, scopeVisits = 0;
  std::map<std::uint32_t,std::shared_ptr<const SqlType>> symbolTypes;
  std::map<ProceduralSymbolId, std::string> positional;
  if (out.ast.dialect == Dialect::PostgreSQL)
    for (std::size_t i = 0; i < out.ast.parameters.size(); ++i)
      positional.emplace(out.ast.parameters[i], "$" + std::to_string(i + 1));
  auto charge = [&](std::size_t bytes, SourceRange origin) {
    constexpr std::size_t maxBytes = 16 * 1024 * 1024;
    if (bytes > maxBytes - loweredBytes) {
      report(
          {DiagnosticCode::ResourceLimit, origin,
           "routine lowering exceeds cumulative context/model byte budget"});
      return false;
    }
    loweredBytes += bytes;
    return true;
  };
  for (const auto &node : out.ast.nodes) {
    if (completionAt && (node.expression.begin > *completionAt ||
                         node.expression.end <= *completionAt))
      continue;
    ProceduralModelNode lowered;
    lowered.syntax = node;
    if (node.expression.end > node.expression.begin) {
      const bool cursorQuery = node.kind == ProceduralKind::CursorDeclare;
      const bool scalar = node.kind != ProceduralKind::Sql && !cursorQuery;
      const std::string prefix = scalar || node.discardRows ? "SELECT " : "";
      const std::string suffix =
          scalar && out.ast.dialect == Dialect::Oracle ? " FROM SYS.DUAL" : "";
      auto text = source.substr(node.expression.begin,
                                node.expression.end - node.expression.begin);
      if (node.selectInto) {
        // Keep the SQL compiler's offsets aligned with the original body.
        // The procedural clause is parsed and bound separately.
        for (auto offset = node.selectInto->origin.begin; offset < node.selectInto->origin.end; ++offset) {
          auto &ch = text.at(offset - node.expression.begin);
          if (ch != '\n' && ch != '\r') ch = ' ';
        }
      }
      if (!charge(sizeof(NameResolutionContext) +
                      names.routines.size() * sizeof(RoutineDefinition),
                  node.origin))
        return;
      for (const auto &routine : names.routines)
        if (!charge(routine.database.size() + routine.schema.size() +
                        routine.name.size(),
                    node.origin))
          return;
      NameResolutionContext context = names;
      context.locals.clear();
      context.dialect = out.ast.dialect;
      context.environment = !scalar ? SemanticEnvironment::Sql
                            : (node.kind == ProceduralKind::If ||
                               node.kind == ProceduralKind::CaseWhen ||
                               node.kind == ProceduralKind::While ||
                               node.kind == ProceduralKind::Repeat ||
                               node.kind == ProceduralKind::Exit ||
                               node.kind == ProceduralKind::Continue)
                                ? SemanticEnvironment::ProceduralCondition
                                : SemanticEnvironment::ProceduralScalar;
      if (!scalar)
        context.localPolicy = out.ast.dialect == Dialect::Oracle
                                  ? LocalNamePolicy::PreferColumns
                              : out.ast.dialect == Dialect::PostgreSQL
                                  ? LocalNamePolicy::ErrorOnConflict
                                  : LocalNamePolicy::PreferLocals;
      std::set<std::string> visible;
      std::set<std::pair<std::string,std::string>> visibleQualified;
      for (auto scope = node.scope; scope;
           scope = out.ast.scopes[scope].parent) {
        const auto &block = out.ast.scopes[scope];
        for (auto id : block.symbols) {
          if ((++scopeVisits & 127u) == 0 && opts.shouldStop &&
              opts.shouldStop())
            throw CompilationAborted{};
          const auto &symbol = out.ast.symbols[id];
          const auto &label = routineSymbolLabel(out.ast, symbol);
          if (!routineSymbolVisible(symbol, node.expression.begin))
            continue;
          auto spelling = out.ast.dialect == Dialect::SQLServer
                              ? "@" + symbol.name
                              : symbol.name;
          if (!charge(sizeof(ExternalSymbol) * 2 + sizeof(std::string) * 3 +
                          spelling.size() + symbol.name.size() +
                          label.size(),
                      node.origin))
            return;
          auto typed=symbolTypes.find(id);
          if(typed==symbolTypes.end()) {
            const auto resolved=resolveDeclaredType(symbol.type,out.ast.dialect,symbol.kind==SymbolKind::Parameter?TypeUse::Signature:TypeUse::Declaration);
            if(!resolved.error.empty()){report({DiagnosticCode::InvalidQueryShape,symbol.origin,resolved.error});return;}
            typed=symbolTypes.emplace(id,std::make_shared<const SqlType>(resolved.type)).first;
          }
          if (visible.insert(spelling).second)
            context.locals.push_back(
                {{spelling},
                 id,
                 symbol.type.arrayDimensions
                     ? ValueType::Unknown
                     : typeCategory(symbol.type.name, out.ast.dialect),typed->second});
          if (!label.empty() && visibleQualified.emplace(label, symbol.name).second)
            context.locals.push_back(
                {{label, symbol.name},
                 id,
                 symbol.type.arrayDimensions
                     ? ValueType::Unknown
                     : typeCategory(symbol.type.name, out.ast.dialect),typed->second});
          if (const auto position = positional.find(id); position != positional.end()) {
            if (!charge(sizeof(ExternalSymbol) + sizeof(std::string) + position->second.size(), node.origin))
              return;
            context.locals.push_back({{position->second}, id, valueCategory(*typed->second), typed->second});
          }
        }
      }
      if (!scalar && out.ast.dialect == Dialect::SQLServer)
        text += ';';
      auto queryOptions = opts;
      queryOptions.describeSelectArity = cursorQuery;
      auto c = compile(out.ast.dialect, prefix + text + suffix, queryOptions, catalog,
                       context);
      if (!c->ok() && (!completionAt || !c->syntaxValid || c->model.empty())) {
        for (auto d : c->diagnostics) {
          if (scalar && d.code == DiagnosticCode::UnknownColumn &&
              !c->model.roots.empty()) {
            const auto &root = c->model.queries.at(c->model.roots.front() - 1);
            const bool scalarScope =
                root.tables.empty() ||
                (out.ast.dialect == Dialect::Oracle &&
                 root.tables.size() == 1 && !root.tables[0].name.empty() &&
                 asciiUpper(root.tables[0].name.back()) == "DUAL");
            if (scalarScope)
              for (const auto &e : root.expressions)
                if (e.kind == ExprKind::Name &&
                    e.origin.begin == d.range.begin &&
                    e.origin.end == d.range.end)
                  d.message = "undeclared procedural variable: " +
                              std::string(c->source.slice(e.origin));
          }
          d.range.begin =
              node.expression.begin + (d.range.begin >= prefix.size()
                                           ? d.range.begin - prefix.size()
                                           : 0);
          d.range.end =
              node.expression.begin +
              (d.range.end >= prefix.size() ? d.range.end - prefix.size() : 0);
          report(std::move(d));
        }
        out.model.nodes.push_back(std::move(lowered));
        continue;
      }
      if (cursorQuery) {
        if (c->statements.size() != 1 || c->statements.front().kind != StatementKind::Select) {
          report({DiagnosticCode::InvalidQueryShape, node.expression,
                  "DECLARE CURSOR requires one SELECT query"});
          out.model.nodes.push_back(std::move(lowered));
          continue;
        }
        lowered.cursorArity = c->selectArity;
      }
      lowered.code = std::move(c->model);
      if (!completionAt && !scalar && !node.selectInto &&
          (out.ast.dialect == Dialect::PostgreSQL || out.ast.dialect == Dialect::Oracle)) {
        const auto &query = lowered.code.at(lowered.code.roots.front() - 1);
        if (query.kind == StatementKind::Select && (!node.discardRows || query.selectInto || query.target)) {
          const auto start = node.origin.begin;
          const auto keyword = source.find_first_of(" \t\n\r", start);
          report({DiagnosticCode::InvalidQueryShape,
              {start, std::min(node.expression.end, keyword)},
              node.discardRows ? "PERFORM cannot have an INTO destination"
              : out.ast.dialect == Dialect::PostgreSQL
                  ? "PL/pgSQL SELECT requires INTO; use PERFORM to discard its rows"
                  : "PL/SQL SELECT requires an INTO destination"});
        }
      }
      if (node.selectInto) {
        auto &query = lowered.code.queries.at(lowered.code.roots.front() - 1);
        if (query.kind != StatementKind::Select || query.setOperation || query.valuesQuery ||
            query.tableQuery != TableQuerySyntax::None ||
            std::any_of(query.projections.begin(), query.projections.end(), [&](const auto &column) {
              return query.expressions.at(column.expression).kind == ExprKind::Star;
            })) {
          report({DiagnosticCode::UnsupportedSyntax, node.selectInto->origin,
                                    "routine SELECT INTO requires an explicit scalar output list"});
        } else if (query.projections.size() != node.selectInto->targets.size()) {
          report({DiagnosticCode::InvalidQueryShape, node.selectInto->origin,
                                    "SELECT INTO variable count does not match the output columns"});
        } else if (std::none_of(node.selectInto->targets.begin(), node.selectInto->targets.end(),
                                [](auto id) { return id == 0; }))
          query.selectInto = node.selectInto;
      }
      if(!charge(lowered.code.types.accountedBytes(),node.origin))return;
      for (auto &q : lowered.code.queries) {
        if (!charge(sizeof(q) + q.expressions.size() * sizeof(ModelExpression) +
                        q.context.locals.size() * sizeof(ExternalSymbol) +
                        q.context.routines.size() * sizeof(RoutineDefinition),
                    node.origin))
          return;
        for (const auto &local : q.context.locals)
          for (const auto &part : local.name)
            if (!charge(sizeof(part) + part.size(), node.origin))
              return;
        for (const auto &e : q.expressions) {
          if (!charge(e.value.size() + e.operands.size() * sizeof(ExprId)+
                      (e.castType?typeSyntaxBytes(*e.castType):0)+(e.interval?sizeof(IntervalSpec):0),node.origin))
            return;
          for (const auto &part : e.name)
            if (!charge(sizeof(part) + part.size(), node.origin))
              return;
        }
        const auto shiftRange = [&](SourceRange range) {
          const auto absolute = [&](std::size_t offset) {
            return node.expression.begin + std::min(node.expression.end - node.expression.begin,
                offset >= prefix.size() ? offset - prefix.size() : 0);
          };
          return SourceRange{absolute(range.begin), absolute(range.end)};
        };
        q.origin = shiftRange(q.origin);
        for (auto &e : q.expressions) {
          if(e.castType) {
            auto syntax=*e.castType;
            syntax.origin = shiftRange(syntax.origin);
            e.castType=std::make_shared<const BasicTypeSpec<std::vector<std::string>>>(std::move(syntax));
          }
          e.origin = shiftRange(e.origin);
          if (e.kind != ExprKind::Name && e.kind != ExprKind::Parameter)
            continue;
          const bool tsqlLocal = out.ast.dialect == Dialect::SQLServer &&
              e.kind == ExprKind::Parameter && e.value.size() > 1 &&
              e.value.front() == '@' && e.value[1] != '@';
          const bool pgPositional = out.ast.dialect == Dialect::PostgreSQL &&
              e.kind == ExprKind::Parameter && !e.value.empty() && e.value.front() == '$';
          if (out.ast.dialect == Dialect::SQLServer && !tsqlLocal)
            continue;
          if (e.binding || e.localSymbol || !e.coalescedBindings.empty())
            continue;
          if (tsqlLocal || pgPositional ||
              (q.context.environment != SemanticEnvironment::Sql && !e.name.empty()))
            report(
                {DiagnosticCode::UnknownColumn, e.origin,
                 "undeclared procedural variable: " +
                 source.substr(e.origin.begin, e.origin.end - e.origin.begin)});
        }
      }
    }
    out.model.nodes.push_back(std::move(lowered));
  }
}

void checkCursorRuntime(RoutineCompilation &out, const LanguageOptions &options) {
  if (!out.ok() || out.ast.cursors.size() <= 1)
    return;
  using Kind = ProceduralKind;
  auto warn = [&](SourceRange origin, std::string reason) {
    if (out.runtimeDiagnostics.size() < std::max<std::size_t>(1, options.limits.diagnostics))
      out.runtimeDiagnostics.push_back({DiagnosticCode::CursorRuntime, origin, std::move(reason)});
  };
  std::vector<std::optional<std::size_t>> widths(out.ast.cursors.size());
  for (const auto &node : out.model.nodes) {
    if (node.syntax.kind == Kind::CursorDeclare)
      widths.at(node.syntax.cursor->cursor) = node.cursorArity;
    if (node.syntax.kind == Kind::CursorFetch) {
      const auto &use = *node.syntax.cursor;
      if (const auto width = widths.at(use.cursor); width && *width != use.targets.size())
        warn(use.origin, "FETCH expects " + std::to_string(*width) +
             " target variables for this cursor, found " + std::to_string(use.targets.size()));
    }
  }

  // Facts describe execution reaching a node. An absent cursor state is unknown.
  // Drop facts at operations or control-flow joins that could change them.
  enum class State { Closed, Open };
  using States = std::map<ProceduralCursorId, State>;
  const auto &nodes = out.ast.nodes;
  std::size_t budget = 250000;
  const auto charge = [&](std::size_t count = 1) {
    if (options.shouldStop && options.shouldStop())
      throw CompilationAborted{};
    if (count > budget) {
      budget = 0;
      return false;
    }
    budget -= count;
    return true;
  };
  const auto affectsSession = [&](std::uint32_t id) {
    for (const auto &q : out.model.nodes.at(id).code.queries)
      if (q.transaction || q.kind == StatementKind::Call ||
          q.transactionEffect == TransactionEffect::ImplicitCommit ||
          q.transactionEffect == TransactionEffect::Dynamic)
        return true;
    return false;
  };
  // Exception handlers can run implicitly, so exclude any cursor they can change.
  const auto invalidate = [&](auto &&self, std::uint32_t id, States &states) -> bool {
    if (!charge()) return false;
    const auto &node = nodes.at(id);
    if (affectsSession(id)) states.clear();
    if (node.kind == Kind::CursorOpen || node.kind == Kind::CursorClose)
      states.erase(node.cursor->cursor);
    for (auto child : node.children)
      if (!self(self, child, states)) return false;
    for (auto child : node.otherwise)
      if (!self(self, child, states)) return false;
    return true;
  };
  States trackable;
  for (const auto &cursor : out.ast.cursors)
    if (cursor.id) trackable.emplace(cursor.id, State::Closed);
  std::vector<bool> hasContinueHandler(out.ast.scopes.size());
  std::vector<bool> hasExitHandler(out.ast.scopes.size());
  for (std::uint32_t id = 1; id < nodes.size(); ++id) {
    const auto &node = nodes[id];
    if (node.kind != Kind::MySqlHandler) continue;
    if (!invalidate(invalidate, id, trackable))
      return;
    if (node.mysqlHandlerMode == MySqlHandlerMode::Continue)
      hasContinueHandler[node.scope] = true;
    else
      hasExitHandler[node.scope] = true;
  }
  for (const auto &scope : out.ast.scopes)
    if (hasContinueHandler[scope.parent]) hasContinueHandler[scope.id] = true;

  const auto intersect = [](States &left, const States &right) {
    for (auto it = left.begin(); it != left.end();) {
      auto other = right.find(it->first);
      if (other == right.end() || other->second != it->second)
        it = left.erase(it);
      else
        ++it;
    }
  };
  std::vector<std::optional<States>> blockExits(out.ast.scopes.size());
  const auto visit = [&](auto &&self, std::uint32_t id, States &states) -> bool {
    if (!charge()) return false;
    const auto &node = nodes.at(id);
    const auto list = [&](const auto &children, States &facts) {
      for (auto child : children)
        if (!self(self, child, facts)) return false;
      return true;
    };
    if (node.cursor) {
      const auto &use = *node.cursor;
      if (!trackable.count(use.cursor)) return true;
      const auto current = states.find(use.cursor);
      const auto &name = out.ast.cursors.at(use.cursor).name;
      if (node.kind == Kind::CursorDeclare) {
        states[use.cursor] = State::Closed;
      } else if (node.kind == Kind::CursorOpen) {
        if (current != states.end() && current->second == State::Open)
          warn(use.origin, "cursor is already open: " + name);
        else if (hasContinueHandler[node.scope])
          // Control could resume at the next statement after a failed OPEN.
          states.erase(use.cursor);
        else
          states[use.cursor] = State::Open;
      } else {
        if (current != states.end() && current->second == State::Closed)
          warn(use.origin, "cursor is not open: " + name);
        if (node.kind == Kind::CursorClose)
          states[use.cursor] = State::Closed;
      }
      return true;
    }
    switch (node.kind) {
    case Kind::Block: {
      bool next=list(node.children,states);
      if (auto &exit=blockExits.at(node.scope);exit) {
        if (next) intersect(states,*exit);
        else states=std::move(*exit);
        exit.reset();
        next=true;
      }
      for (auto cursor : out.ast.scopes.at(node.scope).cursors) states.erase(cursor);
      // An EXIT handler can bypass any cursor operations left in this block.
      return next && (!hasExitHandler[node.scope] || invalidate(invalidate, id, states));
    }
    case Kind::If:
    case Kind::Case: {
      if (!charge(states.size())) return false;
      auto joined = states;
      bool reachable = list(node.otherwise, joined);
      const auto merge = [&](const auto &branch) {
        if (!charge(states.size())) return;
        auto path = states;
        if (!branch(path)) return;
        if (reachable) intersect(joined, path);
        else joined = std::move(path);
        reachable = true;
      };
      if (node.kind == Kind::If)
        merge([&](States &path) { return list(node.children, path); });
      else
        for (auto child : node.children)
          merge([&](States &path) { return self(self, child, path); });
      states = std::move(joined);
      return reachable && budget != 0;
    }
    case Kind::CaseWhen:
      return list(node.children, states);
    case Kind::Loop: case Kind::While: case Kind::Repeat: {
      for (auto child : node.children)
        if (!invalidate(invalidate, child, states)) return false;
      if (!charge(states.size())) return false;
      auto iteration = states;
      list(node.children, iteration);
      return budget != 0;
    }
    case Kind::MySqlHandler:
      return true;
    case Kind::MySqlSignal: case Kind::MySqlResignal:
      states.clear();
      return true;
    case Kind::Exit:
      if (node.blockTarget) {
        if (!charge(states.size()+1)) return false;
        auto &exit=blockExits.at(node.blockTarget);
        if (exit) intersect(*exit,states);
        else exit=states;
      }
      return false;
    case Kind::Return: case Kind::Continue:
      return false;
    default:
      if (affectsSession(id)) states.clear();
      return true;
    }
  };
  States initial;
  visit(visit, out.ast.body, initial);
}

void checkPreparedRuntime(RoutineCompilation &out,const LanguageOptions &options) {
  using Kind=ProceduralKind;
  if (!out.ok() || (out.ast.dialect!=Dialect::MySQL && out.ast.dialect!=Dialect::MariaDB))
    return;
  const auto rootStatement=[](const ProceduralModelNode &node)->const ModelStatement * {
    return node.code.roots.empty()?nullptr:&node.code.at(node.code.roots.front()-1);
  };
  const auto containsPrepare=std::any_of(out.model.nodes.begin(),out.model.nodes.end(),[&](const auto &node) {
    const auto *query=rootStatement(node);
    return query && bool(query->prepared);
  });
  if (!containsPrepare) return;
  ScriptContext context(out.ast.dialect,nullptr);
  // Session handles can survive CALL, but the details are unknowable if
  // branches, loops or handlers reach an instruction by different paths.
  const auto visit=[&](auto &&self,std::uint32_t id)->bool {
    if (options.shouldStop && options.shouldStop()) throw CompilationAborted{};
    const auto &node=out.model.nodes.at(id);
    const auto &syntax=node.syntax;
    if (syntax.kind==Kind::Block) {
      const bool handlers=std::any_of(syntax.children.begin(),syntax.children.end(),[&](auto child) {
        return out.model.nodes.at(child).syntax.kind==Kind::MySqlHandler;
      });
      if (handlers) context.invalidate();
      else
        for (auto child:syntax.children)
          if (!self(self,child)) return false;
    } else if (syntax.kind==Kind::Sql) {
      const auto *query=rootStatement(node);
      if (!query) return true;
      if (query->prepared) {
        if (auto diagnostic=context.observe(node.code,query->id)) {
          if (diagnostic->code==DiagnosticCode::InvalidQueryShape &&
              out.runtimeDiagnostics.size()<std::max<std::size_t>(1,options.limits.diagnostics)) {
            diagnostic->code=DiagnosticCode::PreparedRuntime;
            diagnostic->range=syntax.expression;
            out.runtimeDiagnostics.push_back(std::move(*diagnostic));
          }
          context.invalidate();
        }
      } else if (query->kind==StatementKind::Call) context.invalidate();
    } else if (syntax.kind==Kind::Return || syntax.kind==Kind::Exit || syntax.kind==Kind::Continue) {
      return false;
    } else if (syntax.kind==Kind::If || syntax.kind==Kind::Case || syntax.kind==Kind::Loop ||
               syntax.kind==Kind::While || syntax.kind==Kind::Repeat ||
               syntax.kind==Kind::MySqlSignal || syntax.kind==Kind::MySqlResignal) {
      context.invalidate();
    }
    return true;
  };
  visit(visit,out.ast.body);
}
} // namespace
bool isRoutineStart(Dialect dialect, const std::vector<Token> &tokens, std::size_t index) {
  const auto keyword = [&](std::size_t i, std::string_view word) {
    return i < tokens.size() && tokens[i].kind == TokenKind::Word &&
           asciiUpper(tokens[i].value) == word;
  };
  const bool tsql = dialect == Dialect::SQLServer;
  if (keyword(index, "CREATE")) {
    ++index;
    if (keyword(index, "OR")) {
      if (!keyword(index + 1, tsql ? "ALTER" : "REPLACE"))
        return false;
      index += 2;
    }
  } else if (tsql && keyword(index, "ALTER")) {
    ++index;
  } else {
    return false;
  }
  if (mysql(dialect) && mysqlLoadableFunctionAhead(tokens,index)) return false;
  if (mysql(dialect) && keyword(index,"DEFINER")) {
    const auto symbol=[&](std::size_t i,std::string_view value) {
      return i<tokens.size() && tokens[i].kind==TokenKind::Symbol && tokens[i].value==value;
    };
    const auto accountPart=[&](std::size_t i) {
      return i<tokens.size() && (tokens[i].kind==TokenKind::Word ||
          tokens[i].kind==TokenKind::Identifier || tokens[i].kind==TokenKind::String);
    };
    if (!symbol(++index,"=") || !accountPart(++index)) return false;
    if (keyword(index,"CURRENT_USER")) {
      ++index;
      if (symbol(index,"(") && symbol(index+1,")")) index+=2;
    } else {
      ++index;
      if (symbol(index,"@")) {
        if (!accountPart(++index)) return false;
        ++index;
      }
    }
  }
  return keyword(index, "FUNCTION") || keyword(index, "PROCEDURE") ||
         (tsql && keyword(index, "PROC"));
}
RoutineCompilation parseRoutineForCompletion(Dialect dialect, std::string source,
                                             LanguageOptions options, RoutineCompletionSite *site) {
  if (source.size() > options.limits.sourceBytes) {
    RoutineCompilation result;
    result.diagnostics.push_back({DiagnosticCode::ResourceLimit, {}, "routine source limit exceeded"});
    return result;
  }
  return RoutineParser(dialect, std::move(source), std::move(options), true, site).parse();
}
RoutineCompilation compileRoutineForCompletion(Dialect dialect, std::string source, LanguageOptions options,
                                              const CatalogSnapshot *catalog, NameResolutionContext names,
                                              std::size_t cursor, RoutineCompletionSite *site) {
  auto result = parseRoutineForCompletion(dialect, source, options, site);
  lower(result, source, options, catalog, std::move(names), cursor);
  return result;
}
RoutineCompilation compileRoutinePrefix(Dialect d, std::string source,
                                        LanguageOptions opts,
                                        const CatalogSnapshot *catalog,
                                        NameResolutionContext names) {
  RoutineCompilation out;
  if (source.size() > opts.limits.sourceBytes) {
    out.diagnostics.push_back(
        {DiagnosticCode::ResourceLimit, {}, "routine source limit exceeded"});
    return out;
  }
  if (d == Dialect::TiDB) {
    out.diagnostics.push_back({DiagnosticCode::UnsupportedSyntax,
        {0, source.size()}, "TiDB does not support stored procedures or stored functions"});
    return out;
  }
  try {
    out = RoutineParser(d, source, opts).parse();
    lower(out, source, opts, catalog, names);
    checkCursorRuntime(out, opts);
    checkPreparedRuntime(out,opts);
    std::stable_sort(out.runtimeDiagnostics.begin(),out.runtimeDiagnostics.end(),
        [](const auto &a,const auto &b) { return a.range.begin<b.range.begin; });
    std::stable_sort(out.diagnostics.begin(), out.diagnostics.end(),
        [](const auto &a, const auto &b) { return a.range.begin < b.range.begin; });
  } catch (const CompilationAborted &) {
    out.diagnostics.push_back(
        {DiagnosticCode::Stopped, {}, "routine compilation stopped"});
  } catch (const std::exception &e) {
    out.diagnostics.push_back(
        {DiagnosticCode::InvalidQueryShape, {}, e.what()});
  }
  return out;
}
RoutineCompilation compileRoutine(Dialect d, std::string source,
                                  LanguageOptions opts,
                                  const CatalogSnapshot *catalog,
                                  NameResolutionContext names) {
  if (mysql(d)) {
    RoutineCompilation wrapped;
    auto normalized = normalizeConversionScript(d, source, opts, wrapped.diagnostics);
    if (!wrapped.diagnostics.empty())
      return wrapped;
    source = std::move(normalized);
  }
  auto out = compileRoutinePrefix(d, source, opts, catalog, names);
  if (out.ok()) {
    Compilation tail(d, source.substr(out.ast.origin.end), opts, "<trailing>");
    lex(tail, true, true);
    for (auto &t : tail.tokens)
      if (t.kind != TokenKind::End && t.kind != TokenKind::Comment &&
          t.value != ";") {
        out.diagnostics.push_back({DiagnosticCode::ExpectedToken,
                                   {out.ast.origin.end + t.range.begin,
                                    out.ast.origin.end + t.range.end},
                                   "unexpected statement after routine"});
        break;
      }
  }
  return out;
}
std::string dumpRoutineModel(const RoutineCompilation &c, std::size_t maxBytes,
                             std::function<bool()> shouldStop) {
  BoundedOutputBuffer buffer(maxBytes, shouldStop);
  std::ostream out(&buffer);
  out.exceptions(std::ios::badbit | std::ios::failbit);
  auto quoted = [&](const std::string &text) {
    out << '"';
    for (unsigned char ch : text) {
      if (ch == '"' || ch == '\\')
        out << '\\' << char(ch);
      else if (ch < 32) {
        static const char hex[] = "0123456789abcdef";
        out << "\\u00" << hex[ch >> 4] << hex[ch & 15];
      } else
        out << char(ch);
    }
    out << '"';
  };
  const char *action = "CREATE";
  switch (c.ast.action) {
  case RoutineDefinitionAction::Create: break;
  case RoutineDefinitionAction::CreateOrReplace: action = "CREATE OR REPLACE"; break;
  case RoutineDefinitionAction::CreateOrAlter: action = "CREATE OR ALTER"; break;
  case RoutineDefinitionAction::Alter: action = "ALTER"; break;
  }
  out << "{\"action\":\"" << action << "\",\"kind\":\"" << (c.ast.function ? "function" : "procedure")
      << "\",\"sourceDialect\":";
  quoted(std::string(dialectName(c.ast.dialect)));
  if (c.ast.explicitLanguageSql) out << ",\"explicitLanguageSql\":true";
  if (c.ast.ifNotExists) out << ",\"ifNotExists\":true";
  if (c.ast.comment) {
    out << ",\"comment\":";
    quoted(*c.ast.comment);
  }
  if (c.ast.definer) {
    out << ",\"definer\":{\"currentUser\":" << (c.ast.definer->currentUser?"true":"false")
        << ",\"user\":";
    quoted(c.ast.definer->user);
    out << ",\"host\":";
    quoted(c.ast.definer->host);
    out << '}';
  }
  out << ",\"name\":[";
  for (std::size_t i = 0; i < c.ast.name.size(); ++i) {
    if (i)
      out << ',';
    quoted(c.ast.name[i]);
  }
  out << "],\"body\":" << c.ast.body << ",\"origin\":[" << c.ast.origin.begin
      << ',' << c.ast.origin.end << "],\"parameters\":[";
  for (std::size_t i = 0; i < c.ast.parameters.size(); ++i) {
    if (i)
      out << ',';
    out << c.ast.parameters[i];
  }
  out << "],\"scopes\":[";
  for (std::size_t i = 1; i < c.ast.scopes.size(); ++i) {
    if (i > 1)
      out << ',';
    const auto &s = c.ast.scopes[i];
    out << "{\"id\":" << s.id << ",\"parent\":" << s.parent << ",\"origin\":["
        << s.origin.begin << ',' << s.origin.end << "],\"label\":";
    quoted(s.label);
    out << ",\"symbols\":[";
    for (std::size_t j = 0; j < s.symbols.size(); ++j) {
      if (j)
        out << ',';
      out << s.symbols[j];
    }
    out << "]}";
  }
  out << "],\"symbols\":[";
  for (std::size_t i = 1; i < c.ast.symbols.size(); ++i) {
    if (i > 1)
      out << ',';
    const auto &s = c.ast.symbols[i];
    out << "{\"id\":" << s.id << ",\"scope\":" << s.scope << ",\"name\":\"";
    for (char ch : s.name) {
      if (ch == '"' || ch == '\\')
        out << '\\';
      if (static_cast<unsigned char>(ch) < 32) {
        static const char hex[] = "0123456789abcdef";
        out << "\\u00" << hex[(static_cast<unsigned char>(ch) >> 4) & 15]
            << hex[ch & 15];
      } else
        out << ch;
    }
    out << "\",\"kind\":\""
        << (s.kind == SymbolKind::Parameter ? "parameter" :
            s.kind == SymbolKind::Implicit ? "implicit" :
            s.kind == SymbolKind::LoopIndex ? "loopIndex" :
            s.kind == SymbolKind::ExceptionStatus ? "exceptionStatus" : "variable")
        << "\",\"origin\":[" << s.origin.begin << ',' << s.origin.end
        << "],\"type\":[";
    for (std::size_t part = 0; part < s.type.name.size(); ++part) {
      if (part)
        out << ',';
      quoted(s.type.name[part]);
    }
    out << "],\"mode\":" << int(s.mode) << '}';
  }
  out << "],\"cursors\":[";
  for (std::size_t i = 1; i < c.ast.cursors.size(); ++i) {
    if (i > 1) out << ',';
    const auto &cursor = c.ast.cursors[i];
    out << "{\"id\":" << cursor.id << ",\"scope\":" << cursor.scope
        << ",\"origin\":[" << cursor.origin.begin << ',' << cursor.origin.end << "],\"name\":";
    quoted(cursor.name);
    out << '}';
  }
  out << ']';
  const auto runtimeChecks=[&](const char *field,DiagnosticCode code) {
    out << ",\"" << field << "\":[";
    bool comma=false;
    for (const auto &check:c.runtimeDiagnostics) {
      if (check.code!=code) continue;
      if (comma) out << ',';
      comma=true;
      out << "{\"origin\":[" << check.range.begin << ',' << check.range.end << "],\"message\":";
      quoted(check.message);
      out << '}';
    }
    out << ']';
  };
  runtimeChecks("cursorRuntimeChecks",DiagnosticCode::CursorRuntime);
  runtimeChecks("preparedRuntimeChecks",DiagnosticCode::PreparedRuntime);
  out << ",\"nodes\":[";
  for (std::size_t i = 1; i < c.model.nodes.size(); ++i) {
    if (i > 1)
      out << ',';
    const auto &n = c.model.nodes[i];
    out << "{\"id\":" << i << ",\"kind\":" << int(n.syntax.kind)
        << ",\"scope\":" << n.syntax.scope << ",\"origin\":["
        << n.syntax.origin.begin << ',' << n.syntax.origin.end << "]"
        << ",\"targetSymbol\":" << n.syntax.target << ",\"loop\":" << n.syntax.loop << ",\"loopLabel\":";
    quoted(n.syntax.loopLabel);
    if (n.syntax.blockTarget) out << ",\"blockTarget\":" << n.syntax.blockTarget;
    if (n.syntax.discardRows) out << ",\"discardRows\":true";
    if (!n.syntax.handlers.empty()) {
      out << ",\"handlers\":[";
      for (std::size_t j = 0; j < n.syntax.handlers.size(); ++j) {
        if (j) out << ',';
        out << n.syntax.handlers[j];
      }
      out << ']';
    }
    if (n.syntax.mysqlHandlerMode)
      out << ",\"mysqlHandlerMode\":\""
          << (*n.syntax.mysqlHandlerMode == MySqlHandlerMode::Continue ? "CONTINUE" : "EXIT") << '\"';
    if (n.syntax.mysqlSignalField) {
      out << ",\"mysqlSignalField\":";
      const auto *field=mysqlDiagnosticInfo(*n.syntax.mysqlSignalField);
      quoted(field?field->spelling:"");
    }
    if (n.syntax.mysqlDiagnostics) {
      const auto &read=*n.syntax.mysqlDiagnostics;
      out << ",\"mysqlDiagnostics\":{\"stacked\":" << (read.stacked?"true":"false") << ",\"targets\":[";
      for (std::size_t i=0;i<read.targets.size();++i) {
        if (i) out << ',';
        const auto &target=read.targets[i];
        const auto *field=mysqlDiagnosticInfo(target.field);
        out << "{\"field\":";
        quoted(field?field->spelling:"");
        if (const auto symbol=std::get_if<ProceduralSymbolId>(&target.destination))
          out << ",\"symbol\":" << *symbol;
        else {
          out << ",\"userVariable\":";
          quoted(std::get<std::string>(target.destination));
        }
        out << '}';
      }
      out << "]}";
    }
    if (!n.syntax.conditionName.empty()) {
      out << ",\"conditionName\":";
      quoted(n.syntax.conditionName);
    }
    if (!n.syntax.sessionVariable.empty()) {
      out << ",\"sessionVariable\":";
      quoted(n.syntax.sessionVariable);
    }
    if (!n.syntax.conditions.empty()) {
      out << ",\"exceptionConditions\":[";
      for (std::size_t j = 0; j < n.syntax.conditions.size(); ++j) {
        if (j) out << ',';
        const auto &condition = n.syntax.conditions[j];
        out << "{\"kind\":" << int(condition.kind) << ",\"errorCode\":" << condition.errorCode
            << ",\"sqlState\":";
        quoted(condition.sqlState);
        out << ",\"name\":[";
        for (std::size_t k = 0; k < condition.name.size(); ++k) {
          if (k) out << ',';
          quoted(condition.name[k]);
        }
        out << "]}";
      }
      out << ']';
    }
    if (!n.syntax.rangeBounds.empty()) {
      out << ",\"reverseFor\":" << (n.syntax.reverseFor ? "true" : "false") << ",\"rangeBounds\":[";
      for (std::size_t j = 0; j < n.syntax.rangeBounds.size(); ++j) {
        if (j) out << ',';
        out << n.syntax.rangeBounds[j];
      }
      out << ']';
    }
    if (n.syntax.selectInto) {
      const auto &into = *n.syntax.selectInto;
      out << ",\"selectInto\":{\"mode\":" << int(into.mode) << ",\"origin\":["
          << into.origin.begin << ',' << into.origin.end << "],\"targets\":[";
      for (std::size_t j = 0; j < into.targets.size(); ++j) {
        if (j) out << ',';
        out << into.targets[j];
      }
      out << "]}";
    }
    if (n.syntax.cursor) {
      const auto &use = *n.syntax.cursor;
      const char *operation = n.syntax.kind == ProceduralKind::CursorDeclare ? "declare"
                            : n.syntax.kind == ProceduralKind::CursorOpen ? "open"
                            : n.syntax.kind == ProceduralKind::CursorFetch ? "fetch" : "close";
      out << ",\"cursor\":{\"id\":" << use.cursor << ",\"operation\":\"" << operation
          << "\",\"origin\":[" << use.origin.begin << ',' << use.origin.end << "],\"targets\":[";
      for (std::size_t j = 0; j < use.targets.size(); ++j) {
        if (j) out << ',';
        out << use.targets[j];
      }
      out << ']';
      if (n.cursorArity) out << ",\"columnCount\":" << *n.cursorArity;
      out << '}';
    }
    if (n.syntax.pgDiagnostics) {
      const auto &read = *n.syntax.pgDiagnostics;
      out << ",\"pgDiagnostics\":{\"area\":\""
          << (read.area == PgDiagnosticArea::Stacked ? "stacked" : "current") << "\",\"targets\":[";
      for (std::size_t j = 0; j < read.targets.size(); ++j) {
        if (j) out << ',';
        const auto &item = read.targets[j];
        out << "{\"symbol\":" << item.symbol << ",\"field\":" << int(item.field)
            << ",\"origin\":[" << item.origin.begin << ',' << item.origin.end << "]}";
      }
      out << "]}";
    }
    if (n.syntax.throwArguments) {
      out << ",\"throwArguments\":[";
      const auto &args = *n.syntax.throwArguments;
      for (std::size_t j = 0; j < args.size(); ++j) {
        if (j) out << ',';
        const auto &arg = args[j];
        out << "{\"origin\":[" << arg.origin.begin << ',' << arg.origin.end << ']';
        if (const auto symbol = std::get_if<ProceduralSymbolId>(&arg.value))
          out << ",\"symbol\":" << *symbol;
        else if (const auto integer = std::get_if<std::int64_t>(&arg.value))
          out << ",\"integer\":" << *integer;
        else {
          out << ",\"unicode\":" << (arg.unicode ? "true" : "false") << ",\"message\":";
          quoted(std::get<std::string>(arg.value));
        }
        out << '}';
      }
      out << ']';
    }
    out << ",\"children\":[";
    for (std::size_t j = 0; j < n.syntax.children.size(); ++j) {
      if (j)
        out << ',';
      out << n.syntax.children[j];
    }
    out << "],\"otherwise\":[";
    for (std::size_t j = 0; j < n.syntax.otherwise.size(); ++j) {
      if (j)
        out << ',';
      out << n.syntax.otherwise[j];
    }
    out << "],\"hasElse\":" << (n.syntax.hasElse ? "true" : "false")
        << ",\"caseOperands\":[";
    for (std::size_t j = 0; j < n.syntax.caseOperands.size(); ++j) {
      if (j)
        out << ',';
      out << n.syntax.caseOperands[j];
    }
    out << "],\"boundSymbols\":[";
    bool comma = false;
    for (const auto &q : n.code.queries)
      for (const auto &e : q.expressions)
        if (e.localSymbol) {
          if (comma)
            out << ',';
          comma = true;
          out << e.localSymbol;
        }
    out << "],\"code\":";
    if (n.code.empty())
      out << "null";
    else {
      LanguageOptions options;
      options.shouldStop = shouldStop;
      Compilation embedded(c.ast.dialect, "", options, "<routine report>");
      embedded.syntaxValid = true;
      embedded.model = n.code;
      out << dumpModel(embedded, buffer.remaining());
    }
    out << '}';
  }
  out << "]}";
  return std::move(buffer.value);
}
} // namespace navidog::sql::compiler
