#include "conversion_internal.hpp"
#include <algorithm>
#include <map>
#include <set>
#include <stdexcept>

namespace navidog::sql::compiler {
namespace {
struct EmitFailure {
  Diagnostic diagnostic;
};
class RoutineEmitter {
  RoutineModel model;
  Dialect target;
  ConversionOptions options;
  GenerationOptions names;
  std::string output;
  int indent = 0;
  std::size_t emissionDepth = 0, emissionVisits = 0;
  std::set<std::uint32_t> emitting;
  std::set<std::uint32_t> labeledLoops, repeatLoops;
  std::map<ProceduralScopeId, std::uint32_t> numericLoopScopes;
  const RoutineAst &ast() const { return model.syntax; }
  bool mysql() const {
    return isMySqlDialect(target);
  }
  bool pg() const { return target == Dialect::PostgreSQL; }
  bool oracle() const { return target == Dialect::Oracle; }
  bool mssql() const { return target == Dialect::SQLServer; }
  [[noreturn]] void fail(SourceRange range, const std::string &message) {
    throw EmitFailure{{DiagnosticCode::UnsupportedSyntax, range, message}};
  }
  void append(const std::string &s) {
    if ((options.language.shouldStop && options.language.shouldStop()) ||
        (options.generation.shouldStop && options.generation.shouldStop()))
      throw EmitFailure{{DiagnosticCode::Stopped, ast().origin,
                         "routine generation cancelled"}};
    if (s.size() > options.generation.maxBytes - output.size())
      fail(ast().origin, "routine output limit exceeded");
    output += s;
  }
  void line(const std::string &s) {
    append(std::string(std::size_t(indent) * 2, ' ') + s + "\n");
  }
  std::string quote(const std::string &s) const {
    const char open = mysql()   ? '`'
                      : mssql() ? '['
                                : '"',
               close = mssql() ? ']' : open;
    std::string n(1, open);
    for (char c : s) {
      if (c == close)
        n += c;
      n += c;
    }
    return n + close;
  }
  std::string commentLiteral(const std::string &value) {
    std::string literal = "'";
    for (char ch : value) {
      if (ch == '\0') {
        if (options.language.mysqlNoBackslashEscapes)
          fail(ast().origin, "routine COMMENT with NUL requires MySQL string escapes");
        literal += "\\0";
        continue;
      }
      if (ch == '\'' || (ch == '\\' && !options.language.mysqlNoBackslashEscapes))
        literal += ch;
      literal += ch;
    }
    return literal + "'";
  }
  std::string loopLabel(std::uint32_t loop) const {
    return quote("nvd_loop_" + std::to_string(loop));
  }
  std::string loopPrefix(std::uint32_t loop) const {
    if (mssql() || !labeledLoops.count(loop)) return {};
    const auto label = loopLabel(loop);
    return mysql() ? label + ": " : "<<" + label + ">> ";
  }
  std::string qualified(const std::vector<std::string> &n) const {
    std::string result;
    for (const auto &p : n) {
      if (!result.empty())
        result += '.';
      result += quote(p);
    }
    return result;
  }
  std::string variable(ProceduralSymbolId id, bool qualifiedName = true) const {
    const auto &v = names.localNames.at(id);
    if (mssql())
      return "@" + v.name;
    return qualifiedName && !v.scope.empty()
               ? quote(v.scope) + "." + quote(v.name)
               : quote(v.name);
  }
  std::string type(const BasicTypeSpec<std::vector<std::string>> &t) {
    auto result = generateTypeSql(t, target);
    if (!result.ok())
      throw EmitFailure{result.diagnostics.front()};
    return result.sql;
  }
  std::string scalar(const ProceduralModelNode &node) {
    if (node.code.roots.empty())
      return {};
    const auto &q = node.code.queries.at(node.code.roots.front() - 1);
    if (q.projections.size() != 1)
      fail(node.syntax.origin, "expected scalar procedural expression");
    if (oracle())
      for (const auto &e : q.expressions)
        if (e.kind == ExprKind::Subquery)
          fail(e.origin, "Oracle PL/SQL scalar subqueries require explicit "
                         "SELECT INTO/cardinality handling");
    auto expression = generateExpression(
        node.code, q.id, q.projections.front().expression, target, names);
    if (!expression.ok())
      throw EmitFailure{expression.diagnostics.front()};
    return expression.sql;
  }
  bool portableCaseScalar(const ProceduralModelNode &node) const {
    if (node.code.roots.empty())
      return false;
    const auto &q = node.code.queries.at(node.code.roots.front() - 1);
    const auto &expression = q.expressions.at(q.projections.at(0).expression);
    const auto &type = node.code.types.get(expression.sqlType);
    return expression.kind == ExprKind::Null ||
           exactSignedIntegerLiteral(q, q.projections.at(0).expression).has_value() ||
           (type.family == TypeFamily::Integer && !type.arrayDimensions &&
            !type.sqliteAffinity && type.opaqueName.empty() && type.bits &&
            *type.bits <= 64 && type.unsignedInteger == false);
  }
  void validateCase(const ProceduralModelNode &node) {
    const auto &syntax = node.syntax;
    if (mssql())
      fail(syntax.origin, "T-SQL CASE statements require a control-flow rewrite");
    if (target == ast().dialect)
      return;
    if (!syntax.hasElse)
      fail(syntax.origin, "unmatched CASE raises a dialect-specific exception; supply an explicit ELSE for conversion");
    if (node.code.empty())
      return;
    if (!portableCaseScalar(node))
      fail(syntax.origin, "simple CASE conversion requires a signed integer or NULL selector with portable equality");
    for (auto branchId : syntax.children) {
      const auto &branch = model.nodes.at(branchId).syntax;
      if (branch.caseOperands.size() > 1 && !pg())
        fail(branch.origin, "multiple WHEN values require preserving evaluation order in the target");
      for (auto value : branch.caseOperands)
        if (!portableCaseScalar(model.nodes.at(value)))
          fail(model.nodes.at(value).syntax.origin, "simple CASE conversion requires signed integer or NULL WHEN values");
    }
  }
  void lowerSelectInto(ProceduralModelNode &node) {
    if (!node.syntax.selectInto || target == ast().dialect) return;
    const bool sourceMySql = isMySqlDialect(ast().dialect);
    if (sourceMySql && mysql()) return;
    auto &q = node.code.queries.at(node.code.roots.at(0) - 1);
    if (!(sourceMySql && pg()) && !(ast().dialect == Dialect::PostgreSQL && mysql()))
      fail(node.syntax.origin, "SELECT INTO target requires an explicit assignment and row-count policy");
    if (!q.tables.empty() || !q.ctes.empty() || q.where || q.having || !q.groupBy.empty() ||
        !q.orderBy.empty() || q.limit || q.offset)
      fail(node.syntax.origin, "SELECT INTO conversion needs an exactly-one-row proof; empty and multiple rows behave differently");
    for (std::size_t i = 0; i < q.projections.size(); ++i) {
      const auto id = q.projections[i].expression;
      const auto &value = q.expressions.at(id);
      const auto &symbol = ast().symbols.at(q.selectInto->targets.at(i));
      const auto destination = resolveDeclaredType(symbol.type, target, TypeUse::Declaration).type;
      if (destination.family != TypeFamily::Integer || !destination.bits || *destination.bits > 64 ||
          destination.unsignedInteger != false || destination.arrayDimensions)
        fail(symbol.origin, "SELECT INTO conversion requires an exact signed integer destination");
      if (value.kind == ExprKind::Null) continue;
      if (const auto number = exactSignedIntegerLiteral(q, id)) {
        const auto bits = *destination.bits;
        if (bits == 64 || (*number >= -(std::int64_t{1} << (bits - 1)) &&
                           *number < (std::int64_t{1} << (bits - 1)))) continue;
      }
      const auto &type = node.code.types.get(value.sqlType);
      if (value.localSymbol && type.family == TypeFamily::Integer && type.bits &&
          *type.bits <= *destination.bits && type.unsignedInteger == false && !type.arrayDimensions) continue;
      fail(value.origin, "SELECT INTO value needs a lossless source and target assignment type");
    }
    q.selectInto->mode = pg() ? SelectIntoMode::ExactlyOne : SelectIntoMode::SingleRowOrUnchanged;
  }
  void validateAndLower() {
    if (target == Dialect::TiDB)
      fail(ast().origin, "TiDB does not support stored procedures or stored functions");
    if (target == Dialect::SQLite)
      fail(ast().origin,
           "SQLite has no stored SQL function/procedure language equivalent");
    if (ast().name.empty())
      fail(ast().origin, "routine name is missing");
    if (target != ast().dialect) {
      if (ast().cursors.size() > 1)
        fail(ast().cursors[1].origin,
             "cursor lifetime, FETCH and NOT FOUND require a target dialect mapping");
      if (ast().definer)
        fail(ast().origin,"explicit routine DEFINER requires a target principal mapping");
      if (ast().comment)
        fail(ast().origin,"routine COMMENT requires a target metadata mapping");
      if (ast().ifNotExists)
        fail(ast().origin,"routine IF NOT EXISTS requires a target deployment policy");
      for (const auto &node : ast().nodes) {
        if (node.kind == ProceduralKind::ExceptionHandler || node.kind == ProceduralKind::TsqlTryCatch ||
            node.kind == ProceduralKind::TsqlThrow || node.kind == ProceduralKind::MySqlHandler ||
            node.kind == ProceduralKind::MySqlCondition || node.kind == ProceduralKind::MySqlSignal ||
            node.kind == ProceduralKind::MySqlResignal || node.kind == ProceduralKind::MySqlGetDiagnostics)
          fail(node.origin, "exception handlers require a target error and rollback policy");
        else if (node.kind == ProceduralKind::MySqlSessionAssignment)
          fail(node.origin, "MySQL session variable assignment needs an explicit target session policy");
      }
    }
    if (ast().volatility == "STABLE" && target != ast().dialect)
      fail(ast().origin, "STABLE snapshot/volatility semantics need an "
                         "explicit target policy");
    if (mssql() && ast().securityDefiner)
      fail(ast().origin, "SQL Server EXECUTE AS requires an explicit target "
                         "principal mapping");
    if(ast().dialect==Dialect::PostgreSQL && target!=ast().dialect) {
      auto signature=[&](const auto &type) {
        if(!type.arguments.empty())fail(type.origin,"PostgreSQL routine signature type modifiers are ignored; target constraints could change argument/result semantics");
      };
      if(ast().function)signature(model.syntax.result);
      for(const auto &symbol:model.syntax.symbols)if(symbol.kind==SymbolKind::Parameter)signature(symbol.type);
    }
    if (ast().function)
      lowerTypeTarget(model.syntax.result, ast().dialect, target, errors);
    for (std::size_t i = 1; i < model.syntax.symbols.size(); ++i) {
      auto &symbol = model.syntax.symbols[i];
      if (symbol.kind == SymbolKind::Implicit || symbol.kind == SymbolKind::LoopIndex ||
          symbol.kind == SymbolKind::ExceptionStatus) continue;
      lowerTypeTarget(symbol.type, ast().dialect, target, errors);
      if (symbol.kind == SymbolKind::Parameter &&
          symbol.initializer.end > symbol.initializer.begin)
        fail(symbol.origin, "parameter default evaluation and calling "
                            "signatures require explicit mapping");
      if (target != ast().dialect && symbol.mode != ParameterMode::In)
        fail(symbol.origin, "OUT/INOUT calling conventions require explicit "
                            "caller/signature conversion");
    }
    if (!errors.empty())
      throw EmitFailure{errors.front()};
    std::map<std::uint32_t,std::size_t> jumps;
    for (const auto &node : model.nodes) {
      const auto &syntax = node.syntax;
      if (syntax.blockTarget && target!=ast().dialect)
        fail(syntax.origin,"LEAVE targeting a block needs a target control-flow mapping");
      if ((syntax.kind==ProceduralKind::Exit || syntax.kind==ProceduralKind::Continue) && syntax.loop)
        ++jumps[syntax.loop];
      if (syntax.loop && !syntax.loopLabel.empty()) labeledLoops.insert(syntax.loop);
      if (syntax.kind == ProceduralKind::NumericFor) {
        if (target != ast().dialect)
          fail(syntax.origin, "numeric FOR needs a target range, step and loop-index mapping");
        labeledLoops.insert(syntax.loop);
        numericLoopScopes.emplace(syntax.scope, syntax.loop);
      }
      if (syntax.kind == ProceduralKind::GetDiagnostics && target != ast().dialect)
        fail(syntax.origin, "GET DIAGNOSTICS needs a target row-count and status policy");
    }
    if (mysql()) {
      for (const auto &[loop, count] : jumps)
        if (count) labeledLoops.insert(loop);
      for (const auto &node : model.nodes) {
        const auto &loop = node.syntax;
        if (loop.kind != ProceduralKind::Loop || !loop.loopLabel.empty() || loop.children.size() < 2 ||
            jumps[loop.loop] != 1) continue;
        const auto &exit = model.nodes.at(loop.children.back());
        if (exit.syntax.kind == ProceduralKind::Exit && exit.syntax.loop == loop.loop && !exit.code.empty()) {
          repeatLoops.insert(loop.loop);
          labeledLoops.erase(loop.loop);
        }
      }
    }
    bool returns = false;
    for (std::size_t i = 1; i < model.nodes.size(); ++i) {
      auto &node = model.nodes[i];
      returns |= node.syntax.kind == ProceduralKind::Return;
      if (target != ast().dialect) {
        const auto implicit = [&](ProceduralSymbolId symbol) {
          return symbol && ast().symbols.at(symbol).kind == SymbolKind::Implicit;
        };
        if (implicit(node.syntax.target) || (node.syntax.selectInto &&
            std::any_of(node.syntax.selectInto->targets.begin(), node.syntax.selectInto->targets.end(), implicit)))
          fail(node.syntax.origin, "target needs an explicit mapping for the PL/pgSQL FOUND status variable");
        for (const auto &q : node.code.queries)
          for (const auto &e : q.expressions)
            if (implicit(e.localSymbol))
              fail(e.origin, "target needs an explicit mapping for the PL/pgSQL FOUND status variable");
      }
      if (mssql() && (node.syntax.kind == ProceduralKind::Repeat || node.syntax.kind == ProceduralKind::Loop ||
                      node.syntax.kind == ProceduralKind::Exit || node.syntax.kind == ProceduralKind::Continue))
        fail(node.syntax.origin, "target needs an explicit procedural loop and BREAK mapping");
      if (node.syntax.kind == ProceduralKind::Case)
        validateCase(node);
      if (node.syntax.kind == ProceduralKind::Sql && ast().function && mssql())
        fail(node.syntax.origin, "SQL Server scalar functions cannot preserve "
                                 "arbitrary table side effects/result sets");
      if (node.syntax.kind == ProceduralKind::Return && !ast().function &&
          mysql())
        fail(node.syntax.origin,
             "procedure RETURN requires an outer LEAVE label rewrite in MySQL");
      if (node.syntax.kind == ProceduralKind::Sql &&
          (ast().dialect == Dialect::Oracle ||
           ast().dialect == Dialect::PostgreSQL))
        for (const auto &q : node.code.queries)
          if (!q.tables.empty() && !q.catalogChecked)
            for (const auto &e : q.expressions)
              if (e.localSymbol && e.name.size() == 1)
                fail(
                    e.origin,
                    "column/variable ambiguity needs catalog-backed procedural "
                    "resolution; qualify variables with a block/routine label");
      if (node.syntax.discardRows && !pg())
        fail(node.syntax.origin, "PERFORM needs a target mapping for discarded rows and FOUND status");
      if (node.syntax.kind == ProceduralKind::Sql && !node.syntax.selectInto &&
          !node.syntax.discardRows && !node.code.roots.empty() &&
          node.code.queries.at(node.code.roots.front() - 1).kind ==
              StatementKind::Select &&
          (pg() || oracle()))
        fail(node.syntax.origin,
             "bare SELECT result sets need an explicit INTO/PERFORM/REF "
             "CURSOR/return-signature mapping in this procedural target");
      if (target != ast().dialect && !node.code.roots.empty()) {
        const auto &q = node.code.queries.at(node.code.roots[0] - 1);
        if (q.projections.size() == 1 &&
            node.syntax.kind != ProceduralKind::Sql) {
          const auto &e = q.expressions.at(q.projections[0].expression);
          const bool explicitTruth = node.syntax.kind == ProceduralKind::Repeat || node.syntax.kind == ProceduralKind::Exit ||
              node.syntax.kind == ProceduralKind::Continue ||
              (!mysql() && (node.syntax.kind == ProceduralKind::If || node.syntax.kind == ProceduralKind::While ||
                            node.syntax.kind == ProceduralKind::CaseWhen));
          if (explicitTruth && e.type != ValueType::Boolean &&
              e.type != ValueType::Null)
            fail(node.syntax.origin, "procedural truth-value coercion requires "
                                     "an explicit boolean/predicate rewrite");
          ValueType expected = ValueType::Unknown;
          if (node.syntax.kind == ProceduralKind::Return && ast().function)
            expected = typeCategory(ast().result.name, target);
          if ((node.syntax.kind == ProceduralKind::Assign ||
               node.syntax.kind == ProceduralKind::Declare) &&
              node.syntax.target && node.syntax.target < ast().symbols.size())
            expected = typeCategory(ast().symbols[node.syntax.target].type.name,
                                    target);
          auto numeric = [](ValueType t) {
            return t == ValueType::Integer || t == ValueType::Decimal;
          };
          if (expected != ValueType::Unknown && e.type != ValueType::Unknown &&
              e.type != ValueType::Null && e.type != expected &&
              !(numeric(expected) && numeric(e.type)))
            fail(node.syntax.origin,
                 "procedural assignment/return implicit coercion requires an "
                 "explicit typed rewrite");
        }
      }
      lowerSelectInto(node);
      if (target != ast().dialect &&
          std::any_of(node.code.queries.begin(), node.code.queries.end(),
                      [](const auto &query) { return bool(query.transaction); }))
        fail(node.syntax.origin, "routine transaction control requires a target session policy");
      if (!node.code.empty() &&
          !lowerTarget(node.code, target, options, errors))
        throw EmitFailure{errors.front()};
    }
    if (ast().function && !returns)
      fail(ast().origin, "scalar function has no RETURN");
    if (ast().function && mssql()) {
      const auto &body = ast().nodes.at(ast().body);
      if (body.children.empty() ||
          ast().nodes.at(body.children.back()).kind != ProceduralKind::Return)
        fail(ast().origin, "SQL Server scalar function requires a final "
                           "RETURN; refusing to invent a return value");
    }
    std::set<std::string> used;
    for (const auto &node : model.nodes)
      for (const auto &q : node.code.queries)
        for (const auto &e : q.expressions)
          for (const auto &part : e.name)
            used.insert(part);
    names = options.generation;
    names.localNames.resize(ast().symbols.size());
    for (std::size_t i = 1; i < ast().symbols.size(); ++i) {
      const auto &symbol = ast().symbols[i];
      const bool exceptionStatus = symbol.kind == SymbolKind::ExceptionStatus;
      const bool keepName = symbol.kind == SymbolKind::Parameter || symbol.kind == SymbolKind::Implicit ||
                            exceptionStatus;
      std::string spelling = keepName ? symbol.name : "nvd_v" + std::to_string(i);
      while (!keepName && used.count(spelling))
        spelling += '_';
      used.insert(spelling);
      if (symbol.kind == SymbolKind::Parameter && mssql() &&
          spelling.find_first_not_of("abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNO"
                                     "PQRSTUVWXYZ0123456789_") !=
              std::string::npos)
        fail(
            symbol.origin,
            "SQL Server parameter name cannot preserve this source identifier");
      auto &n = names.localNames[i];
      n.name = spelling;
      if (pg() || oracle()) {
        if (exceptionStatus)
          n.scope = "nvd_b" + std::to_string(ast().scopes.at(symbol.scope).parent);
        else if (keepName)
          n.scope = ast().name.back();
        else if (symbol.kind == SymbolKind::LoopIndex)
          n.scope = "nvd_loop_" + std::to_string(numericLoopScopes.at(symbol.scope));
        else
          n.scope =
              "nvd_b" + std::to_string(symbol.scope == 1
                                           ? ast().nodes.at(ast().body).scope
                                           : symbol.scope);
      }
    }
  }
  std::vector<Diagnostic> errors;
  void declaration(const ProceduralSymbol &symbol,
                   const ProceduralModelNode *initializer) {
    std::string value = variable(symbol.id, false) + " " + type(symbol.type);
    if (initializer && !initializer->code.empty())
      value += (mysql()   ? " DEFAULT "
                : mssql() ? " = "
                          : " := ") +
               scalar(*initializer);
    line((mysql() || mssql() ? "DECLARE " : "") + value + ";");
  }
  void children(const std::vector<std::uint32_t> &nodes) {
    for (auto id : nodes)
      emit(id);
  }
  void caseBody(const std::vector<std::uint32_t> &nodes) {
    ++indent;
    if (nodes.empty())
      line(mysql() ? "DO 0;" : "NULL;");
    else
      children(nodes);
    --indent;
  }
  void block(std::uint32_t id, bool outer = false) {
    const auto &block = ast().nodes.at(id);
    const auto label = "nvd_b" + std::to_string(block.scope);
    const bool sqlServerSource = ast().dialect == Dialect::SQLServer;
    if (pg() || oracle())
      line("<<" + quote(label) + ">>");
    std::vector<std::uint32_t> declarations;
    for (auto child : block.children)
      if (ast().nodes.at(child).kind == ProceduralKind::Declare)
        declarations.push_back(child);
    if ((pg() || oracle()) &&
        ((!declarations.empty() && !sqlServerSource) ||
         (outer && sqlServerSource &&
          ast().symbols.size() > ast().parameters.size() + 1))) {
      line("DECLARE");
      ++indent;
      if (outer && sqlServerSource) {
        for (std::size_t i = 1; i < ast().symbols.size(); ++i)
          if (ast().symbols[i].kind == SymbolKind::Variable)
            declaration(ast().symbols[i], nullptr);
      } else
        for (auto child : declarations)
          declaration(ast().symbols[ast().nodes[child].target],
                      &model.nodes[child]);
      --indent;
    }
    const auto &sourceLabel=ast().scopes.at(block.scope).label;
    const bool labeled=mysql() && isMySqlDialect(ast().dialect) && !sourceLabel.empty();
    line(labeled?quote(sourceLabel)+": BEGIN":"BEGIN");
    ++indent;
    if (mysql()) {
      if (outer && sqlServerSource) {
        for (std::size_t i = 1; i < ast().symbols.size(); ++i)
          if (ast().symbols[i].kind == SymbolKind::Variable)
            declaration(ast().symbols[i], nullptr);
      } else
        for (auto child : declarations)
          declaration(ast().symbols[ast().nodes[child].target],
                      &model.nodes[child]);
    }
    bool any = false;
    for (auto child : block.children) {
      if (ast().nodes[child].kind == ProceduralKind::Declare && !mssql() &&
          !sqlServerSource)
        continue;
      emit(child);
      any = true;
    }
    if (!any && !labeled)
      line(mysql() ? "DO 0;" : mssql() ? ";" : "NULL;");
    --indent;
    if (!block.handlers.empty()) {
      line("EXCEPTION");
      ++indent;
      children(block.handlers);
      --indent;
    }
    line("END" + (labeled?" "+quote(sourceLabel):std::string{}) + (mssql()?std::string{}:std::string(";")));
  }
  std::string exceptionCondition(const ExceptionCondition &item) {
    using Kind = ExceptionConditionKind;
    switch (item.kind) {
    case Kind::Named:
      if (item.name.empty()) fail(item.origin, "exception condition name is missing");
      return qualified(item.name);
    case Kind::SqlState:
      if (item.sqlState.size() != 5 ||
          item.sqlState.find_first_not_of("0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZ") != std::string::npos)
        fail(item.origin, "invalid SQLSTATE condition");
      return "SQLSTATE '" + item.sqlState + "'";
    case Kind::Others:
      if (mysql()) fail(item.origin, "OTHERS is not a MySQL handler condition");
      return "OTHERS";
    case Kind::MySqlErrorCode:
      if (!mysql() || !item.errorCode) fail(item.origin, "invalid MySQL handler error code");
      return std::to_string(item.errorCode);
    case Kind::SqlWarning:
      if (!mysql()) fail(item.origin, "SQLWARNING requires a MySQL-family handler");
      return "SQLWARNING";
    case Kind::NotFound:
      if (!mysql()) fail(item.origin, "NOT FOUND requires a MySQL-family handler");
      return "NOT FOUND";
    case Kind::SqlException:
      if (!mysql()) fail(item.origin, "SQLEXCEPTION requires a MySQL-family handler");
      return "SQLEXCEPTION";
    }
    fail(item.origin, "unknown exception condition");
  }
  void emit(std::uint32_t id) {
    if (++emissionVisits > options.generation.maxVisits ||
        emissionDepth >=
            std::min<std::size_t>(256, options.generation.maxNesting) ||
        !emitting.insert(id).second)
      throw EmitFailure{{DiagnosticCode::ResourceLimit, ast().origin,
                         "routine emission depth/visit/cycle budget exceeded"}};
    ++emissionDepth;
    struct Leave {
      std::size_t &depth;
      std::set<std::uint32_t> &active;
      std::uint32_t id;
      ~Leave() {
        --depth;
        active.erase(id);
      }
    } leave{emissionDepth, emitting, id};
    const auto &node = model.nodes.at(id);
    const auto &n = node.syntax;
    switch (n.kind) {
    case ProceduralKind::Block:
      block(id);
      break;
    case ProceduralKind::Declare:
      if (mssql())
        declaration(ast().symbols[n.target], &node);
      else if (ast().dialect == Dialect::SQLServer && !node.code.empty())
        line((mysql() ? "SET " : "") + variable(n.target, false) +
             (mysql() ? " = " : " := ") + scalar(node) + ";");
      break;
    case ProceduralKind::Assign:
      line((mysql() || mssql() ? "SET " : "") + variable(n.target, false) +
           (mysql() || mssql() ? " = " : " := ") + scalar(node) + ";");
      break;
    case ProceduralKind::Return:
      line("RETURN" + (node.code.empty() ? std::string{} : " " + scalar(node)) +
           ";");
      break;
    case ProceduralKind::Null:
      line(mysql() ? "DO 0;" : mssql() ? ";" : "NULL;");
      break;
    case ProceduralKind::SetNoCountOn:
    case ProceduralKind::SetNoCountOff:
      if (!mssql() || ast().dialect != Dialect::SQLServer || ast().function)
        fail(n.origin, "SET NOCOUNT has no equivalent routine setting in the target dialect");
      line(n.kind == ProceduralKind::SetNoCountOn ? "SET NOCOUNT ON;" : "SET NOCOUNT OFF;");
      break;
    case ProceduralKind::Sql: {
      auto sql = generateSql(node.code, target, names);
      if (!sql.ok())
        throw EmitFailure{sql.diagnostics.front()};
      if (n.discardRows) {
        if (sql.sql.rfind("SELECT ", 0) != 0)
          fail(n.origin, "PERFORM needs a SELECT projection in this subset");
        sql.sql.replace(0, 6, "PERFORM");
      }
      line(sql.sql);
      break;
    }
    case ProceduralKind::If:
      line("IF " + scalar(node) + (mssql() ? "" : " THEN"));
      if (mssql()) {
        auto branch = [&](const auto &childrenIds) {
          if (childrenIds.size() == 1 &&
              model.nodes.at(childrenIds[0]).syntax.kind ==
                  ProceduralKind::Block) {
            emit(childrenIds[0]);
            return;
          }
          line("BEGIN");
          ++indent;
          children(childrenIds);
          --indent;
          line("END");
        };
        branch(n.children);
        if (!n.otherwise.empty()) {
          line("ELSE");
          branch(n.otherwise);
        }
      } else {
        ++indent;
        children(n.children);
        --indent;
        if (!n.otherwise.empty()) {
          line("ELSE");
          ++indent;
          children(n.otherwise);
          --indent;
        }
        line("END IF;");
      }
      break;
    case ProceduralKind::Case:
      line(node.code.empty() ? "CASE" : "CASE " + scalar(node));
      ++indent;
      children(n.children);
      if (n.hasElse) {
        line("ELSE");
        caseBody(n.otherwise);
      }
      --indent;
      line("END CASE;");
      break;
    case ProceduralKind::CaseWhen: {
      auto condition = scalar(node);
      for (auto operand : n.caseOperands) {
        if (!condition.empty())
          condition += ", ";
        condition += scalar(model.nodes.at(operand));
      }
      line("WHEN " + condition + " THEN");
      caseBody(n.children);
      break;
    }
    case ProceduralKind::ExceptionHandler: {
      std::string condition;
      for (const auto &item : n.conditions) {
        if (!condition.empty()) condition += " OR ";
        condition += exceptionCondition(item);
      }
      line("WHEN " + condition + " THEN");
      caseBody(n.children);
      break;
    }
    case ProceduralKind::MySqlCondition:
      if (!mysql() || n.conditionName.empty() || n.conditions.size() != 1 ||
          (n.conditions.front().kind != ExceptionConditionKind::SqlState &&
           n.conditions.front().kind != ExceptionConditionKind::MySqlErrorCode))
        fail(n.origin, "invalid MySQL condition declaration");
      line("DECLARE " + quote(n.conditionName) + " CONDITION FOR " + exceptionCondition(n.conditions.front()) + ";");
      break;
    case ProceduralKind::CursorDeclare:
    case ProceduralKind::CursorOpen:
    case ProceduralKind::CursorFetch:
    case ProceduralKind::CursorClose: {
      if (!mysql() || !n.cursor || !n.cursor->cursor || n.cursor->cursor >= ast().cursors.size())
        fail(n.origin, "invalid cursor reference in the routine model");
      const auto cursor = quote(ast().cursors[n.cursor->cursor].name);
      if (n.kind == ProceduralKind::CursorDeclare) {
        auto query = generateSql(node.code, target, names);
        if (!query.ok()) throw EmitFailure{query.diagnostics.front()};
        line("DECLARE " + cursor + " CURSOR FOR " + query.sql);
      } else if (n.kind == ProceduralKind::CursorFetch) {
        if (n.cursor->targets.empty()) fail(n.origin, "FETCH requires target variables");
        std::string text = "FETCH " + cursor + " INTO ";
        for (std::size_t i = 0; i < n.cursor->targets.size(); ++i) {
          if (i) text += ", ";
          const auto name = variable(n.cursor->targets[i], false);
          if (name.size() > options.generation.maxBytes ||
              text.size() > options.generation.maxBytes - name.size())
            fail(n.origin, "FETCH target list exceeds the SQL byte limit");
          text += name;
        }
        line(text + ";");
      } else {
        line(std::string(n.kind == ProceduralKind::CursorOpen ? "OPEN " : "CLOSE ") + cursor + ";");
      }
      break;
    }
    case ProceduralKind::MySqlHandler: {
      if (!mysql() || !n.mysqlHandlerMode || n.conditions.empty() || n.children.size() != 1)
        fail(n.origin, "invalid MySQL handler declaration");
      std::string text = *n.mysqlHandlerMode == MySqlHandlerMode::Continue
          ? "DECLARE CONTINUE HANDLER FOR " : "DECLARE EXIT HANDLER FOR ";
      for (std::size_t i = 0; i < n.conditions.size(); ++i) {
        if (i) text += ", ";
        const auto condition = exceptionCondition(n.conditions[i]);
        if (condition.size() > options.generation.maxBytes || text.size() > options.generation.maxBytes - condition.size())
          fail(n.origin, "MySQL handler declaration exceeds the SQL byte limit");
        text += condition;
      }
      line(text);
      ++indent;
      emit(n.children.front());
      --indent;
      break;
    }
    case ProceduralKind::MySqlSignal:
    case ProceduralKind::MySqlResignal: {
      const bool resignal=n.kind==ProceduralKind::MySqlResignal;
      if (!mysql() || n.conditions.size()>1 || (!resignal && n.conditions.empty()))
        fail(n.origin,"invalid MySQL SIGNAL statement");
      std::string text=resignal?"RESIGNAL":"SIGNAL";
      if (!n.conditions.empty()) text+=" "+exceptionCondition(n.conditions.front());
      for (std::size_t i=0;i<n.children.size();++i) {
        const auto &item=model.nodes.at(n.children[i]);
        const auto *field=item.syntax.mysqlSignalField?mysqlDiagnosticInfo(*item.syntax.mysqlSignalField):nullptr;
        if (item.syntax.kind!=ProceduralKind::MySqlSignalValue || !field ||
            field->use!=MySqlDiagnosticUse::SignalAndCondition || item.code.roots.empty())
          fail(item.syntax.origin,"invalid MySQL SIGNAL condition field");
        const auto value=scalar(item);
        if (value.size()>options.generation.maxBytes ||
            text.size()>options.generation.maxBytes-value.size())
          fail(item.syntax.origin,"MySQL SIGNAL exceeds the SQL byte limit");
        text+=std::string(i?", ":" SET ")+field->spelling+" = "+value;
      }
      line(text+";");
      break;
    }
    case ProceduralKind::MySqlSignalValue:
      fail(n.origin,"SIGNAL field used outside a SIGNAL statement");
    case ProceduralKind::MySqlGetDiagnostics: {
      if (!mysql() || !n.mysqlDiagnostics || n.mysqlDiagnostics->targets.empty() ||
          (n.mysqlDiagnostics->stacked && target==Dialect::MariaDB))
        fail(n.origin,"invalid MySQL GET DIAGNOSTICS form");
      const auto &read=*n.mysqlDiagnostics;
      std::string text=read.stacked?"GET STACKED DIAGNOSTICS ":"GET DIAGNOSTICS ";
      const bool condition=!node.code.roots.empty();
      if (condition) text+="CONDITION "+scalar(node)+" ";
      for (std::size_t i=0;i<read.targets.size();++i) {
        const auto &item=read.targets[i];
        const auto *field=mysqlDiagnosticInfo(item.field);
        if (!field || !field->readable(target,condition))
          fail(item.origin,"diagnostic field does not match the target form");
        std::string destination;
        if (const auto *symbol=std::get_if<ProceduralSymbolId>(&item.destination))
          destination=variable(*symbol,false);
        else {
          destination=std::get<std::string>(item.destination);
          if (!isSimpleMySqlUserVariable(destination))
            fail(item.origin,"invalid MySQL diagnostic user variable");
        }
        if (i) text+=", ";
        if (destination.size()>options.generation.maxBytes ||
            text.size()>options.generation.maxBytes-destination.size())
          fail(item.origin,"MySQL diagnostics output exceeds the SQL byte limit");
        text+=destination+" = "+field->spelling;
      }
      line(text+";");
      break;
    }
    case ProceduralKind::MySqlSessionAssignment: {
      const auto &var = n.sessionVariable;
      if (!mysql() || !isSimpleMySqlUserVariable(var))
        fail(n.origin, "unsupported MySQL session variable spelling");
      line("SET " + var + " = " + scalar(node) + ";");
      break;
    }
    case ProceduralKind::Rethrow:
      line(mssql() ? "THROW;" : "RAISE;");
      break;
    case ProceduralKind::TsqlTryCatch:
      line("BEGIN TRY");
      ++indent;
      children(n.children);
      --indent;
      line("END TRY");
      line("BEGIN CATCH");
      ++indent;
      children(n.otherwise);
      --indent;
      line("END CATCH;");
      break;
    case ProceduralKind::TsqlThrow: {
      if (!n.throwArguments) fail(n.origin, "THROW arguments are missing");
      std::string statement = "THROW ";
      const auto &args = *n.throwArguments;
      for (std::size_t i = 0; i < args.size(); ++i) {
        if (i) statement += ", ";
        const auto &arg = args[i];
        if (const auto symbol = std::get_if<ProceduralSymbolId>(&arg.value))
          statement += variable(*symbol);
        else if (const auto integer = std::get_if<std::int64_t>(&arg.value))
          statement += std::to_string(*integer);
        else {
          if (arg.unicode) statement += 'N';
          statement += '\'';
          for (char c : std::get<std::string>(arg.value)) {
            if (c == '\'') statement += '\'';
            statement += c;
          }
          statement += '\'';
        }
      }
      line(statement + ";");
      break;
    }
    case ProceduralKind::GetDiagnostics: {
      if (!n.pgDiagnostics || n.pgDiagnostics->targets.empty())
        fail(n.origin, "invalid GET DIAGNOSTICS statement");
      const auto &read = *n.pgDiagnostics;
      std::string statement = read.area == PgDiagnosticArea::Stacked
          ? "GET STACKED DIAGNOSTICS " : "GET DIAGNOSTICS ";
      for (std::size_t i = 0; i < read.targets.size(); ++i) {
        const auto &target = read.targets[i];
        const auto field = std::find_if(std::begin(pgDiagnosticFields), std::end(pgDiagnosticFields),
            [&](const auto &item) { return item.id == target.field; });
        if (field == std::end(pgDiagnosticFields) || !field->available(read.area))
          fail(target.origin, "invalid GET DIAGNOSTICS field");
        if (i) statement += ", ";
        statement += variable(target.symbol) + " = " + field->spelling;
      }
      line(statement + ";");
      break;
    }
    case ProceduralKind::CaseOperand:
      fail(n.origin, "CASE operand cannot be emitted as a statement");
      break;
    case ProceduralKind::RangeBound:
      fail(n.origin, "FOR range bound cannot be emitted as a statement");
      break;
    case ProceduralKind::NumericFor: {
      if ((!pg() && !oracle()) || n.rangeBounds.size() < 2 || n.rangeBounds.size() > 3)
        fail(n.origin, "invalid numeric FOR range");
      auto header = loopPrefix(n.loop) + "FOR " + variable(n.target, false) + " IN " +
          (n.reverseFor ? "REVERSE " : "") + scalar(model.nodes.at(n.rangeBounds[0])) + " .. " +
          scalar(model.nodes.at(n.rangeBounds[1]));
      if (n.rangeBounds.size() == 3)
        header += " BY " + scalar(model.nodes.at(n.rangeBounds[2]));
      line(header + " LOOP");
      ++indent;
      children(n.children);
      --indent;
      line("END LOOP;");
      break;
    }
    case ProceduralKind::While:
      line(loopPrefix(n.loop) + "WHILE " + scalar(node) +
           (mssql()   ? ""
            : mysql() ? " DO"
                      : " LOOP"));
      if (mssql())
        line("BEGIN");
      ++indent;
      children(n.children);
      --indent;
      line(mssql() ? "END" : mysql() ? "END WHILE;" : "END LOOP;");
      break;
    case ProceduralKind::Repeat:
      line(loopPrefix(n.loop) + (mysql() ? "REPEAT" : "LOOP"));
      ++indent;
      children(n.children);
      if (!mysql()) line("EXIT WHEN " + scalar(node) + ";");
      --indent;
      if (mysql()) line("UNTIL " + scalar(node));
      line(mysql() ? "END REPEAT;" : "END LOOP;");
      break;
    case ProceduralKind::Loop: {
      const bool repeat = repeatLoops.count(n.loop);
      line(loopPrefix(n.loop) + (repeat ? "REPEAT" : "LOOP"));
      ++indent;
      if (repeat) {
        for (std::size_t i = 0; i + 1 < n.children.size(); ++i) emit(n.children[i]);
      } else if (n.children.empty() && mysql()) {
        line("DO 0;");
      } else {
        children(n.children);
      }
      --indent;
      if (repeat) line("UNTIL " + scalar(model.nodes.at(n.children.back())));
      line(repeat ? "END REPEAT;" : "END LOOP;");
      break;
    }
    case ProceduralKind::Exit:
    case ProceduralKind::Continue: {
      const bool keepGoing = n.kind == ProceduralKind::Continue;
      const bool conditional = !node.code.empty();
      if (n.blockTarget) {
        if (!mysql() || keepGoing || conditional || n.blockTarget>=ast().scopes.size() ||
            ast().scopes[n.blockTarget].label.empty() || ast().scopes[n.blockTarget].label!=n.loopLabel)
          fail(n.origin,"invalid MySQL block exit target");
        line("LEAVE "+quote(n.loopLabel)+";");
        break;
      }
      if (mysql()) {
        if (conditional) {
          line("IF " + scalar(node) + " THEN");
          ++indent;
        }
        line(std::string(keepGoing ? "ITERATE " : "LEAVE ") + loopLabel(n.loop) + ";");
        if (conditional) {
          --indent;
          line("END IF;");
        }
      } else {
        auto jump = std::string(keepGoing ? "CONTINUE" : "EXIT");
        if (!n.loopLabel.empty()) jump += " " + loopLabel(n.loop);
        if (conditional) jump += " WHEN " + scalar(node);
        line(jump + ";");
      }
      break;
    }
    }
  }

public:
  RoutineEmitter(const RoutineModel &m, Dialect d, ConversionOptions o)
      : model(m), target(d), options(std::move(o)) {}
  GeneratedSql run() {
    GeneratedSql result;
    try {
      validateAndLower();
      std::string header = "CREATE ";
      switch (ast().action) {
      case RoutineDefinitionAction::Create:
        break;
      case RoutineDefinitionAction::CreateOrReplace:
        if (mssql() || mysql())
          fail(ast().origin, "target OR REPLACE routine behavior requires an explicit dependency policy");
        header += "OR REPLACE ";
        break;
      case RoutineDefinitionAction::CreateOrAlter:
      case RoutineDefinitionAction::Alter:
        if (!mssql() || ast().dialect != Dialect::SQLServer)
          fail(ast().origin, "T-SQL routine replacement requires an explicit target dependency policy");
        header = ast().action == RoutineDefinitionAction::Alter ? "ALTER " : "CREATE OR ALTER ";
        break;
      }
      if (ast().definer) {
        const auto &owner=*ast().definer;
        header+="DEFINER = ";
        header+=owner.currentUser?"CURRENT_USER ":quote(owner.user)+"@"+quote(owner.host)+" ";
      }
      header += ast().function ? "FUNCTION " : "PROCEDURE ";
      if (ast().ifNotExists) header += "IF NOT EXISTS ";
      auto routineName = ast().name;
      for (const auto &mapping : options.qualifiers)
        if (routineName.size() > mapping.source.size() &&
            std::equal(mapping.source.begin(), mapping.source.end(),
                       routineName.begin())) {
          std::vector<std::string> mapped = mapping.target;
          mapped.insert(mapped.end(),
                        routineName.begin() + mapping.source.size(),
                        routineName.end());
          routineName = std::move(mapped);
          break;
        }
      header += qualified(routineName);
      if (!mssql() || ast().function)
        header += '(';
      else
        header += ' ';
      for (std::size_t i = 0; i < ast().parameters.size(); ++i) {
        if (i)
          header += ", ";
        const auto id = ast().parameters[i];
        const auto &p = ast().symbols[id];
        if ((mysql() && !ast().function) || pg())
          header += (p.mode == ParameterMode::In    ? "IN "
                     : p.mode == ParameterMode::Out ? "OUT "
                                                    : "INOUT ");
        header += variable(id, false);
        if (oracle())
          header += (p.mode == ParameterMode::In    ? " IN "
                     : p.mode == ParameterMode::Out ? " OUT "
                                                    : " IN OUT ");
        else
          header += ' ';
        header += type(p.type);
        if (mssql() && p.mode != ParameterMode::In)
          header += " OUTPUT";
      }
      if (!mssql() || ast().function)
        header += ')';
      if (ast().function)
        header += (oracle() ? " RETURN " : " RETURNS ") + type(ast().result);
      line(header);
      if (pg()) {
        line("LANGUAGE plpgsql");
        line(ast().securityDefiner ? "SECURITY DEFINER" : "SECURITY INVOKER");
        if (ast().function)
          line(ast().volatility);
        std::string tag = "$navidog$";
        bool collision = true;
        while (collision) {
          collision = false;
          for (const auto &n : model.nodes)
            for (const auto &q : n.code.queries)
              for (const auto &e : q.expressions)
                if (e.value.find(tag) != std::string::npos)
                  collision = true;
          if (collision)
            tag.insert(tag.size() - 1, "_");
        }
        line("AS " + tag);
        block(ast().body, true);
        line(tag + ";");
      } else if (oracle()) {
        line(ast().securityDefiner ? "AUTHID DEFINER" : "AUTHID CURRENT_USER");
        line("AS");
        line("BEGIN");
        ++indent;
        block(ast().body, true);
        --indent;
        line("END;");
      } else if (mssql()) {
        line("AS");
        block(ast().body, true);
      } else {
        if (ast().function)
          line(ast().deterministic ? "DETERMINISTIC" : "NOT DETERMINISTIC");
        switch (ast().dataAccess) {
        case RoutineDataAccess::Unspecified: break;
        case RoutineDataAccess::ContainsSql: line("CONTAINS SQL"); break;
        case RoutineDataAccess::NoSql: line("NO SQL"); break;
        case RoutineDataAccess::ReadsSqlData: line("READS SQL DATA"); break;
        case RoutineDataAccess::ModifiesSqlData: line("MODIFIES SQL DATA"); break;
        }
        line(ast().securityDefiner ? "SQL SECURITY DEFINER"
                                   : "SQL SECURITY INVOKER");
        if (ast().explicitLanguageSql) line("LANGUAGE SQL");
        if (ast().comment) line("COMMENT " + commentLiteral(*ast().comment));
        block(ast().body, true);
      }
      result.sql = std::move(output);
    } catch (const EmitFailure &failure) {
      result.diagnostics.push_back(failure.diagnostic);
    } catch (const std::exception &error) {
      result.diagnostics.push_back({DiagnosticCode::InvalidQueryShape,
                                    model.syntax.origin, error.what()});
    }
    return result;
  }
};
} // namespace
GeneratedSql emitRoutine(const RoutineModel &model, Dialect target,
                         const ConversionOptions &options) {
  return RoutineEmitter(model, target, options).run();
}
} // namespace navidog::sql::compiler
