#pragma once
#include "optimizer.hpp"

namespace navidog::sql::optimizer {
// Serialize a selected logical root to PostgreSQL SQL. Scan bindings must
// come from the destination database's current schema, and the SQL must run
// in the same name-resolution context. Unsupported plans return diagnostics
// with an empty SQL string. The renderer never contacts a database.
compiler::GeneratedSql generatePlanSql(const LogicalPlan &, RelId root,
                                       compiler::GenerationOptions = {});
}
