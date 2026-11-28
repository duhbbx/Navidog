#pragma once
#include "navidog/sql/compiler/compiler.hpp"

namespace navidog::sql::optimizer {

enum class MaterializationSnapshot {
  Unverified,
  // The caller has verified the stored rows reflect the query's snapshot and
  // visibility rules. A populated materialized view alone does not prove this.
  Equivalent
};
enum class MaterializationCoverage { FullOnly, AllowPartial };
enum class MaterializationPattern {
  FilteredScan, ProjectFilter, ProjectJoin, ProjectAggregate, StarAggregate, StarProjectAggregate
};
struct RegisteredMaterialization {
  const compiler::ModelProgram *definition = nullptr;
  const compiler::TableDefinition *storage = nullptr;
  MaterializationSnapshot snapshot = MaterializationSnapshot::Unverified;
};
struct MaterializationCandidate {
  std::size_t registration = 0;
  MaterializationPattern pattern = MaterializationPattern::ProjectFilter;
  bool compensates = false;
  std::string sql;
};
struct MaterializationSearch {
  std::vector<MaterializationCandidate> candidates;
  std::vector<compiler::Diagnostic> diagnostics;
};

// Suggest a PostgreSQL query over a registered materialized view. Both
// programs must be bound with current metadata. The definition is a single
// CREATE MATERIALIZED VIEW and storage describes that exact materialization.
compiler::GeneratedSql rewriteMaterializedView(
    const compiler::ModelProgram &query, const compiler::ModelProgram &definition,
    const compiler::TableDefinition &storage,
    MaterializationSnapshot snapshot = MaterializationSnapshot::Unverified,
    compiler::GenerationOptions = {},
    MaterializationCoverage coverage = MaterializationCoverage::FullOnly);
// Search a bounded registry synchronously. Complete matches precede partial
// matches; within a class the registration order is retained.
MaterializationSearch rewriteMaterializedViews(
    const compiler::ModelProgram &query,const std::vector<RegisteredMaterialization> &,
    compiler::GenerationOptions = {},
    MaterializationCoverage coverage = MaterializationCoverage::FullOnly);
} // namespace navidog::sql::optimizer
