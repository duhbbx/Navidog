#include "navidog/sql/optimizer/optimizer.hpp"
#include "navidog/sql/optimizer/interpreter.hpp"
#include "navidog/sql/optimizer/rel_metadata.hpp"
#include <algorithm>
#include <chrono>
#include <iomanip>
#include <locale>
#include <set>
#include <sstream>
#include <stdexcept>
#include <streambuf>
namespace navidog::sql::optimizer {
namespace {
class Buffer : public std::streambuf {
  std::size_t limit;
  std::string value;

protected:
  std::streamsize xsputn(const char *data, std::streamsize count) override {
    if (count < 0 || std::size_t(count) > limit - value.size())
      throw std::length_error("optimization report byte limit exceeded");
    value.append(data, std::size_t(count));
    return count;
  }
  pos_type seekoff(off_type, std::ios_base::seekdir,
                   std::ios_base::openmode) override {
    return pos_type(off_type(-1));
  }
  pos_type seekpos(pos_type, std::ios_base::openmode) override {
    return pos_type(off_type(-1));
  }
  int_type overflow(int_type ch) override {
    if (traits_type::eq_int_type(ch, traits_type::eof()))
      return traits_type::not_eof(ch);
    char c = traits_type::to_char_type(ch);
    xsputn(&c, 1);
    return ch;
  }

public:
  explicit Buffer(std::size_t n) : limit(n) {}
  std::string take() { return std::move(value); }
};
std::string json(std::string_view value) {
  std::string out = "\"";
  static const char hex[] = "0123456789abcdef";
  for (unsigned char c : value) {
    if (c == '"' || c == '\\') {
      out += '\\';
      out += char(c);
    } else if (c < 32) {
      out += "\\u00";
      out += hex[c >> 4];
      out += hex[c & 15];
    } else
      out += char(c);
  }
  return out + '"';
}
void column(std::ostream &out, const ColumnRef &c) {
  out << "{\"query\":" << c.query << ",\"table\":";
  if (c.table == std::size_t(-1))
    out << "null";
  else
    out << c.table;
  out << ",\"name\":" << json(c.name) << '}';
}
void expression(std::ostream &out, ExpressionRef ref) {
  out << "{\"query\":" << ref.query << ",\"expression\":" << ref.expression
      << '}';
}
void logical(std::ostream &out, const LogicalPlan &p) {
  out << "{\"root\":" << p.root << ",\"nodes\":[";
  for (std::size_t i = 1; i < p.nodes.size(); ++i) {
    if (i > 1)
      out << ',';
    const auto &n = p.nodes[i];
    out << "{\"id\":" << i << ",\"operator\":" << json(relKindName(n.kind))
        << ",\"query\":" << n.query << ",\"begin\":" << n.origin.begin
        << ",\"end\":" << n.origin.end << ",\"inputs\":[";
    for (std::size_t j = 0; j < n.inputs.size(); ++j) {
      if (j)
        out << ',';
      out << n.inputs[j];
    }
    out << "],\"predicates\":[";
    for (std::size_t j = 0; j < n.predicates.size(); ++j) {
      if (j)
        out << ',';
      expression(out, n.predicates[j]);
    }
    out << "],\"keys\":[";
    for (std::size_t j = 0; j < n.keys.size(); ++j) {
      if (j)
        out << ',';
      expression(out, n.keys[j]);
    }
    out << "],\"groupingSets\":[";
    for (std::size_t j=0;j<n.groupingSets.size();++j) {
      if(j)out<<',';out<<'[';
      for(std::size_t k=0;k<n.groupingSets[j].size();++k) { if(k)out<<',';expression(out,n.groupingSets[j][k]); }
      out<<']';
    }
    out << "],\"windowGroups\":[";
    for (std::size_t j=0;j<n.windowGroups.size();++j) {
      if (j) out << ',';
      const auto &window=n.windowGroups[j];
      out << "{\"ordered\":" << (window.ordered?"true":"false") << ",\"functions\":[";
      for (std::size_t k=0;k<window.functions.size();++k) {
        if (k) out << ',';
        expression(out,window.functions[k]);
      }
      out << "]}";
    }
    out << "],\"nonNullWindowSums\":[";
    for (std::size_t j=0;j<n.nonNullWindowSums.size();++j) {
      if (j) out << ',';
      expression(out,n.nonNullWindowSums[j]);
    }
    out << "],\"outputs\":[";
    for (std::size_t j = 0; j < n.outputs.size(); ++j) {
      if (j)
        out << ',';
      column(out, n.outputs[j]);
    }
    out << "],\"distinct\":" << (n.distinct ? "true" : "false")
        << ",\"correlated\":" << (n.correlated ? "true" : "false")
        << ",\"scalar\":" << (n.scalar ? "true" : "false")
        << ",\"fixedOrder\":" << (n.fixedOrder ? "true" : "false")
        << ",\"materialized\":" << (n.materialized ? "true" : "false");
    if (n.kind==RelKind::SetOperation && n.setAlgorithm!=SetAlgorithm::Native) {
      const auto algorithm=n.setAlgorithm==SetAlgorithm::CorrelatedExists?"correlatedExists":
          n.setAlgorithm==SetAlgorithm::NullSafeMembership?"nullSafeMembership":"branchAggregate";
      out << ",\"setAlgorithm\":" << json(algorithm);
    }
    if (n.kind==RelKind::Scan) {
      const auto source=n.scanSource==ScanSource::BaseTable?"baseTable":
          n.scanSource==ScanSource::MaterializedView?"materializedView":"unverified";
      out << ",\"scanSource\":" << json(source);
    }
    if (n.sampling) {
      out << ",\"sampling\":{\"method\":\"bernoulli\",\"percent\":"
          << compiler::tableSamplePercent(n.sampling->milliPercent);
      if (n.sampling->repeatable) out << ",\"repeatable\":" << *n.sampling->repeatable;
      out << '}';
    }
    if (n.kind==RelKind::Pivot && n.table && n.query && n.query<=p.program.queries.size()) {
      const auto &scope=p.program.queries[n.query-1];
      if (*n.table<scope.tables.size() && scope.tables[*n.table].pivot) {
        const auto &shape=*scope.tables[*n.table].pivot;
        out << ",\"pivot\":{\"aggregate\":" << json(shape.aggregate) << ",\"groups\":" << shape.groupingOrdinals.size()
            << ",\"buckets\":" << shape.buckets.size() << '}';
      }
    }
    if (n.kind==RelKind::Unpivot && n.table && n.query && n.query<=p.program.queries.size()) {
      const auto &scope=p.program.queries[n.query-1];
      if (*n.table<scope.tables.size() && scope.tables[*n.table].unpivot) {
        const auto &shape=*scope.tables[*n.table].unpivot;
        out << ",\"unpivot\":{\"measures\":" << shape.measures.size()
            << ",\"descriptors\":" << shape.descriptors.size() << ",\"branches\":" << shape.inputs.size()
            << ",\"includeNulls\":" << (shape.includeNulls.value_or(false)?"true":"false") << '}';
      }
    }
    if (n.kind==RelKind::Match && n.table && n.query && n.query<=p.program.queries.size()) {
      const auto &scope=p.program.queries[n.query-1];
      if (*n.table<scope.tables.size() && scope.tables[*n.table].rowMatch) {
        const auto &match=*scope.tables[*n.table].rowMatch;
        out << ",\"rowMatch\":{\"pattern\":" << json(match.variable+"+")
            << ",\"partition\":" << (match.partition?json(*match.partition):"null")
            << ",\"order\":" << json(match.order) << ",\"measure\":" << json(match.measure)
            << ",\"testColumn\":" << json(match.testColumn) << ",\"comparison\":" << json(match.comparison)
            << ",\"threshold\":" << match.threshold << '}';
      }
    }
    if (n.kind==RelKind::Join || n.kind==RelKind::Correlate) {
      using compiler::JoinKind;
      const auto join=n.join==JoinKind::Left?"left":n.join==JoinKind::Right?"right":
                      n.join==JoinKind::Full?"full":n.join==JoinKind::Cross?"cross":"inner";
      out << ",\"joinKind\":" << json(join);
    }
    if (n.kind==RelKind::Modify && n.query && n.query<=p.program.queries.size()) {
      const auto &query=p.program.queries[n.query-1];
      if (query.kind==compiler::StatementKind::Merge) {
        using namespace compiler;
        out << ",\"merge\":{\"on\":" << query.mergeOn << ",\"actions\":[";
        for (std::size_t j=0;j<query.mergeClauses.size();++j) {
          const auto &branch=query.mergeClauses[j];
          if (j) out << ',';
          const auto match=branch.match==MergeMatch::Matched?"matched":
              branch.match==MergeMatch::NotMatchedTarget?"notMatchedTarget":"notMatchedSource";
          const auto action=branch.action==MergeAction::Update?"update":
              branch.action==MergeAction::Insert?"insert":branch.action==MergeAction::Delete?"delete":"nothing";
          out << "{\"match\":" << json(match) << ",\"action\":" << json(action)
              << ",\"condition\":" << branch.condition << ",\"deleteAfterUpdateWhere\":"
              << branch.deleteWhere << '}';
        }
        out << "]}";
      }
    }
    if (!n.valueTypes.empty()) {
      out << ",\"valueTypes\":[";
      for (std::size_t j=0;j<n.valueTypes.size();++j) {
        if (j) out << ',';
        out << n.valueTypes[j];
      }
      out << ']';
    }
    if (!n.integerWidenings.empty()) {
      out << ",\"integerWidenings\":[";
      for (std::size_t j = 0; j < n.integerWidenings.size(); ++j) {
        if (j) out << ',';
        const auto &conversion = n.integerWidenings[j];
        out << "{\"ordinal\":" << conversion.ordinal
            << ",\"fromBits\":" << p.program.types.get(conversion.source).bits.value_or(0)
            << ",\"toBits\":" << p.program.types.get(conversion.target).bits.value_or(0) << '}';
      }
      out << ']';
    }
    if (n.membershipKeys) {
      const auto &key = *n.membershipKeys;
      out << ",\"membershipKeys\":{\"left\":{\"query\":" << key.left.query
          << ",\"expression\":" << key.left.expression << "},\"right\":{\"query\":"
          << key.right.query << ",\"expression\":" << key.right.expression << "}}";
    }
    if (n.table)
      out << ",\"table\":" << *n.table;
    if (n.rowLimit)
      out << ",\"rowLimit\":" << *n.rowLimit;
    if (n.rowOffset)
      out << ",\"rowOffset\":" << *n.rowOffset;
    if (n.withTies)
      out << ",\"withTies\":true";
    if (n.withOrdinality)
      out << (n.kind==RelKind::Unnest?",\"unnestOrdinality\":true":",\"withOrdinality\":true");
    if (n.workingTableOf)
      out << ",\"workingTableOf\":" << n.workingTableOf;
    if (n.workingSeed)
      out << ",\"workingSeed\":" << n.workingSeed;
    out << '}';
  }
  out << "]}";
}
void statistics(std::ostream &out, const std::vector<TableStatistics> &tables) {
  out << '[';
  for (std::size_t i = 0; i < tables.size(); ++i) {
    if (i)
      out << ',';
    const auto &t = tables[i];
    out << "{\"database\":" << json(t.database)
        << ",\"schema\":" << json(t.schema) << ",\"name\":" << json(t.name)
        << ",\"rows\":" << t.rows
        << ",\"averageRowBytes\":" << t.averageRowBytes
        << ",\"source\":" << json(statisticsSourceName(t.source))
        << ",\"rowBytesAssumed\":" << (t.rowBytesAssumed ? "true" : "false")
        << ",\"provenance\":" << json(t.provenance) << ",\"columns\":[";
    for (std::size_t j = 0; j < t.columns.size(); ++j) {
      if (j)
        out << ',';
      const auto &c = t.columns[j];
      out << "{\"name\":" << json(c.name);
      if (c.distinct)
        out << ",\"distinct\":" << *c.distinct;
      if (c.nullFraction)
        out << ",\"nullFraction\":" << *c.nullFraction;
      if (c.averageBytes)
        out << ",\"averageBytes\":" << *c.averageBytes;
      out << '}';
    }
    out << "],\"indexes\":[";
    for (std::size_t j = 0; j < t.indexes.size(); ++j) {
      if (j)
        out << ',';
      const auto &index = t.indexes[j];
      out << "{\"name\":" << json(index.name)
          << ",\"unique\":" << (index.unique ? "true" : "false")
          << ",\"ordered\":" << (index.ordered ? "true" : "false")
          << ",\"columns\":[";
      for (std::size_t k = 0; k < index.columns.size(); ++k) {
        if (k)
          out << ',';
        out << json(index.columns[k]);
      }
      out << ']';
      if (index.pages)
        out << ",\"pages\":" << *index.pages;
      out << '}';
    }
    out << "]}";
  }
  out << ']';
}
void plan(std::ostream &out, const PhysicalPlan &p) {
  out << "{\"root\":" << p.root << ",\"nodes\":[";
  std::set<PlanId> seen;
  std::vector<PlanId> work;
  if (p.root)
    work.push_back(p.root);
  bool comma = false;
  while (!work.empty()) {
    auto id = work.back();
    work.pop_back();
    if (!id || id >= p.nodes.size() || !seen.insert(id).second)
      continue;
    const auto &n = p.nodes[id];
    if (comma)
      out << ',';
    comma = true;
    out << "{\"id\":" << id
        << ",\"operator\":" << json(physicalKindName(n.kind))
        << ",\"group\":" << n.group << ",\"logical\":" << n.logical
        << ",\"object\":" << json(n.object) << ",\"index\":" << json(n.index)
        << ",\"rows\":" << n.rows << ",\"rowBytes\":" << n.rowBytes
        << ",\"assumed\":" << (n.assumedStatistics ? "true" : "false")
        << ",\"description\":" << json(n.description)
        << ",\"self\":{\"cpu\":" << n.self.cpu << ",\"io\":" << n.self.io
        << ",\"memory\":" << n.self.memory
        << "},\"total\":{\"cpu\":" << n.total.cpu << ",\"io\":" << n.total.io
        << ",\"memory\":" << n.total.memory << "},\"inputs\":[";
    for (std::size_t i = 0; i < n.inputs.size(); ++i) {
      if (i)
        out << ',';
      out << n.inputs[i];
    }
    out << "],\"ordering\":[";
    for (std::size_t i = 0; i < n.ordering.size(); ++i) {
      if (i)
        out << ',';
      column(out, n.ordering[i]);
    }
    out << "],\"predicates\":[";
    for (std::size_t i = 0; i < n.predicates.size(); ++i) {
      if (i)
        out << ',';
      expression(out, n.predicates[i]);
    }
    out << ']';
    if (!n.scanOrdinals.empty()) {
      out << ",\"scanOrdinals\":[";
      for (std::size_t i=0;i<n.scanOrdinals.size();++i) {
        if (i) out << ',';
        out << n.scanOrdinals[i];
      }
      out << ']';
    }
    if (n.assumedIterations)
      out << ",\"assumedIterations\":" << *n.assumedIterations;
    out << '}';
    for (auto it = n.inputs.rbegin(); it != n.inputs.rend(); ++it)
      work.push_back(*it);
  }
  out << "]}";
}
void origins(std::ostream &out, const OptimizationResult &result, std::size_t outputBudget) {
  using Clock = std::chrono::steady_clock;
  const auto &logical = result.logical;
  const auto root = logical.root;
  const auto available = root && root < logical.nodes.size() ? logical.nodes[root].outputs.size() : 0;
  const auto deadline = Clock::now() + std::chrono::milliseconds(15);
  const auto &config = result.configuration;
  RelMetadataQuery metadata(logical, {std::min<std::size_t>(config.maxNodes, 2048), config.maxDepth,
      std::min<std::size_t>(outputBudget / 8, 256 * 1024), [&] { return Clock::now() >= deadline; }});
  std::size_t emitted = 0;
  out << "{\"root\":" << root << ",\"columns\":[";
  for (; emitted < std::min<std::size_t>(128, available); ++emitted) {
    const auto *column = metadata.columnOrigins(root, emitted);
    if (!column) break;
    if (emitted) out << ',';
    out << "{\"ordinal\":" << emitted << ",\"complete\":" << (column->complete ? "true" : "false")
        << ",\"sources\":[";
    for (std::size_t i = 0; i < column->origins.size(); ++i) {
      if (i) out << ',';
      const auto &source = column->origins[i];
      out << "{\"database\":" << json(source.database) << ",\"schema\":" << json(source.schema)
          << ",\"table\":" << json(source.table) << ",\"column\":" << json(source.column)
          << ",\"ordinal\":" << source.ordinal << ",\"derived\":" << (source.derived ? "true" : "false") << '}';
    }
    out << "]}";
  }
  out << "],\"truncated\":" << (emitted == available ? "false" : "true") << '}';
}
} // namespace
std::string dumpOptimization(const OptimizationResult &result,
                             std::size_t maxBytes) {
  Buffer buffer(maxBytes);
  std::ostream out(&buffer);
  out.exceptions(std::ios::badbit | std::ios::failbit);
  out.imbue(std::locale::classic());
  out << std::setprecision(17);
  out << "{\"version\":1,\"costUnits\":\"generic relative CPU/page-IO/bytes; "
         "not milliseconds or server COST\",\"searchComplete\":"
      << (result.searchComplete ? "true" : "false")
      << ",\"elapsedMs\":" << result.elapsedMs
      << ",\"accountedBytes\":" << result.accountedBytes
      << ",\"weights\":{\"cpu\":" << result.weights.cpu
      << ",\"io\":" << result.weights.io
      << ",\"memory\":" << result.weights.memory << '}'
      << ",\"memo\":{\"groups\":" << result.memo.groups
      << ",\"logicalExpressions\":" << result.memo.logicalExpressions
      << ",\"physicalExpressions\":" << result.memo.physicalExpressions
      << ",\"ruleMatches\":" << result.memo.ruleMatches << "},\"baseline\":";
  plan(out, result.baseline);
  out << ",\"best\":";
  plan(out, result.best);
  out<<",\"heuristic\":{\"root\":"<<result.heuristicRoot<<",\"matches\":"<<result.heuristicMatches
     <<",\"passes\":"<<result.heuristicPasses<<",\"complete\":"<<(result.heuristicComplete?"true":"false")<<'}';
  out << ",\"logical\":";
  logical(out, result.logical);
  out << ",\"columnOrigins\":";
  origins(out, result, maxBytes);
  out << ",\"statistics\":";
  statistics(out, result.statistics);
  const auto &o = result.configuration;
  out << ",\"configuration\":{\"maxGroups\":" << o.maxGroups
      << ",\"maxExpressions\":" << o.maxExpressions
      << ",\"maxRuleMatches\":" << o.maxRuleMatches
      << ",\"maxNodes\":" << o.maxNodes << ",\"maxDepth\":" << o.maxDepth
      << ",\"maxBytes\":" << o.maxBytes << ",\"maxTimeMs\":" << o.maxTimeMs
      << ",\"assumedRows\":" << o.assumedRows
      << ",\"assumedRowBytes\":" << o.assumedRowBytes
      << ",\"pageBytes\":" << o.pageBytes
      << ",\"memoryBytes\":" << o.memoryBytes
      << ",\"filterPushdown\":" << (o.filterPushdown ? "true" : "false")
      << ",\"joinCommutation\":" << (o.joinCommutation ? "true" : "false")
      << ",\"joinAssociation\":" << (o.joinAssociation ? "true" : "false")
      << ",\"unionMerge\":" << (o.unionMerge ? "true" : "false")
      << ",\"alternateSetAlgorithms\":" << (o.alternateSetAlgorithms ? "true" : "false")
      << ",\"joinUnionTranspose\":" << (o.joinUnionTranspose ? "true" : "false")
      << ",\"hepEnabled\":" << (o.heuristic.enabled?"true":"false")
      << ",\"hepMaxPasses\":" << o.heuristic.maxPasses
      << ",\"hepMaxMatches\":" << o.heuristic.maxMatches
      << ",\"hepOrder\":" << json(o.heuristic.order==HepMatchOrder::BottomUp?"bottomUp":"topDown")
      << ",\"hepRules\":[";
  for(std::size_t i=0;i<o.heuristic.rules.size();++i){if(i)out<<',';out<<json(hepRuleName(o.heuristic.rules[i]));}
  out << "],\"hepSubstitutions\":[";
  for (std::size_t i=0;i<o.heuristic.substitutions.size();++i) {
    if (i) out << ',';
    out << json(hepRuleName(o.heuristic.substitutions[i]));
  }
  out<<"]}";
  out << ",\"rules\":[";
  for (std::size_t i = 0; i < result.rules.size(); ++i) {
    if (i)
      out << ',';
    const auto &e = result.rules[i];
    out << "{\"rule\":" << json(e.rule) << ",\"group\":" << e.group
        << ",\"before\":" << e.before << ",\"after\":" << e.after
        << ",\"phase\":"<<json(e.substitution?"hep-substitution":e.heuristic?"hep":"volcano")
        << ",\"reason\":" << json(e.reason) << "}";
  }
  out << "],\"sharedRelations\":[";
  for (std::size_t i=0;i<result.sharedRelations.size();++i) {
    if (i) out << ',';
    const auto &relation=result.sharedRelations[i];
    out << "{\"producer\":" << relation.producer << ",\"consumer\":" << relation.consumer
        << ",\"uses\":" << relation.uses << "}";
  }
  out << "],\"advice\":[";
  for (std::size_t i = 0; i < result.advice.size(); ++i) {
    if (i)
      out << ',';
    const auto &a = result.advice[i];
    out << "{\"kind\":" << int(a.kind) << ",\"begin\":" << a.origin.begin
        << ",\"end\":" << a.origin.end << ",\"title\":" << json(a.title)
        << ",\"detail\":" << json(a.detail) << "}";
  }
  out << "],\"diagnostics\":[";
  for (std::size_t i = 0; i < result.diagnostics.size(); ++i) {
    if (i)
      out << ',';
    const auto &d = result.diagnostics[i];
    out << "{\"begin\":" << d.range.begin << ",\"end\":" << d.range.end
        << ",\"message\":" << json(d.message) << "}";
  }
  out << "]}";
  return buffer.take();
}
std::string dumpInterpretation(const Interpretation &result) {
  Buffer buffer(64 * 1024 * 1024);
  std::ostream out(&buffer);
  out.exceptions(std::ios::badbit | std::ios::failbit);
  out.imbue(std::locale::classic());
  out << "{\"ok\":" << (result.ok() ? "true" : "false") << ",\"columns\":[";
  for (std::size_t i = 0; i < result.columns.size(); ++i) {
    if (i) out << ',';
    out << json(result.columns[i]);
  }
  out << "],\"rows\":[";
  for (std::size_t i = 0; i < result.rows.size(); ++i) {
    if (i) out << ',';
    out << '[';
    const auto &row = result.rows[i];
    for (std::size_t j = 0; j < row.size(); ++j) {
      if (j) out << ',';
      if (auto integer = std::get_if<std::int64_t>(&row[j])) out << *integer;
      else if (auto boolean = std::get_if<bool>(&row[j])) out << (*boolean ? "true" : "false");
      else if (auto text = std::get_if<LocalText>(&row[j])) out << json(text->utf8());
      else if (auto decimal = std::get_if<LocalNumeric>(&row[j])) out << json(decimal->decimal());
      else out << "null";
    }
    out << ']';
  }
  out << ']';
  if (result.exchange) {
    const auto list=[&](std::string_view label,const auto &values) {
      out << json(label) << ":[";
      for (std::size_t i=0;i<values.size();++i) {
        if (i) out << ',';
        out << values[i];
      }
      out << ']';
    };
    out << ",\"exchange\":{";
    list("keys",result.exchange->keys);
    out << ',';
    list("removedConstantKeys",result.exchange->removedConstantKeys);
    out << ',';
    list("offsets",result.exchange->offsets);
    if (!result.exchange->removedConstantKeys.empty())
      out << ",\"rule\":\"ExchangeRemoveConstantKeys\"";
    out << '}';
  }
  out << ",\"diagnostics\":[";
  for (std::size_t i = 0; i < result.diagnostics.size(); ++i) {
    if (i) out << ',';
    const auto &d = result.diagnostics[i];
    out << "{\"code\":" << int(d.code) << ",\"begin\":" << d.range.begin
        << ",\"end\":" << d.range.end << ",\"message\":" << json(d.message) << '}';
  }
  out << "]}";
  return buffer.take();
}
} // namespace navidog::sql::optimizer
