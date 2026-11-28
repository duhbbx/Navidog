#pragma once
#include "compiler.hpp"

namespace navidog::sql::compiler {
// Immutable, dialect-aware builtin signatures. This is intentionally separate
// from the catalog of deployed routines and from parser keyword recognition.
struct OperatorInfo {
  std::string name;
  RoutineKind kind = RoutineKind::Unknown;
  FunctionVolatility volatility = FunctionVolatility::Unknown;
  std::size_t minArguments = 0, maxArguments = 0;
  ValueType fixedResult = ValueType::Unknown;
  bool resultFromFirst = false, commonResult = false;
  bool strict = false, mayThrow = true;
  bool requiresWithinGroup = false;
};
// Partial native aggregate signature/result facts. Unknown attributes stay
// unknown; descriptor equality alone is not cross-engine execution equivalence.
TypeResolution resolveAggregateType(Dialect, std::string_view, const SqlType &);
std::optional<OperatorInfo>
builtinOperator(Dialect, const std::vector<std::string> &,
                const std::vector<bool> &quoted, std::size_t arguments);
std::vector<OperatorInfo> builtinOperators(Dialect);
// Known native grammar restrictions; true does not establish that a name is a
// builtin or that an arbitrary aggregate/window invocation is otherwise valid.
bool builtinAcceptsDistinct(Dialect,std::string_view);
bool builtinAcceptsDistinctAll(Dialect,std::string_view);
bool nativeFunctionNeedsAdjacentParen(Dialect,std::string_view);
RoutineKind callKind(const ModelExpression &, Dialect);
FunctionVolatility callVolatility(const ModelExpression &, Dialect);
const char *routineKindName(RoutineKind);
const char *volatilityName(FunctionVolatility);
} // namespace navidog::sql::compiler
