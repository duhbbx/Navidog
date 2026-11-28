#pragma once
#include "sql.hpp"
#include "types.hpp"

namespace navidog {
// Advisory only: never rewrites, splits, suppresses or executes SQL. Caller is
// the query worker, after the existing execution splitter has accepted the script.
SqlExecutionAnalysis analyzeQueryScript(const ConnectionProfile &, const QString &schema,
                                        std::string_view source,
                                        const std::vector<sql::StatementSpan> &statements,
                                        const Cancellation &cancel);
QString queryAnalysisText(const QueryOutcome &outcome);
} // namespace navidog
