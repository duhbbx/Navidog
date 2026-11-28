#include "navidog/sql/compiler/compiler.hpp"
#include "navidog/sql/compiler/formatter.hpp"
#include "navidog/sql/compiler/outline.hpp"
#include "navidog/sql/compiler/operator_catalog.hpp"
#include <algorithm>
#include <functional>
#include <future>
#include <iostream>
#include <random>
#include <stdexcept>

using namespace navidog::sql::compiler;
namespace {
void check(bool value, const char *expression, int line) {
  if (!value)
    throw std::runtime_error(std::string("line ") + std::to_string(line) +
                             ": " + expression);
}
#define CHECK(expression) check(bool(expression), #expression, __LINE__)
void success(const Compilation &c) {
  if (!c.ok())
    throw std::runtime_error(
        c.diagnostics.empty() ? "not valid" : c.diagnostics.front().message);
}
bool has(const Compilation &c, DiagnosticCode code) {
  for (const auto &d : c.diagnostics)
    if (d.code == code)
      return true;
  return false;
}
const ModelExpression &projection(const Compilation &c, std::size_t index = 0) {
  return c.model.at(0).expressions.at(
      c.model.at(0).projections.at(index).expression);
}
void sqlServerDdlBoundaries() {
  for (const auto *sql : {"CREATE SEQUENCE dbo.audit_seq CACHE;",
                          "CREATE SEQUENCE dbo.audit_seq NO CACHE;",
                          "ALTER SEQUENCE dbo.audit_seq NO CACHE;",
                          "CREATE VIEW dbo.audit_v AS SELECT TOP(17) id FROM audit_items ORDER BY id;",
                          "CREATE VIEW audit_v AS SELECT id FROM audit_items ORDER BY id OFFSET 0 ROWS;",
                          "CREATE INDEX audit_idx ON audit_items(id DESC);"}) {
    const auto parsed=compile(Dialect::SQLServer,sql);
    success(*parsed);
    const auto generated=generateSql(parsed->model,Dialect::SQLServer);
    CHECK(generated.ok());
    success(*compile(Dialect::SQLServer,generated.sql));
  }
  const auto noSize=compile(Dialect::SQLServer,"CREATE SEQUENCE audit_seq CACHE;");
  success(*noSize);
  CHECK(noSize->model.front().ddl->sequenceOptions.front().second==0);
  CHECK(!generateSql(noSize->model,Dialect::PostgreSQL).ok());
  const auto functionKey=compile(Dialect::PostgreSQL,"CREATE INDEX audit_idx ON audit_items(ABS(id));");
  success(*functionKey);
  const auto rejectedIndex=generateSql(functionKey->model,Dialect::SQLServer);
  CHECK(!rejectedIndex.ok() && rejectedIndex.sql.empty());
  const auto orderedView=compile(Dialect::PostgreSQL,"CREATE VIEW audit_v AS SELECT id FROM audit_items ORDER BY id;");
  success(*orderedView);
  const auto rejectedView=generateSql(orderedView->model,Dialect::SQLServer);
  CHECK(!rejectedView.ok() && rejectedView.sql.empty());
}
void sqlServerFilteredIndexes() {
  const auto parsed=compile(Dialect::SQLServer,
      "CREATE INDEX audit_idx ON audit_items(id) WHERE id>CAST(17 AS INT) AND label IS NOT NULL;");
  success(*parsed);
  const auto &query=parsed->model.front();
  const auto root=query.ddl->predicate;
  CHECK(query.expressions.at(root).kind==ExprKind::Binary);
  CHECK(query.expressions.at(root).value=="AND");
  const auto comparison=query.expressions.at(root).operands[0];
  CHECK(query.expressions.at(query.expressions.at(comparison).operands[1]).kind==ExprKind::Cast);
  const auto generated=generateSql(parsed->model,Dialect::SQLServer);
  CHECK(generated.ok());
  success(*compile(Dialect::SQLServer,generated.sql));
  auto changed=parsed->model;
  changed.queries.front().expressions.at(root).value="OR";
  const auto rejected=generateSql(changed,Dialect::SQLServer);
  CHECK(!rejected.ok() && rejected.sql.empty());
  for (const auto *sql:{
      "CREATE INDEX audit_idx ON audit_items(id) WHERE id < CAST(CAST(17 AS INT) AS INT);",
      "CREATE INDEX audit_idx ON audit_items(id) WHERE id < CONVERT(INT,CONVERT(INT,23));",
      "CREATE INDEX audit_idx ON audit_items(id) WHERE id > @minimum;"}) {
    const auto invalid=compile(Dialect::SQLServer,sql);
    CHECK(!invalid->syntaxValid && has(*invalid,DiagnosticCode::ExpectedToken));
  }
  const auto distinct=compile(Dialect::SQLServer,
      "SELECT id FROM audit_items WHERE id IS NOT DISTINCT FROM other_id;");
  success(*distinct);
  const auto &predicate=distinct->model.front().expressions.at(distinct->model.front().where);
  CHECK(predicate.kind==ExprKind::Binary && predicate.value=="IS NOT DISTINCT FROM");
  CHECK(distinct->model.types.get(predicate.sqlType).nullability==Nullability::NotNull);
  const auto distinctSql=generateSql(distinct->model,Dialect::SQLServer);
  CHECK(distinctSql.ok());
  success(*compile(Dialect::SQLServer,distinctSql.sql));
}
void sqlServerJsonConstructors() {
  const auto parsed=compile(Dialect::SQLServer,
      "SELECT JSON_OBJECT('item':17,'label':NULL ABSENT ON NULL),JSON_ARRAY(17,NULL NULL ON NULL);");
  success(*parsed);
  const auto &object=projection(*parsed);
  const auto &array=projection(*parsed,1);
  CHECK(object.jsonConstructor && object.jsonConstructor->nullPolicy==JsonNullPolicy::AbsentOnNull);
  CHECK(object.operands.size()==4);
  CHECK(array.jsonConstructor && array.jsonConstructor->nullPolicy==JsonNullPolicy::NullOnNull);
  CHECK(array.operands.size()==2);
  const auto &q=parsed->model.front();
  CHECK(q.expressions.at(object.operands[0]).value=="item");
  CHECK(q.expressions.at(object.operands[1]).value=="17");
  CHECK(q.expressions.at(object.operands[2]).value=="label");
  CHECK(q.expressions.at(object.operands[3]).kind==ExprKind::Null);
  const auto &type=parsed->model.types.get(object.sqlType);
  CHECK(type.family==TypeFamily::VarCharacter && type.nationalCharacter==true);
  CHECK(type.flavor=="max" && type.lengthUnit==LengthUnit::Utf16CodeUnits);
  CHECK(type.nullability==Nullability::NotNull);
  const auto emitted=generateSql(parsed->model,Dialect::SQLServer);
  CHECK(emitted.ok() && emitted.sql.find(": 17")!=std::string::npos);
  const auto again=compile(Dialect::SQLServer,emitted.sql);
  success(*again);
  CHECK(projection(*again).jsonConstructor->nullPolicy==JsonNullPolicy::AbsentOnNull);
  const auto formatted=formatSql(Dialect::SQLServer,emitted.sql);
  CHECK(formatted.ok() && formatted.syntaxSupported);
  CHECK(formatSql(Dialect::SQLServer,formatted.sql).sql==formatted.sql);
  CHECK(dumpAST(*parsed).find("\"jsonConstructor\"")!=std::string::npos);
  CHECK(dumpModel(*parsed).find("ABSENT ON NULL")!=std::string::npos);
  auto malformed=parsed->model;
  const auto id=malformed.front().projections.front().expression;
  malformed.queries.front().expressions.at(id).operands.pop_back();
  const auto rejected=generateSql(malformed,Dialect::SQLServer);
  CHECK(!rejected.ok() && rejected.sql.empty());
  const auto native=compile(Dialect::SQLServer,"SELECT JSON_ARRAY(17 RETURNING JSON);");
  success(*native);
  const auto &nativeType=native->model.types.get(projection(*native).sqlType);
  CHECK(nativeType.opaqueName==std::vector<std::string>{"json"});
  CHECK(nativeType.nullability==Nullability::NotNull);
  const auto nativeSql=generateSql(native->model,Dialect::SQLServer);
  CHECK(nativeSql.ok() && nativeSql.sql.find("RETURNING JSON")!=std::string::npos);
  success(*compile(Dialect::SQLServer,nativeSql.sql));
}
void sqlServerConversions() {
  MemoryCatalog catalog;
  catalog.tables={{"audit","dbo","audit_items",{
      {"item_id",ValueType::Integer},{"label",ValueType::String},
      {"stamp",ValueType::String},{"style_code",ValueType::Integer}},true,true}};
  const auto parsed=compile(Dialect::SQLServer,
      "SELECT CONVERT(INT,item_id),TRY_CONVERT(NVARCHAR(20),label),"
      "TRY_CAST(label AS INT),CONVERT(DATETIME,stamp,style_code) FROM audit_items;",
      {},&catalog,{"audit","dbo",Dialect::SQLServer});
  success(*parsed);
  const auto &q=parsed->model.front();
  const auto &convert=projection(*parsed);
  CHECK(convert.kind==ExprKind::Convert && convert.value=="CONVERT");
  CHECK(convert.operands.size()==1 && convert.castType && convert.castType->name.size()==1);
  CHECK(convert.castType->name.front()=="INT");
  CHECK(q.expressions.at(convert.operands[0]).binding.has_value());
  CHECK(parsed->model.types.get(convert.sqlType).family==TypeFamily::Integer);
  CHECK(parsed->model.types.get(convert.sqlType).bits==32);
  CHECK(parsed->model.types.get(projection(*parsed,1).sqlType).length==20);
  CHECK(parsed->model.types.get(projection(*parsed,1).sqlType).nullability==Nullability::Nullable);
  CHECK(projection(*parsed,2).value=="TRY_CAST");
  CHECK(projection(*parsed,3).operands.size()==2);
  CHECK(q.expressions.at(projection(*parsed,3).operands[1]).binding->column==3);
  const auto generated=generateSql(parsed->model,Dialect::SQLServer);
  CHECK(generated.ok() && generated.sql.find("CONVERT(")!=std::string::npos && generated.sql.find("TRY_CAST(")!=std::string::npos);
  success(*compile(Dialect::SQLServer,generated.sql,{},&catalog,{"audit","dbo",Dialect::SQLServer}));
  const auto formatted=formatSql(Dialect::SQLServer,generated.sql);
  CHECK(formatted.ok() && formatted.syntaxSupported);
  CHECK(formatSql(Dialect::SQLServer,formatted.sql).sql==formatted.sql);
  auto damaged=parsed->model;
  damaged.queries.front().expressions.at(damaged.front().projections.front().expression).operands.clear();
  const auto bad=generateSql(damaged,Dialect::SQLServer);
  CHECK(!bad.ok() && bad.sql.empty());
}
void postgresProjectionKeywords() {
  const auto parsed=compile(Dialect::PostgreSQL,
      "SELECT NULL ISNULL AND,17+23 OR,17 IS,23 AS FROM;");
  success(*parsed);
  const auto &q=parsed->model.front();
  CHECK(q.projections[0].alias=="and" && q.projections[1].alias=="or");
  CHECK(q.projections[2].alias=="is" && q.projections[3].alias=="from");
  CHECK(projection(*parsed).kind==ExprKind::IsNull);
  CHECK(projection(*parsed,1).kind==ExprKind::Binary && projection(*parsed,1).value=="+");
  CHECK(projection(*parsed,2).kind==ExprKind::Number);
  const auto emitted=generateSql(parsed->model,Dialect::PostgreSQL);
  CHECK(emitted.ok());
  const auto again=compile(Dialect::PostgreSQL,emitted.sql);
  success(*again);
  CHECK(again->model.front().projections[0].alias=="and");
  const auto formatted=formatSql(Dialect::PostgreSQL,"select 17 and;");
  CHECK(formatted.ok() && formatted.syntaxSupported);
  CHECK(formatSql(Dialect::PostgreSQL,formatted.sql).sql==formatted.sql);
  const auto named=compile(Dialect::PostgreSQL,"SELECT ESCAPE 'audit';");
  success(*named);
  CHECK(projection(*named).kind==ExprKind::TypedLiteral);
  CHECK(named->model.types.get(projection(*named).sqlType).opaqueName==std::vector<std::string>{"escape"});
  const auto literalSql=generateSql(named->model,Dialect::PostgreSQL);
  CHECK(literalSql.ok());
  success(*compile(Dialect::PostgreSQL,literalSql.sql));
}
void postgresIdentifierContexts() {
  MemoryCatalog catalog;
  catalog.tables={{"audit","public","audit_items",{
      {"analyse",ValueType::Integer},{"like",ValueType::String},
      {"exists",ValueType::Integer},{"coalesce",ValueType::Integer},
      {"filter",ValueType::Integer},{"current_date",ValueType::String}},true,true}};
  const auto parsed=compile(Dialect::PostgreSQL,
      "SELECT audit_items.analyse,audit_items.like,exists,coalesce,filter,\"current_date\",CURRENT_DATE FROM audit_items;",
      {},&catalog,{"audit","public",Dialect::PostgreSQL});
  success(*parsed);
  for (std::size_t i=0;i<6;++i) {
    CHECK(projection(*parsed,i).kind==ExprKind::Name);
    CHECK(projection(*parsed,i).binding && projection(*parsed,i).binding->column==i);
  }
  CHECK(projection(*parsed,6).kind==ExprKind::Call);
  CHECK(!projection(*parsed,6).binding);
  const auto emitted=generateSql(parsed->model,Dialect::PostgreSQL);
  CHECK(emitted.ok());
  success(*compile(Dialect::PostgreSQL,emitted.sql,{},&catalog,{"audit","public",Dialect::PostgreSQL}));
  const auto roles=compile(Dialect::PostgreSQL,
      "SELECT CURRENT_CATALOG,CURRENT_ROLE,CURRENT_SCHEMA,USER,current_schema();");
  success(*roles);
  for (std::size_t i=0;i<5;++i) {
    const auto &e=projection(*roles,i);
    CHECK(e.kind==ExprKind::Call && callKind(e,Dialect::PostgreSQL)==RoutineKind::Scalar);
    CHECK(callVolatility(e,Dialect::PostgreSQL)==FunctionVolatility::Stable);
  }
  const auto functionAndType=compile(Dialect::PostgreSQL,"SELECT like('audit'),like 'audit';");
  success(*functionAndType);
  CHECK(projection(*functionAndType).kind==ExprKind::Call);
  CHECK(projection(*functionAndType,1).kind==ExprKind::TypedLiteral);
  CHECK(functionAndType->model.types.get(projection(*functionAndType,1).sqlType).opaqueName==std::vector<std::string>{"like"});
}
void postgresPaginationExpressions() {
  const auto pg=Dialect::PostgreSQL;
  for (const auto *sql:{
      "SELECT 17 LIMIT 1+1 OFFSET NULL;",
      "SELECT 17 OFFSET (1-1) ROWS FETCH FIRST ABS(-1) ROW ONLY;",
      "SELECT 17 ORDER BY 1 OFFSET 1-1 FETCH FIRST (1+1) ROWS WITH TIES;"}) {
    const auto parsed=compile(pg,sql);
    success(*parsed);
    for (const bool compact:{false,true}) {
      GenerationOptions options;
      options.compactExpressions=compact;
      const auto generated=generateSql(parsed->model,pg,options);
      CHECK(generated.ok());
      success(*compile(pg,generated.sql));
    }
    const auto formatted=formatSql(pg,sql);
    CHECK(formatted.ok() && formatted.syntaxSupported);
    CHECK(formatSql(pg,formatted.sql).sql==formatted.sql);
  }
  const auto arithmetic=compile(pg,"SELECT 17 LIMIT 1+1 OFFSET NULL;");
  success(*arithmetic);
  const auto &q=arithmetic->model.front();
  CHECK(q.expressions.at(q.limit).kind==ExprKind::Binary);
  CHECK(q.expressions.at(q.limit).value=="+");
  CHECK(q.expressions.at(q.offset).kind==ExprKind::Null);
  const auto prepared=compile(pg,"SELECT 17 LIMIT $1 OFFSET $2;");
  success(*prepared);
  const auto &parameters=prepared->model.front();
  CHECK(prepared->model.types.get(parameters.expressions.at(parameters.limit).sqlType).bits==64);
  CHECK(prepared->model.types.get(parameters.expressions.at(parameters.offset).sqlType).bits==64);
  MemoryCatalog catalog;
  catalog.tables={{"audit","public","audit_items",{{"item_id",ValueType::Integer}},true,true}};
  const auto local=compile(pg,"SELECT item_id FROM audit_items LIMIT item_id;",{},&catalog,{"audit","public",pg});
  CHECK(local->syntaxValid && has(*local,DiagnosticCode::InvalidQueryShape));
  const auto correlated=compile(pg,"SELECT (SELECT 17 LIMIT audit_items.item_id) FROM audit_items;",
      {},&catalog,{"audit","public",pg});
  success(*correlated);
  const auto inner=projection(*correlated).query;
  const auto &child=correlated->model.queries.at(inner-1);
  CHECK(child.expressions.at(child.limit).binding->scope!=child.id);
}
void sqlitePaginationExpressions() {
  const auto sqlite=Dialect::SQLite;
  for (const auto *sql:{"SELECT 17 LIMIT -1 OFFSET -1;","SELECT 17 LIMIT 1-1,1+1;",
                        "SELECT 17 LIMIT ABS(-1);","SELECT 17 LIMIT 1.0;"}) {
    const auto parsed=compile(sqlite,sql);
    success(*parsed);
    const auto generated=generateSql(parsed->model,sqlite);
    CHECK(generated.ok());
    success(*compile(sqlite,generated.sql));
    const auto formatted=formatSql(sqlite,sql);
    CHECK(formatted.ok() && formatted.syntaxSupported);
    CHECK(formatSql(sqlite,formatted.sql).sql==formatted.sql);
  }
  const auto comma=compile(sqlite,"SELECT 17 LIMIT 0,1+1;");
  success(*comma);
  const auto &q=comma->model.front();
  CHECK(q.limitSyntax==LimitSyntax::CommaLimit);
  CHECK(q.expressions.at(q.offset).value=="0");
  CHECK(q.expressions.at(q.limit).kind==ExprKind::Binary);
  MemoryCatalog catalog;
  catalog.tables={{"owned","main","audit_items",{{"item_id",ValueType::Integer},{"TRUE",ValueType::Integer}},true,true}};
  const auto boolean=compile(sqlite,"SELECT item_id FROM audit_items LIMIT TRUE;",{},&catalog,{"owned","main",sqlite});
  success(*boolean);
  CHECK(boolean->model.front().expressions.at(boolean->model.front().limit).kind==ExprKind::Boolean);
  const auto outer=compile(sqlite,"SELECT (SELECT 17 LIMIT audit_items.item_id) FROM audit_items;",
      {},&catalog,{"owned","main",sqlite});
  CHECK(outer->syntaxValid && has(*outer,DiagnosticCode::UnknownColumn));
}
void postgresAnonymousDerivedTables() {
  const auto pg=Dialect::PostgreSQL;
  const auto parsed=compile(pg,"SELECT item_id FROM (SELECT 17 AS item_id);");
  success(*parsed);
  const auto &q=parsed->model.front();
  CHECK(q.tables.front().alias.empty() && q.tables.front().name.empty());
  CHECK(q.tables.front().query && projection(*parsed).binding);
  CHECK(projection(*parsed).binding->table==0 && projection(*parsed).binding->column==0);
  const auto generated=generateSql(parsed->model,pg);
  CHECK(generated.ok());
  success(*compile(pg,generated.sql));
  CHECK(!generateSql(parsed->model,Dialect::MySQL).ok());
  const auto values=compile(pg,"SELECT column1 FROM (VALUES(17),(23));");
  success(*values);
  CHECK(projection(*values).binding.has_value());
  CHECK(values->model.front().tables.front().definition->columns.front().name=="column1");
  const auto lateral=compile(pg,
      "SELECT next_id FROM (SELECT 17 AS item_id) items CROSS JOIN LATERAL (SELECT items.item_id+1 AS next_id);");
  success(*lateral);
  CHECK(lateral->model.front().tables[1].alias.empty());
  CHECK(projection(*lateral).binding && projection(*lateral).binding->table==1);
  const auto formatted=formatSql(pg,generated.sql);
  CHECK(formatted.ok() && formatted.syntaxSupported);
  CHECK(formatSql(pg,formatted.sql).sql==formatted.sql);
}
void tidbNativeGrammarEdges() {
  const auto dynamic=compile(Dialect::TiDB,"SELECT NOW(1+1),NOW('3'),NOW(NULL),NOW(000006),NOW(-0);");
  success(*dynamic);
  CHECK(projection(*dynamic).kind==ExprKind::Call);
  CHECK(!dynamic->model.types.get(projection(*dynamic).sqlType).precision);
  CHECK(dynamic->model.types.get(projection(*dynamic,2).sqlType).precision==0);
  CHECK(dynamic->model.types.get(projection(*dynamic,2).sqlType).nullability==Nullability::NotNull);
  CHECK(dynamic->model.types.get(projection(*dynamic,3).sqlType).precision==6);
  const auto negative=compile(Dialect::TiDB,"SELECT NOW(-1);");
  CHECK(negative->syntaxValid && has(*negative,DiagnosticCode::InvalidQueryShape));
  const auto modifiers=compile(Dialect::TiDB,"SELECT SUM(DISTINCT ALL 17),AVG(DISTINCTROW ALL 23),STD(DISTINCTROW ALL 17);");
  success(*modifiers);
  for (std::size_t i=0;i<3;++i)CHECK(projection(*modifiers,i).distinct);
  const auto modifierSql=generateSql(modifiers->model,Dialect::TiDB);
  CHECK(modifierSql.ok());
  success(*compile(Dialect::TiDB,modifierSql.sql));
  LanguageOptions noBackslash;
  noBackslash.mysqlNoBackslashEscapes=true;
  success(*compile(Dialect::TiDB,"SELECT 'item' LIKE 'i%' ESCAPE '';",noBackslash));
  const auto unicode=compile(Dialect::TiDB,"SELECT 'item' LIKE 'i%' ESCAPE '§';");
  CHECK(unicode->syntaxValid && has(*unicode,DiagnosticCode::InvalidQueryShape));
  for (const auto *sql:{"ALTER TABLE audit_items ALTER COLUMN id SET DEFAULT (17+23);",
                        "ALTER TABLE audit_items ALTER COLUMN created_at SET DEFAULT (NOW());"}) {
    const auto parsed=compile(Dialect::TiDB,sql);
    success(*parsed);
    CHECK(parsed->model.front().ddl->actions.front().defaultParenthesized);
    const auto generated=generateSql(parsed->model,Dialect::TiDB);
    CHECK(generated.ok() && generated.sql.find(" SET DEFAULT (")!=std::string::npos);
    success(*compile(Dialect::TiDB,generated.sql));
    auto corrupt=parsed->model;
    corrupt.queries.front().ddl->actions.front().defaultParenthesized=false;
    const auto rejected=generateSql(corrupt,Dialect::TiDB);
    CHECK(!rejected.ok() && rejected.sql.empty());
  }
}
void mysqlNativeFunctionSpacing() {
  for (auto dialect:{Dialect::MySQL,Dialect::MariaDB,Dialect::TiDB}) {
    LanguageOptions mode;
    mode.mysqlIgnoreSpace=true;
    for (const auto *sql:{"SELECT SUM (ALL 17);","SELECT CURDATE ();","SELECT CAST (17 AS SIGNED);"}) {
      const auto ordinary=compile(dialect,sql);
      CHECK(!ordinary->syntaxValid && has(*ordinary,DiagnosticCode::UnsupportedSyntax));
      const auto enabled=compile(dialect,sql,mode);
      success(*enabled);
      const auto generated=generateSql(enabled->model,dialect);
      CHECK(generated.ok());
      success(*compile(dialect,generated.sql));
    }
    const auto comment=compile(dialect,"SELECT SUM/**/(ALL 17);",mode);
    if (dialect==Dialect::MariaDB) success(*comment);
    else CHECK(!comment->syntaxValid && has(*comment,DiagnosticCode::UnsupportedSyntax));
    const auto generic=compile(dialect,"SELECT audit_tools.sum (17);");
    success(*generic);
    CHECK(projection(*generic).name.size()==2 && callKind(projection(*generic),dialect)==RoutineKind::Unknown);
    const auto invalid=compile(dialect,"SELECT CURDATE (1);",mode);
    CHECK(!invalid->syntaxValid && has(*invalid,DiagnosticCode::ExpectedToken));
  }
}
void explicitFunctionAll() {
  for (auto dialect:{Dialect::MySQL,Dialect::PostgreSQL,Dialect::SQLServer,Dialect::Oracle,
                    Dialect::SQLite,Dialect::MariaDB,Dialect::TiDB,Dialect::OceanBase}) {
    const auto from=dialect==Dialect::Oracle?" FROM dual":"";
    const auto parsed=compile(dialect,std::string("SELECT SUM(ALL 17)")+from+";");
    const auto normal=compile(dialect,std::string("SELECT SUM(17)")+from+";");
    success(*parsed);success(*normal);
    CHECK(projection(*parsed).kind==ExprKind::Call && !projection(*parsed).distinct && projection(*parsed).operands.size()==1);
    const auto generated=generateSql(parsed->model,dialect);
    CHECK(generated.ok() && generated.sql==generateSql(normal->model,dialect).sql);
    success(*compile(dialect,generated.sql));
  }
  for (auto dialect:{Dialect::MySQL,Dialect::MariaDB,Dialect::TiDB,Dialect::OceanBase}) {
    const auto parsed=compile(dialect,"SELECT BIT_AND(17),JSON_ARRAYAGG(17),JSON_OBJECTAGG('item',17),STD(17);");
    success(*parsed);
    for (std::size_t i=0;i<4;++i)CHECK(callKind(projection(*parsed,i),dialect)==RoutineKind::Aggregate);
  }
  const auto maria=compile(Dialect::MariaDB,"SELECT JSON_ARRAYAGG(DISTINCT 17);");
  success(*maria);
  CHECK(generateSql(maria->model,Dialect::MariaDB).ok());
  const auto incompatible=generateSql(maria->model,Dialect::MySQL);
  CHECK(!incompatible.ok() && incompatible.sql.empty());
  const auto tidb=compile(Dialect::TiDB,"SELECT STD(DISTINCT 17);");
  success(*tidb);
  CHECK(!generateSql(tidb->model,Dialect::MariaDB).ok());
}
void oracleAnonymousInlineViews() {
  const auto parsed=compile(Dialect::Oracle,
      "SELECT item_id,event_id FROM (SELECT 17 AS item_id FROM dual) "
      "CROSS JOIN (SELECT 23 AS event_id FROM dual);");
  success(*parsed);
  const auto &q=parsed->model.front();
  CHECK(q.tables.size()==2 && q.tables[0].alias.empty() && q.tables[1].alias.empty());
  CHECK(q.projections.size()==2);
  CHECK(projection(*parsed).binding && projection(*parsed,1).binding);
  const auto generated=generateSql(parsed->model,Dialect::Oracle);
  CHECK(generated.ok());
  success(*compile(Dialect::Oracle,generated.sql));
  CHECK(!generateSql(parsed->model,Dialect::MySQL).ok());
  const auto ambiguous=compile(Dialect::Oracle,
      "SELECT item_id FROM (SELECT 17 AS item_id FROM dual),(SELECT 23 AS item_id FROM dual);");
  CHECK(ambiguous->syntaxValid && has(*ambiguous,DiagnosticCode::AmbiguousColumn));
}
void sqlitePredicatePrecedence() {
  const auto parsed=compile(Dialect::SQLite,"SELECT 2=0 LIKE '0',2=0 IN (0),2 BETWEEN 0=0 AND 3,2 BETWEEN 0 AND 3=1;");
  success(*parsed);
  const auto &q=parsed->model.front();
  CHECK(projection(*parsed).value=="LIKE");
  CHECK(q.expressions.at(projection(*parsed).operands[0]).value=="=");
  CHECK(projection(*parsed,1).kind==ExprKind::In);
  CHECK(q.expressions.at(projection(*parsed,1).operands[0]).value=="=");
  CHECK(projection(*parsed,2).kind==ExprKind::Between);
  CHECK(q.expressions.at(projection(*parsed,2).operands[1]).value=="=");
  CHECK(projection(*parsed,3).kind==ExprKind::Binary && projection(*parsed,3).value=="=");
  CHECK(q.expressions.at(projection(*parsed,3).operands[0]).kind==ExprKind::Between);
  const auto generated=generateSql(parsed->model,Dialect::SQLite);
  CHECK(generated.ok());
  success(*compile(Dialect::SQLite,generated.sql));
}
void mysqlRegexpGrammar() {
  for (auto d : {Dialect::MySQL,Dialect::MariaDB,Dialect::TiDB,Dialect::OceanBase}) {
    const auto parsed=compile(d,"SELECT 'item' RLIKE '^i',NULL NOT REGEXP '^i' FROM audit_items WHERE NOT label REGEXP '^i' AND item_id=17;");
    success(*parsed);
    const auto &q=parsed->model.front();
    CHECK(projection(*parsed).kind==ExprKind::Binary && projection(*parsed).value=="REGEXP");
    CHECK(projection(*parsed,1).value=="NOT REGEXP");
    CHECK(parsed->model.types.get(projection(*parsed,1).sqlType).nullability==Nullability::Nullable);
    const auto &condition=q.expressions.at(q.where);
    CHECK(condition.value=="AND");
    const auto &negation=q.expressions.at(condition.operands.front());
    CHECK(negation.kind==ExprKind::Unary && negation.value=="NOT");
    CHECK(q.expressions.at(negation.operands.front()).value=="REGEXP");
    const auto generated=generateSql(parsed->model,d);
    CHECK(generated.ok());
    success(*compile(d,generated.sql));
    CHECK(!generateSql(parsed->model,Dialect::PostgreSQL).ok());
    // Regex syntax and collation rules differ even within the MySQL family.
    CHECK(!generateSql(parsed->model,d==Dialect::MySQL?Dialect::MariaDB:Dialect::MySQL).ok());
  }
  const auto maria=compile(Dialect::MariaDB,"SELECT 2=0 REGEXP '^0$',2=0 LIKE '0',2=0 IN (0),2=0 IS NULL;");
  success(*maria);
  const auto &query=maria->model.front();
  for (std::size_t i=0;i<3;++i) {
    const auto &comparison=projection(*maria,i);
    CHECK(comparison.kind==ExprKind::Binary && comparison.value=="=");
    const auto &predicate=query.expressions.at(comparison.operands[1]);
    CHECK(predicate.kind==(i==2?ExprKind::In:ExprKind::Binary));
  }
  CHECK(projection(*maria,3).kind==ExprKind::IsNull);
  CHECK(query.expressions.at(projection(*maria,3).operands[0]).value=="=");
  const auto roundTrip=generateSql(maria->model,Dialect::MariaDB);
  CHECK(roundTrip.ok());
  success(*compile(Dialect::MariaDB,roundTrip.sql));
  const auto mysql=compile(Dialect::MySQL,"SELECT 2=0 LIKE '0';");
  success(*mysql);
  CHECK(projection(*mysql).value=="=");
  for (const auto dialect:{Dialect::MySQL,Dialect::MariaDB,Dialect::TiDB,Dialect::OceanBase}) {
    const auto boundaries=compile(dialect,
        "SELECT 0=0 BETWEEN 0 AND 1, 0 BETWEEN 0 AND 1=1, "
        "0 BETWEEN 0 AND 1 BETWEEN 0 AND 0, 0=0 IS TRUE;");
    success(*boundaries);
    const auto &expressions=boundaries->model.front().expressions;
    CHECK(projection(*boundaries,0).value=="=");
    CHECK(expressions.at(projection(*boundaries,0).operands[1]).kind==ExprKind::Between);
    CHECK(projection(*boundaries,1).value=="=");
    CHECK(expressions.at(projection(*boundaries,1).operands[0]).kind==ExprKind::Between);
    CHECK(projection(*boundaries,2).kind==ExprKind::Between);
    CHECK(expressions.at(projection(*boundaries,2).operands[2]).kind==ExprKind::Between);
    CHECK(projection(*boundaries,3).kind==ExprKind::TruthTest);
    CHECK(expressions.at(projection(*boundaries,3).operands[0]).value=="=");
  }
}
void postgresNonAssociativeExpressions() {
  for (const auto sql:{"SELECT TRUE BETWEEN 17=17 AND TRUE;",
                       "SELECT TRUE BETWEEN 17 IS DISTINCT FROM 23 AND TRUE;"}) {
    const auto parsed=compile(Dialect::PostgreSQL,sql);
    success(*parsed);
    const auto &between=projection(*parsed);
    CHECK(between.kind==ExprKind::Between);
    const auto &low=parsed->model.front().expressions.at(between.operands[1]);
    CHECK(low.kind==ExprKind::Binary && (low.value=="=" || low.value=="IS DISTINCT FROM"));
    const auto generated=generateSql(parsed->model,Dialect::PostgreSQL);
    CHECK(generated.ok());
    success(*compile(Dialect::PostgreSQL,generated.sql));
  }
  for (const auto sql:{"SELECT 17=23=29;","SELECT TRUE IS DISTINCT FROM FALSE ISNULL;",
                       "SELECT 17 BETWEEN 0 AND 23 IN(TRUE);",
                       "SELECT TRUE BETWEEN 17 IN(17) AND TRUE;"}) {
    const auto parsed=compile(Dialect::PostgreSQL,sql);
    CHECK(!parsed->syntaxValid && has(*parsed,DiagnosticCode::ExpectedToken));
  }
  for (const auto sql:{"SELECT TRUE IS TRUE IS TRUE;","SELECT NULL ISNULL ISNULL;",
                       "SELECT 17 IN(17) IN(TRUE);","SELECT 17=ANY(ARRAY[17])=TRUE;"})
    success(*compile(Dialect::PostgreSQL,sql));
  // Fixed-token IN chains are legal; recursive comparisons are not.
  CHECK(compile(Dialect::MySQL,"SELECT 17=23=29;")->syntaxValid);
}
void sqlServerProjectionAliases() {
  MemoryCatalog catalog;
  catalog.tables={{"owned","dbo","audit_items",{{"item_id",ValueType::Integer},{"TRUE",ValueType::Integer},{"FALSE",ValueType::Integer}},true,true}};
  NameResolutionContext context{"owned","dbo",Dialect::SQLServer};
  const auto named=compile(Dialect::SQLServer,"SELECT flag=item_id,TRUE,FALSE FROM audit_items;",{},&catalog,context);
  success(*named);
  CHECK(named->model.front().projections[0].alias=="flag");
  for (std::size_t i=0;i<3;++i) {
    CHECK(projection(*named,i).kind==ExprKind::Name);
    CHECK(projection(*named,i).binding.has_value());
  }
  for (const auto sql:{"SELECT flag=17,other=23;","SELECT 'field label'=17;",
                       "SELECT [field label]=17;","SELECT TRUE=17,FALSE=23;",
                       "SELECT 17 AS 'field label';","SELECT 17 'field label';"}) {
    const auto parsed=compile(Dialect::SQLServer,sql);
    success(*parsed);
    CHECK(!parsed->model.front().projections.front().alias.empty());
    const auto generated=generateSql(parsed->model,Dialect::SQLServer);
    CHECK(generated.ok());
    success(*compile(Dialect::SQLServer,generated.sql));
    const auto formatted=formatSql(Dialect::SQLServer,sql);
    CHECK(formatted.ok());
    success(*compile(Dialect::SQLServer,formatted.sql));
  }
  CHECK(has(*compile(Dialect::SQLServer,"SELECT @flag=17;"),DiagnosticCode::UnsupportedSyntax));
  CHECK(!compile(Dialect::SQLServer,"SELECT flag=17 AS other;")->syntaxValid);
  CHECK(!compile(Dialect::SQLServer,"UPDATE audit_items SET item_id=17 OUTPUT flag=inserted.item_id;")->ok());
}
void postgresqlOptionalDdlNames() {
  for (const auto *sql:{"CREATE TABLE audit_empty();",
                        "CREATE TEMP TABLE audit_empty();",
                        "CREATE UNLOGGED TABLE audit_empty();",
                        "CREATE INDEX ON audit_items(item_id);",
                        "CREATE UNIQUE INDEX ON audit_items(item_id DESC) WHERE item_id>17;",
                        "CREATE INDEX ON audit_items USING \"btree\"(item_id);",
                        "CREATE INDEX ON owned.audit_items USING btree(item_id);"}) {
    const auto parsed=compile(Dialect::PostgreSQL,sql);
    success(*parsed);
    CHECK(parsed->model.front().ddl.has_value());
    const auto generated=generateSql(parsed->model,Dialect::PostgreSQL);
    CHECK(generated.ok());
    success(*compile(Dialect::PostgreSQL,generated.sql));
    for (const auto target:{Dialect::MySQL,Dialect::MariaDB,Dialect::TiDB,Dialect::OceanBase,
                            Dialect::SQLite,Dialect::SQLServer,Dialect::Oracle}) {
      const auto rejected=generateSql(parsed->model,target);
      CHECK(!rejected.ok());
      CHECK(rejected.sql.empty());
    }
  }
  const auto empty=compile(Dialect::PostgreSQL,"CREATE TABLE audit_empty();");
  CHECK(empty->model.front().ddl->definitions.empty());
  const auto unnamed=compile(Dialect::PostgreSQL,"CREATE INDEX ON audit_items(item_id);");
  CHECK(unnamed->model.front().ddl->name.empty());
  for (const auto *sql:{"CREATE INDEX ON audit_items((17));",
                        "CREATE INDEX ON audit_items((item_id+17));",
                        "CREATE INDEX ON audit_items(ABS(item_id));",
                        "CREATE INDEX ON audit_items(CAST(item_id AS INT));",
                        "CREATE INDEX ON audit_items((item_id::INT));",
                        "CREATE INDEX ON audit_items((audit_items.item_id));"}) {
    const auto parsed=compile(Dialect::PostgreSQL,sql);
    success(*parsed);
    const auto generated=generateSql(parsed->model,Dialect::PostgreSQL);
    CHECK(generated.ok());
    success(*compile(Dialect::PostgreSQL,generated.sql));
  }
  auto invalid=unnamed->model;
  invalid.queries.front().ddl->ifNotExists=true;
  const auto rejected=generateSql(invalid,Dialect::PostgreSQL);
  CHECK(!rejected.ok());
  CHECK(rejected.sql.empty());
  invalid=unnamed->model;
  invalid.queries.front().ddl->indexMethod={"pg_catalog","btree"};
  const auto invalidMethod=generateSql(invalid,Dialect::PostgreSQL);
  CHECK(!invalidMethod.ok() && invalidMethod.sql.empty());
  for (const auto *sql:{"CREATE TABLE audit_empty;","CREATE TABLE audit_empty(,);",
                        "CREATE TABLE audit_empty() AS SELECT 17;",
                        "CREATE TABLE audit_items(item_id INT) AS SELECT 17;",
                        "CREATE INDEX IF NOT EXISTS ON audit_items(item_id);",
                        "CREATE INDEX owned.audit_idx ON audit_items(item_id);",
                        "CREATE INDEX ON audit_items USING pg_catalog.btree(item_id);",
                        "CREATE INDEX ON audit_items(17);",
                        "CREATE INDEX ON audit_items(NULL);",
                        "CREATE INDEX ON audit_items(TRUE);",
                        "CREATE INDEX ON audit_items(item_id+17);",
                        "CREATE INDEX ON audit_items((item_id)+17);",
                        "CREATE INDEX ON audit_items(ABS(item_id)+17);",
                        "CREATE INDEX ON audit_items(item_id::INT);",
                        "CREATE INDEX ON audit_items(ROW(item_id));",
                        "CREATE INDEX ON audit_items(audit_items.item_id);",
                        "CREATE INDEX ON audit_items(SUM(item_id) FILTER(WHERE item_id>0));"})
    CHECK(!compile(Dialect::PostgreSQL,sql)->syntaxValid);
  CHECK(has(*compile(Dialect::PostgreSQL,"CREATE INDEX CONCURRENTLY ON audit_items(item_id);"),DiagnosticCode::UnsupportedSyntax));
  for (const auto *sql:{"CREATE INDEX ON audit_items(CURRENT_DATE);",
                        "CREATE INDEX ON audit_items(random());",
                        "CREATE INDEX ON audit_items((item_id+random()));",
                        "CREATE INDEX ON audit_items(item_id) WHERE CURRENT_DATE>DATE '2020-01-01';"}) {
    const auto parsed=compile(Dialect::PostgreSQL,sql);
    CHECK(parsed->syntaxValid);
    CHECK(!parsed->ok());
    CHECK(has(*parsed,DiagnosticCode::InvalidQueryShape));
  }
  success(*compile(Dialect::PostgreSQL,"CREATE TABLE audit_items(created_at TIMESTAMP DEFAULT NOW());"));
}
void sqlServerKeywordContexts() {
  MemoryCatalog catalog;
  catalog.tables={{"owned","dbo","audit_items",{{"fillfactor",ValueType::Integer},{"LOCALTIME",ValueType::Integer},
      {"LOCALTIMESTAMP",ValueType::Integer},{"USER",ValueType::Integer},{"label",ValueType::String}},true,true}};
  const auto named=compile(Dialect::SQLServer,"SELECT [fillfactor],LOCALTIME,LOCALTIMESTAMP,[USER],USER FROM audit_items;",{},&catalog,{"owned","dbo",Dialect::SQLServer});
  success(*named);
  for (std::size_t i=0;i<4;++i) CHECK(projection(*named,i).kind==ExprKind::Name && projection(*named,i).binding);
  CHECK(projection(*named,4).kind==ExprKind::Call && projection(*named,4).type==ValueType::String);
  for (const auto sql:{"SELECT USER,CURRENT_USER,SESSION_USER,SYSTEM_USER;",
                       "SELECT COALESCE(NULL,17),NULLIF(17,23),LEFT('item',1),RIGHT('item',1);",
                       "SELECT 17 FROM audit_items WHERE CONTAINS(label,'item');",
                       "SELECT DISK,DUMP,LOAD,PRECISION,SECURITYAUDIT FROM audit_items;",
                       "CREATE TABLE audit_items(amount DOUBLE PRECISION);"}) {
    const auto parsed=compile(Dialect::SQLServer,sql);
    success(*parsed);
    const auto generated=generateSql(parsed->model,Dialect::SQLServer);
    CHECK(generated.ok());
    success(*compile(Dialect::SQLServer,generated.sql));
    const auto formatted=formatSql(Dialect::SQLServer,sql);
    CHECK(formatted.ok());
    success(*compile(Dialect::SQLServer,formatted.sql));
  }
  for (const auto sql:{"CREATE SCHEMA audit;SELECT 17;",
                       "SELECT 17;CREATE VIEW audit_v AS SELECT 17 AS item_id;",
                       "CREATE TABLE audit_items(FILLFACTOR INT);"})
    CHECK(!compile(Dialect::SQLServer,sql)->syntaxValid);
  CHECK(has(*compile(Dialect::SQLServer,"SELECT IDENTITYCOL FROM audit_items;"),DiagnosticCode::UnsupportedSyntax));
  auto fabricated=compile(Dialect::PostgreSQL,"CREATE VIEW audit_v AS SELECT 17 AS item_id;SELECT item_id FROM audit_v;")->model;
  fabricated.sourceDialect=Dialect::SQLServer;
  const auto rejected=generateSql(fabricated,Dialect::SQLServer);
  CHECK(!rejected.ok() && rejected.sql.empty());
}
void tidbDdlProductionBoundaries() {
  for (const auto sql:{"CREATE TABLE audit_items (item_id INT3,label SET('item','other'));",
                       "CREATE TABLE audit_items (item_id INT1,amount FIXED(17,2),label VARCHARACTER(23));",
                       "CREATE TABLE audit_items (item_id INT,CONSTRAINT PRIMARY KEY(item_id));",
                       "CREATE TABLE audit_items (item_id INT CONSTRAINT CHECK(item_id>0));",
                       "CREATE TABLE audit_items (`ssl` INT,`require` INT);"}) {
    const auto parsed=compile(Dialect::TiDB,sql);
    success(*parsed);
    const auto generated=generateSql(parsed->model,Dialect::TiDB);
    CHECK(generated.ok());
    success(*compile(Dialect::TiDB,generated.sql));
    const auto formatted=formatSql(Dialect::TiDB,sql);
    CHECK(formatted.ok());
    success(*compile(Dialect::TiDB,formatted.sql));
  }
  const auto generatedAuto=compile(Dialect::TiDB,"CREATE TABLE audit_items (item_id INT,next_id INT AS(item_id+1) PRIMARY KEY AUTO_INCREMENT);");
  CHECK(generatedAuto->syntaxValid && has(*generatedAuto,DiagnosticCode::InvalidQueryShape));
  const auto decimal=compile(Dialect::TiDB,"CREATE TABLE audit_items (amount FIXED(17,2));");
  success(*decimal);
  const auto type=resolveDeclaredType(decimal->model.front().ddl->definitions.front().type,Dialect::TiDB).type;
  CHECK(type.family==TypeFamily::Decimal && type.precision==17 && type.scale==2);
}
void sqliteGlobPredicates() {
  for (const auto sql:{"SELECT 'item_17' GLOB 'item_*';","SELECT 'item_17' NOT GLOB 'item_*';",
                       "SELECT NOT 'item_17' GLOB 'item_*';",
                       "SELECT 'item_17' GLOB 'item_'||'*'=1;"}) {
    const auto parsed=compile(Dialect::SQLite,sql);
    success(*parsed);
    CHECK(projection(*parsed).type==ValueType::Boolean);
    const auto generated=generateSql(parsed->model,Dialect::SQLite);
    CHECK(generated.ok());
    success(*compile(Dialect::SQLite,generated.sql));
    CHECK(!generateSql(parsed->model,Dialect::PostgreSQL).ok());
    const auto formatted=formatSql(Dialect::SQLite,sql);
    CHECK(formatted.ok());
    success(*compile(Dialect::SQLite,formatted.sql));
    CHECK(formatSql(Dialect::SQLite,formatted.sql).sql==formatted.sql);
  }
  const auto missing=compile(Dialect::SQLite,"SELECT NULL GLOB '*';");
  success(*missing);
  CHECK(missing->model.types.get(projection(*missing).sqlType).nullability==Nullability::Nullable);
  const auto negated=compile(Dialect::SQLite,"SELECT 'item_17' NOT GLOB 'item_*';");
  CHECK(projection(*negated).value=="NOT GLOB" && projection(*negated).operands.size()==2);
}
void sqlServerPaginationExpressions() {
  for (const auto sql:{"SELECT TOP(1+1) 17;","SELECT TOP 1 +1;","SELECT TOP 1.5 17;",
                       "SELECT TOP(@take+1) 17;","SELECT TOP(9%2) 17;",
                       "SELECT 17 ORDER BY 1 OFFSET @skip+1 ROWS FETCH NEXT @take+1 ROWS ONLY;",
                       "UPDATE TOP(1+1) audit_items SET item_id=17;",
                       "DELETE TOP(@take+1) FROM audit_items;"}) {
    const auto parsed=compile(Dialect::SQLServer,sql);
    success(*parsed);
    const auto generated=generateSql(parsed->model,Dialect::SQLServer);
    CHECK(generated.ok());
    success(*compile(Dialect::SQLServer,generated.sql));
    const auto formatted=formatSql(Dialect::SQLServer,sql);
    CHECK(formatted.ok());
    success(*compile(Dialect::SQLServer,formatted.sql));
    CHECK(formatSql(Dialect::SQLServer,formatted.sql).sql==formatted.sql);
  }
  const auto bare=compile(Dialect::SQLServer,"SELECT TOP 1 +1;");
  CHECK(bare->model.front().expressions.at(bare->model.front().limit).kind==ExprKind::Number);
  CHECK(projection(*bare).kind==ExprKind::Unary);
  for (const auto sql:{"SELECT TOP(-1) 17;",
                       "SELECT 17 ORDER BY 1 OFFSET -1 ROWS;",
                       "SELECT 17 ORDER BY 1 OFFSET 0 ROWS FETCH NEXT 0 ROWS ONLY;"}) {
    const auto parsed=compile(Dialect::SQLServer,sql);
    CHECK(parsed->syntaxValid && has(*parsed,DiagnosticCode::InvalidQueryShape));
  }
  CHECK(has(*compile(Dialect::SQLServer,"SELECT TOP((SELECT 1)) 17;"),DiagnosticCode::UnsupportedSyntax));
}
void oraclePaginationLiterals() {
  for (const auto sql:{"SELECT 17 FROM dual OFFSET -1 ROWS;",
                       "SELECT 17 FROM dual OFFSET NULL ROWS FETCH NEXT 1.5 ROWS ONLY;",
                       "SELECT 17 FROM dual FETCH FIRST NULL ROWS ONLY;",
                       "SELECT 17 FROM dual FETCH NEXT (1+1) ROWS ONLY;",
                       "SELECT 17 FROM dual FETCH FIRST ROWS WITH TIES;"}) {
    const auto parsed=compile(Dialect::Oracle,sql);
    success(*parsed);
    const auto generated=generateSql(parsed->model,Dialect::Oracle);
    CHECK(generated.ok());
    success(*compile(Dialect::Oracle,generated.sql));
    const auto formatted=formatSql(Dialect::Oracle,sql);
    CHECK(formatted.ok());
    success(*compile(Dialect::Oracle,formatted.sql));
  }
  const auto negative=compile(Dialect::Oracle,"SELECT 17 FROM dual FETCH FIRST -1 ROWS ONLY;");
  success(*negative);
  CHECK(negative->model.front().expressions.at(negative->model.front().limit).kind==ExprKind::Unary);
  CHECK(has(*compile(Dialect::Oracle,"SELECT 17 FROM dual FETCH NEXT ABS(1) ROWS ONLY;"),DiagnosticCode::UnsupportedSyntax));
  CHECK(has(*compile(Dialect::Oracle,"SELECT 17 FROM dual FETCH FIRST 17 PERCENT ROWS ONLY;"),DiagnosticCode::UnsupportedSyntax));
}
void mysqlWindowArgumentProductions() {
  const std::vector<std::pair<Dialect,std::string>> valid={
    {Dialect::MySQL,"SELECT NTILE(2) OVER(ORDER BY ABS(17)),NTH_VALUE(17,(1+1)) OVER(ORDER BY ABS(17)),LAG(17,1,0) OVER(ORDER BY ABS(17));"},
    {Dialect::MariaDB,"SELECT NTILE(1+1) OVER(ORDER BY ABS(17)),NTH_VALUE(17,1+1) OVER(ORDER BY ABS(17)),LAG(17,1+1) OVER(ORDER BY ABS(17));"},
    {Dialect::TiDB,"SELECT NTILE((1+1)) OVER(ORDER BY ABS(17)),NTH_VALUE(17,(1+1)) OVER(ORDER BY ABS(17)),LAG(17,1,0) OVER(ORDER BY ABS(17));"},
    {Dialect::OceanBase,"SELECT NTILE(1+1) OVER(ORDER BY ABS(17)),NTH_VALUE(17,(1+1)) OVER(ORDER BY ABS(17)),LAG(17,1+1,0) OVER(ORDER BY ABS(17));"}};
  for (const auto &[dialect,sql]:valid) {
    const auto parsed=compile(dialect,sql);
    success(*parsed);
    const auto formatted=formatSql(dialect,sql);
    CHECK(formatted.ok());
    success(*compile(dialect,formatted.sql));
    CHECK(formatSql(dialect,formatted.sql).sql==formatted.sql);
  }
  for (const auto sql:{"SELECT NTILE(NULL) OVER();","SELECT NTILE((1)) OVER();",
                       "SELECT LAG(17,-1) OVER();","SELECT LEAD(17,1.5) OVER();"})
    CHECK(!compile(Dialect::MySQL,sql)->syntaxValid);
  CHECK(!compile(Dialect::TiDB,"SELECT NTILE(1+1) OVER();")->syntaxValid);
  CHECK(!compile(Dialect::TiDB,"SELECT LAG(17,@step) OVER();")->syntaxValid);
  CHECK(!compile(Dialect::MariaDB,"SELECT LAG(17,1,0) OVER();")->syntaxValid);
  NameResolutionContext context;
  context.dialect=Dialect::MySQL;
  context.localPolicy=LocalNamePolicy::PreferColumns;
  context.locals.push_back({{"bucket_count"},17,ValueType::Integer});
  const std::string query="SELECT NTILE(bucket_count) OVER(ORDER BY item_id) FROM (SELECT 2 AS bucket_count,17 AS item_id) AS audit_items;";
  const auto bound=compile(Dialect::MySQL,query,{},nullptr,context);
  success(*bound);
  const auto &model=bound->model.front();
  const auto &window=projection(*bound);
  CHECK(window.kind==ExprKind::Window);
  const auto &call=model.expressions.at(window.operands.front());
  const auto &argument=model.expressions.at(call.operands.front());
  CHECK(argument.localSymbol==17 && !argument.binding);
  const auto undeclared=compile(Dialect::MySQL,query);
  CHECK(undeclared->syntaxValid && has(*undeclared,DiagnosticCode::UnknownColumn));
  for (const auto dialect:{Dialect::MySQL,Dialect::MariaDB,Dialect::TiDB}) {
    const auto zero=compile(dialect,"SELECT NTILE(0) OVER(ORDER BY ABS(17));");
    CHECK(zero->syntaxValid && has(*zero,DiagnosticCode::InvalidQueryShape));
  }
  const auto nullable=compile(Dialect::TiDB,"SELECT NTILE(NULL) OVER(ORDER BY ABS(17));");
  success(*nullable);
  CHECK(nullable->model.types.get(projection(*nullable).sqlType).nullability==Nullability::Nullable);
}
void mysqlDeleteProductions() {
  for (const auto dialect:{Dialect::MySQL,Dialect::MariaDB,Dialect::TiDB,Dialect::OceanBase}) {
    const auto wildcard=compile(dialect,"DELETE FROM item.* USING audit_other other JOIN audit_items item ON item.item_id=other.item_id;");
    success(*wildcard);
    CHECK(wildcard->model.front().deleteTargets==std::vector<std::vector<std::string>>{{"item"}});
    CHECK(wildcard->statements.front().targetInTables);
    CHECK(wildcard->model.front().targetTable==std::optional<std::size_t>(1));
    const auto hiddenTarget=compile(dialect,"DELETE FROM audit_items USING audit_items AS item;");
    CHECK(hiddenTarget->syntaxValid && has(*hiddenTarget,DiagnosticCode::UnknownTable));
    const auto generated=generateSql(wildcard->model,dialect);
    CHECK(generated.ok());
    success(*compile(dialect,generated.sql));
    for (const auto sql:{"DELETE FROM audit_items AS item USING audit_items item;",
                        "DELETE FROM audit_items item,audit_other USING audit_items,audit_other;",
                        "DELETE FROM audit_items.* WHERE item_id=17;",
                        "DELETE FROM (SELECT 17 AS item_id) item;"}) {
      const auto bad=compile(dialect,sql);
      CHECK(!bad->syntaxValid && has(*bad,DiagnosticCode::ExpectedToken));
    }
  }
  for (const auto dialect:{Dialect::MySQL,Dialect::MariaDB}) {
    const auto flags=compile(dialect,"DELETE IGNORE QUICK LOW_PRIORITY QUICK IGNORE FROM audit_items WHERE item_id=17;");
    success(*flags);
    CHECK(flags->model.front().modifiers==std::vector<std::string>({"LOW_PRIORITY","QUICK","IGNORE"}));
    const auto generated=generateSql(flags->model,dialect);
    CHECK(generated.ok());
    success(*compile(dialect,generated.sql));
    CHECK(generated.sql.find("DELETE LOW_PRIORITY QUICK IGNORE ")==0);
  }
  const auto ocean=compile(Dialect::OceanBase,"DELETE IGNORE QUICK LOW_PRIORITY QUICK FROM audit_items;");
  success(*ocean);
  const auto oceanSql=generateSql(ocean->model,Dialect::OceanBase);
  CHECK(oceanSql.ok() && oceanSql.sql.find("DELETE IGNORE LOW_PRIORITY QUICK ")==0);
  success(*compile(Dialect::OceanBase,oceanSql.sql));
  for (const auto source:{Dialect::MySQL,Dialect::TiDB,Dialect::OceanBase}) {
    const auto alias=compile(source,"DELETE FROM audit_items AS item WHERE item.item_id=17;");
    success(*alias);
  }
  const auto maria=compile(Dialect::MariaDB,"DELETE FROM audit_items AS item WHERE item.item_id=17;");
  CHECK(!maria->syntaxValid && has(*maria,DiagnosticCode::ExpectedToken));
  CHECK(!compile(Dialect::TiDB,"DELETE QUICK LOW_PRIORITY FROM audit_items;")->syntaxValid);
  const auto missingQuick=compile(Dialect::TiDB,"DELETE QUICK QUICK FROM audit_items;");
  CHECK(missingQuick->syntaxValid && has(*missingQuick,DiagnosticCode::UnknownTable));
  const auto quickTarget=compile(Dialect::TiDB,"DELETE QUICK QUICK FROM audit_items AS QUICK;");
  success(*quickTarget);
  CHECK(quickTarget->model.front().modifiers==std::vector<std::string>{"QUICK"});
  CHECK(quickTarget->model.front().deleteTargets.size()==1);
}
void mysqlTemporalGrammar() {
  for (auto d : {Dialect::MySQL,Dialect::MariaDB,Dialect::TiDB,Dialect::OceanBase}) {
    const auto parsed=compile(d,"SELECT CURDATE(),UTC_DATE,CURTIME(3),UTC_TIME,UTC_TIMESTAMP(6),NOW();");
    success(*parsed);
    const auto &types=parsed->model.types;
    CHECK(types.get(projection(*parsed,0).sqlType).family==TypeFamily::Date);
    CHECK(types.get(projection(*parsed,1).sqlType).nullability==Nullability::NotNull);
    const auto &time=types.get(projection(*parsed,2).sqlType);
    CHECK(time.family==TypeFamily::Time && time.precision==3 && time.timeZone==TimeZoneKind::Without);
    CHECK(types.get(projection(*parsed,3).sqlType).precision==0);
    const auto &timestamp=types.get(projection(*parsed,4).sqlType);
    CHECK(timestamp.family==TypeFamily::Timestamp && timestamp.precision==6 && timestamp.flavor=="datetime");
    const auto generated=generateSql(parsed->model,d);
    CHECK(generated.ok());
    const auto reparsed=compile(d,generated.sql);
    success(*reparsed);
    CHECK(reparsed->model.types.get(projection(*reparsed,4).sqlType)==timestamp);
    const auto columns=compile(d,"SELECT t.curtime,`curtime` FROM audit_clock t;");
    success(*columns);
    CHECK(projection(*columns).kind==ExprKind::Name && projection(*columns,1).kind==ExprKind::Name);
    const auto bare=compile(d,"SELECT curdate,curtime,now FROM audit_clock;");
    success(*bare);
    CHECK(projection(*bare).kind==ExprKind::Name && projection(*bare,1).kind==ExprKind::Name && projection(*bare,2).kind==ExprKind::Name);
    const auto badLiteral=compile(d,"SELECT UTC_TIMESTAMP('3');");
    CHECK(!badLiteral->syntaxValid && has(*badLiteral,DiagnosticCode::ExpectedToken));
    const auto badRange=compile(d,"SELECT UTC_TIMESTAMP(7);");
    CHECK(badRange->syntaxValid && has(*badRange,DiagnosticCode::InvalidQueryShape));
  }
}
void upstreamGrammarBoundaries() {
  for (auto d : {Dialect::MySQL,Dialect::MariaDB,Dialect::TiDB,Dialect::OceanBase}) {
    const auto xorQuery=compile(d,"SELECT 1 XOR 0 AND 1,1 OR 0 XOR 1,NULL XOR 1;");
    success(*xorQuery);
    const auto &xq=xorQuery->model.front();
    CHECK(projection(*xorQuery).value=="XOR");
    CHECK(xq.expressions.at(projection(*xorQuery).operands[1]).value=="AND");
    CHECK(projection(*xorQuery,1).value=="OR");
    CHECK(xq.expressions.at(projection(*xorQuery,1).operands[1]).value=="XOR");
    CHECK(xorQuery->model.types.get(projection(*xorQuery,2).sqlType).nullability==Nullability::Nullable);
    const auto xorSql=generateSql(xorQuery->model,d);
    CHECK(xorSql.ok());
    success(*compile(d,xorSql.sql));
    CHECK(!generateSql(xorQuery->model,Dialect::PostgreSQL).ok());
    for (const auto *query : {"COMMIT AND CHAIN RELEASE;", "ROLLBACK AND CHAIN RELEASE;",
                              "INSERT INTO t(id) SET id=17;", "SELECT + NOT EXISTS(SELECT 17);"}) {
      const auto rejected=compile(d,query);
      CHECK(!rejected->syntaxValid && has(*rejected,DiagnosticCode::ExpectedToken));
    }
    const auto mismatch=compile(d,"INSERT INTO t(id) VALUES();");
    CHECK(mismatch->syntaxValid && has(*mismatch,DiagnosticCode::InvalidQueryShape));
    const auto transaction=compile(d,"COMMIT AND CHAIN NO RELEASE;");
    success(*transaction);
    auto corrupt=transaction->model;
    corrupt.queries.front().transaction->release=TransactionEndOption::Yes;
    const auto rejected=generateSql(corrupt,d);
    CHECK(!rejected.ok() && rejected.sql.empty());
  }
  const auto natural=compile(Dialect::TiDB,"SELECT * FROM t NATURAL JOIN u;");
  success(*natural);
  const auto naturalSql=generateSql(natural->model,Dialect::TiDB);
  CHECK(naturalSql.ok() && naturalSql.sql.find("NATURAL INNER")==std::string::npos);
  success(*compile(Dialect::TiDB,naturalSql.sql));
  const auto empty=compile(Dialect::PostgreSQL,"SELECT FROM (SELECT 17 AS id) AS q;");
  success(*empty);
  CHECK(empty->model.front().projections.empty());
  const auto emptySql=generateSql(empty->model,Dialect::PostgreSQL);
  CHECK(emptySql.ok());
  success(*compile(Dialect::PostgreSQL,emptySql.sql));
  CHECK(!generateSql(empty->model,Dialect::MySQL).ok());
  const auto tsql=compile(Dialect::SQLServer,"SELECT id FROM t WHERE NOT label LIKE 'item%' AND id=17;");
  success(*tsql);
  const auto &q=tsql->model.front();
  const auto &condition=q.expressions.at(q.where);
  CHECK(condition.kind==ExprKind::Binary && condition.value=="AND");
  const auto &negation=q.expressions.at(condition.operands.front());
  CHECK(negation.kind==ExprKind::Unary && negation.value=="NOT");
  CHECK(q.expressions.at(negation.operands.front()).value=="LIKE");
  const auto generated=generateSql(tsql->model,Dialect::SQLServer);
  CHECK(generated.ok());
  success(*compile(Dialect::SQLServer,generated.sql));
}
void explicitLikeEscape() {
  for (auto dialect : {Dialect::MySQL,Dialect::PostgreSQL,Dialect::SQLServer,Dialect::Oracle,
                        Dialect::SQLite,Dialect::MariaDB,Dialect::TiDB,Dialect::OceanBase}) {
    const auto from = dialect==Dialect::Oracle ? " FROM dual" : "";
    const auto parsed=compile(dialect,std::string("SELECT 1")+from+
        " WHERE 'a_b' NOT LIKE 'a!_b' ESCAPE '!';");
    success(*parsed);
    const auto &query=parsed->model.front();
    const auto &pattern=query.expressions.at(query.where);
    CHECK(pattern.kind==ExprKind::LikeEscape && pattern.operands.size()==3);
    CHECK(pattern.value=="NOT LIKE" && pattern.type==ValueType::Boolean);
    CHECK(query.expressions.at(pattern.operands[2]).value=="!");
    CHECK(parsed->source.slice(query.expressions.at(pattern.operands[2]).origin)=="'!'");
    const auto generated=generateSql(parsed->model,dialect);
    CHECK(generated.ok() && generated.sql.find(" ESCAPE ")!=std::string::npos);
    success(*compile(dialect,generated.sql));
    auto malformed=parsed->model;
    malformed.queries.front().expressions.at(query.where).operands.pop_back();
    const auto rejected=generateSql(malformed,dialect);
    CHECK(!rejected.ok() && rejected.sql.empty());
  }
  const auto pg=compile(Dialect::PostgreSQL,"SELECT 'A_B' NOT ILIKE 'a!_b' ESCAPE '!';");
  success(*pg);
  CHECK(projection(*pg).kind==ExprKind::LikeEscape && projection(*pg).value=="NOT ILIKE");
  CHECK(!generateSql(pg->model,Dialect::SQLite).ok());
  LanguageOptions noBackslash;
  noBackslash.mysqlNoBackslashEscapes=true;
  const auto empty=compile(Dialect::MySQL,"SELECT 'a_b' LIKE 'a_b' ESCAPE '';",noBackslash);
  CHECK(empty->syntaxValid && !empty->ok() && has(*empty,DiagnosticCode::InvalidQueryShape));
  const auto sqlite=compile(Dialect::SQLite,"SELECT 'a_b' LIKE 'a§_b' ESCAPE '§';");
  success(*sqlite);
  CHECK(dumpAST(*sqlite).find("likeEscape")!=std::string::npos);
  const auto nullEscape=compile(Dialect::SQLite,"SELECT 'a_b' LIKE 'a_b' ESCAPE NULL;");
  success(*nullEscape);
  CHECK(nullEscape->model.types.get(projection(*nullEscape).sqlType).nullability==Nullability::Nullable);
  const auto mysqlNull=compile(Dialect::MySQL,"SELECT 'a_b' LIKE 'a_b' ESCAPE NULL;");
  success(*mysqlNull);
  CHECK(mysqlNull->model.types.get(projection(*mysqlNull).sqlType).nullability==Nullability::NotNull);
  CHECK(!generateSql(mysqlNull->model,Dialect::SQLite).ok());
  const auto oracleEmpty=compile(Dialect::Oracle,"SELECT 1 FROM dual WHERE 'a_b' LIKE 'a_b' ESCAPE '';");
  success(*oracleEmpty);
  const auto &oracleQuery=oracleEmpty->model.front();
  CHECK(oracleEmpty->model.types.get(oracleQuery.expressions.at(oracleQuery.where).sqlType).nullability==Nullability::Nullable);
  CHECK(!generateSql(oracleEmpty->model,Dialect::PostgreSQL).ok());
}
void sqliteEmptyInAndNullTests() {
  const auto d = Dialect::SQLite;
  for (const auto *query : {"SELECT NULL IN ();", "SELECT NULL NOT IN ();"}) {
    const auto parsed = compile(d, query);
    success(*parsed);
    const auto &predicate = projection(*parsed);
    CHECK(predicate.kind == ExprKind::In && predicate.operands.size() == 1);
    CHECK(parsed->model.types.get(predicate.sqlType).nullability == Nullability::NotNull);
    const auto generated = generateSql(parsed->model, d);
    CHECK(generated.ok());
    success(*compile(d, generated.sql));
    for (auto other : {Dialect::MySQL, Dialect::PostgreSQL, Dialect::Oracle,
                       Dialect::SQLServer, Dialect::MariaDB, Dialect::TiDB, Dialect::OceanBase}) {
      CHECK(!compile(other, other == Dialect::Oracle ?
          std::string(query).substr(0, std::string(query).size() - 1) + " FROM dual;" : query)->syntaxValid);
      const auto foreign = generateSql(parsed->model, other);
      CHECK(!foreign.ok() && foreign.sql.empty());
    }
  }
  for (const auto *query : {"SELECT NULL ISNULL;", "SELECT 1 NOTNULL;", "SELECT 1 NOT NULL;"}) {
    const auto parsed = compile(d, query);
    success(*parsed);
    CHECK(projection(*parsed).kind == ExprKind::IsNull);
    const auto generated = generateSql(parsed->model, d);
    CHECK(generated.ok());
    success(*compile(d, generated.sql));
  }
  const auto precedence = compile(d, "SELECT 3 + NULL ISNULL, NULL ISNULL AND 1 NOTNULL;");
  success(*precedence);
  CHECK(projection(*precedence).kind == ExprKind::IsNull);
  CHECK(precedence->model.front().expressions.at(projection(*precedence).operands[0]).value == "+");
  CHECK(projection(*precedence, 1).value == "AND");
  const auto derived = compile(d, "SELECT item_id FROM (SELECT 17 AS item_id);");
  success(*derived);
  CHECK(derived->model.front().tables.front().alias.empty());
  CHECK(projection(*derived).binding.has_value());
  const auto generated = generateSql(derived->model, d);
  CHECK(generated.ok());
  success(*compile(d, generated.sql));
  const auto foreign = generateSql(derived->model, Dialect::MySQL);
  CHECK(!foreign.ok() && foreign.sql.empty());
}
void postgresOperatorTokens() {
  constexpr auto pg = Dialect::PostgreSQL;
  const auto builtin = compile(pg, "SELECT 2*-3,2<=-3,2+/* gap */3,2+-- gap\n3,2% -3;");
  success(*builtin);
  const auto custom = compile(pg, "SELECT 2%-3;");
  CHECK(!custom->ok());
  CHECK(std::any_of(custom->tokens.begin(), custom->tokens.end(), [](const auto &token) {
    return token.kind == TokenKind::Symbol && token.value == "%-" &&
           token.range.begin == 8 && token.range.end == 10;
  }));
  CHECK(!compile(pg, "SELECT 2!=-3;")->ok());
  CHECK(!compile(pg, "SELECT 2+-*3;")->ok());
  success(*compile(Dialect::SQLite, "SELECT 2%-3;"));
}
void distributedDdl() {
  const auto tidb = Dialect::TiDB, ob = Dialect::OceanBase;
  const auto ti = compile(tidb, "CREATE TABLE a(id BIGINT /*T![auto_rand] AUTO_RANDOM(7,60) */, "
                           "PRIMARY KEY(id) /*T![clustered_index] CLUSTERED */) /*T! PRE_SPLIT_REGIONS=4 */;");
  success(*ti);
  CHECK(ti->statements.front().ddl->definitions.front().autoRandom->shardBits == 7);
  CHECK(ti->model.front().ddl->definitions.front().autoRandom->rangeBits == 60);
  CHECK(ti->model.front().ddl->constraints.front().clustering == PrimaryKeyClustering::Clustered);
  CHECK(std::get<std::uint64_t>(ti->model.front().ddl->distribution.front().value) == 4);
  CHECK(ti->source.slice(ti->model.front().ddl->definitions.front().autoRandom->origin) == "AUTO_RANDOM(7,60)");
  const auto nativeTi = generateSql(ti->model, tidb);
  CHECK(nativeTi.ok() && nativeTi.sql.find("AUTO_RANDOM(7, 60)") != std::string::npos);
  success(*compile(tidb, nativeTi.sql));
  CHECK(!generateSql(ti->model, Dialect::MySQL).ok());
  const auto views = compile(ob, "CREATE TABLEGROUP g SHARDING='NONE'; ALTER TABLE t SET TABLEGROUP=''; "
                                 "CREATE INDEX i ON t(id) GLOBAL; SHOW CREATE TABLEGROUP g;");
  success(*views);
  CHECK(views->model.at(0).ddl->object == ObjectKind::TableGroup);
  CHECK(std::holds_alternative<std::monostate>(views->model.at(1).ddl->distribution.front().value));
  CHECK(views->model.at(2).ddl->indexScope == IndexScope::Global);
  const auto nativeOb = generateSql(views->model, ob);
  CHECK(nativeOb.ok());
  success(*compile(ob, nativeOb.sql));
  CHECK(!generateSql(views->model, Dialect::MySQL).ok());
  const auto inspection = compile(tidb, "ADMIN SHOW DDL JOBS 3 WHERE JOB_ID > 1;");
  success(*inspection);
  CHECK(inspection->model.front().inspection->jobCount == 3);
  CHECK(generateSql(inspection->model, tidb).ok());
  CHECK(projection(*compile(tidb, "SELECT TIDB_VERSION();")).type == ValueType::String);
}
void dialects() {
  postgresOperatorTokens();
  distributedDdl();
  for (auto d :
       {Dialect::MySQL, Dialect::PostgreSQL, Dialect::Oracle, Dialect::SQLite,
        Dialect::SQLServer, Dialect::MariaDB, Dialect::OceanBase, Dialect::TiDB}) {
    CHECK(parseDialect(dialectName(d)) == d);
    auto c = compile(
        d,
        "SELECT t.id, 1 + 2 * 3 AS score FROM items t WHERE t.id BETWEEN 1 AND "
        "5 ORDER BY score DESC;");
    success(*c);
    CHECK(c->statements.size() == 1);
    CHECK(c->model[0].scope.tables.size() == 1);
    CHECK(!c->model[0].catalogChecked);
    auto ddl = compile(d, "CREATE TABLE t (id int)");
    success(*ddl);
    CHECK(ddl->model[0].kind == StatementKind::Create);
    CHECK(ddl->model[0].ddl.has_value());
  }
  success(*compile(Dialect::PostgreSQL,"SELECT value,t.select FROM t;"));
  success(*compile(Dialect::MySQL,"SELECT t.from,t.order FROM t;"));
  CHECK(!compile(Dialect::PostgreSQL,"SELECT t. FROM t;")->ok());
  CHECK(!compile(Dialect::Oracle,"SELECT t.FROM FROM t;")->ok());
  CHECK(!parseDialect("other"));
  {
    const auto pg = Dialect::PostgreSQL, my = Dialect::MySQL, ms = Dialect::SQLServer;
    const auto partial = compile(pg, "SELECT d.renamed,d.keep FROM (SELECT 1 AS old,2 AS keep) AS d(renamed);");
    success(*partial);
    CHECK(partial->model.front().tables.front().columnAliases == std::vector<std::string>{"renamed"});
    CHECK(projection(*partial,0).binding && projection(*partial,0).binding->column == 0);
    CHECK(projection(*partial,1).binding && projection(*partial,1).binding->column == 1);
    CHECK(has(*compile(pg, "SELECT d.old FROM (SELECT 1 AS old,2 AS keep) AS d(renamed);"), DiagnosticCode::UnknownColumn));
    CHECK(has(*compile(pg, "SELECT d.x FROM (SELECT 1) AS d(x,y);"), DiagnosticCode::InvalidQueryShape));
    success(*compile(pg, "SELECT 1 FROM (SELECT 1,2) AS d(x,x);"));
    CHECK(has(*compile(pg, "SELECT d.x FROM (SELECT 1,2) AS d(x,x);"), DiagnosticCode::AmbiguousColumn));
    for (const auto d : {my, ms}) {
      const auto all = compile(d, "SELECT d.x,d.y FROM (SELECT 1 AS a,2 AS b) AS d(x,y);");
      success(*all);
      CHECK(projection(*all,1).binding && projection(*all,1).binding->column == 1);
      const auto generated = generateSql(all->model, d);
      CHECK(generated.ok());
      success(*compile(d, generated.sql));
      CHECK(has(*compile(d, "SELECT x FROM (SELECT 1 AS a,2 AS b) AS d(x);"), DiagnosticCode::InvalidQueryShape));
      CHECK(has(*compile(d, "SELECT 1 FROM (SELECT 1,2) AS d(x,x);"), DiagnosticCode::DuplicateAlias));
    }
    // Unknown '*' arity cannot assign a typed later expression to the first alias.
    const auto offline = compile(my, "SELECT d.x FROM (SELECT *, 1 AS later FROM unavailable) AS d(x,y);");
    success(*offline);
    CHECK(projection(*offline).binding && projection(*offline).type == ValueType::Unknown);

    const auto constructor = compile(ms,
        "SELECT d.id,d.n FROM (VALUES (1,4),(2,NULL)) AS d(id,n);");
    success(*constructor);
    const auto id = constructor->model.front().tables.front().query;
    CHECK(constructor->model.queries.at(id - 1).sqlServerValueConstructor);
    CHECK(projection(*constructor,1).binding && projection(*constructor,1).binding->column == 1);
    CHECK(!compile(ms, "VALUES (1),(2);")->ok());
    CHECK(!compile(ms, "SELECT d.x FROM (VALUES (1)) AS d;")->ok());
    CHECK(!compile(ms, "SELECT d.x FROM (VALUES (1,2),(3)) AS d(x,y);")->ok());
    const auto lateral = compile(ms,
        "SELECT r.n FROM (SELECT 1 AS id) l CROSS APPLY (VALUES (l.id),(l.id+1)) r(n);");
    success(*lateral);
    const auto &nested = lateral->model.queries.at(lateral->model.front().tables.at(1).query - 1);
    CHECK(nested.scope.parent == lateral->model.front().id);
    CHECK(nested.expressions.at(nested.rows.at(0).at(0)).binding);
    const auto generated = generateSql(lateral->model, ms);
    CHECK(generated.ok());
    success(*compile(ms, generated.sql));
  }
  for (const auto dialect : {Dialect::MySQL, Dialect::PostgreSQL}) {
    for (const auto *query : {
             "TABLE items ORDER BY id LIMIT 5;",
             "TABLE items UNION ALL TABLE archived_items;",
             "SELECT id FROM items WHERE id IN (TABLE archived_items);",
             "INSERT INTO items TABLE archived_items;"}) {
      const auto compiled = compile(dialect, query);
      success(*compiled);
      CHECK(std::any_of(compiled->model.begin(), compiled->model.end(),
                        [](const auto &q) { return q.tableQuery == TableQuerySyntax::Table; }));
      const auto generated = generateSql(compiled->model, dialect);
      CHECK(generated.ok());
      success(*compile(dialect, generated.sql));
    }
    const auto bad = compile(dialect, "TABLE items WHERE id=1;");
    CHECK(!bad->syntaxValid && has(*bad, DiagnosticCode::ExpectedToken));
  }
  auto only = compile(Dialect::PostgreSQL, "TABLE ONLY (public.items) ORDER BY id;");
  success(*only);
  CHECK(only->model.front().tableQuery == TableQuerySyntax::Only);
  const auto explicitDescendants = compile(Dialect::PostgreSQL, "TABLE public.items *;");
  success(*explicitDescendants);
  CHECK(explicitDescendants->model.front().tableQuery == TableQuerySyntax::IncludeDescendants);
  CHECK(has(*compile(Dialect::SQLite, "TABLE items;"), DiagnosticCode::UnsupportedSyntax));
  for (const auto dialect : {Dialect::MySQL, Dialect::PostgreSQL, Dialect::SQLite}) {
    const bool mysql = dialect == Dialect::MySQL;
    const auto sql = mysql ? "VALUES ROW(1,'a'), ROW(2,'b');" : "VALUES (1,'a'), (2,'b');";
    const auto values = compile(dialect, sql);
    success(*values);
    CHECK(values->model.front().valuesQuery && values->model.front().rows.size() == 2);
    CHECK(values->model.front().projections.front().alias == (mysql ? "column_0" : "column1"));
    CHECK(values->model.types.get(projection(*values).sqlType).family == TypeFamily::Integer);
    const auto emitted = generateSql(values->model, dialect);
    CHECK(emitted.ok());
    success(*compile(dialect, emitted.sql));
  }
  const auto unequal = compile(Dialect::PostgreSQL, "VALUES (1), (2,3);");
  CHECK(unequal->syntaxValid && has(*unequal, DiagnosticCode::InvalidQueryShape));
  CHECK(!compile(Dialect::SQLite, "VALUES (1),(2) ORDER BY 1;")->syntaxValid);
  CHECK(!compile(Dialect::SQLite, "SELECT 1 UNION ALL VALUES (2) LIMIT 1;")->syntaxValid);
  auto derivedValues = compile(Dialect::PostgreSQL, "SELECT v.column1 FROM (VALUES(1), (2)) v;");
  success(*derivedValues);
  CHECK(projection(*derivedValues).binding.has_value());
  CHECK(derivedValues->model.types.get(projection(*derivedValues).sqlType).family == TypeFamily::Integer);
  success(*compile(Dialect::PostgreSQL, "WITH RECURSIVE s(n) AS (VALUES (1) UNION ALL "
                                      "SELECT n+1 FROM s WHERE n<3) SELECT n FROM s;"));
  const auto forward = compile(Dialect::PostgreSQL,
      "WITH RECURSIVE a(v) AS (SELECT (SELECT k FROM b) AS v), b(k) AS (SELECT 7) SELECT v FROM a;");
  success(*forward);
  const auto &orderedCtes=forward->model.front().ctes;
  CHECK(orderedCtes.size()==2 && orderedCtes[0].name=="a" && orderedCtes[0].query>orderedCtes[1].query);
  CHECK(projection(*forward).binding);
  CHECK(has(*compile(Dialect::PostgreSQL,
      "WITH RECURSIVE a AS (SELECT * FROM b),b AS (SELECT * FROM a) SELECT * FROM a;"),
      DiagnosticCode::UnsupportedSyntax));
  success(*compile(Dialect::PostgreSQL,
      "WITH RECURSIVE a(v) AS (WITH b(v) AS (SELECT 4) SELECT v FROM b), "
      "b(v) AS (SELECT v FROM a) SELECT v FROM b;"));
  const auto partial = compile(Dialect::PostgreSQL,
      "WITH s(a) AS (SELECT 1 AS old,2 AS kept) SELECT s.a,s.kept FROM s;");
  success(*partial);
  const auto &partialCte = partial->model.front().ctes.front();
  CHECK(partialCte.columns == std::vector<std::string>{"a"} && partialCte.definition &&
        partialCte.definition->arityKnown && partialCte.definition->columns.size() == 2);
  CHECK(projection(*partial, 1).binding && projection(*partial, 1).binding->column == 1);
  const auto unknown = compile(Dialect::PostgreSQL,
      "WITH s(a) AS (SELECT * FROM offline_table) SELECT s.a,s.unknown_remainder FROM s;");
  success(*unknown);
  const auto &unknownCte = unknown->model.front().ctes.front();
  CHECK(!unknownCte.definition->arityKnown && unknownCte.definition->columns.front().name == "a");
  CHECK(!projection(*unknown).binding && !projection(*unknown, 1).binding);
  CHECK(has(*compile(Dialect::PostgreSQL, "WITH s(a,b) AS (SELECT 1) SELECT a FROM s;"),
            DiagnosticCode::InvalidQueryShape));
  CHECK(has(*compile(Dialect::MySQL, "WITH s(a) AS (SELECT 1,2 AS kept) SELECT a FROM s;"),
            DiagnosticCode::InvalidQueryShape));
  const auto keywordAlias = compile(Dialect::PostgreSQL,
      "SELECT sub.select FROM (SELECT 1 AS select) sub;");
  success(*keywordAlias);
  CHECK(projection(*keywordAlias).binding);
  for (const auto &[dialect, sql] : std::vector<std::pair<Dialect, const char *>>{
      {Dialect::PostgreSQL, "SELECT value FROM t ORDER BY value OFFSET 1 ROW FETCH NEXT 2 ROWS WITH TIES;"},
      {Dialect::PostgreSQL, "SELECT value FROM t ORDER BY value FETCH FIRST ROW WITH TIES OFFSET 1;"},
      {Dialect::Oracle, "SELECT n FROM t ORDER BY n FETCH FIRST 2 ROWS WITH TIES;"},
      {Dialect::Oracle, "SELECT n FROM t FETCH FIRST 2 ROWS WITH TIES;"},
      {Dialect::SQLServer, "SELECT TOP (2) WITH TIES n FROM t ORDER BY n;"}}) {
    const auto limited = compile(dialect, sql);
    success(*limited);
    CHECK(limited->model.front().limitWithTies);
    const auto generated = generateSql(limited->model, dialect);
    CHECK(generated.ok());
    const auto roundTrip = compile(dialect, generated.sql);
    success(*roundTrip);
    CHECK(roundTrip->model.front().limitWithTies);
  }
  success(*compile(Dialect::PostgreSQL, "SELECT 1 FETCH FIRST ROW ONLY;"));
  CHECK(!compile(Dialect::PostgreSQL, "SELECT 1 FETCH FIRST ROW WITH TIES;")->ok());
  CHECK(!compile(Dialect::PostgreSQL, "SELECT 1 OFFSET 1 LIMIT ALL FETCH FIRST ROW ONLY;")->ok());
  CHECK(!compile(Dialect::SQLServer, "SELECT TOP (2) WITH TIES n FROM t;")->ok());
  CHECK(!compile(Dialect::SQLServer, "SELECT n FROM t ORDER BY n OFFSET 1 ROW FETCH NEXT 2 ROWS WITH TIES;")->ok());
  MemoryCatalog applyCatalog;
  TableDefinition items;
  items.name = "items";
  items.columns = {{"id", ValueType::Integer}};
  applyCatalog.tables.push_back(items);
  for (auto keyword : {"CROSS APPLY", "OUTER APPLY"}) {
    const auto sql = "SELECT a.id, r.n FROM items a " + std::string(keyword) +
                     " (SELECT a.id AS n) r;";
    const auto apply = compile(Dialect::SQLServer, sql, {}, &applyCatalog);
    success(*apply);
    const auto &root = apply->model.front();
    CHECK(root.relations.back().lateral);
    const auto &right = apply->model.at(root.tables.at(1).query - 1);
    CHECK(right.scope.parent == root.id && right.scope.parentTables == std::vector<std::size_t>{0});
    const auto &binding = right.expressions[right.projections.front().expression].binding;
    CHECK(binding && binding->scope == root.id && binding->table == 0);
    if (std::string(keyword) == "OUTER APPLY")
      CHECK(apply->model.types.get(projection(*apply, 1).sqlType).nullability == Nullability::Nullable);
    const auto emitted = generateSql(apply->model, Dialect::SQLServer);
    CHECK(emitted.ok() && emitted.sql.find(keyword) != std::string::npos);
    success(*compile(Dialect::SQLServer, emitted.sql, {}, &applyCatalog));
  }
  const auto outOfScope = compile(Dialect::SQLServer,
      "SELECT r.n FROM items a CROSS APPLY (SELECT b.id AS n) r JOIN items b ON a.id=b.id;",
      {}, &applyCatalog);
  CHECK(outOfScope->syntaxValid && has(*outOfScope, DiagnosticCode::UnknownColumn));
  CHECK(!compile(Dialect::SQLServer,
                 "SELECT r.n FROM items a CROSS APPLY (SELECT 1 AS n) r ON 1=1;")->syntaxValid);
  const auto lateral = compile(Dialect::PostgreSQL,
      "SELECT r.n FROM items a LEFT JOIN LATERAL (SELECT a.id AS n) r ON TRUE;", {}, &applyCatalog);
  success(*lateral);
  CHECK(lateral->statements.front().relations.back().condition != 0);
  CHECK(lateral->model.front().relations.back().lateral && lateral->model.front().relations.back().condition == 0);
  CHECK(has(*compile(Dialect::PostgreSQL,
      "SELECT r.n FROM items a LEFT JOIN LATERAL (SELECT a.id AS n) r ON r.n>0;", {}, &applyCatalog),
      DiagnosticCode::UnsupportedSyntax));
  success(*compile(Dialect::Oracle,
      "SELECT r.n FROM items a CROSS APPLY (SELECT a.id AS n FROM dual) r;"));
}
void precedence() {
  auto c = compile(Dialect::PostgreSQL,
                   "SELECT 1+2*3, -2+4, NOT a=1 AND b=2 OR c=3 FROM t");
  success(*c);
  const auto &m = c->model[0];
  CHECK(projection(*c).value == "+");
  CHECK(m.expressions[projection(*c).operands[1]].value == "*");
  CHECK(projection(*c, 2).value == "OR");
  const auto &andExpr = m.expressions[projection(*c, 2).operands[0]];
  CHECK(andExpr.value == "AND");
  CHECK(m.expressions[andExpr.operands[0]].value == "NOT");
  auto scientific = compile(Dialect::MySQL, "SELECT 1e-3 + .5");
  success(*scientific);
  CHECK(scientific->model[0].expressions[1].value == "1e-3");
  CHECK(scientific->model[0].expressions[1].type == ValueType::Decimal);
}
void mysql() {
  auto c =
      compile(Dialect::MySQL, "SELECT `x`, \"a\", 'b\\n' FROM `t` LIMIT 4, 10");
  success(*c);
  CHECK(projection(*c).name[0] == "x");
  CHECK(projection(*c, 1).kind == ExprKind::String);
  CHECK(projection(*c, 2).value == "b\n");
  const auto &m = c->model[0];
  CHECK(m.expressions[m.limit].value == "10");
  CHECK(m.expressions[m.offset].value == "4");
  auto logical = compile(Dialect::MySQL, "SELECT a || b && c FROM t");
  success(*logical);
  CHECK(projection(*logical).value == "OR");
  LanguageOptions options;
  options.mysqlAnsiQuotes = true;
  options.mysqlPipesAsConcat = true;
  options.mysqlNoBackslashEscapes = true;
  auto modes =
      compile(Dialect::MySQL, "SELECT \"x\" || 'b\\n' FROM t", options);
  success(*modes);
  CHECK(projection(*modes).value == "CONCAT");
  CHECK(modes->model[0].expressions[2].value == "b\\n");
  auto unaryConcat = compile(Dialect::MySQL, "SELECT -a || b FROM t", options);
  success(*unaryConcat);
  CHECK(projection(*unaryConcat).value == "CONCAT");
  auto between = compile(Dialect::MySQL, "SELECT a BETWEEN 1 AND b=3 FROM t");
  success(*between);
  CHECK(projection(*between).value == "=");
  CHECK(between->model[0].expressions[projection(*between).operands[0]].kind ==
        ExprKind::Between);
  auto nullSafe = compile(Dialect::MySQL, "SELECT a <=> ? FROM t");
  success(*nullSafe);
  CHECK(projection(*nullSafe).value == "NULL_SAFE_EQUAL");
  CHECK(
      !compile(Dialect::MySQL, "SELECT * FROM a FULL JOIN b ON a.x=b.x")->ok());
  success(*compile(Dialect::MySQL, "SELECT 2--1"));
  success(*compile(Dialect::MySQL, "SELECT 1 # trailing comment"));
}
void postgres() {
  auto c = compile(
      Dialect::PostgreSQL,
      "SELECT MiXeD, \"MiXeD\", $tag$a'b$tag$, E'a\\n', x::numeric(10,2) FROM "
      "Public.Items WHERE x ILIKE $1 LIMIT ALL OFFSET 2");
  success(*c);
  CHECK(projection(*c).name[0] == "mixed");
  CHECK(projection(*c, 1).name[0] == "MiXeD");
  CHECK(projection(*c, 2).value == "a'b");
  CHECK(projection(*c, 3).value == "a\n");
  CHECK(projection(*c, 4).kind == ExprKind::Cast);
  CHECK(c->model[0].limit == 0);
  CHECK(c->model[0].offset != 0);
  CHECK(c->model[0].tables[0].name[0] == "public");
  success(
      *compile(Dialect::PostgreSQL, "SELECT 1 /* outer /* nested */ done */"));
  CHECK(!compile(Dialect::PostgreSQL, "SELECT `x`")->ok());
  CHECK(!compile(Dialect::PostgreSQL, "SELECT $0")->ok());
  CHECK(!compile(Dialect::PostgreSQL, "SELECT E'\\u1234'")->ok());
  auto concat = compile(Dialect::PostgreSQL, "SELECT a||b+c FROM t");
  success(*concat);
  CHECK(projection(*concat).value == "CONCAT");
  CHECK(concat->model[0].expressions[projection(*concat).operands[1]].value ==
        "+");
  auto between =
      compile(Dialect::PostgreSQL, "SELECT a BETWEEN 1 AND b=3 FROM t");
  success(*between);
  CHECK(projection(*between).value == "=");
}
void oracle() {
  auto c =
      compile(Dialect::Oracle,
              "SELECT mixed, \"mixed\", q'[it's ok]', :name FROM app.items i "
              "ORDER BY i.id NULLS LAST OFFSET 2 ROWS FETCH NEXT 5 ROWS ONLY");
  success(*c);
  CHECK(projection(*c).name[0] == "MIXED");
  CHECK(projection(*c, 1).name[0] == "mixed");
  CHECK(projection(*c, 2).value == "it's ok");
  CHECK(projection(*c, 3).kind == ExprKind::Parameter);
  CHECK(c->model[0].expressions[c->model[0].limit].value == "5");
  CHECK(!compile(Dialect::Oracle, "SELECT x FROM t AS a")->ok());
  CHECK(!compile(Dialect::Oracle, "SELECT x FROM t LIMIT 2")->ok());
  CHECK(!compile(Dialect::Oracle, "SELECT 1")->ok());
  success(
      *compile(Dialect::Oracle, "SELECT x AS a FROM t FETCH FIRST ROW ONLY"));
}
void sqlserver() {
  auto c = compile(Dialect::SQLServer,
                   "SELECT TOP (10) [a]]b], N'你好', @id FROM [dbo].[items]");
  success(*c);
  CHECK(projection(*c).name[0] == "a]b");
  CHECK(projection(*c, 1).value == "你好");
  CHECK(c->model[0].expressions[c->model[0].limit].value == "10");
  success(*compile(
      Dialect::SQLServer,
      "SELECT id FROM t ORDER BY id OFFSET 2 ROWS FETCH NEXT 3 ROWS ONLY"));
  CHECK(!compile(Dialect::SQLServer, "SELECT id FROM t OFFSET 2 ROWS")->ok());
  CHECK(!compile(Dialect::SQLServer, "SELECT TOP 10 PERCENT id FROM t")->ok());
  CHECK(!compile(Dialect::SQLServer,
                 "SELECT TOP 10 id FROM t ORDER BY id OFFSET 2 ROWS")
             ->ok());
  CHECK(!compile(Dialect::SQLServer, "SELECT TOP @n id FROM t")->ok());
  CHECK(
      !compile(Dialect::SQLServer,
               "SELECT id FROM t ORDER BY id OFFSET 0 ROWS FETCH NEXT ROW ONLY")
           ->ok());
  success(*compile(Dialect::SQLServer, "SELECT TOP (@n) id FROM t"));
}
void sqlite() {
  auto c =
      compile(Dialect::SQLite,
              "SELECT [name], ?1, :p, @p, $p FROM main.items LIMIT 3 OFFSET 4");
  success(*c);
  CHECK(projection(*c, 1).value == "?1");
  CHECK(projection(*c, 4).value == "$p");
  auto concat = compile(Dialect::SQLite, "SELECT 'a'||'b'+'c'");
  success(*concat);
  CHECK(projection(*concat).value == "+");
  auto unary = compile(Dialect::SQLite, "SELECT -a||b FROM t");
  success(*unary);
  CHECK(projection(*unary).value == "CONCAT");
  CHECK(!compile(Dialect::SQLite, "SELECT [a]]b] FROM t")->ok());
}
void scopes() {
  MemoryCatalog cat;
  cat.tables = {{"demo",
                 "public",
                 "items",
                 {{"id", ValueType::Integer}, {"title", ValueType::String}}},
                {"demo", "public", "other", {{"id", ValueType::Integer}}}};
  auto c = compile(Dialect::PostgreSQL,
                   "SELECT i.id, i.title AS label FROM items i LEFT JOIN other "
                   "o ON i.id=o.id "
                   "WHERE o.id IS NOT NULL ORDER BY label",
                   {}, &cat, {"demo", "public"});
  success(*c);
  CHECK(c->model[0].catalogChecked);
  CHECK(projection(*c).binding.has_value());
  CHECK(projection(*c).binding->table == 0);
  CHECK(projection(*c, 1).type == ValueType::String);
  CHECK(c->model[0].tables[1].join == JoinKind::Left);
  auto ambiguous = compile(Dialect::PostgreSQL,
                           "SELECT id FROM items i JOIN other o ON i.id=o.id",
                           {}, &cat, {"demo", "public"});
  CHECK(has(*ambiguous, DiagnosticCode::AmbiguousColumn));
  CHECK(has(*compile(Dialect::PostgreSQL, "SELECT missing FROM items", {}, &cat,
                     {"demo", "public"}),
            DiagnosticCode::UnknownColumn));
  CHECK(has(*compile(Dialect::PostgreSQL, "SELECT id FROM absent", {}, &cat,
                     {"demo", "public"}),
            DiagnosticCode::UnknownTable));
  CHECK(has(*compile(Dialect::PostgreSQL,
                     "SELECT 1 FROM items i JOIN other i ON 1=1"),
            DiagnosticCode::DuplicateAlias));
  CHECK(has(*compile(Dialect::PostgreSQL, "SELECT items.id FROM items i"),
            DiagnosticCode::UnknownColumn));
  CHECK(has(*compile(Dialect::PostgreSQL,
                     "SELECT i.id FROM items i JOIN other o ON "
                     "z.id=o.id JOIN items z ON z.id=i.id"),
            DiagnosticCode::UnknownColumn));
  auto unchecked =
      compile(Dialect::PostgreSQL, "SELECT missing FROM unknown_table");
  success(*unchecked);
  CHECK(!projection(*unchecked).binding);
  CHECK(!unchecked->model[0].catalogChecked);
  MemoryCatalog mysqlCat;
  mysqlCat.tables = {{"shop", "", "items", {{"id", ValueType::Integer}}}};
  success(*compile(Dialect::MySQL, "SELECT id FROM shop.items", {}, &mysqlCat));
  MemoryCatalog starCat;
  starCat.tables = {{"", "", "items", {{"*", ValueType::Integer}}}};
  auto literalStar = compile(Dialect::PostgreSQL,
                             "SELECT \"*\", items.* FROM items", {}, &starCat);
  success(*literalStar);
  CHECK(projection(*literalStar).kind == ExprKind::Name);
  CHECK(projection(*literalStar).binding.has_value());
  CHECK(projection(*literalStar, 1).kind == ExprKind::Star);
  CHECK(has(*compile(Dialect::PostgreSQL, "SELECT invalid.* FROM items"),
            DiagnosticCode::UnknownColumn));
}
void sourceAndRecovery() {
  auto c = compile(Dialect::PostgreSQL,
                   "-- first\r\nSELECT '中文' /* keep */;\rSELECT 2");
  success(*c);
  CHECK(c->source.position(10).line == 2);
  CHECK(c->source.position(10).byteColumn == 1);
  CHECK(c->statements.size() == 2);
  CHECK(c->tokens[0].kind == TokenKind::Comment);
  auto token = c->tokens[2];
  CHECK(c->source.slice(token.range) == "'中文'");
  auto bad =
      compile(Dialect::MySQL, "SELECT ; SELECT 2; CREATE TABLE ; SELECT 3;");
  CHECK(!bad->syntaxValid);
  CHECK(bad->statements.size() == 2);
  CHECK(bad->model.empty());
  CHECK(bad->diagnostics.size() == 2);
  CHECK(!compile(Dialect::MySQL, "SELECT 1 /*!80000 + 2 */")->ok());
  CHECK(!compile(Dialect::Oracle, "SELECT /*+ INDEX(t) */ 1 FROM t")->ok());
  CHECK(!compile(Dialect::MySQL, "SELECT 'unterminated")->ok());
  CHECK(has(
      *compile(Dialect::PostgreSQL, std::string("SELECT '") + char(0xff) + "'"),
      DiagnosticCode::InvalidEncoding));
  CHECK(has(*compile(Dialect::MySQL, std::string("SELECT 1\0;", 10)),
            DiagnosticCode::InvalidEncoding));
  CHECK(!compile(Dialect::PostgreSQL,"SELECT x OVER() FROM t")->ok());
  success(*compile(Dialect::PostgreSQL,"SELECT * FROM (SELECT 1)"));
  for (const auto dialect : {Dialect::MySQL, Dialect::MariaDB, Dialect::PostgreSQL, Dialect::SQLite}) {
    const std::string source = "WITH RECURSIVE seq(n) AS (SELECT 1 UNION ALL "
                               "SELECT n + 1 FROM seq WHERE n < 6) SELECT n FROM seq;";
    auto compiled = compile(dialect, source);
    success(*compiled);
    const auto &root = compiled->model.front();
    CHECK(root.recursiveWith && root.ctes.size() == 1);
    const auto &cte = compiled->model.at(root.ctes.front().query - 1);
    CHECK(cte.setOperation && cte.setOperation->recursive && cte.setOperation->all);
    const auto &member = compiled->model.at(cte.setOperation->right - 1);
    CHECK(member.tables.size() == 1 && member.tables.front().workingTableOf == cte.id);
    CHECK(member.tables.front().query == cte.setOperation->left);
    CHECK(compiled->model.types.get(projection(*compiled).sqlType).family == TypeFamily::Integer);
    const auto emitted = generateSql(compiled->model, dialect);
    CHECK(emitted.ok() && emitted.sql.find("WITH RECURSIVE") != std::string::npos);
    success(*compile(dialect, emitted.sql));
  }
  auto nonrecursive = compile(Dialect::PostgreSQL, "WITH RECURSIVE t(n) AS (SELECT 1) SELECT n FROM t;");
  success(*nonrecursive);
  CHECK(nonrecursive->model.front().recursiveWith);
  CHECK(!nonrecursive->model.at(nonrecursive->model.front().ctes.front().query - 1).setOperation);
  for (auto sql : {
           "WITH RECURSIVE t(n) AS (SELECT n FROM t UNION ALL SELECT 1) SELECT n FROM t;",
           "WITH RECURSIVE t(n) AS (SELECT 1 UNION ALL SELECT n FROM (SELECT n FROM t) x) SELECT n FROM t;",
           "WITH RECURSIVE t(n) AS (SELECT 1 UNION ALL SELECT a.n FROM t a JOIN t b ON a.n=b.n) SELECT n FROM t;",
           "WITH RECURSIVE t(n) AS (SELECT 1 UNION ALL SELECT SUM(n) FROM t) SELECT n FROM t;"}) {
    auto rejected = compile(Dialect::PostgreSQL, sql);
    CHECK(rejected->syntaxValid && has(*rejected, DiagnosticCode::UnsupportedSyntax));
  }
  auto wrongArity = compile(Dialect::PostgreSQL,
      "WITH RECURSIVE t(n) AS (SELECT 1 UNION ALL SELECT n, n FROM t) SELECT n FROM t;");
  CHECK(wrongArity->syntaxValid && has(*wrongArity, DiagnosticCode::InvalidQueryShape));
  for (auto sql : {"SELECT * + 1 FROM t", "SELECT * AS x FROM t",
                   "SELECT x FROM t WHERE *", "SELECT x FROM t ORDER BY *",
                   "SELECT COUNT(*, 1) FROM t"})
    CHECK(!compile(Dialect::PostgreSQL, sql)->ok());
  success(*compile(Dialect::PostgreSQL,
                   "SELECT COUNT(*), CAST(price AS numeric(10,2)) FROM items "
                   "WHERE id NOT IN (1,2) "
                   "GROUP BY price HAVING COUNT(*) > 1 ORDER BY price"));
}
void limits() {
  LanguageOptions options;
  options.limits.sourceBytes = 8;
  CHECK(has(*compile(Dialect::MySQL, "SELECT 10", options),
            DiagnosticCode::ResourceLimit));
  options = {};
  options.limits.tokens = 2;
  CHECK(has(*compile(Dialect::MySQL, "SELECT 1+2", options),
            DiagnosticCode::ResourceLimit));
  options = {};
  options.limits.nodes = 2;
  CHECK(has(*compile(Dialect::MySQL, "SELECT 1+2", options),
            DiagnosticCode::ResourceLimit));
  options = {};
  options.limits.nesting = 4;
  CHECK(has(*compile(Dialect::MySQL, "SELECT (((((1)))))", options),
            DiagnosticCode::ResourceLimit));
  options = {};
  options.limits.statements = 1;
  CHECK(has(*compile(Dialect::MySQL, "SELECT 1; SELECT 2", options),
            DiagnosticCode::ResourceLimit));
  options = {};
  options.limits.diagnostics = 1;
  CHECK(compile(Dialect::MySQL, "SELECT ;SELECT ;SELECT ;", options)
            ->diagnostics.size() == 1);
  std::string flat = "SELECT 1";
  for (int i = 0; i < 10000; ++i)
    flat += "+1";
  success(*compile(Dialect::PostgreSQL, flat));
}
void ownershipAndParallelism() {
  ModelSelect retained;
  {
    auto c = compile(
        Dialect::Oracle,
        "SELECT x AS label FROM items OFFSET 2 ROWS FETCH NEXT 3 ROWS ONLY");
    success(*c);
    retained = c->model[0];
  }
  CHECK(retained.tables[0].name[0] == "ITEMS");
  CHECK(retained.projections[0].alias == "LABEL");
  std::vector<std::future<bool>> jobs;
  for (int i = 0; i < 16; ++i)
    jobs.push_back(std::async(std::launch::async, [i] {
      auto c = compile(i % 2 ? Dialect::Oracle : Dialect::PostgreSQL,
                       "SELECT Mixed FROM Items");
      return c->ok() && projection(*c).name[0] == (i % 2 ? "MIXED" : "mixed");
    }));
  for (auto &job : jobs)
    CHECK(job.get());
  auto dump = compile(Dialect::MySQL, "SELECT 'a\\0b', 'x\"y'");
  success(*dump);
  CHECK(dumpAST(*dump).find("\\u0000") != std::string::npos);
  CHECK(dumpModel(*dump).find("\"queries\"") != std::string::npos);
}
void nestedQueriesAndCases() {
  for (auto d :
       {Dialect::MySQL, Dialect::PostgreSQL, Dialect::Oracle, Dialect::SQLite,
        Dialect::SQLServer, Dialect::MariaDB, Dialect::OceanBase, Dialect::TiDB}) {
    auto c = compile(
        d,
        "WITH q AS (SELECT id FROM items) SELECT CASE WHEN q.id > 0 THEN 'yes' "
        "ELSE 'no' END, COUNT(DISTINCT q.id) FROM q GROUP BY q.id");
    success(*c);
    CHECK(c->model.roots.size() == 1);
    CHECK(c->model.size() == 2);
    CHECK(projection(*c).kind == ExprKind::Case);
    CHECK(projection(*c, 1).distinct);
    CHECK(c->model[0].tables[0].query == c->model[0].ctes[0].query);
  }
  auto implicit = compile(
      Dialect::PostgreSQL,
      "SELECT CASE x WHEN 1 THEN 'one' WHEN 2 THEN 'two' END FROM items");
  success(*implicit);
  CHECK(projection(*implicit).value == "simple");
  CHECK(implicit->model[0]
            .expressions[projection(*implicit).operands.back()]
            .kind == ExprKind::Null);
  CHECK(!implicit->ast
             .expression(implicit->statements[0].projections[0].expression)
             .explicitElse);
  CHECK(!compile(Dialect::PostgreSQL, "SELECT CASE WHEN 1 THEN END")->ok());
  CHECK(!compile(Dialect::PostgreSQL, "SELECT COUNT(DISTINCT *) FROM items")
             ->ok());
  auto derived =
      compile(Dialect::PostgreSQL, "SELECT d.id FROM (SELECT 1 AS id) d");
  success(*derived);
  CHECK(projection(*derived).binding.has_value());
  CHECK(projection(*derived).type == ValueType::Integer);
  auto chained = compile(
      Dialect::PostgreSQL,
      "WITH a(n) AS (SELECT 1), b AS (SELECT n FROM a) SELECT b.n FROM b");
  success(*chained);
  CHECK(projection(*chained).binding.has_value());
  CHECK(chained->model.size() == 3);
  CHECK(has(*compile(Dialect::PostgreSQL,
                     "WITH a(x,y) AS (SELECT 1) SELECT x FROM a"),
            DiagnosticCode::InvalidQueryShape));
  CHECK(has(*compile(Dialect::PostgreSQL,
                     "WITH a AS (SELECT 1), a AS (SELECT 2) SELECT * FROM a"),
            DiagnosticCode::DuplicateAlias));
  CHECK(has(*compile(Dialect::PostgreSQL, "SELECT (SELECT 1,2)"),
            DiagnosticCode::InvalidQueryShape));
  CHECK(has(*compile(Dialect::PostgreSQL, "SELECT 1 IN (SELECT 1,2)"),
            DiagnosticCode::InvalidQueryShape));
  success(*compile(Dialect::PostgreSQL, "SELECT EXISTS (SELECT 1,2)"));
  auto partialStar = compile(
      Dialect::PostgreSQL, "SELECT q.id FROM (SELECT * FROM unknown_table) q");
  success(*partialStar);
  CHECK(!projection(*partialStar).binding);
}
void correlatedScopeAndOwnership() {
  MemoryCatalog catalog;
  catalog.tables = {
      {"demo",
       "public",
       "items",
       {{"id", ValueType::Integer}, {"title", ValueType::String}}},
      {"demo", "public", "other", {{"id", ValueType::Integer}}}};
  auto c = compile(Dialect::PostgreSQL,
                   "SELECT i.id, (SELECT o.id FROM other o WHERE o.id=i.id) AS "
                   "related FROM items i",
                   {}, &catalog, {"demo", "public"});
  success(*c);
  CHECK(c->model.size() == 2);
  CHECK(c->model.roots[0] == 1);
  const auto child = projection(*c, 1).query;
  CHECK(child != 0);
  const auto &inner = c->model[child - 1];
  CHECK(inner.scope.parent == c->model[0].id);
  bool correlated = false;
  for (const auto &e : inner.expressions)
    if (e.binding && e.binding->scope == 1)
      correlated = true;
  CHECK(correlated);
  auto fallback = compile(Dialect::PostgreSQL,
                          "SELECT (SELECT title FROM other o) FROM items i", {},
                          &catalog, {"demo", "public"});
  success(*fallback);
  const auto &expr =
      fallback->model[1]
          .expressions[fallback->model[1].projections[0].expression];
  CHECK(expr.binding->scope == 1);
  auto shadow = compile(Dialect::PostgreSQL,
                        "SELECT (SELECT x.title FROM other x) FROM items x", {},
                        &catalog, {"demo", "public"});
  CHECK(has(*shadow, DiagnosticCode::UnknownColumn));
  auto illegalLateral =
      compile(Dialect::PostgreSQL,
              "SELECT d.id FROM items i JOIN (SELECT i.id) d ON d.id=i.id", {},
              &catalog, {"demo", "public"});
  CHECK(has(*illegalLateral, DiagnosticCode::UnknownColumn));
  auto forward = compile(Dialect::PostgreSQL,
                         "SELECT i.id FROM items i JOIN other o ON "
                         "EXISTS(SELECT z.id) JOIN items z ON z.id=i.id",
                         {}, &catalog, {"demo", "public"});
  CHECK(has(*forward, DiagnosticCode::UnknownColumn));
  ModelProgram retained = c->model;
  c.reset();
  CHECK(retained.queries[child - 1].scope.parent == 1);
  CHECK(retained.queries[child - 1].tables[0].name.back() == "other");
  std::string many = "WITH ";
  for (int i = 0; i < 180; ++i) {
    if (i)
      many += ",";
    many += "q" + std::to_string(i) + " AS (SELECT 1 AS n)";
  }
  many += " SELECT n FROM q179";
  auto arena = compile(Dialect::PostgreSQL, many);
  success(*arena);
  CHECK(arena->model.size() == 181);
  CHECK(projection(*arena).binding.has_value());
  LanguageOptions options;
  options.limits.nesting = 12;
  std::string deep = "1";
  for (int i = 0; i < 20; ++i)
    deep = "(SELECT " + deep + ")";
  CHECK(has(*compile(Dialect::PostgreSQL, "SELECT " + deep, options),
            DiagnosticCode::ResourceLimit));
  options = {};
  options.shouldStop = [] { return true; };
  auto stopped = compile(Dialect::PostgreSQL, "SELECT 1", options);
  CHECK(has(*stopped, DiagnosticCode::Stopped));
  CHECK(stopped->model.empty());
}
void inferredShapeBudgets() {
  std::string sql = "WITH q0 AS (SELECT 1 AS n)";
  for (int i = 1; i < 30; ++i)
    sql += ",q" + std::to_string(i) + " AS (SELECT p.*,p.* FROM q" +
           std::to_string(i - 1) + " p)";
  sql += " SELECT * FROM q29";
  auto bounded = compile(Dialect::PostgreSQL, sql);
  CHECK(has(*bounded, DiagnosticCode::ResourceLimit));
  CHECK(bounded->model.empty());
  LanguageOptions options;
  options.limits.queries = 2;
  CHECK(has(
      *compile(Dialect::PostgreSQL,
               "WITH a AS(SELECT 1 AS n),b AS(SELECT n FROM a) SELECT n FROM b",
               options),
      DiagnosticCode::ResourceLimit));
  options = {};
  options.limits.columnsPerQuery = 1;
  CHECK(has(*compile(Dialect::PostgreSQL,
                     "SELECT d.a FROM (SELECT 1 AS a,2 AS b) d", options),
            DiagnosticCode::ResourceLimit));
  options = {};
  options.limits.inferredColumns = 0;
  CHECK(has(*compile(Dialect::PostgreSQL,
                     "WITH a AS (SELECT 1 AS n) SELECT n FROM a", options),
            DiagnosticCode::ResourceLimit));
}
void dmlStatementsAndOwnership() {
  for (auto d :
       {Dialect::MySQL, Dialect::PostgreSQL, Dialect::Oracle, Dialect::SQLite,
        Dialect::SQLServer, Dialect::MariaDB, Dialect::OceanBase, Dialect::TiDB}) {
    for (auto sql :
         {"INSERT INTO items(id,title) VALUES(1,'one')",
          "INSERT INTO items(id) SELECT id FROM other WHERE id IN(SELECT id "
          "FROM third)",
          "UPDATE items SET title='new', id=id+1 WHERE EXISTS(SELECT 1 FROM "
          "other WHERE other.id=items.id)",
          "DELETE FROM items WHERE id=(SELECT max(id) FROM other)"}) {
      auto c = compile(d, sql);
      success(*c);
      CHECK(c->model.roots.size() == 1);
      CHECK(c->model[0].target.has_value());
      CHECK(c->model[0].kind != StatementKind::Select);
      CHECK(!dumpAST(*c).empty());
      CHECK(dumpModel(*c).find("target") != std::string::npos);
    }
  }
  auto c = compile(Dialect::PostgreSQL,
                   "WITH q AS(SELECT id FROM other) INSERT INTO items(id) "
                   "SELECT id FROM q RETURNING id");
  success(*c);
  CHECK(c->model[0].kind == StatementKind::Insert);
  CHECK(c->model[0].source != 0);
  CHECK(c->model[0].returning.size() == 1);
  CHECK(c->model[c->model[0].source - 1].tables[0].query != 0);
  auto m = c->model;
  c.reset();
  CHECK(m[0].target->name.back() == "items");
  CHECK(m[m[0].source - 1].tables[0].name.back() == "q");
  auto many =
      compile(Dialect::SQLite, "INSERT INTO items VALUES(1,2),(3,4);UPDATE "
                               "items SET id=2;DELETE FROM items;SELECT 3");
  success(*many);
  CHECK(many->model.roots.size() == 4);
  CHECK(many->model[0].rows.size() == 2);
  CHECK(many->model[1].assignments.size() == 1);
  CHECK(many->model[2].kind == StatementKind::Delete);
}
void dmlDialectExtensions() {
  {
    const auto source=R"(SELECT j.ord,j.val,j.item FROM (SELECT '[{"a":1}]' AS doc) AS src
       LEFT JOIN JSON_TABLE(src.doc, '$[*]' COLUMNS(
         ord FOR ORDINALITY, val INT PATH '$.a' DEFAULT '0' ON EMPTY,
         NESTED PATH '$.items[*]' COLUMNS(item DECIMAL(10,2) PATH '$')
       )) j ON j.val=1;)";
    const auto compiled=compile(Dialect::MySQL,source);
    success(*compiled);
    const auto &query=compiled->model.front();
    CHECK(query.tables.size()==2 && query.tables[1].jsonTable);
    CHECK(query.tables[1].jsonTable->documentTables==std::vector<std::size_t>{0});
    CHECK(query.tables[1].definition->columns.size()==3);
    CHECK(compiled->model.types.get(projection(*compiled).sqlType).unsignedInteger==true);
    CHECK(compiled->model.types.get(projection(*compiled,2).sqlType).precision==10);
    CHECK(query.relations.back().implicitLateral && query.relations.back().condition);
    CHECK(dumpModel(*compiled).find("\"implicitLateral\":true")!=std::string::npos);
    const auto canonical=generateSql(compiled->model,Dialect::MySQL);
    CHECK(canonical.ok() && canonical.sql.find("NESTED PATH")!=std::string::npos);
    success(*compile(Dialect::MySQL,canonical.sql));
    CHECK(has(*compile(Dialect::MySQL,
        R"(SELECT j.val FROM a CROSS JOIN JSON_TABLE(future.doc, '$[*]' COLUMNS(val INT PATH '$')) j
           JOIN b future ON 1=1;)"),DiagnosticCode::UnknownColumn));
    CHECK(has(*compile(Dialect::MySQL,
        R"(SELECT j.x FROM JSON_TABLE('[]', '$[*]' COLUMNS(x INT PATH '$', NESTED PATH '$' COLUMNS(X INT PATH '$'))) j;)"),
        DiagnosticCode::DuplicateAlias));
    CHECK(!compile(Dialect::MySQL,"SELECT * FROM JSON_TABLE('[]','$' COLUMNS(ord FOR ORDINALITY));")->ok());
  }
  const auto control = compile(Dialect::MySQL, "START TRANSACTION WITH CONSISTENT SNAPSHOT, READ ONLY;");
  success(*control);
  CHECK(control->statements.front().kind == StatementKind::Transaction);
  CHECK(control->model.front().transaction && control->model.front().transaction->consistentSnapshot);
  CHECK(control->model.front().transaction->access == TransactionAccess::ReadOnly);
  CHECK(control->model.front().transactionEffect == TransactionEffect::ExplicitControl);
  for (const auto source : {"START TRANSACTION READ ONLY, READ WRITE;",
                            "START TRANSACTION WITH CONSISTENT SNAPSHOT,;",
                            "COMMIT AND RELEASE;", "SAVEPOINT;"})
    CHECK(!compile(Dialect::MySQL,source)->ok());
  CHECK(!compile(Dialect::SQLServer,"RELEASE SAVEPOINT s;")->ok());
  for (auto d : {Dialect::MySQL, Dialect::MariaDB, Dialect::OceanBase}) {
    for (auto sql :
         {"INSERT IGNORE INTO items(id,title) VALUES(1,DEFAULT),(2,'two') ON "
          "DUPLICATE KEY UPDATE title=VALUES(title)",
          "INSERT items SET id=1,title=DEFAULT",
          "UPDATE items i JOIN other o ON i.id=o.id SET i.title=o.title WHERE "
          "o.id>0",
          "UPDATE items SET id=3 ORDER BY id DESC LIMIT 1",
          "DELETE i FROM items i JOIN other o ON i.id=o.id WHERE i.id=1",
          "DELETE FROM items USING items JOIN other ON items.id=other.id",
          "DELETE FROM items ORDER BY id LIMIT 1",
          "INSERT INTO items () VALUES ()",
          "INSERT INTO items () SELECT 3, 'three'",
          "INSERT INTO items (SELECT 4, 'four')",
          "REPLACE INTO items(id,title) VALUES(1,'new')",
          "REPLACE LOW_PRIORITY items SET id=1,title='new'",
          "INSERT INTO items(id) WITH q AS(SELECT id FROM other) SELECT id "
          "FROM q"})
      success(*compile(d, sql));
    const auto replaced=compile(d, "REPLACE INTO items(id) VALUES(1);");
    CHECK(replaced->statements.front().replaceExisting && replaced->model.front().replaceExisting);
    const auto emitted=generateSql(replaced->model, d);
    CHECK(emitted.ok() && emitted.sql.find("REPLACE INTO") == 0);
    CHECK(!compile(d, "REPLACE INTO items(id) VALUES(1) ON DUPLICATE KEY UPDATE title='x';")->ok());
    for (const auto sql : {"SELECT 1 IN (SELECT 1 LIMIT 1);",
                           "SELECT 1 NOT IN (SELECT 1 LIMIT 1);",
                           "SELECT 1 = ANY (SELECT 1 LIMIT 1);",
                           "SELECT 1 = ALL (SELECT 1 LIMIT 1);"})
      CHECK(has(*compile(d, sql), DiagnosticCode::UnsupportedSyntax));
    success(*compile(d, "SELECT EXISTS(SELECT 1 LIMIT 1),"
                        " 1 IN (SELECT id FROM (SELECT 1 AS id LIMIT 1) t);"));
  }
  success(*compile(Dialect::PostgreSQL, "SELECT 1 IN (SELECT 1 LIMIT 1);"));
  {
    MemoryCatalog catalog;
    catalog.tables = {{"", "", "items", {{"a",ValueType::Integer},{"b",ValueType::Integer},{"c",ValueType::Integer}}}};
    const std::string prefix = "INSERT INTO items(a,b) VALUES(1,2) AS incoming";
    const auto compiled=compile(Dialect::MySQL, prefix +
        "(x,y) ON DUPLICATE KEY UPDATE items.c=incoming.x+items.b;",{},&catalog);
    success(*compiled);
    const auto &model=compiled->model.front();
    CHECK(model.conflict.incomingTable && model.conflict.columnAliases.size()==2);
    const auto &incoming=model.tables[*model.conflict.incomingTable];
    CHECK(incoming.pseudo && incoming.definition && incoming.definition->complete &&
          incoming.definition->columns[0].name=="x");
    const auto &value=model.expressions[model.conflict.assignments[0].value];
    CHECK(model.expressions[value.operands[0]].binding->table==*model.conflict.incomingTable);
    CHECK(model.expressions[value.operands[1]].binding->table==*model.targetTable);
    const auto generated=generateSql(compiled->model,Dialect::MySQL);
    CHECK(generated.ok());
    success(*compile(Dialect::MySQL,generated.sql,{},&catalog));
    success(*compile(Dialect::MySQL, "INSERT INTO items SET a=1,b=2 AS incoming(x,y) "
                                    "ON DUPLICATE KEY UPDATE c=x+y;",{},&catalog));
    success(*compile(Dialect::MySQL, "INSERT INTO future_items VALUES(1,2,3) AS r "
                                    "ON DUPLICATE KEY UPDATE c=r.c;"));
    success(*compile(Dialect::MySQL, prefix + "(x,y);",{},&catalog));
    success(*compile(Dialect::MySQL, prefix + "(x,y) ON DUPLICATE KEY UPDATE c=incoming.X;"));
    CHECK(has(*compile(Dialect::MySQL,prefix+" ON DUPLICATE KEY UPDATE c=b;",{},&catalog),
              DiagnosticCode::AmbiguousColumn));
    CHECK(has(*compile(Dialect::MySQL,prefix+" ON DUPLICATE KEY UPDATE c=incoming.c;",{},&catalog),
              DiagnosticCode::UnknownColumn));
    CHECK(has(*compile(Dialect::MySQL,prefix+"(x,X) ON DUPLICATE KEY UPDATE c=1;",{},&catalog),
              DiagnosticCode::DuplicateAlias));
    CHECK(has(*compile(Dialect::MySQL,"INSERT INTO items(a,b) VALUES(1,2) AS items;",{},&catalog),
              DiagnosticCode::DuplicateAlias));
    CHECK(!compile(Dialect::MySQL,prefix+"(x) ON DUPLICATE KEY UPDATE c=x;")->syntaxValid);
    CHECK(!compile(Dialect::MariaDB,prefix+"(x,y) ON DUPLICATE KEY UPDATE c=x;")->ok());
  }
  for (auto sql :
       {"INSERT INTO items AS i(id,title) VALUES(1,DEFAULT),(2,'x') ON "
        "CONFLICT(id) DO UPDATE SET title=excluded.title WHERE i.id>0 "
        "RETURNING id,title",
        "INSERT INTO items DEFAULT VALUES RETURNING *",
        "UPDATE items i SET title=o.title FROM other o WHERE o.id=i.id "
        "RETURNING i.id",
        "DELETE FROM items i USING other o WHERE i.id=o.id RETURNING i.id",
        "UPDATE items SET (id,title)=(SELECT id,title FROM other)",
        "WITH q AS(SELECT id FROM other) DELETE FROM items WHERE id IN(SELECT "
        "id FROM q)"})
    success(*compile(Dialect::PostgreSQL, sql));
  for (auto sql :
       {"INSERT OR IGNORE INTO items VALUES(1,2) RETURNING *",
        "INSERT INTO items(id) VALUES(1) ON CONFLICT DO NOTHING",
        "UPDATE items SET (id,title)=(1,'two') RETURNING id",
        "DELETE FROM items WHERE id IN(SELECT id FROM other) RETURNING *"})
    success(*compile(Dialect::SQLite, sql));
  for (auto sql : {"INSERT INTO items(id) VALUES(1) RETURNING id INTO :v",
                   "UPDATE items i SET (id,title)=(SELECT id,title FROM other) "
                   "WHERE id=1 RETURNING id INTO :v",
                   "DELETE FROM items WHERE id=1 RETURNING id INTO :v"})
    success(*compile(Dialect::Oracle, sql));
  for (auto sql :
       {"INSERT TOP(1) INTO items(id) OUTPUT inserted.id VALUES(1),(2)",
        "INSERT INTO items OUTPUT INSERTED.* DEFAULT VALUES",
        "UPDATE TOP(1) i SET i.title=o.title OUTPUT inserted.id,deleted.title "
        "FROM items i JOIN other o ON i.id=o.id WHERE i.id=1",
        "DELETE TOP(1) i OUTPUT deleted.id FROM items i JOIN other o ON "
        "i.id=o.id WHERE i.id=1",
        "INSERT items DEFAULT VALUES"})
    success(*compile(Dialect::SQLServer, sql));
  for (auto sql : {
        "INSERT INTO items OUTPUT id DEFAULT VALUES",
        "INSERT INTO items OUTPUT * DEFAULT VALUES",
        "INSERT INTO items OUTPUT deleted.* DEFAULT VALUES",
        "DELETE FROM items OUTPUT inserted.id",
        "UPDATE i SET i.title=o.title OUTPUT i.id FROM items i JOIN other o ON i.id=o.id"}) {
    const auto checked=compile(Dialect::SQLServer,sql);
    CHECK(checked->syntaxValid && !checked->ok());
  }
}
void dmlBindingAndErrors() {
  MemoryCatalog cat;
  cat.tables = {{"",
                 "",
                 "items",
                 {{"id", ValueType::Integer}, {"title", ValueType::String}}},
                {"",
                 "",
                 "other",
                 {{"id", ValueType::Integer}, {"title", ValueType::String}}}};
  for (auto sql : {"INSERT INTO items(id,title) VALUES(1,'one') RETURNING id",
                   "INSERT INTO items(id) SELECT id FROM other",
                   "UPDATE items i SET title=(SELECT o.title FROM other o "
                   "WHERE o.id=i.id) WHERE i.id=1",
                   "DELETE FROM items WHERE id IN(SELECT id FROM other)",
                   "INSERT INTO items(id,title) VALUES(1,'x') ON CONFLICT(id) "
                   "DO UPDATE SET title=excluded.title RETURNING id"})
    success(*compile(Dialect::PostgreSQL, sql, {}, &cat));
  CHECK(
      has(*compile(Dialect::PostgreSQL, "UPDATE items SET absent=1", {}, &cat),
          DiagnosticCode::UnknownColumn));
  CHECK(has(*compile(Dialect::PostgreSQL, "INSERT INTO items(absent) VALUES(1)",
                     {}, &cat),
            DiagnosticCode::UnknownColumn));
  CHECK(has(*compile(Dialect::PostgreSQL,
                     "INSERT INTO items(id) SELECT id,title FROM other", {},
                     &cat),
            DiagnosticCode::InvalidQueryShape));
  CHECK(has(*compile(Dialect::PostgreSQL,
                     "UPDATE items SET (id,title)=(SELECT id FROM other)", {},
                     &cat),
            DiagnosticCode::InvalidQueryShape));
  CHECK(has(*compile(Dialect::PostgreSQL,
                     "INSERT INTO items(id) VALUES(items.id)", {}, &cat),
            DiagnosticCode::UnknownColumn));
  for (auto sql : {"INSERT INTO items(id) VALUES(1,2)",
                   "INSERT INTO items VALUES(1),(1,2)",
                   "INSERT INTO items VALUES", "UPDATE items SET",
                   "UPDATE items SET id= WHERE id=1", "DELETE FROM WHERE id=1",
                   "DELETE items", "INSERT INTO items VALUES(1) garbage"})
    CHECK(!compile(Dialect::PostgreSQL, sql)->ok());
  CHECK(!compile(Dialect::MySQL, "DELETE FROM items RETURNING id")->ok());
  CHECK(!compile(Dialect::MySQL, "INSERT INTO items DEFAULT VALUES")->ok());
  CHECK(!compile(Dialect::Oracle, "INSERT INTO items VALUES(1),(2)")->ok());
  CHECK(!compile(Dialect::Oracle, "UPDATE items SET id=1 FROM other")->ok());
  CHECK(!compile(Dialect::SQLite, "DELETE FROM items USING other")->ok());
  CHECK(!compile(Dialect::PostgreSQL, "UPDATE items SET id=1 LIMIT 1")->ok());
  CHECK(!compile(Dialect::SQLServer, "INSERT INTO items VALUES(1) RETURNING id")
             ->ok());
  auto recovery = compile(
      Dialect::PostgreSQL,
      "INSERT INTO items VALUES(;DELETE FROM items;UPDATE items SET id=2");
  CHECK(!recovery->syntaxValid);
  CHECK(recovery->model.empty());
  CHECK(recovery->statements.size() == 2);
}
void joinTreesAndMergedColumns() {
  MemoryCatalog cat;
  cat.tables = {
      {"", "", "a", {{"id", ValueType::Integer}, {"x", ValueType::String}}},
      {"", "", "b", {{"id", ValueType::Integer}, {"y", ValueType::String}}},
      {"", "", "c", {{"id", ValueType::Integer}}}};
  for (auto d : {Dialect::PostgreSQL, Dialect::MySQL, Dialect::Oracle,
                 Dialect::SQLite}) {
    auto q = compile(d, "SELECT id FROM a JOIN b USING(id)");
    success(*q);
    CHECK(q->model[0].relations.size() == 3);
    CHECK(q->model[0].relations.back().usingColumns.size() == 1);
  }
  auto q =
      compile(Dialect::PostgreSQL,
              "SELECT id,a.id,b.id FROM a FULL JOIN b USING(id)", {}, &cat);
  success(*q);
  CHECK(projection(*q).coalescedBindings.size() == 2);
  auto star = compile(
      Dialect::PostgreSQL,
      "SELECT d.id,d.x,d.y FROM (SELECT * FROM a NATURAL JOIN b) d", {}, &cat);
  success(*star);
  CHECK(projection(*star).binding.has_value());
  success(*compile(
      Dialect::PostgreSQL,
      "SELECT a.id FROM a LEFT JOIN (b JOIN c ON b.id=c.id) ON a.id=b.id", {},
      &cat));
  success(*compile(Dialect::PostgreSQL,
                   "SELECT a.id FROM a,b JOIN c ON b.id=c.id", {}, &cat));
  CHECK(has(*compile(Dialect::PostgreSQL,
                     "SELECT a.id FROM a,b JOIN c ON a.id=c.id", {}, &cat),
            DiagnosticCode::UnknownColumn));
  CHECK(has(*compile(Dialect::PostgreSQL,
                     "SELECT id FROM a JOIN b USING(absent)", {}, &cat),
            DiagnosticCode::UnknownColumn));
  CHECK(!compile(Dialect::SQLServer, "SELECT * FROM a JOIN b USING(id)")->ok());
  CHECK(!compile(Dialect::MySQL, "SELECT * FROM a FULL JOIN b ON a.id=b.id")
             ->ok());
  CHECK(!compile(Dialect::PostgreSQL,
                 "SELECT * FROM a NATURAL JOIN b ON a.id=b.id")
             ->ok());
}
void subqueryPredicatesAndLimits() {
  success(*compile(Dialect::MySQL,
                   "DELETE a.*,b FROM items a JOIN other b ON a.id=b.id"));
  success(*compile(Dialect::MySQL,
                   "DELETE FROM a,b USING items a JOIN other b ON a.id=b.id"));
  success(*compile(Dialect::SQLServer,
                   "INSERT INTO items(id) OUTPUT INSERTED.id VALUES(1)"));
  CHECK(!compile(Dialect::SQLite, "INSERT INTO items VALUES(DEFAULT)")->ok());
  auto boundedJson = compile(Dialect::PostgreSQL, "SELECT 1");
  bool limited = false;
  try {
    dumpModel(*boundedJson, 10);
  } catch (const std::exception &) {
    limited = true;
  }
  CHECK(limited);
  CHECK(dumpModel(*boundedJson).find("SELECT") != std::string::npos);

  success(*compile(Dialect::PostgreSQL,
                   "SELECT id FROM items WHERE (id,title) IN(SELECT id,title "
                   "FROM other) AND id>=ALL(SELECT id FROM other)"));
  CHECK(!compile(Dialect::SQLite,
                 "SELECT id FROM items WHERE id=ANY(SELECT id FROM other)")
             ->ok());
  LanguageOptions o;
  o.limits.nesting = 8;
  std::string source = "a";
  for (int i = 0; i < 15; ++i)
    source = "(" + source + " JOIN b" + std::to_string(i) + " ON 1=1)";
  CHECK(has(*compile(Dialect::PostgreSQL, "SELECT * FROM " + source, o),
            DiagnosticCode::ResourceLimit));
  std::string values = "INSERT INTO items VALUES(1)";
  for (int i = 0; i < 100; ++i)
    values += ",(2)";
  o = {};
  o.limits.nodes = 30;
  CHECK(has(*compile(Dialect::PostgreSQL, values, o),
            DiagnosticCode::ResourceLimit));
}
void routineCallsAndCatalogValidation() {
  MemoryCatalog catalog;
  catalog.routines = {{"demo", "public", "p", 2, 2, true, false},
                      {"demo", "public", "f", 1, 2, false, false},
                      {"demo", "public", "f", 3, 3, false, false}};
  NameResolutionContext context{"demo", "public", Dialect::PostgreSQL};
  auto valid = compile(Dialect::PostgreSQL, "CALL public.p(1,NULL)", {},
                       &catalog, context);
  success(*valid);
  CHECK(valid->model[0].kind == StatementKind::Call);
  CHECK(valid->model[0].arguments.size() == 2);
  CHECK(dumpAST(*valid).find("routine") != std::string::npos);
  CHECK(dumpModel(*valid).find("CALL") != std::string::npos);
  auto sql = generateSql(valid->model, Dialect::PostgreSQL);
  CHECK(sql.ok());
  success(*compile(Dialect::PostgreSQL, sql.sql));
  CHECK(has(
      *compile(Dialect::PostgreSQL, "CALL public.p(1)", {}, &catalog, context),
      DiagnosticCode::RoutineArgumentCount));
  success(*compile(Dialect::PostgreSQL, "SELECT public.f(1)", {}, &catalog,
                   context));
  success(*compile(Dialect::PostgreSQL, "SELECT public.f(1,2,3)", {}, &catalog,
                   context));
  CHECK(has(
      *compile(Dialect::PostgreSQL, "SELECT public.f()", {}, &catalog, context),
      DiagnosticCode::RoutineArgumentCount));
  auto mysql = compile(Dialect::MySQL, "CALL p(1,@result)");
  success(*mysql);
  CHECK(mysql->model[0].expressions[mysql->model[0].arguments[1]].kind ==
        ExprKind::Variable);
  auto emitted = generateSql(mysql->model, Dialect::MySQL);
  CHECK(emitted.ok());
  success(*compile(Dialect::MySQL, emitted.sql));
  auto settings =
      compile(Dialect::MySQL, "SELECT @@version,@@session.sql_mode,@value");
  success(*settings);
  emitted = generateSql(settings->model, Dialect::MySQL);
  CHECK(emitted.ok());
  success(*compile(Dialect::MySQL, emitted.sql));
  CHECK(!compile(Dialect::SQLite, "CALL p(1)")->ok());
}
void ddlCoreAndOwningModel() {
  for (auto d : {Dialect::MySQL, Dialect::PostgreSQL, Dialect::Oracle,
                 Dialect::SQLite, Dialect::SQLServer}) {
    for (auto sql :
         {"CREATE TABLE items(id INTEGER PRIMARY KEY,title VARCHAR(40) NOT "
          "NULL,n DECIMAL(10,2) DEFAULT 0,CONSTRAINT ck CHECK(n>=0))",
          "ALTER TABLE items ADD extra INTEGER",
          "CREATE VIEW item_view AS SELECT id FROM items",
          "CREATE INDEX item_idx ON items(id DESC)", "DROP VIEW item_view",
          "DROP TABLE items"}) {
      auto c = compile(d, sql);
      success(*c);
      CHECK(c->model[0].ddl.has_value());
      CHECK(c->model.sourceDialect == d);
      CHECK(!dumpAST(*c).empty());
      CHECK(dumpModel(*c).find("ddl") != std::string::npos);
    }
  }
  auto c =
      compile(Dialect::MySQL,
              "CREATE TABLE items(id BIGINT UNSIGNED AUTO_INCREMENT PRIMARY "
              "KEY,title VARCHAR(50) CHARACTER SET utf8mb4 COMMENT "
              "'title',created_at TIMESTAMP DEFAULT CURRENT_TIMESTAMP) "
              "ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COMMENT='hello'");
  success(*c);
  CHECK(c->model[0].ddl->engine == "InnoDB");
  CHECK(c->model[0].ddl->definitions[0].identity ==
        IdentityKind::AutoIncrement);
  CHECK(c->model[0].ddl->definitions[1].comment == "title");
  ModelProgram owned = c->model;
  c.reset();
  CHECK(owned[0].ddl->tableComment == "hello");
  CHECK(owned.sourceDialect == Dialect::MySQL);
}
void ddlDialectCases() {
  for (auto sql :
       {"CREATE TABLE IF NOT EXISTS app.items(id bigint GENERATED ALWAYS AS "
        "IDENTITY(START WITH 5 INCREMENT BY 2),tags int[],stamp timestamp WITH "
        "TIME ZONE DEFAULT CURRENT_TIMESTAMP)",
        "CREATE UNIQUE INDEX IF NOT EXISTS ix ON items USING btree(id DESC) "
        "INCLUDE(title) WHERE id>0",
        "ALTER TABLE items ALTER COLUMN id TYPE bigint USING id::bigint, ALTER "
        "COLUMN title SET DEFAULT 'x'",
        "ALTER TABLE items ALTER COLUMN title DROP DEFAULT,ALTER COLUMN title "
        "SET NOT NULL",
        "TRUNCATE TABLE items,other RESTART IDENTITY CASCADE",
        "CREATE SEQUENCE seq START WITH 5 INCREMENT BY 2 CACHE 10 NO CYCLE",
        "COMMENT ON TABLE items IS 'hello'"})
    success(*compile(Dialect::PostgreSQL, sql));
  for (auto sql :
       {"CREATE GLOBAL TEMPORARY TABLE items(id NUMBER(10),title VARCHAR2(40 "
        "CHAR),stamp DATE DEFAULT SYSDATE) ON COMMIT DELETE ROWS",
        "ALTER TABLE items MODIFY title VARCHAR2(80)",
        "ALTER TABLE items RENAME COLUMN title TO label",
        "DROP TABLE items CASCADE CONSTRAINTS PURGE",
        "TRUNCATE TABLE items REUSE STORAGE",
        "CREATE SEQUENCE seq START WITH 1 INCREMENT BY 1 NOCACHE NOCYCLE",
        "RENAME items TO newer"})
    success(*compile(Dialect::Oracle, sql));
  for (auto sql :
       {"CREATE TABLE dbo.items(id int IDENTITY(1,1) NOT NULL,extra AS(id+1) "
        "PERSISTED)",
        "ALTER TABLE dbo.items ALTER COLUMN title nvarchar(100) NULL",
        "CREATE UNIQUE INDEX ix ON dbo.items(id) INCLUDE(title) WHERE id>0",
        "DROP TABLE IF EXISTS dbo.items", "TRUNCATE TABLE dbo.items"})
    success(*compile(Dialect::SQLServer, sql));
  for (auto sql : {"CREATE TEMP TABLE items(id PRIMARY KEY,title)",
                   "CREATE TABLE IF NOT EXISTS items(id INTEGER, title TEXT "
                   "GENERATED ALWAYS AS('x') STORED)",
                   "ALTER TABLE items RENAME COLUMN title TO label",
                   "CREATE INDEX IF NOT EXISTS ix ON items(id) WHERE id>0"})
    success(*compile(Dialect::SQLite, sql));
  for (auto sql :
       {"ALTER TABLE items CHANGE title label VARCHAR(90)",
        "ALTER TABLE items MODIFY id BIGINT NOT NULL", "DROP INDEX ix ON items",
        "RENAME TABLE items TO newer,other TO older",
        "CREATE TABLE IF NOT EXISTS items LIKE other",
        "CREATE TABLE items AS SELECT id FROM other"})
    success(*compile(Dialect::MySQL, sql));
}
void ddlBoundariesAndEffects() {
  CHECK(!compile(Dialect::MySQL, "TRUNCATE t,other")->ok());
  for (auto sql :
       {"CREATE TABLE", "CREATE TABLE t(id)", "CREATE TABLE t(id int,)",
        "CREATE TABLE t(id int DEFAULT)", "ALTER TABLE t", "DROP TABLE",
        "CREATE TABLE t(id int) unsupported"})
    CHECK(!compile(Dialect::PostgreSQL, sql)->ok());
  CHECK(
      !compile(Dialect::MySQL, "CREATE INDEX IF NOT EXISTS ix ON t(id)")->ok());
  CHECK(!compile(Dialect::MySQL, "CREATE SEQUENCE seq")->ok());
  CHECK(
      !compile(Dialect::Oracle, "CREATE TABLE IF NOT EXISTS t(id int)")->ok());
  CHECK(!compile(
             Dialect::Oracle,
             "ALTER TABLE t ADD id int REFERENCES other(id) ON UPDATE CASCADE")
             ->ok());
  CHECK(!compile(Dialect::SQLite, "TRUNCATE TABLE t")->ok());
  CHECK(!compile(Dialect::SQLServer, "RENAME TABLE t TO newer")->ok());
  CHECK(
      compile(Dialect::MySQL, "TRUNCATE TABLE t")->model[0].transactionEffect ==
      TransactionEffect::ImplicitCommit);
  CHECK(compile(Dialect::Oracle, "TRUNCATE TABLE t")
            ->model[0]
            .transactionEffect == TransactionEffect::ImplicitCommit);
  CHECK(compile(Dialect::PostgreSQL, "TRUNCATE TABLE t")
            ->model[0]
            .transactionEffect == TransactionEffect::TransactionalDdl);
  CHECK(compile(Dialect::SQLServer, "TRUNCATE TABLE t")
            ->model[0]
            .transactionEffect == TransactionEffect::TransactionalDdl);
  LanguageOptions o;
  o.limits.nodes = 5;
  CHECK(has(*compile(Dialect::PostgreSQL,
                     "CREATE TABLE t(a int,b int,c int,d int)", o),
            DiagnosticCode::ResourceLimit));
}
void ddlValidationAndLiteralModes() {
  for (const auto &text :
       {"CREATE TABLE t(a int,a int)",
        "CREATE TABLE t(a int PRIMARY KEY,b int PRIMARY KEY)",
        "CREATE TABLE t(a int,PRIMARY KEY(missing))",
        "CREATE TABLE t(a int,CONSTRAINT ck CHECK(a>0),CONSTRAINT ck "
        "CHECK(a<9))",
        "CREATE VIEW v(a,b) AS SELECT 1"})
    CHECK(!compile(Dialect::PostgreSQL, text)->ok());
  for (bool noEscapes : {false, true}) {
    LanguageOptions options;
    options.mysqlNoBackslashEscapes = noEscapes;
    auto c = compile(Dialect::MySQL,
                     R"(SELECT 'a\b''c',1-(2-3),8/(4/2),'中文')", options);
    success(*c);
    auto emitted = generateSql(c->model, Dialect::MySQL);
    CHECK(emitted.ok());
    auto again = compile(Dialect::MySQL, emitted.sql, options);
    success(*again);
    const auto &a = c->model[0];
    const auto &b = again->model[0];
    CHECK(a.expressions[a.projections[0].expression].value ==
          b.expressions[b.projections[0].expression].value);
    CHECK(generateSql(again->model, Dialect::MySQL).sql == emitted.sql);
  }
}
void canonicalSqlRoundTrips() {
  const std::vector<std::pair<Dialect, std::string>> cases = {
      {Dialect::Oracle,
       "ALTER TABLE items ADD(extra NUMBER(10),title VARCHAR2(40))"},
      {Dialect::Oracle,
       "ALTER TABLE items MODIFY(title VARCHAR2(90),extra NOT NULL)"},
      {Dialect::Oracle,
       "CREATE VIEW v AS SELECT id FROM items WITH CHECK OPTION"},
      {Dialect::SQLServer,
       "CREATE VIEW v AS SELECT id FROM items WITH CHECK OPTION"},
      {Dialect::PostgreSQL,
       "CREATE SEQUENCE seq START WITH -5 INCREMENT BY -1 CACHE 5"},
      {Dialect::MySQL,
       "INSERT IGNORE INTO items(id,title) VALUES(1,'one'),(2,DEFAULT) ON "
       "DUPLICATE KEY UPDATE title=VALUES(title)"},
      {Dialect::MySQL, "UPDATE items i JOIN other o ON i.id=o.id SET "
                       "i.title=o.title WHERE o.id>0"},
      {Dialect::MySQL,
       "DELETE i FROM items i JOIN other o ON i.id=o.id WHERE i.id=1"},
      {Dialect::MySQL, "CREATE TABLE items(id BIGINT UNSIGNED AUTO_INCREMENT "
                       "PRIMARY KEY,title VARCHAR(50),stamp TIMESTAMP DEFAULT "
                       "CURRENT_TIMESTAMP) ENGINE=InnoDB COMMENT='hello'"},
      {Dialect::MySQL, "CREATE INDEX ix ON items(title(10) DESC)"},
      {Dialect::PostgreSQL, "WITH q AS(SELECT id FROM other) INSERT INTO "
                            "items(id) SELECT id FROM q RETURNING id"},
      {Dialect::PostgreSQL, "UPDATE items i SET title=o.title FROM other o "
                            "WHERE i.id=o.id RETURNING i.id"},
      {Dialect::PostgreSQL,
       "DELETE FROM items i USING other o WHERE i.id=o.id RETURNING i.id"},
      {Dialect::PostgreSQL,
       "SELECT id,CASE WHEN id>0 THEN 'yes' ELSE 'no' END,(SELECT id FROM "
       "other o WHERE o.id=i.id) FROM items i WHERE id IN(SELECT id FROM "
       "other) ORDER BY id LIMIT 3"},
      {Dialect::PostgreSQL, "SELECT id FROM a NATURAL LEFT JOIN b JOIN c "
                            "USING(id) WHERE id>=ALL(SELECT id FROM other)"},
      {Dialect::PostgreSQL,
       "CREATE TABLE app.items(id bigint GENERATED ALWAYS AS IDENTITY(START "
       "WITH 5 INCREMENT BY 2),tags int[],stamp timestamp WITH TIME ZONE "
       "DEFAULT CURRENT_TIMESTAMP)"},
      {Dialect::PostgreSQL,
       "CREATE VIEW v AS SELECT id FROM items WITH LOCAL CHECK OPTION"},
      {Dialect::PostgreSQL, "CREATE UNIQUE INDEX ix ON items USING btree(id "
                            "DESC NULLS LAST) INCLUDE(title) WHERE id>0"},
      {Dialect::PostgreSQL,
       "ALTER TABLE items DROP COLUMN a CASCADE,DROP COLUMN b RESTRICT"},
      {Dialect::PostgreSQL,
       "TRUNCATE TABLE ONLY items,other * RESTART IDENTITY CASCADE"},
      {Dialect::Oracle,
       "CREATE GLOBAL TEMPORARY TABLE items(id NUMBER(10),title VARCHAR2(40 "
       "CHAR),stamp DATE DEFAULT SYSDATE) ON COMMIT DELETE ROWS"},
      {Dialect::Oracle, "UPDATE items SET (id,title)=(SELECT id,title FROM "
                        "other) RETURNING id INTO :v"},
      {Dialect::Oracle,
       "SELECT id FROM items ORDER BY id OFFSET 2 ROWS FETCH NEXT 3 ROWS ONLY"},
      {Dialect::Oracle, "DROP TABLE items CASCADE CONSTRAINTS PURGE"},
      {Dialect::SQLite,
       "INSERT INTO items(id) VALUES(1) ON CONFLICT DO NOTHING RETURNING id"},
      {Dialect::SQLite, "CREATE TABLE IF NOT EXISTS items(id INTEGER PRIMARY "
                        "KEY,title TEXT GENERATED ALWAYS AS('x') STORED)"},
      {Dialect::SQLServer,
       "UPDATE TOP(1) i SET i.title=o.title OUTPUT inserted.id FROM items i "
       "JOIN other o ON i.id=o.id WHERE i.id=1"},
      {Dialect::SQLServer, "DELETE TOP(1) i OUTPUT deleted.id FROM items i "
                           "JOIN other o ON i.id=o.id WHERE i.id=1"},
      {Dialect::SQLServer,
       "CREATE TABLE dbo.items(id int IDENTITY(1,1) NOT NULL,title "
       "nvarchar(max),extra AS(id+1) PERSISTED)"},
      {Dialect::SQLServer,
       "INSERT INTO items(title) OUTPUT inserted.title VALUES(N'中文')"}};
  for (const auto &sample : cases) {
    auto c = compile(sample.first, sample.second);
    success(*c);
    auto generated = generateSql(c->model, sample.first);
    if (!generated.ok())
      throw std::runtime_error(std::string("generation failed for ") +
                               sample.second + ": " +
                               generated.diagnostics.front().message);
    auto again = compile(sample.first, generated.sql);
    if (!again->ok())
      throw std::runtime_error(std::string("generated SQL failed to parse: ") +
                               generated.sql + ": " +
                               again->diagnostics.front().message);
    auto stable = generateSql(again->model, sample.first);
    CHECK(stable.ok());
    CHECK(stable.sql == generated.sql);
    CHECK(c->model.roots.size() == again->model.roots.size());
  }
}
void generationSafetyAndBudgets() {
  auto c = compile(Dialect::PostgreSQL, "SELECT 1");
  success(*c);
  auto cross = generateSql(c->model, Dialect::MySQL);
  CHECK(!cross.ok());
  CHECK(cross.sql.empty());
  GenerationOptions options;
  options.maxBytes = 5;
  auto shortSql = generateSql(c->model, Dialect::PostgreSQL, options);
  CHECK(!shortSql.ok());
  CHECK(shortSql.sql.empty());
  options = {};
  options.shouldStop = [] { return true; };
  auto stopped = generateSql(c->model, Dialect::PostgreSQL, options);
  CHECK(!stopped.ok());
  CHECK(stopped.diagnostics[0].code == DiagnosticCode::Stopped);
  std::string longSql = "SELECT 1";
  for (int i = 0; i < 10000; ++i)
    longSql += "+1";
  auto longModel = compile(Dialect::PostgreSQL, longSql);
  success(*longModel);
  auto flat = generateSql(longModel->model, Dialect::PostgreSQL);
  CHECK(flat.ok());
  auto reparsed = compile(Dialect::PostgreSQL, flat.sql);
  success(*reparsed);
  CHECK(generateSql(reparsed->model, Dialect::PostgreSQL).sql == flat.sql);
  auto cycle = c->model;
  cycle.queries[0].projections[0].expression = 1;
  cycle.queries[0].expressions[1].kind = ExprKind::Subquery;
  cycle.queries[0].expressions[1].query = 1;
  auto bounded = generateSql(cycle, Dialect::PostgreSQL);
  CHECK(!bounded.ok());
  CHECK(bounded.sql.empty());
}
void malformedInputs() {
  // Deterministic malformed editor input: recovery, progress and bounded
  // output.
  std::mt19937 random(19);
  const std::vector<std::string> pieces = {
      "a",        "1",           " ",         "(",          ")",
      ",",        ";",           "+",         "-",          "*",
      "=",        "'",           "\"",        "NOT ",       "IN ",
      "BETWEEN ", "AND ",        "SELECT ",   "FROM ",      "WHERE ",
      "LIMIT ",   "::",          "$",         "/*",         "*/",
      "\n",       "INSERT ",     "VALUES ",   "UPDATE ",    "DELETE ",
      "SET ",     "JOIN ",       "USING ",    "RETURNING ", "CREATE ",
      "ALTER ",   "DROP ",       "TABLE ",    "DEFAULT ",   "CONSTRAINT ",
      "CHECK ",   "REFERENCES ", "TRUNCATE ", "RENAME "};
  for (int i = 0; i < 2000; ++i) {
    std::string sql = std::vector<std::string>{
        "SELECT ",         "INSERT INTO t ",
        "UPDATE t SET ",   "DELETE FROM t ",
        "WITH q AS(",      "CREATE TABLE t(",
        "ALTER TABLE t ",  "DROP TABLE ",
        "TRUNCATE TABLE ", "CREATE INDEX ix ON t("}[i % 10];
    for (int j = 0, count = int(random() % 60); j < count; ++j)
      sql += pieces[random() % pieces.size()];
    auto c = compile(static_cast<Dialect>(i % 7), sql);
    CHECK(c->ast.size() + c->ast.queryCount() <= c->options.limits.nodes);
    CHECK(c->diagnostics.size() <= c->options.limits.diagnostics);
    if (!c->syntaxValid)
      CHECK(c->model.empty());
    for (const auto &diagnostic : c->diagnostics)
      CHECK(diagnostic.range.end <= sql.size());
  }
}


void setOperationsAndOutputScopes() {
  for(auto dialect:{Dialect::MySQL,Dialect::PostgreSQL,Dialect::Oracle,Dialect::SQLServer,Dialect::SQLite}) {
    const auto from=dialect==Dialect::Oracle?" FROM dual":"";
    auto source=std::string("SELECT 1 AS id")+from+" UNION ALL SELECT 2"+from+" INTERSECT SELECT 2"+from+" ORDER BY id";
    auto c=compile(dialect,source);success(*c);CHECK(c->model.front().setOperation.has_value());
    const auto root=c->model.front().setOperation->kind;
    CHECK(root==((dialect==Dialect::Oracle||dialect==Dialect::SQLite)?SetKind::Intersect:SetKind::Union));
    auto generated=generateSql(c->model,dialect);CHECK(generated.ok());success(*compile(dialect,generated.sql));CHECK(dumpModel(*c).find("setOperation")!=std::string::npos);
    const auto difference=dialect==Dialect::Oracle?" MINUS ":" EXCEPT ";
    c=compile(dialect,std::string("SELECT 1 AS id")+from+difference+"SELECT 2"+from);success(*c);
    CHECK(!compile(dialect,std::string("SELECT 1 AS id,2 AS x")+from+" UNION SELECT 2"+from)->ok());
    CHECK(!compile(dialect,std::string("SELECT 1 AS id")+from+" UNION SELECT 2"+from+" ORDER BY missing")->ok());
    CHECK(!compile(dialect,std::string("SELECT 1 AS id")+from+" UNION SELECT 2"+from+" ORDER BY 2")->ok());
    CHECK(!compile(dialect,std::string("SELECT 1 AS id")+from+" UNION SELECT 2"+from+" ORDER BY t.id")->ok());
    if(dialect!=Dialect::SQLite) {
      c=compile(dialect,std::string("(SELECT 1 AS id")+from+" UNION SELECT 2"+from+") INTERSECT SELECT 2"+from);success(*c);
      generated=generateSql(c->model,dialect);CHECK(generated.ok());success(*compile(dialect,generated.sql));
    }
  }
  for(auto dialect:{Dialect::PostgreSQL,Dialect::MySQL}) {
    auto c=compile(dialect,"WITH q AS(SELECT 1 AS id) SELECT id FROM q UNION ALL SELECT id+1 FROM q ORDER BY id LIMIT 2;");success(*c);auto generated=generateSql(c->model,dialect);CHECK(generated.ok());success(*compile(dialect,generated.sql));
    success(*compile(dialect,"SELECT id FROM (SELECT 1 AS id UNION SELECT 2) u ORDER BY id"));
    success(*compile(dialect,"INSERT INTO t(id) SELECT 1 UNION ALL SELECT 2;"));
    success(*compile(dialect,"SELECT (SELECT 1 UNION SELECT 2 LIMIT 1);"));
    success(*compile(dialect,"SELECT 1 INTERSECT ALL SELECT 1;"));
  }
  for(auto dialect:{Dialect::SQLServer,Dialect::SQLite})CHECK(!compile(dialect,"SELECT 1 INTERSECT ALL SELECT 1;")->ok());
  Limits limits;LanguageOptions options;options.limits.nesting=8;std::string chain="SELECT 1";for(int i=0;i<30;++i)chain+=" UNION SELECT 1";auto bounded=compile(Dialect::PostgreSQL,chain,options);CHECK(!bounded->ok());CHECK(has(*bounded,DiagnosticCode::ResourceLimit));
}
void duplicateAssignmentsRespectDialectRules() {
  for(auto d:{Dialect::PostgreSQL,Dialect::Oracle,Dialect::SQLServer})
    CHECK(!compile(d,"UPDATE t SET x=1,x=2;")->ok());
  success(*compile(Dialect::MySQL,"UPDATE t SET x=1,x=2;"));
  success(*compile(Dialect::SQLite,"UPDATE t SET x=1,x=2;"));
  CHECK(!compile(Dialect::PostgreSQL,"UPDATE t SET (x,x)=(1,2);")->ok());
  CHECK(!compile(Dialect::PostgreSQL,"INSERT INTO t(x) VALUES(1) ON CONFLICT(x) DO UPDATE SET x=2,x=3;")->ok());
}
void nullSafeAndPatternPredicates() {
  for(auto sql:{"SELECT NULL IS DISTINCT FROM NULL;","SELECT x IS NOT DISTINCT FROM y FROM t;",
                "SELECT x FROM t WHERE x NOT ILIKE 'a%' OR y IS DISTINCT FROM NULL;",
                "SELECT (1=1)=TRUE,('x' NOT ILIKE 'X')=FALSE,(NULL IS DISTINCT FROM NULL)=FALSE;"}) {
    auto c=compile(Dialect::PostgreSQL,sql);success(*c);auto emitted=generateSql(c->model,Dialect::PostgreSQL);CHECK(emitted.ok());success(*compile(Dialect::PostgreSQL,emitted.sql));
    auto formatted=formatSql(Dialect::PostgreSQL,sql);CHECK(formatted.ok() && formatted.syntaxSupported);
  }
  for(auto d:{Dialect::PostgreSQL,Dialect::MySQL,Dialect::SQLite}) {
    auto c=compile(d,"SELECT (1=1)=TRUE;");success(*c);auto emitted=generateSql(c->model,d);CHECK(emitted.ok());auto roundtrip=compile(d,emitted.sql);success(*roundtrip);
    const auto &q=roundtrip->model.front();const auto &root=q.expressions.at(q.projections[0].expression);CHECK(root.kind==ExprKind::Binary && q.expressions.at(root.operands[0]).kind==ExprKind::Binary);
  }
  CHECK(!compile(Dialect::MySQL,"SELECT x IS DISTINCT FROM y FROM t;")->ok());
  for (auto dialect : {Dialect::PostgreSQL,Dialect::MySQL}) {
    auto sql="SELECT (1=1) is not false, NULL is unknown, TRUE is not unknown;";
    const auto checked=compile(dialect,sql);
    success(*checked);
    CHECK(projection(*checked).kind==ExprKind::TruthTest);
    CHECK(checked->model.types.get(projection(*checked).sqlType).nullability==Nullability::NotNull);
    const auto emitted=generateSql(checked->model,dialect);
    CHECK(emitted.ok());success(*compile(dialect,emitted.sql));
    const auto formatted=formatSql(dialect,sql);
    CHECK(formatted.ok() && formatted.syntaxSupported && formatted.sql.find("IS UNKNOWN")!=std::string::npos);
  }
  CHECK(!compile(Dialect::PostgreSQL,"SELECT 2 IS TRUE;")->ok());
  success(*compile(Dialect::MySQL,"SELECT 2 IS TRUE;"));
  const auto symmetric=compile(Dialect::PostgreSQL,
      "SELECT 2 BETWEEN SYMMETRIC 3 AND 1, 0 NOT BETWEEN ASYMMETRIC 1 AND 3;");
  success(*symmetric);
  CHECK(projection(*symmetric).value=="BETWEEN SYMMETRIC");
  const auto generatedBetween=generateSql(symmetric->model,Dialect::PostgreSQL);
  CHECK(generatedBetween.ok());success(*compile(Dialect::PostgreSQL,generatedBetween.sql));
  const auto sqlite = Dialect::SQLite;
  for (const auto *sql : {"SELECT 2 IS TRUE, 0 IS NOT FALSE, 1==1;",
                          "SELECT 1 IS NOT DISTINCT FROM NULL, 1 IS NULL < 2;",
                          "SELECT a IS NOT /* kept */ DISTINCT FROM b FROM t;"}) {
    const auto compiled = compile(sqlite, sql);
    success(*compiled);
    const auto generated = generateSql(compiled->model, sqlite);
    CHECK(generated.ok());
    success(*compile(sqlite, generated.sql));
    const auto formatted = formatSql(sqlite, sql);
    CHECK(formatted.ok() && formatted.syntaxSupported);
    CHECK(formatted.sql.find("DISTINCT\nFROM") == std::string::npos);
  }
  const auto renamed = compile(sqlite, "WITH t(true) AS (VALUES (7)) SELECT TRUE, t.column1 FROM t;");
  success(*renamed);
  CHECK(projection(*renamed).kind == ExprKind::Boolean && projection(*renamed,1).binding);
  MemoryCatalog sqliteCatalog;
  sqliteCatalog.tables.push_back({"", "", "flags", {{"TRUE", ValueType::Integer}}});
  const auto shadowed = compile(sqlite, "SELECT TRUE FROM flags;", {}, &sqliteCatalog);
  success(*shadowed);
  CHECK(projection(*shadowed).kind == ExprKind::Name && projection(*shadowed).binding);
  const auto uncertain = compile(sqlite, "SELECT FALSE FROM unknown_table;");
  success(*uncertain);
  CHECK(projection(*uncertain).unresolvedBooleanName);
}
void positionalAndDistinctOrderScopes() {
  for(auto call:{"pg_catalog.sum(n)","\"pg_catalog\".\"sum\"(n)","pg_catalog.count(*)"}) {
    auto c=compile(Dialect::PostgreSQL,std::string("SELECT ")+call+" FROM t;");success(*c);
    auto features=inspectQueryFeatures(c->model.front(),Dialect::PostgreSQL);CHECK(features && features->aggregate);
    CHECK(!compile(Dialect::PostgreSQL,std::string("SELECT id,")+call+" FROM t;")->ok());
  }
  for(auto call:{"other.sum(n)","\"PG_CATALOG\".sum(n)","pg_catalog.\"SUM\"(n)"}) {
    auto c=compile(Dialect::PostgreSQL,std::string("SELECT ")+call+" FROM t;");success(*c);
    auto features=inspectQueryFeatures(c->model.front(),Dialect::PostgreSQL);CHECK(features && !features->aggregate);
  }
  for(auto bad:{"SELECT id,n FROM t ORDER BY 0;","SELECT id,n FROM t ORDER BY 3;",
      "SELECT id,n FROM t ORDER BY -1;","SELECT COUNT(*) FROM t GROUP BY -1;",
      "SELECT id FROM t ORDER BY 1.0;","SELECT COUNT(*) FROM t GROUP BY 'x';",
      "SELECT id FROM t ORDER BY TRUE;","SELECT id FROM t ORDER BY NULL;",
      "SELECT DISTINCT id FROM t ORDER BY n;","SELECT DISTINCT id FROM t ORDER BY id+1;"})
    CHECK(!compile(Dialect::PostgreSQL,bad)->ok());
  for(auto good:{"SELECT id,n FROM t ORDER BY 2;","SELECT DISTINCT id AS n FROM t ORDER BY n;",
      "SELECT DISTINCT id+1 FROM t ORDER BY id+1;","SELECT DISTINCT id+01 FROM t ORDER BY id+1;",
      "SELECT DISTINCT lower(x) FROM t ORDER BY pg_catalog.lower(x);",
      "SELECT DISTINCT id::integer FROM t ORDER BY id::int;",
      "SELECT DISTINCT (SELECT 1) FROM t ORDER BY (SELECT 1);",
      "SELECT DISTINCT id FROM t ORDER BY CAST(id AS INTEGER);",
      "SELECT id FROM t ORDER BY +4;","SELECT COUNT(*) FROM t GROUP BY +1;",
      "SELECT * FROM t ORDER BY 9;","SELECT a.*,a.id FROM(SELECT 1 id,2 n) a GROUP BY 1,2;"})success(*compile(Dialect::PostgreSQL,good));
  for(auto d:{Dialect::PostgreSQL,Dialect::MySQL,Dialect::SQLite}) {
    auto c=compile(d,"SELECT id,n FROM t ORDER BY 2;SELECT id,COUNT(*) FROM t GROUP BY 1;");success(*c);
    const auto &first=c->model.queries.at(c->model.roots[0]-1),&second=c->model.queries.at(c->model.roots[1]-1);
    CHECK(first.expressions.at(first.orderBy[0].expression).outputColumn==1);
    CHECK(second.expressions.at(second.groupBy[0]).outputColumn==0);
    auto emitted=generateSql(c->model,d);CHECK(emitted.ok());success(*compile(d,emitted.sql));
    auto set = compile(d, "SELECT 1 AS a,2 AS b UNION ALL SELECT 3,4 ORDER BY 2;");
    success(*set);
    const auto &result = set->model.front();
    CHECK(result.expressions.at(result.orderBy.front().expression).outputColumn == 1);
  }
  // DML ordering is not a SELECT positional list.
  success(*compile(Dialect::MySQL,"UPDATE t SET n=1 ORDER BY 1 LIMIT 1;"));
}
void groupedColumnSemantics() {
  CHECK(!compile(Dialect::SQLite,"SELECT 1 FROM t HAVING 1;")->ok());
  success(*compile(Dialect::SQLite,"SELECT COUNT(*) FROM t HAVING COUNT(*)>0;"));
  success(*compile(Dialect::SQLite,"SELECT custom_aggregate(n) FROM t HAVING custom_aggregate(n)>0;"));
  success(*compile(Dialect::PostgreSQL,"SELECT custom_aggregate(n),COUNT(*) FROM t;"));
  auto featureSource=compile(Dialect::PostgreSQL,"SELECT SUM(SUM(n)) OVER() FROM t;");success(*featureSource);
  auto features=inspectQueryFeatures(featureSource->model.front(),Dialect::PostgreSQL);
  CHECK(features && features->aggregate && features->window);
  CHECK(!inspectQueryFeatures(featureSource->model.front(),Dialect::PostgreSQL,1));
  CHECK(!inspectQueryFeatures(featureSource->model.front(),Dialect::PostgreSQL,100,[]{return true;}));
  for(auto dialect:{Dialect::PostgreSQL,Dialect::Oracle,Dialect::SQLServer}) {
    for(auto sql:{"SELECT id,COUNT(*) FROM t;","SELECT id FROM t HAVING COUNT(*)>1;",
                  "SELECT SUM(id),SUM(n) OVER() FROM t;"})
      CHECK(!compile(dialect,sql)->ok());
    for(auto sql:{"SELECT COUNT(*) FROM t;","SELECT id+1,SUM(n) FROM t GROUP BY id;",
                  "SELECT id+1,SUM(n) FROM t GROUP BY id+1;",
                  "SELECT SUM(SUM(n)) OVER() FROM t;",
                  "SELECT COUNT(*) OVER() FROM t ORDER BY SUM(n);"})success(*compile(dialect,sql));
  }
  for(auto dialect:{Dialect::Oracle,Dialect::SQLServer}) {
    CHECK(!compile(dialect,"SELECT id,n,SUM(n) FROM t GROUP BY id;")->ok());
    CHECK(!compile(dialect,"SELECT id,SUM(n) FROM t GROUP BY id+1;")->ok());
    success(*compile(dialect,"SELECT t.id,SUM(t.n) FROM t GROUP BY id;"));
  }
  success(*compile(Dialect::PostgreSQL,"SELECT id,SUM(n) FROM t GROUP BY 1;"));
  CHECK(!compile(Dialect::PostgreSQL,"SELECT SUM(n) FROM t GROUP BY 1;")->ok());
  CHECK(!compile(Dialect::PostgreSQL,"CREATE TABLE t(id INTEGER PRIMARY KEY DEFERRABLE,n INTEGER);SELECT id,n,COUNT(*) FROM t GROUP BY id;")->ok());
  CHECK(!compile(Dialect::PostgreSQL,"SELECT id,SUM(n) FROM t GROUP BY 3;")->ok());
  CHECK(!compile(Dialect::PostgreSQL,"WITH q AS(SELECT 1 id,2 n) SELECT id,n,COUNT(*) FROM q GROUP BY id;")->ok());
  // Bare columns can be enabled in MySQL/SQLite; do not invent session modes.
  success(*compile(Dialect::MySQL,"SELECT id,COUNT(*) FROM t;"));
  success(*compile(Dialect::SQLite,"SELECT id,COUNT(*) FROM t;"));
  success(*compile(Dialect::PostgreSQL,"CREATE TABLE t(id INTEGER PRIMARY KEY,n INTEGER);SELECT id,n,COUNT(*) FROM t GROUP BY id;"));
  CHECK(!compile(Dialect::PostgreSQL,"CREATE TABLE t(id INTEGER,n INTEGER);SELECT id,n,COUNT(*) FROM t GROUP BY id;")->ok());
  CHECK(!compile(Dialect::PostgreSQL,"CREATE TABLE t(id INTEGER,n INTEGER,PRIMARY KEY(id,n));SELECT n,COUNT(*) FROM t GROUP BY id;")->ok());
  success(*compile(Dialect::PostgreSQL,"CREATE TABLE t(id INTEGER,n INTEGER,PRIMARY KEY(id,n));SELECT id,n,COUNT(*) FROM t GROUP BY id,n;"));
}
void postgresqlSourceStructure() {
  const auto native=[&](const std::string &sql) {
    auto c=compile(Dialect::PostgreSQL,sql); success(*c);
    ModelProgram owned=c->model; c.reset();
    const auto generated=generateSql(owned,Dialect::PostgreSQL);
    CHECK(generated.ok()); success(*compile(Dialect::PostgreSQL,generated.sql));
    return generated.sql;
  };
  const auto windows=native("SELECT SUM(n) OVER w,SUM(n) OVER(w) FROM generate_series(2,4) AS g(n) WINDOW base AS(),w AS(base ORDER BY n ASC),unused AS();");
  CHECK(windows.find("OVER \"w\"")!=std::string::npos);
  CHECK(windows.find("OVER (\"w\"")!=std::string::npos);
  CHECK(windows.find("WINDOW \"base\" AS ()")!=std::string::npos);
  CHECK(windows.find("\"unused\" AS ()")!=std::string::npos);
  CHECK(windows.find(" ASC")!=std::string::npos);
  CHECK(native("SELECT generate_series FROM generate_series(2,4);").find(" AS ")==std::string::npos);
  CHECK(native("SELECT n,ordinality FROM generate_series(2,4) WITH ORDINALITY AS g(n);").find("AS \"g\"(\"n\")")!=std::string::npos);
  CHECK(native("SELECT a.n,b.n FROM generate_series(2,4) AS a(n) JOIN LATERAL(SELECT a.n+7 AS n) AS b ON TRUE;").find("INNER JOIN LATERAL")!=std::string::npos);
  CHECK(native("CREATE TABLE audit_items(id SERIAL PRIMARY KEY NOT NULL,UNIQUE(id),cost INT NOT NULL DEFAULT 17);").find("PRIMARY KEY NOT NULL")!=std::string::npos);
  CHECK(native("SELECT CASE WHEN TRUE THEN 17 END;").find("ELSE")==std::string::npos);
  CHECK(native("SELECT CASE WHEN TRUE THEN 17 ELSE NULL END;").find("ELSE NULL")!=std::string::npos);
  auto c=compile(Dialect::PostgreSQL,"SELECT ARRAY[17,23];"); success(*c);
  auto owned=c->model;
  CHECK(owned.queries[0].projections[0].implicitAlias);
  owned.queries[0].projections[0].alias="renamed";
  CHECK(generateSql(owned,Dialect::PostgreSQL).sql.find("AS \"renamed\"")!=std::string::npos);
  c=compile(Dialect::PostgreSQL,"SELECT CASE WHEN TRUE THEN 17 END;"); success(*c);
  owned=c->model;
  auto &conditional=owned.queries[0].expressions[owned.queries[0].projections[0].expression];
  auto &fallback=owned.queries[0].expressions[conditional.operands.back()];
  fallback.kind=ExprKind::Number; fallback.value="31";
  CHECK(generateSql(owned,Dialect::PostgreSQL).sql.find("ELSE 31")!=std::string::npos);
  c=compile(Dialect::PostgreSQL,"SELECT SUM(n) OVER w FROM generate_series(2,4) AS g(n) WINDOW w AS(ORDER BY n ASC);"); success(*c);
  owned=c->model;
  auto &q=owned.queries[0];
  auto &window=q.expressions[q.projections[0].expression];
  const auto standalone=generateExpression(owned,owned.roots[0],q.projections[0].expression,Dialect::PostgreSQL);
  CHECK(standalone.ok() && standalone.sql.find(" OVER (")!=std::string::npos);
  CHECK(standalone.sql.find("OVER \"w\"")==std::string::npos);
  CHECK(window.windowSource && window.windowSource->bare);
  window.window->ordering[0].descending=true;
  const auto changed=generateSql(owned,Dialect::PostgreSQL);
  CHECK(changed.ok() && changed.sql.find("OVER \"w\"")==std::string::npos);
  CHECK(changed.sql.find(" DESC")!=std::string::npos);
  success(*compile(Dialect::PostgreSQL,changed.sql));
  GenerationOptions stopped; stopped.shouldStop=[] {return true;};
  const auto cancelled=generateSql(owned,Dialect::PostgreSQL,stopped);
  CHECK(!cancelled.ok() && cancelled.sql.empty());
  c=compile(Dialect::PostgreSQL,"SELECT a.n,b.n,c.n FROM (VALUES(1)) AS a(n),(VALUES(2)) AS b(n) CROSS JOIN (VALUES(3)) AS c(n);"); success(*c);
  owned=c->model;
  auto &relations=owned.queries[0].relations;
  auto comma=std::find_if(relations.begin(),relations.end(),[](const auto &r){return r.comma;});
  auto joined=std::find_if(relations.begin(),relations.end(),[](const auto &r){return r.left && !r.comma;});
  CHECK(comma!=relations.end() && joined!=relations.end());
  // Keep the relation acyclic while moving a comma list into an explicit JOIN operand.
  comma->right=joined->left;
  joined->left=static_cast<RelationId>(comma-relations.begin()+1);
  owned.queries[0].from=static_cast<RelationId>(joined-relations.begin()+1);
  const auto unsafe=generateSql(owned,Dialect::PostgreSQL);
  CHECK(!unsafe.ok() && unsafe.sql.empty());
  for(const auto *sql:{"SELECT SUM(n) OVER missing FROM generate_series(2,4) AS g(n);",
      "SELECT 17 WINDOW w AS(),w AS();","SELECT 17 WINDOW a AS(b),b AS(a);",
      "SELECT SUM(n) OVER(w PARTITION BY n) FROM generate_series(2,4) AS g(n) WINDOW w AS();"}) {
    c=compile(Dialect::PostgreSQL,sql);
    CHECK(c->syntaxValid && !c->ok() && c->model.empty());
    CHECK(!compile(Dialect::PostgreSQL,std::string(sql)+"SELECT (;")->syntaxValid);
  }
  for(const auto *clause:{"PARTITION BY item_id","ORDER BY item_id"}) {
    c=compile(Dialect::PostgreSQL,std::string("SELECT COUNT(*) FROM (VALUES(17),(23)) AS audit_items(item_id) WINDOW unused AS(")+clause+");");
    CHECK(c->syntaxValid && !c->ok() && has(*c,DiagnosticCode::InvalidQueryShape));
  }
}
void namedWindowsAndSemanticRules() {
  const auto copiedFrame=compile(Dialect::PostgreSQL,"SELECT SUM(x) OVER(w) FROM t WINDOW w AS(ROWS UNBOUNDED PRECEDING);");
  CHECK(copiedFrame->syntaxValid && !copiedFrame->ok() && has(*copiedFrame,DiagnosticCode::InvalidQueryShape));
  CHECK(copiedFrame->model.empty());
  const auto unsafe=generateSql(copiedFrame->model,Dialect::PostgreSQL);
  CHECK(!unsafe.ok() && unsafe.sql.empty());
  CHECK(!compile(Dialect::PostgreSQL,"SELECT SUM(x) OVER(w) FROM t WINDOW w AS(ROWS UNBOUNDED PRECEDING);SELECT (; ")->syntaxValid);
  for(auto d:{Dialect::PostgreSQL,Dialect::MySQL,Dialect::SQLite}) {
    auto c=compile(d,"SELECT x+SUM(x) OVER w AS total,ROW_NUMBER() OVER w FROM t WINDOW w AS(PARTITION BY g ORDER BY x ROWS BETWEEN 1 PRECEDING AND CURRENT ROW) ORDER BY total;");success(*c);
    CHECK(c->model[0].namedWindows.size()==1);const auto &q=c->model[0];
    CHECK(q.namedWindows[0].keys.size()==3);
    for(const auto &e:q.expressions)for(auto child:e.operands)CHECK(child>0 && child<q.expressions.size());
    auto generated=generateSql(c->model,d);CHECK(generated.ok());success(*compile(d,generated.sql));CHECK(generated.sql.find("window:")==std::string::npos);
    c=compile(d,"SELECT SUM(x) OVER(w ROWS UNBOUNDED PRECEDING) FROM t WINDOW w AS(PARTITION BY g ORDER BY x);");success(*c);
    c=compile(d,"SELECT SUM(x) OVER w2 FROM t WINDOW w AS(PARTITION BY g),w2 AS(w ORDER BY x);");success(*c);
    for(auto bad:{"SELECT SUM(x) OVER missing FROM t;","SELECT SUM(x) OVER w FROM t WINDOW w AS(),w AS();","SELECT SUM(x) OVER(w ORDER BY y) FROM t WINDOW w AS(ORDER BY x);","SELECT SUM(x) OVER(w) FROM t WINDOW w AS(ROWS UNBOUNDED PRECEDING);","SELECT (SELECT SUM(y) OVER w FROM u) FROM t WINDOW w AS();"}) CHECK(!compile(d,bad)->ok());
    auto formatted=formatSql(d,"select sum(x) over w from t window w as(partition by g order by x);");CHECK(formatted.ok());CHECK(formatted.sql.find("\nWINDOW")!=std::string::npos);CHECK(formatted.sql.find("PARTITION BY")!=std::string::npos);
    for(auto sql:{"SELECT SUM(x) OVER(ORDER BY x RANGE BETWEEN 1 PRECEDING AND CURRENT ROW) FROM t;","SELECT SUM(SUM(x)) OVER(ORDER BY g) FROM t GROUP BY g;"})success(*compile(d,sql));
  }
  success(*compile(Dialect::MySQL,"SELECT SUM(x) OVER w1 FROM t WINDOW w1 AS(w2),w2 AS(PARTITION BY g);"));
  CHECK(!compile(Dialect::PostgreSQL,"SELECT SUM(x) OVER w1 FROM t WINDOW w1 AS(w2),w2 AS();")->ok());
  CHECK(!compile(Dialect::MySQL,"SELECT SUM(x) OVER w1 FROM t WINDOW w1 AS(w2),w2 AS(w1);")->ok());
  for(auto d:{Dialect::MySQL,Dialect::PostgreSQL,Dialect::SQLServer,Dialect::Oracle}) {
    const auto from=d==Dialect::Oracle?" FROM dual":" FROM t";
    for(auto expression:{"ROW_NUMBER()","COUNT()","SUM(DISTINCT *)","SUM(ROW_NUMBER() OVER(ORDER BY x))","SUM(AVG(x))"})CHECK(!compile(d,std::string("SELECT ")+expression+from)->ok());
    CHECK(!compile(d,std::string("SELECT x")+from+" WHERE ROW_NUMBER() OVER(ORDER BY x)=1")->ok());
    CHECK(!compile(d,std::string("SELECT SUM(x) OVER(ORDER BY x ROWS -1 PRECEDING)")+from)->ok());
    if(d!=Dialect::PostgreSQL)CHECK(!compile(d,std::string("SELECT SUM(x) OVER(ORDER BY x ROWS 1.5 PRECEDING)")+from)->ok());
    else success(*compile(d,std::string("SELECT SUM(x) OVER(ORDER BY x ROWS 1.5 PRECEDING)")+from));
    CHECK(!compile(d,std::string("SELECT LAG(x,x,x,x) OVER(ORDER BY x)")+from)->ok());
  }
  CHECK(!compile(Dialect::SQLServer,"SELECT ROW_NUMBER() OVER() FROM t")->ok());
  success(*compile(Dialect::MySQL,"SELECT COUNT(DISTINCT a,b) FROM t"));
  success(*compile(Dialect::SQLite,"SELECT MAX(a,b),MIN(a,b) FROM t"));
  MemoryCatalog catalog;TableDefinition table;table.name="t";table.columns={{"x",ValueType::Integer}};catalog.tables.push_back(table);
  CHECK(!compile(Dialect::PostgreSQL,"SELECT x FROM t WINDOW unused AS(ORDER BY absent)",{},&catalog)->ok());
}
void windowsAndDialectDiagnostics() {
  for(auto d:{Dialect::MySQL,Dialect::PostgreSQL,Dialect::Oracle,Dialect::SQLServer,Dialect::SQLite,Dialect::MariaDB,Dialect::OceanBase}){
    for(auto source:{"SELECT ROW_NUMBER() OVER(PARTITION BY user_id ORDER BY created_at DESC) AS rn FROM orders",
        "WITH valid_orders AS(SELECT user_id,total_amt FROM orders), totals AS(SELECT user_id,SUM(total_amt) AS lifetime_amt,RANK() OVER(ORDER BY SUM(total_amt) DESC) AS amt_rank FROM valid_orders GROUP BY user_id) SELECT * FROM totals",
        "SELECT SUM(amount) OVER(PARTITION BY user_id ORDER BY id ROWS BETWEEN 2 PRECEDING AND CURRENT ROW) FROM orders",
        "SELECT COUNT(*) OVER(), SUM(amount) OVER(ORDER BY id RANGE UNBOUNDED PRECEDING) FROM orders"}){
      auto c=compile(d,source);success(*c);auto generated=generateSql(c->model,d);CHECK(generated.ok());success(*compile(d,generated.sql));CHECK(dumpAST(*c).find("\"window\"")!=std::string::npos);CHECK(dumpModel(*c).find("\"partitions\"")!=std::string::npos);
      ModelProgram owned=c->model;c.reset();CHECK(generateSql(owned,d).ok());
    }
  }
  auto mysql=compile(Dialect::MySQL,"INSERT INTO hot_products(product_id,rank_no) WITH sales AS(SELECT id FROM products) SELECT id,ROW_NUMBER() OVER(ORDER BY id DESC) FROM sales ON CONFLICT(product_id) DO UPDATE SET rank_no=EXCLUDED.rank_no");
  CHECK(!mysql->ok());CHECK(!has(*mysql,DiagnosticCode::UnsupportedSyntax));CHECK(mysql->diagnostics[0].message.find("ON DUPLICATE KEY UPDATE")!=std::string::npos);
  for(auto bad:{"SELECT ROW_NUMBER() OVER(ORDER BY) FROM t","SELECT 1 OVER() FROM t","SELECT SUM(x) OVER(ROWS BETWEEN CURRENT ROW AND 1 PRECEDING) FROM t","SELECT SUM(x) OVER(ROWS UNBOUNDED FOLLOWING) FROM t"})CHECK(!compile(Dialect::MySQL,bad)->ok());
  MemoryCatalog cat;cat.tables.push_back({"owned","public","orders",{{"id",ValueType::Integer},{"amount",ValueType::Integer}}});
  auto missing=compile(Dialect::PostgreSQL,"SELECT SUM(amount) OVER(PARTITION BY absent ORDER BY id) FROM orders",{},&cat,{"owned","public"});CHECK(has(*missing,DiagnosticCode::UnknownColumn));
}
void crossDialectClauseSemantics() {
  for (auto dialect : {Dialect::MySQL, Dialect::PostgreSQL, Dialect::Oracle,
                       Dialect::SQLServer, Dialect::SQLite, Dialect::MariaDB,
                       Dialect::OceanBase}) {
    MemoryCatalog catalog;
    TableDefinition table;
    table.name = dialect == Dialect::Oracle ? "ITEMS" : "items";
    table.columns = {{dialect == Dialect::Oracle ? "ID" : "id", ValueType::Integer},
                     {dialect == Dialect::Oracle ? "QTY" : "qty", ValueType::Integer}};
    catalog.tables.push_back(table);
    const auto expect = [&](const std::string &sql, DiagnosticCode code,
                            std::string_view span, std::string_view explanation) {
      auto c = compile(dialect, sql, {}, &catalog);
      if (!std::any_of(c->diagnostics.begin(), c->diagnostics.end(), [&](const auto &d) {
            return d.code == code && c->source.slice(d.range) == span &&
                   d.message.find(explanation) != std::string::npos;
          })) {
        throw std::runtime_error(dialectName(dialect) + std::string(" ") + sql +
            (c->diagnostics.empty() ? " was accepted" : " produced " + c->diagnostics.front().message));
      }
    };
    const auto shape = DiagnosticCode::InvalidQueryShape;
    success(*compile(dialect, "SELECT SUM(qty) OVER(ORDER BY id), "
                    "ROW_NUMBER() OVER(ORDER BY id) FROM items", {}, &catalog));
    expect("SELECT ABS(qty) OVER(ORDER BY id) FROM items", shape, "ABS(qty)",
           "OVER requires an aggregate or window function");
    expect("UPDATE items SET qty=ROW_NUMBER() OVER(ORDER BY id)", shape,
           "ROW_NUMBER() OVER(ORDER BY id)", "window functions are not allowed");
    expect("DELETE FROM items WHERE SUM(qty)>1", shape, "SUM(qty)",
           "aggregate functions are not allowed");
    expect("CREATE TABLE draft (id INTEGER, CHECK (SUM(id)>0))", shape,
           "SUM(id)", "aggregate");
    expect("CREATE TABLE draft (id INTEGER DEFAULT (SUM(1)))", shape,
           "SUM(1)", "aggregate");
    const std::string subquery = dialect == Dialect::Oracle ? "(SELECT 1 FROM dual)" : "(SELECT 1)";
    expect("CREATE TABLE draft (id INTEGER, CHECK (id > " + subquery + "))",
           shape, subquery, "subqueries are not allowed in CHECK constraints");
    expect("CREATE TABLE draft (id INTEGER DEFAULT (" + subquery + "))",
           shape, subquery, "subqueries are not allowed in column defaults");
    success(*compile(dialect, "CREATE TABLE draft (id INTEGER DEFAULT (1), "
                     "CHECK (id > 0))", {}, &catalog));
    expect("INSERT INTO items (id, absent) VALUES (1,2)",
           DiagnosticCode::UnknownColumn, "absent", "unknown INSERT column");
    expect("INSERT INTO items (id, id) VALUES (1,2)", shape, "id", "duplicate INSERT column");
    expect("INSERT INTO items VALUES (1,2,3)", shape, "1,2,3", "more values");
    const std::string source = "SELECT 1,2,3 FROM items";
    expect("INSERT INTO items " + source, shape, source, "more values");
    expect("INSERT INTO items (qty) " + source, shape, source, "counts differ");
    success(*compile(dialect, "INSERT INTO items (id, qty) SELECT id, qty FROM items", {}, &catalog));
    catalog.tables.front().complete = false;
    success(*compile(dialect, "INSERT INTO items (absent) VALUES (1)", {}, &catalog));
    success(*compile(dialect, "INSERT INTO items VALUES (1,2,3)", {}, &catalog));
  }
  MemoryCatalog catalog;
  catalog.tables.push_back({"", "", "t", {{"id", ValueType::Integer}}});
  const auto aliasCase = compile(Dialect::MySQL, "INSERT INTO t (ID) VALUES (1)", {}, &catalog);
  success(*aliasCase);
  const auto duplicate = compile(Dialect::MySQL,
      "INSERT INTO t (id, ID) VALUES (1,2)", {}, &catalog);
  CHECK(!duplicate->ok() && duplicate->source.slice(duplicate->diagnostics.front().range) == "ID");
  const auto pg = Dialect::PostgreSQL;
  CHECK(!compile(pg, "CREATE INDEX ix ON t ((SUM(id)))")->ok());
  CHECK(!compile(pg, "CREATE INDEX ix ON t (id) WHERE SUM(id)>0")->ok());
  CHECK(!compile(pg, "CALL p(SUM(1))")->ok());
  CHECK(!compile(pg, "INSERT INTO t (id) VALUES (1) "
                     "ON CONFLICT (id) DO UPDATE SET id=2 WHERE SUM(id)>0")->ok());
  success(*compile(pg, "INSERT INTO t (id) VALUES (1) "
                       "ON CONFLICT (id) DO UPDATE SET id=2 "
                       "WHERE (SELECT SUM(id) FROM t)>0"));
}
void windowExclusionsAndTypedFrames() {
  for(auto dialect:{Dialect::PostgreSQL,Dialect::SQLite})for(auto mode:{"NO OTHERS","CURRENT ROW","GROUP","TIES"}) {
    auto sql=std::string("SELECT COUNT(*) OVER(ORDER BY x ROWS BETWEEN UNBOUNDED PRECEDING AND UNBOUNDED FOLLOWING EXCLUDE ")+mode+") FROM t;";
    auto c=compile(dialect,sql);success(*c);const auto &q=c->model.front();const auto &e=q.expressions[q.projections[0].expression];
    CHECK(e.window && std::string(windowExclusionName(e.window->exclusion))==mode);
    CHECK(dumpAST(*c).find("exclusion")!=std::string::npos && dumpModel(*c).find(mode)!=std::string::npos);
    auto generated=generateSql(c->model,dialect);CHECK(generated.ok());success(*compile(dialect,generated.sql));
    CHECK(generated.sql.find(std::string("EXCLUDE ")+mode)!=std::string::npos);
    auto formatted=formatSql(dialect,sql);CHECK(formatted.ok() && formatted.syntaxSupported);success(*compile(dialect,formatted.sql));CHECK(formatSql(dialect,formatted.sql).sql==formatted.sql);
    auto model=c->model;c.reset();CHECK(generateSql(model,dialect).ok());
  }
  success(*compile(Dialect::PostgreSQL,"SELECT COUNT(*) OVER w FROM t WINDOW w AS(ORDER BY x GROUPS BETWEEN 1 PRECEDING AND CURRENT ROW EXCLUDE TIES);"));
  success(*compile(Dialect::SQLite,"SELECT COUNT(*) OVER w FROM t WINDOW base AS(ORDER BY x),w AS(base ROWS UNBOUNDED PRECEDING EXCLUDE CURRENT ROW);"));
  for(auto dialect:{Dialect::MySQL,Dialect::SQLServer,Dialect::Oracle})CHECK(!compile(dialect,"SELECT COUNT(*) OVER(ORDER BY x ROWS UNBOUNDED PRECEDING EXCLUDE CURRENT ROW) FROM t;")->ok());
  for(auto sql:{"SELECT COUNT(*) OVER(EXCLUDE TIES);","SELECT COUNT(*) OVER(ROWS UNBOUNDED PRECEDING EXCLUDE NO);","SELECT COUNT(*) OVER(ROWS UNBOUNDED PRECEDING EXCLUDE OTHER);","SELECT COUNT(*) OVER(ROWS UNBOUNDED PRECEDING EXCLUDE TIES EXCLUDE GROUP);"})CHECK(!compile(Dialect::PostgreSQL,sql)->ok());
  auto c=compile(Dialect::PostgreSQL,"SELECT COUNT(*) OVER(ROWS UNBOUNDED PRECEDING EXCLUDE TIES);");success(*c);
  CHECK(!generateSql(c->model,Dialect::MySQL).ok());auto bad=c->model;bad.queries.front().expressions.at(bad.front().projections[0].expression).window->exclusion=static_cast<WindowExclusion>(99);CHECK(!generateSql(bad,Dialect::PostgreSQL).ok());
  for(auto dialect:{Dialect::PostgreSQL,Dialect::MySQL}) {
    const auto offset=dialect==Dialect::MySQL?"INTERVAL 1 DAY":"INTERVAL '1' DAY";
    success(*compile(dialect,std::string("SELECT COUNT(*) OVER(ORDER BY DATE '2024-01-01' RANGE ")+offset+" PRECEDING);"));
    CHECK(!compile(dialect,"SELECT COUNT(*) OVER(ORDER BY DATE '2024-01-01' RANGE 1 PRECEDING);")->ok());
    CHECK(!compile(dialect,std::string("SELECT COUNT(*) OVER(ORDER BY CAST(1 AS ")+(dialect==Dialect::MySQL?"SIGNED":"INTEGER")+") RANGE "+offset+" PRECEDING);")->ok());
    CHECK(!compile(dialect,std::string("SELECT COUNT(*) OVER(ROWS ")+offset+" PRECEDING);")->ok());
  }
  success(*compile(Dialect::MySQL,"SELECT 1 WINDOW w AS(ORDER BY DATE '2024-01-01' RANGE INTERVAL 1 DAY PRECEDING);"));
  CHECK(!compile(Dialect::MySQL,"SELECT 1 WINDOW w AS(ORDER BY DATE '2024-01-01' RANGE 1 PRECEDING);")->ok());
  CHECK(!compile(Dialect::PostgreSQL,"SELECT 1 FROM t WINDOW w AS(ORDER BY n ROWS n PRECEDING);")->ok());
  CHECK(!compile(Dialect::PostgreSQL,"SELECT 1 WINDOW w AS(ROWS INTERVAL '1' DAY PRECEDING);")->ok());
  CHECK(!compile(Dialect::PostgreSQL,"SELECT 1 WINDOW w AS(ROWS SUM(1) PRECEDING);")->ok());
  CHECK(!compile(Dialect::PostgreSQL,"SELECT COUNT(*) OVER(ROWS SUM(1) PRECEDING);")->ok());
  success(*compile(Dialect::SQLite,"SELECT COUNT(*) OVER(ROWS TRUE PRECEDING);"));
  CHECK(!compile(Dialect::PostgreSQL,"SELECT COUNT(*) OVER(ROWS NULL PRECEDING);")->ok());
}
void sourcePreservingFormatting() {
  std::vector<std::pair<Dialect,std::string>> corpus = {
    {Dialect::PostgreSQL,"select count(*) over(order by x rows between unbounded preceding and unbounded following exclude ties) from t;"},
    {Dialect::SQLite,"select count(*) over w from t window w as(order by x groups 1 preceding exclude current row);"},
    {Dialect::MySQL,"select count(*) over(order by date '2024-01-01' range interval 1 day preceding);"},
    {Dialect::MySQL,"select MixedCase, begin from SensitiveTable where id=1 and x between 2 and 3;"},
    {Dialect::PostgreSQL,"with q as(select a,b from t),r as(select * from q) select case when a>0 then a else 0 end as answer,b from r left join t on t.a=r.a where exists(select 1 from t where t.a=r.a) order by a;"},
    {Dialect::MySQL,"insert into t(a,b) values(1,2),(3,4);insert into t select a,b from q;update t set a=1,long_column=2 where id=3;delete from t where a=1;"},
    {Dialect::SQLite,"begin immediate; update t set a=1; savepoint mark; rollback to mark; commit;"},
    {Dialect::MySQL,"create table t(id int primary key,name varchar(50),constraint ck check(id>0));alter table t add column x int,add column y int;create index ix on t(x,y);drop table t;"},
    {Dialect::MySQL,"DELIMITER $$\nCREATE PROCEDURE p(IN n INT,OUT v INT) BEGIN DECLARE x INT DEFAULT 0; IF n>0 THEN SET v=n; ELSE SET v=0; END IF; WHILE x<n DO SET x=x+1; END WHILE; SELECT x; END$$\nDELIMITER ;"},
    {Dialect::MySQL,"DELIMITER //\nCREATE FUNCTION f(n INT) RETURNS INT BEGIN RETURN n+1; END//\nDELIMITER ;"},
    {Dialect::MySQL,"DELIMITER //\nCREATE FUNCTION f(n INT) RETURNS INT BEGIN CASE n WHEN 1 THEN RETURN CASE WHEN n=1 THEN 2 ELSE 0 END; ELSE RETURN 3; END CASE; END//\nDELIMITER ;"},
    {Dialect::PostgreSQL,"create function f(n int) returns int language plpgsql as $body$begin case when n>0 then return n+1; else return 0; end case; end;$body$;"},
    {Dialect::MySQL,"DELIMITER //\nCREATE FUNCTION f(n INT) RETURNS INT BEGIN REPEAT SET n=n+1; UNTIL n>=3 END REPEAT; RETURN n; END//\nDELIMITER ;"},
    {Dialect::PostgreSQL,"CREATE FUNCTION f(n INT) RETURNS INT LANGUAGE plpgsql AS $$ BEGIN LOOP n:=n+1; CASE WHEN n>2 THEN EXIT WHEN n>=3; ELSE NULL; END CASE; END LOOP; RETURN n; END; $$;"},
    {Dialect::MySQL,"DELIMITER //\nCREATE FUNCTION f(n INT) RETURNS INT BEGIN first_loop: REPEAT SET n=n+1; IF n<1 THEN ITERATE first_loop; END IF; UNTIL n>2 END REPEAT first_loop; RETURN n; END//\nDELIMITER ;"},
    {Dialect::PostgreSQL,"CREATE FUNCTION f(n INT) RETURNS INT LANGUAGE plpgsql AS $$ BEGIN <<again>> LOOP n:=n+1; CASE WHEN n>2 THEN EXIT again WHEN n>=3; ELSE CONTINUE again; END CASE; END LOOP again; RETURN n; END; $$;"},
    {Dialect::PostgreSQL,"create function f(n int) returns int language plpgsql as $body$declare x int; begin if n>0 then return n+1; else return 0; end if; end;$body$;"},
    {Dialect::PostgreSQL,"create function f() returns int as $$select 1+2;$$ language sql;"},
    {Dialect::PostgreSQL,"create function f() returns text as $$not a SQL body$$ language python;"},
    {Dialect::Oracle,"CREATE OR REPLACE PROCEDURE p(n IN NUMBER) AS x NUMBER; BEGIN IF n>0 THEN x:=n; ELSE x:=0; END IF; EXCEPTION WHEN OTHERS THEN RAISE; END;\n/"},
    {Dialect::Oracle,"CREATE OR REPLACE PACKAGE BODY pkg AS PROCEDURE p IS x NUMBER; BEGIN x:=1; END p; END pkg;\n/"},
    {Dialect::SQLServer,"CREATE PROCEDURE p @n int AS BEGIN IF @n>0 BEGIN SELECT @n; END ELSE BEGIN SELECT 0; END; END;\nGO"},
    {Dialect::SQLite,"select \"Odd Name\",'中 文',0.5,-1 from \"T\"; -- keep this\n/* comment */ select 2;"},
    {Dialect::PostgreSQL,"create table intervals(a interval year to month,b interval day to second(3)[]);select cast('1 second' as interval hour to second(2));"},
    {Dialect::Oracle,"create table intervals(a interval year(3) to month,b interval day(4) to second(0));"},
    {Dialect::PostgreSQL,"select interval '1-2' year to month,interval '1:02:03.1234' hour to second(3),extract(year from date '2024-01-01');"},
    {Dialect::MySQL,"select date '2024-01-01'+interval 1 day,extract(day_hour from timestamp '2024-01-01 12:00:00'),interval(23,1,15,17,30);"},
    {Dialect::Oracle,"select interval '123-2' year(3) to month,interval '30.12345' second(2,4),extract(year from date '2024-01-01') from sys.dual;"},
    {Dialect::PostgreSQL,"select date '2024-02-29', timestamp(3) with time zone '2024-02-29 12:34:56.123+08', interval '1 day', numeric(6,-2) '12345';"},
    {Dialect::MySQL,"select date '2024-02-29', time '12:34:56', timestamp '2024-02-29 12:34:56', DATE('2024-01-01') 'alias';"},
    {Dialect::Oracle,"select date '2024-02-29',timestamp '2024-02-29 12:34:56' from sys.dual;"},
    {Dialect::MySQL,"select /*+ MAX_EXECUTION_TIME(1000) */ a from t; /*!40101 SET @x=1 */;"},
    {Dialect::PostgreSQL,"select 'a'\n'b', E'a\\nb', $$raw ; begin end$$, x::text from t;"},
    {Dialect::SQLServer,"select [Name],N'a b' from [MixedCase] where [id]=@id;"}
  };
  for(auto& item:corpus) {
    for(auto preset:{"Readable","Compact","Expanded"}) {
      auto style=formatPreset(preset);
      for(int variant=0;variant<3;++variant) {
        if(variant==1){style.commaPosition=CommaPosition::Leading;style.lineEnding=LineEnding::CRLF;style.finalNewline=true;}
        if(variant==2){style.keywordCase=KeywordCase::Lower;style.useTabs=true;style.spacesInParentheses=true;}
        auto a=formatSql(item.first,item.second,style);
        if(!a.ok())throw std::runtime_error(std::string(preset)+": "+a.diagnostics[0].message+" SQL="+item.second);
        auto b=formatSql(item.first,a.sql,style);
        if(!b.ok() || a.sql!=b.sql)throw std::runtime_error("not idempotent: "+a.sql+"\nSECOND:\n"+b.sql);
        CHECK(!a.mapping.empty());
        for(const auto& map:a.mapping){CHECK(map.before.end<=item.second.size());CHECK(map.after.end<=a.sql.size());}
      }
    }
  }
  auto names=formatSql(Dialect::MySQL,"select MixedCase from SensitiveTable;");
  CHECK(names.sql.find("MixedCase")!=std::string::npos);
  CHECK(names.sql.find("SensitiveTable")!=std::string::npos);
  for (const auto *preset : {"Readable", "Compact", "Expanded"}) {
    const auto style = formatPreset(preset);
    const auto routine = formatSql(Dialect::MySQL,
        "CREATE DEFINER = `root` @ `localhost` FUNCTION f() RETURNS INT DETERMINISTIC RETURN 1;", style);
    CHECK(routine.ok());
    CHECK(routine.sql.find("`root`@`localhost`") != std::string::npos);
    const auto view = formatSql(Dialect::MySQL,
        "CREATE DEFINER = 'admin' @ 'localhost' VIEW v AS SELECT 1;", style);
    CHECK(view.ok());
    CHECK(view.sql.find("'admin'@'localhost'") != std::string::npos);
    CHECK(compile(Dialect::MySQL, view.sql)->ok());
    const auto variables = formatSql(Dialect::MySQL,
        "SELECT @x, @@session.sql_mode;", style);
    CHECK(variables.ok());
    CHECK(variables.sql.find("@x") != std::string::npos);
    CHECK(variables.sql.find("@@session.sql_mode") != std::string::npos);
  }
  auto noBody=FormatStyle{};noBody.formatRoutineBodies=false;
  auto routine=formatSql(Dialect::PostgreSQL,"create function f() returns int as $$select 1+2;$$ language sql;",noBody);
  CHECK(routine.ok());CHECK(routine.sql.find("$$select 1+2;$$")!=std::string::npos);
  auto hint=formatSql(Dialect::MySQL,"select /*+ MAX_EXECUTION_TIME(1000) */ a from t; /*!40101 SET @x=1 */;");
  CHECK(hint.ok());CHECK(hint.sql.find("/*+ MAX_EXECUTION_TIME(1000) */")!=std::string::npos);CHECK(hint.sql.find("/*!40101 SET @x=1 */")!=std::string::npos);CHECK(hint.sql.find("FROM t")!=std::string::npos || hint.sql.find("from t")!=std::string::npos);
  auto dollars=formatSql(Dialect::MySQL,"DELIMITER $$\nCREATE PROCEDURE p() BEGIN SELECT 'a$$b'; END$$\nDELIMITER ;");
  CHECK(dollars.ok());CHECK(dollars.sql.find("'a$$b'")!=std::string::npos);
  CHECK(!formatSql(Dialect::SQLite,"select 'unterminated").ok());
  CHECK(!formatSql(Dialect::SQLite,"select (1").ok());
  FormatOptions options;options.maxOutputBytes=4;
  CHECK(!formatSql(Dialect::SQLite,"select 1;",{},options).ok());
  options={};options.language.limits.tokens=2;
  CHECK(!formatSql(Dialect::SQLite,"select a,b,c;",{},options).ok());
  options={};options.language.shouldStop=[]{return true;};
  CHECK(!formatSql(Dialect::SQLite,"select 1;",{},options).ok());
  options={};options.shouldStop=[]{return true;};
  CHECK(!formatSql(Dialect::SQLite,"select 1;",{},options).ok());
  std::string longList="select ";for(int i=0;i<10000;++i){if(i)longList+=',';longList+="c"+std::to_string(i);}longList+=" from t";
  auto many=formatSql(Dialect::PostgreSQL,longList);CHECK(many.ok());
}
void mergeStatementsAndBranchScopes() {
  const std::string pg="MERGE INTO target_t AS t USING source_t AS s ON t.id=s.id WHEN MATCHED AND s.n<0 THEN DELETE WHEN MATCHED THEN UPDATE SET n=s.n WHEN NOT MATCHED THEN INSERT(id,n) VALUES(s.id,s.n);";
  const std::string ora="MERGE INTO target_t t USING source_t s ON(t.id=s.id) WHEN MATCHED THEN UPDATE SET t.n=s.n WHERE s.n>0 DELETE WHERE t.n=0 WHEN NOT MATCHED THEN INSERT(t.id,t.n) VALUES(s.id,s.n) WHERE s.n>0;";
  const std::string ms="MERGE TOP(10) INTO target_t AS t USING source_t AS s ON t.id=s.id WHEN MATCHED AND s.n<0 THEN DELETE WHEN MATCHED THEN UPDATE SET t.n=s.n WHEN NOT MATCHED BY TARGET THEN INSERT(id,n) VALUES(s.id,s.n) WHEN NOT MATCHED BY SOURCE THEN DELETE;";
  for(auto entry:std::vector<std::pair<Dialect,std::string>>{{Dialect::PostgreSQL,pg},{Dialect::Oracle,ora},{Dialect::SQLServer,ms}}) {
    MemoryCatalog catalog;TableDefinition target;target.name=entry.first==Dialect::Oracle?"TARGET_T":"target_t";target.columns={{entry.first==Dialect::Oracle?"ID":"id",ValueType::Integer},{entry.first==Dialect::Oracle?"N":"n",ValueType::Integer}};catalog.tables.push_back(target);target.name=entry.first==Dialect::Oracle?"SOURCE_T":"source_t";catalog.tables.push_back(target);
    auto c=compile(entry.first,entry.second,{},&catalog);success(*c);CHECK(c->model[0].kind==StatementKind::Merge);CHECK(c->model[0].mergeOn!=0);CHECK(c->model[0].tables.size()==2);CHECK(c->model[0].mergeClauses.size()>=2);
    auto generated=generateSql(c->model,entry.first);CHECK(generated.ok());auto again=compile(entry.first,generated.sql,{},&catalog);success(*again);CHECK(generateSql(again->model,entry.first).sql==generated.sql);
    CHECK(dumpAST(*c).find("\"merge\"")!=std::string::npos);CHECK(dumpModel(*c).find("notMatchedTarget")!=std::string::npos);
    auto formatted=formatSql(entry.first,entry.second);CHECK(formatted.ok());success(*compile(entry.first,formatted.sql,{},&catalog));CHECK(formatSql(entry.first,formatted.sql).sql==formatted.sql);
    const std::string prefix = "MERGE INTO target_t t USING source_t s ON (t.id=s.id) "
                               "WHEN NOT MATCHED THEN INSERT ";
    auto missing = compile(entry.first, prefix + "(absent) VALUES (s.id);", {}, &catalog);
    CHECK(std::any_of(missing->diagnostics.begin(), missing->diagnostics.end(), [&](const auto &d) {
      return d.code == DiagnosticCode::UnknownColumn &&
             missing->source.slice(d.range) == "absent";
    }));
    auto extra = compile(entry.first, prefix + "VALUES (s.id,s.n,3);", {}, &catalog);
    CHECK(std::any_of(extra->diagnostics.begin(), extra->diagnostics.end(), [](const auto &d) {
      return d.code == DiagnosticCode::InvalidQueryShape &&
             d.message.find("more values") != std::string::npos;
    }));
  }
  for(auto text:{"MERGE INTO t USING s ON t.id=s.id;","MERGE INTO t USING s ON t.id=s.id WHEN NOT MATCHED THEN UPDATE SET n=1;","MERGE INTO t USING s ON t.id=s.id WHEN MATCHED THEN INSERT(id) VALUES(s.id);","MERGE INTO t USING s ON t.id=s.id WHEN MATCHED THEN DELETE WHEN MATCHED THEN UPDATE SET n=1;"})CHECK(!compile(Dialect::PostgreSQL,text)->ok());
  CHECK(!compile(Dialect::SQLServer,"MERGE INTO t USING s ON t.id=s.id WHEN MATCHED THEN DELETE")->ok());
  CHECK(!compile(Dialect::SQLServer,"MERGE INTO t USING s ON t.id=s.id WHEN MATCHED AND s.id=1 THEN UPDATE SET n=1 WHEN MATCHED THEN UPDATE SET n=2;")->ok());
  CHECK(!compile(Dialect::Oracle,"MERGE INTO t USING s ON(t.id=s.id) WHEN MATCHED THEN UPDATE SET t.id=s.id;")->ok());
  CHECK(!compile(Dialect::Oracle,"MERGE INTO t USING s ON(t.id=s.id) WHEN MATCHED THEN DELETE;")->ok());
  CHECK(!compile(Dialect::PostgreSQL,"MERGE INTO t USING s ON t.id=s.id WHEN MATCHED THEN UPDATE SET n=1,n=2;")->ok());
  CHECK(!compile(Dialect::PostgreSQL,"CREATE TABLE t(n INTEGER CHECK(n));")->ok());
  CHECK(!compile(Dialect::MySQL,pg)->ok());
  MemoryCatalog catalog;catalog.tables={{"","","t",{{"id",ValueType::Integer},{"n",ValueType::Integer}}},{"","","s",{{"id",ValueType::Integer},{"n",ValueType::Integer}}}};
  CHECK(has(*compile(Dialect::PostgreSQL,"MERGE INTO t USING s ON t.id=s.id WHEN NOT MATCHED THEN INSERT(id) VALUES(t.id);",{},&catalog),DiagnosticCode::UnknownColumn));
  CHECK(has(*compile(Dialect::SQLServer,"MERGE INTO t USING s ON t.id=s.id WHEN NOT MATCHED BY SOURCE AND s.n>0 THEN DELETE;",{},&catalog),DiagnosticCode::UnknownColumn));
  success(*compile(Dialect::SQLServer,"MERGE INTO t AS a USING t AS b ON a.id=b.id WHEN MATCHED THEN UPDATE SET a.n=b.n;",{},&catalog));
  success(*compile(Dialect::PostgreSQL,"WITH changes AS(SELECT 1 AS id) MERGE INTO t USING changes s ON t.id=s.id WHEN MATCHED THEN DO NOTHING;",{},&catalog));
}
void scriptDeclarationContexts() {
  {
    MemoryCatalog catalog;
    catalog.tables.push_back({"app", "", "visible_before_role", {{"id", ValueType::Integer}}});
    const NameResolutionContext names{"app", "", Dialect::MySQL};
    const auto session = compile(Dialect::MySQL,
        "SELECT id FROM visible_before_role; SET ROLE NONE; SELECT id FROM visible_before_role;",
        {}, &catalog, names);
    success(*session);
    CHECK(session->model[0].catalogChecked && !session->model[2].catalogChecked);
    const auto account = compile(Dialect::MySQL,
        "SET DEFAULT ROLE NONE TO user1; SELECT id FROM visible_before_role;",
        {}, &catalog, names);
    success(*account);
    CHECK(account->model[1].catalogChecked);
  }
  for(auto dialect:{Dialect::MySQL,Dialect::PostgreSQL,Dialect::SQLServer,Dialect::SQLite}) {
    auto c=compile(dialect,"CREATE TABLE script_owned(id INTEGER,title VARCHAR(30)); SELECT id,title FROM script_owned;");success(*c);
    const auto &q=c->model.at(1);CHECK(q.catalogChecked);CHECK(q.expressions.at(q.projections[0].expression).binding.has_value());
    CHECK(has(*compile(dialect,"CREATE TABLE script_owned(id INTEGER);SELECT missing FROM script_owned;"),DiagnosticCode::UnknownColumn));
    CHECK(has(*compile(dialect,"CREATE TABLE script_owned(id INTEGER);DROP TABLE script_owned;SELECT id FROM script_owned;"),DiagnosticCode::UnknownTable));
  }
  success(*compile(Dialect::PostgreSQL,"CREATE TABLE script_owned(id INTEGER);ALTER TABLE script_owned ADD COLUMN title TEXT;ALTER TABLE script_owned RENAME COLUMN title TO renamed;SELECT renamed FROM script_owned;"));
  CHECK(has(*compile(Dialect::PostgreSQL,"CREATE TABLE script_owned(id INTEGER);ALTER TABLE script_owned DROP COLUMN id;SELECT id FROM script_owned;"),DiagnosticCode::UnknownColumn));
  success(*compile(Dialect::PostgreSQL,"CREATE TABLE script_owned AS SELECT 1 AS id;CREATE VIEW script_view AS SELECT id FROM script_owned;SELECT id FROM script_view;"));
  success(*compile(Dialect::PostgreSQL,"CREATE TABLE script_owned(id INTEGER);ALTER TABLE script_owned RENAME TO renamed_owned;SELECT id FROM renamed_owned;"));
  success(*compile(Dialect::PostgreSQL,"CREATE TABLE IF NOT EXISTS script_owned(id INTEGER);SELECT missing FROM script_owned;"));
  MemoryCatalog absent;CHECK(has(*compile(Dialect::PostgreSQL,"CREATE TABLE IF NOT EXISTS script_owned(id INTEGER);SELECT missing FROM script_owned;",{},&absent),DiagnosticCode::UnknownColumn));
  CHECK(!compile(Dialect::PostgreSQL,"CREATE TABLE script_owned(id INTEGER);ALTER TABLE script_owned ADD COLUMN id INTEGER;")->ok());
  CHECK(!compile(Dialect::PostgreSQL,"CREATE TABLE script_owned(id INTEGER);ALTER TABLE script_owned DROP COLUMN missing;")->ok());
  auto separate=compile(Dialect::PostgreSQL,"SELECT id FROM script_owned;");success(*separate);CHECK(!separate->model[0].catalogChecked);
  CHECK(has(*compile(Dialect::PostgreSQL,"CREATE TABLE script_owned AS SELECT 1 AS id;CREATE VIEW script_view AS SELECT id FROM script_owned;SELECT missing FROM script_view;"),DiagnosticCode::UnknownColumn));
}
void filteredAggregates() {
  for(auto dialect:{Dialect::PostgreSQL,Dialect::SQLite}) {
    for(auto text:{"SELECT SUM(x) FILTER(WHERE x>1) FROM t;","SELECT COUNT(*) FILTER(WHERE x IS NOT NULL) OVER w FROM t WINDOW w AS(ORDER BY x);","SELECT SUM(SUM(x)) FILTER(WHERE SUM(x)>0) OVER() FROM t;"}) {
      auto c=compile(dialect,text);
      if(dialect==Dialect::PostgreSQL && std::string(text).find("WHERE SUM")!=std::string::npos){CHECK(!c->ok());continue;}
      success(*c);auto generated=generateSql(c->model,dialect);CHECK(generated.ok());success(*compile(dialect,generated.sql));
    }
    for(auto text:{"SELECT ROW_NUMBER() FILTER(WHERE x>0) OVER() FROM t;","SELECT ABS(x) FILTER(WHERE x>0) FROM t;","SELECT SUM(x) FILTER(WHERE MAX(x)>0) FROM t;","SELECT SUM(x) FILTER(WHERE ROW_NUMBER() OVER()>0) FROM t;","SELECT 1 FROM t WHERE SUM(x) FILTER(WHERE x>0)>1;"})CHECK(!compile(dialect,text)->ok());
  }
  CHECK(!compile(Dialect::PostgreSQL,"SELECT COUNT(*) FILTER(WHERE 1) FROM t;")->ok());
  success(*compile(Dialect::SQLite,"SELECT COUNT(*) FILTER(WHERE 1) FROM t;"));
  for(auto dialect:{Dialect::MySQL,Dialect::Oracle,Dialect::SQLServer})CHECK(!compile(dialect,"SELECT COUNT(*) FILTER(WHERE x>0) FROM t;")->ok());
}
void predicateAndScalarEnvironments() {
  for(auto dialect:{Dialect::SQLServer,Dialect::Oracle}) {
    const auto from=dialect==Dialect::Oracle?" FROM SYS.DUAL":"";
    CHECK(!compile(dialect,"SELECT 1=1"+std::string(from)+";")->ok());
    CHECK(!compile(dialect,"SELECT TRUE"+std::string(from)+";")->ok());
    CHECK(!compile(dialect,"SELECT 1"+std::string(from)+" WHERE NULL;")->ok());
    success(*compile(dialect,"SELECT CASE WHEN 1=1 THEN 1 ELSE 0 END"+std::string(from)+";"));
  }
  success(*compile(Dialect::PostgreSQL,"SELECT 1=1 WHERE NULL;"));
  CHECK(!compile(Dialect::SQLServer,"CREATE TABLE t(flag BIT);SELECT 1 FROM t WHERE flag;")->ok());
  success(*compile(Dialect::SQLServer,"CREATE TABLE t(flag BIT);SELECT flag FROM t WHERE flag=1;"));
  success(*compile(Dialect::SQLServer,"SELECT 1 FROM t WHERE CONTAINS(note,'hello');"));
  success(*compile(Dialect::Oracle,"SELECT 1 FROM t WHERE REGEXP_LIKE(note,'hello');"));
}
void conditionTypesAndQuotedRoutines() {
  for(auto dialect:{Dialect::PostgreSQL,Dialect::Oracle,Dialect::SQLServer}) {
    auto sql=[&](std::string text){
      if(dialect==Dialect::Oracle && text.rfind("SELECT",0)==0) {
        auto where=text.find("WHERE");
        text.insert(where==std::string::npos?text.size()-1:where," FROM SYS.DUAL ");
      }
      return text;
    };
    for(auto text:{"SELECT 1 WHERE 1;","SELECT 1 WHERE 1=1 AND 2;","SELECT CASE WHEN 1 THEN 2 ELSE 3 END;","CREATE TABLE t(n INTEGER,CHECK(1));"})
      CHECK(has(*compile(dialect,sql(text)),DiagnosticCode::InvalidQueryShape));
    success(*compile(dialect,sql("SELECT 1 WHERE 1=1 AND 2=2;")));
    success(*compile(dialect,sql("SELECT CASE WHEN 1=1 THEN 2 ELSE 3 END;")));
  }
  success(*compile(Dialect::MySQL,"SELECT 1 WHERE 1;"));
  success(*compile(Dialect::SQLite,"SELECT 1 WHERE 1;"));
  success(*compile(Dialect::PostgreSQL,"SELECT 1 WHERE 'true';"));
  success(*compile(Dialect::PostgreSQL,"SELECT \"ROW_NUMBER\"(1,2);"));
  CHECK(!compile(Dialect::PostgreSQL,"SELECT row_number(1,2);")->ok());
}
void mysqlInspectionStatements() {
  const std::vector<std::string> statements = {
    "SHOW DATABASES LIKE 'app%';", "SHOW SCHEMAS WHERE `Database`='app';",
    "SHOW EXTENDED FULL TABLES IN app LIKE 'i%';",
    "SHOW FULL TABLES FROM app WHERE table_type='VIEW';",
    "SHOW COLUMNS FROM app.items;", "SHOW FULL FIELDS IN items FROM app WHERE field='id';",
    "SHOW EXTENDED INDEXES IN app.items WHERE non_unique=0;",
    "SHOW KEYS FROM items IN app;", "SHOW TABLE STATUS FROM app WHERE name='items';",
    "SHOW GLOBAL VARIABLES LIKE 'max_%';", "SHOW SESSION STATUS WHERE variable_name='Questions';",
    "SHOW FULL PROCESSLIST;", "SHOW CREATE DATABASE IF NOT EXISTS app;",
    "SHOW CREATE TABLE app.items;", "SHOW CREATE VIEW app.v;",
    "SHOW CREATE FUNCTION app.f;", "SHOW CREATE PROCEDURE app.p;",
    "SHOW CREATE TRIGGER app.tr;", "SHOW CREATE EVENT app.e;",
    "DESCRIBE app.items;", "DESC app.items id;", "DESCRIBE app.items 'id%';"};
  for (const auto &text : statements) {
    auto result = compile(Dialect::MySQL, text); success(*result);
    CHECK(result->model[0].kind == StatementKind::Inspect);
    CHECK(result->model[0].inspection.has_value());
    CHECK(result->model[0].transactionEffect == TransactionEffect::PreservesTransaction);
    const auto sql = generateSql(result->model, Dialect::MySQL); CHECK(sql.ok());
    auto reparsed = compile(Dialect::MySQL, sql.sql); success(*reparsed);
    CHECK(generateSql(reparsed->model, Dialect::MySQL).sql == sql.sql);
    CHECK(dumpAST(*result).find("inspection") != std::string::npos);
    CHECK(dumpModel(*result).find("inspection") != std::string::npos);
    CHECK(!generateSql(result->model, Dialect::PostgreSQL).ok());
    CHECK(!generateSql(result->model, Dialect::MariaDB).ok());
    auto formatted = formatSql(Dialect::MySQL, text); CHECK(formatted.ok());
    success(*compile(Dialect::MySQL, formatted.sql));
  }
  MemoryCatalog catalog;
  TableDefinition table; table.database="app"; table.name="items";
  table.columns={{"id", ValueType::Integer}}; catalog.tables.push_back(table);
  const NameResolutionContext names{"app", "", Dialect::MySQL};
  success(*compile(Dialect::MySQL, "SHOW FULL COLUMNS FROM items WHERE field='id';", {}, &catalog, names));
  CHECK(has(*compile(Dialect::MySQL, "SHOW COLUMNS FROM missing;", {}, &catalog, names), DiagnosticCode::UnknownTable));
  CHECK(has(*compile(Dialect::MySQL, "SHOW COLUMNS FROM items WHERE missing=1;", {}, &catalog, names), DiagnosticCode::UnknownColumn));
  CHECK(has(*compile(Dialect::MySQL, "SHOW TABLES FROM app WHERE missing=1;"), DiagnosticCode::UnknownColumn));
  success(*compile(Dialect::MySQL, "SHOW TABLES WHERE Tables_in_unknown='x';"));
  CHECK(has(*compile(Dialect::MySQL, "SHOW TABLES FROM app WHERE COUNT(*)>1;"), DiagnosticCode::InvalidQueryShape));
  for (auto text : {"SHOW TABLES LIKE 1;", "SHOW TABLES LIKE 'x' WHERE a=1;",
                    "SHOW FULL DATABASES;", "SHOW GLOBAL TABLES;", "SHOW EXTENDED STATUS;",
                    "SHOW INDEX FROM app.items LIKE 'id';", "SHOW COLUMNS items;",
                    "SHOW CREATE DATABASE app.other;", "SHOW COLUMNS FROM app.items FROM other;",
                    "WITH x AS (SELECT 1) SHOW TABLES;", "SHOW TABLES junk;"})
    CHECK(!compile(Dialect::MySQL, text)->ok());
  CHECK(has(*compile(Dialect::MySQL, "SHOW ENGINE INNODB STATUS;"), DiagnosticCode::UnsupportedSyntax));
  CHECK(has(*compile(Dialect::MySQL, "DESCRIBE SELECT 1;"), DiagnosticCode::UnsupportedSyntax));
  for (auto d : {Dialect::PostgreSQL, Dialect::Oracle, Dialect::SQLServer, Dialect::SQLite})
    CHECK(has(*compile(d, "SHOW TABLES;"), DiagnosticCode::UnsupportedSyntax));
  success(*compile(Dialect::MariaDB, "SHOW TABLES;"));
  success(*compile(Dialect::OceanBase, "SHOW COLUMNS FROM t;"));
  auto recovered = compile(Dialect::MySQL, "SHOW TABLES LIKE 1; SELECT 2;");
  CHECK(!recovered->ok()); CHECK(recovered->model.empty());
  CHECK(recovered->statements.size() == 1);
  CHECK(recovered->statements[0].kind == StatementKind::Select);
}
void groupingSetsAndBoundedExpansion() {
  MemoryCatalog catalog;TableDefinition table;table.name="t";table.columns={{"a",ValueType::Integer},{"b",ValueType::Integer},{"c",ValueType::Integer}};table.primaryKeyKnown=true;table.primaryKey={"a"};catalog.tables.push_back(table);
  for(auto d:{Dialect::PostgreSQL,Dialect::Oracle,Dialect::SQLServer}) {
    for(auto group:{"GROUPING SETS ((a,b),(a),())","ROLLUP(a,b)","CUBE(a,b)","a,GROUPING SETS((b),())","ROLLUP((a,b),c)","()"}) {
      auto sql=std::string("SELECT COUNT(*) FROM t GROUP BY ")+group+";";
      auto result=compile(d,sql);success(*result);CHECK(!result->model[0].groupingSets.empty());
      auto generated=generateSql(result->model,d);CHECK(generated.ok());success(*compile(d,generated.sql));
      CHECK(dumpModel(*result).find("groupingSets")!=std::string::npos);
    }
  }
  auto result=compile(Dialect::PostgreSQL,"SELECT a,b,COUNT(*),GROUPING(a,b) FROM t GROUP BY CUBE(a,b);",{},&catalog);success(*result);CHECK(result->model[0].groupingSets.size()==4);
  result=compile(Dialect::PostgreSQL,"SELECT COUNT(*) FROM t GROUP BY GROUPING SETS ((),());");success(*result);CHECK(result->model[0].groupingSets.size()==2);
  CHECK(!compile(Dialect::PostgreSQL,"SELECT c FROM t GROUP BY GROUPING SETS((a),());",{},&catalog)->ok());
  CHECK(!compile(Dialect::PostgreSQL,"SELECT GROUPING(c) FROM t GROUP BY CUBE(a,b);",{},&catalog)->ok());
  CHECK(!compile(Dialect::PostgreSQL,"SELECT GROUPING(1);")->ok());
  CHECK(!compile(Dialect::PostgreSQL,"SELECT a FROM t GROUP BY ();",{},&catalog)->ok());
  success(*compile(Dialect::PostgreSQL,"SELECT (a+1)*2,COUNT(*) FROM t GROUP BY (a+1)*2;",{},&catalog));
  success(*compile(Dialect::PostgreSQL,"SELECT COUNT(*) FROM t GROUP BY (SELECT 1);",{},&catalog));
  success(*compile(Dialect::PostgreSQL,"SELECT COUNT(*) FROM t GROUP BY (a)=1;",{},&catalog));
  success(*compile(Dialect::PostgreSQL,"SELECT COUNT(*) FROM t GROUP BY (a) IS NULL;",{},&catalog));
  result=compile(Dialect::MySQL,"SELECT a,b,COUNT(*),GROUPING(a,b) FROM t GROUP BY a,b WITH ROLLUP;",{},&catalog);success(*result);CHECK(result->model[0].groupingSets.size()==3);
  auto sql=generateSql(result->model,Dialect::MySQL);CHECK(sql.ok());CHECK(sql.sql.find("WITH ROLLUP")!=std::string::npos);success(*compile(Dialect::MySQL,sql.sql));
  CHECK(!compile(Dialect::SQLite,"SELECT COUNT(*) FROM t GROUP BY CUBE(a);")->ok());
  CHECK(has(*compile(Dialect::MySQL,"SELECT COUNT(*) FROM t GROUP BY GROUPING SETS((a),());"),DiagnosticCode::UnsupportedSyntax));
  CHECK(has(*compile(Dialect::MySQL,"SELECT COUNT(*) FROM t GROUP BY CUBE(a);"),DiagnosticCode::UnsupportedSyntax));
  LanguageOptions small;small.limits.groupingSets=3;
  CHECK(has(*compile(Dialect::PostgreSQL,"SELECT COUNT(*) FROM t GROUP BY CUBE(a,b);",small),DiagnosticCode::ResourceLimit));
  small.limits.groupingSets=256;small.limits.groupingReferences=3;
  CHECK(has(*compile(Dialect::PostgreSQL,"SELECT COUNT(*) FROM t GROUP BY CUBE(a,b,c);",small),DiagnosticCode::ResourceLimit));
  CHECK(has(*compile(Dialect::PostgreSQL,"SELECT COUNT(*) FROM t GROUP BY CUBE(a,b,c,d,e,f,g,h,i);"),DiagnosticCode::ResourceLimit));
}
void operatorCatalogAndRoutineFacts() {
  using RK=RoutineKind;using FV=FunctionVolatility;
  CHECK(builtinOperator(Dialect::PostgreSQL,{"pg_catalog","sum"},{},1)->kind==RK::Aggregate);
  CHECK(!builtinOperator(Dialect::PostgreSQL,{"pg_catalog","SUM"},{false,true},1));
  CHECK(!builtinOperator(Dialect::PostgreSQL,{"pg_catalog","coalesce"},{},2));
  CHECK(!builtinOperator(Dialect::SQLite,{"date_add"},{},2));
  CHECK(has(*compile(Dialect::PostgreSQL,"SELECT lower(1,2);"),DiagnosticCode::RoutineArgumentCount));
  CHECK(has(*compile(Dialect::MySQL,"SELECT replace('a','b');"),DiagnosticCode::RoutineArgumentCount));
  CHECK(!builtinOperator(Dialect::PostgreSQL,{"app","sum"},{},1));
  CHECK(builtinOperator(Dialect::SQLite,{"max"},{},2)->kind==RK::Scalar);
  CHECK(builtinOperator(Dialect::SQLite,{"max"},{},1)->kind==RK::Aggregate);
  CHECK(projection(*compile(Dialect::SQLServer,"SELECT AVG(1); ")).type==ValueType::Integer);
  CHECK(projection(*compile(Dialect::PostgreSQL,"SELECT AVG(1); ")).type==ValueType::Decimal);
  MemoryCatalog catalog;TableDefinition table;table.name="t";table.columns={{"x",ValueType::Integer}};
  catalog.tables.push_back(table);
  RoutineDefinition f;f.name="own_total";f.minArguments=f.maxArguments=1;f.resultType=ValueType::Integer;
  f.kind=RK::Aggregate;f.volatility=FV::Immutable;catalog.routines.push_back(f);
  auto c=compile(Dialect::PostgreSQL,"SELECT own_total(x) FROM t;",{},&catalog);success(*c);
  CHECK(projection(*c).routineResolved);CHECK(projection(*c).routineKind==RK::Aggregate);
  CHECK(projection(*c).routineVolatility==FV::Immutable);
  CHECK(!compile(Dialect::PostgreSQL,"SELECT x,own_total(x) FROM t;",{},&catalog)->ok());
  CHECK(!compile(Dialect::PostgreSQL,"SELECT x FROM t WHERE own_total(x)>1;",{},&catalog)->ok());
  CHECK(!compile(Dialect::PostgreSQL,"SELECT own_total(own_total(x)) FROM t;",{},&catalog)->ok());
  success(*compile(Dialect::PostgreSQL,"SELECT own_total(x) OVER() FROM t;",{},&catalog));
  f.name="own_window";f.kind=RK::Window;catalog.routines.push_back(f);
  CHECK(!compile(Dialect::PostgreSQL,"SELECT own_window(x) FROM t;",{},&catalog)->ok());
  success(*compile(Dialect::PostgreSQL,"SELECT own_window(x) OVER() FROM t;",{},&catalog));
  f.name="own_total";f.kind=RK::Scalar;f.volatility=FV::Volatile;catalog.routines.push_back(f);
  c=compile(Dialect::PostgreSQL,"SELECT own_total(x) FROM t;",{},&catalog);success(*c);
  CHECK(projection(*c).routineKind==RK::Unknown);CHECK(projection(*c).routineVolatility==FV::Volatile);
  f.name="own_scalar";f.kind=RK::Scalar;catalog.routines.push_back(f);
  c=compile(Dialect::PostgreSQL,"SELECT own_scalar(x) FROM t;",{},&catalog);success(*c);
  CHECK(dumpModel(*c).find("volatile")!=std::string::npos);
}
void outputAliasScopes() {
  for(auto dialect:{Dialect::MySQL,Dialect::PostgreSQL,Dialect::SQLite}) {
    MemoryCatalog catalog; TableDefinition table;
    table.name="t"; table.columns={{"x",ValueType::Integer},{"g",ValueType::Integer}};
    catalog.tables.push_back(table);
    auto c=compile(dialect,"SELECT x+1 AS z FROM t GROUP BY z ORDER BY z;",{},&catalog);
    success(*c);
    const auto &q=c->model[0];
    CHECK(q.expressions.at(q.groupBy[0]).outputColumn==0);
    CHECK(q.expressions.at(q.orderBy[0].expression).outputColumn==0);
    c=compile(dialect,"SELECT x AS g FROM t GROUP BY g ORDER BY g;",{},&catalog);
    success(*c);
    const auto &collision=c->model[0];
    CHECK(!collision.expressions.at(collision.groupBy[0]).outputColumn);
    CHECK(collision.expressions.at(collision.groupBy[0]).binding.has_value());
    CHECK(collision.expressions.at(collision.orderBy[0].expression).outputColumn==0);
    CHECK(!compile(dialect,"SELECT SUM(x) AS z FROM t GROUP BY z;",{},&catalog)->ok());
    if(dialect==Dialect::MySQL || dialect==Dialect::SQLite) {
      success(*compile(dialect,"SELECT SUM(x) AS total FROM t HAVING total>1;",{},&catalog));
      CHECK(!compile(dialect,"SELECT ROW_NUMBER() OVER(ORDER BY x) AS rn FROM t HAVING rn>1;",{},&catalog)->ok());
    } else CHECK(has(*compile(dialect,"SELECT SUM(x) AS total FROM t HAVING total>1;",{},&catalog),DiagnosticCode::UnknownColumn));
  }
}
const SqlOutlineNode *outlineNode(const std::vector<SqlOutlineNode> &nodes,
                                  std::string_view prefix) {
  for (const auto &node : nodes) {
    if (node.label.compare(0, prefix.size(), prefix) == 0) return &node;
    if (auto *child = outlineNode(node.children, prefix)) return child;
  }
  return nullptr;
}

void sourceOutlineNavigatesNestedSql() {
  const std::string sql = u8"-- 数据 😀\n"
    "WITH recent AS (SELECT id FROM orders) "
    "SELECT r.id FROM recent r JOIN customers c ON r.id=c.id "
    "WHERE EXISTS (SELECT 1 FROM audit_log);\n"
    "CREATE TABLE owned (id INT, title VARCHAR(8), CONSTRAINT pk PRIMARY KEY(id));\n"
    "ALTER TABLE owned ADD COLUMN flag INT;\n"
    "DROP TABLE owned;";
  const auto outline = outlineSql(Dialect::PostgreSQL, sql);
  CHECK(!outline.partial && !outline.truncated && outline.roots.size() == 4);
  CHECK(outline.roots[0].range.begin == sql.find("WITH"));
  CHECK(outlineNode(outline.roots[0].children, "CTE recent"));
  CHECK(outlineNode(outline.roots[0].children, "JOIN customers"));
  CHECK(outlineNode(outline.roots[0].children, "SUBQUERY"));
  CHECK(outlineNode(outline.roots[1].children, "COLUMN title"));
  CHECK(outlineNode(outline.roots[1].children, "PRIMARY KEY pk"));
  CHECK(outlineNode(outline.roots[2].children, "ADD COLUMN flag"));
  CHECK(outline.roots.back().label == "DROP TABLE owned");
  CHECK(sql.substr(outline.roots.back().range.begin) == "DROP TABLE owned;");

  const std::string mysql =
      "DELIMITER $$\n"
      "CREATE PROCEDURE owned(IN p INT) BEGIN\n"
      "  DECLARE x INT DEFAULT 0;\n"
      "  DECLARE CONTINUE HANDLER FOR NOT FOUND SET x = 1;\n"
      "  IF p > 0 THEN SELECT '分号;' AS note; ELSE SET x = 2; END IF;\n"
      "END$$\nDELIMITER ;\nDROP TABLE owned;";
  const auto routine = outlineSql(Dialect::MySQL, mysql);
  CHECK(routine.roots.size() == 2 && !routine.truncated);
  CHECK(outlineNode(routine.roots[0].children, "HEADER"));
  CHECK(outlineNode(routine.roots[0].children, "PARAM p"));
  CHECK(outlineNode(routine.roots[0].children, "BODY"));
  CHECK(outlineNode(routine.roots[0].children, "DECLARE CONTINUE HANDLER"));
  CHECK(outlineNode(routine.roots[0].children, "IF p > 0"));
  auto *select = outlineNode(routine.roots[0].children, "SELECT '分号;'");
  CHECK(select && mysql.substr(select->range.begin, select->range.end - select->range.begin) ==
                     u8"SELECT '分号;' AS note;");
  CHECK(mysql.at(routine.roots[0].range.end) == '$');

  const std::string pg =
      "CREATE FUNCTION owned() RETURNS INT AS $runner$\n"
      "BEGIN RETURN 3; EXCEPTION WHEN OTHERS THEN RETURN 4; END;\n"
      "$runner$ LANGUAGE plpgsql;\nSELECT 2;";
  const auto function = outlineSql(Dialect::PostgreSQL, pg);
  CHECK(function.roots.size() == 2);
  auto *returned = outlineNode(function.roots[0].children, "RETURN 4");
  CHECK(returned && pg.substr(returned->range.begin, returned->range.end - returned->range.begin) == "RETURN 4;");
  CHECK(outlineNode(function.roots[0].children, "EXCEPTION"));
  CHECK(outlineNode(function.roots[0].children, "WHEN OTHERS"));

  const std::string oracle =
      "CREATE PACKAGE owned AS PROCEDURE save_value(p IN NUMBER); END owned;\n/\n"
      "CREATE OR REPLACE PACKAGE BODY owned AS\n"
      "  PROCEDURE save_value(p IN NUMBER) IS local_id NUMBER;\n"
      "  BEGIN IF p > 0 THEN local_id := p; ELSE NULL; END IF;\n"
      "  EXCEPTION WHEN OTHERS THEN NULL; END save_value;\n"
      "  FUNCTION get_value RETURN NUMBER IS BEGIN RETURN 1; END get_value;\n"
      "BEGIN NULL; END owned;\n/\nDROP PACKAGE BODY owned;";
  const auto package = outlineSql(Dialect::Oracle, oracle);
  CHECK(package.roots.size() == 3 && package.partial);
  CHECK(outlineNode(package.roots[0].children, "PARAM p"));
  CHECK(package.roots[1].label == "CREATE PACKAGE BODY owned");
  CHECK(outlineNode(package.roots[1].children, "PROCEDURE save_value"));
  CHECK(outlineNode(package.roots[1].children, "FUNCTION get_value"));
  CHECK(outlineNode(package.roots[1].children, "WHEN OTHERS"));
  CHECK(outlineNode(package.roots[1].children, "local_id := p"));
  CHECK(package.roots[2].label == "DROP PACKAGE BODY owned");
  CHECK(oracle.substr(package.roots[2].range.begin) == "DROP PACKAGE BODY owned;");

  const auto trigger = outlineSql(Dialect::MySQL,
      "DELIMITER $$\nCREATE TRIGGER owned BEFORE INSERT ON src FOR EACH ROW "
      "BEGIN IF NEW.id > 10 THEN SET NEW.id = 10; END IF; END$$\nDELIMITER ;");
  CHECK(trigger.roots.size() == 1);
  CHECK(outlineNode(trigger.roots[0].children, "IF NEW.id"));
  CHECK(outlineNode(trigger.roots[0].children, "SET NEW.id"));
  const auto sqlite = outlineSql(Dialect::SQLite,
      "CREATE TRIGGER owned AFTER INSERT ON src BEGIN INSERT INTO log VALUES (NEW.id); END; SELECT 1;");
  CHECK(sqlite.roots.size() == 2 && !sqlite.partial);
  CHECK(outlineNode(sqlite.roots[0].children, "HEADER"));
  CHECK(outlineNode(sqlite.roots[0].children, "BODY"));
  CHECK(outlineNode(sqlite.roots[0].children, "INSERT INTO log"));
  const auto sqlServer = outlineSql(Dialect::SQLServer,
      "CREATE OR ALTER PROCEDURE dbo.owned AS BEGIN TRY SELECT 1; "
      "END TRY BEGIN CATCH THROW; END CATCH;\nGO\nDROP TABLE dbo.owned;");
  CHECK(sqlServer.roots.size() == 2);
  CHECK(outlineNode(sqlServer.roots[0].children, "TRY"));
  CHECK(outlineNode(sqlServer.roots[0].children, "CATCH"));
  CHECK(outlineNode(sqlServer.roots[0].children, "THROW;"));
  const auto bareSqlServer = outlineSql(Dialect::SQLServer,
      "CREATE PROC dbo.p AS SELECT 1; IF 1=1 SELECT 2 ELSE SELECT 3; "
      "SELECT 4;\nGO\nDROP TABLE dbo.items;");
  CHECK(bareSqlServer.roots.size() == 2);
  CHECK(outlineNode(bareSqlServer.roots[0].children, "SELECT 1"));
  const auto *tsqlIf = outlineNode(bareSqlServer.roots[0].children, "IF 1=1");
  CHECK(tsqlIf && outlineNode(tsqlIf->children, "SELECT 2"));
  CHECK(outlineNode(tsqlIf->children, "SELECT 3"));
  CHECK(outlineNode(bareSqlServer.roots[0].children, "SELECT 4"));
  const auto pgTrigger = outlineSql(Dialect::PostgreSQL,
      "CREATE TRIGGER owned AFTER INSERT ON src FOR EACH ROW EXECUTE FUNCTION notify_change();");
  CHECK(pgTrigger.roots.size() == 1);
  CHECK(outlineNode(pgTrigger.roots[0].children, "EXECUTE FUNCTION notify_change"));
  const auto incomplete = outlineSql(Dialect::PostgreSQL, "SELECT 1;\nSELECT (");
  CHECK(incomplete.roots.size() == 2 && incomplete.partial);
  LanguageOptions small;
  small.limits.nodes = 2;
  CHECK(outlineSql(Dialect::PostgreSQL, sql, small).truncated);
}

void formattingConfiguration() {
  for(auto preset:{"Readable","Compact","Expanded"}){
    const auto style=formatPreset(preset);const auto text=dumpFormatStyle(style);
    CHECK(dumpFormatStyle(parseFormatStyle(text))==text);
  }
  auto style=parseFormatStyle("BasedOnStyle: Expanded\nIndentWidth: 3\nKeywordCase: Lower\nColumnLimit: 0\n");
  CHECK(style.indentWidth==3);CHECK(style.keywordCase==KeywordCase::Lower);CHECK(style.columnLimit==0);
  for(auto bad:{"IndentWidth: 0","ColumnLimit: 30","Unknown: true","UseTabs: yes","IndentWidth: 2\nIndentWidth: 4","SelectList: Wrong","BasedOnStyle: Other"}){
    bool threw=false;try{parseFormatStyle(bad);}catch(const std::exception&){threw=true;}CHECK(threw);
  }
}
} // namespace
int main() {
  const std::vector<std::pair<const char *, std::function<void()>>> tests = {
      {"dialects", dialects},
      {"precedence", precedence},
      {"mysql", mysql},
      {"postgresql", postgres},
      {"oracle", oracle},
      {"sqlserver", sqlserver},
      {"sqlite", sqlite},
      {"scope and catalog", scopes},
      {"TiDB native grammar, escape bytes and default expressions",tidbNativeGrammarEdges},
      {"MySQL-family native function spacing and IGNORE_SPACE",mysqlNativeFunctionSpacing},
      {"ALL quantifier normalization and native aggregate signatures",explicitFunctionAll},
      {"Oracle anonymous inline view binding and generation",oracleAnonymousInlineViews},
      {"SQLite predicate precedence and asymmetric BETWEEN bounds",sqlitePredicatePrecedence},
      {"SQL Server DDL grammar and generation boundaries",sqlServerDdlBoundaries},
      {"SQL Server filtered-index shapes and DISTINCT predicate",sqlServerFilteredIndexes},
      {"SQL Server structured JSON constructors and result types",sqlServerJsonConstructors},
      {"SQL Server structured CONVERT/TRY conversions and type binding",sqlServerConversions},
      {"PostgreSQL bare projection keywords and named-type literals",postgresProjectionKeywords},
      {"PostgreSQL column, function, type and relation name contexts",postgresIdentifierContexts},
      {"PostgreSQL pagination expressions, binding and primary grammar",postgresPaginationExpressions},
      {"SQLite pagination expressions, comma order and isolated scope",sqlitePaginationExpressions},
      {"PostgreSQL 16+ anonymous derived-table binding and generation",postgresAnonymousDerivedTables},
      {"PostgreSQL non-associative operators and restricted BETWEEN bounds",postgresNonAssociativeExpressions},
      {"Oracle 19c literal/arithmetic row limiting and explicit scope",oraclePaginationLiterals},
      {"SQL Server assignment/string aliases and boolean-word identifiers",sqlServerProjectionAliases},
      {"SQL Server arithmetic row limits and TOP token boundaries",sqlServerPaginationExpressions},
      {"SQLite GLOB predicates, nullability and target boundaries",sqliteGlobPredicates},
      {"TiDB closed column types, constraints and generated-column rules",tidbDdlProductionBoundaries},
      {"SQL Server keyword roles, value keywords and batch boundaries",sqlServerKeywordContexts},
      {"PostgreSQL zero-column tables and server-chosen index names",postgresqlOptionalDdlNames},
      {"MySQL-family regex predicate structure and boundaries",mysqlRegexpGrammar},
      {"MySQL-family DELETE targets, aliases and modifier productions",mysqlDeleteProductions},
      {"MySQL-family window argument productions and routine-local binding",mysqlWindowArgumentProductions},
      {"MySQL-family temporal grammar and intrinsic types",mysqlTemporalGrammar},
      {"upstream grammar rejection and dialect boundaries",upstreamGrammarBoundaries},
      {"explicit LIKE ESCAPE syntax and generation",explicitLikeEscape},
      {"sqlite empty IN and compact null predicates", sqliteEmptyInAndNullTests},
      {"source and recovery", sourceAndRecovery},
      {"limits and long chains", limits},
      {"ownership and parallelism", ownershipAndParallelism},
      {"nested queries and CASE", nestedQueriesAndCases},
      {"correlated scope and model ownership", correlatedScopeAndOwnership},
      {"inferred shape budgets", inferredShapeBudgets},
      {"DML statements and ownership", dmlStatementsAndOwnership},
      {"DML dialect extensions", dmlDialectExtensions},
      {"DML binding and errors", dmlBindingAndErrors},
      {"JOIN trees and merged columns", joinTreesAndMergedColumns},
      {"subquery predicates and limits", subqueryPredicatesAndLimits},
      {"routine calls and catalog validation",
       routineCallsAndCatalogValidation},
      {"DDL core and owning model", ddlCoreAndOwningModel},
      {"DDL dialect cases", ddlDialectCases},
      {"DDL boundaries and transaction effects", ddlBoundariesAndEffects},
      {"DDL validation and literal modes", ddlValidationAndLiteralModes},
      {"canonical SQL round trips", canonicalSqlRoundTrips},
      {"generation safety and budgets", generationSafetyAndBudgets},
      {"set operations and output scopes",setOperationsAndOutputScopes},
      {"duplicate assignments",duplicateAssignmentsRespectDialectRules},
      {"NULL-safe and pattern predicates",nullSafeAndPatternPredicates},
      {"positional and DISTINCT order scopes",positionalAndDistinctOrderScopes},
      {"grouped column semantics",groupedColumnSemantics},
      {"PostgreSQL source structure",postgresqlSourceStructure},
      {"named windows and semantic rules",namedWindowsAndSemanticRules},
      {"windows and dialect diagnostics",windowsAndDialectDiagnostics},
      {"cross-dialect clause semantics", crossDialectClauseSemantics},
      {"window exclusions and typed frames",windowExclusionsAndTypedFrames},
      {"source-preserving formatting",sourcePreservingFormatting},
      {"MERGE statements and branch scopes",mergeStatementsAndBranchScopes},
      {"script declaration context",scriptDeclarationContexts},
      {"filtered aggregate semantics",filteredAggregates},
      {"predicate/scalar environments",predicateAndScalarEnvironments},
      {"condition types and quoted routines",conditionTypesAndQuotedRoutines},
      {"output aliases and clause scopes",outputAliasScopes},
      {"operator catalog and resolved routine facts",operatorCatalogAndRoutineFacts},
      {"MySQL typed inspection statements",mysqlInspectionStatements},
      {"grouping sets and bounded expansion",groupingSetsAndBoundedExpansion},
      {"formatting configuration",formattingConfiguration},
      {"SQL outline and original source ranges",sourceOutlineNavigatesNestedSql},
      {"malformed inputs", malformedInputs}};
  int failed = 0;
  for (const auto &test : tests) {
    try {
      test.second();
      std::cout << "PASS " << test.first << '\n';
    } catch (const std::exception &error) {
      ++failed;
      std::cerr << "FAIL " << test.first << ": " << error.what() << '\n';
    }
  }
  return failed ? 1 : 0;
}
