#include "optimizer_internal.hpp"
#include <algorithm>
#include <cmath>
namespace navidog::sql::optimizer {
namespace {
using namespace compiler;
class Builder {
  PlannerContext &context;
  LogicalPlan &plan;
  std::map<QueryId, RelId> queries;
  std::map<QueryId, RelId> workingSeeds;
  std::map<QueryId, bool> outerDependencies;
  std::set<QueryId> active;
  std::size_t depth = 0;
  RelId append(RelNode node) {
    context.stop();
    if (plan.nodes.size() >= context.options.maxNodes)
      context.fail(node.origin, "logical plan node limit exceeded");
    context.chargeNode(node);
    plan.nodes.push_back(std::move(node));
    return RelId(plan.nodes.size() - 1);
  }
  const ModelSelect &query(QueryId id) {
    if (!id || id > plan.program.queries.size())
      context.fail({}, "invalid query ID in optimizer input");
    return plan.program.queries[id - 1];
  }
  const ModelExpression &expression(const ModelSelect &q, ExprId id) {
    if (!id || id >= q.expressions.size())
      context.fail(q.origin, "invalid expression ID in optimizer input");
    return q.expressions[id];
  }
  std::vector<ExpressionRef> predicates(const ModelSelect &q, ExprId root) {
    std::vector<ExpressionRef> out;
    std::vector<ExprId> work;
    if (root)
      work.push_back(root);
    std::size_t visits = 0;
    while (!work.empty()) {
      context.stop();
      if (++visits > context.options.maxNodes)
        context.fail(q.origin, "predicate expansion limit exceeded");
      auto id = work.back();
      work.pop_back();
      const auto &e = expression(q, id);
      if (e.kind == ExprKind::Binary && e.value == "AND" &&
          e.operands.size() == 2) {
        work.push_back(e.operands[1]);
        work.push_back(e.operands[0]);
      } else
        out.push_back({q.id, id});
    }
    return out;
  }
  std::vector<WindowGroup> projectWindows(const ModelSelect &q) {
    std::vector<WindowGroup> groups;
    std::map<std::string,std::size_t> orderings;
    std::set<ExprId> visited;
    std::vector<ExprId> work;
    for (const auto &field:q.projections) work.push_back(field.expression);
    for (const auto &item:q.orderBy) work.push_back(item.expression);
    while (!work.empty()) {
      context.stop();
      const auto id=work.back();
      work.pop_back();
      if (!visited.insert(id).second) continue;
      const auto &function=expression(q,id);
      if (function.kind!=ExprKind::Window || !function.window) {
        work.insert(work.end(),function.operands.begin(),function.operands.end());
        continue;
      }
      const auto &window=*function.window;
      const bool ordered=window.partitions || !window.ordering.empty();
      std::string signature=std::to_string(window.partitions)+";";
      bool share=window.partitions+window.ordering.size()<function.operands.size();
      for (std::size_t i=0; share && i<window.partitions+window.ordering.size(); ++i) {
        context.stop();
        const auto &key=expression(q,function.operands[i+1]);
        if (key.kind!=ExprKind::Name || !key.binding || key.binding->scope!=q.id || key.localSymbol ||
            !key.coalescedBindings.empty() || key.outputColumn) {
          share=false;
          break;
        }
        const auto &column=*key.binding;
        signature+=std::to_string(column.table)+"."+std::to_string(column.column)+"."+
            std::to_string(key.sqlType);
        if (i>=window.partitions) {
          const auto &sort=window.ordering[i-window.partitions];
          signature+=sort.descending?"d":"a";
          signature+=sort.nulls;
        }
        signature+=';';
      }
      if (!share) signature="expression:"+std::to_string(id);
      const auto [entry,newGroup]=orderings.emplace(signature,groups.size());
      if (newGroup) {
        if (groups.size()>=256) throw PlannerBudget{};
        context.charge(sizeof(WindowGroup)+signature.size()+64);
        groups.push_back({ordered,{}});
      }
      context.charge(sizeof(ExpressionRef));
      groups[entry->second].functions.push_back({q.id,id});
    }
    return groups;
  }
  std::optional<double> integer(const ModelSelect &q, ExprId id) {
    if (!id)
      return {};
    const auto &e = expression(q, id);
    if (e.kind != ExprKind::Number || e.value.empty() ||
        e.value.find_first_not_of("0123456789") != std::string::npos)
      return {};
    try {
      auto n = std::stod(e.value);
      if (std::isfinite(n) && n <= 9007199254740991.0)
        return n;
    } catch (...) {
    }
    return {};
  }
  ColumnRef column(const ModelSelect &q, const ModelExpression &e) {
    if (auto resolved =
            resolveColumn(plan, {q.id, ExprId(&e - q.expressions.data())}))
      return *resolved;
    if (e.binding) {
      const auto &owner = query(e.binding->scope);
      if (e.binding->table < owner.tables.size()) {
        const auto &t = owner.tables[e.binding->table];
        if (t.definition && e.binding->column < t.definition->columns.size())
          return {owner.id, e.binding->table,
                  t.definition->columns[e.binding->column].name};
      }
    }
    if (e.kind == ExprKind::Name && !e.name.empty()) {
      if (e.name.size() > 1)
        for (std::size_t i = 0; i < q.tables.size(); ++i) {
          const auto &t = q.tables[i];
          if ((t.alias.empty() && !t.name.empty() &&
               t.name.back() == e.name[e.name.size() - 2]) ||
              t.alias == e.name[e.name.size() - 2])
            return {q.id, i, e.name.back()};
        }
      return {q.id, std::size_t(-1), e.name.back()};
    }
    return {q.id, std::size_t(-1), "expression"};
  }
  void subqueries(RelId target, const ModelSelect &q,
                  const std::vector<ExpressionRef> &expressions) {
    std::vector<ExprId> work;
    for (auto e : expressions)
      work.push_back(e.expression);
    std::set<ExprId> seen;
    std::set<QueryId> added;
    while (!work.empty()) {
      context.stop();
      auto id = work.back();
      work.pop_back();
      if (!seen.insert(id).second)
        continue;
      if (seen.size() > context.options.maxNodes)
        context.fail(q.origin, "subquery expression limit exceeded");
      const auto &e = expression(q, id);
      if (e.query && added.insert(e.query).second) {
        RelNode n;
        n.kind = RelKind::Subquery;
        n.query = q.id;
        n.origin = e.origin;
        n.scalar = true;
        n.correlated = correlated(e.query);
        n.inputs = {run(e.query)};
        auto child = append(std::move(n));
        plan.nodes[target].inputs.push_back(child);
      }
      work.insert(work.end(), e.operands.begin(), e.operands.end());
    }
  }
  bool hasOuterDependencies(QueryId id) {
    const auto &root=query(id);
    if (!root.scope.parent) return false;
    std::set<QueryId> outer;
    for (auto parent=root.scope.parent; parent; parent=query(*parent).scope.parent) {
      context.stop();
      if (outer.size()>=context.options.maxDepth || !outer.insert(*parent).second)
        context.fail(root.origin,"cyclic or excessive parent query scopes");
      context.charge(sizeof(QueryId)+4*sizeof(void *));
    }
    std::set<QueryId> visited;
    std::vector<QueryId> pending;
    const auto add=[&](QueryId child) {
      if (!child || !visited.insert(child).second) return;
      context.charge(2*sizeof(QueryId)+4*sizeof(void *));
      pending.push_back(child);
    };
    add(id);
    while (!pending.empty()) {
      const auto &current=query(pending.back());
      pending.pop_back();
      for (const auto &e:current.expressions) {
        context.stop();
        if ((e.binding && outer.count(e.binding->scope)) ||
            std::any_of(e.coalescedBindings.begin(),e.coalescedBindings.end(),
                        [&](const auto &column){ return outer.count(column.scope); }))
          return true;
        // Without a complete catalog an unbound name may use an outer
        // query. Do not cache its result as independent.
        if (current.scope.parent && e.kind==ExprKind::Name && !e.binding &&
            !e.localSymbol && !resolveColumn(plan,{current.id,ExprId(&e-current.expressions.data())}))
          return true;
        add(e.query);
      }
      for (const auto &table:current.tables)
        if (!table.workingTableOf) add(table.query);
      if (current.setOperation) {
        add(current.setOperation->left);
        add(current.setOperation->right);
      }
      add(current.source);
    }
    return false;
  }
  bool correlated(QueryId id) {
    const auto known=outerDependencies.find(id);
    if (known!=outerDependencies.end()) return known->second;
    const auto result=hasOuterDependencies(id);
    context.charge(sizeof(QueryId)+sizeof(bool)+4*sizeof(void *));
    outerDependencies.emplace(id,result);
    return result;
  }
  RelId unary(RelKind kind, RelId input, const ModelSelect &q) {
    RelNode n;
    n.kind = kind;
    n.inputs = {input};
    n.query = q.id;
    n.origin = q.origin;
    n.outputs = plan.nodes[input].outputs;
    return append(std::move(n));
  }
  std::optional<RelId> projectRowFunction(const ModelSelect &q,RelId input) {
    if (plan.program.sourceDialect!=Dialect::PostgreSQL || q.from || !q.tables.empty() ||
        q.where || q.having || q.distinct || !q.groupBy.empty() || !q.groupingSets.empty() ||
        q.limit || q.offset || !q.orderBy.empty() || !q.ctes.empty() || q.selectInto ||
        !q.namedWindows.empty() || q.projections.size()!=1)
      return {};
    const auto &value=expression(q,q.projections.front().expression);
    if (value.kind!=ExprKind::Call || value.name.empty() || value.distinct || value.window ||
        (value.routineResolved && (value.routineSchema!="pg_catalog" ||
         value.routineKind!=RoutineKind::TableFunction)))
      return {};
    const bool series=value.name.back()=="generate_series";
    const bool qualified=value.name.size()==2 && value.name.front()=="pg_catalog";
    if (!qualified && !(series && value.name.size()==1 && value.routineResolved)) return {};
    const auto bits=signedIntegerBits(plan.program.types.get(value.sqlType));
    if (!bits) return {};
    if (series) {
      if (*bits<32 || value.operands.size()<2 || value.operands.size()>3) return {};
    } else {
      if (value.name.back()!="unnest" || value.operands.size()!=1) return {};
      const auto &operand=expression(q,value.operands.front());
      const auto &array=plan.program.types.get(operand.sqlType);
      if (array.arrayDimensions<1 ||
          (operand.kind!=ExprKind::Name && operand.kind!=ExprKind::Cast &&
           operand.kind!=ExprKind::ArrayConstructor))
        return {};
      if (operand.kind==ExprKind::Cast && (operand.operands.size()!=1 ||
          (expression(q,operand.operands.front()).kind!=ExprKind::Null &&
           expression(q,operand.operands.front()).kind!=ExprKind::String &&
           expression(q,operand.operands.front()).kind!=ExprKind::ArrayConstructor)))
        return {};
    }
    RelNode expand;
    expand.kind=series?RelKind::Series:RelKind::Unnest;
    expand.query=q.id;
    expand.origin=value.origin;
    expand.inputs={input};
    for (auto argument:value.operands) expand.keys.push_back({q.id,argument});
    expand.outputs={{q.id,std::size_t(-1),
                     q.projections.front().alias.empty()?value.name.back():q.projections.front().alias}};
    const auto result=append(std::move(expand));
    subqueries(result,q,plan.nodes[result].keys);
    return result;
  }
  RelId relation(const ModelSelect &q, RelationId id, bool preserveTableFunctionOrder) {
    if (!id || id > q.relations.size())
      context.fail(q.origin, "invalid relation ID in optimizer input");
    if (++depth > context.options.maxDepth)
      context.fail(q.origin, "relation nesting limit exceeded");
    struct Leave {
      std::size_t &depth;
      ~Leave() { --depth; }
    } leave{depth};
    const auto r = q.relations[id - 1];
    if (!r.left && !r.right) {
      if (r.table >= q.tables.size())
        context.fail(q.origin, "invalid table ID in optimizer input");
      const auto &t = q.tables[r.table];
      RelNode n;
      n.kind = RelKind::Scan;
      n.query = q.id;
      n.table = r.table;
      n.origin = q.origin;
      if (t.pseudo)
        context.fail(q.origin,
                     "pseudo-table cannot be scanned by the local planner");
      if (t.rowMatch) {
        const auto &binding=t.matchInput;
        if (binding)
          for (const auto &column:binding->columns)
            n.outputs.push_back({q.id,r.table,column.name});
        if (n.outputs.empty())
          n.outputs.push_back({q.id,r.table,"*"});
        if (q.catalogChecked && binding && binding->complete && binding->kind==ObjectKind::Table)
          n.scanSource=ScanSource::BaseTable;
        RelNode match;
        match.kind=RelKind::Match;
        match.query=q.id;
        match.table=r.table;
        match.origin=t.rowMatch->origin;
        match.inputs={append(std::move(n))};
        for (const auto &column:t.definition->columns)
          match.outputs.push_back({q.id,r.table,column.name});
        return append(std::move(match));
      }
      const auto &definition=sourceDefinition(t);
      if ((t.pivot || t.unpivot) && (!definition || !definition->complete || !t.definition || !t.definition->complete))
        context.fail(t.pivot?t.pivot->origin:t.unpivot->origin,
                     "local PIVOT or UNPIVOT planning requires resolved input and output columns");
      if (t.jsonTable) {
        n.kind = RelKind::TableFunction;
        n.keys.push_back({q.id,t.jsonTable->document});
      } else if (t.workingTableOf) {
        const auto seed = workingSeeds.find(t.workingTableOf);
        if (seed == workingSeeds.end() || plan.nodes.at(seed->second).query != t.query)
          context.fail(q.origin, "working table reference is outside its recursive member");
        n.kind = RelKind::WorkingTableScan;
        n.workingTableOf = t.workingTableOf;
        n.workingSeed = seed->second;
      } else if (t.query) {
        n.kind = RelKind::Subquery;
        auto input=run(t.query);
        if (t.ordinalityColumn) {
          auto expansion=plan.nodes.at(input);
          if ((expansion.kind!=RelKind::Unnest && expansion.kind!=RelKind::Series) || expansion.outputs.size()!=1)
            context.fail(q.origin,"ordinality requires a supported native PostgreSQL row function");
          if (expansion.outputs.front().name==*t.ordinalityColumn)
            context.fail(q.origin,"local row function requires distinct output names");
          expansion.withOrdinality=true;
          expansion.outputs.push_back({expansion.query,std::size_t(-1),*t.ordinalityColumn});
          input=append(std::move(expansion));
        }
        n.inputs.push_back(input);
        n.correlated = correlated(t.query);
        n.materialized = t.cteMaterialization == CteMaterialization::Materialized;
      } else if (q.catalogChecked && definition && definition->complete &&
                 definition->arityKnown && !definition->schema.empty()) {
        if (definition->kind==ObjectKind::Table)
          n.scanSource=ScanSource::BaseTable;
        else if (definition->kind==ObjectKind::MaterializedView)
          n.scanSource=ScanSource::MaterializedView;
      }
      if (definition)
        for (const auto &col : definition->columns)
          n.outputs.push_back({q.id, r.table, col.name});
      if (n.outputs.empty())
        n.outputs.push_back({q.id, r.table, "*"});
      const auto result = append(std::move(n));
      if (t.pivot || t.unpivot) {
        RelNode transformed;
        transformed.kind=t.pivot?RelKind::Pivot:RelKind::Unpivot;
        transformed.query=q.id;
        transformed.table=r.table;
        transformed.origin=t.pivot?t.pivot->origin:t.unpivot->origin;
        transformed.inputs={result};
        for (const auto &column:t.definition->columns)
          transformed.outputs.push_back({q.id,r.table,column.name});
        return append(std::move(transformed));
      }
      if (t.jsonTable)
        subqueries(result, q, plan.nodes[result].keys);
      if (t.sampling) {
        if (plan.nodes[result].kind!=RelKind::Scan)
          context.fail(q.origin,"TABLESAMPLE requires a scan");
        RelNode sample;
        sample.kind=RelKind::Sample;
        sample.query=q.id;
        sample.inputs={result};
        sample.outputs=plan.nodes[result].outputs;
        sample.origin=q.origin;
        sample.sampling=t.sampling;
        return append(std::move(sample));
      }
      return result;
    }
    if (!r.left || !r.right)
      context.fail(q.origin, "incomplete join tree");
    RelNode n;
    n.kind = r.lateral ? RelKind::Correlate : RelKind::Join;
    n.correlated = r.lateral;
    n.query = q.id;
    n.join = r.join;
    n.fixedOrder = q.fixedJoinOrder || r.fixedOrder || preserveTableFunctionOrder;
    n.origin = q.origin;
    n.inputs = {relation(q, r.left, preserveTableFunctionOrder), relation(q, r.right, preserveTableFunctionOrder)};
    n.predicates = predicates(q, r.condition);
    n.outputs = plan.nodes[n.inputs[0]].outputs;
    const auto &right = plan.nodes[n.inputs[1]].outputs;
    n.outputs.insert(n.outputs.end(), right.begin(), right.end());
    if (r.natural || !r.usingColumns.empty()) {
      context.result.advice.push_back(
          {AdviceKind::MissingStatistics, q.origin,
           "JOIN USING / NATURAL 的合并列边界",
           "本地成本保留该连接为变换屏障；不会猜测输出列合并或等值键。"});
      n.distinct = true;
    }
    return append(std::move(n));
  }

public:
  Builder(PlannerContext &c) : context(c), plan(c.result.logical) {
    plan.nodes.emplace_back();
  }
  RelId run(QueryId id) {
    context.stop();
    if (queries.count(id))
      return queries[id];
    if (!active.insert(id).second)
      context.fail(query(id).origin,
                   "cyclic query dependency in optimizer input");
    if (++depth > context.options.maxDepth)
      context.fail(query(id).origin, "query nesting limit exceeded");
    struct Leave {
      std::size_t &depth;
      std::set<QueryId> &active;
      QueryId id;
      ~Leave() {
        --depth;
        active.erase(id);
      }
    } leave{depth, active, id};
    const auto &q = query(id);
    if (q.kind != StatementKind::Select && q.kind != StatementKind::Insert &&
        q.kind != StatementKind::Update && q.kind != StatementKind::Delete && q.kind!=StatementKind::Merge)
      context.fail(q.origin, "local relational planning requires SELECT or a supported row modification");
    RelId root = 0;
    if (q.setOperation) {
      RelNode n;
      n.kind = q.setOperation->recursive ? RelKind::RepeatUnion : RelKind::SetOperation;
      n.query = q.id;
      n.origin = q.setOperation->origin;
      n.setKind = q.setOperation->kind;
      n.distinct = !q.setOperation->all;
      const auto seed = run(q.setOperation->left);
      if (q.setOperation->recursive) {
        const auto &member = query(q.setOperation->right);
        if (n.setKind != SetKind::Union || std::count_if(member.tables.begin(), member.tables.end(),
                [&](const auto &table) { return table.workingTableOf == q.id; }) != 1)
          context.fail(q.origin, "repeat union requires one direct working table reference");
        n.workingTableOf = q.id;
        workingSeeds.emplace(q.id, seed);
      }
      const auto step = run(q.setOperation->right);
      if (q.setOperation->recursive) workingSeeds.erase(q.id);
      n.inputs = {seed, step};
      for (const auto &p : q.projections)
        n.outputs.push_back(column(q, expression(q, p.expression)));
      root = append(std::move(n));
    } else if (q.valuesQuery) {
      RelNode n;
      n.kind = RelKind::Values;
      n.query = q.id;
      n.origin = q.origin;
      n.valueCount = q.rows.size();
      if (!n.valueCount || q.tables.size() != 1 || !q.tables.front().definition)
        context.fail(q.origin, "VALUES requires rows and a bound output schema");
      if (std::any_of(q.expressions.begin(), q.expressions.end(),
                      [](const auto &expression) { return expression.query != 0; }))
        context.fail(q.origin, "local VALUES planning does not yet model subquery cell dependencies");
      for (const auto &col : q.tables.front().definition->columns)
        n.outputs.push_back({q.id, 0, col.name});
      for (const auto &row : q.rows)
        for (auto cell : row) n.keys.push_back({q.id, cell});
      root = append(std::move(n));
    } else if (q.kind == StatementKind::Insert) {
      if (q.source)
        root = run(q.source);
      else {
        RelNode n;
        n.kind = RelKind::Values;
        n.query = q.id;
        n.origin = q.origin;
        n.valueCount =
            q.insertSource == InsertSource::DefaultValues ? 1 : q.rows.size();
        root = append(std::move(n));
      }
    } else if (q.from)
      root = relation(q, q.from, std::any_of(q.tables.begin(), q.tables.end(),
                                            [](const auto &table) { return table.jsonTable != nullptr; }));
    else if (!q.tables.empty()) {
      ModelRelation leaf;
      leaf.table = 0;
      RelNode n;
      n.kind = RelKind::Scan;
      n.query = q.id;
      n.table = 0;
      n.origin = q.origin;
      if (q.tables[0].definition)
        for (const auto &col : q.tables[0].definition->columns)
          n.outputs.push_back({q.id, 0, col.name});
      root = append(std::move(n));
    } else {
      RelNode n;
      n.kind = RelKind::Values;
      n.query = q.id;
      n.origin = q.origin;
      n.valueCount = 1;
      root = append(std::move(n));
    }
    if (q.kind==StatementKind::Merge) {
      auto &candidates=plan.nodes.at(root);
      if (candidates.kind!=RelKind::Join || candidates.join!=JoinKind::Cross || candidates.inputs.size()!=2 ||
          plan.nodes.at(candidates.inputs.front()).table!=q.targetTable)
        context.fail(q.origin,"MERGE requires target and source row inputs");
      bool withoutSource=false, withoutTarget=false;
      for (const auto &clause:q.mergeClauses) {
        withoutSource |= clause.match==MergeMatch::NotMatchedSource;
        withoutTarget |= clause.match==MergeMatch::NotMatchedTarget;
      }
      candidates.join=withoutTarget && withoutSource?JoinKind::Full:
          withoutTarget?JoinKind::Right:withoutSource?JoinKind::Left:JoinKind::Inner;
      candidates.fixedOrder=true;
      candidates.predicates=predicates(q,q.mergeOn);
      const auto joins=candidates.predicates;
      subqueries(root,q,joins);
    }
    if (q.where) {
      root = unary(RelKind::Filter, root, q);
      plan.nodes[root].predicates = predicates(q, q.where);
      subqueries(root, q, plan.nodes[root].predicates);
    }
    const auto rowFunction=q.kind==StatementKind::Select?projectRowFunction(q,root):std::nullopt;
    if (rowFunction) {
      root=*rowFunction;
    } else if (q.kind == StatementKind::Select && !q.valuesQuery) {
      const auto features = inspectQueryFeatures(
          q, plan.program.sourceDialect, context.options.maxNodes * 4, [&] {
            context.stop();
            return false;
          });
      if (!features)
        context.fail(
            q.origin,
            "query-block feature inspection limit or invalid reference");
      if(features->tableFunction)
        context.fail(q.origin,"set-returning routine requires a table-function relational operator; scalar projection costs would be invalid");
      if(features->uncertainRoutine && q.groupBy.empty() && !features->aggregate)
        context.fail(q.origin,"routine category is unknown; provide verified scalar/aggregate/window metadata before estimating this query block");
      const auto dialect = plan.program.sourceDialect;
      const bool implicitGrouping = q.having && !isMySqlDialect(dialect) &&
                                    dialect != Dialect::SQLite;
      if (!q.groupBy.empty() || !q.groupingSets.empty() || features->aggregate || implicitGrouping) {
        RelNode aggregate;
        aggregate.kind = RelKind::Aggregate; aggregate.inputs = {root};
        aggregate.query = q.id; aggregate.origin = q.origin;
        aggregate.outputs = plan.nodes[root].outputs;
        for (auto e : q.groupBy) aggregate.keys.push_back({q.id, e});
        for (const auto &set : q.groupingSets) {
          std::vector<ExpressionRef> refs;
          for (auto e : set) refs.push_back({q.id, e});
          aggregate.groupingSets.push_back(std::move(refs));
        }
        root = append(std::move(aggregate));
        if (q.having) {
          root = unary(RelKind::Filter, root, q);
          plan.nodes[root].predicates = predicates(q, q.having);
          subqueries(root, q, plan.nodes[root].predicates);
        }
      } else if (q.having) {
        root = unary(RelKind::Filter, root, q);
        plan.nodes[root].predicates = predicates(q, q.having);
        subqueries(root, q, plan.nodes[root].predicates);
      }
      if (features->window) {
        auto window=RelNode{};
        window.kind=RelKind::Window;
        window.query=q.id;
        window.origin=q.origin;
        window.inputs={root};
        window.outputs=plan.nodes[root].outputs;
        window.windowGroups=projectWindows(q);
        root=append(std::move(window));
      }
      auto projected = unary(RelKind::Project, root, q);
      auto &project = plan.nodes[projected];
      project.outputs.clear();
      for (const auto &p : q.projections) {
        const auto &e = expression(q, p.expression);
        project.keys.push_back({q.id, p.expression});
        if (e.kind == ExprKind::Star) {
          for (const auto &col : plan.nodes[root].outputs) {
            if (e.name.size() > 1) {
              if (col.table >= q.tables.size())
                continue;
              const auto &t = q.tables[col.table];
              if (t.alias != e.name[e.name.size() - 2] &&
                  (t.name.empty() ||
                   t.name.back() != e.name[e.name.size() - 2]))
                continue;
            }
            project.outputs.push_back(col);
          }
        } else {
          auto out = column(q, e);
          if (!p.alias.empty())
            out.name = p.alias;
          project.outputs.push_back(std::move(out));
        }
      }
      subqueries(projected, q, plan.nodes[projected].keys);
      root = projected;
      if (q.distinct) {
        root = unary(RelKind::Aggregate, root, q);
        plan.nodes[root].distinct = true;
        for (const auto &p : q.projections)
          plan.nodes[root].keys.push_back({q.id, p.expression});
      }
    } else if (q.kind != StatementKind::Select) {
      root = unary(RelKind::Modify, root, q);
      plan.nodes[root].table = q.targetTable;
      if (q.kind==StatementKind::Merge) {
        std::vector<ExpressionRef> actions;
        const auto add=[&](ExprId expression) {
          if (!expression) return;
          context.charge(sizeof(ExpressionRef));
          actions.push_back({q.id,expression});
        };
        for (const auto &branch:q.mergeClauses) {
          add(branch.condition);
          add(branch.deleteWhere);
          for (auto value:branch.values) add(value);
          for (const auto &assignment:branch.assignments) add(assignment.value);
        }
        subqueries(root,q,actions);
      }
    }
    if (q.kind == StatementKind::Select && !q.orderBy.empty()) {
      root = unary(RelKind::Sort, root, q);
      for (const auto &order : q.orderBy) {
        plan.nodes[root].keys.push_back({q.id, order.expression});
        plan.nodes[root].descending.push_back(order.descending);
        plan.nodes[root].nullOrdering.push_back(order.nulls);
      }
    }
    if (q.limit || q.offset) {
      root = unary(RelKind::Limit, root, q);
      plan.nodes[root].rowLimit = integer(q, q.limit);
      plan.nodes[root].rowOffset =
          q.offset ? integer(q, q.offset) : std::optional<double>{0};
      plan.nodes[root].withTies = q.limitWithTies && !q.orderBy.empty();
      if (q.limit)
        plan.nodes[root].keys.push_back({q.id, q.limit});
    }
    queries[id] = root;
    return root;
  }
};
} // namespace
void buildLogical(PlannerContext &context) {
  auto &p = context.result.logical;
  if (p.program.roots.size() != 1)
    context.fail({},
                 "select one complete SQL statement for local optimization");
  Builder builder(context);
  p.root = builder.run(p.program.roots.front());
}
} // namespace navidog::sql::optimizer
