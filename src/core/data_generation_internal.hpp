#pragma once
#include "navidog/table_actions.hpp"

namespace navidog::generation {
void generateTable(Database&, const ConnectionProfile&, const TableTask&, const Cancellation&,
                   TableTaskResult&, const GenerationProgress&,
                   const std::function<QString(const QVariant&, DatabaseKind)>& literal);
}
