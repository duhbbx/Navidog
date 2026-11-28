#include "parser.hpp"
#include "navidog/sql/compiler/operator_catalog.hpp"
#include <algorithm>
#include <cmath>
#include <iterator>
#include <set>
#include <tuple>
namespace navidog::sql::compiler {
ValueType typeCategory(const std::vector<std::string> &name, Dialect dialect) {
  if (name.size() != 1)
    return ValueType::Unknown;
  const auto n = asciiUpper(name[0]);
  for (auto word :
       {"INT", "INTEGER", "SMALLINT", "BIGINT", "TINYINT", "MEDIUMINT",
        "PLS_INTEGER", "BINARY_INTEGER", "INT2", "INT4", "INT8"})
    if (n == word)
      return ValueType::Integer;
  for (auto word : {"DECIMAL", "NUMERIC", "NUMBER", "FLOAT", "DOUBLE", "REAL",
                    "DOUBLE PRECISION", "BINARY_FLOAT", "BINARY_DOUBLE",
                    "MONEY", "SMALLMONEY"})
    if (n == word)
      return ValueType::Decimal;
  for (auto word :
       {"CHAR", "VARCHAR", "VARCHAR2", "NVARCHAR", "NVARCHAR2", "NCHAR", "TEXT",
        "NTEXT", "CLOB", "NCLOB", "CHARACTER", "CHARACTER VARYING"})
    if (n == word)
      return ValueType::String;
  if (n == "BIT" && dialect == Dialect::SQLServer)
    return ValueType::Integer;
  if (n == "BOOL" || n == "BOOLEAN")
    return ValueType::Boolean;
  for (auto word :
       {"BINARY", "VARBINARY", "BLOB", "BYTEA", "RAW", "IMAGE", "BIT"})
    if (n == word)
      return ValueType::Binary;
  return ValueType::Unknown;
}
void inferModelTypes(ModelSelect &query, Dialect dialect) {
  auto common = [](ValueType a, ValueType b) {
    if (a == ValueType::Null)
      return b;
    if (b == ValueType::Null)
      return a;
    if (a == b)
      return a;
    if ((a == ValueType::Integer && b == ValueType::Decimal) ||
        (b == ValueType::Integer && a == ValueType::Decimal))
      return ValueType::Decimal;
    return ValueType::Unknown;
  };
  for (std::size_t id = 1; id < query.expressions.size(); ++id) {
    auto &e = query.expressions[id];
    auto child = [&](std::size_t index) {
      return index < e.operands.size() &&
                     e.operands[index] < query.expressions.size()
                 ? query.expressions[e.operands[index]].type
                 : ValueType::Unknown;
    };
    if (e.outputColumn && *e.outputColumn < query.projections.size()) {
      const auto ref = query.projections[*e.outputColumn].expression;
      if (ref < query.expressions.size())
        e.type = query.expressions[ref].type;
      continue;
    }
    if (e.kind == ExprKind::Window || e.kind == ExprKind::AggregateFilter) {
      e.type = child(0);
      continue;
    }
    if (e.kind == ExprKind::TypedLiteral) {
      e.type=e.castType?valueCategory(resolveDeclaredType(*e.castType,dialect,TypeUse::Literal).type):ValueType::Unknown;
      continue;
    }
    if (e.kind == ExprKind::Cast || e.kind == ExprKind::Convert) {
      e.type = e.castType ? valueCategory(resolveDeclaredType(*e.castType,dialect,TypeUse::Cast).type) : typeCategory(e.name, dialect);
      continue;
    }
    if (e.kind == ExprKind::Unary) {
      e.type =
          e.value == "NOT" || e.value == "IS NULL" || e.value == "IS NOT NULL"
              ? ValueType::Boolean
              : child(0);
      continue;
    }
    if (e.kind == ExprKind::LikeEscape || e.kind == ExprKind::IsNull || e.kind == ExprKind::TruthTest || e.kind == ExprKind::In ||
        e.kind == ExprKind::Between || e.kind == ExprKind::Quantified ||
        e.kind == ExprKind::Exists) {
      e.type = ValueType::Boolean;
      continue;
    }
    if (e.kind == ExprKind::Binary) {
      if (e.value == "DIV") {
        e.type = ValueType::Integer;
      } else if (e.value == "+" || e.value == "-" || e.value == "*" ||
          e.value == "/" || e.value == "%") {
        const auto a = child(0), b = child(1);
        const bool numeric =
            (a == ValueType::Integer || a == ValueType::Decimal) &&
            (b == ValueType::Integer || b == ValueType::Decimal);
        if (numeric)
          e.type = e.value == "/" && (dialect == Dialect::Oracle ||
                                      isMySqlDialect(dialect))
                       ? ValueType::Decimal
                       : common(a, b);
      } else if (e.value == "CONCAT")
        e.type = ValueType::String;
      else
        e.type = ValueType::Boolean;
      continue;
    }
    if (e.kind == ExprKind::Case && !e.operands.empty()) {
      auto type = child(e.operands.size() - 1);
      for (std::size_t i = e.value == "simple" ? 2 : 1;
           i + 1 < e.operands.size(); i += 2)
        type = common(type, child(i));
      e.type = type;
      continue;
    }
    if (e.kind != ExprKind::Call || e.name.empty())
      continue;
    if(e.routineResolved) continue;
    auto op=builtinOperator(dialect,e.name,e.nameQuoted,e.operands.size());
    if(!op)continue;
    if(op->fixedResult!=ValueType::Unknown)e.type=op->fixedResult;
    else if(op->resultFromFirst)e.type=child(0);
    else if(op->commonResult) {
      auto type=ValueType::Null;
      for(std::size_t i=0;i<e.operands.size();++i)type=common(type,child(i));
      e.type=type;
    }
  }
}
namespace {
bool builtinCandidate(const ModelExpression &e,Dialect dialect) {
  return !e.routineResolved && e.kind==ExprKind::Call &&
         bool(builtinOperator(dialect,e.name,e.nameQuoted,e.operands.size()));
}
bool aggregate(const ModelExpression &e,Dialect dialect) {
  return callKind(e,dialect)==RoutineKind::Aggregate;
}
bool windowOnly(const ModelExpression &e,Dialect dialect) {
  return callKind(e,dialect)==RoutineKind::Window;
}
class Semantics {
  Compilation &c;
  std::size_t visits = 0;
  std::set<std::tuple<std::size_t, std::size_t, std::string>> reported;
  void report(const ModelExpression &e, const std::string &message,
              DiagnosticCode code = DiagnosticCode::InvalidQueryShape) {
    if (reported.insert({e.origin.begin, e.origin.end, message}).second)
      c.report(code, e.origin, message);
  }
  const ModelExpression &expr(const ModelSelect &q, ExprId id) {
    if (!id || id >= q.expressions.size()) {
      c.report(DiagnosticCode::InvalidQueryShape, q.origin,
               "invalid expression reference in semantic analysis");
      throw CompilationAborted{};
    }
    return q.expressions[id];
  }
  void frame(const ModelSelect &q, const ModelExpression &e) {
    if (!e.window || e.operands.empty())
      return;
    const auto &spec = *e.window;
    const ModelExpression declarationOnly;
    const auto &base = e.operands[0] ? expr(q, e.operands[0]) : declarationOnly;
    const auto &call =
        base.kind == ExprKind::AggregateFilter && !base.operands.empty()
            ? expr(q, base.operands[0])
            : base;
    const auto kind = callKind(call, c.dialect);
    if (kind == RoutineKind::Scalar || kind == RoutineKind::TableFunction ||
        kind == RoutineKind::Procedure)
      report(call, "OVER requires an aggregate or window function");
    const auto name = call.name.empty() ? "" : asciiUpper(call.name.back());
    const bool strictOrder =
        c.dialect == Dialect::SQLServer || c.dialect == Dialect::Oracle;
    if (strictOrder && spec.ordering.empty() &&
        (name == "ROW_NUMBER" || name == "RANK" || name == "DENSE_RANK" ||
         name == "NTILE" || name == "LAG" || name == "LEAD"))
      report(e,
             "this window function requires ORDER BY in the current dialect");
    if (call.distinct) {
      if (c.dialect == Dialect::Oracle) {
        if (!spec.ordering.empty() || !spec.frameUnit.empty())
          report(e, "Oracle DISTINCT analytic aggregates cannot specify ORDER "
                    "BY or a frame");
      } else
        report(e,
               "DISTINCT window aggregates are not supported by this dialect "
               "baseline",
               DiagnosticCode::UnsupportedSyntax);
    }
    const bool offset = spec.start == WindowBound::Preceding ||
                        spec.start == WindowBound::Following ||
                        spec.end == WindowBound::Preceding ||
                        spec.end == WindowBound::Following;
    if (offset && spec.frameUnit == "RANGE" && spec.ordering.size() != 1)
      report(e,
             "RANGE with an offset requires exactly one ORDER BY expression");
    if (offset && spec.frameUnit == "RANGE" && c.dialect == Dialect::SQLServer)
      report(e, "SQL Server RANGE does not accept numeric PRECEDING/FOLLOWING "
                "offsets");
    if (strictOrder && !spec.frameUnit.empty() && windowOnly(call, c.dialect) &&
        name != "FIRST_VALUE" && name != "LAST_VALUE" && name != "NTH_VALUE")
      report(
          e,
          "this window function cannot specify a frame in the current dialect");
    if (spec.frameUnit == "GROUPS" && spec.ordering.empty() &&
        c.dialect == Dialect::PostgreSQL)
      report(e, "GROUPS frame requires ORDER BY in PostgreSQL");
    const auto first = 1 + spec.partitions + spec.ordering.size();
    for (std::size_t i = first; i < e.operands.size(); ++i) {
      std::vector<ExprId> dependencies{e.operands[i]};
      std::set<ExprId> seen;
      while (!dependencies.empty()) {
        const auto id = dependencies.back();
        dependencies.pop_back();
        if (!seen.insert(id).second)
          continue;
        const auto &dependency = expr(q, id);
        if (dependency.kind == ExprKind::Name && !dependency.localSymbol)
          report(e, "window frame offset cannot refer to a row column");
        if(dependency.kind==ExprKind::Window || dependency.kind==ExprKind::AggregateFilter ||
           (dependency.kind==ExprKind::Call && aggregate(dependency,c.dialect)))
          report(e,"window frame offset cannot contain an aggregate or window function");
        if (dependency.query)
          report(e,
                 "subquery in window frame offset requires dialect-specific "
                 "validation",
                 DiagnosticCode::UnsupportedSyntax);
        dependencies.insert(dependencies.end(), dependency.operands.begin(),
                            dependency.operands.end());
        if (seen.size() > c.options.limits.nodes) {
          c.report(DiagnosticCode::ResourceLimit, e.origin,
                   "window offset dependency limit exceeded");
          throw CompilationAborted{};
        }
      }
      auto value = expr(q, e.operands[i]);
      bool negative = false;
      if (value.kind == ExprKind::Unary &&
          (value.value == "-" || value.value == "+") &&
          value.operands.size() == 1) {
        negative = value.value == "-";
        value = expr(q, value.operands[0]);
      }
      if (value.kind == ExprKind::Number) {
        const bool integer =
            value.value.find_first_not_of("0123456789") == std::string::npos;
        const bool integralFrame =
            spec.frameUnit == "ROWS" || spec.frameUnit == "GROUPS";
        std::optional<long double> numeric;
        try {
          const auto v = std::stold(value.value);
          if (std::isfinite(v))
            numeric = v;
        } catch (...) {
        }
        // PostgreSQL coerces ROWS/GROUPS expressions to int8 (including
        // numeric literals); rejecting every decimal would be a false error.
        const bool pgIntegerCoercion =
            c.dialect == Dialect::PostgreSQL && integralFrame;
        if (negative &&
            ((numeric && *numeric >= (pgIntegerCoercion ? .5L : 0.L) &&
              *numeric != 0) ||
             (!numeric && integer &&
              value.value.find_first_not_of('0') != std::string::npos)))
          report(
              e,
              "window frame offset must be nonnegative after dialect coercion");
        if (integralFrame && !pgIntegerCoercion && !integer) {
          const bool sqliteInteger = c.dialect == Dialect::SQLite && numeric &&
                                     std::floor(*numeric) == *numeric;
          if (!sqliteInteger)
            report(e, "ROWS/GROUPS frame offset must be an integer");
        }
      }
    }
  }
  void arity(const ModelExpression &e) {
    if (e.routineResolved && e.routineSchema!="pg_catalog")
      return;
    auto op=builtinOperator(c.dialect,e.name,e.nameQuoted,e.operands.size());
    if(!op)return;
    if (c.dialect==Dialect::PostgreSQL && e.distinct && op->kind==RoutineKind::Scalar &&
        (!e.routineResolved || e.routineKind==RoutineKind::Scalar))
      report(e,"DISTINCT requires an aggregate rather than a scalar function");
    if (op->requiresWithinGroup && !e.withinGroup) {
      report(e,op->name+" requires WITHIN GROUP (ORDER BY …)");
      return;
    }
    if (e.routineResolved) return;
    const auto count=e.operands.size();
    const bool multi=(isMySqlDialect(c.dialect)) && op->name=="COUNT" && e.distinct && count>1;
    const bool sqliteCount=c.dialect==Dialect::SQLite && op->name=="COUNT" && count==0;
    if(!multi && !sqliteCount && (count<op->minArguments || count>op->maxArguments))
      report(e,"invalid argument count for "+op->name,DiagnosticCode::RoutineArgumentCount);
  }

  void condition(const ModelSelect &q, ExprId id) {
    if (!id ||
        (c.dialect != Dialect::PostgreSQL && c.dialect != Dialect::Oracle &&
         c.dialect != Dialect::SQLServer))
      return;
    const auto &e = expr(q, id);
    const bool predicateOnly =
        c.dialect == Dialect::SQLServer ||
        (c.dialect == Dialect::Oracle &&
         q.context.environment == SemanticEnvironment::Sql);
    if (predicateOnly) {
      if (e.kind == ExprKind::Boolean) {
        report(e,
               "SQL boolean literals are unavailable in this dialect baseline");
        return;
      }
      bool predicateCall = false;
      if (e.kind == ExprKind::Call && e.name.size() == 1 &&
          (e.nameQuoted.empty() || !e.nameQuoted[0])) {
        const auto name = asciiUpper(e.name.back());
        predicateCall = c.dialect == Dialect::SQLServer
                            ? (name == "CONTAINS" || name == "FREETEXT")
                            : (name == "REGEXP_LIKE" || name == "LNNVL" ||
                               name == "JSON_EXISTS" || name == "XMLEXISTS");
      }
      const bool predicate =
          e.kind == ExprKind::LikeEscape || e.kind == ExprKind::IsNull || e.kind == ExprKind::In ||
          e.kind == ExprKind::Between || e.kind == ExprKind::Exists ||
          e.kind == ExprKind::Quantified ||
          (e.kind == ExprKind::Unary && e.value == "NOT") ||
          (e.kind == ExprKind::Binary && e.type == ValueType::Boolean) ||
          predicateCall;
      if (!predicate)
        report(e, "condition requires a predicate; scalar values and NULL "
                  "cannot be used as conditions in this dialect baseline");
      return;
    }
    if (e.type == ValueType::Unknown || e.type == ValueType::Null ||
        e.type == ValueType::Boolean)
      return;
    // Untyped PG string literals can acquire boolean type from context.
    if (c.dialect == Dialect::PostgreSQL && e.kind == ExprKind::String)
      return;
    report(e,
           "condition requires a boolean/predicate expression in this dialect");
  }
  void sqlBooleanValues(const ModelSelect &q) {
    if (c.dialect != Dialect::SQLServer &&
        !(c.dialect == Dialect::Oracle &&
          q.context.environment == SemanticEnvironment::Sql))
      return;
    auto value = [&](ExprId id) {
      if (id && expr(q, id).type == ValueType::Boolean) {
        if (expr(q, id).kind == ExprKind::Boolean) {
          report(
              expr(q, id),
              "SQL boolean literals are unavailable in this dialect baseline");
          return;
        }
        report(expr(q, id),
               "this dialect baseline cannot return/store a predicate as a "
               "scalar value; use an explicit CASE expression");
      }
    };
    if (q.context.environment != SemanticEnvironment::ProceduralCondition)
      for (const auto &p : q.projections)
        value(p.expression);
    for (const auto &p : q.returning)
      value(p.expression);
    if (q.outputInto)
      for (const auto &p:q.outputInto->projections) value(p.expression);
    for (const auto &row : q.rows)
      for (auto id : row)
        value(id);
    for (const auto &a : q.assignments)
      value(a.value);
    for (const auto &a : q.conflict.assignments)
      value(a.value);
    for (const auto &branch : q.mergeClauses) {
      for (auto id : branch.values)
        value(id);
      for (const auto &a : branch.assignments)
        value(a.value);
    }
    for (auto id : q.groupBy)
      value(id);
    for (const auto &o : q.orderBy)
      value(o.expression);
    if (q.ddl) {
      auto column = [&](const auto &col) {
        value(col.defaultValue);
        value(col.generated);
      };
      for (const auto &col : q.ddl->definitions)
        column(col);
      for (const auto &a : q.ddl->actions) {
        value(a.value);
        if (a.column)
          column(*a.column);
      }
    }
    for (const auto &e : q.expressions)
      if (e.kind == ExprKind::Boolean)
        report(e,
               "SQL boolean literals are unavailable in this dialect baseline");
  }
  void conditions(const ModelSelect &q) {
    if (q.context.environment == SemanticEnvironment::ProceduralCondition)
      for (const auto &p : q.projections)
        condition(q, p.expression);
    condition(q, q.mergeOn);
    for (const auto &clause : q.mergeClauses) {
      condition(q, clause.condition);
      condition(q, clause.deleteWhere);
    }
    condition(q, q.where);
    condition(q, q.having);
    condition(q, q.conflict.where);
    condition(q, q.conflict.targetWhere);
    for (const auto &r : q.relations)
      condition(q, r.condition);
    for (const auto &e : q.expressions) {
      if ((e.kind == ExprKind::Binary &&
           (e.value == "AND" || e.value == "OR")) ||
          (e.kind == ExprKind::Unary && e.value == "NOT") ||
          (e.kind == ExprKind::TruthTest && c.dialect == Dialect::PostgreSQL))
        for (auto child : e.operands)
          condition(q, child);
      if (e.kind == ExprKind::Case && e.value == "searched")
        for (std::size_t i = 0; i + 1 < e.operands.size(); i += 2)
          condition(q, e.operands[i]);
    }
    if (q.ddl) {
      const auto &d = *q.ddl;
      condition(q, d.predicate);
      auto constraint = [&](const auto &k) { condition(q, k.check); };
      auto column = [&](const auto &col) {
        for (const auto &k : col.constraints)
          constraint(k);
      };
      for (const auto &col : d.definitions)
        column(col);
      for (const auto &k : d.constraints)
        constraint(k);
      for (const auto &a : d.actions) {
        if (a.column)
          column(*a.column);
        if (a.constraint)
          constraint(*a.constraint);
      }
    }
  }
  void walk(const ModelSelect &q, ExprId root, bool allowWindow,
            bool allowAggregate, std::string_view schemaClause = {}, bool rowLimit=false) {
    struct Work {
      ExprId id;
      bool window, aggregate, covered;
    };
    std::vector<Work> work;
    if (root)
      work.push_back({root, false, false, false});
    std::set<std::tuple<ExprId, bool, bool, bool>> seen;
    while (!work.empty()) {
      auto w = work.back();
      work.pop_back();
      if (!seen.insert({w.id, w.window, w.aggregate, w.covered}).second)
        continue;
      if (++visits > c.options.limits.nodes * 32) {
        c.report(DiagnosticCode::ResourceLimit, {},
                 "semantic expression visit limit exceeded");
        throw CompilationAborted{};
      }
      if ((visits & 127u) == 0)
        checkStop(c);
      const auto &e = expr(q, w.id);
      if (rowLimit && e.kind==ExprKind::Name && !e.localSymbol &&
          ((e.binding && e.binding->scope==q.id) || (!e.binding && !q.scope.parent)))
        report(e,"PostgreSQL pagination cannot reference a column of the current query");
      if (e.query && !schemaClause.empty())
        report(e, "subqueries are not allowed in " + std::string(schemaClause));
      if (c.dialect==Dialect::PostgreSQL && e.kind==ExprKind::Call &&
          (schemaClause=="index expressions" || schemaClause=="index predicates")) {
        const auto volatility=callVolatility(e,c.dialect);
        if (volatility==FunctionVolatility::Stable || volatility==FunctionVolatility::Volatile)
          report(e,"PostgreSQL index expressions and predicates require IMMUTABLE functions");
      }
      if (e.outputColumn && *e.outputColumn < q.projections.size())
        work.push_back({q.projections[*e.outputColumn].expression, w.window,
                        w.aggregate, false});
      const bool win = e.kind == ExprKind::Window,
                 filtered = e.kind == ExprKind::AggregateFilter,
                 agg = (aggregate(e, c.dialect) || filtered) && !w.covered;
      if (filtered) {
        if (e.operands.size() != 2 ||
            expr(q, e.operands[0]).kind != ExprKind::Call) {
          report(e, "FILTER requires an aggregate call");
          continue;
        }
        const auto &call = expr(q, e.operands[0]);
        if (windowOnly(call, c.dialect) ||
            callKind(call,c.dialect)==RoutineKind::Scalar)
          report(e, "FILTER is only allowed with aggregate functions");
        condition(q, e.operands[1]);
        // SQLite accepts grouped aggregates in a window's FILTER.
        // PostgreSQL accepts them in window arguments but forbids them here.
        walk(q, e.operands[1], false,
             c.dialect == Dialect::SQLite && allowAggregate && w.covered && w.window && !w.aggregate,
             schemaClause);
      }
      if (win) {
        if (!allowWindow || w.window || w.aggregate)
          report(e,
                 w.window ? "window functions cannot be nested"
                 : w.aggregate
                     ? "window functions cannot be arguments of an aggregate"
                     : "window functions are not allowed in this SQL clause");
        frame(q, e);
      }
      if (agg) {
        if (!allowAggregate)
          report(e, "aggregate functions are not allowed in this SQL clause");
        if (w.aggregate)
          report(
              e,
              "aggregate functions cannot be nested at the same query level");
      }
      if (windowOnly(e, c.dialect) && !w.covered)
        report(e,
               "window function " + asciiUpper(e.name.back()) + " requires OVER");
      if(builtinCandidate(e,c.dialect) && asciiUpper(e.name.back())=="GROUPING") {
        if(!allowAggregate || w.aggregate)report(e,"GROUPING is only allowed in grouped output, HAVING or ORDER BY");
        if(q.groupBy.empty() && q.groupingSets.empty())report(e,"GROUPING requires an explicit grouped query");
      }
      if (e.kind == ExprKind::Call)
        arity(e);
      for (std::size_t i = 0; i < e.operands.size(); ++i) {
        if (filtered && i == 1)
          continue;
        work.push_back({e.operands[i], w.window || win, w.aggregate || agg,
                        (win || filtered) && i == 0});
      }
    }
  }

public:
  explicit Semantics(Compilation &context) : c(context) {}
  void mysqlTemporalOperands(const ModelSelect &q) {
    if(!isMySqlDialect(c.dialect))return;
    std::vector<bool> accepted(q.expressions.size());
    const auto rangeOffset=[&](ExprId id) {
      accepted.at(id)=true;
      if (c.dialect==Dialect::MariaDB && expr(q,id).kind==ExprKind::Interval)
        report(expr(q,id),"MariaDB RANGE frames require numeric offsets and do not accept INTERVAL");
    };
    for(std::size_t id=1;id<q.expressions.size();++id) {
      if((id&255u)==0)checkStop(c);
      const auto &parent=q.expressions[id];
      if(parent.kind==ExprKind::Binary && parent.operands.size()==2 && (parent.value=="+" || parent.value=="-")) {
        if(expr(q,parent.operands[0]).kind!=ExprKind::Interval)accepted.at(parent.operands[1])=true;
        if(parent.value=="+" && expr(q,parent.operands[1]).kind!=ExprKind::Interval)accepted.at(parent.operands[0])=true;
      }
      if(parent.kind==ExprKind::Window && parent.window && parent.window->frameUnit=="RANGE") {
        const auto first=1+parent.window->partitions+parent.window->ordering.size();
        for(std::size_t i=first;i<parent.operands.size();++i)rangeOffset(parent.operands[i]);
      }
      if(parent.kind==ExprKind::Call && parent.name.size()==1 && builtinCandidate(parent,c.dialect) && parent.operands.size()==2) {
        const auto name=asciiUpper(parent.name[0]);
        if(name=="DATE_ADD" || name=="DATE_SUB" || name=="ADDDATE" || name=="SUBDATE") {
          accepted.at(parent.operands[1])=true;
          if((name=="DATE_ADD" || name=="DATE_SUB") && expr(q,parent.operands[1]).kind!=ExprKind::Interval)
            report(parent,"DATE_ADD / DATE_SUB require INTERVAL expression unit as their second argument");
        }
      }
    }
    for(const auto &window:q.namedWindows)if(window.specification.frameUnit=="RANGE") {
      const auto first=window.specification.partitions+window.specification.ordering.size();
      for(std::size_t i=first;i<window.keys.size();++i)rangeOffset(window.keys[i]);
    }
    for(std::size_t id=1;id<q.expressions.size();++id)if(q.expressions[id].kind==ExprKind::Interval && !accepted[id])
      report(q.expressions[id],"MySQL interval must be used by date arithmetic, a supported date-add/subtract operand or RANGE frame");
  }
  void schemaExpressions(const ModelSelect &q) {
    if (!q.ddl)
      return;
    const auto &ddl = *q.ddl;
    auto scalar = [&](ExprId id, std::string_view clause = {}) {
      walk(q, id, false, false, clause);
    };
    auto constraint = [&](const auto &key) { scalar(key.check, "CHECK constraints"); };
    auto column = [&](const auto &col) {
      if (c.dialect==Dialect::TiDB && col.generated && col.identity==IdentityKind::AutoIncrement)
        c.report(DiagnosticCode::InvalidQueryShape,col.origin,"TiDB generated columns cannot also be AUTO_INCREMENT");
      if (c.dialect==Dialect::TiDB && col.type.name.size()==1 &&
          (asciiUpper(col.type.name.front())=="YEAR" || asciiUpper(col.type.name.front())=="SQL_TSI_YEAR") &&
          col.type.arguments.size()==1) {
        const auto &digits=col.type.arguments.front().value;
        const auto nonzero=digits.find_first_not_of('0');
        if (nonzero==std::string::npos || digits.substr(nonzero)!="4")
          c.report(DiagnosticCode::InvalidQueryShape,col.origin,"TiDB supports only YEAR or YEAR(4) columns");
      }
      scalar(col.defaultValue, "column defaults");
      scalar(col.generated, "generated column expressions");
      scalar(col.identityStart);
      scalar(col.identityIncrement);
      for (const auto &key : col.constraints)
        constraint(key);
    };
    for (const auto &col : ddl.definitions)
      column(col);
    for (const auto &key : ddl.constraints)
      constraint(key);
    for (const auto &action : ddl.actions) {
      scalar(action.value, action.kind == AlterKind::SetDefault ? "column defaults" : "");
      if (action.column)
        column(*action.column);
      if (action.constraint)
        constraint(*action.constraint);
    }
    for (const auto &key : ddl.keys) {
      scalar(key.expression, "index expressions");
      scalar(key.prefixLength);
    }
    std::set<std::string> indexNames;
    for (const auto &index:ddl.tableIndexes) {
      if (index.name) {
        const auto indexName=asciiUpper(*index.name);
        if (indexName=="PRIMARY" || !indexNames.insert(indexName).second)
          c.report(DiagnosticCode::DuplicateAlias,index.origin,"invalid or repeated index name: "+*index.name);
      }
      std::set<std::string> columns;
      for (const auto &key:index.keys) {
        const auto &part=expr(q,key.expression);
        if (!part.name.empty() && !columns.insert(asciiUpper(part.name.front())).second)
          c.report(DiagnosticCode::DuplicateAlias,part.origin,"duplicate table index column: "+part.name.front());
        scalar(key.expression,"index expressions");
        scalar(key.prefixLength);
      }
    }
    scalar(ddl.predicate, "index predicates");
    for (const auto &option : ddl.sequenceOptions)
      scalar(option.second);
  }
  void mysqlAutoIncrement(const ModelSelect &q) {
    if ((c.dialect!=Dialect::MySQL && c.dialect!=Dialect::MariaDB) ||
        q.kind!=StatementKind::Create || !q.ddl || q.ddl->object!=ObjectKind::Table)
      return;
    const auto &ddl=*q.ddl;
    const auto autoColumn=std::find_if(ddl.definitions.begin(),ddl.definitions.end(),[](const auto &column) {
      return column.identity==IdentityKind::AutoIncrement;
    });
    if (autoColumn==ddl.definitions.end()) return;
    for (auto other=std::next(autoColumn);other!=ddl.definitions.end();++other)
      if (other->identity==IdentityKind::AutoIncrement)
        c.report(DiagnosticCode::InvalidQueryShape,other->origin,"only one AUTO_INCREMENT column is allowed");
    if (autoColumn->defaultValue)
      report(expr(q,autoColumn->defaultValue),"AUTO_INCREMENT columns cannot have a DEFAULT value");
    if (q.source) return;

    const auto name=asciiUpper(autoColumn->name);
    const auto matchingConstraint=[&](const auto &constraint) {
      if (constraint.kind==ConstraintKind::Check) return false;
      return std::any_of(constraint.columns.begin(),constraint.columns.end(),[&](const auto &field) {
        return asciiUpper(field)==name;
      });
    };
    // Some engines can create a supporting index for a foreign key. Engine
    // restrictions on the order within a composite key are server-specific.
    if (std::any_of(autoColumn->constraints.begin(),autoColumn->constraints.end(),matchingConstraint) ||
        std::any_of(ddl.constraints.begin(),ddl.constraints.end(),matchingConstraint)) return;
    for (const auto &index:ddl.tableIndexes)
      for (const auto &key:index.keys) {
        const auto &field=expr(q,key.expression);
        if (field.kind==ExprKind::Name && field.name.size()==1 && asciiUpper(field.name.front())==name)
          return;
      }
    c.report(DiagnosticCode::InvalidQueryShape,autoColumn->origin,"AUTO_INCREMENT requires an index on this column");
  }
  void mysqlView(const ModelSelect &view) {
    if ((c.dialect != Dialect::MySQL && c.dialect != Dialect::MariaDB) ||
        view.kind != StatementKind::Create || !view.ddl ||
        view.ddl->object != ObjectKind::View || !view.source) return;
    const auto &source = c.model.queries.at(view.source - 1);
    const auto &declared = view.ddl->columns;
    std::set<std::string> names;
    const auto duplicate = [&](const std::string &name, SourceRange origin) {
      if (!name.empty() && !names.insert(asciiUpper(name)).second)
        c.report(DiagnosticCode::DuplicateAlias, origin, "duplicate view column: " + name);
    };
    if (!declared.empty()) {
      for (const auto &name : declared) duplicate(name, view.origin);
      if (!source.setOperation && !source.projections.empty() &&
          std::none_of(source.projections.begin(), source.projections.end(), [&](const auto &projection) {
            return expr(source, projection.expression).kind == ExprKind::Star;
          }) && declared.size() != source.projections.size())
        c.report(DiagnosticCode::InvalidQueryShape, view.origin,
                 "view column list does not match the SELECT output count");
    } else {
      for (const auto &projection : source.projections) {
        const auto &value = expr(source, projection.expression);
        if (!projection.alias.empty()) duplicate(projection.alias, value.origin);
        else if (value.kind == ExprKind::Name && !value.name.empty()) duplicate(value.name.back(), value.origin);
      }
    }

    std::vector<QueryId> pending{view.source};
    std::set<QueryId> seen;
    while (!pending.empty()) {
      checkStop(c);
      const auto id = pending.back();
      pending.pop_back();
      if (!id || !seen.insert(id).second) continue;
      const auto &query = c.model.queries.at(id - 1);
      for (const auto &value : query.expressions) {
        if ((++visits & 63u) == 0) checkStop(c);
        if (value.kind == ExprKind::Variable || value.kind == ExprKind::Parameter || value.localSymbol)
          report(value, "MySQL view definitions cannot reference variables or parameters");
        if (value.query) pending.push_back(value.query);
      }
      for (const auto &relation : query.tables) {
        if (relation.definition && relation.definition->sessionTemporary && !relation.name.empty()) {
          const auto origin=relation.sourceNameOrigin.end?relation.sourceNameOrigin:query.origin;
          c.report(DiagnosticCode::InvalidQueryShape,origin,
                   "view definitions cannot reference temporary tables: "+relation.name.back());
        }
        if (relation.query) pending.push_back(relation.query);
      }
      for (const auto &cte : query.ctes) pending.push_back(cte.query);
      if (query.setOperation) {
        pending.push_back(query.setOperation->left);
        pending.push_back(query.setOperation->right);
      }
    }
  }
  void run() {
    for (const auto &q : c.model.queries) {
      mysqlTemporalOperands(q);
      checkStop(c);
      conditions(q);
      sqlBooleanValues(q);
      for (const auto &p : q.projections)
        walk(q, p.expression, q.kind == StatementKind::Select, true);
      for (const auto &o : q.orderBy)
        walk(q, o.expression, q.kind == StatementKind::Select, true);
      walk(q, q.mergeOn, false, false);
      for (const auto &clause : q.mergeClauses) {
        walk(q, clause.condition, false, false);
        walk(q, clause.deleteWhere, false, false);
        for (auto id : clause.values)
          walk(q, id, false, false);
        for (const auto &a : clause.assignments)
          walk(q, a.value, false, false);
      }
      walk(q, q.where, false, false);
      if (q.trigger) walk(q, q.trigger->when, false, false);
      walk(q, q.having, false, true);
      for (auto id : q.groupBy)
        walk(q, id, false, false);
      for (const auto &r : q.relations)
        walk(q, r.condition, false, false);
      for (const auto &row : q.rows)
        for (auto id : row)
          walk(q, id, false, false);
      for (const auto &a : q.assignments)
        walk(q, a.value, false, false);
      for (auto id : q.arguments)
        walk(q, id, false, false);
      for (auto id : q.conflict.targets)
        walk(q, id, false, false);
      walk(q, q.conflict.targetWhere, false, false);
      walk(q, q.conflict.where, false, false);
      for (const auto &a : q.conflict.assignments)
        walk(q, a.value, false, false);
      walk(q, q.limit, false, false,{},c.dialect==Dialect::PostgreSQL);
      walk(q, q.offset, false, false,{},c.dialect==Dialect::PostgreSQL);
      if (c.dialect==Dialect::PostgreSQL) {
        for (const auto id:{q.limit,q.offset}) if (id) {
          const auto &bound=expr(q,id);
          const auto *literal=&bound;
          bool negative=false;
          while (literal->kind==ExprKind::Unary && literal->operands.size()==1 &&
                 (literal->value=="-" || literal->value=="+")) {
            if (literal->value=="-") negative=!negative;
            literal=&expr(q,literal->operands.front());
          }
          if (negative && literal->kind==ExprKind::Number &&
              literal->value.find_first_not_of("0123456789")==std::string::npos &&
              literal->value.find_first_not_of('0')!=std::string::npos)
            report(bound,"PostgreSQL pagination integer bound must be nonnegative");
        }
      }
      if (c.dialect==Dialect::SQLServer) {
        for (const auto id:{q.limit,q.offset}) if (id) {
          const auto &bound=expr(q,id);
          const auto *literal=&bound;
          bool negative=false;
          while (literal->kind==ExprKind::Unary && literal->operands.size()==1 &&
                 (literal->value=="-" || literal->value=="+")) {
            if (literal->value=="-") negative=!negative;
            literal=&expr(q,literal->operands.front());
          }
          // Check known literals only; arithmetic/parameter evaluation belongs
          // to the server. Preserve TOP 0 and signed zero, but FETCH needs >0.
          if (literal->kind==ExprKind::Number) {
            const auto mantissa=literal->value.substr(0,literal->value.find_first_of("eE"));
            const bool zero=mantissa.find_first_not_of("0.")==std::string::npos;
            if (negative && !zero)
              report(bound,"SQL Server pagination literal must be nonnegative");
            else if (zero && id==q.limit && q.limitSyntax==LimitSyntax::Fetch)
              report(bound,"SQL Server FETCH row count must be greater than zero");
          }
        }
      }
      for (const auto &p : q.returning)
        walk(q,p.expression,false,false,c.dialect==Dialect::SQLServer?"SQL Server OUTPUT":"");
      if (q.outputInto)
        for (const auto &p:q.outputInto->projections)
          walk(q,p.expression,false,false,"SQL Server OUTPUT");
      schemaExpressions(q);
      mysqlAutoIncrement(q);
      mysqlView(q);
      for (const auto &window : q.namedWindows) {
        ModelExpression declaration;declaration.window=window.specification;declaration.origin=window.origin;
        declaration.operands.push_back(0);declaration.operands.insert(declaration.operands.end(),window.keys.begin(),window.keys.end());
        frame(q,declaration);
        for (auto id : window.keys)walk(q,id,false,true);
      }
      for (const auto &e : q.expressions)
        if (e.kind == ExprKind::Call && e.distinct && e.operands.size() == 1 &&
            expr(q, e.operands[0]).kind == ExprKind::Star)
          report(e, "DISTINCT cannot be applied to the * wildcard");
    }
  }
};
} // namespace
void validateModelSemantics(Compilation &c) {
  Semantics(c).run();
  validateGroupingSemantics(c);
}
} // namespace navidog::sql::compiler
