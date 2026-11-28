#pragma once
#include "navidog/sql/optimizer/optimizer.hpp"
#include <map>
#include <set>
namespace navidog::sql::optimizer {
struct PlannerStopped {};
struct PlannerBudget {};
struct PlannerFailure {
  compiler::Diagnostic diagnostic;
};
struct PlannerContext {
  OptimizationResult &result;
  const StatisticsProvider *statistics;
  const PlannerOptions &options;
  std::chrono::steady_clock::time_point deadline;
  void stop() const;
  void charge(std::size_t);
  void chargeNode(const RelNode &);
  [[noreturn]] void fail(compiler::SourceRange, const std::string &) const;
};
std::string uppercase(std::string);
const compiler::ModelSelect &modelQuery(const LogicalPlan &, compiler::QueryId);
const std::shared_ptr<const compiler::TableDefinition> &sourceDefinition(const compiler::ModelTable &);
const compiler::ModelExpression &modelExpression(const LogicalPlan &,
                                                 ExpressionRef);
std::optional<ColumnRef> resolveColumn(const LogicalPlan &, ExpressionRef);
std::optional<ColumnRef> scopedBoundColumn(const LogicalPlan &,ExpressionRef,compiler::QueryId);
std::optional<ColumnRef> scopedScalarColumn(const LogicalPlan &, ExpressionRef, compiler::QueryId);
std::optional<ColumnRef> boundScalarColumn(const LogicalPlan &, ExpressionRef);
bool sameIntegerType(const LogicalPlan &, ExpressionRef, ExpressionRef);
std::optional<int> signedIntegerBits(const compiler::SqlType &);
std::optional<std::int64_t> exactIntegerLiteral(const LogicalPlan &,ExpressionRef);
enum class IntegerArrayStatus { Ready, Unsupported, ResourceLimit };
struct IntegerArrayLiteral {
  IntegerArrayStatus status = IntegerArrayStatus::Unsupported;
  std::vector<std::optional<std::int64_t>> elements;
  struct Dimension { std::int32_t lower; std::size_t size; };
  std::vector<Dimension> dimensions = {};
  bool nullValue = false;
};
IntegerArrayLiteral postgresIntegerArrayLiteral(const LogicalPlan &, ExpressionRef,
    std::size_t maxElements, const std::function<void(std::size_t)> &chargeWork = {});
struct ArrayQueryInput {
  RelId root;
  bool correlated;
};
std::optional<ArrayQueryInput> postgresIntegerArrayQuery(const LogicalPlan &,const RelNode &);
std::optional<std::uint64_t> integerSeriesCount(std::int64_t start,std::int64_t stop,std::int64_t step);
std::optional<std::uint64_t> postgresIntegerSeriesRows(const LogicalPlan &,const RelNode &);
std::string_view postgresGroupingType(const compiler::SqlType &);
bool isUnionAll(const RelNode &);
struct ExtremumLookup {
  RelId scan;
  ExpressionRef argument;
  std::size_t column;
  bool descending;
};
std::optional<ExtremumLookup> matchExtremumLookup(const LogicalPlan &, const RelNode &);
enum class IntegerEquality { SameType, ExactSigned };
std::optional<std::pair<ColumnRef, ColumnRef>>
integerEqualityColumns(const LogicalPlan &, ExpressionRef, IntegerEquality = IntegerEquality::SameType);
std::optional<std::pair<ColumnRef, ColumnRef>>
integerNullSafeEqualityColumns(const LogicalPlan &,ExpressionRef);
std::optional<JoinKeyPair> correlatedIntegerEqualityKey(const LogicalPlan &, ExpressionRef,
                                                        compiler::QueryId outer);
std::optional<std::pair<ColumnRef, ColumnRef>> membershipColumns(const LogicalPlan &, const RelNode &);
std::optional<std::pair<ColumnRef, ColumnRef>> correlationColumns(const LogicalPlan &, const RelNode &);
std::vector<ColumnRef> referencedColumns(PlannerContext &, ExpressionRef);
std::optional<std::vector<ColumnRef>> totalPredicateColumns(const LogicalPlan &, ExpressionRef);
bool movableExpression(PlannerContext &, ExpressionRef);
// Exact SQL truth for a bounded, relation-independent scalar. -1 is UNKNOWN.
std::optional<int> evaluateConstantTruth(PlannerContext &, ExpressionRef);
std::optional<std::vector<std::size_t>> evaluateLiteralFilter(
    PlannerContext &, const RelNode &filter, const RelNode &input, const RelNode &values);
using JoinMask = std::uint16_t;
constexpr std::size_t maxJoinSearchInputs = 6;
struct JoinSearchInput {
  double rows, rowBytes;
};
struct JoinSearchTerm {
  ExpressionRef expression;
  JoinMask inputs;
  double selectivity;
};
struct JoinOrder {
  // Internal entries store the left subset. A zero entry is a scan.
  std::vector<JoinMask> left;
};
JoinOrder searchJoinOrder(PlannerContext &, const std::vector<JoinSearchInput> &,
                          const std::vector<JoinSearchTerm> &, bool leftDeep);
std::optional<RelNode> asScalarCalc(PlannerContext &, const RelNode &);
std::optional<RelNode> mergeScalarCalcs(PlannerContext &, const RelNode &, const RelNode &);
std::optional<RelNode> reduceDecimalPredicates(PlannerContext &,const RelNode &);
bool isIdentityCalc(const LogicalPlan &, const RelNode &);
void buildLogical(PlannerContext &);
void lowerLogical(PlannerContext &, const compiler::ModelProgram &);
RelId runHep(PlannerContext &);
void searchMemo(PlannerContext &);
void inspectAdvice(PlannerContext &);
} // namespace navidog::sql::optimizer
