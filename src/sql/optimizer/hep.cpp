#include "optimizer_internal.hpp"
#include "navidog/sql/compiler/operator_catalog.hpp"
#include "navidog/sql/optimizer/rel_metadata.hpp"
#include <algorithm>
#include <array>
#include <cmath>
#include <charconv>
#include <tuple>

namespace navidog::sql::optimizer {
namespace {
struct HepLimit {};
bool same(const ColumnRef &a,const ColumnRef &b){return a.query==b.query && a.table==b.table && a.name==b.name;}
bool columns(const std::vector<ColumnRef> &a,const std::vector<ColumnRef> &b){return a.size()==b.size() && std::equal(a.begin(),a.end(),b.begin(),same);}
bool expressions(const std::vector<ExpressionRef> &a,const std::vector<ExpressionRef> &b){return a.size()==b.size() && std::equal(a.begin(),a.end(),b.begin(),[](const auto &x,const auto &y){return x.query==y.query && x.expression==y.expression;});}
class HepPlanner {
  PlannerContext &c;
  LogicalPlan &plan;
  RelMetadataQuery metadata;
  std::size_t charged=0;
  std::map<RelId,RelId> memo;
  std::set<RelId> active;
  RelId append(RelNode n) {
    c.stop();
    if(plan.nodes.size()>=c.options.maxNodes)throw HepLimit{};
    c.chargeNode(n);plan.nodes.push_back(std::move(n));return RelId(plan.nodes.size()-1);
  }
  const RelProperties &properties(RelId id) {
    const auto *p=metadata.properties(id);
    const auto bytes=metadata.accountedBytes();
    if(bytes>charged){c.charge(bytes-charged);charged=bytes;}
    if(!p) {
      if(metadata.status()==MetadataStatus::Stopped)throw PlannerStopped{};
      if(metadata.status()==MetadataStatus::ResourceLimit)throw HepLimit{};
      c.fail({},metadata.message());
    }
    return *p;
  }
  compiler::TypeId outputType(RelId id,std::size_t ordinal,std::size_t depth=0) {
    using namespace compiler;
    c.stop();
    const auto &n=plan.nodes.at(id);
    if (depth>=c.options.maxDepth || ordinal>=n.outputs.size()) return 0;
    if (n.valueTypes.size()==n.outputs.size()) return n.valueTypes[ordinal];
    const auto &q=modelQuery(plan,n.query);
    if (n.kind==RelKind::Scan && n.table && *n.table<q.tables.size() && q.catalogChecked) {
      const auto &table=q.tables[*n.table].definition;
      if (!table || ordinal>=table->columns.size() || !table->columns[ordinal].sqlType ||
          table->columns[ordinal].name!=n.outputs[ordinal].name)
        return 0;
      const auto before=plan.program.types.accountedBytes();
      const auto type=plan.program.types.intern(*table->columns[ordinal].sqlType);
      c.charge(plan.program.types.accountedBytes()-before);
      return type;
    }
    if ((n.kind==RelKind::Project || n.kind==RelKind::Calc) && n.keys.size()==n.outputs.size()) {
      const auto &value=modelExpression(plan,n.keys[ordinal]);
      if (value.kind!=ExprKind::Star) return value.sqlType;
    }
    if (n.kind==RelKind::SetOperation || n.kind==RelKind::Values)
      return q.projections.size()==n.outputs.size()
          ?modelExpression(plan,{q.id,q.projections[ordinal].expression}).sqlType:0;
    if (n.kind==RelKind::Join && n.inputs.size()==2 && !n.distinct) {
      const auto width=plan.nodes.at(n.inputs[0]).outputs.size();
      const auto side=ordinal<width?0u:1u;
      const auto position=side?ordinal-width:ordinal;
      const auto &child=plan.nodes.at(n.inputs[side]);
      if (position>=child.outputs.size() || !same(child.outputs[position],n.outputs[ordinal])) return 0;
      return outputType(n.inputs[side],position,depth+1);
    }
    if (n.kind==RelKind::Coerce) {
      for (const auto &conversion:n.integerWidenings)
        if (conversion.ordinal==ordinal) return conversion.target;
    }
    if (n.inputs.empty()) return 0;
    const auto &child=plan.nodes.at(n.inputs.front());
    if (child.outputs.size()!=n.outputs.size()) return 0;
    if (n.kind!=RelKind::Subquery && n.kind!=RelKind::Rename &&
        !same(n.outputs[ordinal],child.outputs[ordinal])) return 0;
    return outputType(n.inputs.front(),ordinal,depth+1);
  }
  void preserveValuesTypes(RelId original,RelNode &values) {
    if (plan.program.sourceDialect!=compiler::Dialect::PostgreSQL ||
        values.outputs.size()>256 || values.outputs.size()!=plan.nodes.at(original).outputs.size())
      return;
    std::vector<compiler::TypeId> types;
    c.charge(values.outputs.size()*sizeof(compiler::TypeId));
    for (std::size_t i=0;i<values.outputs.size();++i) {
      const auto type=outputType(original,i);
      if (!type) return;
      types.push_back(type);
    }
    values.valueTypes=std::move(types);
  }
  bool literalLimit(const RelNode &n) const {
    auto valid=[](std::optional<double> x){return x && std::isfinite(*x) && *x>=0 && *x<=9007199254740991. && std::floor(*x)==*x;};
    return !n.withTies && valid(n.rowOffset) && (valid(n.rowLimit) || (!n.rowLimit && n.keys.empty()));
  }
  using Binding=std::tuple<compiler::QueryId,std::size_t,std::size_t>;
  std::optional<Binding> columnIdentity(ExpressionRef ref,std::size_t depth=0) {
    if(depth>16)return {};
    const auto &q=modelQuery(plan,ref.query);const auto &e=modelExpression(plan,ref);
    if(e.outputColumn && *e.outputColumn<q.projections.size()) {
      const auto projected=q.projections[*e.outputColumn].expression;
      if(projected==ref.expression)return {};
      return columnIdentity({q.id,projected},depth+1);
    }
    if(e.kind!=compiler::ExprKind::Name || e.localSymbol || !e.binding || !e.coalescedBindings.empty())return {};
    const auto &b=*e.binding;if(!b.scope || b.scope>plan.program.queries.size())return {};
    const auto &owner=modelQuery(plan,b.scope);
    if(b.table>=owner.tables.size() || !owner.tables[b.table].definition || b.column>=owner.tables[b.table].definition->columns.size())return {};
    return Binding{b.scope,b.table,b.column};
  }
  RelId removeDependentGroupKeys(RelId id, const RelNode &aggregate, const RelNode &input) {
    using namespace compiler;
    if (plan.program.sourceDialect != Dialect::PostgreSQL || aggregate.kind != RelKind::Aggregate ||
        aggregate.distinct || !aggregate.groupingSets.empty() || aggregate.keys.size() < 2 ||
        aggregate.keys.size() > 64 || input.query != aggregate.query) return id;
    const auto &q = modelQuery(plan, aggregate.query);
    if (!q.catalogChecked || q.tables.size() != 1 || q.selectInto || q.expressions.size() > 4096 ||
        !q.groupingSets.empty() || !q.namedWindows.empty()) return id;
    const auto &table = q.tables.front().definition;
    if (!table || table->kind != ObjectKind::Table || !table->complete || !table->arityKnown ||
        !table->primaryKeyKnown || table->primaryKey.empty()) return id;
    if (input.kind != RelKind::Scan && (input.kind != RelKind::Filter || input.inputs.size() != 1 ||
        plan.nodes.at(input.inputs.front()).kind != RelKind::Scan)) return id;
    std::vector<ColumnRef> groupColumns;
    c.charge(aggregate.keys.size() * sizeof(ColumnRef));
    for (auto ref : aggregate.keys) {
      c.stop();
      auto col = boundScalarColumn(plan, ref);
      const auto &expr = modelExpression(plan, ref);
      if (!col || ref.query != q.id || !signedIntegerBits(plan.program.types.get(expr.sqlType))) return id;
      groupColumns.push_back(std::move(*col));
    }
    if (!coversNonNullUniqueKey(properties(aggregate.inputs.front()), groupColumns)) return id;

    // The unique key determines every row in the scan. Retain additional keys
    // when downstream expressions need their group values.
    std::vector<ExprId> work;
    for (const auto &p : q.projections) work.push_back(p.expression);
    for (const auto &o : q.orderBy) work.push_back(o.expression);
    if (q.having) work.push_back(q.having);
    c.charge(q.expressions.size() * (sizeof(ExprId) + sizeof(Binding)));
    std::vector<bool> visited(q.expressions.size());
    std::set<Binding> used;
    while (!work.empty()) {
      c.stop();
      const auto exprId = work.back();
      work.pop_back();
      if (visited.at(exprId)) continue;
      visited[exprId] = true;
      const auto &expr = modelExpression(plan, {q.id, exprId});
      if (expr.query || expr.window || expr.kind == ExprKind::Star) return id;
      if (expr.kind == ExprKind::Name || expr.outputColumn) {
        if (auto binding = columnIdentity({q.id, exprId})) used.insert(*binding);
        else if (expr.outputColumn && *expr.outputColumn < q.projections.size())
          work.push_back(q.projections[*expr.outputColumn].expression);
        else return id;
      }
      if (expr.kind == ExprKind::Call && expr.name.size() == 1 &&
          (expr.nameQuoted.empty() || !expr.nameQuoted.front()) &&
          uppercase(expr.name.front()) == "COUNT" && expr.operands.size() == 1) {
        const auto &arg = modelExpression(plan, {q.id, expr.operands.front()});
        if (arg.kind == ExprKind::Star && arg.name.empty()) continue;
      }
      work.insert(work.end(), expr.operands.begin(), expr.operands.end());
    }
    RelNode reduced = aggregate;
    reduced.keys.clear();
    for (std::size_t i = 0; i < aggregate.keys.size(); ++i) {
      const auto ref = aggregate.keys[i];
      const auto &binding = *modelExpression(plan, ref).binding;
      const auto &name = groupColumns[i].name;
      if (used.count({binding.scope, binding.table, binding.column}) ||
          std::find(table->primaryKey.begin(), table->primaryKey.end(), name) != table->primaryKey.end())
        reduced.keys.push_back(ref);
    }
    return reduced.keys.empty() || reduced.keys.size() == aggregate.keys.size()
        ? id : append(std::move(reduced));
  }
  RelId removeSingletonAggregate(RelId id, RelNode project) {
    using namespace compiler;
    if (plan.program.sourceDialect != Dialect::PostgreSQL || project.kind != RelKind::Project ||
        project.inputs.size() != 1 || project.keys.empty() || project.keys.size() > 64) return id;
    const auto &aggregate = plan.nodes.at(project.inputs.front());
    if (aggregate.kind != RelKind::Aggregate || aggregate.query != project.query || aggregate.distinct ||
        !aggregate.groupingSets.empty() || aggregate.keys.empty() || aggregate.keys.size() > 64 ||
        aggregate.inputs.size() != 1) return id;
    const auto input = aggregate.inputs.front();
    const auto &source = plan.nodes.at(input);
    if (!columns(aggregate.outputs, source.outputs) || source.query != project.query ||
        (source.kind != RelKind::Scan && (source.kind != RelKind::Filter || source.inputs.size() != 1 ||
         plan.nodes.at(source.inputs.front()).kind != RelKind::Scan))) return id;
    auto &q = plan.program.queries.at(project.query - 1);
    if (!q.catalogChecked || q.tables.size() != 1 || q.having || q.selectInto || !q.namedWindows.empty() ||
        project.keys.size() != q.projections.size() || project.keys.size() != project.outputs.size()) return id;
    for (const auto &order : q.orderBy) {
      const auto &expr = modelExpression(plan, {q.id, order.expression});
      if (!expr.outputColumn && !boundScalarColumn(plan, {q.id, order.expression})) return id;
    }
    const auto bits = [&](ExpressionRef ref) {
      return signedIntegerBits(plan.program.types.get(modelExpression(plan, ref).sqlType));
    };
    std::vector<ColumnRef> groups;
    c.charge(aggregate.keys.size() * sizeof(ColumnRef));
    for (const auto ref : aggregate.keys) {
      c.stop();
      auto col = boundScalarColumn(plan, ref);
      if (!col || !bits(ref) || ref.query != q.id) return id;
      groups.push_back(std::move(*col));
    }
    const auto &facts = properties(input);
    if (!coversNonNullUniqueKey(facts, groups)) return id;

    enum class Rewrite { Keep, IntegerValue, CountOne, CountNullable, SumBigInt, SumNumeric };
    std::vector<Rewrite> actions;
    c.charge(project.keys.size() * sizeof(Rewrite));
    for (auto ref : project.keys) {
      c.stop();
      if (ref.query != q.id) return id;
      const auto &entry = modelExpression(plan, ref);
      if (boundScalarColumn(plan, ref) || fixedLiteral(ref)) {
        actions.push_back(Rewrite::Keep);
        continue;
      }
      if (entry.kind != ExprKind::Call || entry.distinct || entry.window || entry.operands.size() != 1 ||
          (entry.routineResolved && (entry.routineSchema != "pg_catalog" ||
           entry.routineKind != RoutineKind::Aggregate || entry.routineVolatility != FunctionVolatility::Immutable))) return id;
      const auto builtin = builtinOperator(plan.program.sourceDialect, entry.name, entry.nameQuoted, 1);
      if (!builtin || builtin->kind != RoutineKind::Aggregate) return id;
      const ExpressionRef argument{q.id, entry.operands.front()};
      const auto &arg = modelExpression(plan, argument);
      const auto col = boundScalarColumn(plan, argument);
      if (builtin->name == "COUNT" && bits(ref) == 64) {
        if (arg.kind == ExprKind::Star && arg.name.empty()) actions.push_back(Rewrite::CountOne);
        else if (col) {
          const bool notNull = std::any_of(facts.nonNullColumns.begin(), facts.nonNullColumns.end(),
                                              [&](const auto &known) { return same(*col, known); });
          actions.push_back(notNull ? Rewrite::CountOne : Rewrite::CountNullable);
        } else return id;
      } else if (col && bits(argument) && (builtin->name == "MIN" || builtin->name == "MAX") &&
                 sameIntegerType(plan, ref, argument)) {
        actions.push_back(Rewrite::IntegerValue);
      } else if (col && bits(argument) && builtin->name == "SUM") {
        const auto &result = plan.program.types.get(entry.sqlType);
        if (bits(argument) != 64 && bits(ref) == 64) actions.push_back(Rewrite::SumBigInt);
        else if (bits(argument) == 64 && result.family == TypeFamily::Decimal && !result.arrayDimensions &&
                 result.opaqueName.empty() && !result.precision && !result.scale) actions.push_back(Rewrite::SumNumeric);
        else return id;
      } else return id;
    }
    if (project.keys.size() * 5 + q.expressions.size() > c.options.maxNodes) return id;
    const auto add = [&](ModelExpression entry) {
      c.stop();
      c.charge(sizeof(entry) + entry.value.size() + entry.operands.size() * sizeof(ExprId));
      const auto result = static_cast<ExprId>(q.expressions.size());
      q.expressions.push_back(std::move(entry));
      return result;
    };
    const auto literal = [&](std::string value, SourceRange origin) {
      ModelExpression entry;
      entry.kind = ExprKind::Number;
      entry.type = ValueType::Integer;
      entry.origin = origin;
      entry.value = std::move(value);
      return add(std::move(entry));
    };
    for (std::size_t i = 0; i < actions.size(); ++i) {
      const auto action = actions[i];
      if (action == Rewrite::Keep) continue;
      const auto ref = project.keys[i];
      const auto &before = modelExpression(plan, ref);
      const auto origin = before.origin;
      const auto type = before.type;
      const auto sqlType = before.sqlType;
      auto operand = before.operands.front();
      ModelExpression replacement;
      if (action == Rewrite::IntegerValue) {
        replacement = modelExpression(plan, {q.id, operand});
      } else {
        if (action == Rewrite::CountOne) operand = literal("1", origin);
        if (action == Rewrite::CountNullable) {
          ModelExpression test;
          test.kind = ExprKind::IsNull;
          test.type = ValueType::Boolean;
          test.origin = origin;
          test.value = "IS NULL";
          test.operands = {operand};
          ModelExpression conditional;
          conditional.kind = ExprKind::Case;
          conditional.type = ValueType::Integer;
          conditional.origin = origin;
          conditional.value = "searched";
          conditional.operands = {add(std::move(test)), literal("0", origin), literal("1", origin)};
          operand = add(std::move(conditional));
        }
        c.charge(sizeof(BasicTypeSpec<std::vector<std::string>>) + sizeof(std::string) + 8);
        auto target = std::make_shared<BasicTypeSpec<std::vector<std::string>>>();
        target->name = {action == Rewrite::SumNumeric ? "numeric" : "bigint"};
        replacement.kind = ExprKind::Cast;
        replacement.castType = std::move(target);
        replacement.operands = {operand};
      }
      replacement.origin = origin;
      replacement.type = type;
      replacement.sqlType = sqlType;
      project.keys[i].expression = add(std::move(replacement));
    }
    project.inputs.front() = input;
    return append(std::move(project));
  }
  RelId filterCaseAggregates(RelId id, RelNode project) {
    using namespace compiler;
    if (plan.program.sourceDialect != Dialect::PostgreSQL || project.kind != RelKind::Project ||
        project.inputs.size() != 1 || project.keys.empty() || project.keys.size()>64)
      return id;
    const auto &aggregate=plan.nodes.at(project.inputs.front());
    if(aggregate.kind!=RelKind::Aggregate || aggregate.query!=project.query ||
       aggregate.distinct || aggregate.inputs.size()!=1 || !aggregate.groupingSets.empty())
      return id;
    auto &query=plan.program.queries.at(project.query-1);
    if(!query.catalogChecked || query.having || query.selectInto || !query.namedWindows.empty() ||
       project.keys.size()!=query.projections.size() ||
       project.keys.size()!=project.outputs.size() ||
       query.expressions.size()+3*project.keys.size()>c.options.maxNodes)
      return id;
    for(const auto &order:query.orderBy) {
      const ExpressionRef key{query.id,order.expression};
      const auto &expression=modelExpression(plan,key);
      if((!expression.outputColumn || *expression.outputColumn>=project.keys.size()) &&
         !boundScalarColumn(plan,key))return id;
    }
    const auto output=&plan.nodes.at(aggregate.inputs.front()).outputs;
    const auto contains=[&](const ColumnRef &column){
      return std::any_of(output->begin(),output->end(),[&](const auto &candidate){return same(column,candidate);});
    };
    const auto appendExpr=[&](ModelExpression expression) {
      c.charge(sizeof(expression)+expression.operands.size()*sizeof(ExprId));
      const auto expressionId=static_cast<ExprId>(query.expressions.size());
      query.expressions.push_back(std::move(expression));
      return expressionId;
    };
    bool changed=false;
    for(auto &projection:project.keys) {
      c.stop();
      if(projection.query!=query.id)continue;
      const auto &call=modelExpression(plan,projection);
      if(call.kind!=ExprKind::Call || call.distinct || call.window || call.operands.size()!=1 ||
         (call.routineResolved && (call.routineSchema!="pg_catalog" ||
           call.routineKind!=RoutineKind::Aggregate || call.routineVolatility!=FunctionVolatility::Immutable)))
        continue;
      const auto native=builtinOperator(Dialect::PostgreSQL,call.name,call.nameQuoted,1);
      if(!native || native->kind!=RoutineKind::Aggregate ||
         (native->name!="COUNT" && native->name!="SUM" && native->name!="MIN" && native->name!="MAX"))
        continue;
      const ExpressionRef conditional{query.id,call.operands.front()};
      const auto &branch=modelExpression(plan,conditional);
      if(branch.kind!=ExprKind::Case || branch.value!="searched" || branch.operands.size()!=3 ||
         modelExpression(plan,{query.id,branch.operands[2]}).kind!=ExprKind::Null)
        continue;
      const ExpressionRef predicate{query.id,branch.operands[0]}, input{query.id,branch.operands[1]};
      const auto refs=totalPredicateColumns(plan,predicate);
      if(!refs || !std::all_of(refs->begin(),refs->end(),contains) ||
         !sameIntegerType(plan,conditional,input))continue;
      const auto column=boundScalarColumn(plan,input);
      const auto &value=modelExpression(plan,input);
      const bool literal=(value.kind==ExprKind::Number || value.kind==ExprKind::Unary) && fixedLiteral(input);
      if((!column || !contains(*column)) && !literal)continue;
      const auto &type=plan.program.types.get(call.sqlType);
      const bool count=native->name=="COUNT";
      if(count && signedIntegerBits(type)!=64)continue;
      if(native->name=="SUM") {
        const auto inputBits=signedIntegerBits(plan.program.types.get(value.sqlType));
        const bool exactNumeric=type.family==TypeFamily::Decimal && !type.precision && !type.scale &&
                                !type.arrayDimensions && type.opaqueName.empty();
        if(inputBits==64 ? !exactNumeric : signedIntegerBits(type)!=64)continue;
      }
      if((native->name=="MIN" || native->name=="MAX") && !sameIntegerType(plan,projection,input))
        continue;
      auto rewritten=call;
      ModelExpression filter;
      filter.kind=ExprKind::AggregateFilter;
      filter.origin=call.origin;
      filter.type=call.type;
      filter.sqlType=call.sqlType;
      auto argument=input.expression;
      if(count && literal) {
        ModelExpression star;
        star.kind=ExprKind::Star;
        star.origin=modelExpression(plan,input).origin;
        argument=appendExpr(std::move(star));
      }
      rewritten.operands={argument};
      filter.operands={appendExpr(std::move(rewritten)),predicate.expression};
      projection.expression=appendExpr(std::move(filter));
      changed=true;
    }
    return changed?append(std::move(project)):id;
  }
  std::string nativeExtremum(ExpressionRef ref) const {
    using namespace compiler;
    const auto &call = modelExpression(plan, ref);
    if (call.kind != ExprKind::Call || call.distinct || call.window || call.operands.size() != 1 ||
        (call.routineResolved && (call.routineSchema != "pg_catalog" ||
         call.routineKind != RoutineKind::Aggregate || call.routineVolatility != FunctionVolatility::Immutable)))
      return {};
    const auto builtin = builtinOperator(Dialect::PostgreSQL, call.name, call.nameQuoted, 1);
    return builtin && builtin->kind == RoutineKind::Aggregate &&
        (builtin->name == "MIN" || builtin->name == "MAX") ? builtin->name : std::string{};
  }
  RelId reduceGroupKeyExtrema(RelId id,RelNode project) {
    using namespace compiler;
    if (plan.program.sourceDialect!=Dialect::PostgreSQL || project.kind!=RelKind::Project ||
        project.inputs.size()!=1 || project.keys.empty() || project.keys.size()>64 ||
        project.keys.size()!=project.outputs.size())
      return id;
    const auto &group=plan.nodes.at(project.inputs.front());
    if (group.kind!=RelKind::Aggregate || group.query!=project.query || group.distinct ||
        group.keys.empty() || group.keys.size()>64 || !group.groupingSets.empty())
      return id;
    const auto &query=modelQuery(plan,project.query);
    if (!query.catalogChecked || query.selectInto || !query.groupingSets.empty()) return id;
    std::vector<ColumnRef> keys;
    for (auto ref:group.keys) {
      const auto &expr=modelExpression(plan,ref);
      if (expr.outputColumn && *expr.outputColumn<query.projections.size())
        ref={query.id,query.projections[*expr.outputColumn].expression};
      if (const auto field=boundScalarColumn(plan,ref)) keys.push_back(*field);
    }
    c.charge(keys.size()*sizeof(ColumnRef));
    bool changed=false;
    for (auto &ref:project.keys) {
      c.stop();
      if (ref.query!=group.query || nativeExtremum(ref).empty()) continue;
      const ExpressionRef arg{ref.query,modelExpression(plan,ref).operands.front()};
      const auto field=boundScalarColumn(plan,arg);
      if (!field || !sameIntegerType(plan,ref,arg) ||
          !signedIntegerBits(plan.program.types.get(modelExpression(plan,arg).sqlType)) ||
          std::none_of(keys.begin(),keys.end(),[&](const auto &key){return same(key,*field);}))
        continue;
      ref=arg;
      changed=true;
    }
    return changed?append(std::move(project)):id;
  }
  RelId moveGlobalAggregateFilter(RelId id,RelNode project) {
    using namespace compiler;
    if (plan.program.sourceDialect!=Dialect::PostgreSQL || project.kind!=RelKind::Project ||
        project.inputs.size()!=1 || project.keys.empty() || project.keys.size()>32 ||
        project.keys.size()!=project.outputs.size())
      return id;
    auto group=plan.nodes.at(project.inputs.front());
    if (group.kind!=RelKind::Aggregate || group.query!=project.query || group.distinct ||
        group.inputs.size()!=1 || !group.keys.empty() || !group.groupingSets.empty())
      return id;
    auto &query=plan.program.queries.at(group.query-1);
    if (!query.catalogChecked || query.having || query.selectInto ||
        query.expressions.size()+2*project.keys.size()+32>c.options.maxNodes)
      return id;
    const auto &filter=plan.nodes.at(group.inputs.front());
    if (filter.kind!=RelKind::Filter || filter.query!=group.query || filter.inputs.size()!=1 ||
        filter.predicates.empty() || filter.predicates.size()>32)
      return id;
    const auto source=filter.inputs.front();
    const auto &scan=plan.nodes.at(source);
    if (scan.kind!=RelKind::Scan || scan.query!=group.query || !scan.table ||
        !columns(scan.outputs,filter.outputs) || properties(source).mayHaveEffects)
      return id;
    for (const auto ref:filter.predicates) {
      c.stop();
      if (ref.query!=query.id) return id;
      const auto fields=totalPredicateColumns(plan,ref);
      if (!fields || std::any_of(fields->begin(),fields->end(),[&](const auto &field) {
            return !containsOutput(scan,field);
          }))
        return id;
    }
    for (const auto ref:project.keys) {
      c.stop();
      const auto &call=modelExpression(plan,ref);
      if (ref.query!=query.id || call.kind!=ExprKind::Call || call.window || call.distinct ||
          call.operands.size()!=1 || (call.routineResolved &&
          (call.routineSchema!="pg_catalog" || call.routineKind!=RoutineKind::Aggregate ||
           call.routineVolatility!=FunctionVolatility::Immutable)))
        return id;
      const auto op=builtinOperator(Dialect::PostgreSQL,call.name,call.nameQuoted,1);
      if (!op || op->kind!=RoutineKind::Aggregate ||
          (op->name!="COUNT" && op->name!="MIN" && op->name!="MAX" && op->name!="SUM" && op->name!="AVG"))
        return id;
      const ExpressionRef arg{ref.query,call.operands.front()};
      const auto &value=modelExpression(plan,arg);
      if (op->name=="COUNT" && value.kind==ExprKind::Star && value.name.empty()) continue;
      const auto col=boundScalarColumn(plan,arg);
      if (!col || !containsOutput(scan,*col) ||
          !signedIntegerBits(plan.program.types.get(value.sqlType)))
        return id;
    }
    if (std::any_of(query.orderBy.begin(),query.orderBy.end(),[&](const auto &item) {
          return !modelExpression(plan,{query.id,item.expression}).outputColumn;
        }))
      return id;
    const auto add=[&](ModelExpression value) {
      c.charge(sizeof(value)+value.value.size()+value.operands.size()*sizeof(ExprId));
      const auto result=static_cast<ExprId>(query.expressions.size());
      query.expressions.push_back(std::move(value));
      return result;
    };
    auto predicate=filter.predicates.front().expression;
    for (std::size_t i=1; i<filter.predicates.size(); ++i) {
      ModelExpression both;
      both.kind=ExprKind::Binary;
      both.value="AND";
      both.origin=filter.origin;
      both.type=ValueType::Boolean;
      both.sqlType=modelExpression(plan,{query.id,predicate}).sqlType;
      both.operands={predicate,filter.predicates[i].expression};
      predicate=add(std::move(both));
    }
    for (auto &ref:project.keys) {
      const auto &call=modelExpression(plan,ref);
      ModelExpression aggregateFilter;
      aggregateFilter.kind=ExprKind::AggregateFilter;
      aggregateFilter.origin=call.origin;
      aggregateFilter.type=call.type;
      aggregateFilter.sqlType=call.sqlType;
      aggregateFilter.operands={ref.expression,predicate};
      ref.expression=add(std::move(aggregateFilter));
    }
    group.inputs={source};
    project.inputs={append(std::move(group))};
    return append(std::move(project));
  }
  RelId mergeAggregateProject(RelId id,RelNode project) {
    using namespace compiler;
    if (plan.program.sourceDialect!=Dialect::PostgreSQL || project.kind!=RelKind::Project ||
        project.inputs.size()!=1 || project.keys.empty() || project.keys.size()>64 ||
        project.keys.size()!=project.outputs.size())
      return id;
    auto aggregate=plan.nodes.at(project.inputs.front());
    if (aggregate.kind!=RelKind::Aggregate || aggregate.query!=project.query || aggregate.distinct ||
        aggregate.inputs.size()!=1 || aggregate.keys.size()>64 || !aggregate.groupingSets.empty())
      return id;
    const auto &derived=plan.nodes.at(aggregate.inputs.front());
    const auto &outer=modelQuery(plan,project.query);
    if (derived.kind!=RelKind::Subquery || derived.query!=outer.id || !derived.table ||
        derived.inputs.size()!=1 || derived.materialized || derived.correlated || derived.scalar ||
        !outer.catalogChecked || outer.tables.size()!=1 || outer.selectInto || outer.having ||
        !outer.namedWindows.empty())
      return id;
    const auto &binding=outer.tables.at(*derived.table);
    const auto &view=plan.nodes.at(derived.inputs.front());
    if (!binding.name.empty() || !binding.query || !binding.definition ||
        view.kind!=RelKind::Project || view.query!=binding.query || view.inputs.size()!=1 ||
        view.keys.size()!=view.outputs.size() || view.outputs.size()!=derived.outputs.size())
      return id;
    const auto sourceId=view.inputs.front();
    const auto &source=plan.nodes.at(sourceId);
    const auto *base=source.kind==RelKind::Filter && source.inputs.size()==1
        ?&plan.nodes.at(source.inputs.front()):&source;
    if (base->kind==RelKind::Project && base->query==view.query && base->inputs.size()==1 &&
        base->keys.size()==base->outputs.size()) {
      const auto &input=plan.nodes.at(base->inputs.front());
      for (std::size_t i=0; i<base->keys.size(); ++i) {
        const auto field=boundScalarColumn(plan,base->keys[i]);
        if (!field || !same(*field,base->outputs[i]) || !containsOutput(input,*field))
          return id;
      }
      base=&input;
    }
    auto &inner=plan.program.queries.at(view.query-1);
    if (base->kind!=RelKind::Scan || inner.tables.size()!=1 || !inner.catalogChecked ||
        base->query!=inner.id || source.query!=inner.id || inner.selectInto || inner.limit ||
        inner.offset || !inner.orderBy.empty() || inner.expressions.size()+project.keys.size()>c.options.maxNodes ||
        std::any_of(outer.orderBy.begin(),outer.orderBy.end(),[&](const auto &item) {
          return !modelExpression(plan,{outer.id,item.expression}).outputColumn;
        }))
      return id;
    for (auto ref:view.keys) {
      c.stop();
      const auto field=boundScalarColumn(plan,ref);
      if (!field || !containsOutput(source,*field) ||
          !signedIntegerBits(plan.program.types.get(modelExpression(plan,ref).sqlType)))
        return id;
    }
    const auto rebind=[&](ExpressionRef ref)->std::optional<ExpressionRef> {
      if (ref.query!=outer.id) return {};
      const auto &expression=modelExpression(plan,ref);
      if (expression.outputColumn) {
        if (*expression.outputColumn>=outer.projections.size()) return {};
        ref.expression=outer.projections[*expression.outputColumn].expression;
      }
      const auto column=boundScalarColumn(plan,ref);
      if (!column || column->table!=derived.table || !containsOutput(derived,*column)) return {};
      const auto ordinal=modelExpression(plan,ref).binding->column;
      if (ordinal>=view.keys.size() || !sameIntegerType(plan,ref,view.keys[ordinal])) return {};
      return view.keys[ordinal];
    };
    for (auto &key:aggregate.keys) {
      c.stop();
      const auto input=rebind(key);
      if (!input) return id;
      key=*input;
    }
    std::vector<std::optional<ModelExpression>> calls(project.keys.size());
    c.charge(calls.size()*sizeof(calls.front()));
    for (std::size_t i=0; i<project.keys.size(); ++i) {
      c.stop();
      auto &ref=project.keys[i];
      if (const auto input=rebind(ref)) {
        ref=*input;
        continue;
      }
      const auto &call=modelExpression(plan,ref);
      const auto op=builtinOperator(Dialect::PostgreSQL,call.name,call.nameQuoted,call.operands.size());
      if (ref.query!=outer.id || call.kind!=ExprKind::Call || call.window || call.operands.size()!=1 ||
          !op || op->kind!=RoutineKind::Aggregate ||
          (op->name!="COUNT" && op->name!="MIN" && op->name!="MAX" && op->name!="SUM" && op->name!="AVG") ||
          (call.routineResolved && (call.routineSchema!="pg_catalog" ||
          call.routineKind!=RoutineKind::Aggregate || call.routineVolatility!=FunctionVolatility::Immutable)))
        return id;
      const auto arg=rebind({outer.id,call.operands.front()});
      // COUNT(*) does not need a derived column but its star is scoped to
      // the original query, so leave that form to the native baseline.
      if (!arg) return id;
      c.charge(sizeof(ExprId)+call.name.size()*(sizeof(std::string)+16));
      calls[i]=call;
      calls[i]->operands={arg->expression};
    }
    for (std::size_t i=0; i<calls.size(); ++i) {
      if (!calls[i]) continue;
      c.charge(sizeof(ModelExpression));
      project.keys[i]={inner.id,static_cast<ExprId>(inner.expressions.size())};
      inner.expressions.push_back(std::move(*calls[i]));
    }
    aggregate.query=inner.id;
    aggregate.inputs={sourceId};
    aggregate.outputs=source.outputs;
    project.query=inner.id;
    project.inputs={append(std::move(aggregate))};
    RelNode output;
    output.kind=RelKind::Rename;
    output.query=outer.id;
    output.origin=project.origin;
    output.outputs=project.outputs;
    output.inputs={append(std::move(project))};
    return append(std::move(output));
  }
  RelId expandDistinctAggregates(RelId id,RelNode project) {
    using namespace compiler;
    if (plan.program.sourceDialect!=Dialect::PostgreSQL || project.kind!=RelKind::Project ||
        project.inputs.size()!=1 || project.keys.empty() || project.keys.size()>64 ||
        project.keys.size()!=project.outputs.size())
      return id;
    auto group=plan.nodes.at(project.inputs.front());
    if (group.kind!=RelKind::Aggregate || group.query!=project.query || group.distinct ||
        group.inputs.size()!=1 || group.keys.size()>64 || !group.groupingSets.empty())
      return id;
    auto &query=plan.program.queries.at(group.query-1);
    if (!query.catalogChecked || query.having || query.selectInto || !query.groupingSets.empty() ||
        query.expressions.size()+project.keys.size()*2>c.options.maxNodes ||
        std::any_of(query.orderBy.begin(),query.orderBy.end(),[&](const auto &item) {
          return !modelExpression(plan,{query.id,item.expression}).outputColumn;
        }))
      return id;
    const auto &source=plan.nodes.at(group.inputs.front());
    if (source.query!=query.id || (source.kind!=RelKind::Scan && source.kind!=RelKind::Filter &&
                                  source.kind!=RelKind::Project))
      return id;
    RelNode columnsBeforeDistinct;
    columnsBeforeDistinct.kind=RelKind::Project;
    columnsBeforeDistinct.query=query.id;
    columnsBeforeDistinct.origin=group.origin;
    columnsBeforeDistinct.inputs=group.inputs;
    const auto addColumn=[&](ExpressionRef ref)->std::optional<ColumnRef> {
      if (ref.query!=query.id) return {};
      const auto &expr=modelExpression(plan,ref);
      if (expr.outputColumn && *expr.outputColumn<query.projections.size())
        ref.expression=query.projections[*expr.outputColumn].expression;
      const auto col=boundScalarColumn(plan,ref);
      if (!col || !containsOutput(source,*col) ||
          !signedIntegerBits(plan.program.types.get(modelExpression(plan,ref).sqlType)))
        return {};
      if (!containsOutput(columnsBeforeDistinct,*col)) {
        columnsBeforeDistinct.keys.push_back(ref);
        columnsBeforeDistinct.outputs.push_back(*col);
      }
      return col;
    };
    for (const auto ref:group.keys) {
      c.stop();
      if (!addColumn(ref)) return id;
    }
    const auto groupColumns=columnsBeforeDistinct.outputs;
    std::vector<std::size_t> aggregates;
    std::optional<ColumnRef> argument;
    for (std::size_t i=0;i<project.keys.size();++i) {
      c.stop();
      const auto ref=project.keys[i];
      if (ref.query!=query.id) return id;
      const auto &expr=modelExpression(plan,ref);
      const auto grouped=boundScalarColumn(plan,ref);
      if (grouped && std::any_of(groupColumns.begin(),groupColumns.end(),[&](const auto &key) {
            return same(key,*grouped);
          }))
        continue;
      const bool filtered = expr.kind == ExprKind::AggregateFilter;
      if (filtered && expr.operands.size() != 2) return id;
      const auto &call = filtered ? query.expressions.at(expr.operands.front()) : expr;
      const auto op = builtinOperator(Dialect::PostgreSQL,call.name,call.nameQuoted,call.operands.size());
      if (call.kind!=ExprKind::Call || !call.distinct || call.window || call.operands.size()!=1 ||
          !op || op->kind!=RoutineKind::Aggregate ||
          (op->name!="COUNT" && op->name!="MIN" && op->name!="MAX" && op->name!="SUM" && op->name!="AVG") ||
          (call.routineResolved && (call.routineSchema!="pg_catalog" ||
          call.routineKind!=RoutineKind::Aggregate || call.routineVolatility!=FunctionVolatility::Immutable)))
        return id;
      const auto column=addColumn({query.id,call.operands.front()});
      if (!column || (argument && !same(*argument,*column))) return id;
      argument=column;
      aggregates.push_back(i);
    }
    if (aggregates.empty()) return id;
    for (const auto ordinal : aggregates) {
      c.stop();
      const auto &expr = modelExpression(plan, project.keys[ordinal]);
      if (expr.kind != ExprKind::AggregateFilter) continue;
      const auto fields = totalPredicateColumns(plan, {query.id, expr.operands[1]});
      // A filter on the retained tuple has the same truth value for copies
      // of that tuple. Keep FILTER above deduplication so groups that have no
      // matching rows still produce their original aggregate result.
      if (!fields || !std::all_of(fields->begin(), fields->end(), [&](const auto &field) {
            return containsOutput(columnsBeforeDistinct, field);
          }))
        return id;
    }
    const auto appendExpression = [&](ModelExpression expression) {
      c.charge(sizeof(expression) + expression.operands.size() * sizeof(ExprId) +
               expression.name.size() * sizeof(std::string));
      for (const auto &name : expression.name) c.charge(name.size());
      const auto id = static_cast<ExprId>(query.expressions.size());
      query.expressions.push_back(std::move(expression));
      return id;
    };
    for (const auto ordinal : aggregates) {
      c.stop();
      auto expr = modelExpression(plan, project.keys[ordinal]);
      if (expr.kind == ExprKind::AggregateFilter) {
        auto call = query.expressions.at(expr.operands.front());
        call.distinct = false;
        expr.operands.front() = appendExpression(std::move(call));
      } else {
        expr.distinct = false;
      }
      project.keys[ordinal].expression = appendExpression(std::move(expr));
    }
    auto deduplicate=columnsBeforeDistinct;
    deduplicate.kind=RelKind::Aggregate;
    deduplicate.distinct=true;
    deduplicate.inputs={append(std::move(columnsBeforeDistinct))};
    group.outputs=deduplicate.outputs;
    group.inputs={append(std::move(deduplicate))};
    project.inputs={append(std::move(group))};
    return append(std::move(project));
  }
  RelId pruneAggregateOutputs(RelId id,RelNode project) {
    using namespace compiler;
    if (plan.program.sourceDialect!=Dialect::PostgreSQL || project.kind!=RelKind::Project ||
        project.inputs.size()!=1 || project.keys.empty() || project.keys.size()>64 ||
        project.keys.size()!=project.outputs.size())
      return id;
    const auto &derived=plan.nodes.at(project.inputs.front());
    const auto &query=modelQuery(plan,project.query);
    if (derived.kind!=RelKind::Subquery || derived.query!=query.id || !derived.table ||
        derived.inputs.size()!=1 || derived.materialized || derived.correlated || derived.scalar ||
        derived.outputs.size()>128 || !query.catalogChecked ||
        std::any_of(query.orderBy.begin(),query.orderBy.end(),[&](const auto &item) {
          return !modelExpression(plan,{query.id,item.expression}).outputColumn;
        }))
      return id;
    const auto &binding=query.tables.at(*derived.table);
    auto aggregates=plan.nodes.at(derived.inputs.front());
    if (!binding.query || !binding.name.empty() || aggregates.kind!=RelKind::Project ||
        aggregates.query!=binding.query || aggregates.inputs.size()!=1 ||
        aggregates.outputs.size()!=derived.outputs.size() ||
        aggregates.keys.size()!=aggregates.outputs.size())
      return id;
    const auto &group=plan.nodes.at(aggregates.inputs.front());
    const auto &inner=modelQuery(plan,aggregates.query);
    if (group.kind!=RelKind::Aggregate || group.distinct || group.query!=aggregates.query ||
        !inner.catalogChecked || inner.limit || inner.offset || inner.having ||
        !inner.orderBy.empty())
      return id;
    std::vector<bool> needed(derived.outputs.size());
    for (const auto key:project.keys) {
      c.stop();
      const auto col=boundScalarColumn(plan,key);
      if (!col || col->query!=query.id || col->table!=derived.table) return id;
      const auto ordinal=modelExpression(plan,key).binding->column;
      if (ordinal>=derived.outputs.size() || !same(derived.outputs[ordinal],*col)) return id;
      needed[ordinal]=true;
    }
    RelNode columns;
    columns.kind=RelKind::Rename;
    columns.query=query.id;
    columns.origin=derived.origin;
    auto result=aggregates;
    result.keys.clear();
    result.outputs.clear();
    for (std::size_t i=0;i<needed.size();++i) {
      c.stop();
      const auto ref=aggregates.keys[i];
      if (needed[i]) {
        columns.outputs.push_back(derived.outputs[i]);
        result.keys.push_back(ref);
        result.outputs.push_back(aggregates.outputs[i]);
        continue;
      }
      if (boundScalarColumn(plan,ref)) continue;
      if (nativeExtremum(ref).empty()) return id;
      const ExpressionRef arg{ref.query,modelExpression(plan,ref).operands.front()};
      if (!boundScalarColumn(plan,arg) || !sameIntegerType(plan,ref,arg) ||
          !signedIntegerBits(plan.program.types.get(modelExpression(plan,arg).sqlType)))
        return id;
    }
    if (result.outputs.size()==aggregates.outputs.size()) return id;
    columns.inputs={append(std::move(result))};
    project.inputs={append(std::move(columns))};
    return append(std::move(project));
  }
  RelId preaggregateJoinInput(RelId id,RelNode project) {
    using namespace compiler;
    if (plan.program.sourceDialect!=Dialect::PostgreSQL || project.kind!=RelKind::Project ||
        project.inputs.size()!=1 || project.keys.empty() || project.keys.size()>64 ||
        project.keys.size()!=project.outputs.size())
      return id;
    auto aggregate=plan.nodes.at(project.inputs.front());
    const auto &query=modelQuery(plan,project.query);
    if (aggregate.kind!=RelKind::Aggregate || aggregate.query!=project.query || aggregate.distinct ||
        aggregate.inputs.size()!=1 || aggregate.keys.size()>64 || !aggregate.groupingSets.empty() ||
        !query.catalogChecked || query.having || query.selectInto || !query.groupingSets.empty() ||
        std::any_of(query.orderBy.begin(),query.orderBy.end(),[&](const auto &item) {
          return !modelExpression(plan,{query.id,item.expression}).outputColumn;
        }))
      return id;
    auto join=plan.nodes.at(aggregate.inputs.front());
    if (join.kind!=RelKind::Join || join.join!=JoinKind::Inner || join.distinct || join.correlated ||
        join.fixedOrder || join.query!=query.id || join.inputs.size()!=2 || join.predicates.size()!=1)
      return id;
    const auto equality=integerEqualityColumns(plan,join.predicates.front());
    if (!equality || join.predicates.front().query!=query.id) return id;
    const auto &on=modelExpression(plan,join.predicates.front());
    const auto &left=plan.nodes.at(join.inputs.front()), &right=plan.nodes.at(join.inputs.back());
    const auto &rawLeft=left.kind==RelKind::Filter && left.inputs.size()==1
        ?plan.nodes.at(left.inputs.front()):left;
    const auto &rawRight=right.kind==RelKind::Filter && right.inputs.size()==1
        ?plan.nodes.at(right.inputs.front()):right;
    const auto repeatable=[&](const RelNode &node) {
      if (node.kind!=RelKind::Scan || !node.table || node.query!=query.id) return false;
      const auto &def=query.tables.at(*node.table).definition;
      return def && def->kind==ObjectKind::Table && def->noDescendantsKnown &&
          def->rowSecurityDisabledKnown;
    };
    if (!repeatable(rawLeft) || !repeatable(rawRight)) return id;
    const auto aggregateSource=properties(join.inputs.front()).mayHaveEffects;
    const auto otherSource=properties(join.inputs.back()).mayHaveEffects;
    if (aggregateSource || otherSource) return id;

    using State=std::pair<ColumnRef,ExpressionRef>;
    std::map<std::size_t,State> grouped, states;
    const auto originalKey=[&](ExpressionRef ref) {
      const auto &value=modelExpression(plan,ref);
      if (ref.query==query.id && value.outputColumn && *value.outputColumn<query.projections.size())
        ref.expression=query.projections[*value.outputColumn].expression;
      return ref;
    };
    const auto groupColumn=[&](ExpressionRef ref) {
      const auto column=boundScalarColumn(plan,ref);
      if (!column || !containsOutput(left,*column)) return false;
      const auto ordinal=modelExpression(plan,ref).binding->column;
      grouped.try_emplace(ordinal,State{*column,ref});
      return true;
    };
    for (auto operand:on.operands) groupColumn({query.id,operand});
    if (grouped.size()!=1) return id;
    std::vector<ColumnRef> groupKeys;
    for (auto ref:aggregate.keys) {
      c.stop();
      ref=originalKey(ref);
      const auto col=boundScalarColumn(plan,ref);
      if (!col || !signedIntegerBits(plan.program.types.get(modelExpression(plan,ref).sqlType)) ||
          (!groupColumn(ref) && !containsOutput(right,*col)))
        return id;
      groupKeys.push_back(*col);
    }
    for (auto ref:project.keys) {
      c.stop();
      const auto column=boundScalarColumn(plan,ref);
      if (column && std::any_of(groupKeys.begin(),groupKeys.end(),[&](const auto &group){return same(group,*column);}))
        continue;
      const auto op=nativeExtremum(ref);
      if (ref.query!=query.id || op.empty()) return id;
      const ExpressionRef arg{ref.query,modelExpression(plan,ref).operands.front()};
      const auto field=boundScalarColumn(plan,arg);
      if (!field || !containsOutput(left,*field) || !sameIntegerType(plan,ref,arg) ||
          !signedIntegerBits(plan.program.types.get(modelExpression(plan,arg).sqlType)))
        return id;
      const auto ordinal=modelExpression(plan,arg).binding->column;
      if (grouped.count(ordinal)) continue;
      const auto iter=states.try_emplace(ordinal,State{*field,ref});
      if (!iter.second && op!=nativeExtremum(iter.first->second.second)) return id;
    }
    if (states.empty()) return id;
    c.charge((grouped.size()+states.size())*128);

    auto partial=aggregate;
    partial.inputs={join.inputs.front()};
    partial.outputs=left.outputs;
    partial.keys.clear();
    auto selection=project;
    selection.keys.clear();
    selection.outputs.clear();
    for (const auto &[ordinal,field]:grouped) {
      partial.keys.push_back(field.second);
      selection.keys.push_back(field.second);
      selection.outputs.push_back(field.first);
    }
    for (const auto &[ordinal,field]:states) {
      selection.keys.push_back(field.second);
      selection.outputs.push_back(field.first);
    }
    join.outputs=selection.outputs;
    join.outputs.insert(join.outputs.end(),right.outputs.begin(),right.outputs.end());
    selection.inputs={append(std::move(partial))};
    join.inputs.front()=append(std::move(selection));
    aggregate.outputs=join.outputs;
    aggregate.inputs={append(std::move(join))};
    project.inputs={append(std::move(aggregate))};
    return append(std::move(project));
  }
  RelId mergeExtremumAggregates(RelId id, const RelNode &project) {
    using namespace compiler;
    if(plan.program.sourceDialect!=Dialect::PostgreSQL || project.kind!=RelKind::Project ||
       project.inputs.size()!=1 || project.keys.empty() || project.keys.size()>64 ||
       project.keys.size()!=project.outputs.size())return id;
    const auto &top=plan.nodes.at(project.inputs.front());
    if(top.kind!=RelKind::Aggregate || top.query!=project.query || top.distinct ||
       top.inputs.size()!=1 || !top.groupingSets.empty() || top.keys.size()>64)return id;
    const auto &derived=plan.nodes.at(top.inputs.front());
    if(derived.kind!=RelKind::Subquery || derived.inputs.size()!=1 || derived.query!=top.query ||
       !derived.table || derived.scalar || derived.correlated || derived.materialized)return id;
    const auto &innerProject=plan.nodes.at(derived.inputs.front());
    if(innerProject.kind!=RelKind::Project || innerProject.inputs.size()!=1 ||
       innerProject.keys.size()!=innerProject.outputs.size() ||
       derived.outputs.size()!=innerProject.outputs.size())return id;
    const auto &bottom=plan.nodes.at(innerProject.inputs.front());
    if(bottom.kind!=RelKind::Aggregate || bottom.query!=innerProject.query || bottom.distinct ||
       bottom.inputs.size()!=1 || bottom.keys.empty() || bottom.keys.size()>64 ||
       !bottom.groupingSets.empty())return id;
    const auto &input=plan.nodes.at(bottom.inputs.front());
    const auto &scan=input.kind==RelKind::Filter && input.inputs.size()==1?
        plan.nodes.at(input.inputs.front()):input;
    if(scan.kind!=RelKind::Scan || scan.query!=bottom.query || properties(
        input.kind==RelKind::Filter?input.inputs.front():bottom.inputs.front()).mayHaveEffects)return id;
    const auto &parent=modelQuery(plan,project.query);
    const auto &child=modelQuery(plan,innerProject.query);
    if(!parent.catalogChecked || !child.catalogChecked || parent.tables.size()!=1 ||
       parent.having || child.having || parent.where ||
       parent.selectInto || child.selectInto || !parent.ctes.empty() ||
       !parent.orderBy.empty() || parent.limit || parent.offset)return id;
    if(*derived.table>=parent.tables.size() || parent.tables[*derived.table].query!=child.id)
      return id;
    const auto sameSigned=[&](ExpressionRef left,ExpressionRef right) {
      const auto bits=[&](ExpressionRef value) {
        return signedIntegerBits(plan.program.types.get(modelExpression(plan,value).sqlType));
      };
      const auto width=bits(left);
      return width && width==bits(right);
    };
    const auto inputProjection=[&](ExpressionRef ref)->std::optional<ExpressionRef> {
      if(ref.query!=parent.id)return {};
      const auto &expression=modelExpression(plan,ref);
      if(expression.kind!=ExprKind::Name || !expression.binding || expression.localSymbol ||
         expression.outputColumn || !expression.coalescedBindings.empty() ||
         expression.binding->scope!=parent.id || expression.binding->table!=*derived.table ||
         expression.binding->column>=innerProject.keys.size())
        return {};
      auto input=innerProject.keys[expression.binding->column];
      return input.query==child.id && sameSigned(ref,input)?std::optional<ExpressionRef>{input}:std::nullopt;
    };
    for(auto ref:innerProject.keys) {
      c.stop();
      if(ref.query!=child.id)return id;
      if(boundScalarColumn(plan,ref) || fixedLiteral(ref))continue;
      if(nativeExtremum(ref).empty())return id;
      const ExpressionRef operand{child.id,modelExpression(plan,ref).operands.front()};
      if(!boundScalarColumn(plan,operand) || !sameSigned(ref,operand))return id;
    }
    std::vector<ColumnRef> bottomGroup, mergedGroup;
    c.charge((bottom.keys.size()+top.keys.size())*sizeof(ColumnRef));
    for(auto key:bottom.keys) {
      c.stop();
      const auto col=boundScalarColumn(plan,key);
      if(!col || col->query!=child.id)return id;
      bottomGroup.push_back(*col);
    }
    auto combined=bottom;
    combined.keys.clear();
    for(auto key:top.keys) {
      c.stop();
      auto mapped=inputProjection(key);
      const auto column=mapped?boundScalarColumn(plan,*mapped):std::nullopt;
      if(!column || std::none_of(bottomGroup.begin(),bottomGroup.end(),[&](const auto &item){return same(*column,item);}))
        return id;
      mergedGroup.push_back(*column);
      combined.keys.push_back(*mapped);
    }
    auto merged=project;
    merged.query=child.id;
    bool foundExtremum=false;
    for(auto &result:merged.keys) {
      c.stop();
      if(const auto mapped=inputProjection(result)) {
        const auto column=boundScalarColumn(plan,*mapped);
        if(!column || std::none_of(mergedGroup.begin(),mergedGroup.end(),[&](const auto &item){return same(*column,item);}))
          return id;
        result=*mapped;
        continue;
      }
      const auto op=nativeExtremum(result);
      if(op.empty() || result.query!=parent.id)return id;
      const auto source=inputProjection({parent.id,modelExpression(plan,result).operands.front()});
      if(!source || nativeExtremum(*source)!=op || !sameSigned(result,*source))return id;
      const ExpressionRef operand{child.id,modelExpression(plan,*source).operands.front()};
      if(!boundScalarColumn(plan,operand) || !sameSigned(*source,operand))return id;
      result=*source;
      foundExtremum=true;
    }
    if(!foundExtremum)return id;
    merged.inputs={append(std::move(combined))};
    return append(std::move(merged));
  }
  RelId reduceBigintAverage(RelId id,const RelNode &project) {
    using namespace compiler;
    if (plan.program.sourceDialect!=Dialect::PostgreSQL || project.kind!=RelKind::Project ||
        project.inputs.size()!=1 || project.keys.empty() || project.keys.size()>64 ||
        project.keys.size()!=project.outputs.size()) return id;
    const auto &group=plan.nodes.at(project.inputs.front());
    if (group.kind!=RelKind::Aggregate || group.query!=project.query || group.distinct ||
        group.inputs.size()!=1 || !group.groupingSets.empty()) return id;
    auto &q=plan.program.queries.at(project.query-1);
    if (!q.catalogChecked || q.having || !q.groupingSets.empty() || !q.namedWindows.empty() ||
        q.expressions.size()+project.keys.size()*3>c.options.maxNodes) return id;
    for (const auto &order:q.orderBy) {
      const auto &key=q.expressions.at(order.expression);
      if (!key.outputColumn || *key.outputColumn>=project.keys.size()) return id;
    }
    using Domain=std::pair<Binding,bool>;
    struct Terms {ExprId sum=0,count=0;};
    struct Rewrite {std::size_t slot; Domain domain;};
    std::map<Domain,Terms> shared;
    std::vector<Rewrite> averages;
    c.charge(project.keys.size()*(sizeof(Rewrite)+sizeof(Domain)+sizeof(Terms)+3*sizeof(void *)));
    for (std::size_t i=0;i<project.keys.size();++i) {
      c.stop();
      const auto ref=project.keys[i];
      const auto &call=modelExpression(plan,ref);
      if (ref.query!=q.id || call.kind!=ExprKind::Call || call.window || call.query ||
          call.operands.size()!=1) continue;
      const auto routine=builtinOperator(Dialect::PostgreSQL,call.name,call.nameQuoted,1);
      if (!routine || routine->kind!=RoutineKind::Aggregate ||
          (call.routineResolved && (call.routineSchema!="pg_catalog" ||
            call.routineKind!=RoutineKind::Aggregate || call.routineVolatility!=FunctionVolatility::Immutable)))
        continue;
      const auto &name=routine->name;
      if (name!="AVG" && name!="SUM" && name!="COUNT") continue;
      const ExpressionRef operand{q.id,call.operands.front()};
      const auto argument=columnIdentity(operand);
      const auto &type=plan.program.types.get(modelExpression(plan,operand).sqlType);
      if (!argument || !boundScalarColumn(plan,operand) || signedIntegerBits(type)!=64) continue;
      if (plan.program.types.get(call.sqlType)!=resolveAggregateType(Dialect::PostgreSQL,name,type).type)
        continue;
      const Domain domain{*argument,call.distinct};
      auto &terms=shared[domain];
      if (name=="AVG") averages.push_back({i,domain});
      if (name=="SUM") terms.sum=ref.expression;
      if (name=="COUNT") terms.count=ref.expression;
    }
    if (averages.empty()) return id;
    const auto typeBytes=plan.program.types.accountedBytes();
    const auto intType=plan.program.types.intern(resolveAggregateType(Dialect::PostgreSQL,"COUNT",{}).type);
    c.charge(plan.program.types.accountedBytes()-typeBytes);
    const auto add=[&](ModelExpression e) {
      c.charge(sizeof(e)+e.name.size()*sizeof(std::string)+32+e.operands.size()*sizeof(ExprId));
      const auto expr=static_cast<ExprId>(q.expressions.size());
      q.expressions.push_back(std::move(e));
      return expr;
    };
    auto result=project;
    for (const auto &rewrite:averages) {
      c.stop();
      const auto &avg=modelExpression(plan,project.keys[rewrite.slot]);
      const auto origin=avg.origin;
      const auto argument=avg.operands.front();
      const auto resultType=avg.sqlType;
      const auto resultCategory=avg.type;
      const bool distinct=avg.distinct;
      auto &terms=shared.at(rewrite.domain);
      const auto aggregate=[&](const char *name,TypeId type,ValueType category) {
        ModelExpression call;
        call.kind=ExprKind::Call;
        call.name={"pg_catalog",name};
        call.origin=origin;
        call.operands={argument};
        call.sqlType=type;
        call.type=category;
        call.distinct=distinct;
        return add(std::move(call));
      };
      if (!terms.sum) terms.sum=aggregate("sum",resultType,resultCategory);
      if (!terms.count) terms.count=aggregate("count",intType,ValueType::Integer);
      ModelExpression division;
      division.kind=ExprKind::Call;
      division.name={"pg_catalog","numeric_div"};
      division.origin=origin;
      division.operands={terms.sum,terms.count};
      division.sqlType=resultType;
      division.type=resultCategory;
      result.keys[rewrite.slot].expression=add(std::move(division));
    }
    return append(std::move(result));
  }
  RelId splitGroupingSets(RelId id, const RelNode &project) {
    using namespace compiler;
    if (plan.program.sourceDialect != Dialect::PostgreSQL || project.kind != RelKind::Project ||
        project.inputs.size() != 1 || project.keys.empty() || project.keys.size() > 32 ||
        project.keys.size() != project.outputs.size()) return id;
    auto aggregateId = project.inputs.front();
    const auto &queryShape = modelQuery(plan, project.query);
    std::optional<RelNode> having;
    if (queryShape.having) {
      const auto &filter = plan.nodes.at(aggregateId);
      if (filter.kind != RelKind::Filter || filter.query != project.query ||
          filter.inputs.size() != 1 || filter.predicates.empty()) return id;
      having = filter;
      aggregateId = filter.inputs.front();
    }
    const auto aggregate = plan.nodes.at(aggregateId);
    if (aggregate.kind != RelKind::Aggregate || aggregate.query != project.query ||
        aggregate.distinct || aggregate.inputs.size() != 1 ||
        aggregate.groupingSets.size() < 2 || aggregate.groupingSets.size() > 32 ||
        aggregate.keys.size() > 32) return id;
    auto &query = plan.program.queries.at(project.query - 1);
    if (!query.catalogChecked || query.selectInto || !query.namedWindows.empty() ||
        query.projections.size() != project.keys.size() || properties(aggregate.inputs.front()).mayHaveEffects)
      return id;
    // At the outer sort, only keys already present in the output are usable.
    for (const auto &order : query.orderBy) {
      const auto &key = query.expressions.at(order.expression);
      if (!key.outputColumn || *key.outputColumn >= project.keys.size()) return id;
    }

    const auto &input = plan.nodes.at(aggregate.inputs.front());
    const auto scalar = [&](ExpressionRef ref) -> std::optional<Binding> {
      if (ref.query != query.id) return {};
      const auto column = boundScalarColumn(plan, ref);
      if (!column || !containsOutput(input, *column)) return {};
      const auto &expr = modelExpression(plan, ref);
      return postgresGroupingType(plan.program.types.get(expr.sqlType)).empty() ? std::nullopt : columnIdentity(ref);
    };
    c.charge(aggregate.keys.size() * (sizeof(Binding) + sizeof(std::size_t) + 3 * sizeof(void *)) +
             aggregate.groupingSets.size() * sizeof(std::uint32_t));
    std::map<Binding,std::size_t> positions;
    for (const auto key : aggregate.keys) {
      c.stop();
      const auto binding = scalar(key);
      if (!binding) return id;
      positions.try_emplace(*binding, positions.size());
    }
    std::vector<std::uint32_t> setMasks;
    for (const auto &set : aggregate.groupingSets) {
      c.stop();
      std::uint32_t mask = 0;
      for (const auto key : set) {
        const auto binding = scalar(key);
        if (!binding) return id;
        const auto pos = positions.find(*binding);
        if (pos == positions.end()) return id;
        mask |= std::uint32_t(1) << pos->second;
      }
      setMasks.push_back(mask);
    }
    struct Rewrite {
      ExprId expression;
      std::optional<std::size_t> groupColumn;
      std::vector<std::size_t> groupingArguments;
    };
    std::vector<Rewrite> rewrites;
    std::set<ExprId> verified;
    const auto check = [&](const auto &checkChild, ExpressionRef ref, bool predicate,
                           std::size_t depth) -> bool {
      c.stop();
      if (ref.query != query.id || depth >= c.options.maxDepth) return false;
      if (verified.count(ref.expression)) return true;
      const auto &expr = modelExpression(plan, ref);
      Rewrite rewrite{ref.expression, {}, {}};
      if (const auto binding = scalar(ref)) {
        const auto pos = positions.find(*binding);
        if (pos == positions.end()) return false;
        rewrite.groupColumn = pos->second;
      } else if (expr.kind == ExprKind::Call) {
        if (expr.window || expr.query || expr.operands.empty() ||
            (expr.routineResolved && (expr.routineSchema != "pg_catalog" ||
                                     expr.routineVolatility != FunctionVolatility::Immutable))) return false;
        const auto builtin = builtinOperator(Dialect::PostgreSQL, expr.name, expr.nameQuoted, expr.operands.size());
        if (!builtin || (expr.routineResolved && expr.routineKind != builtin->kind)) return false;
        if (builtin->name == "GROUPING") {
          if (expr.distinct || expr.operands.size() > 31) return false;
          c.charge(expr.operands.size() * sizeof(std::size_t));
          rewrite.groupingArguments.reserve(expr.operands.size());
          for (const auto argument : expr.operands) {
            const auto binding = scalar({query.id, argument});
            if (!binding) return false;
            const auto pos = positions.find(*binding);
            if (pos == positions.end()) return false;
            rewrite.groupingArguments.push_back(pos->second);
          }
        } else {
          if (builtin->kind != RoutineKind::Aggregate || expr.operands.size() != 1 ||
              (builtin->name != "COUNT" && builtin->name != "SUM" &&
               builtin->name != "MIN" && builtin->name != "MAX" && builtin->name != "AVG")) return false;
          const ExpressionRef argument{query.id, expr.operands.front()};
          const auto &operand = modelExpression(plan, argument);
          const auto bits = signedIntegerBits(plan.program.types.get(operand.sqlType));
          if (!(builtin->name == "COUNT" && !expr.distinct && operand.kind == ExprKind::Star &&
                operand.name.empty()) && (!bits || !scalar(argument))) return false;
          if ((builtin->name == "SUM" || builtin->name == "AVG") && bits != 64) return false;
          c.charge(sizeof(ExprId) + 3 * sizeof(void *));
          verified.insert(ref.expression);
          return true; // Aggregate operands still read the original input rows.
        }
      } else {
        if (!predicate || expr.window || expr.query || expr.outputColumn) return false;
        const bool literal = (expr.kind == ExprKind::Number &&
                              signedIntegerBits(plan.program.types.get(expr.sqlType))) ||
                             (expr.kind == ExprKind::Boolean && !expr.unresolvedBooleanName) ||
                             expr.kind == ExprKind::Null;
        const auto &op = expr.value;
        const bool binary = expr.kind == ExprKind::Binary && expr.operands.size() == 2 &&
            (op == "AND" || op == "OR" || op == "=" || op == "<>" || op == "!=" ||
             op == "<" || op == "<=" || op == ">" || op == ">=");
        const bool unary = expr.operands.size() == 1 && ((expr.kind == ExprKind::Unary && op == "NOT") ||
            expr.kind == ExprKind::IsNull || expr.kind == ExprKind::TruthTest);
        if (!literal && !binary && !unary) return false;
        for (auto operand : expr.operands)
          if (!checkChild(checkChild, {query.id, operand}, true, depth + 1)) return false;
        if (literal) {
          c.charge(sizeof(ExprId) + 3 * sizeof(void *));
          verified.insert(ref.expression);
          return true;
        }
      }
      c.charge(sizeof(Rewrite) + sizeof(ExprId) + 3 * sizeof(void *));
      verified.insert(ref.expression);
      rewrites.push_back(std::move(rewrite));
      return true;
    };
    for (const auto ref : project.keys)
      if (!check(check, ref, false, 0)) return id;
    if (having)
      for (const auto ref : having->predicates)
        if (!check(check, ref, true, 0)) return id;
    const auto maxExpressions = rewrites.size() * aggregate.groupingSets.size() * 2;
    if (query.expressions.size() + maxExpressions > c.options.maxNodes) return id;

    const auto appendExpr = [&](ModelExpression value) {
      c.charge(sizeof(value) + value.value.size() + value.operands.size() * sizeof(ExprId));
      const auto ref = ExprId(query.expressions.size());
      query.expressions.push_back(std::move(value));
      return ref;
    };
    std::map<ExprId, ExprId> missingColumns;
    std::map<std::pair<ExprId, std::uint32_t>, ExprId> groupingValues;
    RelNode united;
    united.kind = RelKind::SetOperation;
    united.setKind = SetKind::Union;
    united.query = project.query;
    united.origin = project.origin;
    united.outputs = project.outputs;
    for (std::size_t i = 0; i < aggregate.groupingSets.size(); ++i) {
      c.stop();
      auto branch = project;
      std::map<ExprId, ExprId> rewritten;
      const auto replace = [&](ExprId expression) {
        const auto it = rewritten.find(expression);
        return it == rewritten.end() ? expression : it->second;
      };
      const auto present = [&](std::size_t slot) {
        return (setMasks[i] & (std::uint32_t(1) << slot)) != 0;
      };
      for (const auto &action : rewrites) {
        c.stop();
        const auto originalId = action.expression;
        const auto &original = query.expressions.at(originalId);
        ExprId replacement = originalId;
        if (action.groupColumn) {
          if (present(*action.groupColumn)) continue;
          const auto match = missingColumns.find(originalId);
          if (match != missingColumns.end()) {
            replacement = match->second;
          } else {
            const auto type = original.type;
            const auto sqlType = original.sqlType;
            const auto origin = original.origin;
            ModelExpression null;
            null.kind = ExprKind::Null;
            null.type = ValueType::Null;
            null.origin = origin;
            const auto nullRef = appendExpr(std::move(null));
            const auto typeName = postgresGroupingType(plan.program.types.get(sqlType));
            auto target = std::make_shared<BasicTypeSpec<std::vector<std::string>>>();
            c.charge(sizeof(*target) + sizeof(std::string) + typeName.size());
            target->name = {std::string(typeName)};
            ModelExpression cast;
            cast.kind = ExprKind::Cast;
            cast.type = type;
            cast.sqlType = sqlType;
            cast.origin = origin;
            cast.castType = std::move(target);
            cast.operands = {nullRef};
            replacement = appendExpr(std::move(cast));
            c.charge(sizeof(ExprId) * 2 + 3 * sizeof(void *));
            missingColumns.emplace(originalId, replacement);
          }
        } else if (!action.groupingArguments.empty()) {
          std::uint32_t mask = 0;
          for (const auto slot : action.groupingArguments)
            mask = (mask << 1) | std::uint32_t(!present(slot));
          const auto key = std::make_pair(originalId, mask);
          const auto match = groupingValues.find(key);
          if (match != groupingValues.end()) {
            replacement = match->second;
          } else {
            ModelExpression value;
            value.kind = ExprKind::Number;
            value.type = ValueType::Integer;
            value.sqlType = original.sqlType;
            value.origin = original.origin;
            value.value = std::to_string(mask);
            replacement = appendExpr(std::move(value));
            c.charge(sizeof(key) + sizeof(ExprId) + 3 * sizeof(void *));
            groupingValues.emplace(key, replacement);
          }
        } else if (std::any_of(original.operands.begin(), original.operands.end(),
                              [&](ExprId operand) { return replace(operand) != operand; })) {
          auto predicate = original;
          for (auto &operand : predicate.operands) operand = replace(operand);
          replacement = appendExpr(std::move(predicate));
        }
        if (replacement != originalId) {
          c.charge(sizeof(ExprId) * 2 + 3 * sizeof(void *));
          rewritten.emplace(originalId, replacement);
        }
      }
      for (auto &key : branch.keys) key.expression = replace(key.expression);
      auto grouped = aggregate;
      grouped.keys = aggregate.groupingSets[i];
      grouped.groupingSets.clear();
      auto branchInput = append(std::move(grouped));
      if (having) {
        auto filter = *having;
        filter.inputs = {branchInput};
        for (auto &predicate : filter.predicates)
          predicate.expression = replace(predicate.expression);
        branchInput = append(std::move(filter));
      }
      branch.inputs = {branchInput};
      united.inputs.push_back(append(std::move(branch)));
    }
    return append(std::move(united));
  }
  bool pureSortKey(ExpressionRef ref,std::size_t depth=0) {
    if(depth>16)return false;
    const auto &q=modelQuery(plan,ref.query);const auto &e=modelExpression(plan,ref);
    if(e.outputColumn && *e.outputColumn<q.projections.size()) {
      const auto id=q.projections[*e.outputColumn].expression;
      return id!=ref.expression && pureSortKey({q.id,id},depth+1);
    }
    if(columnIdentity(ref))return true;
    if(e.kind==compiler::ExprKind::String || e.kind==compiler::ExprKind::Null ||
       (e.kind==compiler::ExprKind::Boolean && !e.unresolvedBooleanName))return true;
    if(e.kind==compiler::ExprKind::Number)return !e.value.empty() && e.value.size()<=15 && e.value.find_first_not_of("0123456789")==std::string::npos;
    if(e.kind==compiler::ExprKind::Unary && (e.value=="+" || e.value=="-") && e.operands.size()==1)
      return modelExpression(plan,{q.id,e.operands[0]}).kind==compiler::ExprKind::Number && pureSortKey({q.id,e.operands[0]},depth+1);
    return false;
  }
  bool fixedLiteral(ExpressionRef ref, std::size_t depth = 0) {
    using compiler::ExprKind;
    if (depth >= c.options.maxDepth) return false;
    const auto &e = modelExpression(plan, ref);
    if (e.outputColumn) {
      const auto &q = modelQuery(plan, ref.query);
      if (*e.outputColumn >= q.projections.size()) return false;
      const auto projected = q.projections[*e.outputColumn].expression;
      return projected != ref.expression && fixedLiteral({q.id, projected}, depth + 1);
    }
    if (e.kind == ExprKind::Null) return true;
    if (e.kind == ExprKind::Boolean) return !e.unresolvedBooleanName;
    if (e.kind == ExprKind::Number)
      return !e.value.empty() && e.value.size() <= 9 && e.value.find_first_not_of("0123456789") == std::string::npos;
    if (e.kind == ExprKind::Unary && (e.value == "-" || e.value == "+") && e.operands.size() == 1) {
      const ExpressionRef value{ref.query, e.operands.front()};
      return modelExpression(plan, value).kind == ExprKind::Number && fixedLiteral(value, depth + 1);
    }
    return false;
  }
  RelId mergeDirectProjections(RelId id, const RelNode &outer) {
    using namespace compiler;
    if (plan.program.sourceDialect != Dialect::PostgreSQL ||
        outer.kind != RelKind::Project || outer.inputs.size() != 1 ||
        outer.keys.empty() || outer.keys.size() > 64 || outer.keys.size() != outer.outputs.size())
      return id;
    const auto &wrapper = plan.nodes.at(outer.inputs.front());
    if (wrapper.kind != RelKind::Subquery || wrapper.query != outer.query || !wrapper.table ||
        wrapper.inputs.size() != 1 || wrapper.materialized || wrapper.correlated || wrapper.scalar)
      return id;
    const auto &query = modelQuery(plan, outer.query);
    const auto &relation = query.tables.at(*wrapper.table);
    if (!query.catalogChecked || query.selectInto || !query.orderBy.empty() ||
        !relation.query || !relation.definition || !relation.definition->arityKnown)
      return id;
    const auto innerId = wrapper.inputs.front();
    const auto &inner = plan.nodes.at(innerId);
    if (inner.kind != RelKind::Project || inner.query != relation.query || inner.inputs.size() != 1 ||
        inner.keys.empty() || inner.keys.size() > 64 || inner.keys.size() != inner.outputs.size() ||
        inner.outputs.size() != wrapper.outputs.size() || properties(innerId).mayHaveEffects)
      return id;
    const auto &input = plan.nodes.at(inner.inputs.front());
    if (input.query != inner.query || (input.kind != RelKind::Scan &&
        (input.kind != RelKind::Filter || input.inputs.size() != 1 ||
         plan.nodes.at(input.inputs.front()).kind != RelKind::Scan))) return id;
    for (const auto ref : inner.keys) {
      c.stop();
      const auto col = boundScalarColumn(plan, ref);
      if (ref.query != inner.query ||
          !(fixedLiteral(ref) || (col && containsOutput(input, *col)))) return id;
    }
    auto result = outer;
    result.query = inner.query;
    result.inputs = inner.inputs;
    for (auto &ref : result.keys) {
      c.stop();
      const auto col = boundScalarColumn(plan, ref);
      const auto &expr = modelExpression(plan, ref);
      if (!col || col->query != outer.query || col->table != *wrapper.table) return id;
      const auto position = expr.binding->column;
      if (position >= inner.keys.size() || !same(wrapper.outputs.at(position), *col)) return id;
      const auto next = inner.keys.at(position);
      auto before = plan.program.types.get(expr.sqlType);
      auto after = plan.program.types.get(modelExpression(plan, next).sqlType);
      before.nullability = after.nullability = Nullability::Unknown;
      if (before.family == TypeFamily::Unknown || before != after) return id;
      ref = next;
    }
    return append(std::move(result));
  }
  bool exactValuesType(const compiler::SqlType &type) const {
    return signedIntegerBits(type) || (type.family == compiler::TypeFamily::Boolean &&
           type.flavor.empty() && type.opaqueName.empty() && !type.arrayDimensions);
  }
  bool valuesPredicate(ExpressionRef ref, const RelNode &input, std::size_t depth = 0) {
    using namespace compiler;
    c.stop();
    if (depth >= c.options.maxDepth || depth >= 24) return false;
    const auto &e = modelExpression(plan, ref);
    if (e.query || e.window || e.localSymbol || e.outputColumn) return false;
    if (e.kind == ExprKind::Name) {
      const auto column = boundScalarColumn(plan, ref);
      return column && exactValuesType(plan.program.types.get(e.sqlType)) &&
          std::count_if(input.outputs.begin(), input.outputs.end(),
              [&](const auto &out) { return same(out, *column); }) == 1;
    }
    if (fixedLiteral(ref)) return true;
    static const std::set<std::string_view> binary = {
        "AND", "OR", "=", "<>", "!=", "<", "<=", ">", ">=",
        "IS DISTINCT FROM", "IS NOT DISTINCT FROM"};
    const bool comparison = e.kind == ExprKind::Binary && e.operands.size() == 2 && binary.count(e.value);
    const bool negation = e.kind == ExprKind::Unary && e.operands.size() == 1 && e.value == "NOT";
    const bool nullTest = e.kind == ExprKind::IsNull && e.operands.size() == 1 &&
        (e.value == "IS NULL" || e.value == "IS NOT NULL");
    const bool truthTest = e.kind == ExprKind::TruthTest && e.operands.size() == 1;
    const bool between = e.kind == ExprKind::Between && e.operands.size() == 3;
    const bool list = e.kind == ExprKind::In && e.operands.size() >= 2 && e.operands.size() <= 33;
    if (!comparison && !negation && !nullTest && !truthTest && !between && !list) return false;
    return std::all_of(e.operands.begin(), e.operands.end(), [&](auto child) {
      return valuesPredicate({ref.query, child}, input, depth + 1);
    });
  }
  RelId reduceValues(RelId id, const RelNode &operation) {
    using namespace compiler;
    if (plan.program.sourceDialect != Dialect::PostgreSQL ||
        (operation.kind != RelKind::Filter && operation.kind != RelKind::Project) ||
        operation.inputs.size() != 1) return id;
    const auto &input = plan.nodes.at(operation.inputs.front());
    if (!input.table || input.query != operation.query || input.outputs.empty() ||
        input.outputs.size() > 32) return id;
    const RelNode *source = &input;
    if (input.kind == RelKind::Subquery && !input.scalar && !input.correlated &&
        !input.materialized && input.inputs.size() == 1) {
      source = &plan.nodes.at(input.inputs.front());
      if (!modelQuery(plan, source->query).valuesQuery) return id;
    }
    const auto width = input.outputs.size();
    if (source->kind != RelKind::Values || !source->inputs.empty() ||
        !source->valueCount || source->valueCount > 256 || source->outputs.size() != width ||
        source->keys.size() != width * source->valueCount) return id;
    const auto &query = modelQuery(plan, input.query);
    const auto &relation = query.tables.at(*input.table);
    const auto &table = relation.definition;
    if (!table || !table->arityKnown || table->columns.size() != width || !relation.query) return id;
    const auto &values = modelQuery(plan, relation.query);
    if (!values.valuesQuery || values.projections.size() != width) return id;
    std::vector<const SqlType *> types;
    types.reserve(width);
    for (std::size_t i = 0; i < width; ++i) {
      const auto &column = table->columns.at(i);
      const auto &projection = values.expressions.at(values.projections.at(i).expression);
      const auto &type = plan.program.types.get(projection.sqlType);
      if (!exactValuesType(type) ||
          !same(input.outputs.at(i), {input.query, *input.table, column.name})) return id;
      types.push_back(&type);
    }
    // Every source value must be literal and already have the output column's
    // PostgreSQL domain. Implicit widening, collations and effectful cells
    // cannot be removed or pre-evaluated using this rule.
    for (std::size_t i = 0; i < source->keys.size(); ++i) {
      c.stop();
      const auto ref = source->keys.at(i);
      const auto &cell = modelExpression(plan, ref);
      if (cell.outputColumn || !fixedLiteral(ref)) return id;
      if (cell.kind == ExprKind::Null) continue;
      const auto &actual = plan.program.types.get(cell.sqlType);
      const auto &expected = *types.at(i % width);
      if (actual.family != expected.family || actual.bits != expected.bits ||
          !exactValuesType(actual)) return id;
    }
    RelNode out;
    out.kind = RelKind::Values;
    out.query = operation.query;
    out.origin = operation.origin;
    out.outputs = operation.outputs;
    if (operation.kind == RelKind::Filter) {
      if (!columns(operation.outputs, input.outputs) || operation.predicates.empty() ||
          operation.predicates.size() > 32 ||
          !std::all_of(operation.predicates.begin(), operation.predicates.end(), [&](auto pred) {
            return valuesPredicate(pred, input);
          })) return id;
      const auto retained = evaluateLiteralFilter(c, operation, input, *source);
      if (!retained) return id;
      // The alias binding is still available for a succeeding projection.
      out.table = input.table;
      out.valueCount = retained->size();
      c.charge(out.valueCount * width * sizeof(ExpressionRef));
      out.keys.reserve(out.valueCount * width);
      for (const auto row : *retained) {
        const auto begin = source->keys.begin() + row * width;
        out.keys.insert(out.keys.end(), begin, begin + width);
      }
    } else {
      if (operation.keys.empty() || operation.keys.size() > 32 ||
          operation.keys.size() != operation.outputs.size()) return id;
      std::vector<std::size_t> ordinals;
      ordinals.reserve(operation.keys.size());
      for (const auto key : operation.keys) {
        const auto column = boundScalarColumn(plan, key);
        if (!column || column->query != input.query || column->table != *input.table) return id;
        const auto position = modelExpression(plan, key).binding->column;
        if (position >= width || !same(input.outputs.at(position), *column)) return id;
        ordinals.push_back(position);
      }
      out.valueCount = source->valueCount;
      c.charge(out.valueCount * ordinals.size() * sizeof(ExpressionRef));
      out.keys.reserve(out.valueCount * ordinals.size());
      for (std::size_t i = 0; i < out.valueCount; ++i) {
        c.stop();
        for (const auto ordinal : ordinals) out.keys.push_back(source->keys[i * width + ordinal]);
      }
    }
    preserveValuesTypes(id,out);
    return append(std::move(out));
  }
  // A zero expression ID represents a fixed SQL NULL.
  std::optional<std::map<Binding,compiler::ExprId>> columnsFixedByFilter(RelId input, compiler::QueryId query) {
    using namespace compiler;
    const auto &q = modelQuery(plan, query);
    if (!q.catalogChecked) return {};
    std::map<Binding,ExprId> known;
    const auto integerColumn = [&](ExpressionRef ref) -> std::optional<Binding> {
      const auto identity = columnIdentity(ref);
      if (!identity || std::get<0>(*identity) != query || !boundScalarColumn(plan, ref) ||
          !signedIntegerBits(plan.program.types.get(modelExpression(plan, ref).sqlType))) return {};
      return identity;
    };
    const auto integerLiteral = [&](ExpressionRef ref) {
      const auto &value = modelExpression(plan, ref);
      return !value.outputColumn && fixedLiteral(ref) &&
          signedIntegerBits(plan.program.types.get(value.sqlType)).has_value();
    };
    for (std::size_t depth = 0; depth < c.options.maxDepth; ++depth) {
      c.stop();
      const auto &node = plan.nodes.at(input);
      if (node.query != query) return {};
      if (node.kind == RelKind::Scan && node.table && *node.table < q.tables.size()) {
        const auto &definition = q.tables[*node.table].definition;
        if (!definition || definition->kind != ObjectKind::Table ||
            !definition->complete || !definition->arityKnown) return {};
        for (const auto &column : known) if (std::get<1>(column.first) != *node.table) return {};
        return known;
      }
      if (node.inputs.size() != 1) return {};
      input = node.inputs.front();
      if (node.kind == RelKind::Project) {
        for (auto key : node.keys) {
          c.stop();
          if (key.query != query || (!fixedLiteral(key) && !boundScalarColumn(plan, key))) return {};
        }
        continue;
      }
      if (node.kind != RelKind::Filter) return {};
      for (const auto ref : node.predicates) {
        c.stop();
        if (ref.query != query) return {};
        const auto &e = modelExpression(plan, ref);
        std::optional<Binding> fixed;
        ExprId constant=0;
        if (e.kind == ExprKind::IsNull && e.value == "IS NULL" && e.operands.size() == 1) {
          fixed = integerColumn({query, e.operands.front()});
        } else if (e.kind == ExprKind::Binary && e.value == "=" && e.operands.size() == 2) {
          const ExpressionRef left{query, e.operands.front()}, right{query, e.operands.back()};
          if (integerLiteral(left)) {
            fixed=integerColumn(right);
            constant=left.expression;
          } else if (integerLiteral(right)) {
            fixed=integerColumn(left);
            constant=right.expression;
          }
        }
        // Every restriction on this path must be a total, verified predicate.
        if (!fixed) return {};
        if (known.try_emplace(*fixed,constant).second)
          c.charge(sizeof(Binding) + sizeof(ExprId) + 3 * sizeof(void *));
      }
    }
    return {};
  }
  RelId pullUpConstantGroupKeys(RelId id,const RelNode &project) {
    using namespace compiler;
    if (plan.program.sourceDialect!=Dialect::PostgreSQL || project.kind!=RelKind::Project ||
        project.inputs.size()!=1 || project.keys.empty() || project.keys.size()>64 ||
        project.keys.size()!=project.outputs.size()) return id;
    auto group=plan.nodes.at(project.inputs.front());
    if (group.kind!=RelKind::Aggregate || group.query!=project.query || group.inputs.size()!=1 ||
        group.distinct || !group.groupingSets.empty() || group.keys.size()<2 || group.keys.size()>64) return id;
    auto &q=plan.program.queries.at(project.query-1);
    if (!q.catalogChecked || q.having || !q.groupingSets.empty() || !q.namedWindows.empty() ||
        q.projections.size()!=project.keys.size() ||
        q.expressions.size()+project.keys.size()*2>c.options.maxNodes) return id;
    const auto fixed=columnsFixedByFilter(group.inputs.front(),q.id);
    if (!fixed || fixed->empty()) return id;
    for (const auto &order:q.orderBy) {
      const auto &key=q.expressions.at(order.expression);
      if (!key.outputColumn || *key.outputColumn>=project.keys.size()) return id;
    }
    auto remaining=group;
    remaining.keys.clear();
    std::vector<Binding> removed;
    c.charge(group.keys.size() * (sizeof(Binding) + sizeof(ExpressionRef)));
    for (auto key:group.keys) {
      c.stop();
      const auto binding=columnIdentity(key);
      if (key.query!=q.id || !binding || !boundScalarColumn(plan,key) ||
          !signedIntegerBits(plan.program.types.get(modelExpression(plan,key).sqlType))) return id;
      if (fixed->count(*binding)) removed.push_back(*binding);
      else remaining.keys.push_back(key);
    }
    if (removed.empty()) return id;
    if (remaining.keys.empty()) {
      // Ordinary GROUP BY over zero rows yields zero groups. A global group
      // would yield one, so even a wholly fixed grouping keeps one key.
      remaining.keys.push_back(group.keys.front());
      const auto retained=removed.front();
      removed.erase(std::remove(removed.begin(),removed.end(),retained),removed.end());
      if (removed.empty()) return id;
    }

    struct RestoreConstant { std::size_t slot; ExprId value; int bits; };
    std::vector<RestoreConstant> constants;
    c.charge(project.keys.size()*sizeof(RestoreConstant));
    for (std::size_t i=0;i<project.keys.size();++i) {
      c.stop();
      const auto ref=project.keys[i];
      const auto &old=modelExpression(plan,ref);
      if (ref.query!=q.id || old.query || old.window || old.localSymbol) return id;
      if (const auto identity=columnIdentity(ref)) {
        if (std::find(removed.begin(),removed.end(),*identity)==removed.end()) continue;
        const auto bits=signedIntegerBits(plan.program.types.get(old.sqlType));
        if (!bits) return id;
        const auto constant=fixed->at(*identity);
        if (constant && *bits==16 && q.expressions.at(constant).sqlType!=old.sqlType) {
          const auto &literal=q.expressions.at(constant);
          const bool negative=literal.kind==ExprKind::Unary && literal.value=="-";
          const auto &digits=literal.kind==ExprKind::Unary
              ? q.expressions.at(literal.operands.front()).value:literal.value;
          unsigned value=0;
          const auto end=digits.data()+digits.size();
          const auto parsed=std::from_chars(digits.data(),end,value);
          if (parsed.ec!=std::errc{} || parsed.ptr!=end || value>(negative?32768u:32767u)) return id;
        }
        constants.push_back({i,constant,*bits});
        continue;
      }
      if (old.kind!=ExprKind::Call || old.operands.size()!=1) return id;
      const auto builtin=builtinOperator(Dialect::PostgreSQL,old.name,old.nameQuoted,1);
      if (!builtin || builtin->kind!=RoutineKind::Aggregate ||
          (old.routineResolved && (old.routineSchema!="pg_catalog" ||
            old.routineKind!=RoutineKind::Aggregate || old.routineVolatility!=FunctionVolatility::Immutable)) ||
          (builtin->name!="COUNT" && builtin->name!="MIN" && builtin->name!="MAX" && builtin->name!="SUM"))
        return id;
      const ExpressionRef arg{q.id,old.operands.front()};
      const auto &value=modelExpression(plan,arg);
      if (builtin->name=="COUNT" && !old.distinct && value.kind==ExprKind::Star && value.name.empty()) continue;
      if (!boundScalarColumn(plan,arg) || !signedIntegerBits(plan.program.types.get(value.sqlType))) return id;
    }
    auto projection=project;
    for (auto [slot,constant,bits]:constants) {
      const auto &before=modelExpression(plan,project.keys[slot]);
      const auto origin=before.origin;
      const auto outputType=before.sqlType;
      if (constant && q.expressions.at(constant).sqlType==outputType) {
        projection.keys[slot].expression=constant;
        continue;
      }
      if (!constant) {
        ModelExpression null;
        null.kind=ExprKind::Null;
        null.origin=origin;
        null.type=ValueType::Null;
        constant=static_cast<ExprId>(q.expressions.size());
        c.charge(sizeof(null));
        q.expressions.push_back(std::move(null));
      }
      auto target=std::make_shared<BasicTypeSpec<std::vector<std::string>>>();
      target->name={bits==16?"smallint":bits==32?"integer":"bigint"};
      ModelExpression cast;
      cast.kind=ExprKind::Cast;
      cast.origin=origin;
      cast.type=ValueType::Integer;
      cast.sqlType=outputType;
      cast.castType=std::move(target);
      cast.operands={constant};
      c.charge(sizeof(cast)+sizeof(*cast.castType)+sizeof(std::string)+sizeof(ExprId)+8);
      projection.keys[slot].expression=static_cast<ExprId>(q.expressions.size());
      q.expressions.push_back(std::move(cast));
    }
    projection.inputs={append(std::move(remaining))};
    return append(std::move(projection));
  }
  RelId removeConstantSortKeys(RelId id, const RelNode &sort) {
    if (plan.program.sourceDialect != compiler::Dialect::PostgreSQL || sort.kind != RelKind::Sort ||
        sort.keys.empty() || sort.descending.size() != sort.keys.size() ||
        sort.nullOrdering.size() != sort.keys.size()) return id;
    const auto known = columnsFixedByFilter(sort.inputs.front(), sort.query);
    if (!known && properties(sort.inputs.front()).mayHaveEffects) return id;
    c.charge(sort.keys.size() * (sizeof(ExpressionRef) + sizeof(std::string) + sizeof(bool)));
    auto replacement = sort;
    replacement.keys.clear();
    replacement.descending.clear();
    replacement.nullOrdering.clear();
    for (std::size_t i = 0; i < sort.keys.size(); ++i) {
      c.stop();
      const auto key = sort.keys[i];
      const auto identity = columnIdentity(key);
      if (fixedLiteral(key) || (identity && known && known->count(*identity))) continue;
      replacement.keys.push_back(key);
      replacement.descending.push_back(sort.descending[i]);
      replacement.nullOrdering.push_back(sort.nullOrdering[i]);
    }
    if (replacement.keys.size() == sort.keys.size()) return id;
    if (replacement.keys.empty()) {
      if (!modelQuery(plan, sort.query).limitWithTies) return sort.inputs.front();
      if (sort.keys.size() == 1) return id;
      // A constant key still represents a single peer group for WITH TIES.
      replacement.keys.push_back(sort.keys.front());
      replacement.descending.push_back(sort.descending.front());
      replacement.nullOrdering.push_back(sort.nullOrdering.front());
    }
    return append(std::move(replacement));
  }
  bool pureSortKeys(const RelNode &n) {
    if(n.keys.size()>4096)throw HepLimit{};
    for(auto ref:n.keys){c.stop();if(!pureSortKey(ref))return false;}
    return true;
  }
  bool scalarType(ExpressionRef ref) const {
    const auto &type = plan.program.types.get(modelExpression(plan, ref).sqlType);
    return type.family != compiler::TypeFamily::Unknown &&
           type.family != compiler::TypeFamily::Null && !type.arrayDimensions &&
           !type.sqliteAffinity && type.opaqueName.empty();
  }
  RelId simplifyOuterJoin(RelId id, const RelNode &filter, RelNode join) {
    using compiler::ExprKind;
    using compiler::JoinKind;
    if (!c.options.filterPushdown || filter.kind != RelKind::Filter ||
        filter.predicates.empty() || join.kind != RelKind::Join ||
        join.inputs.size() != 2 || join.predicates.empty() || join.distinct || join.fixedOrder ||
        join.query != filter.query ||
        (join.join != JoinKind::Left && join.join != JoinKind::Right &&
         join.join != JoinKind::Full))
      return id;
    const auto &left = plan.nodes.at(join.inputs[0]);
    const auto &right = plan.nodes.at(join.inputs[1]);
    if (left.kind != RelKind::Scan || right.kind != RelKind::Scan ||
        left.query != join.query || right.query != join.query ||
        !left.table || !right.table || left.table == right.table ||
        properties(join.inputs[0]).mayHaveEffects || properties(join.inputs[1]).mayHaveEffects)
      return id;

    // Restrict ON to ordinary integer comparisons. Reclassifying the join
    // enables later reordering, so expressions with effects or implicit casts
    // are outside this rule's proof.
    for (auto ref : join.predicates) {
      c.stop();
      const auto keys = integerEqualityColumns(plan, ref);
      if (ref.query != join.query || !keys || keys->first.table == keys->second.table ||
          (keys->first.table != left.table && keys->first.table != right.table) ||
          (keys->second.table != left.table && keys->second.table != right.table))
        return id;
    }

    // For a null-extended side every direct IS NOT NULL on that side is
    // false. The builder already split the top-level conjunction. Keep all
    // original filters and do not reason through OR, casts or user functions.
    bool rejectsNullLeft = false, rejectsNullRight = false;
    for (auto ref : filter.predicates) {
      c.stop();
      const auto &predicate = modelExpression(plan, ref);
      if (ref.query != join.query || predicate.kind != ExprKind::IsNull ||
          predicate.operands.size() != 1 ||
          (predicate.value != "IS NULL" && predicate.value != "IS NOT NULL"))
        return id;
      const auto column = boundScalarColumn(plan, {ref.query, predicate.operands.front()});
      if (!column || (column->table != left.table && column->table != right.table)) return id;
      if (predicate.value == "IS NOT NULL") {
        rejectsNullLeft |= column->table == left.table;
        rejectsNullRight |= column->table == right.table;
      }
    }
    auto reduced = join.join;
    if (join.join == JoinKind::Full) {
      if (rejectsNullLeft && rejectsNullRight) reduced = JoinKind::Inner;
      else if (rejectsNullLeft) reduced = JoinKind::Left;
      else if (rejectsNullRight) reduced = JoinKind::Right;
    } else if ((join.join == JoinKind::Left && rejectsNullRight) ||
               (join.join == JoinKind::Right && rejectsNullLeft)) {
      reduced = JoinKind::Inner;
    }
    if (reduced == join.join) return id;
    join.join = reduced;
    auto replacement = filter;
    replacement.inputs.front() = append(std::move(join));
    return append(std::move(replacement));
  }
  std::optional<RelNode> membershipPreFilter(const RelNode &where,ExpressionRef selected) {
    const auto &left=plan.nodes.at(where.inputs.front());
    auto filter=where;
    filter.inputs.resize(1);
    filter.predicates.clear();
    for (auto ref:where.predicates) {
      c.stop();
      if (ref.query!=where.query) return {};
      if (ref.expression==selected.expression) continue;
      const auto columns=totalPredicateColumns(plan,ref);
      if (!columns || !std::all_of(columns->begin(),columns->end(),[&](const auto &field) {
            return containsOutput(left,field);
          }))
        return {};
      filter.predicates.push_back(ref);
    }
    return filter;
  }
  RelId decorrelateExists(RelId id, const RelNode &where) {
    using compiler::ExprKind;
    if (where.kind != RelKind::Filter || where.inputs.size() != 2 ||
        where.predicates.empty() || where.predicates.size()>64)
      return id;
    const auto &outer = modelQuery(plan, where.query);
    if (outer.kind != compiler::StatementKind::Select || !outer.where)
      return id;
    const auto selected=std::find_if(where.predicates.begin(),where.predicates.end(),[&](auto ref) {
      const auto &value=modelExpression(plan,ref);
      return value.kind==ExprKind::Exists ||
          (value.kind==ExprKind::Unary && value.value=="NOT" && value.operands.size()==1 &&
           modelExpression(plan,{ref.query,value.operands.front()}).kind==ExprKind::Exists);
    });
    if (selected==where.predicates.end() || selected->query!=where.query) return id;
    const auto *exists = &modelExpression(plan, *selected);
    const bool negated = exists->kind == ExprKind::Unary && exists->value == "NOT" &&
                         exists->operands.size() == 1;
    if (negated)
      exists = &modelExpression(plan, {outer.id, exists->operands.front()});
    if (exists->kind != ExprKind::Exists || !exists->query)
      return id;

    const auto &left = plan.nodes.at(where.inputs[0]);
    const auto &dependency = plan.nodes.at(where.inputs[1]);
    if (left.kind != RelKind::Scan || left.query != outer.id || !left.table ||
        !columns(where.outputs, left.outputs) ||
        dependency.kind != RelKind::Subquery || !dependency.scalar ||
        !dependency.correlated || dependency.inputs.size() != 1)
      return id;
    const auto &inner = modelQuery(plan, exists->query);
    const auto &project = plan.nodes.at(dependency.inputs.front());
    // Check the original query as well as the rewritten child. A previously
    // removed LIMIT, sort, or DISTINCT must not mask an unsupported shape.
    if (inner.kind != compiler::StatementKind::Select || inner.scope.parent != outer.id ||
        inner.distinct || inner.limit || inner.offset || inner.having ||
        !inner.groupBy.empty() || !inner.groupingSets.empty() || !inner.orderBy.empty() ||
        !inner.ctes.empty() || inner.tables.size() != 1 ||
        project.kind != RelKind::Project || project.query != inner.id ||
        project.keys.size() != 1 || project.inputs.size() != 1)
      return id;
    const auto &literal = modelExpression(plan, project.keys.front());
    if (literal.kind != ExprKind::Number || literal.value != "1")
      return id;
    const auto &restriction = plan.nodes.at(project.inputs.front());
    if (restriction.kind != RelKind::Filter || restriction.query != inner.id ||
        restriction.inputs.size() != 1 || restriction.predicates.size() != 1 ||
        restriction.predicates.front().query != inner.id ||
        restriction.predicates.front().expression != inner.where)
      return id;
    const auto rightId = restriction.inputs.front();
    const auto &right = plan.nodes.at(rightId);
    if (right.kind != RelKind::Scan || right.query != inner.id || !right.table ||
        properties(where.inputs[0]).mayHaveEffects || properties(rightId).mayHaveEffects)
      return id;
    RelNode membership = where;
    membership.kind = negated ? RelKind::AntiJoin : RelKind::SemiJoin;
    membership.inputs[1] = rightId;
    membership.predicates = restriction.predicates;
    membership.membershipKeys = correlatedIntegerEqualityKey(plan, restriction.predicates.front(), outer.id);
    if (!membershipColumns(plan, membership)) return id;
    auto pre=membershipPreFilter(where,*selected);
    if (!pre) return id;
    if (!pre->predicates.empty()) membership.inputs[0]=append(std::move(*pre));
    return append(std::move(membership));
  }
  RelId rewriteInSubquery(RelId id, const RelNode &where,bool marked=false) {
    using namespace compiler;
    if (marked && plan.program.sourceDialect!=Dialect::PostgreSQL) return id;
    if (where.kind != RelKind::Filter || where.inputs.size() != 2 ||
        where.predicates.empty() || where.predicates.size()>64)
      return id;
    const auto &outer = modelQuery(plan, where.query);
    const auto operand=[&](ExpressionRef ref)->const ModelExpression * {
      const auto &test=modelExpression(plan,ref);
      if (!marked) return &test;
      if (test.kind!=ExprKind::TruthTest || test.operands.size()!=1 ||
          (test.value!="IS TRUE" && test.value!="IS NOT TRUE")) return nullptr;
      return &modelExpression(plan,{ref.query,test.operands.front()});
    };
    const auto selected=std::find_if(where.predicates.begin(),where.predicates.end(),[&](auto ref) {
      const auto *value=operand(ref);
      return value && value->query && (value->kind==ExprKind::In || value->kind==ExprKind::Quantified);
    });
    if (selected==where.predicates.end() || selected->query!=where.query) return id;
    const auto &predicate=*operand(*selected);
    const bool in = (predicate.kind == compiler::ExprKind::In && predicate.value == "IN") ||
                    (predicate.kind == compiler::ExprKind::Quantified &&
                     (predicate.value == "= ANY" || predicate.value == "= SOME"));
    if (marked && !in) return id;
    const bool anti = (marked && modelExpression(plan,*selected).value=="IS NOT TRUE") ||
                      (predicate.kind == compiler::ExprKind::In && predicate.value == "NOT IN") ||
                      (predicate.kind == compiler::ExprKind::Quantified &&
                       (predicate.value == "<> ALL" || predicate.value == "!= ALL"));
    if (outer.kind != compiler::StatementKind::Select || !outer.where ||
        (!in && !anti) || !predicate.query || predicate.operands.size() != 1)
      return id;
    const auto &left = plan.nodes.at(where.inputs[0]);
    const auto &dependency = plan.nodes.at(where.inputs[1]);
    if (left.kind != RelKind::Scan || left.query != outer.id || !columns(where.outputs, left.outputs) ||
        dependency.kind != RelKind::Subquery || !dependency.scalar || dependency.correlated ||
        dependency.inputs.size() != 1)
      return id;
    const auto &inner = modelQuery(plan, predicate.query);
    if (inner.kind != compiler::StatementKind::Select || inner.where || inner.having ||
        inner.distinct || inner.limit || inner.offset || inner.setOperation ||
        !inner.groupBy.empty() || !inner.groupingSets.empty() || !inner.orderBy.empty() ||
        !inner.ctes.empty() || inner.tables.size() != 1 || inner.projections.size() != 1)
      return id;

    auto rightId = dependency.inputs.front();
    const auto &source = plan.nodes.at(rightId);
    if (source.kind == RelKind::Project && source.inputs.size() == 1)
      rightId = source.inputs.front();
    const auto &right = plan.nodes.at(rightId);
    if (right.kind != RelKind::Scan || right.query != inner.id ||
        properties(where.inputs[0]).mayHaveEffects || properties(rightId).mayHaveEffects)
      return id;
    auto membership = where;
    membership.kind = anti ? RelKind::AntiJoin : RelKind::SemiJoin;
    membership.inputs[1] = rightId;
    membership.predicates={*selected};
    membership.membershipKeys = JoinKeyPair{
        {outer.id, predicate.operands.front()}, {inner.id, inner.projections.front().expression}};
    const auto keys = membershipColumns(plan, membership);
    if (!keys) return id;
    if (anti && !marked) {
      const auto isNonNull = [&](RelId input, const ColumnRef &key) {
        const auto &fields = properties(input).nonNullColumns;
        return std::any_of(fields.begin(), fields.end(), [&](const auto &field) { return same(field, key); });
      };
      // A nullable NOT IN key may turn "no equal row" into UNKNOWN. Plain
      // anti joins are valid only when both sides have a hard non-NULL proof.
      if (!isNonNull(where.inputs[0], keys->first) || !isNonNull(rightId, keys->second))
        return id;
    }
    auto pre=membershipPreFilter(where,*selected);
    if (!pre) return id;
    if (!pre->predicates.empty()) membership.inputs[0]=append(std::move(*pre));
    return append(std::move(membership));
  }
  std::optional<RelId> preservedOuterScan(const RelNode &join,compiler::QueryId owner,
                                          bool requireUnique) {
    using compiler::JoinKind;
    if (join.kind != RelKind::Join || join.distinct || join.fixedOrder || join.inputs.size() != 2 ||
        join.query != owner || join.predicates.empty() ||
        (join.join != JoinKind::Left && join.join != JoinKind::Right))
      return {};
    const std::size_t preserved = join.join == JoinKind::Left ? 0 : 1;
    const auto keepId = join.inputs[preserved], dropId = join.inputs[1 - preserved];
    const auto scanFor=[&](RelId input) -> const RelNode & {
      const auto *source=&plan.nodes.at(input);
      for (int step=0;step<2 && source->query==owner && source->inputs.size()==1;++step) {
        const auto &below=plan.nodes.at(source->inputs.front());
        if (source->kind==RelKind::Filter && columns(source->outputs,below.outputs)) {
          source=&below;
          continue;
        }
        if (source->kind!=RelKind::Project || source->keys.size()!=source->outputs.size()) break;
        bool direct=true;
        for (std::size_t i=0;i<source->keys.size();++i) {
          c.stop();
          const auto field=boundScalarColumn(plan,source->keys[i]);
          if (!field || !same(*field,source->outputs[i]) || !containsOutput(below,*field)) {
            direct=false;
            break;
          }
        }
        if (!direct) break;
        source=&below;
      }
      return *source;
    };
    const auto &keep=scanFor(keepId), &drop=scanFor(dropId);
    if (keep.kind != RelKind::Scan || drop.kind != RelKind::Scan ||
        keep.query != owner || drop.query != owner || !keep.table || !drop.table ||
        keep.table == drop.table)
      return {};
    if (plan.program.sourceDialect==compiler::Dialect::PostgreSQL) {
      const auto &query=modelQuery(plan,owner);
      const auto &source=query.tables.at(*drop.table);
      const bool only=source.inheritance==compiler::TableInheritance::Only ||
                      query.tableQuery==compiler::TableQuerySyntax::Only;
      if (!source.definition || !source.definition->rowSecurityDisabledKnown ||
          (!only && !source.definition->noDescendantsKnown)) return {};
    }
    const auto &discarded = properties(dropId);
    if (discarded.mayHaveEffects || (requireUnique && discarded.uniqueKeys.empty()))
      return {};

    std::set<std::string> matched;
    for (auto ref : join.predicates) {
      c.stop();
      auto equality=integerEqualityColumns(plan,ref);
      const bool nullSafe=!equality;
      if (nullSafe) equality=integerNullSafeEqualityColumns(plan,ref);
      if (!equality || ref.query != owner) return {};
      const auto &a = equality->first, &b = equality->second;
      const auto *dropped=&a,*keptOperand=&b;
      if (b.table==drop.table) std::swap(dropped,keptOperand);
      if (dropped->table!=drop.table || keptOperand->table!=keep.table) return {};
      if (requireUnique && nullSafe) {
        const auto hasProof=[](const RelProperties &relation,const ColumnRef &column) {
          return std::any_of(relation.nonNullColumns.begin(),relation.nonNullColumns.end(),
                             [&](const auto &candidate) { return same(candidate,column); });
        };
        if (!hasProof(discarded,*dropped) && !hasProof(properties(keepId),*keptOperand)) continue;
      }
      matched.insert(dropped->name);
    }
    if (!requireUnique) return keepId;
    const bool unique = std::any_of(discarded.uniqueKeys.begin(), discarded.uniqueKeys.end(),
        [&](const auto &key) {
          return !key.empty() && std::all_of(key.begin(), key.end(), [&](const auto &column) {
            return column.query == owner && column.table == drop.table && matched.count(column.name);
          });
        });
    return unique?std::optional<RelId>{keepId}:std::nullopt;
  }
  RelId removeUnusedOuterJoin(RelId id, const RelNode &project) {
    if (project.kind != RelKind::Project || project.inputs.size() != 1 ||
        project.keys.empty() || project.keys.size() != project.outputs.size())
      return id;
    const auto &query = modelQuery(plan, project.query);
    // The current representation can resolve ORDER BY against hidden input
    // columns after Project. Keep those queries until that lineage is explicit.
    if (query.kind != compiler::StatementKind::Select || query.where || query.having ||
        !query.groupBy.empty() || !query.groupingSets.empty() || !query.orderBy.empty())
      return id;
    const auto keep=preservedOuterScan(plan.nodes.at(project.inputs.front()),query.id,true);
    if (!keep) return id;
    for (auto ref : project.keys) {
      c.stop();
      const auto column = boundScalarColumn(plan, ref);
      if (!column || !containsOutput(plan.nodes.at(*keep),*column)) return id;
    }
    auto replacement = project;
    replacement.inputs.front() = *keep;
    return append(std::move(replacement));
  }
  RelId removeAggregateJoin(RelId id,const RelNode &top) {
    using namespace compiler;
    if (plan.program.sourceDialect!=Dialect::PostgreSQL || top.inputs.size()!=1 ||
        (top.kind!=RelKind::Project && !(top.kind==RelKind::Aggregate && top.distinct)))
      return id;
    const auto &q=modelQuery(plan,top.query);
    if (q.kind!=StatementKind::Select || q.where || q.having || !q.groupingSets.empty() ||
        std::any_of(q.orderBy.begin(),q.orderBy.end(),[&](const auto &key) {
          return !modelExpression(plan,{q.id,key.expression}).outputColumn;
        }))
      return id;
    const bool deduplicate=top.kind==RelKind::Aggregate;
    auto child=plan.nodes.at(top.inputs.front());
    if (child.query!=q.id || child.inputs.size()!=1 || child.distinct ||
        child.kind!=(deduplicate?RelKind::Project:RelKind::Aggregate))
      return id;
    const auto &projection=deduplicate?child:top;
    if (projection.keys.empty() || projection.keys.size()!=projection.outputs.size() ||
        projection.keys.size()>128)
      return id;
    const auto keep=preservedOuterScan(plan.nodes.at(child.inputs.front()),q.id,false);
    if (!keep || properties(*keep).mayHaveEffects) return id;
    const auto &scan=plan.nodes.at(*keep);
    const auto keptColumn=[&](ExpressionRef ref) {
      const auto column=boundScalarColumn(plan,ref);
      return column && containsOutput(scan,*column) &&
             signedIntegerBits(plan.program.types.get(modelExpression(plan,ref).sqlType));
    };
    if (!deduplicate) {
      for (auto key:child.keys) {
        c.stop();
        const auto &field=modelExpression(plan,key);
        if (field.outputColumn && *field.outputColumn<projection.keys.size())
          key=projection.keys[*field.outputColumn];
        if (!keptColumn(key)) return id;
      }
    }
    for (auto ref:projection.keys) {
      c.stop();
      if (keptColumn(ref)) continue;
      const auto &call=modelExpression(plan,ref);
      if (deduplicate || call.kind!=ExprKind::Call || call.operands.size()!=1 ||
          !keptColumn({ref.query,call.operands.front()}) ||
          (call.routineResolved && (call.routineSchema!="pg_catalog" ||
               call.routineVolatility!=FunctionVolatility::Immutable || call.routineKind!=RoutineKind::Aggregate)))
        return id;
      const auto native=builtinOperator(Dialect::PostgreSQL,call.name,call.nameQuoted,1);
      if (!native || (native->name!="MIN" && native->name!="MAX" &&
                      !(call.distinct && (native->name=="COUNT" || native->name=="SUM"))))
        return id;
    }
    child.inputs.front()=*keep;
    if (!deduplicate) child.outputs=scan.outputs;
    auto result=top;
    result.inputs.front()=append(std::move(child));
    return append(std::move(result));
  }
  RelId removeChainedOuterInput(RelId id,RelNode project,bool aggregated) {
    using namespace compiler;
    if (plan.program.sourceDialect!=Dialect::PostgreSQL || project.kind!=RelKind::Project ||
        project.inputs.size()!=1 || project.keys.empty() || project.keys.size()>64 ||
        project.keys.size()!=project.outputs.size())
      return id;
    const auto &query=modelQuery(plan,project.query);
    if (!query.catalogChecked || query.where || query.having || query.selectInto ||
        !query.groupingSets.empty() ||
        std::any_of(query.orderBy.begin(),query.orderBy.end(),[&](const auto &item) {
          return !modelExpression(plan,{query.id,item.expression}).outputColumn;
        }))
      return id;
    auto source=project.inputs.front();
    std::optional<RelNode> group;
    if (aggregated) {
      const auto &child=plan.nodes.at(source);
      if (child.kind!=RelKind::Aggregate || child.distinct || child.inputs.size()!=1 ||
          child.query!=project.query || child.keys.size()>64 || !child.groupingSets.empty())
        return id;
      group=child;
      source=child.inputs.front();
    }
    auto join=plan.nodes.at(source);
    if (join.kind!=RelKind::Join || join.join!=JoinKind::Left || join.query!=query.id ||
        join.distinct || join.fixedOrder || join.correlated || join.inputs.size()!=2 ||
        join.predicates.empty() || join.predicates.size()>64)
      return id;
    const auto &nested=plan.nodes.at(join.inputs.front());
    if (nested.kind!=RelKind::Join || nested.join!=JoinKind::Left || nested.correlated ||
        nested.inputs.size()!=2 || !preservedOuterScan(nested,query.id,true))
      return id;
    const auto &drop=plan.nodes.at(nested.inputs.back());
    if (drop.outputs.empty()) return id;
    const auto unused=drop.outputs.front().table;
    if (unused>=query.tables.size() || !query.tables[unused].definition ||
        !query.tables[unused].definition->rowSecurityDisabledKnown ||
        !query.tables[unused].definition->noDescendantsKnown)
      return id;
    const auto &left=plan.nodes.at(nested.inputs.front()), &right=plan.nodes.at(join.inputs.back());
    const auto kept=[&](const ColumnRef &col) {
      return col.query==query.id && (containsOutput(left,col) || containsOutput(right,col));
    };
    const auto keptField=[&](ExpressionRef ref) {
      const auto &value=modelExpression(plan,ref);
      if (ref.query==query.id && value.outputColumn && *value.outputColumn<query.projections.size())
        ref.expression=query.projections[*value.outputColumn].expression;
      const auto col=boundScalarColumn(plan,ref);
      return col && kept(*col);
    };
    for (auto ref:join.predicates) {
      c.stop();
      const auto fields=totalPredicateColumns(plan,ref);
      if (ref.query!=query.id || !fields || !std::all_of(fields->begin(),fields->end(),kept))
        return id;
    }
    if (group)
      for (auto ref:group->keys)
        if (!keptField(ref)) return id;
    for (auto ref:project.keys) {
      c.stop();
      if (keptField(ref)) continue;
      const auto &call=modelExpression(plan,ref);
      if (!group || ref.query!=query.id || call.kind!=ExprKind::Call || call.window ||
          call.operands.size()!=1 || (call.routineResolved &&
          (call.routineSchema!="pg_catalog" || call.routineKind!=RoutineKind::Aggregate ||
           call.routineVolatility!=FunctionVolatility::Immutable)))
        return id;
      const auto op=builtinOperator(Dialect::PostgreSQL,call.name,call.nameQuoted,1);
      if (!op || op->kind!=RoutineKind::Aggregate ||
          (op->name!="COUNT" && op->name!="SUM" && op->name!="MIN" && op->name!="MAX" && op->name!="AVG"))
        return id;
      const ExpressionRef arg{query.id,call.operands.front()};
      const auto &value=modelExpression(plan,arg);
      if (op->name=="COUNT" && !call.distinct && value.kind==ExprKind::Star && value.name.empty()) continue;
      if (!keptField(arg)) return id;
    }
    join.inputs.front()=nested.inputs.front();
    join.outputs=left.outputs;
    join.outputs.insert(join.outputs.end(),right.outputs.begin(),right.outputs.end());
    if (group) group->outputs=join.outputs;
    auto remaining=append(std::move(join));
    if (group) {
      group->inputs={remaining};
      remaining=append(std::move(*group));
    }
    project.inputs={remaining};
    return append(std::move(project));
  }
  RelId trimJoinInputs(RelId id,const RelNode &project) {
    using namespace compiler;
    if (plan.program.sourceDialect!=Dialect::PostgreSQL || project.kind!=RelKind::Project ||
        project.inputs.size()!=1 || project.keys.empty() || project.keys.size()>128 ||
        project.keys.size()!=project.outputs.size()) return id;
    auto join=plan.nodes.at(project.inputs.front());
    if (join.kind!=RelKind::Join || join.query!=project.query || join.inputs.size()!=2 ||
        join.fixedOrder || join.correlated || join.distinct || join.predicates.size()>64)
      return id;
    const auto &q=modelQuery(plan,project.query);
    // The adjacent top-N rule operates on the original scans. Its local
    // prefix is a stronger reduction than a projection alone.
    if (!q.catalogChecked || q.tables.size()!=2 || q.limit || q.offset) return id;
    const auto &left=plan.nodes.at(join.inputs[0]);
    const auto &right=plan.nodes.at(join.inputs[1]);
    const std::array<const RelNode *,2> children{&left,&right};
    std::array<const RelNode *,2> scans{};
    for (std::size_t i=0;i<children.size();++i) {
      const auto &input=*children[i];
      const auto &scan=input.kind==RelKind::Filter && input.inputs.size()==1
          ?plan.nodes.at(input.inputs.front()):input;
      if (scan.kind!=RelKind::Scan || scan.query!=q.id || input.query!=q.id ||
          !scan.table || *scan.table>=q.tables.size() || scan.outputs.size()>256 ||
          !columns(scan.outputs,input.outputs))
        return id;
      const auto &def=q.tables[*scan.table].definition;
      if (!def || def->kind!=ObjectKind::Table || !def->complete || !def->arityKnown)
        return id;
      scans[i]=&scan;
    }
    if (scans[0]->table==scans[1]->table) return id;

    std::array<std::map<std::size_t,ExpressionRef>,2> needed;
    const auto require=[&](ExpressionRef ref) {
      const auto field=boundScalarColumn(plan,ref);
      if (ref.query!=q.id || !field) return false;
      const auto &e=modelExpression(plan,ref);
      const auto column=e.binding->column;
      const std::size_t side=field->table==scans[0]->table?0:1;
      const auto &scan=*scans[side];
      if (field->table!=scan.table || column>=scan.outputs.size() ||
          !same(scan.outputs[column],*field)) return false;
      if (needed[side].try_emplace(column,ref).second)
        c.charge(sizeof(column)+sizeof(ref)+3*sizeof(void *));
      return true;
    };
    for (auto ref:project.keys) {
      c.stop();
      if (!require(ref)) return id;
    }
    for (const auto &item:q.orderBy) {
      const auto &order=modelExpression(plan,{q.id,item.expression});
      if (order.outputColumn && *order.outputColumn<project.keys.size()) continue;
      const auto orderColumn=boundScalarColumn(plan,{q.id,item.expression});
      if (!orderColumn || std::none_of(project.keys.begin(),project.keys.end(),[&](auto ref) {
             const auto selected=boundScalarColumn(plan,ref);
             return selected && same(*selected,*orderColumn);
           })) return id;
    }
    for (auto ref:join.predicates) {
      c.stop();
      const auto &expression=modelExpression(plan,ref);
      if (ref.query!=q.id || !integerEqualityColumns(plan,ref,IntegerEquality::ExactSigned) ||
          !require({q.id,expression.operands[0]}) || !require({q.id,expression.operands[1]}))
        return id;
    }
    if (needed[0].empty() || needed[1].empty() ||
        (needed[0].size()==left.outputs.size() && needed[1].size()==right.outputs.size()))
      return id;

    join.outputs.clear();
    for (std::size_t i=0;i<needed.size();++i) {
      if (needed[i].size()!=plan.nodes.at(join.inputs[i]).outputs.size()) {
        RelNode projection;
        projection.kind=RelKind::Project;
        projection.query=q.id;
        projection.origin=join.origin;
        projection.inputs={join.inputs[i]};
        for (const auto &[column,ref]:needed[i]) {
          projection.outputs.push_back(plan.nodes.at(join.inputs[i]).outputs[column]);
          projection.keys.push_back(ref);
        }
        join.inputs[i]=append(std::move(projection));
      }
      const auto &projected=plan.nodes.at(join.inputs[i]).outputs;
      join.outputs.insert(join.outputs.end(),projected.begin(),projected.end());
    }
    auto result=project;
    result.inputs={append(std::move(join))};
    return append(std::move(result));
  }
  RelId trimUnaryInput(RelId id,const RelNode &project,RelKind operation) {
    using namespace compiler;
    if (plan.program.sourceDialect!=Dialect::PostgreSQL || project.kind!=RelKind::Project ||
        project.inputs.size()!=1 || project.keys.empty() || project.keys.size()>128 ||
        project.keys.size()!=project.outputs.size())
      return id;
    auto middle=plan.nodes.at(project.inputs.front());
    if (middle.kind!=operation || middle.query!=project.query || middle.inputs.size()!=1 ||
        (operation!=RelKind::Aggregate && !middle.keys.empty()) ||
        middle.distinct || middle.keys.size()>128 || middle.predicates.size()>384)
      return id;
    const auto source=middle.inputs.front();
    const auto &scan=plan.nodes.at(source);
    if (scan.kind!=RelKind::Scan || scan.query!=project.query || !scan.table ||
        scan.outputs.size()>256 || !columns(scan.outputs,middle.outputs) ||
        properties(source).mayHaveEffects)
      return id;
    const auto &q=modelQuery(plan,project.query);
    if (std::any_of(q.orderBy.begin(),q.orderBy.end(),[&](const auto &item) {
          return !modelExpression(plan,{q.id,item.expression}).outputColumn;
        }))
      return id;
    std::map<std::size_t,ExpressionRef> needed;
    std::set<compiler::ExprId> seen;
    std::vector<ExpressionRef> work=project.keys;
    work.insert(work.end(),middle.predicates.begin(),middle.predicates.end());
    work.insert(work.end(),middle.keys.begin(),middle.keys.end());
    for (const auto &group:middle.groupingSets) {
      if (group.size()>512 || work.size()>512-group.size()) return id;
      work.insert(work.end(),group.begin(),group.end());
    }
    while (!work.empty()) {
      c.stop();
      const auto ref=work.back();
      work.pop_back();
      if (ref.query!=q.id) return id;
      if (!seen.insert(ref.expression).second) continue;
      if (seen.size()>512) return id;
      c.charge(sizeof(ExprId)+sizeof(ExpressionRef)+4*sizeof(void *));
      const auto &value=modelExpression(plan,ref);
      if (value.query || value.localSymbol || value.outputColumn) return id;
      if (value.kind==ExprKind::Name) {
        const auto field=boundScalarColumn(plan,ref);
        if (!field || field->table!=scan.table || value.binding->column>=scan.outputs.size() ||
            !same(*field,scan.outputs[value.binding->column]))
          return id;
        if (needed.emplace(value.binding->column,ref).second)
          c.charge(sizeof(std::size_t)+sizeof(ExpressionRef)+4*sizeof(void *));
      } else if (value.kind==ExprKind::Star && !value.name.empty()) {
        return id; // Whole-row arguments can depend on columns outside the list.
      }
      if (value.operands.size()>512-work.size()) return id;
      for (auto operand:value.operands) work.push_back({q.id,operand});
    }
    if (needed.empty() || needed.size()==scan.outputs.size()) return id;
    RelNode narrow;
    narrow.kind=RelKind::Project;
    narrow.query=q.id;
    narrow.origin=middle.origin;
    narrow.inputs={source};
    for (const auto &[column,ref]:needed) {
      narrow.outputs.push_back(scan.outputs[column]);
      narrow.keys.push_back(ref);
    }
    middle.outputs=narrow.outputs;
    middle.inputs={append(std::move(narrow))};
    auto result=project;
    result.inputs={append(std::move(middle))};
    return append(std::move(result));
  }
  RelId trimCorrelateInputs(RelId id,const RelNode &project) {
    using namespace compiler;
    if (plan.program.sourceDialect!=Dialect::PostgreSQL || project.kind!=RelKind::Project ||
        project.inputs.size()!=1 || project.keys.empty() || project.keys.size()>128 ||
        project.keys.size()!=project.outputs.size())
      return id;
    auto correlate=plan.nodes.at(project.inputs.front());
    if (correlate.kind!=RelKind::Correlate || correlate.query!=project.query ||
        correlate.inputs.size()!=2 || correlate.distinct || !correlate.predicates.empty() ||
        (correlate.join!=JoinKind::Cross && correlate.join!=JoinKind::Left))
      return id;
    auto &owner=plan.program.queries.at(project.query-1);
    const auto &left=plan.nodes.at(correlate.inputs[0]), &right=plan.nodes.at(correlate.inputs[1]);
    const std::array<const RelNode *,2> sources{&left,&right};
    if (!owner.catalogChecked || left.kind!=RelKind::Scan || right.kind!=RelKind::Subquery ||
        right.materialized || left.query!=owner.id || right.query!=owner.id ||
        !left.table || !right.table || left.table==right.table ||
        left.outputs.size()>256 || right.outputs.size()>256 ||
        std::any_of(owner.orderBy.begin(),owner.orderBy.end(),[&](const auto &key) {
          return !modelExpression(plan,{owner.id,key.expression}).outputColumn;
        }))
      return id;
    std::array<std::map<std::size_t,ExpressionRef>,2> required;
    const auto require=[&](ExpressionRef ref,std::size_t side) {
      const auto &source=*sources[side];
      const auto col=scopedScalarColumn(plan,ref,owner.id);
      if (!col || col->table!=source.table) return false;
      const auto ordinal=modelExpression(plan,ref).binding->column;
      if (ordinal>=source.outputs.size() || !same(*col,source.outputs[ordinal])) return false;
      if (required[side].emplace(ordinal,ref).second)
        c.charge(sizeof(std::size_t)+sizeof(ExpressionRef)+4*sizeof(void *));
      return true;
    };
    for (auto ref:project.keys) {
      c.stop();
      if (ref.query!=owner.id || (!require(ref,0) && !require(ref,1))) return id;
    }
    // Any nested query may read a column through its enclosing LATERAL
    // frame, even when the immediate right projection never uses it.
    std::size_t visits=0;
    for (const auto &child:plan.program.queries) {
      if (child.id==owner.id || !child.scope.parent) continue;
      for (std::size_t i=1;i<child.expressions.size();++i) {
        c.stop();
        if (++visits>65536) return id;
        const auto &value=child.expressions[i];
        if ((value.kind==ExprKind::Star && !value.name.empty()) ||
            std::any_of(value.coalescedBindings.begin(),value.coalescedBindings.end(),[&](const auto &col) {
              return col.scope==owner.id && col.table==left.table;
            }))
          return id;
        if (value.binding && value.binding->scope==owner.id && value.binding->table==left.table &&
            !require({child.id,static_cast<ExprId>(i)},0))
          return id;
      }
    }
    const auto narrower=[&](std::size_t side) {
      return !required[side].empty() && required[side].size()<sources[side]->outputs.size();
    };
    if ((!narrower(0) && !narrower(1)) ||
        owner.expressions.size()+required[0].size()>c.options.maxNodes)
      return id;
    // Hold output copies while appending new nodes to the owning DAG.
    const std::array originalOutputs{left.outputs,right.outputs};
    const std::array<bool,2> trim{narrower(0),narrower(1)};
    correlate.outputs.clear();
    for (std::size_t side=0;side<2;++side) {
      if (trim[side]) {
        RelNode narrowed;
        narrowed.kind=RelKind::Project;
        narrowed.query=owner.id;
        narrowed.origin=correlate.origin;
        narrowed.inputs={correlate.inputs[side]};
        for (const auto &[column,expression]:required[side]) {
          auto ref=expression;
          if (ref.query!=owner.id) {
            auto captured=modelExpression(plan,ref);
            c.charge(sizeof(captured));
            for (const auto &name:captured.name) c.charge(sizeof(name)+name.size());
            ref={owner.id,static_cast<ExprId>(owner.expressions.size())};
            owner.expressions.push_back(std::move(captured));
          }
          narrowed.outputs.push_back(originalOutputs[side][column]);
          narrowed.keys.push_back(ref);
        }
        correlate.inputs[side]=append(std::move(narrowed));
      }
      const auto &outputs=plan.nodes.at(correlate.inputs[side]).outputs;
      correlate.outputs.insert(correlate.outputs.end(),outputs.begin(),outputs.end());
    }
    auto result=project;
    result.inputs={append(std::move(correlate))};
    return append(std::move(result));
  }
  RelId pushAggregateFilter(RelId id, const RelNode &filter, RelNode aggregate) {
    using namespace compiler;
    if (!c.options.filterPushdown || plan.program.sourceDialect != Dialect::PostgreSQL ||
        filter.kind != RelKind::Filter || filter.predicates.empty() ||
        aggregate.kind != RelKind::Aggregate || aggregate.query != filter.query || aggregate.distinct ||
        aggregate.keys.empty() || !aggregate.groupingSets.empty() || aggregate.inputs.size() != 1)
      return id;
    const auto input = aggregate.inputs.front();
    const auto &scan = plan.nodes.at(input);
    if (scan.kind != RelKind::Scan || scan.query != aggregate.query || !scan.table ||
        !columns(filter.outputs, scan.outputs) || properties(input).mayHaveEffects)
      return id;
    const auto &q = modelQuery(plan, filter.query);
    if (q.kind != StatementKind::Select || !q.having)
      return id;

    std::set<std::string> grouped;
    for (auto key : aggregate.keys) {
      c.stop();
      const auto &expression = modelExpression(plan, key);
      if (expression.outputColumn && *expression.outputColumn < q.projections.size())
        key = {q.id, q.projections[*expression.outputColumn].expression};
      const auto col = boundScalarColumn(plan, key);
      if (!col || col->query != q.id || col->table != *scan.table) return id;
      grouped.insert(col->name);
    }
    const auto isGrouped = [&](const ColumnRef &col) {
      return col.query == q.id && col.table == *scan.table && grouped.count(col.name);
    };
    const auto safeOutput = [&](ExpressionRef ref) {
      if (auto col = boundScalarColumn(plan, ref)) return isGrouped(*col);
      const auto &e = modelExpression(plan, ref);
      const bool native = !e.routineResolved ||
          (e.routineSchema == "pg_catalog" && e.routineKind == RoutineKind::Aggregate &&
           e.routineVolatility == FunctionVolatility::Immutable);
      if (e.kind != ExprKind::Call || !native || e.window || e.distinct || e.operands.size() != 1)
        return false;
      const auto builtin = builtinOperator(plan.program.sourceDialect, e.name, e.nameQuoted, e.operands.size());
      const auto &argument = modelExpression(plan, {q.id, e.operands.front()});
      return builtin && builtin->name == "COUNT" && argument.kind == ExprKind::Star && argument.name.size() <= 1;
    };
    for (const auto &projection : q.projections) {
      c.stop();
      if (!safeOutput({q.id, projection.expression})) return id;
    }
    for (const auto &order : q.orderBy) {
      c.stop();
      const auto &orderKey = q.expressions.at(order.expression);
      const auto expr = orderKey.outputColumn && *orderKey.outputColumn < q.projections.size() ?
          q.projections[*orderKey.outputColumn].expression : order.expression;
      if (!safeOutput({q.id, expr})) return id;
    }
    for (auto predicate : filter.predicates) {
      c.stop();
      if (predicate.query != q.id) return id;
      const auto refs = totalPredicateColumns(plan, predicate);
      if (!refs || refs->empty() || !std::all_of(refs->begin(), refs->end(), isGrouped))
        return id;
    }
    auto early = filter;
    early.inputs = {input};
    aggregate.inputs = {append(std::move(early))};
    return append(std::move(aggregate));
  }
  RelId pushUnpivotFilter(RelId id,const RelNode &filter,RelNode unpivot) {
    using namespace compiler;
    if (!c.options.filterPushdown || plan.program.sourceDialect!=Dialect::PostgreSQL ||
        filter.kind!=RelKind::Filter || filter.predicates.empty() ||
        unpivot.kind!=RelKind::Unpivot || unpivot.query!=filter.query ||
        unpivot.inputs.size()!=1 || !unpivot.table) return id;
    const auto &q=modelQuery(plan,unpivot.query);
    if (*unpivot.table>=q.tables.size()) return id;
    const auto &binding=q.tables[*unpivot.table];
    const auto &shape=binding.unpivot;
    const auto source=unpivot.inputs.front();
    const auto &input=plan.nodes.at(source);
    if (!shape || !binding.definition || !binding.definition->complete ||
        !binding.unpivotInput || properties(source).mayHaveEffects ||
        unpivot.outputs.size()!=binding.definition->columns.size() ||
        unpivot.outputs.size()!=shape->retainedOrdinals.size()+shape->descriptors.size()+shape->measures.size())
      return id;
    for (std::size_t i=shape->retainedOrdinals.size()+shape->descriptors.size();i<unpivot.outputs.size();++i) {
      c.stop();
      const auto &type=binding.definition->columns[i].sqlType;
      if (!type || !signedIntegerBits(*type)) return id;
    }
    std::set<std::string> retained;
    for (std::size_t i=0;i<shape->retainedOrdinals.size();++i) {
      c.stop();
      const auto position=shape->retainedOrdinals[i];
      const auto &out=unpivot.outputs[i];
      if (position>=input.outputs.size() || !same(out,input.outputs[position]) ||
          out.query!=q.id || out.table!=*unpivot.table || !retained.insert(out.name).second) return id;
    }
    auto early=filter;
    early.inputs={source};
    early.outputs=input.outputs;
    early.predicates.clear();
    auto residual=filter;
    residual.predicates.clear();
    for (auto predicate:filter.predicates) {
      c.stop();
      const auto refs=predicate.query==q.id?totalPredicateColumns(plan,predicate):std::nullopt;
      const bool safe=refs && !refs->empty() && std::all_of(refs->begin(),refs->end(),[&](const auto &field) {
        return field.query==q.id && field.table==*unpivot.table && retained.count(field.name);
      });
      (safe?early.predicates:residual.predicates).push_back(predicate);
    }
    if (early.predicates.empty()) return id;
    unpivot.inputs={append(std::move(early))};
    const auto expanded=append(std::move(unpivot));
    if (residual.predicates.empty()) return expanded;
    residual.inputs={expanded};
    return append(std::move(residual));
  }
  RelId pushCorrelateFilter(RelId id,const RelNode &filter,RelNode correlate) {
    using namespace compiler;
    if (plan.program.sourceDialect!=Dialect::PostgreSQL || filter.kind!=RelKind::Filter ||
        filter.predicates.empty() || filter.predicates.size()>64 ||
        correlate.kind!=RelKind::Correlate || correlate.query!=filter.query || !correlate.correlated ||
        correlate.inputs.size()!=2 || correlate.distinct || !correlate.predicates.empty() ||
        (correlate.join!=JoinKind::Cross && correlate.join!=JoinKind::Left))
      return id;
    const auto &left=plan.nodes.at(correlate.inputs[0]), &right=plan.nodes.at(correlate.inputs[1]);
    if (left.query!=filter.query || right.kind!=RelKind::Subquery ||
        properties(correlate.inputs[0]).mayHaveEffects || properties(correlate.inputs[1]).mayHaveEffects)
      return id;

    c.charge(2*filter.predicates.size()*sizeof(ExpressionRef));
    std::vector<ExpressionRef> early,residual;
    for (auto predicate:filter.predicates) {
      c.stop();
      if (predicate.query!=filter.query) return id;
      const auto refs=totalPredicateColumns(plan,predicate);
      if (!refs) return id;
      auto &target=!refs->empty() &&
          std::all_of(refs->begin(),refs->end(),[&](const auto &field) {
            return containsOutput(left,field);
          }) ? early : residual;
      target.push_back(predicate);
    }
    if (early.empty()) return id;

    auto leftFilter=filter;
    leftFilter.inputs={correlate.inputs[0]};
    leftFilter.outputs=left.outputs;
    leftFilter.predicates=std::move(early);
    correlate.inputs[0]=append(std::move(leftFilter));
    const auto pushed=append(std::move(correlate));
    if (residual.empty()) return pushed;
    auto result=filter;
    result.inputs={pushed};
    result.predicates=std::move(residual);
    return append(std::move(result));
  }
  bool verifiedIntegerSetInputs(const RelNode &set, const compiler::ModelStatement &query) {
    if (!query.setOperation || query.setOperation->recursive || set.query != query.id ||
        set.kind != RelKind::SetOperation || set.inputs.size() != 2 || set.outputs.empty()) return false;
    const auto &left = modelQuery(plan, query.setOperation->left);
    const auto &right = modelQuery(plan, query.setOperation->right);
    const auto baseProjection = [&](RelId arm, const compiler::ModelStatement &source) {
      const auto &branch = plan.nodes.at(arm);
      if (branch.query != source.id || branch.outputs.size() != set.outputs.size() ||
          source.projections.size() != branch.outputs.size() || properties(arm).mayHaveEffects)
        return false;
      if (branch.kind == RelKind::Scan) return true;
      return branch.kind == RelKind::Project && branch.inputs.size() == 1 &&
            plan.nodes.at(branch.inputs.front()).kind == RelKind::Scan;
    };
    if (!baseProjection(set.inputs[0], left) || !baseProjection(set.inputs[1], right)) return false;
    for (std::size_t i = 0; i < set.outputs.size(); ++i) {
      c.stop();
      const ExpressionRef a{left.id, left.projections[i].expression},
                          b{right.id, right.projections[i].expression};
      if (!boundScalarColumn(plan, a) || !boundScalarColumn(plan, b) || !sameIntegerType(plan, a, b))
        return false;
    }
    return true;
  }
  const RelNode *directSetInput(RelId id, compiler::QueryId owner) {
    const auto &node = plan.nodes.at(id);
    if (node.kind != RelKind::Project) return &node;
    if (node.query != owner || node.inputs.size() != 1) return nullptr;
    const auto &input = plan.nodes.at(node.inputs.front());
    if (!columns(node.outputs, input.outputs) || node.keys.size() != input.outputs.size())
      return nullptr;
    for (std::size_t i = 0; i < node.keys.size(); ++i) {
      c.stop();
      const auto col = boundScalarColumn(plan, node.keys[i]);
      if (!col || !same(*col, input.outputs[i])) return nullptr;
    }
    return &input;
  }
  RelId pullUpUnionConstants(RelId id,const RelNode &set) {
    using namespace compiler;
    if (plan.program.sourceDialect!=Dialect::PostgreSQL || !isUnionAll(set) ||
        set.inputs.size()!=2 || set.outputs.size()<2 || set.outputs.size()>64)
      return id;
    auto &q=plan.program.queries.at(set.query-1);
    if (!q.setOperation || q.setOperation->recursive || q.projections.size()!=set.outputs.size() ||
        q.tables.size()!=1 || !q.tables.front().pseudo ||
        q.expressions.size()+2*set.outputs.size()>c.options.maxNodes)
      return id;
    const auto &left=plan.nodes.at(set.inputs[0]), &right=plan.nodes.at(set.inputs[1]);
    const auto directArm=[&](const RelNode &arm,QueryId expected) {
      return arm.kind==RelKind::Project && arm.query==expected && arm.inputs.size()==1 &&
          arm.keys.size()==set.outputs.size() && arm.outputs.size()==set.outputs.size() &&
          plan.nodes.at(arm.inputs.front()).kind==RelKind::Scan;
    };
    if (!directArm(left,q.setOperation->left) || !directArm(right,q.setOperation->right) ||
        properties(left.inputs.front()).mayHaveEffects || properties(right.inputs.front()).mayHaveEffects)
      return id;
    for (const auto &field:set.outputs)
      if (std::count_if(set.outputs.begin(),set.outputs.end(),[&](const auto &other){return same(field,other);})!=1)
        return id;

    const auto integerLiteral=[](const ModelExpression &e) {
      return e.kind==ExprKind::Number && !e.value.empty() && e.value.size()<=19 &&
          e.value.find_first_not_of("0123456789")==std::string::npos;
    };
    const auto typedNull=[&](const ModelExpression &e,QueryId owner) {
      return e.kind==ExprKind::Cast && e.operands.size()==1 && e.castType &&
          modelExpression(plan,{owner,e.operands.front()}).kind==ExprKind::Null;
    };
    const auto pure=[&](ExpressionRef ref) {
      const auto &expr=modelExpression(plan,ref);
      return !expr.localSymbol && !expr.query && !expr.window &&
          (boundScalarColumn(plan,ref).has_value() || integerLiteral(expr) || typedNull(expr,ref.query));
    };
    std::vector<bool> fixed(set.outputs.size());
    c.charge(set.outputs.size()*(sizeof(bool)+sizeof(ExpressionRef)));
    for (std::size_t i=0;i<set.outputs.size();++i) {
      c.stop();
      const ExpressionRef output{q.id,q.projections[i].expression};
      const auto field=boundScalarColumn(plan,output);
      if (!field || !same(*field,set.outputs[i]) || left.keys[i].query!=left.query ||
          right.keys[i].query!=right.query || !sameIntegerType(plan,left.keys[i],right.keys[i]) ||
          !sameIntegerType(plan,left.keys[i],output) || !pure(left.keys[i]) || !pure(right.keys[i]))
        return id;
      const auto &a=modelExpression(plan,left.keys[i]), &b=modelExpression(plan,right.keys[i]);
      fixed[i]=(integerLiteral(a) && integerLiteral(b) && a.value==b.value) ||
          (typedNull(a,left.query) && typedNull(b,right.query));
    }
    if (std::all_of(fixed.begin(),fixed.end(),[](auto constant){return constant;}))
      fixed[0]=false; // Retain one field to represent input cardinality.
    if (std::none_of(fixed.begin(),fixed.end(),[](auto constant){return constant;})) return id;

    RelNode projection;
    projection.kind=RelKind::Project;
    projection.query=q.id;
    projection.origin=set.origin;
    projection.outputs=set.outputs;
    const auto add=[&](ModelExpression e) {
      c.charge(sizeof(e)+e.value.size()+e.operands.size()*sizeof(ExprId));
      const auto expression=static_cast<ExprId>(q.expressions.size());
      q.expressions.push_back(std::move(e));
      return expression;
    };
    for (std::size_t i=0;i<fixed.size();++i) {
      c.stop();
      auto value=q.projections[i].expression;
      if (fixed[i]) {
        auto literal=modelExpression(plan,left.keys[i]);
        if (literal.kind==ExprKind::Cast) {
          const auto inner=modelExpression(plan,{left.query,literal.operands.front()});
          literal.operands={add(inner)};
        }
        value=add(std::move(literal));
      }
      projection.keys.push_back({q.id,value});
    }
    auto shortened=set;
    shortened.outputs.clear();
    for (std::size_t i=0;i<fixed.size();++i)
      if (!fixed[i]) shortened.outputs.push_back(set.outputs[i]);
    for (auto &arm:shortened.inputs) {
      auto branch=plan.nodes.at(arm);
      std::vector<ExpressionRef> keys;
      std::vector<ColumnRef> outputs;
      for (std::size_t i=0;i<fixed.size();++i) {
        if (fixed[i]) continue;
        keys.push_back(branch.keys[i]);
        outputs.push_back(branch.outputs[i]);
      }
      branch.keys=std::move(keys);
      branch.outputs=std::move(outputs);
      arm=append(std::move(branch));
    }
    projection.inputs={append(std::move(shortened))};
    return append(std::move(projection));
  }
  RelId pushProjectionIntoUnion(RelId id,const RelNode &project) {
    using namespace compiler;
    if (plan.program.sourceDialect!=Dialect::PostgreSQL || project.kind!=RelKind::Project ||
        project.inputs.size()!=1 || project.keys.empty() || project.keys.size()>64 ||
        project.keys.size()!=project.outputs.size()) return id;
    const auto &wrapper=plan.nodes.at(project.inputs.front());
    const auto &q=modelQuery(plan,project.query);
    if (!q.catalogChecked || wrapper.kind!=RelKind::Subquery || wrapper.query!=q.id ||
        !wrapper.table || *wrapper.table>=q.tables.size() || wrapper.inputs.size()!=1 ||
        wrapper.correlated || wrapper.scalar || wrapper.materialized || wrapper.outputs.empty() ||
        wrapper.outputs.size()>64)
      return id;
    const auto &relation=q.tables[*wrapper.table];
    if (!relation.name.empty() || !relation.query || !relation.definition ||
        !relation.definition->complete || !relation.definition->arityKnown)
      return id;
    const auto &owner=modelQuery(plan,relation.query);
    const auto *input=directSetInput(wrapper.inputs.front(),owner.id);
    if (!input || !isUnionAll(*input) || owner.limit || owner.offset || !owner.orderBy.empty() ||
        wrapper.outputs.size()!=input->outputs.size() || !verifiedIntegerSetInputs(*input,owner))
      return id;
    for (const auto &order:q.orderBy) {
      const auto &key=q.expressions.at(order.expression);
      if (!key.outputColumn || *key.outputColumn>=project.keys.size()) return id;
    }

    const auto &first=modelQuery(plan,owner.setOperation->left);
    std::vector<std::size_t> positions;
    c.charge(project.keys.size()*sizeof(std::size_t));
    for (auto ref:project.keys) {
      c.stop();
      const auto column=boundScalarColumn(plan,ref);
      const auto &model=modelExpression(plan,ref);
      if (ref.query!=q.id || !column || column->table!=wrapper.table) return id;
      const auto ordinal=model.binding->column;
      if (ordinal>=wrapper.outputs.size() || !same(*column,wrapper.outputs[ordinal]) ||
          !sameIntegerType(plan,ref,{first.id,first.projections[ordinal].expression}))
        return id;
      positions.push_back(ordinal);
    }
    if (positions.size()>=input->outputs.size()) return id; // The rule reduces row width.
    for (auto arm:input->inputs) {
      const auto &branch=plan.nodes.at(arm);
      const auto &block=modelQuery(plan,branch.query);
      if (branch.kind==RelKind::Project &&
          (branch.keys.size()!=block.projections.size() ||
           !std::all_of(positions.begin(),positions.end(),[&](auto i) {
             return branch.keys[i].query==block.id && branch.keys[i].expression==block.projections[i].expression;
           }))) return id;
    }

    auto result=*input;
    result.outputs.clear();
    for (auto ordinal:positions) result.outputs.push_back(input->outputs[ordinal]);
    for (auto &arm:result.inputs) {
      c.stop();
      const auto &branch=plan.nodes.at(arm);
      const auto &block=modelQuery(plan,branch.query);
      RelNode narrowed;
      narrowed.kind=RelKind::Project;
      narrowed.query=branch.query;
      narrowed.origin=branch.origin;
      narrowed.inputs=branch.kind==RelKind::Project?branch.inputs:std::vector<RelId>{arm};
      for (auto ordinal:positions) {
        narrowed.outputs.push_back(branch.outputs[ordinal]);
        narrowed.keys.push_back({block.id,block.projections[ordinal].expression});
      }
      arm=append(std::move(narrowed));
    }
    RelNode rename;
    rename.kind=RelKind::Rename;
    rename.query=project.query;
    rename.origin=project.origin;
    rename.inputs={append(std::move(result))};
    rename.outputs=project.outputs;
    return append(std::move(rename));
  }
  RelId pushExtremumIntoUnion(RelId id, const RelNode &project) {
    using namespace compiler;
    if (plan.program.sourceDialect != Dialect::PostgreSQL || project.kind != RelKind::Project ||
        project.inputs.size() != 1 || project.keys.empty() || project.keys.size() > 64 ||
        project.keys.size() != project.outputs.size()) return id;
    auto finalAggregate = plan.nodes.at(project.inputs.front());
    if (finalAggregate.kind != RelKind::Aggregate || finalAggregate.query != project.query ||
        finalAggregate.distinct || finalAggregate.inputs.size() != 1 ||
        !finalAggregate.groupingSets.empty() || finalAggregate.keys.size() > 64) return id;
    const auto wrapper = plan.nodes.at(finalAggregate.inputs.front());
    if (wrapper.kind != RelKind::Subquery || wrapper.query != project.query ||
        wrapper.inputs.size() != 1 || !wrapper.table || wrapper.correlated ||
        wrapper.scalar || wrapper.materialized || wrapper.outputs.empty() ||
        wrapper.outputs.size() > 64) return id;
    if (!columns(finalAggregate.outputs, wrapper.outputs)) return id;
    const auto &query = modelQuery(plan, project.query);
    const auto &relation = query.tables.at(*wrapper.table);
    if (!query.catalogChecked || query.where || query.having || query.selectInto ||
        !relation.query || !relation.definition || !relation.definition->arityKnown) return id;
    const auto &unionQuery = modelQuery(plan, relation.query);
    if (!unionQuery.setOperation || unionQuery.limit || unionQuery.offset ||
        !unionQuery.orderBy.empty()) return id;
    const auto *operand = directSetInput(wrapper.inputs.front(), unionQuery.id);
    if (!operand || !isUnionAll(*operand) || !verifiedIntegerSetInputs(*operand, unionQuery) ||
        operand->outputs.size() != wrapper.outputs.size()) return id;
    auto united = *operand;

    const auto width = wrapper.outputs.size();
    std::vector<ExpressionRef> partialResults(width);
    std::vector<bool> grouping(width);
    c.charge(width * (sizeof(ExpressionRef) + sizeof(bool)));
    const auto integer = [&](ExpressionRef ref) {
      return signedIntegerBits(plan.program.types.get(modelExpression(plan, ref).sqlType));
    };
    const auto position = [&](ExpressionRef ref) -> std::optional<std::size_t> {
      if (ref.query != query.id || !integer(ref)) return {};
      const auto column = boundScalarColumn(plan, ref);
      if (!column || column->table != *wrapper.table) return {};
      const auto index = modelExpression(plan, ref).binding->column;
      if (index >= width || !same(wrapper.outputs.at(index), *column)) return {};
      return index;
    };
    for (const auto key : finalAggregate.keys) {
      c.stop();
      const auto index = position(key);
      if (!index || grouping[*index]) return id;
      grouping[*index] = true;
      partialResults[*index] = key;
    }
    bool hasExtremum = false;
    for (const auto ref : project.keys) {
      c.stop();
      if (ref.query != query.id) return id;
      if (const auto index = position(ref)) {
        if (!grouping[*index]) return id;
        continue;
      }
      if (nativeExtremum(ref).empty()) return id;
      const ExpressionRef argument{query.id, modelExpression(plan, ref).operands.front()};
      const auto index = position(argument);
      if (!index || partialResults[*index].expression || integer(ref) != integer(argument)) return id;
      partialResults[*index] = ref;
      hasExtremum = true;
    }
    // Each UNION field is represented by either a group key or one
    // same-type extremum. Extra output fields and multiple aggregate states
    // per field require independent projection/type mappings.
    if (!hasExtremum || std::any_of(partialResults.begin(), partialResults.end(),
        [](const auto ref) { return !ref.expression; })) return id;
    const auto originalArms = united.inputs;
    united.inputs.clear();
    united.outputs = wrapper.outputs;
    for (const auto arm : originalArms) {
      auto source = wrapper;
      source.inputs = {arm};
      auto partial = finalAggregate;
      partial.inputs = {append(std::move(source))};
      auto output = project;
      output.inputs = {append(std::move(partial))};
      output.keys = partialResults;
      output.outputs = wrapper.outputs;
      united.inputs.push_back(append(std::move(output)));
    }
    finalAggregate.inputs = {append(std::move(united))};
    auto result = project;
    result.inputs = {append(std::move(finalAggregate))};
    return append(std::move(result));
  }
  RelId pushSetFilter(RelId id, const RelNode &filter, const RelNode &wrapper) {
    using namespace compiler;
    if (!c.options.filterPushdown || plan.program.sourceDialect != Dialect::PostgreSQL ||
        filter.kind != RelKind::Filter || filter.predicates.empty() ||
        wrapper.kind != RelKind::Subquery || wrapper.correlated || wrapper.scalar || wrapper.materialized ||
        wrapper.inputs.size() != 1 || !wrapper.table || wrapper.query != filter.query)
      return id;
    const auto &query = modelQuery(plan, filter.query);
    if (query.kind != StatementKind::Select || *wrapper.table >= query.tables.size()) return id;
    const auto &table = query.tables[*wrapper.table];
    if (!table.query || !table.definition || !table.definition->complete || !table.definition->arityKnown)
      return id;
    const auto &setQuery = modelQuery(plan, table.query);
    if (!setQuery.setOperation || setQuery.setOperation->recursive || setQuery.limit || setQuery.offset ||
        !setQuery.orderBy.empty()) return id;
    const auto *set = directSetInput(wrapper.inputs.front(), setQuery.id);
    if (!set || set->outputs.size() != wrapper.outputs.size() ||
        !verifiedIntegerSetInputs(*set, setQuery)) return id;
    const auto left = set->inputs[0], right = set->inputs[1];
    for (auto predicate : filter.predicates) {
      c.stop();
      if (predicate.query != query.id) return id;
      const auto refs = totalPredicateColumns(plan, predicate);
      if (!refs || refs->empty() || !std::all_of(refs->begin(), refs->end(), [&](const auto &column) {
            return column.query == query.id && column.table == *wrapper.table && containsOutput(wrapper, column);
          })) return id;
    }
    // Each arm keeps the set alias's output coordinates. Predicates still
    // reference their original, immutable query expression arena.
    auto moved = *set;
    moved.outputs = filter.outputs;
    auto wrappedFilter = [&](RelId arm) {
      auto alias = wrapper;
      alias.inputs = {arm};
      auto selection = filter;
      selection.inputs = {append(std::move(alias))};
      return append(std::move(selection));
    };
    moved.inputs = {wrappedFilter(left), wrappedFilter(right)};
    return append(std::move(moved));
  }
  struct SetFilterArm {
    RelId scan;
    std::vector<ExpressionRef> projection, predicates;
  };
  std::optional<SetFilterArm> setFilterArm(RelId input, compiler::QueryId owner,
                                             const RelNode &set) {
    using namespace compiler;
    const auto &query=modelQuery(plan,owner);
    const auto &root=plan.nodes.at(input);
    const auto width=set.outputs.size();
    if (root.query!=owner || root.outputs.size()!=width || query.projections.size()!=width ||
        !query.catalogChecked || query.tables.size()!=1 || query.limit || query.offset ||
        query.setOperation || query.distinct || query.selectInto || !query.ctes.empty() ||
        !query.modifiers.empty())
      return {};
    const auto &relation=query.tables.front();
    const auto &source=relation.definition;
    if (!source || source->kind!=ObjectKind::Table || !source->complete || !source->arityKnown ||
        !source->rowSecurityDisabledKnown || !source->noDescendantsKnown ||
        source->schema.empty() || source->name.empty() || relation.name.empty() ||
        relation.query || relation.pseudo || !relation.columnAliases.empty())
      return {};

    auto selection=input;
    if (root.kind==RelKind::Project && root.inputs.size()==1 && root.keys.size()==width)
      selection=root.inputs.front();
    const auto &filter=plan.nodes.at(selection);
    if (filter.kind!=RelKind::Filter || filter.query!=owner || filter.inputs.size()!=1 ||
        filter.predicates.empty() || filter.predicates.size()>32)
      return {};
    auto scanId=filter.inputs.front();
    const auto *scan=&plan.nodes.at(scanId);
    if (scan->kind==RelKind::Project && scan->query==owner && scan->inputs.size()==1 &&
        scan->keys.size()==scan->outputs.size()) {
      const auto child=scan->inputs.front();
      for (std::size_t i=0; i<scan->outputs.size(); ++i) {
        const auto field=boundScalarColumn(plan,scan->keys[i]);
        if (!field || !same(*field,scan->outputs[i]) || !containsOutput(plan.nodes.at(child),*field))
          return {};
      }
      scanId=child;
      scan=&plan.nodes.at(scanId);
    }
    if (scan->kind!=RelKind::Scan || scan->query!=owner || scan->table!=0 ||
        !scan->inputs.empty() || !scan->predicates.empty() || properties(scanId).mayHaveEffects)
      return {};

    SetFilterArm arm{scanId,{},filter.predicates};
    c.charge((width+arm.predicates.size())*sizeof(ExpressionRef));
    for (std::size_t i=0; i<width; ++i) {
      c.stop();
      const ExpressionRef ref{owner,query.projections[i].expression};
      const auto field=boundScalarColumn(plan,ref);
      const auto bits=signedIntegerBits(plan.program.types.get(modelExpression(plan,ref).sqlType));
      if (!field || !bits || !containsOutput(filter,*field) ||
          (root.kind==RelKind::Project &&
           (root.keys[i].query!=ref.query || root.keys[i].expression!=ref.expression)) ||
          (root.kind!=RelKind::Project && !same(root.outputs[i],*field)))
        return {};
      arm.projection.push_back(ref);
    }
    for (const auto predicate:arm.predicates) {
      c.stop();
      if (predicate.query!=owner) return {};
      const auto refs=totalPredicateColumns(plan,predicate);
      if (!refs || !std::all_of(refs->begin(),refs->end(),[&](const auto &field) {
            return containsOutput(*scan,field);
          })) return {};
    }
    return arm;
  }
  RelId mergeSetFilters(RelId id,const RelNode &set) {
    using namespace compiler;
    if (plan.program.sourceDialect!=Dialect::PostgreSQL || set.kind!=RelKind::SetOperation ||
        set.setAlgorithm!=SetAlgorithm::Native || set.inputs.size()!=2 || set.outputs.empty() ||
        set.outputs.size()>64 || (set.setKind==SetKind::Union && !set.distinct))
      return id;
    const auto &owner=modelQuery(plan,set.query);
    if (!owner.setOperation || owner.setOperation->recursive ||
        owner.projections.size()!=set.outputs.size())
      return id;
    const auto left=setFilterArm(set.inputs[0],owner.setOperation->left,set);
    const auto right=setFilterArm(set.inputs[1],owner.setOperation->right,set);
    if (!left || !right) return id;
    const auto scan=plan.nodes.at(left->scan);
    auto &outputQuery=plan.program.queries.at(scan.query-1);
    const auto &rightQuery=modelQuery(plan,plan.nodes.at(right->scan).query);
    const auto &lhs=outputQuery.tables.front();
    const auto &rhs=rightQuery.tables.front();
    const auto &a=*lhs.definition, &b=*rhs.definition;
    if (std::tie(a.database,a.schema,a.name)!=std::tie(b.database,b.schema,b.name) ||
        lhs.inheritance!=rhs.inheritance || a.columns.size()!=b.columns.size())
      return id;
    const auto sameSourceField=[&](std::size_t ordinal) {
      if (ordinal>=a.columns.size()) return false;
      const auto &l=a.columns[ordinal], &r=b.columns[ordinal];
      return l.name==r.name && l.sqlType && r.sqlType && *l.sqlType==*r.sqlType;
    };
    c.charge(set.outputs.size()*sizeof(ColumnRef));
    std::vector<ColumnRef> projected;
    for (std::size_t i=0; i<set.outputs.size(); ++i) {
      c.stop();
      const auto l=left->projection[i], r=right->projection[i];
      const auto position=modelExpression(plan,l).binding->column;
      if (position!=modelExpression(plan,r).binding->column || !sameSourceField(position) ||
          !sameIntegerType(plan,l,r) ||
          !sameIntegerType(plan,l,{owner.id,owner.projections[i].expression}))
        return id;
      projected.push_back(*boundScalarColumn(plan,l));
    }
    // If multiple rows have equal projected values, AND / EXCEPT is valid
    // only when all of those rows agree on both branch predicates.
    const bool keysDetermineRow=set.setKind==SetKind::Union ||
        coversNonNullUniqueKey(properties(left->scan),projected);
    const auto canImport=[&](ExpressionRef ref,bool rightSide) {
      const auto &expr=modelExpression(plan,ref);
      for (auto arg:expr.operands) {
        const auto &value=modelExpression(plan,{ref.query,arg});
        if (value.kind!=ExprKind::Name) continue;
        const auto ordinal=value.binding->column;
        if (!sameSourceField(ordinal)) return false;
        const ColumnRef field{outputQuery.id,*scan.table,a.columns[ordinal].name};
        if ((rightSide && !containsOutput(scan,field)) ||
            (!keysDetermineRow && std::none_of(projected.begin(),projected.end(),
                [&](const auto &column){return same(column,field); })))
          return false;
      }
      return true;
    };
    for (const auto ref:left->predicates) if (!canImport(ref,false)) return id;
    for (const auto ref:right->predicates) if (!canImport(ref,true)) return id;
    if (outputQuery.expressions.size()+4*(left->predicates.size()+right->predicates.size())+2>
        c.options.maxNodes)
      return id;

    const auto add=[&](ModelExpression expr) {
      c.stop();
      c.charge(sizeof(expr)+expr.value.size()+expr.operands.size()*sizeof(ExprId));
      for (const auto &name:expr.name) c.charge(sizeof(name)+name.size()+sizeof(bool));
      const auto index=static_cast<ExprId>(outputQuery.expressions.size());
      outputQuery.expressions.push_back(std::move(expr));
      return index;
    };
    const auto import=[&](const auto &self,ExpressionRef ref)->ExprId {
      auto expr=modelExpression(plan,ref);
      if (expr.binding) {
        expr.binding->scope=outputQuery.id;
        expr.binding->table=*scan.table;
      }
      for (auto &arg:expr.operands) arg=self(self,{ref.query,arg});
      return add(std::move(expr));
    };
    const auto boolType=modelExpression(plan,left->predicates.front()).sqlType;
    const auto boolean=[&](ExprKind kind,std::string operation,std::vector<ExprId> operands) {
      ModelExpression expr;
      expr.kind=kind;
      expr.origin=set.origin;
      expr.value=std::move(operation);
      expr.operands=std::move(operands);
      expr.type=ValueType::Boolean;
      expr.sqlType=boolType;
      return add(std::move(expr));
    };
    const auto conjunction=[&](const std::vector<ExpressionRef> &refs,bool rebind) {
      ExprId result=0;
      for (auto ref:refs) {
        const auto expr=rebind?import(import,ref):ref.expression;
        result=result?boolean(ExprKind::Binary,"AND",{result,expr}):expr;
      }
      return result;
    };
    const auto leftCondition=conjunction(left->predicates,false);
    auto rightCondition=conjunction(right->predicates,true);
    if (set.setKind==SetKind::Except)
      rightCondition=boolean(ExprKind::TruthTest,"IS NOT TRUE",{rightCondition});
    const auto combined=boolean(ExprKind::Binary,set.setKind==SetKind::Union?"OR":"AND",
                                {leftCondition,rightCondition});
    RelNode filter;
    filter.kind=RelKind::Filter;
    filter.query=outputQuery.id;
    filter.origin=set.origin;
    filter.inputs={left->scan};
    filter.outputs=scan.outputs;
    filter.predicates={{outputQuery.id,combined}};
    RelNode project;
    project.kind=RelKind::Project;
    project.query=outputQuery.id;
    project.origin=set.origin;
    project.inputs={append(std::move(filter))};
    project.keys=left->projection;
    project.outputs=plan.nodes.at(set.inputs.front()).outputs;
    const auto projectionOutputs=project.outputs;
    auto result=append(std::move(project));
    if (set.distinct) {
      RelNode unique;
      unique.kind=RelKind::Aggregate;
      unique.distinct=true;
      unique.query=outputQuery.id;
      unique.origin=set.origin;
      unique.inputs={result};
      unique.keys=left->projection;
      unique.outputs=projectionOutputs;
      result=append(std::move(unique));
    }
    RelNode rename;
    rename.kind=RelKind::Rename;
    rename.query=set.query;
    rename.origin=set.origin;
    rename.inputs={result};
    rename.outputs=set.outputs;
    return append(std::move(rename));
  }
  std::optional<std::set<std::string>> commonWindowPartitions(const RelNode &project,RelId scanId) {
    using namespace compiler;
    const auto &scan=plan.nodes.at(scanId);
    std::optional<std::set<std::string>> common;
    for (auto key:project.keys) {
      c.stop();
      const auto &value=modelExpression(plan,key);
      if (value.kind!=ExprKind::Window) continue;
      if (key.query!=scan.query || !value.window || !value.window->partitions ||
          !value.window->frameUnit.empty() ||
          value.operands.size()!=1+value.window->partitions+value.window->ordering.size() ||
          value.operands.size()>65)
        return {};
      const auto &call=modelExpression(plan,{key.query,value.operands.front()});
      const auto op=builtinOperator(Dialect::PostgreSQL,call.name,call.nameQuoted,call.operands.size());
      if (call.kind!=ExprKind::Call || !op || op->kind!=RoutineKind::Window ||
          (op->name!="RANK" && op->name!="DENSE_RANK" && op->name!="ROW_NUMBER") ||
          (call.routineResolved && (call.routineSchema!="pg_catalog" ||
                                    call.routineKind!=RoutineKind::Window)))
        return {};
      std::set<std::string> partitions;
      std::vector<ColumnRef> ordered;
      for (std::size_t i=1;i<value.operands.size();++i) {
        c.stop();
        const ExpressionRef ref{key.query,value.operands[i]};
        const auto column=boundScalarColumn(plan,ref);
        if (!column || !containsOutput(scan,*column) ||
            !signedIntegerBits(plan.program.types.get(modelExpression(plan,ref).sqlType)))
          return {};
        c.charge(sizeof(ColumnRef)+column->name.size()+4*sizeof(void *));
        if (i<=value.window->partitions) partitions.insert(column->name);
        ordered.push_back(*column);
      }
      if (op->name=="ROW_NUMBER" && !coversNonNullUniqueKey(properties(scanId),ordered))
        return {};
      if (!common) common=std::move(partitions);
      else {
        for (auto it=common->begin();it!=common->end();) {
          if (!partitions.count(*it)) it=common->erase(it);
          else ++it;
        }
      }
      if (common->empty()) return {};
    }
    return common;
  }
  RelId markNonNullWindowSums(RelId id,RelNode window) {
    using namespace compiler;
    if (plan.program.sourceDialect!=Dialect::PostgreSQL || window.kind!=RelKind::Window ||
        window.inputs.size()!=1 || !window.nonNullWindowSums.empty() || window.windowGroups.empty() ||
        !modelQuery(plan,window.query).catalogChecked)
      return id;
    const auto &input=properties(window.inputs.front());
    for (const auto &group:window.windowGroups)
      for (const auto ref:group.functions) {
        c.stop();
        const auto &value=modelExpression(plan,ref);
        if (ref.query!=window.query || value.kind!=ExprKind::Window || !value.window ||
            value.operands.empty())
          continue;
        const auto &spec=*value.window;
        if (spec.exclusion!=WindowExclusion::None && spec.exclusion!=WindowExclusion::NoOthers)
          continue;
        if (!spec.frameUnit.empty() &&
            (spec.frameUnit!="ROWS" ||
             (spec.start!=WindowBound::UnboundedPreceding && spec.start!=WindowBound::Preceding &&
              spec.start!=WindowBound::CurrentRow) ||
             (spec.end!=WindowBound::CurrentRow && spec.end!=WindowBound::Following &&
              spec.end!=WindowBound::UnboundedFollowing)))
          continue;
        const auto &call=modelExpression(plan,{ref.query,value.operands.front()});
        const auto op=builtinOperator(Dialect::PostgreSQL,call.name,call.nameQuoted,call.operands.size());
        if (call.kind!=ExprKind::Call || call.distinct || call.operands.size()!=1 ||
            !op || op->name!="SUM" || op->kind!=RoutineKind::Aggregate ||
            (call.routineResolved && (call.routineSchema!="pg_catalog" ||
             call.routineKind!=RoutineKind::Aggregate || call.routineVolatility!=FunctionVolatility::Immutable)))
          continue;
        const ExpressionRef arg{ref.query,call.operands.front()};
        const auto field=boundScalarColumn(plan,arg);
        if (!field || !signedIntegerBits(plan.program.types.get(modelExpression(plan,arg).sqlType)))
          continue;
        if (std::any_of(input.nonNullColumns.begin(),input.nonNullColumns.end(),[&](const auto &entry){
              return same(entry,*field);
            }))
          window.nonNullWindowSums.push_back(ref);
      }
    return window.nonNullWindowSums.empty()?id:append(std::move(window));
  }
  enum class ProjectionFilterPath { Direct, Sort, Window };
  RelId pushProjectionFilter(RelId id, const RelNode &filter, RelNode wrapper,
                             ProjectionFilterPath path = ProjectionFilterPath::Direct) {
    using namespace compiler;
    if (!c.options.filterPushdown || plan.program.sourceDialect != Dialect::PostgreSQL ||
        filter.kind != RelKind::Filter || filter.predicates.empty() ||
        wrapper.kind != RelKind::Subquery || wrapper.scalar || wrapper.correlated || wrapper.materialized ||
        wrapper.inputs.size() != 1 || wrapper.query != filter.query || !wrapper.table)
      return id;
    const auto &outer = modelQuery(plan, filter.query);
    if (*wrapper.table >= outer.tables.size()) return id;
    const auto &derived = outer.tables[*wrapper.table];
    if (!derived.query || !derived.name.empty() || !derived.definition ||
        !derived.definition->complete || !derived.definition->arityKnown)
      return id;

    auto projectId = wrapper.inputs.front();
    std::optional<RelNode> order;
    if (path==ProjectionFilterPath::Sort) {
      const auto &sort = plan.nodes.at(projectId);
      if (sort.kind != RelKind::Sort || sort.query != derived.query || sort.inputs.size() != 1 ||
          sort.keys.empty() || sort.keys.size() > 64 || !pureSortKeys(sort)) return id;
      order = sort;
      projectId = sort.inputs.front();
    }
    auto project = plan.nodes.at(projectId);
    if (project.kind != RelKind::Project || project.query != derived.query ||
        project.inputs.size() != 1 || project.outputs.empty() ||
        project.outputs.size() != wrapper.outputs.size() ||
        project.keys.size() != project.outputs.size() ||
        (order && !columns(order->outputs, project.outputs)))
      return id;
    auto scanId = project.inputs.front();
    std::optional<RelNode> window;
    std::optional<RelNode> narrowWindowInput;
    std::optional<std::set<std::string>> partitions;
    if (path==ProjectionFilterPath::Window) {
      const auto &input=plan.nodes.at(scanId);
      if (input.kind!=RelKind::Window || input.query!=project.query || input.inputs.size()!=1)
        return id;
      window=input;
      scanId=input.inputs.front();
      const auto &narrow=plan.nodes.at(scanId);
      if (narrow.kind==RelKind::Project && narrow.query==project.query && narrow.inputs.size()==1 &&
          narrow.keys.size()==narrow.outputs.size()) {
        const auto &source=plan.nodes.at(narrow.inputs.front());
        for (std::size_t i=0;i<narrow.keys.size();++i) {
          const auto field=boundScalarColumn(plan,narrow.keys[i]);
          if (!field || !same(*field,narrow.outputs[i]) || !containsOutput(source,*field))
            return id;
        }
        narrowWindowInput=narrow;
        scanId=narrow.inputs.front();
      }
      partitions=commonWindowPartitions(project,scanId);
      if (!partitions) return id;
    }
    const auto &scan = plan.nodes.at(scanId);
    if (scan.kind != RelKind::Scan || properties(window?scanId:projectId).mayHaveEffects)
      return id;

    std::map<std::string, ExprId> inputExpression;
    std::set<std::string> aliasNames;
    for (std::size_t i = 0; i < project.keys.size(); ++i) {
      c.stop();
      const auto key = project.keys[i];
      const auto &aliased = wrapper.outputs[i];
      if (aliased.query!=filter.query || aliased.table!=*wrapper.table) return id;
      if (window && !aliasNames.insert(aliased.name).second) return id;
      if (window && modelExpression(plan,key).kind==ExprKind::Window) continue;
      const auto base = boundScalarColumn(plan, key);
      if (!base || key.query != project.query || !containsOutput(scan, *base))
        return id;
      if ((!partitions || partitions->count(base->name)) &&
          !inputExpression.emplace(aliased.name,key.expression).second)
        return id;
    }
    c.charge(2*filter.predicates.size()*sizeof(ExpressionRef));
    std::vector<ExpressionRef> early,residual;
    for (const auto predicate : filter.predicates) {
      c.stop();
      if (predicate.query != filter.query) return id;
      const auto used = totalPredicateColumns(plan, predicate);
      if (!used) return id;
      const bool movable=!used->empty() &&
          std::all_of(used->begin(), used->end(), [&](const auto &column) {
            return column.query == filter.query && column.table == *wrapper.table &&
                   inputExpression.count(column.name);
          });
      if (!movable && !window) return id;
      (movable?early:residual).push_back(predicate);
    }
    if (early.empty()) return id;

    // The verified predicate consists only of direct names, constants and a
    // span-safe comparison or NULL test. Rebind into the input's owning query.
    auto &inner = plan.program.queries.at(project.query - 1);
    const auto import = [&](const auto &self, ExpressionRef ref) -> ExprId {
      c.stop();
      auto expression = modelExpression(plan, ref);
      if (expression.kind == ExprKind::Name) {
        const auto column = boundScalarColumn(plan, ref);
        if (!column) c.fail(expression.origin, "unbound transposed filter column");
        return inputExpression.at(column->name);
      }
      c.charge(sizeof(ModelExpression) + expression.value.size() +
               expression.operands.size() * sizeof(ExprId));
      for (auto &child : expression.operands) child = self(self, ExpressionRef{ref.query, child});
      const auto id = static_cast<ExprId>(inner.expressions.size());
      inner.expressions.push_back(std::move(expression));
      return id;
    };
    auto moved = filter;
    moved.query = project.query;
    moved.inputs.front() = scanId;
    moved.outputs = scan.outputs;
    moved.predicates=std::move(early);
    for (auto &predicate : moved.predicates)
      predicate = {project.query, import(import, predicate)};
    project.inputs.front() = append(std::move(moved));
    if (window) {
      if (narrowWindowInput) {
        narrowWindowInput->inputs.front()=project.inputs.front();
        project.inputs.front()=append(std::move(*narrowWindowInput));
      }
      window->inputs.front()=project.inputs.front();
      project.inputs.front()=append(std::move(*window));
    }
    const auto projection = append(std::move(project));
    if (order) {
      order->inputs.front() = projection;
      wrapper.inputs.front() = append(std::move(*order));
    } else {
      wrapper.inputs.front() = projection;
    }
    const auto replaced=append(std::move(wrapper));
    if (residual.empty()) return replaced;
    auto remaining=filter;
    remaining.inputs={replaced};
    remaining.predicates=std::move(residual);
    return append(std::move(remaining));
  }
  RelId pushUnionTopN(RelId id, const RelNode &limit, RelNode sort) {
    using namespace compiler;
    if (plan.program.sourceDialect != Dialect::PostgreSQL || limit.kind != RelKind::Limit ||
        !literalLimit(limit) || !limit.rowLimit || *limit.rowLimit <= 0 ||
        *limit.rowLimit > 9007199254740991. - *limit.rowOffset ||
        sort.kind != RelKind::Sort || sort.query != limit.query || sort.inputs.size() != 1 ||
        sort.keys.empty() || sort.keys.size() != sort.descending.size() ||
        sort.keys.size() != sort.nullOrdering.size())
      return id;
    auto set = plan.nodes.at(sort.inputs.front());
    const auto &query = modelQuery(plan, sort.query);
    if (set.setKind != SetKind::Union || set.distinct || !columns(set.outputs, sort.outputs) ||
        !verifiedIntegerSetInputs(set, query) || query.tables.size() != 1 || !query.tables[0].pseudo)
      return id;
    // Two rows that tie on every output position are interchangeable even
    // around OFFSET. An ORDER BY prefix can select different visible rows.
    c.charge(set.outputs.size() * sizeof(char));
    std::vector<char> compared(set.outputs.size());
    for (auto ref : sort.keys) {
      c.stop();
      const auto &key = modelExpression(plan, ref);
      if (ref.query != query.id || !key.outputColumn || *key.outputColumn >= compared.size())
        return id;
      compared[*key.outputColumn] = true;
    }
    if (std::find(compared.begin(), compared.end(), false) != compared.end()) return id;
    for (auto &arm : set.inputs) {
      RelNode alias;
      alias.kind = RelKind::Subquery;
      alias.query = query.id;
      alias.origin = set.origin;
      alias.inputs = {arm};
      alias.table = 0;
      alias.outputs = set.outputs;
      auto localSort = sort;
      localSort.inputs = {append(std::move(alias))};
      auto localLimit = limit;
      localLimit.inputs = {append(std::move(localSort))};
      localLimit.rowLimit = *limit.rowLimit + *limit.rowOffset;
      localLimit.rowOffset = 0;
      localLimit.keys.clear();
      arm = append(std::move(localLimit));
    }
    sort.inputs = {append(std::move(set))};
    auto result = limit;
    result.inputs = {append(std::move(sort))};
    return append(std::move(result));
  }
  RelId pushOuterJoinTopN(RelId id, const RelNode &limit, RelNode sort) {
    using namespace compiler;
    if (plan.program.sourceDialect != Dialect::PostgreSQL || limit.kind != RelKind::Limit ||
        !literalLimit(limit) || !limit.rowLimit || *limit.rowLimit <= 0 ||
        *limit.rowLimit > 9007199254740991. - *limit.rowOffset ||
        sort.kind != RelKind::Sort || sort.query != limit.query || sort.inputs.size() != 1 ||
        sort.keys.empty() || sort.keys.size() > 64 || sort.keys.size() != sort.descending.size() ||
        sort.keys.size() != sort.nullOrdering.size())
      return id;
    const auto &query = modelQuery(plan, limit.query);
    if (!query.catalogChecked || query.tables.size() != 2) return id;
    auto project = plan.nodes.at(sort.inputs.front());
    if (project.kind != RelKind::Project || project.query != query.id || project.inputs.size() != 1 ||
        project.keys.size() != project.outputs.size() || project.keys.size() > 256 ||
        !columns(project.outputs, sort.outputs))
      return id;
    auto join = plan.nodes.at(project.inputs.front());
    if (join.kind != RelKind::Join || join.query != query.id || join.inputs.size() != 2 ||
        join.fixedOrder || join.distinct || join.correlated ||
        (join.join != JoinKind::Left && join.join != JoinKind::Right) ||
        join.predicates.empty() || join.predicates.size() > 64)
      return id;
    const std::size_t preserved = join.join == JoinKind::Left ? 0 : 1;
    const auto keepId = join.inputs[preserved], matchId = join.inputs[1-preserved];
    const auto &keep = plan.nodes.at(keepId);
    const auto &matched = plan.nodes.at(matchId);
    const auto &matchScan = matched.kind == RelKind::Filter && matched.inputs.size() == 1
        ? plan.nodes.at(matched.inputs.front()) : matched;
    if (keep.kind != RelKind::Scan || matchScan.kind != RelKind::Scan ||
        keep.query != query.id || matchScan.query != query.id ||
        !keep.table || !matchScan.table || keep.table == matchScan.table ||
        properties(keepId).mayHaveEffects || properties(matchId).mayHaveEffects)
      return id;

    for (auto ref : project.keys) {
      c.stop();
      if (ref.query != query.id || !boundScalarColumn(plan, ref)) return id;
    }
    c.charge((join.predicates.size() + sort.keys.size()) * (sizeof(ColumnRef) + sizeof(ExpressionRef)));
    std::vector<ColumnRef> matchedKeys;
    matchedKeys.reserve(join.predicates.size());
    for (auto ref : join.predicates) {
      c.stop();
      const auto equality = integerEqualityColumns(plan, ref);
      if (ref.query != query.id || !equality) return id;
      const auto &[first, second] = *equality;
      if (first.table == matchScan.table && second.table == keep.table)
        matchedKeys.push_back(first);
      else if (second.table == matchScan.table && first.table == keep.table)
        matchedKeys.push_back(second);
      else
        return id;
    }
    if (!coversNonNullUniqueKey(properties(matchId), matchedKeys)) return id;

    auto localSort = sort;
    std::vector<ColumnRef> orderColumns;
    orderColumns.reserve(sort.keys.size());
    for (auto &ref : localSort.keys) {
      c.stop();
      if (ref.query != query.id) return id;
      const auto &key = modelExpression(plan, ref);
      if (key.outputColumn) {
        if (*key.outputColumn >= project.keys.size()) return id;
        ref = project.keys[*key.outputColumn];
      }
      const auto column = boundScalarColumn(plan, ref);
      const auto type = modelExpression(plan, ref).sqlType;
      if (!column || column->table != keep.table || !signedIntegerBits(plan.program.types.get(type)))
        return id;
      orderColumns.push_back(*column);
    }
    // Each retained row produces exactly one joined row, and the preserved
    // non-null key makes the ordering total. Thus the prefix contains the
    // same rows even with OFFSET and NULLs in earlier ordering columns.
    if (!coversNonNullUniqueKey(properties(keepId), orderColumns)) return id;
    localSort.inputs = {keepId};
    localSort.outputs = keep.outputs;
    auto localLimit = limit;
    localLimit.outputs = keep.outputs;
    localLimit.rowLimit = *limit.rowLimit + *limit.rowOffset;
    localLimit.rowOffset = 0;
    localLimit.keys.clear();
    localLimit.inputs = {append(std::move(localSort))};
    join.inputs[preserved] = append(std::move(localLimit));
    project.inputs = {append(std::move(join))};
    sort.inputs = {append(std::move(project))};
    auto result = limit;
    result.inputs = {append(std::move(sort))};
    return append(std::move(result));
  }
  static bool containsOutput(const RelNode &node, const ColumnRef &column) {
    return std::any_of(node.outputs.begin(), node.outputs.end(),
                       [&](const auto &slot) { return same(slot, column); });
  }
  RelId pruneSetInput(RelId id, const RelNode &set) {
    using compiler::SetKind;
    if (plan.program.sourceDialect != compiler::Dialect::PostgreSQL ||
        set.kind != RelKind::SetOperation || set.setAlgorithm!=SetAlgorithm::Native ||
        set.inputs.size()!=2 ||
        set.setKind == SetKind::Intersect || set.outputs.empty())
      return id;
    const auto &left = properties(set.inputs[0]);
    const auto &right = properties(set.inputs[1]);
    if (left.mayHaveEffects || right.mayHaveEffects) return id;
    const bool leftEmpty = left.rows.maximum == 0;
    const bool rightEmpty = right.rows.maximum == 0;
    if (leftEmpty == rightEmpty || (set.setKind == SetKind::Except && !rightEmpty)) return id;
    const auto &output = modelQuery(plan, set.query);
    if (!output.setOperation || output.projections.size() != set.outputs.size()) return id;
    const auto &lhs = modelQuery(plan, output.setOperation->left);
    const auto &rhs = modelQuery(plan, output.setOperation->right);
    if (lhs.projections.size() != set.outputs.size() || rhs.projections.size() != set.outputs.size() ||
        plan.nodes.at(set.inputs[0]).outputs.size() != set.outputs.size() ||
        plan.nodes.at(set.inputs[1]).outputs.size() != set.outputs.size()) return id;
    for (std::size_t i = 0; i < set.outputs.size(); ++i) {
      c.stop();
      const ExpressionRef result{output.id, output.projections[i].expression};
      const ExpressionRef a{lhs.id, lhs.projections[i].expression}, b{rhs.id, rhs.projections[i].expression};
      const auto scalarInteger = [&](ExpressionRef ref) {
        const auto &type = plan.program.types.get(modelExpression(plan, ref).sqlType);
        return !type.arrayDimensions && !type.sqliteAffinity && type.opaqueName.empty();
      };
      if (set.outputs[i].name == "*" || !scalarInteger(result) || !scalarInteger(a) || !scalarInteger(b) ||
          !sameIntegerType(plan, result, a) || !sameIntegerType(plan, result, b)) return id;
    }
    RelNode rename;
    rename.kind = RelKind::Rename;
    rename.query = set.query;
    rename.origin = set.origin;
    rename.outputs = set.outputs;
    rename.inputs = {set.inputs[leftEmpty ? 1 : 0]};
    const auto surviving=append(std::move(rename));
    if (!set.distinct) return surviving;
    RelNode distinct;
    distinct.kind=RelKind::Aggregate;
    distinct.distinct=true;
    distinct.query=set.query;
    distinct.origin=set.origin;
    distinct.inputs={surviving};
    distinct.outputs=set.outputs;
    for (const auto &field:output.projections)
      distinct.keys.push_back({output.id,field.expression});
    return append(std::move(distinct));
  }
  RelId deriveJoinNullFilters(RelId id, RelNode join) {
    using namespace compiler;
    if (plan.program.sourceDialect != Dialect::PostgreSQL || join.kind != RelKind::Join ||
        join.inputs.size() != 2 || join.fixedOrder || join.distinct || join.correlated ||
        join.predicates.empty() || join.predicates.size() > 64 ||
        (join.join != JoinKind::Inner && join.join != JoinKind::Left && join.join != JoinKind::Right)) return id;
    const auto &lhs = plan.nodes.at(join.inputs[0]);
    const auto &rhs = plan.nodes.at(join.inputs[1]);
    auto &q = plan.program.queries.at(join.query - 1);
    if (!q.catalogChecked || lhs.kind != RelKind::Scan || rhs.kind != RelKind::Scan ||
        !lhs.table || !rhs.table || lhs.query != q.id || rhs.query != q.id || lhs.table == rhs.table) return id;
    const auto &leftFacts = properties(join.inputs[0]), &rightFacts = properties(join.inputs[1]);
    if (leftFacts.mayHaveEffects || rightFacts.mayHaveEffects) return id;
    const std::array available{&leftFacts.nonNullColumns, &rightFacts.nonNullColumns};
    std::array<std::map<std::size_t,ExpressionRef>,2> filters;
    for (const auto ref : join.predicates) {
      c.stop();
      const auto equality = integerEqualityColumns(plan, ref, IntegerEquality::ExactSigned);
      if (ref.query != q.id || !equality) return id;
      const bool reversed = equality->first.table == *rhs.table && equality->second.table == *lhs.table;
      if (!reversed && !(equality->first.table == *lhs.table && equality->second.table == *rhs.table)) return id;
      const auto &condition = modelExpression(plan, ref);
      const auto record = [&](std::size_t side, const ColumnRef &column, ExprId expression) {
        const auto &facts = *available[side];
        if (std::any_of(facts.begin(), facts.end(), [&](const auto &known) { return same(known,column); })) return;
        const auto &bound = *modelExpression(plan, {q.id, expression}).binding;
        if (filters[side].emplace(bound.column, ExpressionRef{q.id,expression}).second)
          c.charge(sizeof(std::pair<std::size_t,ExpressionRef>) + 4 * sizeof(void *));
      };
      if (join.join != JoinKind::Left)
        record(0, reversed ? equality->second : equality->first, condition.operands[reversed ? 1 : 0]);
      if (join.join != JoinKind::Right)
        record(1, reversed ? equality->first : equality->second, condition.operands[reversed ? 0 : 1]);
    }
    if (filters[0].empty() && filters[1].empty()) return id;
    if (q.expressions.size() + filters[0].size() + filters[1].size() > c.options.maxNodes) return id;
    const auto boolean = plan.program.types.intern(categoryType(ValueType::Boolean, Nullability::NotNull));
    for (std::size_t i = 0; i < filters.size(); ++i) {
      if (filters[i].empty()) continue;
      RelNode filter;
      filter.kind = RelKind::Filter;
      filter.query = q.id;
      filter.origin = join.origin;
      filter.inputs = {join.inputs[i]};
      filter.outputs = plan.nodes.at(join.inputs[i]).outputs;
      for (const auto &item : filters[i]) {
        c.stop();
        ModelExpression test;
        test.kind = ExprKind::IsNull;
        test.value = "IS NOT NULL";
        test.origin = modelExpression(plan, item.second).origin;
        test.type = ValueType::Boolean;
        test.sqlType = boolean;
        test.operands = {item.second.expression};
        c.charge(sizeof(test) + test.value.size() + sizeof(ExprId));
        filter.predicates.push_back({q.id,static_cast<ExprId>(q.expressions.size())});
        q.expressions.push_back(std::move(test));
      }
      join.inputs[i] = append(std::move(filter));
    }
    return append(std::move(join));
  }
  RelId rule(RelId id,HepRule rule) {
    const auto n=plan.nodes.at(id);
    if (rule == HepRule::DecorrelateExists)
      return decorrelateExists(id, n);
    if (rule == HepRule::RewriteInSubquery)
      return rewriteInSubquery(id, n);
    if (rule == HepRule::MarkToSemiOrAntiJoin)
      return rewriteInSubquery(id,n,true);
    if (rule == HepRule::RemoveUnusedOuterJoin)
      return removeUnusedOuterJoin(id, n);
    if (rule == HepRule::AggregateRemove)
      return removeSingletonAggregate(id, n);
    if (rule == HepRule::AggregateJoinRemove)
      return removeAggregateJoin(id,n);
    if (rule == HepRule::AggregateCaseToFilter)
      return filterCaseAggregates(id,n);
    if (rule == HepRule::AggregateReduceFunctionsOnGroupKeys)
      return reduceGroupKeyExtrema(id,n);
    if (rule == HepRule::AggregateFilterToFilteredAggregate)
      return moveGlobalAggregateFilter(id,n);
    if (rule == HepRule::AggregateExtractProject)
      return trimUnaryInput(id,n,RelKind::Aggregate);
    if (rule == HepRule::AggregateProjectMerge)
      return mergeAggregateProject(id,n);
    if (rule == HepRule::AggregateExpandDistinct)
      return expandDistinctAggregates(id,n);
    if (rule == HepRule::ProjectAggregateMerge)
      return pruneAggregateOutputs(id,n);
    if (rule==HepRule::AggregateJoinTranspose)
      return preaggregateJoinInput(id,n);
    if (rule == HepRule::ProjectJoinJoinRemove || rule==HepRule::AggregateJoinJoinRemove)
      return removeChainedOuterInput(id,n,rule==HepRule::AggregateJoinJoinRemove);
    if (rule==HepRule::ProjectOverSumToSum0)
      return markNonNullWindowSums(id,n);
    if (rule == HepRule::AggregateMerge)
      return mergeExtremumAggregates(id,n);
    if (rule == HepRule::AggregateUnionTranspose)
      return pushExtremumIntoUnion(id,n);
    if (rule == HepRule::AggregateGroupingSetsToUnion)
      return splitGroupingSets(id,n);
    if (rule == HepRule::JoinDeriveNotNull)
      return deriveJoinNullFilters(id, n);
    if (rule == HepRule::ValuesReduce)
      return reduceValues(id, n);
    if (rule == HepRule::ProjectMerge)
      return mergeDirectProjections(id, n);
    if (rule == HepRule::AggregateProjectPullUpConstants)
      return pullUpConstantGroupKeys(id,n);
    if (rule == HepRule::ProjectJoinTranspose)
      return trimJoinInputs(id,n);
    if (rule == HepRule::ProjectFilterTranspose)
      return trimUnaryInput(id,n,RelKind::Filter);
    if (rule == HepRule::ProjectWindowTranspose)
      return trimUnaryInput(id,n,RelKind::Window);
    if (rule == HepRule::ProjectCorrelateTranspose)
      return trimCorrelateInputs(id,n);
    if (rule == HepRule::AggregateReduceFunctions)
      return reduceBigintAverage(id,n);
    if (rule == HepRule::ProjectSetOpTranspose)
      return pushProjectionIntoUnion(id,n);
    if (rule == HepRule::UnionPullUpConstants)
      return pullUpUnionConstants(id,n);
    if (rule == HepRule::SetOpToFilter)
      return mergeSetFilters(id,n);
    if (rule==HepRule::CalcRemove)
      return isIdentityCalc(plan,n)?n.inputs.front():id;
    if (rule==HepRule::CalcMerge) {
      const auto upper=asScalarCalc(c,n);
      if (!upper) return id;
      const auto lower=asScalarCalc(c,plan.nodes.at(upper->inputs.front()));
      if (!lower) return id;
      auto merged=mergeScalarCalcs(c,*upper,*lower);
      return merged?append(std::move(*merged)):id;
    }
    if (rule==HepRule::CalcSplit) {
      if (n.kind!=RelKind::Calc || !asScalarCalc(c,n)) return id;
      auto source=n.inputs.front();
      if (!n.predicates.empty()) {
        RelNode filter;
        filter.kind=RelKind::Filter;
        filter.query=n.query;
        filter.origin=n.origin;
        filter.inputs={source};
        filter.outputs=plan.nodes.at(source).outputs;
        filter.predicates=n.predicates;
        source=append(std::move(filter));
      }
      if (n.keys.empty()) return source;
      auto projected=n;
      projected.kind=RelKind::Project;
      projected.inputs={source};
      projected.predicates.clear();
      return append(std::move(projected));
    }
    // Empty results preserve this operator's output identity, including aliases
    // and set/outer-join output names. Never return an input with a different
    // row shape. Hard metadata, not estimated row counts, proves emptiness.
    if(rule==HepRule::PruneEmpty) {
      switch(n.kind) {
      case RelKind::Filter:case RelKind::Project:case RelKind::Sort:
      case RelKind::Limit:case RelKind::Window:case RelKind::Aggregate:
      case RelKind::Join:case RelKind::SetOperation:break;
      default:return id;
      }
      const auto expected=(n.kind==RelKind::Join || n.kind==RelKind::SetOperation)?2u:1u;
      if(n.inputs.size()!=expected)return id; // dependent subqueries remain barriers
      const auto &props=properties(id);
      if(props.mayHaveEffects)return id;
      if(props.rows.maximum != 0) return pruneSetInput(id, n);
      RelNode empty;empty.kind=RelKind::Values;empty.query=n.query;empty.origin=n.origin;
      empty.outputs=n.outputs;empty.valueCount=0;
      preserveValuesTypes(id,empty);
      return append(std::move(empty));
    }
    if(n.inputs.size()!=1)return id;
    const auto input=n.inputs[0];const auto child=plan.nodes.at(input);
    if(!columns(n.outputs,child.outputs))return id;
    switch(rule) {
    case HepRule::PruneEmpty:case HepRule::DecorrelateExists:case HepRule::RemoveUnusedOuterJoin:
    case HepRule::RewriteInSubquery:case HepRule::MarkToSemiOrAntiJoin:
    case HepRule::AggregateRemove:case HepRule::AggregateJoinRemove:
    case HepRule::JoinDeriveNotNull:
    case HepRule::AggregateCaseToFilter:case HepRule::AggregateMerge:case HepRule::ValuesReduce:
    case HepRule::AggregateReduceFunctionsOnGroupKeys:case HepRule::AggregateExtractProject:
    case HepRule::AggregateFilterToFilteredAggregate:
    case HepRule::AggregateProjectMerge:case HepRule::AggregateExpandDistinct:
    case HepRule::ProjectAggregateMerge:
    case HepRule::AggregateJoinTranspose:
    case HepRule::ProjectJoinJoinRemove:case HepRule::AggregateJoinJoinRemove:case HepRule::ProjectOverSumToSum0:
    case HepRule::AggregateUnionTranspose:case HepRule::ProjectMerge:
    case HepRule::AggregateGroupingSetsToUnion:case HepRule::AggregateProjectPullUpConstants:
    case HepRule::ProjectJoinTranspose:case HepRule::ProjectFilterTranspose:
    case HepRule::ProjectWindowTranspose:case HepRule::ProjectCorrelateTranspose:
    case HepRule::AggregateReduceFunctions:
    case HepRule::ProjectSetOpTranspose:case HepRule::UnionPullUpConstants:
    case HepRule::SetOpToFilter:case HepRule::CalcMerge:case HepRule::CalcSplit:case HepRule::CalcRemove:
    case HepRule::Count:
      break; // handled before unary / output-shape restrictions
    case HepRule::SimplifyOuterJoin:
      return simplifyOuterJoin(id, n, child);
    case HepRule::FilterAggregateTranspose:
      return pushAggregateFilter(id, n, child);
    case HepRule::FilterCorrelate:
      return pushCorrelateFilter(id, n, child);
    case HepRule::FilterUnpivotTranspose:
      return pushUnpivotFilter(id,n,child);
    case HepRule::FilterSetOpTranspose:
      return pushSetFilter(id, n, child);
    case HepRule::FilterProjectTranspose:
      return pushProjectionFilter(id, n, child);
    case HepRule::FilterSortTranspose:
      return pushProjectionFilter(id, n, child, ProjectionFilterPath::Sort);
    case HepRule::FilterWindowTranspose:
      return pushProjectionFilter(id,n,child,ProjectionFilterPath::Window);
    case HepRule::SortUnionTranspose:
      return pushUnionTopN(id, n, child);
    case HepRule::SortJoinTranspose:
      return pushOuterJoinTopN(id, n, child);
    case HepRule::FilterNullability: {
      if(n.kind!=RelKind::Filter || n.predicates.empty())break;
      if(n.predicates.size()>4096)throw HepLimit{};
      c.charge(n.predicates.size()*sizeof(ExpressionRef));
      const auto &props=properties(input);
      bool impossible=false,pureNullTests=true;auto reduced=n;reduced.predicates.clear();
      for(auto ref:n.predicates) {
        c.stop();const auto &e=modelExpression(plan,ref);
        if(e.kind!=compiler::ExprKind::IsNull || e.operands.size()!=1 ||
           (e.value!="IS NULL" && e.value!="IS NOT NULL")) {pureNullTests=false;reduced.predicates.push_back(ref);continue;}
        const ExpressionRef value{ref.query,e.operands[0]};
        const auto identity=columnIdentity(value);const auto col=resolveColumn(plan,value);
        if(!identity || !col || (plan.program.sourceDialect!=compiler::Dialect::SQLite && !scalarType(value))) {pureNullTests=false;reduced.predicates.push_back(ref);continue;}
        const bool nonNull=std::any_of(props.nonNullColumns.begin(),props.nonNullColumns.end(),[&](const auto &known){return same(known,*col);});
        if(nonNull && e.value=="IS NOT NULL")continue;
        if(nonNull)impossible=true;
        reduced.predicates.push_back(ref);
      }
      if(impossible && pureNullTests && !props.mayHaveEffects && (child.kind==RelKind::Scan || child.kind==RelKind::Values)) {
        RelNode empty;empty.kind=RelKind::Values;empty.query=n.query;empty.origin=n.origin;empty.outputs=n.outputs;empty.valueCount=0;
        preserveValuesTypes(id,empty);
        return append(std::move(empty));
      }
      if(reduced.predicates.empty())return input;
      if(reduced.predicates.size()!=n.predicates.size())return append(std::move(reduced));
      break;
    }
    case HepRule::FilterMerge:
      if(c.options.filterPushdown && n.kind==RelKind::Filter && child.kind==RelKind::Filter && child.inputs.size()==1) {
        for(auto ref:n.predicates)if(!movableExpression(c,ref))return id;
        for(auto ref:child.predicates)if(!movableExpression(c,ref))return id;
        auto merged=n;merged.inputs=child.inputs;merged.predicates=child.predicates;
        merged.predicates.insert(merged.predicates.end(),n.predicates.begin(),n.predicates.end());
        if(merged.predicates.size()>4096)throw HepLimit{};
        return append(std::move(merged));
      }
      break;
    case HepRule::ProjectRemove:
      if(n.kind==RelKind::Project && !n.keys.empty()) {
        if(n.keys.size()==1 && modelExpression(plan,n.keys[0]).kind==compiler::ExprKind::Star) {
          const auto &star=modelExpression(plan,n.keys[0]);
          return star.name.empty() || !child.outputs.empty()?input:id;
        }
        if(n.keys.size()!=child.outputs.size())break;
        for(std::size_t i=0;i<n.keys.size();++i) {
          const auto &e=modelExpression(plan,n.keys[i]);
          if(e.kind!=compiler::ExprKind::Name || e.localSymbol)return id;
          auto col=resolveColumn(plan,n.keys[i]);if(!col || !same(*col,child.outputs[i]))return id;
        }
        return input;
      }
      break;
    case HepRule::SortRemove:
      if(n.kind==RelKind::Sort && child.kind==RelKind::Sort && child.inputs.size()==1 &&
          expressions(n.keys,child.keys) && n.descending==child.descending && n.nullOrdering==child.nullOrdering && pureSortKeys(n))return input;
      break;
    case HepRule::SortLimitRemove: {
      if(n.kind!=RelKind::Sort && n.kind!=RelKind::Limit)break;
      const auto &props=properties(input);
      if(!props.rows.maximum || props.mayHaveEffects)break;
      if(n.kind==RelKind::Sort) {
        if(*props.rows.maximum<=1 && pureSortKeys(n))return input;
      } else if(literalLimit(n) && n.rowLimit && *n.rowLimit>0 && *n.rowOffset==0 &&
                *props.rows.maximum<=static_cast<std::uint64_t>(*n.rowLimit))return input;
      break;
    }
    case HepRule::SortDuplicateKeys: {
      if(n.kind!=RelKind::Sort || n.keys.size()<2 || n.descending.size()!=n.keys.size() || n.nullOrdering.size()!=n.keys.size())break;
      if(n.keys.size()>4096)throw HepLimit{};
      c.charge(n.keys.size()*(sizeof(Binding)+96+sizeof(ExpressionRef)+sizeof(std::string)));
      std::set<Binding> seen;auto reduced=n;
      reduced.keys.clear();reduced.descending.clear();reduced.nullOrdering.clear();
      for(std::size_t i=0;i<n.keys.size();++i) {
        c.stop();auto identity=columnIdentity(n.keys[i]);
        if(identity && !seen.insert(*identity).second)continue;
        reduced.keys.push_back(n.keys[i]);reduced.descending.push_back(n.descending[i]);reduced.nullOrdering.push_back(n.nullOrdering[i]);
      }
      if(reduced.keys.size()!=n.keys.size())return append(std::move(reduced));
      break;
    }
    case HepRule::SortConstantKeys:
      return removeConstantSortKeys(id, n);
    case HepRule::AggregateDuplicateKeys:
      return removeDependentGroupKeys(id, n, child);
    case HepRule::LimitMerge:
      if(n.kind==RelKind::Limit && child.kind==RelKind::Limit && child.inputs.size()==1 && literalLimit(n) && literalLimit(child) &&
          !properties(child.inputs[0]).mayHaveEffects) {
        auto merged=n;
        const double offset=*n.rowOffset+*child.rowOffset;
        if(offset>9007199254740991.)return id;
        merged.inputs=child.inputs;merged.rowOffset=offset;
        if(child.rowLimit) {
          auto remaining=std::max(0.,*child.rowLimit-*n.rowOffset);
          merged.rowLimit=n.rowLimit?std::min(*n.rowLimit,remaining):remaining;
        }
        // Both offsets/limits are now represented by relational literals. Old
        // source expression IDs are retained only on the original DAG.
        merged.keys.clear();return append(std::move(merged));
      }
      break;
    case HepRule::DistinctRemove:
      if(n.kind==RelKind::Aggregate && n.distinct) {
        const auto &props=properties(input);
        if(props.rows.maximum && *props.rows.maximum<=1)return input;
        for(const auto &key:props.uniqueKeys)
          if(!key.empty() && std::all_of(key.begin(),key.end(),[&](const auto &col){return std::any_of(child.outputs.begin(),child.outputs.end(),[&](const auto &out){return same(out,col);});}))return input;
      }
      break;
    }
    return id;
  }
  RelId apply(RelId id,const std::vector<HepRule> &rules) {
    for(auto selected:rules) {
      c.stop();
      if(c.result.heuristicMatches>=c.options.heuristic.maxMatches)throw HepLimit{};
      ++c.result.heuristicMatches;
      auto after=rule(id,selected);
      if(after!=id) {
        c.charge(sizeof(RuleEvent)+192);
        std::string reason="有界 Hep 程序：满足形状、稳定输出与规则前提；未改写原 SQL";
        if(selected==HepRule::PruneEmpty) {
          const auto kind=plan.nodes.at(after).kind;
          reason=kind==RelKind::Rename || kind==RelKind::Aggregate
              ? "集合分支硬行数上界为零，删除该分支，按位置保留输出列和原集合的重复行语义"
              : "硬行数上界为零且完整关系没有已知求值影响，保留原输出身份。空输入上的全局聚合仍可能返回行";
        }
        if(selected==HepRule::FilterNullability)reason="已验证的非空约束、主键或输入谓词与列绑定证明该列非空。证明沿保留列身份的关系节点传播";
        if(selected==HepRule::SortLimitRemove)reason="硬行数上界证明排序 / 正整数 LIMIT 冗余；输入和排序键没有已知求值影响，非零 OFFSET 保留；统计估计不作证明";
        if(selected==HepRule::SortConstantKeys)reason="排序键由字面量、精确整数等值或 IS NULL 固定。保留其他排序键的方向与 NULL 顺序，WITH TIES 全为常量时保留一个键";
        if(selected==HepRule::SortDuplicateKeys)reason="重复排序键解析到相同作用域 / 表 / 列绑定；保留首次出现的方向和 NULL 顺序，不合并函数或未知列";
        if(selected==HepRule::AggregateDuplicateKeys)reason="分组保留完整、已验证且非空的唯一键，去掉下游未引用的依赖列。保留输出身份和分组集合语义";
        if(selected==HepRule::AggregateRemove)reason="完整非空唯一键证明每组最多一行。用同类型列值、计数或加宽转换替代受支持的聚合";
        if(selected==HepRule::AggregateJoinRemove)reason="保留侧行都存在，另一侧只能增加重复。仅引用保留侧的去重、极值和已验证 DISTINCT 聚合可以消除匹配侧";
        if(selected==HepRule::AggregateCaseToFilter)reason="单分支 CASE 的其余输入为 NULL，原生聚合会忽略。保留比较逻辑、结果类型和空组语义，把条件放入各自的 FILTER";
        if(selected==HepRule::AggregateReduceFunctionsOnGroupKeys)reason="同一普通分组内的键值相同。直接读取定宽整数分组键代替同类型 MIN 或 MAX，保留 NULL 分组";
        if(selected==HepRule::AggregateFilterToFilteredAggregate)reason="全局聚合的输入条件可放进每个原生聚合的 FILTER。条件为未知的行不参与聚合，空输入仍返回一行";
        if(selected==HepRule::AggregateExtractProject)reason="先选择分组、分组集合和聚合输出实际引用的字段，保留输出位置、类型与聚合参数";
        if(selected==HepRule::AggregateProjectMerge)reason="按派生表输出位置把分组键和原生聚合参数绑定回输入字段，保留底层过滤、列名和类型";
        if(selected==HepRule::AggregateExpandDistinct)reason="先按分组键和同一个 DISTINCT 参数去重，再运行保留类型的普通聚合。空全局组、NULL 和输出别名保持不变";
        if(selected==HepRule::ProjectAggregateMerge)reason="省去外层投影未引用且可安全舍弃的派生聚合字段，保留分组和引用列的原绑定";
        if(selected==HepRule::AggregateJoinTranspose)reason="先按左侧连接键和相关分组键计算整数极值，再连接并做同种极值聚合。保留 NULL 和空全局组";
        if(selected==HepRule::ProjectJoinJoinRemove || selected==HepRule::AggregateJoinJoinRemove)
          reason="中间左连接的匹配键唯一，后续条件和结果都未使用该输入，可以移除这一层而保持整行重数";
        if(selected==HepRule::ProjectOverSumToSum0)reason="每个窗口帧都保留当前行，且整数求和参数已验证非空。运行时可省去每帧的非空计数";
        if(selected==HepRule::AggregateMerge)reason="外层分组键是内层分组键的子集，已验证的同类型 MIN/MIN 与 MAX/MAX 可只聚合一次，保留 NULL 和全局空组";
        if(selected==HepRule::AggregateUnionTranspose)reason="在 UNION ALL 各分支先计算同类型整数 MIN/MAX，再对同一分组键合并极值。保留 NULL、重复输入与空组语义";
        if(selected==HepRule::AggregateProjectPullUpConstants)reason="已验证的筛选让部分整数分组键保持同值。聚合仅使用变化的键，投影恢复原类型的常量，至少保留一个分组键以维护空输入语义";
        if(selected==HepRule::AggregateGroupingSetsToUnion)reason="各分组集合独立聚合后用 UNION ALL 合并。缺席的键补入同类型 NULL，按分支计算 GROUPING，保留重复集合和空输入上的总计";
        if(selected==HepRule::ValuesReduce)reason="在已知类型的字面量 VALUES 行上预先计算整数/布尔过滤，或按列位置完成直接投影。保留列名、重复行和三值逻辑";
        if(selected==HepRule::ProjectMerge)reason="按位置合并派生表的直接列投影。保留结果列名、顺序、类型与底层过滤，避免重复读取和投影无用字段";
        if(selected==HepRule::JoinDeriveNotNull)reason="已验证的整数等值键在 NULL 时无法匹配。仅过滤两侧内连接输入或外连接的可补 NULL 输入";
        if(selected==HepRule::SimplifyOuterJoin)reason="WHERE 的标量 IS NOT NULL 拒绝对应一侧的补 NULL 行；保留原过滤条件、列身份与基线，统计估计不作证明";
        if(selected==HepRule::DecorrelateExists)reason="EXISTS / NOT EXISTS 只依赖是否有同类型整数等值匹配；保留左侧重复行，右侧重复不放大结果，NULL 不构成匹配";
        if(selected==HepRule::RewriteInSubquery)reason="WHERE IN 只保留找到等值匹配的行；NOT IN 必须证明两个键都非空，才能使用普通反连接";
        if(selected==HepRule::MarkToSemiOrAntiJoin)reason="IS TRUE 仅接受存在等值匹配的行。IS NOT TRUE 同时接受 FALSE 与 UNKNOWN，因此可用普通反连接";
        if(selected==HepRule::RemoveUnusedOuterJoin)reason="投影只引用保留侧，另一侧的完整唯一键证明每行至多匹配一次；外连接仍保留未匹配行，基线保持原连接";
        if(selected==HepRule::FilterAggregateTranspose)reason="HAVING 只检查有明确绑定的分组列，先过滤输入可减少聚合行。全局聚合与分组集合保留原计划";
        if(selected==HepRule::FilterCorrelate)reason="将已验证且只依赖左行的条件移到 LATERAL 左输入，减少右侧调用；保留右列条件与外连接补 NULL";
        if(selected==HepRule::FilterUnpivotTranspose)reason="先按原样保留的输入列筛选，再展开整数度量；生成的标签和度量继续在展开后筛选";
        if(selected==HepRule::FilterProjectTranspose)reason="按位置将派生表过滤中的别名绑定回基表列，在直接列投影前过滤";
        if(selected==HepRule::FilterSortTranspose)reason="将可安全求值的派生表过滤移动到排序和直接列投影之前。保留原排序方向、NULL 排序和输出别名，分页仍是边界";
        if(selected==HepRule::FilterWindowTranspose)reason="按所有窗口共有的整数分区键整组过滤，保留参与排序和计算名次的每个分区。窗口结果条件仍在计算后检查";
        if(selected==HepRule::FilterSetOpTranspose)reason="集合两侧各用原输出别名和同类型整数列应用相同过滤，保留重复和 NULL 的集合语义";
        if(selected==HepRule::SetOpToFilter)reason="两个分支对同一张普通表选择相同列。合并扫描并使用 OR、AND 或 IS NOT TRUE，保留集合输出、重复行和 NULL 的语义";
        if(selected==HepRule::SortUnionTranspose)reason="UNION ALL 两侧最多各保留 LIMIT + OFFSET 行。排序涵盖全部输出列，保留全局排序与分页";
        if(selected==HepRule::AggregateReduceFunctions)reason="PostgreSQL 的 bigint 平均值分解为精确 SUM 和非空 COUNT，以原生 numeric 除法保留精度、NULL 与重复行语义；复用已存在的同组状态";
        if(selected==HepRule::UnionPullUpConstants)reason="UNION ALL 两侧同值同类型的常量从分支投影提到集合上方，保留原列位置、输出类型和重复行";
        if(selected==HepRule::ProjectSetOpTranspose)reason="UNION ALL 两侧先按同一位置列表选择精确整数列，减少中间行宽，同时保持重复行、NULL 和原输出名";
        if(selected==HepRule::ProjectJoinTranspose)reason="连接输入只携带投影、连接键及已暴露排序所需的列，减少连接中的行宽，保留原输出名与顺序";
        if(selected==HepRule::ProjectFilterTranspose)reason="扫描先取投影和过滤共同需要的列，保留原过滤、结果列名和列顺序";
        if(selected==HepRule::ProjectWindowTranspose)reason="窗口输入只携带输出、分区、排序和帧表达式所需的列，保留原窗口计算与结果列";
        if(selected==HepRule::ProjectCorrelateTranspose)reason="LATERAL 两侧保留输出所需字段，左侧还保留所有被嵌套右查询绑定的列";
        if(selected==HepRule::SortJoinTranspose)reason="外连接按保留侧的非空唯一键确定顺序，匹配侧的唯一键保证每行只产生一行。在保留侧先读 LIMIT + OFFSET 行，保留全局排序和分页";
        const bool substitution=&rules==&c.options.heuristic.substitutions;
        c.result.rules.push_back({hepRuleName(selected),std::move(reason),0,id,after,true,substitution});
        id=after;
      }
    }
    return id;
  }
  RelId visit(RelId id,std::size_t depth,const std::vector<HepRule> &rules) {
    c.stop();
    if(!id || id>=plan.nodes.size())c.fail({},"invalid Hep node reference");
    if(auto it=memo.find(id);it!=memo.end())return it->second;
    if(depth>=c.options.maxDepth)throw HepLimit{};
    if(!active.insert(id).second)c.fail({},"cycle in Hep input DAG");
    struct Leave {std::set<RelId> &active;RelId id;~Leave(){active.erase(id);}} leave{active,id};
    auto current=id;
    if(c.options.heuristic.order==HepMatchOrder::TopDown)current=apply(current,rules);
    auto n=plan.nodes.at(current);bool changed=false;
    for(auto &input:n.inputs){auto next=visit(input,depth+1,rules);changed|=input!=next;input=next;}
    if(changed)current=append(std::move(n));
    if(c.options.heuristic.order==HepMatchOrder::BottomUp)current=apply(current,rules);
    c.charge(96);memo[id]=current;return current;
  }
public:
  explicit HepPlanner(PlannerContext &context):c(context),plan(context.result.logical),
      metadata(plan,{context.options.maxNodes,context.options.maxDepth,
        std::min<std::size_t>(4*1024*1024,context.options.maxBytes),[&context]{context.stop();return false;}}){}
  RelId run() {
    auto root=plan.root;
    if(!c.options.heuristic.enabled || !c.options.maxRuleMatches)return root;
    try {
      for (const auto *phase:{&c.options.heuristic.substitutions,&c.options.heuristic.rules}) {
        if (phase->empty()) continue;
        bool stable=false;
        for(std::size_t pass=0;pass<c.options.heuristic.maxPasses;++pass) {
          memo.clear();
          active.clear();
          const auto next=visit(root,0,*phase);
          ++c.result.heuristicPasses;
          if (next==root) {
            stable=true;
            break;
          }
          root=next;
        }
        if (!stable) throw HepLimit{};
      }
    } catch(const HepLimit &) {
      c.result.heuristicComplete=false;c.result.searchComplete=false;
      c.result.advice.push_back({AdviceKind::SearchLimit,{},"启发式规则达到遍数 / 匹配 / 元数据限额","保留完整基线及已完成的遍历结果；后续成本搜索仍有独立规则限额。"});
    }
    return root;
  }
};
}
RelId runHep(PlannerContext &context){return HepPlanner(context).run();}
const char* hepRuleName(HepRule rule) {
  switch(rule) {
  case HepRule::FilterMerge:return "HepFilterMerge";
  case HepRule::ProjectRemove:return "HepProjectRemove";
  case HepRule::ProjectMerge:return "HepProjectMerge";
  case HepRule::SortRemove:return "HepSortRemove";
  case HepRule::LimitMerge:return "HepLimitMerge";
  case HepRule::DistinctRemove:return "HepDistinctRemove";
  case HepRule::SortLimitRemove:return "HepSortLimitRemove";
  case HepRule::SortDuplicateKeys:return "HepSortDuplicateKeys";
  case HepRule::SortConstantKeys:return "HepSortConstantKeys";
  case HepRule::AggregateDuplicateKeys:return "HepAggregateDuplicateKeys";
  case HepRule::AggregateRemove:return "HepAggregateRemove";
  case HepRule::AggregateJoinRemove:return "HepAggregateJoinRemove";
  case HepRule::AggregateCaseToFilter:return "HepAggregateCaseToFilter";
  case HepRule::AggregateReduceFunctionsOnGroupKeys:return "HepAggregateReduceFunctionsOnGroupKeys";
  case HepRule::AggregateFilterToFilteredAggregate:return "HepAggregateFilterToFilteredAggregate";
  case HepRule::AggregateExtractProject:return "HepAggregateExtractProject";
  case HepRule::AggregateProjectMerge:return "HepAggregateProjectMerge";
  case HepRule::AggregateExpandDistinct:return "HepAggregateExpandDistinct";
  case HepRule::ProjectAggregateMerge:return "HepProjectAggregateMerge";
  case HepRule::AggregateJoinTranspose:return "HepAggregateJoinTranspose";
  case HepRule::ProjectJoinJoinRemove:return "HepProjectJoinJoinRemove";
  case HepRule::AggregateJoinJoinRemove:return "HepAggregateJoinJoinRemove";
  case HepRule::ProjectOverSumToSum0:return "HepProjectOverSumToSum0";
  case HepRule::AggregateMerge:return "HepAggregateMerge";
  case HepRule::AggregateUnionTranspose:return "HepAggregateUnionTranspose";
  case HepRule::AggregateGroupingSetsToUnion:return "HepAggregateGroupingSetsToUnion";
  case HepRule::AggregateProjectPullUpConstants:return "HepAggregateProjectPullUpConstants";
  case HepRule::ValuesReduce:return "HepValuesReduce";
  case HepRule::JoinDeriveNotNull:return "HepJoinDeriveNotNull";
  case HepRule::FilterNullability:return "HepFilterNullability";
  case HepRule::PruneEmpty:return "HepPruneEmpty";
  case HepRule::SimplifyOuterJoin:return "HepSimplifyOuterJoin";
  case HepRule::DecorrelateExists:return "HepDecorrelateExists";
  case HepRule::RewriteInSubquery:return "HepRewriteInSubquery";
  case HepRule::MarkToSemiOrAntiJoin:return "HepMarkToSemiOrAntiJoin";
  case HepRule::RemoveUnusedOuterJoin:return "HepRemoveUnusedOuterJoin";
  case HepRule::FilterAggregateTranspose:return "HepFilterAggregateTranspose";
  case HepRule::FilterCorrelate:return "HepFilterCorrelate";
  case HepRule::FilterUnpivotTranspose:return "HepFilterUnpivotTranspose";
  case HepRule::FilterSetOpTranspose:return "HepFilterSetOpTranspose";
  case HepRule::SetOpToFilter:return "HepSetOpToFilter";
  case HepRule::CalcMerge:return "HepCalcMerge";
  case HepRule::CalcSplit:return "HepCalcSplit";
  case HepRule::CalcRemove:return "HepCalcRemove";
  case HepRule::FilterProjectTranspose:return "HepFilterProjectTranspose";
  case HepRule::FilterSortTranspose:return "HepFilterSortTranspose";
  case HepRule::FilterWindowTranspose:return "HepFilterWindowTranspose";
  case HepRule::SortUnionTranspose:return "HepSortUnionTranspose";
  case HepRule::SortJoinTranspose:return "HepSortJoinTranspose";
  case HepRule::ProjectJoinTranspose:return "HepProjectJoinTranspose";
  case HepRule::ProjectFilterTranspose:return "HepProjectFilterTranspose";
  case HepRule::ProjectWindowTranspose:return "HepProjectWindowTranspose";
  case HepRule::ProjectCorrelateTranspose:return "HepProjectCorrelateTranspose";
  case HepRule::ProjectSetOpTranspose:return "HepProjectSetOpTranspose";
  case HepRule::UnionPullUpConstants:return "HepUnionPullUpConstants";
  case HepRule::AggregateReduceFunctions:return "HepAggregateReduceFunctions";
  case HepRule::Count:break;
  }
  return "UnknownHepRule";
}
} // namespace navidog::sql::optimizer
