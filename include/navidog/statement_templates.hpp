#pragma once
#include "table_design.hpp"
namespace navidog {
enum class StatementTemplate { Select,Insert,Update,Delete };
// Produces an editable draft; it never runs SQL. Unfilled predicates match no rows.
QString statementTemplate(const ConnectionProfile&,const TableDefinition&,StatementTemplate);
}
