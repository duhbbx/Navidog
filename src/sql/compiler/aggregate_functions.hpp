#pragma once
#include "navidog/sql/compiler/operator_catalog.hpp"
namespace navidog::sql::compiler {
inline bool builtinAggregate(const ModelExpression &e,Dialect dialect) {
  return callKind(e,dialect)==RoutineKind::Aggregate;
}
} // namespace navidog::sql::compiler
