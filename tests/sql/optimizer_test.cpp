#include "navidog/sql/optimizer/optimizer.hpp"
#include "navidog/sql/optimizer/materialized_view.hpp"
#include <algorithm>
#include <cmath>
#include <iostream>
#include <random>
#include <set>
#include <stdexcept>
using namespace navidog::sql;
namespace sc = compiler;
namespace opt = optimizer;
void check(bool value, const std::string &message) {
  if (!value)
    throw std::runtime_error(message);
}
sc::ModelProgram parse(const std::string &sql) {
  auto c = sc::compile(sc::Dialect::PostgreSQL, sql);
  check(c->ok(), c->diagnostics.empty()
                     ? "invalid SQL"
                     : c->diagnostics.front().message + " in " + sql);
  return c->model;
}
void materializedViewMatching() {
  sc::SqlType nativeInt;
  nativeInt.family=sc::TypeFamily::Integer;
  nativeInt.bits=32;
  nativeInt.unsignedInteger=false;
  const auto type=std::make_shared<const sc::SqlType>(nativeInt);
  sc::TableDefinition base;
  base.database="fixture";base.schema="public";base.name="source_rows";
  base.columns={{"window_key",sc::ValueType::Integer,type},{"row_value",sc::ValueType::Integer,type},
                {"hidden",sc::ValueType::Integer,type}};
  auto storage=base;
  storage.name="mv_rows";
  storage.kind=sc::ObjectKind::MaterializedView;
  storage.columns={{"stored_value",sc::ValueType::Integer,type},{"stored_key",sc::ValueType::Integer,type}};
  sc::MemoryCatalog catalog;
  catalog.tables={base,storage};
  const sc::NameResolutionContext names{"fixture","public",sc::Dialect::PostgreSQL};
  const auto compile=[&](const std::string &sql) {
    auto result=sc::compile(names.dialect,sql,{},&catalog,names);
    check(result->ok(),result->diagnostics.empty()?"invalid materialization fixture":result->diagnostics.front().message);
    return result->model;
  };
  const auto definition=compile("CREATE MATERIALIZED VIEW public.mv_rows (stored_value,stored_key) AS "
                                "SELECT row_value,window_key FROM public.source_rows WHERE window_key>=5;");
  const auto query=compile("SELECT r.row_value AS output_value FROM public.source_rows r "
                           "WHERE r.window_key>=5 AND r.window_key<9;");
  const auto rewrite=opt::rewriteMaterializedView(query,definition,storage,opt::MaterializationSnapshot::Equivalent);
  check(rewrite.ok(),rewrite.diagnostics.empty()?"missing materialized SQL":rewrite.diagnostics.front().message);
  check(rewrite.sql.find("\"stored_value\" AS \"output_value\"")!=std::string::npos &&
        rewrite.sql.find("\"public\".\"mv_rows\"")!=std::string::npos &&
        rewrite.sql.find("\"stored_key\" < 9")!=std::string::npos &&
        rewrite.sql.find(">= 5")==std::string::npos,
        "view substitution missed name mapping or predicate compensation: "+rewrite.sql);
  check(!opt::rewriteMaterializedView(query,definition,storage).ok(),"unchecked materialization was substituted");
  auto present=base;
  present.kind=sc::ObjectKind::MaterializedView;
  present.name="mv_current";
  auto recent=present;
  recent.name="mv_recent";
  const auto currentDefinition=compile(
      "CREATE MATERIALIZED VIEW public.mv_current AS SELECT window_key,row_value,hidden "
      "FROM public.source_rows WHERE window_key>=5;");
  const auto recentDefinition=compile(
      "CREATE MATERIALIZED VIEW public.mv_recent AS SELECT window_key,row_value,hidden "
      "FROM public.source_rows WHERE window_key>=8;");
  const auto selectAll=compile("SELECT * FROM public.source_rows WHERE window_key>=6;");
  const auto candidates=opt::rewriteMaterializedViews(selectAll,{
      {&recentDefinition,&recent,opt::MaterializationSnapshot::Equivalent},
      {&currentDefinition,&present,opt::MaterializationSnapshot::Equivalent},
      {&currentDefinition,&present,opt::MaterializationSnapshot::Unverified}},
      {},opt::MaterializationCoverage::AllowPartial);
  check(candidates.candidates.size()==2 && candidates.diagnostics.empty(),
        "verified materialization registry did not find both coverage levels");
  const auto &complete=candidates.candidates.front(), &partialMatch=candidates.candidates.back();
  check(complete.registration==1 && !complete.compensates &&
        complete.pattern==opt::MaterializationPattern::FilteredScan &&
        complete.sql.find("\"mv_current\"")!=std::string::npos &&
        complete.sql.find("\"hidden\"")!=std::string::npos &&
        partialMatch.registration==0 && partialMatch.compensates &&
        partialMatch.sql.find("UNION ALL")!=std::string::npos,
        "materialized filtered scan lost columns, NULL compensation, or complete-match preference");
  const auto projectMatch=opt::rewriteMaterializedViews(query,{
      {&definition,&storage,opt::MaterializationSnapshot::Equivalent}});
  check(projectMatch.candidates.size()==1 &&
        projectMatch.candidates.front().pattern==opt::MaterializationPattern::ProjectFilter,
        "project/filter match lost its planner evidence");
  for (const auto *ineligible : {
      "SELECT row_value FROM public.source_rows WHERE window_key>=4;",
      "SELECT row_value FROM public.source_rows WHERE window_key>=6 AND hidden=8;",
      "SELECT row_value FROM public.source_rows WHERE window_key IS NULL;",
      "SELECT row_value FROM ONLY public.source_rows WHERE window_key>=5;",
      "SELECT row_value FROM public.source_rows WHERE window_key>=5 OR hidden=7;",
      "SELECT DISTINCT row_value FROM public.source_rows WHERE window_key>=5;"}) {
    check(!opt::rewriteMaterializedView(compile(ineligible),definition,storage,opt::MaterializationSnapshot::Equivalent).ok(),
          "unsafe materialization was accepted for "+std::string(ineligible));
  }
  const auto partial = [&](const sc::ModelProgram &statement, sc::GenerationOptions options = {}) {
    return opt::rewriteMaterializedView(statement, definition, storage,
        opt::MaterializationSnapshot::Equivalent, std::move(options), opt::MaterializationCoverage::AllowPartial);
  };
  const auto overlap = compile(
      "SELECT s.row_value AS result FROM public.source_rows AS s WHERE s.window_key<9;");
  const auto compensated = partial(overlap);
  check(compensated.ok(), compensated.diagnostics.empty() ?
        "missing compensation" : compensated.diagnostics.front().message);
  check(compensated.sql.find("UNION ALL") != std::string::npos &&
        compensated.sql.find("IS NOT TRUE") != std::string::npos &&
        compensated.sql.find("\"stored_key\" < 9") != std::string::npos &&
        compensated.sql.find("\"public\".\"source_rows\"") != std::string::npos,
        "partial materialization produced an invalid split: "+compensated.sql);
  check(partial(compile("SELECT row_value FROM public.source_rows;")).ok(),
        "an unfiltered query cannot use a partial materialization");
  check(!partial(compile("SELECT row_value FROM public.source_rows WHERE window_key<=4;")).ok(),
        "a disjoint materialization was used");
  check(!partial(compile("SELECT row_value FROM public.source_rows WHERE hidden=1 AND hidden=2;")).ok(),
        "an unsatisfiable query got a materialization split");
  check(!partial(compile("SELECT row_value FROM public.source_rows WHERE hidden=8;")).ok(),
        "missing materialized column was ignored in partial mode");
  sc::GenerationOptions narrowOutput;
  narrowOutput.maxBytes = compensated.sql.size() - 1;
  check(!partial(overlap, narrowOutput).ok(), "combined materialized SQL exceeded the output budget");
  auto missingColumn=storage;
  missingColumn.columns.front().name="different_name";
  check(!opt::rewriteMaterializedView(query,definition,missingColumn,opt::MaterializationSnapshot::Equivalent).ok(),
        "wrong materialization layout was accepted");
  sc::GenerationOptions cancelled;
  cancelled.shouldStop=[] { return true; };
  const auto stopped=opt::rewriteMaterializedView(query,definition,storage,opt::MaterializationSnapshot::Equivalent,cancelled);
  check(!stopped.ok() && stopped.sql.empty() && stopped.diagnostics.front().code==sc::DiagnosticCode::Stopped,
        "materialization cancellation published SQL");

  auto grouped = storage;
  grouped.name = "mv_grouped";
  auto countType = nativeInt;
  countType.bits = 64;
  grouped.columns = {{"bucket",sc::ValueType::Integer,type},
                     {"stored_count",sc::ValueType::Integer,std::make_shared<const sc::SqlType>(countType)},
                     {"stored_min",sc::ValueType::Integer,type}};
  catalog.tables.push_back(grouped);
  const auto groupedDefinition = compile(
      "CREATE MATERIALIZED VIEW public.mv_grouped (bucket,stored_count,stored_min) AS "
      "SELECT window_key,COUNT(*),MIN(row_value) FROM public.source_rows "
      "WHERE hidden IS NOT NULL GROUP BY window_key,hidden;");
  const auto rollup = compile(
      "SELECT window_key AS k,COUNT(*),MIN(row_value) AS v FROM public.source_rows "
      "WHERE hidden IS NOT NULL AND window_key>=6 GROUP BY window_key;");
  const auto summarized = opt::rewriteMaterializedView(
      rollup,groupedDefinition,grouped,opt::MaterializationSnapshot::Equivalent);
  check(summarized.ok(), summarized.diagnostics.empty()?"missing materialized rollup":
        summarized.diagnostics.front().message);
  check(summarized.sql.find("pg_catalog.sum(\"stored_count\")")!=std::string::npos &&
        summarized.sql.find("pg_catalog.min(\"stored_min\")")!=std::string::npos &&
        summarized.sql.find("GROUP BY \"bucket\"")!=std::string::npos &&
        summarized.sql.find("\"bucket\" >= 6")!=std::string::npos,
        "materialized rollup lost count/min state or a grouping-key filter");
  const auto groupedCandidates=opt::rewriteMaterializedViews(rollup,{
      {&groupedDefinition,&grouped,opt::MaterializationSnapshot::Equivalent}});
  check(groupedCandidates.candidates.size()==1 &&
        groupedCandidates.candidates.front().pattern==opt::MaterializationPattern::ProjectAggregate,
        "project/aggregate match lost its planner evidence");
  auto peer=base;
  peer.name="peer_rows";
  auto joined=storage;
  joined.name="mv_joined";
  joined.columns={{"lhs",sc::ValueType::Integer,type},{"rhs",sc::ValueType::Integer,type}};
  catalog.tables.push_back(peer);
  catalog.tables.push_back(joined);
  const auto joinedDefinition=compile(
      "CREATE MATERIALIZED VIEW public.mv_joined(lhs,rhs) AS "
      "SELECT a.row_value,b.row_value FROM public.source_rows a JOIN public.peer_rows b "
      "ON a.window_key=b.window_key WHERE b.row_value>=5;");
  const auto eachSide=compile(
      "SELECT p.row_value AS left_value,q.row_value AS right_value "
      "FROM public.source_rows p JOIN public.peer_rows q ON q.window_key=p.window_key "
      "WHERE p.row_value<50 AND q.row_value>=7;");
  const auto joinedSql=opt::rewriteMaterializedView(
      eachSide,joinedDefinition,joined,opt::MaterializationSnapshot::Equivalent);
  check(joinedSql.ok(),joinedSql.diagnostics.empty()?"missing join rewrite":joinedSql.diagnostics.front().message);
  check(joinedSql.sql.find("\"lhs\" < 50")!=std::string::npos &&
        joinedSql.sql.find("\"rhs\" >= 7")!=std::string::npos && joinedSql.sql.find("JOIN")==std::string::npos,
        "overlapping column names in the two sources changed predicate ownership");

}
int main() {
  try {
    materializedViewMatching();
    {
      const auto zero=opt::optimize(parse("SELECT COUNT(*) AS c WHERE FALSE;"));
      check(zero.ok() && zero.best.nodes.at(zero.best.root).kind==opt::PhysicalKind::Values &&
            zero.best.nodes.at(zero.best.root).rows==1,"empty COUNT lost its singleton output");
      const auto count=zero.logical.nodes.at(zero.best.nodes.at(zero.best.root).logical);
      check(count.outputs.size()==1 && count.outputs.front().name=="c" && count.keys.size()==1 &&
            zero.logical.program.front().expressions.at(count.keys.front().expression).value=="0",
            "empty aggregate fold lost result identity or value");
      const auto distinct=opt::optimize(parse(
          "SELECT DISTINCT v.x,v.y FROM (VALUES (1,NULL),(1,NULL),(1,1),(-2,9),(-2,9)) AS v(x,y);"));
      check(distinct.ok() && distinct.best.nodes.at(distinct.best.root).kind==opt::PhysicalKind::Values &&
            distinct.best.nodes.at(distinct.best.root).rows==3 &&
            std::any_of(distinct.rules.begin(),distinct.rules.end(),[](const auto &event) {
              return event.rule=="AggregateValues";
            }),"bounded typed VALUES did not fold DISTINCT rows and NULL duplicates");
      const auto reordered=opt::optimize(parse(
          "SELECT DISTINCT v.y,v.x FROM (VALUES (1,2),(1,2)) AS v(x,y);"));
      check(reordered.ok(),"reordered VALUES failed to plan");
      const auto &physical=reordered.best.nodes.at(reordered.best.root);
      const auto &literal=reordered.logical.nodes.at(physical.logical);
      const auto literalValue=[&](opt::ExpressionRef ref) {
        return reordered.logical.program.queries.at(ref.query-1).expressions.at(ref.expression).value;
      };
      check(physical.kind==opt::PhysicalKind::Values && physical.rows==1 &&
            literal.outputs.size()==2 && literal.outputs[0].name=="y" && literal.outputs[1].name=="x" &&
            literal.keys.size()==2 && literalValue(literal.keys[0])=="2" && literalValue(literal.keys[1])=="1",
            "projection and DISTINCT lost reordered literal positions");
      for (const auto *sql : {"SELECT DISTINCT v.x FROM (VALUES (1),(2147483648),(1)) AS v(x);",
                              "SELECT DISTINCT v.x FROM (VALUES ('x'),('x')) AS v(x);",
                              "SELECT COUNT(*) FROM unverified_table WHERE FALSE;",
                              "SELECT COUNT(1/0) WHERE FALSE;",
                              "SELECT COUNT(*) WHERE TRUE;"}) {
        const auto guarded=opt::optimize(parse(sql));
        check(guarded.ok() && std::none_of(guarded.rules.begin(),guarded.rules.end(),[](const auto &event) {
          return event.rule=="AggregateValues";
        }),"empty aggregate rule folded an unresolved, effectful or nonempty input");
      }
    }

    {
      auto model=parse("SELECT CAST(NULL AS INTEGER);");
      auto &query=model.queries[0];auto &expression=query.expressions[query.projections[0].expression];
      auto syntax=*expression.castType;syntax.modifiers.push_back(std::string(65536,'x'));
      expression.castType=std::make_shared<const sc::BasicTypeSpec<std::vector<std::string>>>(std::move(syntax));
      opt::PlannerOptions bounded;bounded.maxBytes=32768;
      auto rejected=opt::optimize(model,nullptr,bounded);
      check(!rejected.ok(),"structured type syntax bypassed optimizer memory budget");
    }
    {
      auto model=parse("SELECT COUNT(*) OVER(ROWS UNBOUNDED PRECEDING EXCLUDE TIES);");
      auto &window=*model.queries.front().expressions.at(model.front().projections.front().expression).window;
      window.exclusion=static_cast<sc::WindowExclusion>(99);check(!opt::optimize(model).ok(),"invalid window exclusion accepted in public optimizer IR");
      window.exclusion=sc::WindowExclusion::None;window.ordering.push_back({false,std::string(65536,'x')});opt::PlannerOptions bounded;bounded.maxBytes=32768;
      check(!opt::optimize(model,nullptr,bounded).ok(),"window payload escaped optimizer byte budget");
    }
    auto program =
        parse("SELECT a.id,b.value FROM a JOIN b ON a.id=b.a_id WHERE a.id=1;");
    opt::MemoryStatistics stats;
    opt::TableStatistics a;
    a.name = "a";
    a.rows = 100000;
    a.columns = {{"id", 100000, 0, {}}};
    a.indexes = {{"a_pk", {"id"}, true, true, {}}};
    stats.tables.push_back(a);
    auto b = a;
    b.name = "b";
    b.rows = 1000000;
    b.columns = {{"a_id", 100000, 0, {}}};
    b.indexes.clear();
    stats.tables.push_back(b);
    auto r = opt::optimize(program, &stats);
    check(r.ok(),
          r.diagnostics.empty() ? "no plan" : r.diagnostics.front().message);
    check(r.memo.groups > 1 &&
              r.memo.logicalExpressions > r.logical.program.queries.size(),
          "memo not populated");
    check(!r.rules.empty(), "no transformation rules");
    check(r.best.nodes[r.best.root].total.score() <=
              r.baseline.nodes[r.baseline.root].total.score(),
          "optimized plan costs more");
    check(r.logical.program.queries[0].where == program.queries[0].where,
          "source IR mutated");
    auto text = opt::dumpOptimization(r);
    check(text.find("FilterJoinPushDown") != std::string::npos,
          "filter pushdown trace absent");
    check(text.find("Index Scan") != std::string::npos, "index access absent");
    check(text.find("\"configuration\"") != std::string::npos &&
              text.find("\"statistics\"") != std::string::npos &&
              text.find("\"logical\"") != std::string::npos,
          "report lost provenance/configuration/logical graph");
    check(!r.configuration.shouldStop, "result retained a caller callback");
    auto outer = opt::optimize(
        parse("SELECT a.id FROM a LEFT JOIN b ON a.id=b.a_id WHERE a.id=1;"),
        &stats);
    check(outer.ok(), "outer join not represented");
    for (const auto &rule : outer.rules)
      check(rule.rule != "JoinCommute" && rule.rule != "FilterJoinPushDown",
            "outer join illegally transformed");
    {
      const auto values = opt::optimize(parse("VALUES (1, 'some'), (2, NULL), (3, 'other');"));
      check(values.ok() && values.best.nodes.at(values.best.root).rows == 3,
            "VALUES physical cardinality was not exact");
      check(values.logical.nodes.at(values.logical.root).valueCount == 3 &&
            values.logical.nodes.at(values.logical.root).outputs.size() == 2,
            "VALUES plan lost its row or column shape");
      const auto recursive = parse("WITH RECURSIVE seq(n) AS (SELECT 1 UNION ALL "
                                   "SELECT n+1 FROM seq WHERE n<6) SELECT n FROM seq;");
      auto seedNameStats = stats;
      auto unrelated = a;
      unrelated.name = "seq";
      seedNameStats.tables.push_back(unrelated);
      auto result = opt::optimize(recursive, &seedNameStats);
      check(result.ok(), result.diagnostics.empty() ? "recursive plan missing" : result.diagnostics.front().message);
      check(result.statistics.empty(), "working table borrowed statistics from a base table");
      const auto report = opt::dumpOptimization(result);
      check(report.find("Repeat Union") != std::string::npos &&
            report.find("Working Table Scan") != std::string::npos &&
            report.find("\"assumedIterations\":8") != std::string::npos,
            "recursive operator/report lost working table or iteration assumptions");
      for (const auto &node : result.logical.nodes) {
        if (node.kind == opt::RelKind::WorkingTableScan)
          check(node.inputs.empty() && node.workingSeed && node.workingTableOf,
                "working table estimate created a relational cycle");
      }
      auto emptySeed = opt::optimize(parse("WITH RECURSIVE seq(n) AS ((SELECT 1 LIMIT 0) "
                                          "UNION ALL SELECT n+1 FROM seq) SELECT n FROM seq;"));
      check(emptySeed.ok(), emptySeed.diagnostics.empty() ? "empty seed plan absent" : emptySeed.diagnostics.front().message);
      check(emptySeed.best.nodes.at(emptySeed.best.root).rows == 0 &&
            opt::dumpOptimization(emptySeed).find("recursive step is not entered") != std::string::npos,
            "empty recursive seed did not prove a zero-row output");
    }
    {
      const auto compiled = sc::compile(sc::Dialect::SQLServer,
          "SELECT d.id,d.n FROM (VALUES (1,4),(2,8)) AS d(id,n);");
      check(compiled->ok(), "SQL Server derived VALUES did not compile");
      const auto result = opt::optimize(compiled->model);
      check(result.ok() && result.best.nodes.at(result.best.root).rows == 2,
            "SQL Server constructor lost its hard row count");
    }
    auto assoc = opt::optimize(
        parse(
            "SELECT a.id FROM a JOIN b ON a.id=b.a_id JOIN c ON c.id=b.a_id;"),
        &stats);
    check(assoc.ok(), assoc.diagnostics.empty()
                          ? "association failed"
                          : assoc.diagnostics.front().message);
    check(std::any_of(assoc.rules.begin(), assoc.rules.end(),
                      [](const auto &e) { return e.rule == "JoinAssociate"; }),
          "join association absent");
    for (const auto &sql : {
           "SELECT a.id FROM a STRAIGHT_JOIN b ON a.id=b.a_id;",
           "SELECT STRAIGHT_JOIN a.id FROM a JOIN b ON a.id=b.a_id JOIN c ON c.id=b.a_id;"}) {
      auto compiled=sc::compile(sc::Dialect::MySQL,sql);
      check(compiled->ok(),"fixed-order MySQL join did not compile");
      auto fixed=opt::optimize(compiled->model,&stats);
      check(fixed.ok() && std::any_of(fixed.logical.nodes.begin(),fixed.logical.nodes.end(),
              [](const auto &node) { return node.kind==opt::RelKind::Join && node.fixedOrder; }),
            "fixed join order was lost in relational lowering");
      check(std::none_of(fixed.rules.begin(),fixed.rules.end(),
              [](const auto &event) { return event.rule=="JoinCommute" || event.rule=="JoinAssociate"; }),
            "memo reordered an explicitly ordered join");
      check(std::none_of(fixed.best.nodes.begin(),fixed.best.nodes.end(),
              [](const auto &node) { return node.kind==opt::PhysicalKind::HashJoin; }),
            "physical search switched the fixed join build order");
    }
    {
      const auto compiled=sc::compile(sc::Dialect::MySQL,
          R"(SELECT j.x FROM (SELECT '[]' AS doc) d
             LEFT JOIN JSON_TABLE(d.doc,'$[*]' COLUMNS(x INT PATH '$')) j ON 1=0;)");
      check(compiled->ok(),"JSON_TABLE fixture failed compilation");
      const auto result=opt::optimize(compiled->model);
      check(result.ok(),"JSON_TABLE dependent plan missing");
      check(result.statistics.empty(),"JSON table function was looked up as a server table");
      check(result.best.nodes.at(result.best.root).rows==1,"JSON_TABLE LEFT JOIN did not preserve unmatched row");
      const auto report=opt::dumpOptimization(result);
      check(report.find("Table Function Scan")!=std::string::npos &&
            report.find("Correlated Nested Loop")!=std::string::npos,"JSON table relation lost its dependency");
    }
    for (auto clause : {"CROSS APPLY", "OUTER APPLY"}) {
      const auto sql = "SELECT x.id, r.n FROM (SELECT 1 AS id) x " + std::string(clause) +
                       " (SELECT x.id AS n WHERE 1=0) r;";
      auto compiled = sc::compile(sc::Dialect::SQLServer, sql);
      check(compiled->ok(), "APPLY fixture failed compilation");
      auto applied = opt::optimize(compiled->model);
      check(applied.ok(), applied.diagnostics.empty() ? "APPLY plan missing" : applied.diagnostics.front().message);
      check(applied.best.nodes.at(applied.best.root).rows == (std::string(clause)=="CROSS APPLY" ? 0 : 1),
            "APPLY null extension or empty-right cardinality was incorrect");
      check(opt::dumpOptimization(applied).find("Correlated Nested Loop") != std::string::npos,
            "APPLY lost its dependent physical plan");
      check(std::none_of(applied.rules.begin(), applied.rules.end(),
                         [](const auto &rule) { return rule.rule == "JoinCommute"; }),
            "APPLY was reordered as an ordinary join");
    }
    for (auto sql :
         {"SELECT 1;", "SELECT 1 UNION ALL SELECT 2;",
          "SELECT id FROM a INTERSECT SELECT a_id FROM b;",
          "SELECT id FROM a EXCEPT SELECT a_id FROM b;",
          "SELECT COUNT(*) FROM a;", "SELECT DISTINCT id FROM a;",
          "SELECT id FROM a ORDER BY id LIMIT 5;",
          "SELECT (SELECT MAX(id) FROM a);", "INSERT INTO a(id) VALUES(1),(2);",
          "UPDATE a SET id=2 WHERE id=1;", "DELETE FROM a WHERE id=1;"}) {
      auto result = opt::optimize(parse(sql), &stats);
      check(result.ok(), result.diagnostics.empty()
                             ? std::string("no plan: ") + sql
                             : result.diagnostics.front().message);
    }
    for(auto call:{"pg_catalog.sum(id)","\"pg_catalog\".\"sum\"(id)","pg_catalog.count(*)"}) {
      auto aggregate=opt::optimize(parse(std::string("SELECT ")+call+" FROM a;"),&stats);
      check(aggregate.ok(),"qualified builtin aggregate failed");
      check(aggregate.best.nodes.at(aggregate.best.root).rows==1,"qualified aggregate lost single-row cardinality");
      check(std::any_of(aggregate.logical.nodes.begin(),aggregate.logical.nodes.end(),[](const auto& n){return n.kind==opt::RelKind::Aggregate;}),"qualified aggregate lost relational operator");
    }
    auto ordinalOrder=opt::optimize(parse("SELECT id FROM a ORDER BY 1;"),&stats);
    check(ordinalOrder.ok() && opt::dumpOptimization(ordinalOrder).find("required ordering already provided")!=std::string::npos,
          "positional ORDER BY did not reuse index ordering");
    for(auto predicate:{"1=0","NULL=1","NOT(TRUE OR NULL)","TRUE AND FALSE"}) {
      auto constant=opt::optimize(parse(std::string("SELECT id FROM a WHERE ")+predicate+";"),&stats);
      check(constant.ok(),"constant predicate analysis failed");
      check(constant.best.nodes.at(constant.best.root).rows==0,"constant FALSE/UNKNOWN produced rows");
      check(opt::dumpOptimization(constant).find("FilterToEmpty")!=std::string::npos,"constant filter did not create an empty alternative");
      check(std::any_of(constant.advice.begin(),constant.advice.end(),[](const auto &a){return a.kind==opt::AdviceKind::ConstantPredicate;}),"constant filter advice missing");
    }
    auto tautology=opt::optimize(parse("SELECT id FROM a WHERE 1=1;"),&stats);
    check(tautology.ok() && opt::dumpOptimization(tautology).find("FilterRemoveTrue")!=std::string::npos,"tautology was not removed");
    for (const auto *predicate : {"1 = 1 AND 1 = 1", "id > 4 AND 1 = 1 AND 1 = 1"}) {
      const auto sql = std::string("SELECT id FROM a WHERE ") + predicate + ";";
      const auto parsed = sc::compile(sc::Dialect::MySQL, sql);
      check(parsed->ok(), "MySQL duplicate true conditions did not compile");
      const auto result = opt::optimize(parsed->model, &stats);
      check(result.ok() && std::any_of(result.advice.begin(), result.advice.end(),
          [](const auto &a) { return a.kind == opt::AdviceKind::ConstantPredicate; }),
          "Advice missing for MySQL duplicate true conditions: " + sql);
    }
    {
      auto exact = opt::optimize(parse("SELECT 1 WHERE 2 NOT IN(3,NULL);"));
      check(exact.ok() && exact.best.nodes.at(exact.best.root).rows==0 &&
            opt::dumpOptimization(exact).find("FilterToEmpty")!=std::string::npos,
            "NULL-aware exact IN predicate was not reduced");
      auto mixed = opt::optimize(parse(
          "SELECT id FROM a WHERE id>4 AND (CASE WHEN FALSE THEN 0 ELSE 1+2 END) BETWEEN 2 AND 4;"), &stats);
      const auto reduction = std::find_if(mixed.rules.begin(),mixed.rules.end(),[](const auto &rule) {
        return rule.rule=="FilterRemoveTrueConjunct";
      });
      check(mixed.ok() && reduction!=mixed.rules.end() &&
            mixed.logical.nodes.at(reduction->after).predicates.size()==1,
            "true conjunct was not removed independently of the bound filter");
      for (const auto *sql : {"SELECT 1 WHERE 2147483647+1=1;", "SELECT 1 WHERE NULL IN(1,TRUE);",
                              "SELECT 1 WHERE CASE WHEN random()>0 THEN TRUE ELSE FALSE END;"}) {
        const auto unresolved=opt::optimize(parse(sql));
        const auto report=opt::dumpOptimization(unresolved);
        check(unresolved.ok() && report.find("FilterToEmpty")==std::string::npos &&
              report.find("FilterRemoveTrue")==std::string::npos,
              "an overflowing or volatile condition was treated as exact");
      }
    }
    {
      const auto known = sc::compile(sc::Dialect::SQLite, "SELECT 1 WHERE 2 IS TRUE;");
      const auto unresolved = sc::compile(sc::Dialect::SQLite, "SELECT id FROM a WHERE FALSE;");
      check(known->ok() && unresolved->ok(), "SQLite truth predicates did not compile");
      auto rows = opt::optimize(known->model);
      check(rows.ok() && rows.best.nodes.at(rows.best.root).rows == 1 &&
            opt::dumpOptimization(rows).find("FilterRemoveTrue") != std::string::npos,
            "SQLite IS TRUE used equality to integer 1");
      auto unknown = opt::optimize(unresolved->model, &stats);
      check(unknown.ok() && opt::dumpOptimization(unknown).find("FilterToEmpty") == std::string::npos,
            "SQLite optimizer folded a word which may refer to a column");
    }
    opt::PlannerOptions noReduction;noReduction.filterPushdown=false;
    auto unchanged=opt::optimize(parse("SELECT id FROM a WHERE 1=0;"),&stats,noReduction);
    check(unchanged.ok() && opt::dumpOptimization(unchanged).find("FilterToEmpty")==std::string::npos,"disabled filter rules still reduced constants");
    auto effects=opt::optimize(parse("SELECT id FROM a WHERE random()>0 AND FALSE;"),&stats);
    check(effects.ok() && opt::dumpOptimization(effects).find("FilterToEmpty")==std::string::npos,"unknown/volatile predicate was removed");
    opt::MemoryStatistics oracleStats;auto oracleTable=a;oracleTable.name="A";oracleTable.columns={{"ID",100000,0,{}}};oracleTable.indexes.clear();oracleStats.tables.push_back(oracleTable);
    for(auto predicate:{"'' IS NULL","id NOT IN(1,'')"}) {
      auto oracle=sc::compile(sc::Dialect::Oracle,std::string("SELECT id FROM a WHERE ")+predicate+";");check(oracle->ok(),"Oracle null fixture failed");
      auto nulls=opt::optimize(oracle->model,&oracleStats);check(nulls.ok(),"Oracle null analysis failed");
      check(nulls.best.nodes.at(nulls.best.root).rows==(std::string(predicate)=="'' IS NULL"?a.rows:0),"Oracle empty string did not retain NULL semantics");
    }
    auto ordered =
        opt::optimize(parse("SELECT id FROM a ORDER BY id;"), &stats);
    check(ordered.ok(), "ordered trait optimization failed");
    const auto orderedReport = opt::dumpOptimization(ordered);
    check(orderedReport.find("required ordering already provided") !=
                  std::string::npos &&
              orderedReport.find("Index Scan") != std::string::npos,
          "ordering traits did not consider an index with a higher standalone "
          "cost");
    auto selfEqual =
        opt::optimize(parse("SELECT id FROM a WHERE id=id;"), &stats);
    check(selfEqual.ok() &&
              selfEqual.best.nodes[selfEqual.best.root].rows == a.rows,
          "self equality cardinality was treated like two independent columns");
    opt::PlannerOptions smallMemory;
    smallMemory.maxBytes = 65536;
    auto small = opt::optimize(program, &stats, smallMemory);
    check(small.accountedBytes <= 65536,
          "planner space accounting exceeded its cap");
    for (const auto &sql : {"SELECT id AS renamed FROM a ORDER BY renamed;",
                            "SELECT * FROM a ORDER BY id;"}) {
      auto result = opt::optimize(parse(sql), &stats);
      check(result.ok(), "projection ordering failed");
      check(opt::dumpOptimization(result).find(
                "required ordering already provided") != std::string::npos,
            "projection/star alias did not preserve ordering");
    }
    for(const auto &sql:{"SELECT SUM(SUM(id)) OVER() FROM a;",
                         "SELECT ROW_NUMBER() OVER(ORDER BY SUM(id)) FROM a;",
                         "SELECT COUNT(*) OVER() FROM a ORDER BY SUM(id);"}) {
      auto nested=opt::optimize(parse(sql),&stats);
      check(nested.ok(),"aggregate beneath window failed");
      check(std::any_of(nested.logical.nodes.begin(),nested.logical.nodes.end(),
                        [](const auto &n){return n.kind==opt::RelKind::Aggregate;}),
            "query aggregate was hidden by an analytic function/order clause");
      check(nested.best.nodes.at(nested.best.root).rows==1,
            "global aggregate/window cardinality should be one");
    }
    auto analytic=opt::optimize(parse("SELECT SUM(id) OVER() FROM a;"),&stats);
    check(analytic.ok() && analytic.best.nodes.at(analytic.best.root).rows==a.rows,
          "analytic SUM was incorrectly made a global aggregate");
    for(auto predicate:{"TRUE","FALSE"}) {
      auto grouped=opt::optimize(parse(std::string("SELECT 1 FROM a HAVING ")+predicate+";"),&stats);
      check(grouped.ok(),"implicit HAVING group failed");
      check(grouped.best.nodes.at(grouped.best.root).rows==(std::string(predicate)=="TRUE"?1:0),
            "HAVING was discarded or its implicit group was lost");
    }
    auto mysqlHaving=sc::compile(sc::Dialect::MySQL,"SELECT 1 FROM a HAVING TRUE;");
    check(mysqlHaving->ok(),"MySQL row HAVING did not compile");
    auto rowHaving=opt::optimize(mysqlHaving->model,&stats);
    check(rowHaving.ok() && rowHaving.best.nodes.at(rowHaving.best.root).rows==a.rows,
          "MySQL row HAVING was incorrectly changed into a single group");
    auto expressionSort = opt::optimize(
        parse("SELECT SUM(id) AS total FROM a ORDER BY total;"), &stats);
    check(expressionSort.ok(), "computed alias ordering failed");
    {
      opt::MemoryStatistics nullable;
      auto table = a;
      table.rows = 1000;
      table.columns = {{"id", 100, .2, {}}};
      table.indexes.clear();
      nullable.tables.push_back(table);
      auto rows = [&](const std::string &predicate) {
        auto result = opt::optimize(
            parse("SELECT id FROM a WHERE " + predicate + ";"), &nullable);
        check(result.ok(), "NULL selectivity failed");
        return result.best.nodes.at(result.best.root).rows;
      };
      check(std::abs(rows("id<>1") - 792) < .01,
            "inequality included NULL rows");
      check(std::abs(rows("NOT(id=1)") - 792) < .01, "NOT erased UNKNOWN");
      check(rows("NOT NULL") == 0, "NOT NULL produced true rows");
      check(rows("id NOT IN(1,NULL)") == 0, "NOT IN lost NULL list semantics");
      check(std::abs(rows("id IN(1,2)") - 16) < .01,
            "IN non-null fraction failed");
      check(rows("id<>id") == 0, "self inequality produced true rows");
      check(std::abs(rows("id IS DISTINCT FROM NULL")-800)<.01,"DISTINCT NULL lost non-null rows");
      check(std::abs(rows("id IS NOT DISTINCT FROM NULL")-200)<.01,"NOT DISTINCT NULL lost null rows");
      check(rows("id IS DISTINCT FROM id")==0,"self DISTINCT produced true rows");
      check(rows("id IS NOT DISTINCT FROM id")==1000,"self NOT DISTINCT lost null rows");
      check(rows("NOT(id IS DISTINCT FROM id)")==1000,"NOT DISTINCT introduced UNKNOWN");
      check(std::abs(rows("NOT(id IS NULL)") - 800) < .01,
            "IS NULL complement failed");
      std::string chain = "id=1";
      for (int i = 0; i < 600; ++i)
        chain += " OR id=2";
      check(opt::optimize(parse("SELECT id FROM a WHERE " + chain + ";"),
                          &nullable)
                .ok(),
            "deep selectivity did not fall back safely");
    }
    auto projected =
        opt::optimize(parse("SELECT lower(name),COUNT(*) FROM a GROUP BY lower(name);"), &stats);
    check(projected.ok(), "projection advice analysis failed");
    for (const auto &advice : projected.advice)
      check(advice.kind != opt::AdviceKind::StarProjection &&
                advice.kind != opt::AdviceKind::NonSargablePredicate,
            "projection mistaken for filter/star projection");
    auto candidates = opt::optimize(
        parse("SELECT id FROM b WHERE a_id=1 AND amount>2;"), &stats);
    check(candidates.ok() &&
              std::any_of(candidates.advice.begin(), candidates.advice.end(),
                          [](const auto &a) {
                            return a.kind == opt::AdviceKind::IndexCandidate &&
                                   a.title.find("a_id, amount") !=
                                       std::string::npos;
                          }),
          "composite index candidate missing");
    auto disjunction = opt::optimize(
        parse("SELECT id FROM b WHERE a_id=1 OR amount>2;"), &stats);
    check(disjunction.ok(), "OR advice failed");
    for (const auto &advice : disjunction.advice)
      check(advice.kind != opt::AdviceKind::IndexCandidate,
            "OR branches unsafely combined into an index");
    auto indexed = opt::optimize(parse("SELECT id FROM a WHERE id=1;"), &stats);
    for (const auto &advice : indexed.advice)
      check(advice.kind != opt::AdviceKind::IndexCandidate,
            "existing complete key not recognized");
    {
      std::mt19937 random(20261118);
      for (int iteration = 0; iteration < 80; ++iteration) {
        const int count = 1 + random() % 4;
        std::string text = "SELECT a.id FROM a";
        opt::MemoryStatistics fixtures;
        for (int i = 0; i < count; ++i) {
          opt::TableStatistics table;
          table.name = std::string(1, char('a' + i));
          table.rows = random() % 100000;
          table.averageRowBytes = 8 + random() % 1024;
          table.columns = {{"id",
                            double(std::min(100., table.rows)),
                            double(random() % 5) / 10.,
                            {}}};
          if (random() % 2)
            table.indexes = {{table.name + "_id", {"id"}, false, true, {}}};
          fixtures.tables.push_back(table);
          if (i)
            text += " JOIN " + table.name + " ON a.id=" + table.name + ".id";
        }
        text += " WHERE a.id=" + std::to_string(random() % 50);
        if (random() % 2)
          text += " ORDER BY a.id LIMIT " + std::to_string(1 + random() % 100);
        opt::PlannerOptions settings;
        settings.weights = {double(1 + random() % 10),
                            double(1 + random() % 200),
                            double(random() % 10) / 1000.};
        settings.maxRuleMatches = 512;
        auto result = opt::optimize(parse(text), &fixtures, settings);
        check(result.ok(), result.diagnostics.empty()
                               ? "random planner produced no plan"
                               : result.diagnostics[0].message);
        auto bestCost = result.best.nodes.at(result.best.root)
                            .total.score(settings.weights),
             baseCost = result.baseline.nodes.at(result.baseline.root)
                            .total.score(settings.weights);
        check(bestCost <= baseCost + 1e-6,
              "candidate worse than baseline under effective weights");
        std::vector<opt::PlanId> queue{result.best.root};
        std::set<opt::PlanId> seen;
        while (!queue.empty()) {
          auto id = queue.back();
          queue.pop_back();
          check(id > 0 && id < result.best.nodes.size(),
                "bad physical child reference");
          if (!seen.insert(id).second)
            continue;
          const auto &node = result.best.nodes[id];
          check(std::isfinite(node.rows) && node.rows >= 0 &&
                    std::isfinite(node.total.score(settings.weights)),
                "non-finite cost/cardinality");
          queue.insert(queue.end(), node.inputs.begin(), node.inputs.end());
        }
      }
    }
    auto malformed = program;
    malformed.queries.front().rows = {{}};
    malformed.queries.front().rows[0].resize(70000, 1);
    check(!opt::optimize(malformed).ok(),
          "oversized VALUES payload copied without validation");
    opt::PlannerOptions limited;
    limited.maxRuleMatches = 1;
    auto budget = opt::optimize(program, &stats, limited);
    check(budget.ok() && !budget.searchComplete,
          "search limits not reported with a usable baseline");
    opt::PlannerOptions cancelled;
    cancelled.shouldStop = [] { return true; };
    check(!opt::optimize(program, &stats, cancelled).ok(),
          "cancellation ignored");
    opt::MemoryStatistics invalid;
    auto bad = a;
    bad.rows = -1;
    invalid.tables.push_back(bad);
    check(!opt::optimize(program, &invalid).ok(),
          "invalid statistics accepted");
    {
      opt::MemoryStatistics groupedStats;opt::TableStatistics table;
      table.name="a";table.rows=100;table.columns={{"id",10,0,{}}};groupedStats.tables.push_back(table);
      auto grouped=opt::optimize(parse("SELECT id,COUNT(*) FROM a GROUP BY GROUPING SETS ((id),(id),(),());"),&groupedStats);
      check(grouped.ok(),grouped.diagnostics.empty()?"grouping sets no plan":grouped.diagnostics.front().message);
      check(grouped.best.nodes[grouped.best.root].rows==22,"grouping-set duplicates/cardinality lost");
      bool aggregateFound=false;
      for(const auto &node:grouped.logical.nodes)if(node.kind==opt::RelKind::Aggregate && !node.distinct) {
        aggregateFound=true;check(node.groupingSets.size()==4,"logical grouping sets missing");
        check(node.groupingSets[2].empty() && node.groupingSets[3].empty(),"empty grouping sets merged");
      }
      check(aggregateFound,"no grouping aggregate");
      for(const auto &node:grouped.best.nodes)check(node.kind!=opt::PhysicalKind::SortAggregate,"arbitrary sets costed as a single sorted aggregate");
      check(opt::dumpOptimization(grouped).find("groupingSets")!=std::string::npos,"grouping sets not serialized");
      auto nullableSubtotal=opt::optimize(parse("SELECT id,COUNT(*) FROM a GROUP BY ROLLUP(id) HAVING id IS NULL;"),&groupedStats);
      check(nullableSubtotal.ok(),"subtotal HAVING not costed");
      check(nullableSubtotal.best.nodes[nullableSubtotal.best.root].rows>0,"base NULL fraction incorrectly eliminates subtotal rows");
      groupedStats.tables[0].rows=0;groupedStats.tables[0].columns[0].distinct=0;
      auto empty=opt::optimize(parse("SELECT COUNT(*) FROM a GROUP BY CUBE(id);"),&groupedStats);
      check(empty.ok(),empty.diagnostics.empty()?"empty cube no plan":empty.diagnostics.front().message);
      check(empty.best.nodes[empty.best.root].rows==1,"empty cube grand total missing");
      auto duplicateTotals=opt::optimize(parse("SELECT DISTINCT COUNT(*) FROM a GROUP BY GROUPING SETS ((),());"),&groupedStats);
      check(duplicateTotals.ok(),"duplicate totals not planned");
      bool distinctRemains=false;for(const auto &node:duplicateTotals.logical.nodes)if(node.kind==opt::RelKind::Aggregate && node.distinct)distinctRemains=true;
      check(distinctRemains,"DISTINCT over duplicate grand totals lost");
      for(const auto &rule:duplicateTotals.rules)check(rule.rule!="HepDistinctRemove","duplicate totals incorrectly proved unique");
      auto malformed=parse("SELECT id FROM a GROUP BY ROLLUP(id);");malformed.queries[0].groupingSets[0][0]=0;
      check(!opt::optimize(malformed).ok(),"invalid grouping reference accepted");
      malformed=parse("SELECT COUNT(*) FROM a GROUP BY GROUPING SETS(());");malformed.queries[0].groupingSets.resize(257);
      check(!opt::optimize(malformed).ok(),"grouping budget ignored");
    }
    auto unsupported = opt::optimize(parse("CREATE TABLE t(id INTEGER);"));
    check(!unsupported.ok(), "DDL incorrectly given query cost");
    std::cout << "PASS optimizer: relational lowering, memo, rules, costs, "
                 "indexes, barriers, resource limits\n";
    return 0;
  } catch (const std::exception &e) {
    std::cerr << "FAIL optimizer: " << e.what() << '\n';
    return 1;
  }
}
