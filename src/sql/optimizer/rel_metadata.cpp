#include "navidog/sql/optimizer/rel_metadata.hpp"
#include "navidog/sql/compiler/operator_catalog.hpp"
#include "optimizer_internal.hpp"
#include <algorithm>
#include <array>
#include <cmath>
#include <map>
#include <set>
#include <tuple>
#include <utility>

namespace navidog::sql::optimizer {
namespace {
using Count=std::uint64_t;
bool equal(const ColumnRef &a,const ColumnRef &b) {
  return a.query==b.query && a.table==b.table && a.name==b.name;
}
std::optional<Count> add(std::optional<Count> a,std::optional<Count> b) {
  if(!a || !b || *a>UINT64_MAX-*b)return {};
  return *a+*b;
}
Count subtractFloor(Count a, Count b) {
  return a > b ? a - b : 0;
}
std::optional<Count> multiply(std::optional<Count> a,std::optional<Count> b) {
  if((a && *a==0) || (b && *b==0))return Count{0};
  if(!a || !b || (*b && *a>UINT64_MAX / *b))return {};
  return *a * *b;
}
std::optional<Count> number(std::optional<double> n) {
  if(!n || !std::isfinite(*n) || *n<0 || *n>9007199254740991. || std::floor(*n)!=*n)return {};
  return Count(*n);
}
bool contains(const std::vector<ColumnRef> &cols,const ColumnRef &c) {
  return std::any_of(cols.begin(),cols.end(),[&](const auto &v){return equal(v,c);});
}
std::optional<std::size_t> columnOrdinal(const RelNode &node, const ColumnRef &column) {
  std::optional<std::size_t> result;
  for (std::size_t i = 0; i < node.outputs.size(); ++i) {
    if (!equal(node.outputs[i], column)) continue;
    if (result) return {};
    result = i;
  }
  return result;
}
using ColumnKey = std::tuple<compiler::QueryId, std::size_t, std::string_view>;
using ColumnSet = std::set<ColumnKey>;
void intersectColumns(ColumnSet &columns, const ColumnSet &other) {
  for (auto column = columns.begin(); column != columns.end();)
    if (other.count(*column)) ++column;
    else column = columns.erase(column);
}
ColumnKey key(const ColumnRef &column) {
  return {column.query, column.table, column.name};
}
ColumnSet columnsOf(const RelNode &node) {
  ColumnSet columns;
  for (const auto &column : node.outputs) columns.insert(key(column));
  return columns;
}
bool identifiedOutputs(const RelNode &node) {
  return !node.outputs.empty() && node.outputs.size() <= 4096 &&
         std::none_of(node.outputs.begin(), node.outputs.end(),
                      [](const auto &col) { return col.name.empty() || col.name == "*"; }) &&
         columnsOf(node).size() == node.outputs.size();
}
bool containsUniqueKey(const RelProperties &input, const ColumnSet &matched) {
  return std::any_of(input.uniqueKeys.begin(), input.uniqueKeys.end(), [&](const auto &fields) {
    return !fields.empty() && std::all_of(fields.begin(), fields.end(),
        [&](const auto &column) { return matched.count(key(column)); });
  });
}
using IntegerRanges=std::vector<IntegerRange>;
constexpr std::size_t maxSearchRanges=64;
constexpr std::size_t maxDomainColumns=64;
IntegerRanges normalizedSearchRanges(IntegerRanges ranges) {
  std::sort(ranges.begin(),ranges.end(),[](const auto &a,const auto &b) { return a.low<b.low; });
  IntegerRanges merged;
  for (const auto range:ranges) {
    if (merged.empty() || (merged.back().high<range.low && merged.back().high+1<range.low))
      merged.push_back(range);
    else merged.back().high=std::max(merged.back().high,range.high);
  }
  // An enclosing interval keeps the upper bound sound after reaching the
  // range budget. It is never used as input to a complement.
  if (merged.size()>maxSearchRanges) return {{merged.front().low,merged.back().high}};
  return merged;
}
IntegerRanges intersectSearchRanges(const IntegerRanges &a,const IntegerRanges &b) {
  IntegerRanges intersected;
  std::size_t i=0,j=0;
  while (i<a.size() && j<b.size()) {
    const IntegerRange overlap{std::max(a[i].low,b[j].low),std::min(a[i].high,b[j].high)};
    if (overlap.low<=overlap.high) intersected.push_back(overlap);
    if (a[i].high<b[j].high) ++i;
    else ++j;
  }
  if (intersected.size()>maxSearchRanges) return {{intersected.front().low,intersected.back().high}};
  return intersected;
}
std::optional<Count> searchCardinality(const IntegerRanges &ranges) {
  std::optional<Count> count=Count{0};
  for (const auto range:ranges) {
    const auto distance=static_cast<Count>(range.high)-static_cast<Count>(range.low);
    if (distance==UINT64_MAX) return {};
    count=add(count,distance+1);
    if (!count) break;
  }
  return count;
}
struct IntegerPredicate {
  ColumnRef column;
  IntegerRanges ranges;
};
IntegerRanges pointRanges(std::vector<std::int64_t> points,IntegerRange domain,bool exclude) {
  points.erase(std::remove_if(points.begin(),points.end(),[&](auto point) {
    return point<domain.low || point>domain.high;
  }),points.end());
  IntegerRanges ranges;
  if (!exclude) {
    for (const auto point:points) ranges.push_back({point,point});
    return normalizedSearchRanges(std::move(ranges));
  }
  std::sort(points.begin(),points.end());
  points.erase(std::unique(points.begin(),points.end()),points.end());
  auto next=domain.low;
  for (const auto point:points) {
    if (point>next) ranges.push_back({next,point-1});
    if (point==domain.high) return normalizedSearchRanges(std::move(ranges));
    next=point+1;
  }
  ranges.push_back({next,domain.high});
  return normalizedSearchRanges(std::move(ranges));
}
IntegerRange inequalityRange(IntegerRange domain,std::string_view op,std::int64_t endpoint,bool reversed=false) {
  const bool strict=op.size()==1;
  const bool lower=(op.front()=='>')!=reversed;
  if (strict && (lower?endpoint==INT64_MAX:endpoint==INT64_MIN)) return {1,0};
  if (lower) domain.low=std::max(domain.low,endpoint+strict);
  else domain.high=std::min(domain.high,endpoint-strict);
  return domain;
}
std::optional<IntegerRanges> membershipRanges(const LogicalPlan &plan,ExpressionRef ref,
                                              IntegerRange domain) {
  using compiler::ExprKind;
  const auto &expression=modelExpression(plan,ref);
  if (expression.operands.size()>257) return {};
  const bool exclude=expression.value=="NOT IN";
  bool hasNull=false;
  std::vector<std::int64_t> points;
  for (std::size_t i=1;i<expression.operands.size();++i) {
    const ExpressionRef item{ref.query,expression.operands[i]};
    if (modelExpression(plan,item).kind==ExprKind::Null) {
      hasNull=true;
      continue;
    }
    const auto point=exactIntegerLiteral(plan,item);
    if (!point) return {};
    points.push_back(*point);
  }
  if (exclude && hasNull) return IntegerRanges{};
  return pointRanges(std::move(points),domain,exclude);
}
std::optional<IntegerRanges> quantifiedRanges(const LogicalPlan &plan,ExpressionRef ref,
                                             IntegerRange domain) {
  const auto &expression=modelExpression(plan,ref);
  const auto name=std::string_view(expression.value);
  const auto split=name.find(' ');
  if (split==std::string_view::npos) return {};
  const auto op=name.substr(0,split),quantifier=name.substr(split+1);
  const bool all=quantifier=="ALL";
  if (!all && quantifier!="ANY" && quantifier!="SOME") return {};
  if (op!="=" && op!="<>" && op!="<" && op!=">" && op!="<=" && op!=">=") return {};
  const auto array=postgresIntegerArrayLiteral(plan,{ref.query,expression.operands[1]},256);
  if (array.status!=IntegerArrayStatus::Ready) return {};
  if (array.nullValue) return IntegerRanges{};
  // Universal comparison over an empty array also admits NULL left rows.
  if (all && array.elements.empty()) return {};
  std::vector<std::int64_t> points;
  for (const auto &element:array.elements) {
    if (element) points.push_back(*element);
    else if (all) return IntegerRanges{};
  }
  if (points.empty()) return IntegerRanges{};
  const auto [low,high]=std::minmax_element(points.begin(),points.end());
  if (op=="=") {
    if (all && *low!=*high) return IntegerRanges{};
    return pointRanges(std::move(points),domain,false);
  }
  if (op=="<>") {
    if (!all && *low!=*high) return IntegerRanges{domain};
    return pointRanges(std::move(points),domain,true);
  }
  const bool chooseLow=(op.front()=='>')!=all;
  const auto range=inequalityRange(domain,op,chooseLow?*low:*high);
  return range.low<=range.high?IntegerRanges{range}:IntegerRanges{};
}
std::optional<IntegerPredicate> integerPredicate(const LogicalPlan &plan,ExpressionRef ref) {
  using compiler::ExprKind;
  const auto &expression=modelExpression(plan,ref);
  const bool between=expression.kind==ExprKind::Between && expression.value=="BETWEEN" &&
      expression.operands.size()==3;
  const bool equality=expression.value=="=" || expression.value=="IS NOT DISTINCT FROM";
  const bool comparison=expression.kind==ExprKind::Binary && expression.operands.size()==2 &&
      (equality || expression.value=="<" || expression.value=="<=" ||
       expression.value==">" || expression.value==">=");
  const bool membership=expression.kind==ExprKind::In && !expression.query && expression.operands.size()>1 &&
      (expression.value=="IN" || expression.value=="NOT IN");
  const bool quantified=expression.kind==ExprKind::Quantified && !expression.query && expression.operands.size()==2;
  if (!between && !comparison && !membership && !quantified) return {};
  ExpressionRef columnRef{ref.query,expression.operands[0]}, literal{ref.query,expression.operands[1]};
  auto column=boundScalarColumn(plan,columnRef);
  const bool reversed=comparison && !column;
  if (reversed) {
    std::swap(columnRef,literal);
    column=boundScalarColumn(plan,columnRef);
  }
  if (!column) return {};
  const auto bits=signedIntegerBits(plan.program.types.get(modelExpression(plan,columnRef).sqlType));
  if (!bits) return {};
  const auto minimum=*bits==64?INT64_MIN:-(std::int64_t{1}<<(*bits-1));
  const auto maximum=*bits==64?INT64_MAX:(std::int64_t{1}<<(*bits-1))-1;
  if (membership || quantified) {
    auto ranges=membership?membershipRanges(plan,ref,{minimum,maximum}):quantifiedRanges(plan,ref,{minimum,maximum});
    if (!ranges) return {};
    return IntegerPredicate{std::move(*column),std::move(*ranges)};
  }
  const auto value=exactIntegerLiteral(plan,literal);
  if (!value) return {};
  IntegerRange range{minimum,maximum};
  if (between) {
    const auto high=exactIntegerLiteral(plan,{ref.query,expression.operands[2]});
    if (!high) return {};
    range.low=std::max(range.low,*value);
    range.high=std::min(range.high,*high);
  } else if (equality) {
    range.low=std::max(range.low,*value);
    range.high=std::min(range.high,*value);
  } else {
    range=inequalityRange(range,expression.value,*value,reversed);
  }
  return IntegerPredicate{std::move(*column),range.low<=range.high?IntegerRanges{range}:IntegerRanges{}};
}
void tighten(std::optional<Count> &bound, std::optional<Count> candidate) {
  if (candidate && (!bound || *candidate < *bound)) bound = candidate;
}
}
bool coversNonNullUniqueKey(const RelProperties &input, const std::vector<ColumnRef> &columns) {
  return std::any_of(input.uniqueKeys.begin(), input.uniqueKeys.end(), [&](const auto &fields) {
    return !fields.empty() && std::all_of(fields.begin(), fields.end(), [&](const auto &field) {
      return contains(columns, field) && contains(input.nonNullColumns, field);
    });
  });
}
struct RelMetadataQuery::Impl {
  const LogicalPlan &plan;
  MetadataLimits limits;
  MetadataStatus state=MetadataStatus::Ready;
  std::string error;
  std::size_t bytes=0,expressionVisits=0;
  std::map<RelId,RelProperties> cache;
  std::set<RelId> active;
  using LineageKey = std::pair<RelId, std::size_t>;
  std::map<LineageKey, ColumnLineage> lineageCache;
  std::set<LineageKey> lineageActive;
  std::size_t lineageExpressionVisits = 0;
  Impl(const LogicalPlan &p,MetadataLimits l):plan(p),limits(std::move(l)) {
    limits.maxNodes=std::min<std::size_t>(limits.maxNodes,16384);
    limits.maxDepth=std::min<std::size_t>(limits.maxDepth,128);
    limits.maxBytes=std::min<std::size_t>(limits.maxBytes,64*1024*1024);
  }
  const RelProperties *fail(MetadataStatus s,const char* text) {
    state=s;error=text;return nullptr;
  }
  ColumnLineage lineage(RelId id, std::size_t column, std::size_t depth) {
    const auto *found = getLineage(id, column, depth);
    return found ? *found : ColumnLineage{};
  }
  void mergeOrigins(ColumnLineage &to, ColumnLineage from, bool derived = false) {
    to.complete &= from.complete;
    for (auto &source : from.origins) {
      const auto id = [](const auto &item) {
        return std::tie(item.database, item.schema, item.table, item.ordinal, item.column);
      };
      const auto existing = std::find_if(to.origins.begin(), to.origins.end(),
          [&](const auto &item) { return id(item) == id(source); });
      if (existing != to.origins.end()) {
        existing->derived |= derived || source.derived;
      } else if (to.origins.size() < 64) {
        source.derived |= derived;
        to.origins.push_back(std::move(source));
      } else {
        to.complete = false;
      }
    }
  }
  std::optional<RelId> aggregateInput(RelId input) const {
    const auto *node = &plan.nodes.at(input);
    if (node->kind == RelKind::Filter && node->inputs.size() == 1)
      node = &plan.nodes.at(node->inputs.front());
    if (node->kind == RelKind::Aggregate && !node->distinct && node->inputs.size() == 1)
      return node->inputs.front();
    return {};
  }
  ColumnLineage expressionLineage(ExpressionRef ref, RelId input, std::size_t depth) {
    using namespace compiler;
    if (state != MetadataStatus::Ready) return {};
    if (depth >= limits.maxDepth || ++lineageExpressionVisits > limits.maxNodes * 32) {
      fail(MetadataStatus::ResourceLimit, "column origin expression budget exceeded");
      return {};
    }
    if (limits.shouldStop && limits.shouldStop()) {
      fail(MetadataStatus::Stopped, "column origin lookup cancelled");
      return {};
    }
    const auto &e = modelExpression(plan, ref);
    if (e.query || e.window || e.localSymbol || e.unresolvedBooleanName ||
        !e.coalescedBindings.empty() || e.outputColumn) return {};
    if (e.kind == ExprKind::Name) {
      if (!input || !e.binding) return {};
      const auto &bind = *e.binding;
      const auto &owner = modelQuery(plan, bind.scope);
      if (bind.table >= owner.tables.size() || !owner.tables[bind.table].definition ||
          bind.column >= owner.tables[bind.table].definition->columns.size()) return {};
      const auto &name = owner.tables[bind.table].definition->columns[bind.column].name;
      const auto slot = columnOrdinal(plan.nodes.at(input), {bind.scope, bind.table, name});
      return slot ? lineage(input, *slot, depth + 1) : ColumnLineage{};
    }
    switch (e.kind) {
    case ExprKind::Null: case ExprKind::Number: case ExprKind::String: case ExprKind::Boolean:
    case ExprKind::TypedLiteral: case ExprKind::Parameter: case ExprKind::Star:
      return {true, {}};
    case ExprKind::AggregateFilter:
      // FILTER constrains the rows fed into the aggregate. The aggregate's
      // value origins are the operand columns of the call itself.
      return e.operands.size() == 2 ? expressionLineage({ref.query, e.operands.front()}, input, depth + 1)
                                    : ColumnLineage{};
    case ExprKind::Call: {
      const auto builtin = builtinOperator(plan.program.sourceDialect, e.name, e.nameQuoted, e.operands.size());
      if (!builtin || builtin->volatility != FunctionVolatility::Immutable ||
          (e.routineResolved && (plan.program.sourceDialect != Dialect::PostgreSQL ||
                                 e.routineSchema != "pg_catalog" || e.routineKind != builtin->kind ||
                                 e.routineVolatility != FunctionVolatility::Immutable))) return {};
      if (builtin->kind == RoutineKind::Aggregate) {
        const auto before = input ? aggregateInput(input) : std::nullopt;
        if (!before) return {};
        input = *before;
      } else if (builtin->kind != RoutineKind::Scalar) return {};
      break;
    }
    case ExprKind::Unary: case ExprKind::Binary: case ExprKind::Cast: case ExprKind::Case:
    case ExprKind::Between: case ExprKind::In: case ExprKind::IsNull: case ExprKind::TruthTest: case ExprKind::Row:
    case ExprKind::Interval: case ExprKind::Extract: case ExprKind::ArrayConstructor:
      break;
    default:
      return {};
    }
    ColumnLineage result{true, {}};
    for (auto operand : e.operands)
      mergeOrigins(result, expressionLineage({ref.query, operand}, input, depth + 1), true);
    return result;
  }
  ColumnLineage resolveLineage(const RelNode &node, std::size_t ordinal, std::size_t depth) {
    using namespace compiler;
    const auto &column = node.outputs.at(ordinal);
    const auto sameInput = [&]() -> ColumnLineage {
      if (node.inputs.empty()) return {};
      const auto input = node.inputs.front();
      const auto slot = columnOrdinal(plan.nodes.at(input), column);
      return slot ? lineage(input, *slot, depth + 1) : ColumnLineage{};
    };
    switch (node.kind) {
    case RelKind::Scan: {
      const auto &query = modelQuery(plan, node.query);
      if (!query.catalogChecked || !node.table || *node.table >= query.tables.size() ||
          column.query != node.query || column.table != *node.table) return {};
      const auto &source = query.tables[*node.table];
      const auto &table = sourceDefinition(source);
      if (!table || table->kind != ObjectKind::Table || !table->arityKnown || table->name.empty() ||
          ordinal >= table->columns.size() || table->columns[ordinal].name != column.name) return {};
      const auto names = table->database.size() + table->schema.size() + table->name.size() + column.name.size();
      if (names > 16384) return {};
      return {true, {{table->database, table->schema, table->name, column.name, ordinal, false}}};
    }
    case RelKind::Values: {
      ColumnLineage result{true, {}};
      if (!node.valueCount) return result;
      if (node.keys.empty() || node.keys.size() % node.outputs.size() != 0 ||
          node.keys.size() / node.outputs.size() != node.valueCount) return {};
      for (std::size_t i = ordinal; i < node.keys.size(); i += node.outputs.size())
        mergeOrigins(result, expressionLineage(node.keys[i], 0, depth + 1));
      return result;
    }
    case RelKind::Filter: case RelKind::Sort: case RelKind::Limit: case RelKind::Window:
    case RelKind::Sample:
    case RelKind::SemiJoin: case RelKind::AntiJoin:
      return sameInput();
    case RelKind::Unpivot: {
      const auto &query=modelQuery(plan,node.query);
      if (node.inputs.size()!=1 || !node.table || *node.table>=query.tables.size()) return {};
      const auto &shape=query.tables[*node.table].unpivot;
      if (!shape) return {};
      const auto input=node.inputs.front();
      if (ordinal<shape->retainedOrdinals.size())
        return lineage(input,shape->retainedOrdinals[ordinal],depth+1);
      const auto firstMeasure=shape->retainedOrdinals.size()+shape->descriptors.size();
      if (ordinal<firstMeasure) return {true,{}};
      if (ordinal>=firstMeasure+shape->measures.size() || shape->inputOrdinals.empty()) return {};
      ColumnLineage result{true,{}};
      for (const auto &fields:shape->inputOrdinals)
        mergeOrigins(result,lineage(input,fields.at(ordinal-firstMeasure),depth+1),true);
      return result;
    }
    case RelKind::Pivot: {
      const auto &query=modelQuery(plan,node.query);
      if (node.inputs.size()!=1 || !node.table || *node.table>=query.tables.size()) return {};
      const auto &shape=query.tables[*node.table].pivot;
      if (!shape || shape->axisOrdinals.empty()) return {};
      const auto input=node.inputs.front();
      if (ordinal<shape->groupingOrdinals.size())
        return lineage(input,shape->groupingOrdinals[ordinal],depth+1);
      ColumnLineage result{true,{}};
      for (auto axis:shape->axisOrdinals)
        mergeOrigins(result,lineage(input,axis,depth+1),true);
      if (shape->argumentOrdinal)
        mergeOrigins(result,lineage(input,*shape->argumentOrdinal,depth+1),true);
      return result;
    }
    case RelKind::Series:
    case RelKind::Unnest:
      if (node.withOrdinality && ordinal+1==node.outputs.size()) return {true,{}};
      return ordinal+(node.withOrdinality?2:1)<node.outputs.size()?sameInput():ColumnLineage{};
    case RelKind::Aggregate:
      if (node.distinct) return sameInput();
      if (!node.groupingSets.empty()) return {};
      for (auto ref : node.keys) {
        const auto &e = modelExpression(plan, ref);
        const auto &query = modelQuery(plan, ref.query);
        if (e.outputColumn && *e.outputColumn < query.projections.size())
          ref.expression = query.projections[*e.outputColumn].expression;
        const auto key = boundScalarColumn(plan, ref);
        if (key && equal(*key, column)) return sameInput();
      }
      return {};
    case RelKind::Calc:
      if (node.keys.empty()) return sameInput();
      [[fallthrough]];
    case RelKind::Project:
      if (node.inputs.empty()) return {};
      if (node.keys.size() == 1 && modelExpression(plan, node.keys.front()).kind == ExprKind::Star)
        return sameInput();
      if (node.keys.size() != node.outputs.size() ||
          modelExpression(plan, node.keys[ordinal]).kind == ExprKind::Star) return {};
      return expressionLineage(node.keys[ordinal], node.inputs.front(), depth + 1);
    case RelKind::Rename:
      if (node.inputs.size() != 1 || plan.nodes.at(node.inputs.front()).outputs.size() != node.outputs.size())
        return {};
      return lineage(node.inputs.front(), ordinal, depth + 1);
    case RelKind::Coerce: {
      if (node.inputs.size() != 1 || plan.nodes.at(node.inputs.front()).outputs.size() != node.outputs.size())
        return {};
      auto result = lineage(node.inputs.front(), ordinal, depth + 1);
      if (std::any_of(node.integerWidenings.begin(), node.integerWidenings.end(),
                      [&](const auto &conversion) { return conversion.ordinal == ordinal; }))
        for (auto &origin : result.origins) origin.derived = true;
      return result;
    }
    case RelKind::Subquery: {
      if (node.scalar || node.correlated || node.inputs.size() != 1 || !node.table) return {};
      const auto &owner = modelQuery(plan, node.query);
      if (*node.table >= owner.tables.size()) return {};
      const auto &table = owner.tables[*node.table];
      const auto &input = plan.nodes.at(node.inputs.front());
      const auto &definition=sourceDefinition(table);
      if (!definition || !definition->arityKnown || !definition->complete ||
          table.query != input.query || definition->columns.size() != node.outputs.size() ||
          node.outputs.size() != input.outputs.size()) return {};
      return lineage(node.inputs.front(), ordinal, depth + 1);
    }
    case RelKind::Join: case RelKind::Correlate: {
      if (node.distinct || node.inputs.size() != 2) return {};
      const auto &left = plan.nodes.at(node.inputs[0]);
      const auto &right = plan.nodes.at(node.inputs[1]);
      if (left.outputs.size() + right.outputs.size() != node.outputs.size()) return {};
      const auto lhs = columnOrdinal(left, column), rhs = columnOrdinal(right, column);
      if (bool(lhs) == bool(rhs)) return {};
      const auto side = lhs ? 0u : 1u;
      auto source = lineage(node.inputs[side], lhs ? *lhs : *rhs, depth + 1);
      for (auto &origin : source.origins)
        origin.derived |= node.join == JoinKind::Full || (side && node.join == JoinKind::Left) ||
                          (!side && node.join == JoinKind::Right);
      return source;
    }
    case RelKind::SetOperation: {
      if (node.inputs.size() < 2) return {};
      ColumnLineage result{true, {}};
      for (auto input : node.inputs) {
        if (plan.nodes.at(input).outputs.size() != node.outputs.size()) return {};
        mergeOrigins(result, lineage(input, ordinal, depth + 1), true);
      }
      return result;
    }
    default:
      return {};
    }
  }
  const ColumnLineage *getLineage(RelId id, std::size_t ordinal, std::size_t depth = 0) {
    if (state != MetadataStatus::Ready) return nullptr;
    const auto key = LineageKey{id, ordinal};
    if (limits.shouldStop && limits.shouldStop()) {
      fail(MetadataStatus::Stopped, "column origin lookup cancelled");
      return nullptr;
    }
    if (const auto found = lineageCache.find(key); found != lineageCache.end()) return &found->second;
    if (!id || id >= plan.nodes.size() || ordinal >= plan.nodes[id].outputs.size() ||
        plan.nodes[id].outputs.size() > 4096) {
      fail(MetadataStatus::InvalidGraph, "invalid column origin reference");
      return nullptr;
    }
    if (depth >= limits.maxDepth || lineageCache.size() + lineageActive.size() >= limits.maxNodes * 16) {
      fail(MetadataStatus::ResourceLimit, "column origin node budget exceeded");
      return nullptr;
    }
    if (!lineageActive.insert(key).second) {
      fail(MetadataStatus::InvalidGraph, "cycle in column origin input");
      return nullptr;
    }
    struct Leave {
      std::set<LineageKey> &active;
      LineageKey key;
      ~Leave() { active.erase(key); }
    } leave{lineageActive, key};
    const auto &node = plan.nodes[id];
    if (node.inputs.size() > 4096 || std::any_of(node.inputs.begin(), node.inputs.end(),
            [&](auto input) { return !input || input >= plan.nodes.size(); })) {
      fail(MetadataStatus::InvalidGraph, "invalid column origin input");
      return nullptr;
    }
    auto result = resolveLineage(node, ordinal, depth);
    if (state != MetadataStatus::Ready) return nullptr;
    std::size_t charge = sizeof(result) + 128;
    for (const auto &origin : result.origins)
      charge += sizeof(origin) + origin.database.size() + origin.schema.size() + origin.table.size() + origin.column.size();
    if (charge > (limits.maxBytes - bytes) / 2) {
      fail(MetadataStatus::ResourceLimit, "column origin allocation budget exceeded");
      return nullptr;
    }
    bytes += charge * 2;
    return &lineageCache.emplace(key, std::move(result)).first->second;
  }
  bool strengthenJoin(const RelNode &join, const RelProperties &left,
                      const RelProperties &right, RelProperties &out) {
    using compiler::JoinKind;
    if (join.distinct || join.inputs.size() != 2) return true;
    const auto atMostOne = [](const RelProperties &input) {
      return input.rows.maximum && *input.rows.maximum <= 1;
    };
    bool oneLeftPerRight = atMostOne(left);
    bool oneRightPerLeft = atMostOne(right);
    if (!join.predicates.empty() && ((!oneLeftPerRight && !left.uniqueKeys.empty()) ||
                                     (!oneRightPerLeft && !right.uniqueKeys.empty()))) {
      const auto leftColumns = columnsOf(plan.nodes[join.inputs[0]]);
      const auto rightColumns = columnsOf(plan.nodes[join.inputs[1]]);
      ColumnSet matchedLeft, matchedRight;
      for (auto ref : join.predicates) {
        if (limits.shouldStop && limits.shouldStop()) {
          fail(MetadataStatus::Stopped, "join key proof cancelled");
          return false;
        }
        if (ref.query != join.query) continue;
        auto equality = integerEqualityColumns(plan, ref);
        const bool nullSafe=!equality;
        if (nullSafe) equality=integerNullSafeEqualityColumns(plan,ref);
        if (!equality) continue;
        auto a = key(equality->first), b = key(equality->second);
        if (rightColumns.count(a) || leftColumns.count(b)) std::swap(a, b);
        const auto leftColumn = leftColumns.find(a), rightColumn = rightColumns.find(b);
        if (leftColumn != leftColumns.end() && rightColumn != rightColumns.end() &&
            !rightColumns.count(a) && !leftColumns.count(b)) {
          // Save views backed by the immutable input schema, not the temporary equality.
          const auto provesNonNull=[](const RelProperties &properties,const ColumnKey &identity) {
            return std::any_of(properties.nonNullColumns.begin(),properties.nonNullColumns.end(),
                               [&](const auto &column) { return key(column)==identity; });
          };
          // A null-safe equality cannot be used as an ordinary nullable key
          // match. At least one side must rule out a NULL match on this field.
          if (!nullSafe || provesNonNull(left,*leftColumn) || provesNonNull(right,*rightColumn)) {
            matchedLeft.insert(*leftColumn);
            matchedRight.insert(*rightColumn);
          }
        }
      }
      oneLeftPerRight |= containsUniqueKey(left, matchedLeft);
      oneRightPerLeft |= containsUniqueKey(right, matchedRight);
    }

    const auto preserve = [&](const RelProperties &input) {
      // Bound the number of propagated keys. Omitting a key loses a possible
      // optimization without strengthening any cardinality claim.
      constexpr std::size_t maxKeys = 64;
      for (const auto &fields : input.uniqueKeys) {
        if (out.uniqueKeys.size() >= maxKeys) break;
        if (std::all_of(fields.begin(), fields.end(),
                        [&](const auto &column) { return contains(join.outputs, column); }))
          out.uniqueKeys.push_back(fields);
      }
    };
    const bool inner = join.join == JoinKind::Inner || join.join == JoinKind::Cross;
    if (oneRightPerLeft && (inner || join.join == JoinKind::Left)) {
      tighten(out.rows.maximum, left.rows.maximum);
      preserve(left);
    }
    if (oneLeftPerRight && (inner || join.join == JoinKind::Right)) {
      tighten(out.rows.maximum, right.rows.maximum);
      preserve(right);
    }
    if (join.join == JoinKind::Full && (oneLeftPerRight || oneRightPerLeft))
      tighten(out.rows.maximum, add(left.rows.maximum, right.rows.maximum));
    return true;
  }
  bool joinDomains(const RelNode &join,const RelProperties &left,
                   const RelProperties &right,RelProperties &out) {
    using compiler::JoinKind;
    const bool inner=join.join==JoinKind::Inner || join.join==JoinKind::Cross;
    if (plan.program.sourceDialect!=compiler::Dialect::PostgreSQL || join.distinct ||
        join.join==JoinKind::Full || (!left.integerDomains && !right.integerDomains &&
          (!inner || join.predicates.empty())) || !identifiedOutputs(join)) return true;
    const auto fields=columnsOf(join);
    std::map<ColumnKey,IntegerRanges> domains;
    ColumnSet notNull;
    for (const auto &column:out.nonNullColumns) notNull.insert(key(column));
    const auto preserve=[&](const RelProperties &input) {
      if (input.integerDomains)
        for (const auto &domain:*input.integerDomains)
          if (const auto field=fields.find(key(domain.column));field!=fields.end())
            domains.emplace(*field,domain.ranges);
    };
    if (inner || join.join==JoinKind::Left) preserve(left);
    if (inner || join.join==JoinKind::Right) preserve(right);
    if (inner && join.kind==RelKind::Join)
      for (auto predicate:join.predicates) {
        if (limits.shouldStop && limits.shouldStop()) {
          fail(MetadataStatus::Stopped,"join domain inference cancelled");
          return false;
        }
        if (predicate.query!=join.query) continue;
        auto equality=integerEqualityColumns(plan,predicate);
        const bool nullSafe=!equality;
        if (nullSafe) equality=integerNullSafeEqualityColumns(plan,predicate);
        if (!equality) continue;
        const auto a=fields.find(key(equality->first)),b=fields.find(key(equality->second));
        if (a==fields.end() || b==fields.end() || a==b) continue;
        if (nullSafe && !notNull.count(*a) && !notNull.count(*b)) continue;
        if (notNull.insert(*a).second) out.nonNullColumns.push_back(equality->first);
        if (notNull.insert(*b).second) out.nonNullColumns.push_back(equality->second);
        const auto first=domains.find(*a),second=domains.find(*b);
        if (first==domains.end() && second==domains.end()) continue;
        if (first==domains.end() || second==domains.end()) {
          if (domains.size()<2*maxDomainColumns) {
            const auto &known=first==domains.end()?second->second:first->second;
            domains.emplace(first==domains.end()?*a:*b,known);
          }
          continue;
        }
        auto shared=intersectSearchRanges(first->second,second->second);
        if (shared.empty()) {
          out.rows={0,Count{0}};
          return true;
        }
        first->second=shared;
        second->second=std::move(shared);
      }
    std::vector<ColumnIntegerDomain> result;
    for (const auto &column:join.outputs) {
      if (result.size()==maxDomainColumns) break;
      const auto domain=domains.find(key(column));
      if (domain!=domains.end()) result.push_back({column,std::move(domain->second)});
    }
    if (!result.empty())
      out.integerDomains=std::make_shared<const std::vector<ColumnIntegerDomain>>(std::move(result));
    return true;
  }
  bool strengthenFilter(const RelNode &filter, const RelNode &input,
                        RelProperties &output) {
    using compiler::Dialect;
    using compiler::ExprKind;
    if (plan.program.sourceDialect != Dialect::PostgreSQL || filter.predicates.empty())
      return true;
    const auto available=columnsOf(input);
    ColumnSet guaranteedNonNull;
    for (const auto &column:output.nonNullColumns) guaranteedNonNull.insert(key(column));
    struct PredicateFacts {
      ColumnSet nonNull, isNull;
      std::map<ColumnKey,IntegerRanges> ranges;
      bool impossible=false;
    };
    const auto contradiction=[] {
      PredicateFacts facts;
      facts.impossible=true;
      return facts;
    };
    const auto combine=[&](PredicateFacts left,PredicateFacts right,bool conjunction) {
      if (conjunction) {
        if (left.impossible || right.impossible) return contradiction();
        left.nonNull.merge(right.nonNull);
        left.isNull.merge(right.isNull);
        if (std::any_of(left.isNull.begin(),left.isNull.end(),[&](const auto &column) {
              return left.nonNull.count(column);
            })) return contradiction();
        for (auto &[column,value]:right.ranges) {
          const auto [entry,inserted]=left.ranges.try_emplace(column,std::move(value));
          if (inserted) continue;
          entry->second=intersectSearchRanges(entry->second,value);
          if (entry->second.empty()) return contradiction();
        }
      } else {
        if (left.impossible) return right;
        if (right.impossible) return left;
        intersectColumns(left.nonNull,right.nonNull);
        intersectColumns(left.isNull,right.isNull);
        for (auto entry=left.ranges.begin();entry!=left.ranges.end();) {
          const auto match=right.ranges.find(entry->first);
          if (match==right.ranges.end()) entry=left.ranges.erase(entry);
          else {
            auto &ranges=entry->second;
            ranges.insert(ranges.end(),match->second.begin(),match->second.end());
            ranges=normalizedSearchRanges(std::move(ranges));
            ++entry;
          }
        }
      }
      return left;
    };
    std::size_t visited = 0;
    const auto whenTrue = [&](const auto &visit, ExpressionRef ref, std::size_t depth) -> PredicateFacts {
      if (state != MetadataStatus::Ready || ref.query != filter.query ||
          depth >= limits.maxDepth || ++visited > 4096) return {};
      if (limits.shouldStop && limits.shouldStop()) {
        fail(MetadataStatus::Stopped, "filter fact inference cancelled");
        return {};
      }
      const auto &expr = modelExpression(plan, ref);
      if (expr.kind == ExprKind::Binary && expr.operands.size() == 2 &&
          (expr.value == "AND" || expr.value == "OR")) {
        auto left = visit(visit, {ref.query, expr.operands[0]}, depth + 1);
        auto right = visit(visit, {ref.query, expr.operands[1]}, depth + 1);
        return combine(std::move(left), std::move(right), expr.value == "AND");
      }
      PredicateFacts result;
      std::optional<ExpressionRef> nullTest;
      bool testsNull=false;
      if (expr.kind==ExprKind::IsNull && expr.value=="IS NULL" && expr.operands.size()==1) {
        nullTest=ExpressionRef{ref.query,expr.operands.front()};
        testsNull=true;
      } else if (expr.kind==ExprKind::Binary && expr.operands.size()==2 &&
                 (expr.value=="IS DISTINCT FROM" || expr.value=="IS NOT DISTINCT FROM")) {
        const ExpressionRef left{ref.query,expr.operands[0]}, right{ref.query,expr.operands[1]};
        if (modelExpression(plan,left).kind==ExprKind::Null) nullTest=right;
        else if (modelExpression(plan,right).kind==ExprKind::Null) nullTest=left;
        testsNull=expr.value=="IS NOT DISTINCT FROM";
      }
      if (nullTest) {
        const auto argument=*nullTest;
        const auto column=boundScalarColumn(plan,argument);
        const auto &type=plan.program.types.get(modelExpression(plan,argument).sqlType);
        if (column && !postgresGroupingType(type).empty()) {
          const auto known=available.find(key(*column));
          if (known!=available.end()) {
            if (testsNull) {
              if (guaranteedNonNull.count(*known)) return contradiction();
              result.isNull.insert(*known);
            } else result.nonNull.insert(*known);
          }
        }
        return result;
      }
      if (auto comparison=integerPredicate(plan,ref)) {
        const auto known=available.find(key(comparison->column));
        if (known!=available.end()) {
          if (comparison->ranges.empty())
            return contradiction();
          result.nonNull.insert(*known);
          result.ranges.emplace(*known,std::move(comparison->ranges));
          return result;
        }
      }
      if (const auto fields=totalPredicateColumns(plan,ref))
        for (const auto &field:*fields)
          if (const auto known=available.find(key(field));known!=available.end())
            result.nonNull.insert(*known);
      return result;
    };
    PredicateFacts proof;
    if (output.integerDomains)
      for (const auto &domain:*output.integerDomains) {
        const auto name=available.find(key(domain.column));
        if (name==available.end()) continue;
        proof.ranges.emplace(*name,domain.ranges);
        guaranteedNonNull.insert(*name);
      }
    output.integerDomains.reset();
    for (const auto predicate : filter.predicates) {
      auto facts = whenTrue(whenTrue, predicate, 0);
      if (state != MetadataStatus::Ready) return false;
      proof = combine(std::move(proof), std::move(facts), true);
      if (proof.impossible || visited > 4096) break;
    }
    if (proof.impossible) {
      output.rows = {0, Count{0}};
      return true;
    }
    const auto &row=filter.kind==RelKind::Calc?input:filter;
    const auto &outputs=row.outputs;
    for (const auto &field : output.nonNullColumns) proof.nonNull.erase(key(field));
    for (const auto &field : outputs)
      if (proof.nonNull.erase(key(field))) output.nonNullColumns.push_back(field);
    // A non-NULL key tuple can occur at most once. Every constrained
    // component contributes the number of discrete values in its interval.
    for (const auto &unique:output.uniqueKeys) {
      if (unique.empty()) continue;
      std::optional<Count> combinations=Count{1};
      for (const auto &field:unique) {
        const auto range=proof.ranges.find(key(field));
        if (range==proof.ranges.end()) {
          combinations.reset();
          break;
        }
        combinations=multiply(combinations,searchCardinality(range->second));
        if (!combinations) break;
      }
      tighten(output.rows.maximum,combinations);
    }
    if (!proof.ranges.empty() && identifiedOutputs(row)) {
      std::vector<ColumnIntegerDomain> domains;
      for (const auto &column:outputs) {
        if (domains.size()==maxDomainColumns) break;
        const auto domain=proof.ranges.find(key(column));
        if (domain!=proof.ranges.end()) domains.push_back({column,std::move(domain->second)});
      }
      if (!domains.empty())
        output.integerDomains=std::make_shared<const std::vector<ColumnIntegerDomain>>(std::move(domains));
    }
    return true;
  }
  std::optional<ColumnRef> groupingColumn(ExpressionRef ref) const {
    const auto &value = modelExpression(plan, ref);
    if (value.outputColumn) {
      const auto &q = modelQuery(plan, ref.query);
      if (*value.outputColumn >= q.projections.size()) return {};
      ref.expression = q.projections[*value.outputColumn].expression;
    }
    return boundScalarColumn(plan, ref);
  }
  std::vector<ColumnRef> groupedNonNullColumns(const RelNode &aggregate,
                                              const RelProperties &input) {
    if (input.nonNullColumns.empty()) return {};
    ColumnSet provenInput;
    for (const auto &column : input.nonNullColumns) provenInput.insert(key(column));
    const auto keysIn = [&](const std::vector<ExpressionRef> &refs) {
      ColumnSet result;
      for (auto ref : refs) {
        if (limits.shouldStop && limits.shouldStop()) {
          fail(MetadataStatus::Stopped, "grouping non-null inference cancelled");
          return ColumnSet{};
        }
        if (ref.query != aggregate.query) continue;
        const auto field = groupingColumn(ref);
        if (!field) continue;
        const auto known = provenInput.find(key(*field));
        if (known != provenInput.end()) result.insert(*known);
      }
      return result;
    };
    ColumnSet proven;
    if (aggregate.distinct) {
      proven = std::move(provenInput);
    } else if (aggregate.groupingSets.empty()) {
      proven = keysIn(aggregate.keys);
    } else {
      proven = keysIn(aggregate.groupingSets.front());
      for (std::size_t i = 1; !proven.empty() && i < aggregate.groupingSets.size(); ++i) {
        const auto arm = keysIn(aggregate.groupingSets[i]);
        // A missing key is filled with NULL for that grouping set.
        intersectColumns(proven, arm);
      }
    }
    std::vector<ColumnRef> result;
    for (const auto &field : aggregate.outputs)
      if (proven.erase(key(field))) result.push_back(field);
    return result;
  }
  std::vector<std::vector<ColumnRef>> groupedUniqueKeys(const RelNode &aggregate,
                                                        const RelProperties &input) const {
    const auto &keys = aggregate.groupingSets.empty() ? aggregate.keys : aggregate.groupingSets.front();
    if (keys.empty()) return {};
    if (plan.program.sourceDialect == compiler::Dialect::PostgreSQL && !input.uniqueKeys.empty()) {
      const auto outputs = columnsOf(aggregate);
      std::vector<ColumnRef> direct;
      direct.reserve(keys.size());
      if (outputs.size() == aggregate.outputs.size())
        for (auto ref : keys)
          if (auto column = groupingColumn(ref); column && outputs.count(key(*column)))
            direct.push_back(std::move(*column));
      ColumnSet directColumns;
      for (const auto &column : direct) directColumns.insert(key(column));
      const auto containsGroupingColumn = [&](const ColumnRef &column) {
        return directColumns.count(key(column));
      };
      std::vector<std::vector<ColumnRef>> precise;
      for (const auto &key : input.uniqueKeys) {
        if (!key.empty() && std::all_of(key.begin(), key.end(), containsGroupingColumn))
          precise.push_back(key);
        if (precise.size() == 64) break;
      }
      if (!precise.empty()) return precise;
    }
    std::vector<ColumnRef> fallback;
    for (auto ref : keys) {
      auto column = groupingColumn(ref);
      if (!column) column = resolveColumn(plan, ref);
      if (!column) return {};
      fallback.push_back(std::move(*column));
    }
    std::vector<std::vector<ColumnRef>> result;
    result.push_back(std::move(fallback));
    return result;
  }
  void projectedExtremumKeys(const RelNode &project, const RelNode &input, RelProperties &output) {
    using namespace compiler;
    if (plan.program.sourceDialect != Dialect::PostgreSQL || project.kind != RelKind::Project ||
        project.keys.size() != project.outputs.size() || !identifiedOutputs(project))
      return;
    const auto *aggregate = &input;
    if (input.kind == RelKind::Filter && input.query == project.query && input.inputs.size() == 1)
      aggregate = &plan.nodes[input.inputs.front()];
    if (aggregate->kind != RelKind::Aggregate || aggregate->query != project.query ||
        aggregate->distinct || aggregate->keys.empty() || !aggregate->groupingSets.empty() ||
        aggregate->inputs.size() != 1) return;
    const auto facts = cache.find(aggregate->inputs.front());
    if (facts == cache.end() || facts->second.uniqueKeys.empty()) return;
    ColumnSet baseKeys;
    for (const auto &keyFields : facts->second.uniqueKeys)
      if (keyFields.size() == 1 && contains(facts->second.nonNullColumns, keyFields.front()))
        baseKeys.insert(key(keyFields.front()));
    if (baseKeys.empty()) return;

    for (std::size_t i = 0; i < project.keys.size() && output.uniqueKeys.size() < 64; ++i) {
      if (limits.shouldStop && limits.shouldStop()) {
        fail(MetadataStatus::Stopped, "aggregate key inference cancelled");
        return;
      }
      const auto ref = project.keys[i];
      if (ref.query != aggregate->query) continue;
      const auto &call = modelExpression(plan, ref);
      if (call.kind != ExprKind::Call || call.window || call.distinct || call.operands.size() != 1)
        continue;
      const auto op = builtinOperator(Dialect::PostgreSQL, call.name, call.nameQuoted, 1);
      if (!op || op->kind != RoutineKind::Aggregate || (op->name != "MIN" && op->name != "MAX") ||
          (call.routineResolved && (call.routineSchema != "pg_catalog" ||
            call.routineKind != RoutineKind::Aggregate || call.routineVolatility != FunctionVolatility::Immutable)))
        continue;
      const ExpressionRef argument{ref.query, call.operands.front()};
      const auto column = boundScalarColumn(plan, argument);
      if (!column || !baseKeys.count(key(*column)) || !sameIntegerType(plan, ref, argument) ||
          !signedIntegerBits(plan.program.types.get(call.sqlType)))
        continue;
      // Each simple group is nonempty and disjoint. Its extremum therefore
      // comes from one unique input row and cannot recur in another group.
      const auto &field = project.outputs[i];
      output.uniqueKeys.push_back({field});
      if (!contains(output.nonNullColumns, field)) output.nonNullColumns.push_back(field);
    }
  }
  void remapOutputProperties(const RelNode &output,const RelNode &input,
                             const RelProperties &source,RelProperties &result) {
    if (output.outputs.size()!=input.outputs.size() || !identifiedOutputs(input) ||
        !identifiedOutputs(output)) return;
    std::map<ColumnKey,const ColumnRef *> positions;
    for (std::size_t i=0;i<output.outputs.size();++i)
      positions.emplace(key(input.outputs[i]),&output.outputs[i]);
    for (const auto &sourceKey:source.uniqueKeys) {
      std::vector<ColumnRef> mapped;
      for (const auto &column:sourceKey) {
        const auto position=positions.find(key(column));
        if (position==positions.end()) break;
        mapped.push_back(*position->second);
      }
      if (mapped.size()==sourceKey.size()) result.uniqueKeys.push_back(std::move(mapped));
    }
    for (const auto &column:source.nonNullColumns)
      if (const auto position=positions.find(key(column));position!=positions.end())
        result.nonNullColumns.push_back(*position->second);
    if (source.integerDomains) {
      std::vector<ColumnIntegerDomain> domains;
      for (const auto &domain:*source.integerDomains)
        if (const auto position=positions.find(key(domain.column));position!=positions.end())
          domains.push_back({*position->second,domain.ranges});
      if (!domains.empty())
        result.integerDomains=std::make_shared<const std::vector<ColumnIntegerDomain>>(std::move(domains));
    }
  }
  bool strengthenSet(const RelNode &set, const std::vector<const RelProperties *> &inputs,
                     RelProperties &out) {
    using compiler::Dialect;
    using compiler::SetKind;
    if (plan.program.sourceDialect != Dialect::PostgreSQL ||
        set.setAlgorithm != SetAlgorithm::Native || inputs.size() != 2 ||
        !identifiedOutputs(set)) return true;
    const auto &result = modelQuery(plan, set.query);
    const auto width = set.outputs.size();
    if (!result.setOperation || result.projections.size() != width ||
        result.setOperation->kind != set.setKind) return true;
    std::vector<bool> nonNull(width, set.setKind == SetKind::Union);
    std::array<std::vector<const IntegerRanges *>,2> branchDomains{
      std::vector<const IntegerRanges *>(width),std::vector<const IntegerRanges *>(width)
    };
    for (std::size_t side = 0; side < inputs.size(); ++side) {
      if (side && set.setKind == SetKind::Except) break;
      const auto &child = plan.nodes.at(set.inputs[side]);
      const auto expected = side ? result.setOperation->right : result.setOperation->left;
      const auto &query = modelQuery(plan, expected);
      if (child.query != expected || query.projections.size() != width ||
          child.outputs.size() != width || !identifiedOutputs(child)) {
        if (set.setKind == SetKind::Union) return true;
        continue;
      }
      const ColumnSet notNull = [&] {
        ColumnSet columns;
        for (const auto &field : inputs[side]->nonNullColumns) columns.insert(key(field));
        return columns;
      }();
      std::map<ColumnKey,const IntegerRanges *> domains;
      if (inputs[side]->integerDomains)
        for (const auto &domain:*inputs[side]->integerDomains)
          domains.emplace(key(domain.column),&domain.ranges);
      std::map<ColumnKey, std::size_t> exact;
      for (std::size_t i = 0; i < width; ++i) {
        if (limits.shouldStop && limits.shouldStop()) {
          fail(MetadataStatus::Stopped, "set operation key proof cancelled");
          return false;
        }
        const auto &source = modelExpression(plan, {query.id, query.projections[i].expression});
        const auto &target = modelExpression(plan, {result.id, result.projections[i].expression});
        const auto from = signedIntegerBits(plan.program.types.get(source.sqlType));
        const auto to = signedIntegerBits(plan.program.types.get(target.sqlType));
        const auto field = key(child.outputs[i]);
        // A PostgreSQL signed integer widening preserves values and NULLs.
        const bool injective = from && to && *from <= *to;
        if (injective) exact.emplace(field, i);
        const bool known = injective && notNull.count(field);
        if (set.setKind == SetKind::Union) nonNull[i] = nonNull[i] && known;
        else nonNull[i] = nonNull[i] || known;
        if (known)
          if (const auto domain=domains.find(field);domain!=domains.end())
            branchDomains[side][i]=domain->second;
      }
      if (set.setKind == SetKind::Union) continue;
      // INTERSECT is a sub-multiset of either input, EXCEPT of the left input.
      for (const auto &fields : inputs[side]->uniqueKeys) {
        if (out.uniqueKeys.size() >= 64) break;
        if (fields.empty()) continue;
        std::vector<ColumnRef> mapped;
        for (const auto &field : fields) {
          const auto ordinal = exact.find(key(field));
          if (ordinal == exact.end()) break;
          mapped.push_back(set.outputs[ordinal->second]);
        }
        if (mapped.size() == fields.size() &&
            std::none_of(out.uniqueKeys.begin(), out.uniqueKeys.end(), [&](const auto &candidate) {
              return candidate.size() == mapped.size() &&
                     std::equal(candidate.begin(), candidate.end(), mapped.begin(), equal);
            }))
          out.uniqueKeys.push_back(std::move(mapped));
      }
    }
    std::vector<ColumnIntegerDomain> domains;
    for (std::size_t i = 0; i < width; ++i) {
      if (nonNull[i]) out.nonNullColumns.push_back(set.outputs[i]);
      const auto *left=branchDomains[0][i], *right=branchDomains[1][i];
      if (set.setKind==SetKind::Union && (!left || !right)) continue;
      if (!left && !right) continue;
      if (set.setKind==SetKind::Intersect && left && right) {
        auto overlap=intersectSearchRanges(*left,*right);
        if (overlap.empty()) out.rows={0,Count{0}};
        else if (domains.size()<maxDomainColumns) domains.push_back({set.outputs[i],std::move(overlap)});
        continue;
      }
      if (domains.size()==maxDomainColumns) continue;
      auto ranges=left?*left:*right;
      if (set.setKind==SetKind::Union) {
        ranges.insert(ranges.end(),right->begin(),right->end());
        ranges=normalizedSearchRanges(std::move(ranges));
      }
      domains.push_back({set.outputs[i],std::move(ranges)});
    }
    if (!domains.empty())
      out.integerDomains=std::make_shared<const std::vector<ColumnIntegerDomain>>(std::move(domains));
    return true;
  }
  const RelProperties *get(RelId id,std::size_t depth=0) {
    if(state!=MetadataStatus::Ready)return nullptr;
    if(limits.shouldStop && limits.shouldStop())return fail(MetadataStatus::Stopped,"relational metadata cancelled");
    if(auto found=cache.find(id);found!=cache.end())return &found->second;
    if(!id || id>=plan.nodes.size())return fail(MetadataStatus::InvalidGraph,"invalid relational metadata node reference");
    if(depth>=limits.maxDepth || cache.size()+active.size()>=limits.maxNodes)
      return fail(MetadataStatus::ResourceLimit,"relational metadata node/depth limit exceeded");
    if(!active.insert(id).second)return fail(MetadataStatus::InvalidGraph,"cycle in relational metadata input");
    struct Leave {std::set<RelId>& ids;RelId id;~Leave(){ids.erase(id);}} leave{active,id};
    const auto &n=plan.nodes[id];
    if(n.outputs.size()>4096 || n.keys.size()>4096 || n.predicates.size()>4096 || n.inputs.size()>4096)
      return fail(MetadataStatus::ResourceLimit,"relational metadata payload limit exceeded");
    if (n.groupingSets.size()>256) return fail(MetadataStatus::ResourceLimit,"grouping set metadata limit exceeded");
    if (!n.groupingSets.empty() && (n.kind!=RelKind::Aggregate || n.distinct))
      return fail(MetadataStatus::InvalidGraph,"grouping sets require an ordinary aggregate node");
    std::size_t groupingReferences=0;
    for (const auto &set:n.groupingSets) {
      if(set.size()>8192-groupingReferences)return fail(MetadataStatus::ResourceLimit,"grouping metadata reference limit exceeded");
      groupingReferences+=set.size();
      for (auto ref:set) if(std::none_of(n.keys.begin(),n.keys.end(),[&](const auto &key){return key.query==ref.query && key.expression==ref.expression;}))
        return fail(MetadataStatus::InvalidGraph,"grouping metadata reference is not in the key union");
    }
    const auto arity=n.inputs.size();
    const bool leaf=n.kind==RelKind::Values || n.kind==RelKind::Scan || n.kind==RelKind::WorkingTableScan;
    const bool membership=n.kind==RelKind::SemiJoin || n.kind==RelKind::AntiJoin;
    const bool binary=membership || n.kind==RelKind::Join || n.kind==RelKind::SetOperation ||
                      n.kind==RelKind::RepeatUnion || n.kind==RelKind::Correlate;
    const bool variable=n.kind==RelKind::Subquery || n.kind==RelKind::Modify;
    const auto primary=n.kind==RelKind::SetOperation && arity>=2 ? arity :
        leaf || n.kind==RelKind::TableFunction ? 0u : binary?2u:1u;
    if((leaf && arity) || (!leaf && !variable && arity<primary) ||
        (n.kind==RelKind::Subquery && !arity))
      return fail(MetadataStatus::InvalidGraph,"invalid relational metadata input arity");
    // The builder appends scalar/IN/EXISTS dependencies after the row-producing
    // input. They are evaluation dependencies, not a Cartesian product and do
    // not change the primary input's cardinality. Reject arbitrary extra scans.
    if(!variable)for(std::size_t i=primary;i<arity;++i) {
      const auto child=n.inputs[i];
      if(!child || child>=plan.nodes.size() || plan.nodes[child].kind!=RelKind::Subquery || !plan.nodes[child].scalar)
        return fail(MetadataStatus::InvalidGraph,"extra relational input is not a scalar subquery dependency");
    }
    std::vector<const RelProperties*> inputs;bool dependencyEffects=false;
    for(std::size_t i=0;i<arity;++i) {
      auto input=get(n.inputs[i],depth+1);if(!input)return nullptr;
      if(variable || i<primary)inputs.push_back(input);
      else dependencyEffects=true; // execution count / possible exceptions remain barriers
    }
    RelProperties p;
    if(!inputs.empty())p=*inputs[0];
    const auto inputDomains=std::exchange(p.integerDomains,{});
    const auto preserveDomains=[&] {
      if (!inputDomains) return;
      const auto &input=plan.nodes.at(n.inputs.front());
      if (n.outputs.size()==input.outputs.size() &&
          std::equal(n.outputs.begin(),n.outputs.end(),input.outputs.begin(),equal))
        p.integerDomains=inputDomains;
    };
    const auto preserveRetainedFields=[&](const std::vector<std::size_t> &ordinals) {
      const auto &input=plan.nodes.at(n.inputs.front());
      p.nonNullColumns.clear();
      for (std::size_t i=0;i<ordinals.size();++i) {
        const auto &column=input.outputs.at(ordinals[i]);
        if (!equal(column,n.outputs.at(i))) return false;
        if (contains(inputs.front()->nonNullColumns,column)) p.nonNullColumns.push_back(column);
      }
      if (inputDomains) {
        std::vector<ColumnIntegerDomain> domains;
        const auto retained=n.outputs.begin()+ordinals.size();
        for (const auto &domain:*inputDomains)
          if (std::any_of(n.outputs.begin(),retained,[&](const auto &column) { return equal(column,domain.column); }))
            domains.push_back(domain);
        if (!domains.empty()) p.integerDomains=std::make_shared<const std::vector<ColumnIntegerDomain>>(std::move(domains));
      }
      return true;
    };
    p.mayHaveEffects |= dependencyEffects;
    for(const auto *input:inputs)p.mayHaveEffects |= input->mayHaveEffects;
    auto expressions=n.keys;
    if (!membership)
      expressions.insert(expressions.end(),n.predicates.begin(),n.predicates.end());
    for (const auto &window:n.windowGroups)
      expressions.insert(expressions.end(),window.functions.begin(),window.functions.end());
    if(n.kind==RelKind::Aggregate || n.kind==RelKind::Window) {
      const auto &q=modelQuery(plan,n.query);
      if(q.projections.size()>4096)return fail(MetadataStatus::ResourceLimit,"relational metadata projection budget exceeded");
      for(const auto &projection:q.projections)expressions.push_back({q.id,projection.expression});
    }
    for(auto ref:expressions) {
      const auto &q=modelQuery(plan,ref.query);
      std::vector<compiler::ExprId> work{ref.expression};std::set<compiler::ExprId> seen;
      while(!work.empty()) {
        auto e=work.back();work.pop_back();if(!seen.insert(e).second)continue;
        if(++expressionVisits>limits.maxNodes*16)return fail(MetadataStatus::ResourceLimit,"relational metadata expression budget exceeded");
        if((expressionVisits&127u)==0 && limits.shouldStop && limits.shouldStop())return fail(MetadataStatus::Stopped,"relational metadata cancelled");
        if(!e || e>=q.expressions.size())return fail(MetadataStatus::InvalidGraph,"invalid metadata expression reference");
        const auto &expr=q.expressions[e];
        // Immutability alone does not prove exception-free evaluation (casts,
        // integer overflow, immutable UDFs). Limit rewrites preserve observable
        // evaluation boundaries unless both properties can be established.
        using E=compiler::ExprKind;
        bool safeCorrelation=false;
        if (plan.program.sourceDialect==compiler::Dialect::PostgreSQL && q.scope.parent &&
            expr.kind==E::Binary && expr.value=="=") {
          const auto keys=correlatedIntegerEqualityKey(plan,{q.id,e},*q.scope.parent);
          safeCorrelation=keys &&
              signedIntegerBits(plan.program.types.get(modelExpression(plan,keys->left).sqlType)) &&
              signedIntegerBits(plan.program.types.get(modelExpression(plan,keys->right).sqlType));
        }
        const bool primitive=expr.kind==E::Name || expr.kind==E::Number || expr.kind==E::String ||
          expr.kind==E::Null || expr.kind==E::Boolean || expr.kind==E::Star || expr.kind==E::IsNull ||
          (expr.kind==E::Unary && expr.value=="NOT") ||
          (expr.kind==E::Binary && (expr.value=="AND" || expr.value=="OR")) ||
          safeCorrelation;
        if(expr.query || (!primitive && expr.kind!=E::Call))p.mayHaveEffects=true;
        if(expr.kind==E::Call) {
          auto op=compiler::builtinOperator(plan.program.sourceDialect,expr.name,expr.nameQuoted,expr.operands.size());
          if(expr.routineResolved || !op || op->mayThrow ||
              compiler::callVolatility(expr,plan.program.sourceDialect)!=compiler::FunctionVolatility::Immutable)
            p.mayHaveEffects=true;
        }
        work.insert(work.end(),expr.operands.begin(),expr.operands.end());
      }
    }
    switch(n.kind) {
    case RelKind::Values:
      p.rows={Count(n.valueCount),Count(n.valueCount)};p.uniqueKeys.clear();p.nonNullColumns.clear();break;
    case RelKind::WorkingTableScan:
      if (!n.workingTableOf || !n.workingSeed || n.workingSeed >= id)
        return fail(MetadataStatus::InvalidGraph,"invalid working table estimate reference");
      p.rows={0,{}};
      p.mayHaveEffects=true; // Iteration-dependent input is a rewrite boundary.
      break;
    case RelKind::TableFunction:
      p.rows={0,{}};
      p.mayHaveEffects=true; // JSON document/path evaluation can raise an error.
      break;
    case RelKind::Sample:
      if (arity!=1 || !n.sampling || n.sampling->milliPercent>compiler::TableSampling::full)
        return fail(MetadataStatus::InvalidGraph,"invalid sampling relation");
      preserveDomains();
      if (n.sampling->milliPercent==0)
        p.rows={0,0};
      else if (n.sampling->milliPercent!=compiler::TableSampling::full) {
        p.rows.minimum=0;
        p.mayHaveEffects=true;
      }
      break;
    case RelKind::Series:
    case RelKind::Unnest: {
      const bool series=n.kind==RelKind::Series;
      if (!arity || (series?(n.keys.size()<2 || n.keys.size()>3):n.keys.size()!=1))
        return fail(MetadataStatus::InvalidGraph,"invalid PostgreSQL row expansion relation");
      const auto &input=plan.nodes.at(n.inputs.front()).outputs;
      if (n.outputs.size()!=input.size()+(n.withOrdinality?2:1) ||
          !std::equal(input.begin(),input.end(),n.outputs.begin(),equal))
        return fail(MetadataStatus::InvalidGraph,"row expansion changes input field identity");
      p.integerDomains=inputDomains;
      p.uniqueKeys.clear();
      const auto addKey=[&](const ColumnRef &output) {
        p.nonNullColumns.push_back(output);
        // Numbering restarts per input row, so multiple input rows need
        // their own non-null key to make a key with the generated field.
        if (p.rows.maximum && *p.rows.maximum<=1)
          p.uniqueKeys.push_back({output});
        else
          for (const auto &key:inputs.front()->uniqueKeys) {
            if (key.empty() || !std::all_of(key.begin(),key.end(),[&](const auto &field) {
                  return contains(p.nonNullColumns,field);
                })) continue;
            p.uniqueKeys.push_back(key);
            p.uniqueKeys.back().push_back(output);
            if (p.uniqueKeys.size()==64) break;
          }
      };
      if (identifiedOutputs(n)) {
        if (n.withOrdinality) addKey(n.outputs.back());
        if (series && p.uniqueKeys.size()<64) addKey(n.outputs[input.size()]);
      }
      RowBounds perInput{0,std::nullopt};
      if (series) {
        if (const auto elements=postgresIntegerSeriesRows(plan,n)) perInput={*elements,*elements};
      } else {
        const auto literal=postgresIntegerArrayLiteral(plan,n.keys.front(),4096);
        if (literal.status==IntegerArrayStatus::Ready)
          perInput={Count(literal.elements.size()),Count(literal.elements.size())};
        else if (const auto query=postgresIntegerArrayQuery(plan,n); query && !query->correlated) {
          const auto *source=get(query->root,depth+1);
          if (!source) return nullptr;
          perInput=source->rows;
        }
      }
      p.rows.minimum=multiply(p.rows.minimum,perInput.minimum).value_or(UINT64_MAX);
      p.rows.maximum=multiply(p.rows.maximum,perInput.maximum);
      p.mayHaveEffects|=series || modelExpression(plan,n.keys.front()).kind!=compiler::ExprKind::Name;
      break;
    }
    case RelKind::Match:
      if (arity!=1 || !n.table || *n.table>=modelQuery(plan,n.query).tables.size() ||
          !modelQuery(plan,n.query).tables[*n.table].rowMatch)
        return fail(MetadataStatus::InvalidGraph,"invalid row pattern relation");
      p.rows.minimum=0;
      p.uniqueKeys.clear();
      p.nonNullColumns.clear();
      p.mayHaveEffects=true;
      break;
    case RelKind::Unpivot: {
      const auto &q=modelQuery(plan,n.query);
      if (arity!=1 || !n.table || *n.table>=q.tables.size())
        return fail(MetadataStatus::InvalidGraph,"invalid UNPIVOT relation");
      const auto &table=q.tables[*n.table];
      const auto &child=plan.nodes.at(n.inputs.front());
      const auto &shape=table.unpivot;
      if (!shape || !table.definition || !table.unpivotInput ||
          child.outputs.size()!=table.unpivotInput->columns.size() ||
          n.outputs.size()!=table.definition->columns.size() ||
          n.outputs.size()!=shape->retainedOrdinals.size()+shape->descriptors.size()+shape->measures.size() ||
          shape->descriptors.empty() || shape->measures.empty() || shape->inputOrdinals.empty() ||
          shape->inputOrdinals.size()!=shape->inputs.size())
        return fail(MetadataStatus::InvalidGraph,"invalid UNPIVOT row mapping");
      const auto sourceNonNull=[&](std::size_t field) {
        return contains(inputs.front()->nonNullColumns,child.outputs.at(field));
      };
      const bool include=shape->includeNulls.value_or(false);
      Count guaranteed=0;
      std::vector<bool> guaranteedMeasure(shape->measures.size(),true);
      for (const auto &fields:shape->inputOrdinals) {
        if (fields.size()!=shape->measures.size())
          return fail(MetadataStatus::InvalidGraph,"invalid UNPIVOT measure mapping");
        bool hasNonNull=false;
        for (std::size_t i=0;i<fields.size();++i) {
          const bool nonNull=sourceNonNull(fields[i]);
          guaranteedMeasure[i]=guaranteedMeasure[i] && nonNull;
          hasNonNull|=nonNull;
        }
        guaranteed+=hasNonNull;
      }
      const auto branches=Count(shape->inputOrdinals.size());
      p.rows.minimum=multiply(p.rows.minimum,include?branches:guaranteed).value_or(UINT64_MAX);
      p.rows.maximum=multiply(p.rows.maximum,branches);
      p.uniqueKeys.clear();
      if (!preserveRetainedFields(shape->retainedOrdinals))
        return fail(MetadataStatus::InvalidGraph,"UNPIVOT changed a retained input column identity");
      const auto firstDescriptor=shape->retainedOrdinals.size();
      const auto firstMeasure=firstDescriptor+shape->descriptors.size();
      for (std::size_t i=firstDescriptor;i<firstMeasure;++i)
        if (table.definition->columns[i].nonNullKnown) p.nonNullColumns.push_back(n.outputs[i]);
      for (std::size_t i=0;i<shape->measures.size();++i)
        if (guaranteedMeasure[i] || (!include && shape->measures.size()==1))
          p.nonNullColumns.push_back(n.outputs[firstMeasure+i]);
      break;
    }
    case RelKind::Pivot: {
      const auto &q=modelQuery(plan,n.query);
      if (arity!=1 || !n.table || *n.table>=q.tables.size())
        return fail(MetadataStatus::InvalidGraph,"invalid PIVOT relation");
      const auto &table=q.tables[*n.table];
      const auto &child=plan.nodes.at(n.inputs.front());
      const auto &shape=table.pivot;
      if (!shape || !table.definition || !table.pivotInput || shape->axisOrdinals.empty() ||
          std::any_of(shape->axisOrdinals.begin(),shape->axisOrdinals.end(),[&](auto axis) {
            return axis>=child.outputs.size();
          }) ||
          (shape->argumentOrdinal && *shape->argumentOrdinal>=child.outputs.size()) ||
          child.outputs.size()!=table.pivotInput->columns.size() ||
          n.outputs.size()!=table.definition->columns.size() ||
          n.outputs.size()!=shape->groupingOrdinals.size()+shape->buckets.size() || shape->buckets.empty())
        return fail(MetadataStatus::InvalidGraph,"invalid PIVOT row mapping");
      const auto groups=shape->groupingOrdinals.size();
      if (!preserveRetainedFields(shape->groupingOrdinals))
        return fail(MetadataStatus::InvalidGraph,"PIVOT changed an implicit grouping column identity");
      p.uniqueKeys.clear();
      const auto dialect=plan.program.sourceDialect;
      const bool singleEmptyGroup=dialect==compiler::Dialect::Oracle || dialect==compiler::Dialect::PostgreSQL;
      const auto groupEnd=n.outputs.begin()+groups;
      if (!groups) {
        p.rows={singleEmptyGroup || p.rows.minimum?Count{1}:Count{0},Count{1}};
      } else {
        for (const auto &key:inputs.front()->uniqueKeys) {
          if (!key.empty() && std::all_of(key.begin(),key.end(),[&](const auto &field) {
                return std::any_of(n.outputs.begin(),groupEnd,[&](const auto &column) { return equal(field,column); });
              })) p.uniqueKeys.push_back(key);
        }
        if (p.uniqueKeys.empty()) p.rows.minimum=p.rows.minimum?1:0;
        if (p.nonNullColumns.size()==groups && p.uniqueKeys.empty())
          p.uniqueKeys.emplace_back(n.outputs.begin(),groupEnd);
      }
      if (singleEmptyGroup && shape->aggregate=="COUNT")
        p.nonNullColumns.insert(p.nonNullColumns.end(),groupEnd,n.outputs.end());
      // Evaluation may fail during bucket coercion or numeric aggregation.
      p.mayHaveEffects=true;
      break;
    }
    case RelKind::Scan: {
      p.rows={0,{}};p.uniqueKeys.clear();p.nonNullColumns.clear();
      const auto &q=modelQuery(plan,n.query);
      if(!n.table || *n.table>=q.tables.size())return fail(MetadataStatus::InvalidGraph,"invalid scan metadata table reference");
      const auto &source=q.tables[*n.table];
      const auto &table=sourceDefinition(source);
      const bool verifiedBase=table && q.catalogChecked && table->kind==compiler::ObjectKind::Table;
      const bool verifiedMaterialization=table && q.catalogChecked &&
          table->kind==compiler::ObjectKind::MaterializedView && n.scanSource==ScanSource::MaterializedView;
      const bool scansOnlyKnownRows=plan.program.sourceDialect!=compiler::Dialect::PostgreSQL ||
          source.inheritance==compiler::TableInheritance::Only ||
          q.tableQuery==compiler::TableQuerySyntax::Only || (table && table->noDescendantsKnown);
      p.mayHaveEffects |= !verifiedBase && !verifiedMaterialization;
      if(verifiedBase && scansOnlyKnownRows) {
        if(table->columns.size()>4096)return fail(MetadataStatus::ResourceLimit,"relational non-null column budget exceeded");
        for(const auto &column:table->columns)if(column.nonNullKnown) {
          ColumnRef col{n.query,*n.table,column.name};if(contains(n.outputs,col))p.nonNullColumns.push_back(std::move(col));
        }
      }
      if(verifiedBase && scansOnlyKnownRows && table->primaryKeyKnown && !table->primaryKey.empty()) {
        if(table->primaryKey.size()>4096)return fail(MetadataStatus::ResourceLimit,"relational metadata key limit exceeded");
        std::vector<ColumnRef> key;
        for(const auto &name:table->primaryKey) {
          ColumnRef c{n.query,*n.table,name};
          if(!contains(n.outputs,c)){key.clear();break;}key.push_back(std::move(c));
        }
        if(!key.empty()){p.uniqueKeys.push_back(key);for(auto &col:key)if(!contains(p.nonNullColumns,col))p.nonNullColumns.push_back(std::move(col));}
      }
      if (verifiedBase && scansOnlyKnownRows && plan.program.sourceDialect==compiler::Dialect::PostgreSQL) {
        if (table->strictUniqueKeys.size()>64)
          return fail(MetadataStatus::ResourceLimit,"relational secondary key proof count exceeded");
        for (const auto &names:table->strictUniqueKeys) {
          if (names.empty() || names.size()>32) continue;
          std::vector<ColumnRef> fields;
          std::set<std::string_view> seen;
          for (const auto &name:names) {
            const auto column=std::find_if(table->columns.begin(),table->columns.end(),[&](const auto &field) {
              return field.name==name && field.nonNullKnown && field.sqlType && signedIntegerBits(*field.sqlType);
            });
            ColumnRef field{n.query,*n.table,name};
            if (column==table->columns.end() || !contains(n.outputs,field) || !seen.insert(name).second) break;
            fields.push_back(std::move(field));
          }
          if (fields.size()==names.size()) p.uniqueKeys.push_back(std::move(fields));
        }
      }
      break;
    }
    case RelKind::Calc:
      p.integerDomains=inputDomains;
      if (!n.predicates.empty()) {
        p.rows.minimum=0;
        if (!strengthenFilter(n, plan.nodes.at(n.inputs.front()), p)) return nullptr;
      }
      if (n.keys.empty()) {
        if (arity!=1 || n.outputs.size()!=plan.nodes[n.inputs.front()].outputs.size() ||
            !std::equal(n.outputs.begin(),n.outputs.end(),plan.nodes[n.inputs.front()].outputs.begin(),equal))
          return fail(MetadataStatus::InvalidGraph,"invalid identity Calc row shape");
        break;
      }
      if (arity!=1 || n.keys.size()!=n.outputs.size())
        return fail(MetadataStatus::InvalidGraph,"invalid Calc projection row shape");
      [[fallthrough]];
    case RelKind::Project: {
      const auto sourceDomains=n.kind==RelKind::Calc?std::exchange(p.integerDomains,{}):inputDomains;
      p.uniqueKeys.clear();
      auto inputNonNull = std::move(p.nonNullColumns);
      p.nonNullColumns.clear();
      if(inputs.empty())return fail(MetadataStatus::InvalidGraph,"project metadata requires an input");
      const auto &child=plan.nodes[n.inputs[0]];
      std::vector<std::pair<ColumnRef,ColumnRef>> mapping;
      if(n.keys.size()==n.outputs.size())
        for(std::size_t i=0;i<n.keys.size();++i) {
          auto source=n.keys[i];
          const auto &e=modelExpression(plan,source);
          if (plan.program.sourceDialect==compiler::Dialect::PostgreSQL &&
              e.kind==compiler::ExprKind::Cast && e.operands.size()==1) {
            const ExpressionRef operand{source.query,e.operands.front()};
            const auto from=signedIntegerBits(plan.program.types.get(modelExpression(plan,operand).sqlType));
            const auto to=signedIntegerBits(plan.program.types.get(e.sqlType));
            if (from && to && *from<=*to) source=operand;
          }
          if(modelExpression(plan,source).kind==compiler::ExprKind::Name)
            if(auto src=resolveColumn(plan,source))mapping.push_back({*src,n.outputs[i]});
        }
      else if(n.keys.size()==1 && modelExpression(plan,n.keys[0]).kind==compiler::ExprKind::Star)
        for(const auto &col:n.outputs)if(contains(child.outputs,col))mapping.push_back({col,col});
      auto mapped=[&](const ColumnRef &source)->std::optional<ColumnRef>{for(const auto &m:mapping)if(equal(m.first,source))return m.second;return {};};
      for(const auto &source:inputs[0]->uniqueKeys) {
        std::vector<ColumnRef> key;bool complete=true;
        for(const auto &col:source){auto m=mapped(col);if(!m){complete=false;break;}key.push_back(*m);}
        if(complete)p.uniqueKeys.push_back(std::move(key));
      }
      for(const auto &col:inputNonNull)if(auto m=mapped(col))p.nonNullColumns.push_back(*m);
      if (sourceDomains && identifiedOutputs(n)) {
        std::vector<ColumnIntegerDomain> domains;
        for (const auto &domain:*sourceDomains)
          if (auto column=mapped(domain.column)) domains.push_back({std::move(*column),domain.ranges});
        if (!domains.empty())
          p.integerDomains=std::make_shared<const std::vector<ColumnIntegerDomain>>(std::move(domains));
      }
      projectedExtremumKeys(n, child, p);
      if (state != MetadataStatus::Ready) return nullptr;
      break;
    }
    case RelKind::Filter:
      preserveDomains();
      p.rows.minimum=0;
      if (!strengthenFilter(n, plan.nodes.at(n.inputs.front()), p)) return nullptr;
      break;
    case RelKind::Sort:case RelKind::Window:preserveDomains();break;
    case RelKind::Coerce: {
      if (arity != 1 || !n.keys.empty() || !n.predicates.empty() ||
          n.integerWidenings.empty() || n.integerWidenings.size() > n.outputs.size())
        return fail(MetadataStatus::InvalidGraph,"invalid integer coercion input shape");
      const auto &child = plan.nodes.at(n.inputs.front());
      if (n.outputs.size() != child.outputs.size() ||
          !std::equal(n.outputs.begin(), n.outputs.end(), child.outputs.begin(), equal))
        return fail(MetadataStatus::InvalidGraph,"integer coercion changed the output binding");
      std::set<std::size_t> columns;
      for (const auto &conversion : n.integerWidenings) {
        if (conversion.ordinal >= n.outputs.size() || !columns.insert(conversion.ordinal).second ||
            conversion.source >= plan.program.types.size() || conversion.target >= plan.program.types.size())
          return fail(MetadataStatus::InvalidGraph,"invalid positional integer coercion");
        const auto from = signedIntegerBits(plan.program.types.get(conversion.source));
        const auto to = signedIntegerBits(plan.program.types.get(conversion.target));
        if (!from || !to || *from >= *to)
          return fail(MetadataStatus::InvalidGraph,"integer coercion is not a signed widening");
      }
      preserveDomains();
      break; // Injective and order-preserving, including NULL.
    }
    case RelKind::Limit: {
      if(inputs.empty())return fail(MetadataStatus::InvalidGraph,"limit metadata requires an input");
      preserveDomains();
      const auto offset=number(n.rowOffset),limit=number(n.rowLimit);
      if(offset) {
        p.rows.minimum=p.rows.minimum>*offset?p.rows.minimum-*offset:0;
        if(p.rows.maximum)p.rows.maximum=*p.rows.maximum>*offset?*p.rows.maximum-*offset:0;
      } else p.rows.minimum=0;
      if(limit) {
        p.rows.minimum=std::min(p.rows.minimum,*limit);
        if (!n.withTies || *limit == 0)
          p.rows.maximum=p.rows.maximum?std::min(*p.rows.maximum,*limit):*limit;
      } else if(!n.keys.empty())p.rows.minimum=0;
      break;
    }
    case RelKind::Aggregate: {
      if(inputs.empty())return fail(MetadataStatus::InvalidGraph,"aggregate metadata requires an input");
      p.uniqueKeys.clear();
      p.nonNullColumns = groupedNonNullColumns(n, *inputs.front());
      if (state != MetadataStatus::Ready) return nullptr;
      if(!n.groupingSets.empty()) {
        p.rows={0,Count{0}};
        for(const auto &set:n.groupingSets) {
          const auto minimum=set.empty()?Count{1}:inputs[0]->rows.minimum>0?Count{1}:Count{0};
          p.rows.minimum+=minimum;
          p.rows.maximum=add(p.rows.maximum,set.empty()?std::optional<Count>{Count{1}}:inputs[0]->rows.maximum);
        }
        // Duplicates and subtotal NULLs can collide even with distinct sets.
        // No grouping-id/discriminator uniqueness proof is modeled here.
        if(n.groupingSets.size()>1 || n.groupingSets.front().empty())break;
      }
      if(n.keys.empty() && !n.distinct)p.rows={1,Count{1}};
      else {
        p.rows.minimum=inputs[0]->rows.minimum>0?1:0;
        if(n.distinct) {
          if(!n.outputs.empty() && std::none_of(n.outputs.begin(),n.outputs.end(),[](const auto& c){return c.name=="*";}))p.uniqueKeys.push_back(n.outputs);
        } else {
          p.uniqueKeys = groupedUniqueKeys(n, *inputs.front());
        }
      }
      break;
    }
    case RelKind::SemiJoin:case RelKind::AntiJoin: {
      if(inputs.size()!=2 || !membershipColumns(plan,n))
        return fail(MetadataStatus::InvalidGraph,"membership join metadata requires a verified equality key");
      const auto &left=plan.nodes[n.inputs[0]];
      if(n.outputs.size()!=left.outputs.size() ||
         !std::equal(n.outputs.begin(),n.outputs.end(),left.outputs.begin(),equal))
        return fail(MetadataStatus::InvalidGraph,"membership join changes left output identity");
      preserveDomains();
      const auto &right=inputs[1]->rows;
      p.rows.minimum=0;
      if(right.maximum && !*right.maximum)
        p.rows=n.kind==RelKind::AntiJoin?inputs[0]->rows:RowBounds{0,Count{0}};
      break;
    }
    case RelKind::Correlate:
    case RelKind::Join: {
      if(inputs.size()!=2)return fail(MetadataStatus::InvalidGraph,"join metadata requires two inputs");
      auto a=inputs[0]->rows,b=inputs[1]->rows;
      p.rows={0,multiply(a.maximum,b.maximum)};p.uniqueKeys.clear();p.nonNullColumns.clear();
      if(n.join==compiler::JoinKind::Left){p.rows.minimum=a.minimum;p.rows.maximum=multiply(a.maximum,b.maximum?std::optional<Count>{std::max<Count>(1,*b.maximum)}:std::nullopt);}
      else if(n.join==compiler::JoinKind::Right){p.rows.minimum=b.minimum;p.rows.maximum=multiply(b.maximum,a.maximum?std::optional<Count>{std::max<Count>(1,*a.maximum)}:std::nullopt);}
      else if(n.join==compiler::JoinKind::Full){p.rows.minimum=std::max(a.minimum,b.minimum);auto sum=add(a.maximum,b.maximum);if(p.rows.maximum && sum)p.rows.maximum=std::max(*p.rows.maximum,*sum);else p.rows.maximum={};}
      auto preserve=[&](const RelProperties &input){for(const auto &col:input.nonNullColumns)if(p.nonNullColumns.size()<4096 && contains(n.outputs,col) && !contains(p.nonNullColumns,col))p.nonNullColumns.push_back(col);};
      if(n.join==compiler::JoinKind::Inner || n.join==compiler::JoinKind::Cross || n.join==compiler::JoinKind::Left)preserve(*inputs[0]);
      if(n.join==compiler::JoinKind::Inner || n.join==compiler::JoinKind::Cross || n.join==compiler::JoinKind::Right)preserve(*inputs[1]);
      if (n.kind == RelKind::Correlate) p.mayHaveEffects=true;
      else if (!strengthenJoin(n, *inputs[0], *inputs[1], p)) return nullptr;
      if (!joinDomains(n,*inputs[0],*inputs[1],p)) return nullptr;
      break;
    }
    case RelKind::SetOperation: {
      if(inputs.size()<2)
        return fail(MetadataStatus::InvalidGraph,"invalid relational set input arity");
      const auto &a = inputs[0]->rows;
      p.rows.minimum = 0;
      p.uniqueKeys.clear();
      p.nonNullColumns.clear();
      switch (n.setKind) {
      case compiler::SetKind::Union:
        p.rows.maximum = Count{0};
        for (const auto *input : inputs) {
          p.rows.minimum = n.distinct ? Count{p.rows.minimum || input->rows.minimum}
              : add(p.rows.minimum, input->rows.minimum).value_or(UINT64_MAX);
          p.rows.maximum = add(p.rows.maximum, input->rows.maximum);
        }
        break;
      case compiler::SetKind::Intersect:
        p.rows.maximum = a.maximum;
        for (const auto *input : inputs) tighten(p.rows.maximum, input->rows.maximum);
        break;
      case compiler::SetKind::Except: {
        p.rows.maximum = a.maximum;
        std::optional<Count> removed=Count{0};
        for (std::size_t i=1; i<inputs.size(); ++i) removed=add(removed,inputs[i]->rows.maximum);
        if (removed)
          p.rows.minimum = n.distinct ? Count{!*removed && a.minimum > 0}
                                      : subtractFloor(a.minimum,*removed);
        break;
      }
      }
      // DISTINCT compares the entire coerced output row. Keep this proof
      // only when every output field has a distinct representable identity.
      if (n.distinct && identifiedOutputs(n) &&
          std::all_of(n.inputs.begin(),n.inputs.end(),[&](auto input) {
            return n.outputs.size()==plan.nodes[input].outputs.size();
          }))
        p.uniqueKeys.push_back(n.outputs);
      if (!strengthenSet(n, inputs, p)) return nullptr;
      break;
    }
    case RelKind::RepeatUnion: {
      if (inputs.size() != 2 || n.workingTableOf != n.query || !n.workingTableOf)
        return fail(MetadataStatus::InvalidGraph,"repeat union requires seed and recursive step inputs");
      const auto &seed = inputs[0]->rows;
      const auto &step = inputs[1]->rows;
      p.rows.minimum = n.distinct && seed.minimum ? 1 : seed.minimum;
      p.rows.maximum = seed.maximum && (!*seed.maximum || (step.maximum && !*step.maximum))
                            ? seed.maximum : std::nullopt;
      p.uniqueKeys.clear();
      p.nonNullColumns.clear();
      break;
    }
    case RelKind::Rename:
      if (arity != 1 || n.outputs.size() != plan.nodes.at(n.inputs.front()).outputs.size() ||
          !n.keys.empty() || !n.predicates.empty())
        return fail(MetadataStatus::InvalidGraph, "invalid positional column rename");
      p.uniqueKeys.clear();
      p.nonNullColumns.clear();
      remapOutputProperties(n,plan.nodes[n.inputs.front()],*inputs.front(),p);
      break;
    case RelKind::Subquery: {
      p.mayHaveEffects |= n.materialized; // Respect the explicit optimizer fence.
      p.uniqueKeys.clear();
      p.nonNullColumns.clear();
      if (n.scalar || n.correlated || !n.table || inputs.size() != 1)
        break;
      const auto &owner = modelQuery(plan, n.query);
      if (*n.table >= owner.tables.size())
        return fail(MetadataStatus::InvalidGraph,"invalid derived table metadata reference");
      const auto &source = owner.tables[*n.table];
      const auto &definition=sourceDefinition(source);
      const auto &child = plan.nodes[n.inputs[0]];
      if (!source.query || source.query != child.query || !definition ||
          !definition->arityKnown || !definition->complete || definition->columns.size() != n.outputs.size())
        break;
      remapOutputProperties(n,child,*inputs.front(),p);
      break;
    }
    case RelKind::Modify:p.rows={0,{}};p.uniqueKeys.clear();p.nonNullColumns.clear();p.mayHaveEffects=true;break;
    }
    if(p.rows.maximum && p.rows.minimum>*p.rows.maximum)return fail(MetadataStatus::InvalidGraph,"inconsistent relational row bounds");
    std::size_t charge=sizeof(p)+128;
    for(const auto& key:p.uniqueKeys){charge+=sizeof(key);for(const auto &col:key)charge+=sizeof(col)+col.name.size();}
    for(const auto &col:p.nonNullColumns)charge+=sizeof(col)+col.name.size();
    if (p.integerDomains)
      for (const auto &domain:*p.integerDomains)
        charge+=sizeof(domain)+domain.column.name.size()+domain.ranges.size()*sizeof(IntegerRange);
    if(charge>(limits.maxBytes-bytes)/2)return fail(MetadataStatus::ResourceLimit,"relational metadata byte limit exceeded");
    bytes+=2*charge;return &cache.emplace(id,std::move(p)).first->second;
  }
};
RelMetadataQuery::RelMetadataQuery(const LogicalPlan &p,MetadataLimits l):d(std::make_unique<Impl>(p,std::move(l))){}
RelMetadataQuery::~RelMetadataQuery()=default;
const RelProperties *RelMetadataQuery::properties(RelId id) {
  try{return d->get(id);}catch(const std::exception& e){d->state=MetadataStatus::InvalidGraph;d->error=e.what();return nullptr;}
}
const ColumnLineage *RelMetadataQuery::columnOrigins(RelId id, std::size_t ordinal) {
  try { return d->getLineage(id, ordinal); }
  catch (const std::exception &e) { d->state = MetadataStatus::InvalidGraph; d->error = e.what(); return nullptr; }
}
MetadataStatus RelMetadataQuery::status() const{return d->state;}
const std::string &RelMetadataQuery::message() const{return d->error;}
std::size_t RelMetadataQuery::accountedBytes() const{return d->bytes;}
void RelMetadataQuery::clear(){d->cache.clear();d->active.clear();d->lineageCache.clear();d->lineageActive.clear();d->bytes=0;d->expressionVisits=0;d->lineageExpressionVisits=0;d->state=MetadataStatus::Ready;d->error.clear();}
} // namespace navidog::sql::optimizer
