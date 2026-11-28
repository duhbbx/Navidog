#include "optimizer_internal.hpp"
#include "navidog/sql/optimizer/interpreter.hpp"

#include <algorithm>

namespace navidog::sql::optimizer {
namespace {
bool same(const ColumnRef &a,const ColumnRef &b) {
  return a.query==b.query && a.table==b.table && a.name==b.name;
}

bool sameOutputs(const RelNode &a,const RelNode &b) {
  return a.outputs.size()==b.outputs.size() &&
      std::equal(a.outputs.begin(),a.outputs.end(),b.outputs.begin(),same);
}

bool contains(const RelNode &source,const ColumnRef &field) {
  return std::count_if(source.outputs.begin(),source.outputs.end(),[&](const auto &col) {
    return same(col,field);
  })==1;
}

std::optional<std::size_t> projectedField(const RelNode &calc,const ColumnRef &field) {
  const auto position=std::find_if(calc.outputs.begin(),calc.outputs.end(),
      [&](const auto &entry){return same(entry,field);});
  if (position==calc.outputs.end() || !contains(calc,field)) return {};
  return static_cast<std::size_t>(position-calc.outputs.begin());
}
} // namespace

std::optional<RelNode> asScalarCalc(PlannerContext &context,const RelNode &relation) {
  using namespace compiler;
  const auto &plan=context.result.logical;
  if (plan.program.sourceDialect!=Dialect::PostgreSQL || relation.inputs.size()!=1 ||
      relation.outputs.empty() || relation.outputs.size()>128 || relation.predicates.size()>64 ||
      (relation.kind!=RelKind::Filter && relation.kind!=RelKind::Project && relation.kind!=RelKind::Calc))
    return {};
  const auto &input=plan.nodes.at(relation.inputs.front());
  const auto &query=modelQuery(plan,relation.query);
  if (input.query!=relation.query || !query.catalogChecked || query.kind!=StatementKind::Select ||
      (input.kind!=RelKind::Scan && input.kind!=RelKind::Filter &&
       input.kind!=RelKind::Project && input.kind!=RelKind::Calc))
    return {};
  if (input.kind==RelKind::Project && input.inputs.size()==1) {
    const auto &below=plan.nodes.at(input.inputs.front());
    if (below.kind==RelKind::Window || below.kind==RelKind::Aggregate) return {};
  }
  if (relation.keys.empty()) {
    if (relation.kind==RelKind::Project || !sameOutputs(relation,input)) return {};
  } else if (relation.keys.size()!=relation.outputs.size() || relation.kind==RelKind::Filter) {
    return {};
  }
  for (auto key:relation.keys) {
    context.stop();
    const auto field=boundScalarColumn(plan,key);
    if (!field || key.query!=input.query || !contains(input,*field)) return {};
  }
  for (auto condition:relation.predicates) {
    context.stop();
    if (condition.query!=input.query) return {};
    const auto fields=totalPredicateColumns(plan,condition);
    if (!fields || std::any_of(fields->begin(),fields->end(),[&](const auto &col) {
          return !contains(input,col);
        }))
      return {};
  }
  auto calc=relation;
  calc.kind=RelKind::Calc;
  return calc;
}

std::optional<RelNode> mergeScalarCalcs(PlannerContext &context,const RelNode &upper,
                                      const RelNode &lower) {
  const auto &plan=context.result.logical;
  if (upper.kind!=RelKind::Calc || lower.kind!=RelKind::Calc || lower.inputs.size()!=1 ||
      upper.query!=lower.query || upper.predicates.size()+lower.predicates.size()>64)
    return {};
  auto calc=upper;
  const auto preservesField=[&](const ColumnRef &field) {
    const auto position=projectedField(lower,field);
    if (!position) return false;
    if (lower.keys.empty()) return true;
    const auto source=boundScalarColumn(plan,lower.keys.at(*position));
    return source && same(*source,field);
  };
  // The upper predicate stays in its original bindings. Only direct fields
  // that the lower projection leaves unchanged may cross that projection.
  for (auto condition:upper.predicates) {
    context.stop();
    const auto fields=totalPredicateColumns(plan,condition);
    if (!fields || !std::all_of(fields->begin(),fields->end(),preservesField)) return {};
  }
  if (calc.keys.empty()) {
    calc.keys=lower.keys;
  } else if (!lower.keys.empty()) {
    for (auto &ref:calc.keys) {
      context.stop();
      const auto field=boundScalarColumn(plan,ref);
      if (!field) return {};
      const auto position=projectedField(lower,*field);
      if (!position) return {};
      const auto input=lower.keys.at(*position);
      if (!(plan.program.types.get(modelExpression(plan,input).sqlType)==
            plan.program.types.get(modelExpression(plan,ref).sqlType)))
        return {};
      ref=input;
    }
  }
  calc.inputs=lower.inputs;
  calc.predicates=lower.predicates;
  calc.predicates.insert(calc.predicates.end(),upper.predicates.begin(),upper.predicates.end());
  return calc;
}

bool isIdentityCalc(const LogicalPlan &plan,const RelNode &relation) {
  if (relation.kind!=RelKind::Calc || relation.inputs.size()!=1 || !relation.predicates.empty()) return false;
  const auto &input=plan.nodes.at(relation.inputs.front());
  if (!sameOutputs(relation,input)) return false;
  if (relation.keys.empty()) return true;
  if (relation.keys.size()!=input.outputs.size()) return false;
  for (std::size_t i=0;i<relation.keys.size();++i) {
    const auto field=boundScalarColumn(plan,relation.keys[i]);
    if (!field || !same(*field,input.outputs[i])) return false;
  }
  return true;
}

std::optional<RelNode> reduceDecimalPredicates(PlannerContext &context,const RelNode &original) {
  using namespace compiler;
  auto &plan=context.result.logical;
  if (plan.program.sourceDialect!=Dialect::PostgreSQL || original.inputs.size()!=1 ||
      original.predicates.empty() || original.predicates.size()>64 ||
      (original.kind!=RelKind::Filter && original.kind!=RelKind::Calc))
    return {};
  auto &scope=plan.program.queries.at(original.query-1);
  if (!scope.catalogChecked || scope.expressions.size()+2*original.predicates.size()>context.options.maxNodes)
    return {};
  const auto &input=plan.nodes.at(original.inputs.front());
  const auto integerCast=[&](ExpressionRef ref)->std::optional<ExprId> {
    const auto &cast=modelExpression(plan,ref);
    if (cast.kind!=ExprKind::Cast || !cast.castType || cast.operands.size()!=1) return {};
    const auto &output=plan.program.types.get(cast.sqlType);
    if (output.family!=TypeFamily::Decimal || output.arrayDimensions || !output.opaqueName.empty() ||
        output.sqliteAffinity || (output.scale && *output.scale!=0))
      return {};
    const ExpressionRef arg{ref.query,cast.operands.front()};
    const auto field=boundScalarColumn(plan,arg);
    const auto bits=signedIntegerBits(plan.program.types.get(modelExpression(plan,arg).sqlType));
    if (!field || !bits || !contains(input,*field)) return {};
    const auto required=*bits==64?19:*bits==32?10:5;
    if (output.precision && *output.precision<required) return {};
    return arg.expression;
  };
  const auto fixedInteger=[&](ExpressionRef ref)->std::optional<std::string> {
    const auto &expr=modelExpression(plan,ref);
    if (expr.kind!=ExprKind::Number || expr.value.size()>56) return {};
    const auto decimal=LocalNumeric::parse(expr.value);
    if (!decimal) return {};
    auto value=decimal->decimal();
    const auto dot=value.find('.');
    if (dot!=std::string::npos) {
      if (value.find_first_not_of('0',dot+1)!=std::string::npos) return {};
      value.resize(dot);
    }
    // Keep the literal in the native int4 range, rather than silently
    // rationalizing a comparison that would still need numeric coercion.
    if (value.empty() || value.size()>9 || value.find_first_not_of("0123456789")!=std::string::npos)
      return {};
    return value;
  };
  const auto add=[&](ModelExpression expression) {
    context.charge(sizeof(expression)+expression.value.size()+expression.operands.size()*sizeof(ExprId));
    const auto id=static_cast<ExprId>(scope.expressions.size());
    scope.expressions.push_back(std::move(expression));
    return id;
  };
  auto reduced=original;
  bool changed=false;
  for (auto &ref:reduced.predicates) {
    context.stop();
    auto predicate=modelExpression(plan,ref);
    if (ref.query!=scope.id || predicate.kind!=ExprKind::Binary || predicate.operands.size()!=2 ||
        (predicate.value!="=" && predicate.value!="<>" && predicate.value!="!=" &&
         predicate.value!="<" && predicate.value!=">" && predicate.value!="<=" && predicate.value!=">="))
      continue;
    const ExpressionRef left{ref.query,predicate.operands[0]},right{ref.query,predicate.operands[1]};
    const auto a=integerCast(left),b=integerCast(right);
    if (!a && !b) continue;
    if (a && b) {
      if (!sameIntegerType(plan,{ref.query,*a},{ref.query,*b})) continue;
      predicate.operands={*a,*b};
    } else {
      const auto literalRef=a?right:left;
      const auto integral=fixedInteger(literalRef);
      if (!integral) continue;
      auto literal=modelExpression(plan,literalRef);
      literal.type=ValueType::Integer;
      literal.value=*integral;
      SqlType integer;
      integer.family=TypeFamily::Integer;
      integer.bits=32;
      integer.unsignedInteger=false;
      integer.nullability=Nullability::NotNull;
      const auto before=plan.program.types.accountedBytes();
      literal.sqlType=plan.program.types.intern(std::move(integer));
      context.charge(plan.program.types.accountedBytes()-before);
      const auto id=add(std::move(literal));
      predicate.operands=a?std::vector<ExprId>{*a,id}:std::vector<ExprId>{id,*b};
    }
    ref.expression=add(std::move(predicate));
    changed=true;
  }
  return changed?std::optional<RelNode>{std::move(reduced)}:std::nullopt;
}
} // namespace navidog::sql::optimizer
