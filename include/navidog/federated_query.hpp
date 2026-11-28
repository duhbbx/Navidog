#pragma once

#include "database.hpp"
#include "navidog/sql/optimizer/interpreter.hpp"

namespace navidog {
struct FederatedTableBinding {
    std::string source, schema, table;
    DatabaseKind driver;
    Database *session = nullptr;
    QString nativeSchema, nativeTable;
};

// Canonical signed PostgreSQL execution type for a MySQL integer declaration.
std::optional<sql::compiler::SqlType> federatedMySqlInteger(std::string nativeType);

// Run on the worker owning the registered sessions. Bind the ModelProgram
// against an immutable catalog keyed by source / schema / table. Data is
// gathered through read-only projections. Snapshot consistency is per source
// session and can be pinned there by the caller.
sql::optimizer::Interpretation executeFederatedQuery(
    const sql::compiler::ModelProgram &, const std::vector<FederatedTableBinding> &,
    const Cancellation &, sql::optimizer::InterpreterLimits = {});

sql::optimizer::Interpretation executeFederatedQuery(
    const sql::optimizer::PreparedLocalQuery &, const sql::optimizer::LocalRow &parameters,
    const std::vector<FederatedTableBinding> &, const Cancellation &,
    sql::optimizer::InterpreterLimits = {});
}
