#include "navidog/sql/optimizer/interpreter.hpp"
#include <algorithm>
#include <iostream>
#include <map>
#include <limits>
#include <stdexcept>

using namespace navidog::sql::compiler;
using namespace navidog::sql::optimizer;

namespace {
void check(bool condition, const std::string &message) {
  if (!condition) throw std::runtime_error(message);
}
Interpretation run(Dialect d, const std::string &sql, InterpreterLimits limits = {}) {
  const auto parsed = compile(d, sql);
  check(parsed->ok(), parsed->diagnostics.empty() ? "invalid fixture" : parsed->diagnostics.front().message);
  return interpret(parsed->model, std::move(limits));
}
void equal(Dialect d, const std::string &sql, const std::vector<LocalRow> &expected) {
  const auto result = run(d, sql);
  check(result.ok(), result.diagnostics.empty() ? "missing result" : result.diagnostics.front().message);
  check(result.rows == expected, "unexpected rows in " + sql + ": " + dumpInterpretation(result));
}
class FixtureCursor final : public RelationCursor {
  const std::vector<LocalRow> &rows;
  const std::vector<std::size_t> &columns;
  std::size_t index = 0;
public:
  FixtureCursor(const std::vector<LocalRow> &data, const std::vector<std::size_t> &projection)
      : rows(data), columns(projection) {}
  std::optional<LocalRow> next() override {
    if (index == rows.size()) return std::nullopt;
    const auto &source=rows.at(index++);
    LocalRow result;
    for (auto column : columns) result.push_back(source.at(column));
    return result;
  }
};
class FixtureProvider final : public RelationProvider {
public:
  std::map<std::string,std::vector<LocalRow>> sources;
  std::vector<std::string> opened;
  std::vector<RelationPredicate> offered;
  std::optional<RelationKeySet> offeredKeys;
  std::vector<std::size_t> projection;
  bool ordered=false, acceptsOrderedFilters=true, receivedPage=false;
  bool supportsOrderedReads(const TableDefinition &) const override { return ordered; }
  bool supportsOrderedFilters(const TableDefinition &,const std::vector<RelationPredicate> &) const override {
    return acceptsOrderedFilters;
  }
  std::unique_ptr<RelationCursor> open(const RelationScanRequest &request) override {
    check(request.maxRows && request.maxBytes && !request.shouldStop(), "scan lacks an operation budget");
    const auto &name=request.table.database;
    opened.push_back(name);
    offered=request.predicates;
    offeredKeys=request.keySet?std::optional<RelationKeySet>(*request.keySet):std::nullopt;
    projection=request.columns;
    receivedPage|=request.page!=nullptr;
    const auto found=sources.find(name);
    return found==sources.end()?nullptr:std::make_unique<FixtureCursor>(found->second,request.columns);
  }
};
void orderedFilterFallback() {
  const auto pg=Dialect::PostgreSQL;
  const auto integer=std::make_shared<const SqlType>(*catalogTypeFromText(pg,"integer"));
  const NameResolutionContext names{"owned","public",pg};
  MemoryCatalog catalog;
  catalog.tables.push_back({"owned","public","items",{{"id",ValueType::Integer,integer},{"filter_key",ValueType::Integer,integer}}});
  const auto compiled=compile(pg,"SELECT id FROM items WHERE filter_key IS NOT NULL ORDER BY id LIMIT 1",{},&catalog,names);
  check(compiled->ok(),"ordered filter fixture did not compile");
  FixtureProvider source;
  source.ordered=true;
  source.acceptsOrderedFilters=false;
  source.sources["owned"]={{std::int64_t{5},{}},{std::int64_t{2},std::int64_t{10}},{std::int64_t{1},std::int64_t{20}}};
  InterpreterLimits limits;
  limits.maxRows=3;
  const auto result=interpretWithRelations(compiled->model,source,limits);
  check(result.ok() && !source.receivedPage && source.offered.size()==1 &&
        result.rows==std::vector<LocalRow>{{std::int64_t{1}}},
        "source that declines ordered predicates was truncated or skipped local sorting");
}
void dynamicPaginationIsNotDropped() {
  for (const auto *sql:{"SELECT 17 LIMIT 1+1;","SELECT 17 LIMIT NULL;","SELECT 17 OFFSET 1-1;"}) {
    const auto parsed=compile(Dialect::PostgreSQL,sql);
    check(parsed->ok(),"dynamic pagination did not compile");
    const auto result=interpret(parsed->model);
    check(!result.ok() && result.rows.empty(),"local evaluation silently discarded a dynamic pagination bound");
  }
  equal(Dialect::PostgreSQL,"SELECT item_id FROM (SELECT 17 AS item_id);",{{std::int64_t{17}}});
  equal(Dialect::PostgreSQL,"SELECT column1 FROM (VALUES(17),(23));",{{std::int64_t{17}},{std::int64_t{23}}});
}
void calciteUnpivotRows() {
  const auto pg=Dialect::PostgreSQL;
  LanguageOptions options;
  options.calciteExtensions=true;
  const std::string base="SELECT u.* FROM (VALUES (1,5,NULL::bigint),(2,NULL,8::bigint)) "
                         "s(id,q1,q2) UNPIVOT ";
  const auto number=[](std::int64_t value) { return LocalValue{value}; };
  const LocalValue null;
  const std::string excluded=base+"(total FOR label IN (q1,q2)) u";
  check(!compile(pg,excluded)->ok(),"Calcite UNPIVOT was accepted on a native PostgreSQL connection");
  const auto parsed=compile(pg,excluded,options);
  check(parsed->ok(),parsed->diagnostics.empty()?"Calcite UNPIVOT parse failed":parsed->diagnostics.front().message);
  const auto rows=interpret(parsed->model);
  check(rows.ok() && rows.rows==std::vector<LocalRow>{{number(1),LocalText("q1"),number(5)},
                                                     {number(2),LocalText("q2"),number(8)}},
        "Calcite UNPIVOT lost default labels or NULL filtering: "+dumpInterpretation(rows));
  const auto included=compile(pg,base+"INCLUDE NULLS (total FOR label IN (q1 AS '',q2 AS NULL)) u",options);
  check(included->ok(),"Calcite UNPIVOT text and NULL label fixture did not compile");
  const auto all=interpret(included->model);
  check(all.ok() && all.rows==std::vector<LocalRow>{{number(1),LocalText(""),number(5)},{number(1),null,null},
                                                    {number(2),LocalText(""),null},{number(2),null,number(8)}},
        "Calcite UNPIVOT conflated empty and NULL labels or dropped a branch");
  InterpreterLimits shortBudget;
  shortBudget.maxRows=3;
  const auto limited=interpret(included->model,shortBudget);
  check(!limited.ok() && limited.rows.empty() && limited.diagnostics.front().code==DiagnosticCode::ResourceLimit,
        "UNPIVOT row budget published a prefix");
  const auto numeric=compile(pg,"SELECT u.* FROM (VALUES (1.25,2.25)) s(q1,q2) "
                                "UNPIVOT (total FOR label IN (q1,q2)) u",options);
  check(numeric->ok() && !interpret(numeric->model).ok(),"unverified noninteger UNPIVOT execution was accepted");
  const std::string tupleBase="WITH src(id,q1,v1,q2,v2) AS (VALUES "
      "(1,1,NULL::bigint,NULL::int,8::bigint),(2,NULL,NULL::bigint,NULL,NULL::bigint),"
      "(3,NULL,7::bigint,4,NULL::bigint)) SELECT u.* FROM src UNPIVOT ";
  const std::string tupleClause="((total,units) FOR (label,period) IN "
      "((q1,v1),(q2,v2) AS (NULL,''))) u";
  const auto tuples=compile(pg,tupleBase+tupleClause,options);
  check(tuples->ok(),tuples->diagnostics.empty()?"Calcite UNPIVOT tuples did not compile":
        tuples->diagnostics.front().message);
  const auto unfolded=interpret(tuples->model);
  check(unfolded.ok() && unfolded.rows==std::vector<LocalRow>{
      {number(1),LocalText("q1_v1"),LocalText("q1_v1"),number(1),null},
      {number(1),null,LocalText(""),null,number(8)},
      {number(3),LocalText("q1_v1"),LocalText("q1_v1"),null,number(7)},
      {number(3),null,LocalText(""),number(4),null}},
      "tuple UNPIVOT changed labels or excluded a branch with one non-NULL measure");
  const auto inclusiveTuples=compile(pg,tupleBase+"INCLUDE NULLS "+tupleClause,options);
  check(inclusiveTuples->ok(),"inclusive Calcite UNPIVOT tuples did not compile");
  const auto allTuples=interpret(inclusiveTuples->model);
  check(allTuples.ok() && allTuples.rows.size()==6,"tuple UNPIVOT dropped all-NULL branches");
  check(!compile(pg,tupleBase+"((total,units) FOR (label,period) IN (q1)) u",options)->ok() &&
        !compile(pg,tupleBase+"((total,units) FOR (label,period) IN ((q1,v1) AS 'only one')) u",options)->ok(),
        "Calcite UNPIVOT accepted a mismatched measure or label width");
}
void calcitePivotIsBoundedAndRejectsUnverifiedComparisons() {
  LanguageOptions options;
  options.calciteExtensions=true;
  const auto pg=Dialect::PostgreSQL;
  const auto groups=compile(pg,"WITH src(g,k) AS (VALUES (1,1),(2,1),(3,NULL)) "
      "SELECT p.* FROM src PIVOT (COUNT(*) FOR k IN (1 a,1 b,NULL c)) p ORDER BY g",options);
  check(groups->ok(),"Calcite PIVOT fixture did not compile");
  const auto complete=interpret(groups->model);
  check(complete.ok() && complete.rows==std::vector<LocalRow>{{std::int64_t{1},std::int64_t{1},std::int64_t{1},std::int64_t{0}},
       {std::int64_t{2},std::int64_t{1},std::int64_t{1},std::int64_t{0}},
       {std::int64_t{3},std::int64_t{0},std::int64_t{0},std::int64_t{0}}},"PIVOT lost duplicated buckets or unmatched group");
  InterpreterLimits budget;
  budget.maxRows=2;
  const auto limited=interpret(groups->model,budget);
  check(!limited.ok() && limited.rows.empty() && limited.diagnostics.front().code==DiagnosticCode::ResourceLimit,
        "PIVOT returned partially allocated groups");
  const auto text=compile(pg,"WITH src(k) AS (VALUES ('A'),('a')) "
                            "SELECT p.* FROM src PIVOT (COUNT(*) FOR k IN ('A' a)) p",options);
  check(text->ok() && !interpret(text->model).ok(),"unverified text-collation comparison was executed locally");
}
void preparedBindings() {
  constexpr auto pg=Dialect::PostgreSQL;
  const auto model=compile(pg,
      "WITH data(k) AS (VALUES (1),(2),(2),(3),(NULL)) "
      "SELECT k,$1::smallint AS delta,k+$1::smallint AS adjusted FROM data "
      "WHERE k >= $2 AND $3 ORDER BY k;");
  check(model->ok(),model->diagnostics.empty()?"invalid prepare fixture":model->diagnostics.front().message);
  const auto prepared=prepareLocalQuery(model->model);
  check(prepared.ok(),prepared.diagnostics.empty()?"local preparation failed":prepared.diagnostics.front().message);
  const auto &parameters=localParameters(*prepared.query);
  check(parameters.size()==3 && parameters[0].position==1 && parameters[0].type.bits==16 &&
        parameters[1].type.bits==32 && parameters[2].type.family==TypeFamily::Boolean,
        "prepared types or parameter positions lost");
  const auto number=[](std::int64_t n){return LocalValue{n};};
  const LocalValue null;
  const auto first=executeLocalQuery(*prepared.query,{number(-3),number(2),true});
  check(first.ok() && first.rows==std::vector<LocalRow>{
      {number(2),number(-3),number(-1)},{number(2),number(-3),number(-1)},{number(3),number(-3),number(0)}},
      "prepared filter, cast or arithmetic changed results "+dumpInterpretation(first));
  const auto second=executeLocalQuery(*prepared.query,{null,number(3),true});
  check(second.ok() && second.rows==std::vector<LocalRow>{{number(3),null,null}},
        "subsequent execution retained a previous value or lost NULL");
  const auto unknown=executeLocalQuery(*prepared.query,{number(5),number(1),null});
  check(unknown.ok() && unknown.rows.empty(),"NULL Boolean binding did not preserve UNKNOWN");

  for (const auto &bad:std::vector<LocalRow>{
      {number(32768),number(2),true}, {true,number(2),true},
      {number(1),number(2),number(1)}, {number(1)}, {number(1),number(2),true,null}}) {
    const auto rejected=executeLocalQuery(*prepared.query,bad);
    check(!rejected.ok() && rejected.rows.empty() &&
        rejected.diagnostics.front().code==DiagnosticCode::InvalidQueryShape,
        "invalid bind set was accepted or returned partial rows");
  }
  auto stop=InterpreterLimits{};
  stop.shouldStop=[] { return true; };
  const auto cancelled=executeLocalQuery(*prepared.query,{number(1),number(2),true},stop);
  check(!cancelled.ok() && cancelled.diagnostics.front().code==DiagnosticCode::Stopped,
        "prepared execution ignored cancellation");
  for (const auto *sql:{"SELECT $1;","SELECT $2::integer;","SELECT $1::integer,$1::bigint;",
                         "SELECT $1025::integer;"}) {
    const auto parsed=compile(pg,sql);
    check(parsed->ok() && !prepareLocalQuery(parsed->model).ok(),
          "unknown, conflicting, sparse or excessive local parameter accepted");
  }
  const auto writable=compile(pg,"DELETE FROM target RETURNING id;");
  check(writable->ok() && !prepareLocalQuery(writable->model).ok(),
        "a read-only prepare accepted a DELETE");

  const auto pageModel=compile(pg,
      "WITH t(n) AS (VALUES (1),(2),(2),(3)) "
      "SELECT n,$1::boolean AS flag FROM t ORDER BY n LIMIT $2 OFFSET $3;");
  check(pageModel->ok(),"invalid prepared pagination fixture");
  const auto page=prepareLocalQuery(pageModel->model);
  check(page.ok(),"failed to prepare pagination");
  check(localParameters(*page.query)[1].type.bits==64 && localParameters(*page.query)[2].type.bits==64,
        "pagination lost bigint parameter types");
  const auto selected=executeLocalQuery(*page.query,{true,number(2),number(1)});
  check(selected.ok() && selected.rows==std::vector<LocalRow>{{number(2),true},{number(2),true}},
        "bound limit and offset failed");
  const auto unlimited=executeLocalQuery(*page.query,{false,null,number(1)});
  check(unlimited.ok() && unlimited.rows==std::vector<LocalRow>{
        {number(2),false},{number(2),false},{number(3),false}}, "NULL limit did not remove the cap");
  const auto unskipped=executeLocalQuery(*page.query,{null,number(1),null});
  check(unskipped.ok() && unskipped.rows==std::vector<LocalRow>{{number(1),null}},
        "NULL offset or Boolean did not retain its semantics");
  const auto largest=number(std::numeric_limits<std::int64_t>::max());
  const auto beyond=executeLocalQuery(*page.query,{true,largest,largest});
  check(beyond.ok() && beyond.rows.empty(),"large page offset did not produce an empty result");
  const auto zero=executeLocalQuery(*page.query,{true,number(0),null});
  check(zero.ok() && zero.rows.empty(),"zero page limit did not produce an empty result");
  for (const auto &args:std::vector<LocalRow>{{true,number(-1),null},{true,null,number(-1)}}) {
    const auto invalid=executeLocalQuery(*page.query,args);
    check(!invalid.ok() && invalid.diagnostics.front().code==DiagnosticCode::InvalidQueryShape,
          "negative bound page size was accepted");
  }
  equal(pg,"SELECT CAST(-32768 AS smallint),CAST(NULL AS bigint);",{{number(-32768),null}});
  check(!run(pg,"SELECT CAST(32768 AS smallint);").ok(),"narrowing cast truncated integer overflow");

  MemoryCatalog catalog;
  SqlType integer;
  integer.family=TypeFamily::Integer;integer.bits=32;integer.unsignedInteger=false;
  TableDefinition table;
  table.database="owned";table.schema="public";table.name="parameter_rows";
  table.columns={{"k",ValueType::Integer,std::make_shared<const SqlType>(integer)}};
  catalog.tables={table};
  const auto scanned=compile(pg,"SELECT k FROM owned.public.parameter_rows WHERE k=$1;",{},&catalog,
                             {"owned","public",pg});
  check(scanned->ok(),"invalid bind fixture with a relation");
  const auto preparedScan=prepareLocalQuery(scanned->model);
  check(preparedScan.ok(),"relation plan was not prepared");
  FixtureProvider provider;
  provider.sources["owned"]={{number(2)},{number(4)}};
  check(!executeLocalQueryWithRelations(*preparedScan.query,{false},provider).ok() &&
        provider.opened.empty(),"invalid bindings contacted a source");
  const auto filtered=executeLocalQueryWithRelations(*preparedScan.query,{number(4)},provider);
  check(filtered.ok() && filtered.rows==std::vector<LocalRow>{{number(4)}},"prepared scan did not filter");
  const auto scalarCalc=compile(pg,
      "SELECT k AS kept,k AS copied FROM owned.public.parameter_rows WHERE k>1 AND k<4;",
      {},&catalog,{"owned","public",pg});
  check(scalarCalc->ok(),"invalid scalar Calc fixture");
  provider.sources["owned"]={{null},{number(2)},{number(2)},{number(4)}};
  const auto compacted=interpretWithRelations(scalarCalc->model,provider);
  check(compacted.ok() && compacted.columns==std::vector<std::string>{"kept","copied"} &&
        compacted.rows==std::vector<LocalRow>{{number(2),number(2)},{number(2),number(2)}},
        "fused filtering and projection changed UNKNOWN, aliases or multiplicity");
  for (const auto *predicate:{"k IN(2,NULL,2,9)","k = ANY(ARRAY[2,9,NULL,2])",
                             "k = SOME(ARRAY[[2,NULL],[2,9]])"}) {
    const auto query=compile(pg,std::string("SELECT k FROM owned.public.parameter_rows WHERE ")+predicate,
                             {},&catalog,{"owned","public",pg});
    check(query->ok(),"invalid static membership scan query");
    const auto result=interpretWithRelations(query->model,provider);
    check(result.ok() && result.rows==std::vector<LocalRow>{{number(2)},{number(2)}},
          "provider ignoring optional keys changed local membership result");
    check(provider.offeredKeys && provider.offeredKeys->columns.size()==1 &&
          provider.offeredKeys->columns.front().width==32 && provider.offeredKeys->keys.size()==2 &&
          provider.offeredKeys->keys[0][0]==2 && provider.offeredKeys->keys[1][0]==9,
          "integer keys lost exact type, values or de-duplication");
  }
  for (const auto *predicate:{"k IN(2,4,NULL) AND k < 3",
                             "k IN(2,4,NULL) AND k = ANY(ARRAY[2,8,NULL])",
                             "k IN(2,4,NULL) AND k+0 = 2"}) {
    const auto query=compile(pg,std::string("SELECT k FROM owned.public.parameter_rows WHERE ")+predicate,
                             {},&catalog,{"owned","public",pg});
    check(query->ok(),"invalid compound membership fixture");
    const auto result=interpretWithRelations(query->model,provider);
    check(result.ok() && result.rows==std::vector<LocalRow>{{number(2)},{number(2)}},
          "compound membership recheck changed duplicate or NULL behavior");
    const std::string text=predicate;
    if (text.find("+")!=std::string::npos) {
      check(!provider.offeredKeys && provider.offered.empty(),"unexpected pushdown beside a nontrivial expression");
    } else if (text.find("ANY")!=std::string::npos) {
      check(provider.offered.empty() && provider.offeredKeys && provider.offeredKeys->keys.size()==1 &&
            provider.offeredKeys->keys.front()[0]==2,"same-column key sets did not intersect");
    } else {
      check(provider.offered.size()==1 && provider.offered.front().kind==RelationPredicateKind::Less &&
            provider.offeredKeys && provider.offeredKeys->keys.size()==2,"range and membership were not both offered");
    }
  }
  const auto disjoint=compile(pg,"SELECT k FROM owned.public.parameter_rows "
                                 "WHERE k IN(2,NULL) AND k = ANY(ARRAY[4,9]);",
                             {},&catalog,{"owned","public",pg});
  check(disjoint->ok(),"invalid disjoint membership fixture");
  const auto noMatches=interpretWithRelations(disjoint->model,provider);
  check(noMatches.ok() && noMatches.rows.empty() && provider.offeredKeys && provider.offeredKeys->keys.empty(),
        "disjoint sets did not offer an empty scan hint");
  const auto disjunction=compile(pg,"SELECT k FROM owned.public.parameter_rows WHERE "
                                     "k=2 OR k IN(9,2,NULL) OR k = ANY(ARRAY[2,NULL]);",
                                 {},&catalog,{"owned","public",pg});
  check(disjunction->ok(),"invalid disjunction fixture");
  const auto joinedKeys=interpretWithRelations(disjunction->model,provider);
  check(joinedKeys.ok() && joinedKeys.rows==std::vector<LocalRow>{{number(2)},{number(2)}} &&
        provider.offeredKeys && provider.offeredKeys->keys.size()==2 &&
        provider.offeredKeys->keys[0][0]==2 && provider.offeredKeys->keys[1][0]==9,
        "same column alternatives did not combine while retaining local duplicates");
  const auto dynamicOr=compile(pg,"SELECT k FROM owned.public.parameter_rows WHERE k=$1 OR k=$2;",
                               {},&catalog,{"owned","public",pg});
  check(dynamicOr->ok(),"invalid bound OR fixture");
  const auto orQuery=prepareLocalQuery(dynamicOr->model);
  check(orQuery.ok(),"bound OR could not be prepared");
  const auto rebound=executeLocalQueryWithRelations(*orQuery.query,{null,number(4)},provider);
  check(rebound.ok() && rebound.rows==std::vector<LocalRow>{{number(4)}} && provider.offeredKeys &&
        provider.offeredKeys->keys.size()==1 && provider.offeredKeys->keys.front()[0]==4,
        "bound NULL or changed key leaked from a previous execution");
  for (const auto *condition:{"k IS NULL OR k=2","k=2 OR k+0=9","k<0 OR k=2"}) {
    const auto fallback=compile(pg,std::string("SELECT k FROM owned.public.parameter_rows WHERE ")+condition,
                                {},&catalog,{"owned","public",pg});
    check(fallback->ok(),"invalid OR fallback fixture");
    check(interpretWithRelations(fallback->model,provider).ok() && !provider.offeredKeys,
          "incomplete OR proof was pushed");
  }
  auto paired=table;
  paired.name="paired_rows";
  paired.columns.push_back({"v",ValueType::Integer,std::make_shared<const SqlType>(integer)});
  catalog.tables.push_back(paired);
  provider.sources["owned"]={{number(2),number(7)},{number(9),number(7)},{number(2),number(8)}};
  const auto independent=compile(pg,"SELECT k FROM owned.public.paired_rows "
                                     "WHERE k IN(2,4) AND v IN(7) AND k >= 2;",
                                 {},&catalog,{"owned","public",pg});
  check(independent->ok(),"invalid independent membership fixture");
  const auto narrow=interpretWithRelations(independent->model,provider);
  check(narrow.ok() && narrow.rows==std::vector<LocalRow>{{number(2)}} &&
        provider.offeredKeys && provider.offeredKeys->columns.front().ordinal==1 &&
        provider.offeredKeys->keys.size()==1 && provider.offeredKeys->keys.front()[0]==7,
        "independent sets did not choose the narrowest hint and recheck the other set");
  const auto different=compile(pg,"SELECT k FROM owned.public.paired_rows WHERE k=2 OR v=7;",
                               {},&catalog,{"owned","public",pg});
  check(different->ok(),"invalid cross-column OR fixture");
  const auto cross=interpretWithRelations(different->model,provider);
  check(cross.ok() && cross.rows.size()==3 && !provider.offeredKeys,
        "different-column disjunction did not use the full local predicate");
  provider.sources["owned"]={{null},{number(2)},{number(2)},{number(4)}};
  for (const auto &[predicate,expectKeys]:std::vector<std::pair<std::string,bool>>{
          {"k = ANY(ARRAY[]::int[])",true},
          {"k = ANY(NULL::int[])",true},
          {"k = ANY('{2,9}'::int[])",false},
          {"k <> ALL(ARRAY[2,9])",false}}) {
    const auto query=compile(pg,"SELECT k FROM owned.public.parameter_rows WHERE "+predicate,
                             {},&catalog,{"owned","public",pg});
    check(query->ok(),"invalid membership boundary query");
    const auto result=interpretWithRelations(query->model,provider);
    check(result.ok() && provider.offeredKeys.has_value()==expectKeys,
          "empty, negated or textual-array key capability changed");
    if (expectKeys)
      check(provider.offeredKeys->keys.empty() && result.rows.empty(),"empty input admitted rows");
  }
  std::string oversized="SELECT k FROM owned.public.parameter_rows WHERE k IN(";
  for (std::size_t i=0;i<=RelationKeySet::maxKeys;++i) {
    if (i) oversized+=',';
    oversized+=std::to_string(i);
  }
  const auto unpushed=compile(pg,oversized+")",{},&catalog,{"owned","public",pg});
  check(unpushed->ok(),"invalid oversized membership fixture");
  const auto unbounded=interpretWithRelations(unpushed->model,provider);
  check(unbounded.ok() && unbounded.rows.size()==3 && !provider.offeredKeys,
        "keys above the provider limit did not fall back to local filtering");
  provider.sources["owned"]={{number(2)},{number(4)}};
  const auto limited=compile(pg,"SELECT k FROM owned.public.parameter_rows LIMIT $1;",{},&catalog,
                             {"owned","public",pg});
  check(limited->ok(),"invalid scan pagination fixture");
  const auto preparedLimit=prepareLocalQuery(limited->model);
  check(preparedLimit.ok(),"failed to prepare scan pagination");
  provider.opened.clear();
  check(!executeLocalQueryWithRelations(*preparedLimit.query,{number(-1)},provider).ok() &&
        provider.opened.empty(),"negative page contacted a source");
}
void localExchangeDropsOnlyProvedConstantKeys() {
  const auto query=compile(Dialect::PostgreSQL,
      "SELECT 7 AS fixed_key,v.k AS hash_key,v.tag FROM "
      "(VALUES (1,1),(2,2),(1,3),(NULL,4),(NULL,5),(-9,6)) AS v(k,tag) ORDER BY 3");
  check(query->ok(),"invalid exchange fixture");
  const auto hash=interpretPartitioned(query->model,{8,{0,1}});
  check(hash.ok() && hash.exchange &&
        hash.exchange->keys==std::vector<std::size_t>{1} &&
        hash.exchange->removedConstantKeys==std::vector<std::size_t>{0} &&
        hash.exchange->offsets.size()==9 && hash.exchange->offsets.back()==6,
        "exchange removed the wrong positions: "+dumpInterpretation(hash));
  const auto original=interpret(query->model);
  auto arranged=hash.rows,expected=original.rows;
  std::sort(arranged.begin(),arranged.end());
  std::sort(expected.begin(),expected.end());
  check(arranged==expected,"exchange lost duplicate or NULL rows");
  std::map<LocalValue,std::size_t> seen;
  for (std::size_t partition=0;partition+1<hash.exchange->offsets.size();++partition)
    for (auto i=hash.exchange->offsets[partition];i<hash.exchange->offsets[partition+1];++i) {
      auto [it,newKey]=seen.emplace(hash.rows[i][1],partition);
      check(newKey || it->second==partition,"equal keys reached different partitions");
    }
  const auto singleton=interpretPartitioned(query->model,{8,{0}});
  check(singleton.ok() && singleton.exchange &&
        singleton.exchange->offsets==std::vector<std::size_t>{0,6} && singleton.rows==original.rows,
        "all-constant exchange did not become a stable singleton");
  const auto filtered=compile(Dialect::PostgreSQL,
      "SELECT v.k FROM (VALUES (1),(2),(1),(NULL)) AS v(k) WHERE v.k=1");
  check(filtered->ok(),"invalid filtered exchange fixture");
  const auto constrained=interpretPartitioned(filtered->model,{4,{0}});
  check(constrained.ok() && constrained.exchange && constrained.exchange->keys.empty() &&
        constrained.exchange->removedConstantKeys==std::vector<std::size_t>{0},
        "exchange did not derive the constant from an exact WHERE equality");
  const auto rejected=interpretPartitioned(query->model,{8,{3}});
  check(!rejected.ok() && rejected.rows.empty() && !rejected.exchange,
        "out-of-range exchange key returned partial data");
}
void federatedScans() {
  SqlType integer;
  integer.family=TypeFamily::Integer;integer.bits=32;integer.unsignedInteger=false;
  SqlType boolean;
  boolean.family=TypeFamily::Boolean;
  TableDefinition left;
  left.database="west";left.schema="public";left.name="records";
  left.columns={{"k",ValueType::Integer,std::make_shared<const SqlType>(integer)}};
  auto right=left;
  right.database="east";
  right.columns.push_back({"flag",ValueType::Boolean,std::make_shared<const SqlType>(boolean)});
  MemoryCatalog catalog;
  catalog.tables={left,right};
  const auto compileQuery=[&](const std::string &query) {
    auto result=compile(Dialect::PostgreSQL,query,{},&catalog,{"west","public",Dialect::PostgreSQL});
    check(result->ok(),result->diagnostics.empty()?"invalid federation query":result->diagnostics.front().message);
    return result->model;
  };
  const auto model=compileQuery("SELECT l.k,r.flag FROM west.public.records l "
                                "LEFT JOIN east.public.records r ON l.k=r.k ORDER BY 1,2;");
  const auto number=[](std::int64_t number){return LocalValue{number};};
  const LocalValue null;
  FixtureProvider provider;
  provider.sources["west"]={{number(2)},{number(1)},{null}};
  provider.sources["east"]={{number(1),true},{number(1),false},{null,true}};
  const auto result=interpretWithRelations(model,provider);
  check(result.ok() && result.rows==std::vector<LocalRow>{{number(1),false},{number(1),true},{number(2),null},{null,null}},
        "federated join lost namespaces, duplicate or NULL semantics: "+dumpInterpretation(result));
  check(provider.opened==std::vector<std::string>{"west","east"}, "unexpected source access");
  const auto filtered=interpretWithRelations(
      compileQuery("SELECT k FROM west.public.records WHERE 2>k AND k IS NOT NULL;"),provider);
  check(filtered.ok() && filtered.rows==std::vector<LocalRow>{{number(1)}} && provider.offered.size()==2 &&
        provider.offered.front().kind==RelationPredicateKind::Less && provider.offered.front().constant==2,
        "a source ignoring exact predicates did not receive a correct local filter");
  const auto disjunction=interpretWithRelations(
      compileQuery("SELECT k FROM west.public.records WHERE k=1 OR k IS NULL;"),provider);
  check(disjunction.ok() && disjunction.rows.size()==2 && provider.offered.empty(),
        "a disjunction was incorrectly sent as independent conjuncts");
  const auto cardinality=interpretWithRelations(
      compileQuery("SELECT COUNT(*) FROM east.public.records;"),provider);
  check(cardinality.ok() && cardinality.rows==std::vector<LocalRow>{{number(3)}} && provider.projection.empty(),
        "cardinality-only scan did not preserve row count");
  const auto onlyFlag=interpretWithRelations(
      compileQuery("SELECT flag FROM east.public.records ORDER BY 1;"),provider);
  check(onlyFlag.ok() && provider.projection==std::vector<std::size_t>{1},
        "scan projection did not preserve a non-first column ordinal");
  check(!interpret(model).ok(), "constant-only evaluation opened a source");
  for (const auto *hint : {"MATERIALIZED","NOT MATERIALIZED"}) {
    provider.opened.clear();
    const auto cte=std::string("WITH shared(k) AS ")+hint+
        " (SELECT k FROM west.public.records) SELECT a.k,b.k FROM shared a "
        "JOIN shared b ON a.k=b.k ORDER BY 1;";
    const auto reused=interpretWithRelations(compileQuery(cte),provider);
    check(reused.ok() && reused.rows==std::vector<LocalRow>{{number(1),number(1)},{number(2),number(2)}},
          "CTE materialization changed result: "+dumpInterpretation(reused));
    check(provider.opened.size()==(std::string(hint)=="MATERIALIZED"?1:2),
          "CTE evaluation directive did not control source reuse");
  }
  InterpreterLimits shortScan;
  shortScan.maxRows=2;
  const auto bounded=interpretWithRelations(model,provider,shortScan);
  check(!bounded.ok() && bounded.rows.empty() && bounded.diagnostics.front().code==DiagnosticCode::ResourceLimit,
        "source truncation produced a partial join");
  provider.sources["east"].push_back({number(2147483648LL),true});
  const auto overflow=interpretWithRelations(model,provider);
  check(!overflow.ok() && overflow.rows.empty() && overflow.diagnostics.front().code==DiagnosticCode::InvalidQueryShape,
        "provider values ignored the declared integer width");
  provider.sources.erase("east");
  const auto missing=interpretWithRelations(model,provider);
  check(!missing.ok() && missing.rows.empty(), "missing source produced output");
  auto &westRows=provider.sources["west"];
  auto &eastRows=provider.sources["east"];
  westRows.clear();
  for (std::int64_t i=0;i<1024;++i) {
    westRows.push_back({number(i)});
    eastRows.push_back({number(i+1), i % 2 == 0});
  }
  westRows.push_back({null});
  eastRows.push_back({null,true});
  eastRows.push_back({number(4),true});
  const auto fullJoin=compileQuery("SELECT l.k,r.k AS east_key,r.flag FROM west.public.records l "
      "FULL JOIN east.public.records r ON r.k=l.k ORDER BY 1,2,3;");
  InterpreterLimits budget;
  budget.maxSteps=150000;
  const auto indexed=interpretWithRelations(fullJoin,provider,budget);
  check(indexed.ok(),indexed.diagnostics.empty()?"no indexed result":indexed.diagnostics.front().message);
  check(indexed.ok() && indexed.rows.size()==1028 &&
        indexed.rows.front()==LocalRow{number(0),null,null} &&
        indexed.rows.at(indexed.rows.size()-2)==LocalRow{null,null,true} &&
        indexed.rows.back()==LocalRow{null,null,null},
        "indexed integer join lost duplicate or unmatched rows");

  const auto otherKey=ColumnDefinition{"subkey",ValueType::Integer,std::make_shared<const SqlType>(integer)};
  catalog.tables[0].columns.push_back(otherKey);
  catalog.tables[1].columns.push_back(otherKey);
  westRows.clear();
  eastRows.clear();
  for (std::int64_t i=0;i<1024;++i) {
    westRows.push_back({number(1),number(i)});
    eastRows.push_back({number(1),true,number(i+1)});
  }
  eastRows.push_back({number(1),false,number(1)});
  westRows.push_back({number(1),null});
  eastRows.push_back({number(1),true,null});
  const auto compositeJoin=compileQuery(
      "SELECT l.subkey AS west_key,r.subkey AS east_key,r.flag FROM west.public.records l "
      "FULL JOIN east.public.records r ON r.k=l.k AND l.subkey=r.subkey ORDER BY 1,2,3;");
  const auto composite=interpretWithRelations(compositeJoin,provider,budget);
  check(composite.ok(),composite.diagnostics.empty()?"no composite result":composite.diagnostics.front().message);
  check(composite.rows.size()==1028 && composite.rows[0]==LocalRow{number(0),null,null} &&
        composite.rows[1]==LocalRow{number(1),number(1),false} &&
        composite.rows[2]==LocalRow{number(1),number(1),true} &&
        composite.rows.at(composite.rows.size()-3)==LocalRow{null,number(1024),true} &&
        composite.rows.at(composite.rows.size()-2)==LocalRow{null,null,true} &&
        composite.rows.back()==LocalRow{null,null,null},
        "composite integer join lost duplicate, unmatched, or partially NULL keys");

  westRows.resize(22);
  westRows.push_back({number(1),null});
  const auto asymmetric=compileQuery(
      "SELECT l.subkey AS small_key,r.subkey AS large_key FROM west.public.records l "
      "JOIN east.public.records r ON l.k=r.k AND l.subkey=r.subkey ORDER BY 1,2;");
  InterpreterLimits tight;
  tight.maxBytes=320*1024;
  tight.maxSteps=25000;
  const auto smallBuild=interpretWithRelations(asymmetric,provider,tight);
  check(smallBuild.ok(),smallBuild.diagnostics.empty()?"no asymmetric hash result":smallBuild.diagnostics.front().message);
  check(smallBuild.rows.size()==22 && smallBuild.rows[0]==LocalRow{number(1),number(1)} &&
        smallBuild.rows[1]==LocalRow{number(1),number(1)} &&
        smallBuild.rows.back()==LocalRow{number(21),number(21)},
        "smaller hash input changed the output columns, duplicates, or NULL matches");
}
} // namespace

int main() {
  try {
    orderedFilterFallback();
    dynamicPaginationIsNotDropped();
    calciteUnpivotRows();
    calcitePivotIsBoundedAndRejectsUnverifiedComparisons();
    federatedScans();
    preparedBindings();
    localExchangeDropsOnlyProvedConstantKeys();
    constexpr auto pg = Dialect::PostgreSQL, sqlite = Dialect::SQLite;
    const std::monostate null;
    const auto number = [](std::int64_t n) { return LocalValue{n}; };
    equal(sqlite, "SELECT 17 IN (), NULL IN (), NULL NOT IN (), 17 NOT IN ();",
          {{number(0), number(0), number(1), number(1)}});
    equal(sqlite, "SELECT 3 + NULL ISNULL, 17 NOTNULL, NULL NOT NULL, NOT NULL ISNULL;",
          {{number(1), number(1), number(0), number(0)}});
    equal(sqlite, "SELECT 1 WHERE NULL NOT IN ();", {{number(1)}});
    equal(sqlite, "SELECT 1 WHERE NULL IN ();", {});
    equal(sqlite, "SELECT 2 BETWEEN 0=0 AND 3,2 BETWEEN 0 AND 3=1,2 BETWEEN 0 AND (3=1);",
          {{number(1),number(1),number(0)}});
    equal(sqlite, "SELECT 2=0 IN (0),2 BETWEEN 0 AND 3 IN (1);",{{number(1),number(1)}});
    equal(sqlite,"SELECT SUM(ALL 17),COUNT(ALL);",{{number(17),number(1)}});
    equal(sqlite,"SELECT COUNT(),COUNT(ALL) FROM (VALUES(17),(23),(NULL));",{{number(3),number(3)}});
    equal(sqlite,"SELECT COUNT() FROM (SELECT 17 AS id WHERE 0);",{{number(0)}});
    equal(sqlite,"SELECT COUNT() FILTER(WHERE id>17) FROM (SELECT 17 AS id UNION ALL SELECT 23);",{{number(1)}});
    equal(sqlite,"SELECT COUNT() OVER (ORDER BY id) FROM (SELECT 17 AS id UNION ALL SELECT 23);",{{number(1)},{number(2)}});
    equal(pg,"SELECT SUM(ALL 17),COUNT(ALL 17);",{{number(17),number(1)}});
    equal(sqlite, "SELECT * FROM (VALUES (17), (23));", {{number(17)}, {number(23)}});
    equal(sqlite, "SELECT item_id + 1 FROM (SELECT 17 AS item_id);", {{number(18)}});
    equal(sqlite, "SELECT item_id,event_id FROM (SELECT 17 AS item_id) "
                  "CROSS JOIN (SELECT 23 AS event_id);", {{number(17), number(23)}});
    equal(pg, "SELECT NULL ISNULL,17 NOTNULL,NULL NOTNULL,NOT NULL ISNULL;",
          {{true, true, false, false}});
    equal(pg, "SELECT 3 + NULL ISNULL,3 + 5 NOTNULL;", {{true, true}});
    equal(pg, "SELECT;", {LocalRow{}});
    equal(pg, "SELECT WHERE FALSE;", {});
    equal(pg, "SELECT FROM (VALUES (17), (23)) AS q(item_id);", {LocalRow{},LocalRow{}});
    for (auto dialect:{pg,sqlite}) {
      equal(dialect,"WITH t(id,k) AS (VALUES(1,4),(2,NULL),(3,-3),(4,7),(5,2)) "
                    "SELECT id,MIN(k) OVER (ORDER BY id ROWS BETWEEN 1 PRECEDING AND 1 FOLLOWING), "
                    "MAX(k) FILTER (WHERE k<0) OVER (ORDER BY id ROWS BETWEEN 1 PRECEDING AND "
                    "1 FOLLOWING EXCLUDE CURRENT ROW) FROM t ORDER BY id;",
            {{number(1),number(4),null},{number(2),number(-3),number(-3)},{number(3),number(-3),null},
             {number(4),number(-3),number(-3)},{number(5),number(2),null}});
      equal(dialect,"WITH t(id,v) AS (VALUES(1,99),(2,77)) SELECT id,MIN(v) OVER "
                    "(ROWS BETWEEN CURRENT ROW AND CURRENT ROW EXCLUDE CURRENT ROW) FROM t ORDER BY id;",
            {{number(1),null},{number(2),null}});
    }
    {
      InterpreterLimits budget;
      budget.maxSteps=300000;
      const auto sliding=run(pg,"SELECT i,MIN(i) OVER w,MAX(i) OVER w FROM "
          "pg_catalog.generate_series(1,3000) d(i) WINDOW w AS (ORDER BY i ROWS BETWEEN "
          "1000 PRECEDING AND 1000 FOLLOWING) ORDER BY i;",budget);
      check(sliding.ok() && sliding.rows.size()==3000 &&
            sliding.rows[0]==LocalRow{number(1),number(1),number(1001)} &&
            sliding.rows[1499]==LocalRow{number(1500),number(500),number(2500)} &&
            sliding.rows.back()==LocalRow{number(3000),number(2000),number(3000)},
            "local MIN/MAX did not stay within the moving frame work budget");
      check(!run(pg,"SELECT MIN(v) OVER () FROM (VALUES (1.2),(3.4)) d(v);").ok(),
            "unsupported decimal MIN window was treated as a signed integer");
    }
    equal(pg,"WITH t(id,v,d,fallback) AS (VALUES(1,10,1,-1),(2,20,0,-2),(3,NULL,-1,-3),"
             "(4,40,NULL,-4),(5,50,6,-5),(6,NULL,0,-6)) "
             "SELECT id,LAG(v,d,fallback) OVER w,LEAD(v,d,fallback) OVER w FROM t "
             "WINDOW w AS (ORDER BY id) ORDER BY id;",
          {{number(1),number(-1),number(20)},{number(2),number(20),number(20)},
           {number(3),number(40),number(20)},{number(4),null,null},
           {number(5),number(-5),number(-5)},{number(6),null,null}});
    equal(pg,"SELECT id,LAG(v,CAST(-2147483648 AS integer),99) OVER (ORDER BY id) "
             "FROM (VALUES(1,5),(2,7)) d(id,v) ORDER BY id;",
          {{number(1),number(99)},{number(2),number(99)}});
    equal(pg,"SELECT id,LAG(v,NULL,99) OVER (ORDER BY id) FROM (VALUES(1,5),(2,7)) d(id,v) ORDER BY id;",
          {{number(1),null},{number(2),null}});
    equal(pg,"SELECT mode() WITHIN GROUP (ORDER BY v),mode(*) WITHIN GROUP (ORDER BY v DESC),"
             "mode() WITHIN GROUP (ORDER BY v) FILTER (WHERE false) "
             "FROM (VALUES (5),(3),(5),(3),(NULL)) d(v);",{{number(3),number(5),null}});
    equal(pg,"SELECT mode() WITHIN GROUP (ORDER BY v) FROM (VALUES (4)) d(v) WHERE false;",{{null}});
    equal(pg,"SELECT percentile_disc(0) WITHIN GROUP (ORDER BY v),"
             "percentile_disc(0.5) WITHIN GROUP (ORDER BY v DESC),"
             "percentile_disc(1) WITHIN GROUP (ORDER BY v),"
             "percentile_disc(NULL) WITHIN GROUP (ORDER BY v) "
             "FROM (VALUES (1),(2),(8),(8),(NULL)) d(v);",{{number(1),number(8),number(8),null}});
    equal(pg,"SELECT percentile_disc(2) WITHIN GROUP (ORDER BY v) "
             "FROM (VALUES (4)) d(v) WHERE false GROUP BY v;",{});
    check(!run(pg,"SELECT percentile_disc(2) WITHIN GROUP (ORDER BY v) "
                  "FROM (VALUES (4)) d(v) WHERE false;").ok(),
          "empty global percentile skipped invalid fraction evaluation");
    equal(pg, "SELECT 1+2 AS n, TRUE AND NULL AS u, FALSE AND NULL AS f, "
              "TRUE OR NULL AS t, NOT NULL AS n2, NULL IS NOT DISTINCT FROM NULL AS nd;",
          {{number(3), null, false, true, null, true}});
    equal(pg,"SELECT u.item,u.pos FROM pg_catalog.unnest(CAST('[0:2]={-8,NULL,-8}' AS integer[])) "
             "WITH ORDINALITY AS u(item,pos) ORDER BY u.pos;",
          {{number(-8),number(1)},{null,number(2)},{number(-8),number(3)}});
    equal(pg,"SELECT v,n FROM pg_catalog.unnest(ARRAY[-8,NULL,2147483648]) WITH ORDINALITY AS u(v,n) ORDER BY n;",
          {{number(-8),number(1)},{null,number(2)},{number(2147483648),number(3)}});
    equal(pg,"SELECT v FROM pg_catalog.unnest(ARRAY[]::smallint[]) AS u(v);",{});
    equal(pg,"SELECT v,n FROM pg_catalog.unnest(ARRAY[[1,2],[NULL,-3]]) WITH ORDINALITY AS u(v,n) ORDER BY n;",
          {{number(1),number(1)},{number(2),number(2)},{null,number(3)},{number(-3),number(4)}});
    equal(pg,"SELECT v FROM pg_catalog.unnest(ARRAY[[],[]]::smallint[]) AS u(v);",{});
    equal(pg,"SELECT v,n FROM pg_catalog.unnest(ARRAY(SELECT v FROM (VALUES (1),(2),(NULL)) AS s(v) "
             "ORDER BY v DESC NULLS LAST)) WITH ORDINALITY AS u(v,n) ORDER BY n;",
          {{number(2),number(1)},{number(1),number(2)},{null,number(3)}});
    equal(pg,"SELECT v FROM pg_catalog.unnest(ARRAY(SELECT 1 WHERE FALSE)) AS u(v);",{});
    equal(pg,"SELECT i,(ARRAY[1,2])[i] AS v FROM (VALUES (1),(2),(0),(3),(NULL)) s(i) "
             "ORDER BY i NULLS LAST;",{{number(0),null},{number(1),number(1)},{number(2),number(2)},
                                     {number(3),null},{null,null}});
    equal(pg,"SELECT (ARRAY[[1,2],[3,NULL]])[2][1], (ARRAY[[1,2],[3,NULL]])[2][2], "
             "(ARRAY[[1,2],[3,NULL]])[2], ('[-2:0]={1,2,3}'::int[])[-1], "
             "(ARRAY[1,2])[1.5], (NULL::smallint[])[1], (ARRAY[]::smallint[])[1];",
          {{number(3),null,null,number(2),number(2),null,null}});
    equal(pg,"SELECT g.v,g.n FROM pg_catalog.generate_series(7,2,-2) WITH ORDINALITY AS g(v,n) ORDER BY n",
          {{number(7),number(1)},{number(5),number(2)},{number(3),number(3)}});
    equal(pg,"SELECT g FROM pg_catalog.generate_series(NULL::integer,2,0) g",{});
    equal(pg,"SELECT g FROM pg_catalog.generate_series(3,2) g",{});
    equal(pg,"SELECT g FROM pg_catalog.generate_series(9223372036854775806::bigint,9223372036854775807::bigint,2) g",
          {{number(9223372036854775806)}});
    equal(pg,"SELECT a.id,g.v,g.n FROM (VALUES (1),(2)) a(id) LEFT JOIN LATERAL pg_catalog.generate_series(2,a.id) "
             "WITH ORDINALITY AS g(v,n) ON TRUE ORDER BY a.id,g.n",
          {{number(1),null,null},{number(2),number(2),number(1)}});
    equal(pg,"SELECT 2 = ANY(ARRAY[1,2,NULL]), 2 <> ALL(ARRAY[1,NULL]), "
             "2 = ANY(ARRAY[1,NULL]), 2 >= ALL(ARRAY[[1,NULL],[2,3]]), 2 > SOME(ARRAY[1,NULL]);",
          {{true,null,null,false,true}});
    equal(pg,"SELECT 2 = ANY(NULL::int[]), NULL::int = ANY(ARRAY[]::int[]), "
             "NULL::int <> ALL(ARRAY[]::int[]), 2 = ANY(ARRAY[]::int[]), 2.5 > ALL(ARRAY[1,2]);",
          {{null,false,true,false,true}});
    equal(pg,"SELECT v,v = ANY(ARRAY[1,3,NULL]) FROM (VALUES (1),(2),(NULL)) s(v) ORDER BY v NULLS LAST;",
          {{number(1),true},{number(2),null},{null,null}});
    equal(pg,"SELECT a.id,u.item,u.pos FROM (VALUES (1),(2)) a(id) CROSS JOIN LATERAL pg_catalog.unnest("
             "ARRAY(SELECT v FROM (VALUES (1),(2),(3)) s(v) WHERE v<=a.id ORDER BY v DESC)) "
             "WITH ORDINALITY AS u(item,pos) ORDER BY a.id,u.pos;",
          {{number(1),number(1),number(1)},{number(2),number(2),number(1)},{number(2),number(1),number(2)}});
    equal(pg, "SELECT 1 WHERE FALSE AND NULL;", {});
    equal(pg, "SELECT 1 WHERE TRUE AND NULL;", {});
    equal(pg, "SELECT 1 WHERE TRUE OR NULL;", {{number(1)}});
    equal(pg, "SELECT (1=1) IS NOT FALSE, TRUE IS FALSE, NULL IS UNKNOWN, NULL IS NOT TRUE, "
              "2 BETWEEN SYMMETRIC 3 AND 1, 0 NOT BETWEEN SYMMETRIC 3 AND 1, "
              "-1 BETWEEN 1 AND NULL, 2 BETWEEN 1 AND NULL, "
              "3 IN (1,NULL), 1 IN (NULL,1), 3 NOT IN (1,NULL);",
          {{true, false, true, true, true, true, false, null, null, true, null}});
    equal(sqlite, "SELECT -1 BETWEEN 1 AND NULL, 2 BETWEEN 1 AND NULL, "
                  "3 IN (1,NULL), 1 IN (NULL,1), 3 NOT IN (1,NULL);",
          {{number(0), null, null, number(1), null}});
    const auto membership = "WITH l(k) AS (VALUES (1),(2),(3),(NULL)), "
                            "r(k) AS (VALUES (2),(NULL),(2)) "
                            "SELECT k,k IN(SELECT k FROM r),k NOT IN(SELECT k FROM r),"
                            "NULL NOT IN(SELECT k FROM r WHERE FALSE) FROM l "
                            "WHERE EXISTS(SELECT 1 FROM r WHERE k=2) ORDER BY k NULLS LAST;";
    equal(pg, membership, {{number(1), null, null, true}, {number(2), true, false, true},
                           {number(3), null, null, true}, {null, null, null, true}});
    equal(sqlite, membership, {{number(1), null, null, number(1)}, {number(2), number(1), number(0), number(1)},
                               {number(3), null, null, number(1)}, {null, null, null, number(1)}});
    for (auto dialect : {pg, sqlite}) {
      equal(dialect, "SELECT (SELECT (SELECT 4)+1), (SELECT 1 WHERE FALSE);", {{number(5), null}});
      equal(dialect, "WITH t(n) AS (VALUES (2),(1)) "
                     "SELECT CASE WHEN FALSE THEN (SELECT n FROM t) ELSE 8 END;", {{number(8)}});
    }
    const auto multiRowScalar = run(pg, "WITH t(n) AS (VALUES (2),(1)) SELECT (SELECT n FROM t);");
    check(!multiRowScalar.ok() && multiRowScalar.rows.empty(), "PostgreSQL accepted a multirow scalar subquery");
    equal(sqlite, "WITH t(n) AS (VALUES (2),(1)) SELECT (SELECT n FROM t ORDER BY n);", {{number(1)}});
    equal(pg,"WITH t(n) AS (VALUES (1),(2)) SELECT n,(SELECT n+1) FROM t ORDER BY n;",
          {{number(1),number(2)},{number(2),number(3)}});
    equal(pg,"SELECT t.n,(SELECT (SELECT t.n+10)) FROM (VALUES (1),(2)) t(n) ORDER BY t.n;",
          {{number(1),number(11)},{number(2),number(12)}});
    equal(pg, "SELECT CASE WHEN FALSE THEN 2147483647+1 WHEN NULL THEN 9 ELSE 7 END AS x, "
              "CASE NULL WHEN NULL THEN 1 ELSE 2 END AS y, CASE 2 WHEN 1 THEN 9 END AS z;",
          {{number(7), number(2), null}});
    equal(pg, "WITH t(n) AS (VALUES (1),(2),(3),(NULL)) "
              "SELECT n, (CASE n WHEN 1 THEN (CASE WHEN n=1 THEN 10 ELSE 0 END) "
              "WHEN 2 THEN 20 ELSE 30 END)+1 AS result FROM t ORDER BY n;",
          {{number(1), number(11)}, {number(2), number(21)}, {number(3), number(31)}, {null, number(31)}});
    equal(sqlite, "SELECT CASE WHEN 2 THEN 5 ELSE 0 END, CASE 1 WHEN TRUE THEN 6 ELSE 0 END;",
          {{number(5), number(6)}});
    equal(pg, "WITH t(x) AS (VALUES (3),(1),(1),(NULL),(2)) "
              "SELECT DISTINCT x,x+1 AS successor FROM t WHERE x>1 OR x IS NULL "
              "ORDER BY successor NULLS LAST LIMIT 3 OFFSET 1;",
          {{number(3), number(4)}, {null, null}});
    equal(pg, "WITH t(x) AS (VALUES (3),(1),(NULL)) SELECT x FROM t ORDER BY x;",
          {{number(1)}, {number(3)}, {null}});
    equal(pg, "WITH t(x) AS (VALUES (3),(1),(NULL)) SELECT x FROM t ORDER BY 1 DESC;",
          {{null}, {number(3)}, {number(1)}});
    equal(pg, "WITH t(x) AS (VALUES (1),(1),(2),(2),(3)) "
              "SELECT x FROM t ORDER BY x OFFSET 1 ROW FETCH NEXT 2 ROWS WITH TIES;",
          {{number(1)}, {number(2)}, {number(2)}});
    equal(pg, "WITH t(x) AS (VALUES (2),(NULL),(NULL),(1)) "
              "SELECT x FROM t ORDER BY x NULLS FIRST FETCH FIRST ROW WITH TIES OFFSET 0;",
          {{null}, {null}});
    equal(pg, "WITH t(x) AS (VALUES (1),(1),(2)) SELECT x FROM t ORDER BY x FETCH FIRST 0 ROWS WITH TIES;", {});
    equal(sqlite, "WITH t(x) AS (VALUES (3),(1),(NULL)) SELECT x,TRUE AS flag FROM t ORDER BY 1;",
          {{null, number(1)}, {number(1), number(1)}, {number(3), number(1)}});
    equal(sqlite, "WITH t(x) AS (VALUES (3),(1),(NULL)) SELECT x FROM t ORDER BY 1 DESC;",
          {{number(3)}, {number(1)}, {null}});
    equal(sqlite, "SELECT TRUE AS yes WHERE 2;", {{number(1)}});
    equal(sqlite, "SELECT 2 IS TRUE, 2 IS +TRUE, 2=TRUE, 1 IS NULL < 2, "
                  "NULL IS DISTINCT FROM FALSE, 0 IS NOT DISTINCT FROM TRUE, 2==2;",
          {{number(1), number(0), number(0), number(0), number(1), number(0), number(1)}});
    equal(sqlite, "WITH t(\"true\") AS (VALUES (7)) SELECT TRUE, 2 IS TRUE, t.column1 FROM t;",
          {{number(1), number(1), number(7)}});
    equal(sqlite, "SELECT TRUE, 2 IS TRUE, t.column1 FROM (SELECT 7 AS TRUE) t;",
          {{number(1), number(1), number(7)}});
    equal(sqlite, "SELECT 2147483647+1 AS n;", {{number(2147483648LL)}});
    for (const auto dialect : {pg, sqlite}) {
      equal(dialect, "SELECT -9223372036854775808, -4294967296*2147483648, "
                     "2147483648+1, -7/3, -7%3, -9223372036854775808 % -1;",
            {{number(std::numeric_limits<std::int64_t>::min()),
              number(std::numeric_limits<std::int64_t>::min()), number(2147483649LL),
              number(-2), number(-1), number(0)}});
      for (const auto *sql : {
          "SELECT 9223372036854775807+1;", "SELECT -9223372036854775808-1;",
          "SELECT -9223372036854775808/-1;", "SELECT -2*-9223372036854775808;"}) {
        const auto rejected = run(dialect, sql);
        check(!rejected.ok() && rejected.rows.empty(),
              std::string("signed arithmetic overflow published values: ") + sql);
      }
    }
    equal(pg, "SELECT -(-2147483648), -(-(-9223372036854775808)), "
              "CASE WHEN FALSE THEN 1/0 ELSE 9 END;",
          {{number(2147483648LL), number(std::numeric_limits<std::int64_t>::min()), number(9)}});
    const auto pgZero = run(pg, "SELECT 1/0;");
    check(!pgZero.ok() && pgZero.rows.empty() &&
          pgZero.diagnostics.front().message.find("division by zero") != std::string::npos,
          "PostgreSQL divide by zero did not fail");
    equal(sqlite, "SELECT 1/0, 2%0, NULL/0;", {{null, null, null}});

    const auto operands = std::string{"WITH l(x) AS (VALUES (1),(1),(1),(2),(NULL),(NULL)), "}
        + "r(x) AS (VALUES (1),(2),(NULL)) SELECT x FROM l ";
    equal(pg, operands + "EXCEPT ALL SELECT x FROM r ORDER BY 1 NULLS FIRST;",
          {{null}, {number(1)}, {number(1)}});
    equal(pg, operands + "INTERSECT ALL SELECT x FROM r ORDER BY 1 DESC NULLS LAST;",
          {{number(2)}, {number(1)}, {null}});
    equal(pg, operands + "UNION SELECT x FROM r ORDER BY 1;",
          {{number(1)}, {number(2)}, {null}});
    equal(sqlite, operands + "INTERSECT SELECT x FROM r ORDER BY 1;",
          {{null}, {number(1)}, {number(2)}});
    const std::string multicolumn="WITH big(a,b) AS (VALUES (2,3),(2,3),(2,3),(NULL,3),(1,NULL),(NULL,NULL)), "
                           "small(a,b) AS (VALUES (2,3),(2,3),(NULL,3),(NULL,NULL)) ";
    equal(pg, multicolumn+"SELECT a,b FROM big INTERSECT ALL SELECT a,b FROM small ORDER BY 1,2;",
          {{number(2),number(3)},{number(2),number(3)},{null,number(3)},{null,null}});
    equal(pg, multicolumn+"SELECT a,b FROM small INTERSECT ALL SELECT a,b FROM big ORDER BY 1,2;",
          {{number(2),number(3)},{number(2),number(3)},{null,number(3)},{null,null}});
    equal(pg, multicolumn+"SELECT a,b FROM big EXCEPT ALL SELECT a,b FROM small ORDER BY 1,2;",
          {{number(1),null},{number(2),number(3)}});

    equal(pg, "SELECT x.n FROM (SELECT 2 AS n) x;", {{number(2)}});
    equal(pg, "SELECT d.n FROM (VALUES (2),(1),(NULL)) AS d(n) ORDER BY d.n;",
          {{number(1)}, {number(2)}, {null}});
    const auto joinInputs = std::string{"WITH l(id) AS (VALUES (1),(2),(NULL)), "}
        + "r(id) AS (VALUES (2),(3),(NULL)) SELECT l.id AS left_id,r.id AS right_id FROM l ";
    equal(pg, joinInputs + "INNER JOIN r ON l.id=r.id ORDER BY 1,2;", {{number(2), number(2)}});
    equal(pg, joinInputs + "LEFT JOIN r ON l.id=r.id ORDER BY 1,2;",
          {{number(1), null}, {number(2), number(2)}, {null, null}});
    equal(pg, joinInputs + "RIGHT JOIN r ON l.id=r.id ORDER BY 1,2;",
          {{number(2), number(2)}, {null, number(3)}, {null, null}});
    equal(pg, joinInputs + "FULL JOIN r ON l.id=r.id ORDER BY 1,2;",
          {{number(1), null}, {number(2), number(2)}, {null, number(3)}, {null, null}, {null, null}});
    equal(sqlite, joinInputs + "LEFT JOIN r ON l.id=r.id AND r.id>2 ORDER BY 1,2;",
          {{null, null}, {number(1), null}, {number(2), null}});
    equal(pg, "SELECT a.x,b.y FROM (SELECT 7 AS x) a CROSS JOIN (SELECT 9 AS y) b;",
          {{number(7), number(9)}});
    equal(pg, "SELECT a.x,b.y FROM (SELECT 7 AS x WHERE FALSE) a RIGHT JOIN (SELECT 9 AS y) b ON TRUE;",
          {{null, number(9)}});
    const auto usingJoin = run(pg, joinInputs + "JOIN r USING (id);");
    check(!usingJoin.ok() && usingJoin.rows.empty(), "local join ignored USING coalescing semantics");
    equal(pg, "WITH RECURSIVE seq(n) AS (SELECT 1 UNION ALL SELECT n+1 FROM seq WHERE n<4) "
              "SELECT n FROM seq ORDER BY n DESC;", {{number(4)}, {number(3)}, {number(2)}, {number(1)}});
    equal(pg, "WITH RECURSIVE seq(n,k) AS (VALUES (1,7),(1,NULL),(1,NULL) UNION SELECT 3-n,k FROM seq) "
              "SELECT n,k FROM seq ORDER BY n,k NULLS LAST;",
          {{number(1), number(7)}, {number(1), null}, {number(2), number(7)}, {number(2), null}});
    equal(pg, "WITH RECURSIVE seq(n) AS ((SELECT 1 WHERE FALSE) UNION ALL SELECT n+1 FROM seq) "
              "SELECT n FROM seq;", {});
    equal(pg, "WITH t(k,n) AS (VALUES (1,10),(1,10),(1,20),(1,NULL),(2,NULL),(NULL,99)) "
              "SELECT k,COUNT(*),COUNT(n),COUNT(DISTINCT n),SUM(n) FILTER (WHERE n>10),MIN(n),MAX(n) "
              "FROM t GROUP BY k HAVING COUNT(*)>=1 ORDER BY 1;",
          {{number(1), number(4), number(3), number(2), number(20), number(10), number(20)},
           {number(2), number(1), number(0), number(0), null, null, null},
           {null, number(1), number(1), number(1), number(99), number(99), number(99)}});
    for (auto d : {pg, sqlite}) {
      equal(d, "WITH t(k) AS (SELECT 1 WHERE FALSE) SELECT COUNT(*),SUM(k),MIN(k),MAX(k) FROM t;",
            {{number(0), null, null, null}});
      equal(d, "WITH t(k) AS (SELECT 1 WHERE FALSE) SELECT k, COUNT(*) FROM t GROUP BY k;", {});
      equal(d, "WITH t(k,n) AS (VALUES (1,2),(1,3),(2,4)) "
               "SELECT k, CASE WHEN COUNT(*) > 1 THEN SUM(n) ELSE 0 END AS score FROM t GROUP BY 1 "
               "HAVING MIN(n)>2 ORDER BY 1;", {{number(2), number(0)}});
    }
    const auto bare = run(sqlite, "WITH t(k,n) AS (VALUES (1,2),(1,3)) SELECT k,n,COUNT(*) FROM t GROUP BY k;");
    check(!bare.ok() && bare.rows.empty(), "local aggregate guessed an ungrouped SQLite column");
    const auto subtotals = "WITH t(a,b,n) AS (VALUES (1,10,5),(1,10,7),(1,NULL,3),(2,20,2)) "
        "SELECT a,b,GROUPING(a,b) AS mask,COUNT(*),SUM(n) FROM t "
        "GROUP BY GROUPING SETS((a,b),(a),(),()) ORDER BY mask,a,b NULLS LAST;";
    equal(pg, subtotals,
          {{number(1), number(10), number(0), number(2), number(12)},
           {number(1), null, number(0), number(1), number(3)},
           {number(2), number(20), number(0), number(1), number(2)},
           {number(1), null, number(1), number(3), number(15)},
           {number(2), null, number(1), number(1), number(2)},
           {null, null, number(3), number(4), number(17)},
           {null, null, number(3), number(4), number(17)}});
    equal(pg, "WITH t(k) AS (SELECT 1 WHERE FALSE) SELECT k,GROUPING(k),COUNT(*) "
              "FROM t GROUP BY GROUPING SETS((k),(),()) ORDER BY 1;", {{null,number(1),number(0)}, {null,number(1),number(0)}});
    equal(pg, "WITH t(k) AS (VALUES (1),(1),(2)) SELECT k,GROUPING(k) AS g,COUNT(*),"
              "RANK() OVER(ORDER BY COUNT(*) DESC) AS place FROM t GROUP BY ROLLUP(k) "
              "HAVING GROUPING(k)>0 OR COUNT(*)>1 ORDER BY place;",
          {{null,number(1),number(3),number(1)}, {number(1),number(0),number(2),number(2)}});
    InterpreterLimits groupingLimit;
    groupingLimit.maxRows = 4;
    const auto tooManyGroups = run(pg, subtotals, groupingLimit);
    check(!tooManyGroups.ok() && tooManyGroups.rows.empty() &&
          tooManyGroups.diagnostics.front().code==DiagnosticCode::ResourceLimit,
          "grouping sets published partial results beyond the combined budget");
    const auto sumOverflow = run(sqlite,
        "WITH t(n) AS (VALUES (9223372036854775807),(1)) SELECT SUM(n) FROM t;");
    check(!sumOverflow.ok() && sumOverflow.rows.empty(), "local SUM overflow returned a partial result");
    const auto ranking = "WITH t(team,id,n) AS (VALUES (1,11,5),(1,10,5),(1,12,2),"
                         "(2,20,NULL),(2,21,7),(NULL,99,9)) "
                         "SELECT team,id,ROW_NUMBER() OVER(PARTITION BY team ORDER BY n DESC NULLS LAST,id) AS pos, "
                         "RANK() OVER w AS sparse,DENSE_RANK() OVER w AS dense FROM t "
                         "WINDOW w AS(PARTITION BY team ORDER BY n DESC NULLS LAST) ORDER BY team NULLS LAST,pos;";
    for (auto d : {pg, sqlite}) {
      equal(d, ranking, {{number(1), number(10), number(1), number(1), number(1)},
                         {number(1), number(11), number(2), number(1), number(1)},
                         {number(1), number(12), number(3), number(3), number(2)},
                         {number(2), number(21), number(1), number(1), number(1)},
                         {number(2), number(20), number(2), number(2), number(2)},
                         {null, number(99), number(1), number(1), number(1)}});
      equal(d, "WITH t(n) AS (VALUES (2),(1),(NULL)) SELECT n,RANK() OVER(ORDER BY 1), "
               "DENSE_RANK() OVER() FROM t ORDER BY 1 NULLS FIRST;",
            {{null, number(1), number(1)}, {number(1), number(1), number(1)}, {number(2), number(1), number(1)}});
      equal(d, "WITH t(n) AS (SELECT 1 WHERE FALSE) SELECT ROW_NUMBER() OVER(ORDER BY n) FROM t;", {});
      equal(d, "WITH d(k,n) AS (VALUES (1,3),(1,4),(2,7),(3,2),(4,NULL)) "
               "SELECT k,SUM(n) AS total,RANK() OVER(ORDER BY SUM(n) DESC NULLS LAST) AS place "
               "FROM d GROUP BY k HAVING COUNT(*)>0 ORDER BY place,k;",
            {{number(1), number(7), number(1)}, {number(2), number(7), number(1)},
             {number(3), number(2), number(3)}, {number(4), null, number(4)}});
      equal(d, "WITH d(k,n) AS (VALUES (1,8),(2,6),(3,2)) "
               "SELECT k,RANK() OVER(ORDER BY SUM(n) DESC) AS place FROM d "
               "GROUP BY k HAVING SUM(n)<8 ORDER BY place,k;",
            {{number(2), number(1)}, {number(3), number(2)}});
      equal(d, "WITH t(i,k,n) AS (VALUES (1,1,5),(2,1,NULL),(3,2,4),(4,3,6),(5,3,2)) "
               "SELECT i,SUM(n) OVER (ORDER BY k),COUNT(*) OVER (ORDER BY i ROWS BETWEEN 1 PRECEDING AND 1 FOLLOWING), "
               "COUNT(n) FILTER (WHERE n>4) OVER (ORDER BY k GROUPS BETWEEN 1 PRECEDING AND CURRENT ROW), "
               "SUM(n) OVER (ORDER BY i ROWS BETWEEN 1 FOLLOWING AND 2 FOLLOWING),COUNT(n) OVER() FROM t ORDER BY i;",
            {{number(1), number(5), number(2), number(1), number(4), number(4)},
             {number(2), number(5), number(3), number(1), number(10), number(4)},
             {number(3), number(9), number(3), number(1), number(8), number(4)},
             {number(4), number(17), number(3), number(1), number(2), number(4)},
             {number(5), number(17), number(2), number(1), null, number(4)}});
      equal(d, "WITH t(i,k,n) AS (VALUES (1,1,5),(2,1,7),(3,2,4),(4,NULL,NULL)) SELECT i,"
               "COUNT(*) OVER (ORDER BY k NULLS LAST ROWS BETWEEN UNBOUNDED PRECEDING AND UNBOUNDED FOLLOWING EXCLUDE CURRENT ROW),"
               "SUM(n) OVER (ORDER BY k NULLS LAST ROWS BETWEEN UNBOUNDED PRECEDING AND UNBOUNDED FOLLOWING EXCLUDE GROUP),"
               "SUM(n) OVER (ORDER BY k NULLS LAST ROWS BETWEEN UNBOUNDED PRECEDING AND UNBOUNDED FOLLOWING EXCLUDE TIES),"
               "SUM(n) OVER (ORDER BY k NULLS LAST RANGE UNBOUNDED PRECEDING EXCLUDE GROUP) FROM t ORDER BY i;",
            {{number(1),number(3),number(4),number(9),null}, {number(2),number(3),number(4),number(11),null},
             {number(3),number(3),number(12),number(16),number(12)}, {number(4),number(3),number(16),number(16),number(16)}});
      equal(d, "WITH t(k,n) AS (VALUES (1,5),(2,7),(3,NULL)) SELECT k,"
               "COUNT(*) OVER(ORDER BY k ROWS BETWEEN 9223372036854775807 PRECEDING AND 2 PRECEDING),"
               "SUM(n) OVER(ORDER BY k RANGE BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING) FROM t ORDER BY k;",
            {{number(1), number(0), number(12)}, {number(2), number(0), number(7)}, {number(3), number(1), null}});
      equal(d, "WITH t(i,k,n) AS (VALUES (1,NULL,9),(2,-3,2),(3,-2,3),(4,-2,5),(5,0,7),(6,2,11),(7,NULL,13)) "
               "SELECT i,SUM(n) OVER (ORDER BY k NULLS LAST RANGE BETWEEN 1 PRECEDING AND 1 FOLLOWING),"
               "SUM(n) OVER (ORDER BY k DESC NULLS FIRST RANGE BETWEEN 2 PRECEDING AND 0 FOLLOWING EXCLUDE TIES),"
               "SUM(n) OVER (ORDER BY k NULLS FIRST RANGE BETWEEN UNBOUNDED PRECEDING AND 1 PRECEDING) "
               "FROM t ORDER BY i;",
            {{number(1),number(22),number(9),number(22)}, {number(2),number(10),number(10),number(22)},
             {number(3),number(10),number(10),number(24)}, {number(4),number(10),number(12),number(24)},
             {number(5),number(7),number(18),number(32)}, {number(6),number(11),number(11),number(39)},
             {number(7),number(22),number(13),number(22)}});
      equal(d, "WITH t(k) AS (VALUES (-2147483647-1),(-1),(0),(2147483647)) SELECT k,"
               "COUNT(*) OVER(ORDER BY k DESC RANGE BETWEEN 2147483647 PRECEDING AND 2147483647 FOLLOWING) "
               "FROM t ORDER BY k;",
            {{number(-2147483648LL),number(2)}, {number(-1),number(3)}, {number(0),number(3)},
             {number(2147483647),number(2)}});
      equal(d, "WITH t(i,k,n) AS (VALUES (1,1,NULL),(2,1,7),(3,2,4),(4,3,6),(5,3,2)) "
               "SELECT i,NTILE(3) OVER(ORDER BY i),NTILE(9) OVER(PARTITION BY k ORDER BY i),"
               "FIRST_VALUE(n) OVER(ORDER BY i ROWS BETWEEN 1 PRECEDING AND CURRENT ROW EXCLUDE CURRENT ROW),"
               "LAST_VALUE(n) OVER(ORDER BY i ROWS BETWEEN 1 PRECEDING AND 1 FOLLOWING EXCLUDE CURRENT ROW),"
               "NTH_VALUE(n,2) OVER(ORDER BY i RANGE BETWEEN 1 PRECEDING AND 1 FOLLOWING EXCLUDE CURRENT ROW) "
               "FROM t ORDER BY i;",
            {{number(1),number(1),number(1),null,number(7),null},
             {number(2),number(1),number(2),null,number(4),number(4)},
             {number(3),number(2),number(1),number(7),number(6),number(6)},
             {number(4),number(2),number(1),number(4),number(2),number(2)},
             {number(5),number(3),number(2),number(6),number(6),null}});
      equal(d, "WITH t(k,n) AS (VALUES (1,NULL),(1,7),(2,4)) SELECT k,n,"
               "NTH_VALUE(n,2) OVER(ORDER BY k RANGE BETWEEN CURRENT ROW AND CURRENT ROW EXCLUDE TIES),"
               "FIRST_VALUE(n) OVER(ORDER BY k,n NULLS FIRST) FROM t ORDER BY k,n NULLS FIRST;",
            {{number(1),null,null,null}, {number(1),number(7),null,null}, {number(2),number(4),null,null}});
      equal(d, "WITH t(i,g,n,fallback) AS (VALUES (1,1,2,101),(2,1,NULL,102),(3,1,7,103),(4,2,9,104)) "
               "SELECT i,LAG(n,1,fallback) OVER w,LEAD(n,2,fallback) OVER w,LAG(n,0) OVER w,LEAD(n) OVER w "
               "FROM t WINDOW w AS (PARTITION BY g ORDER BY i) ORDER BY i;",
            {{number(1),number(101),number(7),number(2),null},
             {number(2),number(2),number(102),null,number(7)},
             {number(3),null,number(103),number(7),null},
             {number(4),number(104),number(104),number(9),null}});
    }
    equal(pg, "WITH t(k,n) AS (VALUES (1,5),(1,6),(2,7),(3,NULL)) SELECT k,SUM(n),"
              "SUM(SUM(n)) OVER(ORDER BY k),COUNT(*) FILTER (WHERE k<2) OVER() FROM t GROUP BY k ORDER BY k;",
          {{number(1), number(11), LocalNumeric(11), number(1)}, {number(2), number(7), LocalNumeric(18), number(1)},
           {number(3), null, LocalNumeric(18), number(1)}});
    equal(sqlite, "WITH t(k,n) AS (VALUES (1,5),(1,6),(2,7),(3,NULL)) SELECT k,SUM(n),"
                  "SUM(SUM(n)) OVER(ORDER BY k),COUNT(*) FILTER (WHERE SUM(n)>7) OVER() FROM t GROUP BY k ORDER BY k;",
          {{number(1), number(11), number(11), number(1)}, {number(2), number(7), number(18), number(1)},
           {number(3), null, number(18), number(1)}});
    equal(pg,"SELECT LAG(1,-1) OVER(ORDER BY 1);",{{null}});

    equal(pg,"SELECT 1 UNION ALL SELECT 1.5;",{{LocalNumeric(1)},{*LocalNumeric::parse("1.5")}});
    equal(pg,"SELECT AVG(n) FROM (SELECT 1 AS n) s;",{{*LocalNumeric::parse("1.00000000000000000000")}});
    equal(pg,"SELECT 1.5::numeric(3,1);",{{*LocalNumeric::parse("1.5")}});
    for (const auto *sql : {"SELECT 2147483647+1;", "SELECT 2147483647+1 WHERE FALSE;",
                            "SELECT RANDOM() WHERE FALSE;", "SELECT n FROM server_table;",
                            "SELECT n FROM (SELECT 1 AS n, 2 AS hidden) s ORDER BY hidden;",
                            "SELECT SUM(1) OVER(ORDER BY 2147483648 RANGE 1 PRECEDING);",
                            "SELECT NTILE(0) OVER(ORDER BY 1);",
                            "SELECT NTH_VALUE(2,0) OVER(ORDER BY 1);",
                            "SELECT 1; SELECT 2;", "DELETE FROM server_table;"}) {
      const auto result = run(pg, sql);
      check(!result.ok() && result.rows.empty() && result.columns.empty(),
            std::string{"unsupported execution published output: "} + sql);
    }
    const auto columns = run(pg, "SELECT 1 AS \"a\"\"b\", TRUE AS flag, NULL AS n;");
    check(columns.columns == std::vector<std::string>{"a\"b", "flag", "n"},
          "output names were not preserved");
    check(dumpInterpretation(columns).find("[1,true,null]") != std::string::npos,
          "JSON output lost exact integers, Boolean or NULL");

    InterpreterLimits maxRows;
    maxRows.maxRows = 2;
    const auto large = run(pg, "VALUES (1),(2),(3);", maxRows);
    check(!large.ok() && large.rows.empty() && large.diagnostics.front().code == DiagnosticCode::ResourceLimit,
          "row limit did not fail atomically");
    const auto product = run(pg, "WITH l(x) AS (VALUES (1),(2)), r(y) AS (VALUES (8),(9)) "
                                 "SELECT x,y FROM l CROSS JOIN r LIMIT 1;", maxRows);
    check(!product.ok() && product.rows.empty() && product.diagnostics.front().code == DiagnosticCode::ResourceLimit,
          "oversized intermediate join leaked partial rows");
    InterpreterLimits iterationBudget;
    iterationBudget.maxIterations = 2;
    const auto cycles = run(pg, "WITH RECURSIVE seq(n) AS (SELECT 1 UNION ALL SELECT n FROM seq) "
                                "SELECT n FROM seq;", iterationBudget);
    check(!cycles.ok() && cycles.rows.empty() && cycles.diagnostics.front().code == DiagnosticCode::ResourceLimit,
          "recursive UNION ALL reached its bound without an atomic failure");
    InterpreterLimits maxWork;
    maxWork.maxSteps = 1;
    const auto expensive = run(pg, "SELECT 1 + 2;", maxWork);
    check(!expensive.ok() && expensive.rows.empty() && expensive.diagnostics.front().code == DiagnosticCode::ResourceLimit,
          "work limit did not fail atomically");
    InterpreterLimits cancelled;
    cancelled.shouldStop = [] { return true; };
    const auto stopped = run(pg, "SELECT 1;", cancelled);
    check(!stopped.ok() && stopped.rows.empty() && stopped.diagnostics.front().code == DiagnosticCode::Stopped,
          "cancellation returned a partial result");
  } catch (const std::exception &e) {
    std::cerr << e.what() << '\n';
    return 1;
  }
  return 0;
}
