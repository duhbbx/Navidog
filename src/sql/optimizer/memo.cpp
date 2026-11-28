#include "optimizer_internal.hpp"
#include "navidog/sql/compiler/operator_catalog.hpp"
#include "navidog/sql/optimizer/rel_metadata.hpp"
#include <algorithm>
#include <array>
#include <charconv>
#include <cmath>
#include <sstream>
#include <unordered_map>
namespace navidog::sql::optimizer {
namespace {
constexpr std::size_t assumedRecursiveIterations = 8;
using namespace compiler;
struct MemoLimit : PlannerBudget {};
struct Estimate {
  double rows = 0, bytes = 64;
  bool assumed = false;
};
struct Group {
  RelId original = 0;
  std::vector<RelId> alternatives;
  std::set<std::string> digests;
  Estimate estimate;
  RowBounds bounds;
};
double bounded(double n) {
  return std::isfinite(n) ? std::max(0.0, std::min(1e80, n)) : 1e80;
}
bool sameColumn(const ColumnRef &a, const ColumnRef &b) {
  return a.query == b.query && a.table == b.table && a.name == b.name;
}
Cost plus(Cost a, const Cost &b) {
  a.cpu = bounded(a.cpu + b.cpu);
  a.io = bounded(a.io + b.io);
  a.memory = std::max(a.memory, b.memory);
  return a;
}
class Memo {
  PlannerContext &c;
  LogicalPlan &logical;
  std::unique_ptr<RelMetadataQuery> metadata;
  std::size_t metadataBytes=0;
  const RelProperties *properties(RelId id) {
    if(!metadata)metadata=std::make_unique<RelMetadataQuery>(logical,MetadataLimits{c.options.maxNodes,c.options.maxDepth,std::min<std::size_t>(4*1024*1024,c.options.maxBytes),[this]{c.stop();return false;}});
    const auto *props=metadata->properties(id);const auto bytes=metadata->accountedBytes();
    if(bytes>metadataBytes){c.charge(bytes-metadataBytes);metadataBytes=bytes;}
    return props;
  }
  bool uniqueRows(RelId id) {
    const auto *props=properties(id);if(!props)return false;
    if(props->rows.maximum && *props->rows.maximum<=1)return true;
    const auto &out=logical.nodes.at(id).outputs;
    for(const auto &key:props->uniqueKeys)
      if(!key.empty() && std::all_of(key.begin(),key.end(),[&](const auto& col){return std::any_of(out.begin(),out.end(),[&](const auto& v){return sameColumn(v,col);});}))return true;
    return false;
  }
  std::vector<Group> groups{1};
  std::unordered_map<RelId, GroupId> owner;
  std::unordered_map<std::string, GroupId> interned;
  std::map<std::pair<QueryId, std::size_t>, TableStatistics> statistics;
  std::map<std::pair<QueryId, ExprId>, std::optional<int>> constantTruths;
  std::set<std::string> joinGraphs;
  std::map<std::string,RelId> sharedInputs;
  using TraitKey = std::pair<GroupId, std::string>;
  std::map<TraitKey, PlanId> winners;
  std::set<TraitKey> solving;
  PhysicalPlan *physical = nullptr;
  bool baseline = false;
  std::size_t expressionCount = 0, physicalCount = 0;
  const RelNode &node(RelId id) const { return logical.nodes.at(id); }
  const ModelSelect &query(QueryId id) const { return modelQuery(logical, id); }
  const TableStatistics &table(QueryId id, std::size_t index) {
    auto key = std::make_pair(id, index);
    auto found = statistics.find(key);
    if (found != statistics.end())
      return found->second;
    const auto &q = query(id);
    if (index >= q.tables.size())
      c.fail(q.origin, "invalid table statistics reference");
    const auto &t = q.tables[index];
    const auto &definition=sourceDefinition(t);
    std::vector<std::string> name = t.name;
    auto context = q.context;
    if (definition) {
      context.database=definition->database;
      context.schema=definition->schema;
      name={definition->name};
    }
    std::optional<TableStatistics> value;
    if (c.statistics && !t.workingTableOf)
      value = c.statistics->table(context, name);
    TableStatistics out;
    if (value)
      out = *value;
    else {
      out.database = context.database;
      out.schema = context.schema;
      out.name = name.empty() ? "unknown" : name.back();
      out.rows = c.options.assumedRows;
      out.averageRowBytes = c.options.assumedRowBytes;
      out.source = StatisticsSource::Assumed;
      out.provenance = "no local statistics; configured assumptions";
    }
    if (!std::isfinite(out.rows) || out.rows < 0 || out.rows > 1e15 ||
        !std::isfinite(out.averageRowBytes) || out.averageRowBytes <= 0 ||
        out.averageRowBytes > 1e9)
      c.fail(q.origin, "invalid table statistics: row count/width must be "
                       "finite and within limits");
    for (const auto &col : out.columns)
      if ((col.distinct &&
           (!std::isfinite(*col.distinct) || *col.distinct < 0 ||
            *col.distinct > out.rows + 1)) ||
          (col.nullFraction &&
           (!std::isfinite(*col.nullFraction) || *col.nullFraction < 0 ||
            *col.nullFraction > 1)) ||
          (col.averageBytes &&
           (!std::isfinite(*col.averageBytes) || *col.averageBytes < 0 ||
            *col.averageBytes > 1e9)))
        c.fail(q.origin, "invalid column distinct/null statistics");
    if (out.columns.size() > 4096 || out.indexes.size() > 4096)
      c.fail(q.origin, "table statistics exceed column/index limits");
    std::size_t bytes = sizeof(out) + out.provenance.size() + out.name.size() +
                        out.schema.size() + out.database.size();
    for (const auto &col : out.columns)
      bytes += sizeof(col) + col.name.size();
    for (const auto &index : out.indexes) {
      if (index.columns.size() > 4096 ||
          (index.pages && (!std::isfinite(*index.pages) || *index.pages < 0)))
        c.fail(q.origin, "invalid or excessive index statistics");
      bytes += sizeof(index) + index.name.size();
      for (const auto &col : index.columns)
        bytes += sizeof(col) + col.size();
    }
    c.charge(bytes * 2);
    if (!t.workingTableOf) c.result.statistics.push_back(out);
    return statistics.emplace(key, std::move(out)).first->second;
  }
  bool hasSourceStatistics(const ColumnRef &column) const {
    const auto &owner=query(column.query);
    if (column.table>=owner.tables.size()) return false;
    const auto &source=owner.tables[column.table];
    if (source.pivot)
      return source.pivotInput && std::any_of(source.pivot->groupingOrdinals.begin(),source.pivot->groupingOrdinals.end(),
          [&](auto field) { return source.pivotInput->columns.at(field).name==column.name; });
    if (!source.unpivot) return true;
    const auto contains=[&](const auto &names) { return std::find(names.begin(),names.end(),column.name)!=names.end(); };
    return !contains(source.unpivot->measures) && !contains(source.unpivot->descriptors);
  }
  std::pair<double, bool> ndv(const ColumnRef &col) {
    if (!hasSourceStatistics(col))
      return {std::max(1.0, std::sqrt(c.options.assumedRows)), true};
    const auto &t = table(col.query, col.table);
    for (const auto &value : t.columns)
      if (value.name == col.name && value.distinct)
        return {std::max(1.0, *value.distinct),
                t.source == StatisticsSource::Assumed};
    for (const auto &index : t.indexes)
      if (index.unique && index.columns.size() == 1 &&
          index.columns[0] == col.name)
        return {std::max(1.0, t.rows), t.source == StatisticsSource::Assumed};
    return {std::max(1.0, std::sqrt(t.rows)), true};
  }
  bool knownNullFraction(const ColumnRef &col) {
    if (!hasSourceStatistics(col))
      return false;
    const auto &t = table(col.query, col.table);
    return std::any_of(t.columns.begin(), t.columns.end(), [&](const auto &v) {
      return v.name == col.name && v.nullFraction.has_value();
    });
  }
  double nullFraction(const ColumnRef &col) {
    if (!hasSourceStatistics(col))
      return .1;
    const auto &t = table(col.query, col.table);
    for (const auto &value : t.columns)
      if (value.name == col.name && value.nullFraction)
        return *value.nullFraction;
    return 0.1;
  }
  struct Truth {
    double yes = .25, no = .65;
    bool assumed = true;
  };
  bool nullConstant(const ModelExpression &e) const {
    return e.kind == ExprKind::Null ||
           (logical.program.sourceDialect == Dialect::Oracle &&
            e.kind == ExprKind::String && e.value.empty());
  }
  // Exact, deliberately small constant domain. Never calls a function, folds
  // a subquery/cast, compares collation-sensitive strings or approximates a
  // decimal through floating point. -1 denotes SQL UNKNOWN.
  std::optional<int> constantPredicate(ExpressionRef ref, std::size_t depth,
                                       std::size_t &visits) {
    c.stop();
    if (depth >= std::min<std::size_t>(c.options.maxDepth, 64) ||
        ++visits > c.options.maxNodes * 4)
      return {};
    const auto &e = modelExpression(logical, ref);
    if (e.unresolvedBooleanName) return {};
    if (e.kind == ExprKind::Boolean)
      return uppercase(e.value) == "TRUE" ? 1 : 0;
    if (nullConstant(e))
      return -1;
    auto scalar = [&](const ModelExpression &value)
        -> std::optional<std::pair<int, std::string>> {
      if (value.unresolvedBooleanName) return {};
      if (nullConstant(value))
        return std::pair<int, std::string>{-1, {}};
      if (value.kind == ExprKind::Boolean)
        return std::pair<int, std::string>{
            0, uppercase(value.value) == "TRUE" ? "1" : "0"};
      if (value.kind == ExprKind::String)
        return std::pair<int, std::string>{2, {}};
      if (value.kind != ExprKind::Number || value.value.empty() ||
          value.value.size() > 18 ||
          value.value.find_first_not_of("0123456789") != std::string::npos)
        return {};
      auto first = value.value.find_first_not_of('0');
      return std::pair<int, std::string>{
          1, first == std::string::npos ? "0" : value.value.substr(first)};
    };
    const auto dialect = logical.program.sourceDialect;
    if (e.kind == ExprKind::Number &&
        (isMySqlDialect(dialect) || dialect == Dialect::SQLite))
      if (auto value = scalar(e))
        return value->second == "0" ? 0 : 1;
    if (e.kind == ExprKind::Unary && e.value == "NOT" &&
        e.operands.size() == 1) {
      auto value =
          constantPredicate({ref.query, e.operands[0]}, depth + 1, visits);
      return value ? std::optional<int>{*value < 0 ? -1 : 1 - *value}
                   : std::nullopt;
    }
    if (e.kind == ExprKind::IsNull && e.operands.size() == 1) {
      auto value = scalar(modelExpression(logical, {ref.query, e.operands[0]}));
      if (!value)
        return {};
      const bool isNull = value->first == -1;
      return (e.value == "IS NOT NULL" ? !isNull : isNull) ? 1 : 0;
    }
    if (e.kind != ExprKind::Binary || e.operands.size() != 2)
      return {};
    if (e.value == "AND" || e.value == "OR") {
      auto a = constantPredicate({ref.query, e.operands[0]}, depth + 1, visits);
      if (!a)
        return {};
      auto b = constantPredicate({ref.query, e.operands[1]}, depth + 1, visits);
      if (!b)
        return {};
      if (e.value == "AND")
        return *a == 0 || *b == 0 ? 0 : *a < 0 || *b < 0 ? -1 : 1;
      return *a == 1 || *b == 1 ? 1 : *a < 0 || *b < 0 ? -1 : 0;
    }
    const bool nullSafe = e.value == "NULL_SAFE_EQUAL" ||
                          e.value == "IS DISTINCT FROM" ||
                          e.value == "IS NOT DISTINCT FROM";
    if (!nullSafe && e.value != "=" && e.value != "<>" && e.value != "<" &&
        e.value != ">" && e.value != "<=" && e.value != ">=")
      return {};
    const auto a = scalar(modelExpression(logical, {ref.query, e.operands[0]})),
               b = scalar(modelExpression(logical, {ref.query, e.operands[1]}));
    if (!a || !b)
      return {};
    if (dialect == Dialect::SQLite && nullSafe && b->first == 0) {
      if (a->first == 2) return {};
      const bool equal = a->first >= 0 && (a->second != "0") == (b->second != "0");
      return (e.value == "IS DISTINCT FROM" ? !equal : equal) ? 1 : 0;
    }
    if (a->first < 0 || b->first < 0) {
      if (!nullSafe)
        return -1;
      const bool equal = a->first == b->first;
      return (e.value == "IS DISTINCT FROM" ? !equal : equal) ? 1 : 0;
    }
    if (a->first != b->first || a->first == 2)
      return {};
    const int comparison = a->second.size() != b->second.size()
                               ? (a->second.size() < b->second.size() ? -1 : 1)
                           : a->second == b->second ? 0
                           : a->second < b->second  ? -1
                                                    : 1;
    if (e.value == "=" || e.value == "NULL_SAFE_EQUAL" ||
        e.value == "IS NOT DISTINCT FROM")
      return comparison == 0 ? 1 : 0;
    if (e.value == "<>" || e.value == "IS DISTINCT FROM")
      return comparison != 0 ? 1 : 0;
    if (e.value == "<")
      return comparison < 0 ? 1 : 0;
    if (e.value == ">")
      return comparison > 0 ? 1 : 0;
    if (e.value == "<=")
      return comparison <= 0 ? 1 : 0;
    return comparison >= 0 ? 1 : 0;
  }
  std::optional<int> constantPredicate(ExpressionRef ref) {
    const auto key = std::make_pair(ref.query, ref.expression);
    if (const auto found = constantTruths.find(key); found != constantTruths.end())
      return found->second;
    std::size_t visits = 0;
    auto value = constantPredicate(ref, 0, visits);
    if (!value) value = evaluateConstantTruth(c, ref);
    c.charge(96);
    constantTruths.emplace(key, value);
    return value;
  }
  Truth truth(ExpressionRef ref, std::size_t depth = 0) {
    c.stop();
    if (depth >= c.options.maxDepth)
      return {};
    const auto &e = modelExpression(logical, ref);
    if (e.unresolvedBooleanName) return {};
    if (auto value = constantPredicate(ref))
      return {*value == 1 ? 1. : 0., *value == 0 ? 1. : 0., false};
    auto child = [&](ExprId id) { return truth({ref.query, id}, depth + 1); };
    if (e.kind == ExprKind::Boolean)
      return uppercase(e.value) == "TRUE" ? Truth{1, 0, false}
                                          : Truth{0, 1, false};
    if (e.kind == ExprKind::Null)
      return {0, 0, false};
    if (e.kind == ExprKind::Number) {
      try {
        const auto n = std::stold(e.value);
        if (std::isfinite(n))
          return n == 0 ? Truth{0, 1, false} : Truth{1, 0, false};
      } catch (const std::exception &) {
      }
    }
    if (e.kind == ExprKind::In && !e.query && !e.operands.empty()) {
      bool null = false, known = true;
      std::set<std::string> values;
      for (std::size_t i = 1; i < e.operands.size(); ++i) {
        const auto &value =
            modelExpression(logical, {ref.query, e.operands[i]});
        null |= nullConstant(value);
        if (!nullConstant(value) &&
            (value.kind == ExprKind::Number || value.kind == ExprKind::String ||
             value.kind == ExprKind::Boolean))
          values.insert(std::to_string(int(value.kind)) + ":" + value.value);
        else if (!nullConstant(value))
          known = false;
      }
      if (known)
        if (auto col = resolveColumn(logical, {ref.query, e.operands[0]})) {
          const auto nonNull = 1 - nullFraction(*col);
          const auto equal =
              nonNull * std::min(1., double(values.size()) / ndv(*col).first);
          Truth out{equal, null ? 0 : nonNull - equal, true};
          if (e.value == "NOT IN")
            std::swap(out.yes, out.no);
          return out;
        }
    }
    if (e.kind == ExprKind::Binary && e.operands.size() == 2) {
      if (e.value == "AND" || e.value == "OR") {
        auto a = child(e.operands[0]), b = child(e.operands[1]);
        if (e.value == "AND")
          return {a.yes * b.yes, a.no + b.no - a.no * b.no, true};
        return {a.yes + b.yes - a.yes * b.yes, a.no * b.no, true};
      }
      const auto &left = modelExpression(logical, {ref.query, e.operands[0]}),
                 &right = modelExpression(logical, {ref.query, e.operands[1]});
      const auto a = resolveColumn(logical, {ref.query, e.operands[0]}),
                 b = resolveColumn(logical, {ref.query, e.operands[1]});
      const bool distinct = e.value == "IS DISTINCT FROM";
      const bool nullSafe = e.value == "NULL_SAFE_EQUAL" || distinct ||
                            e.value == "IS NOT DISTINCT FROM";
      auto polarity = [&](Truth value) {
        if (distinct)
          std::swap(value.yes, value.no);
        return value;
      };
      if (logical.program.sourceDialect == Dialect::SQLite && nullSafe &&
          right.kind == ExprKind::Boolean && !right.unresolvedBooleanName && a) {
        const auto estimate = (1 - nullFraction(*a)) * .5;
        return polarity({estimate, 1 - estimate, true});
      }
      if (nullConstant(left) || nullConstant(right)) {
        if (!nullSafe)
          return {0, 0, false};
        if (nullConstant(left) && nullConstant(right))
          return polarity({1, 0, false});
        const auto col = a ? a : b;
        if (col) {
          auto n = nullFraction(*col);
          return polarity({n, 1 - n, !knownNullFraction(*col)});
        }
        auto &other = nullConstant(left) ? right : left;
        if (other.kind == ExprKind::Number || other.kind == ExprKind::String ||
            other.kind == ExprKind::Boolean)
          return polarity({0, 1, false});
        return polarity({.1, .9, true});
      }
      if (e.value == "=" || e.value == "<>" || e.value == "!=" || nullSafe) {
        Truth out{.1, nullSafe ? .9 : .8, true};
        if (a || b) {
          const auto col = a ? *a : *b;
          auto d = ndv(col);
          auto n = nullFraction(col);
          auto nonNull = 1 - n;
          out = {nonNull / d.first, nonNull - nonNull / d.first,
                 d.second || !knownNullFraction(col)};
          if (a && b && sameColumn(*a, *b))
            out = nullSafe ? Truth{1, 0, false}
                           : Truth{nonNull, 0, !knownNullFraction(col)};
          else if (a && b) {
            const auto bd = ndv(*b);
            auto bn = nullFraction(*b);
            nonNull = (1 - n) * (1 - bn);
            const auto equal = nonNull / std::max(d.first, bd.first);
            out = {equal, nonNull - equal,
                   true}; // Independence remains a hypothesis.
            if (nullSafe) {
              out.yes += n * bn;
              out.no = 1 - out.yes;
            }
          } else if (nullSafe)
            out.no = 1 - out.yes;
        } else if (left.kind == ExprKind::Number &&
                   right.kind == ExprKind::Number && left.value == right.value)
          out = {1, 0, false};
        if (e.value == "<>" || e.value == "!=" || distinct)
          std::swap(out.yes, out.no);
        return out;
      }
      if (e.value == "<" || e.value == ">" || e.value == "<=" ||
          e.value == ">=") {
        double nonNull = 1;
        if (a)
          nonNull *= 1 - nullFraction(*a);
        if (b && (!a || !sameColumn(*a, *b)))
          nonNull *= 1 - nullFraction(*b);
        if (a && b && sameColumn(*a, *b))
          return (e.value == "<=" || e.value == ">=")
                     ? Truth{nonNull, 0, !knownNullFraction(*a)}
                     : Truth{0, nonNull, !knownNullFraction(*a)};
        return {nonNull / 3, nonNull * 2 / 3, true};
      }
      if (e.value == "LIKE" || e.value == "NOT LIKE" || e.value == "ILIKE" ||
          e.value == "NOT ILIKE") {
        const auto nonNull = a ? 1 - nullFraction(*a) : .9;
        return (e.value == "LIKE" || e.value == "ILIKE")
                   ? Truth{nonNull * .1, nonNull * .9, true}
                   : Truth{nonNull * .9, nonNull * .1, true};
      }
    }
    if ((e.kind == ExprKind::Unary || e.kind == ExprKind::IsNull) &&
        e.operands.size() == 1) {
      if (e.value == "NOT") {
        auto v = child(e.operands[0]);
        std::swap(v.yes, v.no);
        return v;
      }
      if (e.value == "IS NULL" || e.value == "IS NOT NULL") {
        double fraction = .1;
        bool assumed = true;
        if (auto col = resolveColumn(logical, {ref.query, e.operands[0]})) {
          fraction = nullFraction(*col);
          assumed = !knownNullFraction(*col);
        } else {
          const auto &operand =
              modelExpression(logical, {ref.query, e.operands[0]});
          if (nullConstant(operand)) {
            fraction = 1;
            assumed = false;
          } else if (operand.kind == ExprKind::Number ||
                     operand.kind == ExprKind::String ||
                     (operand.kind == ExprKind::Boolean && !operand.unresolvedBooleanName)) {
            fraction = 0;
            assumed = false;
          }
        }
        return e.value == "IS NULL" ? Truth{fraction, 1 - fraction, assumed}
                                    : Truth{1 - fraction, fraction, assumed};
      }
    }
    return {};
  }
  std::pair<double, bool> selectivity(ExpressionRef ref) {
    auto value = truth(ref);
    return {std::clamp(value.yes, 0., 1.), value.assumed};
  }
  double unpivotFactor(const RelNode &n,bool &assumed) {
    const auto &shape=*query(n.query).tables.at(*n.table).unpivot;
    if (shape.includeNulls.value_or(false)) return double(shape.inputOrdinals.size());
    const auto childId=n.inputs.front();
    const auto &child=node(childId);
    const auto *facts=properties(childId);
    std::set<std::string_view> knownNonNull;
    if (facts)
      for (const auto &column:facts->nonNullColumns)
        if (column.query==child.query && column.table==*n.table) knownNonNull.insert(column.name);
    std::map<std::string_view,double> nullFractions;
    if (child.kind==RelKind::Scan)
      for (const auto &column:table(child.query,*child.table).columns)
        if (column.nullFraction) nullFractions.emplace(column.name,*column.nullFraction);
    double factor=0;
    for (const auto &branch:shape.inputOrdinals) {
      c.stop();
      double allNull=1.;
      bool unknown=false;
      std::size_t uncertain=0;
      std::set<std::size_t> considered;
      for (auto ordinal:branch) {
        if (!considered.insert(ordinal).second) continue;
        const auto &field=child.outputs.at(ordinal);
        if (knownNonNull.count(field.name)) { allNull=0.; break; }
        if (const auto known=nullFractions.find(field.name);known!=nullFractions.end()) {
          allNull*=known->second;
          uncertain+=known->second>0 && known->second<1;
        } else {
          allNull*=.1;
          unknown=true;
        }
      }
      factor+=1.-allNull;
      assumed|=allNull>0 && (unknown || uncertain>1);
    }
    return factor;
  }
  double pivotRows(const RelNode &n,double inputRows,bool &assumed) {
    const auto &shape=*query(n.query).tables.at(*n.table).pivot;
    if (shape.groupingOrdinals.empty()) {
      const auto dialect=logical.program.sourceDialect;
      if (dialect==Dialect::Oracle || dialect==Dialect::PostgreSQL || inputRows>0) return 1.;
      assumed=true;
      return 0.;
    }
    if (inputRows==0) return 0.;
    const auto childId=n.inputs.front();
    const auto &child=node(childId);
    const auto *facts=properties(childId);
    const auto grouped=[&](const auto &column) {
      return std::any_of(shape.groupingOrdinals.begin(),shape.groupingOrdinals.end(),
                         [&](auto index) { return sameColumn(child.outputs.at(index),column); });
    };
    if (facts && std::any_of(facts->uniqueKeys.begin(),facts->uniqueKeys.end(),[&](const auto &key) {
          return !key.empty() && std::all_of(key.begin(),key.end(),grouped);
        })) return inputRows;
    assumed|=shape.groupingOrdinals.size()>1;
    double rows=1.;
    for (auto index:shape.groupingOrdinals) {
      c.stop();
      const auto &field=child.outputs.at(index);
      const auto [values,estimated]=child.kind==RelKind::Scan?ndv(field):
          std::make_pair(std::max(1.,std::sqrt(inputRows)),true);
      const bool nonNull=facts && std::any_of(facts->nonNullColumns.begin(),facts->nonNullColumns.end(),
                                             [&](const auto &other) { return sameColumn(field,other); });
      assumed|=estimated || (!nonNull && (child.kind!=RelKind::Scan || !knownNullFraction(field)));
      const bool nullGroup=!nonNull && (child.kind!=RelKind::Scan || nullFraction(field)>0);
      rows=std::min(inputRows,rows*(values+nullGroup));
    }
    return rows;
  }
  Estimate estimate(const RelNode &n) {
    std::vector<Estimate> in;
    for (auto id : n.inputs)
      in.push_back(groups.at(intern(id)).estimate);
    Estimate out = in.empty() ? Estimate{} : in.front();
    for (const auto &input : in)
      out.assumed |= input.assumed;
    switch (n.kind) {
    case RelKind::Scan: {
      const auto &t = table(n.query, *n.table);
      return {t.rows, t.averageRowBytes,
              (t.source == StatisticsSource::Assumed || t.rowBytesAssumed)};
    }
    case RelKind::Values:
      return {double(n.valueCount), std::max(8.0, 16.0 * n.outputs.size()),
              false};
    case RelKind::TableFunction:
      return {8.0, std::max(8.0,16.0*n.outputs.size()), true};
    case RelKind::Sample:
      out.rows=bounded(out.rows*n.sampling->milliPercent/TableSampling::full);
      break;
    case RelKind::Unnest: {
      const auto literal=postgresIntegerArrayLiteral(logical,n.keys.front(),4096);
      const auto query=postgresIntegerArrayQuery(logical,n);
      const bool direct=literal.status==IntegerArrayStatus::Ready;
      const double elements=direct?literal.elements.size():query?in.back().rows:8;
      return {bounded(out.rows*elements),std::max(8.,out.bytes+(n.withOrdinality?16:8)),
              out.assumed || (!direct && (!query || query->correlated))};
    }
    case RelKind::Series: {
      const auto rows=postgresIntegerSeriesRows(logical,n);
      return {bounded(out.rows*rows.value_or(1000)),std::max(8.,out.bytes+(n.withOrdinality?16:8)),
              out.assumed || !rows};
    }
    case RelKind::Match:
      return {bounded(out.rows*.1),std::max(8.,16.*n.outputs.size()),true};
    case RelKind::Unpivot:
      out.rows=bounded(out.rows*unpivotFactor(n,out.assumed));
      out.bytes=std::max(8.,16.*n.outputs.size());
      break;
    case RelKind::Pivot:
      out.rows=pivotRows(n,out.rows,out.assumed);
      out.bytes=std::max(8.,16.*n.outputs.size());
      break;
    case RelKind::WorkingTableScan: {
      const auto seed = groups.at(intern(n.workingSeed)).estimate;
      return {seed.rows, seed.bytes, true};
    }
    case RelKind::Calc:
      if (!n.keys.empty()) out.bytes=std::max(8.,16.*n.outputs.size());
      [[fallthrough]];
    case RelKind::Filter:
      for (auto p : n.predicates) {
        const auto &query=modelQuery(logical,p.query);
        const auto s = !query.groupingSets.empty() && query.having &&
                       !n.inputs.empty() && node(n.inputs.front()).kind==RelKind::Aggregate
                         ? std::make_pair(.33,true) : selectivity(p);
        out.rows = bounded(out.rows * s.first);
        out.assumed |= s.second;
      }
      break;
    case RelKind::SemiJoin:
    case RelKind::AntiJoin: {
      if (in.size() != 2 || n.predicates.size() != 1)
        c.fail(n.origin, "membership join requires two inputs and an equality key");
      const auto keys = membershipColumns(logical, n);
      if (!keys) c.fail(n.origin, "membership join has an invalid equality key");
      const auto leftDistinct = ndv(keys->first).first;
      const auto rightNonNull = in[1].rows * (1.0 - nullFraction(keys->second));
      const auto rightDistinct = std::min(ndv(keys->second).first, rightNonNull);
      // Marginal NDVs do not determine the overlap of the two key domains.
      // This rough coverage estimate is always marked as assumed.
      const auto matching = in[0].rows * (1.0 - nullFraction(keys->first)) *
                            std::min(1.0, rightDistinct / leftDistinct);
      out.rows = n.kind == RelKind::SemiJoin ? matching : in[0].rows - matching;
      out.assumed = true;
      break;
    }
    case RelKind::Correlate:
    case RelKind::Join: {
      if (in.size() < 2)
        c.fail(n.origin, "join has fewer than two inputs");
      out.rows = bounded(in[0].rows * in[1].rows);
      out.bytes = in[0].bytes + in[1].bytes;
      for (auto p : n.predicates) {
        auto s = selectivity(p);
        out.rows = bounded(out.rows * s.first);
        out.assumed |= s.second;
      }
      if (n.distinct || n.kind == RelKind::Correlate)
        out.assumed = true;
      if (n.join == JoinKind::Left)
        out.rows = std::max(out.rows, in[0].rows);
      else if (n.join == JoinKind::Right)
        out.rows = std::max(out.rows, in[1].rows);
      else if (n.join == JoinKind::Full)
        out.rows = std::max(out.rows, std::max(in[0].rows, in[1].rows));
      break;
    }
    case RelKind::SetOperation:
      if (in.size() < 2)
        c.fail(n.origin, "invalid set estimate input arity");
      out.bytes = in.front().bytes;
      for (const auto &input : in) out.bytes = std::max(out.bytes, input.bytes);
      if (n.setKind == SetKind::Union) {
        out.rows = 0;
        for (const auto &input : in)
          out.rows = bounded(out.rows + input.rows);
      } else if (n.setKind == SetKind::Intersect) {
        out.rows = in[0].rows;
        for (std::size_t i=1; i<in.size(); ++i)
          out.rows = std::min(out.rows, in[i].rows) * .5;
      } else {
        out.rows = std::ldexp(in[0].rows,-static_cast<int>(in.size()-1));
      }
      if (n.distinct)
        out.rows = std::min(
            out.rows, std::max(1.0, std::sqrt(std::max(0.0, out.rows)) * 4));
      out.assumed |= n.distinct || n.setKind != SetKind::Union;
      break;
    case RelKind::RepeatUnion:
      if (in.size() != 2) c.fail(n.origin, "repeat union requires two inputs");
      out.bytes = std::max(in[0].bytes, in[1].bytes);
      out.rows = bounded(in[0].rows + assumedRecursiveIterations * in[1].rows);
      out.assumed = true;
      break;
    case RelKind::Aggregate:
      if (!n.groupingSets.empty()) {
        const auto inputRows=out.rows;
        out.rows=0;
        for (const auto &set:n.groupingSets) {
          c.stop();
          double rows=1;
          for (auto key:set) {
            if (auto col=resolveColumn(logical,key)) {
              const auto distinct=ndv(*col);
              rows=bounded(rows*(distinct.first+(nullFraction(*col)>0?1:0)));
              out.assumed |= distinct.second;
            } else {
              rows=bounded(rows*std::max(1.,std::sqrt(inputRows)));out.assumed=true;
            }
          }
          // Each empty set contributes exactly one row, even on empty input.
          out.rows=bounded(out.rows+(set.empty()?1:std::min(inputRows,rows)));
        }
        // Marginal NDVs do not prove cross-column correlation/equivalence.
        out.assumed |= n.groupingSets.size()>1;
        break;
      }
      if(n.distinct && !n.inputs.empty() && uniqueRows(n.inputs[0]))break;
      if (!n.distinct && !n.inputs.empty() && !n.keys.empty() && n.keys.size() <= 64 &&
          logical.program.sourceDialect == Dialect::PostgreSQL) {
        std::vector<ColumnRef> groups;
        for (auto ref : n.keys) {
          auto group = boundScalarColumn(logical, ref);
          if (!group || !signedIntegerBits(logical.program.types.get(modelExpression(logical, ref).sqlType))) break;
          groups.push_back(std::move(*group));
        }
        if (const auto *facts = groups.size() == n.keys.size() ? properties(n.inputs.front()) : nullptr)
          if (coversNonNullUniqueKey(*facts, groups)) break;
      }
      if (n.keys.empty() && !n.distinct)
        out.rows = 1;
      else {
        double distinct = 1;
        for (auto k : n.keys) {
          if (auto col = resolveColumn(logical, k)) {
            auto value = ndv(*col);
            distinct = bounded(
                distinct * (value.first + (nullFraction(*col) > 0 ? 1 : 0)));
            out.assumed |= value.second;
          } else {
            distinct = bounded(distinct * std::max(1.0, std::sqrt(out.rows)));
            out.assumed = true;
          }
        }
        out.rows = std::min(out.rows, distinct);
      }
      break;
    case RelKind::Coerce:
      for (const auto &cast : n.integerWidenings)
        out.bytes += (*logical.program.types.get(cast.target).bits -
                      *logical.program.types.get(cast.source).bits) / 8.;
      break;
    case RelKind::Project:
      if (!n.outputs.empty() &&
          std::none_of(n.outputs.begin(), n.outputs.end(),
                       [](const auto &col) { return col.name == "*"; }))
        out.bytes = std::max(8.0, 16.0 * n.outputs.size());
      break;
    case RelKind::Limit:
      if (n.rowOffset)
        out.rows = std::max(0.0, out.rows - *n.rowOffset);
      else
        out.assumed = true;
      if (n.rowLimit && (!n.withTies || *n.rowLimit == 0))
        out.rows = std::min(out.rows, *n.rowLimit);
      else if (!n.keys.empty())
        out.assumed = true;
      break;
    default:
      break;
    }
    return out;
  }
  std::string digest(const RelNode &n) {
    std::ostringstream s;
    s << int(n.kind) << ':' << n.query << ':'
      << (n.table ? std::to_string(*n.table) : "-") << ':' << int(n.scanSource) << ':' << int(n.join) << ':'
      << int(n.setKind) << ':' << int(n.setAlgorithm) << ':' << n.distinct << ':' << n.scalar << ':'
      << n.correlated << ':' << n.fixedOrder << ':' << n.materialized << ':' << n.valueCount << ':' << n.workingTableOf << ':'
      << n.workingSeed << ':';
    if (n.rowLimit)
      s << *n.rowLimit;
    s << '/';
    if (n.rowOffset)
      s << *n.rowOffset;
    if (n.withTies)
      s << "ties";
    if (n.withOrdinality)
      s << "ordinality";
    if (n.sampling) {
      s << 's' << n.sampling->milliPercent << '/';
      if (n.sampling->repeatable) s << *n.sampling->repeatable;
    }
    for (auto input : n.inputs)
      s << 'i' << intern(input);
    for (auto p : n.predicates)
      s << 'p' << p.query << '.' << p.expression;
    if (n.membershipKeys) {
      const auto &key = *n.membershipKeys;
      s << 'j' << key.left.query << '.' << key.left.expression
        << '=' << key.right.query << '.' << key.right.expression;
    }
    if (n.correlationKeys) {
      const auto &key=*n.correlationKeys;
      s << 'c' << key.left.query << '.' << key.left.expression
        << '=' << key.right.query << '.' << key.right.expression;
    }
    for (auto k : n.keys)
      s << 'k' << k.query << '.' << k.expression;
    for (const auto &cast : n.integerWidenings)
      s << 'w' << cast.ordinal << '.' << cast.source << '.' << cast.target;
    for (auto type:n.valueTypes) s << 't' << type;
    for (const auto &set:n.groupingSets) {
      s << "g[";
      for (auto ref:set) s << ref.query << '.' << ref.expression << ',';
      s << ']';
    }
    for (const auto &window:n.windowGroups) {
      s << 'f' << window.ordered << '[';
      for (auto ref:window.functions) s << ref.query << '.' << ref.expression << ',';
      s << ']';
    }
    for (auto ref:n.nonNullWindowSums) s << 'z' << ref.query << '.' << ref.expression;
    for (const auto &out : n.outputs)
      s << 'o' << out.query << '.' << out.table << '.' << out.name.size() << ':'
        << out.name;
    for (bool d : n.descending)
      s << 'd' << d;
    for (const auto &nulls : n.nullOrdering)
      s << 'n' << nulls;
    auto key = s.str();
    if (key.size() > 256 * 1024)
      throw MemoLimit{};
    return key;
  }
  GroupId intern(RelId id) {
    c.stop();
    if (!id || id >= logical.nodes.size())
      c.fail({}, "invalid logical node reference");
    if (auto it = owner.find(id); it != owner.end())
      return it->second;
    const auto n = node(id);
    auto key = digest(n);
    if (auto found = interned.find(key); found != interned.end()) {
      owner[id] = found->second;
      return found->second;
    }
    if (groups.size() > c.options.maxGroups ||
        expressionCount >= c.options.maxExpressions)
      throw MemoLimit{};
    auto value = estimate(n);
    auto group = GroupId(groups.size());
    c.charge(sizeof(Group) + key.size() * 2 + 256);
    RowBounds bounds;if(const auto *proof=properties(id))bounds=proof->rows;
    value.rows=std::max(value.rows,double(bounds.minimum));
    if(bounds.maximum)value.rows=std::min(value.rows,double(*bounds.maximum));
    groups.push_back({id, {id}, {key}, value,bounds});
    owner[id] = group;
    interned.emplace(std::move(key), group);
    ++expressionCount;
    return group;
  }
  RelId make(RelNode n) {
    if (logical.nodes.size() >= c.options.maxNodes)
      throw MemoLimit{};
    c.chargeNode(n);
    logical.nodes.push_back(std::move(n));
    return RelId(logical.nodes.size() - 1);
  }
  bool equivalent(GroupId group, RelNode n, const std::string &rule,
                  RelId before, const std::string &reason) {
    c.stop();
    // Logical transformations may change operator shape, but the Memo
    // group represents a stable row contract. Physical traits are compared
    // only when implementing that contract.
    const auto &original=node(groups.at(group).original);
    if (n.outputs.size()!=original.outputs.size() ||
        !std::equal(n.outputs.begin(),n.outputs.end(),original.outputs.begin(),sameColumn))
      return false;
    auto key = digest(n);
    if (groups[group].digests.count(key))
      return false;
    if (expressionCount >= c.options.maxExpressions)
      throw MemoLimit{};
    auto id = make(std::move(n));
    c.charge(key.size() + reason.size() + rule.size() + sizeof(RuleEvent) +
             128);
    groups[group].alternatives.push_back(id);
    groups[group].digests.insert(std::move(key));
    owner[id] = group;
    ++expressionCount;
    if(const auto *proof=properties(id)) {
      auto &bounds=groups[group].bounds;
      bounds.minimum=std::max(bounds.minimum,proof->rows.minimum);
      if(proof->rows.maximum)bounds.maximum=bounds.maximum?std::min(*bounds.maximum,*proof->rows.maximum):proof->rows.maximum;
      if(bounds.maximum && bounds.minimum>*bounds.maximum)c.fail(node(id).origin,"equivalence rule conflicts with proved cardinality bounds");
      auto &estimate=groups[group].estimate;
      estimate.rows=std::max(estimate.rows,double(bounds.minimum));
      if(bounds.maximum)estimate.rows=std::min(estimate.rows,double(*bounds.maximum));
    }
    c.result.rules.push_back({rule, reason, group, before, id});
    return true;
  }
  std::set<std::pair<QueryId, std::size_t>> tables(RelId id) {
    std::set<std::pair<QueryId, std::size_t>> out;
    std::vector<RelId> work{id};
    std::set<RelId> seen;
    while (!work.empty()) {
      auto next = work.back();
      work.pop_back();
      if (!seen.insert(next).second)
        continue;
      const auto &n = node(next);
      if (n.table)
        out.insert({n.query, *n.table});
      work.insert(work.end(), n.inputs.begin(), n.inputs.end());
    }
    return out;
  }
  bool contained(const std::vector<ColumnRef> &refs,
                 const std::set<std::pair<QueryId, std::size_t>> &subset) {
    return !refs.empty() &&
           std::all_of(refs.begin(), refs.end(), [&](const auto &col) {
             return subset.count({col.query, col.table});
           });
  }
  bool inner(const RelNode &n) const {
    return n.kind == RelKind::Join &&
           (n.join == JoinKind::Inner || n.join == JoinKind::Cross) &&
           n.inputs.size() == 2 && !n.distinct && !n.fixedOrder;
  }
  bool safe(const std::vector<ExpressionRef> &predicates) {
    for (auto p : predicates)
      if (!movableExpression(c, p))
        return false;
    return true;
  }
  RelId filterInput(const RelNode &owner, RelId input, std::vector<ExpressionRef> predicates) {
    RelNode filter;
    filter.kind = RelKind::Filter;
    filter.query = owner.query;
    filter.origin = owner.origin;
    filter.inputs = {input};
    filter.outputs = node(input).outputs;
    filter.predicates = std::move(predicates);
    return make(std::move(filter));
  }
  void pushJoinCondition(GroupId group, RelId id, const RelNode &join) {
    if (join.kind != RelKind::Join || join.inputs.size() != 2 || join.fixedOrder || join.distinct ||
        join.predicates.empty() || modelQuery(logical, join.query).kind != StatementKind::Select)
      return;
    const bool pushLeft = join.join == JoinKind::Inner || join.join == JoinKind::Right;
    const bool pushRight = join.join == JoinKind::Inner || join.join == JoinKind::Left;
    if (!pushLeft && !pushRight) return;
    const auto &left = node(join.inputs[0]);
    const auto &right = node(join.inputs[1]);
    if (left.kind != RelKind::Scan || right.kind != RelKind::Scan || !left.table || !right.table ||
        left.query != join.query || right.query != join.query || left.table == right.table)
      return;
    for (auto input : join.inputs) {
      const auto *proof = properties(input);
      if (!proof || proof->mayHaveEffects) return;
    }
    std::vector<ExpressionRef> lhs, rhs, residual;
    for (auto ref : join.predicates) {
      c.stop();
      if (ref.query != join.query) return;
      const auto columns = totalPredicateColumns(logical, ref);
      if (!columns) return;
      const auto only = [&](std::size_t table) {
        return !columns->empty() && std::all_of(columns->begin(), columns->end(),
            [&](const auto &column) { return column.query == join.query && column.table == table; });
      };
      if (pushLeft && only(*left.table)) lhs.push_back(ref);
      else if (pushRight && only(*right.table)) rhs.push_back(ref);
      else residual.push_back(ref);
    }
    if (lhs.empty() && rhs.empty()) return;
    auto pushed = join;
    if (!lhs.empty()) pushed.inputs[0] = filterInput(join, join.inputs[0], std::move(lhs));
    if (!rhs.empty()) pushed.inputs[1] = filterInput(join, join.inputs[1], std::move(rhs));
    pushed.predicates = std::move(residual);
    equivalent(group, std::move(pushed), "JoinConditionPushDown", id,
               "将无副作用的单侧 ON 条件移入内连接输入或外连接的非保留侧，仍保留外连接行");
  }
  bool orderedJoinOutputs(const RelNode &join) const {
    const auto &left=node(join.inputs[0]).outputs;
    const auto &right=node(join.inputs[1]).outputs;
    return join.outputs.size()==left.size()+right.size() &&
        std::equal(left.begin(),left.end(),join.outputs.begin(),sameColumn) &&
        std::equal(right.begin(),right.end(),join.outputs.begin()+left.size(),sameColumn);
  }
  struct JoinGraph {
    std::vector<RelId> scans;
    std::vector<ExpressionRef> conditions;
    std::array<JoinMask,std::size_t{1}<<maxJoinSearchInputs> originalLeft{};
    std::array<JoinMask,64> tableMasks{};
    std::size_t preservedOuterInputs=0;
  };
  bool verifiedGraphScan(const RelNode &relation,QueryId queryId) {
    if (relation.kind!=RelKind::Scan || relation.query!=queryId || !relation.table ||
        *relation.table>=query(queryId).tables.size() || !relation.inputs.empty() ||
        !relation.predicates.empty())
      return false;
    const auto &schema=query(queryId).tables.at(*relation.table).definition;
    return schema && schema->kind==ObjectKind::Table && schema->complete && schema->arityKnown &&
        schema->rowSecurityDisabledKnown && schema->noDescendantsKnown &&
        relation.outputs.size()==schema->columns.size();
  }
  JoinMask collectJoinTree(RelId id,QueryId queryId,JoinGraph &graph,std::size_t depth=0,
                           bool preserveOuter=false) {
    c.stop();
    const auto &relation=node(id);
    if (relation.query!=queryId || relation.correlated || depth>=maxJoinSearchInputs)
      return 0;
    if (verifiedGraphScan(relation,queryId)) {
      const auto table=*relation.table;
      if (graph.scans.size()>=maxJoinSearchInputs || table>=graph.tableMasks.size() ||
          graph.tableMasks[table])
        return 0;
      const auto mask=JoinMask(JoinMask{1}<<graph.scans.size());
      graph.scans.push_back(id);
      graph.tableMasks[table]=mask;
      return mask;
    }
    if (preserveOuter && relation.kind==RelKind::Join && !relation.fixedOrder && !relation.distinct &&
        (relation.join==JoinKind::Left || relation.join==JoinKind::Right || relation.join==JoinKind::Full) &&
        relation.inputs.size()==2 && relation.predicates.size()==1 && orderedJoinOutputs(relation) &&
        graph.scans.size()<maxJoinSearchInputs && relation.predicates.front().query==queryId &&
        integerEqualityColumns(logical,relation.predicates.front())) {
      const auto &left=node(relation.inputs.front()),&right=node(relation.inputs.back());
      if (!verifiedGraphScan(left,queryId) || !verifiedGraphScan(right,queryId) ||
          left.table==right.table || *left.table>=graph.tableMasks.size() || *right.table>=graph.tableMasks.size() ||
          graph.tableMasks[*left.table] || graph.tableMasks[*right.table])
        return 0;
      // A NULL-generating relationship is one indivisible graph input.
      // A predicate referencing either member cannot split the pair.
      const auto mask=JoinMask(JoinMask{1}<<graph.scans.size());
      graph.scans.push_back(id);
      graph.tableMasks[*left.table]=graph.tableMasks[*right.table]=mask;
      ++graph.preservedOuterInputs;
      return mask;
    }
    if (!inner(relation) || !orderedJoinOutputs(relation))
      return 0;
    const auto left=collectJoinTree(relation.inputs.front(),queryId,graph,depth+1,preserveOuter);
    if (!left) return 0;
    const auto right=collectJoinTree(relation.inputs.back(),queryId,graph,depth+1,preserveOuter);
    if (!right || relation.predicates.size()>64-graph.conditions.size()) return 0;
    graph.conditions.insert(graph.conditions.end(),relation.predicates.begin(),relation.predicates.end());
    graph.originalLeft[left|right]=left;
    return JoinMask(left|right);
  }
  std::optional<JoinMask> joinConditionInputs(ExpressionRef ref,const JoinGraph &graph,
                                             std::size_t &visits,std::size_t depth=0) {
    c.stop();
    if (++visits>256 || depth>16) return {};
    const auto &expression=modelExpression(logical,ref);
    if (expression.kind==ExprKind::Binary && expression.operands.size()==2 &&
        (expression.value=="AND" || expression.value=="OR")) {
      const auto left=joinConditionInputs({ref.query,expression.operands[0]},graph,visits,depth+1);
      if (!left) return {};
      const auto right=joinConditionInputs({ref.query,expression.operands[1]},graph,visits,depth+1);
      return right?std::optional<JoinMask>{JoinMask(*left|*right)}:std::nullopt;
    }
    const auto fields=totalPredicateColumns(logical,ref);
    if (!fields) return {};
    JoinMask mask=0;
    for (const auto &field:*fields) {
      if (field.query!=ref.query || field.table>=graph.tableMasks.size() || !graph.tableMasks[field.table])
        return {};
      mask=JoinMask(mask|graph.tableMasks[field.table]);
    }
    return mask;
  }
  std::vector<ExpressionRef> outputBindings(const RelNode &root) {
    auto &scope=logical.program.queries.at(root.query-1);
    if (scope.expressions.size()+root.outputs.size()>c.options.maxNodes) return {};
    std::map<std::pair<std::size_t,std::size_t>,ExprId> available;
    for (std::size_t i=1;i<scope.expressions.size();++i) {
      c.stop();
      const auto &entry=scope.expressions[i];
      if (!entry.binding) continue;
      const auto id=static_cast<ExprId>(i);
      if (boundScalarColumn(logical,{root.query,id}) &&
          available.try_emplace(std::make_pair(entry.binding->table,entry.binding->column),id).second)
        c.charge(64);
    }
    std::vector<ExpressionRef> result;
    c.charge(root.outputs.size()*sizeof(ExpressionRef));
    for (const auto &out:root.outputs) {
      c.stop();
      if (out.query!=root.query || out.table>=scope.tables.size()) return {};
      const auto &table=scope.tables[out.table];
      if (!table.definition) return {};
      const auto &columns=table.definition->columns;
      const auto found=std::find_if(columns.begin(),columns.end(),[&](const auto &column) {
        return column.name==out.name;
      });
      if (found==columns.end()) return {};
      const auto ordinal=std::size_t(found-columns.begin());
      const auto existing=available.find({out.table,ordinal});
      if (existing!=available.end()) {
        result.push_back({root.query,existing->second});
        continue;
      }
      if (!found->sqlType) return {};
      ModelExpression expression;
      expression.kind=ExprKind::Name;
      expression.origin=root.origin;
      expression.name={out.name};
      expression.nameQuoted={true};
      expression.type=found->type;
      expression.binding=ColumnBinding{out.table,ordinal,root.query};
      const auto bytes=logical.program.types.accountedBytes();
      expression.sqlType=logical.program.types.intern(*found->sqlType);
      c.charge(logical.program.types.accountedBytes()-bytes+sizeof(expression)+
               sizeof(std::string)+out.name.size()+sizeof(bool));
      result.push_back({root.query,static_cast<ExprId>(scope.expressions.size())});
      scope.expressions.push_back(std::move(expression));
    }
    return result;
  }
  std::optional<std::vector<std::vector<ColumnRef>>> joinProjection(
      const RelNode &root,const JoinGraph &graph,const std::vector<JoinSearchTerm> &terms) {
    const auto &scope=query(root.query);
    if (root.keys.size()!=root.outputs.size() || root.keys.empty() ||
        std::any_of(scope.orderBy.begin(),scope.orderBy.end(),[&](const auto &item) {
          return !modelExpression(logical,{scope.id,item.expression}).outputColumn;
        }))
      return {};
    std::set<std::pair<std::size_t,std::string>> needed;
    for (auto ref:root.keys) {
      c.stop();
      const auto field=boundScalarColumn(logical,ref);
      if (!field || ref.query!=scope.id) return {};
      needed.emplace(field->table,field->name);
    }
    for (const auto &term:terms)
      for (const auto &field:referencedColumns(c,term.expression))
        needed.emplace(field.table,field.name);
    c.charge(needed.size()*96);
    std::vector<std::vector<ColumnRef>> outputs;
    for (auto scan:graph.scans) {
      std::vector<ColumnRef> fields;
      const auto &source=node(scan);
      for (const auto &field:source.outputs) {
        c.stop();
        if (needed.count({field.table,field.name})) fields.push_back(field);
      }
      if (fields.empty() && !source.outputs.empty()) fields.push_back(source.outputs.front());
      c.charge(fields.size()*sizeof(ColumnRef));
      outputs.push_back(std::move(fields));
    }
    return outputs;
  }
  void reorderJoinGraph(GroupId group,RelId id,const RelNode &root) {
    if (!c.options.joinAssociation || logical.program.sourceDialect!=Dialect::PostgreSQL ||
        root.outputs.empty() || root.outputs.size()>128)
      return;
    const bool projected=root.kind==RelKind::Project && root.inputs.size()==1;
    const auto body=projected?root.inputs.front():id;
    const auto &input=node(body);
    const bool filter=input.kind==RelKind::Filter && input.inputs.size()==1;
    if (!inner(input) && (!filter || !c.options.filterPushdown || !inner(node(input.inputs.front()))))
      return;
    const auto &scope=query(root.query);
    if (!scope.catalogChecked || scope.kind!=StatementKind::Select) return;
    JoinGraph graph;
    if (filter && input.predicates.size()>64) return;
    if (filter) graph.conditions=input.predicates;
    const auto full=collectJoinTree(filter?input.inputs.front():body,root.query,graph,0,true);
    if (!full || graph.scans.size()<3 || graph.conditions.empty()) return;
    c.charge(graph.scans.size()*(sizeof(RelId)+sizeof(JoinSearchInput))+
             graph.conditions.size()*(sizeof(ExpressionRef)+sizeof(JoinSearchTerm)));

    std::vector<JoinSearchTerm> terms;
    for (auto ref:graph.conditions) {
      std::size_t visits=0;
      if (ref.query!=root.query) return;
      const auto inputs=joinConditionInputs(ref,graph,visits);
      if (!inputs || !*inputs) return;
      terms.push_back({ref,*inputs,selectivity(ref).first});
    }
    const auto projectedFields=projected?joinProjection(root,graph,terms):std::nullopt;
    if (projected && !projectedFields) return;
    bool trims=false;
    if (projectedFields)
      for (std::size_t i=0;i<graph.scans.size();++i)
        trims|=(*projectedFields)[i].size()<node(graph.scans[i]).outputs.size();
    if (projected && !trims) return; // An untrimmed search already runs on the Join.
    auto scans=graph.scans;
    std::sort(scans.begin(),scans.end());
    auto conditions=graph.conditions;
    const auto expressionOrder=[](const auto &a,const auto &b) {
      return std::tie(a.query,a.expression)<std::tie(b.query,b.expression);
    };
    std::sort(conditions.begin(),conditions.end(),expressionOrder);
    std::string signature=std::to_string(root.query);
    for (auto scan:scans) signature+=":"+std::to_string(scan);
    signature+="|";
    for (auto expression:conditions) signature+=":"+std::to_string(expression.expression);
    if (projected) {
      signature+="|";
      for (auto key:root.keys) signature+=":"+std::to_string(key.expression);
    }
    if (!joinGraphs.insert(signature).second) return;
    c.charge(signature.size()+80);
    std::vector<JoinSearchInput> inputs;
    for (std::size_t i=0;i<graph.scans.size();++i) {
      const auto scan=graph.scans[i];
      const auto &estimate=groups.at(intern(scan)).estimate;
      auto width=estimate.bytes;
      if (projectedFields && !node(scan).outputs.empty())
        width=std::max(1.,width*(*projectedFields)[i].size()/node(scan).outputs.size());
      inputs.push_back({estimate.rows,width});
    }
    std::vector<RelId> leaves=graph.scans;
    bool leavesReady=false;
    std::vector<ExpressionRef> restore;
    enum class Method { LeftDeep, DynamicProgramming, PushThrough };
    std::vector<std::pair<Method,JoinOrder>> alternatives;
    if (!projected && graph.scans.size()==3) {
      const auto left=graph.originalLeft[full];
      const auto first=graph.originalLeft[left];
      const auto right=JoinMask(full^left);
      const auto middle=JoinMask(first|right);
      if (first && right && !(right&(right-1)) &&
          std::any_of(terms.begin(),terms.end(),[&](const auto &term){return term.inputs==middle;})) {
        JoinOrder push;
        push.left.resize(full+1);
        c.charge(push.left.size()*sizeof(JoinMask));
        push.left[full]=middle;
        push.left[middle]=first;
        alternatives.emplace_back(Method::PushThrough,std::move(push));
      }
    }
    alternatives.emplace_back(Method::LeftDeep,searchJoinOrder(c,inputs,terms,true));
    alternatives.emplace_back(Method::DynamicProgramming,searchJoinOrder(c,inputs,terms,false));
    for (const auto &alternative:alternatives) {
      const auto method=alternative.first;
      const auto &order=alternative.second;
      if (order.left.empty()) continue;
      const auto original=[&](const auto &self,JoinMask subset)->bool {
        const auto left=order.left[subset];
        return graph.originalLeft[subset]==left &&
            (!left || (self(self,left) && self(self,JoinMask(subset^left))));
      };
      if (!filter && !trims && original(original,full)) continue;
      if (!leavesReady) {
        for (std::size_t i=0;i<leaves.size();++i) {
          std::vector<ExpressionRef> predicates;
          for (const auto &term:terms)
            if (term.inputs==(JoinMask{1}<<i)) predicates.push_back(term.expression);
          if (!predicates.empty())
            leaves[i]=filterInput(root,leaves[i],std::move(predicates));
          if (projectedFields && (*projectedFields)[i].size()<node(leaves[i]).outputs.size()) {
            RelNode narrow;
            narrow.kind=RelKind::Project;
            narrow.query=root.query;
            narrow.origin=root.origin;
            narrow.inputs={leaves[i]};
            narrow.outputs=(*projectedFields)[i];
            narrow.keys=outputBindings(narrow);
            if (narrow.keys.empty()) return;
            leaves[i]=make(std::move(narrow));
          }
        }
        leavesReady=true;
      }
      const auto build=[&](const auto &self,JoinMask subset)->RelId {
        c.stop();
        const auto leftMask=order.left[subset];
        if (!leftMask) {
          for (std::size_t i=0;i<leaves.size();++i) if ((JoinMask{1}<<i)==subset) return leaves[i];
          c.fail(root.origin,"join search leaf bit has no input");
        }
        const auto rightMask=JoinMask(subset^leftMask);
        RelNode join;
        join.kind=RelKind::Join;
        join.query=root.query;
        join.origin=root.origin;
        join.inputs={self(self,leftMask),self(self,rightMask)};
        for (const auto input:join.inputs) {
          const auto &fields=node(input).outputs;
          join.outputs.insert(join.outputs.end(),fields.begin(),fields.end());
        }
        for (const auto &term:terms)
          if ((term.inputs&subset)==term.inputs && (term.inputs&leftMask) && (term.inputs&rightMask))
            join.predicates.push_back(term.expression);
        join.join=join.predicates.empty()?JoinKind::Cross:JoinKind::Inner;
        return make(std::move(join));
      };
      const auto tree=build(build,full);
      auto answer=node(tree);
      if (projected) {
        answer=root;
        answer.inputs={tree};
      } else if (answer.outputs.size()!=root.outputs.size()) {
        return;
      } else if (!std::equal(answer.outputs.begin(),answer.outputs.end(),root.outputs.begin(),sameColumn)) {
        if (restore.empty()) restore=outputBindings(root);
        if (restore.empty()) return;
        answer=RelNode{};
        answer.kind=RelKind::Project;
        answer.query=root.query;
        answer.origin=root.origin;
        answer.inputs={tree};
        answer.outputs=root.outputs;
        answer.keys=restore;
      }
      const auto name=method==Method::PushThrough?"JoinPushThroughJoin":
          method==Method::LeftDeep?"MultiJoinLeftDeep":"DphypJoinReorder";
      std::string detail=
                 projected?"各连接输入只保留结果和原有谓词使用的字段，并比较缩窄后的连接计划":
                 method==Method::PushThrough?"交换三表连接中的第二和第三个输入，使首尾表的条件提前应用":
                 method==Method::LeftDeep?"将多表连接按原有谓词及统计量排列为左深树，恢复原输出顺序":
                 "按子集计算连接代价，比较允许两侧同时成组的顺序。每条条件只在其所有输入可见后求值";
      if (graph.preservedOuterInputs)
        detail+="。外连接作为一个完整输入保留，补 NULL 的关系保持原位";
      equivalent(group,std::move(answer),projected?"MultiJoinProjectTranspose":name,id,detail);
    }
  }
  void extractJoinFilter(GroupId group,RelId id,const RelNode &join) {
    if (logical.program.sourceDialect!=Dialect::PostgreSQL || !inner(join) ||
        join.join!=JoinKind::Inner || join.correlated || join.predicates.empty() ||
        join.predicates.size()>32 || !orderedJoinOutputs(join))
      return;
    const auto &owner=query(join.query);
    if (!owner.catalogChecked || owner.kind!=StatementKind::Select ||
        std::any_of(join.inputs.begin(),join.inputs.end(),[&](auto child) {
          return node(child).kind!=RelKind::Scan || node(child).query!=join.query;
        }))
      return;
    for (const auto ref:join.predicates) {
      c.stop();
      if (ref.query!=join.query || !totalPredicateColumns(logical,ref)) return;
    }
    auto cross=join;
    cross.join=JoinKind::Cross;
    cross.predicates.clear();
    RelNode filter;
    filter.kind=RelKind::Filter;
    filter.query=join.query;
    filter.origin=join.origin;
    filter.inputs={make(std::move(cross))};
    filter.outputs=join.outputs;
    filter.predicates=join.predicates;
    equivalent(group,std::move(filter),"JoinExtractFilter",id,
               "将内连接的安全条件提取到笛卡尔连接上方，保留列绑定和 SQL 三值逻辑");
  }
  void transposeJoinProjection(GroupId group,RelId id,const RelNode &relation) {
    if (logical.program.sourceDialect!=Dialect::PostgreSQL || !inner(relation) ||
        relation.correlated || relation.outputs.size()>128 || !orderedJoinOutputs(relation) ||
        !query(relation.query).catalogChecked)
      return;
    for (auto ref:relation.predicates)
      if (ref.query!=relation.query || !totalPredicateColumns(logical,ref)) return;
    const auto &owner=query(relation.query);
    auto join=relation;
    bool changed=false;
    for (auto &input:join.inputs) {
      c.stop();
      const auto &derived=node(input);
      if (derived.kind!=RelKind::Subquery || derived.inputs.size()!=1 || !derived.table ||
          derived.correlated || derived.scalar || derived.materialized)
        continue;
      const auto &binding=owner.tables.at(*derived.table);
      const auto &projection=node(derived.inputs.front());
      if (!binding.query || !binding.name.empty() || projection.kind!=RelKind::Project ||
          projection.query!=binding.query || projection.inputs.size()!=1 ||
          projection.outputs.size()!=derived.outputs.size() ||
          projection.keys.size()!=projection.outputs.size())
        continue;
      const auto sourceId=projection.inputs.front();
      const auto &source=node(sourceId);
      if (source.kind!=RelKind::Scan || source.outputs.size()>128 ||
          source.outputs.size()<=derived.outputs.size())
        continue;
      RelNode renamed;
      renamed.kind=RelKind::Rename;
      renamed.query=relation.query;
      renamed.origin=derived.origin;
      renamed.inputs={sourceId};
      renamed.outputs=source.outputs;
      std::set<std::size_t> mapped;
      for (std::size_t i=0;i<projection.keys.size();++i) {
        c.stop();
        const auto field=boundScalarColumn(logical,projection.keys[i]);
        if (!field) break;
        const auto ordinal=modelExpression(logical,projection.keys[i]).binding->column;
        if (ordinal>=source.outputs.size() || !sameColumn(*field,source.outputs[ordinal]) ||
            !mapped.insert(ordinal).second)
          break;
        renamed.outputs[ordinal]=derived.outputs[i];
      }
      if (mapped.size()!=projection.keys.size()) continue;
      input=make(std::move(renamed));
      changed=true;
    }
    if (!changed) return;
    auto keys=outputBindings(relation);
    if (keys.empty()) return;
    join.outputs.clear();
    for (auto input:join.inputs) {
      const auto &columns=node(input).outputs;
      join.outputs.insert(join.outputs.end(),columns.begin(),columns.end());
    }
    RelNode projection;
    projection.kind=RelKind::Project;
    projection.query=relation.query;
    projection.origin=relation.origin;
    projection.inputs={make(std::move(join))};
    projection.outputs=relation.outputs;
    projection.keys=std::move(keys);
    equivalent(group,std::move(projection),"JoinProjectTranspose",id,
               "从内连接输入提起直接列投影，按原位置恢复结果。连接仍使用经过验证的同一列绑定");
  }
  ExpressionRef booleanExpression(QueryId queryId,SourceRange origin,TypeId type,
                                  ExprKind kind,std::string operation,std::vector<ExprId> operands) {
    c.stop();
    auto &query=logical.program.queries.at(queryId-1);
    if (query.expressions.size()>=c.options.maxNodes) throw MemoLimit{};
    ModelExpression expression;
    expression.origin=origin;
    expression.kind=kind;
    expression.type=ValueType::Boolean;
    expression.sqlType=type;
    expression.value=std::move(operation);
    expression.operands=std::move(operands);
    c.charge(sizeof(expression)+expression.value.size()+expression.operands.size()*sizeof(ExprId));
    const auto id=static_cast<ExprId>(query.expressions.size());
    query.expressions.push_back(std::move(expression));
    return {queryId,id};
  }
  bool wideningKey(ExpressionRef ref,const RelNode &source) {
    const auto &cast=modelExpression(logical,ref);
    if (cast.kind!=ExprKind::Cast || !cast.castType || cast.operands.size()!=1 ||
        signedIntegerBits(logical.program.types.get(cast.sqlType))!=64)
      return false;
    const ExpressionRef inner{ref.query,cast.operands.front()};
    const auto sourceType=signedIntegerBits(logical.program.types.get(modelExpression(logical,inner).sqlType));
    const auto field=boundScalarColumn(logical,inner);
    return sourceType && *sourceType<64 && field &&
        std::any_of(source.outputs.begin(),source.outputs.end(),[&](const auto &col) {
          return sameColumn(col,*field);
        });
  }
  std::optional<ColumnRef> appendPrivateField(QueryId queryId,ExpressionRef value,std::string_view prefix) {
    c.stop();
    auto &owner=logical.program.queries.at(queryId-1);
    if (owner.tables.size()>=256 || owner.expressions.size()>=c.options.maxNodes)
      return {};
    const auto &original=modelExpression(logical,value);
    const auto ordinal=owner.tables.size();
    const auto name=std::string(prefix)+std::to_string(ordinal);
    auto definition=std::make_shared<TableDefinition>();
    definition->name=name;
    definition->columns={{name,original.type,
        std::make_shared<const SqlType>(logical.program.types.get(original.sqlType))}};
    ModelTable binding;
    binding.pseudo=true;
    binding.definition=std::move(definition);
    ModelExpression key;
    key.kind=ExprKind::Name;
    key.origin=original.origin;
    key.name={name};
    key.nameQuoted={true};
    key.type=original.type;
    key.sqlType=original.sqlType;
    key.binding=ColumnBinding{ordinal,0,owner.id};
    c.charge(sizeof(ModelTable)+sizeof(TableDefinition)+sizeof(ColumnDefinition)+sizeof(SqlType)+
             sizeof(ModelExpression)+6*name.size()+128);
    owner.tables.push_back(std::move(binding));
    owner.expressions.push_back(std::move(key));
    return ColumnRef{queryId,ordinal,name};
  }
  void pushComputedEquality(GroupId group,RelId id,const RelNode &operation) {
    if (logical.program.sourceDialect!=Dialect::PostgreSQL || operation.predicates.size()!=1 ||
        operation.outputs.empty() || operation.outputs.size()>64)
      return;
    const bool filter=operation.kind==RelKind::Filter && operation.inputs.size()==1;
    const auto &scope=query(operation.query);
    if (!scope.catalogChecked || (filter ? !scope.scope.parent :
        !inner(operation) || operation.join!=JoinKind::Inner || !orderedJoinOutputs(operation)))
      return;
    const auto predicate=operation.predicates.front();
    const auto &expression=modelExpression(logical,predicate);
    if (predicate.query!=scope.id || expression.kind!=ExprKind::Binary ||
        expression.value!="=" || expression.operands.size()!=2 ||
        !sameIntegerType(logical,{scope.id,expression.operands[0]},{scope.id,expression.operands[1]}))
      return;

    for (std::size_t side=0;side<operation.inputs.size();++side) {
      c.stop();
      const auto inputId=operation.inputs[side];
      const auto &input=node(inputId);
      if (input.kind!=RelKind::Scan || input.query!=scope.id || !input.table) continue;
      std::optional<std::size_t> position;
      for (std::size_t i=0;i<2;++i)
        if (wideningKey({scope.id,expression.operands[i]},input)) position=i;
      if (!position) continue;
      const auto computed=expression.operands[*position];
      const ExpressionRef other{scope.id,expression.operands[1-*position]};
      const auto opposite=scopedScalarColumn(logical,other,
          filter?*scope.scope.parent:scope.id);
      if (!opposite || (!filter &&
          std::none_of(node(operation.inputs[1-side]).outputs.begin(),node(operation.inputs[1-side]).outputs.end(),
                       [&](const auto &field){return sameColumn(field,*opposite); })))
        continue;
      auto lower=RelNode{};
      lower.kind=RelKind::Project;
      lower.query=scope.id;
      lower.origin=operation.origin;
      lower.inputs={inputId};
      lower.outputs=input.outputs;
      auto rewritten=expression;
      auto restore=outputBindings(operation);
      lower.keys=outputBindings(lower);
      if (restore.empty() || lower.keys.size()!=lower.outputs.size()) return;

      // The original catalog bindings remain immutable. Append a private
      // binding for the projected key so SQL generation can resolve it by
      // position without exposing it in the original result row.
      auto &owner=logical.program.queries.at(scope.id-1);
      if (owner.expressions.size()+2>c.options.maxNodes) return;
      rewritten.operands[*position]=static_cast<ExprId>(owner.expressions.size());
      const auto field=appendPrivateField(scope.id,{scope.id,computed},"navidog_key_");
      if (!field) return;
      const ExpressionRef condition{owner.id,static_cast<ExprId>(owner.expressions.size())};
      c.charge(sizeof(rewritten)+rewritten.operands.size()*sizeof(ExprId)+rewritten.value.size());
      owner.expressions.push_back(std::move(rewritten));
      lower.keys.push_back({owner.id,computed});
      lower.outputs.push_back(*field);

      auto planned=operation;
      planned.inputs[side]=make(std::move(lower));
      planned.predicates={condition};
      planned.outputs.clear();
      for (const auto child:planned.inputs) {
        const auto &fields=node(child).outputs;
        planned.outputs.insert(planned.outputs.end(),fields.begin(),fields.end());
      }
      RelNode output;
      output.kind=RelKind::Project;
      output.query=owner.id;
      output.origin=operation.origin;
      output.inputs={make(std::move(planned))};
      output.outputs=operation.outputs;
      output.keys=std::move(restore);
      equivalent(group,std::move(output),filter?"FilterFlattenCorrelatedCondition":"JoinPushExpressions",id,
                 "在输入上先计算可证明无溢出的整数加宽键，比较绑定列后去掉临时字段");
      return;
    }
  }
  void groupConstantWithDummyRow(GroupId group,RelId id,const RelNode &project) {
    if (logical.program.sourceDialect!=Dialect::PostgreSQL ||
        project.kind!=RelKind::Project || project.inputs.size()!=1 ||
        project.keys.size()!=project.outputs.size())
      return;
    auto aggregate=node(project.inputs.front());
    if (aggregate.kind!=RelKind::Aggregate || aggregate.query!=project.query ||
        aggregate.distinct || aggregate.inputs.size()!=1 || aggregate.keys.empty() ||
        aggregate.keys.size()>16 || !aggregate.groupingSets.empty())
      return;
    const auto &owner=query(project.query);
    if (!owner.catalogChecked || owner.having ||
        std::any_of(owner.orderBy.begin(),owner.orderBy.end(),[&](const auto &item) {
          return !modelExpression(logical,{owner.id,item.expression}).outputColumn;
        }))
      return;
    const auto inputId=aggregate.inputs.front();
    const auto &input=node(inputId);
    if (input.kind!=RelKind::Scan && input.kind!=RelKind::Filter) return;
    std::vector<std::pair<std::size_t,ExpressionRef>> constants;
    for (std::size_t i=0;i<aggregate.keys.size();++i) {
      const auto &key=aggregate.keys[i];
      if (key.query!=owner.id) return;
      auto expr=key;
      const auto &original=modelExpression(logical,key);
      if (original.outputColumn) {
        if (*original.outputColumn>=project.keys.size()) return;
        expr=project.keys[*original.outputColumn];
      }
      const auto &resolved=modelExpression(logical,expr);
      if (!signedIntegerBits(logical.program.types.get(resolved.sqlType))) return;
      if (resolved.kind==ExprKind::Number && !resolved.outputColumn &&
          !resolved.value.empty() && resolved.value.find_first_not_of("0123456789")==std::string::npos)
        constants.emplace_back(i,expr);
      else if (!boundScalarColumn(logical,expr))
        return;
    }
    if (constants.empty() || owner.tables.size()+constants.size()>256 ||
        owner.expressions.size()+constants.size()>c.options.maxNodes)
      return;
    RelNode dummy;
    dummy.kind=RelKind::Values;
    dummy.query=owner.id;
    dummy.origin=aggregate.origin;
    for (const auto &[position,ref]:constants) {
      const auto field=appendPrivateField(owner.id,ref,"navidog_group_");
      if (!field) return;
      dummy.keys.push_back(ref);
      dummy.outputs.push_back(*field);
      dummy.valueTypes.push_back(modelExpression(logical,ref).sqlType);
      aggregate.keys[position]={owner.id,static_cast<ExprId>(owner.expressions.size()-1)};
    }
    RelNode join;
    join.kind=RelKind::Join;
    join.join=JoinKind::Cross;
    join.query=owner.id;
    join.origin=aggregate.origin;
    join.outputs=input.outputs;
    join.outputs.insert(join.outputs.end(),dummy.outputs.begin(),dummy.outputs.end());
    join.inputs={inputId,make(std::move(dummy))};
    aggregate.outputs=join.outputs;
    aggregate.inputs={make(std::move(join))};
    auto result=project;
    result.inputs={make(std::move(aggregate))};
    equivalent(group,std::move(result),"AggregateProjectConstantToDummyJoin",id,
               "用只含一行类型化常量的 VALUES 保存分组键，空输入仍产生零个分组");
  }
  void expandNullSafeConditions(GroupId group,RelId id,const RelNode &relation) {
    if (logical.program.sourceDialect!=Dialect::PostgreSQL || relation.predicates.empty() ||
        relation.predicates.size()>32 ||
        (relation.kind!=RelKind::Filter && relation.kind!=RelKind::Join) ||
        (relation.kind==RelKind::Join && (relation.join==JoinKind::Full || relation.fixedOrder)) ||
        !query(relation.query).catalogChecked)
      return;
    auto expanded=relation;
    bool changed=false;
    for (auto &ref:expanded.predicates) {
      const auto &before=modelExpression(logical,ref);
      if (ref.query!=relation.query || before.kind!=ExprKind::Binary ||
          before.value!="IS NOT DISTINCT FROM" || before.operands.size()!=2)
        continue;
      const auto a=before.operands[0], b=before.operands[1], type=before.sqlType;
      const ExpressionRef left{ref.query,a}, right{ref.query,b};
      if (!boundScalarColumn(logical,left) || !boundScalarColumn(logical,right) ||
          !sameIntegerType(logical,left,right) ||
          !signedIntegerBits(logical.program.types.get(modelExpression(logical,left).sqlType)))
        continue;
      const auto make=[&](ExprKind kind,std::string operation,std::vector<ExprId> operands) {
        return booleanExpression(relation.query,relation.origin,type,kind,std::move(operation),std::move(operands)).expression;
      };
      const auto equal=make(ExprKind::Binary,"=",{a,b});
      const auto nonNullEqual=make(ExprKind::TruthTest,"IS TRUE",{equal});
      const auto aNull=make(ExprKind::IsNull,"IS NULL",{a});
      const auto bNull=make(ExprKind::IsNull,"IS NULL",{b});
      const auto bothNull=make(ExprKind::Binary,"AND",{aNull,bNull});
      ref.expression=make(ExprKind::Binary,"OR",{nonNullEqual,bothNull});
      changed=true;
    }
    if (changed)
      equivalent(group,std::move(expanded),relation.kind==RelKind::Filter?"FilterRemoveIsNotDistinctFrom":
                 "JoinConditionExpandIsNotDistinctFrom",id,
                 "以相等为 TRUE 或两列均为 NULL 展开同类型整数的 NULL 安全相等，保持布尔结果");
  }
  void splitDisjunctiveJoin(GroupId group,RelId id,const RelNode &join) {
    if (logical.program.sourceDialect!=Dialect::PostgreSQL || !inner(join) ||
        join.join!=JoinKind::Inner || join.predicates.empty() || join.predicates.size()>16 ||
        !query(join.query).catalogChecked || !query(join.query).groupingSets.empty() ||
        !orderedJoinOutputs(join))
      return;
    JoinGraph graph;
    if (!collectJoinTree(id,join.query,graph) || graph.scans.size()!=2) return;
    for (auto source:graph.scans) {
      const auto &definition=query(join.query).tables.at(*node(source).table).definition;
      if (!std::all_of(definition->columns.begin(),definition->columns.end(),[](const auto &column) {
            return column.sqlType && signedIntegerBits(*column.sqlType).has_value();
          }))
        return;
    }
    std::size_t position=join.predicates.size();
    ExprId first=0,second=0;
    TypeId type=0;
    for (std::size_t i=0;i<join.predicates.size();++i) {
      const auto predicate=join.predicates[i];
      if (predicate.query!=join.query) return;
      const auto &expr=modelExpression(logical,predicate);
      if (expr.kind!=ExprKind::Binary || expr.value!="OR" || expr.operands.size()!=2) {
        if (!totalPredicateColumns(logical,predicate)) return;
        continue;
      }
      if (first) return; // A single OR keeps the number of full scans bounded.
      first=expr.operands[0];
      second=expr.operands[1];
      type=expr.sqlType;
      position=i;
    }
    if (!first) return;
    for (auto term:{first,second}) {
      std::size_t visits=0;
      const ExpressionRef ref{join.query,term};
      if (!integerEqualityColumns(logical,ref) || joinConditionInputs(ref,graph,visits)!=JoinMask{3})
        return;
    }
    auto left=join;
    auto right=join;
    left.predicates[position].expression=first;
    right.predicates[position].expression=second;
    const auto exclusion=booleanExpression(join.query,join.origin,type,ExprKind::TruthTest,"IS NOT TRUE",{first});
    const auto leftRows=make(std::move(left));
    const auto rightRows=filterInput(join,make(std::move(right)),{exclusion});
    RelNode appendRows;
    appendRows.kind=RelKind::SetOperation;
    appendRows.query=join.query;
    appendRows.origin=join.origin;
    appendRows.outputs=join.outputs;
    appendRows.inputs={leftRows,rightRows};
    equivalent(group,std::move(appendRows),"JoinExpandOrToUnion",id,
               "两个等值分支可独立优化。第二支排除第一条件为 TRUE 的匹配，保留原行重数和未知条件");
  }
  std::vector<ExpressionRef> conjuncts(ExpressionRef ref) {
    std::vector<ExpressionRef> result,work{ref};
    while (!work.empty() && work.size()+result.size()<=16) {
      c.stop();
      const auto current=work.back();
      work.pop_back();
      const auto &expr=modelExpression(logical,current);
      if (expr.kind==ExprKind::Binary && expr.value=="AND" && expr.operands.size()==2) {
        work.push_back({ref.query,expr.operands[1]});
        work.push_back({ref.query,expr.operands[0]});
      } else {
        result.push_back(current);
      }
    }
    return work.empty()?result:std::vector<ExpressionRef>{};
  }
  void inferDisjunctFilters(GroupId group,RelId id,const RelNode &owner) {
    if (!c.options.filterPushdown || logical.program.sourceDialect!=Dialect::PostgreSQL ||
        owner.predicates.empty() || owner.predicates.size()>16 || !query(owner.query).catalogChecked)
      return;
    const bool external=owner.kind==RelKind::Filter && owner.inputs.size()==1;
    const auto joinId=external?owner.inputs.front():id;
    auto join=node(joinId);
    if (!inner(join) || join.query!=owner.query) return;
    JoinGraph graph;
    const auto all=collectJoinTree(joinId,owner.query,graph);
    if (!all) return;
    const std::array<JoinMask,2> sides{graph.originalLeft[all],JoinMask(all^graph.originalLeft[all])};
    for (const auto ref:graph.conditions) {
      std::size_t visits=0;
      if (ref.query!=owner.query || !joinConditionInputs(ref,graph,visits)) return;
    }
    for (const auto ref:owner.predicates) {
      std::size_t visits=0;
      if (ref.query!=owner.query || !joinConditionInputs(ref,graph,visits)) return;
    }
    for (const auto ref:owner.predicates) {
      const auto &condition=modelExpression(logical,ref);
      if (condition.kind!=ExprKind::Binary || condition.value!="OR" || condition.operands.size()!=2)
        continue;
      const auto type=condition.sqlType;
      const auto begin=conjuncts({ref.query,condition.operands[0]});
      const auto end=conjuncts({ref.query,condition.operands[1]});
      if (begin.empty() || end.empty()) continue;
      bool changed=false, multipleTables=false;
      for (std::size_t side=0;side<sides.size();++side) {
        const auto candidates=[&](const std::vector<ExpressionRef> &terms) {
          std::vector<ExprId> result;
          for (const auto term:terms) {
            std::size_t visits=0;
            const auto mask=joinConditionInputs(term,graph,visits);
            if (mask && *mask && (*mask&sides[side])==*mask) result.push_back(term.expression);
          }
          return result;
        };
        const auto a=candidates(begin),b=candidates(end);
        if (a.empty() || b.empty()) continue;
        const auto combine=[&](const std::vector<ExprId> &terms) {
          auto result=terms.front();
          for (std::size_t i=1;i<terms.size();++i)
            result=booleanExpression(ref.query,owner.origin,type,ExprKind::Binary,"AND",
                                     {result,terms[i]}).expression;
          return result;
        };
        const auto left=combine(a),right=combine(b);
        const auto inferred=booleanExpression(ref.query,owner.origin,type,ExprKind::Binary,"OR",{left,right});
        join.inputs[side]=filterInput(owner,join.inputs[side],{inferred});
        changed=true;
        multipleTables|=(sides[side]&(sides[side]-1))!=0;
      }
      if (!changed) continue;
      auto answer=join;
      if (external) {
        const auto filtered=make(std::move(answer));
        answer=owner;
        answer.inputs={filtered};
      }
      equivalent(group,std::move(answer),multipleTables?"ExpandDisjunctionForJoinInputs":"ExpandDisjunctionForTable",id,
                 "每个 OR 分支都必须满足相应输入的部分条件，因此可先过滤该输入，同时保留原 OR 的完整判定");
      return;
    }
  }
  void splitFullJoin(GroupId group,RelId id,const RelNode &join) {
    if (logical.program.sourceDialect!=Dialect::PostgreSQL || join.kind!=RelKind::Join ||
        join.join!=JoinKind::Full || join.inputs.size()!=2 || join.distinct || join.fixedOrder ||
        join.correlated || join.predicates.empty() || join.predicates.size()>32 ||
        join.outputs.empty() || join.outputs.size()>64 || !orderedJoinOutputs(join))
      return;
    auto &owner=logical.program.queries.at(join.query-1);
    if (!owner.catalogChecked || owner.kind!=StatementKind::Select || !owner.groupingSets.empty() ||
        owner.expressions.size()>=c.options.maxNodes)
      return;
    for (const auto id:join.inputs) {
      c.stop();
      const auto &scan=node(id);
      if (scan.kind!=RelKind::Scan || scan.query!=join.query || !scan.table ||
          *scan.table>=owner.tables.size())
        return;
      const auto &table=owner.tables[*scan.table].definition;
      if (!table || table->kind!=ObjectKind::Table || !table->complete || !table->arityKnown ||
          !table->noDescendantsKnown || !table->rowSecurityDisabledKnown ||
          table->columns.size()!=scan.outputs.size() ||
          !std::all_of(table->columns.begin(),table->columns.end(),[](const auto &field) {
            return field.sqlType && signedIntegerBits(*field.sqlType).has_value();
          }))
        return;
    }
    const auto &left=node(join.inputs.front()), &right=node(join.inputs.back());
    std::optional<ExprId> matchColumn;
    for (const auto ref:join.predicates) {
      c.stop();
      if (ref.query!=join.query || !totalPredicateColumns(logical,ref)) return;
      if (matchColumn || !integerEqualityColumns(logical,ref)) continue;
      const auto &equal=modelExpression(logical,ref);
      const auto &first=owner.expressions.at(equal.operands.front());
      const auto &second=owner.expressions.at(equal.operands.back());
      if (first.binding && second.binding) {
        if (first.binding->table==left.table && second.binding->table==right.table)
          matchColumn=equal.operands.front();
        else if (second.binding->table==left.table && first.binding->table==right.table)
          matchColumn=equal.operands.back();
      }
    }
    if (!matchColumn) return;

    // Matched rows necessarily have a non-NULL left equality key. The
    // right-join's padded NULL is therefore an unambiguous unmatched marker.
    ModelExpression unmatched;
    unmatched.kind=ExprKind::IsNull;
    unmatched.origin=join.origin;
    unmatched.type=ValueType::Boolean;
    unmatched.sqlType=modelExpression(logical,join.predicates.front()).sqlType;
    unmatched.value="IS NULL";
    unmatched.operands={*matchColumn};
    const ExpressionRef predicate{join.query,static_cast<ExprId>(owner.expressions.size())};
    c.charge(sizeof(unmatched)+sizeof(ExprId)+unmatched.value.size());
    owner.expressions.push_back(std::move(unmatched));
    auto keepLeft=join;
    keepLeft.join=JoinKind::Left;
    auto probeRight=join;
    probeRight.join=JoinKind::Right;
    auto leftRows=make(std::move(keepLeft));
    auto rightOnly=filterInput(join,make(std::move(probeRight)),{predicate});
    RelNode combined;
    combined.kind=RelKind::SetOperation;
    combined.query=join.query;
    combined.origin=join.origin;
    combined.setKind=SetKind::Union;
    combined.inputs={leftRows,rightOnly};
    combined.outputs=join.outputs;
    equivalent(group,std::move(combined),"FullToLeftAndRightJoin",id,
               "左连接保留全部匹配和左侧未匹配的行，再合并右连接中左键补 NULL 的行，保留重复和列顺序");
  }
  std::optional<TypeId> integerOutputType(const RelNode &relation, std::size_t ordinal) const {
    const auto &source = query(relation.query);
    if (source.kind != StatementKind::Select || relation.outputs.size() != source.projections.size() ||
        ordinal >= relation.outputs.size() || relation.outputs[ordinal].name == "*")
      return {};
    const auto &value = modelExpression(logical, {source.id, source.projections[ordinal].expression});
    if (value.kind == ExprKind::Star) return {};
    auto typeId = value.sqlType;
    if (relation.kind == RelKind::Coerce) {
      const auto found = std::lower_bound(relation.integerWidenings.begin(), relation.integerWidenings.end(), ordinal,
            [](const auto &conversion, auto column) { return conversion.ordinal < column; });
      if (found != relation.integerWidenings.end() && found->ordinal == ordinal)
        typeId = found->target;
    }
    if (!signedIntegerBits(logical.program.types.get(typeId))) return {};
    return typeId;
  }
  bool sameIntegerOutputs(const RelNode &left, const RelNode &right) {
    const auto width = left.outputs.size();
    if (!width || width != right.outputs.size()) return false;
    for (std::size_t i = 0; i < width; ++i) {
      c.stop();
      const auto a = integerOutputType(left, i), b = integerOutputType(right, i);
      if (!a || !b || logical.program.types.get(*a).bits != logical.program.types.get(*b).bits)
        return false;
    }
    return true;
  }
  void coerceSetInputs(GroupId group, RelId id, const RelNode &set) {
    if (logical.program.sourceDialect != Dialect::PostgreSQL ||
        set.kind != RelKind::SetOperation || set.inputs.size() != 2 || set.outputs.empty())
      return;
    const auto &source = query(set.query);
    if (!source.setOperation || source.setOperation->recursive ||
        node(set.inputs[0]).query != source.setOperation->left ||
        node(set.inputs[1]).query != source.setOperation->right)
      return;
    const auto *proof = properties(id);
    if (!proof || proof->mayHaveEffects) return;
    std::vector<RelNode> casts;
    for (const auto arm : set.inputs) {
      const auto &branch = node(arm);
      if (branch.outputs.size() != set.outputs.size()) return;
      RelNode coerce;
      coerce.kind = RelKind::Coerce;
      coerce.query = branch.query;
      coerce.origin = branch.origin;
      coerce.inputs = {arm};
      coerce.outputs = branch.outputs;
      for (std::size_t i = 0; i < set.outputs.size(); ++i) {
        c.stop();
        const auto input = integerOutputType(branch, i), output = integerOutputType(set, i);
        if (!input || !output) return;
        const auto from = *logical.program.types.get(*input).bits;
        const auto to = *logical.program.types.get(*output).bits;
        if (from > to) return;
        if (from < to) coerce.integerWidenings.push_back({i, *input, *output});
      }
      casts.push_back(std::move(coerce));
    }
    auto replacement = set;
    bool changed = false;
    for (std::size_t i = 0; i < casts.size(); ++i) {
      if (casts[i].integerWidenings.empty()) continue;
      replacement.inputs[i] = make(std::move(casts[i]));
      changed = true;
    }
    if (changed)
      equivalent(group, std::move(replacement), "CoerceInputs", id,
                 "按集合公共类型显式扩宽有符号整数列，保留列位置、NULL 和顺序");
  }
  bool directSetProjection(const RelNode &project, const RelNode &set) {
    if (project.kind != RelKind::Project || project.query != set.query ||
        project.inputs.size() != 1 || project.keys.size() != set.outputs.size() ||
        project.outputs.size() != set.outputs.size())
      return false;
    const auto &owner = query(set.query);
    if (!owner.setOperation || owner.projections.size() != project.keys.size())
      return false;
    for (std::size_t i = 0; i < project.keys.size(); ++i) {
      if (!sameColumn(project.outputs[i], set.outputs[i]) || project.keys[i].query != set.query ||
          project.keys[i].expression != owner.projections[i].expression)
        return false;
    }
    return true;
  }
  void mergeSets(GroupId group, RelId id, const RelNode &set) {
    constexpr std::size_t maxArms = 64;
    if (!c.options.unionMerge || logical.program.sourceDialect != Dialect::PostgreSQL ||
        set.kind != RelKind::SetOperation || set.setAlgorithm!=SetAlgorithm::Native ||
        set.inputs.size() < 2 || set.inputs.size() >= maxArms)
      return;
    const auto *proof = properties(id);
    if (!proof || proof->mayHaveEffects ||
        !std::all_of(set.inputs.begin(),set.inputs.end(),[&](RelId arm) {
          return sameIntegerOutputs(set,node(arm));
        }))
      return;
    const auto matching = [&](const RelNode &child) {
      return child.kind == RelKind::SetOperation && child.setKind == set.setKind &&
             child.distinct == set.distinct && child.setAlgorithm==SetAlgorithm::Native;
    };
    auto merged = set;
    merged.inputs.clear();
    auto remaining = maxArms - set.inputs.size();
    for (std::size_t i=0; i<set.inputs.size(); ++i) {
      c.stop();
      const auto arm = set.inputs[i];
      // Difference associates only on the left. Keep the evaluation order
      // and reject mixed ALL / DISTINCT boundaries for every operator.
      if (set.setKind == SetKind::Except && i) {
        merged.inputs.push_back(arm);
        continue;
      }
      auto child = node(arm);
      if (child.kind == RelKind::Project && child.inputs.size() == 1) {
        const auto &inside = node(child.inputs.front());
        if (matching(inside) && directSetProjection(child, inside))
          child = inside;
      }
      if (matching(child) && sameIntegerOutputs(set, child) &&
          child.inputs.size() >= 2 && child.inputs.size() - 1 <= remaining &&
          std::all_of(child.inputs.begin(), child.inputs.end(), [&](auto branch) {
            return sameIntegerOutputs(child, node(branch));
          })) {
        merged.inputs.insert(merged.inputs.end(), child.inputs.begin(), child.inputs.end());
        remaining -= child.inputs.size() - 1;
      } else {
        merged.inputs.push_back(arm);
      }
    }
    if (merged.inputs.size() <= set.inputs.size()) return;
    const char *name=set.setKind==SetKind::Union ? "UnionMerge" :
                     set.setKind==SetKind::Intersect ? "IntersectMerge" : "ExceptMerge";
    equivalent(group, std::move(merged), name, id,
        "合并同类型、同重复语义的嵌套集合，保留分支顺序、行重数与输出位置");
  }
  void reorderIntersect(GroupId group, RelId id, const RelNode &set) {
    if (logical.program.sourceDialect != Dialect::PostgreSQL ||
        set.kind != RelKind::SetOperation || set.setKind != SetKind::Intersect ||
        set.setAlgorithm!=SetAlgorithm::Native || set.inputs.size() != 2)
      return;
    const auto *proof = properties(id);
    if (!proof || proof->mayHaveEffects ||
        !sameIntegerOutputs(set, node(set.inputs[0])) || !sameIntegerOutputs(set, node(set.inputs[1])))
      return;
    const auto left = groups.at(intern(set.inputs[0])).estimate;
    const auto right = groups.at(intern(set.inputs[1])).estimate;
    if (right.rows >= left.rows) return;
    auto reordered = set;
    std::swap(reordered.inputs[0], reordered.inputs[1]);
    equivalent(group, std::move(reordered), "IntersectReorder", id,
               "将已验证的 INTERSECT 小输入放在哈希构建侧，保留原输出列名和重复语义");
  }
  void alternateDistinctSets(GroupId group,RelId id,const RelNode &set) {
    if (!c.options.alternateSetAlgorithms || logical.program.sourceDialect!=Dialect::PostgreSQL ||
        set.kind!=RelKind::SetOperation || set.setKind==SetKind::Union || !set.distinct ||
        set.setAlgorithm!=SetAlgorithm::Native || set.inputs.size()<2 || set.inputs.size()>64 ||
        set.outputs.empty() || set.outputs.size()>64)
      return;
    const auto *facts=properties(id);
    if (!facts || facts->mayHaveEffects ||
        !std::all_of(set.inputs.begin(),set.inputs.end(),[&](auto arm) {
          return sameIntegerOutputs(set,node(arm));
        }))
      return;
    const bool intersect=set.setKind==SetKind::Intersect;
    auto membership=set;
    membership.setAlgorithm=SetAlgorithm::NullSafeMembership;
    equivalent(group,std::move(membership),intersect?"IntersectToSemiJoin":"MinusToAntiJoin",id,
               "用 NULL 安全的逐列匹配检查各分支，再按完整输出行去重");
    if (intersect) {
      auto exists=set;
      exists.setAlgorithm=SetAlgorithm::CorrelatedExists;
      equivalent(group,std::move(exists),"IntersectToExists",id,
                 "对首分支的候选行执行可提前结束的相关 EXISTS，保留整行 NULL 匹配及最终去重");
    }
    auto aggregate=set;
    aggregate.setAlgorithm=SetAlgorithm::BranchAggregate;
    equivalent(group,std::move(aggregate),intersect?"IntersectToDistinct":"MinusToDistinct",id,
               "记录每行来自的分支，按完整结果行分组并检查各分支是否包含该行");
  }
  void transposeSemiJoinFilter(GroupId group,RelId id,const RelNode &semi) {
    if (!c.options.filterPushdown || logical.program.sourceDialect!=Dialect::PostgreSQL ||
        semi.kind!=RelKind::SemiJoin || semi.inputs.size()!=2 || !membershipColumns(logical,semi))
      return;
    auto filter=node(semi.inputs.front());
    if (filter.kind!=RelKind::Filter || filter.inputs.size()!=1 || filter.predicates.empty() ||
        filter.predicates.size()>64 || filter.query!=semi.query)
      return;
    const auto *left=properties(filter.inputs.front()), *right=properties(semi.inputs[1]);
    if (!left || !right || left->mayHaveEffects || right->mayHaveEffects ||
        !std::all_of(filter.predicates.begin(),filter.predicates.end(),[&](auto predicate) {
          return predicate.query==semi.query && totalPredicateColumns(logical,predicate).has_value();
        }))
      return;
    auto earlier=semi;
    earlier.inputs.front()=filter.inputs.front();
    filter.inputs={make(std::move(earlier))};
    equivalent(group,std::move(filter),"SemiJoinFilterTranspose",id,
               "同一左行独立检查条件和半连接匹配，比较两种执行顺序的成本");
  }
  std::optional<RelNode> membershipForJoin(const RelNode &join) {
    if (logical.program.sourceDialect!=Dialect::PostgreSQL || !inner(join) || join.correlated ||
        join.join!=JoinKind::Inner || join.predicates.size()!=1 ||
        !query(join.query).catalogChecked || join.predicates.front().query!=join.query ||
        !integerEqualityColumns(logical,join.predicates.front()))
      return {};
    const auto &expression=modelExpression(logical,join.predicates.front());
    RelNode semi;
    semi.kind=RelKind::SemiJoin;
    semi.query=join.query;
    semi.origin=join.origin;
    semi.inputs=join.inputs;
    semi.outputs=node(join.inputs.front()).outputs;
    semi.predicates=join.predicates;
    semi.membershipKeys=JoinKeyPair{{join.query,expression.operands.front()},{join.query,expression.operands.back()}};
    if (!membershipColumns(logical,semi))
      std::swap(semi.membershipKeys->left,semi.membershipKeys->right);
    return membershipColumns(logical,semi)?std::optional<RelNode>{std::move(semi)}:std::nullopt;
  }
  void correlateEquijoin(GroupId group,RelId id,const RelNode &join) {
    if (logical.program.sourceDialect!=Dialect::PostgreSQL || join.kind!=RelKind::Join ||
        join.inputs.size()!=2 || (join.join!=JoinKind::Inner && join.join!=JoinKind::Left) ||
        join.fixedOrder || join.distinct || join.correlated || !orderedJoinOutputs(join))
      return;
    const auto &left=node(join.inputs.front()), &right=node(join.inputs.back());
    if (left.kind!=RelKind::Scan || right.kind!=RelKind::Scan || !right.table ||
        left.query!=join.query || right.query!=join.query)
      return;
    const auto &definition=query(join.query).tables.at(*right.table).definition;
    if (!definition || definition->kind!=ObjectKind::Table || !definition->noDescendantsKnown ||
        !definition->rowSecurityDisabledKnown)
      return;
    auto innerJoin=join;
    innerJoin.join=JoinKind::Inner;
    const auto keys=membershipForJoin(innerJoin);
    if (!keys) return;
    auto correlate=join;
    correlate.kind=RelKind::Correlate;
    correlate.correlated=true;
    correlate.correlationKeys=keys->membershipKeys;
    equivalent(group,std::move(correlate),"JoinToCorrelate",id,
               "以左键为参数探测右侧，在同一 Memo 中比较普通连接和依赖查找，保留左连接补 NULL");
  }
  void convertUniqueJoinToSemi(GroupId group,RelId id,const RelNode &project) {
    if (project.kind!=RelKind::Project || project.inputs.size()!=1 || project.keys.empty() ||
        project.keys.size()>64 || project.keys.size()!=project.outputs.size())
      return;
    const auto &join=node(project.inputs.front());
    auto semi=membershipForJoin(join);
    if (!semi || join.query!=project.query) return;
    const auto &owner=query(project.query);
    if (std::any_of(owner.orderBy.begin(),owner.orderBy.end(),[&](const auto &item){
          return !modelExpression(logical,{owner.id,item.expression}).outputColumn;
        }))
      return;
    const auto keys=membershipColumns(logical,*semi);
    const auto *right=properties(semi->inputs.back());
    if (!keys || !right || right->mayHaveEffects || !coversNonNullUniqueKey(*right,{keys->second}))
      return;
    const auto &rightTable=owner.tables.at(keys->second.table).definition;
    if (keys->second.query!=owner.id || !rightTable || !rightTable->rowSecurityDisabledKnown)
      return;
    const auto &left=node(semi->inputs.front());
    for (auto ref:project.keys) {
      c.stop();
      const auto field=boundScalarColumn(logical,ref);
      if (!field || ref.query!=owner.id ||
          std::none_of(left.outputs.begin(),left.outputs.end(),[&](const auto &out){return sameColumn(out,*field);}))
        return;
    }
    auto reduced=project;
    reduced.inputs={make(std::move(*semi))};
    equivalent(group,std::move(reduced),"SemiJoin",id,
               "右侧连接键唯一且投影只使用左侧字段。将每行至多一次的内连接改为存在性检查");
  }
  void redundantSemiJoin(GroupId group,RelId id,const RelNode &join) {
    if (logical.program.sourceDialect!=Dialect::PostgreSQL || !inner(join) ||
        join.join!=JoinKind::Inner || join.predicates.size()!=1)
      return;
    const auto &left=node(join.inputs.front()), &right=node(join.inputs.back());
    if (left.kind==RelKind::SemiJoin && left.inputs.size()==2 && left.inputs.back()==join.inputs.back() &&
        left.predicates.size()==1 && left.predicates.front().query==join.predicates.front().query &&
        left.predicates.front().expression==join.predicates.front().expression &&
        membershipColumns(logical,left)) {
      auto restored=join;
      restored.inputs.front()=left.inputs.front();
      equivalent(group,std::move(restored),"SemiJoinRemove",id,
                 "后续内连接已经检查同一输入与同一等值条件，移除重复的存在性检查");
      return;
    }
    if (!c.options.filterPushdown || left.kind!=RelKind::Scan || right.kind!=RelKind::Scan)
      return;
    JoinGraph graph;
    if (!collectJoinTree(id,join.query,graph) || graph.scans.size()!=2) return;
    auto semi=membershipForJoin(join);
    if (!semi) return;
    auto filtered=join;
    filtered.inputs.front()=make(std::move(*semi));
    equivalent(group,std::move(filtered),"JoinAddRedundantSemiJoin",id,
               "加入可提前筛掉无匹配左行的半连接，原内连接仍保留右侧输出和匹配重数");
  }
  void transposeSemiInput(GroupId group,RelId id,const RelNode &semi) {
    if (logical.program.sourceDialect!=Dialect::PostgreSQL || semi.kind!=RelKind::SemiJoin)
      return;
    const auto keys=membershipColumns(logical,semi);
    const auto *right=semi.inputs.size()==2?properties(semi.inputs.back()):nullptr;
    if (!keys || !right || right->mayHaveEffects) return;
    auto outer=node(semi.inputs.front());
    if (outer.query!=semi.query) return;
    if (outer.kind==RelKind::Project && outer.inputs.size()==1 &&
        outer.keys.size()==outer.outputs.size()) {
      const auto calc=asScalarCalc(c,outer);
      if (!calc) return;
      auto moved=semi;
      moved.inputs.front()=outer.inputs.front();
      moved.outputs=node(moved.inputs.front()).outputs;
      const auto projected=std::find_if(outer.outputs.begin(),outer.outputs.end(),[&](const auto &field) {
        return sameColumn(field,keys->first);
      });
      if (projected==outer.outputs.end()) return;
      moved.membershipKeys->left=outer.keys[std::size_t(projected-outer.outputs.begin())];
      if (!membershipColumns(logical,moved)) return;
      outer.inputs={make(std::move(moved))};
      equivalent(group,std::move(outer),"SemiJoinProjectTranspose",id,
                 "按输出位置将半连接键绑定到直接投影前，在投影前执行存在性检查");
    } else if (inner(outer) && !outer.correlated && orderedJoinOutputs(outer)) {
      for (auto ref:outer.predicates)
        if (!totalPredicateColumns(logical,ref)) return;
      for (std::size_t side=0;side<2;++side) {
        auto moved=semi;
        moved.inputs.front()=outer.inputs[side];
        moved.outputs=node(moved.inputs.front()).outputs;
        if (!membershipColumns(logical,moved)) continue;
        outer.inputs[side]=make(std::move(moved));
        equivalent(group,std::move(outer),"SemiJoinJoinTranspose",id,
                   "半连接键只来自内连接的一侧，在这一侧先检查存在性并保持原列顺序");
        return;
      }
    }
  }
  void distributeJoinOverUnion(GroupId group,RelId id,const RelNode &join) {
    if (!c.options.joinUnionTranspose || logical.program.sourceDialect!=Dialect::PostgreSQL ||
        join.kind!=RelKind::Join || join.distinct || join.correlated || join.fixedOrder ||
        join.inputs.size()!=2 || join.outputs.empty() || join.outputs.size()>64 ||
        join.predicates.empty() || join.predicates.size()>16)
      return;
    const auto &ownerQuery=query(join.query);
    if (!ownerQuery.catalogChecked || !ownerQuery.groupingSets.empty()) return;
    const auto &left=node(join.inputs[0]), &right=node(join.inputs[1]);
    if (left.outputs.size()+right.outputs.size()!=join.outputs.size() ||
        !std::equal(left.outputs.begin(),left.outputs.end(),join.outputs.begin(),sameColumn) ||
        !std::equal(right.outputs.begin(),right.outputs.end(),join.outputs.begin()+left.outputs.size(),sameColumn))
      return;
    for (std::size_t side=0;side<2;++side) {
      if (join.join!=JoinKind::Inner && !(side==0 && join.join==JoinKind::Left) &&
          !(side==1 && join.join==JoinKind::Right)) continue;
      const auto unionId=join.inputs[side], otherId=join.inputs[1-side];
      auto derived=node(unionId);
      const auto &other=node(otherId);
      if (derived.kind!=RelKind::Subquery || derived.query!=join.query || !derived.table ||
          derived.inputs.size()!=1 || derived.scalar || derived.correlated || derived.materialized ||
          other.kind!=RelKind::Scan || other.query!=join.query || !other.table ||
          derived.table==other.table)
        continue;
      const auto &binding=ownerQuery.tables.at(*derived.table);
      if (!binding.query || !binding.name.empty() || !binding.definition ||
          binding.definition->columns.size()!=derived.outputs.size()) continue;
      auto root=node(derived.inputs.front());
      if (root.kind==RelKind::Project && root.inputs.size()==1) {
        const auto &inside=node(root.inputs.front());
        if (isUnionAll(inside) && directSetProjection(root,inside)) root=inside;
      }
      const auto *unionFacts=properties(unionId), *otherFacts=properties(otherId);
      if (!isUnionAll(root) || root.query!=binding.query || root.inputs.size()<2 ||
          root.inputs.size()>8 || root.outputs.size()!=derived.outputs.size() ||
          !unionFacts || !otherFacts || unionFacts->mayHaveEffects || otherFacts->mayHaveEffects ||
          !std::all_of(root.inputs.begin(),root.inputs.end(),[&](auto arm) {
            return sameIntegerOutputs(root,node(arm));
          }))
        continue;
      if (!std::all_of(join.outputs.begin(),join.outputs.end(),[&](const auto &field) {
            if (field.query!=join.query || field.table>=ownerQuery.tables.size()) return false;
            const auto &definition=ownerQuery.tables[field.table].definition;
            if (!definition) return false;
            const auto column=std::find_if(definition->columns.begin(),definition->columns.end(),
                [&](const auto &item){return item.name==field.name;});
            if (column==definition->columns.end()) return false;
            // The derived output's type comes from the resolved set, whose
            // arms were checked above. Its catalog descriptor only has names.
            return field.table==derived.table ||
                   (column->sqlType && signedIntegerBits(*column->sqlType).has_value());
          }))
        continue;
      if (!std::all_of(join.predicates.begin(),join.predicates.end(),[&](auto ref) {
            if (ref.query!=join.query) return false;
            const auto equality=integerEqualityColumns(logical,ref,IntegerEquality::ExactSigned);
            return equality &&
                ((equality->first.table==derived.table && equality->second.table==other.table) ||
                 (equality->first.table==other.table && equality->second.table==derived.table));
          }))
        continue;
      RelNode result;
      result.kind=RelKind::SetOperation;
      result.query=join.query;
      result.origin=join.origin;
      result.outputs=join.outputs;
      for (auto arm:root.inputs) {
        c.stop();
        auto branch=join;
        derived.inputs={arm};
        branch.inputs[side]=make(derived);
        result.inputs.push_back(make(std::move(branch)));
      }
      equivalent(group,std::move(result),"JoinUnionTranspose",id,
                 "将已验证的内连接或外连接保留侧分配到 UNION ALL 各分支，保持重数和补 NULL");
    }
  }
  void unionToDistinct(GroupId group, RelId id, const RelNode &set) {
    if (logical.program.sourceDialect != Dialect::PostgreSQL ||
        set.kind != RelKind::SetOperation || set.setKind != SetKind::Union ||
        !set.distinct || set.inputs.size() != 2)
      return;
    const auto *proof = properties(id);
    if (!proof || proof->mayHaveEffects ||
        !std::all_of(set.inputs.begin(), set.inputs.end(), [&](RelId arm) {
          return sameIntegerOutputs(set, node(arm));
        }))
      return;
    auto all = set;
    all.distinct = false;
    RelNode aggregate;
    aggregate.kind = RelKind::Aggregate;
    aggregate.query = set.query;
    aggregate.origin = set.origin;
    aggregate.outputs = set.outputs;
    aggregate.distinct = true;
    std::set<std::string> names;
    const auto &source = query(set.query);
    for (std::size_t i = 0; i < set.outputs.size(); ++i) {
      auto ref = ExpressionRef{source.id, source.projections[i].expression};
      const auto col = boundScalarColumn(logical, ref);
      if (!col || !sameColumn(*col, set.outputs[i]) || !names.insert(col->name).second)
        return;
      aggregate.keys.push_back(ref);
    }
    aggregate.inputs = {make(std::move(all))};
    equivalent(group, std::move(aggregate), "UnionToDistinct", id,
               "以定宽整数 UNION ALL 读取原分支，再按完整结果行去重");
  }
  void pullUpUnionDistinct(GroupId group, RelId id, const RelNode &set) {
    if (logical.program.sourceDialect != Dialect::PostgreSQL ||
        set.kind != RelKind::SetOperation || set.setKind != SetKind::Union ||
        !set.distinct || set.inputs.size() != 2 || set.outputs.empty() ||
        set.outputs.size() > 64)
      return;
    const auto *proof = properties(id);
    if (!proof || proof->mayHaveEffects)
      return;
    for (auto input : set.inputs)
      if (!sameIntegerOutputs(set,node(input))) return;
    auto unified=set;
    for (auto &arm : unified.inputs) {
      c.stop();
      const auto &distinct=node(arm);
      if (distinct.kind!=RelKind::Aggregate || !distinct.distinct ||
          distinct.inputs.size()!=1 || !distinct.groupingSets.empty() ||
          distinct.keys.size()!=set.outputs.size())
        continue;
      const auto &projection=node(distinct.inputs.front());
      if(projection.kind!=RelKind::Project || projection.inputs.size()!=1 ||
         projection.query!=distinct.query || projection.keys.size()!=distinct.keys.size() ||
         projection.outputs.size()!=distinct.outputs.size() ||
         !sameIntegerOutputs(set,projection))
        continue;
      bool sameRows=true;
      for(std::size_t i=0;i<projection.keys.size();++i) {
        const auto projected=projection.keys[i], key=distinct.keys[i];
        if (projected.query!=key.query || projected.expression!=key.expression ||
            !sameColumn(projection.outputs[i],distinct.outputs[i])) {
          sameRows=false;
          break;
        }
      }
      if(sameRows)arm=distinct.inputs.front();
    }
    if(unified.inputs!=set.inputs)
      equivalent(group,std::move(unified),"AggregateUnionAggregate",id,
                 "外层 UNION 已按完整定宽整数行去重，移除分支的重复去重并保留 NULL 和列顺序");
  }
  void transposeSortProject(GroupId group, RelId id, const RelNode &sort) {
    if (logical.program.sourceDialect != Dialect::PostgreSQL ||
        sort.kind != RelKind::Sort || sort.inputs.size() != 1 ||
        sort.keys.empty())
      return;
    const auto &project = node(sort.inputs.front());
    if (project.kind != RelKind::Project || project.query != sort.query ||
        project.inputs.size() != 1 || project.keys.empty() ||
        project.keys.size() != project.outputs.size())
      return;
    const auto *proof = properties(id);
    if (!proof || proof->mayHaveEffects)
      return;

    const auto &input = node(project.inputs.front());
    using ColumnKey = std::tuple<QueryId, std::size_t, std::string>;
    std::map<ColumnKey, std::size_t> inputCounts;
    for (const auto &col : input.outputs)
      ++inputCounts[{col.query, col.table, col.name}];
    const auto available = [&](ExpressionRef ref) {
      const auto column = boundScalarColumn(logical, ref);
      if (!column)
        return false;
      const auto found = inputCounts.find({column->query, column->table, column->name});
      return found != inputCounts.end() && found->second == 1;
    };
    std::set<ColumnKey> names;
    for (std::size_t i = 0; i < project.keys.size(); ++i) {
      c.stop();
      const auto &out = project.outputs[i];
      if (!available(project.keys[i]) || !names.emplace(out.query, out.table, out.name).second)
        return;
    }

    auto inside = sort;
    inside.inputs = project.inputs;
    inside.outputs = input.outputs;
    for (auto &key : inside.keys) {
      const auto &expr = modelExpression(logical, key);
      if (expr.outputColumn) {
        if (*expr.outputColumn >= project.keys.size())
          return;
        key = project.keys[*expr.outputColumn];
      }
      if (!available(key))
        return;
    }
    auto outside = project;
    outside.inputs = {make(std::move(inside))};
    equivalent(group, std::move(outside), "SortProjectTranspose", id,
               "在直接列投影之前排序，按输出位置还原排序列，保留方向和 NULL 顺序");
  }
  void copyJoinSort(GroupId group,RelId id,const RelNode &sort) {
    if (logical.program.sourceDialect!=Dialect::PostgreSQL || sort.kind!=RelKind::Sort ||
        sort.inputs.size()!=1 || sort.keys.empty() || sort.keys.size()>32 ||
        sort.descending.size()!=sort.keys.size() || sort.nullOrdering.size()!=sort.keys.size())
      return;
    auto sourceId=sort.inputs.front();
    std::optional<RelNode> projection;
    const auto &sourceNode=node(sourceId);
    if (sourceNode.kind==RelKind::Project) {
      if (sourceNode.query!=sort.query || sourceNode.inputs.size()!=1 ||
          sourceNode.keys.size()!=sourceNode.outputs.size() ||
          !std::all_of(sourceNode.keys.begin(),sourceNode.keys.end(),[&](const auto ref) {
            return boundScalarColumn(logical,ref).has_value();
          }))
        return;
      projection=sourceNode;
      sourceId=sourceNode.inputs.front();
    }
    auto join=node(sourceId);
    if (join.kind!=RelKind::Join || join.query!=sort.query || join.inputs.size()!=2 ||
        join.distinct || join.correlated || join.fixedOrder || !orderedJoinOutputs(join))
      return;
    const auto &left=node(join.inputs[0]), &right=node(join.inputs[1]);
    if (left.kind!=RelKind::Scan || right.kind!=RelKind::Scan ||
        !query(join.query).catalogChecked || !left.table || !right.table)
      return;
    std::array<RelNode,2> copies;
    for (std::size_t i=0; i<sort.keys.size(); ++i) {
      c.stop();
      auto ref=sort.keys[i];
      const auto &expression=modelExpression(logical,ref);
      const auto &scope=query(sort.query);
      if (ref.query!=scope.id) return;
      if (expression.outputColumn) {
        if (*expression.outputColumn>=scope.projections.size()) return;
        ref.expression=scope.projections[*expression.outputColumn].expression;
      }
      const auto field=boundScalarColumn(logical,ref);
      if (!field || !signedIntegerBits(logical.program.types.get(modelExpression(logical,ref).sqlType)))
        return;
      const auto side=field->table==left.table?0u:1u;
      const auto &source=side?right:left;
      if (std::none_of(source.outputs.begin(),source.outputs.end(),[&](const auto &column) {
            return sameColumn(column,*field);
          }))
        return;
      auto &copy=copies[side];
      copy.keys.push_back(ref);
      copy.descending.push_back(sort.descending[i]);
      copy.nullOrdering.push_back(sort.nullOrdering[i]);
    }
    for (std::size_t side=0; side<copies.size(); ++side) {
      auto &copy=copies[side];
      if (copy.keys.empty()) continue;
      copy.kind=RelKind::Sort;
      copy.query=sort.query;
      copy.origin=sort.origin;
      copy.inputs={join.inputs[side]};
      copy.outputs=node(join.inputs[side]).outputs;
      join.inputs[side]=make(std::move(copy));
    }
    auto input=make(std::move(join));
    if (projection) {
      projection->inputs={input};
      input=make(std::move(*projection));
    }
    auto retained=sort;
    retained.inputs={input};
    equivalent(group,std::move(retained),"SortJoinCopy",id,
               "将可分别求值的整型排序键复制到连接输入，保留全局排序与所有分页语义");
  }
  void scalarCalcAlternatives(GroupId group,RelId id,const RelNode &relation) {
    const auto upper=asScalarCalc(c,relation);
    if (!upper) return;
    const auto &input=node(upper->inputs.front());
    if (relation.kind!=RelKind::Calc) {
      equivalent(group,*upper,relation.kind==RelKind::Project?"ProjectToCalc":"FilterToCalc",id,
                 "用 Calc 表示同一作用域的标量过滤或直接列投影");
      // Adding an alternative may grow the owning node arena.
    } else if (isIdentityCalc(logical,relation)) {
      equivalent(group,input,"CalcRemove",id,"完全相同的无条件 Calc 可直接使用输入");
      return;
    }
    const auto lower=asScalarCalc(c,node(upper->inputs.front()));
    if (!lower) return;
    if (auto fused=mergeScalarCalcs(c,*upper,*lower)) {
      const auto rule=relation.kind==RelKind::Project?"ProjectCalcMerge":
          relation.kind==RelKind::Filter?"FilterCalcMerge":"CalcMerge";
      equivalent(group,std::move(*fused),rule,id,
                 "合并相邻的标量过滤及列投影，在同一行上先检查条件再计算输出");
    }
  }
  void transposeAggregateFilter(GroupId group,RelId id,const RelNode &aggregate) {
    if (logical.program.sourceDialect!=Dialect::PostgreSQL || aggregate.kind!=RelKind::Aggregate ||
        aggregate.distinct || aggregate.inputs.size()!=1 || aggregate.keys.empty() ||
        aggregate.keys.size()>64 || !aggregate.groupingSets.empty())
      return;
    const auto &owner=query(aggregate.query);
    if (!owner.catalogChecked || owner.having || !owner.namedWindows.empty() ||
        owner.projections.empty() || owner.projections.size()>64)
      return;
    auto filter=node(aggregate.inputs.front());
    if (filter.kind!=RelKind::Filter || filter.query!=aggregate.query || filter.inputs.size()!=1 ||
        filter.predicates.empty() || filter.predicates.size()>32)
      return;
    const auto &source=node(filter.inputs.front());
    if (source.kind!=RelKind::Scan || source.query!=aggregate.query ||
        !properties(filter.inputs.front()) || properties(filter.inputs.front())->mayHaveEffects)
      return;
    std::vector<ColumnRef> keys;
    for (auto key:aggregate.keys) {
      c.stop();
      if (key.query!=owner.id) return;
      const auto &expression=modelExpression(logical,key);
      if (expression.outputColumn) {
        if (*expression.outputColumn>=owner.projections.size()) return;
        key.expression=owner.projections[*expression.outputColumn].expression;
      }
      const auto field=boundScalarColumn(logical,key);
      if (!field || !signedIntegerBits(logical.program.types.get(modelExpression(logical,key).sqlType)))
        return;
      keys.push_back(*field);
    }
    const auto grouped=[&](const ColumnRef &field) {
      return std::any_of(keys.begin(),keys.end(),[&](const auto &key){return sameColumn(key,field);});
    };
    for (auto predicate:filter.predicates) {
      c.stop();
      if (predicate.query!=owner.id) return;
      const auto fields=totalPredicateColumns(logical,predicate);
      if (!fields || !std::all_of(fields->begin(),fields->end(),grouped)) return;
    }
    // Extra groups will be computed and then discarded. Limit the rule to
    // group references and native extrema with no evaluation errors.
    for (const auto &output:owner.projections) {
      c.stop();
      const ExpressionRef ref{owner.id,output.expression};
      if (const auto field=boundScalarColumn(logical,ref); field && grouped(*field)) continue;
      const auto &call=modelExpression(logical,ref);
      const auto op=builtinOperator(Dialect::PostgreSQL,call.name,call.nameQuoted,call.operands.size());
      if (call.kind!=ExprKind::Call || call.distinct || call.window || call.operands.size()!=1 ||
          !op || op->kind!=RoutineKind::Aggregate || (op->name!="MIN" && op->name!="MAX") ||
          (call.routineResolved && (call.routineSchema!="pg_catalog" ||
          call.routineKind!=RoutineKind::Aggregate || call.routineVolatility!=FunctionVolatility::Immutable)))
        return;
      const ExpressionRef arg{owner.id,call.operands.front()};
      if (!boundScalarColumn(logical,arg) || !sameIntegerType(logical,ref,arg) ||
          !signedIntegerBits(logical.program.types.get(modelExpression(logical,arg).sqlType)))
        return;
    }
    auto input=aggregate;
    input.inputs=filter.inputs;
    filter.inputs={make(std::move(input))};
    equivalent(group,std::move(filter),"AggregateFilterTranspose",id,
               "先按已验证的整数键分组，再以相同键条件过滤。为聚合物化表匹配保留额外候选");
  }
  using LiteralTuple = std::vector<std::optional<std::int64_t>>;
  std::optional<LiteralTuple> literalTuple(const RelNode &values, std::size_t offset,
                                           const std::vector<int> &widths) {
    LiteralTuple tuple(widths.size());
    for (std::size_t j = 0; j < widths.size(); ++j) {
      c.stop();
      const auto ref = values.keys[offset + j];
      const auto &expression = modelExpression(logical, ref);
      if (expression.kind == ExprKind::Null) continue;
      if (signedIntegerBits(logical.program.types.get(expression.sqlType)) != widths[j]) return {};
      const ModelExpression *literal = &expression;
      std::string sign;
      if (expression.kind == ExprKind::Unary && expression.operands.size() == 1 &&
          (expression.value == "+" || expression.value == "-")) {
        literal = &modelExpression(logical, {ref.query, expression.operands.front()});
        if (expression.value == "-") sign = "-";
      }
      if (literal->kind != ExprKind::Number || literal->value.size() > 19) return {};
      const auto text = sign + literal->value;
      const auto *end = text.data() + text.size();
      std::int64_t number = 0;
      const auto parsed = std::from_chars(text.data(), end, number);
      if (parsed.ec != std::errc{} || parsed.ptr != end) return {};
      tuple[j] = number;
    }
    return tuple;
  }
  void foldValuesUnion(GroupId group, RelId id, const RelNode &set) {
    if (logical.program.sourceDialect != Dialect::PostgreSQL || set.kind != RelKind::SetOperation ||
        set.setKind != SetKind::Union || set.inputs.size() < 2 || set.inputs.size() > 16 ||
        set.outputs.empty() || set.outputs.size() > 32) return;
    const auto width = set.outputs.size();
    std::vector<int> widths;
    for (std::size_t i = 0; i < width; ++i) {
      const auto type = integerOutputType(set, i);
      if (!type) return;
      widths.push_back(*logical.program.types.get(*type).bits);
    }
    std::size_t count = 0;
    for (auto ref : set.inputs) {
      c.stop();
      const auto &values = node(ref);
      if (values.kind != RelKind::Values || !values.inputs.empty() || values.valueCount > 256 - count ||
          values.outputs.size() != width || values.keys.size() != values.valueCount * width ||
          !sameIntegerOutputs(set, values)) return;
      count += values.valueCount;
    }
    c.charge(count * (sizeof(LiteralTuple) + 3 * sizeof(void *) +
                         width * (sizeof(LiteralTuple::value_type) + sizeof(ExpressionRef))));
    auto result = set;
    result.kind = RelKind::Values;
    result.distinct = false;
    result.valueCount = 0;
    result.inputs.clear();
    result.keys.clear();
    result.keys.reserve(count * width);
    std::set<LiteralTuple> seen;
    for (auto ref : set.inputs) {
      const auto &values = node(ref);
      for (std::size_t offset = 0; offset < values.keys.size(); offset += width) {
        auto row = literalTuple(values, offset, widths);
        if (!row) return;
        if (set.distinct && !seen.insert(std::move(*row)).second) continue;
        ++result.valueCount;
        result.keys.insert(result.keys.end(), values.keys.begin() + offset, values.keys.begin() + offset + width);
      }
    }
    equivalent(group, std::move(result), "UnionToValues", id,
               "已验证的同类型整数 VALUES 合并为一组字面量行，保留 ALL 重复或按整行去重");
  }
  std::string sharedArmSignature(RelId id,const RelNode &set) {
    const auto &projection=node(id);
    if (projection.kind!=RelKind::Project || projection.inputs.size()!=1 ||
        projection.keys.size()!=set.outputs.size() || !sameIntegerOutputs(set,projection))
      return {};
    const auto &owner=query(projection.query);
    if (!owner.catalogChecked || owner.tables.size()!=1 || owner.selectInto ||
        !owner.ctes.empty() || !owner.modifiers.empty())
      return {};
    const auto &body=node(projection.inputs.front());
    const auto filtered=body.kind==RelKind::Filter && body.inputs.size()==1;
    const auto &scan=filtered?node(body.inputs.front()):body;
    if (scan.kind!=RelKind::Scan || scan.query!=owner.id || !scan.table || *scan.table!=0 ||
        (filtered && (body.query!=owner.id || body.predicates.size()>32)))
      return {};
    const auto &binding=owner.tables.front();
    const auto &table=binding.definition;
    if (!table || table->kind!=ObjectKind::Table || table->database.empty() || table->schema.empty() ||
        !table->noDescendantsKnown || !table->rowSecurityDisabledKnown || !table->complete ||
        !table->arityKnown)
      return {};
    std::ostringstream signature;
    const auto str=[&](std::string_view value) { signature<<value.size()<<':'<<value; };
    str(table->database);
    str(table->schema);
    str(table->name);
    signature<<'/'<<int(binding.inheritance)<<'/';
    for (auto ref:projection.keys) {
      c.stop();
      const auto col=boundScalarColumn(logical,ref);
      if (!col || ref.query!=owner.id || col->table!=0) return {};
      const auto &value=modelExpression(logical,ref);
      const auto bits=signedIntegerBits(logical.program.types.get(value.sqlType));
      if (!bits) return {};
      signature<<value.binding->column<<','<<*bits<<';';
    }
    if (filtered) for (auto ref:body.predicates) {
      c.stop();
      const auto used=totalPredicateColumns(logical,ref);
      if (!used || ref.query!=owner.id) return {};
      const auto &predicate=modelExpression(logical,ref);
      signature<<'/'<<static_cast<int>(predicate.kind)<<':';
      str(predicate.value);
      for (auto operand:predicate.operands) {
        const auto &value=modelExpression(logical,{ref.query,operand});
        if (value.kind==ExprKind::Name && value.binding && value.binding->scope==owner.id &&
            value.binding->table==0) {
          signature<<'c'<<value.binding->column<<';';
        } else if (value.kind==ExprKind::Number) {
          signature<<'n';
          str(value.value);
        } else {
          return {};
        }
      }
    }
    auto key=signature.str();
    c.charge(key.size()+80);
    return key;
  }
  RelId materializeSharedInput(RelId producer,SourceRange origin) {
    using namespace compiler;
    const auto &source=node(producer);
    if (logical.program.queries.size()>=c.options.maxNodes) throw MemoLimit{};
    const auto queryId=static_cast<QueryId>(logical.program.queries.size()+1);
    auto definition=std::make_shared<TableDefinition>();
    definition->name="navidog_shared_"+std::to_string(queryId);
    RelNode consume;
    consume.kind=RelKind::Subquery;
    consume.query=queryId;
    consume.origin=origin;
    consume.inputs={producer};
    consume.table=0;
    consume.materialized=true;
    for (std::size_t i=0;i<source.outputs.size();++i) {
      const auto type=integerOutputType(source,i);
      if (!type) return 0;
      const auto name="field_"+std::to_string(i);
      definition->columns.push_back({name,ValueType::Integer,
          std::make_shared<const SqlType>(logical.program.types.get(*type))});
      consume.outputs.push_back({queryId,0,name});
    }
    ModelStatement wrapper;
    wrapper.id=queryId;
    wrapper.origin=origin;
    wrapper.catalogChecked=true;
    wrapper.context=query(source.query).context;
    ModelTable binding;
    binding.name={definition->name};
    binding.query=source.query;
    binding.definition=std::move(definition);
    binding.cteMaterialization=CteMaterialization::Materialized;
    c.charge(sizeof(wrapper)+sizeof(binding)+sizeof(TableDefinition)+
             source.outputs.size()*(sizeof(ColumnDefinition)+sizeof(SqlType)+64));
    wrapper.tables.push_back(std::move(binding));
    logical.program.queries.push_back(std::move(wrapper));
    return make(std::move(consume));
  }
  void shareUnionInputs(GroupId group,RelId id,const RelNode &set) {
    if (logical.program.sourceDialect!=Dialect::PostgreSQL || !isUnionAll(set) ||
        set.inputs.size()<2 || set.inputs.size()>8 || set.outputs.empty() || set.outputs.size()>32)
      return;
    auto combined=set;
    std::map<std::string,std::vector<std::size_t>> matches;
    for (std::size_t i=0;i<set.inputs.size();++i) {
      auto key=sharedArmSignature(set.inputs[i],set);
      if (!key.empty()) matches[std::move(key)].push_back(i);
    }
    std::vector<SharedRelation> evidence;
    for (const auto &[key,positions]:matches) {
      c.stop();
      if (positions.size()<2) continue;
      auto entry=sharedInputs.find(key);
      if (entry==sharedInputs.end()) {
        const auto consumer=materializeSharedInput(set.inputs[positions.front()],set.origin);
        if (!consumer) continue;
        c.charge(key.size()+64);
        entry=sharedInputs.emplace(key,consumer).first;
      }
      for (auto position:positions) combined.inputs[position]=entry->second;
      evidence.push_back({node(entry->second).inputs.front(),entry->second,positions.size()});
    }
    if (!evidence.empty() && equivalent(group,std::move(combined),"CombineSimpleEquivalence",id,
        "相同类型、投影和条件的 UNION ALL 分支共用一次物化读取，每个分支仍保留原有结果重数")) {
      c.charge(evidence.size()*sizeof(SharedRelation));
      c.result.sharedRelations.insert(c.result.sharedRelations.end(),evidence.begin(),evidence.end());
    }
  }
  void foldLiteralDistinct(GroupId group, RelId id, const RelNode &aggregate) {
    if (logical.program.sourceDialect != Dialect::PostgreSQL || aggregate.kind != RelKind::Aggregate ||
        !aggregate.distinct || aggregate.inputs.size() != 1 || aggregate.outputs.empty() ||
        aggregate.outputs.size() > 32) return;
    const auto &query = modelQuery(logical, aggregate.query);
    const auto width = aggregate.outputs.size();
    if (!query.distinct || query.projections.size() != width) return;
    std::vector<int> widths;
    for (const auto &projection : query.projections) {
      const auto &expression = modelExpression(logical, {query.id, projection.expression});
      const auto bits = signedIntegerBits(logical.program.types.get(expression.sqlType));
      if (!bits || expression.kind != ExprKind::Name || !expression.binding) return;
      widths.push_back(*bits);
    }

    auto input = aggregate.inputs.front();
    for (std::size_t depth = 0; depth < 8; ++depth) {
      c.stop();
      const auto &wrapper = node(input);
      if (wrapper.kind == RelKind::Values) break;
      if (wrapper.inputs.size() != 1 || wrapper.outputs.size() != width || wrapper.materialized ||
          wrapper.correlated || wrapper.scalar) return;
      const auto &source = node(wrapper.inputs.front());
      if (source.outputs.size() != width) return;
      if (wrapper.kind == RelKind::Project) {
        if (wrapper.keys.size() != width) return;
        for (std::size_t i = 0; i < width; ++i) {
          const auto column = boundScalarColumn(logical, wrapper.keys[i]);
          if (!column || !sameColumn(*column, source.outputs[i])) return;
        }
      } else if (wrapper.kind != RelKind::Subquery && wrapper.kind != RelKind::Rename) return;
      input = wrapper.inputs.front();
    }
    const auto &values = node(input);
    if (values.kind != RelKind::Values || values.outputs.size() != width || values.valueCount > 256 ||
        values.keys.size() != values.valueCount * width || !values.inputs.empty()) return;
    auto replacement = aggregate;
    replacement.kind = RelKind::Values;
    replacement.inputs.clear();
    replacement.keys.clear();
    replacement.distinct = false;
    replacement.valueCount = 0;
    c.charge(values.valueCount * (sizeof(LiteralTuple) + 3 * sizeof(void *) +
                               width * (sizeof(LiteralTuple::value_type) + sizeof(ExpressionRef))));
    std::set<LiteralTuple> seen;
    for (std::size_t cell = 0; cell < values.keys.size(); cell += width) {
      auto tuple = literalTuple(values, cell, widths);
      if (!tuple) return;
      if (!seen.insert(std::move(*tuple)).second) continue;
      ++replacement.valueCount;
      replacement.keys.insert(replacement.keys.end(), values.keys.begin() + cell, values.keys.begin() + cell + width);
    }
    if (replacement.valueCount == values.valueCount) return;
    equivalent(group, std::move(replacement), "AggregateValues", id,
               "对有界的同类型整数 VALUES 做整行去重，保留 NULL 与输出列位置");
  }
  void foldEmptyAggregate(GroupId group, RelId id, const RelNode &project) {
    if (logical.program.sourceDialect != Dialect::PostgreSQL || project.kind != RelKind::Project ||
        project.inputs.size() != 1 || project.keys.empty() || project.keys.size() != project.outputs.size())
      return;
    const auto &aggregate = node(project.inputs.front());
    if (aggregate.kind != RelKind::Aggregate || aggregate.query != project.query || aggregate.distinct ||
        !aggregate.keys.empty() || !aggregate.groupingSets.empty() || aggregate.inputs.size() != 1)
      return;
    const auto &query = modelQuery(logical, project.query);
    // A fold must not mask a missing or effectful source (such as a view or
    // table function). The first implementation proves only a direct base
    // source or a scalar SELECT without FROM.
    const auto input = aggregate.inputs.front();
    const auto &relation = node(input);
    const auto base = relation.kind == RelKind::Filter && relation.inputs.size() == 1
        ? relation.inputs.front() : input;
    const auto &source = node(base);
    if (source.kind != RelKind::Values && source.kind != RelKind::Scan) return;
    const auto *proof = properties(base);
    if (!proof || proof->mayHaveEffects) return;
    const auto &alternatives = groups.at(intern(input)).alternatives;
    if (std::none_of(alternatives.begin(), alternatives.end(), [&](auto candidate) {
          const auto &values = node(candidate);
          return values.kind == RelKind::Values && values.valueCount == 0;
        })) return;

    std::vector<ModelExpression> constants;
    c.charge(project.keys.size() * sizeof(ModelExpression));
    for (auto ref : project.keys) {
      c.stop();
      const auto &call = modelExpression(logical, ref);
      if (ref.query != query.id || call.kind != ExprKind::Call || call.distinct || call.window ||
          call.name.empty() || call.operands.size() != 1 ||
          (call.routineResolved && (call.routineSchema != "pg_catalog" || call.routineKind != RoutineKind::Aggregate ||
                                    call.routineVolatility != FunctionVolatility::Immutable))) return;
      const auto name = uppercase(call.name.back());
      if (name != "COUNT" && name != "SUM" && name != "MIN" && name != "MAX") return;
      const auto op = builtinOperator(logical.program.sourceDialect, call.name, call.nameQuoted, 1);
      if (!op || op->kind != RoutineKind::Aggregate) return;
      const ExpressionRef argument{ref.query, call.operands.front()};
      const auto &arg = modelExpression(logical, argument);
      const auto &result = logical.program.types.get(call.sqlType);
      const bool star = name == "COUNT" && arg.kind == ExprKind::Star && arg.name.empty();
      if ((!star && (!boundScalarColumn(logical, argument) ||
                     !signedIntegerBits(logical.program.types.get(arg.sqlType)))) ||
          result.arrayDimensions || !result.opaqueName.empty() ||
          (name == "COUNT" ? signedIntegerBits(result) != 64
                           : !signedIntegerBits(result) && result.family != TypeFamily::Decimal)) return;
      ModelExpression literal;
      literal.kind = name == "COUNT" ? ExprKind::Number : ExprKind::Null;
      literal.type = name == "COUNT" ? ValueType::Integer : ValueType::Null;
      if (name == "COUNT") literal.value = "0";
      literal.sqlType = call.sqlType;
      literal.origin = call.origin;
      constants.push_back(std::move(literal));
    }
    auto replacement = project;
    replacement.kind = RelKind::Values;
    replacement.inputs.clear();
    replacement.keys.clear();
    replacement.valueCount = 1;
    auto &arena = logical.program.queries.at(project.query - 1).expressions;
    for (auto &constant : constants) {
      c.stop();
      c.charge(sizeof(ModelExpression) + constant.value.size());
      replacement.keys.push_back({project.query, static_cast<ExprId>(arena.size())});
      arena.push_back(std::move(constant));
    }
    equivalent(group, std::move(replacement), "AggregateValues", id,
               "空输入上的全局聚合仍输出一行，COUNT 为 0，SUM / MIN / MAX 为带类型 NULL");
  }
  void limitExtremum(GroupId group, RelId id, const RelNode &project) {
    const auto lookup = matchExtremumLookup(logical, project);
    if (!lookup) return;
    const auto scan = node(lookup->scan);
    auto aggregate = node(project.inputs.front());
    auto &context = logical.program.queries.at(project.query - 1);
    auto filter = scan;
    filter.kind = RelKind::Filter;
    filter.table.reset();
    filter.inputs = {lookup->scan};
    const auto input = lookup->argument.expression;
    ModelExpression present;
    present.kind = ExprKind::IsNull;
    present.value = "IS NOT NULL";
    present.type = ValueType::Boolean;
    present.operands = {input};
    present.origin = modelExpression(logical, lookup->argument).origin;
    SqlType type;
    type.family = TypeFamily::Boolean;
    type.nullability = Nullability::NotNull;
    c.charge(sizeof(ModelExpression) + 512);
    present.sqlType = logical.program.types.intern(type);
    filter.predicates = {{project.query, static_cast<ExprId>(context.expressions.size())}};
    context.expressions.push_back(std::move(present));
    auto sort = scan;
    sort.kind = RelKind::Sort;
    sort.table.reset();
    sort.inputs = {make(std::move(filter))};
    sort.keys = {lookup->argument};
    sort.descending = {lookup->descending};
    sort.nullOrdering = {"LAST"};
    auto limit = scan;
    limit.kind = RelKind::Limit;
    limit.table.reset();
    limit.inputs = {make(std::move(sort))};
    limit.rowOffset = 0;
    limit.rowLimit = 1;
    // Keep the final global aggregate: empty/all-NULL input still emits one
    // NULL rather than an empty result. Its argument type and alias stay intact.
    aggregate.inputs = {make(std::move(limit))};
    auto answer = project;
    answer.inputs = {make(std::move(aggregate))};
    equivalent(group, std::move(answer), "AggregateMinMaxToLimit", id,
               "对定宽整数 MIN / MAX 先跳过 NULL，再取有序首行，最终聚合保留空输入的 NULL");
  }
  std::optional<RelNode> unnestInput(const RelNode &relation) {
    if (logical.program.sourceDialect!=Dialect::PostgreSQL || relation.kind!=RelKind::Correlate ||
        relation.join!=JoinKind::Cross || relation.distinct || relation.inputs.size()!=2 ||
        !relation.predicates.empty())
      return {};
    const auto &left=node(relation.inputs.front()),&right=node(relation.inputs.back());
    if (left.query!=relation.query || right.kind!=RelKind::Subquery || !right.correlated ||
        !right.table || right.query!=relation.query || right.inputs.size()!=1 ||
        !orderedJoinOutputs(relation))
      return {};
    const auto &inner=node(right.inputs.front());
    if (inner.kind!=RelKind::Unnest || inner.keys.size()!=1 ||
        inner.outputs.size()!=(inner.withOrdinality?2:1) || right.outputs.size()!=inner.outputs.size() ||
        inner.inputs.size()!=1 || node(inner.inputs.front()).kind!=RelKind::Values ||
        query(inner.query).scope.parent!=relation.query)
      return {};
    const auto ref=inner.keys.front();
    const auto &field=modelExpression(logical,ref);
    if (field.kind!=ExprKind::Name || !field.binding || field.binding->scope!=relation.query ||
        field.localSymbol || field.outputColumn || !field.coalescedBindings.empty())
      return {};
    const auto &owner=query(relation.query);
    const auto &binding=*field.binding;
    if (binding.table>=owner.tables.size() || !owner.tables[binding.table].definition ||
        binding.column>=owner.tables[binding.table].definition->columns.size())
      return {};
    const ColumnRef column{owner.id,binding.table,owner.tables[binding.table].definition->columns[binding.column].name};
    if (std::none_of(left.outputs.begin(),left.outputs.end(),[&](const auto &out) {
          return sameColumn(out,column);
        }))
      return {};
    RelNode expanded;
    expanded.kind=RelKind::Unnest;
    expanded.query=relation.query;
    expanded.origin=relation.origin;
    expanded.inputs={relation.inputs.front()};
    expanded.outputs=relation.outputs;
    expanded.keys=inner.keys;
    expanded.withOrdinality=inner.withOrdinality;
    return expanded;
  }
  void unnestAlternatives(GroupId group,RelId id,const RelNode &n) {
    if (auto expanded=unnestInput(n)) {
      equivalent(group,std::move(*expanded),"UnnestDecorrelate",id,
                 "把关联子查询中的单个整数数组展开合并为左输入上的展开算子");
      return;
    }
    if (!c.options.filterPushdown || n.kind!=RelKind::Filter || n.inputs.size()!=1 ||
        n.predicates.empty())
      return;
    auto expanded=unnestInput(node(n.inputs.front()));
    if (!expanded) return;
    const auto &left=node(expanded->inputs.front());
    for (const auto ref:n.predicates) {
      const auto columns=totalPredicateColumns(logical,ref);
      if (ref.query!=left.query || !columns ||
          std::any_of(columns->begin(),columns->end(),[&](const auto &field) {
            return std::none_of(left.outputs.begin(),left.outputs.end(),[&](const auto &output) {
              return sameColumn(output,field);
            });
          }))
        return;
    }
    auto filter=n;
    filter.inputs=expanded->inputs;
    filter.outputs=left.outputs;
    expanded->inputs={make(std::move(filter))};
    equivalent(group,std::move(*expanded),"FilterTableFunctionTranspose",id,
               "过滤只用数组函数按原身份透传的列，在展开前评估整数条件");
  }
  void sampleAlternatives(GroupId group,RelId id,const RelNode &n) {
    if (logical.program.sourceDialect!=Dialect::PostgreSQL || n.inputs.size()!=1) return;
    const auto deterministic=[](const RelNode &sample) {
      return sample.kind==RelKind::Sample && sample.sampling && sample.inputs.size()==1 &&
          (sample.sampling->milliPercent==0 || sample.sampling->milliPercent==TableSampling::full);
    };
    const auto source=node(n.inputs.front());
    if (deterministic(n) && verifiedGraphScan(source,n.query)) {
      auto replacement=n;
      replacement.kind=RelKind::Filter;
      replacement.sampling.reset();
      replacement.predicates={booleanExpression(n.query,n.origin,0,ExprKind::Boolean,
          n.sampling->milliPercent?"TRUE":"FALSE",{})};
      equivalent(group,std::move(replacement),"SampleToFilter",id,
                 "固定百分比为 0 或 100，原生 BERNOULLI 始终拒绝或保留每一行");
    }
    if (n.kind!=RelKind::Filter || n.predicates.empty() || !deterministic(source) ||
        !verifiedGraphScan(node(source.inputs.front()),n.query))
      return;
    if (std::any_of(n.predicates.begin(),n.predicates.end(),[&](auto ref) {
          return ref.query!=n.query || !totalPredicateColumns(logical,ref);
        }))
      return;
    auto inner=n;
    inner.inputs=source.inputs;
    auto replacement=source;
    replacement.inputs={make(std::move(inner))};
    equivalent(group,std::move(replacement),"FilterSampleTranspose",id,
               "在端点采样前评估无副作用的过滤条件，保留行和列的身份");
  }
  void rules(GroupId group, RelId id) {
    const auto n = node(id);
    scalarCalcAlternatives(group,id,n);
    sampleAlternatives(group,id,n);
    unnestAlternatives(group,id,n);
    groupConstantWithDummyRow(group,id,n);
    if (auto reduced=reduceDecimalPredicates(c,n))
      equivalent(group,std::move(*reduced),"ReduceDecimals",id,
                 "定宽整数可无损转换到指定 numeric 域，比较常量也是整数，使用原生整数比较");
    expandNullSafeConditions(group,id,n);
    pushComputedEquality(group,id,n);
    reorderJoinGraph(group,id,n);
    coerceSetInputs(group, id, n);
    reorderIntersect(group, id, n);
    mergeSets(group, id, n);
    alternateDistinctSets(group,id,n);
    transposeSemiJoinFilter(group,id,n);
    convertUniqueJoinToSemi(group,id,n);
    correlateEquijoin(group,id,n);
    redundantSemiJoin(group,id,n);
    transposeSemiInput(group,id,n);
    distributeJoinOverUnion(group,id,n);
    splitFullJoin(group,id,n);
    splitDisjunctiveJoin(group,id,n);
    inferDisjunctFilters(group,id,n);
    transposeJoinProjection(group,id,n);
    foldValuesUnion(group, id, n);
    shareUnionInputs(group,id,n);
    unionToDistinct(group, id, n);
    pullUpUnionDistinct(group,id,n);
    transposeSortProject(group, id, n);
    copyJoinSort(group,id,n);
    transposeAggregateFilter(group,id,n);
    foldEmptyAggregate(group, id, n);
    foldLiteralDistinct(group, id, n);
    limitExtremum(group, id, n);
    if (c.options.filterPushdown) {
      pushJoinCondition(group, id, n);
      extractJoinFilter(group,id,n);
    }
    if (c.options.filterPushdown && n.kind == RelKind::Filter &&
        n.inputs.size() == 1 && !n.predicates.empty()) {
      bool allKnown = true;
      auto remaining = n;
      remaining.predicates.clear();
      for (auto predicate : n.predicates) {
        const auto value = constantPredicate(predicate);
        allKnown &= value.has_value();
        if (value != 1) remaining.predicates.push_back(predicate);
      }
      if (allKnown && !remaining.predicates.empty()) {
        const auto &child = node(n.inputs[0]);
        const auto *facts = properties(n.inputs[0]);
        if (child.kind == RelKind::Scan ||
            (child.kind == RelKind::Values && facts && !facts->mayHaveEffects)) {
          RelNode empty;
          empty.kind = RelKind::Values;
          empty.query = n.query;
          empty.origin = n.origin;
          empty.outputs = n.outputs;
          empty.valueCount = 0;
          equivalent(group, std::move(empty), "FilterToEmpty", id,
                     "精确常量条件拒绝所有行，可直接使用空结果");
        }
      } else if (remaining.predicates.empty()) {
        auto child = node(n.inputs[0]);
        child.outputs = n.outputs;
        equivalent(group, std::move(child), "FilterRemoveTrue", id,
                   "精确常量条件恒为 TRUE，移除过滤并保留输入关系");
      } else if (remaining.predicates.size() < n.predicates.size()) {
        equivalent(group, std::move(remaining), "FilterRemoveTrueConjunct", id,
                   "移除恒为 TRUE 的条件，其余条件及作用域保留");
      }
    }
    if (c.options.joinCommutation && inner(n) && safe(n.predicates)) {
      auto swapped = n;
      std::swap(swapped.inputs[0], swapped.inputs[1]);
      equivalent(group, std::move(swapped), "JoinCommute", id,
                 "INNER/CROSS join operands commute; stable output column "
                 "identities are retained");
    }
    if (c.options.filterPushdown && n.kind == RelKind::Filter &&
        n.inputs.size() == 1) {
      auto childGroup = intern(n.inputs[0]);
      auto variants = groups[childGroup].alternatives;
      for (auto joinId : variants) {
        const auto join = node(joinId);
        if (!inner(join) || !safe(join.predicates))
          continue;
        const auto leftTables = tables(join.inputs[0]),
                   rightTables = tables(join.inputs[1]);
        std::vector<ExpressionRef> left, right, residual;
        for (auto p : n.predicates) {
          auto refs = referencedColumns(c, p);
          if (movableExpression(c, p) && contained(refs, leftTables))
            left.push_back(p);
          else if (movableExpression(c, p) && contained(refs, rightTables))
            right.push_back(p);
          else
            residual.push_back(p);
        }
        if (left.empty() && right.empty())
          continue;
        auto pushed = join;
        if (!left.empty()) pushed.inputs[0] = filterInput(n, join.inputs[0], std::move(left));
        if (!right.empty()) pushed.inputs[1] = filterInput(n, join.inputs[1], std::move(right));
        pushed.outputs = n.outputs;
        if (residual.empty())
          equivalent(group, std::move(pushed), "FilterJoinPushDown", id,
                     "single-input predicates pushed below an INNER join; no "
                     "outer join or volatile expression rewrite");
        else {
          auto next = make(std::move(pushed));
          auto f = n;
          f.inputs = {next};
          f.predicates = residual;
          equivalent(
              group, std::move(f), "FilterJoinPushDown", id,
              "independent predicates pushed down; cross-input/unsupported "
              "predicates remain above the join");
        }
      }
    }
    if (c.options.joinAssociation && inner(n) && safe(n.predicates)) {
      auto leftGroup = intern(n.inputs[0]);
      auto variants = groups[leftGroup].alternatives;
      for (auto leftId : variants) {
        const auto left = node(leftId);
        if (!inner(left) || !safe(left.predicates))
          continue;
        auto bc = tables(left.inputs[1]);
        auto cs = tables(n.inputs[1]);
        bc.insert(cs.begin(), cs.end());
        std::vector<ExpressionRef> middle, upper;
        auto all = left.predicates;
        all.insert(all.end(), n.predicates.begin(), n.predicates.end());
        for (auto p : all)
          if (contained(referencedColumns(c, p), bc))
            middle.push_back(p);
          else
            upper.push_back(p);
        RelNode joined;
        joined.kind = RelKind::Join;
        joined.query = n.query;
        joined.origin = n.origin;
        joined.join = middle.empty() ? JoinKind::Cross : JoinKind::Inner;
        joined.inputs = {left.inputs[1], n.inputs[1]};
        joined.predicates = middle;
        joined.outputs = node(joined.inputs[0]).outputs;
        const auto extra = node(joined.inputs[1]).outputs;
        joined.outputs.insert(joined.outputs.end(), extra.begin(), extra.end());
        auto mid = make(std::move(joined));
        auto root = n;
        root.inputs = {left.inputs[0], mid};
        root.predicates = upper;
        root.join = upper.empty() ? JoinKind::Cross : JoinKind::Inner;
        equivalent(group, std::move(root), "JoinAssociate", id,
                   "INNER join association with each predicate retained at a "
                   "scope containing its referenced inputs");
      }
    }
  }
  PlanId append(PhysicalNode n) {
    if (physical->nodes.size() >= c.options.maxExpressions * 4)
      throw MemoLimit{};
    if(n.group && n.group<groups.size()) {
      const auto &bounds=groups[n.group].bounds;n.rows=std::max(n.rows,double(bounds.minimum));
      if(bounds.maximum)n.rows=std::min(n.rows,double(*bounds.maximum));
    }
    n.total = n.self;
    std::set<PlanId> materializations;
    for (auto input:n.inputs) {
      const auto &source=physical->nodes.at(input);
      const auto &rel=node(source.logical);
      if (n.kind==PhysicalKind::Append && rel.kind==RelKind::Subquery && rel.materialized &&
          !materializations.insert(input).second) {
        const auto bytes=source.rows*source.rowBytes;
        n.total=plus(n.total,{source.rows*.01,
            bytes>c.options.memoryBytes?bytes/c.options.pageBytes:0.,0.});
      } else {
        n.total=plus(n.total,source.total);
      }
    }
    c.charge(sizeof(n) + n.inputs.size() * sizeof(PlanId) + n.object.size() +
             n.index.size() + n.description.size() +
             n.predicates.size() * sizeof(ExpressionRef) +
             n.ordering.size() * sizeof(ColumnRef) +
             n.scanOrdinals.size() * sizeof(std::size_t));
    physical->nodes.push_back(std::move(n));
    ++physicalCount;
    return PlanId(physical->nodes.size() - 1);
  }
  std::vector<ColumnRef> ordering(const RelNode &n) {
    std::vector<ColumnRef> out;
    for (std::size_t i = 0; i < n.keys.size(); ++i) {
      if ((i < n.descending.size() && n.descending[i]) ||
          (i < n.nullOrdering.size() && !n.nullOrdering[i].empty()))
        return {};
      auto col = resolveColumn(logical, n.keys[i]);
      if (!col)
        return {};
      out.push_back(*col);
    }
    return out;
  }
  bool ordered(const std::vector<ColumnRef> &offered,
               const std::vector<ColumnRef> &required) {
    return !required.empty() && offered.size() >= required.size() &&
           std::equal(required.begin(), required.end(), offered.begin(),
                      sameColumn);
  }
  bool equijoin(const RelNode &n) {
    if (n.inputs.size() != 2)
      return false;
    auto left = tables(n.inputs[0]), right = tables(n.inputs[1]);
    for (auto p : n.predicates) {
      const auto &e = modelExpression(logical, p);
      if (e.kind != ExprKind::Binary || e.value != "=" ||
          e.operands.size() != 2)
        continue;
      auto a = resolveColumn(logical, {p.query, e.operands[0]}),
           b = resolveColumn(logical, {p.query, e.operands[1]});
      if (a && b &&
          ((left.count({a->query, a->table}) &&
            right.count({b->query, b->table})) ||
           (right.count({a->query, a->table}) &&
            left.count({b->query, b->table}))))
        return true;
    }
    return false;
  }
  struct MergeJoinOrder {
    std::array<std::vector<ColumnRef>,2> keys;
    std::size_t outputSide = 0;
  };
  std::optional<MergeJoinOrder> mergeJoinOrder(
      const RelNode &join,const std::vector<ColumnRef> &requested) {
    using compiler::Dialect;
    if (logical.program.sourceDialect!=Dialect::PostgreSQL || join.kind!=RelKind::Join ||
        join.inputs.size()!=2 || join.fixedOrder || join.distinct || join.correlated ||
        join.predicates.empty() || join.predicates.size()>8 ||
        (join.join!=JoinKind::Inner && join.join!=JoinKind::Left && join.join!=JoinKind::Right) ||
        !query(join.query).catalogChecked)
      return {};
    const auto *lhs=properties(join.inputs[0]), *rhs=properties(join.inputs[1]);
    if (!lhs || !rhs || lhs->mayHaveEffects || rhs->mayHaveEffects) return {};
    const auto inOutput=[](const RelNode &n,const ColumnRef &field) {
      return std::count_if(n.outputs.begin(),n.outputs.end(),
          [&](const auto &column){return sameColumn(column,field);})==1;
    };
    const auto &left=node(join.inputs[0]), &right=node(join.inputs[1]);
    std::vector<std::array<ColumnRef,2>> pairs;
    c.charge(join.predicates.size()*(sizeof(pairs.front())+2*sizeof(ColumnRef)+sizeof(std::size_t)+sizeof(bool)));
    for (auto ref:join.predicates) {
      c.stop();
      auto pair=integerEqualityColumns(logical,ref,IntegerEquality::ExactSigned);
      if (ref.query!=join.query || !pair) return {};
      auto &a=pair->first;
      auto &b=pair->second;
      if (inOutput(right,a) && inOutput(left,b)) std::swap(a,b);
      if (!inOutput(left,a) || !inOutput(right,b) || inOutput(right,a) || inOutput(left,b) ||
          !inOutput(join,a) || !inOutput(join,b) ||
          std::any_of(pairs.begin(),pairs.end(),[&](const auto &old) {
            return sameColumn(old[0],a) || sameColumn(old[1],b);
          })) return {};
      pairs.push_back({std::move(a),std::move(b)});
    }
    MergeJoinOrder out;
    out.outputSide=join.join==JoinKind::Right?1:0;
    std::vector<std::size_t> prefix;
    const auto match=[&](std::size_t side) {
      prefix.clear();
      for (const auto &column:requested) {
        const auto found=std::find_if(pairs.begin(),pairs.end(),
            [&](const auto &pair){return sameColumn(pair[side],column);});
        if (found==pairs.end()) return false;
        const auto slot=static_cast<std::size_t>(found-pairs.begin());
        if (std::find(prefix.begin(),prefix.end(),slot)!=prefix.end()) return false;
        prefix.push_back(slot);
      }
      return true;
    };
    if (!match(out.outputSide)) {
      if (join.join==JoinKind::Inner && match(1)) out.outputSide=1;
      else prefix.clear();
    }
    for (std::size_t i=0;i<pairs.size();++i)
      if (std::find(prefix.begin(),prefix.end(),i)==prefix.end()) prefix.push_back(i);
    for (auto slot:prefix) {
      out.keys[0].push_back(std::move(pairs[slot][0]));
      out.keys[1].push_back(std::move(pairs[slot][1]));
    }
    return out;
  }
  PhysicalNode mergeJoin(const RelNode &join,PhysicalNode base,const MergeJoinOrder &order) {
    for (std::size_t side=0;side<order.keys.size();++side)
      base.inputs[side]=solve(intern(join.inputs[side]),order.keys[side]);
    base.kind=PhysicalKind::MergeJoin;
    base.ordering=order.keys[order.outputSide];
    const auto &left=physical->nodes.at(base.inputs[0]), &right=physical->nodes.at(base.inputs[1]);
    const auto outer=order.outputSide;
    const auto inner=1-outer;
    const auto &build=physical->nodes.at(base.inputs[inner]);
    base.buildLeft=inner==0;
    const auto *outerFacts=properties(join.inputs[outer]);
    const auto *innerFacts=properties(join.inputs[inner]);
    double distinct=1;
    for (const auto &key:order.keys[inner]) {
      const auto estimate=ndv(key);
      distinct=std::max(distinct,estimate.first);
      base.assumedStatistics|=estimate.second;
    }
    const auto oneOrKey=[](const RelProperties *facts,const std::vector<ColumnRef> &keys) {
      return facts && ((facts->rows.maximum && *facts->rows.maximum<=1) ||
          coversNonNullUniqueKey(*facts,keys));
    };
    const bool single=oneOrKey(outerFacts,order.keys[outer]);
    const bool unique=oneOrKey(innerFacts,order.keys[inner]);
    const auto runRows=single || unique ? std::min(1.0,build.rows)
        : std::clamp(build.rows/distinct,0.0,build.rows);
    const auto bytes=bounded(left.rowBytes+right.rowBytes+runRows*build.rowBytes);
    base.self.cpu=bounded((left.rows+right.rows)*join.predicates.size()+base.rows);
    base.self.memory=std::min(bytes,c.options.memoryBytes);
    base.self.io=bytes>c.options.memoryBytes?bounded(2*bytes/c.options.pageBytes):0;
    base.description=base.self.io>0
        ?"merge ordered integer keys; estimated duplicate run spills"
        :"merge ordered integer keys; retain only the current duplicate run";
    return base;
  }
  PlanId enforce(PlanId input, const std::vector<ColumnRef> &required) {
    if (required.empty() || ordered(physical->nodes[input].ordering, required))
      return input;
    const auto original = physical->nodes[input];
    PhysicalNode sort;
    sort.kind = PhysicalKind::Sort;
    sort.group = original.group;
    sort.logical = original.logical;
    sort.inputs = {input};
    sort.rows = original.rows;
    sort.rowBytes = original.rowBytes;
    sort.assumedStatistics = original.assumedStatistics;
    sort.ordering = required;
    sort.self.cpu = sort.rows * std::log2(std::max(2.0, sort.rows));
    sort.self.memory = bounded(sort.rows * sort.rowBytes);
    sort.self.io = sort.self.memory > c.options.memoryBytes
                       ? 2 * sort.self.memory / c.options.pageBytes
                       : 0;
    sort.description = "ordering trait enforcer";
    return append(std::move(sort));
  }
  std::optional<std::vector<std::vector<ColumnRef>>>
  mergeUnionOrder(const RelNode &set, const std::vector<ColumnRef> &required) {
    if (logical.program.sourceDialect != Dialect::PostgreSQL || required.empty()) return std::nullopt;
    const auto &owner = query(set.query);
    if (!owner.setOperation || owner.setOperation->recursive || set.inputs.size() < 2)
      return std::nullopt;
    if (set.inputs.size() == 2 && (node(set.inputs[0]).query != owner.setOperation->left ||
                                  node(set.inputs[1]).query != owner.setOperation->right))
      return std::nullopt;
    const auto width = set.outputs.size();
    for (auto arm : set.inputs) {
      const auto *facts = properties(arm);
      const auto &branch = node(arm);
      if (!facts || facts->mayHaveEffects || branch.outputs.size() != width ||
          query(branch.query).projections.size() != width) return std::nullopt;
    }
    const auto unique = [&](const auto &outputs, const ColumnRef &key) {
      return std::count_if(outputs.begin(), outputs.end(),
                           [&](const auto &column) { return sameColumn(column, key); }) == 1;
    };
    std::vector<std::vector<ColumnRef>> result(set.inputs.size());
    for (const auto &key : required) {
      c.stop();
      if (!unique(set.outputs, key)) return std::nullopt;
      const auto position = static_cast<std::size_t>(std::find_if(set.outputs.begin(), set.outputs.end(),
          [&](const auto &column) { return sameColumn(column, key); }) - set.outputs.begin());
      const auto expected = integerOutputType(set, position);
      if (!expected) return std::nullopt;
      for (std::size_t i = 0; i < set.inputs.size(); ++i) {
        c.stop();
        const auto &branch = node(set.inputs[i]);
        const auto &model = query(branch.query);
        const ExpressionRef keyRef{model.id, model.projections[position].expression};
        const auto observed = integerOutputType(branch, position);
        if (!boundScalarColumn(logical, keyRef) || !observed ||
            logical.program.types.get(*expected).bits != logical.program.types.get(*observed).bits ||
            !unique(branch.outputs, branch.outputs[position]))
          return std::nullopt;
        result[i].push_back(branch.outputs[position]);
      }
    }
    return result;
  }
  std::optional<PhysicalNode> nonNullIndexFirst(const RelNode &limit, const RelNode &sort,
                                                     PhysicalNode base) {
    if (logical.program.sourceDialect != Dialect::PostgreSQL ||
        limit.rowLimit != 1 || limit.rowOffset != 0 || sort.keys.size() != 1 ||
        sort.descending.size() != 1) return {};
    const auto &filter = node(sort.inputs.front());
    if (filter.kind != RelKind::Filter || filter.inputs.size() != 1 || filter.predicates.size() != 1)
      return {};
    const auto &scan = node(filter.inputs.front());
    if (scan.kind != RelKind::Scan || !scan.table || scan.query != sort.query) return {};
    const auto &query = modelQuery(logical, scan.query);
    const auto &source = query.tables[*scan.table];
    if (!query.catalogChecked || !source.definition || source.definition->kind != ObjectKind::Table ||
        !(source.inheritance == TableInheritance::Only || source.definition->noDescendantsKnown)) return {};
    const auto predicate = filter.predicates.front();
    const auto &condition = modelExpression(logical, predicate);
    if (condition.kind != ExprKind::IsNull || condition.value != "IS NOT NULL" ||
        condition.operands.size() != 1) return {};
    const auto key = sort.keys.front();
    const auto column = boundScalarColumn(logical, key);
    const auto nonNull = boundScalarColumn(logical, {predicate.query, condition.operands.front()});
    if (!column || !nonNull || !sameColumn(*column, *nonNull) || column->query != scan.query ||
        column->table != *scan.table || !signedIntegerBits(logical.program.types.get(modelExpression(logical, key).sqlType)))
      return {};
    const auto &stats = table(scan.query, *scan.table);
    const IndexStatistics *best = nullptr;
    for (const auto &index : stats.indexes) {
      c.stop();
      if (!index.ordered || index.columns.empty() || index.columns.front() != column->name) continue;
      if (!best || index.pages.value_or(stats.rows) < best->pages.value_or(stats.rows)) best = &index;
    }
    if (!best) return {};
    base.kind = PhysicalKind::IndexScan;
    base.inputs.clear();
    base.predicates = filter.predicates;
    base.object = (stats.schema.empty() ? "" : stats.schema + ".") + stats.name;
    base.index = best->name;
    base.ordering = ordering(sort);
    base.self.cpu = 1 + std::log2(std::max(2., stats.rows));
    base.self.io = 1 + std::ceil(std::log2(std::max(2., best->pages.value_or(stats.rows))) / 6.);
    base.self.memory = base.rowBytes;
    base.description = sort.descending.front()
        ? "backward ordered index seek, skip NULL and read at most one row"
        : "forward ordered index seek, skip NULL and read at most one row";
    return base;
  }
  std::optional<PhysicalNode> projectedScan(const RelNode &project,const PhysicalNode &child,
                                             const PhysicalNode &output) {
    if (project.inputs.size()!=1 || project.keys.empty() || project.keys.size()>256 ||
        project.keys.size()!=project.outputs.size() || !child.inputs.empty() ||
        child.logical!=project.inputs.front() ||
        (child.kind!=PhysicalKind::TableScan && child.kind!=PhysicalKind::IndexScan))
      return {};
    const auto &scan=node(project.inputs.front());
    const auto &q=query(project.query);
    if (scan.kind!=RelKind::Scan || scan.query!=q.id || !q.catalogChecked ||
        !scan.table || *scan.table>=q.tables.size() || project.keys.size()>=scan.outputs.size())
      return {};
    const auto &definition=q.tables[*scan.table].definition;
    if (!definition || definition->kind!=compiler::ObjectKind::Table || !definition->complete ||
        !definition->arityKnown || definition->columns.size()!=scan.outputs.size())
      return {};
    auto result=child;
    result.logical=output.logical;
    result.group=output.group;
    result.ordering=output.ordering;
    result.rowBytes=0;
    const auto &statistics=table(q.id,*scan.table);
    c.charge(project.keys.size()*sizeof(std::size_t));
    result.scanOrdinals.reserve(project.keys.size());
    for (auto ref:project.keys) {
      c.stop();
      const auto field=boundScalarColumn(logical,ref);
      if (ref.query!=q.id || !field || field->table!=scan.table)
        return {};
      const auto ordinal=modelExpression(logical,ref).binding->column;
      if (ordinal>=scan.outputs.size() || !sameColumn(*field,scan.outputs[ordinal]))
        return {};
      result.scanOrdinals.push_back(ordinal);
      const auto info=std::find_if(statistics.columns.begin(),statistics.columns.end(),[&](const auto &col) {
        return col.name==field->name;
      });
      const auto average=info==statistics.columns.end()?std::nullopt:info->averageBytes;
      result.rowBytes+=std::max(1.0,average.value_or(16));
      result.assumedStatistics|=!average.has_value();
    }
    result.rowBytes=std::max(8.0,result.rowBytes);
    result.description+="; project source columns at scan";
    return result;
  }
  PlanId implementation(GroupId group, RelId id,
                        const std::vector<ColumnRef> &required) {
    const auto n = node(id);
    std::vector<PlanId> children;
    for (std::size_t i = 0; i < n.inputs.size(); ++i) {
      std::vector<ColumnRef> childOrder;
      if (i == 0 && n.kind == RelKind::Sort)
        childOrder = ordering(n);
      else if (i == 0 && (n.kind == RelKind::Project ||
                         (n.kind==RelKind::Calc && !n.keys.empty()))) {
        for (const auto &key : required) {
          auto match = std::find_if(
              n.outputs.begin(), n.outputs.end(),
              [&](const auto &out) { return sameColumn(out, key); });
          if (match == n.outputs.end()) {
            childOrder.clear();
            break;
          }
          const auto index = std::size_t(match - n.outputs.begin());
          const bool star =
              std::any_of(n.keys.begin(), n.keys.end(), [&](auto key) {
                return modelExpression(logical, key).kind == ExprKind::Star;
              });
          std::optional<ColumnRef> input;
          if (index < n.keys.size()) {
            const auto &value = modelExpression(logical, n.keys[index]);
            if (value.kind == ExprKind::Name)
              input = resolveColumn(logical, n.keys[index]);
            else if (value.kind == ExprKind::Star)
              input = key;
          } else if (star)
            input = key;
          if (!input) {
            childOrder.clear();
            break;
          }
          childOrder.push_back(*input);
        }
      } else if (i == 0 &&
                 (n.kind == RelKind::Filter || n.kind == RelKind::Limit ||
                  n.kind == RelKind::Coerce || n.kind==RelKind::Calc))
        childOrder = required;
      children.push_back(solve(intern(n.inputs[i]), childOrder));
    }
    auto addPlan = [&](PhysicalNode node) {
      return enforce(append(std::move(node)), required);
    };
    const auto estimate = groups[group].estimate;
    PhysicalNode base;
    base.group = group;
    base.logical = id;
    base.inputs = children;
    base.rows = estimate.rows;
    base.rowBytes = estimate.bytes;
    base.assumedStatistics = estimate.assumed;
    base.predicates = n.predicates;
    const auto inputRows =
        children.empty() ? 0 : physical->nodes[children[0]].rows;
    auto choose = [&](PlanId a, PlanId b) {
      return physical->nodes[b].total.score(c.options.weights) + 1e-9 <
                     physical->nodes[a].total.score(c.options.weights)
                 ? b
                 : a;
    };
    for (std::size_t i = 1; i < children.size(); ++i)
      if (node(n.inputs[i]).scalar && node(n.inputs[i]).correlated) {
        const auto &sub = physical->nodes[children[i]];
        base.self.cpu = bounded(base.self.cpu +
                                sub.total.cpu * std::max(0.0, inputRows - 1));
        base.self.io =
            bounded(base.self.io + sub.total.io * std::max(0.0, inputRows - 1));
      }
    switch (n.kind) {
    case RelKind::Values:
      base.rows=double(n.valueCount);
      base.kind =
          n.valueCount == 0 ? PhysicalKind::Empty : PhysicalKind::Values;
      base.self.cpu = base.rows * .01 + n.keys.size() * .01;
      return addPlan(base);
    case RelKind::TableFunction:
      base.kind = PhysicalKind::TableFunctionScan;
      base.object = "JSON_TABLE";
      base.description = "assumed eight output rows per JSON document; path cardinality is unknown";
      base.self.cpu = base.rows * .5 + 1;
      return addPlan(base);
    case RelKind::WorkingTableScan:
      base.kind = PhysicalKind::WorkingTableScan;
      base.description = "per-iteration working rows estimated from seed shape and cardinality";
      base.self.cpu = base.rows * .03;
      return addPlan(base);
    case RelKind::Scan: {
      base.kind = PhysicalKind::TableScan;
      const auto &t = table(n.query, *n.table);
      base.rows=t.rows;
      base.object = (t.schema.empty() ? "" : t.schema + ".") + t.name;
      base.description =
          std::string(statisticsSourceName(t.source)) + ": " + t.provenance;
      base.self.cpu = base.rows * .1;
      base.self.io = std::max(
          1.0, std::ceil(base.rows * base.rowBytes / c.options.pageBytes));
      auto best = addPlan(base);
      for (const auto &index : t.indexes)
        if (index.ordered && !index.columns.empty()) {
          auto candidate = base;
          candidate.kind = PhysicalKind::IndexScan;
          candidate.index = index.name;
          candidate.ordering.clear();
          for (const auto &col : index.columns)
            candidate.ordering.push_back({n.query, *n.table, col});
          candidate.self.cpu = base.rows * .2;
          candidate.self.io =
              index.pages.value_or(std::max(
                  1.0, std::ceil(base.rows * 24 / c.options.pageBytes))) +
              base.self.io;
          best = choose(best, addPlan(candidate));
        }
      return best;
    }
    case RelKind::Sample:
      base.kind=PhysicalKind::Sample;
      base.rows=bounded(inputRows*n.sampling->milliPercent/TableSampling::full);
      base.self.cpu=inputRows*.05;
      base.description="PostgreSQL BERNOULLI "+tableSamplePercent(n.sampling->milliPercent)+"%";
      return addPlan(base);
    case RelKind::Unnest: {
      const auto literal=postgresIntegerArrayLiteral(logical,n.keys.front(),4096);
      const auto query=postgresIntegerArrayQuery(logical,n);
      const bool direct=literal.status==IntegerArrayStatus::Ready;
      const double elements=direct?literal.elements.size():query?physical->nodes[children.back()].rows:8;
      base.kind=PhysicalKind::Unnest;
      base.rows=bounded(inputRows*elements);
      base.self.cpu=bounded(base.self.cpu+base.rows*.2);
      if (direct)
        base.description="verified PostgreSQL array literal with "+std::to_string(literal.elements.size())+" elements";
      else if (query)
        base.description="PostgreSQL ARRAY query row estimate per input row";
      else
        base.description="assumed eight elements per native PostgreSQL integer array";
      return addPlan(base);
    }
    case RelKind::Series: {
      const auto rows=postgresIntegerSeriesRows(logical,n);
      base.kind=PhysicalKind::Series;
      base.rows=bounded(inputRows*rows.value_or(1000));
      base.self.cpu=bounded(base.self.cpu+base.rows*.2);
      base.description=rows?"verified PostgreSQL integer series with "+std::to_string(*rows)+" rows":
                            "assumed 1,000 rows per integer series with unknown bounds";
      return addPlan(base);
    }
    case RelKind::Match:
      base.kind=PhysicalKind::Match;
      base.rows=std::min(inputRows,base.rows);
      base.self.cpu=bounded(inputRows*(1+std::log2(std::max(1.,inputRows))));
      base.self.memory=std::min(c.options.memoryBytes,inputRows*physical->nodes[children.front()].rowBytes);
      base.description="Oracle ordered greedy single-variable runs; row count assumes an unknown pass rate";
      return addPlan(base);
    case RelKind::Unpivot: {
      const auto &shape=*query(n.query).tables.at(*n.table).unpivot;
      base.kind=PhysicalKind::Unpivot;
      base.rows=bounded(inputRows*unpivotFactor(n,base.assumedStatistics));
      base.self.cpu=bounded(inputRows*shape.inputOrdinals.size()*shape.measures.size()*.2+base.rows*.1);
      base.description=shape.includeNulls.value_or(false)?"UNPIVOT preserves NULL measures":
                                                           "UNPIVOT filters NULL measures";
      return addPlan(base);
    }
    case RelKind::Pivot: {
      const auto &shape=*query(n.query).tables.at(*n.table).pivot;
      base.kind=PhysicalKind::Pivot;
      base.rows=pivotRows(n,inputRows,base.assumedStatistics);
      base.self.cpu=bounded(inputRows*(1.+shape.axisOrdinals.size()*.12+shape.buckets.size()*.2));
      base.self.memory=std::min(c.options.memoryBytes,bounded(base.rows*base.rowBytes));
      base.description=shape.aggregate+" across "+std::to_string(shape.buckets.size())+
          " buckets; "+std::to_string(shape.groupingOrdinals.size())+" implicit group columns";
      return addPlan(base);
    }
    case RelKind::Filter: {
      base.rows=std::min(base.rows,inputRows);
      base.kind = PhysicalKind::Filter;
      base.self.cpu =
          bounded(base.self.cpu +
                  inputRows * std::max<std::size_t>(1, n.predicates.size()));
      base.ordering = physical->nodes[children[0]].ordering;
      auto best = addPlan(base);
      const auto &input=node(n.inputs[0]);
      const auto &physicalInput=physical->nodes[children[0]];
      const bool projected=input.kind==RelKind::Project && input.inputs.size()==1 &&
                           physicalInput.logical==n.inputs[0] && !physicalInput.scanOrdinals.empty();
      const auto &scan=node(projected?input.inputs.front():n.inputs[0]);
      const auto ordinals=projected?physicalInput.scanOrdinals:std::vector<std::size_t>{};
      if (n.inputs.size() == 1 && scan.kind == RelKind::Scan && scan.table) {
        const auto &t = table(scan.query, *scan.table);
        for (const auto &index : t.indexes) {
          if (index.columns.empty())
            continue;
          bool usable = false;
          double accessFraction = 1;
          for (auto p : n.predicates) {
            const auto &e = modelExpression(logical, p);
            if (e.kind != ExprKind::Binary || e.operands.size() != 2 ||
                (e.value != "=" && e.value != "<" && e.value != ">" &&
                 e.value != "<=" && e.value != ">="))
              continue;
            auto a = resolveColumn(logical, {p.query, e.operands[0]}),
                 b = resolveColumn(logical, {p.query, e.operands[1]});
            const auto &other = modelExpression(
                logical, {p.query, a ? e.operands[1] : e.operands[0]});
            auto col = a ? a : b;
            if (col && col->query == scan.query && col->table == *scan.table &&
                col->name == index.columns[0] &&
                (other.kind == ExprKind::Number ||
                 other.kind == ExprKind::String ||
                 other.kind == ExprKind::Parameter)) {
              usable = true;
              accessFraction = std::min(accessFraction, selectivity(p).first);
            }
          }
          if (!usable)
            continue;
          auto candidate = base;
          candidate.kind = PhysicalKind::IndexScan;
          candidate.inputs.clear();
          candidate.scanOrdinals=ordinals;
          candidate.index = index.name;
          candidate.object = (t.schema.empty() ? "" : t.schema + ".") + t.name;
          candidate.description =
              "index access + residual filter; selectivity estimate";
          const double accessRows = t.rows * accessFraction;
          candidate.self.cpu = std::max(1.0, std::log2(t.rows + 1)) +
                               accessRows * (1.2 + n.predicates.size());
          candidate.self.io =
              std::max(1.0, std::log2(std::max(2.0, t.rows)) / 4.0) +
              accessRows * 2;
          candidate.ordering.clear();
          if (index.ordered) {
            for (const auto &col : index.columns) {
              ColumnRef key{scan.query,*scan.table,col};
              if (projected && std::none_of(n.outputs.begin(),n.outputs.end(),[&](const auto &field) {
                    return sameColumn(field,key);
                  }))
                break;
              candidate.ordering.push_back(std::move(key));
            }
          }
          best = choose(best, addPlan(candidate));
        }
      }
      return best;
    }
    case RelKind::Correlate: {
      if (children.size() != 2) c.fail(n.origin, "correlate requires two physical inputs");
      const auto &left = physical->nodes[children[0]];
      const auto &right = physical->nodes[children[1]];
      base.kind = PhysicalKind::CorrelatedNestedLoop;
      base.rows = bounded(left.rows * right.rows);
      for (auto predicate : n.predicates)
        base.rows = bounded(base.rows * selectivity(predicate).first);
      if (n.join == JoinKind::Left) base.rows = std::max(base.rows,left.rows);
      base.assumedStatistics = true;
      const auto repeated = std::max(0.0, left.rows - 1.0);
      base.self.cpu = bounded(base.self.cpu + repeated * right.total.cpu + base.rows * .1);
      base.self.io = bounded(base.self.io + repeated * right.total.io);
      base.ordering = left.ordering;
      base.description = n.join == JoinKind::Left ? "outer lateral join; restart the right input for each left row and null-extend empty results"
                                                : "lateral join; restart the right input for each left row";
      if (n.correlationKeys) {
        const auto keys=correlationColumns(logical,n);
        if (!keys) c.fail(n.origin,"invalid parameterized join key");
        const auto &source=table(keys->second.query,keys->second.table);
        const auto index=std::find_if(source.indexes.begin(),source.indexes.end(),[&](const auto &item) {
          return !item.columns.empty() && item.columns.front()==keys->second.name;
        });
        base.description="parameterized right lookup on "+keys->second.name;
        if (index!=source.indexes.end()) {
          base.self.cpu=bounded(left.rows*std::log2(std::max(2.,right.rows))+base.rows*.1);
          base.self.io=bounded(left.rows*std::max(1.,std::log2(index->pages.value_or(2.))*.2));
          base.description+="; a matching index is available";
        }
      }
      return addPlan(base);
    }
    case RelKind::SemiJoin:
    case RelKind::AntiJoin:
    case RelKind::Join: {
      if (children.size() < 2)
        c.fail(n.origin, "invalid physical join");
      const auto left = physical->nodes[children[0]],
                 right = physical->nodes[children[1]];
      const bool membership = n.kind != RelKind::Join;
      if (membership) {
        base.rows = std::min(base.rows, left.rows);
        if (!right.rows)
          base.rows = n.kind == RelKind::AntiJoin ? left.rows : 0;
        base.kind = n.kind == RelKind::SemiJoin ? PhysicalKind::NestedLoopSemiJoin : PhysicalKind::NestedLoopAntiJoin;
        base.description = "test right-side existence; preserve left duplicates and ignore NULL equality";
      } else if(n.join==JoinKind::Inner || n.join==JoinKind::Cross) {if(left.rows==0 || right.rows==0)base.rows=0;}
      else if(n.join==JoinKind::Left) {if(left.rows==0)base.rows=0;else if(right.rows==0)base.rows=left.rows;}
      else if(n.join==JoinKind::Right) {if(right.rows==0)base.rows=0;else if(left.rows==0)base.rows=right.rows;}
      else if(n.join==JoinKind::Full) {if(left.rows==0)base.rows=right.rows;else if(right.rows==0)base.rows=left.rows;}
      if (!membership) base.kind = PhysicalKind::NestedLoopJoin;
      if (n.fixedOrder) base.description = "source relation preserves the left-to-right evaluation order";
      base.self.cpu = bounded(left.rows * right.rows *
                              std::max<std::size_t>(1, n.predicates.size()));
      base.self.memory =
          std::min(c.options.memoryBytes, right.rows * right.rowBytes);
      if (right.rows * right.rowBytes > c.options.memoryBytes)
        base.self.io =
            bounded(left.rows * std::ceil(right.rows * right.rowBytes /
                                          c.options.pageBytes));
      base.ordering = left.ordering;
      auto best = addPlan(base);
      if (!n.fixedOrder && (membership || equijoin(n))) {
        for (bool buildLeft : {false, true}) {
          if (membership && buildLeft) break;
          auto hash = base;
          hash.kind = n.kind == RelKind::SemiJoin ? PhysicalKind::HashSemiJoin :
                      n.kind == RelKind::AntiJoin ? PhysicalKind::HashAntiJoin : PhysicalKind::HashJoin;
          hash.buildLeft = buildLeft;
          hash.ordering.clear();
          const auto build = buildLeft ? left : right,
                     probe = buildLeft ? right : left;
          const auto bytes = bounded(build.rows * build.rowBytes * 1.3);
          hash.self.memory = bytes;
          hash.self.cpu = bounded(
              (left.rows + right.rows) * 3 +
              base.rows * std::max<std::size_t>(1, n.predicates.size()));
          hash.self.io = 0;
          if (bytes > c.options.memoryBytes) {
            const auto passes =
                1 + std::ceil(std::log2(bytes / c.options.memoryBytes));
            hash.self.io =
                bounded(2 * passes * (bytes + probe.rows * probe.rowBytes) /
                        c.options.pageBytes);
            hash.description = "estimated partition spill; build side exceeds "
                               "the configured memory budget";
          } else
            hash.description =
                buildLeft ? "hash build left" : "hash build right";
          best = choose(best, addPlan(hash));
        }
      }
      if (const auto keys=mergeJoinOrder(n,required))
        best=choose(best,addPlan(mergeJoin(n,base,*keys)));
      return best;
    }
    case RelKind::SetOperation: {
      if (children.size() < 2)
        c.fail(n.origin, "invalid set physical input arity");
      const auto firstRows = physical->nodes[children.front()].rows;
      double input = 0, minRows=firstRows, maxRightRows=0;
      for (std::size_t i=0; i<children.size(); ++i) {
        const auto rows=physical->nodes[children[i]].rows;
        input = bounded(input + rows);
        minRows=std::min(minRows,rows);
        if (i) maxRightRows=std::max(maxRightRows,rows);
      }
      if(n.setKind==SetKind::Union)base.rows=n.distinct?std::min(base.rows,input):input;
      else if(n.setKind==SetKind::Intersect)base.rows=std::min(base.rows,minRows);
      else base.rows=std::min(base.rows,firstRows);
      if (n.setAlgorithm!=SetAlgorithm::Native) {
        if (!n.distinct || n.setKind==SetKind::Union)
          c.fail(n.origin,"alternate set implementation requires a distinct intersection or difference");
        if (n.setAlgorithm==SetAlgorithm::NullSafeMembership) {
          base.kind=n.setKind==SetKind::Intersect?PhysicalKind::HashSemiJoin:PhysicalKind::HashAntiJoin;
          base.description="null-safe membership on all columns followed by distinct";
          base.self.cpu=bounded(input+firstRows*(children.size()-1)*.3+base.rows*.8);
          base.self.memory=bounded((maxRightRows+base.rows)*base.rowBytes*1.3);
        } else if (n.setAlgorithm==SetAlgorithm::CorrelatedExists) {
          base.kind=PhysicalKind::NestedLoopSemiJoin;
          base.description="correlated existence probes on complete null-safe rows followed by distinct";
          double probes=firstRows;
          for (std::size_t i=1; i<children.size(); ++i) {
            const auto rows=physical->nodes[children[i]].rows;
            base.self.cpu=bounded(base.self.cpu+probes*(1+rows*.75));
            probes=std::min(probes,rows)*.5;
          }
          base.self.cpu=bounded(base.self.cpu+firstRows*.8);
          base.self.memory=bounded(base.rows*base.rowBytes*1.3);
          base.assumedStatistics=true;
        } else {
          base.kind=PhysicalKind::HashAggregate;
          base.description="group complete rows and track membership by input";
          const auto groups=std::max({firstRows,maxRightRows,base.rows,std::min(input,std::sqrt(input)*4)});
          base.self.cpu=bounded(input*(1.5+children.size()*.05));
          base.self.memory=bounded(groups*(base.rowBytes+children.size()+16));
        }
        if (base.self.memory>c.options.memoryBytes)
          base.self.io=2*base.self.memory/c.options.pageBytes;
        return addPlan(base);
      }
      base.description =
          std::string(n.setKind == SetKind::Union       ? "UNION"
                      : n.setKind == SetKind::Intersect ? "INTERSECT"
                                                        : "EXCEPT") +
          (n.distinct ? " DISTINCT" : " ALL");
      if (n.setKind == SetKind::Union && !n.distinct) {
        base.kind = PhysicalKind::Append;
        base.self.cpu = input * .01;
        const auto appendPlan = addPlan(base);
        const auto orders = mergeUnionOrder(n, required);
        if (!orders) return appendPlan;
        auto merge = base;
        merge.kind = PhysicalKind::MergeUnion;
        merge.description = "ordered UNION ALL merge using identical integer key types";
        merge.ordering = required;
        for (std::size_t i = 0; i < n.inputs.size(); ++i)
          merge.inputs[i] = solve(intern(n.inputs[i]), (*orders)[i]);
        merge.self.cpu = input * std::max<std::size_t>(1, required.size()) *
            std::log2(static_cast<double>(children.size())) * .1;
        merge.self.memory = children.size() * merge.rowBytes;
        return choose(appendPlan, addPlan(std::move(merge)));
      }
      base.kind = PhysicalKind::HashSet;
      base.self.cpu = input * 2;
      double buildRows = input;
      if (n.setKind == SetKind::Intersect) {
        buildRows = firstRows;
        base.description += "; retain first-input key counts";
      } else if (n.setKind == SetKind::Except) {
        buildRows = maxRightRows;
        base.description += "; subtract right-side key counts in order";
      }
      base.self.memory = bounded(buildRows * base.rowBytes * 1.3);
      if (base.self.memory > c.options.memoryBytes)
        base.self.io = 2 * base.self.memory / c.options.pageBytes;
      auto best = addPlan(base);
      auto sorted = base;
      sorted.kind = PhysicalKind::SortSet;
      sorted.description = "sort set inputs and compare multiplicities";
      sorted.self.cpu = input * std::log2(std::max(2.0, input));
      sorted.self.memory = bounded(input * base.rowBytes);
      sorted.self.io = sorted.self.memory > c.options.memoryBytes
                           ? 2 * sorted.self.memory / c.options.pageBytes
                           : 0;
      return choose(best, addPlan(sorted));
    }
    case RelKind::RepeatUnion: {
      if (children.size() != 2) c.fail(n.origin, "repeat union requires two physical inputs");
      const auto &seed = physical->nodes[children[0]];
      const auto &step = physical->nodes[children[1]];
      base.kind = PhysicalKind::RepeatUnion;
      const auto *seedFacts = properties(n.inputs[0]);
      if (seedFacts && seedFacts->rows.maximum && !*seedFacts->rows.maximum) {
        base.inputs.resize(1);
        base.rows = 0;
        base.assumedStatistics = seed.assumedStatistics;
        base.description = "seed is proved empty; recursive step is not entered";
        return addPlan(base);
      }
      base.assumedIterations = assumedRecursiveIterations;
      base.assumedStatistics = true;
      base.rows = bounded(seed.rows + assumedRecursiveIterations * step.rows);
      const auto repeated = double(assumedRecursiveIterations - 1);
      base.self.cpu = bounded(base.self.cpu + repeated * step.total.cpu +
                              base.rows * (n.distinct ? 1.5 : .1));
      base.self.io = bounded(base.self.io + repeated * step.total.io);
      const auto retainedRows = n.distinct ? base.rows : std::max(seed.rows, step.rows);
      base.self.memory = bounded(retainedRows * base.rowBytes);
      if (base.self.memory > c.options.memoryBytes)
        base.self.io = bounded(base.self.io + 2 * base.self.memory / c.options.pageBytes);
      base.description = n.distinct ? "recursive distinct union with accumulated duplicate tracking"
                                    : "recursive union all with a reusable working table";
      base.description += "; cost assumes " + std::to_string(assumedRecursiveIterations) + " iterations";
      return addPlan(base);
    }
    case RelKind::Aggregate: {
      if(inputRows==0) {
        if(!n.groupingSets.empty())base.rows=double(std::count_if(n.groupingSets.begin(),n.groupingSets.end(),[](const auto &set){return set.empty();}));
        else base.rows=n.keys.empty() && !n.distinct?1:0;
      }
      base.kind = PhysicalKind::HashAggregate;
      base.self.cpu = bounded(inputRows * 2 * std::max<std::size_t>(1,n.groupingSets.size()) + n.groupingSets.size());
      const auto keyCpu = inputRows * .15 * n.keys.size();
      if (n.groupingSets.empty()) base.self.cpu = bounded(base.self.cpu + keyCpu);
      base.self.memory =
          bounded(base.rows * std::max(32.0, base.rowBytes) * 1.5);
      if (base.self.memory > c.options.memoryBytes)
        base.self.io = 2 * base.self.memory / c.options.pageBytes;
      if (!n.groupingSets.empty()) {
        base.description="hash states for "+std::to_string(n.groupingSets.size())+" ordered grouping sets (estimate)";
        base.ordering.clear();
        return addPlan(base); // An ordinary sorted aggregate cannot implement arbitrary sets.
      }
      auto best = addPlan(base);
      auto sorted = base;
      sorted.kind = PhysicalKind::SortAggregate;
      const auto keys = ordering(n);
      bool ready = ordered(physical->nodes[children[0]].ordering, keys);
      const auto &columns = node(n.inputs.front()).outputs;
      if (!ready && !keys.empty() &&
          std::all_of(keys.begin(), keys.end(), [&](const auto &key) {
            return std::count_if(columns.begin(), columns.end(),
                                 [&](const auto &col) { return sameColumn(key, col); }) == 1;
          })) {
        const auto input = solve(intern(n.inputs.front()), keys);
        const auto &orderedInput = physical->nodes[input];
        const auto sortCpu = inputRows * std::log2(std::max(2.0, inputRows));
        const auto sortBytes = bounded(inputRows * base.rowBytes);
        const auto sortIo = sortBytes > c.options.memoryBytes ? 2 * sortBytes / c.options.pageBytes : 0;
        const auto local = plus(physical->nodes[children[0]].total, {sortCpu, sortIo, sortBytes});
        if (orderedInput.total.score(c.options.weights) <= local.score(c.options.weights)) {
          sorted.inputs[0] = input;
          ready = true;
        }
      }
      sorted.self.cpu =
          inputRows + keyCpu +
          (ready ? 0 : inputRows * std::log2(std::max(2.0, inputRows)));
      sorted.self.memory = ready ? base.rowBytes : inputRows * base.rowBytes;
      sorted.self.io = sorted.self.memory > c.options.memoryBytes
                           ? 2 * sorted.self.memory / c.options.pageBytes
                           : 0;
      sorted.ordering = keys;
      return choose(best, addPlan(sorted));
    }
    case RelKind::Project:
    case RelKind::Calc: {
      const bool calc=n.kind==RelKind::Calc;
      base.rows=calc && !n.predicates.empty()?std::min(inputRows,base.rows):inputRows;
      base.kind=calc?PhysicalKind::Calc:PhysicalKind::Project;
      if (calc) {
        base.self.cpu=bounded(inputRows*(.05+n.predicates.size()*.75));
        base.description="scalar predicates and projection in one pass";
        if (n.keys.empty()) {
          base.ordering=physical->nodes[children[0]].ordering;
          return addPlan(base);
        }
      }
      base.self.cpu =
          bounded(base.self.cpu +
                  base.rows * std::max<std::size_t>(1, n.keys.size()) * .1);
      base.ordering.clear();
      for (const auto &input : physical->nodes[children[0]].ordering) {
        bool mapped = false;
        if (std::any_of(n.keys.begin(), n.keys.end(), [&](auto key) {
              return modelExpression(logical, key).kind == ExprKind::Star;
            })) {
          const auto found = std::find_if(
              n.outputs.begin(), n.outputs.end(),
              [&](const auto &column) { return sameColumn(column, input); });
          if (found != n.outputs.end()) {
            base.ordering.push_back(*found);
            mapped = true;
          }
        }
        for (std::size_t i = 0;
             !mapped && i < n.keys.size() && i < n.outputs.size(); ++i) {
          const auto &value = modelExpression(logical, n.keys[i]);
          if (value.kind == ExprKind::Name)
            if (auto column = resolveColumn(logical, n.keys[i]);
                column && sameColumn(*column, input)) {
              base.ordering.push_back(n.outputs[i]);
              mapped = true;
              break;
            }
        }
        if (!mapped)
          break;
      }
      auto projected=calc?std::nullopt:projectedScan(n,physical->nodes[children.front()],base);
      const auto baseline=addPlan(base);
      return projected?choose(baseline,addPlan(std::move(*projected))):baseline;
    }
    case RelKind::Sort: {
      base.rows=inputRows;
      base.kind = PhysicalKind::Sort;
      base.ordering = ordering(n);
      const bool ready =
          ordered(physical->nodes[children[0]].ordering, base.ordering);
      base.self.cpu =
          ready ? 0 : inputRows * std::log2(std::max(2.0, inputRows));
      base.self.memory = ready ? 0 : bounded(inputRows * base.rowBytes);
      base.self.io = base.self.memory > c.options.memoryBytes
                         ? 2 * base.self.memory / c.options.pageBytes
                         : 0;
      if (ready)
        base.description = "required ordering already provided by the input";
      return addPlan(base);
    }
    case RelKind::Limit: {
      if (n.withTies) {
        base.rows = n.rowLimit == 0 ? 0 : std::max(0., inputRows - n.rowOffset.value_or(0));
        base.description = "WITH TIES keeps peers of the last ordered row; peer count is not estimated";
      } else if(n.rowOffset && n.rowLimit)base.rows=std::min(std::max(0.,inputRows-*n.rowOffset),*n.rowLimit);
      else base.rows=std::min(base.rows,inputRows);
      base.kind = PhysicalKind::Limit;
      base.self.cpu = base.rows * .01;
      base.ordering = physical->nodes[children[0]].ordering;
      auto best = addPlan(base);
      const auto sort = node(n.inputs[0]);
      if (!n.withTies && n.rowLimit && n.rowOffset && sort.kind == RelKind::Sort &&
          sort.inputs.size() == 1) {
        auto child = solve(intern(sort.inputs[0]));
        const auto input = physical->nodes[child];
        auto top = base;
        top.kind = PhysicalKind::TopNSort;
        top.inputs = {child};
        const auto capacity =
            std::max(1.0, std::min(input.rows, *n.rowLimit + *n.rowOffset));
        top.self.cpu = input.rows * std::log2(std::max(2.0, capacity));
        top.self.memory = capacity * input.rowBytes;
        top.self.io =
            top.self.memory > c.options.memoryBytes
                ? 2 * input.rows * input.rowBytes / c.options.pageBytes
                : 0;
        top.ordering = ordering(sort);
        top.description = "bounded top-N heap; input still fully inspected";
        best = choose(best, addPlan(top));
        if (auto orderedIndex = nonNullIndexFirst(n, sort, base))
          best = choose(best, addPlan(std::move(*orderedIndex)));
      }
      return best;
    }
    case RelKind::Window: {
      base.rows=inputRows;
      base.kind = PhysicalKind::Window;
      std::size_t ordered=0,functions=0,extrema=0;
      for (const auto &group:n.windowGroups) {
        ordered+=group.ordered;
        functions+=group.functions.size();
        for (const auto ref:group.functions) {
          c.stop();
          const auto &window=modelExpression(logical,ref);
          if (window.operands.empty()) continue;
          const auto *call=&modelExpression(logical,{ref.query,window.operands.front()});
          if (call->kind==ExprKind::AggregateFilter && !call->operands.empty())
            call=&modelExpression(logical,{ref.query,call->operands.front()});
          const auto op=builtinOperator(logical.program.sourceDialect,call->name,call->nameQuoted,call->operands.size());
          if (op && op->kind==RoutineKind::Aggregate && (op->name=="MIN" || op->name=="MAX")) ++extrema;
        }
      }
      const auto passes=std::log2(std::max(2.0,inputRows));
      base.self.cpu =
          bounded(inputRows*(passes*(ordered+extrema)+
              std::max<std::size_t>(1,functions)-n.nonNullWindowSums.size()*.05));
      base.self.memory = bounded(inputRows*(base.rowBytes+(extrema?2*sizeof(std::optional<std::int64_t>):0)));
      base.self.io = base.self.memory > c.options.memoryBytes
                         ? 2 * base.self.memory / c.options.pageBytes
                         : 0;
      base.description = std::to_string(n.windowGroups.size())+" window groups; "+
                         std::to_string(ordered)+" partition/order passes";
      if (extrema) base.description+=", "+std::to_string(extrema)+" frame extrema";
      return addPlan(base);
    }
    case RelKind::Subquery:
      base.kind = PhysicalKind::Subquery;
      base.description = n.materialized ? "explicitly materialized CTE boundary" :
          n.correlated ? "correlated subquery dependency" : "derived/CTE/scalar dependency";
      base.self.cpu = base.rows * .01;
      if (n.materialized) {
        base.self.memory=bounded(base.rows*base.rowBytes);
        if (base.self.memory>c.options.memoryBytes)
          base.self.io=2*base.self.memory/c.options.pageBytes;
      }
      return addPlan(base);
    case RelKind::Coerce:
      base.kind = PhysicalKind::Project;
      base.rows = inputRows;
      base.ordering = physical->nodes[children.front()].ordering;
      base.self.cpu = bounded(inputRows * .02 * n.integerWidenings.size());
      base.description = "exact signed integer widening with preserved input order";
      return addPlan(base);
    case RelKind::Rename:
      base.kind = PhysicalKind::Project;
      base.rows = inputRows;
      base.description = "preserve the original set output columns by position";
      return addPlan(base);
    case RelKind::Modify:
      base.kind = PhysicalKind::Modify;
      base.self.cpu = base.rows * 2;
      base.self.io = base.rows * base.rowBytes / c.options.pageBytes;
      if (modelQuery(logical,n.query).kind==StatementKind::Merge) {
        base.assumedStatistics=true;
        base.description="MERGE candidate rows; ordered branch selectivity, duplicate-match errors and write effects are not modeled";
        return addPlan(base);
      }
      base.description = modelQuery(logical, n.query).replaceExisting
                             ? "REPLACE may delete conflicting rows and insert replacements; conflict and trigger costs are unknown"
                             : "generic row modification estimate; locks, triggers, constraint/index maintenance not modeled";
      return addPlan(base);
    }
    c.fail(n.origin, "unknown logical operator");
  }
  PlanId solve(GroupId group, const std::vector<ColumnRef> &required = {}) {
    c.stop();
    std::ostringstream key;
    for (const auto &col : required)
      key << col.query << ':' << col.table << ':' << col.name.size() << ':'
          << col.name << ';';
    TraitKey trait{group, key.str()};
    if (auto it = winners.find(trait); it != winners.end())
      return it->second;
    c.charge(sizeof(TraitKey) + trait.second.size() + 128);
    if (!solving.insert(trait).second)
      c.fail(node(groups[group].original).origin, "cyclic memo dependency");
    PlanId best = 0;
    auto alternatives = baseline ? std::vector<RelId>{groups[group].original}
                                 : groups[group].alternatives;
    for (auto rel : alternatives) {
      auto candidate = implementation(group, rel, required);
      if (!best ||
          physical->nodes[candidate].total.score(c.options.weights) + 1e-9 <
              physical->nodes[best].total.score(c.options.weights))
        best = candidate;
    }
    solving.erase(trait);
    winners[trait] = best;
    return best;
  }
  void build(PhysicalPlan &target, GroupId root, bool original) {
    physical = &target;
    target.nodes.clear();
    target.nodes.emplace_back();
    winners.clear();
    solving.clear();
    baseline = original;
    target.root = solve(root);
  }

public:
  explicit Memo(PlannerContext &context)
      : c(context), logical(c.result.logical) {}
  void run() {
    GroupId root = 0;
    try {
      root = intern(logical.root);
      build(c.result.baseline, root, true);
      c.result.best = c.result.baseline;
    } catch (const PlannerBudget &) {
      c.fail(node(logical.root).origin,
             "memo budget too small to represent the original query");
    }
    std::set<std::pair<GroupId, RelId>> visited;
    try {
      c.result.heuristicRoot=runHep(c);
      if(c.result.heuristicRoot!=logical.root)
        equivalent(root,node(c.result.heuristicRoot),"HepProgram",logical.root,
                   "按配置顺序运行启发式规则，将已完成的等价结果交给 Volcano 成本选择");
      bool changed = true;
      while (changed) {
        changed = false;
        const auto before = expressionCount;
        for (GroupId group = 1; group < groups.size(); ++group) {
          for (std::size_t i = 0; i < groups[group].alternatives.size(); ++i) {
            auto rel = groups[group].alternatives[i];
            if (!visited.insert({group, rel}).second)
              continue;
            if (++c.result.memo.ruleMatches > c.options.maxRuleMatches)
              throw MemoLimit{};
            c.stop();
            rules(group, rel);
          }
        }
        changed = expressionCount != before;
      }
    } catch (const PlannerBudget &) {
      c.result.searchComplete = false;
      c.result.advice.push_back(
          {AdviceKind::SearchLimit, node(logical.root).origin,
           "本地搜索达到限额",
           "展示已纳入 Memo 的候选中找到的方案，不宣称全局最优。"});
    }
    try {
      build(c.result.best, root, false);
    } catch (const PlannerBudget &) {
      c.result.best = c.result.baseline;
      c.result.searchComplete = false;
      c.result.advice.push_back(
          {AdviceKind::SearchLimit,
           {},
           "物理候选达到限额",
           "回退到完整的基线计划；可缩小查询或提高分析预算。"});
    }
    c.result.memo.groups = groups.size() - 1;
    c.result.memo.logicalExpressions = expressionCount;
    c.result.memo.physicalExpressions = physicalCount;
  }
};
} // namespace
void searchMemo(PlannerContext &context) { Memo(context).run(); }
} // namespace navidog::sql::optimizer
