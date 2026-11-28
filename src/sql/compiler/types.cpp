#include "parser.hpp"
#include <algorithm>
#include <limits>
#include <set>
#include <stdexcept>

namespace navidog::sql::compiler {
namespace {
std::string upper(std::string value) {
  for(auto &c:value)if(c>='a' && c<='z')c-=32;
  return value;
}
std::size_t typeBytes(const SqlType &type) {
  std::size_t bytes=sizeof(type)+type.flavor.size();
  for(const auto *parts:{&type.opaqueName,&type.opaqueArguments,&type.opaqueModifiers,&type.characterSet,&type.collation})
    for(const auto &part:*parts)bytes+=sizeof(part)+part.size();
  return bytes;
}
bool mysql(Dialect d) { return isMySqlDialect(d); }
std::optional<int> integer(std::string text) {
  if(text.empty() || text.size()>12)return {};
  std::size_t start=text[0]=='-' || text[0]=='+'?1:0;
  if(start==text.size() || text.find_first_not_of("0123456789",start)!=std::string::npos)return {};
  try { const auto value=std::stoll(text);if(value < std::numeric_limits<int>::min() || value > std::numeric_limits<int>::max())return {};return int(value); }
  catch(const std::exception &){return {};}
}
}
const std::vector<std::string> &mysqlCastTypeNames() {
  static const std::vector<std::string> names={"BINARY","CHAR","NCHAR","DATE","DATETIME","DECIMAL","DOUBLE","FLOAT","JSON","REAL","SIGNED","TIME","UNSIGNED","YEAR","POINT","LINESTRING","POLYGON","MULTIPOINT","MULTILINESTRING","MULTIPOLYGON","GEOMETRYCOLLECTION"};
  return names;
}
TypeContext::TypeContext() { intern({}); }
TypeId TypeContext::intern(SqlType type,std::size_t limit) {
  if(static_cast<unsigned>(type.family)>static_cast<unsigned>(TypeFamily::Interval) ||
     static_cast<unsigned>(type.nullability)>static_cast<unsigned>(Nullability::NotNull) ||
     static_cast<unsigned>(type.timeZone)>static_cast<unsigned>(TimeZoneKind::Local) ||
     static_cast<unsigned>(type.lengthUnit)>static_cast<unsigned>(LengthUnit::Utf16CodeUnits) ||
     static_cast<unsigned>(type.intervalLeading)>static_cast<unsigned>(IntervalField::Second) ||
     static_cast<unsigned>(type.intervalTrailing)>static_cast<unsigned>(IntervalField::Second) ||
     static_cast<unsigned>(type.intervalUnit)>static_cast<unsigned>(IntervalUnit::SecondMicrosecond) ||
     (type.intervalLeadingPrecision && (*type.intervalLeadingPrecision<0 || *type.intervalLeadingPrecision>9)) ||
     (type.bits && (*type.bits<1 || *type.bits>128)))throw std::invalid_argument("invalid SQL type descriptor");
  if(typeBytes(type)>65536)throw std::length_error("SQL type descriptor payload limit exceeded");
  if(auto found=ids_.find(type);found!=ids_.end())return found->second;
  if(types_.size()>=std::min<std::size_t>(65536,std::max<std::size_t>(1,limit)))
    throw std::length_error("SQL type context limit exceeded");
  if(type.arrayDimensions<0 || type.arrayDimensions>32 || typeBytes(type)>65536)
    throw std::length_error("SQL type descriptor payload limit exceeded");
  if(type.precision && (*type.precision < -1000000 || *type.precision > 1000000))throw std::invalid_argument("invalid type precision");
  if(type.scale && (*type.scale < -1000000 || *type.scale > 1000000))throw std::invalid_argument("invalid type scale");
  const auto charge=2*(typeBytes(type)+sizeof(TypeId)+64);
  if(charge>8*1024*1024-bytes_)throw std::length_error("SQL type context byte limit exceeded");
  const auto id=TypeId(types_.size());
  // Store both copies before exposing the ID; a failed allocation leaves no dangling handle.
  auto inserted=ids_.emplace(type,id);
  try { types_.push_back(std::move(type)); }
  catch(...) { ids_.erase(inserted.first);throw; }
  bytes_+=charge;
  return id;
}
TypeId TypeContext::withNullability(TypeId id,Nullability nullability,std::size_t limit) {
  auto type=get(id);type.nullability=nullability;return intern(std::move(type),limit);
}
const SqlType &TypeContext::get(TypeId id) const {return types_.at(id);}
const char *typeFamilyName(TypeFamily type) {
  switch(type) {
  case TypeFamily::Unknown:return "unknown";case TypeFamily::Null:return "null";
  case TypeFamily::Boolean:return "boolean";case TypeFamily::Integer:return "integer";
  case TypeFamily::Numeric:return "numeric-unknown-exactness";
  case TypeFamily::String:return "string";case TypeFamily::Bytes:return "bytes";
  case TypeFamily::Decimal:return "decimal";case TypeFamily::Floating:return "floating";
  case TypeFamily::Character:return "character";case TypeFamily::VarCharacter:return "varchar";
  case TypeFamily::Binary:return "binary";case TypeFamily::VarBinary:return "varbinary";
  case TypeFamily::Date:return "date";case TypeFamily::Time:return "time";
  case TypeFamily::Timestamp:return "timestamp";case TypeFamily::Interval:return "interval";
  }
  return "unknown";
}
const char *nullabilityName(Nullability n) {
  return n==Nullability::Nullable?"nullable":n==Nullability::NotNull?"not-null":"unknown";
}
const char *timeZoneName(TimeZoneKind zone) {
  switch(zone){case TimeZoneKind::Without:return "without";case TimeZoneKind::With:return "with";
  case TimeZoneKind::Local:return "local";default:return "unknown";}
}
const char *lengthUnitName(LengthUnit unit) {
  switch(unit){case LengthUnit::Characters:return "characters";case LengthUnit::Bytes:return "bytes";
  case LengthUnit::Utf16CodeUnits:return "utf16-code-units";default:return "unknown";}
}
SqlType categoryType(ValueType value,Nullability nullable) {
  SqlType type;type.nullability=nullable;
  switch(value) {
  case ValueType::Null:type.family=TypeFamily::Null;type.nullability=Nullability::Nullable;break;
  case ValueType::Boolean:type.family=TypeFamily::Boolean;break;
  case ValueType::Integer:type.family=TypeFamily::Integer;break;
  case ValueType::Decimal:type.family=TypeFamily::Numeric;break;
  case ValueType::String:type.family=TypeFamily::String;break;
  case ValueType::Binary:type.family=TypeFamily::Bytes;break;
  default:break;
  }
  return type;
}
ValueType valueCategory(const SqlType &type) {
  if(type.arrayDimensions)return ValueType::Unknown;
  switch(type.family) {
  case TypeFamily::Null:return ValueType::Null;
  case TypeFamily::Boolean:return ValueType::Boolean;
  case TypeFamily::Integer:return ValueType::Integer;
  case TypeFamily::Numeric:case TypeFamily::Decimal:case TypeFamily::Floating:return ValueType::Decimal;
  case TypeFamily::String:case TypeFamily::Character:case TypeFamily::VarCharacter:return ValueType::String;
  case TypeFamily::Bytes:case TypeFamily::Binary:case TypeFamily::VarBinary:return ValueType::Binary;
  default:return ValueType::Unknown;
  }
}
TypeResolution resolveDeclaredType(const BasicTypeSpec<std::vector<std::string>> &syntax,
                                   Dialect dialect,TypeUse use) {
  TypeResolution result;auto &type=result.type;
  type.arrayDimensions=dialect==Dialect::PostgreSQL && syntax.arrayDimensions>0?1:syntax.arrayDimensions;
  if(syntax.name.empty())return result;
  if(dialect==Dialect::SQLite) {
    type.sqliteAffinity=true;type.opaqueName=syntax.name;type.opaqueModifiers=syntax.modifiers;
    for(const auto &arg:syntax.arguments)type.opaqueArguments.push_back(std::to_string(int(arg.kind))+":"+arg.value);
    return result;
  }
  if(use==TypeUse::Signature && dialect==Dialect::PostgreSQL) {
    auto checked=resolveDeclaredType(syntax,dialect,TypeUse::Declaration);
    if(!checked.error.empty())return checked;
    auto plain=syntax;
    const bool floatAlias=plain.name.size()==1 && upper(plain.name.front())=="FLOAT" &&
                         (plain.nameQuoted.empty() || !plain.nameQuoted.front());
    if(!floatAlias)plain.arguments.clear();
    plain.intervalType.reset(); // PostgreSQL signatures discard interval typmods too.
    auto result=resolveDeclaredType(plain,dialect,TypeUse::Declaration);
    if(result.type.family==TypeFamily::Decimal || result.type.family==TypeFamily::Time || result.type.family==TypeFamily::Timestamp) {result.type.precision.reset();result.type.scale.reset();}
    result.type.length.reset();
    return result;
  }
  if(use==TypeUse::Signature && dialect==Dialect::Oracle && (!syntax.arguments.empty() || (syntax.intervalType && (syntax.intervalType->leadingPrecision || syntax.intervalType->fractionalPrecision)))) {
    result.error="Oracle formal parameter / return types cannot include inline length, precision or scale constraints";
    return result;
  }
  // Qualified pg_catalog names designate actual catalog type names, not SQL
  // aliases (pg_catalog.integer is not pg_catalog.int4). Quoted names must
  // match their lower-case catalog spelling exactly. Keep original AST/IR.
  if(dialect==Dialect::PostgreSQL && syntax.name.size()==2) {
    const auto quoted=[&](std::size_t i){return i<syntax.nameQuoted.size() && syntax.nameQuoted[i];};
    const auto schema=quoted(0)?syntax.name[0]:canonicalIdentifier(dialect,syntax.name[0],false);
    const auto native=quoted(1)?syntax.name[1]:canonicalIdentifier(dialect,syntax.name[1],false);
    static const std::map<std::string,std::string> names={
      {"int2","SMALLINT"},{"int4","INTEGER"},{"int8","BIGINT"},{"float4","REAL"},{"float8","DOUBLE"},
      {"numeric","NUMERIC"},{"varchar","VARCHAR"},{"bpchar","CHAR"},{"text","TEXT"},{"bool","BOOLEAN"},
      {"bytea","BYTEA"},{"date","DATE"},{"time","TIME"},{"timetz","TIMETZ"},{"timestamp","TIMESTAMP"},{"timestamptz","TIMESTAMPTZ"}};
    const auto found=names.find(native);
    if(schema=="pg_catalog" && found!=names.end()) {
      static const std::set<std::string> modifiers={"numeric","varchar","bpchar","time","timetz","timestamp","timestamptz"};
      if(!syntax.arguments.empty() && !modifiers.count(native)) {
        result.error="this PostgreSQL native catalog type does not accept type modifiers";return result;
      }
      auto plain=syntax;plain.name={found->second};plain.nameQuoted={false};
      auto resolved=resolveDeclaredType(plain,dialect,use);
      if(native=="bpchar" && syntax.arguments.empty())resolved.type.length.reset();
      return resolved;
    }
  }
  const auto name=upper(syntax.name.back());
  if (dialect==Dialect::TiDB && syntax.name.size()==1 &&
      (syntax.nameQuoted.empty() || !syntax.nameQuoted[0])) {
    static const std::map<std::string,std::string> aliases{
        {"MIDDLEINT","MEDIUMINT"},{"INT1","TINYINT"},{"INT2","SMALLINT"},{"INT3","MEDIUMINT"},
        {"INT4","INT"},{"INT8","BIGINT"},{"FIXED","DECIMAL"},{"FLOAT4","FLOAT"},{"FLOAT8","DOUBLE"},
        {"VARCHARACTER","VARCHAR"},{"SQL_TSI_YEAR","YEAR"}};
    if (const auto found=aliases.find(name);found!=aliases.end()) {
      auto plain=syntax;plain.name={found->second};plain.nameQuoted={false};
      return resolveDeclaredType(plain,dialect,use);
    }
  }
  if(dialect==Dialect::PostgreSQL && syntax.name.size()==1 &&
      (syntax.nameQuoted.empty() || !syntax.nameQuoted[0]) && (name=="FLOAT4" || name=="FLOAT8")) {
    if(!syntax.arguments.empty()){result.error="PostgreSQL float4 / float8 do not accept type modifiers";return result;}
    auto plain=syntax;plain.name={name=="FLOAT4"?"REAL":"DOUBLE"};plain.nameQuoted={false};
    return resolveDeclaredType(plain,dialect,use);
  }
  if (dialect == Dialect::TiDB && syntax.name.size() == 1) {
    static const std::set<std::string> spatial = {"GEOMETRY", "POINT", "LINESTRING", "POLYGON",
        "MULTIPOINT", "MULTILINESTRING", "MULTIPOLYGON", "GEOMETRYCOLLECTION"};
    if (spatial.count(name)) {
      result.error = "TiDB does not support spatial types";
      return result;
    }
  }
  if((dialect==Dialect::MySQL || dialect==Dialect::TiDB) && use==TypeUse::Cast) {
    static const std::set<std::string> declarationOnly={"INT","INTEGER","BIGINT","SMALLINT","TINYINT","MEDIUMINT","VARCHAR","NVARCHAR","VARBINARY","BOOLEAN","BOOL","TEXT","BLOB","TIMESTAMP"};
    if(syntax.name.size()!=1 || (!syntax.nameQuoted.empty() && syntax.nameQuoted[0]) || declarationOnly.count(name)) {
      result.error="This dialect does not accept this CAST target; integer casts use SIGNED / UNSIGNED (64-bit)";return result;
    }
    for(const auto &modifier:syntax.modifiers)if(modifier=="UNSIGNED" || modifier=="ZEROFILL" || modifier=="VARYING") {
      result.error="MySQL CAST does not accept this column type modifier";return result;
    }
    if(!syntax.arguments.empty() && (name=="DATE" || name=="DOUBLE" || name=="REAL" || name=="JSON" || name=="SIGNED" || name=="UNSIGNED" || name=="YEAR" || name=="POINT" || name=="LINESTRING" || name=="POLYGON" || name=="MULTIPOINT" || name=="MULTILINESTRING" || name=="MULTIPOLYGON" || name=="GEOMETRYCOLLECTION")) {
      result.error="this MySQL CAST type does not accept parameters";return result;
    }
  }
  if (dialect==Dialect::MariaDB && use==TypeUse::Cast && syntax.name.size()==1 &&
      (syntax.nameQuoted.empty() || !syntax.nameQuoted.front())) {
    static const std::set<std::string> declarationOnly={
        "BIGINT","SMALLINT","BOOLEAN","BOOL","TIMESTAMP","YEAR","JSON"};
    if (declarationOnly.count(name)) {
      result.error="MariaDB CAST does not support this declaration type";
      return result;
    }
    if (name=="INT" || name=="INTEGER") {
      auto signedType=syntax;
      signedType.name={"SIGNED"};
      return resolveDeclaredType(signedType,dialect,use);
    }
  }
  auto opaque=[&] {
    type.opaqueName=syntax.name;type.opaqueModifiers=syntax.modifiers;
    for(std::size_t i=0;i<type.opaqueName.size();++i)type.opaqueName[i]=canonicalIdentifier(dialect,type.opaqueName[i],i<syntax.nameQuoted.size() && syntax.nameQuoted[i]);
    for(const auto &arg:syntax.arguments)type.opaqueArguments.push_back(std::to_string(int(arg.kind))+":"+arg.value);
  };
  const bool postgresType = name=="BYTEA" || name=="TIMESTAMPTZ" || name=="TIMETZ" ||
      ((name=="INT2" || name=="INT4" || name=="INT8") && dialect!=Dialect::TiDB);
  if((postgresType && dialect!=Dialect::PostgreSQL) ||
     ((name=="VARCHAR2" || name=="NVARCHAR2" || name=="NUMBER" || name=="RAW" || name=="PLS_INTEGER" || name=="BINARY_INTEGER" || name=="BINARY_FLOAT" || name=="BINARY_DOUBLE") && dialect!=Dialect::Oracle) ||
     ((name=="DATETIME2" || name=="DATETIMEOFFSET") && dialect!=Dialect::SQLServer) ||
     (name=="TINYINT" && !mysql(dialect) && dialect!=Dialect::SQLServer) ||
     (name=="MEDIUMINT" && !mysql(dialect))) {opaque();return result;}
  if(syntax.name.size()!=1 || (!syntax.nameQuoted.empty() && syntax.nameQuoted.front())) {opaque();return result;}
  auto has=[&](const char *modifier){return std::find(syntax.modifiers.begin(),syntax.modifiers.end(),modifier)!=syntax.modifiers.end();};
  auto arg=[&](std::size_t index)->std::optional<int>{return index<syntax.arguments.size() && syntax.arguments[index].kind==TokenKind::Number?integer(syntax.arguments[index].value):std::nullopt;};
  auto error=[&](std::string text){if(result.error.empty())result.error=std::move(text);};
  auto count=[&](std::size_t max){if(syntax.arguments.size()>max)error("too many arguments for type "+name);};
  if(name=="DECIMAL" || name=="DEC" || name=="NUMERIC" || name=="NUMBER") {
    type.family=TypeFamily::Decimal;count(2);
    type.unsignedInteger=mysql(dialect) && (has("UNSIGNED") || has("ZEROFILL"));
    if(has("ZEROFILL"))type.opaqueModifiers.push_back("ZEROFILL");
    if(dialect==Dialect::SQLite){type.sqliteAffinity=true;opaque();return result;}
    if(!syntax.arguments.empty()) {
      type.precision=arg(0);
      if(!type.precision && !(dialect==Dialect::Oracle && syntax.arguments[0].value=="*"))error("type precision must be an integer");
      type.scale=syntax.arguments.size()>1?arg(1):std::optional<int>{0};
      if(!type.scale)error("type scale must be an integer");
    } else if(mysql(dialect)) {type.precision=10;type.scale=0;}
    else if(dialect==Dialect::SQLServer){type.precision=18;type.scale=0;}
    else if(dialect==Dialect::Oracle && name!="NUMBER"){type.precision=38;type.scale=0;}
    const int maxPrecision=mysql(dialect)?65:dialect==Dialect::PostgreSQL?1000:38;
    if(type.precision && (*type.precision<1 || *type.precision>maxPrecision))error("type precision is outside this dialect's supported range");
    if(type.scale) {
      if(dialect==Dialect::PostgreSQL) {if(*type.scale < -1000 || *type.scale>1000)error("PostgreSQL numeric scale must be between -1000 and 1000");}
      else if(dialect==Dialect::Oracle){if(*type.scale < -84 || *type.scale>127)error("Oracle NUMBER scale must be between -84 and 127");}
      else if(*type.scale<0 || *type.scale>(dialect==Dialect::MariaDB?38:mysql(dialect)?30:38) || (type.precision && *type.scale>*type.precision))
        error("decimal scale is outside this dialect's supported range");
    }
  } else if(mysql(dialect) && (name=="SIGNED" || name=="UNSIGNED")) {type.family=TypeFamily::Integer;type.bits=64;type.unsignedInteger=name=="UNSIGNED";}
  else if(name=="INT" || name=="INTEGER" || name=="INT4" || name=="SMALLINT" || name=="INT2" || name=="BIGINT" || name=="INT8" || name=="TINYINT" || name=="MEDIUMINT" || name=="PLS_INTEGER" || name=="BINARY_INTEGER") {
    if(dialect==Dialect::Oracle && name!="PLS_INTEGER" && name!="BINARY_INTEGER") {
      type.family=TypeFamily::Decimal;type.precision=38;type.scale=0;
    } else {
      type.family=TypeFamily::Integer;
      type.bits=name=="SMALLINT" || name=="INT2"?16:name=="BIGINT" || name=="INT8"?64:name=="TINYINT"?8:name=="MEDIUMINT"?24:32;
      type.unsignedInteger=(mysql(dialect) && (has("UNSIGNED") || has("ZEROFILL"))) || (dialect==Dialect::SQLServer && name=="TINYINT");
    }
    if(!mysql(dialect) && !syntax.arguments.empty())error("integer type does not accept a length here");
  } else if(name=="FLOAT" || name=="REAL" || name=="DOUBLE" || name=="BINARY_FLOAT" || name=="BINARY_DOUBLE") {
    type.family=TypeFamily::Floating;
    type.bits=name=="DOUBLE" || name=="BINARY_DOUBLE"?64:32;
    if(name=="FLOAT" && (dialect==Dialect::PostgreSQL || dialect==Dialect::SQLServer)) {
      count(1);const auto precision=syntax.arguments.empty()?std::optional<int>{53}:arg(0);
      if(!precision || *precision<1 || *precision>53)error("FLOAT binary precision must be between 1 and 53");
      else type.bits=*precision<=24?32:64;
    } else if(dialect==Dialect::Oracle && (name=="FLOAT" || name=="REAL" || name=="DOUBLE")) {
      type.family=TypeFamily::Decimal;type.bits.reset();type.flavor="oracle-float";opaque();
      if(name=="FLOAT" && !syntax.arguments.empty()) {count(1);auto p=arg(0);if(!p || *p<1 || *p>126)error("Oracle FLOAT binary precision must be between 1 and 126");}
    } else if(mysql(dialect) && name=="REAL") {type.bits.reset();type.flavor="mysql-real-sql-mode";opaque();}
    else if(mysql(dialect) && name=="FLOAT" && syntax.arguments.size()==1) {
      auto p=arg(0);if(!p || *p<0 || *p>53)error("MySQL FLOAT binary precision must be between 0 and 53");else type.bits=*p<=24?32:64;
    } else if(!syntax.arguments.empty()) {
      if(dialect==Dialect::PostgreSQL || dialect==Dialect::SQLServer)error("this floating type does not accept precision arguments");
      opaque();
    }
  } else if(name=="BOOL" || name=="BOOLEAN") {type.family=TypeFamily::Boolean;if(mysql(dialect)){type.family=TypeFamily::Integer;type.bits=8;type.unsignedInteger=false;}}
  else if(name=="BIT" && dialect==Dialect::SQLServer){type.family=TypeFamily::Integer;type.bits=1;type.unsignedInteger=true;}
  else if(name=="INTERVAL" && (dialect==Dialect::PostgreSQL || dialect==Dialect::Oracle)) {
    type.family=TypeFamily::Interval;
    if(syntax.intervalType) {
      const auto &s=*syntax.intervalType;
      if(const auto invalid=validateIntervalSpec(s,dialect);!invalid.empty())error(invalid);
      type.intervalLeading=s.leading;type.intervalTrailing=s.trailing;type.precision=s.fractionalPrecision;
      type.intervalLeadingPrecision=s.leadingPrecision;
      if(dialect==Dialect::Oracle) {
        if(!((s.leading==IntervalField::Year && s.trailing==IntervalField::Month) || (s.leading==IntervalField::Day && s.trailing==IntervalField::Second)))error("invalid Oracle interval type qualifier");
        type.intervalLeadingPrecision=s.leadingPrecision.value_or(2);
        if(s.leading==IntervalField::Day && !type.precision)type.precision=6;
        type.flavor=s.leading==IntervalField::Year?"oracle-interval-year-month":"oracle-interval-day-second";
      }
    }
    count(1);
    if(!syntax.arguments.empty()) {type.precision=arg(0);if(!type.precision || *type.precision<0 || *type.precision>6)error("interval precision must be between 0 and 6");}
  }
  else if(name=="DATE") {count(0);type.family=dialect==Dialect::Oracle?TypeFamily::Timestamp:TypeFamily::Date;if(dialect==Dialect::Oracle)type.flavor="oracle-date";}
  else if(name=="TIME" || name=="TIMESTAMP" || name=="DATETIME" || name=="DATETIME2" || name=="TIMESTAMPTZ" || name=="TIMETZ" || name=="DATETIMEOFFSET") {
    if(dialect==Dialect::SQLServer && name=="TIMESTAMP"){type.family=TypeFamily::Binary;type.length=8;type.lengthUnit=LengthUnit::Bytes;type.flavor="rowversion";}
    else {
      type.family=name=="TIME" || name=="TIMETZ"?TypeFamily::Time:TypeFamily::Timestamp;
      count(1);if(!syntax.arguments.empty()){type.precision=arg(0);if(!type.precision || *type.precision<0 || *type.precision>(dialect==Dialect::SQLServer?7:dialect==Dialect::Oracle?9:6))error("fractional-second precision is outside this dialect's supported range");}
      type.timeZone=name=="TIMESTAMPTZ" || name=="TIMETZ" || name=="DATETIMEOFFSET" || has("WITH TIME ZONE")?TimeZoneKind::With:has("WITH LOCAL TIME ZONE")?TimeZoneKind::Local:TimeZoneKind::Without;
      type.flavor=name=="DATETIME" || (mysql(dialect) && use==TypeUse::Literal && name=="TIMESTAMP")?"datetime":std::string{};
      if(use==TypeUse::Literal && dialect==Dialect::Oracle && name=="TIMESTAMP" && syntax.modifiers.empty())
        type.timeZone=TimeZoneKind::Unknown; // Oracle may derive a zone from literal text.
    }
  } else if(name=="CHAR" || name=="CHARACTER" || name=="NCHAR" || name=="VARCHAR" || name=="VARCHAR2" || name=="NVARCHAR" || name=="NVARCHAR2" || name=="TEXT" || name=="NTEXT" || name=="CLOB" || name=="NCLOB" || name=="BYTEA" || name=="BINARY" || name=="VARBINARY" || name=="BLOB" || name=="RAW") {
    const bool binary=name=="BYTEA" || name=="BINARY" || name=="VARBINARY" || name=="BLOB" || name=="RAW";
    const bool fixed=((name=="CHAR" || name=="CHARACTER") && !has("VARYING")) || name=="NCHAR" || name=="BINARY";
    type.family=binary?(fixed?TypeFamily::Binary:TypeFamily::VarBinary):(fixed?TypeFamily::Character:TypeFamily::VarCharacter);
    type.lengthUnit=binary?LengthUnit::Bytes:dialect==Dialect::PostgreSQL || mysql(dialect)?LengthUnit::Characters:LengthUnit::Unknown;
    if(dialect==Dialect::SQLServer && !binary)type.lengthUnit=name=="NCHAR" || name=="NVARCHAR" || name=="NTEXT"?LengthUnit::Utf16CodeUnits:LengthUnit::Bytes;
    if(dialect==Dialect::Oracle && !binary)type.lengthUnit=name=="NCHAR" || name=="NVARCHAR2"?LengthUnit::Characters:
        syntax.lengthSemantics==std::optional<std::string>{"CHAR"}?LengthUnit::Characters:
        syntax.lengthSemantics==std::optional<std::string>{"BYTE"}?LengthUnit::Bytes:LengthUnit::Unknown;
    if(use==TypeUse::Cast && mysql(dialect)) {
      if(name=="CHAR" || name=="NCHAR" || name=="CHARACTER") {type.family=TypeFamily::VarCharacter;type.flavor="mysql-cast-character";}
      if(name=="BINARY") {type.family=TypeFamily::Bytes;type.flavor="mysql-cast-binary";}
    }
    if(!binary)type.nationalCharacter=name=="NCHAR" || name=="NVARCHAR" || name=="NVARCHAR2" || name=="NTEXT" || name=="NCLOB";
    if(name=="TEXT" || name=="NTEXT" || name=="CLOB" || name=="NCLOB" || name=="BLOB" || name=="BYTEA")type.flavor=name;
    count(1);
    if((name=="TEXT" || name=="NTEXT" || name=="CLOB" || name=="NCLOB" || name=="BYTEA" || name=="BLOB") && !syntax.arguments.empty()) {
      if(!mysql(dialect))error("this large-object type does not accept a length");
      else {opaque();return result;}
    }
    if(!syntax.arguments.empty()) {
      auto length=arg(0);
      if(length && *length>=0)type.length=std::uint64_t(*length);
      else if(upper(syntax.arguments[0].value)=="MAX" && dialect==Dialect::SQLServer &&
              (name=="VARCHAR" || name=="NVARCHAR" || name=="VARBINARY"))type.flavor="max";
      else error("character / binary length must be a nonnegative integer or a supported MAX length");
      if(type.length && !mysql(dialect) && *type.length==0)error("character / binary length must be positive in this dialect");
    } else if((fixed && !(use==TypeUse::Cast && mysql(dialect))) || (dialect==Dialect::SQLServer && (name=="VARCHAR" || name=="NVARCHAR" || name=="VARBINARY"))) type.length=use==TypeUse::Cast && dialect==Dialect::SQLServer?30:1;
    if(type.length) {
      const auto length=*type.length;
      if(dialect==Dialect::SQLServer && length>((name=="NCHAR" || name=="NVARCHAR")?4000:8000))error("character / binary length exceeds the SQL Server bounded type limit; use a supported MAX type");
      if(dialect==Dialect::PostgreSQL && !binary && length>10485760)error("PostgreSQL character length cannot exceed 10485760");
      if(mysql(dialect) && use!=TypeUse::Cast && length>(fixed?255:65535))error("character / binary length exceeds the MySQL-family declaration limit");
      if(dialect==Dialect::Oracle && length>(fixed?2000:32767))error("character length exceeds the maximum Oracle declaration limit");
    }
    if(syntax.lengthSemantics)type.flavor=*syntax.lengthSemantics;
  } else {opaque();}
  if(dialect==Dialect::SQLite){type.sqliteAffinity=true;type.precision.reset();type.scale.reset();type.bits.reset();type.length.reset();type.unsignedInteger.reset();opaque();}
  if(use==TypeUse::Catalog && syntax.arguments.empty()) {
    if(type.family==TypeFamily::Decimal && (mysql(dialect) || dialect==Dialect::SQLServer)) {type.precision.reset();type.scale.reset();}
    if(type.family==TypeFamily::Character || type.family==TypeFamily::VarCharacter || type.family==TypeFamily::VarBinary || (type.family==TypeFamily::Binary && type.flavor!="rowversion"))type.length.reset();
    if(dialect==Dialect::SQLServer && name=="FLOAT")type.bits.reset();
  }
  return result;
}
std::optional<SqlType> catalogTypeFromText(Dialect dialect,std::string text) {
  if(text.empty() || text.size()>4096)return {};
  LanguageOptions options;options.limits.sourceBytes=4096;options.limits.tokens=256;
  options.limits.nodes=256;options.limits.nesting=16;options.limits.types=32;
  Compilation parsed(dialect,std::move(text),options,"<catalog type>");
  try {
    lex(parsed);
    if(!parsed.diagnostics.empty())return {};
    auto parser=createParser(parsed);
    const auto syntax=parser->parseTypeAt(0);
    for(const auto &token:parsed.tokens)
      if(token.range.begin>=syntax.origin.end && token.kind!=TokenKind::End && token.kind!=TokenKind::Comment)return {};
    const auto resolved=resolveDeclaredType(lowerTypeSpec(parsed,syntax),dialect,TypeUse::Catalog);
    if(!resolved.error.empty())return {};
    return resolved.type;
  }catch(const ParseFailure &){return {};}catch(const CompilationAborted &){return {};}

}
} // namespace navidog::sql::compiler
