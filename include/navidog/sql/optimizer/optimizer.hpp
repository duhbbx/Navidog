#pragma once
#include "navidog/sql/compiler/compiler.hpp"
#include <chrono>
#include <limits>
namespace navidog::sql::optimizer {
using RelId = std::uint32_t;
using GroupId = std::uint32_t;
using PlanId = std::uint32_t;
struct ExpressionRef {
  compiler::QueryId query = 0;
  compiler::ExprId expression = 0;
};
struct ColumnRef {
  compiler::QueryId query = 0;
  std::size_t table = 0;
  std::string name;
};
struct JoinKeyPair {
  ExpressionRef left, right;
};
enum class RelKind {
  Values,
  Scan,
  Filter,
  Join,
  SetOperation,
  Aggregate,
  Project,
  Sort,
  Limit,
  Window,
  Subquery,
  Modify,
  WorkingTableScan,
  RepeatUnion,
  Correlate,
  SemiJoin,
  AntiJoin,
  TableFunction,
  Rename,
  Coerce,
  Calc,
  Sample,
  Unnest,
  Series,
  Match,
  Unpivot,
  Pivot
};
enum class PhysicalKind {
  Values,
  TableScan,
  IndexScan,
  Filter,
  NestedLoopJoin,
  HashJoin,
  Append,
  HashSet,
  SortSet,
  HashAggregate,
  SortAggregate,
  Project,
  Sort,
  TopNSort,
  Limit,
  Window,
  Subquery,
  Modify,
  Empty,
  WorkingTableScan,
  RepeatUnion,
  CorrelatedNestedLoop,
  NestedLoopSemiJoin,
  NestedLoopAntiJoin,
  HashSemiJoin,
  HashAntiJoin,
  TableFunctionScan,
  MergeUnion,
  MergeJoin,
  Calc,
  Sample,
  Unnest,
  Series,
  Match,
  Unpivot,
  Pivot
};
enum class StatisticsSource { Assumed, Manual, ServerCatalog };
struct ColumnStatistics {
  std::string name;
  std::optional<double> distinct, nullFraction, averageBytes;
};
struct IndexStatistics {
  std::string name;
  std::vector<std::string> columns;
  bool unique = false, ordered = true;
  std::optional<double> pages;
};
struct TableStatistics {
  std::string database, schema, name;
  double rows = 0, averageRowBytes = 64;
  StatisticsSource source = StatisticsSource::Manual;
  bool rowBytesAssumed = false;
  std::vector<ColumnStatistics> columns;
  std::vector<IndexStatistics> indexes;
  std::string provenance;
};
// Implementations must use immutable local data; never query a live database
// from inside the planner. Server catalog reads belong to the caller's worker.
class StatisticsProvider {
public:
  virtual ~StatisticsProvider() = default;
  virtual std::optional<TableStatistics>
  table(const compiler::NameResolutionContext &,
        const std::vector<std::string> &) const = 0;
};
class MemoryStatistics final : public StatisticsProvider {
public:
  std::vector<TableStatistics> tables;
  std::optional<TableStatistics>
  table(const compiler::NameResolutionContext &,
        const std::vector<std::string> &) const override;
};
struct IntegerWidening {
  std::size_t ordinal;
  compiler::TypeId source, target;
};
struct WindowGroup {
  bool ordered = false;
  std::vector<ExpressionRef> functions;
};
enum class SetAlgorithm { Native, NullSafeMembership, BranchAggregate, CorrelatedExists };
enum class ScanSource { Unverified, BaseTable, MaterializedView };
struct RelNode {
  RelKind kind = RelKind::Values;
  std::vector<RelId> inputs;
  compiler::QueryId query = 0;
  std::optional<std::size_t> table;
  ScanSource scanSource = ScanSource::Unverified;
  std::optional<compiler::TableSampling> sampling;
  compiler::JoinKind join = compiler::JoinKind::Inner;
  compiler::SetKind setKind = compiler::SetKind::Union;
  SetAlgorithm setAlgorithm = SetAlgorithm::Native;
  std::vector<ExpressionRef> predicates;
  std::vector<ColumnRef> outputs;
  std::vector<ExpressionRef> keys;
  std::vector<IntegerWidening> integerWidenings;
  // Resolved positional types for VALUES manufactured by rewrites.
  std::vector<compiler::TypeId> valueTypes;
  // Explicit ordered grouping sets; duplicates and empty grand totals survive.
  std::vector<std::vector<ExpressionRef>> groupingSets;
  // Windows sharing a verified partition and ordering can reuse one sort.
  std::vector<WindowGroup> windowGroups;
  std::vector<ExpressionRef> nonNullWindowSums;
  compiler::SourceRange origin;
  bool distinct = false, correlated = false, scalar = false;
  bool fixedOrder = false;
  bool materialized = false;
  std::optional<JoinKeyPair> membershipKeys;
  std::optional<JoinKeyPair> correlationKeys;
  std::vector<bool> descending;
  std::vector<std::string> nullOrdering;
  std::optional<double> rowLimit, rowOffset;
  bool withTies = false;
  bool withOrdinality = false;
  std::size_t valueCount = 1;
  compiler::QueryId workingTableOf = 0;
  RelId workingSeed = 0; // Estimation context; never a child of the work-table scan.
};
struct LogicalPlan {
  compiler::ModelProgram program;
  std::vector<RelNode> nodes; // ID 0 reserved, owning DAG.
  RelId root = 0;
};
struct CostWeights {
  double cpu = 1, io = 100, memory = 0;
};
struct Cost {
  double cpu = 0, io = 0, memory = 0;
  double score(const CostWeights &weights = {}) const;
};
struct PhysicalNode {
  PhysicalKind kind = PhysicalKind::Values;
  GroupId group = 0;
  RelId logical = 0;
  std::vector<PlanId> inputs;
  std::vector<ExpressionRef> predicates;
  std::string object, index, description;
  double rows = 0, rowBytes = 0;
  Cost self, total;
  bool assumedStatistics = false, buildLeft = false;
  std::vector<ColumnRef> ordering;
  // When a scan carries a fused projection, native source columns in output order.
  std::vector<std::size_t> scanOrdinals;
  std::optional<std::size_t> assumedIterations;
};
struct PhysicalPlan {
  std::vector<PhysicalNode> nodes;
  PlanId root = 0;
};
struct RuleEvent {
  std::string rule, reason;
  GroupId group = 0;
  RelId before = 0, after = 0;
  bool heuristic = false;
  bool substitution = false;
};
struct MemoSummary {
  std::size_t groups = 0, logicalExpressions = 0, physicalExpressions = 0,
              ruleMatches = 0;
};
struct SharedRelation {
  RelId producer = 0, consumer = 0;
  std::size_t uses = 0;
};
enum class AdviceKind {
  MissingStatistics,
  FullScan,
  CartesianJoin,
  NonSargablePredicate,
  LeadingWildcard,
  StarProjection,
  SortMemory,
  HashMemory,
  CorrelatedSubquery,
  MissingLimit,
  SearchLimit,
  IndexCandidate,
  ConstantPredicate
};
struct Advice {
  AdviceKind kind = AdviceKind::MissingStatistics;
  compiler::SourceRange origin;
  std::string title, detail;
};
enum class HepRule { FilterMerge, ProjectRemove, SortRemove, LimitMerge, DistinctRemove, SortLimitRemove, SortDuplicateKeys, FilterNullability, PruneEmpty, SimplifyOuterJoin, DecorrelateExists, RemoveUnusedOuterJoin, RewriteInSubquery, FilterAggregateTranspose, FilterSetOpTranspose, SortUnionTranspose, FilterProjectTranspose, SortConstantKeys, AggregateDuplicateKeys, AggregateRemove, JoinDeriveNotNull, AggregateCaseToFilter, AggregateMerge, ValuesReduce, FilterSortTranspose, AggregateUnionTranspose, ProjectMerge, AggregateGroupingSetsToUnion, SortJoinTranspose, AggregateProjectPullUpConstants, ProjectJoinTranspose, AggregateReduceFunctions, ProjectSetOpTranspose, UnionPullUpConstants, FilterCorrelate, FilterWindowTranspose, AggregateJoinRemove, ProjectFilterTranspose, ProjectWindowTranspose, ProjectCorrelateTranspose, SetOpToFilter, AggregateReduceFunctionsOnGroupKeys, AggregateExtractProject, AggregateFilterToFilteredAggregate, AggregateProjectMerge, CalcMerge, CalcSplit, CalcRemove, AggregateExpandDistinct, ProjectAggregateMerge, ProjectJoinJoinRemove, AggregateJoinJoinRemove, ProjectOverSumToSum0, MarkToSemiOrAntiJoin, AggregateJoinTranspose, FilterUnpivotTranspose, Count };
enum class HepMatchOrder { BottomUp, TopDown };
struct HepProgram {
  bool enabled=true;
  HepMatchOrder order=HepMatchOrder::BottomUp;
  std::vector<HepRule> rules={HepRule::DecorrelateExists,HepRule::RewriteInSubquery,HepRule::MarkToSemiOrAntiJoin,
      HepRule::SimplifyOuterJoin,HepRule::RemoveUnusedOuterJoin,HepRule::FilterAggregateTranspose,HepRule::FilterCorrelate,HepRule::FilterSetOpTranspose,HepRule::FilterWindowTranspose,HepRule::FilterSortTranspose,HepRule::FilterProjectTranspose,HepRule::FilterNullability,HepRule::FilterMerge,HepRule::ValuesReduce,HepRule::ProjectMerge,HepRule::ProjectRemove,HepRule::ProjectFilterTranspose,HepRule::ProjectWindowTranspose,HepRule::ProjectCorrelateTranspose,HepRule::ProjectJoinTranspose,HepRule::ProjectSetOpTranspose,
      HepRule::FilterUnpivotTranspose,HepRule::SortRemove,HepRule::LimitMerge,HepRule::DistinctRemove,HepRule::SetOpToFilter,HepRule::UnionPullUpConstants,HepRule::SortUnionTranspose,HepRule::SortJoinTranspose,
      HepRule::SortLimitRemove,HepRule::SortDuplicateKeys,HepRule::SortConstantKeys,HepRule::AggregateDuplicateKeys,
      HepRule::AggregateJoinRemove,HepRule::ProjectJoinJoinRemove,HepRule::AggregateJoinJoinRemove,
      HepRule::AggregateRemove,HepRule::AggregateProjectPullUpConstants,HepRule::JoinDeriveNotNull,HepRule::AggregateCaseToFilter,
      HepRule::AggregateMerge,HepRule::AggregateUnionTranspose,HepRule::AggregateGroupingSetsToUnion,
      HepRule::AggregateReduceFunctionsOnGroupKeys,HepRule::AggregateReduceFunctions,HepRule::AggregateFilterToFilteredAggregate,
      HepRule::AggregateProjectMerge,HepRule::AggregateExpandDistinct,HepRule::AggregateExtractProject,
      HepRule::ProjectAggregateMerge,HepRule::AggregateJoinTranspose,HepRule::ProjectOverSumToSum0,HepRule::PruneEmpty};
  // Optional normalizations that reach a fixed point before other rules.
  std::vector<HepRule> substitutions;
  std::size_t maxPasses=8,maxMatches=8192;
};
struct PlannerOptions {
  std::size_t maxGroups = 2048, maxExpressions = 8192, maxRuleMatches = 16384,
              maxNodes = 4096, maxDepth = 64, maxBytes = 64 * 1024 * 1024;
  std::size_t maxTimeMs = 2000;
  double assumedRows = 10000, assumedRowBytes = 64, pageBytes = 8192,
         memoryBytes = 64 * 1024 * 1024;
  CostWeights weights;
  bool filterPushdown = true, joinCommutation = true, joinAssociation = true;
  bool unionMerge = true, alternateSetAlgorithms = true, joinUnionTranspose = true;
  HepProgram heuristic;
  std::function<bool()> shouldStop;
};
struct OptimizationResult {
  LogicalPlan logical;
  PhysicalPlan baseline, best;
  MemoSummary memo;
  CostWeights weights;
  PlannerOptions configuration; // Effective options; never retains a callback.
  std::size_t accountedBytes = 0;
  std::vector<TableStatistics> statistics;
  std::vector<SharedRelation> sharedRelations;
  std::vector<RuleEvent> rules;
  std::vector<Advice> advice;
  std::vector<compiler::Diagnostic> diagnostics;
  double elapsedMs = 0;
  bool searchComplete = true;
  RelId heuristicRoot=0;
  std::size_t heuristicMatches=0,heuristicPasses=0;
  bool heuristicComplete=true;
  bool ok() const { return diagnostics.empty() && best.root != 0; }
};
// SQL is parsed/bound by the caller. This layer consumes only the owning IR and
// local statistics. Cost units are relative, never server timing predictions.
OptimizationResult optimize(const compiler::ModelProgram &,
                            const StatisticsProvider * = nullptr,
                            PlannerOptions = {});
const char *relKindName(RelKind);
const char *physicalKindName(PhysicalKind);
const char *statisticsSourceName(StatisticsSource);
const char *hepRuleName(HepRule);
std::string dumpOptimization(const OptimizationResult &,
                             std::size_t maxBytes = 4 * 1024 * 1024);
} // namespace navidog::sql::optimizer
