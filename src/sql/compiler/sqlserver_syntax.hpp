#pragma once

#include "navidog/sql/compiler/compiler.hpp"

namespace navidog::sql::compiler {
inline bool sqlServerDefinitionRequiresBatch(StatementKind kind,ObjectKind object) {
  return (kind==StatementKind::Create && (object==ObjectKind::Schema || object==ObjectKind::View)) ||
         (kind==StatementKind::Alter && object==ObjectKind::View);
}
// ScriptDOM's filterExpression and CheckComparisonOperandForIndexFilter are
// narrower than a normal WHERE expression. Keep the same shape guard at the
// parser and owning-model emitter boundaries. An explicit work list also bounds
// caller-supplied models containing cycles or very long conjunctions.
template<class Lookup>
bool validSqlServerIndexFilter(ExprId root, Lookup lookup) {
  enum class Part { Predicate, Constant, ConvertedConstant };
  std::vector<std::pair<ExprId,Part>> pending{{root,Part::Predicate}};
  std::size_t visits=0;
  while (!pending.empty()) {
    if (++visits>100000) return false;
    const auto [id,part]=pending.back();
    pending.pop_back();
    if (!id) return false;
    const auto &e=lookup(id);
    if (part!=Part::Predicate) {
      if (e.kind==ExprKind::Number || e.kind==ExprKind::String || e.kind==ExprKind::Null)
        continue;
      if (e.kind==ExprKind::Unary && (e.value=="+" || e.value=="-") && e.operands.size()==1) {
        pending.emplace_back(e.operands[0],part);
        continue;
      }
      if (part==Part::Constant) {
        if (e.kind==ExprKind::Cast && e.operands.size()==1) {
          pending.emplace_back(e.operands[0],Part::ConvertedConstant);
          continue;
        }
        if (e.kind==ExprKind::Convert && e.value=="CONVERT" &&
            (e.operands.size()==1 || e.operands.size()==2)) {
          // The target type is structured, so operand zero is the value.
          pending.emplace_back(e.operands[0],Part::ConvertedConstant);
          continue;
        }
      }
      return false;
    }
    if (e.kind==ExprKind::Binary && e.value=="AND" && e.operands.size()==2) {
      for (const auto operand:e.operands) pending.emplace_back(operand,Part::Predicate);
      continue;
    }
    if (e.operands.empty()) return false;
    const auto &column=lookup(e.operands.front());
    if (column.kind!=ExprKind::Name || column.name.empty()) return false;
    if (e.kind==ExprKind::IsNull && e.operands.size()==1 &&
        (e.value=="IS NULL" || e.value=="IS NOT NULL")) continue;
    if (e.kind==ExprKind::Binary && e.operands.size()==2) {
      // SQL Server 2022's DISTINCT predicate permits a general right operand.
      if (e.value=="IS DISTINCT FROM" || e.value=="IS NOT DISTINCT FROM") continue;
      if (e.value=="=" || e.value=="<>" || e.value=="!=" || e.value=="<" ||
          e.value==">" || e.value=="<=" || e.value==">=" || e.value=="!<" || e.value=="!>") {
        pending.emplace_back(e.operands[1],Part::Constant);
        continue;
      }
    }
    if (e.kind==ExprKind::In && e.value=="IN" && !e.query && e.operands.size()>1) {
      for (std::size_t i=1;i<e.operands.size();++i)
        pending.emplace_back(e.operands[i],Part::Constant);
      continue;
    }
    return false;
  }
  return true;
}
} // namespace navidog::sql::compiler
