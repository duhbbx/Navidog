#include "navidog/sql/compiler/conversion.hpp"
#include "navidog/sql/compiler/formatter.hpp"
#include "navidog/sql/compiler/script_context.hpp"
#include <algorithm>
#include <future>
#include <iostream>
#include <stdexcept>
using namespace navidog::sql::compiler;
void require(bool b, const char *msg) {
  if (!b)
    throw std::runtime_error(msg);
}
ConversionResult good(Dialect a, Dialect b, const std::string &s,
                      ConversionOptions o = {}) {
  auto r = convertSql(a, b, s, o);
  if (!r.ok())
    throw std::runtime_error(r.diagnostics.empty()
                                 ? "no conversion"
                                 : r.diagnostics.front().message+" in "+s);
  require(!r.sql.empty(), "empty success");
  return r;
}
void bad(Dialect a, Dialect b, const std::string &s, ConversionOptions o = {}) {
  auto r = convertSql(a, b, s, o);
  if(r.ok() || r.diagnostics.empty() || !r.sql.empty())
    throw std::runtime_error("unsafe conversion was accepted or partially published: "+s);
}
class EmptyDeployedCatalog final : public CatalogSnapshot {
public:
  std::optional<TableDefinition> findTable(const NameResolutionContext &,
      const std::vector<std::string> &) const override { return {}; }
  bool routineLookupComplete(const NameResolutionContext &,
      const std::vector<std::string> &,bool) const override { return true; }
};
int main() {
  try {
    auto my = Dialect::MySQL, pg = Dialect::PostgreSQL, ora = Dialect::Oracle,
         ms = Dialect::SQLServer, sq = Dialect::SQLite;
    {
      bad(my,pg,"SELECT 'item' LIKE 'i%' ESCAPE NULL;");
      bad(my,Dialect::MariaDB,"SELECT 'item' LIKE 'i%' ESCAPE NULL;");
      bad(pg,sq,"SELECT 'item' LIKE 'i%' ESCAPE '!';");
      bad(my,Dialect::MariaDB,"SELECT 'item' REGEXP '^ITEM';");
      bad(Dialect::MariaDB,my,"SELECT 'item' NOT REGEXP '^ITEM';");
      bad(Dialect::MariaDB,my,"SELECT JSON_ARRAYAGG(DISTINCT 17);");
      bad(Dialect::TiDB,my,"SELECT STD(DISTINCT 17);");
      bad(Dialect::MariaDB,my,"SELECT NTILE(1+1) OVER(ORDER BY ABS(17));");
      bad(Dialect::TiDB,my,"SELECT NTILE((1+1)) OVER(ORDER BY ABS(17));");
      bad(my,Dialect::MariaDB,"SELECT LAG(17,1,0) OVER(ORDER BY ABS(17));");
      for (const auto source:{my,Dialect::TiDB,Dialect::OceanBase})
        bad(source,Dialect::MariaDB,"DELETE FROM audit_items AS item WHERE item.item_id=17;");
      for (const auto source:{my,Dialect::MariaDB})
        for (const auto target:{my,Dialect::MariaDB,Dialect::TiDB,Dialect::OceanBase}) {
          const auto flags=good(source,target,"DELETE IGNORE QUICK LOW_PRIORITY QUICK FROM audit_items WHERE item_id=17;");
          require(compile(target,flags.sql)->ok(),"converted DELETE modifiers violate the target grammar");
          require(flags.sql.find(target==Dialect::OceanBase?"DELETE IGNORE LOW_PRIORITY QUICK ":"DELETE LOW_PRIORITY QUICK IGNORE ")==0,
                  "DELETE modifiers were not emitted in target order");
        }
      bad(sq,my,"SELECT NULL NOT IN ();");
      bad(sq,my,"SELECT 'item_17' GLOB 'item_*';");
      bad(sq,pg,"SELECT 'item_17' NOT GLOB 'item_*';");
      bad(ms,my,"SELECT JSON_OBJECT('item':17 ABSENT ON NULL);");
      bad(ms,pg,"SELECT JSON_ARRAY(17,NULL NULL ON NULL);");
      bad(ms,pg,"SELECT CONVERT(INT,17);");
      const auto definitionBatch=good(pg,ms,"CREATE VIEW audit_v AS SELECT 17 AS item_id;SELECT item_id FROM audit_v;");
      require(definitionBatch.sql.find("\nGO\n")!=std::string::npos,"SQL Server definition was emitted with a following query in the same batch");
      const auto precedingBatch=good(pg,ms,"SELECT 17;CREATE VIEW audit_v AS SELECT 23 AS item_id;");
      require(precedingBatch.sql.find("\nGO\n")!=std::string::npos,"SQL Server definition did not begin a fresh batch");
      bad(ms,pg,"SELECT TOP(1+1) 17;");
      bad(ms,my,"SELECT 17 ORDER BY 1 OFFSET @skip+1 ROWS FETCH NEXT @take+1 ROWS ONLY;");
      const auto assignedLabel=good(ms,pg,"SELECT field_label=17;");
      require(compile(pg,assignedLabel.sql)->ok(),"T-SQL assignment alias did not convert to a scalar projection label");
      bad(ms,my,"SELECT TRY_CAST('17' AS INT);");
      bad(ms,sq,"SELECT TRY_CONVERT(DATETIME,'2026-11-27',23);");
      bad(pg,my,"SELECT 17 LIMIT NULL;");
      bad(ora,pg,"SELECT 17 FROM dual FETCH FIRST NULL ROWS ONLY;");
      bad(ora,my,"SELECT 17 FROM dual OFFSET -1 ROWS;");
      bad(ora,pg,"SELECT 17 FROM dual FETCH NEXT 1.5 ROWS ONLY;");
      bad(ora,ms,"SELECT 17 FROM dual FETCH NEXT 1+1 ROWS ONLY;");
      bad(pg,ms,"SELECT 17 LIMIT 1+1;");
      bad(pg,ora,"SELECT 17 OFFSET NULL;");
      bad(sq,pg,"SELECT 17 LIMIT -1;");
      bad(sq,my,"SELECT 17 LIMIT 1+1;");
      require(compile(pg,good(pg,pg,"SELECT 17 LIMIT 1+1 OFFSET NULL;").sql)->ok(),
              "same-dialect pagination expressions did not reparse");
      require(compile(ms,good(ms,ms,"SELECT CONVERT(DECIMAL(12,3),17.25),TRY_CAST('17' AS INT);").sql)->ok(),
              "same-dialect SQL Server conversion lost its type/value order");
      const auto sameJson=good(ms,ms,"SELECT JSON_OBJECT('item':17 NULL ON NULL);");
      require(compile(ms,sameJson.sql)->ok(),"same-dialect JSON constructor lost its key/value syntax");
      bad(pg,sq,"SELECT;");
      bad(ora,my,"SELECT id FROM (SELECT 17 AS id FROM dual);");
      const auto inlineView=good(sq,ora,"SELECT id FROM (SELECT 17 AS id);");
      require(compile(ora,inlineView.sql)->ok(),"converted Oracle inline view did not reparse");
      const auto anonymousPg=good(sq,pg,"SELECT id FROM (SELECT 17 AS id);");
      require(compile(pg,anonymousPg.sql)->ok(),"converted PostgreSQL inline view required an unnecessary alias");
      bad(pg,my,"SELECT id FROM (SELECT 17 AS id);");
      bad(pg,ms,"SELECT id FROM (SELECT 17 AS id);");
      const auto escaped=compile(my,"SELECT 'item' LIKE 'i%' ESCAPE NULL;");
      require(escaped->ok(),"source escape query failed");
      const auto &query=escaped->model.front();
      const auto expression=generateExpression(escaped->model,query.id,query.projections.front().expression,pg);
      require(!expression.ok() && expression.sql.empty(),"expression emission bypassed the target escape policy");
    }
    {
      MemoryCatalog catalog;
      catalog.tables={
        {"app","dbo","items",{{"id",ValueType::Integer},{"title",ValueType::String}},true,true},
        {"app","dbo","audit_log",{{"item_id",ValueType::Integer},{"old_title",ValueType::String}},true,true}};
      auto notTable=catalog.tables.back();
      notTable.name="audit_view";
      notTable.kind=ObjectKind::View;
      catalog.tables.push_back(notTable);
      const NameResolutionContext context{"app","dbo",ms};
      const std::string both="UPDATE dbo.items SET title='again' "
                             "OUTPUT deleted.id,deleted.title INTO dbo.audit_log(item_id,old_title) "
                             "OUTPUT inserted.id AS changed;";
      const auto parsed=compile(ms,both,{},&catalog,context);
      require(parsed->ok(),"OUTPUT INTO with a separate result projection was rejected");
      const auto &q=parsed->model.front();
      require(q.outputInto && q.outputInto->table==std::vector<std::string>{"dbo","audit_log"} &&
              q.outputInto->columns==std::vector<std::string>{"item_id","old_title"} &&
              q.outputInto->projections.size()==2 && q.returning.size()==1 && q.returning.front().alias=="changed",
              "stored and returned OUTPUT projections were conflated");
      require(q.expressions.at(q.outputInto->projections.front().expression).binding.has_value() &&
              q.expressions.at(q.returning.front().expression).binding.has_value(),
              "OUTPUT pseudo-row references were lost in one of the lists");
      require(good(ms,ms,both).sql.find("INTO [dbo].[audit_log]")!=std::string::npos &&
              formatSql(ms,both).ok(),"stored OUTPUT did not survive generation and formatting");
      bad(ms,pg,both);
      require(good(ms,ms,"DELETE FROM dbo.items OUTPUT deleted.id INTO @audit (item_id);")
                  .sql.find("INTO @audit")!=std::string::npos,"OUTPUT table variable lost its spelling");
      require(good(ms,ms,"INSERT INTO dbo.items OUTPUT inserted.id INTO #audit (item_id) VALUES (1,'a');")
                  .sql.find("INTO [#audit]")!=std::string::npos,"OUTPUT temporary table lost its name");
      require(compile(ms,"DELETE FROM dbo.items OUTPUT deleted.* INTO dbo.audit_log(item_id,old_title)",{},&catalog,context)->ok(),
              "wildcard OUTPUT list could not be compiled");
      for (const auto *sql:{
              "UPDATE dbo.items SET title='x' OUTPUT inserted.id INTO dbo.audit_log(item_id,old_title)",
              "UPDATE dbo.items SET title='x' OUTPUT inserted.id INTO dbo.audit_log(missing_field)",
              "UPDATE dbo.items SET title='x' OUTPUT inserted.id,inserted.id INTO dbo.audit_log(item_id,item_id)",
              "UPDATE dbo.items SET title='x' OUTPUT inserted.id INTO dbo.missing_log(item_id)",
              "UPDATE dbo.items SET title='x' OUTPUT inserted.id INTO dbo.audit_view(item_id)",
              "UPDATE dbo.items SET title='x' OUTPUT (SELECT 1) INTO dbo.audit_log(item_id)"}) {
        const auto rejected=compile(ms,sql,{},&catalog,context);
        require(rejected->syntaxValid && !rejected->ok(),"invalid OUTPUT INTO target or projection was accepted");
      }
      auto stage=catalog.tables.front();
      stage.name="imported";
      stage.columns[1].nonNullKnown=true;
      catalog.tables.front().columns[0].nonNullKnown=true;
      catalog.tables.push_back(stage);
      const std::string merge="MERGE INTO dbo.items AS t USING dbo.imported AS s ON t.id=s.id "
                              "WHEN MATCHED THEN UPDATE SET t.title=s.title "
                              "WHEN NOT MATCHED THEN INSERT (id,title) VALUES (s.id,s.title) "
                              "WHEN NOT MATCHED BY SOURCE THEN DELETE ";
      const std::string actions=merge+"OUTPUT $ACTION AS operation,inserted.id AS updated_id,"
                                      "deleted.id AS previous_id,s.title AS source_title;";
      const auto merged=compile(ms,actions,{},&catalog,context);
      require(merged->ok(),"MERGE OUTPUT did not compile");
      const auto &shape=merged->model.front();
      const auto &action=shape.expressions.at(shape.returning.front().expression);
      const auto &type=merged->model.types.get(action.sqlType);
      require(action.kind==ExprKind::MergeAction && type.family==TypeFamily::VarCharacter &&
              type.length==10 && type.nationalCharacter==true && type.nullability==Nullability::NotNull,
              "SQL Server $action did not have the documented result type");
      for (std::size_t i=1;i<shape.returning.size();++i) {
        const auto &column=shape.expressions.at(shape.returning[i].expression);
        require(column.binding.has_value() && merged->model.types.get(column.sqlType).nullability==Nullability::Nullable,
                "MERGE OUTPUT row lost its binding or NULL extension");
      }
      require(good(ms,ms,actions).sql.find("OUTPUT $action AS [operation]")!=std::string::npos &&
              formatSql(ms,actions).ok(),"MERGE $action did not survive SQL generation");
      const auto storing=good(ms,ms,merge+"OUTPUT $action,deleted.id INTO @changes(action_name,item_id) "
                                          "OUTPUT inserted.id;");
      require(storing.sql.find("INTO @changes")!=std::string::npos,"MERGE sink did not survive SQL generation");
      bad(ms,pg,actions);
      require(!compile(ms,"SELECT $action")->ok(),"MERGE pseudo-column escaped into SELECT");
      require(!compile(ms,"UPDATE dbo.items SET title='x' OUTPUT $action;")->ok(),
              "MERGE pseudo-column was accepted in UPDATE");
      require(!compile(ms,merge+"OUTPUT $action")->syntaxValid,"MERGE lost its terminating semicolon requirement");
    }
    {
      const std::string mode="SELECT pg_catalog.mode(*) WITHIN GROUP (ORDER BY v DESC NULLS FIRST) "
                             "FILTER (WHERE v > 0) FROM (VALUES (1::smallint),(2::smallint)) s(v);";
      const auto compiled=compile(pg,mode);
      require(compiled->ok(),"typed PostgreSQL MODE query did not compile");
      const auto &query=compiled->model.front();
      const auto &filter=query.expressions.at(query.projections.front().expression);
      const auto &call=query.expressions.at(filter.operands.front());
      const auto &type=compiled->model.types.get(call.sqlType);
      require(call.withinGroup && call.withinGroup->descending && call.withinGroup->nulls=="FIRST" &&
              call.operands.size()==1 && type.family==TypeFamily::Integer && type.bits==16 &&
              type.nullability==Nullability::Nullable,"ordered aggregate lost direction or its input type");
      const auto normalized=good(pg,pg,mode);
      require(normalized.sql.find("mode() WITHIN GROUP")!=std::string::npos &&
              normalized.sql.find("DESC NULLS FIRST")!=std::string::npos,
              "normalized MODE did not preserve ordered aggregation");
      require(formatSql(pg,mode).ok(),"ordered aggregate formatting failed");
      bad(pg,my,mode);
      for (const auto *sql:{"SELECT mode(1)",
                            "SELECT mode() WITHIN GROUP (ORDER BY NULL)",
                            "SELECT mode() WITHIN GROUP (ORDER BY 'a')",
                            "SELECT mode() WITHIN GROUP (ORDER BY 1) OVER ()",
                            "SELECT mode(1) WITHIN GROUP (ORDER BY 2)",
                            "SELECT mode() WITHIN GROUP (ORDER BY 1,2)",
                            "SELECT 1 WHERE mode() WITHIN GROUP (ORDER BY 1)=1",
                            "SELECT DISTINCT mode() WITHIN GROUP (ORDER BY 1 ASC) "
                            "ORDER BY mode() WITHIN GROUP (ORDER BY 1 DESC)"})
        require(!compile(pg,sql)->ok(),"invalid PostgreSQL ordered aggregate accepted");
      require(compile(pg,"SELECT mode() WITHIN GROUP (ORDER BY NULL::integer)")->ok(),
              "typed NULL in an ordered aggregate was rejected");
    }
    {
      const std::string sql="SELECT percentile_disc(0.5) WITHIN GROUP (ORDER BY v DESC), "
                            "percentile_disc(ARRAY[0,1,NULL]) WITHIN GROUP (ORDER BY v), "
                            "percentile_cont($1) WITHIN GROUP (ORDER BY v), "
                            "percentile_cont(0.5) WITHIN GROUP (ORDER BY INTERVAL '1 day') "
                            "FROM (VALUES (1::smallint),(7::smallint)) s(v)";
      auto parsed=compile(pg,sql);
      require(parsed->ok(),"typed percentile query did not compile");
      const auto &query=parsed->model.front();
      const auto typeAt=[&](std::size_t n) {
        return parsed->model.types.get(query.expressions.at(query.projections[n].expression).sqlType);
      };
      require(typeAt(0).family==TypeFamily::Integer && typeAt(0).bits==16 && !typeAt(0).arrayDimensions &&
              typeAt(1).family==TypeFamily::Integer && typeAt(1).bits==16 && typeAt(1).arrayDimensions==1 &&
              typeAt(2).family==TypeFamily::Floating && typeAt(2).bits==64 &&
              typeAt(3).family==TypeFamily::Interval,"percentile overloads did not retain result types");
      const auto &continuous=query.expressions.at(query.projections[2].expression);
      const auto &fraction=parsed->model.types.get(query.expressions.at(continuous.operands.front()).sqlType);
      require(fraction.family==TypeFamily::Floating && fraction.bits==64,"percentile bind parameter did not infer double precision");
      require(good(pg,pg,sql).sql.find("percentile_disc(0.5) WITHIN GROUP")!=std::string::npos,
              "percentile regeneration lost its direct argument");
      require(formatSql(pg,sql).ok(),"percentile query did not format");
      bad(pg,my,sql);
      for (const auto *invalid:{
              "SELECT percentile_disc(0.5)",
              "SELECT percentile_disc() WITHIN GROUP (ORDER BY 1)",
              "SELECT percentile_disc(0.5::text) WITHIN GROUP (ORDER BY 1)",
              "SELECT percentile_disc(ARRAY[1]) WITHIN GROUP (ORDER BY ARRAY[4])",
              "SELECT percentile_cont(0.5) WITHIN GROUP (ORDER BY TRUE)",
              "SELECT percentile_disc(v) WITHIN GROUP (ORDER BY v) FROM (VALUES (1),(2)) d(v)",
              "SELECT percentile_disc(v) WITHIN GROUP (ORDER BY v) FILTER(WHERE TRUE) FROM (VALUES (1),(2)) d(v)"})
        require(!compile(pg,invalid)->ok(),"invalid percentile query was accepted");
      require(compile(pg,"SELECT percentile_disc(g) WITHIN GROUP (ORDER BY v) "
                         "FROM (VALUES (0.5,1),(0.5,2)) s(g,v) GROUP BY g")->ok(),
              "grouped PostgreSQL percentile fraction was rejected");
    }
    {
      const auto maria=Dialect::MariaDB, tidb=Dialect::TiDB;
      const std::string guarded="ALTER TABLE owned_alter ADD COLUMN IF NOT EXISTS note INTEGER, "
                                "DROP COLUMN IF EXISTS absent;";
      for (auto dialect:{pg,maria,tidb}) {
        const auto parsed=compile(dialect,guarded);
        require(parsed->ok(),"guarded ALTER table did not compile");
        const auto &actions=parsed->model.front().ddl->actions;
        require(actions.size()==2 && actions[0].ifNotExists && actions[1].ifExists,
                "ALTER column guards did not reach the SQL model");
        const auto normalized=good(dialect,dialect,guarded);
        require(normalized.sql.find("ADD COLUMN IF NOT EXISTS")!=std::string::npos &&
                normalized.sql.find("DROP COLUMN IF EXISTS")!=std::string::npos,
                "ALTER column guards were removed during generation");
        require(formatSql(dialect,guarded).ok(),"formatting failed on ALTER column guards");
        bad(dialect,my,guarded);
      }
      require(!compile(my,guarded)->syntaxValid,"MySQL claimed MariaDB's ALTER guard");
      require(!compile(pg,"ALTER TABLE t ADD IF NOT EXISTS CONSTRAINT c CHECK (1=1)")->syntaxValid,
              "column guard was accepted on a constraint");
      require(good(pg,pg,"ALTER TABLE t RENAME previous_name TO next_name").sql.find("RENAME COLUMN")!=std::string::npos,
              "optional PostgreSQL COLUMN did not resolve to a column rename");
      require(good(pg,pg,"ALTER TABLE t ALTER previous_name SET DATA TYPE BIGINT USING previous_name::BIGINT")
                  .sql.find("TYPE BIGINT USING")!=std::string::npos,
              "PostgreSQL SET DATA TYPE did not preserve its USING expression");
      const MemoryCatalog empty;
      const NameResolutionContext context{"app","public",pg};
      require(compile(pg,"ALTER TABLE IF EXISTS missing ADD id INTEGER",{},&empty,context)->ok(),
              "known missing ALTER target should honor IF EXISTS");
      require(!compile(pg,"ALTER TABLE missing ADD id INTEGER",{},&empty,context)->ok(),
              "unknown unguarded ALTER target was accepted");
      require(good(maria,maria,"ALTER TABLE IF EXISTS missing ADD COLUMN id INTEGER")
                  .sql.find("ALTER TABLE IF EXISTS")!=std::string::npos,
              "MariaDB table guard was lost");
    }
    {
      const auto source="select array[1, null, 2147483648] as nums, array[]::integer[] as no_values, "
                        "array[1,true]::text[] as converted, array[null] as untyped";
      const auto compiled=compile(pg,source);
      require(compiled->ok(),"PostgreSQL array constructors failed to bind");
      const auto &query=compiled->model.front();
      const auto typeOf=[&](std::size_t projection) {
        return compiled->model.types.get(query.expressions.at(query.projections.at(projection).expression).sqlType);
      };
      require(typeOf(0).family==TypeFamily::Integer && typeOf(0).arrayDimensions==1 && typeOf(0).bits==64,
              "PostgreSQL common array element type was lost");
      require(typeOf(1).arrayDimensions==1 && typeOf(1).bits==32 &&
              typeOf(2).arrayDimensions==1 && typeOf(2).family==TypeFamily::VarCharacter && typeOf(2).flavor=="TEXT",
              "explicit PostgreSQL array cast did not determine element type");
      require(typeOf(3).arrayDimensions==1 && typeOf(3).family==TypeFamily::VarCharacter && typeOf(3).flavor=="TEXT",
              "all-NULL PostgreSQL array did not default to text");
      require(good(pg,pg,source).sql.find("ARRAY[1, NULL, 2147483648]")!=std::string::npos,
              "array SQL generation lost a typed constructor");
      require(formatSql(pg,source).ok(),"array formatting failed");
      require(!compile(pg,"SELECT ARRAY[]")->ok(),"untyped empty array was accepted");
      require(!compile(pg,"SELECT ARRAY[1,TRUE]")->ok(),"incompatible array elements were accepted");
      bad(pg,my,source);
      const std::string matrix="SELECT ARRAY[[1,2],[3,4]] AS grid, "
                               "ARRAY[[],[]]::integer[] AS empty_grid, "
                               "ARRAY[[1,TRUE],['a','b']]::text[] AS converted_grid";
      const auto normalized=good(pg,pg,matrix);
      require(normalized.sql.find("ARRAY[[1, 2], [3, 4]]")!=std::string::npos,
              "nested bracket array spelling lost its element structure");
      require(formatSql(pg,normalized.sql).ok(),"nested array formatting failed");
      const auto dimensions=compile(pg,"SELECT ARRAY[[1,2],[3,4]] AS grid, ARRAY[5,6] AS row_values");
      require(dimensions->ok(),"typed rectangular arrays did not parse");
      const auto &fields=dimensions->model.front();
      require(fields.expressions.at(fields.projections[0].expression).sqlType==
              fields.expressions.at(fields.projections[1].expression).sqlType,
              "PostgreSQL value dimensions were encoded as distinct array SQL types");
      require(!compile(pg,"SELECT ARRAY[[1],[2,3]] AS ragged")->ok(),"ragged bracket array was accepted");
      require(!compile(pg,"SELECT ARRAY[ARRAY[1],NULL::integer[]]")->ok(),"nonempty array and NULL array were treated as rectangular");
      require(!compile(pg,"SELECT ARRAY[ARRAY[1],2]::integer[]")->ok(),"mixed nested and scalar elements were accepted");
      require(!compile(pg,"SELECT ARRAY[[[[[[[1]]]]]]]")->ok(),"an oversized PostgreSQL array rank was accepted");
      const auto fromQuery="SELECT ARRAY(SELECT n FROM (VALUES (1),(2)) AS v(n) ORDER BY n DESC), "
                           "ARRAY(VALUES (1),(2)) AS by_values";
      const auto collected=compile(pg,fromQuery);
      require(collected->ok(),"ARRAY query syntax or type inference failed");
      const auto &result=collected->model.front();
      for (const auto &projection:result.projections) {
        const auto &expression=result.expressions.at(projection.expression);
        const auto &type=collected->model.types.get(expression.sqlType);
        require(expression.query && type.family==TypeFamily::Integer && type.bits==32 && type.arrayDimensions==1,
                "ARRAY query did not retain its typed input query");
      }
      require(good(pg,pg,fromQuery).sql.find("ARRAY(SELECT")!=std::string::npos,"ARRAY query SQL was lost");
      require(compile(pg,"SELECT q.\"array\" FROM (SELECT ARRAY(SELECT 1)) q")->ok(),"ARRAY query output name was lost");
      const auto multiple=compile(pg,"SELECT ARRAY(SELECT 1,2)");
      require(multiple->syntaxValid && !multiple->ok(),"ARRAY query accepted multiple columns");
      require(!compile(pg,"SELECT ARRAY(1)")->ok(),"ARRAY query accepted a scalar expression");
      const auto access="SELECT (ARRAY[[1,2],[3,4]])[2][1] AS cell, "
                        "(ARRAY[[1,2],[3,4]])[ : 2 ][1:] AS slice, "
                        "('{\"a\": [4,5]}'::jsonb)['a'][-1] AS item";
      const auto indexed=compile(pg,access);
      require(indexed->ok(),"PostgreSQL array or JSONB subscript was rejected");
      const auto &indexedQuery=indexed->model.front();
      const auto indexedType=[&](std::size_t n) {
        return indexed->model.types.get(indexedQuery.expressions.at(indexedQuery.projections[n].expression).sqlType);
      };
      require(indexedType(0).family==TypeFamily::Integer && !indexedType(0).arrayDimensions &&
              indexedType(0).nullability==Nullability::Nullable && indexedType(1).arrayDimensions==1 &&
              indexedType(2).opaqueName==std::vector<std::string>{"jsonb"},"subscript result type was not preserved");
      require(good(pg,pg,access).sql.find("[:2][1:]")!=std::string::npos,"array slice bounds were changed in normalized SQL");
      const auto formatted=formatSql(pg,access);
      require(formatted.ok() && formatted.sql.find("[:2][1:]")!=std::string::npos,"array slice formatting lost bound spacing");
      require(compile(pg,"SELECT s.a FROM (SELECT a[1] FROM (VALUES (ARRAY[1])) v(a)) s")->ok(),
              "subscript on a column changed the derived output name");
      const auto wronglyTyped=compile(pg,"SELECT (ARRAY[1,2])[TRUE]");
      require(wronglyTyped->syntaxValid && !wronglyTyped->ok(),"boolean subscript was accepted");
      require(!compile(pg,"SELECT (ARRAY[1,2])[]")->ok(),"empty subscript was accepted");
      require(!compile(pg,"SELECT ARRAY[1,2][1]")->ok(),"unparenthesized array literal subscript was accepted");
      require(!compile(pg,"SELECT ('{}'::jsonb)[:1]")->ok(),"JSONB slice was accepted");
      require(!compile(pg,"SELECT ('{}'::jsonb)[1::bigint]")->ok(),"JSONB bigint subscript was accepted");
      bad(pg,my,access);
      const auto comparisons="SELECT 2 = ANY(ARRAY[1,2,NULL]) AS any_hit, "
                             "5 >= ALL(ARRAY[1,2]) AS all_hit, "
                             "2 < SOME(ARRAY(SELECT x FROM (VALUES (1),(3)) s(x))) AS from_query, "
                             "2 = ANY(SELECT x FROM (VALUES (1),(2)) s(x)) AS query_any";
      const auto compared=compile(pg,comparisons);
      require(compared->ok(),"PostgreSQL array comparisons failed to parse or bind");
      const auto &quantified=compared->model.front();
      const auto &array=quantified.expressions.at(quantified.projections.front().expression);
      const auto &subquery=quantified.expressions.at(quantified.projections.back().expression);
      require(!array.query && array.operands.size()==2 && subquery.query && subquery.operands.size()==1,
              "array and subquery comparisons lost their distinct shapes");
      require(good(pg,pg,comparisons).sql.find("ANY (ARRAY[1, 2, NULL])")!=std::string::npos,
              "same-dialect generation lost array comparison");
      require(formatSql(pg,comparisons).ok(),"array comparison formatting failed");
      require(!compile(pg,"SELECT 2 = ANY(1)")->ok(),"ordinary scalar right operand accepted");
      require(!compile(pg,"SELECT 2 = ANY(ARRAY['x'])")->ok(),"incompatible array operand accepted");
      bad(pg,my,comparisons);
    }
    {
      const auto partial="UPDATE source SET values_array[1] = values_array[2], "
                         "values_array[2:3] = ARRAY[7,8], payload['name'] = '3' WHERE id=1";
      const auto compiled=compile(pg,partial);
      require(compiled->ok(),"partial PostgreSQL subscript assignment did not parse");
      const auto &query=compiled->model.front();
      for (const auto &assignment:query.assignments)
        require(query.expressions.at(assignment.targets.front()).kind==ExprKind::Subscript,
                "partial UPDATE lost its full destination path");
      const auto emitted=good(pg,pg,partial);
      require(emitted.sql.find("[2:3]")!=std::string::npos &&
              emitted.sql.find("['name']")!=std::string::npos,"partial SET SQL lost paths or slice boundaries");
      require(formatSql(pg,partial).ok(),"partial SET formatting failed");
      require(!compile(pg,"UPDATE source SET values_array[1]=DEFAULT")->ok(),"partial assignment accepted DEFAULT");
      require(!compile(pg,"UPDATE source SET values_array[1]::int[]=ARRAY[1]")->ok(),"partial target accepted a cast");
      require(!compile(pg,"UPDATE source SET values_array=ARRAY[1],values_array[1]=3")->ok(),
              "whole and partial writes to the same column were accepted");
      require(compile(pg,"UPDATE source SET values_array[1]=3,values_array[1]=4")->ok(),
              "valid repeated partial writes were rejected");
      bad(pg,my,partial);
    }
    {
      const auto series="SELECT g.v,g.n FROM generate_series(1,3,1::bigint) WITH ORDINALITY AS g(v,n)";
      const auto compiled=compile(pg,series);
      require(compiled->ok(),"PostgreSQL series source failed to bind");
      const auto &query=compiled->model.front();
      require(query.tables.front().postgresFunction==PostgresRowFunction::GenerateSeries &&
              query.tables.front().ordinalityColumn=="n","PostgreSQL series source lost its row shape");
      for (const auto &field:query.projections)
        require(compiled->model.types.get(query.expressions.at(field.expression).sqlType).bits==64,
                "PostgreSQL series did not bind bigint values and ordinality");
      require(good(pg,pg,series).sql.find("WITH ORDINALITY")!=std::string::npos,"series SQL lost WITH ORDINALITY");
      require(formatSql(pg,series).ok(),"series formatting failed");
      require(compile(pg,"SELECT n FROM generate_series(1.1,4,1.3) s(n)")->ok(),"decimal server series was rejected");
      require(compile(pg,"SELECT n FROM generate_series(TIMESTAMP '2020-01-01',TIMESTAMP '2020-01-03',"
                         "INTERVAL '1 day') s(n)")->ok(),"timestamp server series was rejected");
      require(!compile(pg,"SELECT n FROM generate_series(1) s(n)")->ok(),"series accepted one argument");
      bad(pg,my,series);
    }
    for (const auto dialect : {my,Dialect::MariaDB}) {
      const std::string definition="create aggregate function if not exists `sum_udf` returns int soname 'demo''lib.so';";
      const auto parsed=compile(dialect,definition);
      require(parsed->ok() && parsed->model.front().ddl && parsed->model.front().ddl->mysqlLoadableFunction,
              "loadable function was not lowered to DDL");
      const auto &function=*parsed->model.front().ddl->mysqlLoadableFunction;
      require(function.result==MySqlLoadableResult::Integer && function.aggregate && function.library=="demo'lib.so",
              "loadable function metadata did not preserve its SQL values");
      const auto canonical=good(dialect,dialect,definition+" SELECT 1;");
      require(canonical.sql.find("CREATE AGGREGATE FUNCTION IF NOT EXISTS")!=std::string::npos &&
              canonical.sql.find("RETURNS INTEGER SONAME 'demo''lib.so'")!=std::string::npos,
              "same-dialect UDF SQL lost its attributes");
      const auto checked=checkSqlScript(dialect,definition+" SELECT 1;");
      require(checked.ok() && checked.statements==2 && checked.routines==0,
              "loadable function consumed a routine body or following statement");
      const auto formatted=formatSql(dialect,definition);
      require(formatted.ok() && compile(dialect,formatted.sql)->ok(),"loadable function formatting broke syntax");
      bad(dialect,pg,definition);
      require(!compile(dialect,"CREATE FUNCTION f RETURNS BIGINT SONAME 'example.so';")->ok(),"wrong UDF result was accepted");
      require(!compile(dialect,"CREATE FUNCTION f RETURNS INTEGER SONAME example;")->ok(),"SONAME accepted an identifier");
    }
    good(Dialect::MariaDB,Dialect::MariaDB,"CREATE OR REPLACE FUNCTION native_f RETURNS REAL SONAME 'example.so';");
    bad(my,my,"CREATE OR REPLACE FUNCTION native_f RETURNS REAL SONAME 'example.so';");
    {
      const std::string definition = "CREATE OR REPLACE ALGORITHM=MERGE DEFINER='reader'@'localhost' "
          "SQL SECURITY INVOKER VIEW v (k) AS SELECT id FROM source_table WITH LOCAL CHECK OPTION;";
      for (const auto dialect : {my,Dialect::MariaDB}) {
        auto parsed = compile(dialect,definition);
        require(parsed->ok(), "CREATE VIEW options did not parse");
        const auto &header = *parsed->model.front().ddl->mysqlViewOptions;
        require(header.algorithm == MySqlViewAlgorithm::Merge && header.security == MySqlViewSecurity::Invoker &&
                header.definer && header.definer->user == "reader" && header.definer->host == "localhost",
                "VIEW options were lost while lowering to SQL IR");
        const auto generated = good(dialect,dialect,definition);
        require(generated.sql.find("ALGORITHM = MERGE") != std::string::npos &&
                generated.sql.find("DEFINER = 'reader'@'localhost'") != std::string::npos &&
                generated.sql.find("SQL SECURITY INVOKER") != std::string::npos,
                "same-dialect generation dropped a view property");
        bad(dialect,pg,definition);
      }
      require(good(my,my,"CREATE DEFINER=CURRENT_USER() VIEW v AS SELECT 1 AS n;").sql.find("CURRENT_USER")
                != std::string::npos, "CURRENT_USER definer not preserved");
      bad(my,my,"CREATE ALGORITHM=MERGE TABLE v(id INT);");
      bad(my,my,"CREATE ALGORITHM=MERGE ALGORITHM=TEMPTABLE VIEW v AS SELECT 1 AS n;");
      bad(my,my,"CREATE SQL SECURITY PUBLIC VIEW v AS SELECT 1 AS n;");
    }
    {
      const std::string withComment=R"(CREATE PROCEDURE p() LANGUAGE SQL COMMENT 'it''s \\ a comment' SELECT 1;)";
      for (auto dialect:{my,Dialect::MariaDB}) {
        const auto output=good(dialect,dialect,withComment);
        require(output.sql.find("LANGUAGE SQL")!=std::string::npos &&
                output.sql.find(R"(COMMENT 'it''s \\ a comment')")!=std::string::npos,
                "routine generation lost a characteristic");
        const auto roundtrip=compileRoutine(dialect,output.sql);
        require(roundtrip.ok() && roundtrip.ast.explicitLanguageSql &&
                roundtrip.ast.comment==R"(it's \ a comment)",
                "routine comment was not preserved");
        bad(dialect,pg,withComment);
      }
      const std::string guarded="CREATE PROCEDURE IF NOT EXISTS p(a INT) SELECT 1;";
      for (auto dialect:{my,Dialect::MariaDB}) {
        const auto result=good(dialect,dialect,guarded);
        require(result.sql.find("IF NOT EXISTS")!=std::string::npos,"routine guard was lost");
        require(compileRoutine(dialect,result.sql).ast.ifNotExists,"routine guard was not preserved in the model");
        bad(dialect,pg,guarded);
      }
      bad(my,my,"CREATE OR REPLACE PROCEDURE IF NOT EXISTS p() SELECT 1;");
      const auto protectedSignature=checkSqlScript(my,
          "CREATE PROCEDURE p(a INT) SELECT 1; "
          "CREATE PROCEDURE IF NOT EXISTS p() SELECT 2; CALL p(1);");
      require(protectedSignature.diagnostics.empty(),"IF NOT EXISTS replaced a known routine signature");
      bad(my,my,"CREATE PROCEDURE p() COMMENT nil SELECT 1;");
      bad(my,my,"CREATE PROCEDURE p() LANGUAGE C SELECT 1;");
    }
    {
      const auto tidb = Dialect::TiDB, ob = Dialect::OceanBase;
      const std::string random = "CREATE TABLE t(id BIGINT PRIMARY KEY CLUSTERED AUTO_RANDOM);";
      require(good(tidb,tidb,random).sql.find("AUTO_RANDOM") != std::string::npos,
              "TiDB ID allocation was discarded");
      bad(tidb,my,random);
      bad(ob,tidb,"CREATE INDEX ix ON t(id) LOCAL;");
      bad(my,tidb,"CREATE PROCEDURE p() SELECT 1;");
      bad(tidb,pg,"SELECT TIDB_CURRENT_TSO();");
      bad(ob,tidb,"SELECT OB_VERSION();");
      good(my,tidb,"SELECT id FROM t WHERE id > 1;");
      good(tidb,ob,"SELECT id FROM t WHERE id > 1;");
    }
    const std::string packageSource=
        "CREATE PACKAGE owned_fixture AUTHID CURRENT_USER AS "
        "seed CONSTANT PLS_INTEGER := 7; state NUMBER := seed+1; "
        "FUNCTION f(p NUMBER DEFAULT seed) RETURN NUMBER; END owned_fixture;\n"
        "/\nDROP PACKAGE owned_fixture;\nSELECT 1 FROM DUAL;";
    const auto packed=good(ora,ora,packageSource);
    const auto checkedPackage=checkSqlScript(ora,packed.sql);
    require(checkedPackage.ok() && checkedPackage.statements==3 &&
              packed.sql.find("AUTHID CURRENT_USER")!=std::string::npos &&
              packed.sql.find("DEFAULT \"SEED\"")!=std::string::npos &&
              packed.sql.find("\n/")!=std::string::npos,
            "package state, implicit commit DDL or Oracle client boundary was lost");
    bad(ora,pg,packageSource);
    for (const auto *pragma : {"PRAGMA encoding;", "PRAGMA main.user_version = -23;",
             "PRAGMA temp.cache_size(+32);", "PRAGMA main.table_info('name.with.dot');",
             "PRAGMA journal_mode(DELETE);"}) {
      const auto canonical = good(sq,sq,pragma);
      const auto native = compile(sq,canonical.sql);
      require(native->ok() && native->model.front().kind==StatementKind::Pragma && native->model.front().pragma,
              "SQLite PRAGMA canonical round trip failed");
      require(dumpModel(*native).find("\"pragma\":{")!=std::string::npos,"PRAGMA absent from unified IR");
      bad(sq,pg,pragma);
    }
    require(good(sq,sq,"PRAGMA temp.cache_size = -23;").sql.find(" = -23")!=std::string::npos,
            "signed PRAGMA emitted a general unary expression");
    for (const auto *invalid : {"PRAGMA a();", "PRAGMA a(1,2);", "PRAGMA a.b.c;", "PRAGMA a=1+2;",
                                 "PRAGMA a(SELECT);", "PRAGMA a(?)", "PRAGMA WHERE;"})
      require(!compile(sq,invalid)->ok(),"invalid SQLite PRAGMA accepted");
    const std::string handlerHeader="CREATE PROCEDURE handle_owned() BEGIN ";
    const auto handlerSql=handlerHeader+"DECLARE n INT DEFAULT 0; DECLARE duplicate_value CONDITION FOR 1062; "
        "DECLARE CONTINUE HANDLER FOR duplicate_value, SQLWARNING BEGIN SET n=n+1; SET @counted=n; END; "
        "BEGIN DECLARE duplicate_value CONDITION FOR SQLSTATE VALUE '22012'; "
        "DECLARE EXIT HANDLER FOR duplicate_value, NOT FOUND SET n=42; SELECT n; END; SELECT n; END;";
    const auto handler=compileRoutine(my,handlerSql);
    require(handler.ok(),"MySQL condition / handler declarations did not compile");
    bool scopedHandlerBody=false;
    for (const auto &n : handler.model.nodes) {
      if (n.syntax.kind!=ProceduralKind::MySqlSessionAssignment) continue;
      for (const auto &q : n.code.queries) for (const auto &e : q.expressions)
        scopedHandlerBody |= e.localSymbol!=0;
    }
    require(scopedHandlerBody,"handler's compound statement lost outer local binding");
    const auto handlerCanonical=good(my,my,handlerSql);
    require(handlerCanonical.sql.find("DECLARE CONTINUE HANDLER FOR")!=std::string::npos &&
            handlerCanonical.sql.find("DECLARE EXIT HANDLER FOR")!=std::string::npos,
            "canonical MySQL dropped handler continuation modes");
    bad(my,pg,handlerSql);
    for (const auto *body : {
          "DECLARE CONTINUE HANDLER FOR SQLSTATE '00000' SET @x=1; SELECT 1; END;",
          "DECLARE CONTINUE HANDLER FOR 0 SET @x=1; SELECT 1; END;",
          "DECLARE CONTINUE HANDLER FOR 123.5 SET @x=1; SELECT 1; END;",
          "DECLARE CONTINUE HANDLER FOR undeclared SET @x=1; SELECT 1; END;",
          "DECLARE e CONDITION FOR 1062; DECLARE CONTINUE HANDLER FOR e SET @x=1; "
          "DECLARE EXIT HANDLER FOR 1062 SET @x=2; SELECT 1; END;",
          "DECLARE CONTINUE HANDLER FOR SQLEXCEPTION SET @x=1; DECLARE v INT; SELECT 1; END;",
          "outside: LOOP BEGIN DECLARE CONTINUE HANDLER FOR SQLEXCEPTION LEAVE outside; SELECT 1; END; "
          "LEAVE outside; END LOOP; END;"})
      require(!compileRoutine(my,handlerHeader+body).ok(), "invalid MySQL handler declaration accepted");
    {
      const std::string sql="SELECT p.account, p.[2024], p.[2025] FROM "
          "(VALUES (CAST(101 AS INT),CAST(3 AS INT),2024),(CAST(102 AS INT),CAST(NULL AS INT),2025)) "
          "AS src(account,amount,sales_year) "
          "PIVOT (SUM(amount) FOR sales_year IN ([2024],[2025])) AS p ORDER BY p.account;";
      const auto source=compile(ms,sql);
      require(source->ok() && source->model.front().tables.front().pivot,"SQL Server PIVOT did not lower");
      const auto &root=source->model.front();
      const auto &table=root.tables.front();
      require(table.definition->complete && table.definition->columns.size()==3 &&
              table.definition->columns[0].name=="account" && table.definition->columns[1].name=="2024" &&
              table.pivot->inputAlias=="src" && table.alias=="p","SQL Server PIVOT output scope and grouping columns are incorrect");
      for (const auto &projection : root.projections) {
        const auto &type=source->model.types.get(root.expressions.at(projection.expression).sqlType);
        require(type.family==TypeFamily::Integer && type.bits==32,"PIVOT output did not preserve integer type");
      }
      const auto canonical=good(ms,ms,sql);
      const auto repeated=compile(ms,canonical.sql);
      require(repeated->ok() && repeated->model.front().tables.front().definition->complete &&
              dumpModel(*repeated).find("\"pivot\":{")!=std::string::npos,"SQL Server canonical PIVOT lost its IR");
      for (const auto *invalid : {
          "SELECT * FROM (VALUES (1,2)) s(a,b) PIVOT(SUM(unknown_col) FOR a IN ([1])) p;",
          "SELECT * FROM (VALUES (1,2)) s(a,b) PIVOT(SUM(b) FOR missing IN ([1])) p;",
          "SELECT * FROM (VALUES (1,2)) s(a,b) PIVOT(SUM(b) FOR a IN ([1],[1])) p;",
          "SELECT s.a FROM (VALUES (1,2)) s(a,b) PIVOT(SUM(b) FOR a IN ([1])) p;",
          "SELECT * FROM (VALUES (1,2)) s(a,b) PIVOT(SUM(b) FOR a IN ([1]));",
          "SELECT * FROM t PIVOT(COUNT(*) FOR a IN ([1])) p;"})
        require(!compile(ms,invalid)->ok(),"invalid SQL Server PIVOT accepted");
      bad(ms,pg,sql);
    }
    {
      const std::string oraclePivot="SELECT p.grp,p.a_total,p.b_total FROM (SELECT 1 grp, 2 amount, 'A' state FROM SYS.DUAL) "
          "PIVOT (SUM(amount) AS total FOR state IN ('A' AS a, 'B' AS b)) p;";
      const auto generated=good(ora,ora,oraclePivot);
      const auto roundTrip=compile(ora,generated.sql);
      require(roundTrip->ok(),"Oracle static PIVOT did not round trip");
      const auto &columns=roundTrip->model.front().tables.front().definition->columns;
      require(columns.size()==3 && columns[0].name=="GRP" && columns[1].name=="A_TOTAL" &&
              columns[2].name=="B_TOTAL","Oracle PIVOT alias suffix or implicit groups changed");
      good(ora,ora,"SELECT * FROM t PIVOT(COUNT(*) FOR state IN (-1 AS missing, 2 AS present));");
      bad(ora,pg,oraclePivot);
      require(!compile(ora,"SELECT * FROM t PIVOT XML(SUM(n) FOR status IN (ANY));")->ok(),
              "PIVOT XML incorrectly accepted as static pivot");
      require(!compile(ora,"SELECT * FROM t PIVOT(SUM(n) FOR t.status IN ('active' AS active));")->syntaxValid,
              "Oracle PIVOT axis accepted a qualified input name");
    }
    {
      const std::string oracle="SELECT u.ID,u.QUARTER,u.TOTAL FROM (SELECT 1 id, 2 q1, "
          "CAST(NULL AS NUMBER) q2 FROM SYS.DUAL) UNPIVOT INCLUDE NULLS "
          "(total FOR quarter IN (q1 AS 'first',q2 AS 'second')) u;";
      const std::string sqlServer="SELECT u.id,u.quarter,u.total FROM (VALUES (1,2,CAST(NULL AS INT))) "
          "AS s(id,q1,q2) UNPIVOT (total FOR quarter IN (q1,q2)) AS u;";
      for (const auto &entry:{std::make_pair(ora,oracle),std::make_pair(ms,sqlServer)}) {
        const bool isOracle=entry.first==ora;
        const auto generated=good(entry.first,entry.first,entry.second);
        for (const auto &sql:{entry.second,generated.sql}) {
          const auto compiled=compile(entry.first,sql);
          require(compiled->ok() && formatSql(entry.first,sql).ok(),"UNPIVOT parse/format round trip failed");
          const auto &root=compiled->model.front();
          const auto &table=root.tables.front();
          require(table.unpivot && table.definition && table.definition->complete &&
                  table.unpivotInput && table.unpivotInput->columns.size()==3 &&
                  table.definition->columns.size()==3 &&
                  table.unpivot->retainedOrdinals==std::vector<std::size_t>{0} &&
                  table.unpivot->inputOrdinals==std::vector<std::vector<std::size_t>>{{1},{2}},
                  "UNPIVOT input ordinals or output shape were lost");
          const auto &descriptor=compiled->model.types.get(root.expressions.at(root.projections.at(1).expression).sqlType);
          const auto &measure=compiled->model.types.get(root.expressions.at(root.projections.at(2).expression).sqlType);
          require(descriptor.family==TypeFamily::VarCharacter && descriptor.nullability==Nullability::NotNull &&
                  measure.nullability==(isOracle?Nullability::Nullable:Nullability::NotNull),
                  "UNPIVOT descriptor or measure type has incorrect nullability");
          if (isOracle) {
            require(root.expressions.at(table.unpivot->inputs.at(0).labels.at(0)).value=="first" &&
                    table.unpivot->includeNulls==true,
                    "Oracle UNPIVOT lost labels or INCLUDE NULLS");
          } else {
            require(measure.family==TypeFamily::Integer && measure.bits==32 && descriptor.length==128 &&
                    table.unpivot->inputAlias=="s" && table.unpivot->relationAlias=="u",
                    "SQL Server UNPIVOT lost types or aliases");
          }
          require(dumpModel(*compiled).find("\"unpivot\":{")!=std::string::npos,"UNPIVOT missing in IR dump");
        }
        bad(entry.first,pg,entry.second);
      }
      for (const auto *sql:{
          "SELECT u.q1 FROM (VALUES (1,2)) s(id,q1) UNPIVOT(total FOR q IN (q1)) u;",
          "SELECT * FROM (VALUES (1,2)) s(id,q1) UNPIVOT(total FOR q IN (missing)) u;",
          "SELECT * FROM (VALUES (1,2)) s(id,q1) UNPIVOT(id FOR q IN (q1)) u;",
          "SELECT * FROM (SELECT CAST(1 AS SMALLINT) q1, CAST(2 AS INT) q2) s UNPIVOT(total FOR q IN (q1,q2)) u;",
          "SELECT * FROM (VALUES (1,2)) s(id,q1) UNPIVOT(total FOR q IN (q1));",
          "SELECT * FROM (VALUES (1,2)) s(id,q1) UNPIVOT INCLUDE NULLS (total FOR q IN (q1)) u;"})
        require(!compile(ms,sql)->ok(),"invalid SQL Server UNPIVOT was accepted");

      const auto twoMeasures=std::string("SELECT u.quarter,u.year_no,u.total,u.category FROM (SELECT 1 id,2 q1, ")+
          "3.5 q2,CAST(NULL AS CHAR(3)) label1,CAST('second' AS VARCHAR2(6)) label2 FROM SYS.DUAL) "
          "UNPIVOT EXCLUDE NULLS ((total,category) FOR (quarter,year_no) "
          "IN ((q1,label1) AS (DATE '2026-01-01',-2026),(q2,label2) AS (NULL,2027))) u";
      const auto generated=good(ora,ora,twoMeasures);
      const auto repeated=compile(ora,generated.sql);
      require(repeated->ok() && formatSql(ora,twoMeasures).ok(),"Oracle multi-column UNPIVOT did not round trip");
      const auto &query=repeated->model.front();
      const auto &shape=*query.tables.front().unpivot;
      require(shape.measures==std::vector<std::string>{"TOTAL","CATEGORY"} &&
              shape.descriptors==std::vector<std::string>{"QUARTER","YEAR_NO"} &&
              shape.inputOrdinals==std::vector<std::vector<std::size_t>>{{1,3},{2,4}},
              "Oracle multi-column UNPIVOT crossed descriptor or measure positions");
      const auto dataType=[&](std::size_t slot) {
        return repeated->model.types.get(query.expressions.at(query.projections.at(slot).expression).sqlType);
      };
      require((dataType(0).family==TypeFamily::Date || dataType(0).flavor=="oracle-date") &&
              dataType(0).nullability==Nullability::Nullable && dataType(1).family==TypeFamily::Decimal &&
              dataType(1).nullability==Nullability::NotNull && dataType(2).family==TypeFamily::Decimal &&
              dataType(3).family==TypeFamily::VarCharacter && dataType(3).nullability==Nullability::Nullable,
              "Oracle UNPIVOT did not keep the independent label and measure types or per-field NULL semantics");
      bad(ora,pg,twoMeasures);
      for (const auto *sql:{
          "SELECT * FROM t UNPIVOT ((total,units) FOR quarter IN (q1 AS 'first'));",
          "SELECT * FROM t UNPIVOT ((total,units) FOR (quarter,yr) IN ((q1,v1) AS 'first'));",
          "SELECT * FROM t UNPIVOT (total FOR quarter IN (q1 AS 1,q2));",
          "SELECT * FROM t UNPIVOT (total FOR quarter IN (q1 AS 1,q2 AS 'second'));",
          "SELECT * FROM t UNPIVOT (total FOR quarter IN (q1 AS 1+2));"})
        require(!compile(ora,sql)->ok(),"invalid Oracle UNPIVOT tuple or label accepted");
      good(ora,ora,"SELECT * FROM t UNPIVOT ((total,units) FOR (quarter,yr) IN ((q1,v1),(q1,v2))); ");
    }
    {
      ConversionOptions calcite;
      calcite.language.calciteExtensions=true;
      const std::string sql="WITH src(id,q1,q2) AS (VALUES (1,2,NULL::bigint),(2,NULL,3::bigint)) "
                            "SELECT u.* FROM src UNPIVOT (total FOR label IN (q1 AS 'same',q2 AS 'same')) AS u";
      const auto converted=good(pg,pg,sql,calcite);
      require(converted.sql.find("CROSS JOIN LATERAL (VALUES")!=std::string::npos &&
              converted.sql.find(" IS NOT NULL")!=std::string::npos && compile(pg,converted.sql)->ok(),
              "Calcite UNPIVOT generation did not produce native PostgreSQL SQL");
      require(formatSql(pg,sql,{},FormatOptions{calcite.language}).syntaxSupported,"Calcite UNPIVOT did not format in the enabled mode");
      bad(pg,my,sql,calcite);
      bad(pg,pg,"SELECT * FROM unbound_table UNPIVOT(total FOR label IN(q1,q2)) u",calcite);
    }
    for (const auto *sql : {
          "CREATE TABLE hash_owned(id INT PRIMARY KEY,v VARCHAR(20)) ENGINE=InnoDB PARTITION BY HASH(id) PARTITIONS 4;",
          "CREATE TABLE key_owned(id INT,code VARCHAR(20),PRIMARY KEY(id,code)) "
          "PARTITION BY LINEAR KEY ALGORITHM=2 (id,code) PARTITIONS 3;",
          "CREATE TABLE key_default_owned(id INT PRIMARY KEY) PARTITION BY KEY() PARTITIONS 2;"}) {
      const auto local=compile(my,sql);
      require(local->ok() && local->model.front().ddl->partition,"MySQL CREATE TABLE lost its partition spec");
      const auto canonical=good(my,my,sql);
      const auto roundTrip=compile(my,canonical.sql);
      require(roundTrip->ok() && roundTrip->model.front().ddl->partition->count==local->model.front().ddl->partition->count,
              "MySQL partition count changed during generation");
      require(dumpModel(*roundTrip).find("\"partition\":{")!=std::string::npos,"partition is missing from unified IR");
      bad(my,pg,sql);
    }
    for (const auto *sql : {
          "CREATE TABLE t(id INT) PARTITION BY HASH(absent);",
          "CREATE TABLE t(id VARCHAR(12)) PARTITION BY HASH(id);",
          "CREATE TABLE t(id INT,u INT UNIQUE) PARTITION BY HASH(id);",
          "CREATE TABLE t(id INT,p INT,PRIMARY KEY(p)) PARTITION BY KEY(id,p);",
          "CREATE TABLE t(id INT) PARTITION BY KEY(id,id);",
          "CREATE TABLE t(id INT) PARTITION BY HASH(id) PARTITIONS 0;",
          "CREATE TABLE t(id INT) PARTITION BY KEY ALGORITHM=3 (id);",
          "CREATE TEMPORARY TABLE t(id INT) PARTITION BY HASH(id);"})
      require(!compile(my,sql)->ok(),"invalid MySQL partition specification accepted");
    const auto recursive = "WITH RECURSIVE seq(n) AS (SELECT 1 UNION ALL "
                           "SELECT n+1 FROM seq WHERE n<4) SELECT n FROM seq;";
    require(good(pg, pg, recursive).sql.find("WITH RECURSIVE") != std::string::npos,
            "same-dialect generation lost recursive scope");
    bad(pg, my, recursive);
    for (const auto dialect : {pg, sq}) {
      for (const auto *hint : {"MATERIALIZED","NOT MATERIALIZED"}) {
        const auto withHint=std::string("WITH x(k) AS ")+hint+" (SELECT 1) SELECT k FROM x;";
        const auto canonical=good(dialect,dialect,withHint);
        require(canonical.sql.find(std::string(" AS ")+hint+" (")!=std::string::npos,
                "native CTE materialization directive was lost");
        const auto parsed=compile(dialect,canonical.sql);
        const auto materialization=std::string(hint)=="MATERIALIZED" ? CteMaterialization::Materialized :
            CteMaterialization::NotMaterialized;
        require(parsed->ok() && parsed->model.front().ctes.at(0).materialization==materialization &&
                parsed->model.front().tables.at(0).cteMaterialization==materialization &&
                dumpAST(*parsed).find("\"materialization\"")!=std::string::npos &&
                dumpModel(*parsed).find("\"materialization\"")!=std::string::npos,
                "CTE materialization directive was lost while binding output scope");
        require(!generateSql(parsed->model,dialect==pg?sq:pg).ok(), "generation changed CTE evaluation contract");
        bad(dialect,my,withHint);
        bad(dialect,dialect==pg?sq:pg,withHint);
      }
    }
    const auto forwardCteSql="WITH RECURSIVE a AS (SELECT n FROM b), b AS (SELECT 7 AS n) SELECT n FROM a;";
    const auto forwardConversion=good(pg,my,forwardCteSql);
    const auto forwardTarget=compile(my,forwardConversion.sql);
    require(forwardTarget->ok() && !forwardTarget->model.front().recursiveWith &&
            forwardTarget->model.front().ctes.front().name=="b",
            "forward CTE became a base-table reference in a sequential WITH target");
    const auto partialCte = "WITH q(a) AS (SELECT 1 AS previous, 2 AS retained) SELECT a,retained FROM q;";
    const auto expandedCte = good(pg, my, partialCte);
    const auto checkedCte = compile(my, expandedCte.sql);
    require(checkedCte->ok() && checkedCte->model.front().ctes.front().columns ==
            std::vector<std::string>{"a", "retained"}, "target did not get the complete CTE column list");
    require(good(pg, pg, partialCte).sql.find("retained") != std::string::npos,
            "same-dialect CTE generation lost a trailing source column");
    bad(pg, my, "WITH q(a) AS (SELECT * FROM unresolved_table) SELECT a FROM q;");
    const auto paged = "SELECT 1 AS n ORDER BY n NULLS FIRST FETCH FIRST 2 ROWS WITH TIES;";
    require(good(pg, ms, paged).sql.find("TOP (2) WITH TIES") != std::string::npos,
            "SQL Server generation lost tied rows");
    require(good(ms, pg, "SELECT TOP (2) WITH TIES 1 AS n ORDER BY n;").sql.find("ROWS WITH TIES") != std::string::npos,
            "PostgreSQL generation lost tied rows");
    bad(pg, my, paged);
    bad(pg, ms, "SELECT 1 AS n ORDER BY n NULLS FIRST OFFSET 1 ROW FETCH FIRST ROW WITH TIES;");
    bad(pg, ms, "SELECT 'name' AS n ORDER BY n NULLS FIRST FETCH FIRST ROW WITH TIES;");
    good(pg, my, "SELECT (1=1) IS NOT FALSE, NULL IS UNKNOWN;");
    good(my, pg, "SELECT (1=1) IS NOT FALSE, NULL IS UNKNOWN;");
    bad(my, pg, "SELECT 2 IS TRUE;");
    bad(pg, my, "SELECT 'true' IS TRUE;");
    const auto symmetric=good(pg,my,"SELECT 2 BETWEEN SYMMETRIC 3 AND 1;");
    require(symmetric.sql.find("BETWEEN 1 AND 3")!=std::string::npos,
            "symmetric bound normalization changed comparison");
    require(symmetric.changes.size()==1 &&
              symmetric.changes.front().kind==ConversionChangeKind::SymmetricBounds &&
              symmetric.changes.front().before=="BETWEEN SYMMETRIC 3 AND 1" &&
              symmetric.changes.front().after=="BETWEEN 1 AND 3",
            "conversion omitted the reason and ordered bounds");
    ConversionOptions compact;
    compact.generation.compactExpressions=true;
    const auto readable=good(my,pg,
        "SELECT 1 + 2 * 3 AS a, (1 + 2) * 3 AS b, 9 - (3 - 1) AS c "
        "WHERE 1 = 1 OR (2 = 3 AND 3 = 4);",compact);
    require(readable.sql.find("1 + 2 * 3 AS")!=std::string::npos &&
              readable.sql.find("(1 + 2) * 3 AS")!=std::string::npos &&
              readable.sql.find("9 - (3 - 1) AS")!=std::string::npos &&
              readable.sql.find("WHERE 1 = 1 OR 2 = 3 AND 3 = 4")!=std::string::npos,
            "compact generation lost operator precedence or kept unnecessary parentheses");
    const auto paginationReport=good(pg,sq,"SELECT 1;\nSELECT 2 FETCH FIRST ROW ONLY;");
    require(paginationReport.changes.size()==1 &&
              paginationReport.changes.front().kind==ConversionChangeKind::Pagination &&
              paginationReport.changes.front().statement==2 &&
              paginationReport.changes.front().origin.begin==10 &&
              paginationReport.changes.front().after=="LIMIT",
            "pagination report did not map to the originating statement");
    bad(pg,my,"SELECT x BETWEEN SYMMETRIC a AND b FROM t;");
    const auto intoPg="CREATE FUNCTION assign_owned(p INTEGER) RETURNS INTEGER LANGUAGE plpgsql "
        "AS $$ DECLARE v INTEGER; BEGIN SELECT p INTO STRICT v; RETURN v; END; $$;";
    const auto intoMy="CREATE FUNCTION assign_owned(p INT) RETURNS INT DETERMINISTIC "
        "BEGIN DECLARE v INT; SELECT p INTO v; RETURN v; END;";
    require(good(pg,pg,intoPg).sql.find("INTO STRICT")!=std::string::npos,
            "PL/pgSQL SELECT INTO lost STRICT");
    require(good(pg,my,intoPg).sql.find("INTO ")!=std::string::npos,
            "PL/pgSQL SELECT INTO lost its assignment in MySQL");
    require(good(my,pg,intoMy).sql.find("INTO STRICT")!=std::string::npos,
            "exact one-row MySQL SELECT INTO did not emit a PL/pgSQL assignment");
    good(ora,ora,"CREATE FUNCTION get_owned RETURN NUMBER AS v NUMBER; BEGIN "
                 "SELECT 1 INTO v FROM SYS.DUAL; RETURN v; END;");
    const auto boundInto=compileRoutine(pg,
        "CREATE FUNCTION scoped() RETURNS INTEGER LANGUAGE plpgsql AS $$ "
        "<<outer_block>> DECLARE v INTEGER; BEGIN DECLARE v INTEGER; BEGIN "
        "SELECT INTO outer_block.v 4; END; RETURN v; END outer_block; $$;");
    require(boundInto.ok(),boundInto.diagnostics.empty()?"SELECT INTO did not parse":boundInto.diagnostics.front().message.c_str());
    const auto assigned=std::find_if(boundInto.ast.nodes.begin(),boundInto.ast.nodes.end(),
        [](const auto &node){return node.selectInto.has_value();});
    require(assigned!=boundInto.ast.nodes.end() && assigned->selectInto->targets.size()==1 &&
            boundInto.ast.symbols.at(assigned->selectInto->targets.front()).scope!=assigned->scope,
            "SELECT INTO did not bind the explicitly qualified outer variable");
    require(dumpRoutineModel(boundInto).find("\"selectInto\"")!=std::string::npos,"SELECT INTO absent from owning IR");
    for(const auto sql:{
          "CREATE FUNCTION f() RETURNS INTEGER LANGUAGE plpgsql AS $$ BEGIN SELECT 1 INTO missing; RETURN 1; END; $$;",
          "CREATE FUNCTION f() RETURNS INTEGER LANGUAGE plpgsql AS $$ DECLARE v INTEGER; BEGIN SELECT 1,2 INTO v; RETURN v; END; $$;"})
      require(!compileRoutine(pg,sql).ok(),"unbound or mismatched SELECT INTO targets passed diagnostics");
    bad(pg,my,"CREATE FUNCTION f() RETURNS INTEGER LANGUAGE plpgsql AS $$ DECLARE v INTEGER; "
              "BEGIN SELECT n INTO v FROM t; RETURN v; END; $$;");
    bad(pg,my,"CREATE FUNCTION f() RETURNS INTEGER LANGUAGE plpgsql AS $$ DECLARE v INTEGER; "
              "BEGIN SELECT 1 INTO v WHERE FALSE; RETURN v; END; $$;");
    const auto foundSql="CREATE FUNCTION found_owned() RETURNS BOOLEAN LANGUAGE plpgsql AS $$ DECLARE v INTEGER; "
        "BEGIN SELECT 1 INTO v; RETURN found_owned.FOUND; END; $$;";
    const auto foundRoutine=compileRoutine(pg,foundSql);
    require(foundRoutine.ok() && std::any_of(foundRoutine.ast.symbols.begin(),foundRoutine.ast.symbols.end(),
            [](const auto &s){return s.kind==SymbolKind::Implicit && s.name=="found";}),
            "PL/pgSQL implicit FOUND did not enter the binding scope");
    good(pg,pg,foundSql);
    bad(pg,my,foundSql);
    const auto pgException="CREATE FUNCTION exception_owned(p INTEGER) RETURNS TEXT LANGUAGE plpgsql AS $$ "
        "BEGIN RETURN (10/p)::text; EXCEPTION WHEN SQLSTATE '22012' OR numeric_value_out_of_range "
        "THEN RETURN SQLSTATE; WHEN OTHERS THEN RAISE; END; $$;";
    require(good(pg,pg,pgException).sql.find("SQLSTATE '22012'")!=std::string::npos,
            "exception condition lost its SQLSTATE");
    bad(pg,my,pgException);
    good(ora,ora,"CREATE PROCEDURE p AS BEGIN NULL; EXCEPTION WHEN zero_divide THEN NULL; "
                 "WHEN OTHERS THEN RAISE; END;");
    for (const auto sql : {
        "BEGIN RAISE; END;",
        "BEGIN NULL; EXCEPTION WHEN SQLSTATE 'wrong' THEN NULL; END;",
        "BEGIN NULL; EXCEPTION WHEN OTHERS THEN SQLSTATE:='00000'; END;",
        "BEGIN NULL; EXCEPTION WHEN OTHERS THEN SELECT 'x' INTO SQLERRM; END;",
        "BEGIN BEGIN NULL; EXCEPTION WHEN OTHERS THEN NULL; END; RETURN SQLERRM; END;"})
      require(!compileRoutine(pg, "CREATE FUNCTION f() RETURNS TEXT LANGUAGE plpgsql AS $$ " +
                                  std::string(sql) + " $$;").ok(), "invalid exception routine accepted");
    require(!compileRoutine(ora,"CREATE PROCEDURE p AS BEGIN NULL; EXCEPTION WHEN OTHERS THEN NULL; "
                                "WHEN zero_divide THEN NULL; END;").ok(), "Oracle OTHERS must be last");
    const std::string pgDiagnosticHeader="CREATE FUNCTION f() RETURNS BIGINT LANGUAGE plpgsql AS $$ "
        "<<outer_scope>> DECLARE rows_seen BIGINT; msg TEXT; ";
    const auto pgStatus=pgDiagnosticHeader+"BEGIN GET CURRENT DIAGNOSTICS outer_scope.rows_seen := ROW_COUNT, "
        "msg = PG_CONTEXT; RETURN rows_seen; END outer_scope; $$;";
    const auto diagnosticsRoutine=compileRoutine(pg,pgStatus);
    require(diagnosticsRoutine.ok() && dumpRoutineModel(diagnosticsRoutine).find("\"pgDiagnostics\"")!=std::string::npos,
            "GET DIAGNOSTICS assignments missing from owning routine model");
    good(pg,pg,pgStatus);
    bad(pg,my,pgStatus);
    const auto statusFormat=formatSql(pg,pgDiagnosticHeader+
        "BEGIN get /* status */ current diagnostics rows_seen := row_count, msg = pg_context; "
        "RETURN rows_seen; END outer_scope; $$;");
    require(statusFormat.ok() && statusFormat.sql.find("GET /* status */ CURRENT DIAGNOSTICS rows_seen := ROW_COUNT")!=std::string::npos &&
            statusFormat.sql.find("msg = PG_CONTEXT")!=std::string::npos && compileRoutine(pg,statusFormat.sql).ok(),
            "diagnostics keywords were not formatted with their target names and comments preserved");
    for (const auto sql : {
        "BEGIN GET STACKED DIAGNOSTICS msg = MESSAGE_TEXT; RETURN 0; END;",
        "BEGIN GET DIAGNOSTICS msg = MESSAGE_TEXT; RETURN 0; END;",
        "BEGIN GET DIAGNOSTICS missing = ROW_COUNT; RETURN 0; END;",
        "BEGIN NULL; EXCEPTION WHEN OTHERS THEN GET STACKED DIAGNOSTICS rows_seen = ROW_COUNT; RETURN 0; END;",
        "BEGIN NULL; EXCEPTION WHEN OTHERS THEN GET STACKED DIAGNOSTICS sqlerrm = MESSAGE_TEXT; RETURN 0; END;"})
      require(!compileRoutine(pg,pgDiagnosticHeader+sql+" $$;").ok(),
              "invalid diagnostics area, field or target was accepted");
    good(pg,pg,pgDiagnosticHeader+"BEGIN RETURN 1; EXCEPTION WHEN OTHERS THEN "
               "GET STACKED DIAGNOSTICS msg = MESSAGE_TEXT, msg = PG_CONTEXT; RETURN 0; END; $$;");
    const auto pgFor="CREATE FUNCTION sum_index_owned() RETURNS INTEGER LANGUAGE plpgsql AS $$ "
        "DECLARE i INTEGER:=9; v INTEGER:=0; BEGIN <<outer_loop>> "
        "FOR i IN REVERSE i..1 BY 2 LOOP CONTINUE outer_loop WHEN i=5; "
        "v:=v*10+outer_loop.i; END LOOP outer_loop; RETURN v*10+i; END; $$;";
    const auto forRoutine=compileRoutine(pg,pgFor);
    require(forRoutine.ok(),forRoutine.diagnostics.empty()?"numeric FOR did not parse":forRoutine.diagnostics.front().message.c_str());
    const auto loop=std::find_if(forRoutine.ast.nodes.begin(),forRoutine.ast.nodes.end(),
        [](const auto &node){return node.kind==ProceduralKind::NumericFor;});
    require(loop!=forRoutine.ast.nodes.end() && loop->rangeBounds.size()==3 && loop->reverseFor &&
            forRoutine.ast.symbols.at(loop->target).kind==SymbolKind::LoopIndex &&
            forRoutine.ast.nodes.at(loop->rangeBounds.front()).scope!=loop->scope,
            "loop body and range share the wrong binding scope");
    require(good(pg,pg,pgFor).sql.find(" BY 2 LOOP")!=std::string::npos,"numeric FOR lost step");
    bad(pg,my,pgFor);
    good(ora,ora,"CREATE PROCEDURE p AS BEGIN FOR i IN REVERSE 1..3 LOOP NULL; END LOOP; END;");
    require(!compileRoutine(ora,"CREATE PROCEDURE p AS BEGIN FOR i IN 1..3 LOOP i:=2; END LOOP; END;").ok(),
            "Oracle FOR index accepted assignment");
    require(!compileRoutine(pg,"CREATE FUNCTION f() RETURNS INTEGER LANGUAGE plpgsql AS $$ "
                               "BEGIN FOR i IN 1..3 LOOP NULL; END LOOP; RETURN i; END; $$;").ok(),
            "FOR index escaped its loop scope");
    require(good(my, pg, "TABLE items ORDER BY id LIMIT 3;").sql.find("SELECT * FROM") != std::string::npos,
            "plain TABLE shorthand did not lower to portable SELECT");
    bad(pg, my, "TABLE ONLY public.items;");
    good(pg, pg, "SELECT t.id FROM ONLY (public.items) AS t;");
    good(pg, pg, "UPDATE ONLY public.items AS t SET id=3 WHERE t.id=2;");
    good(pg, pg, "DELETE FROM public.items * AS t WHERE t.id=2;");
    bad(pg, my, "SELECT t.id FROM ONLY public.items t;");
    bad(pg, ms, "UPDATE ONLY public.items SET id=3;");
    good(my, my, "VALUES ROW(1,'a'), ROW(2,'b');");
    bad(pg, my, "VALUES (1,'a'), (2,'b');");
    const auto lateral = "SELECT r.n FROM users u OUTER APPLY (SELECT u.id AS n WHERE u.id>1) r;";
    const auto toPg = good(ms, pg, lateral);
    require(toPg.sql.find("LEFT JOIN LATERAL") != std::string::npos &&
            toPg.sql.find("ON TRUE") != std::string::npos, "OUTER APPLY lost empty-right behavior");
    require(good(pg, ms, toPg.sql).sql.find("OUTER APPLY") != std::string::npos,
            "LEFT JOIN LATERAL did not round-trip to APPLY");
    require(good(ms, ora, lateral).sql.find("OUTER APPLY") != std::string::npos,
            "Oracle target lost OUTER APPLY");
    bad(ms, sq, lateral);
    {
      const auto repeat = "CREATE FUNCTION f_repeat(p INT) RETURNS INT DETERMINISTIC BEGIN "
          "DECLARE v INT DEFAULT 0; REPEAT SET v=v+1; -- after the body\n"
          "UNTIL v=3 OR v>=p END REPEAT; RETURN v; END;";
      const auto native = compileRoutine(my, repeat);
      require(native.ok(), "MySQL REPEAT failed to compile");
      const auto loop = std::find_if(native.model.nodes.begin(), native.model.nodes.end(),
          [](const auto &node) { return node.syntax.kind == ProceduralKind::Repeat; });
      require(loop != native.model.nodes.end() && loop->syntax.loop && loop->syntax.children.size() == 1 &&
              std::any_of(loop->code.front().expressions.begin(), loop->code.front().expressions.end(),
                          [](const auto &e) { return e.localSymbol != 0; }),
              "REPEAT condition lost its loop or routine variable binding");
      good(my, my, repeat);
      const auto pgLoop = good(my, pg, repeat);
      require(pgLoop.sql.find("EXIT WHEN") != std::string::npos, "post-test was rewritten as a pre-test");
      require(good(pg, my, pgLoop.sql).sql.find("END REPEAT;") != std::string::npos,
              "terminal EXIT WHEN did not round-trip to REPEAT");
      bad(my, ms, repeat);
      const auto nested = compileRoutine(pg,
          "CREATE FUNCTION f(p INT) RETURNS INT LANGUAGE plpgsql AS $$ BEGIN "
          "LOOP WHILE p>1 LOOP EXIT WHEN p>2; END LOOP; EXIT WHEN p>3; END LOOP; RETURN p; END; $$;");
      require(nested.ok(), "unlabeled nested EXIT targets did not compile");
      std::vector<std::uint32_t> targets;
      for (const auto &node : nested.ast.nodes)
        if (node.kind == ProceduralKind::Exit) targets.push_back(node.loop);
      require(targets.size() == 2 && targets[0] != targets[1], "EXIT missed the nearest enclosing loop");
      const auto early = "CREATE FUNCTION f(p INT) RETURNS INT LANGUAGE plpgsql AS $$ BEGIN "
          "LOOP IF p>1 THEN EXIT; END IF; p:=p+1; EXIT WHEN p>3; END LOOP; RETURN p; END; $$;";
      good(pg, pg, early);
      require(good(pg, my, early).sql.find("LEAVE") != std::string::npos,
              "early EXIT missed the generated MySQL loop label");
      const auto labeled = "CREATE FUNCTION f(p INT) RETURNS INT LANGUAGE plpgsql AS $$ BEGIN "
          "<<outer_label>> LOOP p:=p+1; <<inner_label>> LOOP EXIT outer_label WHEN p>=4; "
          "CONTINUE outer_label WHEN p<2; EXIT inner_label; END LOOP inner_label; "
          "EXIT WHEN p>=3; END LOOP outer_label; RETURN p; END; $$;";
      const auto mysqlJumps = good(pg, my, labeled);
      require(mysqlJumps.sql.find("ITERATE") != std::string::npos, "outer CONTINUE did not generate ITERATE");
      good(my, pg, mysqlJumps.sql);
      const auto iterateRepeat = "CREATE FUNCTION f() RETURNS INT DETERMINISTIC BEGIN "
          "DECLARE n INT DEFAULT 0; again: REPEAT SET n=n+1; IF n=1 THEN ITERATE again; END IF; "
          "UNTIL n>=1 END REPEAT again; RETURN n; END;";
      require(good(my, pg, iterateRepeat).sql.find("CONTINUE") != std::string::npos,
              "REPEAT ITERATE lost control transfer before the tail test");
      good(my, my, iterateRepeat);
      auto rejects = [&](Dialect dialect, const char *definition, DiagnosticCode code) {
        const auto invalid = compileRoutine(dialect, definition);
        require(!invalid.ok() && !invalid.diagnostics.empty() && invalid.diagnostics.front().code == code,
                "invalid loop label produced the wrong diagnostic");
      };
      rejects(my, "CREATE FUNCTION f() RETURNS INT BEGIN a: LOOP LEAVE a; END LOOP b; RETURN 1; END;",
              DiagnosticCode::InvalidQueryShape);
      rejects(my, "CREATE FUNCTION f() RETURNS INT BEGIN a: LOOP a: LOOP LEAVE a; END LOOP; END LOOP; RETURN 1; END;",
              DiagnosticCode::DuplicateAlias);
      rejects(my, "CREATE FUNCTION f() RETURNS INT BEGIN a: LOOP LEAVE a; END LOOP; ITERATE a; RETURN 1; END;",
              DiagnosticCode::InvalidQueryShape);
      rejects(pg, "CREATE FUNCTION f() RETURNS INT LANGUAGE plpgsql AS $$ BEGIN <<a>> LOOP "
                  "<<a>> BEGIN EXIT a; END; EXIT; END LOOP; RETURN 1; END; $$;",
              DiagnosticCode::UnsupportedSyntax);
      require(!compileRoutine(pg, "CREATE FUNCTION f() RETURNS INT LANGUAGE plpgsql AS $$ BEGIN "
                                  "EXIT WHEN TRUE; RETURN 1; END; $$;").ok(),
              "EXIT outside a loop compiled");
      require(!compileRoutine(my, "CREATE FUNCTION f() RETURNS INT BEGIN DECLARE v INT DEFAULT 0; "
                                  "REPEAT SET v=v+1; UNTIL missing>0 END REPEAT; RETURN v; END;").ok(),
              "undeclared UNTIL variable compiled");
    }
    const auto derived = "SELECT d.x,d.y FROM (SELECT 1 AS a,2 AS b) AS d(x,y);";
    good(pg, ms, derived);
    good(my, pg, derived);
    bad(pg, my, "SELECT d.x FROM (SELECT 1 AS a,2 AS b) AS d(x);");
    bad(pg, sq, derived);
    const auto literalRows = "SELECT d.id,d.n FROM (VALUES (1,NULL),(2,-3)) AS d(id,n);";
    good(ms, ms, literalRows);
    good(ms, pg, literalRows);
    good(pg, ms, literalRows);
    bad(ms, pg, "SELECT d.id FROM (VALUES (NULL)) AS d(id);");
    bad(ms, pg, "SELECT d.id FROM (VALUES (1),('2')) AS d(id);");
    bad(pg, ms, "VALUES (1),(2);");
    bad(pg, ms, "SELECT d.column1 FROM (VALUES (1)) AS d;");
    good(pg,my,"SELECT CAST(42 AS pg_catalog.int8);");
    good(pg,ms,"SELECT CAST(42 AS pg_catalog.int4);");
    bad(pg,ms,"SELECT CAST(42 AS app.int4);");
    bad(pg,ms,"SELECT CAST(42 AS pg_catalog.\"INT4\");");
    for(auto mode:{"NO OTHERS","CURRENT ROW","GROUP","TIES"}) {
      const auto sql=std::string("SELECT COUNT(*) OVER(ORDER BY x ROWS BETWEEN UNBOUNDED PRECEDING AND UNBOUNDED FOLLOWING EXCLUDE ")+mode+") FROM t;";
      auto r=good(pg,sq,sql);require(r.sql.find(std::string("EXCLUDE ")+mode)!=std::string::npos,"conversion lost exclusion semantics");
      good(sq,pg,r.sql);
      if(std::string(mode)=="NO OTHERS") {auto target=good(pg,my,"SELECT COUNT(*) OVER(ROWS UNBOUNDED PRECEDING EXCLUDE NO OTHERS) FROM t;");require(target.sql.find("EXCLUDE")==std::string::npos,"default exclusion was not normalized for target");}
      else bad(pg,my,sql);
    }
    bad(pg,my,"SELECT SUM(CAST(1 AS INTEGER));");
    bad(pg,ms,"SELECT SUM(CAST(1 AS INTEGER));");
    bad(pg,ms,"SELECT AVG(CAST(1 AS INTEGER));");
    bad(ms,pg,"SELECT SUM(CAST(1 AS INTEGER));");
    bad(my,pg,"SELECT SUM(CAST(1 AS SIGNED));");
    bad(pg,ms,"SELECT SUM(CAST(1 AS BIGINT));");
    good(pg,pg,"SELECT SUM(CAST(1 AS INTEGER));");
    {
      MemoryCatalog typed;TableDefinition table;table.name="future_values";
      SqlType type;type.family=TypeFamily::Floating;type.bits=64;
      table.columns={{"n",ValueType::Decimal,std::make_shared<const SqlType>(type)}};typed.tables.push_back(table);
      ConversionOptions options;options.catalog=&typed;
      good(pg,ms,"SELECT SUM(n) FROM future_values;",options);
    }
    good(Dialect::SQLite,Dialect::SQLite,"SELECT TOTAL(NULL);");
    bad(Dialect::SQLite,pg,"SELECT TOTAL(NULL);");
    for(const auto &sql:{"SELECT CAST(42 AS BIGINT);","SELECT CAST(-9223372036854775808 AS INT8);","SELECT CAST('9223372036854775807' AS BIGINT);","SELECT CAST(NULL AS BIGINT);"}) {
      auto r=good(pg,my,sql);require(r.sql.find("SIGNED")!=std::string::npos,"64-bit PG CAST did not use the native MySQL target");
    }
    for(const auto &sql:{"SELECT CAST(42 AS SIGNED);","SELECT CAST(-9223372036854775808 AS SIGNED);","SELECT CAST(NULL AS SIGNED INTEGER);"}) {
      auto r=good(my,pg,sql);require(r.sql.find("BIGINT")!=std::string::npos,"MySQL SIGNED result narrowed to PostgreSQL INTEGER");
    }
    good(pg,ms,"SELECT CAST(-2147483648 AS INT4),CAST('2147483647' AS INTEGER);");
    good(ms,pg,"SELECT CAST(42 AS INTEGER);");
    for(const auto &sql:{"SELECT CAST(1.5 AS INTEGER);","SELECT CAST('1.5' AS INTEGER);","SELECT CAST(2147483648 AS INTEGER);","SELECT CAST(-2147483649 AS INTEGER);","SELECT CAST(unknown_value AS INTEGER) FROM future_table;"})bad(pg,ms,sql);
    bad(pg,my,"SELECT CAST(42 AS INTEGER);");
    bad(pg,my,"SELECT CAST(9223372036854775808 AS BIGINT);");
    bad(my,pg,"SELECT CAST(-1 AS UNSIGNED);");
    bad(sq,pg,"SELECT CAST('2024-01-01' AS DATE);");
    bad(pg,sq,"SELECT CAST('2024-01-01' AS DATE);");
    bad(ora,pg,"CREATE TABLE future_table(id INTEGER);");
    bad(ora,pg,"SELECT CAST(42 AS INTEGER) FROM SYS.DUAL;");
    {
      MemoryCatalog typed;TableDefinition table;table.name="future_table";
      SqlType type;type.family=TypeFamily::Integer;type.bits=32;type.unsignedInteger=false;
      table.columns={{"id",ValueType::Integer,std::make_shared<const SqlType>(type)}};typed.tables.push_back(table);
      ConversionOptions options;options.catalog=&typed;
      good(pg,ms,"SELECT CAST(id AS INTEGER) FROM future_table;",options);
    }
    for(auto source:{my,pg,ora})for(auto target:{my,pg,ora,ms}) {
      auto sql=std::string("SELECT DATE '2024-02-29', DATE '2000-01-01'")+(source==ora?" FROM SYS.DUAL;":";");
      auto converted=good(source,target,sql);
      require(compile(target,converted.sql)->ok(),"converted date literal does not parse in target");
      if(target==ms)require(converted.sql.find("20240229")!=std::string::npos && converted.sql.find("CAST")!=std::string::npos,"SQL Server date mapping is locale dependent");
    }
    bad(pg,my,"SELECT DATE '2024-01-01'+1;");
    bad(pg,my,"SELECT DATE '2024-01-01'-DATE '2023-01-01';");
    bad(pg,my,"SELECT COALESCE(DATE '2024-01-01','2023-01-01');");
    bad(pg,my,"SELECT TIMESTAMP WITH TIME ZONE '2024-01-01 12:00:00+08';");
    bad(pg,my,"SELECT NUMERIC(5,2) '1.23';");
    bad(pg,my,"SELECT DATE 'infinity';");
    bad(pg,my,"SELECT DATE '0999-01-01';");
    bad(pg,my,"SELECT DATE '2023-02-29';");
    bad(pg,sq,"SELECT DATE '2024-01-01';");
    {
      bool observed=false;
      for(std::size_t limit=2;limit<24;++limit) {
        ConversionOptions bounded;bounded.language.limits.nodes=limit;
        auto r=convertSql(pg,ms,"SELECT DATE '2024-01-01',DATE '2024-01-02',DATE '2024-01-03';",bounded);
        if(!r.ok())for(const auto &d:r.diagnostics)if(d.message.find("target expression node limit")!=std::string::npos) {require(r.sql.empty(),"budget failure published partial SQL");observed=true;}
      }
      require(observed,"target expression growth limit was not exercised");
    }
    good(my,my,"SHOW FULL TABLES FROM future_db WHERE table_type='VIEW';");
    good(my,my,"SHOW CREATE TABLE future_db.future_table;");
    bad(my,pg,"SHOW TABLES;");
    bad(my,Dialect::MariaDB,"SHOW TABLES;");
    good(my,my,"REPLACE INTO future_table(id) VALUES(3);");
    bad(my,pg,"REPLACE INTO future_table(id) VALUES(3);");
    {
      const auto defaults=good(my,my,"CREATE TABLE owned_defaults(b BLOB DEFAULT ('x'), "
                                     "d DATE DEFAULT (CURRENT_DATE), n JSON DEFAULT NULL);");
      require(defaults.sql.find("DEFAULT ('x')")!=std::string::npos,
              "MySQL blob default lost required parentheses");
      for (auto statement : {"CREATE TABLE bad_default(b BLOB DEFAULT 'x');",
                             "CREATE TABLE bad_default(b JSON DEFAULT '{}');",
                             "CREATE TABLE bad_default(b POINT DEFAULT 1);",
                             "CREATE TABLE bad_default(b BLOB NOT NULL DEFAULT NULL);"}) {
        require(!compile(my,statement)->syntaxValid,"invalid MySQL DDL default reached the model");
      }
      good(my,my,"ALTER TABLE owned_defaults ALTER COLUMN d SET DEFAULT (CURRENT_DATE);");
    }
    good(my,my,"SELECT a.id FROM a STRAIGHT_JOIN b ON b.id=a.id;");
    good(my,my,"SELECT DISTINCT STRAIGHT_JOIN a.id FROM a JOIN b ON b.id=a.id;");
    good(my,my,"SELECT a.id FROM a STRAIGHT_JOIN b;");
    good(my,my,R"(SELECT j.x FROM (SELECT '[]' AS doc) d,
       JSON_TABLE(d.doc,'$[*]' COLUMNS(x INT PATH '$' NULL ON ERROR)) j;)");
    bad(my,pg,R"(SELECT j.x FROM JSON_TABLE('[1]','$[*]' COLUMNS(x INT PATH '$')) j;)");
    {
      const auto roles = compile(my, "SET /* mode */ ROLE ALL EXCEPT `Reader`@localhost, 'Audit'; "
                                     "SET DEFAULT ROLE 'Reader'@'localhost' TO 'user1'@'localhost', user2; "
                                     "SET ROLE NONE; SET ROLE DEFAULT;");
      require(roles->ok(), "MySQL role controls did not compile");
      const auto &active = *roles->model[0].role;
      const auto &defaults = *roles->model[1].role;
      require(active.mode == RoleMode::AllExcept && active.roles[0].name == "Reader" &&
              active.roles[0].host == "localhost" && !active.roles[1].host,
              "role accounts lost the separate user and host parts");
      require(defaults.scope == RoleScope::AccountDefault && defaults.recipients.size() == 2 &&
              roles->model[1].transactionEffect == TransactionEffect::ImplicitCommit,
              "account default role scope or effects were lost");
      const auto generated = generateSql(roles->model, my);
      require(generated.ok() && compile(my, generated.sql)->ok(), "canonical role controls did not reparse");
      for (const auto *invalid : {"SET ROLE ALL EXCEPT;", "SET ROLE r@;", "SET DEFAULT ROLE ALL;",
                                  "SET DEFAULT ROLE DEFAULT TO user1;", "SET DEFAULT ROLE ALL TO CURRENT_USER;",
                                  "SET DEFAULT ROLE ALL EXCEPT r TO user1;"})
        require(!compile(my, invalid)->ok(), "invalid role control compiled");
      bad(my, pg, "SET ROLE NONE;");
      good(my, my, "set default role all to 'user1'@'localhost';");
    }
    bad(my,pg,"SELECT a.id FROM a STRAIGHT_JOIN b ON b.id=a.id;");
    bad(my,pg,"SELECT STRAIGHT_JOIN a.id FROM a JOIN b ON b.id=a.id;");
    require(!compile(my,"SELECT a.id FROM a STRAIGHT_JOIN b USING(id);")->ok(),
            "STRAIGHT_JOIN accepted unsupported USING");
    good(my,my,"START TRANSACTION WITH CONSISTENT SNAPSHOT, READ ONLY; SAVEPOINT sp; "
               "ROLLBACK TO sp; RELEASE SAVEPOINT sp; COMMIT AND NO CHAIN NO RELEASE;");
    good(pg,pg,"START TRANSACTION ISOLATION LEVEL SERIALIZABLE READ ONLY DEFERRABLE; "
               "SAVEPOINT s; ROLLBACK WORK TO s; RELEASE s; END AND CHAIN; ABORT;");
    good(sq,sq,"BEGIN IMMEDIATE TRANSACTION; SAVEPOINT s; ROLLBACK TRANSACTION TO s; "
               "RELEASE s; END TRANSACTION;");
    good(ora,ora,"SAVEPOINT s; ROLLBACK WORK TO s; COMMIT WORK;");
    good(ms,ms,"BEGIN TRAN t; SAVE TRAN s; ROLLBACK TRAN s; COMMIT TRAN t;");
    bad(my,pg,"START TRANSACTION; COMMIT;");
    bad(pg,ora,"SAVEPOINT s;");
    {
      const auto sql="PREPARE find_item FROM 'SELECT ''?'' AS literal_q, ? AS n /* ? */;'; "
                     "EXECUTE find_item USING @value; DROP PREPARE find_item;";
      auto converted=good(my,my,sql);
      require(converted.sql.find("DEALLOCATE PREPARE")!=std::string::npos,
              "prepared command canonicalization lost deallocation");
      const auto prepared=compile(my,"PREPARE find_item FROM 'SELECT ''?'', ? /* ? */';");
      require(prepared->ok() && prepared->model.front().prepared->parameterCount==1,
              "parameter count included string or comment markers");
      require(!checkSqlScript(my,"PREPARE p FROM 'SELECT ?'; EXECUTE p;").ok(),
              "known prepared arity mismatch was accepted");
      require(!checkSqlScript(my,"PREPARE p FROM 'SELECT ?'; EXECUTE p USING @a; "
                                 "ROLLBACK; EXECUTE P USING @a,@b;").ok(),
              "rollback or execution erased a known session handle signature");
      require(!checkSqlScript(my,"PREPARE p FROM 'SELECT 1'; DEALLOCATE PREPARE p; EXECUTE P;").ok(),
              "deallocated statement handle was still considered present");
      require(checkSqlScript(my,"PREPARE p FROM @sql_text; EXECUTE p USING @a,@b; DEALLOCATE PREPARE p;").ok(),
              "dynamic statement was assigned an invented parameter count");
      require(checkSqlScript(my,"EXECUTE existing_connection_handle USING @a;").ok(),
              "execution assumed a session could contain only local handles");
      require(!compile(my,"EXECUTE p USING @@max_prepared_stmt_count;")->ok(),
              "system variable was accepted in EXECUTE USING");
      bad(my,pg,sql);
    }
    for (const auto dialect : {pg, sq}) {
      ScriptContext context(dialect, nullptr);
      auto observe = [&](const char *sql) {
        const auto compiled=compile(dialect,sql,{},&context);
        if (!compiled->ok()) throw std::runtime_error(compiled->diagnostics.front().message+" in "+sql);
        for (auto root : compiled->model.roots)
          require(!context.observe(compiled->model,root),"transaction declaration failed");
      };
      observe("CREATE TABLE old_decl(id INTEGER);");
      observe("BEGIN;");
      observe("DROP TABLE old_decl;");
      observe("CREATE TABLE kept_decl(id INTEGER);");
      observe("SAVEPOINT step;");
      observe("CREATE TABLE undone_decl(id INTEGER);");
      observe("CREATE INDEX discarded_ix ON undone_decl(id);");
      observe("SAVEPOINT extra;");
      observe("DROP TABLE kept_decl;");
      RoutineDefinition routine;
      routine.name="discarded_function";
      require(!context.declareRoutine({"discarded_function"},routine),"routine declaration failed");
      observe("ROLLBACK TO step;");
      require(!context.findTable({},{"undone_decl"}) && context.findTable({},{"kept_decl"}) &&
              !context.findTable({},{"old_decl"}) &&
              context.listObjects({},CatalogObjectKind::Index,10).empty() &&
              context.findRoutines({},{"discarded_function"},false).empty(),
              "savepoint did not restore table, index, and routine declarations");
      observe("CREATE TABLE later_decl(id INTEGER);");
      observe("ROLLBACK TO step;");
      require(!context.findTable({},{"later_decl"}),"rollback removed its own savepoint");
      observe("RELEASE SAVEPOINT step;");
      observe("ROLLBACK;");
      require(context.findTable({},{"old_decl"}) && !context.findTable({},{"kept_decl"}),
              "transaction rollback did not restore the state before BEGIN");
    }
    {
      ScriptContext context(sq,nullptr);
      const auto compiled=compile(sq,"SAVEPOINT p; CREATE TABLE committed(id INT); "
                                     "SAVEPOINT q; CREATE TABLE included(id INT); RELEASE p; "
                                     "BEGIN; DROP TABLE committed; ROLLBACK;");
      require(compiled->ok(),"SQLite outer savepoint setup did not parse");
      for (auto root : compiled->model.roots)
        require(!context.observe(compiled->model,root),"SQLite outer savepoint transition failed");
      require(context.findTable({},{"committed"}) && context.findTable({},{"included"}),
              "SQLite outermost RELEASE failed to preserve committed declarations");
    }
    good(my,pg,"SELECT a,b,COUNT(*) FROM t GROUP BY a,b WITH ROLLUP;");
    good(pg,ms,"SELECT a,b,COUNT(*) FROM t GROUP BY CUBE(a,b);");
    good(pg,ora,"SELECT a,COUNT(*) FROM t GROUP BY GROUPING SETS((a),());");
    bad(pg,my,"SELECT a,b,COUNT(*) FROM t GROUP BY CUBE(a,b);");
    bad(pg,sq,"SELECT a,COUNT(*) FROM t GROUP BY ROLLUP(a);");
    for (const auto &prefix : {"CREATE PROCEDURE", "CREATE OR ALTER PROC", "ALTER PROCEDURE"}) {
      const std::string routine = std::string(prefix) + " dbo.p AS BEGIN "
          "SET NOCOUNT ON; DECLARE @v INT = 1; SELECT @v; SET NOCOUNT OFF; END;";
      auto compiled = compileRoutine(ms, routine);
      require(compiled.ok(), "T-SQL no-argument procedure / NOCOUNT did not bind");
      require(std::any_of(compiled.ast.nodes.begin(), compiled.ast.nodes.end(),
                          [](const auto &node) { return node.kind == ProceduralKind::SetNoCountOn; }),
              "T-SQL NOCOUNT disappeared from the procedural AST");
      auto canonical = good(ms, ms, routine);
      require(canonical.sql.find("SET NOCOUNT ON;") != std::string::npos &&
              canonical.sql.find("SET NOCOUNT OFF;") != std::string::npos,
              "native generation changed procedure settings");
      if (std::string(prefix) == "CREATE OR ALTER PROC")
        require(canonical.sql.find("CREATE OR ALTER PROCEDURE") == 0, "replacement action lost");
      if (std::string(prefix) == "ALTER PROCEDURE")
        require(canonical.sql.find("ALTER PROCEDURE") == 0, "ALTER became CREATE");
      bad(ms, pg, routine);
    }
    require(!compileRoutine(ms, "CREATE PROCEDURE dbo.p AS BEGIN SET NOCOUNT 'ON'; END;").ok(),
            "invalid quoted SET option accepted");
    require(!compileRoutine(ms, "CREATE FUNCTION dbo.f() RETURNS INT AS BEGIN SET NOCOUNT ON; RETURN 1; END;").ok(),
            "session-setting statement accepted in a T-SQL function");
    const std::string tryCatchSql="CREATE PROC dbo.p AS BEGIN TRY DECLARE @prior INT = 7; "
        "SELECT @prior END TRY BEGIN CATCH BEGIN TRY SELECT ERROR_NUMBER(), @prior; "
        "END TRY BEGIN CATCH SELECT ERROR_MESSAGE() END CATCH; THROW END CATCH;";
    const auto tried=compileRoutine(ms,tryCatchSql);
    require(tried.ok() && std::count_if(tried.ast.nodes.begin(),tried.ast.nodes.end(),
            [](const auto &node){return node.kind==ProceduralKind::TsqlTryCatch;})==2,
            "nested TRY/CATCH or procedure-scoped binding lost");
    const auto nativeTry=good(ms,ms,tryCatchSql);
    require(nativeTry.sql.find("END TRY")!=std::string::npos &&
            nativeTry.sql.find("BEGIN CATCH")!=std::string::npos &&
            nativeTry.sql.find("THROW;")!=std::string::npos && checkSqlScript(ms,nativeTry.sql).ok(),
            "TRY/CATCH or rethrow missing in native generation");
    const auto tryFormat=formatSql(ms,tryCatchSql);
    require(tryFormat.ok() && compileRoutine(ms,tryFormat.sql).ok() &&
            formatSql(ms,tryFormat.sql).sql==tryFormat.sql,
            "T-SQL TRY/CATCH formatting failed to preserve the procedure body");
    good(ms,ms,"CREATE PROCEDURE p AS BEGIN TRY SELECT 1 END TRY BEGIN CATCH END CATCH;");
    const auto raised=good(ms,ms,"CREATE PROC p AS BEGIN TRY DECLARE @state TINYINT=2; "
        "THROW 50000, N'错误: ''值'' 20%%', @state; END TRY BEGIN CATCH SELECT ERROR_STATE(); END CATCH;");
    require(raised.sql.find("THROW 50000, N'错误: ''值'' 20%%', @")!=std::string::npos &&
            raised.modelJson.find("\"throwArguments\"")!=std::string::npos && checkSqlScript(ms,raised.sql).ok(),
            "T-SQL THROW lost a bound parameter or native Unicode message");
    bad(ms,pg,"CREATE PROCEDURE p AS BEGIN THROW 50000, N'failed', 1; END;");
    bad(ms,pg,tryCatchSql);
    for (const auto sql : {
        "CREATE PROCEDURE p AS BEGIN THROW; END;",
        "CREATE PROCEDURE p AS BEGIN THROW 49999, N'low', 0; END;",
        "CREATE PROCEDURE p AS BEGIN THROW 50000, N'bad state', 256; END;",
        "CREATE PROCEDURE p AS BEGIN THROW 50000, @undeclared, 1; END;",
        "CREATE PROCEDURE p AS BEGIN TRY END TRY BEGIN CATCH END CATCH;",
        "CREATE PROCEDURE p AS BEGIN TRY SELECT 1; END TRY SELECT 2; BEGIN CATCH END CATCH;",
        "CREATE PROCEDURE p AS BEGIN TRY SELECT 1; END TRY BEGIN CATCH RAISE; END CATCH;",
        "CREATE PROCEDURE p AS BEGIN TRY SELECT 1; END TRY BEGIN CATCH SELECT @future; END CATCH;",
        "CREATE FUNCTION f() RETURNS INT AS BEGIN BEGIN TRY RETURN 1; END TRY BEGIN CATCH RETURN 0; END CATCH; END;"})
      require(!compileRoutine(ms,sql).ok(),"invalid T-SQL exception control flow accepted");
    {
      const std::string simple = "CREATE FUNCTION p_case(p INT) RETURNS INT DETERMINISTIC "
          "SQL SECURITY INVOKER BEGIN DECLARE v INT DEFAULT p; "
          "CASE v WHEN 1 THEN SET v=8; WHEN NULL THEN SET v=9; ELSE SET v=0; END CASE; RETURN v; END;";
      auto bound = compileRoutine(my, simple);
      require(bound.ok(), "simple procedural CASE did not bind");
      const auto tail = std::find_if(bound.model.nodes.begin(), bound.model.nodes.end(),
          [](const auto &entry) { return entry.syntax.kind == ProceduralKind::Case; });
      require(tail != bound.model.nodes.end() && tail->syntax.hasElse &&
              tail->syntax.children.size() == 2 &&
              std::any_of(tail->code.front().expressions.begin(), tail->code.front().expressions.end(),
                          [](const auto &e) { return e.localSymbol != 0; }),
              "simple CASE lost its selector binding or branch structure");
      const auto convertedCase = good(my, pg, simple);
      require(convertedCase.sql.find("END CASE;") != std::string::npos, "selector was expanded to repeated conditions");
      good(pg, my, convertedCase.sql);
      good(my, ora, "CREATE PROCEDURE p_case() SQL SECURITY INVOKER BEGIN "
                    "CASE 1 WHEN 1 THEN DO 0; ELSE DO 0; END CASE; END;");
      good(ora, ora, "CREATE PROCEDURE p_case AS n NUMBER; BEGIN "
                     "CASE n WHEN 1 THEN NULL; ELSE NULL; END CASE; END;");
      bad(my, ms, simple);
      const auto searched = "CREATE FUNCTION p_case(p INT) RETURNS INT LANGUAGE plpgsql IMMUTABLE "
          "AS $$ BEGIN CASE WHEN p IS NULL THEN RETURN 0; -- preserve comment\n ELSE RETURN p; END CASE; END; $$;";
      good(pg, my, searched);
      const auto values = "CREATE FUNCTION p_case(p INT) RETURNS INT LANGUAGE plpgsql IMMUTABLE "
          "AS $$ BEGIN CASE p WHEN 1,-2 THEN RETURN 7; ELSE RETURN 0; END CASE; END; $$;";
      good(pg, pg, values);
      bad(pg, my, values);
      const auto absentElse = "CREATE FUNCTION p_case(p INT) RETURNS INT LANGUAGE plpgsql IMMUTABLE "
          "AS $$ BEGIN CASE p WHEN 1 THEN RETURN 1; END CASE; END; $$;";
      good(pg, pg, absentElse);
      bad(pg, my, absentElse);
      const auto invalid = compileRoutine(pg, "CREATE FUNCTION p_case(p INT) RETURNS INT LANGUAGE plpgsql "
          "AS $$ BEGIN CASE WHEN p THEN RETURN 1; ELSE RETURN 0; END CASE; END; $$;");
      require(!invalid.ok() && !invalid.diagnostics.empty() &&
              invalid.diagnostics.front().message.find("condition requires a boolean") != std::string::npos,
              "searched WHEN did not use procedural condition validation");
      const std::string unknown = "CREATE PROCEDURE p_case() BEGIN CASE 2 WHEN typo THEN SELECT 1; END CASE; END;";
      const auto diagnostic = compileRoutine(my, unknown);
      require(!diagnostic.ok() && diagnostic.diagnostics.front().code == DiagnosticCode::UnknownColumn &&
              diagnostic.diagnostics.front().range.begin == unknown.find("typo"),
              "CASE operand diagnostic did not point at the unknown symbol");
      require(compileRoutine(my, "CREATE PROCEDURE p_case() BEGIN CASE 'WHEN' WHEN 'THEN' THEN "
                                 "SELECT 'END', 'CASE', ';'; END CASE; END;").ok(),
              "quoted CASE markers were mistaken for control tokens");
      require(!compileRoutine(my, "CREATE PROCEDURE p_case() BEGIN CASE 1 WHEN 1 THEN "
                                  "SELECT 1; END; END;").ok(), "CASE accepted a missing END CASE");
    }
    EmptyDeployedCatalog deployed;
    ConversionOptions hypothetical;hypothetical.catalog=&deployed;
    require(!compile(pg,"SELECT id FROM future_orders;",{},&deployed)->ok(),
            "ordinary diagnostics lost strict missing-object checks");
    for(auto text:{"SELECT o.id FROM future_orders o JOIN future_customers c ON c.id=o.customer_id;",
                   "INSERT INTO future_orders(id) SELECT id FROM future_source;",
                   "UPDATE future_orders SET n=1 WHERE id=2;",
                   "DELETE FROM future_orders WHERE id=2;"})
      good(my,pg,text,hypothetical);
    good(pg,pg,"SELECT future_schema.future_function(id) FROM future_orders;",hypothetical);
    good(pg,pg,"CREATE VIEW future_view AS SELECT id FROM future_orders;",hypothetical);
    bad(pg,pg,"CREATE TABLE owned_shape(id INTEGER);SELECT missing FROM owned_shape;",hypothetical);
    auto unmapped=convertSql(pg,my,"SELECT future_schema.future_function(id) FROM future_orders;",hypothetical);
    require(!unmapped.ok() && !unmapped.diagnostics.empty(),"unmapped routine lost capability check");
    for(const auto& d:unmapped.diagnostics)
      require(d.code!=DiagnosticCode::UnknownTable && d.code!=DiagnosticCode::UnknownRoutine,
              "conversion still requires deployed objects");
    {
      MemoryCatalog routines;
      RoutineDefinition own;own.schema="app";own.name="abs";own.minArguments=own.maxArguments=1;
      own.kind=RoutineKind::Scalar;own.volatility=FunctionVolatility::Immutable;own.resultType=ValueType::Integer;
      routines.routines.push_back(own);
      ConversionOptions context;context.catalog=&routines;context.names.schema="app";
      // A same-spelled deployed UDF is not the portable builtin. Preserve it
      // for the same dialect, require an explicit mapping across dialects.
      good(pg,pg,"SELECT app.abs(1);",context);
      bad(pg,my,"SELECT app.abs(1);",context);
      bad(pg,my,"SELECT abs(1);",context);
    }
    for (auto sql :
         {"SELECT t.id FROM t LEFT JOIN u ON t.id=u.id WHERE t.id IN(SELECT id "
          "FROM z) LIMIT 2;",
          "INSERT INTO t(id,n) VALUES(1,2),(2,NULL);",
          "INSERT INTO t(id) SELECT id FROM u;",
          "UPDATE t SET n=n+1 WHERE id=1;", "DELETE FROM t WHERE id=2;",
          "WITH x AS(SELECT id FROM t) SELECT id,ROW_NUMBER() OVER(ORDER BY "
          "id) rn FROM x;"})
      good(my, pg, sql);
    good(pg, my, "UPDATE t SET n=n+1 WHERE id=1;");
    for(auto source:{my,pg}) {
      auto target=source==my?pg:my;
      bad(source,target,"UPDATE t SET a=a+1,b=a;");
      bad(source,target,"UPDATE t SET a=a+1,b=COALESCE(a,0);");
      bad(source,target,"UPDATE t SET a=a+1,b=(SELECT a);");
      good(source,target,"UPDATE t SET a=1,b=2;");
    }
    good(my,my,"UPDATE t SET a=a+1,b=a;");
    bad(my,pg,"SELECT 1 FROM t HAVING 1=1;");
    bad(pg,my,"SELECT 1 FROM t HAVING 1=1;");
    good(my,pg,"SELECT COUNT(*) FROM t HAVING COUNT(*)>0;");
    bad(my,pg,"UPDATE t SET a=1,a=2;");
    good(pg,pg,"SELECT x IS DISTINCT FROM NULL,x NOT ILIKE 'abc' FROM t;");
    bad(pg,my,"SELECT x IS DISTINCT FROM NULL FROM t;");
    good(sq, sq, "SELECT 2 IS TRUE, 2==1, NULL IS DISTINCT FROM 1;");
    bad(sq, pg, "SELECT 2 IS TRUE;");
    bad(sq, pg, "SELECT FALSE FROM unknown_table;");
    bad(pg,my,"SELECT x NOT ILIKE 'abc' FROM t;");
    auto ordered = good(my, pg, "SELECT id FROM t ORDER BY id DESC LIMIT 2;");
    require(ordered.sql.find("NULLS LAST") != std::string::npos,
            "NULL order lost");
    auto top = good(my, ms, "SELECT id FROM t LIMIT 2;");
    require(top.sql.find("TOP") != std::string::npos, "TOP not lowered");
    auto windowNulls=good(my,pg,"SELECT ROW_NUMBER() OVER w FROM t WINDOW w AS(ORDER BY id);");
    require(windowNulls.sql.find("NULLS FIRST")!=std::string::npos,"window NULL ordering was lost");
    bad(pg,my,"SELECT ROW_NUMBER() OVER(ORDER BY id) FROM t;");
    good(pg,my,"SELECT ROW_NUMBER() OVER(ORDER BY id NULLS FIRST) FROM t;");
    auto batches=checkSqlScript(ms,"SELECT 1\nGO\nSELECT 2\nGO\n");require(batches.ok()&&batches.statements==2,"GO batch boundaries were discarded");
    auto mixed=checkSqlScript(my,"DELIMITER $$\nCREATE FUNCTION f(p INTEGER) RETURNS INTEGER DETERMINISTIC RETURN p+1;$$\nDELIMITER ;\nSELECT 1;");require(mixed.ok()&&mixed.routines==1&&mixed.statements==2,"mixed routine/script check failed");
    {
      const std::string source =
          "DELIMITER //\nCREATE PROCEDURE cursor_owned(IN p_limit INT, OUT total INT) "
          "READS SQL DATA BEGIN "
          "DECLARE v_id, v_amount INT; DECLARE done INT DEFAULT 0; "
          "DECLARE rows_found CURSOR FOR WITH matches AS "
          "(SELECT 1 AS id, 2 AS amount) SELECT id, amount FROM matches WHERE id <= p_limit; "
          "DECLARE CONTINUE HANDLER FOR NOT FOUND SET done=1; "
          "OPEN ROWS_FOUND; read_rows: LOOP FETCH NEXT FROM rows_found INTO v_id, v_amount; "
          "IF done THEN LEAVE read_rows; END IF; SET total=v_amount; END LOOP; "
          "CLOSE rows_found; END//\nDELIMITER ;";
      const auto routine = compileRoutine(my, source);
      require(routine.ok() && routine.runtimeDiagnostics.empty() && routine.ast.cursors.size() == 2,
              routine.diagnostics.empty() ? "valid cursor failed checking" : routine.diagnostics.front().message.c_str());
      bool declaration = false, fetch = false, local = false;
      for (const auto &node : routine.model.nodes) {
        if (node.syntax.kind == ProceduralKind::CursorFetch) {
          fetch = node.syntax.cursor->cursor == 1 && node.syntax.cursor->targets.size() == 2 &&
                  routine.ast.symbols.at(node.syntax.cursor->targets[0]).name == "v_id" &&
                  routine.ast.symbols.at(node.syntax.cursor->targets[1]).name == "v_amount";
        }
        if (node.syntax.kind == ProceduralKind::CursorDeclare) {
          declaration = node.cursorArity == 2 && !node.code.empty();
          for (const auto &query : node.code.queries)
            for (const auto &expr : query.expressions)
              local |= expr.localSymbol == routine.ast.parameters[0];
        }
      }
      require(declaration && fetch && local, "cursor query or FETCH symbol binding lost in the IR");
      const auto rendered = good(my, my, source);
      require(rendered.modelJson.find("\"operation\":\"fetch\"") != std::string::npos &&
              rendered.modelJson.find("\"columnCount\":2") != std::string::npos &&
              rendered.sql.find("READS SQL DATA") != std::string::npos &&
              checkSqlScript(my, rendered.sql).ok(), "cursor round trip lost typed shape or SQL data access");
      bad(my, pg, source);

      const auto scoped = compileRoutine(my, "CREATE PROCEDURE p() BEGIN DECLARE v INT; "
          "DECLARE row_cursor CURSOR FOR SELECT 1; OPEN row_cursor; "
          "BEGIN DECLARE ROW_CURSOR CURSOR FOR SELECT 2; OPEN row_cursor; "
          "FETCH FROM row_cursor INTO v; CLOSE row_cursor; END; "
          "FETCH row_cursor INTO v; CLOSE row_cursor; END;");
      std::vector<ProceduralCursorId> bound;
      for (const auto &node : scoped.model.nodes)
        if (node.syntax.kind == ProceduralKind::CursorFetch) bound.push_back(node.syntax.cursor->cursor);
      require(scoped.ok() && scoped.runtimeDiagnostics.empty() &&
              bound == std::vector<ProceduralCursorId>{2, 1}, "nested cursor scope or cursor name folding failed");

      const auto broken = compileRoutine(my, "CREATE PROCEDURE p() BEGIN DECLARE v INT; "
          "DECLARE row_cursor CURSOR FOR SELECT 1; OPEN missing; FETCH row_cursor INTO missing_var; END;");
      require(!broken.ok() && broken.diagnostics.size() == 2 &&
              broken.diagnostics[0].message.find("undeclared MySQL cursor") != std::string::npos &&
              broken.diagnostics[1].message.find("undeclared procedural variable") != std::string::npos,
              "cursor references and FETCH target bindings did not report independently");
      const auto wrongOrder = compileRoutine(my, "CREATE PROCEDURE p() BEGIN "
          "DECLARE row_cursor CURSOR FOR SELECT 1; DECLARE v INT; END;");
      require(!wrongOrder.ok() && wrongOrder.diagnostics.front().code == DiagnosticCode::InvalidQueryShape,
              "variables declared after a cursor were accepted");
      const auto noVisibleCursor = compileRoutine(my, "CREATE PROCEDURE p() BEGIN "
          "BEGIN DECLARE row_cursor CURSOR FOR SELECT 1; END; OPEN row_cursor; END;");
      require(!noVisibleCursor.ok() &&
              noVisibleCursor.diagnostics.front().message.find("undeclared MySQL cursor") != std::string::npos,
              "cursor escaped its declaration block");

      const std::string wildcard = "CREATE PROCEDURE p() BEGIN DECLARE v INT; "
          "DECLARE c CURSOR FOR SELECT * FROM t; OPEN c; FETCH c INTO v; CLOSE c; END;";
      MemoryCatalog catalog;
      catalog.tables.push_back({"", "", "t", {{"a", ValueType::Integer}, {"b", ValueType::Integer}}});
      auto offline = compileRoutine(my, wildcard);
      auto online = compileRoutine(my, wildcard, {}, &catalog);
      require(offline.ok() && offline.runtimeDiagnostics.empty() && online.ok() &&
              online.runtimeDiagnostics.size() == 1 &&
              online.runtimeDiagnostics[0].message.find("expects 2 target") != std::string::npos,
              "cursor wildcard row width requires reliable catalog information");
      const auto unknownSet = compileRoutine(my, "CREATE PROCEDURE p() BEGIN DECLARE v INT; "
          "DECLARE c CURSOR FOR SELECT * FROM t UNION ALL SELECT 1; OPEN c; FETCH c INTO v; END;");
      require(unknownSet.ok() && unknownSet.runtimeDiagnostics.empty(),
              "an unresolved set operand must leave cursor row width unknown");
      const auto badRuntime = checkSqlScript(my, "SELECT 1;\nDELIMITER //\nCREATE PROCEDURE p() BEGIN "
          "DECLARE v INT; DECLARE c CURSOR FOR SELECT 1; FETCH c INTO v; "
          "OPEN c; OPEN c; CLOSE c; CLOSE c; END//\nDELIMITER ;");
      require(badRuntime.ok() && badRuntime.runtimeDiagnostics.size() == 3 &&
              badRuntime.runtimeDiagnostics.front().range.begin > std::string("SELECT 1;").size(),
              "definite cursor runtime failures became definition errors or lost source offsets");
      const auto conditional = compileRoutine(my, "CREATE PROCEDURE p(IN x INT) BEGIN DECLARE v INT; "
          "DECLARE c CURSOR FOR SELECT 1; IF x=1 THEN OPEN c; END IF; FETCH c INTO v; END;");
      require(conditional.ok() && conditional.runtimeDiagnostics.empty(),
              "uncertain cursor state after IF was treated as definite");
      const auto preparedArity=compileRoutine(my,"CREATE PROCEDURE p() BEGIN "
          "PREPARE known_handle FROM 'SELECT ?,?'; EXECUTE known_handle USING @first; END;");
      require(preparedArity.ok() && preparedArity.runtimeDiagnostics.size()==1 &&
              preparedArity.runtimeDiagnostics.front().code==DiagnosticCode::PreparedRuntime &&
              preparedArity.runtimeDiagnostics.front().message.find("argument count")!=std::string::npos,
              "stored procedure did not check a definitely known PREPARE parameter count");
      const auto releasedHandle=compileRoutine(my,"CREATE PROCEDURE p() BEGIN PREPARE h FROM 'SELECT 1'; "
          "DEALLOCATE PREPARE h; EXECUTE h; END;");
      require(releasedHandle.ok() && releasedHandle.runtimeDiagnostics.size()==1 &&
              releasedHandle.runtimeDiagnostics.front().message.find("deallocated")!=std::string::npos,
              "definitely deallocated handle was not diagnosed");
      const auto uncertainHandle=compileRoutine(my,"CREATE PROCEDURE p(IN choose INT) BEGIN "
          "IF choose=1 THEN PREPARE h FROM 'SELECT ?'; ELSE PREPARE h FROM 'SELECT 2'; END IF; EXECUTE h; END;");
      require(uncertainHandle.ok() && uncertainHandle.runtimeDiagnostics.empty(),
              "one conditional PREPARE definition was treated as definite");
      const auto optionalBlockExit = compileRoutine(my,
          "CREATE PROCEDURE p(IN x INT) BEGIN DECLARE v INT; DECLARE c CURSOR FOR SELECT 1; "
          "scope_block: BEGIN IF x=1 THEN LEAVE scope_block; END IF; OPEN c; END scope_block; "
          "FETCH c INTO v; END;");
      require(optionalBlockExit.ok() && optionalBlockExit.runtimeDiagnostics.empty(),
              "uncertain cursor state after LEAVE became a definite diagnostic");
      const auto forcedBlockExit = compileRoutine(my,
          "CREATE PROCEDURE p() BEGIN DECLARE v INT; DECLARE c CURSOR FOR SELECT 1; OPEN c; "
          "scope_block: BEGIN CLOSE c; LEAVE scope_block; OPEN c; END scope_block; "
          "FETCH c INTO v; END;");
      require(forcedBlockExit.ok() && forcedBlockExit.runtimeDiagnostics.size()==1 &&
              forcedBlockExit.runtimeDiagnostics[0].message.find("not open")!=std::string::npos,
              "block exit did not reach the following cursor read with the closed state");
      const auto continued = compileRoutine(my, "CREATE PROCEDURE p() BEGIN DECLARE v INT; "
          "DECLARE c CURSOR FOR SELECT 1; DECLARE CONTINUE HANDLER FOR SQLEXCEPTION SET v=0; "
          "BEGIN OPEN c; END; OPEN c; END;");
      require(continued.ok() && continued.runtimeDiagnostics.empty(),
              "a continue handler may resume after a failed OPEN");
      const auto exited = compileRoutine(my, "CREATE PROCEDURE p() BEGIN DECLARE v INT; "
          "DECLARE c CURSOR FOR SELECT 1; OPEN c; "
          "BEGIN DECLARE EXIT HANDLER FOR SQLEXCEPTION SET v=0; SET v=1; CLOSE c; END; "
          "FETCH c INTO v; END;");
      require(exited.ok() && exited.runtimeDiagnostics.empty(),
              "an exit handler may skip a cursor operation in its block");
      for (const auto *query : {"TABLE t", "VALUES ROW(1,2)", "(SELECT 1,2)"}) {
        const auto shorthand = compileRoutine(my, std::string("CREATE PROCEDURE p() BEGIN "
            "DECLARE v,w INT; DECLARE c CURSOR FOR ") + query +
            "; OPEN c; FETCH c INTO v,w; END;", {}, &catalog);
        require(shorthand.ok() && shorthand.runtimeDiagnostics.empty(),
                "MySQL cursor rejected a valid query expression");
      }
      const auto badDirection = compileRoutine(my, "CREATE PROCEDURE p() BEGIN DECLARE v INT; "
          "DECLARE next CURSOR FOR SELECT 1; OPEN next; FETCH NEXT INTO v; END;");
      require(!badDirection.ok() && badDirection.diagnostics.front().code == DiagnosticCode::ExpectedToken,
              "FETCH NEXT must be followed by FROM");
      const auto quoted = compileRoutine(my, "CREATE PROCEDURE p() BEGIN DECLARE v INT; "
          "DECLARE `next` CURSOR FOR SELECT 1; OPEN `next`; FETCH `next` INTO v; END;");
      require(quoted.ok() && quoted.runtimeDiagnostics.empty(), "quoted NEXT cursor was parsed as a direction");
    }
    const std::string delimitedFunction =
        "DELIMITER $$\nCREATE FUNCTION f_get_quantity(_id INT) RETURNS INT READS SQL DATA "
        "BEGIN IF _id > 0 THEN\n  _id\n  RETURN (SELECT SUM(id) FROM t_activity WHERE id = _id); "
        "ELSE RETURN (SELECT SUM(id) FROM t_activity); END IF; END$$";
    const auto badStatement = checkSqlScript(my, delimitedFunction);
    require(!badStatement.ok() && !badStatement.diagnostics.empty(),
            "a bare MySQL variable was accepted as a statement");
    const auto bare = badStatement.diagnostics.front().range;
    require(bare.begin == delimitedFunction.find("\n  _id") + 3 &&
            delimitedFunction.substr(bare.begin, bare.end - bare.begin) == "_id",
            "delimiter-wrapped routine syntax error was not located on the stray variable");
    const std::string returnsSubquery =
        "DELIMITER $$\nCREATE FUNCTION f_get_quantity(_id INT) RETURNS INT READS SQL DATA "
        "BEGIN IF _id > 0 THEN RETURN (SELECT SUM(id) FROM t_activity WHERE id = _id); "
        "ELSE RETURN (SELECT SUM(id) FROM t_activity); END IF; "
        "IF _id = 0 THEN RETURN 1; END IF; END$$";
    for (auto dialect : {my, Dialect::MariaDB, Dialect::OceanBase}) {
      require(checkSqlScript(dialect, returnsSubquery).ok(),
              "offline subquery columns were treated as undeclared procedural variables");
      MemoryCatalog catalog;
      TableDefinition table;
      table.name = "t_activity";
      table.columns = {{"id", ValueType::Integer}};
      catalog.tables.push_back(table);
      require(checkSqlScript(dialect, returnsSubquery, {}, &catalog).ok(),
              "catalog-backed RETURN subquery failed to bind its column and parameter");
      catalog.tables.front().columns = {{"other_column", ValueType::Integer}};
      const auto missingColumn = checkSqlScript(dialect, returnsSubquery, {}, &catalog);
      require(!missingColumn.ok() && !missingColumn.diagnostics.empty(),
              "a missing catalog-backed subquery column was accepted");
      for (const auto &error : missingColumn.diagnostics) {
        require(error.code == DiagnosticCode::UnknownColumn &&
                    error.message.find("column not found") != std::string::npos &&
                    returnsSubquery.substr(error.range.begin, error.range.end - error.range.begin) == "id",
                "subquery column error was reported as a procedural variable");
      }
    }
    auto invalidScope=checkSqlScript(my,"CREATE FUNCTION f(p INTEGER) RETURNS INTEGER DETERMINISTIC RETURN not_declared;");require(!invalidScope.ok(),"undeclared variable not diagnosed by script checker");
    for (auto dialect : {my, pg, ora, ms}) {
      const std::string body = dialect == my
          ? "CREATE PROCEDURE p() BEGIN SET missing_one=missing_rhs; SET missing_two=2; END;"
          : dialect == pg
              ? "CREATE PROCEDURE p() LANGUAGE plpgsql AS $$ BEGIN missing_one:=missing_rhs; missing_two:=2; END; $$;"
              : dialect == ora
                  ? "CREATE PROCEDURE p AS BEGIN missing_one:=missing_rhs; missing_two:=2; END;"
                  : "CREATE PROCEDURE p AS BEGIN SET @missing_one=@missing_rhs; SET @missing_two=2; END;";
      const auto verify = [&](const auto &diagnostics, const auto &sql, std::size_t count) {
        require(diagnostics.size() == count, "routine semantic diagnostics were lost or duplicated");
        const std::vector<std::string> expected{"missing_one", "missing_rhs", "missing_two"};
        for (std::size_t i = 0; i < std::min(count, expected.size()); ++i) {
          const auto &error = diagnostics[i];
          const auto span = sql.substr(error.range.begin, error.range.end - error.range.begin);
          require(error.code == DiagnosticCode::UnknownColumn &&
                  span == (dialect == ms ? "@" + expected[i] : expected[i]),
                  "routine diagnostic range or source order is wrong");
        }
      };
      const auto routine = compileRoutine(dialect, body);
      require(!routine.ok() && routine.ast.origin.end == body.size(),
              "semantic errors discarded the routine's structural boundary");
      verify(routine.diagnostics, body, 3);
      LanguageOptions limited;
      limited.limits.diagnostics = 2;
      verify(compileRoutine(dialect, body, limited).diagnostics, body, 2);
      const std::string script = body +
          (dialect == ora ? "\n/\nSELECT 1 + FROM dual;" : "\nSELECT 1 + FROM t;");
      const auto checked = checkSqlScript(dialect, script);
      require(checked.complete && checked.routines == 1 && checked.statements == 2,
              "invalid routine prevented checking the following statement");
      verify(checked.diagnostics, script, 4);
      require(checked.diagnostics.back().range.begin >= body.size(),
              "following statement diagnostic did not map to its source location");
    }
    for(auto dialect:{my,pg,ora,ms}) {
      MemoryCatalog catalog;TableDefinition table;table.database="owned";table.schema=dialect==ora?"OWNED":dialect==my?"owned":"public";table.name=dialect==ora?"T":"t";table.columns={{dialect==ora?"V":"v",ValueType::Integer},{dialect==ora?"ID":"id",ValueType::Integer}};catalog.tables.push_back(table);
      NameResolutionContext names;names.database="owned";names.schema=table.schema;names.dialect=dialect;
      const std::string sql=dialect==my?"CREATE PROCEDURE p(IN v INTEGER) SQL SECURITY INVOKER BEGIN UPDATE t SET v=v+1 WHERE id=1; END;":
          dialect==pg?"CREATE PROCEDURE p(v INTEGER) LANGUAGE plpgsql AS $$ BEGIN UPDATE t SET v=v+1 WHERE id=1; END; $$;":
          dialect==ora?"CREATE PROCEDURE p(v NUMBER) AS BEGIN UPDATE t SET v=v+1 WHERE id=1; END;":
          "CREATE PROCEDURE p @v INTEGER AS BEGIN UPDATE t SET v=@v+1 WHERE id=1; END;";
      auto routine=compileRoutine(dialect,sql,{},&catalog,names);
      if(dialect==pg)require(!routine.ok() && std::any_of(routine.diagnostics.begin(),routine.diagnostics.end(),[](const auto &d){return d.code==DiagnosticCode::AmbiguousColumn;}),"PL/pgSQL local/column conflict not diagnosed");
      else {
        require(routine.ok(),routine.diagnostics.empty()?"no routine":routine.diagnostics.front().message.c_str());bool inspected=false;
        for(const auto &node:routine.model.nodes)for(const auto &q:node.code.queries)if(q.kind==StatementKind::Update){inspected=true;const auto &assignment=q.assignments.at(0);const auto &lhs=q.expressions.at(assignment.targets.at(0));require(lhs.binding.has_value()&&!lhs.localSymbol,"assignment target bound to a local variable");bool local=false;for(const auto &e:q.expressions)local|=e.localSymbol!=0;require(local==(dialect!=ora),"dialect-specific column/local precedence ignored");}
        require(inspected,"routine SQL not inspected");
      }
    }
    good(pg,sq,"SELECT COUNT(*) FILTER(WHERE id>0) FROM t;");
    auto ordinalGrouping=good(pg,ora,"SELECT id,SUM(n) FROM t GROUP BY 1;");
    require(ordinalGrouping.sql.find("GROUP BY \"id\"")!=std::string::npos,"GROUP BY position was not expanded for Oracle");
    bad(pg,ora,"SELECT * FROM t GROUP BY 1;");
    bad(pg,my,"SELECT COUNT(*) FILTER(WHERE id>0) FROM t;");
    for(const auto target:{my,ms}) {
      const auto count=good(pg,target,
          "SELECT COUNT(*) FILTER (WHERE id IS NOT NULL), COUNT(x) FILTER (WHERE id IS NULL) FROM t;");
      const auto function=target==ms?"COUNT_BIG(CASE WHEN":"COUNT(CASE WHEN";
      require(count.sql.find(function)!=std::string::npos &&
              count.sql.find("ELSE NULL END")!=std::string::npos &&
              count.changes.size()==2 &&
              count.changes.front().kind==ConversionChangeKind::AggregateFilter,
              "portable COUNT FILTER must retain NULL behavior and report the target aggregate");
      bad(pg,target,"SELECT COUNT(DISTINCT x) FILTER(WHERE id IS NULL) FROM t;");
      bad(pg,target,"SELECT COUNT(*) FILTER (WHERE id IS NULL) OVER () FROM t;");
      bad(pg,target,"SELECT COUNT(*) FILTER (WHERE t IS NULL) FROM t;");
      bad(pg,target,"SELECT MIN(x) FILTER (WHERE id IS NULL) FROM t;");
    }
    good(pg,ora,"SELECT 1 AS n UNION ALL SELECT 2;");
    good(ora,pg,"SELECT 1 AS n FROM SYS.DUAL UNION SELECT 2 FROM SYS.DUAL INTERSECT SELECT 2 FROM SYS.DUAL;");
    good(my,pg,"WITH q AS(SELECT 1 AS id) SELECT id FROM q UNION ALL SELECT id+1 FROM q;");
    bad(pg,ms,"SELECT 1 INTERSECT ALL SELECT 1;");
    {
      MemoryCatalog catalog; TableDefinition table; table.name="t";
      table.columns={{"x",ValueType::Integer}}; catalog.tables.push_back(table);
      ConversionOptions context; context.catalog=&catalog;
      auto grouping=good(my,ora,"SELECT x+1 AS g FROM t GROUP BY g;",context);
      auto groupedModel=compile(ora,grouping.sql); require(groupedModel->ok() && groupedModel->model[0].expressions.at(groupedModel->model[0].groupBy[0]).kind==ExprKind::Binary,"GROUP alias not expanded for Oracle");
      auto having=good(my,pg,"SELECT SUM(x) AS total FROM t HAVING total>1;",context);
      auto havingModel=compile(pg,having.sql); require(havingModel->ok(),"expanded HAVING target invalid"); const auto &hq=havingModel->model[0]; require(hq.expressions.at(hq.expressions.at(hq.having).operands[0]).kind==ExprKind::Call,"HAVING alias not expanded for PostgreSQL");
      bad(my,ora,"SELECT x+1 AS g FROM t GROUP BY g;");
    }
    ConversionOptions ddl;
    ddl.allowDdlTransactionChange = true;
    {
      MemoryCatalog catalog;ConversionOptions context=ddl;context.catalog=&catalog;
      good(my,pg,"CREATE TABLE script_owned(id INTEGER);INSERT INTO script_owned(id) VALUES(1);SELECT id FROM script_owned;",context);
      bad(my,pg,"CREATE TABLE script_owned(id INTEGER);SELECT missing FROM script_owned;",context);
      auto script=checkSqlScript(my,"CREATE TABLE script_owned(id INTEGER);SELECT missing FROM script_owned;",{},&catalog);require(!script.ok(),"script declaration did not bind the following SELECT");
      script=checkSqlScript(my,"CREATE FUNCTION script_f(p INTEGER) RETURNS INTEGER DETERMINISTIC RETURN p+1;SELECT script_f();",{},&catalog);require(!script.ok(),"new routine signature was not checked in the next statement");
      script=checkSqlScript(my,"CREATE FUNCTION Script_F(p INTEGER) RETURNS INTEGER DETERMINISTIC RETURN p+1;SELECT script_f();",{},&catalog);require(!script.ok(),"MySQL routine names were incorrectly case-sensitive");
    }
    for (auto sql :
         {"CREATE TABLE t(id INTEGER PRIMARY KEY,n DECIMAL(10,2) DEFAULT 0);",
          "ALTER TABLE t ADD n INTEGER;", "CREATE INDEX ix ON t(id);",
          "CREATE VIEW v AS SELECT id FROM t;", "DROP VIEW v;",
          "DROP TABLE t;"})
      good(my, pg, sql, ddl);
    bad(my, pg, "CREATE TABLE t(id INTEGER);");
    good(pg, ora, "CREATE MATERIALIZED VIEW mv AS SELECT id FROM t;", ddl);
    good(ora, pg, "CREATE MATERIALIZED VIEW mv AS SELECT id FROM t;", ddl);
    bad(pg, my, "CREATE MATERIALIZED VIEW mv AS SELECT id FROM t;", ddl);
    bad(pg, ora,
        "CREATE MATERIALIZED VIEW mv AS SELECT id FROM t WITH NO DATA;", ddl);
    bad(my, pg, "SELECT 1; SELECT NOW();");
    bad(my, pg, "SELECT 4/2;");
    good(pg,pg,"MERGE INTO t USING s ON t.id=s.id WHEN MATCHED THEN UPDATE SET n=s.n WHEN NOT MATCHED THEN INSERT(id,n) VALUES(s.id,s.n);");
    bad(pg,ms,"MERGE INTO t USING s ON t.id=s.id WHEN MATCHED THEN UPDATE SET n=s.n;");
    bad(ms,pg,"SELECT 'a'+'b';");
    bad(my,pg,"SELECT '2'+1;");
    bad(my,pg,"SELECT 1='1';");
    bad(my,pg,"SELECT COALESCE(1,'a');");
    bad(my,pg,"CREATE FUNCTION f() RETURNS INTEGER DETERMINISTIC SQL SECURITY INVOKER BEGIN IF 1 THEN RETURN 1; END IF; RETURN 0; END;");
    bad(my,pg,"CREATE FUNCTION f() RETURNS INTEGER DETERMINISTIC SQL SECURITY INVOKER RETURN 'a';");
    good(my,pg,"CREATE FUNCTION choose_value(p INTEGER) RETURNS INTEGER DETERMINISTIC SQL SECURITY INVOKER BEGIN IF p>1 THEN RETURN 2; ELSEIF p=1 THEN RETURN 1; ELSE RETURN 0; END IF; END;");
    good(pg,my,"CREATE FUNCTION choose_value(p INTEGER) RETURNS INTEGER LANGUAGE plpgsql IMMUTABLE AS $$ BEGIN IF p>1 THEN RETURN 2; ELSIF p=1 THEN RETURN 1; ELSE RETURN 0; END IF; END; $$;");
    good(ora,ora,"CREATE FUNCTION choose_value(p INTEGER) RETURN INTEGER AS BEGIN IF p>1 THEN RETURN 2; ELSIF p=1 THEN RETURN 1; ELSE RETURN 0; END IF; END;");
    ConversionOptions shallow;shallow.generation.maxNesting=1;bad(my,pg,"CREATE FUNCTION f(p INTEGER) RETURNS INTEGER DETERMINISTIC BEGIN IF p>1 THEN RETURN 2; END IF; RETURN 0; END;",shallow);
    good(ms,ms,"CREATE FUNCTION f(@p INTEGER) RETURNS INTEGER AS BEGIN IF @p>0 BEGIN RETURN 1; END; RETURN 0; END;");
    bad(ms,ms,"CREATE FUNCTION f(@p BIT) RETURNS INTEGER AS BEGIN IF @p BEGIN RETURN 1; END; RETURN 0; END;");
    good(ora,ora,"CREATE FUNCTION f(p BOOLEAN) RETURN BOOLEAN AS v BOOLEAN:=p; BEGIN IF v THEN RETURN TRUE; ELSE RETURN FALSE; END IF; END;");
    bad(pg,ms,"SELECT 1=1;");
    std::string expanded="CREATE FUNCTION budget_owned(p INTEGER) RETURNS INTEGER DETERMINISTIC BEGIN ";
    for(int i=0;i<500;++i)expanded+="DECLARE v"+std::to_string(i)+" INTEGER DEFAULT p;";
    for(int i=0;i<500;++i)expanded+="SET v0=v0+1;";
    expanded+="RETURN v0;END;";
    auto tinyReport=compileRoutine(my,"CREATE FUNCTION owned_report(p INTEGER) RETURNS INTEGER DETERMINISTIC RETURN p+1;");
    require(tinyReport.ok(),"report fixture did not compile");bool reportBounded=false;
    try{dumpRoutineModel(tinyReport,32);}catch(const std::length_error &){reportBounded=true;}
    require(reportBounded,"routine report ignored its byte budget");
    auto boundedRoutine=compileRoutine(my,expanded);
    require(!boundedRoutine.ok() && std::any_of(boundedRoutine.diagnostics.begin(),boundedRoutine.diagnostics.end(),[](const auto &d){return d.code==DiagnosticCode::ResourceLimit;}),"cumulative routine context expansion was not bounded");
    auto pgCondition=compileRoutine(pg,"CREATE FUNCTION f() RETURNS INTEGER LANGUAGE plpgsql AS $$ BEGIN IF 1 THEN RETURN 1; END IF; RETURN 0; END; $$;");require(!pgCondition.ok(),"numeric PL/pgSQL IF condition accepted");
    bad(my,pg,"SELECT CASE WHEN 1=1 THEN 1 ELSE 'a' END;");
    bad(my,pg,"SELECT 1 WHERE 'yes';");
    bad(sq,pg,"SELECT 1 WHERE 'true';");
    good(my,pg,"SELECT COALESCE('a','b');");
    bad(pg, my, "INSERT INTO t VALUES(1) ON CONFLICT DO NOTHING;");
    bad(my, pg, "UPDATE t SET n=1 LIMIT 1;");
    bad(my, pg, "CREATE TABLE t(id INTEGER AUTO_INCREMENT PRIMARY KEY);", ddl);
    ConversionOptions mapping;
    mapping.qualifiers.push_back({{"old_db"}, {"new_schema"}});
    auto mapped = good(my, pg, "SELECT t.id FROM old_db.t t;", mapping);
    require(mapped.sql.find("new_schema") != std::string::npos,
            "namespace mapping lost");
    auto same = good(my, my, "SELECT id FROM old_db.t;", mapping);
    require(same.sql.find("new_schema") != std::string::npos,
            "same dialect mapping lost");
    std::string fn =
        "CREATE FUNCTION add_one(p INTEGER) RETURNS INTEGER DETERMINISTIC SQL "
        "SECURITY INVOKER BEGIN DECLARE v INTEGER DEFAULT p+1; RETURN v; END;";
    auto converted = good(my, pg, fn);
    require(converted.modelJson.find("boundSymbols") != std::string::npos,
            "routine context absent");
    require(converted.sql.find("\"p\"") != std::string::npos,
            "parameter name changed");
    good(my, pg,
         "CREATE FUNCTION simple_f(p INTEGER) RETURNS INTEGER SQL SECURITY "
         "INVOKER RETURN p+1;");
    auto oracleScope =
        compileRoutine(ora, "CREATE FUNCTION f(p NUMBER) RETURN NUMBER AS v "
                            "NUMBER:=p; BEGIN RETURN v+1; END;");
    require(oracleScope.ok(), "Oracle routine scope not parsed");
    bad(my, ora, "SELECT '' FROM dual;");
    bad(pg, ms, "SELECT CHAR_LENGTH('a');");
    bad(my,pg,"CREATE PROCEDURE p() SQL SECURITY INVOKER SELECT 1;");
    auto returnTrip = good(pg, my, converted.sql);
    require(returnTrip.sql.find("DELIMITER") != std::string::npos,
            "client delimiter absent");
    good(my, pg, returnTrip.sql);
    good(my, pg,
         "CREATE PROCEDURE proc(IN p INTEGER) SQL SECURITY INVOKER BEGIN "
         "DECLARE v INTEGER DEFAULT p; IF v>1 THEN SET v=v+1; ELSE SET v=0; "
         "END IF; WHILE v<5 DO SET v=v+1; END WHILE; INSERT INTO audit_log(n) "
         "VALUES(v); END;");
    good(ms, pg,
         "CREATE FUNCTION f(@p INTEGER) RETURNS INTEGER AS BEGIN DECLARE @v "
         "INTEGER=@p+1; RETURN @v; END;\nGO\n");
    auto nested = compileRoutine(
        pg, "CREATE FUNCTION f(p INTEGER) RETURNS INTEGER LANGUAGE plpgsql AS "
            "$$ DECLARE v INTEGER:=p; BEGIN DECLARE v INTEGER:=2; BEGIN "
            "v:=v+1; END; RETURN v; END; $$;");
    require(nested.ok(), nested.diagnostics.empty()
                             ? "no nested context"
                             : nested.diagnostics.front().message.c_str());
    std::vector<ProceduralScopeId> scopes;
    for (const auto &symbol : nested.ast.symbols)
      if (symbol.kind == SymbolKind::Variable && symbol.name == "v" && symbol.id) scopes.push_back(symbol.scope);
    require(scopes.size() == 2 && scopes[0] != scopes[1], "shadowed variables not distinct");
    {
      const std::string source = "CREATE FUNCTION f(p BIGINT) RETURNS BIGINT LANGUAGE plpgsql AS $$ "
          "<<root_block>> DECLARE v BIGINT := 1; BEGIN DECLARE v INTEGER := root_block.v; "
          "BEGIN root_block.v := $1; END; PERFORM root_block.v; RETURN $1; END root_block; $$;";
      const auto routine = compileRoutine(pg,source);
      require(routine.ok(),routine.diagnostics.empty()?"routine scope regression":routine.diagnostics.front().message.c_str());
      const auto parameter=routine.ast.parameters.front();
      bool found=false, foreignAssignment=false;
      for (const auto &node:routine.model.nodes) {
        if (node.syntax.kind==ProceduralKind::Assign)
          foreignAssignment=routine.ast.symbols.at(node.syntax.target).scope!=node.syntax.scope;
        for (const auto &query:node.code.queries)
          for (const auto &value:query.expressions)
            if (value.kind==ExprKind::Parameter && value.value=="$1") {
              found=true;
              require(value.localSymbol==parameter && node.code.types.get(value.sqlType).bits==64,
                      "PL/pgSQL positional parameter did not preserve its declared type");
            }
      }
      require(found && foreignAssignment,"nested qualified assignment or positional references were lost");
      const auto native=good(pg,pg,source);
      require(native.sql.find("PERFORM ")!=std::string::npos && compileRoutine(pg,native.sql).ok(),
              "PERFORM lost native round-trip semantics");
      const std::string missing = "CREATE PROCEDURE p(f INT) LANGUAGE plpgsql AS $$ BEGIN f := $2; END; $$;";
      const auto missingParam=compileRoutine(pg,missing);
      require(!missingParam.ok() && missingParam.diagnostics.front().range.begin==missing.find("$2") &&
              missingParam.diagnostics.front().range.end==missing.find("$2")+2,
              "out-of-scope positional reference did not point at the parameter");
      const std::string unknownTarget="CREATE PROCEDURE p AS BEGIN missing := 1; END;";
      const auto undeclared=compileRoutine(ora,unknownTarget);
      require(!undeclared.ok() && undeclared.diagnostics.front().range.begin==unknownTarget.find("missing"),
              "assignment error underlined the assignment operator instead of the target");
      require(!compileRoutine(ora,"CREATE PROCEDURE p(input_value IN NUMBER) AS BEGIN input_value := 1; END;").ok(),
              "read-only Oracle IN parameter accepted as an assignment target");
      require(compileRoutine(ora,"CREATE PROCEDURE p(input_value IN OUT NUMBER) AS BEGIN input_value := 1; END;").ok(),
              "Oracle IN OUT parameter refused as an assignment target");
      require(!compileRoutine(pg,"CREATE PROCEDURE p() LANGUAGE plpgsql AS $$ BEGIN SELECT 1; END; $$;").ok(),
              "bare row-producing SELECT accepted as a PL/pgSQL statement");
    }
    bad(my, pg,
        "CREATE FUNCTION f(p INTEGER) RETURNS INTEGER BEGIN RETURN missing+1; "
        "END;");
    bad(my, pg,
        "CREATE FUNCTION f() RETURNS INTEGER BEGIN DECLARE v INTEGER; DECLARE "
        "v INTEGER; RETURN 0; END;");
    bad(my, pg,
        "CREATE PROCEDURE p() BEGIN DECLARE CONTINUE HANDLER FOR SQLEXCEPTION "
        "SET x=1; END;");
    ConversionOptions cancelled;
    cancelled.language.shouldStop = [] { return true; };
    bad(my, pg, fn, cancelled);
    ConversionOptions limited;
    limited.generation.maxBytes = 20;
    bad(my, pg, "SELECT 1; SELECT 2;", limited);
    auto async =
        std::async(std::launch::async, [&] { return good(my, pg, fn).sql; });
    require(async.get() == converted.sql,
            "non-deterministic/thread-unsafe conversion");
    std::cout << "PASS conversion: DML, DDL, views, materialized views, "
                 "routine scopes, atomicity, resource limits\n";
    return 0;
  } catch (const std::exception &e) {
    std::cerr << "FAIL conversion: " << e.what() << '\n';
    return 1;
  }
}
