#include "navidog/sql/compiler/compiler.hpp"
#include "navidog/sql/compiler/conversion.hpp"
#include "navidog/sql/compiler/script_context.hpp"
#include <future>
#include <iostream>
#include <stdexcept>
using namespace navidog::sql::compiler;
void check(bool yes,const char *text){if(!yes)throw std::runtime_error(text);}
std::unique_ptr<Compilation> parse(Dialect d,const std::string &sql,const CatalogSnapshot *catalog=nullptr,NameResolutionContext names={}) {
  auto c=compile(d,sql,{},catalog,std::move(names));
  if(!c->ok())throw std::runtime_error(c->diagnostics.empty()?"no model":c->diagnostics.front().message+" in "+sql);
  return c;
}
SqlType declared(Dialect d,const std::string &text) {
  auto c=parse(d,"CREATE TABLE t(c "+text+");");
  auto type=resolveDeclaredType(c->model[0].ddl->definitions[0].type,d);
  check(type.error.empty(),"type resolution failed");return type.type;
}
const SqlType &projected(const Compilation &c,std::size_t column=0) {
  const auto &q=c.model.front();return c.model.types.get(q.expressions.at(q.projections.at(column).expression).sqlType);
}
void canonicalArena() {
  TypeContext arena;check(arena.size()==1,"unknown type sentinel missing");
  SqlType integer;integer.family=TypeFamily::Integer;integer.bits=32;
  auto first=arena.intern(integer);check(first==arena.intern(integer),"types are not canonical");
  auto notNull=arena.withNullability(first,Nullability::NotNull);
  check(first!=notNull && arena.get(first).nullability==Nullability::Unknown,"nullability changed canonical source type");
  auto copy=arena;check(copy.get(notNull)==arena.get(notNull),"copy lost type IDs");
  copy.withNullability(first,Nullability::Nullable);check(copy.size()>arena.size(),"copy shares mutable type state");
  bool limited=false;try{arena.withNullability(first,Nullability::Nullable,arena.size());}catch(const std::length_error&){limited=true;}
  check(limited,"type count limit ignored");
  SqlType opaque;opaque.opaqueName={std::string(65537,'x')};limited=false;
  try{arena.intern(opaque);}catch(const std::length_error&){limited=true;}
  check(limited,"type payload limit ignored");
  check(categoryType(ValueType::Decimal).family==TypeFamily::Numeric,"coarse numeric facts falsely claim exact decimal");
  check(categoryType(ValueType::String).family==TypeFamily::String,"coarse string facts falsely claim a native VARCHAR domain");
}
void declaredDomains() {
  auto my=declared(Dialect::MySQL,"DECIMAL");check(my.precision==10 && my.scale==0,"MySQL decimal defaults");
  auto ms=declared(Dialect::SQLServer,"DECIMAL");check(ms.precision==18 && ms.scale==0,"SQL Server decimal defaults");
  auto pg=declared(Dialect::PostgreSQL,"NUMERIC");check(!pg.precision && !pg.scale,"unconstrained numeric invented typmod");
  auto oracleDecimal=declared(Dialect::Oracle,"DECIMAL");check(oracleDecimal.precision==38 && oracleDecimal.scale==0,"Oracle DECIMAL confused with unconstrained NUMBER");
  auto zeroFill=declared(Dialect::MySQL,"INTEGER ZEROFILL");check(zeroFill.unsignedInteger==true,"ZEROFILL unsigned domain lost");
  auto ora=declared(Dialect::Oracle,"NUMBER");check(!ora.precision && !ora.scale,"unconstrained NUMBER invented scale");
  for(const auto &test:std::vector<std::pair<Dialect,std::string>>{{Dialect::MySQL,"DECIMAL(65,30)"},{Dialect::MariaDB,"DECIMAL(65,38)"},
      {Dialect::PostgreSQL,"NUMERIC(4,-2)"},{Dialect::PostgreSQL,"NUMERIC(2,3)"},{Dialect::Oracle,"NUMBER(38,-84)"},{Dialect::SQLServer,"DECIMAL(38,38)"}})
    check(declared(test.first,test.second).family==TypeFamily::Decimal,"decimal declaration not preserved");
  for(const auto &test:std::vector<std::pair<Dialect,std::string>>{{Dialect::MySQL,"DECIMAL(66,2)"},{Dialect::MySQL,"DECIMAL(4,5)"},
      {Dialect::PostgreSQL,"NUMERIC(1001,2)"},{Dialect::PostgreSQL,"NUMERIC(2,-1001)"},{Dialect::Oracle,"NUMBER(0)"},
      {Dialect::Oracle,"NUMBER(2,-85)"},{Dialect::SQLServer,"DECIMAL(2,3)"},{Dialect::SQLServer,"VARCHAR(0)"},
      {Dialect::PostgreSQL,"VARCHAR(MAX)"},{Dialect::PostgreSQL,"TEXT(2)"}})
    check(!compile(test.first,"CREATE TABLE t(c "+test.second+");")->ok(),"invalid type accepted");
  check(declared(Dialect::PostgreSQL,"DOUBLE PRECISION").bits==64,"double precision width lost");
  check(declared(Dialect::SQLServer,"FLOAT(24)").bits==32,"FLOAT(24) width lost");
  check(declared(Dialect::SQLServer,"FLOAT(53)").bits==64,"FLOAT(53) width lost");
  check(declared(Dialect::Oracle,"FLOAT(63)").family==TypeFamily::Decimal,"Oracle FLOAT incorrectly IEEE");
  check(declared(Dialect::SQLite,"DECIMAL(4,2)").family==TypeFamily::Unknown,"SQLite affinity treated as exact decimal domain");
  check(declared(Dialect::PostgreSQL,"VARCHAR(30)").lengthUnit==LengthUnit::Characters,"PostgreSQL length not characters");
  check(declared(Dialect::SQLServer,"VARCHAR(30)").lengthUnit==LengthUnit::Bytes,"SQL Server VARCHAR length not bytes");
  check(declared(Dialect::SQLServer,"NVARCHAR(30)").lengthUnit==LengthUnit::Utf16CodeUnits,"SQL Server NVARCHAR length not code units");
  check(declared(Dialect::Oracle,"VARCHAR2(30 CHAR)").lengthUnit==LengthUnit::Characters,"Oracle explicit CHAR length lost");
  check(declared(Dialect::Oracle,"VARCHAR2(30)").lengthUnit==LengthUnit::Unknown,"Oracle unknown NLS length semantics invented");
  check(declared(Dialect::PostgreSQL,"VARCHAR(10485760)").length==10485760,"valid large PostgreSQL type length rejected");
  for(const auto &test:std::vector<std::pair<Dialect,std::string>>{{Dialect::PostgreSQL,"VARCHAR(10485761)"},{Dialect::SQLServer,"VARCHAR(8001)"},{Dialect::SQLServer,"NVARCHAR(4001)"},{Dialect::MySQL,"CHAR(256)"}})
    check(!compile(test.first,"CREATE TABLE t(c "+test.second+");")->ok(),"known type length limit ignored");
  auto mysqlCast=parse(Dialect::MySQL,"SELECT CAST('a' AS CHAR(1000));");
  check(projected(*mysqlCast).family==TypeFamily::VarCharacter,"MySQL CHAR cast confused with fixed CHAR column");
  auto sqlite=declared(Dialect::SQLite,"DECIMAL(4,2)");check(sqlite.sqliteAffinity && !sqlite.precision,"SQLite declared precision became enforced precision");
}
void catalogTypeParsing() {
  auto decimal=catalogTypeFromText(Dialect::SQLServer,"decimal");check(decimal && !decimal->precision && !decimal->scale,"catalog type invented declaration defaults");
  auto varchar=catalogTypeFromText(Dialect::SQLServer,"nvarchar");check(varchar && !varchar->length,"catalog type invented column length");
  auto known=catalogTypeFromText(Dialect::PostgreSQL,"numeric(12,3)");check(known && known->precision==12 && known->scale==3,"catalog decimal lost typmod");
  auto timestamp=catalogTypeFromText(Dialect::PostgreSQL,"timestamp(3) with time zone");check(timestamp && timestamp->timeZone==TimeZoneKind::With,"catalog timestamp timezone lost");
  for(const auto &text:{"integer); SELECT 1; --","integer, extra integer","integer DEFAULT 1","integer NOT NULL","varchar(30) COLLATE c"})
    check(!catalogTypeFromText(Dialect::PostgreSQL,text),"catalog type wrapper accepted a declaration/statement splice");
  check(!catalogTypeFromText(Dialect::PostgreSQL,std::string(4097,'x')),"catalog type budget ignored");
}
void typedCastsAndOwnership() {
  for(const auto &test:std::vector<std::pair<Dialect,std::string>>{
      {Dialect::PostgreSQL,"SELECT '13'::NUMERIC(6,-2);"},
      {Dialect::PostgreSQL,"SELECT CAST(NULL AS NUMERIC(6,2)[]);"},
      {Dialect::PostgreSQL,"SELECT CAST(NULL AS TIMESTAMP(3) WITH TIME ZONE);"},
      {Dialect::PostgreSQL,"SELECT CAST(1 AS DOUBLE PRECISION);"},
      {Dialect::MySQL,"SELECT CAST(1 AS SIGNED INTEGER);"},
      {Dialect::MySQL,"SELECT CAST(1 AS UNSIGNED);"},
      {Dialect::SQLServer,"SELECT CAST(1 AS VARCHAR(MAX));"}}) {
    auto parsed=parse(test.first,test.second);const auto &q=parsed->model.front();
    const auto &expression=q.expressions.at(q.projections[0].expression);
    check(bool(expression.castType),"CAST type is still hidden in raw text");
    auto generated=generateSql(parsed->model,test.first);
    check(generated.ok(),generated.diagnostics.empty()?"no generated SQL":generated.diagnostics.front().message.c_str());
    auto again=parse(test.first,generated.sql);check(projected(*again)==projected(*parsed),"CAST facts changed on round trip");
    auto model=parsed->model;auto typeId=expression.sqlType;auto type=model.types.get(typeId);parsed.reset();
    check(model.types.get(typeId)==type && !model.queries[0].expressions.empty(),"type facts reference destroyed AST/source");
    check(dumpModel(*again).find("\"types\"")!=std::string::npos,"type pool absent from IR dump");
    check(dumpAST(*again).find("castType")!=std::string::npos,"structured CAST absent from AST dump");
  }
  auto timestamp=parse(Dialect::PostgreSQL,"SELECT CAST(NULL AS TIMESTAMP(3) WITH TIME ZONE);");
  check(projected(*timestamp).timeZone==TimeZoneKind::With && projected(*timestamp).precision==3 && projected(*timestamp).nullability==Nullability::Nullable,"timestamp type facts lost");
  auto mysqlCharacter=parse(Dialect::MySQL,"SELECT CAST('abcd' AS CHAR);");
  check(!projected(*mysqlCharacter).length && projected(*mysqlCharacter).family==TypeFamily::VarCharacter,"MySQL CHAR cast without N invented a one-character limit");
  auto sqlServerCount=parse(Dialect::SQLServer,"SELECT COUNT(*);");
  check(projected(*sqlServerCount).nullability==Nullability::Unknown,"SQL Server arithmetic session settings ignored for COUNT nullability");
  auto varchar=parse(Dialect::SQLServer,"SELECT CAST(1 AS VARCHAR);");check(projected(*varchar).length==30,"CAST default length confused with column default");
  check(declared(Dialect::SQLServer,"VARCHAR").length==1,"column default length confused with CAST default");
  auto conversion=convertSql(Dialect::PostgreSQL,Dialect::SQLServer,"SELECT CAST(1 AS NUMERIC(10,2));");
  check(!conversion.ok() && conversion.sql.empty(),"unproven decimal cast rounding conversion accepted");
}
void qualifiedNativeTypes() {
  auto c=parse(Dialect::PostgreSQL,"SELECT CAST(1 AS pg_catalog.int4),CAST(1 AS pg_catalog.\"int8\"),CAST(NULL AS pg_catalog.numeric(8,3)),CAST(NULL AS pg_catalog.timestamptz),CAST(NULL AS pg_catalog.bpchar),CAST(1 AS float4),CAST(1 AS float8);");
  check(projected(*c).bits==32 && projected(*c,1).bits==64,"qualified native integer types remained opaque");
  check(projected(*c,2).precision==8 && projected(*c,2).scale==3,"qualified numeric typmod lost");
  check(projected(*c,3).timeZone==TimeZoneKind::With,"qualified native timestamp timezone lost");
  check(projected(*c,4).family==TypeFamily::Character && !projected(*c,4).length,"native bpchar without typmod was confused with SQL CHAR(1)");
  check(projected(*c,5).bits==32 && projected(*c,6).bits==64,"PG float4 / float8 aliases unknown");
  auto generated=generateSql(c->model,Dialect::PostgreSQL);check(generated.ok(),"qualified native type generation failed");
  auto again=parse(Dialect::PostgreSQL,generated.sql);for(std::size_t i=0;i<c->model.front().projections.size();++i)check(projected(*c,i)==projected(*again,i),"native qualified type changed on round trip");
  for(const auto &name:{"app.int4","pg_catalog.integer","pg_catalog.\"INT4\"","\"PG_CATALOG\".int4","\"int4\""})
    check(projected(*parse(Dialect::PostgreSQL,std::string("SELECT CAST(NULL AS ")+name+");")).family==TypeFamily::Unknown,"unproved type namespace or quote folded into native type");
  for(const auto &type:{"pg_catalog.bool(1)","pg_catalog.float4(3)","float4(3)","float8(3)"})
    check(!compile(Dialect::PostgreSQL,std::string("SELECT CAST(NULL AS ")+type+");")->ok(),"native catalog type accepted unsupported typmod");
  auto upper=parse(Dialect::PostgreSQL,"SELECT CAST(1 AS PG_CATALOG.INT4);");auto upperSql=generateSql(upper->model,Dialect::PostgreSQL);check(upperSql.ok() && projected(*parse(Dialect::PostgreSQL,upperSql.sql)).bits==32,"unquoted qualified native name acquired case-sensitive quotes");
  auto names=parse(Dialect::PostgreSQL,"SELECT CAST(NULL AS app.MyType),CAST(NULL AS \"app\".\"mytype\"),CAST(NULL AS app.\"MyType\");");
  check(projected(*names,0)==projected(*names,1) && projected(*names,0)!=projected(*names,2),"opaque type identity lost quote / folding semantics");
  auto namesSql=generateSql(names->model,Dialect::PostgreSQL);check(namesSql.ok(),"opaque type generation failed");auto namesAgain=parse(Dialect::PostgreSQL,namesSql.sql);
  for(std::size_t i=0;i<3;++i)check(projected(*names,i)==projected(*namesAgain,i),"opaque type identity changed after generation");
  auto aggregate=parse(Dialect::PostgreSQL,"SELECT SUM(CAST(1 AS pg_catalog.int4));");check(projected(*aggregate).bits==64,"qualified native type not used by aggregate inference");
}
void aggregateResultDomains() {
  auto pg=parse(Dialect::PostgreSQL,"SELECT SUM(CAST(1 AS SMALLINT)),SUM(CAST(1 AS INTEGER)),SUM(CAST(1 AS BIGINT)),AVG(CAST(1 AS INTEGER)),AVG(CAST(1 AS REAL)),SUM(CAST(1 AS REAL)),SUM(CAST(1 AS NUMERIC(8,3))),COUNT(*);");
  check(projected(*pg).bits==64 && projected(*pg,1).bits==64,"PG small / integer SUM did not widen to bigint");
  check(projected(*pg,2).family==TypeFamily::Decimal && !projected(*pg,2).precision,"PG bigint SUM is not unconstrained numeric");
  check(projected(*pg,3).family==TypeFamily::Decimal && !projected(*pg,3).scale,"PG integer AVG is not unconstrained numeric");
  check(projected(*pg,4).family==TypeFamily::Floating && projected(*pg,4).bits==64 && projected(*pg,5).bits==32,"PG real SUM / AVG domains collapsed");
  check(projected(*pg,6).family==TypeFamily::Decimal && !projected(*pg,6).precision && !projected(*pg,6).scale,"PG numeric SUM inherited a column typmod");
  check(projected(*pg,7).bits==64 && projected(*pg,7).nullability==Nullability::NotNull,"PG COUNT result lost bigint / non-null fact");
  auto ms=parse(Dialect::SQLServer,"SELECT SUM(CAST(1 AS TINYINT)),SUM(CAST(1 AS BIGINT)),SUM(CAST(1 AS DECIMAL(8,3))),AVG(CAST(1 AS DECIMAL(8,3))),AVG(CAST(1 AS SMALLINT)),SUM(CAST(1 AS REAL)),COUNT(*),COUNT_BIG(*);");
  check(projected(*ms).bits==32 && projected(*ms).unsignedInteger==false && projected(*ms,1).bits==64,"SQL Server integer SUM promotion failed");
  check(projected(*ms,2).precision==38 && projected(*ms,2).scale==3 && projected(*ms,3).precision==38 && projected(*ms,3).scale==6,"SQL Server decimal aggregate result precision / scale wrong");
  check(projected(*ms,4).bits==32 && projected(*ms,5).bits==64,"SQL Server AVG integer / SUM real promotion failed");
  check(projected(*ms,6).bits==32 && projected(*ms,7).bits==64 && projected(*ms,6).nullability==Nullability::Unknown,"SQL Server COUNT session-sensitive nullability or width lost");
  auto my=parse(Dialect::MySQL,"SELECT SUM(CAST(1 AS SIGNED)),AVG(CAST(1 AS DECIMAL(8,3))),SUM(CAST(1 AS DOUBLE));");
  check(projected(*my).family==TypeFamily::Decimal && projected(*my,1).family==TypeFamily::Decimal && !projected(*my,1).scale,"MySQL aggregate guessed session precision / scale");
  check(projected(*my,2).family==TypeFamily::Floating && projected(*my,2).bits==64,"MySQL approximate SUM is not double");
  auto sq=parse(Dialect::SQLite,"SELECT AVG(1),SUM(1),TOTAL(NULL),COUNT(*);");
  check(projected(*sq).family==TypeFamily::Floating && projected(*sq).bits==64 && projected(*sq,1).family==TypeFamily::Numeric,"SQLite SUM runtime result claimed a fixed domain");
  check(projected(*sq,2).bits==64 && projected(*sq,2).nullability==Nullability::NotNull && projected(*sq,3).bits==64,"SQLite TOTAL / COUNT result facts wrong");
  auto interval=parse(Dialect::PostgreSQL,"SELECT SUM(INTERVAL '1' DAY),AVG(INTERVAL '1' DAY);");
  check(projected(*interval).family==TypeFamily::Interval && projected(*interval).intervalLeading==IntervalField::None && !projected(*interval).precision,"PG aggregate retained an interval input typmod");
  auto nested=parse(Dialect::PostgreSQL,"WITH q AS(SELECT SUM(CAST(1 AS INTEGER)) AS n) SELECT n FROM q;");
  check(projected(*nested).bits==64,"aggregate result type lost through CTE");
  auto window=parse(Dialect::PostgreSQL,"SELECT SUM(CAST(1 AS INTEGER)) OVER();");check(projected(*window).bits==64,"aggregate type lost through OVER");
  const auto frameValue=parse(Dialect::PostgreSQL,
      "SELECT NTH_VALUE(CAST(1 AS INT),2) OVER(ORDER BY 1 ROWS CURRENT ROW);");
  check(projected(*frameValue).bits==32 && projected(*frameValue).nullability==Nullability::Nullable,
        "the typed frame position can be missing even for a non-null value input");
  for(const auto &test:std::vector<std::pair<Dialect,std::string>>{
    {Dialect::PostgreSQL,"SELECT SUM(CAST('x' AS TEXT));"},{Dialect::PostgreSQL,"SELECT AVG(DATE '2024-01-01');"},
    {Dialect::PostgreSQL,"SELECT SUM(TRUE);"},{Dialect::PostgreSQL,"SELECT SUM(CAST(NULL AS INTEGER[]));"},
    {Dialect::SQLServer,"SELECT SUM(CAST(1 AS BIT));"},{Dialect::SQLServer,"SELECT AVG(CAST('1' AS VARCHAR(3)));"}})
    check(!compile(test.first,test.second)->ok(),"known unsupported numeric aggregate input accepted");
  auto unknown=parse(Dialect::PostgreSQL,"SELECT SUM(n) FROM future_table;");check(!projected(*unknown).bits,"unknown aggregate input invented a result width");
  MemoryCatalog own;RoutineDefinition f;f.name="sum";f.minArguments=f.maxArguments=1;f.kind=RoutineKind::Scalar;f.resultType=ValueType::String;
  auto text=declared(Dialect::PostgreSQL,"TEXT");f.sqlType=std::make_shared<const SqlType>(text);own.routines.push_back(f);
  auto overload=parse(Dialect::PostgreSQL,"SELECT sum(CAST('x' AS TEXT));",&own);
  check(projected(*overload).family==TypeFamily::VarCharacter,"resolved user overload was treated as native SUM");
}
void postgresNumericResolution() {
  constexpr auto pg=Dialect::PostgreSQL;
  const auto literals=parse(pg,"SELECT 2147483647,2147483648,-2147483648,-(-2147483648),"
      "9223372036854775807,9223372036854775808,-9223372036854775808,1e2,1.25;");
  for (const std::size_t index : {0u,2u})
    check(projected(*literals,index).bits==32, "PostgreSQL int4 literal boundary was lost");
  for (const std::size_t index : {1u,3u,4u,6u})
    check(projected(*literals,index).bits==64, "PostgreSQL int8 literal boundary was lost");
  for (const std::size_t index : {5u,7u,8u})
    check(projected(*literals,index).family==TypeFamily::Decimal &&
          !projected(*literals,index).precision, "PostgreSQL numeric literal received a guessed typmod");
  const auto arithmetic=parse(pg,"SELECT 1+2,CAST(1 AS SMALLINT)/CAST(2 AS SMALLINT),"
      "CAST(1 AS SMALLINT)*CAST(2 AS BIGINT),CAST(3 AS BIGINT)%1;");
  check(projected(*arithmetic).bits==32 && projected(*arithmetic,1).bits==16 &&
        projected(*arithmetic,2).bits==64 && projected(*arithmetic,3).bits==64,
        "PostgreSQL integral arithmetic result widths were not resolved");
  const auto choice=parse(pg,"SELECT CASE WHEN TRUE THEN 1 ELSE 2147483648 END,"
      "COALESCE(NULL,1,2147483648),GREATEST(1,1.25),LEAST(1,CAST(2 AS REAL));");
  check(projected(*choice).bits==64 && projected(*choice,1).bits==64 &&
        projected(*choice,2).family==TypeFamily::Decimal && projected(*choice,3).bits==32 &&
        projected(*choice,3).family==TypeFamily::Floating, "PostgreSQL scalar type selection lost promotion");
  check(projected(*parse(pg,"VALUES (1),(2147483648),(NULL);")).bits==64,
        "PostgreSQL VALUES dropped its wider numeric type");
  check(projected(*parse(pg,"SELECT 1 UNION ALL SELECT 2.5;")).family==TypeFamily::Decimal,
        "PostgreSQL UNION lost its numeric common type");
  const auto contextual=parse(pg,
      "SELECT CASE WHEN TRUE THEN '2' ELSE 1 END,COALESCE(NULL,'value'),"
      "GREATEST('2',1),NULLIF(NULL,1),NULLIF(1,2.2),NULLIF(1,2147483648),"
      "NULLIF(1::REAL,2::DOUBLE PRECISION);");
  check(projected(*contextual,0).bits==32 && projected(*contextual,1).flavor=="TEXT" &&
        projected(*contextual,2).bits==32 && projected(*contextual,3).bits==32 &&
        projected(*contextual,4).family==TypeFamily::Decimal && projected(*contextual,5).bits==32 &&
        projected(*contextual,6).bits==32 && projected(*contextual,6).family==TypeFamily::Floating,
        "PostgreSQL contextual unknown and NULLIF types diverged");
  check(projected(*parse(pg,"VALUES (1),('2'),(NULL);")).bits==32 &&
        projected(*parse(pg,"SELECT NULL UNION ALL SELECT 'value';")).flavor=="TEXT",
        "untyped VALUES or UNION inputs changed the selected PostgreSQL type");
  for (const auto *sql : {
      "SELECT NULL UNION SELECT NULL UNION SELECT 1;",
      "SELECT CASE WHEN TRUE THEN 1 ELSE 'x'::TEXT END;",
      "SELECT COALESCE(1,'x'::TEXT);",
      "SELECT GREATEST(TRUE,1);"}) {
    const auto rejected=compile(pg,sql);
    check(rejected->syntaxValid && !rejected->ok(),
          ("PostgreSQL incompatible result categories accepted: "+std::string(sql)).c_str());
  }
  for (const auto *sql : {
      "WITH RECURSIVE r(n) AS (SELECT 1 UNION ALL SELECT n+2147483648 FROM r WHERE n<2) SELECT n FROM r;",
      "WITH RECURSIVE r(n) AS (VALUES (1) UNION ALL SELECT n+1.5 FROM r WHERE n<2) SELECT n FROM r;",
      "WITH RECURSIVE r(n) AS (SELECT CAST(1 AS NUMERIC(4,1)) UNION ALL SELECT n+1.0 FROM r WHERE n<2) SELECT n FROM r;"}) {
    const auto rejected=compile(pg,sql);
    check(rejected->syntaxValid && !rejected->ok() &&
          rejected->diagnostics.front().message.find("recursive CTE column 1")!=std::string::npos,
          ("recursive common-type mismatch missed for "+std::string(sql)).c_str());
  }
  check(projected(*parse(pg,
      "WITH RECURSIVE r(n) AS (SELECT 2147483648 UNION ALL SELECT 1 FROM r WHERE n>1) SELECT n FROM r;")).bits==64,
      "recursive working-table type did not match a wider seed");
  check(projected(*parse(pg,
      "WITH RECURSIVE r(n) AS (SELECT 9223372036854775808 UNION ALL SELECT 1.5 FROM r WHERE n>2) SELECT n FROM r;")).family==TypeFamily::Decimal,
      "a recursive numeric seed was classified from syntax rather than its type");
  check(projected(*parse(pg,
      "WITH RECURSIVE r(n) AS (SELECT CAST(1 AS NUMERIC(4,1)) UNION ALL SELECT CAST(n+1.0 AS NUMERIC(4,1)) FROM r WHERE n<2) SELECT n FROM r;")).scale==1,
      "recursive CTE lost its agreed numeric typmod");
}
void postgresParameterTypes() {
  constexpr auto pg=Dialect::PostgreSQL;
  const auto source=parse(pg,
      "SELECT $1::BIGINT,COALESCE($2,1),CASE WHEN $3 THEN $2 ELSE 0 END,"
      "(SELECT $1 WHERE $4=2) LIMIT $5;");
  const auto checkParameter=[&](const Compilation &program, std::string_view name,
                                TypeFamily family, std::optional<int> bits, std::size_t expectedUses) {
    std::size_t uses=0;
    for (const auto &query : program.model.queries)
      for (const auto &expression : query.expressions) {
        if (expression.kind!=ExprKind::Parameter || expression.value!=name) continue;
        ++uses;
        const auto &type=program.model.types.get(expression.sqlType);
        check(type.family==family && type.bits==bits && type.nullability==Nullability::Unknown,
              "incorrect prepared-parameter type or nullability");
      }
    check(uses==expectedUses, "prepared-parameter scope lost a reference");
  };
  checkParameter(*source,"$1",TypeFamily::Integer,64,2);
  checkParameter(*source,"$2",TypeFamily::Integer,32,2);
  checkParameter(*source,"$3",TypeFamily::Boolean,{},1);
  checkParameter(*source,"$4",TypeFamily::Integer,32,1);
  checkParameter(*source,"$5",TypeFamily::Integer,64,1);

  const auto set=parse(pg,"VALUES ($1),(2) UNION ALL SELECT $2;");
  checkParameter(*set,"$1",TypeFamily::Integer,32,1);
  checkParameter(*set,"$2",TypeFamily::Integer,32,1);
  const auto conflicting=parse(pg,"SELECT $1::INTEGER,$1::TEXT;");
  checkParameter(*conflicting,"$1",TypeFamily::Unknown,{},2);
  const auto batch=parse(pg,"SELECT $1::INTEGER; SELECT $1::TEXT;");
  const auto first=batch->model.queries.at(batch->model.roots.at(0)-1);
  const auto second=batch->model.queries.at(batch->model.roots.at(1)-1);
  const auto facts=[&](const ModelStatement &q) {
    const auto &cast=q.expressions.at(q.projections.front().expression);
    return batch->model.types.get(q.expressions.at(cast.operands.front()).sqlType);
  };
  check(facts(first).bits==32 && facts(second).flavor=="TEXT","parameter numbers leaked across statements");
}
void castDialectContracts() {
  for(const auto &target:{"INTEGER","INT","BIGINT","SMALLINT","VARCHAR(30)","BOOLEAN","TIMESTAMP","SIGNED(3)","DATE(3)","DOUBLE(3)","CHAR VARYING(20)"})
    check(!compile(Dialect::MySQL,std::string("SELECT CAST(1 AS ")+target+");")->ok(),"MySQL CAST accepted a declaration-only / invalid target");
  for(const auto &target:{"SIGNED","SIGNED INTEGER","UNSIGNED","CHAR(30)","DECIMAL(8,3)","DATETIME(3)","TIME(3)","YEAR","JSON","DOUBLE","FLOAT(24)"}) {
    auto c=parse(Dialect::MySQL,std::string("SELECT CAST(NULL AS ")+target+");");
    auto sql=generateSql(c->model,Dialect::MySQL);check(sql.ok(),"native CAST target cannot be generated");
    check(compile(Dialect::MySQL,sql.sql)->ok(),"native CAST target cannot round trip");
  }
  auto signedCast=parse(Dialect::MySQL,"SELECT CAST(1 AS SIGNED),CAST(1 AS UNSIGNED);");
  check(projected(*signedCast).bits==64 && projected(*signedCast).unsignedInteger==false,"MySQL SIGNED CAST is not a signed 64-bit result");
  check(projected(*signedCast,1).bits==64 && projected(*signedCast,1).unsignedInteger==true,"MySQL UNSIGNED CAST is not a 64-bit result");
  auto forged=signedCast->model;auto &expression=forged.queries.front().expressions.at(forged.front().projections[0].expression);
  auto invalid=*expression.castType;invalid.name={"INTEGER"};expression.castType=std::make_shared<const BasicTypeSpec<std::vector<std::string>>>(invalid);
  check(!generateSql(forged,Dialect::MySQL).ok(),"backend emitted an invalid MySQL CAST from hand-built IR");
}
void typedLiterals() {
  for(const auto &test:std::vector<std::pair<Dialect,std::string>>{
    {Dialect::PostgreSQL,"SELECT DATE '2024-02-29', TIMESTAMP(3) WITH TIME ZONE '2024-02-29 12:34:56.123+08', TIME '12:34:56';"},
    {Dialect::PostgreSQL,"SELECT NUMERIC(6,-2) '12345', DOUBLE PRECISION '1.25', app.opaque_type 'payload', \"Date\" 'text';"},
    {Dialect::PostgreSQL,"SELECT VARCHAR(3) $$abcdef$$, INTERVAL '1 day';"},
    {Dialect::MySQL,"SELECT DATE '2024-02-29', TIME '12:34:56', TIMESTAMP '2024-02-29 12:34:56.123456';"},
    {Dialect::Oracle,"SELECT DATE '2024-02-29',TIMESTAMP '2024-02-29 12:34:56.123456' FROM dual;"}}) {
    auto c=parse(test.first,test.second);auto generated=generateSql(c->model,test.first);
    check(generated.ok(),generated.diagnostics.empty()?"no generated typed literal":generated.diagnostics.front().message.c_str());
    auto again=parse(test.first,generated.sql);
    for(std::size_t i=0;i<c->model.front().projections.size();++i)check(projected(*c,i)==projected(*again,i),"typed literal type changed after canonical generation");
    check(dumpAST(*c).find("literalType")!=std::string::npos && dumpModel(*c).find("typedLiteral")!=std::string::npos,"typed literal missing from AST/IR");
    auto model=c->model;c.reset();check(generateSql(model,test.first).ok(),"typed literal references destroyed source");
  }
  auto my=parse(Dialect::MySQL,"SELECT TIMESTAMP '2024-01-01 12:00:00';");
  check(projected(*my).family==TypeFamily::Timestamp && projected(*my).flavor=="datetime","MySQL timestamp literal confused with TIMESTAMP storage type");
  auto pg=parse(Dialect::PostgreSQL,"SELECT TIMESTAMP WITH TIME ZONE '2024-01-01 12:00:00+08';");
  check(projected(*pg).timeZone==TimeZoneKind::With,"literal timezone lost");
  check(projected(*parse(Dialect::PostgreSQL,"SELECT app.custom 'x';")).family==TypeFamily::Unknown,"custom literal type guessed as builtin");
  for(const auto &test:std::vector<std::pair<Dialect,std::string>>{
    {Dialect::PostgreSQL,"SELECT INTEGER[] '{1,2}';"},
    {Dialect::PostgreSQL,"SELECT DATE(3) '2024-01-01';"},
    {Dialect::PostgreSQL,"SELECT TIMESTAMP WITH LOCAL TIME ZONE '2024-01-01 12:00:00';"},
    {Dialect::Oracle,"CREATE TABLE t(c TIMESTAMP WITHOUT TIME ZONE);"},
    {Dialect::SQLServer,"SELECT DATE '2024-01-01';"},
    {Dialect::SQLite,"SELECT DATE '2024-01-01';"},
    {Dialect::Oracle,"SELECT TIME '12:00:00' FROM dual;"}})
    check(!compile(test.first,test.second)->ok(),"invalid typed literal accepted");
  // A MySQL function call with a string alias must keep its existing meaning.
  auto alias=parse(Dialect::MySQL,"SELECT DATE('2024-01-01') 'label';");
  const auto &q=alias->model.front();check(q.expressions[q.projections[0].expression].kind==ExprKind::Call,"function/string alias reinterpreted as typed literal");
}
void intervalAndExtractContexts() {
  for(const auto &test:std::vector<std::pair<Dialect,std::string>>{
    {Dialect::PostgreSQL,"SELECT INTERVAL '1 day', INTERVAL(3) '1.234567 second', INTERVAL '1-2' YEAR TO MONTH, INTERVAL '2 03:04:05.6789' DAY TO SECOND(3), EXTRACT(YEAR FROM DATE '2024-02-29'), EXTRACT('epoch' FROM TIMESTAMP '2024-02-29 12:00:00');"},
    {Dialect::MySQL,"SELECT DATE '2024-02-29'+INTERVAL 1 DAY, DATE_ADD(DATE '2024-02-29',INTERVAL (1+2) MONTH), EXTRACT(DAY_HOUR FROM TIMESTAMP '2024-02-29 12:00:00'), INTERVAL(23,1,15,17,30);"},
    {Dialect::Oracle,"SELECT INTERVAL '123-2' YEAR(3) TO MONTH,INTERVAL '400 5:12:10.222' DAY(3) TO SECOND(3), INTERVAL '30.12345' SECOND(2,4), EXTRACT(YEAR FROM DATE '2024-02-29') FROM SYS.DUAL;"}}) {
    auto c=parse(test.first,test.second);auto emitted=generateSql(c->model,test.first);
    check(emitted.ok(),emitted.diagnostics.empty()?"no generated temporal SQL":emitted.diagnostics.front().message.c_str());
    auto again=parse(test.first,emitted.sql);
    for(std::size_t i=0;i<c->model.front().projections.size();++i)
      check(projected(*c,i)==projected(*again,i),"interval / EXTRACT type facts changed on round trip");
    check(dumpAST(*c).find("interval")!=std::string::npos && dumpModel(*c).find("extract")!=std::string::npos,"temporal syntax absent from AST/IR");
  }
  auto dateTime=parse(Dialect::PostgreSQL,"SELECT DATE '2024-01-01'+INTERVAL '1 day', NOW(), CURRENT_TIME, DATE '2024-01-02'-DATE '2024-01-01';");
  check(projected(*dateTime).family==TypeFamily::Timestamp && projected(*dateTime).timeZone==TimeZoneKind::Without,"PG date + interval domain lost");
  check(projected(*dateTime,1).family==TypeFamily::Timestamp && projected(*dateTime,1).timeZone==TimeZoneKind::With,"PG NOW timezone lost");
  check(projected(*dateTime,2).family==TypeFamily::Time && projected(*dateTime,2).timeZone==TimeZoneKind::With,"PG CURRENT_TIME timezone lost");
  check(projected(*dateTime,3).family==TypeFamily::Integer,"PG date difference domain lost");
  auto ora=parse(Dialect::Oracle,"SELECT INTERVAL '123-2' YEAR(3) TO MONTH,INTERVAL '1' DAY FROM SYS.DUAL;");
  check(projected(*ora).intervalLeading==IntervalField::Year && projected(*ora).intervalTrailing==IntervalField::Month && projected(*ora).intervalLeadingPrecision==3 && !projected(*ora).scale,"interval leading precision hidden in decimal scale");
  check(projected(*ora,1).intervalLeadingPrecision==2 && projected(*ora,1).precision==6,"Oracle interval defaults lost");
  auto my=parse(Dialect::MySQL,"SELECT DATE '2024-01-01'+INTERVAL 1 DAY,DATE '2024-01-01'+INTERVAL 1 HOUR,NOW();");
  check(projected(*my).family==TypeFamily::Date && projected(*my,1).family==TypeFamily::Timestamp && projected(*my,2).flavor=="datetime","MySQL interval result units ignored");
  for(const auto &test:std::vector<std::pair<Dialect,std::string>>{
    {Dialect::PostgreSQL,"SELECT INTERVAL '1' MONTH TO YEAR;"},
    {Dialect::PostgreSQL,"SELECT INTERVAL '1' YEAR TO DAY;"},
    {Dialect::PostgreSQL,"SELECT INTERVAL '1' DAY(3);"},
    {Dialect::PostgreSQL,"SELECT INTERVAL(7) '1';"},
    {Dialect::PostgreSQL,"SELECT INTERVAL(3) '1' DAY;"},
    {Dialect::PostgreSQL,"SELECT EXTRACT(BOGUS FROM NOW());"},
    {Dialect::Oracle,"SELECT INTERVAL '1' FROM SYS.DUAL;"},
    {Dialect::Oracle,"SELECT INTERVAL '1' DAY(10) FROM SYS.DUAL;"},
    {Dialect::Oracle,"SELECT EXTRACT(HOUR FROM DATE '2024-01-01') FROM SYS.DUAL;"},
    {Dialect::MySQL,"SELECT DATE '2024-01-01'+INTERVAL 1 BOGUS;"},
    {Dialect::MySQL,"SELECT EXTRACT(EPOCH FROM NOW());"},
    {Dialect::MySQL,"SELECT INTERVAL 1 DAY;"},
    {Dialect::MySQL,"SELECT DATE_ADD(CURRENT_DATE,1);"},
    {Dialect::MySQL,"SELECT DATE '2024-01-01'+INTERVAL * DAY;"},
    {Dialect::PostgreSQL,"SELECT EXTRACT(YEAR FROM *);"},
    {Dialect::SQLServer,"SELECT INTERVAL '1' DAY;"},
    {Dialect::SQLite,"SELECT EXTRACT(YEAR FROM CURRENT_TIMESTAMP);"}})
    check(!compile(test.first,test.second)->ok(),"invalid interval / EXTRACT accepted");
  for(auto d:{Dialect::PostgreSQL,Dialect::MySQL,Dialect::Oracle}) {
    std::string sql=d==Dialect::Oracle?"SELECT EXTRACT(YEAR FROM DATE '2024-01-01') FROM SYS.DUAL;":"SELECT EXTRACT(YEAR FROM DATE '2024-01-01');";
    auto converted=convertSql(d,d==Dialect::PostgreSQL?Dialect::MySQL:Dialect::PostgreSQL,sql);
    check(!converted.ok() && converted.sql.empty(),"unproven temporal field conversion accepted");
  }
}
void intervalDeclarationContexts() {
  for(const auto &test:std::vector<std::pair<Dialect,std::string>>{
    {Dialect::PostgreSQL,"CREATE TABLE t(a INTERVAL,b INTERVAL(3),c INTERVAL YEAR TO MONTH,d INTERVAL DAY TO SECOND(3)[]);"},
    {Dialect::PostgreSQL,"SELECT CAST('1 day 12:34:56.1234' AS INTERVAL DAY TO SECOND(3));"},
    {Dialect::Oracle,"CREATE TABLE t(a INTERVAL YEAR(3) TO MONTH,b INTERVAL DAY(4) TO SECOND(0));"},
    {Dialect::Oracle,"SELECT CAST('123-2' AS INTERVAL YEAR(3) TO MONTH) FROM SYS.DUAL;"}}) {
    auto c=parse(test.first,test.second);auto generated=generateSql(c->model,test.first);
    check(generated.ok(),generated.diagnostics.empty()?"interval type generation failed":generated.diagnostics.front().message.c_str());
    auto again=parse(test.first,generated.sql);check(again->ok(),"generated interval type failed to parse");
    if(c->model.front().kind==StatementKind::Select)check(projected(*c)==projected(*again),"interval CAST facts changed on round trip");
    check(dumpAST(*c).find("intervalType")!=std::string::npos,"interval type missing in AST dump");
  }
  auto type=catalogTypeFromText(Dialect::PostgreSQL,"interval day to second(3)");
  check(type && type->family==TypeFamily::Interval && type->intervalLeading==IntervalField::Day && type->intervalTrailing==IntervalField::Second && type->precision==3,"catalog interval typmod lost");
  auto routine=compileRoutine(Dialect::PostgreSQL,"CREATE FUNCTION f(p INTERVAL DAY TO SECOND(3)) RETURNS INTERVAL HOUR TO SECOND(2) LANGUAGE plpgsql AS $$ DECLARE v INTERVAL DAY TO SECOND(1):=p; BEGIN RETURN v; END; $$;");
  check(routine.ok(),routine.diagnostics.empty()?"interval routine failed":routine.diagnostics.front().message.c_str());
  bool parameter=false,variable=false;
  for(const auto &node:routine.model.nodes)for(const auto &q:node.code.queries)for(const auto &e:q.expressions)if(e.localSymbol) {
    const auto &t=node.code.types.get(e.sqlType);
    if(routine.ast.symbols.at(e.localSymbol).name=="p") {check(t.family==TypeFamily::Interval && t.intervalLeading==IntervalField::None && !t.precision,"ignored PG signature interval typmod became a constraint");parameter=true;}
    else if(routine.ast.symbols.at(e.localSymbol).name=="v") {check(t.intervalLeading==IntervalField::Day && t.precision==1,"local interval constraint lost");variable=true;}
  }
  check(parameter && variable,"interval variables absent from local scopes");
  for(const auto &test:std::vector<std::pair<Dialect,std::string>>{
    {Dialect::PostgreSQL,"CREATE TABLE t(a INTERVAL YEAR(3));"},
    {Dialect::PostgreSQL,"CREATE TABLE t(a INTERVAL DAY TO SECOND(7));"},
    {Dialect::PostgreSQL,"CREATE TABLE t(a INTERVAL(3) DAY TO SECOND);"},
    {Dialect::Oracle,"CREATE TABLE t(a INTERVAL HOUR TO SECOND);"},
    {Dialect::Oracle,"CREATE TABLE t(a INTERVAL DAY);"},
    {Dialect::MySQL,"CREATE TABLE t(a INTERVAL);"}})
    check(!compile(test.first,test.second)->ok(),"invalid interval declaration accepted");
  auto mixed=parse(Dialect::PostgreSQL,"SELECT COALESCE(INTERVAL '1' YEAR,INTERVAL '2' DAY);");
  check(projected(*mixed).intervalLeading==IntervalField::None && !projected(*mixed).precision,"mixed interval ranges retained a false subtype constraint");
  auto malformed=parse(Dialect::PostgreSQL,"SELECT INTERVAL '1' DAY;");
  auto &q=malformed->model.queries.front();auto &e=q.expressions[q.projections[0].expression];
  auto invalid=*e.interval;invalid.trailing=IntervalField::Year;e.interval=std::make_shared<const IntervalSpec>(invalid);
  check(!generateSql(malformed->model,Dialect::PostgreSQL).ok(),"backend accepted malformed interval IR");
  auto dateFunctions=parse(Dialect::MySQL,"SELECT DATE('2024-01-01'),TIME('12:34:56'),TIMESTAMP('2024-01-01'),DATE_ADD(DATE('2024-01-01'),INTERVAL 1 HOUR);");
  check(projected(*dateFunctions).family==TypeFamily::Date && projected(*dateFunctions,1).family==TypeFamily::Time && projected(*dateFunctions,3).family==TypeFamily::Timestamp,"date builtin result domain lost");
  auto badPrecision=compile(Dialect::MySQL,"SELECT NOW(9);");check(!badPrecision->ok(),"invalid NOW precision ignored");
  auto now=parse(Dialect::MySQL,"SELECT NOW(3),CURRENT_TIMESTAMP(6);");check(projected(*now).precision==3 && projected(*now,1).precision==6,"temporal intrinsic precision lost");
}
void storedNullabilityConstraints() {
  for(const auto &sql:{"CREATE TABLE t(id TEXT PRIMARY KEY);","CREATE TABLE t(id INT PRIMARY KEY);","CREATE TABLE t(id INTEGER,n INTEGER,PRIMARY KEY(id,n));"}) {
    auto c=parse(Dialect::SQLite,sql);const auto &table=c->model.front().tables.front().definition;
    check(table && table->columns[0].sqlType->nullability!=Nullability::NotNull,"DDL pseudo-table invented SQLite non-null primary key facts");
  }
  auto rowidDeclaration=parse(Dialect::SQLite,"CREATE TABLE t(id INTEGER,PRIMARY KEY(id));");
  check(rowidDeclaration->model.front().tables.front().definition->columns[0].sqlType->nullability==Nullability::NotNull,"table-level rowid alias lost its type fact");

  auto observed=[](Dialect d,const std::string &sql) {
    ScriptContext context(d,nullptr,{});auto c=compile(d,sql,{},&context);check(c->ok(),"constraint definition failed");
    check(!context.observe(c->model,c->model.roots.back()),"constraint context observation failed");
    return context.findTable({"","",d},{"t"});
  };
  for(const auto &sql:{"CREATE TABLE t(id TEXT PRIMARY KEY);","CREATE TABLE t(id INT PRIMARY KEY);"}) {
    auto table=observed(Dialect::SQLite,sql);check(table && table->primaryKey.empty() && !table->columns[0].nonNullKnown,"nullable SQLite primary key became a non-null proof");
    check(table->columns[0].sqlType->nullability!=Nullability::NotNull,"SQLite nullable key has a false inferred non-null type");
  }
  auto rowid=observed(Dialect::SQLite,"CREATE TABLE t(id INTEGER PRIMARY KEY);");check(rowid && rowid->primaryKey==std::vector<std::string>{"id"},"SQLite INTEGER rowid key was lost");
  auto explicitKey=observed(Dialect::SQLite,"CREATE TABLE t(id TEXT PRIMARY KEY NOT NULL);");check(explicitKey && explicitKey->columns[0].nonNullKnown && !explicitKey->primaryKey.empty(),"explicit SQLite NOT NULL key was lost");
  auto ctas=observed(Dialect::PostgreSQL,"CREATE TABLE t AS SELECT 1 AS id;");check(ctas && !ctas->columns[0].nonNullKnown && ctas->columns[0].sqlType->nullability==Nullability::Unknown,"CTAS inherited a runtime NOT NULL proof as a stored constraint");
  ScriptContext altered(Dialect::PostgreSQL,nullptr,{});
  auto apply=[&](const std::string &sql){auto c=compile(Dialect::PostgreSQL,sql,{},&altered);check(c->ok(),"ALTER nullability parse failed");check(!altered.observe(c->model,c->model.roots.back()),"ALTER nullability observation failed");};
  apply("CREATE TABLE t(n INTEGER);");apply("ALTER TABLE t ALTER COLUMN n SET NOT NULL;");
  check(altered.findTable({}, {"t"})->columns[0].nonNullKnown,"SET NOT NULL proof not retained");
  apply("ALTER TABLE t ALTER COLUMN n DROP NOT NULL;");check(!altered.findTable({}, {"t"})->columns[0].nonNullKnown,"DROP NOT NULL retained stale proof");
}
void nullableScopes() {
  MemoryCatalog catalog;TableDefinition table;table.name="a";
  SqlType type;type.family=TypeFamily::Integer;type.bits=32;type.nullability=Nullability::NotNull;
  table.columns={{"id",ValueType::Integer,std::make_shared<const SqlType>(type)}};catalog.tables.push_back(table);table.name="b";catalog.tables.push_back(table);
  auto joined=parse(Dialect::PostgreSQL,"SELECT a.id,b.id FROM a LEFT JOIN b ON a.id=b.id;",&catalog);
  check(projected(*joined,0).nullability==Nullability::NotNull && projected(*joined,1).nullability==Nullability::Nullable,"outer join null extension lost");
  auto nested=parse(Dialect::PostgreSQL,"WITH q AS(SELECT id FROM a) SELECT id FROM q;",&catalog);
  check(projected(*nested).bits==32 && projected(*nested).nullability==Nullability::NotNull,"CTE type propagation failed");
  auto grouped=parse(Dialect::PostgreSQL,"SELECT id,COUNT(*) FROM a GROUP BY ROLLUP(id);",&catalog);
  check(projected(*grouped).nullability==Nullability::Nullable,"subtotal falsely kept NOT NULL");
  check(projected(*grouped,1).nullability==Nullability::NotNull,"COUNT falsely nullable");
  auto scalar=parse(Dialect::PostgreSQL,"SELECT (SELECT id FROM a WHERE id=0);");
  check(projected(*scalar).nullability==Nullability::Nullable,"scalar subquery falsely non-null");
  auto coalesce=parse(Dialect::PostgreSQL,"SELECT COALESCE(NULL,1),CASE WHEN TRUE THEN 1 END,EXISTS(SELECT 1); ");
  check(projected(*coalesce,0).nullability==Nullability::NotNull,"COALESCE guaranteed fallback lost");
  check(projected(*coalesce,1).nullability==Nullability::Nullable,"implicit ELSE NULL lost");
  check(projected(*coalesce,2).nullability==Nullability::NotNull,"EXISTS falsely nullable");
}
void routineTypeContexts() {
  for(const auto &test:std::vector<std::pair<Dialect,std::string>>{
      {Dialect::PostgreSQL,"CREATE FUNCTION typed(p NUMERIC(6,2)) RETURNS NUMERIC(8,3) LANGUAGE plpgsql AS $$ DECLARE v NUMERIC(8,3):=p; BEGIN RETURN v; END; $$;"},
      {Dialect::MySQL,"CREATE FUNCTION typed(p DECIMAL(6,2)) RETURNS DECIMAL(8,3) BEGIN DECLARE v DECIMAL(8,3) DEFAULT p; RETURN v; END;"},
      {Dialect::SQLServer,"CREATE FUNCTION dbo.typed(@p DECIMAL(6,2)) RETURNS DECIMAL(8,3) AS BEGIN DECLARE @v DECIMAL(8,3)=@p; RETURN @v; END;"}}) {
    auto routine=compileRoutine(test.first,test.second);
    check(routine.ok(),routine.diagnostics.empty()?"routine type context missing":routine.diagnostics.front().message.c_str());
    bool parameter=false,variable=false;
    for(const auto &node:routine.model.nodes)for(const auto &query:node.code.queries)for(const auto &e:query.expressions)
      if(e.localSymbol) {
        const auto &type=node.code.types.get(e.sqlType);
        const auto &symbol=routine.ast.symbols.at(e.localSymbol);
        if(symbol.kind==SymbolKind::Parameter){
          if(test.first==Dialect::PostgreSQL)check(!type.precision && !type.scale,"ignored PostgreSQL parameter typmod became a constraint");
          else check(type.precision==6 && type.scale==2,"formal parameter precision lost");
          parameter=true;}
        else {check(type.precision==8 && type.scale==3,"local variable precision lost");variable=true;}
      }
    check(parameter && variable,"routine expression scopes not typed");
  }
  auto multiword=compileRoutine(Dialect::PostgreSQL,"CREATE FUNCTION typed(p CHARACTER VARYING(10)) RETURNS TIMESTAMP(3) WITH TIME ZONE LANGUAGE plpgsql AS $$ DECLARE v INTEGER[]; BEGIN RETURN NULL; END; $$;");
  check(multiword.ok(),multiword.diagnostics.empty()?"shared routine type parser missing":multiword.diagnostics.front().message.c_str());
  check(multiword.ast.symbols.at(1).type.arguments[0].value=="10","multiword parameter type lost length");
  bool array=false;for(const auto &symbol:multiword.ast.symbols)array |= symbol.type.arrayDimensions==1;check(array,"routine local array type not parsed");
  check(!compileRoutine(Dialect::Oracle,"CREATE FUNCTION typed(p IN NUMBER(6,2)) RETURN NUMBER IS BEGIN RETURN p; END;").ok(),"Oracle constrained formal parameter accepted");
  auto ignored=compileRoutine(Dialect::PostgreSQL,"CREATE FUNCTION typed(p NUMERIC(0)) RETURNS NUMERIC(0) LANGUAGE plpgsql AS $$ BEGIN RETURN p; END; $$;");
  check(!ignored.ok(),"invalid PostgreSQL signature type modifier was accepted");
  auto conversion=convertSql(Dialect::PostgreSQL,Dialect::MySQL,"CREATE FUNCTION typed(p NUMERIC(6,2)) RETURNS NUMERIC(8,3) LANGUAGE plpgsql AS $$ BEGIN RETURN p; END; $$;");
  check(!conversion.ok() && conversion.sql.empty(),"ignored source typmod converted into an enforced target constraint");
  MemoryCatalog catalog;RoutineDefinition f;f.name="typed_f";f.minArguments=f.maxArguments=1;
  f.resultType=ValueType::Decimal;f.kind=RoutineKind::Scalar;
  auto decimal=declared(Dialect::PostgreSQL,"NUMERIC(6,2)");f.sqlType=std::make_shared<const SqlType>(decimal);catalog.routines.push_back(f);
  auto call=parse(Dialect::PostgreSQL,"SELECT typed_f(1);",&catalog);
  check(projected(*call).precision==6 && projected(*call).scale==2,"resolved routine result type lost");
  decimal.precision=8;f.sqlType=std::make_shared<const SqlType>(decimal);catalog.routines.push_back(f);
  auto ambiguous=parse(Dialect::PostgreSQL,"SELECT typed_f(1);",&catalog);
  check(!projected(*ambiguous).precision,"overload ambiguity invented an exact return type");
}
void scriptTypesAndLimits() {
  ScriptContext context(Dialect::PostgreSQL,nullptr);
  auto source=parse(Dialect::PostgreSQL,"CREATE TABLE t(id NUMERIC(6,2) NOT NULL);");
  check(!context.observe(source->model,source->model.roots[0]),"script declaration failed");
  auto selected=parse(Dialect::PostgreSQL,"SELECT id FROM t;",&context);
  check(projected(*selected).precision==6 && projected(*selected).scale==2 && projected(*selected).nullability==Nullability::NotNull,"script declaration type facts lost");
  auto alter=parse(Dialect::PostgreSQL,"ALTER TABLE t ALTER COLUMN id TYPE NUMERIC(8,3);",&context);
  check(!context.observe(alter->model,alter->model.roots[0]),"script ALTER failed");
  selected=parse(Dialect::PostgreSQL,"SELECT id FROM t;",&context);check(projected(*selected).precision==8 && projected(*selected).scale==3,"ALTER kept stale type facts");
  auto nullable=parse(Dialect::PostgreSQL,"ALTER TABLE t ALTER COLUMN id DROP NOT NULL;",&context);
  check(!context.observe(nullable->model,nullable->model.roots[0]),"script nullable change failed");
  selected=parse(Dialect::PostgreSQL,"SELECT id FROM t;",&context);check(projected(*selected).nullability==Nullability::Nullable,"ALTER kept stale nullability");
  LanguageOptions limited;limited.limits.types=1;
  auto failed=compile(Dialect::PostgreSQL,"SELECT 1;",limited);check(!failed->ok() && failed->model.empty(),"type limit did not abort atomically");
  auto parallel=[] {auto own=parse(Dialect::PostgreSQL,"SELECT CAST(NULL AS NUMERIC(12,3));");return projected(*own).precision;};
  auto a=std::async(std::launch::async,parallel),b=std::async(std::launch::async,parallel);check(a.get()==12 && b.get()==12,"parallel type contexts failed");
}
void sqlServerNumericArithmetic() {
  constexpr auto ms = Dialect::SQLServer;
  const auto literals = parse(ms,"SELECT 2147483647,2147483648,9223372036854775808,12.345,0.003,1e2,-12.345;");
  check(projected(*literals,0).family==TypeFamily::Integer && projected(*literals,0).bits==32,
        "SQL Server int literal boundary mismatch");
  const auto shape=[](const SqlType &type,int precision,int scale) {
    return type.family==TypeFamily::Decimal && type.precision==precision && type.scale==scale;
  };
  check(shape(projected(*literals,1),10,0) && shape(projected(*literals,2),19,0),
        "SQL Server large literal should use decimal instead of implicit bigint");
  check(shape(projected(*literals,3),5,3) && shape(projected(*literals,4),3,3) && shape(projected(*literals,6),5,3),
        "SQL Server decimal literal precision/scale mismatch");
  check(projected(*literals,5).family==TypeFamily::Floating && projected(*literals,5).bits==64,
        "SQL Server scientific literal should use float");
  const auto arithmetic=parse(ms,
      "SELECT CAST(1 AS decimal(8,3))+CAST(2 AS decimal(5,2)),"
      "CAST(1 AS decimal(8,3))-CAST(2 AS decimal(5,2)),"
      "CAST(1 AS decimal(8,3))*CAST(2 AS decimal(5,2)),"
      "CAST(1 AS decimal(8,3))/CAST(2 AS decimal(5,2)),"
      "CAST(1 AS decimal(8,3))%CAST(2 AS decimal(5,2)),"
      "12.345+CAST(1 AS smallint),"
      "CAST(.0000009000 AS decimal(30,20))*CAST(1 AS decimal(30,20)),"
      "CAST(.0000009000 AS decimal(30,10))*CAST(1 AS decimal(30,10)),"
      "CAST(1 AS decimal(20,2))*CAST(1 AS decimal(20,2)),"
      "CAST(1 AS decimal(8,3))+2,"
      "CAST(1 AS decimal(8,3))+CAST(1 AS real),"
      "CAST(1 AS smallint)+CAST(1 AS bigint),-CAST(3 AS tinyint),2/3;");
  for (std::size_t i=0;i<9;++i) {
    constexpr std::pair<int,int> expected[]={{9,3},{9,3},{14,5},{16,9},{6,3},{9,3},{38,17},{38,6},{38,4}};
    check(shape(projected(*arithmetic,i),expected[i].first,expected[i].second),
          "SQL Server decimal arithmetic lost precision or scale");
  }
  const auto &modeDependent=projected(*arithmetic,9);
  check(modeDependent.family==TypeFamily::Decimal && !modeDependent.precision && !modeDependent.scale,
        "auto-parameterization-dependent precision should remain unknown");
  check(projected(*arithmetic,10).family==TypeFamily::Floating && projected(*arithmetic,10).bits==32 &&
        projected(*arithmetic,11).bits==64 && projected(*arithmetic,12).bits==16 &&
        projected(*arithmetic,12).unsignedInteger==false && projected(*arithmetic,13).bits==32,
        "SQL Server native numeric precedence / unary promotion mismatch");
}
void mysqlNumericArithmetic() {
  constexpr auto mysql = Dialect::MySQL;
  const auto shape=[](const SqlType &type,int precision,int scale) {
    return type.family==TypeFamily::Decimal && type.precision==precision && type.scale==scale;
  };
  const auto signedness=[](const SqlType &type,int bits,bool isUnsigned) {
    return type.family==TypeFamily::Integer && type.bits==bits &&
           type.unsignedInteger==isUnsigned;
  };
  const auto literals=parse(mysql,
      "SELECT 2147483647,2147483648,9223372036854775808,18446744073709551616,"
      "-2147483648,-9223372036854775808,-9223372036854775809,"
      ".05,000.0020,0.00000,-.0020,2.34E0;");
  check(signedness(projected(*literals,0),64,false) && signedness(projected(*literals,1),64,false)
        && signedness(projected(*literals,2),64,true) && shape(projected(*literals,3),20,0),
        "MySQL signed, unsigned and decimal literal boundaries");
  check(signedness(projected(*literals,4),64,false) && signedness(projected(*literals,5),64,false)
        && shape(projected(*literals,6),19,0),"MySQL negative literal boundaries");
  check(shape(projected(*literals,7),2,2) && shape(projected(*literals,8),5,4)
        && shape(projected(*literals,9),6,5) && shape(projected(*literals,10),4,4)
        && projected(*literals,11).family==TypeFamily::Floating && projected(*literals,11).bits==64,
        "MySQL fixed-point and approximate literal metadata");
  constexpr auto sql = "SELECT CAST(1 AS DECIMAL(8,3)) + CAST(2 AS DECIMAL(5,2)),"
      "CAST(1 AS DECIMAL(8,3)) - 000001,"
      "CAST(1 AS DECIMAL(60,30))*CAST(2 AS DECIMAL(60,20)),"
      "CAST(1 AS DECIMAL(8,3))/CAST(2 AS DECIMAL(5,2)),"
      "(CAST(1 AS DECIMAL(8,3))/CAST(2 AS DECIMAL(5,2)))/CAST(3 AS DECIMAL(5,2)),"
      "SUM(CAST(1 AS DECIMAL(8,3))),AVG(CAST(1 AS DECIMAL(8,3))),"
      "9223372036854775808-1;";
  auto unknown=parse(mysql,sql);
  check(!projected(*unknown,3).scale && !projected(*unknown,6).precision
        && !projected(*unknown,7).unsignedInteger,"missing MySQL session settings guessed a default");
  LanguageOptions options;
  options.mysqlDivisionPrecisionIncrement=9;
  options.mysqlNoUnsignedSubtraction=true;
  auto explicitMode=compile(mysql,sql,options);
  check(explicitMode->ok(),"explicit MySQL session options prevented compilation");
  constexpr std::pair<int,int> expected[]={{9,3},{10,3},{65,30},{19,12},{30,21},{30,3},{17,12}};
  for (std::size_t i=0;i<7;++i)
    check(shape(projected(*explicitMode,i),expected[i].first,expected[i].second),
          "MySQL exact arithmetic or aggregation precision and scale");
  check(projected(*explicitMode,3).nullability==Nullability::Nullable
        && projected(*explicitMode,7).unsignedInteger==false,
        "MySQL division nullability or NO_UNSIGNED_SUBTRACTION missing");
  const auto division=parse(mysql,"SELECT 10+15 DIV 2, 21 MOD 4 * 2, 9.9 DIV 2.1;");
  const auto &projection=division->model.front();
  const auto &sum=projection.expressions.at(projection.projections.at(0).expression);
  check(sum.value=="+" && projection.expressions.at(sum.operands.back()).value=="DIV",
        "MySQL DIV precedence differs from multiplication");
  const auto &product=projection.expressions.at(projection.projections.at(1).expression);
  check(product.value=="*" && projection.expressions.at(product.operands.front()).value=="%",
        "MySQL MOD shares multiplicative precedence with percent");
  check(projected(*division,2).family==TypeFamily::Integer && projected(*division,2).bits==64
        && projected(*division,2).nullability==Nullability::Nullable,
        "MySQL DIV did not retain its nullable integer domain");
  const auto cross=convertSql(mysql,Dialect::PostgreSQL,"SELECT 9.9 DIV 2.1;");
  check(!cross.ok(),"cross-dialect integer division guessed rounding or signedness");

}
int main(){try{
  canonicalArena();declaredDomains();catalogTypeParsing();typedCastsAndOwnership();qualifiedNativeTypes();aggregateResultDomains();postgresNumericResolution();postgresParameterTypes();sqlServerNumericArithmetic();mysqlNumericArithmetic();castDialectContracts();typedLiterals();intervalAndExtractContexts();intervalDeclarationContexts();storedNullabilityConstraints();nullableScopes();routineTypeContexts();scriptTypesAndLimits();
  std::cout<<"PASS canonical type contexts, declarations, casts, scopes, script updates and budgets\n";return 0;
}catch(const std::exception &e){std::cerr<<"FAIL SQL types: "<<e.what()<<'\n';return 1;}}
