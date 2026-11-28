#include "parser.hpp"
#include "aggregate_functions.hpp"
#include <set>
namespace navidog::sql::compiler {
std::optional<QueryFeatures>
inspectQueryFeatures(const ModelStatement &q, Dialect dialect,
                     std::size_t maxVisits, std::function<bool()> shouldStop) {
  if (shouldStop && shouldStop())
    return {};
  QueryFeatures result;
  struct Work {
    ExprId id;
    bool covered;
  };
  std::vector<Work> work;
  for (const auto &p : q.projections)
    work.push_back({p.expression, false});
  for (const auto &o : q.orderBy)
    work.push_back({o.expression, false});
  if (q.having)
    work.push_back({q.having, false});
  for (const auto &window : q.namedWindows)
    for (auto id : window.keys)
      work.push_back({id, false});
  std::set<std::pair<ExprId, bool>> seen;
  while (!work.empty()) {
    auto current = work.back();
    work.pop_back();
    if (!seen.insert({current.id, current.covered}).second)
      continue;
    if (seen.size() > maxVisits || !current.id ||
        current.id >= q.expressions.size())
      return {};
    if ((seen.size() & 127u) == 0 && shouldStop && shouldStop())
      return {};
    const auto &e = q.expressions[current.id];
    if (e.outputColumn && *e.outputColumn < q.projections.size())
      work.push_back(
          {q.projections[*e.outputColumn].expression, current.covered});
    const bool filtered = e.kind == ExprKind::AggregateFilter,
               window = e.kind == ExprKind::Window;
    result.window |= window;
    const auto kind=callKind(e,dialect);
    if(e.kind==ExprKind::Call) {
      result.uncertainRoutine |= kind==RoutineKind::Unknown;
      result.tableFunction |= kind==RoutineKind::TableFunction;
    }
    const bool call = kind==RoutineKind::Aggregate;
    result.aggregate |= !current.covered && (filtered || call);
    for (std::size_t i = 0; i < e.operands.size(); ++i)
      work.push_back(
          {e.operands[i], i == 0 && (window || (filtered && current.covered))});
  }
  return result;
}
} // namespace navidog::sql::compiler
