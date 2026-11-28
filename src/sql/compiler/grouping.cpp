#include "parser.hpp"
#include "aggregate_functions.hpp"
#include <algorithm>
#include <map>
#include <set>
#include <sstream>
#include <tuple>

namespace navidog::sql::compiler {
namespace {
// Grouping is a semantic property of the query block. An aggregate inside
// OVER is analytic, whereas an aggregate in its arguments/keys still groups
// the input block. Keep both purposes explicit when traversing expressions.
class Grouping {
  Compilation &c;
  const ModelSelect &q;
  std::size_t visits = 0;
  std::map<std::string, std::size_t> shapes;
  std::map<ExprId, std::optional<std::size_t>> fingerprints;
  std::set<ExprId> active, reported;
  void budget() {
    if (++visits > c.options.limits.nodes * 32) {
      c.report(DiagnosticCode::ResourceLimit, q.origin,
               "grouping analysis visit limit exceeded");
      throw CompilationAborted{};
    }
    if ((visits & 127u) == 0)
      checkStop(c);
  }
  const ModelExpression &expression(ExprId id) { return q.expressions.at(id); }
  bool aggregate(const ModelExpression &e) const {
    return builtinAggregate(e, c.dialect);
  }
  bool knownScalar(const ModelExpression &e) const {
    return e.kind==ExprKind::Call &&
           (callKind(e,c.dialect)==RoutineKind::Scalar || callKind(e,c.dialect)==RoutineKind::Window);
  }

  std::optional<std::string> column(const ModelExpression &e) {
    if (e.binding) {
      if (e.binding->scope != q.id)
        return {}; // Outer constants are not local columns.
      const auto &table = q.tables.at(e.binding->table);
      if (table.definition &&
          e.binding->column < table.definition->columns.size())
        return std::to_string(e.binding->table) + ":" +
               table.definition->columns[e.binding->column].name;
      return {};
    }
    if (e.name.empty() || q.tables.empty())
      return {};
    std::optional<std::size_t> table;
    if (e.name.size() == 1 && q.tables.size() == 1)
      table = 0;
    else if (e.name.size() > 1) {
      const auto &qualifier = e.name[e.name.size() - 2];
      for (std::size_t i = 0; i < q.tables.size(); ++i) {
        const auto &t = q.tables[i];
        if ((!t.alias.empty() && t.alias == qualifier) ||
            (t.alias.empty() && !t.name.empty() &&
             t.name.back() == qualifier)) {
          if (table)
            return {};
          table = i;
        }
      }
    }
    if (!table)
      return {};
    return std::to_string(*table) + ":" + e.name.back();
  }
  std::optional<std::size_t> fingerprint(ExprId id, std::size_t depth = 0) {
    budget();
    if (auto found = fingerprints.find(id); found != fingerprints.end())
      return found->second;
    if (depth > c.options.limits.nesting || !active.insert(id).second) {
      c.report(DiagnosticCode::ResourceLimit, expression(id).origin,
               "grouping expression depth/cycle limit exceeded");
      throw CompilationAborted{};
    }
    const auto &e = expression(id);
    // Cast identities, routine OIDs/search paths and structurally identical
    // subqueries require richer type/catalog facts. Their arena IDs or raw
    // spellings alone cannot prove that two expressions differ.
    if (e.query || e.kind == ExprKind::Cast || e.kind == ExprKind::Convert ||
        (e.kind == ExprKind::Call && e.name.size() != 1)) {
      active.erase(id);
      fingerprints[id] = std::nullopt;
      return {};
    }
    std::optional<std::size_t> result;
    if (e.outputColumn && *e.outputColumn < q.projections.size())
      result =
          fingerprint(q.projections[*e.outputColumn].expression, depth + 1);
    else {
      std::ostringstream key;
      key << int(e.kind) << ':' << e.localSymbol << ':' << e.distinct << ':'
          << e.query << ':';
      if (e.withinGroup) key << "within:" << e.withinGroup->descending << ':' << e.withinGroup->nulls << ':';
      if (e.jsonConstructor) key << "json:" << int(e.jsonConstructor->nullPolicy) << ':' << e.jsonConstructor->returningJson << ':';
      bool complete = true;
      if (e.kind == ExprKind::Name && !e.localSymbol) {
        if (auto name = column(e))
          key << name->size() << ':' << *name;
        else
          complete = false;
      } else {
        auto value = e.value;
        if (e.kind == ExprKind::Number && !value.empty() &&
            value.find_first_not_of("0123456789") == std::string::npos) {
          auto start = value.find_first_not_of('0');
          value = start == std::string::npos ? "0" : value.substr(start);
        }
        key << value.size() << ':' << value << ':';
        for (const auto &name : e.name)
          key << name.size() << ':' << name;
      }
      for (auto child : e.operands) {
        auto shape = fingerprint(child, depth + 1);
        if (!shape) {
          complete = false;
          break;
        }
        key << '/' << *shape;
      }
      if (complete) {
        const auto value = key.str();
        result = shapes.emplace(value, shapes.size() + 1).first->second;
      }
    }
    active.erase(id);
    fingerprints[id] = result;
    return result;
  }
  struct Work {
    ExprId id;
    bool covered;
  };
  std::vector<Work> roots() {
    std::vector<Work> work;
    for (const auto &p : q.projections)
      work.push_back({p.expression, false});
    for (const auto &o : q.orderBy)
      work.push_back({o.expression, false});
    if (q.having)
      work.push_back({q.having, false});
    for(const auto &window:q.namedWindows)
      for(auto key:window.keys) work.push_back({key,false});
    return work;
  }
  void children(std::vector<Work> &work, const ModelExpression &e,
                bool covered) {
    if (e.outputColumn && *e.outputColumn < q.projections.size()) {
      work.push_back({q.projections[*e.outputColumn].expression, covered});
      return;
    }
    for (std::size_t i = 0; i < e.operands.size(); ++i)
      work.push_back(
          {e.operands[i],
           i == 0 && (e.kind == ExprKind::Window ||
                      (covered && e.kind == ExprKind::AggregateFilter))});
  }
  void distinctOrder() {
    if (c.dialect != Dialect::PostgreSQL || !q.distinct || q.orderBy.empty() ||
        std::any_of(q.projections.begin(), q.projections.end(),
                    [&](const auto &p) {
                      return expression(p.expression).kind == ExprKind::Star;
                    }))
      return;
    std::set<std::size_t> outputs;
    for (const auto &p : q.projections) {
      auto shape = fingerprint(p.expression);
      if (!shape)
        return;
      outputs.insert(*shape);
    }
    for (const auto &order : q.orderBy)
      if (auto shape = fingerprint(order.expression);
          shape && !outputs.count(*shape))
        c.report(DiagnosticCode::InvalidQueryShape,
                 expression(order.expression).origin,
                 "SELECT DISTINCT requires ORDER BY expressions to appear in "
                 "the SELECT list");
  }
  void report(const ModelExpression &e, ExprId id) {
    if (reported.insert(id).second)
      c.report(DiagnosticCode::InvalidQueryShape, e.origin,
               "column must appear in GROUP BY or be used inside an aggregate "
               "function");
  }

public:
  Grouping(Compilation &context, const ModelSelect &query)
      : c(context), q(query) {}
  void run() {
    if (q.kind != StatementKind::Select || q.setOperation ||
        (c.dialect != Dialect::PostgreSQL && c.dialect != Dialect::Oracle &&
         c.dialect != Dialect::SQLServer && c.dialect != Dialect::SQLite))
      return;
    distinctOrder();
    const auto features = inspectQueryFeatures(
        q, c.dialect, std::min<std::size_t>(c.options.limits.nodes, 250000) * 4,
        c.options.shouldStop);
    if (!features) {
      const bool stopped = c.options.shouldStop && c.options.shouldStop();
      c.report(stopped ? DiagnosticCode::Stopped
                       : DiagnosticCode::ResourceLimit,
               q.origin,
               "query-block feature inspection stopped or exceeded limits");
      throw CompilationAborted{};
    }
    if (c.dialect == Dialect::SQLite) {
      const bool unknownCall = std::any_of(
          q.expressions.begin(), q.expressions.end(), [&](const auto &e) {
            return e.kind == ExprKind::Call && !knownScalar(e) && !aggregate(e);
          });
      if (q.having && q.groupBy.empty() && !features->aggregate && !unknownCall)
        c.report(DiagnosticCode::InvalidQueryShape, expression(q.having).origin,
                 "SQLite HAVING requires an aggregate/grouped query");
      return;
    }
    if (q.groupBy.empty() && q.groupingSets.empty() && !q.having && !features->aggregate)
      return;
    std::set<std::size_t> grouped;
    bool incompleteGroups = false;
    std::map<std::size_t, std::set<std::string>> groupedColumns;
    for (auto id : q.groupBy) {
      const auto &reference = expression(id);
      if (reference.kind == ExprKind::Number && !reference.outputColumn &&
          (c.dialect == Dialect::PostgreSQL || c.dialect == Dialect::SQLite ||
           isMySqlDialect(c.dialect)))
        incompleteGroups = true;
      if (reference.outputColumn &&
          *reference.outputColumn < q.projections.size())
        id = q.projections[*reference.outputColumn].expression;
      const auto &group = expression(id);
      if (group.kind == ExprKind::Name && group.binding &&
          group.binding->scope == q.id) {
        const auto &table = q.tables.at(group.binding->table);
        if (table.definition &&
            group.binding->column < table.definition->columns.size())
          {
            const bool everySet=std::all_of(q.groupingSets.begin(),q.groupingSets.end(),[&](const auto &set){
              return std::any_of(set.begin(),set.end(),[&](auto id){
                const auto &ref=expression(id);const auto &value=ref.outputColumn?expression(q.projections.at(*ref.outputColumn).expression):ref;
                return value.binding && value.binding->scope==group.binding->scope && value.binding->table==group.binding->table && value.binding->column==group.binding->column;
              });
            });
            if(everySet)groupedColumns[group.binding->table].insert(table.definition->columns[group.binding->column].name);
          }
      }
      if (auto shape = fingerprint(id))
        grouped.insert(*shape);
      else
        incompleteGroups = true;
    }
    auto work = roots();
    std::set<std::pair<ExprId, bool>> seen;
    while (!work.empty()) {
      budget();
      auto current = work.back();
      work.pop_back();
      if (!seen.insert({current.id, current.covered}).second)
        continue;
      const auto &e = expression(current.id);
      if(e.kind==ExprKind::Call && !e.routineResolved) {
        auto op=builtinOperator(c.dialect,e.name,e.nameQuoted,e.operands.size());
        if(op && op->name=="GROUPING")for(auto argument:e.operands) {
          auto shape=fingerprint(argument);
          if(shape && !grouped.count(*shape))c.report(DiagnosticCode::InvalidQueryShape,expression(argument).origin,"GROUPING argument must match a GROUP BY expression");
        }
      }
      if (!current.covered && aggregate(e)) {
        const auto &call=e.kind==ExprKind::AggregateFilter?expression(e.operands.front()):e;
        if (call.withinGroup && call.operands.size()==2)
          work.push_back({call.operands.front(),false});
        continue;
      }
      // Unresolved / ambiguously classified UDFs can still be aggregates.
      if (!current.covered && e.kind == ExprKind::Call && !knownScalar(e))
        continue;
      if (auto shape = fingerprint(current.id); shape && grouped.count(*shape))
        continue;
      if (e.kind == ExprKind::Star) {
        // Do not expand unknown table shapes into invented columns.
        if (q.groupBy.empty() && !q.tables.empty() &&
            std::any_of(
                q.projections.begin(), q.projections.end(),
                [&](const auto &p) { return p.expression == current.id; }))
          report(e, current.id);
      } else if (e.kind == ExprKind::Name && !e.localSymbol &&
                 !e.outputColumn && (!e.binding || e.binding->scope == q.id)) {
        if (q.groupBy.empty())
          report(e, current.id);
        else if (!incompleteGroups && column(e)) {
          // PostgreSQL permits functional dependency only for actual table
          // primary keys. Until key metadata is known, defer base-table
          // cases; derived tables/CTEs never inherit that exception.
          bool defer = false;
          if (c.dialect == Dialect::PostgreSQL) {
            defer = true;
            if (e.binding && e.binding->table < q.tables.size()) {
              const auto &table = q.tables[e.binding->table];
              defer = !table.query &&
                      (!table.definition ||
                       (table.definition->kind == ObjectKind::Table &&
                        !table.definition->primaryKeyKnown));
              if (!table.query && table.definition &&
                  table.definition->primaryKeyKnown &&
                  !table.definition->primaryKey.empty()) {
                const auto &groupedNames = groupedColumns[e.binding->table];
                defer = std::all_of(
                    table.definition->primaryKey.begin(),
                    table.definition->primaryKey.end(),
                    [&](const auto &key) { return groupedNames.count(key); });
              }
            }
          }
          if (!defer)
            report(e, current.id);
        }
      }
      children(work, e, current.covered);
    }
  }
};
} // namespace
void validateGroupingSemantics(Compilation &c) {
  for (const auto &q : c.model.queries) {
    checkStop(c);
    Grouping(c, q).run();
  }
}
} // namespace navidog::sql::compiler
