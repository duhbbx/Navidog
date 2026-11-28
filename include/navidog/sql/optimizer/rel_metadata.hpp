#pragma once
#include "optimizer.hpp"

namespace navidog::sql::optimizer {
// Hard bounds and keys are proofs under the supplied relational/schema model;
// statistical row estimates are deliberately not treated as hard upper bounds.
struct RowBounds {
  std::uint64_t minimum=0;
  std::optional<std::uint64_t> maximum;
};
struct IntegerRange {
  std::int64_t low,high;
};
struct ColumnIntegerDomain {
  ColumnRef column;
  // All output values in this column are non-NULL and belong to these sorted disjoint intervals.
  std::vector<IntegerRange> ranges;
};
struct RelProperties {
  RowBounds rows;
  std::vector<std::vector<ColumnRef>> uniqueKeys;
  std::vector<ColumnRef> nonNullColumns;
  bool mayHaveEffects=false; // Includes unknown effects and observable evaluation errors.
  std::shared_ptr<const std::vector<ColumnIntegerDomain>> integerDomains;
};
bool coversNonNullUniqueKey(const RelProperties &, const std::vector<ColumnRef> &columns);
struct ColumnOrigin {
  std::string database, schema, table, column;
  std::size_t ordinal = 0;
  bool derived = false;
};
struct ColumnLineage {
  // A complete empty result is a value without base column inputs.
  bool complete = false;
  std::vector<ColumnOrigin> origins;
};
enum class MetadataStatus { Ready, ResourceLimit, Stopped, InvalidGraph };
struct MetadataLimits {
  std::size_t maxNodes=4096,maxDepth=64,maxBytes=4*1024*1024;
  std::function<bool()> shouldStop;
};
// Nodes may be appended to the source DAG, but existing nodes/model definitions
// must remain immutable while this cache is alive. No database calls or globals.
class RelMetadataQuery {
  struct Impl;
  std::unique_ptr<Impl> d;
public:
  explicit RelMetadataQuery(const LogicalPlan &,MetadataLimits = {});
  ~RelMetadataQuery();
  RelMetadataQuery(const RelMetadataQuery&)=delete;
  RelMetadataQuery& operator=(const RelMetadataQuery&)=delete;
  const RelProperties *properties(RelId);
  const ColumnLineage *columnOrigins(RelId, std::size_t outputOrdinal);
  MetadataStatus status() const;
  const std::string &message() const;
  std::size_t accountedBytes() const;
  void clear();
};
} // namespace navidog::sql::optimizer
