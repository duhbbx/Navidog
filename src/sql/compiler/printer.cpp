#include "navidog/sql/compiler/compiler.hpp"
#include "navidog/sql/compiler/operator_catalog.hpp"
#include "parser.hpp"
#include "sqlserver_syntax.hpp"
#include <algorithm>
#include <stdexcept>
#include <unordered_set>

namespace navidog::sql::compiler {
namespace {
struct PrintFailure {
  DiagnosticCode code;
  std::string message;
};
class Printer {
  const ModelProgram &program;
  Dialect dialect;
  GenerationOptions options;
  ExpressionColumnResolver columns;
  ExpressionQueryResolver subqueries;
  std::string sql;
  std::size_t visits = 0, lastPoll = 0;
  std::vector<QueryId> active;
  bool allowNamedWindowReferences = true;
  SourceRange origin;
  bool mysql() const {
    return isMySqlDialect(dialect);
  }
  bool pg() const { return dialect == Dialect::PostgreSQL; }
  bool oracle() const { return dialect == Dialect::Oracle; }
  bool mssql() const { return dialect == Dialect::SQLServer; }
  bool sqlite() const { return dialect == Dialect::SQLite; }
  [[noreturn]] void
  fail(std::string message,
       DiagnosticCode code = DiagnosticCode::UnsupportedSyntax) const {
    throw PrintFailure{code, std::move(message)};
  }
  void poll() {
    if (options.shouldStop && options.shouldStop())
      fail("SQL generation stopped", DiagnosticCode::Stopped);
  }
  void tick() {
    if (++visits > options.maxVisits)
      fail("SQL generation visit limit exceeded",
           DiagnosticCode::ResourceLimit);
    if ((visits & 255u) == 0)
      poll();
  }
  void put(std::string_view text) {
    if (text.size() > options.maxBytes - sql.size())
      fail("generated SQL byte limit exceeded", DiagnosticCode::ResourceLimit);
    sql.append(text.data(), text.size());
    if (sql.size() - lastPoll >= 4096) {
      lastPoll = sql.size();
      poll();
    }
  }
  std::string quotedIdentifier(const std::string &name) const {
    if (name.empty() || name.find('\0') != std::string::npos)
      fail("invalid empty/NUL identifier", DiagnosticCode::InvalidQueryShape);
    if (name.size() > options.maxBytes)
      fail("identifier exceeds output budget", DiagnosticCode::ResourceLimit);
    const char open = mysql() ? '`' : mssql() ? '[' : '"';
    const char close = mssql() ? ']' : open;
    std::string out(1, open);
    for (char ch : name) {
      if (ch == close)
        out.push_back(close);
      out.push_back(ch);
    }
    out.push_back(close);
    return out;
  }
  void identifier(const std::string &name) { put(quotedIdentifier(name)); }
  void parameter(std::string_view value, bool userVariable = false) {
    if (value.empty() || std::string_view("?@:$").find(value.front())==std::string_view::npos)
      fail("invalid parameter prefix",DiagnosticCode::InvalidQueryShape);
    for (std::size_t i=1;i<value.size();++i) {
      const auto ch=static_cast<unsigned char>(value[i]);
      if (!((ch>='a' && ch<='z') || (ch>='A' && ch<='Z') ||
            (ch>='0' && ch<='9') || ch=='_' || ch=='$' || ch>=128 ||
            (userVariable && ((i==1 && ch=='@') || (value[1]=='@' && ch=='.')))))
        fail("invalid parameter spelling",DiagnosticCode::InvalidQueryShape);
    }
    put(value);
  }
  void name(const std::vector<std::string> &parts) {
    if (parts.empty())
      fail("missing qualified name", DiagnosticCode::InvalidQueryShape);
    for (std::size_t i = 0; i < parts.size(); ++i) {
      if (i)
        put(".");
      identifier(parts[i]);
    }
  }
  bool numeric(std::string_view value) const {
    std::size_t i = 0, digits = 0;
    while (i < value.size() && value[i] >= '0' && value[i] <= '9') {
      ++i;
      ++digits;
    }
    if (i < value.size() && value[i] == '.') {
      ++i;
      while (i < value.size() && value[i] >= '0' && value[i] <= '9') {
        ++i;
        ++digits;
      }
    }
    if (!digits)
      return false;
    if (i < value.size() && (value[i] == 'e' || value[i] == 'E')) {
      ++i;
      if (i < value.size() && (value[i] == '+' || value[i] == '-'))
        ++i;
      std::size_t start = i;
      while (i < value.size() && value[i] >= '0' && value[i] <= '9')
        ++i;
      if (i == start)
        return false;
    }
    return i == value.size();
  }
  void literal(const std::string &value, bool unicode = false) {
    if (unicode) {
      if (!mssql())
        fail("Unicode literal prefix is SQL Server-specific");
      put("N");
    }
    put("'");
    for (char ch : value) {
      if (ch == '\0') {
        if (!mysql() || program.mysqlNoBackslashEscapes)
          fail("NUL literal needs a dialect-specific encoder");
        put("\\0");
      } else if (ch == '\\' && mysql() && !program.mysqlNoBackslashEscapes)
        put("\\\\");
      else {
        if (ch == '\'')
          put("'");
        put(std::string(1, ch));
      }
    }
    put("'");
  }
  template <class T, class Emit>
  void separated(const std::vector<T> &values, Emit emit) {
    for (std::size_t i = 0; i < values.size(); ++i) {
      if (i)
        put(", ");
      emit(values[i]);
    }
  }
  const ModelStatement &query(QueryId id) const {
    if (!id || id > program.queries.size())
      fail("invalid query reference", DiagnosticCode::InvalidQueryShape);
    return program.queries[id - 1];
  }
  const ModelExpression &expression(const ModelStatement &q, ExprId id) const {
    if (!id || id >= q.expressions.size())
      fail("invalid expression reference", DiagnosticCode::InvalidQueryShape);
    return q.expressions[id];
  }
  bool operatorAllowed(const std::string &op) const {
    static const std::unordered_set<std::string> operators = {
        "+", "-",  "*",  "/",   "%",  "=",    "<>",   "<",
        ">", "<=", ">=", "AND", "OR", "LIKE", "ILIKE"};
    if (op == "DIV") return mysql();
    if (op == "XOR") return mysql();
    if (op == "REGEXP" || op == "NOT REGEXP")
      return mysql() && program.sourceDialect==dialect;
    if (op == "GLOB" || op == "NOT GLOB")
      return sqlite() && program.sourceDialect==dialect;
    if (op == "NOT ILIKE") return pg();
    if (op == "IS DISTINCT FROM" || op == "IS NOT DISTINCT FROM") return pg() || sqlite() || mssql();
    return operators.count(op);
  }
  static int portableBinaryPriority(std::string_view op) {
    if (op == "OR") return 1;
    if (op == "AND") return 2;
    if (op == "=" || op == "<>" || op == "<" || op == ">" ||
        op == "<=" || op == ">=") return 3;
    if (op == "+" || op == "-") return 4;
    if (op == "*" || op == "/" || op == "%" || op == "DIV") return 5;
    return 0;
  }
  void expressionQuery(QueryId id) {
    if (!subqueries) {
      statement(id);
      return;
    }
    const auto rendered=subqueries(id);
    if (!rendered || rendered->empty())
      fail("expression subquery has no accepted relational input",DiagnosticCode::InvalidQueryShape);
    put(*rendered);
  }
  struct ExpressionAction {
    ExprId id = 0;
    std::string text;
    QueryId query = 0;
    ExprId cast = 0;
    bool noOuter = false;
  };
  bool sameWindow(const WindowSpec &a,const WindowSpec &b) const {
    if(a.partitions!=b.partitions || a.ordering.size()!=b.ordering.size() || a.frameUnit!=b.frameUnit ||
       a.between!=b.between || a.start!=b.start || a.end!=b.end || a.exclusion!=b.exclusion) return false;
    for(std::size_t i=0;i<a.ordering.size();++i)
      if(a.ordering[i].descending!=b.ordering[i].descending || a.ordering[i].nulls!=b.ordering[i].nulls ||
         a.ordering[i].explicitAscending!=b.ordering[i].explicitAscending) return false;
    return true;
  }
  bool sourceWindowMatches(const ModelStatement &q,const WindowSpec &resolved,
                           const std::vector<ExprId> &keys,const WindowSource &source,
                           std::size_t definitionLimit) {
    if(source.keys>keys.size() || source.specification.partitions>source.keys ||
       source.specification.ordering.size()>source.keys-source.specification.partitions) return false;
    std::size_t slots=source.specification.partitions+source.specification.ordering.size();
    const auto offset=[](WindowBound b){return b==WindowBound::Preceding || b==WindowBound::Following;};
    if(!source.specification.frameUnit.empty()) {
      if(offset(source.specification.start)) ++slots;
      if(source.specification.between && offset(source.specification.end)) ++slots;
    }
    if(slots!=source.keys) return false;
    if(source.reference.empty()) return !source.bare && source.keys==keys.size() && sameWindow(source.specification,resolved);
    const auto last=q.namedWindows.begin()+std::min(definitionLimit,q.namedWindows.size());
    const auto found=std::find_if(q.namedWindows.begin(),last,[&](const auto &w){return w.name==source.reference;});
    if(found==last) return false;
    if(source.bare)
      return !source.keys && sameWindow(found->specification,resolved) && found->keys==keys;
    if(source.specification.partitions || !found->specification.frameUnit.empty() ||
       (!source.specification.ordering.empty() && !found->specification.ordering.empty()) ||
       found->keys.size()!=keys.size()-source.keys || !std::equal(found->keys.begin(),found->keys.end(),keys.begin())) return false;
    auto merged=found->specification;
    if(!source.specification.ordering.empty()) merged.ordering=source.specification.ordering;
    merged.frameUnit=source.specification.frameUnit;merged.between=source.specification.between;
    merged.start=source.specification.start;merged.end=source.specification.end;merged.exclusion=source.specification.exclusion;
    return sameWindow(merged,resolved);
  }
  std::vector<ExpressionAction> windowSequence(const WindowSpec &spec,
                    const std::vector<ExprId> &keys,const std::string &reference={}) {
        if (spec.frameUnit == "GROUPS" &&
            (mysql() || mssql() || dialect == Dialect::Oracle))
          fail("GROUPS window frames unavailable in target dialect");
        std::vector<ExpressionAction> sequence;
        if(!reference.empty()) sequence.push_back({0,quotedIdentifier(reference)+" "});
        std::size_t index = 0;
        auto expressionItem = [&] {
          tick();
          if (index >= keys.size())
            fail("missing window operand", DiagnosticCode::InvalidQueryShape);
          sequence.push_back({keys[index++], {}});
        };
        if (spec.partitions) {
          sequence.push_back({0, "PARTITION BY "});
          for (std::size_t i = 0; i < spec.partitions; ++i) {
            if (i)
              sequence.push_back({0, ", "});
            expressionItem();
          }
        }
        if (!spec.ordering.empty()) {
          sequence.push_back({0, spec.partitions ? " ORDER BY " : "ORDER BY "});
          for (std::size_t i = 0; i < spec.ordering.size(); ++i) {
            if (i)
              sequence.push_back({0, ", "});
            expressionItem();
            const auto &order = spec.ordering[i];
            if (order.descending)
              sequence.push_back({0, " DESC"});
            if (!order.descending && order.explicitAscending && pg() && program.sourceDialect==Dialect::PostgreSQL)
              sequence.push_back({0, " ASC"});
            if (!order.nulls.empty()) {
              if ((mysql() || mssql()) ||
                  (order.nulls != "FIRST" && order.nulls != "LAST"))
                fail("invalid window null ordering");
              sequence.push_back({0, " NULLS " + order.nulls});
            }
          }
        }
        auto bound = [&](WindowBound kind) {
          switch (kind) {
          case WindowBound::UnboundedPreceding:
            sequence.push_back({0, "UNBOUNDED PRECEDING"});
            break;
          case WindowBound::CurrentRow:
            sequence.push_back({0, "CURRENT ROW"});
            break;
          case WindowBound::UnboundedFollowing:
            sequence.push_back({0, "UNBOUNDED FOLLOWING"});
            break;
          case WindowBound::Preceding:
          case WindowBound::Following:
            expressionItem();
            sequence.push_back({0, kind == WindowBound::Preceding
                                       ? " PRECEDING"
                                       : " FOLLOWING"});
            break;
          default:
            fail("invalid window frame bound",
                 DiagnosticCode::InvalidQueryShape);
          }
        };
        if (!spec.frameUnit.empty()) {
          if (spec.frameUnit != "ROWS" && spec.frameUnit != "RANGE" &&
              spec.frameUnit != "GROUPS")
            fail("invalid window frame unit",
                 DiagnosticCode::InvalidQueryShape);
          sequence.push_back(
              {0, (spec.partitions || !spec.ordering.empty() ? " " : "") +
                      spec.frameUnit + " "});
          if (spec.between)
            sequence.push_back({0, "BETWEEN "});
          bound(spec.start);
          if (spec.between) {
            sequence.push_back({0, " AND "});
            bound(spec.end);
          }
        }
        if(static_cast<unsigned>(spec.exclusion)>static_cast<unsigned>(WindowExclusion::Ties))
          fail("invalid window exclusion",DiagnosticCode::InvalidQueryShape);
        if(spec.exclusion!=WindowExclusion::None) {
          if(spec.frameUnit.empty())fail("window exclusion requires a frame",DiagnosticCode::InvalidQueryShape);
          if(dialect!=Dialect::PostgreSQL && dialect!=Dialect::SQLite)fail("window EXCLUDE is unavailable in target dialect");
          sequence.push_back({0,std::string(" EXCLUDE ")+windowExclusionName(spec.exclusion)});
        }
        if (index != keys.size())
          fail("unexpected window operand", DiagnosticCode::InvalidQueryShape);
        return sequence;
  }
  void namedWindowClause(const ModelStatement &q) {
    if(!pg() || program.sourceDialect!=Dialect::PostgreSQL || q.namedWindows.empty()) return;
    put(" WINDOW ");
    for(std::size_t i=0;i<q.namedWindows.size();++i) {
      tick();if(i) put(", ");const auto &w=q.namedWindows[i];
      identifier(w.name);put(" AS (");
      auto spec=w.specification;auto keys=w.keys;std::string reference;
      if(w.source && !w.source->bare && sourceWindowMatches(q,w.specification,w.keys,*w.source,i)) {
        spec=w.source->specification;reference=w.source->reference;
        keys.erase(keys.begin(),keys.end()-w.source->keys);
      }
      for(const auto &action:windowSequence(spec,keys,reference)) {
        if(action.id) expr(q,action.id);else put(action.text);
      }
      put(")");
    }
  }
  void expr(const ModelStatement &q, ExprId root, bool delimited = false) {
    using Action=ExpressionAction;
    std::vector<Action> work{{root, {}, 0, 0,
        options.compactExpressions && delimited}};
    auto text = [&](std::string value) {
      work.push_back({0, std::move(value)});
    };
    auto child = [&](ExprId id) { work.push_back({id, {}}); };
    auto operands = [&](const ModelExpression &e) {
      for (std::size_t i = e.operands.size(); i > 0; --i) {
        child(e.operands[i - 1]);
        if (i > 1)
          text(", ");
      }
    };
    while (!work.empty()) {
      tick();
      auto action = std::move(work.back());
      work.pop_back();
      if (action.query) {
        expressionQuery(action.query);
        continue;
      }
      if (action.cast) {
        const auto &e = expression(q, action.cast);
        if(e.castType) {
          const auto resolved=resolveDeclaredType(*e.castType,dialect,TypeUse::Cast);
          if(!resolved.error.empty())fail(resolved.error,DiagnosticCode::InvalidQueryShape);
          type(*e.castType);continue;
        }
        BasicTypeSpec<std::vector<std::string>> t;
        t.name = e.name;
        t.nameQuoted = e.nameQuoted;
        if (!e.value.empty()) {
          if (e.value.front() != '(' || e.value.back() != ')' ||
              e.value.find_first_not_of("0123456789,()") != std::string::npos)
            fail("unsupported cast type arguments");
          auto args = e.value.substr(1, e.value.size() - 2);
          std::size_t start = 0;
          while (start < args.size()) {
            auto comma = args.find(',', start);
            auto part = args.substr(
                start, comma == std::string::npos ? comma : comma - start);
            if (!numeric(part))
              fail("invalid cast type argument");
            t.arguments.push_back({TokenKind::Number, part});
            if (comma == std::string::npos)
              break;
            start = comma + 1;
          }
        }
        const auto resolved=resolveDeclaredType(t,dialect,TypeUse::Cast);
        if(!resolved.error.empty())fail(resolved.error,DiagnosticCode::InvalidQueryShape);
        type(t);
        continue;
      }
      if (!action.id) {
        put(action.text);
        continue;
      }
      const auto &e = expression(q, action.id);
      if (columns && (e.kind == ExprKind::Name || e.outputColumn)) {
        const auto resolved = columns(q.id, action.id);
        if (!resolved)
          fail("expression column has no unique relational slot", DiagnosticCode::InvalidQueryShape);
        name(*resolved);
        continue;
      }
      if (e.localSymbol) {
        if(q.package && oracle()) {
          if(e.localSymbol>q.package->declarations.size())
            fail("unbound package state symbol",DiagnosticCode::InvalidQueryShape);
          identifier(q.package->declarations[e.localSymbol-1].name);
          continue;
        }
        if (e.localSymbol >= options.localNames.size() ||
            options.localNames[e.localSymbol].name.empty())
          fail("unbound procedural symbol in target backend",
               DiagnosticCode::InvalidQueryShape);
        const auto &local = options.localNames[e.localSymbol];
        if (mssql()) {
          if (local.name.find_first_not_of("abcdefghijklmnopqrstuvwxyzABCDEFGHI"
                                           "JKLMNOPQRSTUVWXYZ0123456789_") !=
              std::string::npos)
            fail("invalid local variable name");
          put("@");
          put(local.name);
        } else {
          if (!local.scope.empty()) {
            identifier(local.scope);
            put(".");
          }
          identifier(local.name);
        }
        continue;
      }
      switch (e.kind) {
      case ExprKind::MergeAction:
        if (!mssql() || q.kind!=StatementKind::Merge)
          fail("$action requires SQL Server MERGE OUTPUT",DiagnosticCode::InvalidQueryShape);
        put("$action");
        break;
      case ExprKind::Name:
        name(e.name);
        break;
      case ExprKind::Star:
        if (!e.name.empty()) {
          if (e.name.back() != "*")
            fail("invalid wildcard name", DiagnosticCode::InvalidQueryShape);
          if (e.name.size() > 1) {
            name({e.name.begin(), e.name.end() - 1});
            put(".");
          }
        }
        put("*");
        break;
      case ExprKind::Number:
        if (!numeric(e.value))
          fail("invalid numeric literal", DiagnosticCode::InvalidQueryShape);
        put(e.value);
        break;
      case ExprKind::Interval: {
        if(!e.interval || (!mysql() && !oracle() && dialect!=Dialect::PostgreSQL))
          fail("invalid interval expression",DiagnosticCode::InvalidQueryShape);
        const auto &s=*e.interval;
        if(const auto error=validateIntervalSpec(s,dialect);!error.empty())fail(error,DiagnosticCode::InvalidQueryShape);
        put("INTERVAL ");
        if(mysql()) {
          if(e.operands.size()!=1 || s.mysqlUnit==IntervalUnit::None || !*intervalUnitName(s.mysqlUnit))fail("invalid MySQL interval shape");
          text(std::string(" ")+intervalUnitName(s.mysqlUnit));child(e.operands[0]);
        } else {
          if(!e.operands.empty() || s.mysqlUnit!=IntervalUnit::None)fail("invalid interval literal shape");
          if(s.prefixPrecision) {if(oracle() || !s.fractionalPrecision)fail("invalid interval prefix precision");put("("+std::to_string(*s.fractionalPrecision)+") ");}
          literal(e.value,false);
          if(s.leading!=IntervalField::None) {
            put(std::string(" ")+intervalFieldName(s.leading));
            if(oracle() && s.leadingPrecision) {
              put("("+std::to_string(*s.leadingPrecision));
              if(s.leading==IntervalField::Second && s.fractionalPrecision)put(","+std::to_string(*s.fractionalPrecision));
              put(")");
            }
            if(s.trailing!=IntervalField::None)put(std::string(" TO ")+intervalFieldName(s.trailing));
            if(s.fractionalPrecision && !s.prefixPrecision && (!oracle() || s.leading!=IntervalField::Second))
              put("("+std::to_string(*s.fractionalPrecision)+")");
          }
        }
        break;
      }
      case ExprKind::Extract:
        if(e.operands.size()!=1 || mssql() || sqlite())fail("invalid EXTRACT target shape");
        // Parser supplied a dialect-validated field; reject malformed external IR.
        if(!validExtractField(e.value,dialect))fail("invalid EXTRACT field");
        put("EXTRACT("+e.value+" FROM ");text(")");child(e.operands[0]);
        break;
      case ExprKind::TypedLiteral:
        if(!e.castType || mssql() || dialect==Dialect::SQLite)
          fail("typed literal has no valid target type",DiagnosticCode::InvalidQueryShape);
        type(*e.castType);put(" ");literal(e.value,false);
        break;
      case ExprKind::String:
        literal(e.value, e.unicodeString);
        break;
      case ExprKind::Null:
        put("NULL");
        break;
      case ExprKind::Default:
        put("DEFAULT");
        break;
      case ExprKind::Boolean:
        if ((oracle() && q.context.environment == SemanticEnvironment::Sql) ||
            mssql() || (e.value != "TRUE" && e.value != "FALSE"))
          fail("boolean literal needs target version/type support");
        put(e.value);
        break;
      case ExprKind::Variable:
        if (!mysql() || e.value.empty() || e.value[0] != '@')
          fail("unsupported user variable");
        [[fallthrough]];
      case ExprKind::Parameter:
        parameter(e.value,e.kind==ExprKind::Variable);
        break;
      case ExprKind::Unary:
        if (e.operands.size() != 1 ||
            (e.value != "+" && e.value != "-" && e.value != "NOT"))
          fail("invalid unary expression", DiagnosticCode::InvalidQueryShape);
        put("(");
        put(e.value);
        put(" ");
        text(")");
        child(e.operands[0]);
        break;
      case ExprKind::LikeEscape:
        if (e.operands.size()!=3 ||
            (e.value!="LIKE" && e.value!="NOT LIKE" && e.value!="ILIKE" && e.value!="NOT ILIKE"))
          fail("invalid LIKE ESCAPE model",DiagnosticCode::InvalidQueryShape);
        if (program.sourceDialect!=dialect)
          fail("LIKE/ILIKE ESCAPE requires an explicit target escape and collation policy");
        if ((e.value=="ILIKE" || e.value=="NOT ILIKE") && !pg())
          fail("ILIKE ESCAPE requires PostgreSQL semantics");
        if (const auto &escape=expression(q,e.operands[2]);
            escape.kind==ExprKind::String && escape.value.empty() && !pg() && !oracle() && !mysql())
          fail("target does not support an empty ESCAPE character");
        put("(");
        text(")");
        child(e.operands[2]);
        text(" ESCAPE ");
        child(e.operands[1]);
        text(" " + e.value + " ");
        child(e.operands[0]);
        break;
      case ExprKind::Binary: {
        if (e.operands.size() != 2)
          fail("invalid binary arity", DiagnosticCode::InvalidQueryShape);
        auto op = e.value;
        if (op == "CONCAT") {
          if (mysql() && !program.mysqlPipesAsConcat)
            fail("MySQL concat operator requires PIPES_AS_CONCAT");
          op = "||";
        } else if (op == "NULL_SAFE_EQUAL") {
          if (!mysql())
            fail("null-safe equality is MySQL-specific");
          op = "<=>";
        } else if (op == "NOT LIKE") {
        } else if (!operatorAllowed(op))
          fail("unsupported operator: " + op);
        if (sqlite()) {
          if (op == "IS DISTINCT FROM") op = "IS NOT";
          else if (op == "IS NOT DISTINCT FROM") op = "IS";
        }
        if (!action.noOuter) {
          put("(");
          text(")");
        }
        const auto &left = expression(q, e.operands[0]);
        const auto &right = expression(q, e.operands[1]);
        const auto priority = options.compactExpressions ? portableBinaryPriority(e.value) : 0;
        const bool compactRight = priority && right.kind == ExprKind::Binary &&
                                  portableBinaryPriority(right.value) > priority;
        const bool compactLeft = priority && left.kind == ExprKind::Binary &&
                                 (portableBinaryPriority(left.value) > priority ||
                                  (priority != 3 && portableBinaryPriority(left.value) == priority));
        work.push_back({e.operands[1], {}, 0, 0, compactRight});
        text(" " + op + " ");
        work.push_back(
            {e.operands[0],
             {},
             0,
             0,
             compactLeft || (left.kind == ExprKind::Binary && left.value == e.value &&
                 (e.value == "+" || e.value == "-" || e.value == "*" ||
                  e.value == "/" || e.value == "%" || e.value == "AND" ||
                  e.value == "OR" || e.value == "CONCAT"))});
        break;
      }
      case ExprKind::AggregateFilter: {
        if (dialect != Dialect::PostgreSQL && dialect != Dialect::SQLite)
          fail("FILTER is unavailable in target dialect");
        if (e.operands.size() != 2 ||
            expression(q, e.operands[0]).kind != ExprKind::Call)
          fail("invalid aggregate FILTER model",
               DiagnosticCode::InvalidQueryShape);
        text(")");
        child(e.operands[1]);
        text(" FILTER (WHERE ");
        child(e.operands[0]);
        break;
      }
      case ExprKind::Window: {
        if (!e.window || e.operands.empty())
          fail("invalid window model", DiagnosticCode::InvalidQueryShape);
        if (expression(q, e.operands[0]).kind != ExprKind::Call &&
            expression(q, e.operands[0]).kind != ExprKind::AggregateFilter)
          fail("window requires a function", DiagnosticCode::InvalidQueryShape);
        auto spec=*e.window;
        std::vector<ExprId> keys(e.operands.begin()+1,e.operands.end());
        std::string reference;bool bare=false;
        if(allowNamedWindowReferences && pg() && program.sourceDialect==Dialect::PostgreSQL && e.windowSource &&
           sourceWindowMatches(q,spec,keys,*e.windowSource,q.namedWindows.size())) {
          spec=e.windowSource->specification;reference=e.windowSource->reference;bare=e.windowSource->bare;
          keys.erase(keys.begin(),keys.end()-e.windowSource->keys);
        }
        std::vector<Action> sequence{{e.operands.front(),{}}};
        if(bare) sequence.push_back({0," OVER "+quotedIdentifier(reference)});
        else {
          sequence.push_back({0," OVER ("});
          auto body=windowSequence(spec,keys,reference);
          sequence.insert(sequence.end(),body.begin(),body.end());
          sequence.push_back({0,")"});
        }
        for(auto it=sequence.rbegin();it!=sequence.rend();++it) work.push_back(std::move(*it));
        break;
      }
      case ExprKind::Call: {
        if (e.name.empty())
          fail("call has no name", DiagnosticCode::InvalidQueryShape);
        if (e.jsonConstructor) {
          const auto function=asciiUpper(e.name.back());
          const bool object=function=="JSON_OBJECT";
          if (!mssql() || e.name.size()!=1 || (!e.nameQuoted.empty() && e.nameQuoted[0]) ||
              (!object && function!="JSON_ARRAY") || e.distinct || e.withinGroup ||
              (object && e.operands.size()%2))
            fail("invalid JSON constructor shape or target",DiagnosticCode::InvalidQueryShape);
          const auto &syntax=*e.jsonConstructor;
          if (syntax.returningJson && e.operands.empty() && syntax.nullPolicy==JsonNullPolicy::Default)
            fail("empty JSON constructor with RETURNING requires an explicit null policy");
          std::string suffix;
          switch (syntax.nullPolicy) {
          case JsonNullPolicy::Default:break;
          case JsonNullPolicy::NullOnNull:suffix="NULL ON NULL";break;
          case JsonNullPolicy::AbsentOnNull:suffix="ABSENT ON NULL";break;
          default:fail("invalid JSON constructor null policy",DiagnosticCode::InvalidQueryShape);
          }
          if (syntax.returningJson) {
            if (!suffix.empty()) suffix+=" ";
            suffix+="RETURNING JSON";
          }
          put(function+"(");
          text(suffix+")");
          if (!suffix.empty() && !e.operands.empty()) text(" ");
          for (std::size_t i=e.operands.size();i>0;--i) {
            if (expression(q,e.operands[i-1]).kind==ExprKind::Star)
              fail("JSON constructor cannot emit wildcard arguments",DiagnosticCode::InvalidQueryShape);
            child(e.operands[i-1]);
            if (i>1) text(object && i%2==0?": ":", ");
          }
          break;
        }
        if (e.value == "KEYWORD") {
          static const std::unordered_set<std::string> keywords = {
              "CURRENT_TIMESTAMP", "CURRENT_DATE",   "CURRENT_TIME",
              "CURRENT_USER",      "SESSION_USER",   "SYSTEM_USER",
              "LOCALTIME",         "LOCALTIMESTAMP", "SYSDATE",
              "SYSTIMESTAMP",      "CURRENT_CATALOG", "CURRENT_ROLE",
              "CURRENT_SCHEMA",    "USER"};
          auto keyword = e.name[0];
          for (auto &ch : keyword)
            if (ch >= 'a' && ch <= 'z')
              ch -= 32;
          if (e.name.size() != 1 || !e.operands.empty() ||
              !keywords.count(keyword))
            fail("invalid SQL value keyword",
                 DiagnosticCode::InvalidQueryShape);
          if (!pg() && (keyword=="CURRENT_CATALOG" || keyword=="CURRENT_ROLE" ||
                        keyword=="CURRENT_SCHEMA" || (keyword=="USER" && !mssql())))
            fail("PostgreSQL session value keyword requires an explicit target mapping");
          put(keyword);
          break;
        }
        for (std::size_t i = 0; i < e.name.size(); ++i) {
          if (i)
            put(".");
          if (i < e.nameQuoted.size() && e.nameQuoted[i])
            identifier(e.name[i]);
          else {
            for (unsigned char ch : e.name[i])
              if (!((ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') ||
                    (ch >= '0' && ch <= '9') || ch == '_' || ch == '$' ||
                    ch >= 128))
                fail("invalid unquoted function identifier",
                     DiagnosticCode::InvalidQueryShape);
            put(e.name[i]);
          }
        }
        if (e.withinGroup) {
          if (!pg()) fail("target cannot preserve this ordered-set aggregate");
          const auto op=builtinOperator(dialect,e.name,e.nameQuoted,e.operands.size());
          if (e.distinct || !op || !op->requiresWithinGroup || e.operands.size()!=op->minArguments)
            fail("invalid ordered-set arguments",DiagnosticCode::InvalidQueryShape);
          const auto &order=*e.withinGroup;
          if (!order.nulls.empty() && order.nulls!="FIRST" && order.nulls!="LAST")
            fail("invalid ordered-set NULL ordering",DiagnosticCode::InvalidQueryShape);
          put("(");
          std::string suffix=order.descending?" DESC":"";
          if (!order.nulls.empty()) suffix+=" NULLS "+order.nulls;
          text(suffix+")");
          child(e.operands.back());
          text(") WITHIN GROUP (ORDER BY ");
          if (e.operands.size()==2) child(e.operands.front());
          break;
        }
        if (mysql() && e.name.size()==1 && (e.nameQuoted.empty() || !e.nameQuoted[0]) &&
            e.distinct && !builtinAcceptsDistinct(dialect,e.name[0]))
          fail("target aggregate does not accept DISTINCT");
        put("(");
        if (e.distinct)
          put("DISTINCT ");
        text(")");
        operands(e);
        break;
      }
      case ExprKind::Cast:
        if (e.operands.size() != 1)
          fail("invalid cast", DiagnosticCode::InvalidQueryShape);
        put("CAST(");
        text(")");
        work.push_back({0, {}, 0, action.id});
        text(" AS ");
        child(e.operands[0]);
        break;
      case ExprKind::Convert: {
        const bool cast=e.value=="TRY_CAST";
        if (!mssql() || !e.castType || (e.value!="CONVERT" && e.value!="TRY_CONVERT" && !cast) ||
            e.operands.empty() || e.operands.size()>(cast?1u:2u) || e.distinct || e.window)
          fail("invalid SQL Server conversion shape or target",DiagnosticCode::InvalidQueryShape);
        for (const auto id:e.operands)
          if (expression(q,id).kind==ExprKind::Star)
            fail("SQL Server conversion cannot emit wildcard arguments",DiagnosticCode::InvalidQueryShape);
        put(e.value+"(");
        text(")");
        if (cast) {
          work.push_back({0,{},0,action.id});
          text(" AS ");
          child(e.operands.front());
        } else {
          if (e.operands.size()==2) {
            child(e.operands.back());
            text(", ");
          }
          child(e.operands.front());
          text(", ");
          work.push_back({0,{},0,action.id});
        }
        break;
      }
      case ExprKind::Between:
        if (e.operands.size() != 3 ||
            (e.value != "BETWEEN" && e.value != "NOT BETWEEN" &&
             (!pg() || (e.value != "BETWEEN SYMMETRIC" && e.value != "NOT BETWEEN SYMMETRIC"))))
          fail("invalid BETWEEN", DiagnosticCode::InvalidQueryShape);
        put("(");
        text(")");
        child(e.operands[2]);
        text(" AND ");
        child(e.operands[1]);
        text(" " + e.value + " ");
        child(e.operands[0]);
        break;
      case ExprKind::IsNull:
        if (e.operands.size() != 1 ||
            (e.value != "IS NULL" && e.value != "IS NOT NULL"))
          fail("invalid IS NULL", DiagnosticCode::InvalidQueryShape);
        put("(");
        text(" " + e.value + ")");
        child(e.operands[0]);
        break;
      case ExprKind::TruthTest:
        if ((!pg() && !mysql()) || e.operands.size() != 1 ||
            (e.value != "IS TRUE" && e.value != "IS FALSE" && e.value != "IS UNKNOWN" &&
             e.value != "IS NOT TRUE" && e.value != "IS NOT FALSE" && e.value != "IS NOT UNKNOWN"))
          fail("invalid truth test for target dialect", DiagnosticCode::InvalidQueryShape);
        put("(");
        text(" " + e.value + ")");
        child(e.operands[0]);
        break;
      case ExprKind::In:
        if (e.operands.empty() || (e.value != "IN" && e.value != "NOT IN"))
          fail("invalid IN", DiagnosticCode::InvalidQueryShape);
        put("(");
        text("))");
        if (e.query)
          work.push_back({0, {}, e.query});
        else {
          if (e.operands.size() < 2 && !sqlite())
            fail("empty IN list", DiagnosticCode::InvalidQueryShape);
          for (std::size_t i = e.operands.size(); i > 1; --i) {
            child(e.operands[i - 1]);
            if (i > 2)
              text(", ");
          }
        }
        text(" " + e.value + " (");
        child(e.operands[0]);
        break;
      case ExprKind::Exists:
      case ExprKind::Subquery:
        if (e.kind == ExprKind::Exists)
          put("EXISTS ");
        put("(");
        expressionQuery(e.query);
        put(")");
        break;
      case ExprKind::Quantified: {
        if (sqlite() || (e.query?e.operands.size()!=1:!pg() || e.operands.size()!=2))
          fail("unsupported quantified comparison");
        const auto space = e.value.find(' ');
        if (space == std::string::npos ||
            !operatorAllowed(e.value.substr(0, space)))
          fail("invalid quantified comparison",
               DiagnosticCode::InvalidQueryShape);
        const auto quantifier = e.value.substr(space + 1);
        if (quantifier != "ANY" && quantifier != "ALL" && quantifier != "SOME")
          fail("invalid quantifier");
        put("(");
        text("))");
        if (e.query) work.push_back({0, {}, e.query});
        else child(e.operands[1]);
        text(" " + e.value + " (");
        child(e.operands[0]);
        break;
      }
      case ExprKind::Row:
        if (e.value=="ROW") {
          if (!pg()) fail("explicit PostgreSQL row constructor requires target row equality semantics");
          put("ROW");
        }
        put("(");
        text(")");
        operands(e);
        break;
      case ExprKind::ArrayConstructor:
        if (!pg()) fail("array constructors require a target array-type mapping");
        if (e.query) {
          put("ARRAY(");
          expressionQuery(e.query);
          put(")");
        } else {
          put(e.value=="[]"?"[":"ARRAY[");
          text("]");
          operands(e);
        }
        break;
      case ExprKind::Subscript: {
        if (!pg()) fail("subscripting requires a target array or document mapping");
        if (e.operands.empty() || e.value.empty())
          fail("invalid subscript expression",DiagnosticCode::InvalidQueryShape);
        std::vector<Action> sequence;
        const auto base=q.expressions.at(e.operands.front()).kind;
        const bool wrap=base!=ExprKind::Name && base!=ExprKind::Parameter && base!=ExprKind::Subquery;
        if (wrap) put("(");
        sequence.push_back({e.operands.front(),{}});
        if (wrap) sequence.push_back({0,")"});
        std::size_t part=1;
        const auto bound=[&] {
          if (part==e.operands.size())
            fail("missing subscript bound",DiagnosticCode::InvalidQueryShape);
          sequence.push_back({e.operands[part++],{}});
        };
        for (const auto syntax:e.value) {
          sequence.push_back({0,"["});
          switch (static_cast<SubscriptKind>(syntax)) {
          case SubscriptKind::Index: bound(); break;
          case SubscriptKind::Range: bound(); sequence.push_back({0,":"}); bound(); break;
          case SubscriptKind::Lower: bound(); sequence.push_back({0,":"}); break;
          case SubscriptKind::Upper: sequence.push_back({0,":"}); bound(); break;
          case SubscriptKind::Full: sequence.push_back({0,":"}); break;
          default: fail("invalid subscript dimension",DiagnosticCode::InvalidQueryShape);
          }
          sequence.push_back({0,"]"});
        }
        if (part!=e.operands.size())
          fail("extra subscript bound",DiagnosticCode::InvalidQueryShape);
        work.insert(work.end(),sequence.rbegin(),sequence.rend());
        break;
      }
      case ExprKind::Case: {
        const bool simple = e.value == "simple";
        const auto start = simple ? 1u : 0u;
        if ((!simple && e.value != "searched") ||
            e.operands.size() < start + 3 ||
            (e.operands.size() - start) % 2 != 1)
          fail("invalid CASE shape", DiagnosticCode::InvalidQueryShape);
        put("CASE ");
        std::vector<Action> sequence;
        if (simple) {
          sequence.push_back({e.operands[0], {}});
          sequence.push_back({0, " "});
        }
        for (std::size_t i = start; i + 1 < e.operands.size() - 1; i += 2) {
          sequence.push_back({0, "WHEN "});
          sequence.push_back({e.operands[i], {}});
          sequence.push_back({0, " THEN "});
          sequence.push_back({e.operands[i + 1], {}});
          sequence.push_back({0, " "});
        }
        if (!(pg() && program.sourceDialect==Dialect::PostgreSQL && e.implicitCaseElse &&
              expression(q,e.operands.back()).kind==ExprKind::Null)) {
          sequence.push_back({0, "ELSE "});
          sequence.push_back({e.operands.back(), {}});
        }
        sequence.push_back({0, " END"});
        for (auto i = sequence.rbegin(); i != sequence.rend(); ++i)
          work.push_back(*i);
        break;
      }
      }
    }
  }
  void integerLiteral(const ModelStatement &q, ExprId id) {
    const auto *e = &expression(q, id);
    if (e->kind == ExprKind::Unary && (e->value == "+" || e->value == "-") &&
        e->operands.size() == 1) {
      put(e->value);
      e = &expression(q, e->operands[0]);
    }
    if (e->kind != ExprKind::Number || e->value.empty() ||
        e->value.find_first_not_of("0123456789") != std::string::npos)
      fail("sequence/identity option requires a signed integer literal");
    put(e->value);
  }
  void tableName(const ModelTable &table) {
    if (table.inheritance != TableInheritance::Default && !pg())
      fail("explicit table inheritance requires PostgreSQL scan semantics");
    if (table.inheritance == TableInheritance::Only)
      put("ONLY ");
    name(table.name);
    if (table.inheritance == TableInheritance::IncludeDescendants)
      put(" *");
  }
  void transaction(const ModelStatement &q) {
    if (!q.transaction || program.sourceDialect != dialect)
      fail("transaction control requires the original dialect and session policy");
    const auto &control = *q.transaction;
    if (mysql() && control.chain==TransactionEndOption::Yes && control.release==TransactionEndOption::Yes)
      fail("AND CHAIN cannot be combined with RELEASE",DiagnosticCode::InvalidQueryShape);
    switch (control.action) {
    case TransactionAction::Begin: {
      if (oracle()) fail("Oracle does not have a standalone BEGIN transaction");
      put(mysql() ? "START TRANSACTION" : mssql() ? "BEGIN TRANSACTION" : "BEGIN");
      switch (control.mode) {
      case TransactionStartMode::Default: break;
      case TransactionStartMode::Deferred: put(" DEFERRED"); break;
      case TransactionStartMode::Immediate: put(" IMMEDIATE"); break;
      case TransactionStartMode::Exclusive: put(" EXCLUSIVE"); break;
      }
      if (control.name) { put(" "); identifier(*control.name); }
      std::vector<std::string> options;
      if (control.consistentSnapshot) options.push_back("WITH CONSISTENT SNAPSHOT");
      switch (control.isolation) {
      case TransactionIsolation::Default: break;
      case TransactionIsolation::ReadUncommitted: options.push_back("ISOLATION LEVEL READ UNCOMMITTED"); break;
      case TransactionIsolation::ReadCommitted: options.push_back("ISOLATION LEVEL READ COMMITTED"); break;
      case TransactionIsolation::RepeatableRead: options.push_back("ISOLATION LEVEL REPEATABLE READ"); break;
      case TransactionIsolation::Serializable: options.push_back("ISOLATION LEVEL SERIALIZABLE"); break;
      }
      if (control.access == TransactionAccess::ReadOnly) options.push_back("READ ONLY");
      if (control.access == TransactionAccess::ReadWrite) options.push_back("READ WRITE");
      if (control.deferrable) options.push_back(*control.deferrable ? "DEFERRABLE" : "NOT DEFERRABLE");
      if (!options.empty()) {
        put(" ");
        separated(options, [&](const auto &value) { put(value); });
      }
      break;
    }
    case TransactionAction::Savepoint:
    case TransactionAction::RollbackTo:
    case TransactionAction::NamedRollback:
    case TransactionAction::Release:
      if (!control.name) fail("transaction control requires a name", DiagnosticCode::InvalidQueryShape);
      if (control.action == TransactionAction::Savepoint) put(mssql() ? "SAVE TRANSACTION " : "SAVEPOINT ");
      else if (control.action == TransactionAction::RollbackTo) put("ROLLBACK TO SAVEPOINT ");
      else if (control.action == TransactionAction::NamedRollback) put("ROLLBACK TRANSACTION ");
      else put("RELEASE SAVEPOINT ");
      identifier(*control.name);
      break;
    case TransactionAction::Commit:
    case TransactionAction::Rollback:
      put(control.action == TransactionAction::Commit ? "COMMIT" : "ROLLBACK");
      if (control.name) { put(" TRANSACTION "); identifier(*control.name); }
      if (control.chain != TransactionEndOption::Default)
        put(control.chain == TransactionEndOption::Yes ? " AND CHAIN" : " AND NO CHAIN");
      if (control.release != TransactionEndOption::Default)
        put(control.release == TransactionEndOption::Yes ? " RELEASE" : " NO RELEASE");
      break;
    }
  }
  void prepared(const ModelStatement &q) {
    if (!q.prepared || !mysql() || program.sourceDialect != dialect)
      fail("prepared statement controls require the original MySQL-family dialect");
    const auto &command = *q.prepared;
    switch (command.action) {
    case PreparedAction::Prepare:
      put("PREPARE ");
      identifier(command.name);
      put(" FROM ");
      expr(q, command.source);
      break;
    case PreparedAction::Execute:
      put("EXECUTE ");
      identifier(command.name);
      if (!command.arguments.empty()) {
        put(" USING ");
        separated(command.arguments, [&](auto argument) { expr(q, argument); });
      }
      break;
    case PreparedAction::Deallocate:
      put("DEALLOCATE PREPARE ");
      identifier(command.name);
      break;
    }
  }
  void role(const ModelStatement &q) {
    if (!q.role || dialect != Dialect::MySQL || program.sourceDialect != dialect)
      fail("role controls require the original MySQL account semantics");
    const auto &command = *q.role;
    const auto account = [&](const RoleAccount &value) {
      literal(value.name);
      if (value.host) {
        put("@");
        literal(*value.host);
      }
    };
    const bool accountDefault = command.scope == RoleScope::AccountDefault;
    put(accountDefault ? "SET DEFAULT ROLE " : "SET ROLE ");
    switch (command.mode) {
    case RoleMode::Default: put("DEFAULT"); break;
    case RoleMode::None: put("NONE"); break;
    case RoleMode::All: put("ALL"); break;
    case RoleMode::AllExcept:
      put("ALL EXCEPT ");
      separated(command.roles, account);
      break;
    case RoleMode::Named:
      separated(command.roles, account);
      break;
    }
    if (accountDefault) {
      put(" TO ");
      separated(command.recipients, account);
    }
  }
  void table(const ModelStatement &q, std::size_t index) {
    if (index >= q.tables.size())
      fail("invalid table reference", DiagnosticCode::InvalidQueryShape);
    const auto &t = q.tables[index];
    if (t.pseudo)
      fail("pseudo-table cannot be a FROM source");
    if (t.pivot && pg() && program.sourceDialect==Dialect::PostgreSQL) {
      postgresPivot(q,t);
      return;
    }
    if (t.unpivot && pg() && program.sourceDialect==Dialect::PostgreSQL) {
      postgresUnpivot(q,t);
      return;
    }
    if (t.postgresFunction!=PostgresRowFunction::None) {
      if (!pg() || program.sourceDialect!=Dialect::PostgreSQL || !t.query || t.alias.empty())
        fail("set-returning function requires compatible PostgreSQL semantics");
      const auto &rows=query(t.query);
      if (rows.projections.size()!=1 || rows.projections.front().alias.empty())
        fail("set-returning function has an incomplete output shape",DiagnosticCode::InvalidQueryShape);
      const auto &projection=rows.projections.front();
      expr(rows,projection.expression);
      if (t.ordinalityColumn) put(" WITH ORDINALITY");
      const auto defaultAlias=t.postgresFunction==PostgresRowFunction::Unnest?"unnest":"generate_series";
      const bool omitAlias=t.sourceFunctionColumnAliases && t.inferredFunctionAlias && t.alias==defaultAlias;
      const auto effectiveColumn=omitAlias?std::string(defaultAlias):t.alias;
      auto aliases=t.sourceFunctionColumnAliases.value_or(t.ordinalityColumn?2:1);
      if((!aliases && projection.alias!=effectiveColumn) || (t.ordinalityColumn && *t.ordinalityColumn!="ordinality" && aliases<2))
        aliases=t.ordinalityColumn?2:1;
      if(aliases>2 || (aliases==2 && !t.ordinalityColumn))
        fail("invalid PostgreSQL function column alias count",DiagnosticCode::InvalidQueryShape);
      if(!omitAlias || aliases) {
        put(" AS ");identifier(t.alias);
      }
      if(aliases) {
        put("(");identifier(projection.alias);
        if (aliases==2) {put(", ");identifier(*t.ordinalityColumn);}
        put(")");
      }
      return;
    }
    if (t.jsonTable) {
      if (dialect != Dialect::MySQL || program.sourceDialect != dialect || t.alias.empty())
        fail("JSON_TABLE requires a native MySQL document and relation alias");
      put("JSON_TABLE(");
      expr(q, t.jsonTable->document);
      put(", ");
      literal(t.jsonTable->path);
      jsonTableColumns(t.jsonTable->columns, 0);
      put(")");
    } else if (t.name.empty()) {
      put("(");
      statement(t.query);
      put(")");
    } else
      tableName(t);
    if (t.rowMatch) rowMatch(*t.rowMatch);
    std::string inputAlias=t.alias;
    if (t.pivot) inputAlias=t.pivot->inputAlias.value_or(std::string{});
    if (t.unpivot) inputAlias=t.unpivot->inputAlias.value_or(std::string{});
    if (t.name.empty() && t.query && inputAlias.empty() && !sqlite() && !oracle() && !pg())
      fail("target requires an explicit derived table alias");
    if (!inputAlias.empty()) {
      put(oracle() ? " " : " AS ");
      identifier(inputAlias);
    }
    if (!t.columnAliases.empty()) {
      if (!allowsDerivedColumnAliases(dialect) || inputAlias.empty() || !t.query || !t.name.empty())
        fail("target does not support this derived table column alias list");
      if (dialect != program.sourceDialect && !pg() &&
          (!t.definition || !t.definition->arityKnown || t.definition->columns.size() != t.columnAliases.size()))
        fail("target requires one derived alias per column; source has a partial alias list");
      put(" (");
      separated(t.columnAliases, [&](const auto &name) { identifier(name); });
      put(")");
    }
    if (t.sampling) {
      if (!pg() || program.sourceDialect!=Dialect::PostgreSQL || t.query ||
          t.sampling->milliPercent>TableSampling::full)
        fail("TABLESAMPLE requires native PostgreSQL scan and sampling semantics");
      put(t.sampling->method == TableSamplingMethod::System ? " TABLESAMPLE SYSTEM (" : " TABLESAMPLE BERNOULLI (");
      put(tableSamplePercent(t.sampling->milliPercent));
      put(")");
      if (t.sampling->repeatable) {
        put(" REPEATABLE (");
        put(std::to_string(*t.sampling->repeatable));
        put(")");
      }
    }
    if (t.pivot) pivot(q, *t.pivot);
    if (t.unpivot) unpivot(q,*t.unpivot);
  }
  void rowMatch(const ModelRowMatch &match) {
    if (!oracle() || dialect!=program.sourceDialect ||
        (match.comparison!="=" && match.comparison!="<>" && match.comparison!="<" &&
         match.comparison!=">" && match.comparison!="<=" && match.comparison!=">=") ||
        (!match.nullOrdering.empty() && match.nullOrdering!="FIRST" && match.nullOrdering!="LAST"))
      fail("MATCH_RECOGNIZE requires native Oracle row pattern semantics");
    put(" MATCH_RECOGNIZE (");
    if (match.partition) {
      put("PARTITION BY ");
      identifier(*match.partition);
      put(" ");
    }
    put("ORDER BY ");
    identifier(match.order);
    put(match.descending?" DESC":" ASC");
    if (!match.nullOrdering.empty()) {
      put(" NULLS ");
      put(match.nullOrdering);
    }
    put(" MEASURES COUNT(*) AS ");
    identifier(match.measure);
    put(" ONE ROW PER MATCH AFTER MATCH SKIP PAST LAST ROW PATTERN (");
    identifier(match.variable);
    put("+) DEFINE ");
    identifier(match.variable);
    put(" AS ");
    identifier(match.variable);
    put(".");
    identifier(match.testColumn);
    put(" ");
    put(match.comparison);
    put(" ");
    put(std::to_string(match.threshold));
    put(")");
  }
  void postgresTransformSource(const ModelTable &table,std::string_view alias,std::size_t width) {
    put(" FROM (");
    if (table.name.empty()) statement(table.query);
    else {
      put("SELECT * FROM ");
      tableName(table);
    }
    put(") AS ");
    identifier(std::string(alias));
    put(" (");
    for (std::size_t i=0;i<width;++i) {
      if (i) put(", ");
      identifier("__nvd_col_"+std::to_string(i));
    }
    put(")");
  }
  void postgresPivot(const ModelStatement &query,const ModelTable &table) {
    const auto &shape=*table.pivot;
    if (table.sampling || !shape.relationAlias || !table.pivotInput || !table.definition ||
        !table.definition->complete || shape.axisOrdinals.empty() || shape.buckets.empty() ||
        table.definition->columns.size()!=shape.groupingOrdinals.size()+shape.buckets.size())
      fail("Calcite PIVOT emission needs resolved columns and an output alias");
    const auto aggregate=asciiUpper(shape.aggregate);
    if (aggregate!="COUNT" && aggregate!="SUM" && aggregate!="MIN" && aggregate!="MAX" && aggregate!="AVG")
      fail("unsupported PostgreSQL PIVOT aggregate");
    if (!shape.argumentOrdinal && aggregate!="COUNT")
      fail("PostgreSQL PIVOT aggregate is missing its input",DiagnosticCode::InvalidQueryShape);
    constexpr auto source="__nvd_pivot_input";
    const auto field=[&](std::size_t ordinal) {
      if (ordinal>=table.pivotInput->columns.size())
        fail("invalid PostgreSQL PIVOT input ordinal",DiagnosticCode::InvalidQueryShape);
      identifier(source);
      put(".");
      identifier("__nvd_col_"+std::to_string(ordinal));
    };
    put("(SELECT ");
    for (std::size_t i=0;i<shape.groupingOrdinals.size();++i) {
      field(shape.groupingOrdinals[i]);
      put(" AS ");
      identifier(table.definition->columns[i].name);
      put(", ");
    }
    for (std::size_t i=0;i<shape.buckets.size();++i) {
      const auto &bucket=shape.buckets[i];
      if (i) put(", ");
      put("pg_catalog.");
      put(aggregate);
      put("(");
      if (shape.argumentOrdinal) field(*shape.argumentOrdinal);
      else put("*");
      put(") FILTER (WHERE ");
      if (bucket.values.size()!=shape.axisOrdinals.size())
        fail("invalid PostgreSQL PIVOT bucket width",DiagnosticCode::InvalidQueryShape);
      for (std::size_t j=0;j<bucket.values.size();++j) {
        if (j) put(" AND ");
        field(shape.axisOrdinals[j]);
        put(" = ");
        expr(query,bucket.values[j]);
      }
      put(") AS ");
      identifier(table.definition->columns[shape.groupingOrdinals.size()+i].name);
    }
    postgresTransformSource(table,source,table.pivotInput->columns.size());
    put(" GROUP BY ");
    if (shape.groupingOrdinals.empty()) put("()");
    else separated(shape.groupingOrdinals,field);
    put(") AS ");
    identifier(*shape.relationAlias);
  }
  void postgresUnpivot(const ModelStatement &query,const ModelTable &table) {
    const auto &shape=*table.unpivot;
    if (!table.query || table.sampling || !shape.relationAlias || !table.unpivotInput ||
        !table.definition || !table.definition->complete || shape.measures.empty() ||
        shape.descriptors.empty() || shape.inputOrdinals.empty() || shape.inputOrdinals.size()!=shape.inputs.size())
      fail("Calcite UNPIVOT emission needs a derived input with resolved columns");
    const std::string source="__nvd_unpivot_input",values="__nvd_unpivot_values";
    const auto sourceColumn=[&](std::size_t ordinal) { return "__nvd_col_"+std::to_string(ordinal); };
    const auto field=[&](const std::string &alias,const std::string &column) {
      identifier(alias);
      put(".");
      identifier(column);
    };
    put("(SELECT ");
    for (auto ordinal:shape.retainedOrdinals) {
      field(source,sourceColumn(ordinal));
      put(" AS ");
      identifier(table.unpivotInput->columns.at(ordinal).name);
      put(", ");
    }
    for (const auto &label:shape.descriptors) {
      field(values,label);
      put(", ");
    }
    separated(shape.measures,[&](const auto &measure) { field(values,measure); });
    postgresTransformSource(table,source,table.unpivotInput->columns.size());
    put(" CROSS JOIN LATERAL (VALUES ");
    for (std::size_t i=0;i<shape.inputs.size();++i) {
      const auto &input=shape.inputs[i];
      if (shape.inputOrdinals[i].size()!=shape.measures.size() || input.columns.size()!=shape.measures.size() ||
          (!input.labels.empty() && input.labels.size()!=shape.descriptors.size()))
        fail("invalid Calcite UNPIVOT row mapping",DiagnosticCode::InvalidQueryShape);
      if (i) put(", ");
      put("(");
      const auto defaultLabel=input.labels.empty()?unpivotDefaultLabel(input.columns):std::string{};
      for (std::size_t j=0;j<shape.descriptors.size();++j) {
        put("CAST(");
        if (input.labels.empty()) literal(defaultLabel);
        else expr(query,input.labels[j]);
        put(" AS text), ");
      }
      separated(shape.inputOrdinals[i],[&](auto ordinal) { field(source,sourceColumn(ordinal)); });
      put(")");
    }
    put(") AS ");
    identifier(values);
    put(" (");
    for (const auto &label:shape.descriptors) {
      identifier(label);
      put(", ");
    }
    separated(shape.measures,[&](const auto &measure) { identifier(measure); });
    put(")");
    if (!shape.includeNulls.value_or(false)) {
      put(" WHERE ");
      for (std::size_t i=0;i<shape.measures.size();++i) {
        if (i) put(" OR ");
        field(values,shape.measures[i]);
        put(" IS NOT NULL");
      }
    }
    put(") AS ");
    identifier(*shape.relationAlias);
  }
  void unpivot(const ModelStatement &query,const ModelUnpivot &unpivot) {
    if (dialect!=program.sourceDialect || (!oracle() && !mssql()))
      fail("UNPIVOT requires an explicit target mapping");
    if (unpivot.inputs.empty() || unpivot.measures.empty() || unpivot.descriptors.empty() ||
        (mssql() && (!unpivot.relationAlias || unpivot.includeNulls ||
                     unpivot.measures.size()!=1 || unpivot.descriptors.size()!=1)))
      fail("invalid UNPIVOT model",DiagnosticCode::InvalidQueryShape);
    const auto names=[&](const std::vector<std::string> &columns) {
      if (columns.size()>1) put("(");
      separated(columns,[&](const auto &column) { identifier(column); });
      if (columns.size()>1) put(")");
    };
    put(" UNPIVOT");
    if (unpivot.includeNulls)
      put(*unpivot.includeNulls?" INCLUDE NULLS":" EXCLUDE NULLS");
    put(" (");
    names(unpivot.measures);
    put(" FOR ");
    names(unpivot.descriptors);
    put(" IN (");
    separated(unpivot.inputs,[&](const auto &input) {
      if (input.columns.size()!=unpivot.measures.size()) fail("invalid UNPIVOT input tuple");
      names(input.columns);
      if (!input.labels.empty()) {
        if (!oracle() || input.labels.size()!=unpivot.descriptors.size()) fail("invalid UNPIVOT label tuple");
        put(" AS ");
        if (input.labels.size()>1) put("(");
        separated(input.labels,[&](auto label) { expr(query,label); });
        if (input.labels.size()>1) put(")");
      }
    });
    put("))");
    if (unpivot.relationAlias) {
      put(oracle()?" ":" AS ");
      identifier(*unpivot.relationAlias);
    }
  }
  void pivot(const ModelStatement &q, const ModelPivot &pivot) {
    if (dialect != program.sourceDialect || (!oracle() && !mssql()))
      fail("PIVOT requires proven target grouping and output column semantics");
    const auto &fn = pivot.aggregate;
    if ((fn != "SUM" && fn != "AVG" && fn != "MIN" && fn != "MAX" && fn != "COUNT" &&
         !(mssql() && fn == "COUNT_BIG")) || pivot.axes.size()!=1 || pivot.buckets.empty() ||
        (pivot.argument.empty() && !(oracle() && fn == "COUNT")) ||
        (mssql() && (!pivot.relationAlias || pivot.aggregateAlias)))
      fail("invalid PIVOT model", DiagnosticCode::InvalidQueryShape);
    put(" PIVOT (");
    put(fn);
    put("(");
    if (pivot.argument.empty()) put("*");
    else name(pivot.argument);
    put(")");
    if (pivot.aggregateAlias) { put(" AS "); identifier(*pivot.aggregateAlias); }
    put(" FOR ");
    identifier(pivot.axes.front());
    put(" IN (");
    separated(pivot.buckets, [&](const auto &bucket) {
      if (mssql()) {
        if (!bucket.name || !bucket.values.empty()) fail("invalid SQL Server PIVOT output column");
        identifier(*bucket.name);
      } else {
        if (bucket.values.size()!=1) fail("Oracle PIVOT needs a static IN value");
        expr(q,bucket.values.front());
        if (bucket.name) { put(" AS "); identifier(*bucket.name); }
      }
    });
    put("))");
    if (pivot.relationAlias) {
      put(oracle() ? " " : " AS ");
      identifier(*pivot.relationAlias);
    }
  }
  void jsonTableColumns(const std::vector<BasicJsonTableColumn<std::string,std::vector<std::string>>> &columns,
                        std::size_t depth) {
    if (columns.empty() || depth >= options.maxNesting)
      fail("invalid or over-budget JSON_TABLE column structure", DiagnosticCode::ResourceLimit);
    auto response = [&](const JsonTableResponse &behavior, const char *event) {
      switch (behavior.kind) {
      case JsonTableResponseKind::Unspecified: return;
      case JsonTableResponseKind::Null: put(" NULL"); break;
      case JsonTableResponseKind::Error: put(" ERROR"); break;
      case JsonTableResponseKind::Default: put(" DEFAULT "); literal(behavior.json); break;
      }
      put(event);
    };
    put(" COLUMNS (");
    separated(columns, [&](const auto &column) {
      tick();
      if (column.kind == JsonTableColumnKind::Nested) {
        put("NESTED PATH ");
        literal(column.path);
        jsonTableColumns(column.children, depth + 1);
        return;
      }
      if (!column.name) fail("JSON_TABLE output column name is missing", DiagnosticCode::InvalidQueryShape);
      identifier(*column.name);
      if (column.kind == JsonTableColumnKind::Ordinality) {
        put(" FOR ORDINALITY");
        return;
      }
      put(" ");
      type(column.type);
      put(column.kind == JsonTableColumnKind::Exists ? " EXISTS PATH " : " PATH ");
      literal(column.path);
      response(column.onEmpty, " ON EMPTY");
      response(column.onError, " ON ERROR");
    });
    put(")");
  }
  void relation(const ModelStatement &q, RelationId root) {
    struct Action {
      RelationId relation = 0;
      ExprId expression = 0;
      std::string text;
      bool allowComma = true;
    };
    std::vector<Action> work{{root, 0, {}}};
    auto text = [&](std::string t) { work.push_back({0, 0, std::move(t)}); };
    while (!work.empty()) {
      tick();
      auto a = std::move(work.back());
      work.pop_back();
      if (a.expression) {
        expr(q, a.expression, true);
        continue;
      }
      if (!a.relation) {
        put(a.text);
        continue;
      }
      if (a.relation > q.relations.size())
        fail("invalid relation reference", DiagnosticCode::InvalidQueryShape);
      const auto &r = q.relations[a.relation - 1];
      if (!r.left) {
        table(q, r.table);
        continue;
      }
      if (!r.right)
        fail("join missing right operand", DiagnosticCode::InvalidQueryShape);
      const bool nativeComma=pg() && program.sourceDialect==Dialect::PostgreSQL && r.comma;
      if(nativeComma) {
        if(!a.allowComma || r.join!=JoinKind::Cross || r.condition || r.natural || !r.usingColumns.empty())
          fail("comma source list cannot be used as a joined-table operand",DiagnosticCode::InvalidQueryShape);
        work.push_back({r.right,0,{}});text(", ");work.push_back({r.left,0,{}});
        continue;
      }
      const bool sourceInner=pg() && program.sourceDialect==Dialect::PostgreSQL &&
          r.sourceJoinKind==JoinKind::Inner && r.join==JoinKind::Cross && r.lateral &&
          !r.implicitLateral && !r.condition && !r.natural && r.usingColumns.empty();
      std::string join;
      if (r.fixedOrder) {
        if (!mysql() || (r.lateral && !r.implicitLateral) || r.join != JoinKind::Inner || r.natural || !r.usingColumns.empty())
          fail("fixed join order requires a native STRAIGHT_JOIN");
        join = "STRAIGHT_JOIN";
      } else if (r.lateral && !r.implicitLateral) {
        if ((!mssql() && !oracle() && !pg()) || (r.join != JoinKind::Cross && r.join != JoinKind::Left) ||
            r.condition || r.natural || !r.usingColumns.empty())
          fail("invalid lateral relation");
        if (pg()) {
          const bool keyword=program.sourceDialect!=Dialect::PostgreSQL || r.sourceLateralKeyword.value_or(true);
          join = r.join == JoinKind::Left ? "LEFT JOIN" : sourceInner ? "INNER JOIN" : "CROSS JOIN";
          if(keyword) join+=" LATERAL";
        }
        else join = r.join == JoinKind::Left ? "OUTER APPLY" : "CROSS APPLY";
      } else switch (r.join) {
      case JoinKind::Inner:
        join = r.natural && dialect==Dialect::TiDB ? "JOIN" : "INNER JOIN";
        break;
      case JoinKind::Left:
        join = "LEFT JOIN";
        break;
      case JoinKind::Right:
        join = "RIGHT JOIN";
        break;
      case JoinKind::Full:
        join = "FULL JOIN";
        break;
      case JoinKind::Cross:
        join = "CROSS JOIN";
        break;
      default:
        fail("invalid JOIN kind", DiagnosticCode::InvalidQueryShape);
      }
      put("(");
      text(")");
      if (pg() && r.lateral && (r.join == JoinKind::Left || sourceInner)) {
        text(" ON TRUE");
      } else if (r.condition) {
        work.push_back({0, r.condition, {}});
        text(" ON ");
      } else if (!r.usingColumns.empty()) {

        text(")");
        for (std::size_t i = r.usingColumns.size(); i > 0; --i) {
          text(quotedIdentifier(r.usingColumns[i - 1]));
          if (i > 1)
            text(", ");
        }
        text(" USING (");
      }
      work.push_back({r.right, 0, {},false});
      text(" " + std::string(r.natural ? "NATURAL " : "") + join + " ");
      work.push_back({r.left, 0, {},false});
    }
  }
  void projections(const ModelStatement &q,
                   const std::vector<ModelProjection> &projections) {
    if (projections.empty())
      fail("empty projection list", DiagnosticCode::InvalidQueryShape);
    separated(projections, [&](const auto &p) {
      expr(q, p.expression, true);
      bool nativeDefault=false;
      if(pg() && program.sourceDialect==Dialect::PostgreSQL && p.implicitAlias) {
        auto field=p.expression;
        while(expression(q,field).kind==ExprKind::Subscript && !expression(q,field).operands.empty()) {
          tick();field=expression(q,field).operands.front();
        }
        const auto &base=expression(q,field);
        nativeDefault=(base.kind==ExprKind::ArrayConstructor && p.alias=="array") ||
            (field!=p.expression && base.kind==ExprKind::Name && !base.name.empty() && p.alias==base.name.back()) ||
            (field==p.expression && base.kind==ExprKind::Call && !base.name.empty() &&
             base.name.back()=="generate_series" && p.alias==base.name.back());
      }
      if (!p.alias.empty() && !nativeDefault) {
        put(" AS ");
        identifier(p.alias);
      }
    });
  }
  void assignments(const ModelStatement &q,
                   const std::vector<Assignment> &assignments) {
    if (assignments.empty())
      fail("empty SET assignments", DiagnosticCode::InvalidQueryShape);
    separated(assignments, [&](const auto &a) {
      if (a.targets.empty())
        fail("assignment has no target", DiagnosticCode::InvalidQueryShape);
      if (a.targets.size() > 1)
        put("(");
      separated(a.targets, [&](auto id) { expr(q, id); });
      if (a.targets.size() > 1)
        put(")");
      put(" = ");
      expr(q, a.value, true);
    });
  }
  void order(const ModelStatement &q) {
    if (q.orderBy.empty())
      return;
    put(" ORDER BY ");
    separated(q.orderBy, [&](const auto &o) {
      expr(q, o.expression, true);
      if (o.descending)
        put(" DESC");
      else if (o.explicitAscending && pg() && program.sourceDialect==Dialect::PostgreSQL)
        put(" ASC");
      if (!o.nulls.empty()) {
        if (o.nulls != "FIRST" && o.nulls != "LAST")
          fail("invalid NULL ordering");
        put(" NULLS ");
        put(o.nulls);
      }
    });
  }
  void where(const ModelStatement &q) {
    if (q.where) {
      put(" WHERE ");
      expr(q, q.where, true);
    }
  }
  void returning(const ModelStatement &q) {
    if (q.outputInto) {
      if (!mssql()) fail("target cannot preserve OUTPUT INTO");
      const auto &sink=*q.outputInto;
      put(" OUTPUT ");
      projections(q,sink.projections);
      put(" INTO ");
      if (!sink.tableVariable.empty()) {
        if (!sink.table.empty() || sink.tableVariable.size()<2 || sink.tableVariable.front()!='@')
          fail("invalid OUTPUT table variable",DiagnosticCode::InvalidQueryShape);
        parameter(sink.tableVariable);
      } else {
        if (sink.table.empty() || sink.table.size()>3)
          fail("invalid OUTPUT INTO table",DiagnosticCode::InvalidQueryShape);
        name(sink.table);
      }
      if (!sink.columns.empty()) {
        put(" (");
        separated(sink.columns,[&](const auto &column) { identifier(column); });
        put(")");
      }
    }
    if (q.returning.empty())
      return;
    put(mssql() ? " OUTPUT " : " RETURNING ");
    projections(q, q.returning);
    if (oracle()) {
      if (q.returningInto.empty())
        fail("Oracle RETURNING needs INTO bindings");
      put(" INTO ");
      separated(q.returningInto, [&](auto e) { expr(q, e); });
    }
  }
  void target(const ModelStatement &q, bool withAlias = true) {
    const auto &t = q.targetReference ? q.targetReference : q.target;
    if (!t)
      fail("statement has no target", DiagnosticCode::InvalidQueryShape);
    tableName(*t);
    if (withAlias && !t->alias.empty()) {
      put(oracle() ? " " : " AS ");
      identifier(t->alias);
    }
  }
  void top(const ModelStatement &q) {
    if (mssql() && q.limit && !q.offset) {
      put("TOP (");
      expr(q, q.limit);
      put(q.limitWithTies ? ") WITH TIES " : ") ");
    }
  }
  int setPriority(SetKind kind) const {
    return kind == SetKind::Intersect && !oracle() && !sqlite() ? 20 : 10;
  }
  void setQuery(const ModelStatement &q) {
    const auto &set = *q.setOperation;
    if (set.all && set.kind != SetKind::Union &&
        (oracle() || mssql() || sqlite()))
      fail("target baseline does not support ALL for this set operator");
    auto operand = [&](QueryId id, bool right) {
      const auto &child = query(id);
      bool grouped = !child.ctes.empty() || !child.orderBy.empty() ||
                     child.offset || (child.limit && !mssql());
      if (child.setOperation) {
        const auto priority = setPriority(child.setOperation->kind),
                   parent = setPriority(set.kind);
        grouped |= right ? priority <= parent : priority < parent;
      }
      if (grouped && sqlite())
        fail("grouped set operand needs an explicit SQLite derived-query "
             "wrapper");
      if (grouped)
        put("(");
      statement(id);
      if (grouped)
        put(")");
    };
    operand(set.left, false);
    put(set.kind == SetKind::Union       ? " UNION "
        : set.kind == SetKind::Intersect ? " INTERSECT "
        : oracle()                       ? " MINUS "
                                         : " EXCEPT ");
    if (set.all)
      put("ALL ");
    operand(set.right, true);
    order(q);
    if (mssql() && q.limit && !q.offset)
      fail("SQL Server set LIMIT/TOP requires an explicit derived-query "
           "wrapper");
    pagination(q);
  }
  void select(const ModelStatement &q) {
    if (q.setOperation) {
      setQuery(q);
      return;
    }
    if (q.valuesQuery) {
      if (q.rows.empty() || (q.sqlServerValueConstructor && (!mssql() || q.limit || q.offset || !q.orderBy.empty())) ||
          (!q.sqlServerValueConstructor && dialect != Dialect::MySQL && !pg() && !sqlite()))
        fail("invalid VALUES query or dialect");
      put("VALUES ");
      separated(q.rows, [&](const auto &row) {
        if (dialect == Dialect::MySQL) put("ROW");
        put("(");
        separated(row, [&](auto cell) { expr(q, cell); });
        put(")");
      });
      order(q);
      pagination(q);
      return;
    }
    if (q.tableQuery != TableQuerySyntax::None) {
      if (q.tables.size() != 1 || q.tables.front().name.empty() ||
          (dialect != Dialect::MySQL && !pg()) ||
          (!pg() && q.tableQuery != TableQuerySyntax::Table))
        fail("invalid TABLE-query relation or dialect");
      put(q.tableQuery == TableQuerySyntax::Only ? "TABLE ONLY " : "TABLE ");
      name(q.tables.front().name);
      if (q.tableQuery == TableQuerySyntax::IncludeDescendants) put(" *");
      order(q);
      pagination(q);
      return;
    }
    put("SELECT ");
    if (q.distinct)
      put("DISTINCT ");
    if (q.fixedJoinOrder) {
      if (!mysql()) fail("SELECT STRAIGHT_JOIN is unavailable in the target dialect");
      put("STRAIGHT_JOIN ");
    }
    top(q);
    if (!q.projections.empty() || !pg() || q.distinct)
      projections(q, q.projections);
    if (q.selectInto) {
      const auto &assignment = *q.selectInto;
      put(pg() && assignment.mode == SelectIntoMode::ExactlyOne ? " INTO STRICT " : " INTO ");
      separated(assignment.targets, [&](auto id) {
        if (!id || id >= options.localNames.size() || options.localNames[id].name.empty())
          fail("unbound SELECT INTO target in routine generation", DiagnosticCode::InvalidQueryShape);
        identifier(options.localNames[id].name);
      });
    }
    if (q.from) {
      put(" FROM ");
      relation(q, q.from);
    }
    where(q);
    if(!q.groupingSets.empty()) {
      if(q.groupingSets.size()>4096)fail("grouping set generation limit exceeded",DiagnosticCode::ResourceLimit);
      if(sqlite())fail("SQLite cannot represent grouping sets");
      if(mysql()) {
        bool rollup=q.groupingSets.size()==q.groupBy.size()+1 && !q.groupBy.empty();
        if(rollup)for(std::size_t i=0;i<q.groupingSets.size();++i)
          rollup &= q.groupingSets[i].size()==q.groupBy.size()-i && std::equal(q.groupingSets[i].begin(),q.groupingSets[i].end(),q.groupBy.begin());
        if(!rollup)fail("target MySQL-family syntax cannot represent these grouping sets without a relational rewrite");
        put(" GROUP BY ");separated(q.groupBy,[&](auto e){expr(q,e);});put(" WITH ROLLUP");
      } else {
        put(" GROUP BY GROUPING SETS (");separated(q.groupingSets,[&](const auto &set){tick();put("(");separated(set,[&](auto e){expr(q,e);});put(")");});put(")");
      }
    } else if (!q.groupBy.empty()) {
      put(" GROUP BY ");
      separated(q.groupBy, [&](auto e) { expr(q, e); });
    }
    if (q.having) {
      put(" HAVING ");
      expr(q, q.having, true);
    }
    namedWindowClause(q);
    order(q);
    pagination(q);
  }
  void pagination(const ModelStatement &q) {
    if (q.limitWithTies && (pg() || oracle())) {
      if (q.offset) {
        put(" OFFSET ");
        expr(q, q.offset);
        put(" ROWS");
      }
      put(" FETCH FIRST ");
      expr(q, q.limit);
      put(" ROWS WITH TIES");
      return;
    }
    if (mysql() || pg() || sqlite()) {
      if (q.limit) {
        put(" LIMIT ");
        expr(q, q.limit);
      } else if (q.offset && sqlite())
        put(" LIMIT -1");
      if (q.offset) {
        put(" OFFSET ");
        expr(q, q.offset);
      }
    } else if (oracle() || mssql()) {
      if (q.offset) {
        put(" OFFSET ");
        expr(q, q.offset);
        put(" ROWS");
      }
      if (q.limit && (oracle() || q.offset)) {
        put(" FETCH NEXT ");
        expr(q, q.limit);
        put(" ROWS ONLY");
      }
    }
  }
  void insert(const ModelStatement &q) {
    if (q.replaceExisting && (!mysql() || q.conflict.action != ConflictClause::Action::None))
      fail("REPLACE requires native delete/insert and unique-key semantics");
    put(q.replaceExisting ? "REPLACE " : "INSERT ");
    for (const auto &modifier : q.modifiers) {
      static const std::unordered_set<std::string> allowed = {
          "IGNORE",     "LOW_PRIORITY", "HIGH_PRIORITY", "OR IGNORE",
          "OR REPLACE", "OR ABORT",     "OR ROLLBACK",   "OR FAIL"};
      if (!allowed.count(modifier))
        fail("unsupported INSERT modifier");
      put(modifier);
      put(" ");
    }
    top(q);
    put("INTO ");
    target(q);
    if (!q.columns.empty()) {
      put(" (");
      separated(q.columns, [&](const auto &col) { identifier(col); });
      put(")");
    }
    if (mssql())
      returning(q);
    switch (q.insertSource) {
    case InsertSource::Values:
      put(" VALUES ");
      if (q.rows.empty())
        fail("INSERT has no rows", DiagnosticCode::InvalidQueryShape);
      separated(q.rows, [&](const auto &row) {
        put("(");
        separated(row, [&](auto e) { expr(q, e); });
        put(")");
      });
      break;
    case InsertSource::Query:
      put(" ");
      statement(q.source);
      break;
    case InsertSource::DefaultValues:
      put(" DEFAULT VALUES");
      break;
    case InsertSource::Assignments:
      put(" SET ");
      assignments(q, q.assignments);
      break;
    default:
      fail("INSERT source missing", DiagnosticCode::InvalidQueryShape);
    }
    const auto &conflict = q.conflict;
    if (!conflict.rowAlias.empty()) {
      if (dialect != Dialect::MySQL || q.replaceExisting ||
          (q.insertSource != InsertSource::Values && q.insertSource != InsertSource::Assignments))
        fail("incoming row aliases require native MySQL VALUES or SET");
      put(" AS ");
      identifier(conflict.rowAlias);
      if (!conflict.columnAliases.empty()) {
        put("(");
        separated(conflict.columnAliases, [&](const auto &col) { identifier(col); });
        put(")");
      }
    }
    if (conflict.action == ConflictClause::Action::DuplicateKey) {
      put(" ON DUPLICATE KEY UPDATE ");
      assignments(q, conflict.assignments);
    } else if (conflict.action != ConflictClause::Action::None) {
      put(" ON CONFLICT");
      if (!conflict.targets.empty()) {
        put(" (");
        separated(conflict.targets, [&](auto e) { expr(q, e); });
        put(")");
      } else if (!conflict.constraint.empty()) {
        put(" ON CONSTRAINT ");
        identifier(conflict.constraint);
      }
      if (conflict.targetWhere) {
        put(" WHERE ");
        expr(q, conflict.targetWhere);
      }
      if (conflict.action == ConflictClause::Action::Nothing)
        put(" DO NOTHING");
      else {
        put(" DO UPDATE SET ");
        assignments(q, conflict.assignments);
        if (conflict.where) {
          put(" WHERE ");
          expr(q, conflict.where);
        }
      }
    }
    if (!mssql())
      returning(q);
  }
  void update(const ModelStatement &q) {
    put("UPDATE ");
    for (const auto &modifier : q.modifiers) {
      if (modifier != "LOW_PRIORITY" && modifier != "IGNORE" &&
          modifier.rfind("OR ", 0) != 0)
        fail("unsupported UPDATE modifier");
      put(modifier);
      put(" ");
    }
    top(q);
    if (mysql())
      relation(q, q.sourceFrom);
    else
      target(q, !mssql());
    put(" SET ");
    assignments(q, q.assignments);
    if (mssql())
      returning(q);
    if (!mysql() && q.sourceFrom) {
      put(" FROM ");
      relation(q, q.sourceFrom);
    }
    where(q);
    if (mysql()) {
      order(q);
      if (q.limit) {
        put(" LIMIT ");
        expr(q, q.limit);
      }
    }
    if (!mssql())
      returning(q);
  }
  void deleteStatement(const ModelStatement &q) {
    const auto &deletionTarget=q.targetReference?q.targetReference:q.target;
    if (dialect==Dialect::MariaDB && q.deleteTargets.empty() && deletionTarget &&
        !deletionTarget->alias.empty())
      fail("single-table DELETE aliases require a newer MariaDB baseline");
    if (!mysql() && !q.modifiers.empty()) fail("DELETE modifiers require a MySQL-family target");
    put("DELETE ");
    for (const auto &modifier : q.modifiers) {
      if (modifier != "LOW_PRIORITY" && modifier != "QUICK" &&
          modifier != "IGNORE")
        fail("unsupported DELETE modifier");
    }
    const std::vector<std::string_view> modifierOrder=dialect==Dialect::OceanBase?
        std::vector<std::string_view>{"IGNORE","LOW_PRIORITY","QUICK"}:
        std::vector<std::string_view>{"LOW_PRIORITY","QUICK","IGNORE"};
    for (const auto modifier:modifierOrder)
      if (std::find(q.modifiers.begin(),q.modifiers.end(),modifier)!=q.modifiers.end()) {
        put(modifier);
        put(" ");
      }
    top(q);
    if (mysql() && !q.deleteTargets.empty()) {
      separated(q.deleteTargets, [&](const auto &n) { name(n); });
      put(" FROM ");
      relation(q, q.sourceFrom);
    } else if (mssql() && q.sourceFrom) {
      target(q, false);
      returning(q);
      put(" FROM ");
      relation(q, q.sourceFrom);
    } else {
      put("FROM ");
      target(q);
      if (mssql())
        returning(q);
      if (q.sourceFrom) {
        put(" USING ");
        relation(q, q.sourceFrom);
      }
    }
    where(q);
    if (mysql()) {
      order(q);
      if (q.limit) {
        put(" LIMIT ");
        expr(q, q.limit);
      }
    }
    if (!mssql())
      returning(q);
  }
  void merge(const ModelStatement &q) {
    if ((!mssql() && (!q.returning.empty() || q.outputInto)) || !q.returningInto.empty())
      fail("MERGE output clauses are not covered");
    if (mysql() || sqlite())
      fail("MERGE is unavailable in target dialect");
    if (!q.mergeOn || !q.sourceFrom || q.mergeClauses.empty())
      fail("incomplete MERGE model", DiagnosticCode::InvalidQueryShape);
    put("MERGE ");
    top(q);
    put("INTO ");
    target(q);
    put(" USING ");
    relation(q, q.sourceFrom);
    put(" ON ");
    if (oracle())
      put("(");
    expr(q, q.mergeOn);
    if (oracle())
      put(")");
    for (const auto &clause : q.mergeClauses) {
      put(" WHEN ");
      if (clause.match == MergeMatch::Matched)
        put("MATCHED");
      else if (clause.match == MergeMatch::NotMatchedTarget)
        put("NOT MATCHED");
      else {
        if (!mssql())
          fail("NOT MATCHED BY SOURCE is unavailable in target baseline");
        put("NOT MATCHED BY SOURCE");
      }
      if (!oracle() && clause.condition) {
        put(" AND ");
        expr(q, clause.condition);
      }
      put(" THEN ");
      switch (clause.action) {
      case MergeAction::Update:
        put("UPDATE SET ");
        assignments(q, clause.assignments);
        break;
      case MergeAction::Delete:
        if (oracle())
          fail("Oracle MERGE DELETE must follow UPDATE");
        put("DELETE");
        break;
      case MergeAction::Nothing:
        if (dialect != Dialect::PostgreSQL)
          fail("DO NOTHING is PostgreSQL-specific");
        put("DO NOTHING");
        break;
      case MergeAction::Insert:
        put("INSERT");
        if (!clause.columns.empty()) {
          put(" (");
          separated(clause.columns,
                    [&](const auto &column) { identifier(column); });
          put(")");
        }
        if (clause.defaultValues) {
          if (oracle())
            fail("Oracle MERGE has no DEFAULT VALUES branch");
          put(" DEFAULT VALUES");
        } else {
          put(" VALUES (");
          separated(clause.values, [&](auto id) { expr(q, id); });
          put(")");
        }
        break;
      }
      if (oracle() && clause.condition) {
        put(" WHERE ");
        expr(q, clause.condition);
      }
      if (clause.deleteWhere) {
        if (!oracle() || clause.action != MergeAction::Update)
          fail("post-update DELETE predicate is Oracle-specific");
        put(" DELETE WHERE ");
        expr(q, clause.deleteWhere);
      }
    }
    if (mssql()) returning(q);
  }
  void ddl(const ModelStatement &);

public:
  Printer(const ModelProgram &p,Dialect d,GenerationOptions opts,ExpressionColumnResolver resolver={},
          ExpressionQueryResolver queries={})
      :program(p),dialect(d),options(std::move(opts)),columns(std::move(resolver)),subqueries(std::move(queries)) {}
  void statement(QueryId id) {
    tick();
    if (active.size() >= options.maxNesting ||
        std::find(active.begin(), active.end(), id) != active.end())
      fail("cyclic or excessive query nesting", DiagnosticCode::ResourceLimit);
    const auto &q = query(id);
    const auto oldOrigin = origin;
    origin = q.origin;
    if (q.prewhere || q.qualify || q.systemTime || q.limitBy || q.groupByAll ||
        std::any_of(q.tables.begin(), q.tables.end(), [](const auto &table) { return table.finalRead || table.clickHouseSampling; }))
      fail("target has no proven mapping for the source query modifier");
    active.push_back(id);
    if (q.selectInto && (q.kind != StatementKind::Select || q.setOperation || q.valuesQuery ||
        q.tableQuery != TableQuerySyntax::None || q.selectInto->targets.empty() ||
        q.selectInto->targets.size() != q.projections.size() ||
        (pg() && q.selectInto->mode == SelectIntoMode::SingleRowOrUnchanged) ||
        (oracle() && q.selectInto->mode != SelectIntoMode::ExactlyOne) ||
        (mysql() && q.selectInto->mode != SelectIntoMode::SingleRowOrUnchanged) ||
        (!pg() && !oracle() && !mysql())))
      fail("invalid SELECT INTO assignment for target dialect", DiagnosticCode::InvalidQueryShape);
    if (q.limitWithTies && (q.kind != StatementKind::Select || !q.limit ||
        (q.orderBy.empty() && !oracle()) || (!pg() && !oracle() &&
         (!mssql() || q.offset || q.setOperation))))
      fail("target cannot express this WITH TIES row limit");
    if (!q.ctes.empty()) {
      put(q.recursiveWith ? "WITH RECURSIVE " : "WITH ");
      separated(q.ctes, [&](const auto &cte) {
        if (query(cte.query).kind!=StatementKind::Select &&
            (!pg() || program.sourceDialect!=Dialect::PostgreSQL))
          fail("target cannot represent a PostgreSQL data-modifying CTE");
        identifier(cte.name);
        if (!cte.columns.empty()) {
          put(" (");
          separated(cte.columns, [&](const auto &n) { identifier(n); });
          put(")");
        }
        put(" AS ");
        if (cte.materialization != CteMaterialization::Default) {
          if (program.sourceDialect != dialect || (!pg() && dialect != Dialect::SQLite))
            fail("target cannot preserve this CTE materialization directive");
          switch (cte.materialization) {
          case CteMaterialization::Materialized: put("MATERIALIZED "); break;
          case CteMaterialization::NotMaterialized: put("NOT MATERIALIZED "); break;
          default: fail("invalid CTE materialization directive", DiagnosticCode::InvalidQueryShape);
          }
        }
        put("(");
        statement(cte.query);
        put(")");
      });
      put(" ");
    }
    switch (q.kind) {
    case StatementKind::Transaction:
      transaction(q);
      break;
    case StatementKind::Prepared:
      prepared(q);
      break;
    case StatementKind::Role:
      role(q);
      break;
    case StatementKind::Copy: {
      if (!pg() || program.sourceDialect != dialect || !q.copy)
        fail("COPY requires PostgreSQL client stream semantics");
      const auto &copy = *q.copy;
      const bool from = copy.direction == CopyDirection::From;
      if ((copy.direction != CopyDirection::From && copy.direction != CopyDirection::To) ||
          bool(q.source) == bool(q.target) || (q.source && (from || !copy.columns.empty())) ||
          (!from && q.where) || (copy.forceQuoteAll && !copy.forceQuote.empty()))
        fail("invalid PostgreSQL COPY transfer shape", DiagnosticCode::InvalidQueryShape);
      if (const auto error = copyOptionError(copy)) fail(*error, DiagnosticCode::InvalidQueryShape);
      const auto names = [&](const auto &columns) {
        put("(");
        separated(columns, [&](const auto &column) { identifier(column); });
        put(")");
      };
      put("COPY ");
      if (q.source) {
        put("(");
        statement(q.source);
        put(")");
      } else {
        name(q.target->name);
        if (!copy.columns.empty()) { put(" "); names(copy.columns); }
      }
      put(from ? " FROM STDIN" : " TO STDOUT");
      bool first = true;
      const auto option = [&](std::string_view label) {
        put(first ? " WITH (" : ", ");
        first = false;
        put(label);
        put(" ");
      };
      if (copy.format) {
        if (!*copyFormatName(*copy.format)) fail("invalid COPY format", DiagnosticCode::InvalidQueryShape);
        option("FORMAT"); put(copyFormatName(*copy.format));
      }
      if (copy.header) {
        if (!*copyHeaderName(*copy.header)) fail("invalid COPY header mode", DiagnosticCode::InvalidQueryShape);
        option("HEADER"); put(copyHeaderName(*copy.header));
      }
      if (copy.freeze) { option("FREEZE"); put(*copy.freeze ? "TRUE" : "FALSE"); }
      const auto stringOption = [&](std::string_view label, const std::optional<std::string> &value) {
        if (value) { option(label); literal(*value); }
      };
      stringOption("DELIMITER", copy.delimiter);
      stringOption("NULL", copy.nullMarker);
      stringOption("QUOTE", copy.quote);
      stringOption("ESCAPE", copy.escape);
      stringOption("ENCODING", copy.encoding);
      if (copy.forceQuoteAll) { option("FORCE_QUOTE"); put("*"); }
      else if (!copy.forceQuote.empty()) { option("FORCE_QUOTE"); names(copy.forceQuote); }
      if (!copy.forceNotNull.empty()) { option("FORCE_NOT_NULL"); names(copy.forceNotNull); }
      if (!copy.forceNull.empty()) { option("FORCE_NULL"); names(copy.forceNull); }
      if (!first) put(")");
      if (q.where) { put(" WHERE "); expr(q, q.where); }
      break;
    }
    case StatementKind::Trigger: {
      if ((!sqlite() && !mysql()) || program.sourceDialect != dialect || !q.trigger)
        fail("trigger generation requires the original MySQL-family or SQLite dialect");
      const auto &trigger = *q.trigger;
      if (trigger.name.empty() || trigger.name.size() > 2)
        fail("invalid trigger name", DiagnosticCode::InvalidQueryShape);
      if (trigger.drop) {
        put("DROP TRIGGER ");
        if (trigger.ifGuard) put("IF EXISTS ");
        name(trigger.name);
        break;
      }
      const auto *timing = triggerTimingName(trigger.timing);
      const auto *event = triggerEventName(trigger.event);
      if (!*event || (!*timing && trigger.timing != TriggerTiming::Default) || trigger.target.empty() ||
          trigger.target.size() > 2 || trigger.body.empty() ||
          (!trigger.updateColumns.empty() && trigger.event != TriggerEvent::Update) ||
          (sqlite() && (!trigger.compound || trigger.definer || trigger.relativeTrigger)) ||
          (mysql() && (!trigger.explicitRow || trigger.temporary || trigger.when ||
             !trigger.updateColumns.empty() || (trigger.timing!=TriggerTiming::Before &&
             trigger.timing!=TriggerTiming::After))) ||
          (trigger.order!=TriggerOrder::None)!=trigger.relativeTrigger.has_value() ||
          (!trigger.compound && trigger.body.size()!=1))
        fail("invalid trigger header or empty body", DiagnosticCode::InvalidQueryShape);
      put(trigger.temporary ? "CREATE TEMP " : "CREATE ");
      if(trigger.definer) {
        put("DEFINER = ");
        if(trigger.definer->currentUser)put("CURRENT_USER");
        else {literal(trigger.definer->user);put("@");literal(trigger.definer->host);}
        put(" ");
      }
      put("TRIGGER ");
      if (trigger.ifGuard) put("IF NOT EXISTS ");
      name(trigger.name);
      if (*timing) { put(" "); put(timing); }
      put(" ");
      put(event);
      if (!trigger.updateColumns.empty()) {
        put(" OF ");
        separated(trigger.updateColumns, [&](const auto &column) { identifier(column); });
      }
      put(" ON ");
      name(trigger.target);
      if (trigger.explicitRow) put(" FOR EACH ROW");
      if(trigger.relativeTrigger) {
        if(trigger.order==TriggerOrder::Follows)put(" FOLLOWS ");
        else if(trigger.order==TriggerOrder::Precedes)put(" PRECEDES ");
        else fail("invalid trigger ordering",DiagnosticCode::InvalidQueryShape);
        identifier(*trigger.relativeTrigger);
      }
      if (trigger.when) { put(" WHEN "); expr(q, trigger.when); }
      put(trigger.compound?" BEGIN ":" ");
      for (const auto step : trigger.body) {
        tick();
        if (!step || step > program.queries.size())
          fail("invalid trigger step reference", DiagnosticCode::InvalidQueryShape);
        const auto &body = program.queries.at(step - 1);
        const bool modification = body.kind == StatementKind::Insert || body.kind == StatementKind::Update ||
                                  body.kind == StatementKind::Delete;
        if (!modification && body.kind != (sqlite()?StatementKind::Select:StatementKind::TriggerSet))
          fail("invalid trigger step kind",DiagnosticCode::InvalidQueryShape);
        if(sqlite() && (!body.ctes.empty() || !body.returning.empty() ||
           body.insertSource == InsertSource::DefaultValues ||
           (modification && (!body.target || body.target->name.size()!=1 || !body.orderBy.empty() ||
                             body.limit || body.offset))))
          fail("invalid SQLite trigger body shape",DiagnosticCode::InvalidQueryShape);
        statement(step);
        if(trigger.compound)put("; ");
      }
      if(trigger.compound)put("END");
      break;
    }
    case StatementKind::TriggerSet: {
      if(!mysql() || !q.scope.parent || !query(*q.scope.parent).trigger)
        fail("trigger SET requires a MySQL-family trigger scope",DiagnosticCode::InvalidQueryShape);
      put("SET ");
      assignments(q,q.assignments);
      break;
    }
    case StatementKind::Package: {
      using Kind=PackageDeclarationKind;
      if(!oracle() || program.sourceDialect!=dialect || !q.package)
        fail("Oracle packages require the original dialect");
      const auto &package=*q.package;
      if(package.name.empty() || package.name.size()>2)
        fail("invalid package name",DiagnosticCode::InvalidQueryShape);
      if(package.drop) {
        put("DROP PACKAGE ");name(package.name);
        break;
      }
      put(package.replace?"CREATE OR REPLACE ":"CREATE ");
      if(package.edition==PackageEdition::Editionable)put("EDITIONABLE ");
      if(package.edition==PackageEdition::NonEditionable)put("NONEDITIONABLE ");
      put("PACKAGE ");name(package.name);
      if(package.auth==PackageAuth::CurrentUser)put(" AUTHID CURRENT_USER");
      if(package.auth==PackageAuth::Definer)put(" AUTHID DEFINER");
      put(" AS ");
      for(const auto &member:package.declarations) {
        tick();
        const bool routine=member.kind==Kind::Procedure || member.kind==Kind::Function;
        if(member.kind==Kind::Procedure)put("PROCEDURE ");
        if(member.kind==Kind::Function)put("FUNCTION ");
        identifier(member.name);
        if(routine) {
          if(!member.parameters.empty()) {
            put("(");
            separated(member.parameters,[&](const auto &parameter) {
              identifier(parameter.name);
              if(parameter.mode==ParameterMode::In)put(" IN ");
              if(parameter.mode==ParameterMode::InOut)put(" IN OUT ");
              if(parameter.mode==ParameterMode::Out)put(" OUT ");
              if(parameter.noCopy)put("NOCOPY ");
              type(parameter.type);
              if(parameter.defaultValue) {put(" DEFAULT ");expr(q,parameter.defaultValue);}
            });
            put(")");
          }
          if(member.kind==Kind::Function) {
            put(" RETURN ");type(member.type);
            if(member.deterministic)put(" DETERMINISTIC");
          }
        } else if(member.kind==Kind::Exception) {
          put(" EXCEPTION");
        } else {
          put(member.kind==Kind::Constant?" CONSTANT ":" ");
          type(member.type);
          if(member.initializer) {put(" := ");expr(q,member.initializer);}
        }
        put("; ");
      }
      put("END");
      if(package.endName) {put(" ");identifier(*package.endName);}
      break;
    }
    case StatementKind::Vacuum: {
      if (!pg() || program.sourceDialect != dialect || !q.vacuum)
        fail("VACUUM requires the original PostgreSQL dialect");
      put("VACUUM");
      if (!q.vacuum->options.empty()) {
        put(" (");
        separated(q.vacuum->options, [&](const auto &option) {
          const auto *spelling = vacuumOptionName(option.kind);
          if (!*spelling) fail("invalid VACUUM option", DiagnosticCode::InvalidQueryShape);
          put(spelling);
          if (option.kind == VacuumOptionKind::Parallel) {
            const auto workers = std::get_if<int>(&option.argument);
            if (!workers || *workers < 0 || *workers > 1024)
              fail("invalid VACUUM PARALLEL value", DiagnosticCode::InvalidQueryShape);
            put(" " + std::to_string(*workers));
          } else if (!std::holds_alternative<std::monostate>(option.argument)) {
            if (option.kind == VacuumOptionKind::IndexCleanup) {
              const auto cleanup = std::get_if<VacuumIndexCleanup>(&option.argument);
              if (!cleanup || !*vacuumIndexCleanupName(*cleanup))
                fail("invalid VACUUM INDEX_CLEANUP value", DiagnosticCode::InvalidQueryShape);
              put(" ");
              put(vacuumIndexCleanupName(*cleanup));
            } else {
              const auto enabled = std::get_if<bool>(&option.argument);
              if (!enabled) fail("invalid VACUUM boolean value", DiagnosticCode::InvalidQueryShape);
              put(*enabled ? " TRUE" : " FALSE");
            }
          }
        });
        put(")");
      }
      if (!q.vacuum->targets.empty()) {
        put(" ");
        separated(q.vacuum->targets, [&](const auto &target) {
          if (target.name.empty() || target.name.size() > 3)
            fail("invalid VACUUM relation name", DiagnosticCode::InvalidQueryShape);
          name(target.name);
          if (!target.columns.empty()) {
            put(" (");
            separated(target.columns, [&](const auto &column) { identifier(column); });
            put(")");
          }
        });
      }
      break;
    }
    case StatementKind::Pragma: {
      if (!sqlite() || program.sourceDialect != dialect || !q.pragma)
        fail("PRAGMA requires the original SQLite dialect");
      const auto &pragma = *q.pragma;
      if (pragma.name.empty() || pragma.name.size() > 2 ||
          static_cast<unsigned>(pragma.style) > static_cast<unsigned>(PragmaArgumentStyle::Parenthesized) ||
          bool(pragma.argument) != (pragma.style != PragmaArgumentStyle::None))
        fail("invalid PRAGMA argument shape", DiagnosticCode::InvalidQueryShape);
      put("PRAGMA ");
      name(pragma.name);
      if (pragma.argument) {
        const bool parens = pragma.style == PragmaArgumentStyle::Parenthesized;
        put(parens ? "(" : " = ");
        auto value = pragma.argument;
        const auto &argument = q.expressions.at(value);
        if (argument.kind == ExprKind::Unary && argument.operands.size() == 1 &&
            (argument.value == "-" || argument.value == "+") &&
            argument.operands.front() &&
            q.expressions.at(argument.operands.front()).kind == ExprKind::Number) {
          // PRAGMA's signed-number production accepts a sign directly before
          // the number. General SQL unary expressions add parentheses.
          put(argument.value);
          value = argument.operands.front();
        } else if (argument.kind != ExprKind::Number && argument.kind != ExprKind::String) {
          fail("PRAGMA argument must be a name or signed number", DiagnosticCode::InvalidQueryShape);
        }
        expr(q, value);
        if (parens) put(")");
      }
      break;
    }
    case StatementKind::Inspect: {
      if (!mysql() || program.sourceDialect != dialect || !q.inspection)
        fail("inspection statements require the original MySQL-family dialect");
      const auto &inspection = *q.inspection;
      if (inspection.kind == InspectionKind::DdlStatus ||
          inspection.kind == InspectionKind::DdlJobs) {
        if (dialect != Dialect::TiDB) fail("ADMIN SHOW DDL requires TiDB");
        put(inspectionKindName(inspection.kind));
        if (inspection.jobCount) put(" " + std::to_string(*inspection.jobCount));
        if (q.where) { put(" WHERE "); expr(q, q.where); }
        break;
      }
      if (inspection.kind == InspectionKind::Describe) {
        put("DESCRIBE "); name(inspection.target);
        if (inspection.column) { put(" "); identifier(*inspection.column); }
        if (inspection.pattern) { put(" "); expr(q, inspection.pattern); }
        break;
      }
      put("SHOW ");
      if (inspection.extended) put("EXTENDED ");
      if (inspection.full) put("FULL ");
      if (inspection.scope == InspectionScope::Global) put("GLOBAL ");
      if (inspection.scope == InspectionScope::Session) put("SESSION ");
      put(inspectionKindName(inspection.kind));
      if (inspection.kind == InspectionKind::CreateDefinition) {
        put(" "); put(inspectionObjectName(inspection.object));
        if (inspection.ifNotExists) put(" IF NOT EXISTS");
        put(" "); name(inspection.target);
      } else if (!inspection.target.empty()) { put(" FROM "); name(inspection.target); }
      if (inspection.database) { put(" FROM "); identifier(*inspection.database); }
      if (inspection.pattern) { put(" LIKE "); expr(q, inspection.pattern); }
      if (q.where) { put(" WHERE "); expr(q, q.where); }
      break;
    }
    case StatementKind::Merge:
      merge(q);
      break;
    case StatementKind::Call:
      put("CALL ");
      name(q.routine);
      put("(");
      separated(q.arguments, [&](auto id) { expr(q, id); });
      put(")");
      break;
    case StatementKind::Select:
      select(q);
      break;
    case StatementKind::Insert:
      insert(q);
      break;
    case StatementKind::Update:
      update(q);
      break;
    case StatementKind::Delete:
      deleteStatement(q);
      break;
    default:
      ddl(q);
    }
    active.pop_back();
    origin = oldOrigin;
  }
  GeneratedSql runExpression(QueryId queryId, ExprId value) {
    // The caller owns the surrounding SELECT and its declarations. Standalone
    // expressions must carry their resolved window instead of a source name.
    allowNamedWindowReferences=false;
    GeneratedSql result;
    try {
      poll();
      origin = query(queryId).origin;
      expr(query(queryId), value);
      result.sql = std::move(sql);
    } catch (const PrintFailure &e) {
      result.diagnostics.push_back({e.code, origin, e.message});
    } catch (const std::exception &) {
      result.diagnostics.push_back(
          {DiagnosticCode::InvalidQueryShape, origin, "invalid expression IR"});
    }
    return result;
  }
  GeneratedSql runType(const BasicTypeSpec<std::vector<std::string>> &spec) {
    GeneratedSql result;
    try {
      type(spec);
      result.sql = std::move(sql);
    } catch (const PrintFailure &e) {
      result.diagnostics.push_back({e.code, spec.origin, e.message});
    }
    return result;
  }
  GeneratedSql run() {
    GeneratedSql result;
    try {
      poll();
      if (program.roots.empty())
        fail("IR has no top-level statements",
             DiagnosticCode::InvalidQueryShape);
      if (dialect != program.sourceDialect)
        fail("cross-dialect generation requires semantic capability checks; "
             "this backend currently emits the source dialect only");
      if (mssql() && program.roots.size()>1)
        for (const auto id:program.roots) {
          const auto &root=query(id);
          if (root.ddl && sqlServerDefinitionRequiresBatch(root.kind,root.ddl->object))
            fail("SQL Server schema/view definitions require explicit separate batch boundaries");
        }
      for (std::size_t i = 0; i < program.roots.size(); ++i) {
        if (i)
          put("\n");
        statement(program.roots[i]);
        put(";");
      }
      result.sql = std::move(sql);
    } catch (const PrintFailure &e) {
      result.diagnostics.push_back({e.code, origin, e.message});
    } catch (const std::exception &) {
      result.diagnostics.push_back(
          {DiagnosticCode::InvalidQueryShape, origin, "invalid canonical IR"});
    }
    return result;
  }
  void type(const BasicTypeSpec<std::vector<std::string>> &);
  void column(const ModelStatement &,
              const BasicColumnSpec<std::string, std::vector<std::string>> &,
              bool allowMissingType = false);
  void
  constraint(const ModelStatement &,
             const BasicConstraint<std::string, std::vector<std::string>> &,
             bool inlineColumn = false);
  void sequence(const ModelStatement &, const ModelDdl &);
  void distributionOptions(const ModelDdl &, bool alter = false);
  void tableOptions(const ModelDdl &);
};
void Printer::type(const BasicTypeSpec<std::vector<std::string>> &t) {
  if (t.name.empty()) {
    if (sqlite())
      return;
    fail("column type is missing", DiagnosticCode::InvalidQueryShape);
  }
  static const std::unordered_set<std::string> builtins = {"INT",
                                                           "INTEGER",
                                                           "SMALLINT",
                                                           "BIGINT",
                                                           "TINYINT",
                                                           "MEDIUMINT",
                                                           "DECIMAL",
                                                           "NUMERIC",
                                                           "NUMBER",
                                                           "FLOAT",
                                                           "REAL",
                                                           "DOUBLE",
                                                           "BOOLEAN",
                                                           "BOOL",
                                                           "BIT",
                                                           "CHAR",
                                                           "CHARACTER",
                                                           "VARCHAR",
                                                           "VARCHAR2",
                                                           "NVARCHAR",
                                                           "NVARCHAR2",
                                                           "NCHAR",
                                                           "TEXT",
                                                           "TINYTEXT",
                                                           "MEDIUMTEXT",
                                                           "LONGTEXT",
                                                           "BINARY",
                                                           "VARBINARY",
                                                           "BLOB",
                                                           "TINYBLOB",
                                                           "MEDIUMBLOB",
                                                           "LONGBLOB",
                                                           "BYTEA",
                                                           "DATE",
                                                           "TIME",
                                                           "TIMESTAMP",
                                                           "TIMESTAMPTZ",
                                                           "DATETIME",
                                                           "DATETIME2",
                                                           "SMALLDATETIME",
                                                           "DATETIMEOFFSET",
                                                           "INTERVAL",
                                                           "JSON",
                                                           "JSONB",
                                                           "UUID",
                                                           "UNIQUEIDENTIFIER",
                                                           "XML",
                                                           "MONEY",
                                                           "SMALLMONEY",
                                                           "SERIAL",
                                                           "BIGSERIAL",
                                                           "SMALLSERIAL",
                                                           "ENUM",
                                                           "SET",
                                                           "CLOB",
                                                           "NCLOB",
                                                           "RAW",
                                                           "LONG",
                                                           "YEAR", "POINT", "LINESTRING", "POLYGON", "MULTIPOINT", "MULTILINESTRING", "MULTIPOLYGON", "GEOMETRYCOLLECTION", "INTERVAL", "ROWVERSION", "SIGNED", "UNSIGNED", "INT2", "INT4", "INT8", "DEC", "TIMETZ", "BINARY_FLOAT", "BINARY_DOUBLE", "PLS_INTEGER", "BINARY_INTEGER"};
  auto upper = [](std::string value) {
    for (auto &ch : value)
      if (ch >= 'a' && ch <= 'z')
        ch -= 32;
    return value;
  };
  static const std::unordered_set<std::string> tidbNames{
      "MIDDLEINT","INT1","INT3","FIXED","FLOAT4","FLOAT8","VARCHARACTER","VECTOR","SQL_TSI_YEAR"};
  const auto spelling=t.name.size()==1?upper(t.name.front()):std::string{};
  if (t.name.size() == 1 && (t.nameQuoted.empty() || !t.nameQuoted[0]) &&
      (builtins.count(spelling) || (dialect==Dialect::PostgreSQL && (spelling=="FLOAT4" || spelling=="FLOAT8")) ||
       (dialect==Dialect::TiDB && tidbNames.count(spelling))))
    put(spelling);
  else {
    auto resolved=t.name;
    for(std::size_t i=0;i<resolved.size();++i)resolved[i]=canonicalIdentifier(dialect,resolved[i],i<t.nameQuoted.size() && t.nameQuoted[i]);
    name(resolved);
  }
  if(t.intervalType) {
    const auto &s=*t.intervalType;
    if(upper(t.name.back())!="INTERVAL" || t.name.size()!=1 || (!t.nameQuoted.empty() && t.nameQuoted[0]) || !t.arguments.empty())fail("invalid structured interval type");
    if(const auto error=validateIntervalSpec(s,dialect);!error.empty())fail(error,DiagnosticCode::InvalidQueryShape);
    if(oracle() && !((s.leading==IntervalField::Year && s.trailing==IntervalField::Month) || (s.leading==IntervalField::Day && s.trailing==IntervalField::Second)))fail("invalid Oracle interval type range");
    if(s.prefixPrecision)put("("+std::to_string(*s.fractionalPrecision)+")");
    if(s.leading!=IntervalField::None) {
      put(std::string(" ")+intervalFieldName(s.leading));
      if(s.leadingPrecision)put("("+std::to_string(*s.leadingPrecision)+")");
      if(s.trailing!=IntervalField::None)put(std::string(" TO ")+intervalFieldName(s.trailing));
      if(s.fractionalPrecision && !s.prefixPrecision)put("("+std::to_string(*s.fractionalPrecision)+")");
    }
  }
  for (const auto &modifier : t.modifiers)
    if (modifier == "PRECISION" || modifier == "VARYING") {
      put(" ");
      put(modifier);
    }
  if (!t.arguments.empty()) {
    put("(");
    separated(t.arguments, [&](const auto &a) {
      if (a.kind == TokenKind::String)
        literal(a.value);
      else if (a.kind == TokenKind::Number && (numeric(a.value) ||
               (!a.value.empty() && (a.value.front()=='-' || a.value.front()=='+') && numeric(std::string_view(a.value).substr(1)))))
        put(a.value);
      else if ((upper(a.value) == "MAX" && mssql()) ||
               (a.value == "*" && oracle()))
        put(upper(a.value));
      else
        fail("unsupported type argument");
    });
    if (t.lengthSemantics) {
      if (!oracle() ||
          (*t.lengthSemantics != "CHAR" && *t.lengthSemantics != "BYTE"))
        fail("invalid Oracle length semantics");
      put(" ");
      put(*t.lengthSemantics);
    }
    put(")");
  }
  for (const auto &modifier : t.modifiers)
    if (modifier != "PRECISION" && modifier != "VARYING") {
      static const std::unordered_set<std::string> allowed = {
          "UNSIGNED", "ZEROFILL", "WITH TIME ZONE", "WITHOUT TIME ZONE",
          "WITH LOCAL TIME ZONE"};
      if (!allowed.count(modifier))
        fail("unsupported type modifier");
      put(" ");
      put(modifier);
    }
  if (t.arrayDimensions < 0 || t.arrayDimensions > 128)
    fail("invalid array dimensions", DiagnosticCode::ResourceLimit);
  for (int i = 0; i < t.arrayDimensions; ++i)
    put("[]");
}
void Printer::constraint(
    const ModelStatement &q,
    const BasicConstraint<std::string, std::vector<std::string>> &k,
    bool inlineColumn) {
  if (k.name) {
    put("CONSTRAINT ");
    identifier(*k.name);
    put(" ");
  }
  switch (k.kind) {
  case ConstraintKind::PrimaryKey:
    put("PRIMARY KEY");
    break;
  case ConstraintKind::Unique:
    put("UNIQUE");
    break;
  case ConstraintKind::ForeignKey:
    if (!inlineColumn)
      put("FOREIGN KEY");
    break;
  case ConstraintKind::Check:
    put("CHECK (");
    expr(q, k.check);
    put(")");
    break;
  }
  if (k.kind != ConstraintKind::Check && !inlineColumn) {
    if (k.columns.empty())
      fail("constraint has no columns", DiagnosticCode::InvalidQueryShape);
    put(" (");
    separated(k.columns, [&](const auto &n) { identifier(n); });
    put(")");
  }
  if (k.clustering != PrimaryKeyClustering::Unspecified) {
    if (dialect != Dialect::TiDB || k.kind != ConstraintKind::PrimaryKey)
      fail("target cannot preserve TiDB primary key clustering");
    put(k.clustering == PrimaryKeyClustering::Clustered ? " CLUSTERED" : " NONCLUSTERED");
  }
  if (k.kind == ConstraintKind::ForeignKey) {
    if (!inlineColumn)
      put(" ");
    put("REFERENCES ");
    name(k.referencedTable);
    if (!k.referencedColumns.empty()) {
      put(" (");
      separated(k.referencedColumns, [&](const auto &n) { identifier(n); });
      put(")");
    }
    auto action = [&](ReferenceAction a, const char *event) {
      if (a == ReferenceAction::None)
        return;
      put(" ON ");
      put(event);
      put(" ");
      switch (a) {
      case ReferenceAction::NoAction:
        put("NO ACTION");
        break;
      case ReferenceAction::Restrict:
        put("RESTRICT");
        break;
      case ReferenceAction::Cascade:
        put("CASCADE");
        break;
      case ReferenceAction::SetNull:
        put("SET NULL");
        break;
      case ReferenceAction::SetDefault:
        put("SET DEFAULT");
        break;
      default:
        fail("invalid reference action", DiagnosticCode::InvalidQueryShape);
      }
    };
    action(k.onDelete, "DELETE");
    action(k.onUpdate, "UPDATE");
  }
  if (k.deferrable) {
    put(*k.deferrable ? " DEFERRABLE" : " NOT DEFERRABLE");
  }
  if (k.initiallyDeferred)
    put(*k.initiallyDeferred ? " INITIALLY DEFERRED" : " INITIALLY IMMEDIATE");
}
void Printer::column(
    const ModelStatement &q,
    const BasicColumnSpec<std::string, std::vector<std::string>> &col,
    bool allowMissingType) {
  identifier(col.name);
  if (mssql() && col.generated) {
    put(" AS ");
    expr(q, col.generated);
    if (col.stored)
      put(" PERSISTED");
    return;
  }
  if (!col.type.name.empty()) {
    put(" ");
    type(col.type);
  } else if (!sqlite() && !allowMissingType)
    fail("missing column type", DiagnosticCode::InvalidQueryShape);
  if (!col.characterSet.empty()) {
    if (!mysql())
      fail("column charset is MySQL-specific");
    put(" CHARACTER SET ");
    name(col.characterSet);
  }
  if (!col.collation.empty()) {
    put(" COLLATE ");
    name(col.collation);
  }
  // Preserve PostgreSQL's ordered constraint nodes without changing the bound column.
  if (pg() && program.sourceDialect==Dialect::PostgreSQL &&
      col.nullabilityOrigin.end>col.nullabilityOrigin.begin && col.nullable &&
      col.identity==IdentityKind::None && !col.generated && !col.autoRandom && !col.comment &&
      std::all_of(col.constraints.begin(),col.constraints.end(),[](const auto &k) { return k.origin.end>k.origin.begin; })) {
    struct Clause { std::size_t position; unsigned kind; std::size_t index; };
    std::vector<Clause> clauses{{col.nullabilityOrigin.begin,0,0}};
    if (col.defaultValue) clauses.push_back({expression(q,col.defaultValue).origin.begin,1,0});
    for (std::size_t i=0;i<col.constraints.size();++i)
      clauses.push_back({col.constraints[i].origin.begin,2,i});
    std::stable_sort(clauses.begin(),clauses.end(),[](const auto &a,const auto &b) { return a.position<b.position; });
    for (const auto &clause:clauses) {
      tick();
      if (clause.kind==0) put(*col.nullable ? " NULL" : " NOT NULL");
      else if (clause.kind==1) {
        put(" DEFAULT ");
        if (col.defaultParenthesized) put("(");
        expr(q,col.defaultValue);
        if (col.defaultParenthesized) put(")");
      } else { put(" "); constraint(q,col.constraints[clause.index],true); }
    }
    return;
  }
  if (col.defaultValue) {
    put(" DEFAULT ");
    if (col.defaultParenthesized) put("(");
    expr(q, col.defaultValue);
    if (col.defaultParenthesized) put(")");
  }
  if (col.autoRandom) {
    if (dialect != Dialect::TiDB) fail("target cannot preserve TiDB AUTO_RANDOM");
    put(" AUTO_RANDOM");
    if (col.autoRandom->shardBits) {
      put("(" + std::to_string(*col.autoRandom->shardBits));
      if (col.autoRandom->rangeBits) put(", " + std::to_string(*col.autoRandom->rangeBits));
      put(")");
    }
  }
  if (col.identity == IdentityKind::AutoIncrement)
    put(" AUTO_INCREMENT");
  else if (col.identity == IdentityKind::Identity) {
    if (!mssql())
      fail("IDENTITY is SQL Server-specific");
    put(" IDENTITY");
    if (col.identityStart || col.identityIncrement) {
      if (!col.identityStart || !col.identityIncrement)
        fail("incomplete IDENTITY options", DiagnosticCode::InvalidQueryShape);
      put("(");
      integerLiteral(q, col.identityStart);
      put(", ");
      integerLiteral(q, col.identityIncrement);
      put(")");
    }
  } else if (col.identity == IdentityKind::GeneratedAlways ||
             col.identity == IdentityKind::GeneratedByDefault) {
    if (!pg() && !oracle())
      fail("generated identity unsupported in this dialect");
    put(col.identity == IdentityKind::GeneratedAlways
            ? " GENERATED ALWAYS AS IDENTITY"
            : " GENERATED BY DEFAULT AS IDENTITY");
    if (col.identityStart || col.identityIncrement) {
      put("(");
      if (col.identityStart) {
        put("START WITH ");
        integerLiteral(q, col.identityStart);
      }
      if (col.identityIncrement) {
        if (col.identityStart)
          put(" ");
        put("INCREMENT BY ");
        integerLiteral(q, col.identityIncrement);
      }
      put(")");
    }
  }
  if (col.generated) {
    put(" GENERATED ALWAYS AS (");
    expr(q, col.generated);
    put(")");
    if (col.stored)
      put(" STORED");
    else if (!pg())
      put(" VIRTUAL");
    else
      fail("PostgreSQL baseline requires STORED generated columns");
  }
  if (col.nullable)
    put(*col.nullable ? " NULL" : " NOT NULL");
  for (const auto &k : col.constraints) {
    put(" ");
    constraint(q, k, true);
  }
  if (col.comment) {
    if (!mysql())
      fail("inline column comment is MySQL-specific");
    put(" COMMENT ");
    literal(*col.comment);
  }
}
void Printer::sequence(const ModelStatement &q, const ModelDdl &d) {
  if (mssql() && q.kind==StatementKind::Alter)
    for (const auto &option:d.sequenceOptions)
      if (option.first==SequenceOptionKind::Start)
        fail("SQL Server ALTER SEQUENCE requires RESTART rather than START");
  for (const auto &opt : d.sequenceOptions) {
    put(" ");
    switch (opt.first) {
    case SequenceOptionKind::Start:
      put("START WITH ");
      break;
    case SequenceOptionKind::Increment:
      put("INCREMENT BY ");
      break;
    case SequenceOptionKind::Minimum:
      put("MINVALUE ");
      break;
    case SequenceOptionKind::Maximum:
      put("MAXVALUE ");
      break;
    case SequenceOptionKind::Cache:
      if (!opt.second && !mssql()) fail("target requires an explicit sequence CACHE size");
      put(opt.second?"CACHE ":"CACHE");
      break;
    case SequenceOptionKind::Cycle:
      put("CYCLE");
      break;
    case SequenceOptionKind::NoMinimum:
      put(oracle() ? "NOMINVALUE" : "NO MINVALUE");
      break;
    case SequenceOptionKind::NoMaximum:
      put(oracle() ? "NOMAXVALUE" : "NO MAXVALUE");
      break;
    case SequenceOptionKind::NoCache:
      if (!oracle() && !mssql())
        fail("target cannot preserve sequence NO CACHE");
      put(oracle()?"NOCACHE":"NO CACHE");
      break;
    case SequenceOptionKind::NoCycle:
      put(oracle() ? "NOCYCLE" : "NO CYCLE");
      break;
    }
    if (opt.second)
      integerLiteral(q, opt.second);
  }
}
void Printer::distributionOptions(const ModelDdl &d, bool alter) {
  using Option = DistributionOptionKind;
  for (const auto &option : d.distribution) {
    const bool oceanbase = option.kind == Option::TableGroup || option.kind == Option::Sharding;
    if (dialect != (oceanbase ? Dialect::OceanBase : Dialect::TiDB))
      fail("target cannot preserve " + std::string(distributionOptionName(option.kind)));
    put(alter && option.kind == Option::TableGroup ? " SET " : " ");
    put(distributionOptionName(option.kind));
    put(" = ");
    if (const auto *number = std::get_if<std::uint64_t>(&option.value)) {
      put(std::to_string(*number));
    } else if (const auto *value = std::get_if<std::string>(&option.value)) {
      if (option.kind == Option::Sharding) literal(*value);
      else identifier(*value);
    } else if (option.kind == Option::PlacementPolicy) {
      put("DEFAULT");
    } else {
      literal("");
    }
  }
}
void Printer::tableOptions(const ModelDdl &d) {
  if (!d.engine.empty()) {
    if (!mysql())
      fail("ENGINE is MySQL-specific");
    put(" ENGINE=");
    identifier(d.engine);
  }
  if (!d.characterSet.empty()) {
    if (!mysql())
      fail("table charset is MySQL-specific");
    put(" DEFAULT CHARACTER SET=");
    identifier(d.characterSet);
  }
  if (!d.collation.empty()) {
    put(" COLLATE=");
    identifier(d.collation);
  }
  if (d.tableComment) {
    if (!mysql())
      fail("table comment option is MySQL-specific");
    put(" COMMENT=");
    literal(*d.tableComment);
  }
  if (!d.tablespace.empty()) {
    put(" TABLESPACE ");
    identifier(d.tablespace);
  }
  if (!d.onCommit.empty()) {
    if (d.onCommit != "PRESERVE ROWS" && d.onCommit != "DELETE ROWS" &&
        d.onCommit != "DROP")
      fail("invalid ON COMMIT action", DiagnosticCode::InvalidQueryShape);
    put(" ON COMMIT ");
    put(d.onCommit);
  }
  distributionOptions(d);
  if (d.partition) {
    if (!mysql() || program.sourceDialect != dialect)
      fail("native partition layout requires an explicit target mapping");
    const auto &partition = *d.partition;
    const bool hash = partition.method == PartitionMethod::Hash;
    if ((!hash && partition.method != PartitionMethod::Key) ||
        (hash && (partition.columns.size() != 1 || partition.algorithm)) ||
        (partition.algorithm && *partition.algorithm != 1 && *partition.algorithm != 2) ||
        (partition.count && (!*partition.count || *partition.count > 8192)))
      fail("invalid partition specification", DiagnosticCode::InvalidQueryShape);
    put(" PARTITION BY ");
    if (partition.linear) put("LINEAR ");
    put(hash ? "HASH" : "KEY");
    if (partition.algorithm) put(" ALGORITHM=" + std::to_string(*partition.algorithm));
    put(" (");
    separated(partition.columns, [&](const auto &column) { identifier(column); });
    put(")");
    if (partition.count) put(" PARTITIONS " + std::to_string(*partition.count));
  }
}
void Printer::ddl(const ModelStatement &q) {
  if (!q.ddl)
    fail("DDL statement has no typed definition",
         DiagnosticCode::InvalidQueryShape);
  const auto &d = *q.ddl;
  if (d.olap || d.tabletCount)
    fail("target has no proven mapping for the distributed table layout");
  if (mssql() && d.object==ObjectKind::Sequence) {
    if (d.name.size()>2) fail("SQL Server sequence names permit only a schema qualifier");
    for (const auto &target:d.names)
      if (target.size()>2) fail("SQL Server sequence names permit only a schema qualifier");
  }
  if (d.object==ObjectKind::LoadableFunction) {
    if (dialect!=program.sourceDialect || (dialect!=Dialect::MySQL && dialect!=Dialect::MariaDB))
      fail("loadable functions require a target library and deployment mapping");
    if (!d.mysqlLoadableFunction || d.name.size()!=1 || q.kind!=StatementKind::Create ||
        (d.orReplace && (dialect!=Dialect::MariaDB || d.ifNotExists)))
      fail("invalid loadable function DDL",DiagnosticCode::InvalidQueryShape);
    const auto &function=*d.mysqlLoadableFunction;
    const std::string_view result=mysqlLoadableResultName(function.result);
    if (result.empty()) fail("invalid loadable function result",DiagnosticCode::InvalidQueryShape);
    put("CREATE ");
    if (d.orReplace) put("OR REPLACE ");
    if (function.aggregate) put("AGGREGATE ");
    put("FUNCTION ");
    if (d.ifNotExists) put("IF NOT EXISTS ");
    name(d.name);
    put(" RETURNS ");
    put(result);
    put(" SONAME ");
    literal(function.library);
    return;
  }
  if (d.virtualTable) {
    if (!sqlite() || program.sourceDialect != dialect)
      fail("CREATE VIRTUAL TABLE requires the original SQLite dialect");
    const auto &module = *d.virtualTable;
    if (q.kind != StatementKind::Create || d.object != ObjectKind::Table || d.name.empty() ||
        d.name.size() > 2 || module.module.empty() || (!module.argumentList && !module.arguments.empty()) ||
        q.source || !d.definitions.empty() || !d.constraints.empty() || !d.likeTable.empty() || d.orReplace ||
        d.temporary || d.unlogged || d.unique || d.partition)
      fail("invalid SQLite virtual table definition", DiagnosticCode::InvalidQueryShape);
    put("CREATE VIRTUAL TABLE ");
    if (d.ifNotExists) put("IF NOT EXISTS ");
    name(d.name);
    put(" USING ");
    identifier(module.module);
    if (module.argumentList) {
      put("(");
      separated(module.arguments, [&](const auto &argument) {
        poll();
        if (argument.payload.size() > options.maxBytes - sql.size())
          fail("generated SQL byte limit exceeded", DiagnosticCode::ResourceLimit);
        if (const auto error = validateVirtualTableArgument(argument.payload, options.shouldStop))
          fail(error->message, error->code);
        put(argument.payload);
      });
      put(")");
    }
    return;
  }
  auto object = [&] {
    if (d.object == ObjectKind::TableGroup && dialect != Dialect::OceanBase)
      fail("target does not support OceanBase TABLEGROUP");
    put(objectKindName(d.object));
  };
  auto behavior = [&](DropBehavior b) {
    switch (b) {
    case DropBehavior::Default:
      break;
    case DropBehavior::Cascade:
      put(" CASCADE");
      break;
    case DropBehavior::Restrict:
      put(" RESTRICT");
      break;
    case DropBehavior::CascadeConstraints:
      put(" CASCADE CONSTRAINTS");
      break;
    }
  };
  if (q.kind == StatementKind::Create) {
    put("CREATE ");
    if (d.orReplace)
      put("OR REPLACE ");
    if (d.mysqlViewOptions) {
      if (d.object != ObjectKind::View || dialect != program.sourceDialect ||
          (dialect != Dialect::MySQL && dialect != Dialect::MariaDB))
        fail("view algorithm, definer and security require an explicit target mapping");
      const auto &header = *d.mysqlViewOptions;
      if (header.algorithm != MySqlViewAlgorithm::Unspecified) {
        put("ALGORITHM = ");
        put(mysqlViewAlgorithmName(header.algorithm));
        put(" ");
      }
      if (header.definer) {
        put("DEFINER = ");
        if (header.definer->currentUser) put("CURRENT_USER");
        else {
          literal(header.definer->user);
          put("@");
          literal(header.definer->host);
        }
        put(" ");
      }
      if (header.security != MySqlViewSecurity::Unspecified) {
        put("SQL SECURITY ");
        put(mysqlViewSecurityName(header.security));
        put(" ");
      }
    }
    if (d.globalTemporary)
      put("GLOBAL TEMPORARY ");
    else if (d.temporary)
      put("TEMPORARY ");
    if (d.unlogged)
      put("UNLOGGED ");
    if (d.unique)
      put("UNIQUE ");
    object();
    if (d.object==ObjectKind::Index && d.name.empty()) {
      if (!pg() || program.sourceDialect!=Dialect::PostgreSQL || d.ifNotExists)
        fail("server-chosen index names require native PostgreSQL creation");
    } else {
      put(" ");
      if (d.ifNotExists)
        put("IF NOT EXISTS ");
      name(d.name);
    }
    switch (d.object) {
    case ObjectKind::Table: {
      if (!d.likeTable.empty()) {
        put(" LIKE ");
        name(d.likeTable);
        break;
      }
      if (pg() && q.source && (!d.definitions.empty() || !d.constraints.empty() || !d.tableIndexes.empty()))
        fail("typed CREATE TABLE AS needs a separate PostgreSQL deployment plan");
      if (d.definitions.empty() && d.constraints.empty() && d.tableIndexes.empty() && !q.source) {
        if (!pg() || program.sourceDialect!=Dialect::PostgreSQL)
          fail("zero-column tables require native PostgreSQL semantics");
        put(" ()");
      }
      if (!d.definitions.empty() || !d.constraints.empty() || !d.tableIndexes.empty()) {
        if (!d.tableIndexes.empty() && (!mysql() || dialect!=program.sourceDialect))
          fail("inline MySQL indexes need an explicit target deployment mapping");
        put(" (");
        bool comma=false;
        const auto separator=[&] {
          if (comma) put(", ");
          comma=true;
        };
        const auto positioned=[](const auto &item) { return item.origin.end>item.origin.begin; };
        const bool sourceOrder=pg() && program.sourceDialect==Dialect::PostgreSQL &&
            !d.constraints.empty() &&
            std::all_of(d.definitions.begin(),d.definitions.end(),positioned) &&
            std::all_of(d.constraints.begin(),d.constraints.end(),positioned);
        if (d.tableIndexes.empty() && !sourceOrder) {
          for (const auto &col:d.definitions) {
            separator();
            column(q,col);
          }
          for (const auto &key:d.constraints) {
            separator();
            constraint(q,key);
          }
        } else {
          // Source order can affect the server's names for unnamed indexes.
          std::size_t col=0,key=0,index=0;
          while (col<d.definitions.size() || key<d.constraints.size() || index<d.tableIndexes.size()) {
            separator();
            const auto columnAt=col<d.definitions.size()?d.definitions[col].origin.begin:std::string::npos;
            const auto constraintAt=key<d.constraints.size()?d.constraints[key].origin.begin:std::string::npos;
            const auto indexAt=index<d.tableIndexes.size()?d.tableIndexes[index].origin.begin:std::string::npos;
            if (columnAt<=constraintAt && columnAt<=indexAt)
              column(q,d.definitions[col++]);
            else if (constraintAt<=indexAt)
              constraint(q,d.constraints[key++]);
            else {
              const auto &definition=d.tableIndexes[index++];
              put(definition.unique?"UNIQUE KEY":"KEY");
              if (definition.name) {
                put(" ");
                identifier(*definition.name);
              }
              put(" (");
              if (definition.keys.empty()) fail("table index has no keys",DiagnosticCode::InvalidQueryShape);
              separated(definition.keys,[&](const auto &part) {
                expr(q,part.expression);
                if (part.prefixLength) {
                  put("(");
                  expr(q,part.prefixLength);
                  put(")");
                }
                if (part.descending) put(" DESC");
              });
              put(")");
              if (definition.method!=TableIndexMethod::Default)
                put(definition.method==TableIndexMethod::Btree?" USING BTREE":" USING HASH");
            }
          }
        }
        put(")");
      }
      tableOptions(d);
      if (q.source) {
        put(" AS ");
        statement(q.source);
        if (d.withNoData)
          put(" WITH NO DATA");
      }
      break;
    }
    case ObjectKind::View:
    case ObjectKind::MaterializedView:
      if (mssql()) {
        if (d.name.empty() || d.name.size()>2 || d.name.back().find('#')==0)
          fail("SQL Server view name requires a non-temporary name with at most a schema qualifier");
        const auto &source=query(q.source);
        if (!source.orderBy.empty() && !source.limit && !source.offset)
          fail("ORDER BY in a SQL Server view requires TOP or OFFSET");
      }
      if (d.object == ObjectKind::MaterializedView && !pg() && !oracle())
        fail("target has no equivalent native materialized view");
      if (!d.columns.empty()) {
        put(" (");
        separated(d.columns, [&](const auto &n) { identifier(n); });
        put(")");
      }
      put(" AS ");
      statement(q.source);
      if (d.object == ObjectKind::MaterializedView && d.withNoData) {
        if (!pg())
          fail("deferred materialized-view population has different semantics");
        put(" WITH NO DATA");
      }
      if (!d.viewCheck.empty()) {
        if (d.viewCheck != "LOCAL" && d.viewCheck != "CASCADED" &&
            d.viewCheck != "DEFAULT")
          fail("invalid view check option");
        put(" WITH ");
        if (d.viewCheck != "DEFAULT") {
          put(d.viewCheck);
          put(" ");
        }
        put("CHECK OPTION");
      }
      break;
    case ObjectKind::Index:
      if (pg() && d.name.size()>1)
        fail("PostgreSQL CREATE INDEX names cannot have a schema qualifier");
      if (pg() && d.indexMethod.size()>1)
        fail("PostgreSQL index access methods cannot have a schema qualifier");
      put(" ON ");
      name(d.indexedTable);
      if (!d.indexMethod.empty()) {
        put(" USING ");
        name(d.indexMethod);
      }
      put(" (");
      if (d.keys.empty())
        fail("index has no keys", DiagnosticCode::InvalidQueryShape);
      separated(d.keys, [&](const auto &key) {
        if (mssql()) {
          const auto &column=expression(q,key.expression);
          if (column.kind!=ExprKind::Name || column.name.size()!=1 || key.prefixLength || !key.nulls.empty())
            fail("SQL Server index keys require unqualified columns");
        }
        const auto &indexExpression=expression(q,key.expression);
        const bool postgresExpression=pg() &&
            !(indexExpression.kind==ExprKind::Name && indexExpression.name.size()==1);
        if (postgresExpression) put("(");
        expr(q, key.expression);
        if (postgresExpression) put(")");
        if (key.prefixLength) {
          put("(");
          expr(q, key.prefixLength);
          put(")");
        }
        if (key.descending)
          put(" DESC");
        if (!key.nulls.empty()) {
          if (key.nulls != "FIRST" && key.nulls != "LAST")
            fail("invalid NULL ordering");
          put(" NULLS ");
          put(key.nulls);
        }
      });
      put(")");
      if (d.indexScope != IndexScope::Unspecified) {
        if (dialect != Dialect::OceanBase) fail("target cannot preserve OceanBase index scope");
        put(d.indexScope == IndexScope::Local ? " LOCAL" : " GLOBAL");
      }
      if (!d.includeColumns.empty()) {
        put(" INCLUDE (");
        separated(d.includeColumns, [&](const auto &n) { identifier(n); });
        put(")");
      }
      if (d.predicate) {
        if (mssql() && !validSqlServerIndexFilter(d.predicate,
            [&](ExprId id)->const ModelExpression & {return expression(q,id);}))
          fail("target SQL Server filtered index cannot preserve this predicate");
        put(" WHERE ");
        expr(q, d.predicate);
      }
      break;
    case ObjectKind::Sequence:
      sequence(q, d);
      break;
    case ObjectKind::TableGroup:
    case ObjectKind::Schema:
    case ObjectKind::Database:
      tableOptions(d);
      break;
    case ObjectKind::LoadableFunction:
      fail("invalid loadable function dispatch",DiagnosticCode::InvalidQueryShape);
    }
  } else if (q.kind == StatementKind::Alter) {
    const bool maria = dialect == Dialect::MariaDB, tidb = dialect == Dialect::TiDB;
    put("ALTER ");
    object();
    put(" ");
    if (d.ifExists) {
      if (!pg() && !(maria && d.object == ObjectKind::Table))
        fail("target cannot preserve ALTER IF EXISTS for this object");
      put("IF EXISTS ");
    }
    name(d.name);
    if (d.object == ObjectKind::Sequence) {
      sequence(q, d);
      return;
    }
    if (d.object == ObjectKind::Table && !d.distribution.empty() && d.actions.empty()) {
      distributionOptions(d, true);
      return;
    }
    if (d.object != ObjectKind::Table || d.actions.empty())
      fail("unsupported ALTER shape");
    put(" ");
    separated(d.actions, [&](const auto &a) {
      switch (a.kind) {
      case AlterKind::AddColumn:
        if (!a.column)
          fail("ADD missing column", DiagnosticCode::InvalidQueryShape);
        if (a.ifNotExists && !pg() && !maria && !tidb)
          fail("target cannot preserve ADD COLUMN IF NOT EXISTS");
        put(a.ifNotExists ? "ADD COLUMN IF NOT EXISTS " : "ADD ");
        column(q, *a.column);
        break;
      case AlterKind::DropColumn:
        if (!a.name)
          fail("DROP missing column");
        put("DROP COLUMN ");
        if (a.ifExists) {
          if (!pg() && !mssql() && !maria && !tidb)
            fail("target cannot preserve DROP COLUMN IF EXISTS");
          put("IF EXISTS ");
        }
        identifier(*a.name);
        break;
      case AlterKind::RenameColumn:
        if (!a.name || !a.newName)
          fail("RENAME missing name");
        put("RENAME COLUMN ");
        identifier(*a.name);
        put(" TO ");
        identifier(*a.newName);
        break;
      case AlterKind::RenameTable:
        if (!a.newName)
          fail("RENAME missing new name");
        put("RENAME TO ");
        identifier(*a.newName);
        break;
      case AlterKind::AlterType:
        if (!a.name || !a.type)
          fail("ALTER TYPE missing fields");
        put("ALTER COLUMN ");
        identifier(*a.name);
        put(mssql() ? " " : " TYPE ");
        type(*a.type);
        if (a.value) {
          put(" USING ");
          expr(q, a.value);
        }
        if (a.nullable)
          put(*a.nullable ? " NULL" : " NOT NULL");
        break;
      case AlterKind::SetDefault:
      case AlterKind::DropDefault:
      case AlterKind::SetNotNull:
      case AlterKind::DropNotNull:
        if (!a.name)
          fail("ALTER missing column");
        put("ALTER COLUMN ");
        identifier(*a.name);
        switch (a.kind) {
        case AlterKind::SetDefault:
          if (dialect==Dialect::TiDB && !a.defaultParenthesized) {
            const auto &value=expression(q,a.value);
            const bool literal=value.kind==ExprKind::Number || value.kind==ExprKind::String ||
              value.kind==ExprKind::Null || value.kind==ExprKind::Boolean;
            const bool signedNumber=value.kind==ExprKind::Unary && value.operands.size()==1 &&
              (value.value=="+" || value.value=="-") && expression(q,value.operands.front()).kind==ExprKind::Number;
            if (!literal && !signedNumber) fail("TiDB ALTER DEFAULT expressions require parentheses");
          }
          put(" SET DEFAULT ");
          if (a.defaultParenthesized) put("(");
          expr(q, a.value);
          if (a.defaultParenthesized) put(")");
          break;
        case AlterKind::DropDefault:
          put(" DROP DEFAULT");
          break;
        case AlterKind::SetNotNull:
          put(" SET NOT NULL");
          break;
        default:
          put(" DROP NOT NULL");
        }
        break;
      case AlterKind::AddConstraint:
        if (!a.constraint)
          fail("ADD missing constraint");
        put("ADD ");
        constraint(q, *a.constraint);
        break;
      case AlterKind::DropConstraint:
        if (a.constraint && a.constraint->kind == ConstraintKind::PrimaryKey) {
          put("DROP PRIMARY KEY");
        } else {
          if (!a.name)
            fail("DROP missing constraint");
          put("DROP CONSTRAINT ");
          identifier(*a.name);
        }
        break;
      case AlterKind::ModifyColumn:
        if (!a.column)
          fail("MODIFY missing column");
        put("MODIFY ");
        column(q, *a.column, oracle());
        break;
      case AlterKind::ChangeColumn:
        if (!a.column || !a.name)
          fail("CHANGE missing fields");
        put("CHANGE COLUMN ");
        identifier(*a.name);
        put(" ");
        column(q, *a.column);
        break;
      }
      behavior(a.dropBehavior);
    });
  } else if (q.kind == StatementKind::Drop) {
    put("DROP ");
    if (d.temporary)
      put("TEMPORARY ");
    object();
    put(" ");
    if (d.ifExists)
      put("IF EXISTS ");
    if (d.names.empty())
      fail("DROP missing targets");
    separated(d.names, [&](const auto &n) { name(n); });
    if (!d.indexedTable.empty()) {
      put(" ON ");
      name(d.indexedTable);
    }
    behavior(d.dropBehavior);
    if (d.purge)
      put(" PURGE");
  } else if (q.kind == StatementKind::Truncate) {
    put("TRUNCATE TABLE ");
    if (d.truncateTargets.empty())
      fail("TRUNCATE missing per-target semantics");
    separated(d.truncateTargets, [&](const auto &target) {
      if (target.only)
        put("ONLY ");
      name(target.name);
      if (target.descendants)
        put(" *");
    });
    if (d.restartIdentity)
      put(*d.restartIdentity ? " RESTART IDENTITY" : " CONTINUE IDENTITY");
    behavior(d.dropBehavior);
    if (d.reuseStorage)
      put(*d.reuseStorage ? " REUSE STORAGE" : " DROP STORAGE");
  } else if (q.kind == StatementKind::Rename) {
    put(mysql() ? "RENAME TABLE " : "RENAME ");
    if (d.renames.empty())
      fail("RENAME missing pairs");
    separated(d.renames, [&](const auto &pair) {
      name(pair.first);
      put(" TO ");
      name(pair.second);
    });
  } else if (q.kind == StatementKind::Comment) {
    put("COMMENT ON ");
    object();
    put(" ");
    name(d.name);
    put(" IS ");
    expr(q, d.commentValue);
  } else
    fail("unsupported statement kind");
}
} // namespace
GeneratedSql generateSql(const ModelProgram &model, Dialect target,
                         GenerationOptions options) {
  return Printer(model, target, std::move(options)).run();
}
GeneratedSql generateExpression(const ModelProgram &model,QueryId query,ExprId expression,Dialect target,
                                GenerationOptions options,ExpressionColumnResolver columns,
                                ExpressionQueryResolver subqueries) {
  return Printer(model,target,std::move(options),std::move(columns),std::move(subqueries))
      .runExpression(query,expression);
}
GeneratedSql
generateTypeSql(const BasicTypeSpec<std::vector<std::string>> &spec,
                Dialect target) {
  ModelProgram p;
  p.sourceDialect = target;
  return Printer(p, target, {}).runType(spec);
}
} // namespace navidog::sql::compiler
