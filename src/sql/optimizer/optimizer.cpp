#include "optimizer_internal.hpp"
#include "navidog/sql/compiler/operator_catalog.hpp"
#include <algorithm>
#include <charconv>
#include <cmath>
#include <stdexcept>
#include <tuple>
namespace navidog::sql::optimizer {
std::string uppercase(std::string value) {
  for (auto &c : value)
    if (c >= 'a' && c <= 'z')
      c -= 32;
  return value;
}
std::optional<TableStatistics>
MemoryStatistics::table(const compiler::NameResolutionContext &context,
                        const std::vector<std::string> &name) const {
  if (name.empty() || name.size() > 3)
    return {};
  auto database = context.database, schema = context.schema;
  if (name.size() == 3) {
    database = name[0];
    schema = name[1];
  } else if (name.size() == 2) {
    if (compiler::isMySqlDialect(context.dialect))
      database = name[0];
    else
      schema = name[0];
  }
  if ((compiler::isMySqlDialect(context.dialect)) &&
      schema.empty())
    schema = database;
  for (const auto &t : tables)
    if (t.database == database && t.schema == schema && t.name == name.back())
      return t;
  return {};
}
double Cost::score(const CostWeights &weights) const {
  const auto score =
      cpu * weights.cpu + io * weights.io + memory * weights.memory;
  return std::isfinite(score) ? std::min(1e100, score) : 1e100;
}
void PlannerContext::charge(std::size_t bytes) {
  // Conservative accounting includes vector capacity and associative-node
  // overhead.
  if (bytes > (options.maxBytes - result.accountedBytes) / 2)
    throw PlannerBudget{};
  result.accountedBytes += bytes * 2;
}
void PlannerContext::chargeNode(const RelNode &n) {
  if (n.outputs.size() > 4096 || n.keys.size() > 4096 ||
      n.predicates.size() > 4096 || n.integerWidenings.size() > 4096 ||
      n.valueTypes.size()>4096 || n.nonNullWindowSums.size()>4096)
    throw PlannerBudget{};
  std::size_t bytes =
      sizeof(n) + n.inputs.size() * sizeof(RelId) +
      (n.keys.size() + n.predicates.size() + n.nonNullWindowSums.size()) * sizeof(ExpressionRef) +
      n.outputs.size() * sizeof(ColumnRef) + n.integerWidenings.size() * sizeof(IntegerWidening) +
      n.valueTypes.size()*sizeof(compiler::TypeId);
  if (n.groupingSets.size() > 256) throw PlannerBudget{};
  std::size_t refs=0;
  for (const auto &set : n.groupingSets) {
    if (set.size()>8192-refs) throw PlannerBudget{};
    refs+=set.size(); bytes+=sizeof(set)+set.size()*sizeof(ExpressionRef);
  }
  if (n.windowGroups.size()>256) throw PlannerBudget{};
  for (const auto &window:n.windowGroups) {
    if (window.functions.size()>8192-refs) throw PlannerBudget{};
    refs+=window.functions.size();
    bytes+=sizeof(window)+window.functions.size()*sizeof(ExpressionRef);
  }
  for (const auto &column : n.outputs)
    bytes += column.name.size();
  for (const auto &nulls : n.nullOrdering)
    bytes += sizeof(nulls) + nulls.size();
  charge(bytes);
}
void PlannerContext::stop() const {
  if (options.shouldStop && options.shouldStop())
    throw PlannerStopped{};
  if (std::chrono::steady_clock::now() >= deadline)
    throw PlannerBudget{};
}
void PlannerContext::fail(compiler::SourceRange origin,
                          const std::string &message) const {
  throw PlannerFailure{
      {compiler::DiagnosticCode::UnsupportedSyntax, origin, message}};
}
const std::shared_ptr<const compiler::TableDefinition> &sourceDefinition(const compiler::ModelTable &table) {
  if (table.rowMatch) return table.matchInput;
  if (table.pivot) return table.pivotInput;
  if (table.unpivot) return table.unpivotInput;
  return table.definition;
}
const compiler::ModelSelect &modelQuery(const LogicalPlan &plan,
                                        compiler::QueryId id) {
  if (!id || id > plan.program.queries.size())
    throw std::out_of_range("invalid optimizer query reference");
  return plan.program.queries[id - 1];
}
const compiler::ModelExpression &modelExpression(const LogicalPlan &plan,
                                                 ExpressionRef ref) {
  const auto &q = modelQuery(plan, ref.query);
  if (!ref.expression || ref.expression >= q.expressions.size())
    throw std::out_of_range("invalid optimizer expression reference");
  return q.expressions[ref.expression];
}
static std::optional<ColumnRef> resolveColumnImpl(const LogicalPlan &plan,
                                                  ExpressionRef ref,
                                                  std::size_t depth) {
  if (depth > 64)
    return {};
  const auto &q = modelQuery(plan, ref.query);
  const auto &e = modelExpression(plan, ref);
  if (e.localSymbol)
    return {};
  if (e.outputColumn && *e.outputColumn < q.projections.size()) {
    const auto &projection = q.projections[*e.outputColumn];
    const auto &value = q.expressions.at(projection.expression);
    auto col =
        resolveColumnImpl(plan, {q.id, projection.expression}, depth + 1);
    ColumnRef output =
        col.value_or(ColumnRef{q.id, std::size_t(-1), "expression"});
    if (!projection.alias.empty())
      output.name = projection.alias;
    else if (!value.name.empty())
      output.name = value.name.back();
    return output;
  }
  if (e.kind != compiler::ExprKind::Name)
    return {};
  if (e.binding) {
    const auto &owner = modelQuery(plan, e.binding->scope);
    if (e.binding->table < owner.tables.size()) {
      const auto &t = owner.tables[e.binding->table];
      if (t.definition && e.binding->column < t.definition->columns.size())
        return ColumnRef{owner.id, e.binding->table,
                         t.definition->columns[e.binding->column].name};
    }
  }
  if (e.name.size() > 1)
    for (std::size_t i = 0; i < q.tables.size(); ++i) {
      const auto &t = q.tables[i];
      if (t.alias == e.name[e.name.size() - 2] ||
          (t.alias.empty() && !t.name.empty() &&
           t.name.back() == e.name[e.name.size() - 2]))
        return ColumnRef{q.id, i, e.name.back()};
    }
  if (e.name.size() == 1 && q.tables.size() == 1)
    return ColumnRef{q.id, 0, e.name[0]};
  return {};
}
std::optional<ColumnRef> resolveColumn(const LogicalPlan &plan,
                                       ExpressionRef ref) {
  return resolveColumnImpl(plan, ref, 0);
}
std::optional<ColumnRef> scopedBoundColumn(const LogicalPlan &plan,
                                          ExpressionRef ref,compiler::QueryId scope) {
  const auto &expression = modelExpression(plan, ref);
  if (expression.kind != compiler::ExprKind::Name || expression.localSymbol ||
      expression.outputColumn || !expression.binding ||
      !expression.coalescedBindings.empty() || expression.binding->scope != scope)
    return {};
  const auto &query = modelQuery(plan, scope);
  const auto &binding = *expression.binding;
  if (binding.table >= query.tables.size()) return {};
  const auto &definition = query.tables[binding.table].definition;
  if (!definition || binding.column >= definition->columns.size()) return {};
  return ColumnRef{scope, binding.table, definition->columns[binding.column].name};
}
std::optional<ColumnRef> scopedScalarColumn(const LogicalPlan &plan,
                                           ExpressionRef ref,compiler::QueryId scope) {
  using compiler::TypeFamily;
  const auto &type=plan.program.types.get(modelExpression(plan,ref).sqlType);
  if (type.family==TypeFamily::Unknown || type.family==TypeFamily::Null ||
      type.arrayDimensions || type.sqliteAffinity || !type.opaqueName.empty())
    return {};
  return scopedBoundColumn(plan,ref,scope);
}
std::optional<ColumnRef> boundScalarColumn(const LogicalPlan &plan,
                                          ExpressionRef ref) {
  return scopedScalarColumn(plan, ref, ref.query);
}
bool sameIntegerType(const LogicalPlan &plan, ExpressionRef left, ExpressionRef right) {
  const auto &leftType = plan.program.types.get(modelExpression(plan, left).sqlType);
  const auto &rightType = plan.program.types.get(modelExpression(plan, right).sqlType);
  using compiler::TypeFamily;
  return leftType.family == TypeFamily::Integer && rightType.family == TypeFamily::Integer &&
         leftType.bits && leftType.bits == rightType.bits && leftType.unsignedInteger &&
         leftType.unsignedInteger == rightType.unsignedInteger && leftType.flavor == rightType.flavor;
}
std::optional<int> signedIntegerBits(const compiler::SqlType &type) {
  if (type.family == compiler::TypeFamily::Integer && type.unsignedInteger == false &&
      (type.bits == 16 || type.bits == 32 || type.bits == 64) && type.flavor.empty() &&
      type.opaqueName.empty() && !type.arrayDimensions && !type.sqliteAffinity)
    return type.bits;
  return {};
}
namespace {
constexpr std::string_view arrayWhitespace=" \t\n\r\f\v";
void skipArrayWhitespace(std::string_view &text) {
  const auto first=text.find_first_not_of(arrayWhitespace);
  text.remove_prefix(std::min(first,text.size()));
}
bool takeArrayToken(std::string_view &text,char token) {
  skipArrayWhitespace(text);
  if (text.empty() || text.front()!=token) return false;
  text.remove_prefix(1);
  return true;
}
std::optional<std::int64_t> arrayInteger(std::string_view &text) {
  skipArrayWhitespace(text);
  if (!text.empty() && text.front()=='+') {
    text.remove_prefix(1);
    if (text.empty() || text.front()<'0' || text.front()>'9') return {};
  }
  if (text.empty()) return {};
  std::int64_t value=0;
  const auto result=std::from_chars(text.data(),text.data()+text.size(),value);
  if (result.ec!=std::errc{}) return {};
  text.remove_prefix(static_cast<std::size_t>(result.ptr-text.data()));
  return value;
}
bool inArrayIntegerDomain(std::int64_t value,int bits) {
  if (bits==64) return true;
  const auto bound=std::int64_t{1}<<(bits-1);
  return value>=-bound && value<bound;
}
bool arrayNull(std::string_view text) {
  if (text.size()!=4) return false;
  constexpr std::string_view name="NULL";
  for (std::size_t i=0;i<name.size();++i)
    if (text[i]!=name[i] && text[i]!=name[i]+('a'-'A')) return false;
  return true;
}
} // namespace
IntegerArrayLiteral postgresIntegerArrayLiteral(const LogicalPlan &plan,ExpressionRef reference,
      std::size_t maxElements,const std::function<void(std::size_t)> &chargeWork) {
  using namespace compiler;
  if (plan.program.sourceDialect!=Dialect::PostgreSQL) return {};
  const auto &root=modelExpression(plan,reference);
  const bool cast=root.kind==ExprKind::Cast && root.operands.size()==1;
  auto type=plan.program.types.get(root.sqlType);
  if ((!cast && root.kind!=ExprKind::ArrayConstructor) || type.arrayDimensions<1) return {};
  type.arrayDimensions=0;
  const auto bits=signedIntegerBits(type);
  if (!bits) return {};
  const auto &argument=cast?modelExpression(plan,{reference.query,root.operands.front()}):root;
  if (argument.kind==ExprKind::ArrayConstructor) {
    IntegerArrayLiteral literal{IntegerArrayStatus::Ready,{}};
    const auto append=[&](const ModelExpression &element) {
      if (literal.elements.size()==maxElements) {
        literal.status=IntegerArrayStatus::ResourceLimit;
        return false;
      }
      if (element.kind==ExprKind::Null) {
        literal.elements.push_back({});
        return true;
      }
      const auto *number=&element;
      if (element.kind==ExprKind::Unary && element.operands.size()==1 &&
          (element.value=="+" || element.value=="-"))
        number=&modelExpression(plan,{reference.query,element.operands.front()});
      if (number->kind!=ExprKind::Number || number->value.size()>32 ||
          number->value.find_first_not_of("0123456789")!=std::string::npos) return false;
      const auto signedValue=number==&element?number->value:element.value+number->value;
      std::string_view digits=signedValue;
      const auto value=arrayInteger(digits);
      if (!value || !digits.empty() || !inArrayIntegerDomain(*value,*bits)) return false;
      literal.elements.push_back(*value);
      return true;
    };
    using Shape=std::vector<std::size_t>;
    std::size_t visited=0;
    const auto flatten=[&](const auto &self,const ModelExpression &array,unsigned depth)->std::optional<Shape> {
      if (depth>=postgresArrayMaxDimensions || array.query) return {};
      if (++visited>8192) {
        literal.status=IntegerArrayStatus::ResourceLimit;
        return {};
      }
      if (chargeWork) chargeWork(1);
      if (array.operands.empty()) return Shape{0};
      const auto &first=modelExpression(plan,{reference.query,array.operands.front()});
      const bool nested=first.kind==ExprKind::ArrayConstructor;
      std::optional<Shape> extent;
      for (const auto id:array.operands) {
        const auto &element=modelExpression(plan,{reference.query,id});
        if (chargeWork) chargeWork(1);
        if ((element.kind==ExprKind::ArrayConstructor)!=nested) return {};
        if (nested) {
          auto shape=self(self,element,depth+1);
          if (!shape || (extent && *shape!=*extent)) return {};
          if (!extent) extent=std::move(shape);
        } else if (!append(element)) return {};
      }
      if (extent && *extent==Shape{0}) return extent;
      Shape result{array.operands.size()};
      if (extent) result.insert(result.end(),extent->begin(),extent->end());
      return result;
    };
    const auto shape=flatten(flatten,argument,0);
    if (!shape)
      return {literal.status==IntegerArrayStatus::ResourceLimit?literal.status:IntegerArrayStatus::Unsupported,{}};
    if (!literal.elements.empty())
      for (const auto size:*shape) literal.dimensions.push_back({1,size});
    return literal;
  }
  if (argument.kind==ExprKind::Null) return {IntegerArrayStatus::Ready,{},{},true};
  if (argument.kind!=ExprKind::String) return {};
  std::string_view text=argument.value;
  if (text.size()>64*1024) return {IntegerArrayStatus::ResourceLimit,{}};
  if (chargeWork) chargeWork(text.size()+1);
  std::optional<std::size_t> extent;
  std::int32_t lower=1;
  if (takeArrayToken(text,'[')) {
    const auto low=arrayInteger(text);
    if (!low || !inArrayIntegerDomain(*low,32) || !takeArrayToken(text,':')) return {};
    const auto high=arrayInteger(text);
    if (!high || !inArrayIntegerDomain(*high,32) || *high<*low ||
        !takeArrayToken(text,']') || !takeArrayToken(text,'=')) return {};
    const auto count=static_cast<std::uint64_t>(*high-*low)+1;
    if (count>maxElements) return {IntegerArrayStatus::ResourceLimit,{}};
    extent=static_cast<std::size_t>(count);
    lower=static_cast<std::int32_t>(*low);
  }
  if (!takeArrayToken(text,'{')) return {};
  IntegerArrayLiteral literal{IntegerArrayStatus::Ready,{}};
  if (!takeArrayToken(text,'}')) {
    do {
      skipArrayWhitespace(text);
      if (chargeWork) chargeWork(1);
      if (literal.elements.size()==maxElements) return {IntegerArrayStatus::ResourceLimit,{}};
      const bool quoted=takeArrayToken(text,'"');
      const auto end=quoted?text.find('"'):text.find_first_of(",}");
      if (end==std::string_view::npos) return {};
      auto element=text.substr(0,end);
      const auto last=element.find_last_not_of(arrayWhitespace);
      if (last==std::string_view::npos) return {};
      element=element.substr(0,last+1);
      text.remove_prefix(end+static_cast<std::size_t>(quoted));
      if (!quoted && arrayNull(element)) literal.elements.push_back({});
      else {
        const auto value=arrayInteger(element);
        if (!value || !element.empty() || !inArrayIntegerDomain(*value,*bits)) return {};
        literal.elements.push_back(*value);
      }
    } while (takeArrayToken(text,','));
    if (!takeArrayToken(text,'}')) return {};
  }
  skipArrayWhitespace(text);
  if (!text.empty() || (extent && *extent!=literal.elements.size())) return {};
  if (!literal.elements.empty()) literal.dimensions.push_back({lower,literal.elements.size()});
  return literal;
}
std::optional<ArrayQueryInput> postgresIntegerArrayQuery(const LogicalPlan &plan,const RelNode &node) {
  using namespace compiler;
  if (plan.program.sourceDialect!=Dialect::PostgreSQL || node.kind!=RelKind::Unnest ||
      node.keys.size()!=1 || node.inputs.size()!=2) return {};
  const auto ref=node.keys.front();
  const auto &value=modelExpression(plan,ref);
  if (value.kind!=ExprKind::ArrayConstructor || !value.query) return {};
  const auto &dependency=plan.nodes.at(node.inputs.back());
  if (dependency.kind!=RelKind::Subquery || !dependency.scalar || dependency.query!=ref.query ||
      dependency.inputs.size()!=1) return {};
  const auto root=dependency.inputs.front();
  const auto &input=plan.nodes.at(root);
  if (input.query!=value.query || input.outputs.size()!=1) return {};
  const auto &query=modelQuery(plan,value.query);
  if (query.projections.size()!=1 ||
      !signedIntegerBits(plan.program.types.get(query.expressions.at(query.projections.front().expression).sqlType)))
    return {};
  return ArrayQueryInput{root,dependency.correlated};
}
std::optional<std::int64_t> exactIntegerLiteral(const LogicalPlan &plan,ExpressionRef ref) {
  using compiler::ExprKind;
  const auto &expression=modelExpression(plan,ref);
  const auto *number=&expression;
  bool negative=false;
  if (expression.kind==ExprKind::Unary && expression.operands.size()==1 &&
      (expression.value=="+" || expression.value=="-")) {
    negative=expression.value=="-";
    number=&modelExpression(plan,{ref.query,expression.operands.front()});
  }
  if (number->kind!=ExprKind::Number || number->value.empty() || number->value.size()>32 ||
      number->value.find_first_not_of("0123456789")!=std::string::npos)
    return {};
  std::uint64_t magnitude=0;
  const auto end=number->value.data()+number->value.size();
  const auto converted=std::from_chars(number->value.data(),end,magnitude);
  if (converted.ec!=std::errc{} || converted.ptr!=end) return {};
  if (negative && magnitude==std::uint64_t(INT64_MAX)+1) return INT64_MIN;
  if (magnitude>std::uint64_t(INT64_MAX)) return {};
  return negative?-static_cast<std::int64_t>(magnitude):static_cast<std::int64_t>(magnitude);
}
std::optional<std::uint64_t> integerSeriesCount(std::int64_t start,std::int64_t stop,std::int64_t step) {
  if (!step) return {};
  if ((step>0 && start>stop) || (step<0 && start<stop)) return 0;
  const auto distance=step>0?std::uint64_t(stop)-std::uint64_t(start):std::uint64_t(start)-std::uint64_t(stop);
  // Unsigned subtraction handles the full distance between signed extremes
  // and the magnitude of INT64_MIN without overflowing signed arithmetic.
  const auto stride=step>0?std::uint64_t(step):0-std::uint64_t(step);
  const auto intervals=distance/stride;
  if (intervals==UINT64_MAX) return {};
  return intervals+1;
}
std::optional<std::uint64_t> postgresIntegerSeriesRows(const LogicalPlan &plan,const RelNode &node) {
  using compiler::ExprKind;
  if (plan.program.sourceDialect!=compiler::Dialect::PostgreSQL || node.kind!=RelKind::Series ||
      node.keys.size()<2 || node.keys.size()>3) return {};
  std::int64_t values[]{0,0,1};
  for (std::size_t i=0;i<node.keys.size();++i) {
    auto ref=node.keys[i];
    const auto &argument=modelExpression(plan,ref);
    const auto bits=signedIntegerBits(plan.program.types.get(argument.sqlType));
    if (argument.kind==ExprKind::Cast && bits && argument.operands.size()==1)
      ref.expression=argument.operands.front();
    if (modelExpression(plan,ref).kind==ExprKind::Null) return 0; // The native function is strict.
    const auto value=exactIntegerLiteral(plan,ref);
    if (!value || !bits || !inArrayIntegerDomain(*value,*bits)) return {};
    values[i]=*value;
  }
  return integerSeriesCount(values[0],values[1],values[2]);
}
std::string_view postgresGroupingType(const compiler::SqlType &type) {
  if (const auto bits = signedIntegerBits(type))
    return *bits == 16 ? "smallint" : *bits == 32 ? "integer" : "bigint";
  if (type.family == compiler::TypeFamily::Boolean && !type.arrayDimensions &&
      !type.sqliteAffinity && type.opaqueName.empty() && type.flavor.empty())
    return "boolean";
  return {};
}
std::optional<ExtremumLookup> matchExtremumLookup(const LogicalPlan &plan, const RelNode &project) {
  using namespace compiler;
  if (plan.program.sourceDialect != Dialect::PostgreSQL || project.kind != RelKind::Project ||
      project.inputs.size() != 1 || project.keys.size() != 1 || project.outputs.size() != 1)
    return {};
  const auto &query = modelQuery(plan, project.query);
  const auto &aggregate = plan.nodes.at(project.inputs.front());
  if (!query.catalogChecked || aggregate.kind != RelKind::Aggregate || aggregate.distinct ||
      aggregate.query != project.query || !aggregate.keys.empty() || !aggregate.groupingSets.empty() ||
      aggregate.inputs.size() != 1) return {};
  const auto scanId = aggregate.inputs.front();
  const auto &scan = plan.nodes.at(scanId);
  if (scan.kind != RelKind::Scan || !scan.table || scan.query != project.query ||
      *scan.table >= query.tables.size()) return {};
  const auto &table = query.tables[*scan.table].definition;
  if (!table || table->kind != ObjectKind::Table || !table->complete || !table->arityKnown) return {};
  const auto key = project.keys.front();
  const auto &call = modelExpression(plan, key);
  if (key.query != project.query || call.kind != ExprKind::Call || call.distinct || call.window ||
      call.operands.size() != 1 || call.name.empty()) return {};
  const auto builtin = builtinOperator(plan.program.sourceDialect, call.name, call.nameQuoted, 1);
  if (!builtin || builtin->kind != RoutineKind::Aggregate ||
      (call.routineResolved && (call.routineSchema != "pg_catalog" ||
       call.routineKind != RoutineKind::Aggregate || call.routineVolatility != FunctionVolatility::Immutable)))
    return {};
  const auto name = uppercase(call.name.back());
  if (name != "MIN" && name != "MAX") return {};
  const ExpressionRef argument{key.query, call.operands.front()};
  const auto &value = modelExpression(plan, argument);
  if (!boundScalarColumn(plan, argument) || !value.binding || value.binding->table != *scan.table ||
      value.binding->column >= scan.outputs.size() ||
      scan.outputs[value.binding->column].name != table->columns[value.binding->column].name ||
      !signedIntegerBits(plan.program.types.get(value.sqlType)) || !sameIntegerType(plan, key, argument)) return {};
  return ExtremumLookup{scanId, argument, value.binding->column, name == "MAX"};
}
bool isUnionAll(const RelNode &rel) {
  return rel.kind == RelKind::SetOperation && rel.setKind == compiler::SetKind::Union && !rel.distinct;
}
static std::optional<JoinKeyPair> integerEqualityOperands(
    const LogicalPlan &plan, ExpressionRef ref, IntegerEquality comparison = IntegerEquality::SameType) {
  const auto &condition = modelExpression(plan, ref);
  if (condition.kind != compiler::ExprKind::Binary || condition.value != "=" ||
      condition.operands.size() != 2)
    return {};
  const JoinKeyPair keys{{ref.query, condition.operands[0]}, {ref.query, condition.operands[1]}};
  if (comparison == IntegerEquality::SameType)
    return sameIntegerType(plan, keys.left, keys.right) ? std::optional<JoinKeyPair>(keys) : std::nullopt;
  const auto exact = [&](ExpressionRef key) {
    return signedIntegerBits(plan.program.types.get(modelExpression(plan, key).sqlType)).has_value();
  };
  if (!exact(keys.left) || !exact(keys.right)) return {};
  return keys;
}
std::optional<std::pair<ColumnRef, ColumnRef>>
integerEqualityColumns(const LogicalPlan &plan, ExpressionRef ref, IntegerEquality comparison) {
  const auto operands = integerEqualityOperands(plan, ref, comparison);
  if (!operands) return {};
  auto a = boundScalarColumn(plan, operands->left),
       b = boundScalarColumn(plan, operands->right);
  if (!a || !b) return {};
  return std::make_pair(std::move(*a), std::move(*b));
}
std::optional<std::pair<ColumnRef,ColumnRef>>
integerNullSafeEqualityColumns(const LogicalPlan &plan,ExpressionRef ref) {
  if (plan.program.sourceDialect!=compiler::Dialect::PostgreSQL) return {};
  const auto &condition=modelExpression(plan,ref);
  if (condition.kind!=compiler::ExprKind::Binary || condition.value!="IS NOT DISTINCT FROM" ||
      condition.operands.size()!=2) return {};
  const ExpressionRef first{ref.query,condition.operands[0]},second{ref.query,condition.operands[1]};
  if (!sameIntegerType(plan,first,second)) return {};
  auto a=boundScalarColumn(plan,first),b=boundScalarColumn(plan,second);
  if (!a || !b) return {};
  return std::make_pair(std::move(*a),std::move(*b));
}
std::optional<std::vector<ColumnRef>> totalPredicateColumns(const LogicalPlan &plan,
                                                               ExpressionRef ref) {
  using compiler::Dialect;
  using compiler::ExprKind;
  using compiler::TypeFamily;
  const auto &condition = modelExpression(plan, ref);
  if (condition.kind == ExprKind::Boolean) {
    if (condition.unresolvedBooleanName) return {};
    return std::vector<ColumnRef>{};
  }
  if (condition.kind == ExprKind::IsNull && condition.operands.size() == 1 &&
      (condition.value == "IS NULL" || condition.value == "IS NOT NULL") &&
      (plan.program.sourceDialect == Dialect::PostgreSQL || plan.program.sourceDialect == Dialect::SQLServer)) {
    if (auto column = boundScalarColumn(plan, {ref.query, condition.operands[0]}))
      return std::vector<ColumnRef>{std::move(*column)};
    return {};
  }
  if (condition.kind != ExprKind::Binary || condition.operands.size() != 2 ||
      (condition.value != "=" && condition.value != "<>" && condition.value != "!=" &&
       condition.value != "<" && condition.value != ">" && condition.value != "<=" && condition.value != ">="))
    return {};
  const ExpressionRef left{ref.query, condition.operands[0]}, right{ref.query, condition.operands[1]};
  auto a = boundScalarColumn(plan, left), b = boundScalarColumn(plan, right);
  if (a && b && sameIntegerType(plan, left, right))
    return std::vector<ColumnRef>{std::move(*a), std::move(*b)};
  if (bool(a) == bool(b)) return {};
  const auto &value = modelExpression(plan, a ? right : left);
  const auto &type = plan.program.types.get(modelExpression(plan, a ? left : right).sqlType);
  // The literal fits a signed 32-bit value. Comparing it to a native integer
  // of at least that width does not need a narrowing or textual conversion.
  if (type.family != TypeFamily::Integer || !type.bits || *type.bits < 32 ||
      !type.unsignedInteger || value.kind != ExprKind::Number || value.value.empty() ||
      value.value.size() > 9 || value.value.find_first_not_of("0123456789") != std::string::npos)
    return {};
  return std::vector<ColumnRef>{a ? std::move(*a) : std::move(*b)};
}
std::optional<JoinKeyPair> correlatedIntegerEqualityKey(const LogicalPlan &plan, ExpressionRef ref,
                                                        compiler::QueryId outer) {
  if (!outer || ref.query == outer) return {};
  const auto operands = integerEqualityOperands(plan, ref);
  if (!operands) return {};
  if (scopedScalarColumn(plan, operands->left, outer) && boundScalarColumn(plan, operands->right))
    return operands;
  if (scopedScalarColumn(plan, operands->right, outer) && boundScalarColumn(plan, operands->left))
    return JoinKeyPair{operands->right, operands->left};
  return {};
}
static std::optional<std::pair<ColumnRef,ColumnRef>>
boundJoinKeys(const LogicalPlan &plan,const RelNode &join,const JoinKeyPair &keys) {
  if (join.inputs.size()!=2 || join.predicates.size()!=1) return {};
  const auto &outer=plan.nodes.at(join.inputs[0]);
  const auto &inner = plan.nodes.at(join.inputs[1]);
  if (!sameIntegerType(plan,keys.left,keys.right) ||
      !signedIntegerBits(plan.program.types.get(modelExpression(plan,keys.left).sqlType)))
    return {};
  const auto &left=modelExpression(plan,keys.left), &right=modelExpression(plan,keys.right);
  if (!left.binding || !right.binding ||
      (left.binding->scope==right.binding->scope && left.binding->table==right.binding->table))
    return {};
  auto a=scopedScalarColumn(plan,keys.left,left.binding->scope);
  auto b=scopedScalarColumn(plan,keys.right,right.binding->scope);
  const auto contains=[](const RelNode &input,const ColumnRef &key) {
    return std::count_if(input.outputs.begin(),input.outputs.end(),[&](const auto &field) {
      return key.query==field.query && key.table==field.table && key.name==field.name;
    })==1;
  };
  if (!a || !b || !contains(outer,*a) || !contains(inner,*b)) return {};
  return std::make_pair(std::move(*a), std::move(*b));
}
std::optional<std::pair<ColumnRef,ColumnRef>>
membershipColumns(const LogicalPlan &plan,const RelNode &join) {
  return join.membershipKeys?boundJoinKeys(plan,join,*join.membershipKeys):std::nullopt;
}
std::optional<std::pair<ColumnRef,ColumnRef>>
correlationColumns(const LogicalPlan &plan,const RelNode &join) {
  return join.correlationKeys?boundJoinKeys(plan,join,*join.correlationKeys):std::nullopt;
}
std::vector<ColumnRef> referencedColumns(PlannerContext &c, ExpressionRef ref) {
  std::vector<ColumnRef> out;
  std::vector<compiler::ExprId> work{ref.expression};
  std::set<compiler::ExprId> seen;
  const auto &q = modelQuery(c.result.logical, ref.query);
  while (!work.empty()) {
    c.stop();
    auto id = work.back();
    work.pop_back();
    if (!seen.insert(id).second)
      continue;
    if (seen.size() > c.options.maxNodes)
      c.fail(q.origin, "expression reference limit exceeded");
    const auto &e = modelExpression(c.result.logical, {q.id, id});
    if (auto col = resolveColumn(c.result.logical, {q.id, id}))
      out.push_back(*col);
    work.insert(work.end(), e.operands.begin(), e.operands.end());
  }
  return out;
}
bool movableExpression(PlannerContext &c, ExpressionRef ref) {
  std::vector<compiler::ExprId> work{ref.expression};
  std::set<compiler::ExprId> seen;
  const auto &q = modelQuery(c.result.logical, ref.query);
  while (!work.empty()) {
    c.stop();
    auto id = work.back();
    work.pop_back();
    if (!seen.insert(id).second)
      continue;
    if (seen.size() > c.options.maxNodes)
      return false;
    const auto &e = modelExpression(c.result.logical, {q.id, id});
    if (e.localSymbol || e.query || e.window || e.unresolvedBooleanName)
      return false;
    switch (e.kind) {
    case compiler::ExprKind::Name:
      if (!resolveColumn(c.result.logical, {q.id, id}))
        return false;
      break;
    case compiler::ExprKind::Number:
    case compiler::ExprKind::String:
    case compiler::ExprKind::Null:
    case compiler::ExprKind::Boolean:
      break;
    case compiler::ExprKind::Binary:
      if (e.value != "AND" && e.value != "OR" && e.value != "=" &&
          e.value != "<>" && e.value != "!=" && e.value != "<" &&
          e.value != ">" && e.value != "<=" && e.value != ">=")
        return false;
      break;
    case compiler::ExprKind::IsNull:
      break;
    case compiler::ExprKind::Unary:
      if (e.value != "NOT" && e.value != "IS NULL" && e.value != "IS NOT NULL")
        return false;
      break;
    default:
      return false;
    }
    work.insert(work.end(), e.operands.begin(), e.operands.end());
  }
  return true;
}
void lowerLogical(PlannerContext &context, const compiler::ModelProgram &model) {
  const auto &options = context.options;
  context.stop();
  if (model.queries.size() > options.maxNodes || model.roots.size() != 1)
    context.fail({}, "local optimization requires one bounded statement");
  context.charge(model.types.accountedBytes());
  for (const auto &q : model.queries) {
    for (const auto &cte:q.ctes)
      if (!cte.query || cte.query>model.queries.size() ||
          model.queries[cte.query-1].kind!=compiler::StatementKind::Select)
        context.fail(q.origin,"local planning requires read-only, bound CTEs");
    if (q.selectInto)
      context.fail(q.origin, "procedural SELECT INTO changes local state and requires a routine plan");
    if (q.limitWithTies && (q.kind != compiler::StatementKind::Select ||
        !q.limit || q.limit >= q.expressions.size() ||
        (q.orderBy.empty() && model.sourceDialect != compiler::Dialect::Oracle)))
      context.fail(q.origin, "invalid WITH TIES limit in optimizer input");
    if (q.scope.parentTables)
      context.charge(q.scope.parentTables->size() * sizeof(std::size_t));
    if (q.groupingSets.size()>256) context.fail(q.origin,"optimizer grouping set limit exceeded");
    std::size_t groupingReferences=0;
    for (const auto &set:q.groupingSets) {
      if(set.size()>8192-groupingReferences)context.fail(q.origin,"optimizer grouping reference limit exceeded");
      groupingReferences+=set.size();
      for(auto e:set) {
        if(!e || e>=q.expressions.size() || std::find(q.groupBy.begin(),q.groupBy.end(),e)==q.groupBy.end())
          context.fail(q.origin,"invalid grouping-set expression reference");
      }
      context.charge(sizeof(set)+set.size()*sizeof(compiler::ExprId));
    }
    if (q.kind==compiler::StatementKind::Merge) {
      using namespace compiler;
      if (!q.mergeOn || q.mergeOn>=q.expressions.size() || !q.targetTable ||
          *q.targetTable>=q.tables.size() || !q.sourceFrom || q.sourceFrom>=q.relations.size() ||
          q.mergeClauses.empty() || q.mergeClauses.size()>256 || q.limit || q.offset)
        context.fail(q.origin,"MERGE planning needs a bound target, source, join predicate and ordered actions without TOP");
      const auto valid=[&](ExprId ref) { return ref && ref<q.expressions.size(); };
      for (const auto &branch:q.mergeClauses) {
        context.stop();
        if (static_cast<unsigned>(branch.match)>static_cast<unsigned>(MergeMatch::NotMatchedSource) ||
            static_cast<unsigned>(branch.action)>static_cast<unsigned>(MergeAction::Nothing) ||
            (branch.condition && !valid(branch.condition)) || (branch.deleteWhere && !valid(branch.deleteWhere)) ||
            branch.assignments.size()>4096 || branch.values.size()>4096 || branch.columns.size()>4096 ||
            !std::all_of(branch.values.begin(),branch.values.end(),valid))
          context.fail(branch.origin,"invalid MERGE action in optimizer input");
        context.charge(sizeof(branch)+branch.values.size()*sizeof(ExprId));
        for (const auto &name:branch.columns) context.charge(sizeof(name)+name.size());
        for (const auto &assignment:branch.assignments) {
          if (!valid(assignment.value) || assignment.targets.empty() || assignment.targets.size()>4096 ||
              !std::all_of(assignment.targets.begin(),assignment.targets.end(),valid))
            context.fail(branch.origin,"invalid MERGE assignment in optimizer input");
          context.charge(sizeof(assignment)+assignment.targets.size()*sizeof(ExprId));
        }
      }
    }
    if (!q.rowLocks.empty())
      context.fail(q.rowLocks.front().origin, "locking reads must use the database optimizer");
    if (q.prewhere || q.qualify || q.systemTime || q.limitBy || q.groupByAll ||
        std::any_of(q.tables.begin(), q.tables.end(), [](const auto &table) { return table.finalRead || table.clickHouseSampling; }))
      context.fail(q.origin, "vendor query modifiers require the database optimizer");
    if (q.ddl || q.expressions.size() > 65536 ||
        q.tables.size() > options.maxNodes ||
        q.relations.size() > options.maxNodes ||
        q.rows.size() > options.maxNodes)
      context.fail(q.origin,
                   "statement shape exceeds local optimizer limits");
    if (q.context.locals.size() > 4096 || q.context.routines.size() > 4096 ||
        q.namedWindows.size() > 1024)
      context.fail(q.origin, "optimizer scope payload exceeds limits");
    for (const auto &table : q.tables) {
      if (table.definition && table.definition->columns.size() > 4096)
        context.fail(q.origin, "optimizer table output exceeds 4096 columns");
      std::size_t bytes = table.alias.size();
      if (table.ordinalityColumn) {
        if (table.postgresFunction==compiler::PostgresRowFunction::None || !table.query ||
            !table.definition || table.definition->columns.size()!=2)
          context.fail(q.origin,"invalid array ordinality in optimizer input");
        bytes += table.ordinalityColumn->size();
      }
      for (const auto &part : table.name)
        bytes += sizeof(part) + part.size();
      if (table.columnAliases.size() > 4096)
        context.fail(q.origin, "optimizer derived alias limit exceeded");
      for (const auto &alias : table.columnAliases)
        bytes += sizeof(alias) + alias.size();
      context.charge(bytes);
      if (table.jsonTable) {
        const auto &json = *table.jsonTable;
        if (!json.document || json.document >= q.expressions.size() ||
            json.documentTables.size() > q.tables.size() ||
            std::any_of(json.documentTables.begin(), json.documentTables.end(),
                        [&](const auto index) { return index >= q.tables.size(); }))
          context.fail(q.origin,"invalid JSON_TABLE document scope");
        context.charge(sizeof(json)+json.path.size()+json.documentTables.size()*sizeof(std::size_t));
        using Columns=decltype(json.columns);
        std::vector<std::pair<const Columns *,std::size_t>> pending{{&json.columns,0}};
        std::size_t count=0;
        while (!pending.empty()) {
          context.stop();
          const auto [columns,depth] = pending.back();
          pending.pop_back();
          if (depth > options.maxDepth || columns->size() > 4096-count)
            context.fail(q.origin,"JSON_TABLE planner column limit exceeded");
          count += columns->size();
          for (const auto &column : *columns) {
            context.charge(sizeof(column)+column.path.size()+column.onEmpty.json.size()+
                column.onError.json.size()+(column.name?column.name->size():0)+
                compiler::typeSyntaxBytes(column.type));
            if (!column.children.empty()) pending.push_back({&column.children,depth+1});
          }
        }
      }
    }
    std::size_t references = 0;
    if (q.conflict.columnAliases.size() > 4096 ||
        (q.conflict.incomingTable && *q.conflict.incomingTable >= q.tables.size()))
      context.fail(q.origin, "invalid or over-budget UPSERT row alias");
    context.charge(q.conflict.rowAlias.size());
    if (q.transaction && q.transaction->name)
      context.charge(q.transaction->name->size());
    if (q.prepared) {
      if (q.prepared->arguments.size()>4096)
        context.fail(q.origin,"optimizer prepared argument limit exceeded");
      context.charge(q.prepared->name.size()+q.prepared->arguments.size()*sizeof(compiler::ExprId));
    }
    for (const auto &alias : q.conflict.columnAliases)
      context.charge(sizeof(alias) + alias.size());
    for (const auto &row : q.rows) {
      if (row.size() > 65536 || references > 65536 - row.size())
        context.fail(q.origin, "optimizer VALUES reference limit exceeded");
      references += row.size();
      context.charge(sizeof(row) + row.size() * sizeof(compiler::ExprId));
    }
    for (const auto &local : q.context.locals) {
      std::size_t bytes = sizeof(local);
      for (const auto &part : local.name)
        bytes += sizeof(part) + part.size();
      context.charge(bytes);
    }
    for (const auto &routine : q.context.routines)
      context.charge(sizeof(routine) + routine.database.size() +
                     routine.schema.size() + routine.name.size());
    context.charge(q.projections.size() * sizeof(compiler::ModelProjection) +
                   q.orderBy.size() * sizeof(compiler::OrderItem) +
                   q.groupBy.size() * sizeof(compiler::ExprId));
    context.charge(sizeof(q) +
                   q.expressions.size() * sizeof(compiler::ModelExpression) +
                   q.tables.size() * sizeof(compiler::ModelTable) +
                   q.relations.size() * sizeof(compiler::ModelRelation));
    if (q.outputInto) {
      const auto &output=*q.outputInto;
      context.charge(output.tableVariable.size()+
          output.projections.size()*sizeof(compiler::ModelProjection)+
          (output.table.size()+output.columns.size())*sizeof(std::string));
      for (const auto &piece:output.table) context.charge(piece.size());
      for (const auto &column:output.columns) context.charge(column.size());
      for (const auto &projection:output.projections) context.charge(projection.alias.size());
    }
    for (const auto &e : q.expressions) {
      std::size_t bytes =
          e.value.size() + e.routineSchema.size() + e.operands.size() * sizeof(compiler::ExprId);
      for (const auto &name : e.name)
        bytes += sizeof(std::string) + name.size();
      if(e.window) {
        const auto &window=*e.window;
        if(window.ordering.size()>4096 || window.partitions>4096 ||
           static_cast<unsigned>(window.exclusion)>static_cast<unsigned>(compiler::WindowExclusion::Ties))
          context.fail(e.origin,"invalid or over-budget window specification in optimizer input");
        bytes+=sizeof(compiler::WindowSpec)+window.frameUnit.size()+window.ordering.size()*sizeof(compiler::WindowOrdering);
        for(const auto &order:window.ordering)bytes+=order.nulls.size();
      }
      if(e.interval)bytes+=sizeof(compiler::IntervalSpec);
      if(e.withinGroup)bytes+=sizeof(compiler::WindowOrdering)+e.withinGroup->nulls.size();
      if(e.castType)bytes+=compiler::typeSyntaxBytes(*e.castType);
      context.charge(bytes);
    }
  }
  context.result.logical.program = model;
  buildLogical(context);
}
OptimizationResult optimize(const compiler::ModelProgram &model,
                            const StatisticsProvider *statistics,
                            PlannerOptions options) {
  OptimizationResult result;
  const auto began = std::chrono::steady_clock::now();
  try {
    if (!std::isfinite(options.assumedRows) || options.assumedRows < 0 ||
        options.assumedRows > 1e15 || !std::isfinite(options.assumedRowBytes) ||
        options.assumedRowBytes <= 0 || !std::isfinite(options.memoryBytes) ||
        options.memoryBytes <= 0 || !std::isfinite(options.pageBytes) ||
        options.pageBytes <= 0 || !std::isfinite(options.weights.cpu) ||
        options.weights.cpu < 0 || !std::isfinite(options.weights.io) ||
        options.weights.io < 0 || !std::isfinite(options.weights.memory) ||
        options.weights.memory < 0 ||
        options.weights.cpu + options.weights.io + options.weights.memory <= 0)
      throw std::invalid_argument("invalid local cost model options");
    options.maxGroups = std::max<std::size_t>(
        1, std::min<std::size_t>(8192, options.maxGroups));
    options.maxExpressions = std::max<std::size_t>(
        1, std::min<std::size_t>(32768, options.maxExpressions));
    options.maxNodes = std::max<std::size_t>(
        1, std::min<std::size_t>(16384, options.maxNodes));
    options.maxDepth =
        std::max<std::size_t>(1, std::min<std::size_t>(128, options.maxDepth));
    options.maxBytes =
        std::clamp<std::size_t>(options.maxBytes, 65536, 256 * 1024 * 1024);
    options.maxTimeMs = std::clamp<std::size_t>(options.maxTimeMs, 1, 10000);
    options.maxRuleMatches =
        std::min<std::size_t>(options.maxRuleMatches, 65536);
    options.heuristic.maxPasses=std::min<std::size_t>(64,options.heuristic.maxPasses);
    options.heuristic.maxMatches=std::min<std::size_t>(65536,options.heuristic.maxMatches);
    if(options.heuristic.rules.size()>64 || options.heuristic.substitutions.size()>64 ||
       static_cast<unsigned>(options.heuristic.order)>1)
      throw std::invalid_argument("invalid Hep program shape or match order");
    for (const auto *phase:{&options.heuristic.rules,&options.heuristic.substitutions})
      for (auto rule:*phase)
        if (static_cast<unsigned>(rule)>=static_cast<unsigned>(HepRule::Count))
          throw std::invalid_argument("unknown Hep program rule");
    result.weights = options.weights;
    result.configuration = options;
    result.configuration.shouldStop = {};
    PlannerContext context{result, statistics, options,
                           began +
                               std::chrono::milliseconds(options.maxTimeMs)};
    lowerLogical(context, model);
    searchMemo(context);
    inspectAdvice(context);
  } catch (const PlannerStopped &) {
    result.searchComplete = false;
    result.diagnostics.push_back(
        {compiler::DiagnosticCode::Stopped,
         {},
         "local planning cancelled or time budget exhausted"});
  } catch (const PlannerBudget &) {
    result.searchComplete = false;
    if (result.baseline.root) {
      if (!result.best.root || result.best.root >= result.best.nodes.size())
        result.best = result.baseline;
      result.advice.push_back({AdviceKind::SearchLimit,
                               {},
                               "本地分析达到时间或空间预算",
                               "保留完整基线计划；请缩小查询或调整分析预算。"});
    } else
      result.diagnostics.push_back({compiler::DiagnosticCode::ResourceLimit,
                                    {},
                                    "local planning time or space budget "
                                    "exhausted before baseline was complete"});
  } catch (const PlannerFailure &e) {
    result.diagnostics.push_back(e.diagnostic);
  } catch (const std::exception &e) {
    result.diagnostics.push_back(
        {compiler::DiagnosticCode::InvalidQueryShape, {}, e.what()});
  }
  result.elapsedMs = std::chrono::duration<double, std::milli>(
                         std::chrono::steady_clock::now() - began)
                         .count();
  return result;
}
const char *relKindName(RelKind kind) {
  switch (kind) {
  case RelKind::Calc:
    return "Calc";
  case RelKind::Sample:
    return "Sample";
  case RelKind::Unnest:
    return "Unnest";
  case RelKind::Series:
    return "GenerateSeries";
  case RelKind::Match:
    return "Match";
  case RelKind::Unpivot:
    return "Unpivot";
  case RelKind::Pivot:
    return "Pivot";
  case RelKind::Values:
    return "Values";
  case RelKind::Scan:
    return "Scan";
  case RelKind::Filter:
    return "Filter";
  case RelKind::Join:
    return "Join";
  case RelKind::SetOperation:
    return "Set Operation";
  case RelKind::Aggregate:
    return "Aggregate";
  case RelKind::Project:
    return "Project";
  case RelKind::Sort:
    return "Sort";
  case RelKind::Limit:
    return "Limit";
  case RelKind::Window:
    return "Window";
  case RelKind::Subquery:
    return "Subquery";
  case RelKind::Modify:
    return "Modify";
  case RelKind::WorkingTableScan:
    return "Working Table Scan";
  case RelKind::RepeatUnion:
    return "Repeat Union";
  case RelKind::Correlate:
    return "Correlate";
  case RelKind::SemiJoin:
    return "Semi Join";
  case RelKind::AntiJoin:
    return "Anti Join";
  case RelKind::TableFunction:
    return "Table Function";
  case RelKind::Rename:
    return "Rename Columns";
  case RelKind::Coerce:
    return "Widen Integer Columns";
  }
  return "Unknown";
}
const char *physicalKindName(PhysicalKind kind) {
  switch (kind) {
  case PhysicalKind::Calc:
    return "Calc";
  case PhysicalKind::Sample:
    return "Bernoulli Sample";
  case PhysicalKind::Unnest:
    return "Array Unnest";
  case PhysicalKind::Series:
    return "Generate Series";
  case PhysicalKind::Match:
    return "Row Pattern Match";
  case PhysicalKind::Unpivot:
    return "Unpivot";
  case PhysicalKind::Pivot:
    return "Pivot Aggregate";
  case PhysicalKind::Values:
    return "Values";
  case PhysicalKind::TableScan:
    return "Table Scan";
  case PhysicalKind::IndexScan:
    return "Index Scan";
  case PhysicalKind::Filter:
    return "Filter";
  case PhysicalKind::NestedLoopJoin:
    return "Nested Loop Join";
  case PhysicalKind::MergeJoin:
    return "Merge Join";
  case PhysicalKind::HashJoin:
    return "Hash Join";
  case PhysicalKind::Append:
    return "Append";
  case PhysicalKind::MergeUnion:
    return "Merge Union";
  case PhysicalKind::HashSet:
    return "Hash Set";
  case PhysicalKind::SortSet:
    return "Sort Set";
  case PhysicalKind::HashAggregate:
    return "Hash Aggregate";
  case PhysicalKind::SortAggregate:
    return "Sort Aggregate";
  case PhysicalKind::Project:
    return "Project";
  case PhysicalKind::Sort:
    return "Sort";
  case PhysicalKind::TopNSort:
    return "Top-N Sort";
  case PhysicalKind::Limit:
    return "Limit";
  case PhysicalKind::Window:
    return "Window";
  case PhysicalKind::Subquery:
    return "Subquery";
  case PhysicalKind::Modify:
    return "Modify";
  case PhysicalKind::Empty:
    return "Empty";
  case PhysicalKind::WorkingTableScan:
    return "Working Table Scan";
  case PhysicalKind::RepeatUnion:
    return "Repeat Union";
  case PhysicalKind::CorrelatedNestedLoop:
    return "Correlated Nested Loop";
  case PhysicalKind::NestedLoopSemiJoin:
    return "Nested Loop Semi Join";
  case PhysicalKind::NestedLoopAntiJoin:
    return "Nested Loop Anti Join";
  case PhysicalKind::HashSemiJoin:
    return "Hash Semi Join";
  case PhysicalKind::HashAntiJoin:
    return "Hash Anti Join";
  case PhysicalKind::TableFunctionScan:
    return "Table Function Scan";
  }
  return "Unknown";
}
const char *statisticsSourceName(StatisticsSource source) {
  switch (source) {
  case StatisticsSource::Assumed:
    return "assumed";
  case StatisticsSource::Manual:
    return "manual";
  case StatisticsSource::ServerCatalog:
    return "server catalog estimate";
  }
  return "unknown";
}
} // namespace navidog::sql::optimizer
