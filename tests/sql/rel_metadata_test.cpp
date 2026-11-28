#include "navidog/sql/optimizer/rel_metadata.hpp"
#include "../../src/sql/optimizer/optimizer_internal.hpp"
#include <algorithm>
#include <iostream>
#include <stdexcept>
using namespace navidog::sql;
namespace c=compiler;namespace o=optimizer;
void check(bool b,const char* m){if(!b)throw std::runtime_error(m);}
c::ModelProgram parse(const std::string &s){auto p=c::compile(c::Dialect::PostgreSQL,s);if(!p->ok())throw std::runtime_error(p->diagnostics.front().message);return p->model;}
o::RelId append(o::LogicalPlan &p,o::RelNode n){p.nodes.push_back(std::move(n));return p.root=o::RelId(p.nodes.size()-1);}
o::RelId hep(o::OptimizationResult &r,o::PlannerOptions settings={}) {
  o::PlannerContext context{r,nullptr,settings,std::chrono::steady_clock::now()+std::chrono::seconds(5)};
  return o::runHep(context);
}
int main(){try{
  auto fired=[](const o::OptimizationResult &result,const char *name){return std::any_of(result.rules.begin(),result.rules.end(),[&](const auto &event){return event.rule==name;});};
  {
    c::MemoryCatalog catalog;
    c::TableDefinition orders;
    orders.database="owned";orders.schema="OWNED";orders.name="SALES";
    orders.columns={{"REGION_ID",c::ValueType::Integer},{"AMOUNT",c::ValueType::Decimal},
                    {"SALES_YEAR",c::ValueType::Integer}};
    catalog.tables.push_back(orders);
    o::MemoryStatistics statistics;
    o::TableStatistics sales;
    sales.database=orders.database;sales.schema=orders.schema;sales.name=orders.name;sales.rows=100;
    sales.columns={{"REGION_ID",5.,.2,{}}};
    statistics.tables.push_back(sales);
    const auto dialect=c::Dialect::Oracle;
    const c::NameResolutionContext names{orders.database,orders.schema,dialect};
    const auto parsePivot=[&](const std::string &sql) {
      auto parsed=c::compile(dialect,sql,{},&catalog,names);
      check(parsed->ok(),"Oracle PIVOT metadata fixture did not compile");
      auto plan=o::optimize(parsed->model,&statistics);
      check(plan.ok(),"Oracle PIVOT was not planned");
      return plan;
    };
    const auto pivotId=[](const o::OptimizationResult &plan) {
      const auto found=std::find_if(plan.logical.nodes.begin(),plan.logical.nodes.end(),
                                    [](const auto &node) { return node.kind==o::RelKind::Pivot; });
      check(found!=plan.logical.nodes.end(),"PIVOT did not get a relational node");
      return o::RelId(found-plan.logical.nodes.begin());
    };
    const auto grouped=parsePivot("SELECT p.* FROM SALES PIVOT(SUM(amount) FOR sales_year "
                                  "IN (2025 AS earlier,2026 AS later)) p");
    const auto groupedId=pivotId(grouped);
    o::RelMetadataQuery facts(grouped.logical);
    const auto *properties=facts.properties(groupedId);
    const auto *retained=facts.columnOrigins(groupedId,0),*bucket=facts.columnOrigins(groupedId,1);
    check(properties && properties->rows.minimum==0 && !properties->rows.maximum && properties->uniqueKeys.empty() &&
          retained && retained->complete && retained->origins.front().column=="REGION_ID" && bucket && bucket->complete &&
          bucket->origins.size()==2 && std::all_of(bucket->origins.begin(),bucket->origins.end(),
              [](const auto &column) { return column.derived; }),"PIVOT hard bounds or axis/measure origins are incorrect");
    const auto physical=std::find_if(grouped.best.nodes.begin(),grouped.best.nodes.end(),
                                     [](const auto &node) { return node.kind==o::PhysicalKind::Pivot; });
    check(physical!=grouped.best.nodes.end() && physical->rows==6 && !physical->assumedStatistics,
          "PIVOT did not estimate an extra NULL group separately");
    const auto global=parsePivot("SELECT * FROM (SELECT amount,sales_year FROM SALES WHERE 1=0) "
                                 "PIVOT(COUNT(amount) FOR sales_year IN (2026 AS total))");
    o::RelMetadataQuery globalMetadata(global.logical);
    const auto *globalProps=globalMetadata.properties(pivotId(global));
    check(globalProps && globalProps->rows.minimum==1 && globalProps->rows.maximum==1 &&
          globalProps->nonNullColumns.size()==1,"Oracle empty-input ungrouped PIVOT COUNT lost its result row");
    const auto ms=c::compile(c::Dialect::SQLServer,"SELECT p.* FROM (VALUES (1,2,2025),(1,3,2026)) "
                    "s(region_id,amount,sales_year) PIVOT(SUM(amount) FOR sales_year IN ([2025],[2026])) p");
    check(ms->ok(),"SQL Server PIVOT metadata fixture did not compile");
    const auto msPlan=o::optimize(ms->model);
    check(msPlan.ok(),"SQL Server PIVOT was not planned");
    o::RelMetadataQuery msFacts(msPlan.logical);
    const auto *msProps=msFacts.properties(pivotId(msPlan));
    check(msProps && msProps->rows.minimum==1 && msProps->rows.maximum==2,"PIVOT VALUES hard bounds are invalid");
  }
  {
    c::MemoryCatalog catalog;
    c::TableDefinition table;
    table.database="owned";table.schema="OWNED";table.name="SALES";
    table.columns={{"ID",c::ValueType::Integer,nullptr,true},{"Q1",c::ValueType::Integer,nullptr,true},
                   {"Q2",c::ValueType::Integer}};
    table.primaryKeyKnown=true;table.primaryKey={"ID"};
    catalog.tables.push_back(table);
    o::MemoryStatistics statistics;
    o::TableStatistics sales;
    sales.database=table.database;sales.schema=table.schema;sales.name=table.name;sales.rows=100;
    sales.columns={{"Q2",12.,.75,{}}};
    statistics.tables.push_back(sales);
    const auto dialect=c::Dialect::Oracle;
    for (const bool include:{false,true}) {
      const std::string sql=std::string("SELECT DISTINCT u.id,u.quarter,u.total FROM SALES UNPIVOT ")+
          (include?"INCLUDE NULLS ":"EXCLUDE NULLS ")+"(total FOR quarter IN (Q1 AS 'first',Q2 AS 'second')) u";
      const auto parsed=c::compile(dialect,sql,{},&catalog,{table.database,table.schema,dialect});
      check(parsed->ok(),"Oracle UNPIVOT metadata fixture did not compile");
      const auto planned=o::optimize(parsed->model,&statistics);
      check(planned.ok(),"Oracle UNPIVOT was not planned");
      const auto node=std::find_if(planned.logical.nodes.begin(),planned.logical.nodes.end(),
                                   [](const auto &n) { return n.kind==o::RelKind::Unpivot; });
      check(node!=planned.logical.nodes.end() && node->outputs.size()==3 &&
            planned.logical.nodes.at(node->inputs.front()).outputs.at(1).name=="Q1", "UNPIVOT input and output were collapsed");
      o::RelMetadataQuery metadata(planned.logical);
      const auto id=o::RelId(node-planned.logical.nodes.begin());
      const auto *properties=metadata.properties(id);
      check(properties && properties->rows.minimum==0 && !properties->rows.maximum &&
            properties->uniqueKeys.empty(),"UNPIVOT advertised a statistical hard bound or an unverified key");
      check(properties->nonNullColumns.size()==(include?2:3),"UNPIVOT measure nullability incorrect");
      const auto *key=metadata.columnOrigins(id,0),*label=metadata.columnOrigins(id,1),*measure=metadata.columnOrigins(id,2);
      check(key && key->complete && key->origins.size()==1 && key->origins.front().column=="ID" &&
            !key->origins.front().derived && label && label->complete && label->origins.empty() &&
            measure && measure->complete && measure->origins.size()==2 &&
            std::all_of(measure->origins.begin(),measure->origins.end(),[](const auto &column) { return column.derived; }),
            "UNPIVOT lineage lost retained fields, label or measure sources");
      const auto physical=std::find_if(planned.best.nodes.begin(),planned.best.nodes.end(),
                                       [](const auto &n) { return n.kind==o::PhysicalKind::Unpivot; });
      check(physical!=planned.best.nodes.end() && physical->rows==(include?200:125) && !physical->assumedStatistics,
            "UNPIVOT cost did not account for measured NULL fractions");
    }
    const auto values=c::compile(c::Dialect::SQLServer,"SELECT u.* FROM (VALUES (1,2,3),(2,4,CAST(NULL AS INT))) "
                                                        "s(id,q1,q2) UNPIVOT(total FOR label IN (q1,q2)) u");
    check(values->ok(),"SQL Server UNPIVOT metadata fixture did not compile");
    const auto planned=o::optimize(values->model);
    check(planned.ok(),"SQL Server UNPIVOT over VALUES was not planned");
    const auto unpivot=std::find_if(planned.logical.nodes.begin(),planned.logical.nodes.end(),
                                   [](const auto &n) { return n.kind==o::RelKind::Unpivot; });
    check(unpivot!=planned.logical.nodes.end(),"missing SQL Server UNPIVOT operator");
    o::RelMetadataQuery metadata(planned.logical);
    const auto *bounds=metadata.properties(o::RelId(unpivot-planned.logical.nodes.begin()));
    check(bounds && bounds->rows.maximum==4 && bounds->rows.minimum<=3,"UNPIVOT VALUES output bound ignored expansion");
    auto invalid=planned.logical;
    const auto id=o::RelId(unpivot-planned.logical.nodes.begin());
    invalid.nodes[id].outputs.pop_back();
    o::RelMetadataQuery broken(invalid);
    check(!broken.properties(id) && broken.status()==o::MetadataStatus::InvalidGraph,
          "invalid UNPIVOT row mapping was accepted");

    catalog.tables.front().columns.push_back({"V1",c::ValueType::Integer});
    catalog.tables.front().columns.push_back({"V2",c::ValueType::Integer});
    statistics.tables.front().columns.push_back({"V2",15.,.5,{}});
    const auto multi=c::compile(dialect,"SELECT u.* FROM sales UNPIVOT "
        "((total,units) FOR (quarter,yr) IN ((q1,v1) AS ('one',2026),(q2,v2) AS ('two',2027))) u",
        {},&catalog,{table.database,table.schema,dialect});
    check(multi->ok(),"Oracle multi-measure UNPIVOT metadata fixture did not compile");
    const auto multiPlan=o::optimize(multi->model,&statistics);
    check(multiPlan.ok(),"Oracle multi-measure UNPIVOT was not planned");
    const auto multiNode=std::find_if(multiPlan.logical.nodes.begin(),multiPlan.logical.nodes.end(),
                                     [](const auto &n) { return n.kind==o::RelKind::Unpivot; });
    check(multiNode!=multiPlan.logical.nodes.end() && multiNode->outputs.size()==5,
          "Oracle multi-measure UNPIVOT lost its row shape");
    o::RelMetadataQuery multiMetadata(multiPlan.logical);
    const auto multiId=o::RelId(multiNode-multiPlan.logical.nodes.begin());
    const auto *multiProps=multiMetadata.properties(multiId);
    const auto *units=multiMetadata.columnOrigins(multiId,4);
    check(multiProps && multiProps->nonNullColumns.size()==3 && units && units->complete && units->origins.size()==2 &&
          units->origins.front().column=="V1" && units->origins.back().column=="V2",
          "multi-measure NULL filtering or per-position column lineage is incorrect");
    const auto multiEstimate=std::find_if(multiPlan.best.nodes.begin(),multiPlan.best.nodes.end(),
                                         [](const auto &n) { return n.kind==o::PhysicalKind::Unpivot; });
    check(multiEstimate!=multiPlan.best.nodes.end() && multiEstimate->rows==162.5 && multiEstimate->assumedStatistics,
          "multi-measure UNPIVOT did not identify unknown NULL correlation in its cost estimate");
  }
  {
    o::OptimizationResult result;
    const o::PlannerOptions options;
    o::PlannerContext context{result,nullptr,options,std::chrono::steady_clock::now()+std::chrono::seconds(5)};
    const std::vector<o::JoinSearchInput> inputs(4,{10.,8.});
    const std::vector<o::JoinSearchTerm> terms{{{},3,.01},{{},12,.01},{{},6,.5}};
    const auto bushy=o::searchJoinOrder(context,inputs,terms,false);
    const auto leftDeep=o::searchJoinOrder(context,inputs,terms,true);
    check(bushy.left.size()==16 && (bushy.left[15]==3 || bushy.left[15]==12),
          "join search did not retain two independently selective subtrees");
    const auto last=15^leftDeep.left.at(15);
    check(last && !(last&(last-1)),"left-deep join search returned a non-leaf right input");
  }
  o::OptimizationResult r;r.logical.program=parse("SELECT 1 WHERE 1=1;");r.logical.nodes.resize(1);
  o::RelNode values;values.kind=o::RelKind::Values;values.query=1;values.valueCount=20;
  auto v=append(r.logical,values);
  o::RelNode limit;limit.kind=o::RelKind::Limit;limit.query=1;limit.inputs={v};limit.rowLimit=8;limit.rowOffset=3;
  auto inner=append(r.logical,limit);limit.inputs={inner};limit.rowLimit=10;limit.rowOffset=6;
  auto outer=append(r.logical,limit);
  o::RelMetadataQuery metadata(r.logical);
  auto p=metadata.properties(outer);check(p && p->rows.minimum==2 && p->rows.maximum==2,"nested limit hard bound");
  auto normalized=hep(r);const auto &n=r.logical.nodes.at(normalized);
  check(n.inputs==std::vector<o::RelId>{v} && n.rowLimit==2 && n.rowOffset==9,"Hep limit merge");
  check(r.logical.nodes.at(outer).inputs==std::vector<o::RelId>{inner},"Hep mutated source DAG");
  check(std::any_of(r.rules.begin(),r.rules.end(),[](const auto&e){return e.heuristic && e.rule=="HepLimitMerge";}),"Hep rule evidence");
  // Cached values remain valid after append; statistics never create hard bounds.
  check(p->rows.maximum==2,"appending nodes invalidated metadata pointer");
  auto original=r.logical;original.nodes[inner].inputs={outer};
  o::RelMetadataQuery cyclic(original);check(!cyclic.properties(outer) && cyclic.status()==o::MetadataStatus::InvalidGraph,"cyclic graph accepted");
  o::MetadataLimits tiny;tiny.maxBytes=1;o::RelMetadataQuery bytes(r.logical,tiny);
  check(!bytes.properties(v) && bytes.status()==o::MetadataStatus::ResourceLimit,"metadata byte limit");
  o::MetadataLimits cancelled;cancelled.shouldStop=[]{return true;};o::RelMetadataQuery stop(r.logical,cancelled);
  check(!stop.properties(v) && stop.status()==o::MetadataStatus::Stopped,"metadata cancellation");
  original=r.logical;original.nodes[outer].inputs.clear();o::RelMetadataQuery arity(original);
  check(!arity.properties(outer) && arity.status()==o::MetadataStatus::InvalidGraph,"invalid input arity");
  o::RelNode agg;agg.kind=o::RelKind::Aggregate;agg.query=1;agg.inputs={v};
  auto single=append(r.logical,agg);p=metadata.properties(single);check(p && p->rows.minimum==1 && p->rows.maximum==1,"global aggregate bound");
  o::RelNode distinct=agg;distinct.distinct=true;distinct.inputs={single};auto d=append(r.logical,distinct);
  check(hep(r)==single,"distinct single row elimination");
  // PK lineage is a proof; row estimates / statistical unique indexes are not.
  c::MemoryCatalog catalog;c::TableDefinition table;table.name="owned_key";table.columns={{"id",c::ValueType::Integer},{"n",c::ValueType::Integer}};table.primaryKeyKnown=true;table.primaryKey={"id"};table.noDescendantsKnown=true;catalog.tables.push_back(table);
  auto bound=c::compile(c::Dialect::PostgreSQL,"SELECT DISTINCT id AS renamed FROM owned_key;",{},&catalog);check(bound->ok(),"key fixture compilation");auto known=bound->model;
  auto key=o::optimize(known);check(key.ok(),"known key query optimization");
  check(std::any_of(key.rules.begin(),key.rules.end(),[](const auto&e){return e.rule=="HepDistinctRemove";}),"primary key lineage lost through alias projection");
  {
    c::SqlType integer;integer.family=c::TypeFamily::Integer;integer.bits=32;integer.unsignedInteger=false;
    auto typed=catalog;
    for (auto &column:typed.tables.front().columns) column.sqlType=std::make_shared<const c::SqlType>(integer);
    const auto compiled=c::compile(c::Dialect::PostgreSQL,
        "SELECT n AS answer,id AS repeated,id FROM owned_key WHERE id>1 AND n IS NOT NULL;",{},&typed);
    check(compiled->ok(),"Calc test didn't compile");
    auto planned=o::optimize(compiled->model);
    const auto fused=std::find_if(planned.rules.begin(),planned.rules.end(),[](const auto &entry) {
      return entry.rule=="ProjectCalcMerge";
    });
    check(planned.ok() && fused!=planned.rules.end(),"Memo did not combine scalar expressions");
    const auto original=planned.logical.nodes.at(fused->after);
    check(original.kind==o::RelKind::Calc && original.keys.size()==3 && original.predicates.size()==2,
          "combined Calc lost output expressions or predicates");
    planned.logical.root=fused->after;
    o::PlannerOptions split;
    split.heuristic.rules={o::HepRule::CalcSplit};
    const auto converted=hep(planned,split);
    const auto &project=planned.logical.nodes.at(converted);
    const auto &filter=planned.logical.nodes.at(project.inputs.front());
    check(project.kind==o::RelKind::Project && filter.kind==o::RelKind::Filter &&
          filter.inputs==original.inputs && project.keys.size()==original.keys.size() &&
          filter.predicates.size()==original.predicates.size(),
          "Calc split changed the original scalar program");
    planned.logical.root=converted;
    o::PlannerOptions merge;
    merge.heuristic.rules={o::HepRule::CalcMerge};
    const auto combined=hep(planned,merge);
    check(planned.logical.nodes.at(combined).kind==o::RelKind::Calc,"Hep did not merge the scalar pipeline");
    planned.logical.root=combined;
    const auto events=planned.rules.size();
    auto phased=merge;
    phased.heuristic.substitutions={o::HepRule::CalcSplit};
    const auto normalized=hep(planned,phased);
    check(planned.logical.nodes.at(normalized).kind==o::RelKind::Calc &&
          planned.rules.size()==events+2 &&
          planned.rules[events].rule=="HepCalcSplit" && planned.rules[events].substitution &&
          planned.rules[events+1].rule=="HepCalcMerge" && !planned.rules[events+1].substitution,
          "Hep did not run substitutions to a fixed point before the main rules");
    o::RelNode trivial;
    trivial.kind=o::RelKind::Calc;
    trivial.query=original.query;
    trivial.inputs=original.inputs;
    trivial.outputs=planned.logical.nodes.at(original.inputs.front()).outputs;
    append(planned.logical,std::move(trivial));
    o::PlannerOptions remove;
    remove.heuristic.rules={o::HepRule::CalcRemove};
    check(hep(planned,remove)==original.inputs.front(),"identity Calc was not eliminated");
  }
  {
    auto typed = catalog;
    c::SqlType integer;
    integer.family = c::TypeFamily::Integer;
    integer.bits = 32;
    integer.unsignedInteger = false;
    for (auto &column : typed.tables.front().columns)
      column.sqlType = std::make_shared<const c::SqlType>(integer);
    auto repeated = typed.tables.front();
    repeated.name = "owned_repeat";
    repeated.primaryKey.clear();
    repeated.primaryKeyKnown = false;
    typed.tables.push_back(repeated);
    const auto checkSet = [&](const std::string &sql, bool unique, bool nonNull) {
      auto compiled = c::compile(c::Dialect::PostgreSQL, sql, {}, &typed);
      check(compiled->ok(), "set key fixture compilation failed");
      auto plan = o::optimize(compiled->model);
      check(plan.ok(), "set key fixture optimization failed");
      o::RelMetadataQuery metadata(plan.logical);
      const auto *facts = metadata.properties(plan.logical.root);
      const auto &outputs = plan.logical.nodes.at(plan.logical.root).outputs;
      check(facts && outputs.size() == 2, "missing set result metadata");
      check((!facts->uniqueKeys.empty()) == unique, "unexpected set key proof");
      if (unique)
        check(facts->uniqueKeys.front().size() == 1 && facts->uniqueKeys.front().front().name == "k",
              "set key failed to remap to the result name");
      check((!facts->nonNullColumns.empty()) == nonNull, "unexpected set non-null proof");
      if (nonNull)
        check(facts->nonNullColumns.size() == 1 && facts->nonNullColumns.front().name == "k",
              "set non-null proof failed to remap to the result name");
    };
    for (const auto &[relation, unique] : std::vector<std::pair<std::string,bool>>{
          {"owned_key",true},{"owned_repeat",false}}) {
      const auto sql="SELECT DISTINCT subt.k FROM (SELECT id AS k,COUNT(*) AS grouped_count FROM " + relation +
                     " GROUP BY k,n) subt;";
      const auto bound=c::compile(c::Dialect::PostgreSQL,sql,{},&typed);
      check(bound->ok(), "grouped key proof SQL failed to bind");
      o::PlannerOptions opts;
      opts.heuristic.rules = {o::HepRule::DistinctRemove};
      auto grouped = o::optimize(bound->model,nullptr,opts);
      check(grouped.ok() && fired(grouped,"HepDistinctRemove")==unique,
            "GROUP BY did not preserve exactly the available primary key");
    }
    for (const auto &[definition, unique] : std::vector<std::pair<std::string,bool>>{
          {"SELECT n,MIN(id) AS edge FROM owned_key GROUP BY n", true},
          {"SELECT n,MAX(id) AS edge FROM owned_key GROUP BY n HAVING COUNT(*)>0", true},
          {"SELECT n,MIN(id) AS edge FROM owned_repeat GROUP BY n", false},
          {"SELECT id,MAX(n) AS edge FROM owned_key GROUP BY id", false},
          {"SELECT n,MIN(id) AS edge FROM owned_key GROUP BY GROUPING SETS ((n),())", false}}) {
      const auto sql = "SELECT DISTINCT q.edge FROM (" + definition + ") q;";
      const auto bound = c::compile(c::Dialect::PostgreSQL,sql,{},&typed);
      check(bound->ok(), "extremum key proof SQL failed to bind");
      o::PlannerOptions opts;
      opts.heuristic.rules = {o::HepRule::DistinctRemove};
      const auto planned = o::optimize(bound->model,nullptr,opts);
      check(planned.ok() && fired(planned,"HepDistinctRemove")==unique,
            "MIN/MAX key proof ignored the input key or the grouping boundary");
    }
    auto composite = typed.tables.front();
    composite.name = "owned_composite_key";
    composite.primaryKey = {"id","n"};
    typed.tables.push_back(composite);
    for (const auto &[sql, one] : std::vector<std::pair<std::string,bool>>{
          {"SELECT DISTINCT n FROM owned_key WHERE id=7",true},
          {"SELECT DISTINCT n FROM owned_key WHERE 7 IS NOT DISTINCT FROM id",true},
          {"SELECT DISTINCT n FROM owned_key WHERE id IS DISTINCT FROM 7",false},
          {"SELECT DISTINCT n FROM owned_key WHERE 7=id AND n>0",true},
          {"SELECT DISTINCT n FROM owned_key WHERE id=7 OR id=8",false},
          {"SELECT DISTINCT n FROM owned_key WHERE (id=7 AND n>0) OR (07=id AND n<3)",true},
          {"SELECT DISTINCT n FROM owned_key WHERE (id=7 AND id=8) OR id=9",true},
          {"SELECT DISTINCT n FROM owned_key WHERE id>=7 AND 8>id",true},
          {"SELECT DISTINCT n FROM owned_key WHERE id BETWEEN -7 AND -7",true},
          {"SELECT DISTINCT n FROM owned_key WHERE id BETWEEN SYMMETRIC 7 AND 8",false},
          {"SELECT DISTINCT n FROM owned_key WHERE id>=7 AND id<=8",false},
          {"SELECT DISTINCT n FROM owned_key WHERE id<=-2147483648",true},
          {"SELECT DISTINCT n FROM owned_key WHERE (id>6 AND id<8) OR (id BETWEEN 7 AND 7)",true},
          {"SELECT DISTINCT n FROM owned_repeat WHERE id=7",false},
          {"SELECT DISTINCT n FROM owned_composite_key WHERE id=7",false},
          {"SELECT DISTINCT n FROM owned_composite_key WHERE id=7 AND n=9",true},
          {"SELECT DISTINCT n FROM owned_composite_key WHERE id>=7 AND 8>id AND n BETWEEN 2 AND 2",true}}) {
      const auto sqlModel=c::compile(c::Dialect::PostgreSQL,sql,{},&typed);
      check(sqlModel->ok(), "fixed key proof SQL failed to bind");
      const auto planned=o::optimize(sqlModel->model);
      o::RelMetadataQuery metadata(planned.logical);
      const auto *bounds=metadata.properties(planned.logical.root);
      check(planned.ok() && bounds && (bounds->rows.maximum==1)==one &&
            fired(planned,"HepDistinctRemove")==one, "equality did not bound exactly one complete unique key");
    }
    for (const auto &[clause,maximum]:std::vector<std::pair<std::string,std::optional<std::uint64_t>>>{
          {"owned_key WHERE id BETWEEN -3 AND 5",9},
          {"owned_key WHERE id BETWEEN 1 AND 2 OR id=4",3},
          {"owned_key WHERE id IN (1,5,5,10,NULL)",3},
          {"owned_key WHERE id = ANY(ARRAY[1,5,5,10,NULL])",3},
          {"owned_key WHERE id = SOME(ARRAY[[1,5],[NULL,10]]) AND id BETWEEN 2 AND 7",1},
          {"owned_key WHERE id <> ALL(ARRAY[1,3]) AND id BETWEEN 1 AND 4",2},
          {"owned_key WHERE id > SOME(ARRAY[1,3,NULL]) AND id<4",2},
          {"owned_key WHERE id >= ALL(ARRAY[1,3,3]) AND id<=7",5},
          {"owned_key WHERE id < ALL(ARRAY[1,3]) AND id>=-2",3},
          {"owned_key WHERE id = ALL(ARRAY[3,3])",1},
          {"owned_key WHERE id <> ANY(ARRAY[3,3,NULL]) AND id BETWEEN 1 AND 5",4},
          {"owned_key WHERE id <> ANY(ARRAY[3,4,NULL]) AND id BETWEEN 1 AND 5",5},
          {"owned_repeat WHERE id <> ALL(ARRAY[]::int[]) AND id IS NULL",{}},
          {"owned_key WHERE id IN (1,5,10) AND id BETWEEN 2 AND 7",1},
          {"owned_key WHERE id NOT IN (1,3) AND id BETWEEN 1 AND 4",2},
          {"owned_key WHERE id NOT IN (1,NULL) OR id=7",1},
          {"owned_key WHERE id BETWEEN -7 AND -3 OR id BETWEEN -5 AND 3",11},
          {"owned_key WHERE id IN (1,5,ABS(n))",{}},
          {"owned_composite_key WHERE id BETWEEN 2 AND 4 AND n BETWEEN -5 AND 2",24},
          {"owned_composite_key WHERE id BETWEEN 2 AND 4",{}},
          {"owned_repeat WHERE id BETWEEN 1 AND 2",{}}}) {
      const auto bound=c::compile(c::Dialect::PostgreSQL,"SELECT n FROM "+clause,{},&typed);
      check(bound->ok(),"finite key range did not bind");
      const auto planned=o::optimize(bound->model);
      o::RelMetadataQuery metadata(planned.logical);
      const auto *result=metadata.properties(planned.logical.root);
      check(planned.ok() && result && result->rows.maximum==maximum,"incorrect finite unique-key range upper bound");
      if (maximum)
        check(planned.best.nodes.at(planned.best.root).rows<=*maximum,"Memo cost exceeds the hard interval bound");
    }
    for (const auto *predicate:{"id=7 AND (id=8 OR (id=9 AND id=10))",
                                "id>7 AND id<8","id< -2147483648","id BETWEEN 9 AND 8",
                                "id IN (1,5) AND id BETWEEN 2 AND 4","id NOT IN (1,NULL)",
                                "id = ANY(ARRAY[]::int[])","id = ANY(NULL::int[])",
                                "id <> ALL(ARRAY[1,NULL])","id = ANY(ARRAY[1,5]) AND id BETWEEN 2 AND 4",
                                "id >= ALL(ARRAY[1,NULL])","id = ALL(ARRAY[1,9])",
                                "id < ANY(ARRAY[-9223372036854775808]::bigint[])",
                                "id>9223372036854775807","id < -9223372036854775808",
                                "id IS NULL","id IS NOT DISTINCT FROM NULL",
                                "n IS NOT DISTINCT FROM NULL AND NULL IS DISTINCT FROM n",
                                "n IS NULL AND n>0","n IS NULL AND n IS NOT NULL",
                                "(n IS NULL OR id IS NULL) AND n>0"}) {
      const auto query=std::string("SELECT n FROM owned_key WHERE ")+predicate;
      const auto contradictions=c::compile(c::Dialect::PostgreSQL,query,{},&typed);
      check(contradictions->ok(),"contradiction proof fixture did not compile");
      const auto empty=o::optimize(contradictions->model);
      o::RelMetadataQuery emptyMetadata(empty.logical);
      const auto *emptyBounds=emptyMetadata.properties(empty.logical.root);
      check(empty.ok() && emptyBounds && emptyBounds->rows.maximum==0,
            ("contradictory comparisons did not prove an empty filter: "+query).c_str());
    }
    {
      std::string memberList="SELECT id FROM owned_key WHERE id IN (";
      for (unsigned i=0;i<65;++i) {
        if (i) memberList+=',';
        memberList+=std::to_string(i*100);
      }
      const auto compiled=c::compile(c::Dialect::PostgreSQL,memberList+")",{},&typed);
      check(compiled->ok(),"bounded search ranges fixture did not bind");
      const auto planned=o::optimize(compiled->model);
      o::RelMetadataQuery metadata(planned.logical);
      const auto *bounds=metadata.properties(planned.logical.root);
      check(planned.ok() && bounds && bounds->rows.maximum==6401,
            "range budget fallback failed to preserve a sound enclosing interval");
    }
    for (const auto &[sql,maximum]:std::vector<std::pair<std::string,std::optional<std::uint64_t>>>{
          {"SELECT c.k FROM (SELECT id FROM owned_key WHERE id IN (1,9)) AS c(k) WHERE c.k BETWEEN 2 AND 8",0},
          {"SELECT k FROM (SELECT id AS k FROM owned_key WHERE id IN (1,5,19) ORDER BY 1 LIMIT 10) c WHERE k>2",2},
          {"WITH s(k) AS MATERIALIZED (SELECT id FROM owned_key WHERE id BETWEEN 1 AND 3) SELECT k FROM s WHERE k>10",0},
          {"SELECT a,b FROM (SELECT id AS a,n AS b FROM owned_composite_key WHERE id BETWEEN 1 AND 3) c WHERE b IN (1,50)",6},
          {"SELECT k FROM (SELECT id+1 AS k FROM owned_key WHERE id IN (1,9)) c WHERE k IN (2,10)",2},
          {"SELECT a.id FROM owned_key a LEFT JOIN (SELECT id FROM owned_repeat WHERE id=1) b ON a.id=b.id WHERE b.id IS NULL",{}},
          {"SELECT b.id FROM (SELECT id FROM owned_key WHERE id IN (1,9)) a "
           "JOIN owned_repeat b ON a.id=b.id WHERE b.id BETWEEN 2 AND 8",0},
          {"SELECT a.id FROM owned_key a JOIN owned_repeat b ON a.n=b.n WHERE b.n IS NULL",0},
          {"SELECT a.id FROM owned_key a JOIN owned_repeat b ON a.n IS NOT DISTINCT FROM b.n WHERE b.n IS NULL",{}},
          {"SELECT a.id FROM (SELECT id FROM owned_key WHERE id IN (1,9)) a JOIN "
           "(SELECT id FROM owned_repeat WHERE id BETWEEN 2 AND 8) b ON a.id IS NOT DISTINCT FROM b.id",0},
          {"SELECT a.id FROM (SELECT id FROM owned_key WHERE id IN (1,9)) a "
           "LEFT JOIN owned_repeat b ON a.id=b.id WHERE a.id BETWEEN 2 AND 8",0},
          {"SELECT a.id FROM (SELECT id FROM owned_key WHERE id IN (1,9)) a "
           "LEFT JOIN owned_repeat b ON a.id=b.id WHERE b.id IS NULL",{}},
          {"SELECT a.id FROM owned_repeat b RIGHT JOIN (SELECT id FROM owned_key WHERE id IN (1,9)) a "
           "ON a.id=b.id WHERE a.id BETWEEN 2 AND 8",0},
          {"SELECT k FROM (SELECT id AS k FROM owned_key WHERE id IN (1,9) "
           "UNION ALL SELECT id FROM owned_repeat WHERE id=20) c WHERE k BETWEEN 2 AND 8",0},
          {"SELECT k FROM (SELECT id AS k FROM owned_key WHERE id IN (1,9) "
           "UNION SELECT id::bigint FROM owned_repeat WHERE id=20) c WHERE k BETWEEN 2 AND 8",0},
          {"SELECT id AS k FROM owned_key WHERE id IN (1,9) "
           "INTERSECT ALL SELECT id FROM owned_repeat WHERE id IN (2,8)",0},
          {"SELECT k FROM (SELECT id AS k FROM owned_key WHERE id IN (1,9) "
           "INTERSECT ALL SELECT id FROM owned_repeat WHERE id BETWEEN 3 AND 10) c WHERE k=1",0},
          {"SELECT k FROM (SELECT id AS k FROM owned_key WHERE id IN (1,9) "
           "EXCEPT ALL SELECT id FROM owned_repeat) c WHERE k BETWEEN 2 AND 8",0},
          {"SELECT k FROM (SELECT id AS k FROM owned_key WHERE id=5 "
           "UNION ALL SELECT id FROM owned_repeat WHERE id IS NULL) c WHERE k IS NULL",{}},
          {"SELECT k FROM (SELECT id AS k FROM owned_key WHERE id=5 UNION ALL SELECT id FROM owned_repeat) c WHERE k=10",{}}}) {
      const auto compiled=c::compile(c::Dialect::PostgreSQL,sql,{},&typed);
      check(compiled->ok(),"projected predicates fixture did not bind");
      const auto planned=o::optimize(compiled->model);
      o::RelMetadataQuery metadata(planned.logical);
      const auto *bounds=metadata.properties(planned.logical.root);
      check(planned.ok() && bounds && bounds->rows.maximum==maximum,
            ("wrong row bound after projecting predicate domains: "+sql).c_str());
    }
    {
      const auto bound=c::compile(c::Dialect::PostgreSQL,"SELECT id,n FROM owned_key",{},&typed);
      check(bound->ok(),"rename facts fixture did not bind");
      auto planned=o::optimize(bound->model);
      check(planned.ok(),"rename facts fixture did not plan");
      auto &graph=planned.logical;
      o::RelNode rename;
      rename.kind=o::RelKind::Rename;
      rename.inputs={graph.root};
      rename.query=graph.nodes[graph.root].query;
      rename.outputs=graph.nodes[graph.root].outputs;
      rename.outputs[0].name="k";
      rename.outputs[1].name="amount";
      const auto uniqueAlias=append(graph,rename);
      rename.outputs[1]=rename.outputs[0];
      const auto repeatedAlias=append(graph,rename);
      o::RelMetadataQuery metadata(graph);
      const auto *known=metadata.properties(uniqueAlias);
      check(known && o::coversNonNullUniqueKey(*known,{graph.nodes[uniqueAlias].outputs.front()}),
            "direct positional rename dropped a key or its non-null proof");
      const auto *unknown=metadata.properties(repeatedAlias);
      check(unknown && unknown->uniqueKeys.empty() && unknown->nonNullColumns.empty(),
            "ambiguous aliases borrowed a hard key or non-null proof");
    }
    checkSet("SELECT id AS k,n FROM owned_key EXCEPT ALL SELECT id,n FROM owned_repeat;", true, true);
    checkSet("SELECT id AS k,n FROM owned_repeat EXCEPT ALL SELECT id,n FROM owned_key;", false, false);
    checkSet("SELECT id AS k,n FROM owned_repeat INTERSECT ALL SELECT id,n FROM owned_key;", true, true);
    checkSet("SELECT id AS k,n FROM owned_key UNION ALL SELECT id,n FROM owned_repeat;", false, false);
    checkSet("SELECT id AS k,n FROM owned_key UNION ALL SELECT id,n FROM owned_key;", false, true);
    checkSet("SELECT id AS k,n FROM owned_key EXCEPT ALL SELECT CAST(id AS BIGINT),n FROM owned_repeat;", true, true);
    checkSet("SELECT id AS k,n FROM owned_key EXCEPT ALL SELECT CAST(id AS DOUBLE PRECISION),n FROM owned_repeat;", false, false);
  }
  {
    c::MemoryCatalog typed;
    c::SqlType integer;
    integer.family = c::TypeFamily::Integer;
    integer.bits = 32;
    integer.unsignedInteger = false;
    c::TableDefinition table;
    table.name = "proof_nullable";
    table.columns = {{"n",c::ValueType::Integer},{"m",c::ValueType::Integer}};
    for (auto &column : table.columns)
      column.sqlType = std::make_shared<const c::SqlType>(integer);
    typed.tables.push_back(std::move(table));
    const auto plan = [&](const std::string &sql) {
      auto compiled = c::compile(c::Dialect::PostgreSQL, sql, {}, &typed);
      check(compiled->ok(), "filter fact SQL did not compile");
      auto planned = o::optimize(compiled->model);
      check(planned.ok(), "filter fact SQL did not plan");
      return planned;
    };
    const auto prove = [&](const std::string &condition, bool nonNull) {
      auto planned = plan("SELECT n AS k FROM proof_nullable WHERE " + condition);
      o::RelMetadataQuery metadata(planned.logical);
      const auto *facts = metadata.properties(planned.logical.root);
      check(facts && (!facts->nonNullColumns.empty()) == nonNull, "wrong filter nullability evidence");
      if (nonNull)
        check(facts->nonNullColumns.front().name == "k", "filter nullability did not follow projection alias");
      for (const auto &event : planned.rules) {
        if (event.rule != "ProjectCalcMerge") continue;
        const auto *fused = metadata.properties(event.after);
        check(fused && (!fused->nonNullColumns.empty()) == nonNull,
              "fused Calc changed filter nullability");
      }
    };
    prove("n>0",true);
    prove("n IS NOT NULL",true);
    prove("n>0 OR n IS NOT NULL",true);
    prove("(m>0 AND n>0) OR n<2",true);
    prove("n>0 OR m>0",false);
    prove("n IS NULL OR n>0",false);
    prove("n IS DISTINCT FROM 1",false);
    prove("n IS NOT DISTINCT FROM 1",true);
    prove("NULL IS DISTINCT FROM n",true);
    prove("n IS NOT DISTINCT FROM NULL",false);
    prove("n IS NOT DISTINCT FROM NULL OR n IS NOT DISTINCT FROM 1",false);
    prove("COALESCE(n,1)>0",false);
    const auto grouped = [&](const std::string &sql, bool nonNull) {
      auto planned = plan(sql);
      o::RelMetadataQuery metadata(planned.logical);
      const auto *facts = metadata.properties(planned.logical.root);
      check(facts && (!facts->nonNullColumns.empty()) == nonNull, "wrong grouping nullability evidence");
    };
    grouped("SELECT DISTINCT n FROM proof_nullable WHERE n>0",true);
    grouped("SELECT n AS k FROM proof_nullable WHERE n>0 GROUP BY k",true);
    grouped("SELECT n FROM proof_nullable WHERE n>0 GROUP BY GROUPING SETS ((n,m),(n))",true);
    grouped("SELECT n FROM proof_nullable WHERE n>0 GROUP BY GROUPING SETS ((n),())",false);
    grouped("SELECT MIN(n) FROM proof_nullable WHERE n>0",false);
    auto redundant = plan("SELECT s.k FROM (SELECT n AS k FROM proof_nullable WHERE n>0) s WHERE s.k IS NOT NULL");
    check(fired(redundant,"HepFilterNullability"), "derived filter evidence did not remove a redundant null test");
  }
  auto noKey=o::optimize(parse("SELECT DISTINCT id FROM owned_key;"));check(noKey.ok(),"unknown key query");
  check(std::none_of(noKey.rules.begin(),noKey.rules.end(),[](const auto&e){return e.rule=="HepDistinctRemove";}),"unknown key treated as unique");
  auto projected=o::optimize(parse("SELECT * FROM owned_key;"));check(projected.ok(),"identity project optimization");
  check(std::any_of(projected.rules.begin(),projected.rules.end(),[](const auto&e){return e.rule=="HepProjectRemove";}),"identity project retained");
  o::PlannerOptions disabled;disabled.heuristic.enabled=false;
  auto disabledResult=o::optimize(parse("SELECT * FROM owned_key;"),nullptr,disabled);
  check(disabledResult.ok() && disabledResult.heuristicMatches==0,"disabled Hep executed");
  o::PlannerOptions small;small.heuristic.maxMatches=1;
  auto limited=o::optimize(parse("SELECT * FROM owned_key;"),nullptr,small);
  check(limited.ok() && !limited.heuristicComplete && !limited.searchComplete,"bounded Hep lost baseline/limit report");
  o::PlannerOptions top;top.heuristic.order=o::HepMatchOrder::TopDown;
  check(o::optimize(parse("SELECT * FROM owned_key;"),nullptr,top).ok(),"top-down Hep failed");
  {
    auto inherited = catalog;
    inherited.tables.front().noDescendantsKnown = false;
    for (const auto &source : {"owned_key", "owned_key *", "ONLY (owned_key)"}) {
      auto compiled = c::compile(c::Dialect::PostgreSQL,
          std::string("SELECT DISTINCT id FROM ") + source + ";", {}, &inherited);
      check(compiled->ok(), "table inheritance fixture did not compile");
      auto plan = o::optimize(compiled->model);
      check(plan.ok() && fired(plan, "HepDistinctRemove") == (std::string(source) == "ONLY (owned_key)"),
            "parent primary key was applied to potentially inherited rows");
    }
  }
  {
    auto peers = o::optimize(parse("VALUES (1),(1),(1),(2) ORDER BY 1 FETCH FIRST ROW WITH TIES;"));
    check(peers.ok(),"WITH TIES plan failed");
    o::RelMetadataQuery peerFacts(peers.logical);
    const auto *range = peerFacts.properties(peers.logical.root);
    check(range && range->rows.minimum == 1 && range->rows.maximum == 4,
          "WITH TIES incorrectly used row count as a hard upper bound");
    check(std::none_of(peers.best.nodes.begin(),peers.best.nodes.end(),
          [](const auto &node){return node.kind==o::PhysicalKind::TopNSort;}),
          "ordinary top-N heap cannot retain an unbounded peer group");
  }
  {
    struct ExpectedSet {
      const char *sql;
      std::uint64_t minimum, maximum;
      bool distinct;
    };
    for (const auto &expected : {
           ExpectedSet{"VALUES (1), (1), (2) UNION ALL VALUES (3), (4);", 5, 5, false},
           ExpectedSet{"VALUES (1), (1), (2) UNION VALUES (3), (4);", 1, 5, true},
           ExpectedSet{"VALUES (1), (1), (2) EXCEPT ALL VALUES (1);", 2, 3, false},
           ExpectedSet{"VALUES (1), (1), (2) EXCEPT VALUES (1);", 0, 3, true},
           ExpectedSet{"VALUES (1), (1), (2) INTERSECT ALL VALUES (1);", 0, 1, false},
           ExpectedSet{"VALUES (1), (2) EXCEPT (SELECT 1 LIMIT 0);", 1, 2, true}}) {
      auto set = o::optimize(parse(expected.sql));
      check(set.ok(), "set operation could not be estimated");
      o::RelMetadataQuery setFacts(set.logical);
      const auto *facts = setFacts.properties(set.logical.root);
      check(facts && facts->rows.minimum == expected.minimum &&
            facts->rows.maximum == expected.maximum &&
            facts->uniqueKeys.empty() != expected.distinct,
            "set operation lost its hard row bounds or distinct key");
      const auto rows = set.best.nodes.at(set.best.root).rows;
      check(rows >= expected.minimum && rows <= expected.maximum,
            "physical set estimate violated proven row bounds");
    }
    for (const auto &sql : {
           "SELECT DISTINCT s.id FROM (SELECT 1 AS id UNION SELECT 2) s;",
           "WITH s(renamed) AS (SELECT 1 AS id UNION SELECT 2) SELECT DISTINCT renamed FROM s;",
           "SELECT DISTINCT s.x,s.y FROM (SELECT 1 AS x,2 AS y UNION SELECT 3,4) s;"}) {
      auto set = o::optimize(parse(sql));
      check(set.ok() && fired(set, "HepDistinctRemove"),
            "set distinctness was not propagated through the derived alias");
    }
    for (const auto &sql : {
           "SELECT DISTINCT s.id FROM (SELECT 1 AS id UNION ALL SELECT 1) s;",
           "SELECT DISTINCT s.x FROM (SELECT 1 AS x,2 AS y UNION SELECT 1,3) s;"}) {
      auto set = o::optimize(parse(sql));
      check(set.ok() && !fired(set, "HepDistinctRemove"),
            "set plan dropped DISTINCT without a key on the projected columns");
    }
  }
  for(const auto &sql:{"SELECT 1 ORDER BY 1;","SELECT 1 LIMIT 10;","SELECT 1 ORDER BY 1 LIMIT 10;"}) {
    auto simplified=o::optimize(parse(sql));check(simplified.ok() && fired(simplified,"HepSortLimitRemove"),"proved redundant sort / limit retained");
  }
  for(const auto &sql:{"SELECT 1 LIMIT 0;","SELECT 1 LIMIT 1 OFFSET 1;","SELECT random() ORDER BY 1 LIMIT 10;","SELECT 1 ORDER BY CAST('bad' AS INTEGER);"}) {
    auto kept=o::optimize(parse(sql));check(kept.ok() && !fired(kept,"HepSortLimitRemove"),"redundant sort / limit rule crossed an effect or OFFSET boundary");
  }
  auto duplicateBound=c::compile(c::Dialect::PostgreSQL,"SELECT id AS a,id AS b,n FROM owned_key ORDER BY a ASC NULLS LAST,b DESC NULLS FIRST,n;",{},&catalog);
  check(duplicateBound->ok(),"duplicate sort binding fixture");
  auto before=c::dumpModel(*duplicateBound);auto duplicate=o::optimize(duplicateBound->model);
  check(duplicate.ok() && fired(duplicate,"HepSortDuplicateKeys"),"duplicate bound / alias sort keys retained");
  check(c::dumpModel(*duplicateBound)==before,"Hep sorting rules mutated source model");
  for(const auto &event:duplicate.rules)if(event.rule=="HepSortDuplicateKeys") {
    const auto &sort=duplicate.logical.nodes.at(event.after);
    check(sort.keys.size()==2 && sort.descending[0]==false && sort.nullOrdering[0]=="LAST","duplicate rule lost first key direction / NULL placement");
  }
  for(const auto &sql:{"SELECT id FROM owned_key ORDER BY random(),random();","SELECT ABS(n) AS a,ABS(n) AS b FROM owned_key ORDER BY a,b;"}) {
    auto cpl=c::compile(c::Dialect::PostgreSQL,sql,{},&catalog);check(cpl->ok(),"unknown sort expression fixture");
    auto kept=o::optimize(cpl->model);check(kept.ok() && !fired(kept,"HepSortDuplicateKeys"),"duplicate rule inferred arbitrary function equivalence");
  }
  auto unresolved=o::optimize(parse("SELECT id FROM owned_key ORDER BY id,id;"));
  check(unresolved.ok() && !fired(unresolved,"HepSortDuplicateKeys"),"duplicate rule guessed unresolved column identity");
  auto sortedBound=c::compile(c::Dialect::PostgreSQL,"SELECT id FROM owned_key ORDER BY id;",{},&catalog);
  o::MemoryStatistics oneEstimate;o::TableStatistics one;one.name="owned_key";one.rows=1;oneEstimate.tables.push_back(one);
  auto estimatedOne=o::optimize(sortedBound->model,&oneEstimate);
  check(estimatedOne.ok() && !fired(estimatedOne,"HepSortLimitRemove"),"one statistical row became a hard cardinality proof");
  {
    o::OptimizationResult large;large.logical.program=parse("SELECT 1;");large.logical.nodes.resize(1);
    auto huge=values;huge.valueCount=9007199254740993ULL;const auto input=append(large.logical,huge);
    auto boundary=limit;boundary.inputs={input};boundary.rowLimit=9007199254740992.;boundary.rowOffset=0;
    const auto root=append(large.logical,boundary);
    check(hep(large)==root,"double rounding changed a hard cardinality comparison above 2^53");
  }
  {
    auto plan=[&](const std::string &sql,const c::CatalogSnapshot *source)->o::OptimizationResult {
      auto parsed=c::compile(c::Dialect::PostgreSQL,sql,{},source);check(parsed->ok(),"nullability fixture compilation failed");return o::optimize(parsed->model);
    };
    for(const auto &sql:{"SELECT id FROM owned_key WHERE id IS NOT NULL;","SELECT id FROM owned_key WHERE id IS NULL;","SELECT id FROM owned_key WHERE id IS NOT NULL AND n>0;"}) {
      auto result=plan(sql,&catalog);check(result.ok() && fired(result,"HepFilterNullability"),"verified scalar nullability proof was not used");
    }
    for(const auto &entry:std::vector<std::pair<std::string,double>>{
      {"SELECT id FROM owned_key WHERE id IS NULL ORDER BY id LIMIT 10;",0},
      {"SELECT COUNT(*) FROM owned_key WHERE id IS NULL;",1},
      {"SELECT id,COUNT(*) FROM owned_key WHERE id IS NULL GROUP BY id;",0},
      {"SELECT id,COUNT(*) FROM owned_key WHERE id IS NULL GROUP BY GROUPING SETS((id),(),());",2}}) {
      auto propagated=plan(entry.first,&catalog);check(propagated.ok() && propagated.best.nodes[propagated.best.root].rows==entry.second,"empty input cardinality did not propagate through relational operators");
    }
    auto impossible=plan("SELECT id FROM owned_key WHERE id IS NULL;",&catalog);
    check(impossible.best.nodes[impossible.best.root].rows==0,"impossible null filter did not produce empty estimate");
    for(const auto &sql:{"SELECT id FROM owned_key WHERE n IS NOT NULL;","SELECT id FROM owned_key WHERE id IS NULL AND CAST('bad' AS INTEGER)=1;"}) {
      auto kept=plan(sql,&catalog);check(kept.ok() && !fired(kept,"HepFilterNullability"),"nullability rule crossed nullable / error evaluation boundary");
    }
    auto typed=catalog;typed.tables[0].primaryKeyKnown=false;typed.tables[0].primaryKey.clear();
    c::SqlType soft;soft.family=c::TypeFamily::Integer;soft.nullability=c::Nullability::NotNull;
    typed.tables[0].columns[1].sqlType=std::make_shared<const c::SqlType>(soft);
    check(!fired(plan("SELECT n FROM owned_key WHERE n IS NOT NULL;",&typed),"HepFilterNullability"),"soft inferred type became a hard constraint");
    typed.tables[0].columns[1].nonNullKnown=true;
    check(fired(plan("SELECT n FROM owned_key WHERE n IS NOT NULL;",&typed),"HepFilterNullability"),"explicit immediate non-null constraint ignored");
    typed.tables[0].columns[1].type=c::ValueType::Unknown;typed.tables[0].columns[1].sqlType.reset();
    check(!fired(plan("SELECT n FROM owned_key WHERE n IS NOT NULL;",&typed),"HepFilterNullability"),"PG composite / opaque type used scalar NULL semantics");
    auto outer=catalog;auto other=table;other.name="owned_other";outer.tables.push_back(other);
    auto left=plan("SELECT a.id FROM owned_key a LEFT JOIN owned_other b ON a.id=b.id WHERE a.id IS NOT NULL;",&outer);
    check(left.ok() && fired(left,"HepFilterNullability"),"preserved outer-join side lost non-null proof");
    for(const auto &sql:{"SELECT a.id FROM owned_key a LEFT JOIN owned_other b ON a.id=b.id WHERE b.id IS NOT NULL;","SELECT a.id FROM owned_key a FULL JOIN owned_other b ON a.id=b.id WHERE a.id IS NOT NULL;"})
      check(!fired(plan(sql,&outer),"HepFilterNullability"),"outer join NULL extension was ignored");
    auto source=c::compile(c::Dialect::PostgreSQL,"SELECT DISTINCT id FROM owned_key WHERE id IS NOT NULL;",{},&catalog);check(source->ok(),"stale model fixture failed");
    auto stale=source->model;for(auto &query:stale.queries)query.catalogChecked=false;
    auto guarded=o::optimize(stale);check(guarded.ok() && !fired(guarded,"HepFilterNullability") && !fired(guarded,"HepDistinctRemove"),"unverified imported constraints still drive rewrite proofs");
  }
  {
    auto joinCatalog = catalog;
    auto &columns = joinCatalog.tables.front().columns;
    c::SqlType integer;
    integer.family = c::TypeFamily::Integer;
    integer.bits = 32;
    integer.unsignedInteger = false;
    const auto type = std::make_shared<const c::SqlType>(integer);
    for (auto &column : columns) column.sqlType = type;
    auto right = joinCatalog.tables.front();
    right.name = "owned_other";
    joinCatalog.tables.push_back(right);
    const auto query = [](const std::string &joinType, const std::string &predicate,
                          const std::string &condition = "ON a.id=b.id") {
      return "SELECT a.n AS left_value, b.n AS right_value FROM owned_key AS a " + joinType +
             " JOIN owned_other AS b " + condition + " WHERE " + predicate + ";";
    };
    const auto plan = [&](const std::string &sql, const o::PlannerOptions &options) {
      auto parsed = c::compile(c::Dialect::PostgreSQL, sql, {}, &joinCatalog);
      if (!parsed->ok()) throw std::runtime_error(parsed->diagnostics.front().message + ": " + sql);
      auto result = o::optimize(parsed->model, nullptr, options);
      if (!result.ok()) throw std::runtime_error(result.diagnostics.front().message + ": " + sql);
      return result;
    };
    using c::JoinKind;
    for (const auto &[join, predicate, expected] : std::vector<std::tuple<std::string, std::string, JoinKind>>{
             {"LEFT", "b.n IS NOT NULL", JoinKind::Inner},
             {"RIGHT", "a.n IS NOT NULL", JoinKind::Inner},
             {"FULL", "a.n IS NOT NULL", JoinKind::Left},
             {"FULL", "b.n IS NOT NULL", JoinKind::Right},
             {"FULL", "a.n IS NOT NULL AND b.n IS NOT NULL", JoinKind::Inner}}) {
      auto result = plan(query(join, predicate), {});
      const auto event = std::find_if(result.rules.begin(), result.rules.end(),
          [](const auto &r) { return r.rule == "HepSimplifyOuterJoin"; });
      check(event != result.rules.end(), "safe outer join was not simplified");
      const auto &before = result.logical.nodes.at(event->before);
      const auto &after = result.logical.nodes.at(event->after);
      const auto &beforeJoin = result.logical.nodes.at(before.inputs.front());
      const auto &afterJoin = result.logical.nodes.at(after.inputs.front());
      check(beforeJoin.join != expected && afterJoin.kind == o::RelKind::Join && afterJoin.join == expected,
            "outer join rule picked the wrong preserved side");
      check(beforeJoin.inputs == afterJoin.inputs && before.predicates.size() == after.predicates.size(),
            "outer join rule changed join inputs or dropped filters");
    }
    for (const auto &sql : {
             query("LEFT", "a.n IS NOT NULL"),
             query("LEFT", "b.n IS NULL"),
             query("LEFT", "b.n IS NOT NULL OR a.n IS NOT NULL"),
             query("LEFT", "b.n IS NOT NULL AND random()>0"),
             query("LEFT", "b.n IS NOT NULL", "ON a.id+1=b.id"),
             query("LEFT", "b.n IS NOT NULL", "USING (id)")}) {
      check(!fired(plan(sql, {}), "HepSimplifyOuterJoin"), "unsafe outer join was simplified");
    }
    auto disabledRule = o::PlannerOptions{};
    disabledRule.filterPushdown = false;
    check(!fired(plan(query("LEFT", "b.n IS NOT NULL"), disabledRule), "HepSimplifyOuterJoin"),
          "disabled outer join rule ran");
    auto unknown = o::optimize(parse(query("LEFT", "b.n IS NOT NULL")));
    check(unknown.ok() && !fired(unknown, "HepSimplifyOuterJoin"),
          "outer join simplification used unbound aliases");

    const auto withOn = [](const std::string &join, const std::string &condition) {
      return "SELECT a.n, b.n FROM owned_key a " + join + " JOIN owned_other b ON " + condition + ";";
    };
    for (const auto &[join, predicateCount, leftFilter, rightFilter] :
         std::vector<std::tuple<std::string, std::size_t, bool, bool>>{
             {"INNER", 1, true, true}, {"LEFT", 2, false, true}, {"RIGHT", 2, true, false}}) {
      const auto result = plan(withOn(join, "a.id=b.id AND a.n>7 AND b.n IS NOT NULL"), {});
      const auto event = std::find_if(result.rules.begin(), result.rules.end(),
          [](const auto &r) { return r.rule == "JoinConditionPushDown"; });
      check(event != result.rules.end(), "side-local ON condition was not pushed down");
      const auto &before = result.logical.nodes.at(event->before);
      const auto &after = result.logical.nodes.at(event->after);
      check(before.join == after.join && before.outputs.size() == after.outputs.size() &&
            before.predicates.size() == 3 && after.predicates.size() == predicateCount,
            "ON pushdown changed join identity or discarded a retained condition");
      check((result.logical.nodes.at(after.inputs[0]).kind == o::RelKind::Filter) == leftFilter &&
            (result.logical.nodes.at(after.inputs[1]).kind == o::RelKind::Filter) == rightFilter,
            "ON pushdown filtered the preserved side of an outer join");
      check(result.best.nodes.at(result.best.root).total.score() <=
            result.baseline.nodes.at(result.baseline.root).total.score(),
            "ON pushdown discarded the lower-cost baseline");
    }
    for (const auto &sql : {withOn("FULL", "a.id=b.id AND b.n>7"),
                            withOn("LEFT", "a.id=b.id AND a.n>7"),
                            withOn("LEFT", "a.id=b.id AND b.n>7 AND random()>0"),
                            withOn("LEFT", "a.id=b.id AND b.n>7 AND b.n::text='bad'"),
                            withOn("LEFT", "a.id=b.id AND (b.n>7 OR a.n>7)")}) {
      check(!fired(plan(sql, {}), "JoinConditionPushDown"), "unsafe ON condition was pushed down");
    }
    const auto onFilter = withOn("LEFT", "a.id=b.id AND b.n>7");
    check(!fired(plan(onFilter, disabledRule), "JoinConditionPushDown") &&
          !fired(o::optimize(parse(onFilter)), "JoinConditionPushDown"),
          "ON pushdown ignored its configuration or unverified catalog");

    const auto grouped = "SELECT n, COUNT(*) AS count_value FROM owned_key "
                         "GROUP BY n HAVING n>7 AND n IS NOT NULL ORDER BY 1;";
    const auto pushedGroup = plan(grouped, {});
    const auto push = std::find_if(pushedGroup.rules.begin(), pushedGroup.rules.end(),
        [](const auto &event) { return event.rule == "HepFilterAggregateTranspose"; });
    check(push != pushedGroup.rules.end(), "HAVING predicates on the group key were not pushed");
    const auto &beforeGroup = pushedGroup.logical.nodes.at(push->before);
    const auto &afterGroup = pushedGroup.logical.nodes.at(push->after);
    const auto &beforeInput = pushedGroup.logical.nodes.at(beforeGroup.inputs.front());
    const auto &afterInput = pushedGroup.logical.nodes.at(afterGroup.inputs.front());
    check(beforeGroup.kind == o::RelKind::Filter && beforeInput.kind == o::RelKind::Aggregate &&
          afterGroup.kind == o::RelKind::Aggregate && afterInput.kind == o::RelKind::Filter &&
          afterInput.predicates.size() == 2 && beforeInput.inputs == afterInput.inputs,
          "HAVING pushdown changed aggregate identity or dropped a predicate");
    for (const auto *sql : {
           "SELECT n,COUNT(*) FROM owned_key GROUP BY n HAVING COUNT(*)>1;",
           "SELECT n,COUNT(random()) FROM owned_key GROUP BY n HAVING n>7;",
           "SELECT n,SUM(n) FROM owned_key GROUP BY n HAVING n>7;",
           "SELECT COUNT(*) FROM owned_key HAVING COUNT(*)>=1;",
           "SELECT n,COUNT(*) FROM owned_key GROUP BY n HAVING n>7 AND random()>0;"})
      check(!fired(plan(sql, {}), "HepFilterAggregateTranspose"), "unsafe grouped filter was moved");
    o::PlannerOptions onlyPushdown;
    onlyPushdown.heuristic.rules = {o::HepRule::FilterAggregateTranspose};
    check(!fired(plan("SELECT n,COUNT(*) FROM owned_key GROUP BY GROUPING SETS ((n),()) HAVING n>7;",
                     onlyPushdown), "HepFilterAggregateTranspose"),
          "HAVING was moved below an aggregate with multiple grouping sets");
    check(!fired(plan(grouped, disabledRule), "HepFilterAggregateTranspose") &&
          !fired(o::optimize(parse(grouped)), "HepFilterAggregateTranspose"),
          "HAVING pushdown ignored configuration or unverified catalog");

    const auto setFilter = [](const std::string &operation) {
      return "SELECT s.k FROM (SELECT n AS old FROM owned_key " + operation +
             " SELECT n AS other FROM owned_other) s(k) WHERE s.k>7 AND s.k IS NOT NULL;";
    };
    for (const auto *operation : {"UNION", "UNION ALL", "INTERSECT", "INTERSECT ALL", "EXCEPT", "EXCEPT ALL"}) {
      const auto result = plan(setFilter(operation), {});
      const auto found = std::find_if(result.rules.begin(), result.rules.end(),
          [](const auto &event) { return event.rule == "HepFilterSetOpTranspose"; });
      check(found != result.rules.end(), "typed set output filter was not distributed");
      const auto &previous = result.logical.nodes.at(found->before);
      const auto &next = result.logical.nodes.at(found->after);
      check(next.kind == o::RelKind::SetOperation && next.inputs.size() == 2 &&
            next.outputs.front().name == "k", "set rewrite lost multiplicity or outer alias");
      for (auto arm : next.inputs) {
        const auto &filter = result.logical.nodes.at(arm);
        check(filter.kind == o::RelKind::Filter && filter.predicates.size() == previous.predicates.size() &&
              filter.predicates.front().query == previous.predicates.front().query &&
              result.logical.nodes.at(filter.inputs.front()).outputs.front().name == "k",
              "rewritten set operand lost the original selection scope");
      }
    }
    for (const auto &sql : {
           "SELECT s.k FROM (SELECT n FROM owned_key UNION ALL SELECT n+1 FROM owned_other) s(k) WHERE s.k>7;",
           "SELECT s.k FROM (SELECT n FROM owned_key UNION ALL SELECT n FROM owned_other LIMIT 3) s(k) WHERE s.k>7;",
           "SELECT s.k FROM (SELECT n FROM owned_key UNION ALL SELECT n FROM owned_other) s(k) WHERE random()>0;"})
      check(!fired(plan(sql, {}), "HepFilterSetOpTranspose"), "unsafe set selection was moved");
    check(!fired(plan(setFilter("UNION ALL"), disabledRule), "HepFilterSetOpTranspose") &&
          !fired(o::optimize(parse(setFilter("UNION ALL"))), "HepFilterSetOpTranspose"),
          "set distribution ignored configuration or unverified catalog");

    const std::string projectedFilter =
        "SELECT s.shown FROM (SELECT n AS original,id FROM owned_key) s(shown,filter_id) "
        "WHERE s.filter_id>1 AND s.shown IS NOT NULL;";
    const auto pushedProject = plan(projectedFilter, {});
    const auto event = std::find_if(pushedProject.rules.begin(), pushedProject.rules.end(),
        [](const auto &rule) { return rule.rule == "HepFilterProjectTranspose"; });
    check(event != pushedProject.rules.end(), "derived alias filter did not move through projection");
    const auto &wrapped = pushedProject.logical.nodes.at(event->after);
    const auto &movedProject = pushedProject.logical.nodes.at(wrapped.inputs.at(0));
    const auto &selection = pushedProject.logical.nodes.at(movedProject.inputs.at(0));
    const auto &sourceQuery = pushedProject.logical.program.queries.at(movedProject.query - 1);
    const auto comparison = selection.predicates.at(0);
    const auto &rewrite = sourceQuery.expressions.at(comparison.expression);
    check(wrapped.kind == o::RelKind::Subquery && wrapped.outputs.at(0).name == "shown" &&
          movedProject.kind == o::RelKind::Project && selection.kind == o::RelKind::Filter &&
          comparison.query == movedProject.query && rewrite.operands.at(0) == movedProject.keys.at(1).expression,
          "projection pushdown lost an output alias or mapped a filter to the wrong input column");
    for (const auto *sql : {
        "SELECT s.n FROM (SELECT n+1 AS n FROM owned_key) s WHERE s.n>7;",
        "SELECT s.n FROM (SELECT n,random() FROM owned_key) s WHERE s.n>7;",
        "SELECT s.n FROM (SELECT n FROM owned_key LIMIT 2) s WHERE s.n>7;",
        "SELECT s.n FROM (SELECT n FROM owned_key) s WHERE s.n>7 OR s.n IS NULL;",
        "WITH s AS MATERIALIZED (SELECT n FROM owned_key) SELECT n FROM s WHERE n>7;"})
      check(!fired(plan(sql, {}), "HepFilterProjectTranspose"), "a projection evaluation boundary was crossed");
    check(!fired(plan(projectedFilter, disabledRule), "HepFilterProjectTranspose"),
          "disabled projection pushdown ran");

    const auto explicitlyMaterialized = plan(
        "WITH s(k) AS MATERIALIZED (SELECT n FROM owned_key UNION ALL SELECT n FROM owned_other) "
        "SELECT k FROM s WHERE k>7;", {});
    check(explicitlyMaterialized.ok() && !fired(explicitlyMaterialized,"HepFilterSetOpTranspose") &&
          std::any_of(explicitlyMaterialized.logical.nodes.begin(), explicitlyMaterialized.logical.nodes.end(),
                      [](const auto &node) { return node.kind==o::RelKind::Subquery && node.materialized; }),
          "set filters crossed the explicit CTE materialization boundary");

    const auto described = plan("SELECT id AS renamed,n+1 AS adjusted,7 AS constant_value FROM owned_key;", {});
    o::RelMetadataQuery origins(described.logical);
    const auto describe = [&](std::size_t ordinal) {
      const auto *lineage = origins.columnOrigins(described.logical.root, ordinal);
      check(lineage && lineage->complete, "verified SQL column origin is missing");
      return lineage;
    };
    const auto direct = describe(0);
    check(direct->origins.size() == 1 && direct->origins[0].table == "owned_key" &&
          direct->origins[0].column == "id" && direct->origins[0].ordinal == 0 && !direct->origins[0].derived,
          "direct alias changed the base column position or derivation flag");
    const auto computed = describe(1);
    check(computed->origins.size() == 1 && computed->origins[0].column == "n" && computed->origins[0].derived &&
          describe(2)->origins.empty() && describe(0) == direct,
          "computed and constant expressions lost origins or cache stability");
    check(o::dumpOptimization(described).find("\"columnOrigins\"") != std::string::npos,
          "plan JSON did not expose column origins");

    const auto renamed = plan("SELECT d.x FROM (SELECT id FROM owned_key) d(x);", {});
    o::RelMetadataQuery renamedMeta(renamed.logical);
    const auto throughAlias = renamedMeta.columnOrigins(renamed.logical.root, 0);
    check(throughAlias && throughAlias->complete && throughAlias->origins[0].column == "id" &&
          !throughAlias->origins[0].derived, "derived column alias lost positional origin");

    const auto joined = plan("SELECT a.id,b.n FROM owned_key a LEFT JOIN owned_other b ON a.id=b.id;", {});
    o::RelMetadataQuery joinedMeta(joined.logical);
    const auto leftOrigin = joinedMeta.columnOrigins(joined.logical.root, 0);
    const auto rightOrigin = joinedMeta.columnOrigins(joined.logical.root, 1);
    check(leftOrigin && leftOrigin->complete && !leftOrigin->origins[0].derived &&
          rightOrigin && rightOrigin->complete && rightOrigin->origins[0].derived &&
          rightOrigin->origins[0].table == "owned_other", "outer join origin missed null extension");
    const auto innerJoin = plan("SELECT a.id,b.n FROM owned_key a JOIN owned_other b ON a.id=b.id;", {});
    const auto swapped = std::find_if(innerJoin.rules.begin(), innerJoin.rules.end(),
        [](const auto &event) { return event.rule == "JoinCommute"; });
    check(swapped != innerJoin.rules.end(), "no swapped input shape to test");
    o::RelMetadataQuery commutedMeta(innerJoin.logical);
    const auto prior = commutedMeta.columnOrigins(swapped->before, 0);
    const auto alternate = commutedMeta.columnOrigins(swapped->after, 0);
    check(prior && prior->complete && alternate && alternate->complete &&
          prior->origins[0].table == alternate->origins[0].table &&
          prior->origins[0].column == alternate->origins[0].column,
          "swapping join children changed stable output identity");

    const auto groupedOrigins = plan("SELECT n,COUNT(*),MIN(id) AS lowest FROM owned_key GROUP BY n;", {});
    o::RelMetadataQuery groupedMeta(groupedOrigins.logical);
    const auto keyOrigin = groupedMeta.columnOrigins(groupedOrigins.logical.root, 0);
    const auto countOrigin = groupedMeta.columnOrigins(groupedOrigins.logical.root, 1);
    const auto minOrigin = groupedMeta.columnOrigins(groupedOrigins.logical.root, 2);
    check(keyOrigin && keyOrigin->complete && keyOrigin->origins[0].column == "n" &&
          countOrigin && countOrigin->complete && countOrigin->origins.empty() &&
          minOrigin && minOrigin->complete && minOrigin->origins[0].column == "id" && minOrigin->origins[0].derived,
          "group and aggregate input provenance were not distinguished");
    const auto combinedOrigins = plan("SELECT p.id FROM (SELECT id FROM owned_key UNION ALL SELECT id FROM owned_other) p;", {});
    o::RelMetadataQuery setMeta(combinedOrigins.logical);
    const auto combined = setMeta.columnOrigins(combinedOrigins.logical.root, 0);
    check(combined && combined->complete && combined->origins.size() == 2 &&
          combined->origins[0].table == "owned_key" && combined->origins[1].table == "owned_other",
          "set result lost one input source");
    o::RelMetadataQuery offlineMeta(noKey.logical);
    const auto unavailable = offlineMeta.columnOrigins(noKey.logical.root, 0);
    check(unavailable && !unavailable->complete, "unverified catalog supplied a definitive origin");
    o::RelMetadataQuery limitedOrigins(described.logical, tiny);
    check(!limitedOrigins.columnOrigins(described.logical.root, 0) &&
          limitedOrigins.status() == o::MetadataStatus::ResourceLimit, "column origin budget was ignored");
    o::RelMetadataQuery stoppedOrigins(described.logical, cancelled);
    check(!stoppedOrigins.columnOrigins(described.logical.root, 0) &&
          stoppedOrigins.status() == o::MetadataStatus::Stopped, "column origin cancellation was ignored");

    const auto unusedJoin = "SELECT a.n AS value FROM owned_key a LEFT JOIN owned_other b ON a.id=b.id;";
    auto safeCatalog=joinCatalog;
    for (auto &relation:safeCatalog.tables) relation.rowSecurityDisabledKnown=true;
    const auto removeJoin=[&](const std::string &sql) {
      const auto compiled=c::compile(c::Dialect::PostgreSQL,sql,{},&safeCatalog);
      check(compiled->ok(),"outer join removal fixture did not bind");
      o::PlannerOptions options;
      options.heuristic.rules={o::HepRule::RemoveUnusedOuterJoin};
      return o::optimize(compiled->model,nullptr,options);
    };
    const auto pruned = removeJoin(unusedJoin);
    const auto removal = std::find_if(pruned.rules.begin(), pruned.rules.end(),
        [](const auto &event) { return event.rule == "HepRemoveUnusedOuterJoin"; });
    check(removal != pruned.rules.end(), "outer join was retained despite the complete unique key");
    const auto &projection = pruned.logical.nodes.at(removal->after);
    check(projection.outputs.size() == 1 && projection.outputs[0].name == "value" &&
          pruned.logical.nodes.at(projection.inputs.front()).table == 0,
          "outer join removal changed projection or kept the wrong scan");
    check(fired(removeJoin("SELECT b.n FROM owned_key a RIGHT JOIN owned_other b ON a.id=b.id;"),
                "HepRemoveUnusedOuterJoin"), "RIGHT JOIN preserved side was not recognized");
    for (const auto &sql : {
           "SELECT b.n FROM owned_key a LEFT JOIN owned_other b ON a.id=b.id;",
           "SELECT a.n FROM owned_key a LEFT JOIN owned_other b ON a.n=b.n;",
           "SELECT a.n FROM owned_key a LEFT JOIN owned_other b ON a.id=b.id ORDER BY b.n;",
           "SELECT a.n FROM owned_key a LEFT JOIN owned_other b ON a.id=b.id AND random()>0;",
           "SELECT a.n FROM owned_key a JOIN owned_other b ON a.id=b.id;",
           "SELECT a.n FROM owned_key a FULL JOIN owned_other b ON a.id=b.id;"}) {
      check(!fired(removeJoin(sql), "HepRemoveUnusedOuterJoin"),
            "outer join removal lost an output, ordering, multiplicity or effect dependency");
    }
    auto inheritedRight = safeCatalog;
    inheritedRight.tables.at(1).noDescendantsKnown = false;
    auto inherited = c::compile(c::Dialect::PostgreSQL,unusedJoin,{},&inheritedRight);
    check(inherited->ok() && !fired(o::optimize(inherited->model), "HepRemoveUnusedOuterJoin"),
          "outer join removal used a key that might not cover descendant rows");

    for (const bool negated : {false, true}) {
      const auto sql = std::string("SELECT a.id, a.n FROM owned_key a WHERE ") +
                       (negated ? "NOT " : "") +
                       "EXISTS (SELECT 1 FROM owned_other b WHERE b.n=a.n);";
      auto result = plan(sql, {});
      const auto event = std::find_if(result.rules.begin(), result.rules.end(),
          [](const auto &r) { return r.rule == "HepDecorrelateExists"; });
      check(event != result.rules.end(), "bound EXISTS did not decorrelate");
      const auto &joined = result.logical.nodes.at(event->after);
      check(joined.kind == (negated ? o::RelKind::AntiJoin : o::RelKind::SemiJoin) &&
            joined.outputs.size() == 2 && joined.predicates.size() == 1 &&
            result.logical.nodes.at(joined.inputs[1]).kind == o::RelKind::Scan,
            "EXISTS rewrite lost membership or the left output shape");
      o::RelMetadataQuery facts(result.logical);
      const auto *rows = facts.properties(event->after);
      check(rows && rows->rows.minimum == 0 && rows->uniqueKeys.size() == 1 &&
            rows->nonNullColumns.size() == 1 && rows->nonNullColumns[0].name == "id",
            "membership join lost the left PK or asserted the nullable key was non-null");
      const auto physical = negated ? o::PhysicalKind::HashAntiJoin : o::PhysicalKind::HashSemiJoin;
      check(std::any_of(result.best.nodes.begin(), result.best.nodes.end(),
              [&](const auto &candidate) { return candidate.kind == physical && !candidate.buildLeft; }) &&
            std::none_of(result.baseline.nodes.begin(), result.baseline.nodes.end(),
              [&](const auto &candidate) { return candidate.kind == physical; }),
            "EXISTS alternatives lost separate baseline or built hash from output side");
      auto unverified = o::optimize(parse(sql));
      check(unverified.ok() && !fired(unverified, "HepDecorrelateExists"),
            "unverified EXISTS correlation was rewritten");
    }
    for (const auto &sql : {
           "SELECT a.n FROM owned_key a WHERE a.n NOT IN (SELECT b.n FROM owned_other b);",
           "SELECT a.n FROM owned_key a WHERE EXISTS (SELECT 1 FROM owned_other b WHERE b.n=a.n LIMIT 0);",
           "SELECT a.n FROM owned_key a WHERE EXISTS (SELECT COUNT(*) FROM owned_other b WHERE b.n=a.n);",
           "SELECT a.n FROM owned_key a WHERE EXISTS (SELECT 1 FROM owned_other b WHERE ABS(b.n)=a.n);",
           "SELECT a.n FROM owned_key a WHERE EXISTS (SELECT 1 FROM owned_other b WHERE b.n=a.n) OR a.n IS NULL;"}) {
      check(!fired(plan(sql, {}), "HepDecorrelateExists"), "EXISTS rewrite crossed a semantic boundary");
    }

    for (const auto &entry : {
           std::pair<const char *,o::RelKind>{"a.n IN (SELECT b.n FROM owned_other b)",o::RelKind::SemiJoin},
           {"a.n = ANY (SELECT b.n FROM owned_other b)",o::RelKind::SemiJoin},
           {"a.id NOT IN (SELECT b.id FROM owned_other b)",o::RelKind::AntiJoin},
           {"a.id <> ALL (SELECT b.id FROM owned_other b)",o::RelKind::AntiJoin}}) {
      auto rewrite=plan(std::string("SELECT a.n FROM owned_key a WHERE ")+entry.first+";",{});
      const auto event=std::find_if(rewrite.rules.begin(),rewrite.rules.end(),
          [](const auto &rule) { return rule.rule=="HepRewriteInSubquery"; });
      check(event!=rewrite.rules.end(),"verified IN/NOT IN was not rewritten");
      const auto &node=rewrite.logical.nodes.at(event->after);
      check(node.kind==entry.second && node.membershipKeys &&
            node.membershipKeys->left.query==node.query &&
            node.membershipKeys->right.query!=node.query,
            "IN rewrite did not preserve independently scoped key expressions");
    }
    for (const auto &predicate : {
           "a.n NOT IN (SELECT b.id FROM owned_other b)",
           "a.id NOT IN (SELECT b.n FROM owned_other b)",
           "a.id = ALL (SELECT b.id FROM owned_other b)",
           "a.id IN (SELECT b.id FROM owned_other b WHERE b.n=a.n)",
           "a.n IN (SELECT b.n FROM owned_other b LIMIT 1)",
           "a.id IN (SELECT ABS(b.id) FROM owned_other b)",
           "a.id IN (SELECT b.id FROM owned_other b) OR a.n IS NULL"}) {
      check(!fired(plan(std::string("SELECT a.id FROM owned_key a WHERE ")+predicate+";",{}),
                   "HepRewriteInSubquery"),"IN rewrite ignored NULL semantics or an expression boundary");
    }

    o::PlannerOptions keysOnly;
    keysOnly.heuristic.rules = {o::HepRule::DistinctRemove};
    const auto distinctQuery = [](std::string join, std::string projected, std::string condition) {
      return "SELECT DISTINCT " + projected + " FROM owned_key a " + join +
             " JOIN owned_other b ON " + condition + ";";
    };
    for (const auto &sql : {distinctQuery("INNER", "a.id AS left_id", "a.id=b.id"),
                            distinctQuery("LEFT", "a.id", "b.id=a.id"),
                            distinctQuery("RIGHT", "b.id", "a.id=b.id"),
                            distinctQuery("LEFT", "a.id", "a.n IS NOT DISTINCT FROM b.id")}) {
      check(fired(plan(sql, keysOnly), "HepDistinctRemove"), "proven join key lost before DISTINCT");
    }
    for (const auto &sql : {distinctQuery("FULL", "a.id", "a.id=b.id"),
                            distinctQuery("LEFT", "b.id", "a.id=b.id"),
                            distinctQuery("LEFT", "a.id", "a.n IS NOT DISTINCT FROM b.n"),
                            distinctQuery("LEFT", "a.id", "a.id=b.id OR a.n=b.n"),
                            distinctQuery("LEFT", "a.id", "a.id=b.id+1"),
                            distinctQuery("LEFT", "a.id", "a.id=b.n")}) {
      check(!fired(plan(sql, keysOnly), "HepDistinctRemove"), "unproved join key removed DISTINCT");
    }
    auto capped = plan("SELECT a.id FROM (SELECT id FROM owned_key LIMIT 7) a "
                       "LEFT JOIN owned_other b ON a.id=b.id;", {});
    o::RelMetadataQuery cappedMetadata(capped.logical);
    const auto *cappedRoot = cappedMetadata.properties(capped.logical.root);
    check(cappedRoot && cappedRoot->rows.maximum == 7,
          "matching a unique key failed to cap a left join at seven rows");
    check(capped.best.nodes[capped.best.root].rows <= 7,
          "physical plan bypassed the hard join cardinality cap");

    joinCatalog.tables[1].primaryKey = {"id", "n"};
    check(!fired(plan(distinctQuery("LEFT", "a.id", "a.id=b.id"), keysOnly), "HepDistinctRemove"),
          "part of a composite key became a uniqueness proof");
    check(fired(plan(distinctQuery("LEFT", "a.id", "a.id=b.id AND a.n=b.n"), keysOnly), "HepDistinctRemove"),
          "complete composite key did not preserve an outer join key");
    check(fired(plan(distinctQuery("LEFT", "a.id", "a.id=b.id AND a.n IS NOT DISTINCT FROM b.n"), keysOnly),
                "HepDistinctRemove"), "complete null-safe composite key lost the preserved side");
    auto wider = integer;
    wider.bits = 64;
    joinCatalog.tables[1].columns[0].sqlType = std::make_shared<const c::SqlType>(wider);
    check(!fired(plan(distinctQuery("LEFT", "a.id", "a.id=b.id AND a.n=b.n"), keysOnly), "HepDistinctRemove"),
          "mixed type equality was used to prove uniqueness");
  }
  {
    auto plan=[&](const std::string &sql){auto cpl=c::compile(c::Dialect::PostgreSQL,sql,{},&catalog);check(cpl->ok(),"prune-empty fixture compilation");return o::optimize(cpl->model);};
    for(const auto &sql:{"SELECT id AS renamed FROM owned_key WHERE id IS NULL;",
                        "SELECT id FROM owned_key WHERE id IS NULL GROUP BY id;",
                        "SELECT id FROM owned_key WHERE id IS NULL ORDER BY id LIMIT 10;",
                        "SELECT DISTINCT id FROM owned_key WHERE id IS NULL;",
                        "SELECT id FROM owned_key LIMIT 0;"}) {
      auto result=plan(sql);if(!result.ok() || !fired(result,"HepPruneEmpty"))throw std::runtime_error(std::string("proved empty relation retained operators: ")+sql+(result.diagnostics.empty()?"":result.diagnostics.front().message));
      const auto &root=result.logical.nodes.at(result.heuristicRoot);
      check(root.kind==o::RelKind::Values && root.valueCount==0,"empty rewrite does not terminate in zero-row Values");
      check(result.best.nodes[result.best.root].rows==0,"empty rewrite changed physical output cardinality");
      for(const auto &event:result.rules)if(event.rule=="HepPruneEmpty") {
        const auto &before=result.logical.nodes.at(event.before),&after=result.logical.nodes.at(event.after);
        check(before.outputs.size()==after.outputs.size(),"empty rewrite changed row width");
        for(std::size_t i=0;i<before.outputs.size();++i)check(before.outputs[i].query==after.outputs[i].query && before.outputs[i].table==after.outputs[i].table && before.outputs[i].name==after.outputs[i].name,"empty rewrite changed column identity / alias");
      }
    }
    for(const auto &entry:std::vector<std::pair<std::string,double>>{
      {"SELECT COUNT(*) FROM owned_key WHERE id IS NULL;",1},
      {"SELECT id,COUNT(*) FROM owned_key WHERE id IS NULL GROUP BY GROUPING SETS((id),(),());",2}}) {
      auto result=plan(entry.first);check(result.ok() && result.best.nodes[result.best.root].rows==entry.second,"empty pruning erased global / repeated grand-total aggregate rows");
    }
    for(const auto &sql:{"SELECT CAST('bad' AS INTEGER) FROM owned_key LIMIT 0;","SELECT random() FROM owned_key LIMIT 0;"}) {
      auto result=plan(sql);check(result.ok() && !fired(result,"HepPruneEmpty"),"empty rewrite crossed an observable evaluation boundary");
    }
    o::MemoryStatistics zero;o::TableStatistics stats;stats.name="owned_key";stats.rows=0;zero.tables.push_back(stats);
    auto scanned=c::compile(c::Dialect::PostgreSQL,"SELECT id FROM owned_key;",{},&catalog);
    auto estimated=o::optimize(scanned->model,&zero);check(estimated.ok() && !fired(estimated,"HepPruneEmpty"),"statistics zero rows became an emptiness proof");
    o::PlannerOptions only;only.heuristic.rules={o::HepRule::PruneEmpty};
    auto constant=parse("SELECT 1 LIMIT 0;");auto pruned=o::optimize(constant,nullptr,only);
    check(pruned.ok() && fired(pruned,"HepPruneEmpty"),"configured isolated prune-empty rule is unusable");
    only.heuristic.enabled=false;auto disabled=o::optimize(constant,nullptr,only);check(disabled.ok() && !fired(disabled,"HepPruneEmpty"),"disabled prune rule ran");
  }
  {
    c::MemoryCatalog schema;
    c::SqlType integer;
    integer.family=c::TypeFamily::Integer;integer.bits=32;integer.unsignedInteger=false;
    c::TableDefinition lhs;
    lhs.name="prune_lhs";lhs.columns={{"n",c::ValueType::Integer}};
    lhs.columns.front().sqlType=std::make_shared<const c::SqlType>(integer);
    auto rhs=lhs;rhs.name="prune_rhs";
    schema.tables={lhs,rhs};
    const auto plan=[&](const std::string &sql) {
      const auto bound=c::compile(c::Dialect::PostgreSQL,sql,{},&schema);
      if(!bound->ok())throw std::runtime_error(bound->diagnostics.front().message);
      return o::optimize(bound->model);
    };
    const auto renamed=[](const o::OptimizationResult &result) {
      return result.logical.nodes.at(result.heuristicRoot).kind==o::RelKind::Rename;
    };
    const auto leftEmpty=plan("(SELECT n AS kept FROM prune_lhs LIMIT 0) UNION ALL SELECT n AS replaced FROM prune_rhs;");
    const auto rightEmpty=plan("SELECT n AS kept FROM prune_lhs EXCEPT ALL (SELECT n FROM prune_rhs LIMIT 0);");
    for (const auto *result : {&leftEmpty,&rightEmpty}) {
      check(result->ok() && renamed(*result), "known empty ALL set input stayed in the plan");
      const auto &root=result->logical.nodes.at(result->heuristicRoot);
      check(root.outputs.size()==1 && root.outputs.front().name=="kept", "set pruning changed the left output name");
      const auto &baseline=result->logical.nodes.at(result->logical.root);
      const auto &original=baseline.kind==o::RelKind::Project
          ? result->logical.nodes.at(baseline.inputs.at(0)) : baseline;
      check(original.kind==o::RelKind::SetOperation && original.inputs.size()==2,
            "set pruning modified the baseline");
      o::RelMetadataQuery metadata(result->logical);
      const auto lineage=metadata.columnOrigins(result->heuristicRoot,0);
      const auto table=result==&leftEmpty?"prune_rhs":"prune_lhs";
      check(lineage && lineage->complete && lineage->origins.size()==1 && lineage->origins.front().table==table,
            "set pruning lost the surviving column origin");
    }
    check(!renamed(plan("(SELECT n FROM prune_lhs LIMIT 0) UNION SELECT n FROM prune_rhs;")),
          "set pruning removed DISTINCT");
    check(!renamed(plan("SELECT n FROM prune_lhs UNION ALL SELECT n FROM prune_rhs;")),
          "set pruning guessed an unproven empty input");
    const auto chain=plan("SELECT n AS kept FROM prune_lhs UNION ALL SELECT n FROM prune_rhs "
                          "UNION ALL SELECT n FROM prune_lhs;");
    const auto merged=std::find_if(chain.rules.begin(),chain.rules.end(),
        [](const auto &event){return event.rule=="UnionMerge";});
    check(chain.ok() && merged!=chain.rules.end(), "compatible UNION ALL did not get a flat alternative");
    const auto &flat=chain.logical.nodes.at(merged->after);
    check(flat.inputs.size()==3 && flat.outputs.front().name=="kept" &&
          chain.logical.nodes.at(merged->before).inputs.size()==2,
          "flat UNION ALL changed the first arm name or baseline shape");
    o::RelMetadataQuery flatMetadata(chain.logical);
    const auto origins=flatMetadata.columnOrigins(merged->after,0);
    check(origins && origins->complete && origins->origins.size()==2,
          "flat UNION ALL lost column provenance across its arms");
    const auto distinctUnion=plan("SELECT n AS kept FROM prune_lhs UNION SELECT n FROM prune_rhs;");
    const auto dedup=std::find_if(distinctUnion.rules.begin(),distinctUnion.rules.end(),
        [](const auto &event){return event.rule=="UnionToDistinct";});
    check(distinctUnion.ok() && dedup!=distinctUnion.rules.end(),
          "typed UNION DISTINCT did not get an aggregate alternative");
    const auto &aggregate=distinctUnion.logical.nodes.at(dedup->after);
    const auto &append=distinctUnion.logical.nodes.at(aggregate.inputs.at(0));
    check(aggregate.kind==o::RelKind::Aggregate && aggregate.distinct && aggregate.keys.size()==1 &&
          aggregate.outputs.at(0).name=="kept" && append.kind==o::RelKind::SetOperation &&
          !append.distinct && append.inputs==distinctUnion.logical.nodes.at(dedup->before).inputs,
          "UNION deduplication changed output shape or inputs");
    o::RelMetadataQuery dedupMetadata(distinctUnion.logical);
    const auto facts=dedupMetadata.properties(dedup->after);
    check(facts && facts->uniqueKeys.size()==1 && facts->uniqueKeys.front().size()==1,
          "complete UNION deduplication lost key evidence");
    check(!fired(plan("SELECT n FROM prune_lhs INTERSECT SELECT n FROM prune_rhs;"),"UnionToDistinct") &&
          !fired(plan("SELECT n FROM prune_lhs UNION SELECT CAST(n AS BIGINT) FROM prune_rhs;"),"UnionToDistinct"),
          "UNION deduplication applied to another set operator or a coercion boundary");
    const auto mixedDistinct=plan("(SELECT n FROM prune_lhs UNION SELECT n FROM prune_rhs) "
                                  "UNION ALL SELECT n FROM prune_lhs;");
    check(mixedDistinct.ok() && !fired(mixedDistinct,"UnionMerge"), "nested DISTINCT boundary was removed");
    const auto branchLimit=plan("(SELECT n FROM prune_lhs UNION ALL SELECT n FROM prune_rhs LIMIT 2) "
                                 "UNION ALL SELECT n FROM prune_lhs;");
    check(branchLimit.ok() && !fired(branchLimit,"UnionMerge"), "nested pagination was moved out of its arm");
    check(!renamed(plan("(SELECT CAST(n AS INT) FROM prune_lhs LIMIT 0) UNION ALL SELECT n FROM prune_rhs;")),
          "set pruning crossed a cast's evaluation boundary");
    integer.bits=64;
    schema.tables[1].columns.front().sqlType=std::make_shared<const c::SqlType>(integer);
    check(!renamed(plan("(SELECT n FROM prune_lhs LIMIT 0) UNION ALL SELECT n FROM prune_rhs;")),
          "set pruning ignored an implicit output type conversion");
    const auto widened=plan("(SELECT n FROM prune_lhs UNION ALL SELECT n FROM prune_rhs) "
                            "UNION ALL SELECT n FROM prune_rhs;");
    check(widened.ok() && !fired(widened,"UnionMerge"), "set flattening lost a narrower arm's coercion");
    const auto typedSet=plan("SELECT n AS kept FROM prune_lhs UNION ALL SELECT n FROM prune_rhs;");
    const auto cast=std::find_if(typedSet.rules.begin(),typedSet.rules.end(),
        [](const auto &event){return event.rule=="CoerceInputs";});
    check(typedSet.ok() && cast!=typedSet.rules.end(),
          "different signed integer widths did not get an explicit conversion");
    const auto &after=typedSet.logical.nodes.at(cast->after);
    const auto &before=typedSet.logical.nodes.at(cast->before);
    const auto &input=typedSet.logical.nodes.at(after.inputs.at(0));
    check(input.kind==o::RelKind::Coerce && input.integerWidenings.size()==1 &&
          input.inputs.front()==before.inputs.front() && after.inputs.at(1)==before.inputs.at(1) &&
          after.outputs.front().name=="kept", "coercion changed a set output or an unaffected arm");
    const auto &target=input.integerWidenings.front();
    check(target.ordinal==0 && typedSet.logical.program.types.get(target.source).bits==32 &&
          typedSet.logical.program.types.get(target.target).bits==64,
          "coercion lost the signed width and output order");
    o::RelMetadataQuery typeLineage(typedSet.logical);
    const auto *castOrigin=typeLineage.columnOrigins(after.inputs.at(0),0);
    check(castOrigin && castOrigin->complete && castOrigin->origins.size()==1 &&
          castOrigin->origins.front().derived && castOrigin->origins.front().table=="prune_lhs",
          "computed widening lost its base column origin");
    check(o::dumpOptimization(typedSet).find("\"fromBits\":32,\"toBits\":64")!=std::string::npos,
          "explicit conversion widths are missing from the plan report");
    check(!fired(plan("SELECT n FROM prune_lhs UNION ALL SELECT CAST(n AS NUMERIC) FROM prune_rhs;"),"CoerceInputs"),
          "a set coercion was inferred across an unsupported numeric type");

    const auto sorted = plan(
        "SELECT n AS saved FROM prune_lhs ORDER BY saved DESC NULLS FIRST;");
    const auto transpose = std::find_if(sorted.rules.begin(), sorted.rules.end(),
        [](const auto &event) { return event.rule == "SortProjectTranspose"; });
    check(sorted.ok() && transpose != sorted.rules.end(),
          "sort over a bound projected alias has no transposed candidate");
    const auto &output = sorted.logical.nodes.at(transpose->after);
    const auto &below = sorted.logical.nodes.at(output.inputs.at(0));
    check(output.kind == o::RelKind::Project && output.outputs.at(0).name == "saved" &&
          below.kind == o::RelKind::Sort && below.outputs.at(0).name == "n" &&
          below.keys.at(0).expression == output.keys.at(0).expression &&
          below.descending.at(0) && below.nullOrdering.at(0) == "FIRST" &&
          sorted.logical.nodes.at(transpose->before).kind == o::RelKind::Sort,
          "sort transposition changed ordering, aliases, or the original plan");
    check(fired(plan("SELECT n AS saved FROM prune_lhs ORDER BY 1;"), "SortProjectTranspose"),
          "ordinal ordering was not mapped back to its input column");
    for (const auto *sql : {
        "SELECT n+1 FROM prune_lhs ORDER BY 1;",
        "SELECT n, random() FROM prune_lhs ORDER BY n;",
        "SELECT DISTINCT n FROM prune_lhs ORDER BY n;",
        "SELECT n AS saved,n AS saved FROM prune_lhs ORDER BY 1;"}) {
      const auto unchanged = plan(sql);
      check(unchanged.ok() && !fired(unchanged, "SortProjectTranspose"),
            "sort transposition accepted an unproved projection");
    }
  }
  {
    const auto query=parse("VALUES (1),(1),(NULL),(2),(3) INTERSECT ALL VALUES (1),(NULL);");
    o::PlannerOptions options;
    options.weights.memory=.25;
    const auto chosen=o::optimize(query,nullptr,options);
    const auto reordered=std::find_if(chosen.rules.begin(),chosen.rules.end(),
        [](const auto &rule){return rule.rule=="IntersectReorder";});
    // Use typed NULL rows in the actual SQL test. These VALUES use integer
    // common types independently on both input arms.
    check(chosen.ok() && reordered!=chosen.rules.end(), "smaller INTERSECT input did not become a candidate");
    const auto &before=chosen.logical.nodes.at(reordered->before);
    const auto &after=chosen.logical.nodes.at(reordered->after);
    check(after.inputs.at(0)==before.inputs.at(1) && after.inputs.at(1)==before.inputs.at(0) &&
          after.outputs.front().name==before.outputs.front().name && after.distinct==before.distinct,
          "INTERSECT reorder changed output identity or multiplicity");
    check(chosen.best.nodes.at(chosen.best.root).total.memory <
          chosen.baseline.nodes.at(chosen.baseline.root).total.memory,
          "reordering INTERSECT did not reduce the selected hash-build allocation");
    const auto except=o::optimize(parse("VALUES (1),(1),(2) EXCEPT ALL VALUES (1);"),nullptr,options);
    check(except.ok() && !fired(except,"IntersectReorder"), "EXCEPT was reordered as if commutative");
  }
  auto qualifiedUnknown=o::optimize(parse("SELECT a.* FROM a JOIN b ON a.id=b.id;"));
  check(qualifiedUnknown.ok() && !fired(qualifiedUnknown,"HepProjectRemove"),"unknown qualified wildcard was treated as an identity projection");
  auto report=o::dumpOptimization(key);
  check(report.find("hepOrder")!=std::string::npos && report.find("HepDistinctRemove")!=std::string::npos,"Hep report provenance absent");
  for(const auto &sql:{"SELECT (SELECT MAX(id) FROM owned_key);","SELECT id FROM owned_key WHERE id IN (SELECT id FROM owned_key);","SELECT (SELECT MAX(id) FROM owned_key) ORDER BY 1;"}) {
    auto nested=o::optimize(parse(sql));check(nested.ok(),"dependent subquery optimization failed");
    o::RelMetadataQuery dependent(nested.logical);auto props=dependent.properties(nested.logical.root);
    check(props && props->mayHaveEffects,"dependent subquery lost its evaluation boundary in metadata");
  }
  // A window / aggregate routine's effect belongs to its relational operator.
  auto effect=o::optimize(parse("SELECT random();"));check(effect.ok(),"volatile call compilation");
  o::RelMetadataQuery volatileMetadata(effect.logical);p=volatileMetadata.properties(effect.logical.root);check(p && p->mayHaveEffects,"volatile expression omitted from metadata");
  auto scan=o::optimize(parse("SELECT * FROM owned_key;"));o::RelMetadataQuery unknown(scan.logical);
  p=unknown.properties(scan.logical.root);check(p && !p->rows.maximum,"assumed statistics became a hard row bound");
  check(!o::optimize(parse("SELECT unknown_call();")).ok(),"unknown routine assumed scalar");
  // All non-trivial normalization rules can operate on manually built plans.
  o::OptimizationResult filters;filters.logical.program=parse("SELECT 1 WHERE 1=1;");filters.logical.nodes.resize(1);auto leaf=append(filters.logical,values);
  const auto &q=filters.logical.program.queries.front();o::RelNode filter;filter.kind=o::RelKind::Filter;filter.query=1;filter.inputs={leaf};filter.predicates={{1,q.where}};
  auto first=append(filters.logical,filter);filter.inputs={first};append(filters.logical,filter);
  auto merged=hep(filters);check(filters.logical.nodes[merged].inputs==std::vector<o::RelId>{leaf} && filters.logical.nodes[merged].predicates.size()==2,"filter merge missing");
  o::OptimizationResult sorts;sorts.logical.program=parse("SELECT 1 ORDER BY 1;");sorts.logical.nodes.resize(1);leaf=append(sorts.logical,values);
  o::RelNode sort;sort.kind=o::RelKind::Sort;sort.query=1;sort.inputs={leaf};sort.keys={{1,sorts.logical.program.queries.front().orderBy.front().expression}};sort.descending={false};sort.nullOrdering={""};
  first=append(sorts.logical,sort);sort.inputs={first};append(sorts.logical,sort);
  o::PlannerOptions mergeOnly;mergeOnly.heuristic.rules={o::HepRule::SortRemove};
  check(hep(sorts,mergeOnly)==first,"sort merge missing");
  // Volatile input must prohibit merging limits; no evaluation-count rewrite.
  auto effects=effect;auto effectRoot=effects.logical.root;limit.inputs={effectRoot};limit.rowLimit=8;limit.rowOffset=0;first=append(effects.logical,limit);limit.inputs={first};auto last=append(effects.logical,limit);
  check(hep(effects)==last,"limit merge changed volatile evaluations");
  auto throwing=o::optimize(parse("SELECT 1/0;"));check(throwing.ok(),"throwing expression planner fixture");
  limit.inputs={throwing.logical.root};first=append(throwing.logical,limit);limit.inputs={first};last=append(throwing.logical,limit);
  check(hep(throwing)==last,"limit merge moved a potentially throwing expression");
  {
    o::LogicalPlan grouping;grouping.program=parse("SELECT id,COUNT(*) FROM t GROUP BY GROUPING SETS((id),(),());");grouping.nodes.resize(1);
    o::RelNode input;input.kind=o::RelKind::Values;input.query=1;input.valueCount=20;
    const auto child=append(grouping,input);
    const auto key=grouping.program.queries[0].groupBy[0];
    o::RelNode aggregate;aggregate.kind=o::RelKind::Aggregate;aggregate.query=1;aggregate.inputs={child};aggregate.keys={{1,key}};aggregate.groupingSets={{{1,key}},{},{}};
    const auto root=append(grouping,aggregate);
    o::RelMetadataQuery proof(grouping);auto props=proof.properties(root);
    check(props && props->rows.minimum==3 && props->rows.maximum==22,"grouping bounds do not sum per set");
    check(props->uniqueKeys.empty() && props->nonNullColumns.empty(),"subtotal/duplicate rows gained a false key or non-null proof");
    grouping.nodes[child].valueCount=0;proof.clear();props=proof.properties(root);
    check(props && props->rows.minimum==2 && props->rows.maximum==2,"grand totals vanish on empty input");
    grouping.nodes[root].groupingSets={{{1,key}}};proof.clear();props=proof.properties(root);
    check(props && props->rows.minimum==0 && props->rows.maximum==0,"nonempty grouping set creates a row on empty input");
    grouping.nodes[root].groupingSets={{},{}};proof.clear();props=proof.properties(root);
    check(props && props->rows.minimum==2 && props->rows.maximum==2 && props->uniqueKeys.empty(),"duplicate grand totals gained uniqueness");
    grouping.nodes[root].groupingSets={{{1,0}}};proof.clear();check(!proof.properties(root) && proof.status()==o::MetadataStatus::InvalidGraph,"invalid metadata grouping reference accepted");
    grouping.nodes[root].groupingSets.resize(257);proof.clear();check(!proof.properties(root) && proof.status()==o::MetadataStatus::ResourceLimit,"metadata grouping budget ignored");
  }
  std::cout<<"PASS relational metadata and Hep proofs, lineage, effects, budgets, original DAG\n";return 0;
}catch(const std::exception&e){std::cerr<<"FAIL metadata/Hep: "<<e.what()<<'\n';return 1;}}
