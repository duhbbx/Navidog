#include "navidog/sql/optimizer/plan_sql.hpp"
#include "optimizer_internal.hpp"
#include "navidog/sql/compiler/operator_catalog.hpp"
#include <algorithm>
#include <cmath>
#include <map>

namespace navidog::sql::optimizer {
namespace {
using namespace compiler;

struct RenderFailure {
  Diagnostic diagnostic;
};

class PlanRenderer {
  using Demand = std::vector<bool>;
  struct Input {
    const RelNode *node;
    std::string alias;
    Demand columns;
  };
  const LogicalPlan &plan;
  const GenerationOptions &options;
  struct Cte {
    RelId root;
    CteMaterialization materialization;
    bool recursive;
  };
  struct ScalarDependency { RelId root; bool correlated; };
  std::map<QueryId,Cte> ctes;
  std::vector<QueryId> cteOrder;
  std::map<std::pair<QueryId,QueryId>,ScalarDependency> scalarInputs;
  std::optional<QueryId> recursiveScope;
  std::vector<Input *> outerInputs;
  std::size_t rowDepth=0;
  std::string sql;
  std::size_t visits = 0, nextAlias = 0, pendingBytes = 0;

  [[noreturn]] void fail(SourceRange origin, std::string message,
                         DiagnosticCode code = DiagnosticCode::UnsupportedSyntax) const {
    throw RenderFailure{{code, origin, std::move(message)}};
  }
  void visit(SourceRange origin) {
    if (++visits > options.maxVisits)
      fail(origin,"relational SQL visit limit exceeded",DiagnosticCode::ResourceLimit);
    if (options.shouldStop && options.shouldStop())
      fail(origin,"relational SQL generation cancelled",DiagnosticCode::Stopped);
  }
  void write(std::string_view text) {
    if (text.size() > options.maxBytes - sql.size() - pendingBytes)
      fail({},"relational SQL byte limit exceeded",DiagnosticCode::ResourceLimit);
    sql.append(text);
  }
  template <class Write>
  std::string collectSuffix(Write emit) {
    const auto begin=sql.size();
    emit();
    auto suffix=sql.substr(begin);
    sql.resize(begin);
    pendingBytes+=suffix.size();
    return suffix;
  }
  void appendSuffix(const std::string &suffix) {
    pendingBytes-=suffix.size();
    write(suffix);
  }
  void identifier(std::string_view text) {
    if (text.empty() || text.find('\0') != std::string_view::npos)
      fail({},"invalid relational SQL identifier",DiagnosticCode::InvalidQueryShape);
    write("\"");
    for (char ch : text) {
      if (ch=='"') write("\"");
      write(std::string_view(&ch,1));
    }
    write("\"");
  }
  static std::string slotName(std::size_t ordinal) {
    return "c" + std::to_string(ordinal + 1);
  }
  void column(Input &input,std::size_t i) {
    input.columns.at(i)=true;
    identifier(input.alias);
    write(".");
    identifier(slotName(i));
  }
  const RelNode &node(RelId id) {
    if (!id || id>=plan.nodes.size())
      fail({},"invalid relational SQL input ID",DiagnosticCode::InvalidQueryShape);
    return plan.nodes[id];
  }
  Input input(RelId id) {
    const auto &child=node(id);
    return {&child,"r"+std::to_string(++nextAlias),Demand(child.outputs.size())};
  }
  const RelNode &unary(const RelNode &n) {
    if (n.inputs.size()!=1)
      fail(n.origin,"relational SQL operator requires exactly one row input");
    return node(n.inputs.front());
  }
  const RelNode &primary(const RelNode &n) {
    if (n.kind!=RelKind::Filter && n.kind!=RelKind::Project) return unary(n);
    if (n.inputs.empty()) fail(n.origin,"relational SQL operator is missing its row input");
    for (std::size_t i=1;i<n.inputs.size();++i) {
      const auto &dependency=node(n.inputs[i]);
      if (dependency.kind!=RelKind::Subquery || !dependency.scalar || dependency.query!=n.query)
        fail(n.origin,"relational SQL operator has an unsupported dependent input");
    }
    return node(n.inputs.front());
  }
  static bool sameColumn(const ColumnRef &a,const ColumnRef &b) {
    return a.query==b.query && a.table==b.table && a.name==b.name;
  }
  void sameShape(const RelNode &n,const RelNode &child) {
    if (n.outputs.size()!=child.outputs.size() ||
        !std::equal(n.outputs.begin(),n.outputs.end(),child.outputs.begin(),sameColumn))
      fail(n.origin,"relational SQL input and output identities do not agree",DiagnosticCode::InvalidQueryShape);
  }
  void label(const RelNode &n,std::size_t i,bool visible) {
    const auto &output=n.outputs[i];
    if (visible && output.table==std::size_t(-1) && output.name=="expression") {
      const auto &owner=modelQuery(plan,output.query);
      if (i>=owner.projections.size() || owner.projections[i].alias!="expression")
        fail(n.origin,"computed relational outputs require explicit aliases");
    }
    write(" AS ");
    identifier(visible?output.name:slotName(i));
  }
  template <class Field>
  void fields(const RelNode &n,const Demand &needed,bool visible,Field emit) {
    bool any=false;
    for (std::size_t i=0;i<n.outputs.size();++i) {
      if (!needed[i]) continue;
      visit(n.origin);
      if (any) write(", ");
      emit(i);
      label(n,i,visible);
      any=true;
    }
    if (!any) write("1");
  }
  void inputColumns(const RelNode &n,Input &source,const Demand &needed,bool visible) {
    sameShape(n,*source.node);
    fields(n,needed,visible,[&](auto i) { column(source,i); });
  }
  void from(RelId id,const Input &source,std::size_t depth) {
    write("(");
    rows(id,source.columns,false,depth+1);
    write(") AS ");
    identifier(source.alias);
  }

  enum class ExpressionScope { Scalar, Sort, Grouped, Windowed, GroupedWindow };

  void expression(ExpressionRef ref,std::initializer_list<Input *> scope,
                  ExpressionScope mode=ExpressionScope::Scalar) {
    const bool positional=mode==ExpressionScope::Sort;
    const bool grouped=mode==ExpressionScope::Grouped || mode==ExpressionScope::GroupedWindow;
    const bool windowed=mode==ExpressionScope::Windowed || mode==ExpressionScope::GroupedWindow;
    const auto subqueryScope=[&](QueryId owner,QueryId child,const ScalarDependency &dependency) {
      return !dependency.correlated ||
          (!grouped && scope.size() && modelQuery(plan,child).scope.parent==owner);
    };
    // Limit the first backend to scalar SQL whose field references can be
    // replaced without preserving additional query or procedural state.
    std::vector<ExprId> pending{ref.expression};
    while (!pending.empty()) {
      const ExpressionRef part{ref.query,pending.back()};
      pending.pop_back();
      const auto &e=modelExpression(plan,part);
      visit(e.origin);
      if (e.localSymbol || (e.window && (!windowed || e.kind!=ExprKind::Window)) ||
          (e.distinct && e.kind!=ExprKind::Call))
        fail(e.origin,"relational SQL expression contains procedural or window state");
      if (!e.query && (e.kind==ExprKind::Subquery || e.kind==ExprKind::Exists ||
                       (e.kind==ExprKind::Quantified && e.operands.size()!=2)))
        fail(e.origin,"relational SQL expression is missing a nested query");
      if (e.query) {
        const auto dependency=scalarInputs.find({part.query,e.query});
        if (dependency==scalarInputs.end() || !subqueryScope(part.query,e.query,dependency->second))
          fail(e.origin,"relational SQL expression requires a subquery in its declared scope");
        if (e.kind!=ExprKind::Exists && node(dependency->second.root).outputs.size()!=1)
          fail(e.origin,"relational SQL expression subquery must return one column");
      }
      if (positional && e.outputColumn) continue;
      switch(e.kind) {
      case ExprKind::Name:case ExprKind::Number:case ExprKind::Boolean:case ExprKind::Null:
      case ExprKind::String:case ExprKind::Binary:case ExprKind::Unary:case ExprKind::Cast:
      case ExprKind::IsNull:case ExprKind::TruthTest:case ExprKind::Between:case ExprKind::In:
      case ExprKind::Exists:case ExprKind::Subquery:case ExprKind::Quantified:
      case ExprKind::Case:case ExprKind::ArrayConstructor:case ExprKind::Subscript:
        break;
      case ExprKind::Row:
        if (!grouped || e.value!="ROW" || e.operands.empty() || e.operands.size()>8 ||
            !std::all_of(e.operands.begin(),e.operands.end(),[&](auto child) {
              return signedIntegerBits(plan.program.types.get(modelExpression(plan,{part.query,child}).sqlType)).has_value();
            }))
          fail(e.origin,"relational SQL supports typed integer ROW keys in grouped expressions");
        break;
      case ExprKind::Window:
        if (!windowed || !e.window || e.operands.empty())
          fail(e.origin,"relational SQL window expression needs a window projection");
        break;
      case ExprKind::Call: {
        if (!grouped && !windowed)
          fail(e.origin,"relational SQL aggregate or window call requires its owning operator");
        if (grouped && !e.distinct && e.operands.size()==2 && e.name.size()==2 &&
            e.name[0]=="pg_catalog" && e.name[1]=="numeric_div") break;
        const auto routine=builtinOperator(Dialect::PostgreSQL,e.name,e.nameQuoted,e.operands.size());
        if (!routine || (e.routineResolved && (e.routineSchema!="pg_catalog" ||
                e.routineKind!=routine->kind || e.routineVolatility!=FunctionVolatility::Immutable)))
          fail(e.origin,"relational SQL needs a verified PostgreSQL aggregate call");
        const auto name=routine->name;
        if (windowed && routine->kind==RoutineKind::Window && !e.distinct) break;
        if (grouped && name=="GROUPING" && !e.distinct && e.name.size()==1) break;
        if (routine->kind!=RoutineKind::Aggregate)
          fail(e.origin,"relational SQL aggregate function is unsupported");
        if (e.operands.size()!=1 ||
            (name!="COUNT" && name!="SUM" && name!="MIN" && name!="MAX" && name!="AVG"))
          fail(e.origin,"relational SQL aggregate function is unsupported");
        const auto &argument=modelExpression(plan,{ref.query,e.operands.front()});
        if (name=="COUNT" && !e.distinct && argument.kind==ExprKind::Star && argument.name.empty())
          continue;
        if (name=="COUNT" && e.distinct && argument.kind==ExprKind::Row && argument.value=="ROW")
          break;
        if (!signedIntegerBits(plan.program.types.get(argument.sqlType)))
          fail(argument.origin,"relational SQL aggregate argument needs a verified signed integer type");
        break;
      }
      case ExprKind::AggregateFilter:
        if ((!grouped && !windowed) || e.operands.size()!=2)
          fail(e.origin,"relational SQL aggregate FILTER requires a grouped or window operator");
        break;
      default:
        fail(e.origin,"relational SQL backend does not yet support this scalar expression");
      }
      if (e.outputColumn || e.operands.size()>options.maxVisits-visits ||
          pending.size()>options.maxVisits-visits-e.operands.size())
        fail(e.origin,"invalid or excessive relational expression inputs",DiagnosticCode::ResourceLimit);
      pending.insert(pending.end(),e.operands.begin(),e.operands.end());
    }
    const auto resolve=[&](QueryId query,ExprId id)->std::optional<std::vector<std::string>> {
      const auto &e=modelExpression(plan,{query,id});
      if (e.outputColumn) {
        if (positional && scope.size()==1 && *e.outputColumn<(*scope.begin())->node->outputs.size()) {
          auto &source=**scope.begin();
          source.columns[*e.outputColumn]=true;
          return std::vector<std::string>{source.alias,slotName(*e.outputColumn)};
        }
        return {};
      }
      const auto &type=plan.program.types.get(e.sqlType);
      const auto binding=e.binding?(type.arrayDimensions?
          scopedBoundColumn(plan,{query,id},e.binding->scope):
          scopedScalarColumn(plan,{query,id},e.binding->scope)):std::nullopt;
      if (!binding) return {};
      std::optional<std::vector<std::string>> match;
      const auto resolveFrom=[&](Input *source) {
        for (std::size_t i=0;i<source->node->outputs.size();++i)
          if (sameColumn(*binding,source->node->outputs[i])) {
            if (match) return false;
            source->columns[i]=true;
            match=std::vector<std::string>{source->alias,slotName(i)};
          }
        return true;
      };
      if (binding->query==query) {
        for (auto *source:scope)
          if (!resolveFrom(source)) return {};
      } else {
        for (auto *source:outerInputs)
          if (!resolveFrom(source)) return {};
      }
      return match;
    };
    auto printerOptions=options;
    printerOptions.maxBytes-=sql.size()+pendingBytes;
    printerOptions.compactExpressions=true;
    const auto resolveQuery=[&](QueryId target)->std::optional<std::string> {
      const auto dependency=scalarInputs.find({ref.query,target});
      if (dependency==scalarInputs.end() || !subqueryScope(ref.query,target,dependency->second)) return {};
      const auto root=dependency->second.root;
      const auto begin=sql.size();
      const auto outer=outerInputs.size();
      if (dependency->second.correlated)
        outerInputs.insert(outerInputs.end(),scope.begin(),scope.end());
      rows(root,Demand(node(root).outputs.size(),true),false,rowDepth+1);
      outerInputs.resize(outer);
      auto text=sql.substr(begin);
      sql.resize(begin);
      return text;
    };
    auto rendered=generateExpression(plan.program,ref.query,ref.expression,Dialect::PostgreSQL,
                                     std::move(printerOptions),resolve,resolveQuery);
    if (!rendered.ok()) {
      auto diag=std::move(rendered.diagnostics.front());
      diag.range=modelExpression(plan,ref).origin;
      throw RenderFailure{std::move(diag)};
    }
    write(rendered.sql);
  }
  void predicates(const RelNode &n,std::initializer_list<Input *> scope,
                  ExpressionScope mode=ExpressionScope::Scalar) {
    for (std::size_t i=0;i<n.predicates.size();++i) {
      if (i) write(" AND ");
      write("(");
      expression(n.predicates[i],scope,mode);
      write(")");
    }
  }

  void scalarTypeSql(const SqlType &type,SourceRange origin) {
    if (type.sqliteAffinity || !type.opaqueName.empty())
      fail(origin,"relational SQL VALUES type is outside the PostgreSQL backend");
    if (const auto bits=signedIntegerBits(type)) {
      write(*bits==16?"smallint":*bits==32?"integer":"bigint");
      return;
    }
    switch (type.family) {
    case TypeFamily::Boolean:write("boolean");return;
    case TypeFamily::Decimal:
      if (type.scale && !type.precision) break;
      write("numeric");
      if (type.precision) {
        write("(");
        write(std::to_string(*type.precision));
        if (type.scale) {
          write(",");
          write(std::to_string(*type.scale));
        }
        write(")");
      }
      return;
    case TypeFamily::Floating:
      if (type.bits==32 || type.bits==64) {write(type.bits==32?"real":"double precision");return;}
      break;
    case TypeFamily::Date:write("date");return;
    default:break;
    }
    fail(origin,"relational SQL VALUES needs a renderable PostgreSQL output type");
  }
  void typeSql(TypeId id,SourceRange origin) {
    const auto &type=plan.program.types.get(id);
    if (!type.arrayDimensions) {
      scalarTypeSql(type,origin);
      return;
    }
    auto element=type;
    element.arrayDimensions=0;
    scalarTypeSql(element,origin);
    write("[]");
  }
  void values(const RelNode &n,const Demand &needed,bool visible) {
    const bool typed=n.valueTypes.size()==n.outputs.size();
    if (!n.valueCount) {
      if (!typed || !n.keys.empty())
        fail(n.origin,"relational SQL requires resolved output types for an empty relation");
      write("SELECT ");
      fields(n,needed,visible,[&](auto i) {
        write("CAST(NULL AS ");
        typeSql(n.valueTypes[i],n.origin);
        write(")");
      });
      write(" WHERE FALSE");
      return;
    }
    if (n.valueCount==1 && n.outputs.empty() && n.keys.empty()) {
      write("SELECT 1");
      return;
    }
    if (n.outputs.empty() || n.outputs.size()>options.maxVisits/n.valueCount ||
        n.keys.size()!=n.outputs.size()*n.valueCount)
      fail(n.origin,"relational SQL requires typed nonempty VALUES rows");
    Input source{&n,"r"+std::to_string(++nextAlias),Demand(n.outputs.size())};
    write("SELECT ");
    inputColumns(n,source,needed,visible);
    write(" FROM (VALUES ");
    for (std::size_t row=0;row<n.valueCount;++row) {
      if (row) write(", ");
      write("(");
      for (std::size_t i=0;i<n.outputs.size();++i) {
        if (i) write(", ");
        if (typed) write("CAST(");
        expression(n.keys[row*n.outputs.size()+i],{});
        if (typed) {
          write(" AS ");
          typeSql(n.valueTypes[i],n.origin);
          write(")");
        }
      }
      write(")");
    }
    write(") AS ");
    identifier(source.alias);
    write("(");
    for (std::size_t i=0;i<n.outputs.size();++i) {
      if (i) write(", ");
      identifier(slotName(i));
    }
    write(")");
  }
  void scan(const RelNode &n,const ModelStatement &q,const Demand &needed,bool visible,
            const TableSampling *sampling=nullptr) {
    if (!q.catalogChecked || !n.table || *n.table>=q.tables.size())
      fail(n.origin,"relational SQL scan needs a verified catalog binding");
    const auto &binding=q.tables[*n.table];
    const auto &table=sourceDefinition(binding);
    if (!table || !table->complete ||
        (table->kind!=ObjectKind::Table &&
         (table->kind!=ObjectKind::MaterializedView || n.scanSource!=ScanSource::MaterializedView ||
          binding.inheritance!=TableInheritance::Default || q.tableQuery==TableQuerySyntax::Only)) ||
        table->schema.empty() || (!table->database.empty() && table->database!=q.context.database) ||
        !binding.columnAliases.empty() || n.outputs.empty() ||
        n.outputs.size()!=table->columns.size())
      fail(n.origin,"relational SQL scan needs a complete table in the current database");
    const std::string alias="r"+std::to_string(++nextAlias);
    write("SELECT ");
    fields(n,needed,visible,[&](auto i) {
      const auto &ref=n.outputs[i];
      if (ref.query!=q.id || ref.table!=*n.table || ref.name!=table->columns[i].name)
        fail(n.origin,"relational SQL scan column identity changed",DiagnosticCode::InvalidQueryShape);
      identifier(alias);
      write(".");
      identifier(ref.name);
    });
    write(" FROM ");
    if (binding.inheritance==TableInheritance::Only || q.tableQuery==TableQuerySyntax::Only)
      write("ONLY ");
    identifier(table->schema);
    write(".");
    identifier(table->name);
    if (binding.inheritance==TableInheritance::IncludeDescendants) write("*");
    write(" AS ");
    identifier(alias);
    if (sampling) {
      write(" TABLESAMPLE BERNOULLI (");
      write(tableSamplePercent(sampling->milliPercent));
      write(")");
      if (sampling->repeatable) {
        write(" REPEATABLE (");
        write(std::to_string(*sampling->repeatable));
        write(")");
      }
    }
  }
  static std::string cteName(QueryId id) {
    return "navidog_cte_"+std::to_string(id);
  }
  const ModelTable &relationalBinding(const RelNode &n) {
    const auto &q=modelQuery(plan,n.query);
    if (!n.table || *n.table>=q.tables.size() || n.scalar)
      fail(n.origin,"relational SQL needs a derived table or CTE");
    const auto &binding=q.tables[*n.table];
    const auto &definition=sourceDefinition(binding);
    if (!binding.query || binding.workingTableOf || !definition ||
        !definition->arityKnown || n.outputs.size()!=definition->columns.size() ||
        (n.materialized!=(binding.cteMaterialization==CteMaterialization::Materialized)) ||
        (n.correlated && !binding.name.empty()))
      fail(n.origin,"relational SQL needs complete derived relation bindings");
    for (std::size_t i=0;i<n.outputs.size();++i) {
      const auto &output=n.outputs[i];
      if (output.query!=q.id || output.table!=*n.table ||
          output.name!=definition->columns[i].name)
        fail(n.origin,"relational SQL derived column identity changed",DiagnosticCode::InvalidQueryShape);
    }
    return binding;
  }
  void discoverCtes(RelId id,std::vector<bool> &done,std::vector<bool> &active,std::size_t depth) {
    const auto &n=node(id);
    if (done[id]) return;
    visit(n.origin);
    if (active[id] || depth>=std::min<std::size_t>(128,options.maxNesting))
      fail(n.origin,"cyclic or excessive relational SQL dependency depth",DiagnosticCode::ResourceLimit);
    active[id]=true;
    for (auto child:n.inputs) discoverCtes(child,done,active,depth+1);
    if (n.kind==RelKind::Subquery && n.scalar) {
      const auto &inner=unary(n);
      const auto [found,inserted]=scalarInputs.try_emplace(std::make_pair(n.query,inner.query),
          ScalarDependency{n.inputs.front(),n.correlated});
      if (!inserted && (found->second.root!=n.inputs.front() || found->second.correlated!=n.correlated))
        fail(n.origin,"relational SQL has inconsistent scalar dependency plans",DiagnosticCode::InvalidQueryShape);
    } else if (n.kind==RelKind::Subquery) {
      const auto &binding=relationalBinding(n);
      if (!binding.name.empty()) {
        unary(n);
        const auto &query=modelQuery(plan,binding.query);
        const Cte definition{n.inputs.front(),binding.cteMaterialization,
                             query.setOperation && query.setOperation->recursive};
        const auto [known,inserted]=ctes.try_emplace(binding.query,definition);
        if (inserted) cteOrder.push_back(binding.query);
        else if (known->second.root!=definition.root || known->second.materialization!=definition.materialization)
          fail(n.origin,"relational SQL CTE has inconsistent definitions",DiagnosticCode::InvalidQueryShape);
      }
    }
    active[id]=false;
    done[id]=true;
  }
  void recursiveDefinition(QueryId owner,const Cte &definition) {
    const auto *repeat=&node(definition.root);
    if (repeat->kind==RelKind::Project && repeat->keys.size()==repeat->outputs.size()) {
      const auto &input=unary(*repeat);
      sameShape(*repeat,input);
      for (std::size_t i=0;i<repeat->keys.size();++i) {
        const auto column=boundScalarColumn(plan,repeat->keys[i]);
        if (!column || !sameColumn(*column,input.outputs[i]))
          fail(repeat->origin,"recursive SQL projection changes a working-table column");
      }
      repeat=&input;
    }
    if (repeat->kind!=RelKind::RepeatUnion || repeat->setKind!=SetKind::Union ||
        repeat->query!=owner || repeat->workingTableOf!=owner || repeat->inputs.size()!=2 ||
        repeat->outputs.empty())
      fail(repeat->origin,"relational SQL recursive CTE requires a seed and recursive UNION member");
    const auto width=repeat->outputs.size();
    if (node(repeat->inputs[0]).outputs.size()!=width || node(repeat->inputs[1]).outputs.size()!=width)
      fail(repeat->origin,"recursive SQL seed and step must have the same output width");
    for (std::size_t i=0;i<width;++i) {
      const auto seed=integerWidth(repeat->inputs[0],i);
      if (!seed || seed!=integerWidth(repeat->inputs[1],i))
        fail(repeat->origin,"relational SQL recursive UNION needs identical verified integer types");
    }
    const Demand all(width,true);
    write("(");
    rows(repeat->inputs[0],all,false,1);
    write(repeat->distinct?") UNION (":") UNION ALL (");
    recursiveScope=owner;
    recursiveStep(repeat->inputs[1],all);
    recursiveScope.reset();
    write(")");
  }
  void verifyWorkingTable(const RelNode &n) {
    if (!recursiveScope || n.workingTableOf!=*recursiveScope || !n.table || !n.workingSeed)
      fail(n.origin,"relational SQL working table is outside its recursive step");
    const auto &owner=modelQuery(plan,n.query);
    const auto &binding=owner.tables.at(*n.table);
    if (binding.workingTableOf!=*recursiveScope || !binding.definition ||
        binding.definition->columns.size()!=n.outputs.size() ||
        node(n.workingSeed).outputs.size()!=n.outputs.size())
      fail(n.origin,"relational SQL working-table output shape is incomplete");
    for (std::size_t i=0;i<n.outputs.size();++i) {
      if (n.outputs[i].query!=owner.id || n.outputs[i].table!=*n.table ||
          n.outputs[i].name!=binding.definition->columns[i].name)
        fail(n.origin,"relational SQL working-table column identity changed",
             DiagnosticCode::InvalidQueryShape);
    }
  }
  void workingFrom(const Input &source) {
    identifier(cteName(*recursiveScope));
    write(" AS ");
    identifier(source.alias);
  }
  void workingTable(const RelNode &n,const Demand &needed,bool visible) {
    verifyWorkingTable(n);
    Input source{&n,"r"+std::to_string(++nextAlias),Demand(n.outputs.size())};
    write("SELECT ");
    fields(n,needed,visible,[&](auto i) { column(source,i); });
    write(" FROM ");
    workingFrom(source);
  }
  void recursiveStep(RelId id,const Demand &needed) {
    const auto &root=node(id);
    const auto *project=root.kind==RelKind::Project?&root:nullptr;
    const auto &body=project?unary(*project):root;
    const auto *filter=body.kind==RelKind::Filter?&body:nullptr;
    const auto &sourceNode=filter?unary(*filter):body;
    if (filter) sameShape(*filter,sourceNode);
    if (project && project->keys.size()!=project->outputs.size())
      fail(root.origin,"relational SQL recursive projection needs explicit columns");
    if (project && sourceNode.kind==RelKind::Join && sourceNode.inputs.size()==2 &&
        !sourceNode.distinct && !sourceNode.correlated &&
        (sourceNode.join==JoinKind::Inner || sourceNode.join==JoinKind::Cross)) {
      auto left=input(sourceNode.inputs[0]),right=input(sourceNode.inputs[1]);
      const bool leftWorking=left.node->kind==RelKind::WorkingTableScan;
      const bool rightWorking=right.node->kind==RelKind::WorkingTableScan;
      if (leftWorking==rightWorking ||
          sourceNode.predicates.empty()!=(sourceNode.join==JoinKind::Cross))
        fail(root.origin,"relational SQL recursive join needs one direct working-table input");
      verifyWorkingTable(*(leftWorking?left.node:right.node));
      write("SELECT ");
      fields(root,needed,false,[&](auto i) { expression(project->keys[i],{&left,&right}); });
      const auto suffix=collectSuffix([&] {
        if (sourceNode.join==JoinKind::Inner) {
          write(" ON ");
          predicates(sourceNode,{&left,&right});
        }
        if (filter) {
          write(" WHERE ");
          predicates(*filter,{&left,&right});
        }
      });
      write(" FROM ");
      if (leftWorking) workingFrom(left);
      else from(sourceNode.inputs[0],left,1);
      write(sourceNode.join==JoinKind::Inner?" INNER JOIN ":" CROSS JOIN ");
      if (rightWorking) workingFrom(right);
      else from(sourceNode.inputs[1],right,1);
      appendSuffix(suffix);
      return;
    }
    if (sourceNode.kind!=RelKind::WorkingTableScan) {
      if (root.kind==RelKind::Values) {
        rows(id,needed,false,1);
        return;
      }
      fail(root.origin,"relational SQL recursive member needs a direct working-table input");
    }
    verifyWorkingTable(sourceNode);
    Input source{&sourceNode,"r"+std::to_string(++nextAlias),Demand(sourceNode.outputs.size())};
    write("SELECT ");
    fields(root,needed,false,[&](auto i) {
      if (project) expression(project->keys[i],{&source});
      else column(source,i);
    });
    write(" FROM ");
    workingFrom(source);
    if (filter) {
      write(" WHERE ");
      predicates(*filter,{&source});
    }
  }
  void emitCtes() {
    const bool recursive=std::any_of(ctes.begin(),ctes.end(),[](const auto &entry) {
      return entry.second.recursive;
    });
    for (std::size_t i=0;i<cteOrder.size();++i) {
      const auto id=cteOrder[i];
      const auto &definition=ctes.at(id);
      const auto width=node(definition.root).outputs.size();
      if (!width) fail(node(definition.root).origin,"relational SQL CTE has no output shape");
      write(i?", ":recursive?"WITH RECURSIVE ":"WITH ");
      identifier(cteName(id));
      write(" AS ");
      if (definition.materialization==CteMaterialization::Materialized) write("MATERIALIZED ");
      if (definition.materialization==CteMaterialization::NotMaterialized) write("NOT MATERIALIZED ");
      write("(");
      if (definition.recursive) recursiveDefinition(id,definition);
      else rows(definition.root,Demand(width,true),false,0);
      write(")");
    }
    if (!cteOrder.empty()) write(" ");
  }
  void positionalProjection(const RelNode &n,const Demand &needed,bool visible,
                            std::size_t depth) {
    const auto sourceId=n.inputs.front();
    auto source=input(sourceId);
    if (n.outputs.size()!=source.node->outputs.size())
      fail(n.origin,"relational SQL positional projection changed the column count",
           DiagnosticCode::InvalidQueryShape);
    const auto *binding=n.kind==RelKind::Subquery?&relationalBinding(n):nullptr;
    if (n.correlated && (outerInputs.empty() || !binding ||
                         modelQuery(plan,binding->query).scope.parent!=n.query))
      fail(n.origin,"relational SQL correlated input requires its LATERAL scope");
    write("SELECT ");
    fields(n,needed,visible,[&](auto i) { column(source,i); });
    write(" FROM ");
    if (binding && !binding->name.empty()) {
      if (!ctes.count(binding->query))
        fail(n.origin,"relational SQL CTE was not declared",DiagnosticCode::InvalidQueryShape);
      identifier(cteName(binding->query));
      write(" AS ");
      identifier(source.alias);
    } else {
      from(sourceId,source,depth);
    }
  }
  void distinctRows(const RelNode &n,const Demand &needed,bool visible,std::size_t depth) {
    const auto sourceId=n.inputs.front();
    auto source=input(sourceId);
    sameShape(n,*source.node);
    if (n.outputs.empty())
      fail(n.origin,"relational SQL DISTINCT requires its complete row shape");
    const bool narrowed=std::find(needed.begin(),needed.end(),false)!=needed.end();
    const Input allFields{&n,"r"+std::to_string(++nextAlias),Demand(n.outputs.size(),true)};
    if (narrowed) {
      auto deduplicated=allFields;
      write("SELECT ");
      inputColumns(n,deduplicated,needed,visible);
      write(" FROM (");
    }
    write("SELECT DISTINCT ");
    inputColumns(n,source,allFields.columns,narrowed?false:visible);
    write(" FROM ");
    from(sourceId,source,depth);
    if (narrowed) {
      write(") AS ");
      identifier(allFields.alias);
    }
  }
  void scalarCalc(const RelNode &n,const Demand &needed,bool visible,std::size_t depth) {
    const auto sourceId=n.inputs.front();
    auto source=input(sourceId);
    write("SELECT ");
    if (n.keys.empty()) {
      sameShape(n,*source.node);
      inputColumns(n,source,needed,visible);
    } else {
      if (n.keys.size()!=n.outputs.size())
        fail(n.origin,"relational SQL Calc has incomplete projection",DiagnosticCode::InvalidQueryShape);
      fields(n,needed,visible,[&](auto i) { expression(n.keys[i],{&source}); });
    }
    const auto suffix=collectSuffix([&] {
      if (!n.predicates.empty()) {
        write(" WHERE ");
        predicates(n,{&source});
      }
    });
    write(" FROM ");
    from(sourceId,source,depth);
    appendSuffix(suffix);
  }
  void groupKeys(const std::vector<ExpressionRef> &keys,Input &source) {
    for (std::size_t i=0;i<keys.size();++i) {
      auto ref=keys[i];
      const auto &original=modelExpression(plan,ref);
      if (original.outputColumn) {
        const auto &owner=modelQuery(plan,ref.query);
        if (*original.outputColumn>=owner.projections.size())
          fail(original.origin,"relational SQL group ordinal is outside the projection",
               DiagnosticCode::InvalidQueryShape);
        ref.expression=owner.projections[*original.outputColumn].expression;
      }
      const auto &key=modelExpression(plan,ref);
      if (!boundScalarColumn(plan,ref) || postgresGroupingType(plan.program.types.get(key.sqlType)).empty())
        fail(key.origin,"relational SQL grouping needs a bound PostgreSQL integer or Boolean column");
      if (i) write(", ");
      expression(ref,{&source});
    }
  }
  void groupBy(const RelNode &group,Input &source) {
    write(" GROUP BY ");
    if (!group.groupingSets.empty()) {
      write("GROUPING SETS (");
      for (std::size_t i=0;i<group.groupingSets.size();++i) {
        visit(group.origin);
        if (i) write(", ");
        write("(");
        groupKeys(group.groupingSets[i],source);
        write(")");
      }
      write(")");
    } else if (group.keys.empty()) {
      // An empty grouping list produces one group even on an empty input.
      write("()");
    } else {
      groupKeys(group.keys,source);
    }
  }
  void project(const RelNode &n,const ModelStatement &q,const Demand &needed,
               bool visible,std::size_t depth) {
    auto sourceId=n.inputs.front();
    const auto &child=node(sourceId);
    const bool windowed=child.kind==RelKind::Window;
    if (windowed) {
      const auto &below=unary(child);
      sameShape(child,below);
      if (child.query!=q.id)
        fail(child.origin,"relational SQL window input is outside its query block");
      sourceId=child.inputs.front();
    }
    const auto &body=node(sourceId);
    const RelNode *having=nullptr;
    const RelNode *group=nullptr;
    if (body.kind==RelKind::Aggregate) {
      group=&body;
    } else if (body.kind==RelKind::Filter && body.inputs.size()==1 &&
               node(body.inputs.front()).kind==RelKind::Aggregate) {
      having=&body;
      group=&node(body.inputs.front());
    }
    if (group) {
      visit(group->origin);
      if (having) visit(having->origin);
      if (group->query!=q.id || group->distinct ||
          (having && (having->query!=q.id || having->predicates.empty())))
        fail(group->origin,"invalid grouped projection shape",DiagnosticCode::InvalidQueryShape);
      sameShape(*group,unary(*group));
      sourceId=group->inputs.front();
    }
    auto source=input(sourceId);
    const bool star=!group && n.keys.size()==1 &&
        modelExpression(plan,n.keys.front()).kind==ExprKind::Star;
    if (!star && (n.keys.size()!=n.outputs.size() || n.keys.empty()))
      fail(n.origin,"relational SQL projection requires explicit columns");
    const auto mode=windowed?(group?ExpressionScope::GroupedWindow:ExpressionScope::Windowed):
        group?ExpressionScope::Grouped:ExpressionScope::Scalar;
    write("SELECT ");
    fields(n,needed,visible,[&](auto i) {
      if (star) {
        const auto &outputs=source.node->outputs;
        const auto same=[&](const auto &candidate){return sameColumn(candidate,n.outputs[i]);};
        const auto position=std::find_if(outputs.begin(),outputs.end(),same);
        if (position==outputs.end() || std::find_if(position+1,outputs.end(),same)!=outputs.end())
          fail(n.origin,"relational SQL wildcard field is ambiguous",DiagnosticCode::InvalidQueryShape);
        column(source,static_cast<std::size_t>(position-outputs.begin()));
        return;
      }
      expression(n.keys[i],{&source},mode);
    });
    const auto suffix=collectSuffix([&] {
      if (group) groupBy(*group,source);
      if (having) {
        write(" HAVING ");
        predicates(*having,{&source},ExpressionScope::Grouped);
      }
    });
    write(" FROM ");
    from(sourceId,source,depth);
    appendSuffix(suffix);
  }
  void groupingUnion(const RelNode &n,const ModelStatement &query,const Demand &needed,
                     bool visible,std::size_t depth) {
    if (!isUnionAll(n) || query.setOperation || query.groupingSets.empty() ||
        n.inputs.size()<2 || n.inputs.size()>32)
      fail(n.origin,"relational SQL set operation needs verified grouping-set branches");
    for (std::size_t arm=0;arm<n.inputs.size();++arm) {
      const auto id=n.inputs[arm];
      const auto &branch=node(id);
      sameShape(n,branch);
      if (branch.kind!=RelKind::Project || branch.query!=query.id ||
          branch.keys.size()!=query.projections.size())
        fail(branch.origin,"invalid grouping-set branch projection",DiagnosticCode::InvalidQueryShape);
      auto *group = &unary(branch);
      if (group->kind == RelKind::Filter) {
        if (!query.having || group->query != query.id || group->predicates.empty())
          fail(group->origin, "invalid grouping-set branch HAVING", DiagnosticCode::InvalidQueryShape);
        group = &unary(*group);
      }
      if (group->kind != RelKind::Aggregate || !group->groupingSets.empty())
        fail(group->origin,"invalid grouping-set branch aggregate",DiagnosticCode::InvalidQueryShape);
      for (std::size_t i=0;i<branch.keys.size();++i) {
        visit(branch.origin);
        const auto &result=modelExpression(plan,branch.keys[i]);
        const auto &original=modelExpression(plan,{query.id,query.projections[i].expression});
        if (!result.sqlType || result.sqlType!=original.sqlType)
          fail(result.origin,"grouping-set branch output type changed",DiagnosticCode::InvalidQueryShape);
      }
      if (arm) write(" UNION ALL ");
      write("(");
      rows(id,needed,visible,depth+1);
      write(")");
    }
  }
  std::optional<int> integerWidth(RelId id,std::size_t column,std::size_t depth=0) {
    const auto &n=node(id);
    visit(n.origin);
    if (depth>=options.maxNesting || depth>=128 || column>=n.outputs.size()) return {};
    const auto &q=modelQuery(plan,n.query);
    if (n.kind==RelKind::Values && n.valueTypes.size()==n.outputs.size())
      return signedIntegerBits(plan.program.types.get(n.valueTypes[column]));
    if ((n.kind==RelKind::Project || n.kind==RelKind::Calc) && n.keys.size()==n.outputs.size()) {
      const auto &e=modelExpression(plan,n.keys[column]);
      if (e.kind==ExprKind::Null || e.kind==ExprKind::String ||
          e.kind==ExprKind::Parameter || e.kind==ExprKind::Star)
        return {};
      return signedIntegerBits(plan.program.types.get(e.sqlType));
    }
    if (n.kind==RelKind::Scan && n.table && *n.table<q.tables.size()) {
      const auto &table=sourceDefinition(q.tables[*n.table]);
      if (!table || column>=table->columns.size() || !table->columns[column].sqlType) return {};
      return signedIntegerBits(*table->columns[column].sqlType);
    }
    if (n.kind==RelKind::WorkingTableScan && n.workingSeed)
      return integerWidth(n.workingSeed,column,depth+1);
    if ((n.kind==RelKind::Join || n.kind==RelKind::Correlate) && !n.distinct && n.inputs.size()==2) {
      const auto &left=node(n.inputs[0]), &right=node(n.inputs[1]);
      if (n.outputs.size()!=left.outputs.size()+right.outputs.size()) return {};
      const auto side=column<left.outputs.size()?0u:1u;
      const auto ordinal=side?column-left.outputs.size():column;
      const auto &child=side?right:left;
      if (!sameColumn(n.outputs[column],child.outputs[ordinal])) return {};
      return integerWidth(n.inputs[side],ordinal,depth+1);
    }
    if (n.kind==RelKind::Values && n.valueCount && n.outputs.size()<=options.maxVisits/n.valueCount &&
        n.keys.size()==n.valueCount*n.outputs.size()) {
      std::optional<int> width;
      for (std::size_t i=0;i<n.valueCount;++i) {
        const auto &value=modelExpression(plan,n.keys[i*n.outputs.size()+column]);
        visit(value.origin);
        if (value.kind==ExprKind::Null) continue;
        const auto type=signedIntegerBits(plan.program.types.get(value.sqlType));
        if (!type || value.kind==ExprKind::String || value.kind==ExprKind::Parameter) return {};
        width=std::max(width.value_or(0),*type);
      }
      return width;
    }
    if (n.kind==RelKind::Coerce && n.inputs.size()==1) {
      const auto found=std::find_if(n.integerWidenings.begin(),n.integerWidenings.end(),
          [&](const auto &coercion){return coercion.ordinal==column;});
      if (found!=n.integerWidenings.end())
        return signedIntegerBits(plan.program.types.get(found->target));
      return integerWidth(n.inputs.front(),column,depth+1);
    }
    if (n.inputs.size()==1 && (n.kind==RelKind::Sort || n.kind==RelKind::Limit ||
        n.kind==RelKind::Filter || n.kind==RelKind::Rename || n.kind==RelKind::Subquery ||
        n.kind==RelKind::Sample ||
        (n.kind==RelKind::Calc && n.keys.empty()) ||
        (n.kind==RelKind::Aggregate && n.distinct)))
      return integerWidth(n.inputs.front(),column,depth+1);
    if (n.kind==RelKind::SetOperation && n.inputs.size()>=2) {
      int width=0;
      for (const auto arm:n.inputs) {
        const auto bit=integerWidth(arm,column,depth+1);
        if (!bit) return {};
        width=std::max(width,*bit);
      }
      return width;
    }
    return {};
  }
  void coerce(const RelNode &n,const Demand &needed,bool visible,std::size_t depth) {
    auto source=input(n.inputs.front());
    sameShape(n,*source.node);
    if (n.integerWidenings.empty() || n.integerWidenings.size()>n.outputs.size())
      fail(n.origin,"relational SQL integer widening needs valid output ordinals");
    std::vector<int> widths(n.outputs.size(),0);
    for (const auto &coercion:n.integerWidenings) {
      visit(n.origin);
      const auto from=signedIntegerBits(plan.program.types.get(coercion.source));
      const auto to=signedIntegerBits(plan.program.types.get(coercion.target));
      if (coercion.ordinal>=widths.size() || widths[coercion.ordinal] ||
          !from || !to || *from>=*to || integerWidth(n.inputs.front(),coercion.ordinal)!=from)
        fail(n.origin,"relational SQL integer widening is not verified");
      widths[coercion.ordinal]=*to;
    }
    write("SELECT ");
    fields(n,needed,visible,[&](auto i) {
      if (!widths[i]) {
        column(source,i);
        return;
      }
      write("CAST(");
      column(source,i);
      write(widths[i]==64?" AS bigint)":" AS integer)");
    });
    write(" FROM ");
    from(n.inputs.front(),source,depth);
  }
  void setMembership(const RelNode &n,const Demand &needed,bool visible,std::size_t depth) {
    const bool narrow=std::find(needed.begin(),needed.end(),false)!=needed.end();
    if (narrow) {
      Input projected{&n,"r"+std::to_string(++nextAlias),Demand(n.outputs.size())};
      write("SELECT ");
      inputColumns(n,projected,needed,visible);
      write(" FROM (");
      setMembership(n,Demand(n.outputs.size(),true),false,depth+1);
      write(") AS ");
      identifier(projected.alias);
      return;
    }
    auto left=input(n.inputs.front());
    write("SELECT DISTINCT ");
    fields(n,needed,visible,[&](auto i) { column(left,i); });
    write(" FROM ");
    from(n.inputs.front(),left,depth);
    for (std::size_t arm=1;arm<n.inputs.size();++arm) {
      auto right=input(n.inputs[arm]);
      std::fill(right.columns.begin(),right.columns.end(),true);
      write(arm==1?" WHERE ":" AND ");
      if (n.setKind==SetKind::Except) write("NOT ");
      write("EXISTS (SELECT 1 FROM ");
      from(n.inputs[arm],right,depth);
      write(" WHERE ");
      for (std::size_t i=0;i<n.outputs.size();++i) {
        visit(n.origin);
        if (i) write(" AND ");
        column(left,i);
        write(" IS NOT DISTINCT FROM ");
        column(right,i);
      }
      write(")");
    }
  }
  void setBranchAggregate(const RelNode &n,const Demand &needed,bool visible,std::size_t depth) {
    constexpr std::string_view branchName="navidog_branch";
    Input combined{&n,"r"+std::to_string(++nextAlias),Demand(n.outputs.size(),true)};
    write("SELECT ");
    inputColumns(n,combined,needed,visible);
    write(" FROM (");
    for (std::size_t arm=0;arm<n.inputs.size();++arm) {
      auto source=input(n.inputs[arm]);
      if (arm) write(" UNION ALL ");
      write("SELECT ");
      fields(n,combined.columns,false,[&](auto i) { column(source,i); });
      write(", ");
      write(std::to_string(arm));
      write(" AS ");
      identifier(branchName);
      write(" FROM ");
      from(n.inputs[arm],source,depth);
    }
    write(") AS ");
    identifier(combined.alias);
    write(" GROUP BY ");
    for (std::size_t i=0;i<n.outputs.size();++i) {
      visit(n.origin);
      if (i) write(", ");
      column(combined,i);
    }
    write(" HAVING ");
    for (std::size_t arm=0;arm<n.inputs.size();++arm) {
      visit(n.origin);
      if (arm) write(" AND ");
      write("COUNT(*) FILTER (WHERE ");
      identifier(combined.alias);
      write(".");
      identifier(branchName);
      write(" = ");
      write(std::to_string(arm));
      write(n.setKind==SetKind::Except && arm?") = 0":") > 0");
    }
  }
  void setOperation(const RelNode &n,const ModelStatement &query,const Demand &needed,
                    bool visible,std::size_t depth) {
    if (!query.setOperation && !query.groupingSets.empty()) {
      groupingUnion(n,query,needed,visible,depth);
      return;
    }
    if (n.inputs.size()<2 || n.inputs.size()>64)
      fail(n.origin,"relational SQL set operation needs bounded explicit arms");
    std::vector<int> widths(n.outputs.size());
    for (std::size_t arm=0;arm<n.inputs.size();++arm) {
      const auto child=n.inputs[arm];
      if (n.outputs.size()!=node(child).outputs.size())
        fail(n.origin,"relational SQL set operation changed the column count",DiagnosticCode::InvalidQueryShape);
      for (std::size_t i=0;i<n.outputs.size();++i) {
        const auto width=integerWidth(child,i);
        if (!width || (n.setAlgorithm!=SetAlgorithm::Native && arm && *width!=widths[i]))
          fail(n.origin,"relational SQL set inputs require independently typed signed integer columns");
        if (!arm) widths[i]=*width;
      }
    }
    if (n.setAlgorithm!=SetAlgorithm::Native) {
      if (!n.distinct || n.setKind==SetKind::Union || n.outputs.empty() ||
          (n.setAlgorithm==SetAlgorithm::CorrelatedExists && n.setKind!=SetKind::Intersect))
        fail(n.origin,"alternate relational SQL set requires distinct intersection or difference");
      if (n.setAlgorithm==SetAlgorithm::BranchAggregate) setBranchAggregate(n,needed,visible,depth);
      else setMembership(n,needed,visible,depth);
      return;
    }
    std::string_view keyword;
    switch(n.setKind) {
    case SetKind::Union:keyword=" UNION";break;
    case SetKind::Intersect:keyword=" INTERSECT";break;
    case SetKind::Except:keyword=" EXCEPT";break;
    }
    Input combined{&n,"r"+std::to_string(++nextAlias),Demand(n.outputs.size())};
    write("SELECT ");
    inputColumns(n,combined,needed,visible);
    write(" FROM (");
    // Non-ALL sets and INTERSECT/EXCEPT account for every field when comparing rows.
    const auto armsDemand=isUnionAll(n)?combined.columns:Demand(n.outputs.size(),true);
    for (std::size_t i=0;i<n.inputs.size();++i) {
      if (i) {
        write(keyword);
        write(n.distinct?" ":" ALL ");
      }
      write("(");
      rows(n.inputs[i],armsDemand,false,depth+1);
      write(")");
    }
    write(") AS ");
    identifier(combined.alias);
  }
  void membership(const RelNode &n,const Demand &needed,bool visible,std::size_t depth) {
    const auto keys=membershipColumns(plan,n);
    if (!keys) fail(n.origin,"relational SQL membership needs a verified integer key pair");
    auto outer=input(n.inputs[0]),inner=input(n.inputs[1]);
    const auto slot=[&](const Input &source,const ColumnRef &field) {
      const auto &columns=source.node->outputs;
      const auto match=[&](const auto &candidate){return sameColumn(candidate,field);};
      const auto found=std::find_if(columns.begin(),columns.end(),match);
      if (found==columns.end() || std::find_if(found+1,columns.end(),match)!=columns.end())
        fail(n.origin,"relational SQL membership key has no unique input slot",DiagnosticCode::InvalidQueryShape);
      return static_cast<std::size_t>(found-columns.begin());
    };
    const auto outerKey=slot(outer,keys->first), innerKey=slot(inner,keys->second);
    outer.columns[outerKey]=true;
    inner.columns[innerKey]=true;
    write("SELECT ");
    inputColumns(n,outer,needed,visible);
    write(" FROM ");
    from(n.inputs[0],outer,depth);
    if (n.kind==RelKind::AntiJoin && !n.predicates.empty()) {
      const auto &original=modelExpression(plan,n.predicates.front());
      if (original.kind==ExprKind::In || original.kind==ExprKind::Quantified) {
        // Retain UNKNOWN and empty-set semantics if a database's
        // non-null proofs have changed since the plan was built.
        write(" WHERE ");
        column(outer,outerKey);
        write(" NOT IN (SELECT ");
        column(inner,innerKey);
        write(" FROM ");
        from(n.inputs[1],inner,depth);
        write(")");
        return;
      }
    }
    write(n.kind==RelKind::AntiJoin?" WHERE NOT EXISTS (SELECT 1 FROM ":" WHERE EXISTS (SELECT 1 FROM ");
    from(n.inputs[1],inner,depth);
    write(" WHERE ");
    column(outer,outerKey);
    write(" = ");
    column(inner,innerKey);
    write(")");
  }
  void integerArrayArgument(ExpressionRef ref,Input &source) {
    const auto &argument=modelExpression(plan,ref);
    auto element=plan.program.types.get(argument.sqlType);
    if (element.arrayDimensions<1) fail(argument.origin,"native array expansion needs a typed array");
    element.arrayDimensions=0;
    if (!signedIntegerBits(element)) fail(argument.origin,"native array expansion requires exact integer elements");
    if (argument.kind==ExprKind::Name && argument.binding && !argument.outputColumn &&
        !argument.localSymbol && argument.coalescedBindings.empty()) {
      const auto &binding=*argument.binding;
      const auto &scope=modelQuery(plan,binding.scope);
      const auto &definition=scope.tables.at(binding.table).definition;
      if (!definition || binding.column>=definition->columns.size())
        fail(argument.origin,"array expansion has an incomplete column binding");
      const ColumnRef field{binding.scope,binding.table,definition->columns[binding.column].name};
      const auto emit=[&](Input &input) {
        const auto &outputs=input.node->outputs;
        const auto found=std::find_if(outputs.begin(),outputs.end(),[&](const auto &candidate) {
          return sameColumn(candidate,field);
        });
        if (found==outputs.end()) return false;
        column(input,static_cast<std::size_t>(found-outputs.begin()));
        return true;
      };
      if (!emit(source) && std::none_of(outerInputs.rbegin(),outerInputs.rend(),[&](auto *input) {
            return emit(*input);
          }))
        fail(argument.origin,"array expansion is missing its input binding");
    } else if (argument.kind==ExprKind::ArrayConstructor) {
      expression(ref,{&source});
    } else if (argument.kind==ExprKind::Cast && argument.operands.size()==1) {
      const auto &value=modelExpression(plan,{ref.query,argument.operands.front()});
      if (value.kind!=ExprKind::Null && value.kind!=ExprKind::String && value.kind!=ExprKind::ArrayConstructor)
        fail(argument.origin,"native array expansion requires an integer array constructor or literal cast");
      expression(ref,{&source});
    } else fail(argument.origin,"native array expansion requires a bound integer array");
  }
  void rowExpansion(const RelNode &n,const Demand &needed,bool visible,std::size_t depth) {
    const bool series=n.kind==RelKind::Series;
    if (plan.program.sourceDialect!=Dialect::PostgreSQL || n.inputs.empty() ||
        (series?(n.keys.size()<2 || n.keys.size()>3):n.keys.size()!=1))
      fail(n.origin,"invalid native PostgreSQL row expansion");
    auto source=input(n.inputs.front());
    const auto width=source.node->outputs.size();
    if (n.outputs.size()!=width+(n.withOrdinality?2:1) ||
        !std::equal(source.node->outputs.begin(),source.node->outputs.end(),n.outputs.begin(),sameColumn))
      fail(n.origin,"native row expansion changed input field identity",DiagnosticCode::InvalidQueryShape);
    const auto alias="r"+std::to_string(++nextAlias);
    write("SELECT ");
    fields(n,needed,visible,[&](auto ordinal) {
      if (ordinal<width) column(source,ordinal);
      else {
        identifier(alias);
        write(".");
        identifier(ordinal==width?"value":"ordinality");
      }
    });
    const auto call=collectSuffix([&] {
      write(series?" CROSS JOIN LATERAL pg_catalog.generate_series(":" CROSS JOIN LATERAL pg_catalog.unnest(");
      if (series) {
        for (std::size_t i=0;i<n.keys.size();++i) {
          if (i) write(", ");
          expression(n.keys[i],{&source});
        }
      } else integerArrayArgument(n.keys.front(),source);
      write(n.withOrdinality?") WITH ORDINALITY AS ":") AS ");
      identifier(alias);
      write("(");
      identifier("value");
      if (n.withOrdinality) {
        write(", ");
        identifier("ordinality");
      }
      write(")");
    });
    write(" FROM ");
    from(n.inputs.front(),source,depth);
    appendSuffix(call);
  }
  void unpivotText(std::string_view value,SourceRange origin) {
    if (value.find('\0')!=std::string_view::npos)
      fail(origin,"PostgreSQL UNPIVOT labels cannot contain NUL",DiagnosticCode::InvalidQueryShape);
    write("E'");
    for (char ch:value) {
      if (ch=='\\' || ch=='\'') write(std::string_view(&ch,1));
      write(std::string_view(&ch,1));
    }
    write("'::pg_catalog.text");
  }
  void pivot(const RelNode &n,const ModelStatement &q,const Demand &needed,
             bool visible,std::size_t depth) {
    if (plan.program.sourceDialect!=Dialect::PostgreSQL || n.inputs.size()!=1 ||
        !n.table || *n.table>=q.tables.size())
      fail(n.origin,"relational SQL needs a resolved PostgreSQL PIVOT");
    const auto &table=q.tables[*n.table];
    auto source=input(n.inputs.front());
    const auto &shape=table.pivot;
    if (!shape || !table.definition || !table.definition->complete || !table.pivotInput ||
        source.node->outputs.size()!=table.pivotInput->columns.size() ||
        n.outputs.size()!=table.definition->columns.size() ||
        n.outputs.size()!=shape->groupingOrdinals.size()+shape->buckets.size() ||
        shape->axisOrdinals.empty() || shape->buckets.empty())
      fail(n.origin,"invalid relational SQL PIVOT shape",DiagnosticCode::InvalidQueryShape);
    const auto &aggregate=shape->aggregate;
    if (aggregate!="COUNT" && aggregate!="SUM" && aggregate!="MIN" && aggregate!="MAX" && aggregate!="AVG")
      fail(n.origin,"relational SQL cannot emit this PIVOT aggregate");
    if (!shape->argumentOrdinal && aggregate!="COUNT")
      fail(n.origin,"relational SQL PIVOT aggregate input is missing",DiagnosticCode::InvalidQueryShape);
    const auto groups=shape->groupingOrdinals.size();
    const auto sourceField=[&](std::size_t i) {
      if (i>=source.node->outputs.size())
        fail(n.origin,"invalid relational SQL PIVOT input ordinal",DiagnosticCode::InvalidQueryShape);
      const auto &field=source.node->outputs[i];
      if (field.query!=q.id || field.table!=*n.table || field.name!=table.pivotInput->columns[i].name)
        fail(n.origin,"relational SQL PIVOT changed an input binding",DiagnosticCode::InvalidQueryShape);
      column(source,i);
    };
    write("SELECT ");
    fields(n,needed,visible,[&](std::size_t i) {
      const auto &output=n.outputs[i];
      if (output.query!=q.id || output.table!=*n.table || output.name!=table.definition->columns[i].name)
        fail(n.origin,"relational SQL PIVOT changed an output binding",DiagnosticCode::InvalidQueryShape);
      if (i<groups) {
        sourceField(shape->groupingOrdinals[i]);
        return;
      }
      const auto &bucket=shape->buckets[i-groups];
      write("pg_catalog.");
      write(aggregate);
      write("(");
      if (shape->argumentOrdinal) sourceField(*shape->argumentOrdinal);
      else write("*");
      write(") FILTER (WHERE ");
      if (bucket.values.size()!=shape->axisOrdinals.size())
        fail(bucket.origin,"invalid relational SQL PIVOT bucket width",DiagnosticCode::InvalidQueryShape);
      for (std::size_t j=0;j<bucket.values.size();++j) {
        visit(bucket.origin);
        if (j) write(" AND ");
        sourceField(shape->axisOrdinals[j]);
        write(" = ");
        expression({q.id,bucket.values[j]},{});
      }
      write(")");
    });
    const auto grouping=collectSuffix([&] {
      write(" GROUP BY ");
      if (!groups) write("()");
      for (std::size_t i=0;i<groups;++i) {
        if (i) write(", ");
        visit(n.origin);
        sourceField(shape->groupingOrdinals[i]);
      }
    });
    write(" FROM ");
    from(n.inputs.front(),source,depth);
    appendSuffix(grouping);
  }
  void unpivot(const RelNode &n,const ModelStatement &q,const Demand &needed,
               bool visible,std::size_t depth) {
    if (plan.program.sourceDialect!=Dialect::PostgreSQL || n.inputs.size()!=1 ||
        !n.table || *n.table>=q.tables.size())
      fail(n.origin,"relational SQL needs a resolved PostgreSQL UNPIVOT");
    const auto &table=q.tables[*n.table];
    auto source=input(n.inputs.front());
    const auto &shape=table.unpivot;
    if (!shape || !table.definition || !table.definition->complete || !table.unpivotInput ||
        source.node->outputs.size()!=table.unpivotInput->columns.size() ||
        n.outputs.size()!=table.definition->columns.size() ||
        n.outputs.size()!=shape->retainedOrdinals.size()+shape->descriptors.size()+shape->measures.size() ||
        shape->inputs.empty() || shape->inputs.size()!=shape->inputOrdinals.size() ||
        shape->descriptors.empty() || shape->measures.empty())
      fail(n.origin,"invalid relational SQL UNPIVOT shape",DiagnosticCode::InvalidQueryShape);
    const auto checkField=[&](const ColumnRef &ref,const std::string &name) {
      visit(n.origin);
      if (ref.query!=q.id || ref.table!=*n.table || ref.name!=name)
        fail(n.origin,"relational SQL UNPIVOT changed a column binding",DiagnosticCode::InvalidQueryShape);
    };
    for (std::size_t i=0;i<n.outputs.size();++i)
      checkField(n.outputs[i],table.definition->columns[i].name);
    for (std::size_t i=0;i<source.node->outputs.size();++i)
      checkField(source.node->outputs[i],table.unpivotInput->columns[i].name);
    const auto sourceField=[&](std::size_t i) {
      if (i>=source.node->outputs.size())
        fail(n.origin,"invalid relational SQL UNPIVOT input ordinal",DiagnosticCode::InvalidQueryShape);
      column(source,i);
    };
    const auto retained=shape->retainedOrdinals.size();
    const auto labels=shape->descriptors.size();
    const auto measures=shape->measures.size();
    const auto alias="r"+std::to_string(++nextAlias);
    const auto expandedField=[&](std::size_t i) {
      identifier(alias);
      write(".");
      identifier(slotName(i));
    };
    write("SELECT ");
    fields(n,needed,visible,[&](std::size_t i) {
      if (i<retained) sourceField(shape->retainedOrdinals[i]);
      else expandedField(i-retained);
    });
    const auto expansion=collectSuffix([&] {
      write(" CROSS JOIN LATERAL (VALUES ");
      for (std::size_t i=0;i<shape->inputs.size();++i) {
        const auto &branch=shape->inputs[i];
        const auto &ordinals=shape->inputOrdinals[i];
        if (ordinals.size()!=measures || branch.columns.size()!=measures ||
            (!branch.labels.empty() && branch.labels.size()!=labels))
          fail(branch.origin,"invalid relational SQL UNPIVOT branch width",DiagnosticCode::InvalidQueryShape);
        if (i) write(", ");
        write("(");
        const auto defaultLabel=branch.labels.empty()?unpivotDefaultLabel(branch.columns):std::string{};
        for (std::size_t j=0;j<labels;++j) {
          visit(branch.origin);
          if (branch.labels.empty()) unpivotText(defaultLabel,branch.origin);
          else {
            const auto &value=modelExpression(plan,{q.id,branch.labels[j]});
            if (value.kind==ExprKind::Null) write("NULL::pg_catalog.text");
            else if (value.kind==ExprKind::String) unpivotText(value.value,value.origin);
            else fail(value.origin,"relational SQL UNPIVOT requires text or NULL labels");
          }
          write(", ");
        }
        for (std::size_t j=0;j<measures;++j) {
          visit(branch.origin);
          if (j) write(", ");
          sourceField(ordinals[j]);
        }
        write(")");
      }
      write(") AS ");
      identifier(alias);
      write("(");
      for (std::size_t i=0;i<labels+measures;++i) {
        if (i) write(", ");
        identifier(slotName(i));
      }
      write(")");
      if (!shape->includeNulls.value_or(false)) {
        write(" WHERE ");
        for (std::size_t i=0;i<measures;++i) {
          if (i) write(" OR ");
          expandedField(labels+i);
          write(" IS NOT NULL");
        }
      }
    });
    write(" FROM ");
    from(n.inputs.front(),source,depth);
    appendSuffix(expansion);
  }
  void join(const RelNode &n,const Demand &needed,bool visible,std::size_t depth) {
    const bool lateral=n.kind==RelKind::Correlate;
    if (n.inputs.size()!=2 || n.distinct || n.correlated!=lateral)
      fail(n.origin,"relational SQL join needs two inputs and an explicit ON condition or CROSS JOIN");
    auto left=input(n.inputs[0]), right=input(n.inputs[1]);
    const auto width=left.node->outputs.size();
    if (n.outputs.size()!=width+right.node->outputs.size())
      fail(n.origin,"relational join output width changed",DiagnosticCode::InvalidQueryShape);
    std::string_view op;
    switch(n.join) {
    case JoinKind::Inner:op=" INNER JOIN ";break;
    case JoinKind::Left:op=" LEFT JOIN ";break;
    case JoinKind::Right:op=" RIGHT JOIN ";break;
    case JoinKind::Full:op=" FULL JOIN ";break;
    case JoinKind::Cross:op=" CROSS JOIN ";break;
    default:fail(n.origin,"relational SQL join kind is unsupported");
    }
    if (lateral) {
      if (n.correlationKeys) {
        if ((n.join!=JoinKind::Inner && n.join!=JoinKind::Left) || !correlationColumns(plan,n))
          fail(n.origin,"relational SQL requires a verified parameterized join key");
      } else {
        const auto &dependent=right.node->kind==RelKind::Project?unary(*right.node):*right.node;
        if ((n.join!=JoinKind::Cross && n.join!=JoinKind::Left) || !n.predicates.empty() ||
            dependent.kind!=RelKind::Subquery || dependent.query!=n.query || right.node->query!=n.query)
          fail(n.origin,"relational SQL LATERAL requires CROSS or LEFT and a derived right input");
        const auto &binding=relationalBinding(dependent);
        const auto &inner=modelQuery(plan,binding.query);
        if (!binding.name.empty() || inner.scope.parent!=n.query || !inner.scope.parentTables)
          fail(n.origin,"relational SQL LATERAL requires verified outer scope bindings");
      }
    } else if (n.predicates.empty()!=(n.join==JoinKind::Cross)) {
      fail(n.origin,"relational SQL join predicate shape is unsupported");
    }
    write("SELECT ");
    fields(n,needed,visible,[&](auto i) {
      auto &source=i<width?left:right;
      const auto ordinal=i<width?i:i-width;
      if (!sameColumn(n.outputs[i],source.node->outputs[ordinal]))
        fail(n.origin,"relational join output position changed",DiagnosticCode::InvalidQueryShape);
      column(source,ordinal);
    });
    const auto suffix=collectSuffix([&] {
      if (lateral) {
        write(op);
        write("LATERAL ");
        if (n.correlationKeys) {
          auto probe=input(n.inputs.back());
          write("(SELECT ");
          inputColumns(*probe.node,probe,right.columns,false);
          const auto where=collectSuffix([&] {
            write(" WHERE ");
            predicates(n,{&left,&probe});
          });
          write(" FROM ");
          from(n.inputs.back(),probe,depth);
          appendSuffix(where);
          write(") AS ");
          identifier(right.alias);
          write(" ON TRUE");
          return;
        }
        // Render the right input first to discover the outer columns it
        // needs; insert its SQL after the left input in the final statement.
        outerInputs.push_back(&left);
        from(n.inputs[1],right,depth);
        outerInputs.pop_back();
        if (n.join==JoinKind::Left) write(" ON TRUE");
        return;
      }
      if (n.predicates.empty()) return;
      write(" ON ");
      predicates(n,{&left,&right});
    });
    write(" FROM ");
    from(n.inputs[0],left,depth);
    if (!lateral) {
      write(op);
      from(n.inputs[1],right,depth);
    }
    appendSuffix(suffix);
  }
  void order(const RelNode &n,Input &source) {
    if (n.keys.empty() || n.keys.size()!=n.descending.size() || n.keys.size()!=n.nullOrdering.size())
      fail(n.origin,"relational SQL ordering metadata is incomplete",DiagnosticCode::InvalidQueryShape);
    write(" ORDER BY ");
    for (std::size_t i=0;i<n.keys.size();++i) {
      if (i) write(", ");
      expression(n.keys[i],{&source},ExpressionScope::Sort);
      write(n.descending[i]?" DESC":" ASC");
      if (n.nullOrdering[i]=="FIRST") write(" NULLS FIRST");
      else if (n.nullOrdering[i]=="LAST") write(" NULLS LAST");
      else if (!n.nullOrdering[i].empty())
        fail(n.origin,"invalid relational SQL NULL ordering",DiagnosticCode::InvalidQueryShape);
    }
  }
  void page(const RelNode &n) {
    const auto valid=[](double value) {
      return std::isfinite(value) && value>=0 && value<=9007199254740991. && std::floor(value)==value;
    };
    if (n.withTies || !n.rowOffset || !valid(*n.rowOffset) ||
        (!n.rowLimit && !n.keys.empty()) || (n.rowLimit && !valid(*n.rowLimit)))
      fail(n.origin,"relational SQL pagination requires literal LIMIT and OFFSET without ties");
    if (n.rowLimit) {
      write(" LIMIT ");
      write(std::to_string(static_cast<std::uint64_t>(*n.rowLimit)));
    }
    if (*n.rowOffset) {
      write(" OFFSET ");
      write(std::to_string(static_cast<std::uint64_t>(*n.rowOffset)));
    }
  }

  void rows(RelId id,const Demand &needed,bool visible,std::size_t depth) {
    const auto &n=node(id);
    visit(n.origin);
    struct RestoreDepth {
      std::size_t &current;
      std::size_t previous;
      ~RestoreDepth(){current=previous;}
    } restoreDepth{rowDepth,rowDepth};
    rowDepth=depth;
    if (needed.size()!=n.outputs.size())
      fail(n.origin,"relational SQL output demand has a different width",DiagnosticCode::InvalidQueryShape);
    if (depth>=std::min<std::size_t>(128,options.maxNesting))
      fail(n.origin,"relational SQL nesting limit exceeded",DiagnosticCode::ResourceLimit);
    if (visible && n.outputs.empty())
      fail(n.origin,"relational SQL needs explicit output columns");
    const auto &q=modelQuery(plan,n.query);
    if (q.kind!=StatementKind::Select || q.selectInto)
      fail(n.origin,"relational SQL backend only emits read-only SELECT plans");
    if (n.kind==RelKind::Scan || n.kind==RelKind::Values) {
      if (!n.inputs.empty()) fail(n.origin,"relational SQL leaf has an input",DiagnosticCode::InvalidQueryShape);
      if (n.kind==RelKind::Scan) scan(n,q,needed,visible);
      else values(n,needed,visible);
      return;
    }
    if (n.kind==RelKind::WorkingTableScan) {
      if (!n.inputs.empty())
        fail(n.origin,"invalid relational SQL working-table inputs",DiagnosticCode::InvalidQueryShape);
      workingTable(n,needed,visible);
      return;
    }
    if (n.kind==RelKind::Join || n.kind==RelKind::Correlate) {
      join(n,needed,visible,depth);
      return;
    }
    if (n.kind==RelKind::Unnest || n.kind==RelKind::Series) {
      rowExpansion(n,needed,visible,depth);
      return;
    }
    if (n.kind==RelKind::Unpivot) {
      unpivot(n,q,needed,visible,depth);
      return;
    }
    if (n.kind==RelKind::Pivot) {
      pivot(n,q,needed,visible,depth);
      return;
    }
    if (n.kind==RelKind::SetOperation) {
      setOperation(n,q,needed,visible,depth);
      return;
    }
    if (n.kind==RelKind::SemiJoin || n.kind==RelKind::AntiJoin) {
      membership(n,needed,visible,depth);
      return;
    }
    const auto &child=primary(n);
    if (n.kind==RelKind::Sample) {
      sameShape(n,child);
      if (plan.program.sourceDialect!=Dialect::PostgreSQL || !n.sampling ||
          n.sampling->milliPercent>TableSampling::full)
        fail(n.origin,"relational SQL requires a valid PostgreSQL sample");
      if (child.kind==RelKind::Scan && child.inputs.empty()) {
        scan(child,q,needed,visible,&*n.sampling);
        return;
      }
      if (n.sampling->milliPercent==TableSampling::full) {
        rows(n.inputs.front(),needed,visible,depth+1);
        return;
      }
      if (n.sampling->milliPercent!=0)
        fail(n.origin,"nontrivial sampling must retain its native scan");
      auto source=input(n.inputs.front());
      write("SELECT ");
      inputColumns(n,source,needed,visible);
      write(" FROM ");
      from(n.inputs.front(),source,depth);
      write(" WHERE FALSE");
      return;
    }
    if (n.kind==RelKind::Calc) {
      scalarCalc(n,needed,visible,depth);
      return;
    }
    if (n.kind==RelKind::Subquery || n.kind==RelKind::Rename) {
      positionalProjection(n,needed,visible,depth);
      return;
    }
    if (n.kind==RelKind::Coerce) {
      coerce(n,needed,visible,depth);
      return;
    }
    if (n.kind==RelKind::Aggregate && n.distinct) {
      distinctRows(n,needed,visible,depth);
      return;
    }
    if (n.kind==RelKind::Project) {
      project(n,q,needed,visible,depth);
      return;
    }
    if (n.kind!=RelKind::Sort && n.kind!=RelKind::Limit && n.kind!=RelKind::Filter)
      fail(n.origin,std::string("relational SQL backend does not support ")+relKindName(n.kind));
    sameShape(n,child);
    const auto *ordering=n.kind==RelKind::Sort?&n:nullptr;
    auto sourceId=n.inputs.front();
    if (n.kind==RelKind::Limit && child.kind==RelKind::Sort) {
      visit(child.origin);
      sameShape(child,unary(child));
      ordering=&child;
      sourceId=child.inputs.front();
    }
    auto source=input(sourceId);
    write("SELECT ");
    inputColumns(n,source,needed,visible);
    const auto suffix=collectSuffix([&] {
      if (n.kind==RelKind::Filter) {
        if (n.predicates.empty()) fail(n.origin,"relational SQL filter needs a predicate");
        write(" WHERE ");
        predicates(n,{&source});
      }
      if (ordering) order(*ordering,source);
      if (n.kind==RelKind::Limit) page(n);
    });
    write(" FROM ");
    from(sourceId,source,depth);
    appendSuffix(suffix);
  }

public:
  PlanRenderer(const LogicalPlan &p,const GenerationOptions &limits):plan(p),options(limits) {}
  std::string run(RelId root) {
    if (plan.program.sourceDialect!=Dialect::PostgreSQL)
      fail({},"relational SQL backend currently requires PostgreSQL");
    // An unused data-modifying CTE is still executed by PostgreSQL. A
    // read-only emitter must reject one even when it is outside the root DAG.
    for (const auto &query:plan.program.queries) {
      visit(query.origin);
      if (query.kind!=StatementKind::Select || query.selectInto)
        fail(query.origin,"relational SQL backend only emits read-only SELECT plans");
    }
    std::vector<bool> done(plan.nodes.size()),active(plan.nodes.size());
    discoverCtes(root,done,active,0);
    emitCtes();
    rows(root,Demand(node(root).outputs.size(),true),true,0);
    write(";");
    return std::move(sql);
  }
};
} // namespace

GeneratedSql generatePlanSql(const LogicalPlan &plan,RelId root,GenerationOptions options) {
  try {
    return {PlanRenderer(plan,options).run(root),{}};
  } catch (const RenderFailure &error) {
    return {{},{error.diagnostic}};
  } catch (const std::bad_alloc &) {
    return {{},{{DiagnosticCode::ResourceLimit,{},"relational SQL allocation failed"}}};
  } catch (const std::exception &error) {
    return {{},{{DiagnosticCode::InvalidQueryShape,{},error.what()}}};
  }
}
} // namespace navidog::sql::optimizer
