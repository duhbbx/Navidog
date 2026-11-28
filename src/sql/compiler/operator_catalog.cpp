#include "navidog/sql/compiler/operator_catalog.hpp"
#include "parser.hpp"
#include <limits>
#include <algorithm>
#include <unordered_map>
#include <unordered_set>

namespace navidog::sql::compiler {
namespace {
constexpr unsigned mask(Dialect d) {
  const auto index = static_cast<unsigned>(d);
  return index < std::numeric_limits<unsigned>::digits ? 1u << index : 0u;
}
constexpr unsigned PG=mask(Dialect::PostgreSQL), ORA=mask(Dialect::Oracle),
 SS=mask(Dialect::SQLServer), SQ=mask(Dialect::SQLite),
 TI=mask(Dialect::TiDB), OB=mask(Dialect::OceanBase),
 MY=mask(Dialect::MySQL)|mask(Dialect::MariaDB)|OB|TI, ALL=PG|ORA|SS|SQ|MY;
constexpr auto many=std::numeric_limits<std::size_t>::max();
struct Entry { OperatorInfo info;unsigned dialects;bool pgQualified; };
const std::unordered_map<std::string,Entry> &registry() {
  static const auto entries=[] {
    std::unordered_map<std::string,Entry> out;
    auto add=[&](const char* name,RoutineKind kind,unsigned dialects,
                 std::size_t lo,std::size_t hi,ValueType type=ValueType::Unknown,
                 bool first=false,bool common=false,bool qualified=true,
                 FunctionVolatility volatility=FunctionVolatility::Immutable) {
      out.emplace(name,Entry{{name,kind,volatility,lo,hi,type,first,common,false,true},dialects,qualified});
    };
    const auto agg=RoutineKind::Aggregate, scalar=RoutineKind::Scalar, win=RoutineKind::Window;
    add("GROUPING",scalar,PG|ORA|SS|MY,1,64,ValueType::Integer,false,false,false);
    add("COUNT",agg,ALL,1,1,ValueType::Integer);
    add("COUNT_BIG",agg,SS,1,1,ValueType::Integer);
    add("TIDB_VERSION",scalar,TI,0,0,ValueType::String,false,false,false);
    add("TIDB_CURRENT_TSO",scalar,TI,0,0,ValueType::Integer,false,false,false,FunctionVolatility::Volatile);
    add("OB_VERSION",scalar,OB,0,0,ValueType::String,false,false,false);
    for(auto name:{"SUM","MIN","MAX"})add(name,agg,ALL,1,1,ValueType::Unknown,true);
    add("AVG",agg,ALL,1,1,ValueType::Decimal);
    add("TOTAL",agg,SQ,1,1,ValueType::Decimal);
    for(auto name:{"STDDEV","STDDEV_POP","STDDEV_SAMP","VARIANCE","VAR_POP","VAR_SAMP"})
      add(name,agg,PG|ORA|MY,1,1,ValueType::Decimal);
    add("STD",agg,MY,1,1,ValueType::Decimal);
    for(auto name:{"BIT_AND","BIT_OR","BIT_XOR","JSON_ARRAYAGG"})
      add(name,agg,MY,1,1);
    add("JSON_OBJECTAGG",agg,MY,2,2);
    add("STRING_AGG",agg,PG|SS,2,2,ValueType::String);
    add("GROUP_CONCAT",agg,MY|SQ,1,many,ValueType::String);
    add("LISTAGG",agg,ORA,1,2,ValueType::String);
    for(auto name:{"ARRAY_AGG","JSON_AGG","JSONB_AGG"})add(name,agg,PG,1,1);
    add("MODE",agg,PG,1,1,ValueType::Unknown,true);
    for (auto name:{"PERCENTILE_DISC","PERCENTILE_CONT"})
      add(name,agg,PG,2,2);
    for (auto name:{"MODE","PERCENTILE_DISC","PERCENTILE_CONT"})
      out.at(name).info.requiresWithinGroup=true;
    add("UNNEST",RoutineKind::TableFunction,PG,1,1,ValueType::Unknown);
    add("GENERATE_SERIES",RoutineKind::TableFunction,PG,2,4,ValueType::Unknown,
        false,false,true,FunctionVolatility::Stable);
    for(auto name:{"BOOL_AND","BOOL_OR"})add(name,agg,PG,1,1,ValueType::Boolean);
    for(auto name:{"ROW_NUMBER","RANK","DENSE_RANK"})add(name,win,ALL,0,0,ValueType::Integer);
    for(auto name:{"PERCENT_RANK","CUME_DIST"})add(name,win,PG|ORA|MY|SQ|SS,0,0,ValueType::Decimal);
    add("NTILE",win,ALL,1,1,ValueType::Integer);
    for(auto name:{"LAG","LEAD"})add(name,win,ALL,1,3,ValueType::Unknown,true);
    for(auto name:{"FIRST_VALUE","LAST_VALUE"})add(name,win,ALL,1,1,ValueType::Unknown,true);
    add("NTH_VALUE",win,PG|ORA|MY|SQ,2,2,ValueType::Unknown,true);
    add("ABS",scalar,ALL,1,1,ValueType::Unknown,true);
    add("ROUND",scalar,ALL,1,3,ValueType::Unknown,true);
    add("COALESCE",scalar,ALL,1,many,ValueType::Unknown,false,true,false);
    add("NULLIF",scalar,ALL,2,2,ValueType::Unknown,true,false,false);
    for(auto name:{"LOWER","UPPER"})add(name,scalar,ALL,1,1,ValueType::String);
    for(auto name:{"TRIM","LTRIM","RTRIM"})add(name,scalar,ALL,1,2,ValueType::String);
    add("REPLACE",scalar,ALL,3,3,ValueType::String);
    for(auto name:{"CONCAT","CONCAT_WS"})add(name,scalar,PG|MY|SS,1,many,ValueType::String);
    add("SUBSTRING",scalar,PG|MY|SS|SQ,2,3,ValueType::String);
    add("LENGTH",scalar,PG|ORA|MY|SQ,1,1,ValueType::Integer);
    add("CHAR_LENGTH",scalar,PG|MY,1,1,ValueType::Integer);
    add("OCTET_LENGTH",scalar,PG|MY|SQ,1,1,ValueType::Integer);
    add("LEN",scalar,SS,1,1,ValueType::Integer);
    add("INSTR",scalar,ORA|MY|SQ,2,4,ValueType::Integer);
    add("SUBSTR",scalar,PG|ORA|MY|SQ,2,3,ValueType::String);
    for(auto name:{"LEFT","RIGHT"})add(name,scalar,PG|MY|SS,2,2,ValueType::String);
    add("IFNULL",scalar,MY|SQ,2,2,ValueType::Unknown,false,true);
    add("IF",scalar,MY,3,3,ValueType::Unknown,false,true);
    add("NVL",scalar,ORA,2,2,ValueType::Unknown,false,true);
    add("NVL2",scalar,ORA,3,3);
    add("DECODE",scalar,ORA,3,many);
    add("ISNULL",scalar,SS,2,2,ValueType::Unknown,true);
    for(auto name:{"TO_CHAR","TO_DATE","TO_NUMBER"})add(name,scalar,PG|ORA,1,3);
    for(auto name:{"GREATEST","LEAST"})add(name,scalar,PG|ORA|MY,1,many,ValueType::Unknown,false,true,false);
    for(auto name:{"DATE_TRUNC","EXTRACT","SPLIT_PART"})add(name,scalar,PG,1,3);
    for(auto name:{"DATE_FORMAT","DATE_ADD","DATE_SUB","ADDDATE","SUBDATE"})add(name,scalar,MY,2,2);
    add("JSON_EXTRACT",scalar,MY|SQ,2,many);
    for (auto name:{"JSON_OBJECT","JSON_ARRAY"})add(name,scalar,SS,0,many,ValueType::String);
    for(auto name:{"ADD_MONTHS","MONTHS_BETWEEN","TRUNC"})add(name,scalar,ORA,1,2);
    for(auto name:{"DATEADD","DATEDIFF","CONVERT","TRY_CONVERT","TRY_CAST"})add(name,scalar,SS,1,4);
    for(auto name:{"ERROR_NUMBER","ERROR_SEVERITY","ERROR_STATE","ERROR_LINE"})
      add(name,scalar,SS,0,0,ValueType::Integer,false,false,false,FunctionVolatility::Stable);
    for(auto name:{"ERROR_MESSAGE","ERROR_PROCEDURE"})
      add(name,scalar,SS,0,0,ValueType::String,false,false,false,FunctionVolatility::Stable);
    for(auto name:{"STRFTIME","JULIANDAY","DATETIME"})add(name,scalar,SQ,1,many);
    for(auto name:{"DATE","TIME"})add(name,scalar,MY,1,1);
    add("TIMESTAMP",scalar,MY,1,2);
    add("NOW",scalar,PG|MY,0,1,ValueType::Unknown,false,false,true,FunctionVolatility::Stable);
    for(auto name:{"CURDATE","UTC_DATE"})
      add(name,scalar,MY,0,0,ValueType::Unknown,false,false,false,FunctionVolatility::Stable);
    for(auto name:{"CURTIME","UTC_TIME","UTC_TIMESTAMP"})
      add(name,scalar,MY,0,1,ValueType::Unknown,false,false,false,FunctionVolatility::Stable);
    add("CURRENT_DATE",scalar,ALL,0,0,ValueType::Unknown,false,false,false,FunctionVolatility::Stable);
    add("CURRENT_USER",scalar,PG|MY|SS,0,0,ValueType::String,false,false,false,FunctionVolatility::Stable);
    for (auto name:{"CURRENT_CATALOG","CURRENT_ROLE","CURRENT_SCHEMA","USER"})
      add(name,scalar,PG,0,0,ValueType::String,false,false,false,FunctionVolatility::Stable);
    for (auto name:{"USER","SESSION_USER","SYSTEM_USER"})
      if (auto found=out.find(name);found!=out.end()) found->second.dialects|=SS;
      else add(name,scalar,SS,0,0,ValueType::String,false,false,false,FunctionVolatility::Stable);
    for(auto name:{"CURRENT_TIME","CURRENT_TIMESTAMP","LOCALTIME","LOCALTIMESTAMP"})
      add(name,scalar,std::string_view(name)=="CURRENT_TIMESTAMP"?ALL:ALL&~SS,0,1,ValueType::Unknown,false,false,false,FunctionVolatility::Stable);
    for(auto name:{"SYSDATE","SYSTIMESTAMP"})add(name,scalar,ORA,0,0,ValueType::Unknown,false,false,false,FunctionVolatility::Stable);
    add("INTERVAL",scalar,MY,2,many,ValueType::Integer);
    for(auto name:{"GETDATE","SYSDATETIME"})add(name,scalar,SS,0,0,ValueType::Unknown,false,false,true,FunctionVolatility::Stable);
    add("RANDOM",scalar,PG|SQ,0,0,ValueType::Unknown,false,false,true,FunctionVolatility::Volatile);
    add("RANDOMBLOB",scalar,SQ,1,1,ValueType::Binary,false,false,true,FunctionVolatility::Volatile);
    add("RAND",scalar,MY|SS,0,1,ValueType::Decimal,false,false,true,FunctionVolatility::Volatile);
    add("UUID",scalar,MY,0,0,ValueType::String,false,false,true,FunctionVolatility::Volatile);
    add("NEWID",scalar,SS,0,0,ValueType::Unknown,false,false,true,FunctionVolatility::Volatile);
    for(auto name:{"NEXTVAL","SETVAL"})add(name,scalar,PG,1,3,ValueType::Integer,false,false,true,FunctionVolatility::Volatile);
    for(auto name:{"VAR","VARP","STDEV","STDEVP"})add(name,agg,SS,1,1,ValueType::Decimal);
    for(auto name:{"REGEXP_LIKE","JSON_EXISTS","LNNVL","XMLEXISTS"})add(name,scalar,ORA,1,4,ValueType::Boolean);
    for(auto name:{"CONTAINS","FREETEXT"})add(name,scalar,SS,2,3,ValueType::Boolean);
    return out;
  }();
  return entries;
}
}
TypeResolution resolveAggregateType(Dialect dialect,std::string_view function,const SqlType &input) {
  TypeResolution out;auto &type=out.type;type.nullability=Nullability::Nullable;
  const auto name=asciiUpper(std::string(function));
  auto integer=[&](int bits){type.family=TypeFamily::Integer;type.bits=bits;type.unsignedInteger=false;};
  auto decimal=[&]{type.family=TypeFamily::Decimal;};
  auto floating=[&](int bits){type.family=TypeFamily::Floating;type.bits=bits;};
  if(name=="COUNT" || name=="COUNT_BIG") {
    if(dialect==Dialect::Oracle)decimal();
    else if(dialect==Dialect::SQLServer)integer(name=="COUNT_BIG"?64:32);
    else {integer(64);if(isMySqlDialect(dialect))type.unsignedInteger.reset();}
    type.nullability=dialect==Dialect::SQLServer?Nullability::Unknown:Nullability::NotNull;return out;
  }
  if(name=="TOTAL" && dialect==Dialect::SQLite){floating(64);type.nullability=Nullability::NotNull;return out;}
  if(name=="MIN" || name=="MAX") {
    if(dialect==Dialect::MySQL || dialect==Dialect::SQLServer)type=input;
    else if(dialect==Dialect::PostgreSQL && (input.family==TypeFamily::Integer || input.family==TypeFamily::Floating || input.family==TypeFamily::Date || input.family==TypeFamily::Time || input.family==TypeFamily::Timestamp || input.family==TypeFamily::Decimal)) {
      type=input;type.precision.reset();type.scale.reset();
    }
    type.nullability=Nullability::Nullable;return out;
  }
  if(name!="SUM" && name!="AVG")return out;
  if(dialect==Dialect::SQLite) {if(name=="AVG")floating(64);else type.family=TypeFamily::Numeric;return out;}
  const bool pg=dialect==Dialect::PostgreSQL,ms=dialect==Dialect::SQLServer;
  const bool numeric=input.family==TypeFamily::Integer || input.family==TypeFamily::Decimal || input.family==TypeFamily::Floating || input.family==TypeFamily::Numeric;
  if((pg || ms) && input.family!=TypeFamily::Unknown && input.family!=TypeFamily::Null &&
      ((!numeric && !(pg && input.family==TypeFamily::Interval)) || input.arrayDimensions || input.sqliteAffinity ||
       (ms && input.family==TypeFamily::Integer && input.bits==1))) {
    out.error="no matching known native "+name+" overload for this argument type";return out;
  }
  if(input.arrayDimensions || input.sqliteAffinity || input.family==TypeFamily::Unknown || input.family==TypeFamily::Null)return out;
  if(pg) {
    if(input.family==TypeFamily::Integer) {
      if(name=="AVG" || input.bits==64)decimal();
      else if(input.bits && *input.bits<=32)integer(64);
      else type.family=TypeFamily::Numeric;
    } else if(input.family==TypeFamily::Decimal)decimal();
    else if(input.family==TypeFamily::Floating) {if(name=="AVG")floating(64);else if(input.bits)floating(*input.bits);else type.family=TypeFamily::Floating;}
    else if(input.family==TypeFamily::Interval)type.family=TypeFamily::Interval;
    else if(input.family==TypeFamily::Numeric)type.family=TypeFamily::Numeric;
  } else if(ms) {
    if(input.family==TypeFamily::Integer) {if(input.bits)integer(*input.bits<=32?32:64);else type.family=TypeFamily::Integer;}
    else if(input.family==TypeFamily::Decimal) {decimal();type.precision=38;if(input.scale)type.scale=name=="AVG"?std::max(6,*input.scale):*input.scale;}
    else if(input.family==TypeFamily::Floating)floating(64);
    else if(input.family==TypeFamily::Numeric)type.family=TypeFamily::Numeric;
  } else if(dialect==Dialect::MySQL) {
    if(input.family==TypeFamily::Integer || input.family==TypeFamily::Decimal)decimal();
    else if(input.family==TypeFamily::Floating)floating(64);
    else type.family=TypeFamily::Numeric; // implicit input conversion / SQL-mode facts are not known
  } else if(dialect==Dialect::Oracle) {
    if(input.family==TypeFamily::Decimal || input.family==TypeFamily::Integer)decimal();
    else if(input.family==TypeFamily::Floating){type=input;type.nullability=Nullability::Nullable;}
  }
  return out;
}
std::optional<OperatorInfo> builtinOperator(Dialect dialect,
    const std::vector<std::string>& name,const std::vector<bool>& quoted,std::size_t arguments) {
  if(name.empty() || name.size()>2)return {};
  const auto last=name.size()-1;
  if(dialect==Dialect::SQLServer && last<quoted.size() && quoted[last] &&
      (asciiUpper(name[last])=="JSON_OBJECT" || asciiUpper(name[last])=="JSON_ARRAY"))return {};
  if(last<quoted.size() && quoted[last] && canonicalIdentifier(dialect,name[last],false)!=name[last])return {};
  const bool qualified=name.size()==2;
  if(qualified && (dialect!=Dialect::PostgreSQL || name[0]!="pg_catalog"))return {};
  auto it=registry().find(asciiUpper(name[last]));
  if(it==registry().end() || !(it->second.dialects & mask(dialect)) || (qualified && !it->second.pgQualified))return {};
  auto out=it->second.info;
  if(out.name=="CURRENT_DATE")out.maxArguments=0;
  if(out.name=="GROUPING")out.maxArguments=(dialect==Dialect::Oracle || dialect==Dialect::SQLServer)?1:dialect==Dialect::PostgreSQL?31:64;
  if(dialect==Dialect::Oracle && out.name=="REPLACE")out.minArguments=2;
  if(out.name=="ROUND") {out.maxArguments=dialect==Dialect::SQLServer?3:2;if(dialect==Dialect::SQLServer)out.minArguments=2;}
  if(dialect==Dialect::PostgreSQL && out.name=="NOW")out.maxArguments=0;
  if(dialect==Dialect::SQLServer && out.name=="AVG") {out.fixedResult=ValueType::Unknown;out.resultFromFirst=true;}
  if(dialect==Dialect::SQLite && (out.name=="MIN" || out.name=="MAX") && arguments>1) {
    out.kind=RoutineKind::Scalar;out.minArguments=2;out.maxArguments=many;
  }
  return out;
}
std::vector<OperatorInfo> builtinOperators(Dialect dialect) {
  std::vector<OperatorInfo> out;
  for(const auto& entry:registry())if(entry.second.dialects & mask(dialect))out.push_back(entry.second.info);
  return out;
}
bool builtinAcceptsDistinct(Dialect dialect,std::string_view spelling) {
  if (!isMySqlDialect(dialect)) return true;
  const auto name=asciiUpper(std::string(spelling));
  if (name=="BIT_AND" || name=="BIT_OR" || name=="BIT_XOR" || name=="JSON_OBJECTAGG") return false;
  if (name=="JSON_ARRAYAGG") return dialect==Dialect::MariaDB || dialect==Dialect::OceanBase;
  if (name=="STD" || name=="STDDEV" || name=="STDDEV_POP" || name=="STDDEV_SAMP" ||
      name=="VARIANCE" || name=="VAR_POP" || name=="VAR_SAMP") return dialect==Dialect::TiDB;
  if (dialect!=Dialect::OceanBase) {
    const auto found=registry().find(name);
    if (found!=registry().end() && (found->second.dialects & mask(dialect)) &&
        found->second.info.kind!=RoutineKind::Aggregate) return false;
  }
  return true;
}
bool nativeFunctionNeedsAdjacentParen(Dialect dialect,std::string_view spelling) {
  if (dialect==Dialect::TiDB) {
    // TiDB's btFuncTokenMap differs from MySQL's function token group.
    static const std::unordered_set<std::string> names{
        "BIT_AND","BIT_OR","BIT_XOR","CAST","COUNT","APPROX_COUNT_DISTINCT","APPROX_PERCENTILE",
        "CURDATE","CURTIME","DATE_ADD","DATE_SUB","EXTRACT","GROUP_CONCAT","MAX","MID","MIN",
        "NOW","POSITION","STD","STDDEV","STDDEV_POP","STDDEV_SAMP","SUBSTR","SUBSTRING","SUM",
        "SYSDATE","TRANSLATE","TRIM","VARIANCE","VAR_POP","VAR_SAMP"};
    return names.count(asciiUpper(std::string(spelling)));
  }
  if (dialect!=Dialect::MySQL && dialect!=Dialect::MariaDB) return false;
  // Pinned MySQL 8.4 lex.h SG_FUNCTIONS / MariaDB 11.4 sql_functions.
  static const std::unordered_set<std::string> shared{
      "ADDDATE","BIT_AND","BIT_OR","BIT_XOR","CAST","COUNT","CURDATE","CURTIME",
      "DATE_ADD","DATE_SUB","EXTRACT","GROUP_CONCAT","JSON_ARRAYAGG","JSON_OBJECTAGG",
      "MAX","MID","MIN","NOW","POSITION","SESSION_USER","STD","STDDEV","STDDEV_POP",
      "STDDEV_SAMP","SUBDATE","SUBSTR","SUBSTRING","SUM","SYSTEM_USER","TRIM",
      "VARIANCE","VAR_POP","VAR_SAMP"};
  const auto name=asciiUpper(std::string(spelling));
  if (shared.count(name)) return true;
  if (dialect==Dialect::MySQL) return name=="SYSDATE" || name=="ST_COLLECT";
  static const std::unordered_set<std::string> maria{
      "CUME_DIST","DENSE_RANK","FIRST_VALUE","LAG","LEAD","MEDIAN","NTH_VALUE","NTILE",
      "PERCENT_RANK","PERCENTILE_CONT","PERCENTILE_DISC","RANK","TRIM_ORACLE"};
  return maria.count(name);
}
bool builtinAcceptsDistinctAll(Dialect dialect,std::string_view spelling) {
  const auto name=asciiUpper(std::string(spelling));
  if (dialect==Dialect::MySQL || dialect==Dialect::MariaDB || dialect==Dialect::TiDB) {
    if (name=="SUM" || name=="AVG" || name=="MIN" || name=="MAX") return true;
  }
  return dialect==Dialect::TiDB && (name=="GROUP_CONCAT" || name=="STD" || name=="STDDEV" ||
      name=="STDDEV_POP" || name=="STDDEV_SAMP" || name=="VARIANCE" || name=="VAR_POP" || name=="VAR_SAMP");
}
RoutineKind callKind(const ModelExpression& e,Dialect dialect) {
  if(e.kind==ExprKind::AggregateFilter)return RoutineKind::Aggregate;
  if(e.kind!=ExprKind::Call)return RoutineKind::Unknown;
  if(e.routineResolved)return e.routineKind;
  auto op=builtinOperator(dialect,e.name,e.nameQuoted,e.operands.size());
  return op?op->kind:RoutineKind::Unknown;
}
FunctionVolatility callVolatility(const ModelExpression& e,Dialect dialect) {
  if(e.routineResolved)return e.routineVolatility;
  auto op=builtinOperator(dialect,e.name,e.nameQuoted,e.operands.size());
  return op?op->volatility:FunctionVolatility::Unknown;
}
const char* routineKindName(RoutineKind kind) {
  switch(kind) {
  case RoutineKind::Scalar:return "scalar";
  case RoutineKind::Aggregate:return "aggregate";
  case RoutineKind::Window:return "window";
  case RoutineKind::Procedure:return "procedure";
  case RoutineKind::TableFunction:return "table-function";
  default:return "unknown";
  }
}
const char* volatilityName(FunctionVolatility value) {
  switch(value) {
  case FunctionVolatility::Immutable:return "immutable";
  case FunctionVolatility::Stable:return "stable";
  case FunctionVolatility::Volatile:return "volatile";
  default:return "unknown";
  }
}
} // namespace navidog::sql::compiler
