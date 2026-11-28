#include "parser.hpp"
#include "navidog/sql/compiler/operator_catalog.hpp"
#include <algorithm>
#include <charconv>
#include <cmath>
#include <limits>
#include <map>
#include <set>
#include <tuple>

namespace navidog::sql::compiler {
ColumnDefinition declaredColumn(const BasicColumnSpec<std::string,std::vector<std::string>> &syntax,Dialect dialect) {
  ColumnDefinition column;column.name=syntax.name;
  column.type=syntax.type.arrayDimensions?ValueType::Unknown:typeCategory(syntax.type.name,dialect);
  auto type=resolveDeclaredType(syntax.type,dialect).type;
  type.nullability=syntax.nullable?(*syntax.nullable?Nullability::Nullable:Nullability::NotNull):
      dialect==Dialect::SQLServer?Nullability::Unknown:Nullability::Nullable;
  column.nonNullKnown=syntax.nullable && !*syntax.nullable;
  for(const auto &key:syntax.constraints)if(key.kind==ConstraintKind::PrimaryKey) {
    const bool sqliteInteger=dialect==Dialect::SQLite && syntax.type.name.size()==1 && asciiUpper(syntax.type.name[0])=="INTEGER" && syntax.type.arguments.empty() &&
      (syntax.type.nameQuoted.empty() || !syntax.type.nameQuoted[0]);
    if(key.deferrable!=true && (dialect!=Dialect::SQLite || sqliteInteger))type.nullability=Nullability::NotNull;
    else if(dialect!=Dialect::SQLite && !column.nonNullKnown)type.nullability=Nullability::Unknown;
  }
  type.collation=syntax.collation;type.characterSet=syntax.characterSet;
  column.sqlType=std::make_shared<const SqlType>(std::move(type));return column;
}
namespace {
enum class PgTypeCategory { Numeric, Text, Boolean, Bytes, DateTime, Interval };
std::optional<PgTypeCategory> knownPgCategory(const SqlType &type) {
  if (type.arrayDimensions || !type.opaqueName.empty() ||
      type.flavor == "untyped-string-literal")
    return {};
  switch (type.family) {
  case TypeFamily::Integer: case TypeFamily::Numeric: case TypeFamily::Decimal: case TypeFamily::Floating:
    return PgTypeCategory::Numeric;
  case TypeFamily::String: case TypeFamily::Character: case TypeFamily::VarCharacter:
    return PgTypeCategory::Text;
  case TypeFamily::Boolean: return PgTypeCategory::Boolean;
  case TypeFamily::Bytes: case TypeFamily::Binary: case TypeFamily::VarBinary:
    return PgTypeCategory::Bytes;
  case TypeFamily::Date: case TypeFamily::Time: case TypeFamily::Timestamp:
    return PgTypeCategory::DateTime;
  case TypeFamily::Interval: return PgTypeCategory::Interval;
  case TypeFamily::Null: case TypeFamily::Unknown: return {};
  }
  return {};
}
Nullability combined(const std::vector<SqlType> &types) {
  bool unknown=false;
  for(const auto &type:types) {
    if(type.nullability==Nullability::Nullable)return Nullability::Nullable;
    unknown |= type.nullability==Nullability::Unknown;
  }
  return unknown?Nullability::Unknown:Nullability::NotNull;
}
// Conservative common facts, not a substitute for each dialect's full overload
// and coercion rules. Incompatible/unknown attributes are explicitly forgotten.
SqlType common(const std::vector<SqlType> &types) {
  SqlType result;bool initialized=false;
  for(auto type:types) {
    if(type.family==TypeFamily::Null)continue;
    type.nullability=Nullability::Unknown;
    if(!initialized){result=std::move(type);initialized=true;continue;}
    if(result==type)continue;
    if(result.family!=type.family || result.arrayDimensions!=type.arrayDimensions ||
       result.sqliteAffinity!=type.sqliteAffinity || result.opaqueName!=type.opaqueName ||
       result.opaqueArguments!=type.opaqueArguments || result.opaqueModifiers!=type.opaqueModifiers ||
       result.flavor!=type.flavor)return {};
    if(result.intervalLeading!=type.intervalLeading || result.intervalTrailing!=type.intervalTrailing || result.intervalUnit!=type.intervalUnit || result.intervalLeadingPrecision!=type.intervalLeadingPrecision) {
      result.intervalLeading=IntervalField::None;result.intervalTrailing=IntervalField::None;result.intervalUnit=IntervalUnit::None;
      result.intervalLeadingPrecision.reset();if(result.family==TypeFamily::Interval)result.precision.reset();
    }
    if(result.precision!=type.precision)result.precision.reset();
    if(result.scale!=type.scale)result.scale.reset();
    if(result.bits!=type.bits)result.bits.reset();
    if(result.length!=type.length)result.length.reset();
    if(result.lengthUnit!=type.lengthUnit){result.lengthUnit=LengthUnit::Unknown;result.length.reset();}
    if(result.unsignedInteger!=type.unsignedInteger)result.unsignedInteger.reset();
    if(result.nationalCharacter!=type.nationalCharacter)result.nationalCharacter.reset();
    if(result.timeZone!=type.timeZone)result.timeZone=TimeZoneKind::Unknown;
    if(result.characterSet!=type.characterSet)result.characterSet.clear();
    if(result.collation!=type.collation)result.collation.clear();
  }
  if(!initialized)result.family=types.empty()?TypeFamily::Unknown:TypeFamily::Null;
  result.nullability=types.empty()?Nullability::Unknown:combined(types);return result;
}
SqlType pgNumericLiteral(std::string_view spelling, bool negative = false) {
  SqlType type;
  type.nullability = Nullability::NotNull;
  std::uint64_t magnitude = 0;
  const auto *end = spelling.data() + spelling.size();
  const auto parsed = std::from_chars(spelling.data(), end, magnitude);
  const auto max64 = static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max());
  const auto max32 = static_cast<std::uint64_t>(std::numeric_limits<std::int32_t>::max());
  if (parsed.ec != std::errc{} || parsed.ptr != end || magnitude > max64 + negative) {
    type.family = TypeFamily::Decimal;
  } else {
    type.family = TypeFamily::Integer;
    type.bits = magnitude <= max32 + negative ? 32 : 64;
    type.unsignedInteger = false;
  }
  return type;
}
SqlType tsqlNumericLiteral(std::string_view spelling) {
  SqlType type;
  type.nullability = Nullability::NotNull;
  if (spelling.find_first_of("eE") != std::string_view::npos) {
    type.family = TypeFamily::Floating;
    type.bits = 64;
    return type;
  }
  const auto dot = spelling.find('.');
  if (dot == std::string_view::npos) {
    std::int32_t value = 0;
    const auto *end = spelling.data() + spelling.size();
    const auto parsed = std::from_chars(spelling.data(), end, value);
    if (parsed.ec == std::errc{} && parsed.ptr == end) {
      type.family = TypeFamily::Integer;
      type.bits = 32;
      type.unsignedInteger = false;
      return type;
    }
  }
  type.family = TypeFamily::Decimal;
  type.unsignedInteger = false;
  const auto whole = spelling.substr(0, dot);
  const auto first = whole.find_first_not_of('0');
  const auto scale = dot == std::string_view::npos ? 0 : spelling.size() - dot - 1;
  const auto precision = scale + (first == std::string_view::npos ? 0 : whole.size() - first);
  if (precision <= 38) {
    type.precision = std::max(1, static_cast<int>(precision));
    type.scale = static_cast<int>(scale);
  }
  return type;
}
int tsqlNumericRank(const SqlType &type) {
  if (type.arrayDimensions || type.sqliteAffinity || !type.opaqueName.empty() || !type.flavor.empty()) return 0;
  if (type.family == TypeFamily::Integer) {
    if (type.bits == 8 && type.unsignedInteger == true) return 1;
    if (type.unsignedInteger == false) {
      if (type.bits == 16) return 2;
      if (type.bits == 32) return 3;
      if (type.bits == 64) return 4;
    }
  }
  if (type.family == TypeFamily::Decimal && type.unsignedInteger != true) return 5;
  if (type.family == TypeFamily::Floating) {
    if (type.bits == 32) return 6;
    if (type.bits == 64) return 7;
  }
  return 0;
}
std::optional<std::pair<int,int>> tsqlDecimalShape(const SqlType &type, const ModelExpression &source) {
  if (type.family == TypeFamily::Decimal && type.precision && type.scale && *type.precision >= 1 &&
      *type.precision <= 38 && *type.scale >= 0 && *type.scale <= *type.precision)
    return std::make_pair(*type.precision, *type.scale);
  // SQL Server can assign a different decimal precision to an integer
  // constant depending on whether the server auto-parameterizes the query.
  if (type.family != TypeFamily::Integer ||
      (source.kind != ExprKind::Cast && !source.binding && !source.localSymbol)) return {};
  constexpr int precision[] = {0, 3, 5, 10, 19};
  const auto rank = tsqlNumericRank(type);
  if (rank < 1 || rank > 4) return {};
  return std::make_pair(precision[rank], 0);
}
std::optional<SqlType> tsqlNumericArithmetic(const ModelSelect &q, const ModelExpression &e,
                                            const SqlType &a, const SqlType &b) {
  const int left = tsqlNumericRank(a), right = tsqlNumericRank(b);
  if (!left || !right) return {};
  const int rank = std::max(left, right);
  if (rank != 5) {
    if (e.value == "%" && rank > 5) return {};
    return left >= right ? a : b;
  }
  SqlType result;
  result.family = TypeFamily::Decimal;
  result.unsignedInteger = false;
  const auto x = tsqlDecimalShape(a, q.expressions.at(e.operands.at(0)));
  const auto y = tsqlDecimalShape(b, q.expressions.at(e.operands.at(1)));
  if (!x || !y) return result;
  const auto [p1, s1] = *x;
  const auto [p2, s2] = *y;
  int scale = std::max(s1, s2);
  int precision;
  const bool product = e.value == "*", quotient = e.value == "/";
  if (product) {
    scale = s1 + s2;
    precision = p1 + p2 + 1;
  } else if (quotient) {
    scale = std::max(6, s1 + p2 + 1);
    precision = p1 - s1 + s2 + scale;
  } else if (e.value == "%") {
    precision = std::min(p1 - s1, p2 - s2) + scale;
  } else {
    precision = std::max(p1 - s1, p2 - s2) + scale + 1;
  }
  if (precision > 38) {
    result.precision = 38;
    if (!product && !quotient) return result;
    const int wholeDigits = precision - scale;
    scale = wholeDigits < 32 ? std::min(scale, 38 - wholeDigits) : std::min(scale, 6);
  } else {
    result.precision = precision;
  }
  result.scale = scale;
  return result;
}
constexpr int mysqlMaxPrecision = 65;
constexpr int mysqlMaxScale = 30;

SqlType mysqlNumericLiteral(std::string_view spelling, bool negative = false) {
  SqlType type;
  type.nullability = Nullability::NotNull;
  if (spelling.find_first_of("eE") != std::string_view::npos) {
    type.family = TypeFamily::Floating;
    type.bits = 64;
    return type;
  }
  const auto dot = spelling.find('.');
  if (dot == std::string_view::npos) {
    std::uint64_t magnitude = 0;
    const auto *end = spelling.data() + spelling.size();
    const auto parsed = std::from_chars(spelling.data(), end, magnitude);
    const auto max64 = static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max());
    if (parsed.ec == std::errc{} && parsed.ptr == end && (!negative || magnitude <= max64 + 1)) {
      type.family = TypeFamily::Integer;
      // Native SELECT result metadata uses MYSQL_TYPE_LONGLONG. A
      // CREATE TABLE AS SELECT may choose a narrower storage column.
      type.bits = 64;
      type.unsignedInteger = !negative && magnitude > max64;
      return type;
    }
  }
  type.family = TypeFamily::Decimal;
  type.unsignedInteger = false;
  const auto whole = spelling.substr(0, dot);
  const auto first = whole.find_first_not_of('0');
  const auto scale = dot == std::string_view::npos ? 0 : spelling.size() - dot - 1;
  const auto wholeDigits = first == std::string_view::npos
      ? std::size_t(!whole.empty()) : whole.size() - first;
  const auto precision = wholeDigits + scale;
  if (precision <= mysqlMaxPrecision && scale <= mysqlMaxScale) {
    type.precision = std::max(1, static_cast<int>(precision));
    type.scale = static_cast<int>(scale);
  }
  return type;
}

bool mysqlArithmeticNumeric(const SqlType &type) {
  return !type.arrayDimensions && !type.sqliteAffinity && type.opaqueName.empty() &&
      type.flavor.empty() && (type.family == TypeFamily::Integer ||
      type.family == TypeFamily::Decimal || type.family == TypeFamily::Floating);
}

std::optional<int> mysqlDivisionIncrement(const LanguageOptions &options) {
  const auto value = options.mysqlDivisionPrecisionIncrement;
  if (!value || *value < 0 || *value > mysqlMaxScale) return {};
  return value;
}

std::optional<std::pair<int,int>> mysqlExactShape(const ModelSelect &query,
                                                 ExprId id, const SqlType &type) {
  if (!mysqlArithmeticNumeric(type)) return {};
  if (type.family == TypeFamily::Decimal && type.precision && type.scale &&
      *type.precision >= 1 && *type.precision <= mysqlMaxPrecision &&
      *type.scale >= 0 && *type.scale <= mysqlMaxScale && *type.scale <= *type.precision)
    return std::make_pair(*type.precision, *type.scale);
  if (type.family != TypeFamily::Integer || !type.unsignedInteger) return {};
  const auto &expression = query.expressions.at(id);
  const ModelExpression *literal = &expression;
  if (expression.kind == ExprKind::Unary && expression.value == "-" &&
      expression.operands.size() == 1)
    literal = &query.expressions.at(expression.operands.front());
  if (literal->kind == ExprKind::Number && literal->value.size() <= mysqlMaxPrecision &&
      literal->value.find_first_not_of("0123456789") == std::string::npos)
    return std::make_pair(static_cast<int>(literal->value.size()), 0);
  // Width alone cannot recover the server's expression display length.
  // Infer from declared column types when the input is directly bound.
  if (expression.kind != ExprKind::Name || !expression.binding ||
      expression.binding->scope != query.id || expression.binding->table >= query.tables.size() ||
      query.tables.at(expression.binding->table).query) return {};
  switch (type.bits.value_or(0)) {
  case 8: return std::make_pair(3, 0);
  case 16: return std::make_pair(5, 0);
  case 24: return std::make_pair(8, 0);
  case 32: return std::make_pair(10, 0);
  case 64: return std::make_pair(*type.unsignedInteger ? 20 : 19, 0);
  default: return {};
  }
}

std::optional<SqlType> mysqlNumericArithmetic(const ModelSelect &query,
    const ModelExpression &expression, const SqlType &left, const SqlType &right,
    const LanguageOptions &options) {
  if (!mysqlArithmeticNumeric(left) || !mysqlArithmeticNumeric(right)) return {};
  SqlType result;
  const auto &op = expression.value;
  if (op == "DIV") {
    result.family = TypeFamily::Integer;
    result.bits = 64;
    if (left.unsignedInteger==false && right.unsignedInteger==false)
      result.unsignedInteger = false;
    else if (left.family==TypeFamily::Integer && right.family==TypeFamily::Integer &&
             left.unsignedInteger && right.unsignedInteger)
      result.unsignedInteger = *left.unsignedInteger || *right.unsignedInteger;
    return result;
  }
  if (left.family == TypeFamily::Floating || right.family == TypeFamily::Floating) {
    result.family = TypeFamily::Floating;
    result.bits = 64;
    return result;
  }
  const bool integer = left.family == TypeFamily::Integer &&
      right.family == TypeFamily::Integer && op != "/";
  result.family = integer ? TypeFamily::Integer : TypeFamily::Decimal;
  if (left.unsignedInteger && right.unsignedInteger) {
    const bool isUnsigned = op == "%" ? *left.unsignedInteger :
        integer ? (*left.unsignedInteger || *right.unsignedInteger) :
        (*left.unsignedInteger && *right.unsignedInteger);
    if (op != "-" || !isUnsigned) result.unsignedInteger = isUnsigned;
    else if (options.mysqlNoUnsignedSubtraction)
      result.unsignedInteger = !*options.mysqlNoUnsignedSubtraction;
  }
  if (integer) {
    result.bits = 64;
    return result;
  }
  // MOD scale and precision depend on the native display length.
  if (op == "%") return result;
  const auto a = mysqlExactShape(query, expression.operands.at(0), left);
  const auto b = mysqlExactShape(query, expression.operands.at(1), right);
  if (!a || !b) return result;
  const auto [p1, s1] = *a;
  const auto [p2, s2] = *b;
  if (op == "/") {
    if (const auto increment = mysqlDivisionIncrement(options)) {
      result.precision = std::min(mysqlMaxPrecision, p1 + s2 + *increment);
      result.scale = std::min(mysqlMaxScale, s1 + *increment);
    }
  } else if (op == "*") {
    result.precision = std::min(mysqlMaxPrecision, p1 + p2);
    result.scale = std::min(mysqlMaxScale, s1 + s2);
  } else {
    result.scale = std::max(s1, s2);
    result.precision = std::min(mysqlMaxPrecision, std::max(p1 - s1, p2 - s2) + 1 + *result.scale);
  }
  return result;
}

unsigned pgNumericRank(const SqlType &type) {
  if (type.arrayDimensions || type.sqliteAffinity || !type.opaqueName.empty() || !type.flavor.empty())
    return 0;
  if (type.family == TypeFamily::Integer && type.unsignedInteger == false) {
    if (type.bits == 16) return 1;
    if (type.bits == 32) return 2;
    if (type.bits == 64) return 3;
  }
  if (type.family == TypeFamily::Decimal) return 4;
  // Both implicit numeric-to-floating conversions exist. Once a floating
  // input chooses that overload family, widening can reach double.
  if (type.family == TypeFamily::Floating && type.bits == 32) return 5;
  if (type.family == TypeFamily::Floating && type.bits == 64) return 6;
  return 0;
}
SqlType pgNumericCommon(const std::vector<SqlType> &types) {
  unsigned least = 7, greatest = 0;
  const SqlType *choice = nullptr;
  for (const auto &type : types) {
    if (type.family == TypeFamily::Null) continue;
    const auto rank = pgNumericRank(type);
    if (!rank) return common(types);
    least = std::min(least, rank);
    if (rank > greatest) {
      greatest = rank;
      choice = &type;
    }
  }
  if (!choice || least == greatest) return common(types);
  auto result = *choice;
  result.nullability = combined(types);
  result.precision.reset();
  result.scale.reset();
  return result;
}
bool pgUntyped(const SqlType &type) {
  return type.family == TypeFamily::Null || type.flavor == "untyped-string-literal" ||
      type.flavor == "postgres-bind-parameter";
}
SqlType pgCommon(const std::vector<SqlType> &types) {
  if (types.empty()) return {};
  std::vector<SqlType> concrete;
  concrete.reserve(types.size());
  for (const auto &type : types)
    if (!pgUntyped(type)) concrete.push_back(type);
  if (concrete.empty()) {
    SqlType text;
    text.family = TypeFamily::VarCharacter;
    text.flavor = "TEXT";
    text.nationalCharacter = false;
    text.lengthUnit = LengthUnit::Characters;
    text.nullability = combined(types);
    return text;
  }
  auto chosen = pgNumericCommon(concrete);
  chosen.nullability = combined(types);
  if (concrete.size() != types.size()) {
    // Coercing an untyped value does not prove that it has a declared
    // typmod, even when the surrounding expression chooses that base type.
    chosen.precision.reset();
    chosen.scale.reset();
    chosen.length.reset();
  }
  return chosen;
}
class Inference {
  Compilation &c;
  std::vector<std::vector<unsigned char>> states;
  std::vector<std::vector<bool>> nullableTables;
  std::size_t visits=0;
  std::map<std::uint32_t,std::shared_ptr<const SqlType>> localTypes;
  std::map<std::pair<QueryId, std::size_t>, SqlType> valuesTypes;
  std::map<std::tuple<QueryId,std::size_t,std::size_t>,SqlType> unpivotTypes;
  std::map<std::pair<QueryId,ExprId>, SqlType> arrayCastTypes;
  std::map<std::pair<QueryId,ExprId>,std::vector<std::size_t>> arrayShapes;
  [[noreturn]] void limit(const char *text) {c.report(DiagnosticCode::ResourceLimit,{},text);throw CompilationAborted{};}
  void tick(std::size_t depth) {
    if((++visits & 255u)==0)checkStop(c);
    if(depth>std::min<std::size_t>(256,c.options.limits.nesting) || visits>std::min<std::size_t>(1000000,std::max<std::size_t>(1,c.options.limits.nodes))*16)
      limit("SQL type inference visit/depth limit exceeded");
  }
  TypeId intern(SqlType type) {
    try{return c.model.types.intern(std::move(type),c.options.limits.types);}
    catch(const std::length_error &e){limit(e.what());}
    catch(const std::invalid_argument &){return 0;} // Invalid optional external facts remain unknown.
  }
  void validate(const BasicTypeSpec<std::vector<std::string>> &syntax,TypeUse use) {
    const auto type=resolveDeclaredType(syntax,c.dialect,use);
    if(!type.error.empty())c.report(DiagnosticCode::InvalidQueryShape,syntax.origin,type.error);
    intern(type.error.empty()?type.type:SqlType{});
  }
  SqlType commonResult(const std::vector<SqlType> &types,
                       std::optional<SourceRange> conflict = {}) {
    if (c.dialect != Dialect::PostgreSQL) return common(types);
    if (conflict) {
      std::optional<PgTypeCategory> category;
      for (const auto &type : types) {
        const auto known = knownPgCategory(type);
        if (!known) continue;
        if (category && *category != *known) {
          c.report(DiagnosticCode::InvalidQueryShape, *conflict,
                   "expression combines incompatible PostgreSQL type categories");
          return {};
        }
        category = known;
      }
    }
    return pgCommon(types);
  }
  SqlType arrayType(QueryId query,ExprId id,const ModelExpression &e,std::vector<SqlType> elements) {
    const auto key=std::make_pair(query,id);
    const auto expected=arrayCastTypes.find(key);
    if (elements.empty() && expected==arrayCastTypes.end()) {
      c.report(DiagnosticCode::InvalidQueryShape,e.origin,"an empty PostgreSQL ARRAY requires an explicit array cast");
      return {};
    }
    bool nested=false,scalar=false;
    for (auto &element:elements) {
      nested|=element.arrayDimensions>0;
      scalar|=!element.arrayDimensions && element.family!=TypeFamily::Unknown && element.family!=TypeFamily::Null;
      element.arrayDimensions=0;
    }
    if (nested && scalar) {
      c.report(DiagnosticCode::InvalidQueryShape,e.origin,"PostgreSQL ARRAY cannot mix scalar and array elements");
      return {};
    }
    auto type=expected==arrayCastTypes.end()?commonResult(elements,e.origin):expected->second;
    if (type.family==TypeFamily::Unknown && type.opaqueName.empty()) return type;
    // PostgreSQL stores the element type in the SQL type identity. The
    // actual dimensions belong to each array value, even after a cast.
    type.arrayDimensions=1;
    type.nullability=Nullability::NotNull;

    std::vector<std::size_t> shape{elements.size()};
    if (nested) {
      const auto &expressions=c.model.queries[query-1].expressions;
      const std::vector<std::size_t> empty{0};
      const std::vector<std::size_t> *extent=nullptr;
      bool allKnown=true;
      for (auto child:e.operands) {
        tick(0);
        const auto known=arrayShapes.find({query,child});
        const auto &value=expressions.at(child);
        const auto &valueType=c.model.types.get(value.sqlType);
        const bool literalNull=value.kind==ExprKind::Null ||
            (value.kind==ExprKind::Cast && valueType.arrayDimensions && valueType.opaqueName.empty() &&
             value.operands.size()==1 &&
             expressions.at(value.operands.front()).kind==ExprKind::Null);
        const auto *current=known!=arrayShapes.end()?&known->second:literalNull?&empty:nullptr;
        if (!current) { allKnown=false; continue; }
        if (extent && *extent!=*current) {
          c.report(DiagnosticCode::InvalidQueryShape,e.origin,"PostgreSQL multidimensional arrays require matching dimensions");
          return type;
        }
        extent=current;
      }
      if (!allKnown || !extent) return type;
      if (*extent==empty) shape=empty;
      else shape.insert(shape.end(),extent->begin(),extent->end());
    } else if (std::any_of(elements.begin(),elements.end(),[](const auto &element) {
      return element.family==TypeFamily::Unknown && !pgUntyped(element);
    })) return type;
    if (shape.size()>postgresArrayMaxDimensions) {
      c.report(DiagnosticCode::InvalidQueryShape,e.origin,"PostgreSQL array values support at most six dimensions");
      return type;
    }
    arrayShapes.insert_or_assign(key,std::move(shape));
    return type;
  }
  void declarations(ModelStatement &q) {
    if(!q.ddl)return;
    auto column=[&](const auto &col){validate(col.type,TypeUse::Declaration);};
    for(const auto &col:q.ddl->definitions)column(col);
    for(const auto &action:q.ddl->actions) {
      if(action.column)column(*action.column);
      if(action.type)validate(*action.type,TypeUse::Declaration);
    }
    if(q.kind!=StatementKind::Create || q.ddl->object!=ObjectKind::Table || q.tables.empty() || !q.tables.front().pseudo || !q.tables.front().definition)return;
    auto definition=*q.tables.front().definition;
    for(std::size_t i=0;i<definition.columns.size() && i<q.ddl->definitions.size();++i) {
      const auto &syntax=q.ddl->definitions[i];
      auto column=declaredColumn(syntax,c.dialect);auto type=*column.sqlType;
      for(const auto &key:q.ddl->constraints)if(key.kind==ConstraintKind::PrimaryKey &&
          std::find(key.columns.begin(),key.columns.end(),syntax.name)!=key.columns.end()) {
        const bool rowid=c.dialect==Dialect::SQLite && key.columns.size()==1 && syntax.type.name.size()==1 &&
          asciiUpper(syntax.type.name[0])=="INTEGER" && syntax.type.arguments.empty() &&
          (syntax.type.nameQuoted.empty() || !syntax.type.nameQuoted[0]);
        if(key.deferrable!=true && (c.dialect!=Dialect::SQLite || rowid))type.nullability=Nullability::NotNull;
        else if(c.dialect!=Dialect::SQLite && !column.nonNullKnown)type.nullability=Nullability::Unknown;
      }
      column.sqlType=std::make_shared<const SqlType>(std::move(type));
      definition.columns[i]=std::move(column);
    }
    q.tables.front().definition=std::make_shared<const TableDefinition>(std::move(definition));
  }
  void outerNullable(std::size_t query) {
    const auto &q=c.model.queries[query];auto &nullable=nullableTables[query];nullable.resize(q.tables.size());
    auto mark=[&](RelationId root) {
      std::vector<RelationId> work{root};std::set<RelationId> seen;
      while(!work.empty()) {
        tick(0);const auto id=work.back();work.pop_back();
        if(!id || id>q.relations.size() || !seen.insert(id).second)continue;
        const auto &r=q.relations[id-1];
        if(!r.left && !r.right){if(r.table<nullable.size())nullable[r.table]=true;}
        else {work.push_back(r.left);work.push_back(r.right);}
      }
    };
    for(const auto &r:q.relations) {
      if(r.join==JoinKind::Left || r.join==JoinKind::Full)mark(r.right);
      if(r.join==JoinKind::Right || r.join==JoinKind::Full)mark(r.left);
    }
  }
  SqlType projected(QueryId query,std::size_t column,std::size_t depth) {
    if(!query || query>c.model.queries.size())return {};
    const auto &q=c.model.queries[query-1];
    const auto &fields=q.kind==StatementKind::Select?q.projections:q.returning;
    if(column>=fields.size())return {};
    // An expanded star can shift later expression ordinals. Its bound
    // definition supplies the column's type when this is a returning shape.
    if (q.expressions.at(fields[column].expression).kind==ExprKind::Star ||
        (q.kind!=StatementKind::Select && std::any_of(fields.begin(),fields.end(),[&](const auto &field) {
          return q.expressions.at(field.expression).kind==ExprKind::Star;
        }))) return {};
    return expression(query,fields[column].expression,depth+1);
  }
  enum class OracleUnpivotGroup { Numeric, Character, DateTime, Interval, Binary };
  static std::optional<OracleUnpivotGroup> unpivotGroup(const SqlType &type) {
    if (type.arrayDimensions || !type.opaqueName.empty()) return {};
    switch (type.family) {
    case TypeFamily::Integer:case TypeFamily::Numeric:case TypeFamily::Decimal:case TypeFamily::Floating:
      return OracleUnpivotGroup::Numeric;
    case TypeFamily::Character:case TypeFamily::VarCharacter:case TypeFamily::String:
      return OracleUnpivotGroup::Character;
    case TypeFamily::Date:case TypeFamily::Time:case TypeFamily::Timestamp:
      return OracleUnpivotGroup::DateTime;
    case TypeFamily::Interval:return OracleUnpivotGroup::Interval;
    case TypeFamily::Binary:case TypeFamily::VarBinary:case TypeFamily::Bytes:
      return OracleUnpivotGroup::Binary;
    default:return {};
    }
  }
  SqlType unpivotCharacter() {
    auto type=catalogTypeFromText(c.dialect,"VARCHAR2(128)").value_or(SqlType{});
    type.length.reset();
    return type;
  }
  SqlType oracleUnpivotType(const std::vector<SqlType> &types,
                            const std::vector<SourceRange> &origins,bool label) {
    std::optional<OracleUnpivotGroup> group;
    bool unknown=false;
    for (std::size_t i=0;i<types.size();++i) {
      const auto &type=types[i];
      if (type.family==TypeFamily::Null) continue;
      const auto found=unpivotGroup(type);
      if (!found) unknown=true;
      else if (group && group!=found) {
        c.report(DiagnosticCode::InvalidQueryShape,origins[i],label?
            "corresponding UNPIVOT labels require compatible data types":
            "corresponding Oracle UNPIVOT measures require the same data type group");
        return {};
      } else group=found;
    }
    auto combinedType=common(types);
    if (unknown) return combinedType;
    if (group==OracleUnpivotGroup::Numeric) {
      const auto rank=[](const SqlType &type) {
        return type.family==TypeFamily::Floating?type.bits.value_or(32):0;
      };
      const auto widest=std::max_element(types.begin(),types.end(),[&](const auto &a,const auto &b) { return rank(a)<rank(b); });
      if (widest!=types.end() && rank(*widest)>0) combinedType=*widest;
      else if (label || combinedType.family!=TypeFamily::Decimal)
        combinedType=catalogTypeFromText(c.dialect,"NUMBER").value_or(SqlType{});
    } else if (group==OracleUnpivotGroup::Character && (label || combinedType.family==TypeFamily::Unknown)) {
      combinedType=unpivotCharacter();
      if (!label && std::all_of(types.begin(),types.end(),[](const auto &type) {
            return type.family==TypeFamily::Character || type.family==TypeFamily::Null;
          })) combinedType.family=TypeFamily::Character;
    } else if (!group && label && combinedType.family==TypeFamily::Null) {
      combinedType=unpivotCharacter();
    }
    combinedType.nullability=combined(types);
    return combinedType;
  }
  SqlType unpivotDescriptor(const ModelStatement &q,std::size_t tableIndex,std::size_t index,std::size_t depth) {
    const auto &table=q.tables.at(tableIndex);
    const auto &shape=*table.unpivot;
    if (c.dialect!=Dialect::Oracle) return {};
    auto [cached,fresh]=unpivotTypes.try_emplace({q.id,tableIndex,index});
    if (!fresh) return cached->second;
    std::vector<SqlType> types;
    std::vector<SourceRange> origins;
    for (const auto &input:shape.inputs) {
      tick(depth);
      if (input.labels.empty()) {
        auto type=categoryType(ValueType::String);
        type.nullability=Nullability::NotNull;
        types.push_back(std::move(type));
        origins.push_back(input.origin);
      } else {
        if (index>=input.labels.size()) return {};
        const auto id=input.labels[index];
        const auto &label=q.expressions.at(id);
        if (label.kind==ExprKind::Null || (label.kind==ExprKind::String && label.value.empty()))
          types.push_back(categoryType(ValueType::Null));
        else
          types.push_back(expression(q.id,id,depth+1));
        origins.push_back(label.origin);
      }
    }
    return cached->second=oracleUnpivotType(types,origins,true);
  }
  SqlType unpivotMeasure(const ModelStatement &q,std::size_t tableIndex,std::size_t index,std::size_t depth) {
    const auto &table=q.tables.at(tableIndex);
    const auto &shape=*table.unpivot;
    if (!table.unpivotInput || shape.inputOrdinals.size()!=shape.inputs.size() || shape.inputOrdinals.empty()) return {};
    auto [cached,fresh]=unpivotTypes.try_emplace({q.id,tableIndex,shape.descriptors.size()+index});
    if (!fresh) return cached->second;
    std::vector<SqlType> types;
    std::vector<SourceRange> origins;
    std::optional<SqlType> exact;
    bool reported=false;
    for (std::size_t i=0;i<shape.inputOrdinals.size();++i) {
      tick(depth);
      const auto ordinal=shape.inputOrdinals[i].at(index);
      const auto &column=table.unpivotInput->columns.at(ordinal);
      auto input=table.query?projected(table.query,ordinal,depth+1):SqlType{};
      const bool exactType=input.family!=TypeFamily::Unknown || column.sqlType;
      if (input.family==TypeFamily::Unknown) input=column.sqlType?*column.sqlType:categoryType(column.type);
      if (column.nonNullKnown) input.nullability=Nullability::NotNull;
      if (c.dialect==Dialect::SQLServer && exactType &&
          input.family!=TypeFamily::Unknown && input.family!=TypeFamily::Null) {
        auto signature=input;
        signature.nullability=Nullability::Unknown;
        if (!exact) exact=std::move(signature);
        else if (!reported && *exact!=signature) {
          c.report(DiagnosticCode::InvalidQueryShape,shape.inputs[i].origin,
                   "SQL Server UNPIVOT requires identical input column types");
          reported=true;
        }
      }
      types.push_back(std::move(input));
      origins.push_back(shape.inputs[i].origin);
    }
    if (c.dialect==Dialect::Oracle) return cached->second=oracleUnpivotType(types,origins,false);
    return cached->second=commonResult(types,shape.origin);
  }
  SqlType pivotInputType(const ModelTable &table,std::size_t ordinal,std::size_t depth) {
    if (table.query) {
      auto type=projected(table.query,ordinal,depth+1);
      if (type.family!=TypeFamily::Unknown) return type;
    }
    if (!table.pivotInput || ordinal>=table.pivotInput->columns.size()) return {};
    const auto &column=table.pivotInput->columns[ordinal];
    return column.sqlType?*column.sqlType:categoryType(column.type);
  }
  void finalizePostgresPivot(const ModelStatement &query,ModelTable &table) {
    const auto &shape=*table.pivot;
    if (!table.definition || !table.definition->complete || shape.axisOrdinals.empty()) return;
    std::vector<SqlType> axes;
    for (auto ordinal:shape.axisOrdinals) {
      tick(0);
      axes.push_back(pivotInputType(table,ordinal,0));
    }
    for (const auto &bucket:shape.buckets) {
      for (std::size_t i=0;i<bucket.values.size();++i) {
        tick(0);
        const auto axis=knownPgCategory(axes.at(i));
        const auto &constant=query.expressions.at(bucket.values[i]);
        const auto category=knownPgCategory(c.model.types.get(constant.sqlType));
        if (axis && category && axis!=category)
          c.report(DiagnosticCode::InvalidQueryShape,constant.origin,
                   "PIVOT bucket type is incompatible with its axis");
      }
    }
    const auto argument=shape.argumentOrdinal?pivotInputType(table,*shape.argumentOrdinal,0):SqlType{};
    auto result=resolveAggregateType(c.dialect,shape.aggregate,argument);
    if (!result.error.empty()) {
      c.report(DiagnosticCode::InvalidQueryShape,shape.argumentOrigin,result.error);
      return;
    }
    auto definition=*table.definition;
    const auto type=std::make_shared<const SqlType>(std::move(result.type));
    for (std::size_t i=shape.groupingOrdinals.size();i<definition.columns.size();++i) {
      tick(0);
      auto &column=definition.columns[i];
      column.type=valueCategory(*type);
      column.sqlType=type;
    }
    table.definition=std::make_shared<const TableDefinition>(std::move(definition));
    if (table.query && table.pivotInput) {
      auto input=*table.pivotInput;
      const auto set=[&](std::size_t ordinal,const SqlType &value) {
        auto &column=input.columns.at(ordinal);
        if (value.family==TypeFamily::Unknown) return;
        column.type=valueCategory(value);
        column.sqlType=std::make_shared<const SqlType>(value);
      };
      for (std::size_t i=0;i<axes.size();++i)
        set(shape.axisOrdinals[i],axes[i]);
      if (shape.argumentOrdinal) set(*shape.argumentOrdinal,argument);
      table.pivotInput=std::make_shared<const TableDefinition>(std::move(input));
    }
  }
  SqlType binding(const ColumnBinding &binding,std::size_t depth) {
    if(!binding.scope || binding.scope>c.model.queries.size())return {};
    const auto &q=c.model.queries[binding.scope-1];
    if(binding.table>=q.tables.size())return {};
    const auto &table=q.tables[binding.table];SqlType type;
    if(q.valuesQuery && binding.table==0) {
      const auto key=std::make_pair(binding.scope,binding.column);
      if(auto known=valuesTypes.find(key);known!=valuesTypes.end())type=known->second;
      else {
        std::vector<SqlType> cells;
        std::optional<PgTypeCategory> category;
        bool incompatible=false;
        for(const auto &row:q.rows) {
          tick(depth);
          if(binding.column>=row.size())continue;
          cells.push_back(expression(q.id,row[binding.column],depth+1));
          if(c.dialect!=Dialect::PostgreSQL || incompatible)continue;
          if(const auto observed=knownPgCategory(cells.back())) {
            if(category && category!=observed) {
              c.report(DiagnosticCode::InvalidQueryShape,q.expressions[row[binding.column]].origin,
                       "VALUES column combines incompatible PostgreSQL type categories");
              incompatible=true;
            }
            category=observed;
          }
        }
        type=commonResult(cells);
        valuesTypes.emplace(key,type);
      }
    } else if(q.setOperation && binding.table==0) {
      type=projected(q.setOperation->left,binding.column,depth);
      if(!q.setOperation->recursive)
        type=commonResult({std::move(type),projected(q.setOperation->right,binding.column,depth)}, q.setOperation->origin);
    }
    else if (table.unpivot) {
      const auto &unpivot=*table.unpivot;
      if (table.query && binding.column<unpivot.retainedOrdinals.size())
        type=projected(table.query,unpivot.retainedOrdinals[binding.column],depth);
      else if (binding.column>=unpivot.retainedOrdinals.size()) {
        const auto generated=binding.column-unpivot.retainedOrdinals.size();
        if (generated<unpivot.descriptors.size()) type=unpivotDescriptor(q,binding.table,generated,depth);
        else if (generated<unpivot.descriptors.size()+unpivot.measures.size())
          type=unpivotMeasure(q,binding.table,generated-unpivot.descriptors.size(),depth);
      }
    } else if (table.query && table.pivot) {
      const auto &pivot = *table.pivot;
      if (binding.column < pivot.groupingOrdinals.size())
        type = projected(table.query, pivot.groupingOrdinals[binding.column], depth);
      else if (pivot.argumentOrdinal)
        type = resolveAggregateType(c.dialect, pivot.aggregate,
                                    projected(table.query, *pivot.argumentOrdinal, depth)).type;
    } else if (table.ordinalityColumn && binding.column==1 && table.definition &&
               table.definition->columns.size()==2 && table.definition->columns.back().sqlType) {
      type=*table.definition->columns.back().sqlType;
    } else if(table.query)type=projected(table.query,binding.column,depth);
    if(type.family==TypeFamily::Unknown && table.definition && binding.column<table.definition->columns.size()) {
      const auto &column=table.definition->columns[binding.column];
      type=column.sqlType?*column.sqlType:categoryType(column.type);
      if(!q.catalogChecked)type.nullability=Nullability::Unknown;
      else if(column.nonNullKnown || (table.definition->primaryKeyKnown && std::find(table.definition->primaryKey.begin(),table.definition->primaryKey.end(),column.name)!=table.definition->primaryKey.end()))type.nullability=Nullability::NotNull;
    }
    const bool mergeOutputNullable=c.dialect==Dialect::SQLServer && q.kind==StatementKind::Merge &&
        (table.pseudo || (q.targetTable!=binding.table &&
         std::any_of(q.mergeClauses.begin(),q.mergeClauses.end(),[](const auto &branch) {
           return branch.match==MergeMatch::NotMatchedSource;
         })));
    if (table.unpivot && table.definition && binding.column>=table.unpivot->retainedOrdinals.size() &&
        binding.column<table.definition->columns.size())
      type.nullability=table.definition->columns[binding.column].nonNullKnown || type.nullability==Nullability::NotNull?
          Nullability::NotNull:Nullability::Nullable;
    if(nullableTables[binding.scope-1][binding.table] || table.workingTableOf || mergeOutputNullable ||
       (q.setOperation && q.setOperation->recursive))
      type.nullability=Nullability::Nullable;
    return type;
  }
  void recursiveTypes(const ModelStatement &q) {
    if (c.dialect != Dialect::PostgreSQL || !q.setOperation || !q.setOperation->recursive)
      return;
    const auto &seed = c.model.queries.at(q.setOperation->left - 1);
    const auto &member = c.model.queries.at(q.setOperation->right - 1);
    const auto hasStar = [](const ModelStatement &arm) {
      return std::any_of(arm.projections.begin(), arm.projections.end(), [&](const auto &projection) {
        return arm.expressions.at(projection.expression).kind == ExprKind::Star;
      });
    };
    if (seed.projections.size() != member.projections.size() || hasStar(seed) || hasStar(member))
      return;
    for (std::size_t i = 0; i < seed.projections.size(); ++i) {
      tick(0);
      const auto initial = projected(seed.id, i, 0);
      const auto recursive = projected(member.id, i, 0);
      const auto initialRank = pgNumericRank(initial);
      const auto recursiveRank = pgNumericRank(recursive);
      if (!initialRank || !recursiveRank)
        continue;
      const auto overall = commonResult({initial, recursive});
      if (initialRank != pgNumericRank(overall) || initial.precision != overall.precision ||
          initial.scale != overall.scale) {
        c.report(DiagnosticCode::InvalidQueryShape,
                 seed.expressions.at(seed.projections[i].expression).origin,
                 "recursive CTE column " + std::to_string(i + 1) +
                 " changes its PostgreSQL numeric type; cast the non-recursive term to the result type");
      }
    }
  }
  void windowTypes(const ModelStatement &q,const ModelExpression &e) {
    if(!e.window)return;
    const auto &spec=*e.window;const auto first=1+spec.partitions+spec.ordering.size();
    auto known=[&](ExprId id)->const SqlType&{return c.model.types.get(q.expressions.at(id).sqlType);};
    const auto temporal=[](TypeFamily f){return f==TypeFamily::Date || f==TypeFamily::Time || f==TypeFamily::Timestamp;};
    const auto numeric=[](TypeFamily f){return f==TypeFamily::Integer || f==TypeFamily::Decimal || f==TypeFamily::Floating || f==TypeFamily::Numeric;};
    for(std::size_t i=first;i<e.operands.size();++i) {
      tick(0);const auto &offset=known(e.operands[i]);const auto &value=q.expressions.at(e.operands[i]);
      auto report=[&](const char *message){c.report(DiagnosticCode::InvalidQueryShape,value.origin,message);};
      if(value.kind==ExprKind::Null)report("window frame offset must not be NULL");
      if(spec.frameUnit=="ROWS" || spec.frameUnit=="GROUPS") {
        if(offset.family==TypeFamily::Interval || temporal(offset.family) || (offset.family==TypeFamily::Boolean && (c.dialect==Dialect::PostgreSQL || c.dialect==Dialect::SQLServer)) || offset.arrayDimensions)
          report("ROWS / GROUPS offset requires a numeric value coercible to the native row-count type");
      }
      if(spec.frameUnit!="RANGE" || spec.ordering.size()!=1 ||
          (c.dialect!=Dialect::PostgreSQL && c.dialect!=Dialect::MySQL && c.dialect!=Dialect::MariaDB))continue;
      const auto &order=known(e.operands.at(1+spec.partitions));
      if (c.dialect==Dialect::MariaDB) {
        if (temporal(order.family))
          report("MariaDB RANGE with an offset requires numeric ordering");
        else if (offset.family!=TypeFamily::Interval && offset.family!=TypeFamily::Unknown &&
                 offset.family!=TypeFamily::Null && !numeric(offset.family))
          report("MariaDB RANGE requires a numeric offset");
        continue;
      }
      if(temporal(order.family) && offset.family!=TypeFamily::Unknown && offset.family!=TypeFamily::Null && offset.family!=TypeFamily::Interval)
        report("temporal RANGE ordering requires an interval offset in this dialect");
      else if(numeric(order.family) && offset.family!=TypeFamily::Unknown && offset.family!=TypeFamily::Null && !numeric(offset.family))
        report("numeric RANGE ordering requires a compatible numeric offset");
    }
  }
  SqlType expression(QueryId query,ExprId id,std::size_t depth) {
    tick(depth);
    if(!query || query>c.model.queries.size() || !id || id>=c.model.queries[query-1].expressions.size())return {};
    auto &state=states[query-1][id];auto &q=c.model.queries[query-1];auto &e=q.expressions[id];
    if(state==2)return c.model.types.get(e.sqlType);
    if(state==1)return {}; // Unknown facts for malformed/recursive alias dependencies.
    state=1;
    std::vector<SqlType> operands;for(auto child:e.operands)operands.push_back(expression(query,child,depth+1));
    auto type=categoryType(e.type);
    auto child=[&](std::size_t index){return index<operands.size()?operands[index]:SqlType{};};
    if(e.outputColumn && *e.outputColumn<q.projections.size() && q.projections[*e.outputColumn].expression!=id)
      type=expression(query,q.projections[*e.outputColumn].expression,depth+1);
    else if(e.localSymbol && localTypes.count(e.localSymbol) && localTypes.at(e.localSymbol))type=*localTypes.at(e.localSymbol);
    else if(e.binding)type=binding(*e.binding,depth+1);
    else if(!e.coalescedBindings.empty()) {
      std::vector<SqlType> columns;for(const auto &column:e.coalescedBindings)columns.push_back(binding(column,depth+1));
      type=common(columns);type.nullability=Nullability::Unknown;
    } else switch(e.kind) {
    case ExprKind::Null:type=categoryType(ValueType::Null);break;
    case ExprKind::MergeAction: {
      static const auto actionType=catalogTypeFromText(Dialect::SQLServer,"nvarchar(10)").value_or(SqlType{});
      type=actionType;
      type.nullability=Nullability::NotNull;
      break;
    }
    case ExprKind::String:
      type=categoryType(c.dialect==Dialect::Oracle && e.value.empty()?ValueType::Null:ValueType::String,Nullability::NotNull);
      type.flavor=c.dialect==Dialect::PostgreSQL?"untyped-string-literal":"connection-string-literal";
      if(c.dialect==Dialect::PostgreSQL)type.family=TypeFamily::Unknown;
      break;
    case ExprKind::Parameter:
      if (!e.localSymbol && c.dialect == Dialect::PostgreSQL && !e.value.empty() && e.value.front() == '$') {
        const auto &resolved = c.model.types.get(e.sqlType);
        if (resolved.family == TypeFamily::Unknown) type.flavor = "postgres-bind-parameter";
        else type = resolved;
      }
      break;
    case ExprKind::Number:
      if (c.dialect == Dialect::PostgreSQL) {
        type = pgNumericLiteral(e.value);
        if (type.family == TypeFamily::Decimal) e.type = ValueType::Decimal;
      } else if (c.dialect == Dialect::SQLServer) {
        type = tsqlNumericLiteral(e.value);
        e.type = valueCategory(type);
      } else if (c.dialect == Dialect::MySQL) {
        type = mysqlNumericLiteral(e.value);
        e.type = valueCategory(type);
      } else type.nullability=Nullability::NotNull;
      break;
    case ExprKind::Boolean:
      if (e.unresolvedBooleanName) type = {};
      else type.nullability=Nullability::NotNull;
      break;
    case ExprKind::Interval:
      type={};type.family=TypeFamily::Interval;
      if(e.interval) {
        const auto &s=*e.interval;
        type.precision=s.fractionalPrecision;
        type.intervalLeading=s.leading;type.intervalTrailing=s.trailing;type.intervalUnit=s.mysqlUnit;
        type.intervalLeadingPrecision=s.leadingPrecision;
        if(c.dialect==Dialect::Oracle) {
          if(!type.precision && static_cast<int>(s.leading)>=static_cast<int>(IntervalField::Day))type.precision=6;
          type.intervalLeadingPrecision=s.leadingPrecision.value_or(2);
          type.flavor=static_cast<int>(s.leading)<=static_cast<int>(IntervalField::Month)?"oracle-interval-year-month":"oracle-interval-day-second";
        } else if(s.mysqlUnit!=IntervalUnit::None)type.flavor="mysql-interval-expression";
      }
      break;
    case ExprKind::Extract:
      type={};type.family=isMySqlDialect(c.dialect)?TypeFamily::Integer:TypeFamily::Decimal;
      if(type.family==TypeFamily::Integer)type.bits=64;
      if(c.dialect==Dialect::Oracle && (e.value=="TIMEZONE_REGION" || e.value=="TIMEZONE_ABBR"))type.family=TypeFamily::String;
      type.nullability=child(0).nullability;
      if(c.dialect==Dialect::Oracle && child(0).flavor=="oracle-date" && e.value!="YEAR" && e.value!="MONTH" && e.value!="DAY")
        c.report(DiagnosticCode::InvalidQueryShape,e.origin,"Oracle EXTRACT treats DATE as ANSI DATE; this field requires TIMESTAMP or a compatible interval");
      break;
    case ExprKind::TypedLiteral:
      if(e.castType) {
        validate(*e.castType,TypeUse::Literal);
        const auto resolved=resolveDeclaredType(*e.castType,c.dialect,TypeUse::Literal);
        type=resolved.error.empty()?resolved.type:SqlType{};
        // Custom input functions and SQL modes may reject or alter a value.
        // Parsing syntax alone is not proof of a non-null successful value.
        type.nullability=Nullability::Unknown;
      }
      break;
    case ExprKind::Cast:case ExprKind::Convert:
      if(e.castType) {
        validate(*e.castType,TypeUse::Cast);const auto resolved=resolveDeclaredType(*e.castType,c.dialect,TypeUse::Cast);
        type=resolved.error.empty()?resolved.type:SqlType{};
        type.nullability=child(0).nullability==Nullability::Nullable?Nullability::Nullable:Nullability::Unknown;
        if (e.kind==ExprKind::Convert && (e.value!="CONVERT" ||
            (e.operands.size()==2 && child(1).nullability==Nullability::Nullable)))
          type.nullability=Nullability::Nullable;
        if (type.arrayDimensions && e.operands.size()==1) {
          const auto known=arrayShapes.find({query,e.operands.front()});
          if (known!=arrayShapes.end()) {
            arrayShapes.insert_or_assign(std::make_pair(query,id),known->second);
          }
        }
      }
      break;
    case ExprKind::Exists:type=categoryType(ValueType::Boolean,Nullability::NotNull);break;
    case ExprKind::IsNull:case ExprKind::TruthTest:type=categoryType(ValueType::Boolean,Nullability::NotNull);break;
    case ExprKind::Subquery:type=projected(e.query,0,depth+1);type.nullability=Nullability::Nullable;break;
    case ExprKind::Unary:
      if(e.value=="IS NULL" || e.value=="IS NOT NULL")type=categoryType(ValueType::Boolean,Nullability::NotNull);
      else {type=e.value=="NOT"?categoryType(ValueType::Boolean):child(0);type.nullability=combined(operands);}
      if (c.dialect == Dialect::PostgreSQL && e.value == "-" && e.operands.size() == 1) {
        // PostgreSQL folds chains of raw negative constants before choosing
        // int4/int8/numeric. Unary plus and casts remain actual operators.
        auto operand = e.operands.front();
        bool negative = true;
        while (operand && operand < q.expressions.size()) {
          tick(depth + 1);
          const auto &inner = q.expressions[operand];
          if (inner.kind == ExprKind::Number) {
            type = pgNumericLiteral(inner.value, negative);
            break;
          }
          if (inner.kind != ExprKind::Unary || inner.value != "-" || inner.operands.size() != 1) break;
          negative = !negative;
          operand = inner.operands.front();
        }
      }
      if (c.dialect == Dialect::MySQL && e.value == "-" && e.operands.size() == 1 &&
          mysqlArithmeticNumeric(type)) {
        const auto &inner = q.expressions.at(e.operands.front());
        if (inner.kind == ExprKind::Number) {
          type = mysqlNumericLiteral(inner.value, true);
        } else {
          if (type.family == TypeFamily::Integer) type.bits.reset();
          type.unsignedInteger = false;
        }
      }
      if (c.dialect == Dialect::SQLServer && e.value == "-" && tsqlNumericRank(type) == 1) {
        type.bits = 16;
        type.unsignedInteger = false;
      }
      break;
    case ExprKind::LikeEscape: {
      type=categoryType(ValueType::Boolean);
      type.nullability=combined(operands);
      if (e.operands.size()!=3) {
        c.report(DiagnosticCode::InvalidQueryShape,e.origin,"LIKE ESCAPE requires three operands");
        break;
      }
      const auto &escape=q.expressions.at(e.operands[2]);
      if (c.dialect==Dialect::MySQL && escape.kind==ExprKind::Null)
        type.nullability=combined(std::vector<SqlType>{operands[0],operands[1]});
      if (c.dialect==Dialect::Oracle && escape.kind==ExprKind::String && escape.value.empty())
        type.nullability=Nullability::Nullable;
      if (escape.kind==ExprKind::String) {
        const auto characters=c.dialect==Dialect::TiDB?escape.value.size():std::count_if(escape.value.begin(),escape.value.end(),
            [](unsigned char ch) { return (ch & 0xc0) != 0x80; });
        // Oracle's empty SQL string is NULL, not a zero-length escape value.
        const bool emptyAllowed=c.dialect==Dialect::Oracle || c.dialect==Dialect::PostgreSQL || c.dialect==Dialect::TiDB ||
            (isMySqlDialect(c.dialect) && !c.options.mysqlNoBackslashEscapes);
        if (characters!=1 && !(emptyAllowed && characters==0))
          c.report(DiagnosticCode::InvalidQueryShape,escape.origin,
                   c.dialect==Dialect::TiDB?"TiDB ESCAPE requires one byte (or an empty string)":
                   "ESCAPE requires one character (or an empty string where supported)");
      }
      break;
    }
    case ExprKind::Binary: {
      const auto a=child(0),b=child(1);
      const bool pg=c.dialect==Dialect::PostgreSQL,ora=c.dialect==Dialect::Oracle;
      if(e.value=="+" || e.value=="-") {
        const auto dateTime=[](const SqlType &t){return t.family==TypeFamily::Date || t.family==TypeFamily::Time || t.family==TypeFamily::Timestamp;};
        if(dateTime(a) && b.family==TypeFamily::Interval) {
          type=a;
          if(pg && a.family==TypeFamily::Date){type.family=TypeFamily::Timestamp;type.timeZone=TimeZoneKind::Without;}
          // MySQL returns DATETIME for DATE + sub-day intervals, DATE for
          // year/month/day units; unknown units must not invent one domain.
          if(!pg && !ora && a.family==TypeFamily::Date) {
            const auto u=b.intervalUnit;
            if(u!=IntervalUnit::Year && u!=IntervalUnit::Quarter && u!=IntervalUnit::Month && u!=IntervalUnit::Week && u!=IntervalUnit::Day && u!=IntervalUnit::YearMonth)
              {type.family=TypeFamily::Timestamp;type.flavor="datetime";}
          }
        } else if(e.value=="+" && a.family==TypeFamily::Interval && dateTime(b)) {
          type=b;if(pg && b.family==TypeFamily::Date){type.family=TypeFamily::Timestamp;type.timeZone=TimeZoneKind::Without;}
          else if(!pg && !ora && b.family==TypeFamily::Date)type={}; // infer conservatively until unit-specific symmetry is validated.
        } else if(a.family==TypeFamily::Interval && b.family==TypeFamily::Interval) {
          type=common({a,b});if(type.family==TypeFamily::Unknown && pg){type={};type.family=TypeFamily::Interval;}
        } else if(pg && a.family==TypeFamily::Date && b.family==TypeFamily::Integer)type=a;
        else if(pg && e.value=="-" && a.family==TypeFamily::Date && b.family==TypeFamily::Date)type=categoryType(ValueType::Integer);
        else if((pg || ora) && e.value=="-" && a.family==TypeFamily::Timestamp && b.family==TypeFamily::Timestamp) {
          type={};type.family=ora && a.flavor=="oracle-date" && b.flavor=="oracle-date"?TypeFamily::Decimal:TypeFamily::Interval;
        }
      }
      if (pg && (e.value=="+" || e.value=="-" || e.value=="*" || e.value=="/" || e.value=="%")) {
        const auto leftRank = pgNumericRank(a), rightRank = pgNumericRank(b);
        if (leftRank && rightRank && (e.value != "%" || std::max(leftRank, rightRank) <= 4)) {
          type = commonResult({a,b});
          // Numeric operators return an unconstrained value even when both
          // operands were declared with the same precision and scale.
          type.precision.reset();
          type.scale.reset();
        }
      }
      if (c.dialect == Dialect::SQLServer &&
          (e.value=="+" || e.value=="-" || e.value=="*" || e.value=="/" || e.value=="%")) {
        if (auto numeric = tsqlNumericArithmetic(q, e, a, b)) type = *numeric;
      }
      if (c.dialect == Dialect::MySQL &&
          (e.value=="+" || e.value=="-" || e.value=="*" || e.value=="/" || e.value=="%" || e.value=="DIV")) {
        if (auto numeric = mysqlNumericArithmetic(q, e, a, b, c.options)) type = *numeric;
      }
      type.nullability=combined(operands);
      if(e.value=="NULL_SAFE_EQUAL" || e.value=="IS DISTINCT FROM" || e.value=="IS NOT DISTINCT FROM")type.nullability=Nullability::NotNull;
      else if(!pg && (e.value=="/" || e.value=="%" || e.value=="DIV"))type.nullability=Nullability::Nullable;
      break;
    }
    case ExprKind::In:
      type.nullability = e.query ? Nullability::Nullable :
          c.dialect == Dialect::SQLite && operands.size() == 1 ?
              Nullability::NotNull : combined(operands);
      break;
    case ExprKind::Quantified: {
      type.nullability=Nullability::Nullable;
      if (!e.query && c.dialect==Dialect::PostgreSQL && operands.size()==2) {
        const auto &array=operands[1];
        const auto location=q.expressions.at(e.operands[1]).origin;
        if (!array.arrayDimensions && array.family!=TypeFamily::Unknown &&
            array.family!=TypeFamily::Null && array.opaqueName.empty())
          c.report(DiagnosticCode::InvalidQueryShape,location,
                   "PostgreSQL ANY, SOME and ALL require an array or a query on the right");
        if (array.arrayDimensions) {
          auto item=array;
          item.arrayDimensions=0;
          const auto left=knownPgCategory(operands.front()),element=knownPgCategory(item);
          if ((left && element && *left!=*element) ||
              (operands.front().arrayDimensions && element && operands.front().opaqueName.empty()))
            c.report(DiagnosticCode::InvalidQueryShape,location,
                     "array elements and the left operand have incompatible PostgreSQL comparison types");
        }
      }
      break;
    }
    case ExprKind::Between:type.nullability=combined(operands);break;
    case ExprKind::ArrayConstructor:
      if (e.query) {
        type=commonResult({projected(e.query,0,depth+1)});
        if (type.family!=TypeFamily::Unknown || !type.opaqueName.empty()) type.arrayDimensions=1;
        type.nullability=Nullability::NotNull;
      } else type=arrayType(query,id,e,std::move(operands));
      break;
    case ExprKind::Subscript: {
      type=child(0);
      type.nullability=Nullability::Nullable;
      const auto nativeJson=[&](std::string_view name) {
        return !type.opaqueName.empty() && type.opaqueName.back()==name &&
            (type.opaqueName.size()==1 || (type.opaqueName.size()==2 && type.opaqueName.front()=="pg_catalog"));
      };
      const bool slice=std::any_of(e.value.begin(),e.value.end(),[](auto axis) {
        return axis!=static_cast<char>(SubscriptKind::Index);
      });
      if (type.arrayDimensions) {
        if (!slice) type.arrayDimensions=0;
        if (e.value.size()>postgresArrayMaxDimensions)
          c.report(DiagnosticCode::InvalidQueryShape,e.origin,"PostgreSQL arrays support at most six subscripts");
        for (std::size_t i=1;i<operands.size();++i) {
          const auto &index=operands[i];
          const auto family=index.family;
          if (index.arrayDimensions || (index.opaqueName.empty() &&
              family!=TypeFamily::Unknown && family!=TypeFamily::Null &&
              family!=TypeFamily::Integer && family!=TypeFamily::Numeric &&
              family!=TypeFamily::Decimal && family!=TypeFamily::Floating))
            c.report(DiagnosticCode::InvalidQueryShape,q.expressions.at(e.operands[i]).origin,
                     "PostgreSQL array bounds must be coercible to integer");
        }
      } else if (nativeJson("jsonb")) {
        if (slice) c.report(DiagnosticCode::InvalidQueryShape,e.origin,"PostgreSQL JSONB subscripts do not support slices");
        for (std::size_t i=1;!slice && i<operands.size();++i) {
          const auto &index=operands[i];
          const auto family=index.family;
          const bool integer=family==TypeFamily::Integer && index.bits.value_or(32)<=32;
          const bool text=family==TypeFamily::String || family==TypeFamily::Character || family==TypeFamily::VarCharacter;
          if (index.arrayDimensions || (index.opaqueName.empty() && family!=TypeFamily::Unknown &&
              family!=TypeFamily::Null && !integer && !text))
            c.report(DiagnosticCode::InvalidQueryShape,q.expressions.at(e.operands[i]).origin,
                     "PostgreSQL JSONB subscripts require integer or text");
        }
      } else if (nativeJson("json") || (type.opaqueName.empty() && type.family!=TypeFamily::Unknown)) {
        c.report(DiagnosticCode::InvalidQueryShape,e.origin,"this PostgreSQL value type does not support subscripting");
        type={};
      } else type={}; // A custom PostgreSQL type may supply its own subscript handler.
      break;
    }
    case ExprKind::Case: {
      std::vector<SqlType> branches;
      for(std::size_t i=e.value=="simple"?2:1;i+1<operands.size();i+=2)branches.push_back(operands[i]);
      if(!operands.empty())branches.push_back(operands.back());
      type=commonResult(branches, e.origin);break;
    }
    case ExprKind::Call: {
      const auto op=builtinOperator(c.dialect,e.name,e.nameQuoted,e.operands.size());
      if (c.dialect==Dialect::SQLServer && e.jsonConstructor) {
        if (e.jsonConstructor->returningJson) {
          type={};
          type.opaqueName={"json"};
        } else type=catalogTypeFromText(Dialect::SQLServer,"nvarchar(max)").value_or(SqlType{});
        type.nullability=Nullability::NotNull;
        break;
      }
      if (e.withinGroup && op && op->requiresWithinGroup &&
          operands.size()==op->minArguments &&
          (!e.routineResolved || e.routineSchema=="pg_catalog")) {
        const auto &key=operands.back();
        const bool percentile=op->name!="MODE";
        const bool continuous=op->name=="PERCENTILE_CONT";
        const auto numeric=[](const SqlType &input) {
          return input.family==TypeFamily::Integer || input.family==TypeFamily::Decimal ||
                 input.family==TypeFamily::Floating;
        };
        const auto error=[&](ExprId part,const char *message) {
          c.report(DiagnosticCode::InvalidQueryShape,q.expressions.at(part).origin,message);
        };
        bool fractions=false;
        if (percentile) {
          const auto &fraction=operands.front();
          fractions=fraction.arrayDimensions!=0;
          if (fraction.opaqueName.empty() && !numeric(fraction) &&
              fraction.family!=TypeFamily::Unknown && fraction.family!=TypeFamily::Null)
            error(e.operands.front(),"percentile fraction must be a numeric scalar or array");
        }
        type=key;
        if (continuous) {
          type={};
          if (key.opaqueName.empty() && !key.arrayDimensions) {
            if (key.family==TypeFamily::Interval) type.family=TypeFamily::Interval;
            else if (numeric(key) || pgUntyped(key)) {
              type.family=TypeFamily::Floating;
              type.bits=64;
            } else if (key.family!=TypeFamily::Unknown)
              error(e.operands.back(),"continuous percentile requires numeric or interval ordering");
          } else if (key.arrayDimensions)
            error(e.operands.back(),"continuous percentile ordering cannot be an array");
        } else {
          if (key.family==TypeFamily::Null || key.flavor=="untyped-string-literal")
            error(e.operands.back(),"ordered-set aggregate needs a known ordered value type; use an explicit cast");
          if (fractions && key.arrayDimensions)
            error(e.operands.back(),"array fractions require a non-array discrete ordering value");
        }
        if (fractions) type.arrayDimensions=1;
        type.length.reset();
        type.precision.reset();
        type.scale.reset();
        type.nullability=Nullability::Nullable;
        break;
      }
      if (c.dialect==Dialect::PostgreSQL && op && op->name=="UNNEST" && operands.size()==1 &&
          (!e.routineResolved || (e.routineSchema=="pg_catalog" && e.routineKind==RoutineKind::TableFunction))) {
        const auto &array=operands.front();
        if (array.arrayDimensions>0 && array.opaqueName.empty()) {
          type=array;
          type.arrayDimensions=0;
          type.nullability=Nullability::Nullable;
        }
        break;
      }
      if (c.dialect==Dialect::PostgreSQL && op && op->name=="GENERATE_SERIES" &&
          operands.size()>=2 && operands.size()<=4 &&
          (!e.routineResolved || (e.routineSchema=="pg_catalog" && e.routineKind==RoutineKind::TableFunction))) {
        const auto exactNumeric=[](const auto &operand) {
          return !operand.arrayDimensions && operand.opaqueName.empty() &&
              (operand.family==TypeFamily::Integer || operand.family==TypeFamily::Decimal ||
               operand.family==TypeFamily::Unknown || operand.family==TypeFamily::Null);
        };
        if (operands.size()<=3 && std::all_of(operands.begin(),operands.end(),exactNumeric)) {
          const auto has=[&](TypeFamily family,int bits=0) {
            return std::any_of(operands.begin(),operands.end(),[&](const auto &operand) {
              return operand.family==family && (!bits || operand.bits==bits);
            });
          };
          if (has(TypeFamily::Decimal)) type.family=TypeFamily::Decimal;
          else if (has(TypeFamily::Integer,64)) type=pgNumericLiteral("2147483648");
          else if (has(TypeFamily::Integer,32)) type=pgNumericLiteral("0");
        } else if (operands.size()>=3 && operands[0].family==TypeFamily::Timestamp &&
                   operands[1].family==TypeFamily::Timestamp && operands[2].family==TypeFamily::Interval &&
                   std::none_of(operands.begin(),operands.end(),[](const auto &operand) { return operand.arrayDimensions; })) {
          type=operands[0].timeZone==TimeZoneKind::With?operands[0]:operands[1];
          type.precision.reset();
        }
        type.nullability=Nullability::NotNull;
        break;
      }
      if (e.routineResolved) {
        if (e.routineType) type = *e.routineType;
        // The catalog binds a family of native overloads by name and arity.
        // Infer its exact aggregate result from the bound argument instead of
        // treating the intersection of all overloads as an exact result.
        if (c.dialect != Dialect::PostgreSQL || e.routineSchema != "pg_catalog" ||
            e.routineKind != RoutineKind::Aggregate || !op || op->kind != RoutineKind::Aggregate)
          break;
      }
      if(!op)break;
      const auto name=asciiUpper(e.name.back());
      const bool pg=c.dialect==Dialect::PostgreSQL,ora=c.dialect==Dialect::Oracle,
        my=isMySqlDialect(c.dialect);
      if (pg && op->kind==RoutineKind::Window &&
          ((name=="NTILE" && !operands.empty()) ||
           ((name=="NTH_VALUE" || name=="LAG" || name=="LEAD") && operands.size()>1))) {
        const auto position=name=="NTILE"?0u:1u;
        const auto &argument=operands.at(position);
        if (argument.opaqueName.empty() && (argument.arrayDimensions || (knownPgCategory(argument) &&
            (argument.family!=TypeFamily::Integer || (argument.bits && *argument.bits>32)))))
          c.report(DiagnosticCode::InvalidQueryShape,q.expressions.at(e.operands[position]).origin,
                   name+" requires a PostgreSQL integer argument in this position");
      }
      if (op->kind==RoutineKind::Window &&
          (c.dialect==Dialect::MySQL || c.dialect==Dialect::MariaDB || c.dialect==Dialect::TiDB) &&
          ((name=="NTILE" && !operands.empty()) ||
           ((name=="NTH_VALUE" || name=="LAG" || name=="LEAD") && operands.size()>1))) {
        const bool tidb=c.dialect==Dialect::TiDB;
        const auto position=name=="NTILE"?0u:1u;
        const auto &argument=q.expressions.at(e.operands.at(position));
        const bool positive=name=="NTILE" || (name=="NTH_VALUE" && c.dialect!=Dialect::MariaDB);
        const bool exactInteger=tidb || (c.dialect==Dialect::MySQL && name=="NTH_VALUE");
        const bool nullForbidden=(c.dialect==Dialect::MySQL && name=="NTH_VALUE") ||
            (c.dialect==Dialect::MariaDB && name=="NTILE");
        auto report=[&](const std::string &message) { c.report(DiagnosticCode::InvalidQueryShape,argument.origin,message); };
        if (argument.kind==ExprKind::Null && nullForbidden)
          report(name+" does not permit a NULL position/bucket count in this dialect");
        if (positive && argument.kind==ExprKind::Boolean && argument.value=="FALSE")
          report(name+" requires a positive position/bucket count");
        if (exactInteger && argument.kind==ExprKind::String)
          report(name+" requires an integer-typed argument, not a string literal");
        if (((c.dialect==Dialect::MySQL && name=="NTH_VALUE") ||
             (tidb && (name=="NTILE" || name=="NTH_VALUE"))) &&
            argument.kind==ExprKind::Name && argument.binding)
          report(name+" requires an execution-constant argument rather than a query column");
        const ModelExpression *literal=&argument;
        bool negative=false;
        if (argument.kind==ExprKind::Unary && argument.operands.size()==1 &&
            (argument.value=="-" || argument.value=="+")) {
          negative=argument.value=="-";
          literal=&q.expressions.at(argument.operands.front());
        }
        const auto &sqlType=operands.at(position);
        if (exactInteger && literal->kind!=ExprKind::Number && argument.kind!=ExprKind::String &&
            argument.kind!=ExprKind::Null && sqlType.opaqueName.empty() &&
            (sqlType.family==TypeFamily::Decimal || sqlType.family==TypeFamily::Floating ||
             sqlType.family==TypeFamily::String || sqlType.family==TypeFamily::Character ||
             sqlType.family==TypeFamily::VarCharacter || sqlType.family==TypeFamily::Date ||
             sqlType.family==TypeFamily::Time || sqlType.family==TypeFamily::Timestamp ||
             sqlType.family==TypeFamily::Interval || sqlType.family==TypeFamily::Bytes ||
             sqlType.family==TypeFamily::Binary || sqlType.family==TypeFamily::VarBinary))
          report(name+" requires an integer-typed execution-constant argument");
        if (literal->kind==ExprKind::Number) {
          std::uint64_t value=0;
          const auto end=literal->value.data()+literal->value.size();
          const auto parsed=std::from_chars(literal->value.data(),end,value);
          const bool integer=!literal->value.empty() && parsed.ec==std::errc{} && parsed.ptr==end;
          if (exactInteger && !integer)
            report(name+" requires an integer-typed numeric literal");
          if (positive && integer && (value==0 || negative))
            report(name+" requires a positive position/bucket count");
          if (integer && !negative && value>static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max()) &&
              (c.dialect==Dialect::MySQL || (c.dialect==Dialect::MariaDB && name=="NTILE")))
            report(name+" integer literal exceeds this dialect's signed execution range");
        }
      }
      if(pg || ora || my || c.dialect==Dialect::SQLServer) {
        if(name=="NOW" || name=="CURRENT_TIMESTAMP" || name=="LOCALTIMESTAMP" || name=="GETDATE" || name=="SYSDATETIME" || name=="SYSDATE" || name=="SYSTIMESTAMP" || (my && name=="UTC_TIMESTAMP")) {
          type={};type.family=TypeFamily::Timestamp;
          type.timeZone=(pg && (name=="NOW" || name=="CURRENT_TIMESTAMP")) || (ora && (name=="SYSTIMESTAMP" || name=="CURRENT_TIMESTAMP"))?TimeZoneKind::With:TimeZoneKind::Without;
          if(my || name=="GETDATE")type.flavor="datetime";
          if(ora && name=="SYSDATE")type.flavor="oracle-date";
          type.nullability=Nullability::NotNull;
        } else if(name=="CURRENT_DATE" || (my && (name=="CURDATE" || name=="UTC_DATE"))) {
          type={};type.family=ora?TypeFamily::Timestamp:TypeFamily::Date;if(ora)type.flavor="oracle-date";
          type.nullability=Nullability::NotNull;
        } else if(name=="CURRENT_TIME" || name=="LOCALTIME" || (my && (name=="CURTIME" || name=="UTC_TIME"))) {
          type={};type.family=TypeFamily::Time;type.timeZone=pg && name=="CURRENT_TIME"?TimeZoneKind::With:TimeZoneKind::Without;
          type.nullability=Nullability::NotNull;
        }
      }
      if(my && (name=="DATE" || name=="TIME" || name=="TIMESTAMP")) {
        type={};type.family=name=="DATE"?TypeFamily::Date:name=="TIME"?TypeFamily::Time:TypeFamily::Timestamp;
        if(name=="TIMESTAMP")type.flavor="datetime";
        type.nullability=Nullability::Nullable;
      }
      if(my && (name=="DATE_ADD" || name=="DATE_SUB" || name=="ADDDATE" || name=="SUBDATE") && child(1).family==TypeFamily::Interval) {
        type=child(0);const auto u=child(1).intervalUnit;
        const bool dateUnit=u==IntervalUnit::Year || u==IntervalUnit::Quarter || u==IntervalUnit::Month || u==IntervalUnit::Week || u==IntervalUnit::Day || u==IntervalUnit::YearMonth;
        if(type.family==TypeFamily::Date && !dateUnit)type.family=TypeFamily::Timestamp;
        if(type.family==TypeFamily::Timestamp)type.flavor="datetime";
        if(type.family==TypeFamily::Time && dateUnit)type={};
        type.nullability=Nullability::Nullable;
      }
      if(type.family==TypeFamily::Timestamp || type.family==TypeFamily::Time) {
        const bool precisionFunction=name=="NOW" || name=="CURRENT_TIMESTAMP" || name=="CURRENT_TIME" || name=="LOCALTIME" || name=="LOCALTIMESTAMP" ||
          (my && (name=="CURTIME" || name=="UTC_TIME" || name=="UTC_TIMESTAMP"));
        if(precisionFunction && !e.operands.empty()) {
          const auto &argument=q.expressions.at(e.operands[0]);
          const bool dynamicNow=c.dialect==Dialect::TiDB && name=="NOW";
          const auto *literal=&argument;
          bool negative=false;
          if (dynamicNow && argument.kind==ExprKind::Unary && argument.operands.size()==1 &&
              (argument.value=="+" || argument.value=="-")) {
            negative=argument.value=="-";
            literal=&q.expressions.at(argument.operands.front());
          }
          if (dynamicNow && literal->kind==ExprKind::Null) type.precision=0;
          else if (literal->kind==ExprKind::Number && !literal->value.empty() &&
                   literal->value.find_first_not_of("0123456789")==std::string::npos) {
            auto digits=std::string_view(literal->value);
            const auto nonzero=digits.find_first_not_of('0');
            if (nonzero==std::string_view::npos) type.precision=0;
            else {
              digits.remove_prefix(nonzero);
              if (negative || digits.size()>1 || digits.front()>'0'+(ora?9:6))
                c.report(DiagnosticCode::InvalidQueryShape,argument.origin,"temporal precision is outside this dialect's supported range");
              else type.precision=digits.front()-'0';
            }
          } else if (dynamicNow) {
            // TiDB NOW accepts expressions and performs an integer cast at
            // evaluation time. Do not invent a literal-only grammar or fsp.
            type.precision.reset();
          } else c.report(DiagnosticCode::InvalidQueryShape,argument.origin,"temporal precision requires an integer literal");
        } else if(my && precisionFunction)type.precision=0;
      }
      if(op->kind==RoutineKind::Aggregate) {
        const auto resolved=resolveAggregateType(c.dialect,name,child(0));
        if(!resolved.error.empty())c.report(DiagnosticCode::InvalidQueryShape,e.operands.empty()?e.origin:q.expressions.at(e.operands[0]).origin,resolved.error);
        if(resolved.type.family!=TypeFamily::Unknown)type=resolved.type;
        if (c.dialect == Dialect::MySQL && type.family == TypeFamily::Decimal &&
            (name == "SUM" || name == "AVG") && !e.operands.empty()) {
          type.unsignedInteger = false;
          const auto shape = mysqlExactShape(q, e.operands.front(), child(0));
          const auto growth = name == "SUM" ? std::optional<int>(22) : mysqlDivisionIncrement(c.options);
          if (shape && growth) {
            type.precision = std::min(mysqlMaxPrecision, shape->first + *growth);
            type.scale = name == "SUM" ? shape->second : std::min(mysqlMaxScale, shape->second + *growth);
          }
        }
      }
      if(name=="COUNT" || name=="COUNT_BIG" || name=="ROW_NUMBER" || name=="RANK" || name=="DENSE_RANK" || name=="GROUPING") {
        type.nullability=Nullability::NotNull;
        if(c.dialect==Dialect::SQLServer && (name=="COUNT" || name=="COUNT_BIG"))type.nullability=Nullability::Unknown;
      } else if(name=="COALESCE") {

        type=commonResult(operands, e.origin);
        for(const auto &operand:operands)if(operand.nullability==Nullability::NotNull){type.nullability=Nullability::NotNull;break;}
      } else if (pg && (name=="GREATEST" || name=="LEAST")) {

        type=commonResult(operands, e.origin);
        for(const auto &operand:operands)if(operand.nullability==Nullability::NotNull){type.nullability=Nullability::NotNull;break;}
      } else if (name=="NULLIF") {
        const auto left = child(0), right = child(1);
        type = left;
        // PostgreSQL has equality overloads for different widths within
        // the integer and floating families. Those preserve the left type.
        if (pg && (pgUntyped(left) ||
                   (pgNumericRank(left) && pgNumericRank(right) && left.family != right.family)))
          type = commonResult(operands, e.origin);
        type.nullability = Nullability::Nullable;
      }
      else if(c.dialect==Dialect::SQLServer && name.compare(0,6,"ERROR_")==0) {
        type.nullability=Nullability::Nullable;
        if(type.family==TypeFamily::Integer) {
          type.bits=32;
          type.unsignedInteger=false;
        }
      }
      else if ((pg || c.dialect==Dialect::TiDB) && name=="NTILE") {
        if (pg) type=pgNumericLiteral("0");
        type.nullability=Nullability::Nullable;
      }
      else if(name=="FIRST_VALUE" || name=="LAST_VALUE" || name=="NTH_VALUE") {
        const auto input = child(0);
        if (!input.arrayDimensions && !input.sqliteAffinity && input.opaqueName.empty() &&
            (input.family == TypeFamily::Integer || input.family == TypeFamily::Boolean))
          type=input;
        type.nullability=Nullability::Nullable; // The selected frame position may not exist.
      }
      else if(op->kind==RoutineKind::Aggregate && !(c.dialect==Dialect::SQLite && name=="TOTAL"))type.nullability=Nullability::Nullable;
      break;
    }
    case ExprKind::AggregateFilter:type=child(0);break;
    case ExprKind::Window:type=child(0);windowTypes(q,e);break;
    default:break;
    }
    // Grouped source columns may be replaced by NULL in a subtotal. This is a
    // conservative fact even when the same column occurs inside an aggregate.
    if(e.binding && !q.groupingSets.empty()) {
      bool omitted=false;
      for(const auto &set:q.groupingSets) {
        bool retained=false;
        for(auto key:set) {
          const auto &k=q.expressions.at(key);
          if(k.binding && k.binding->scope==e.binding->scope && k.binding->table==e.binding->table && k.binding->column==e.binding->column)retained=true;
        }
        omitted |= !retained;
      }
      if(omitted)type.nullability=Nullability::Nullable;
    }
    e.sqlType=intern(std::move(type));state=2;return c.model.types.get(e.sqlType);
  }
  bool parameterTypes() {
    if (c.dialect != Dialect::PostgreSQL || c.names.environment != SemanticEnvironment::Sql)
      return false;
    bool changed = false;
    struct Evidence {
      SqlType type;
      bool conflicting = false;
    };
    static constexpr std::string_view comparisons[] = {
        "=", "<>", "!=", "<", "<=", ">", ">=", "IS DISTINCT FROM", "IS NOT DISTINCT FROM"};
    const auto parameter = [](const ModelExpression &e) {
      return e.kind == ExprKind::Parameter && !e.localSymbol &&
             !e.value.empty() && e.value.front() == '$';
    };
    const auto boolean = categoryType(ValueType::Boolean);
    const auto integer = pgNumericLiteral("0");
    const auto bigint = pgNumericLiteral("2147483648");
    // Lowering reserves a root before visiting its subqueries, so contiguous
    // query IDs between roots belong to one SQL statement's parameter scope.
    for (std::size_t root = 0; root < c.model.roots.size(); ++root) {
      const auto first = c.model.roots[root] - 1;
      const auto end = root + 1 == c.model.roots.size() ?
          c.model.queries.size() : c.model.roots[root + 1] - 1;
      std::map<std::string_view, Evidence> candidates;
      for (auto index = std::size_t(first); index < end; ++index) {
        const auto &q = c.model.queries[index];
        const auto typeOf = [&](ExprId id) -> const SqlType & {
          return c.model.types.get(q.expressions.at(id).sqlType);
        };
        const auto observe = [&](const ModelStatement &owner, ExprId id, const SqlType &expectedType) {
          if (!id || id >= owner.expressions.size() || !parameter(owner.expressions[id])) return;
          auto scalarType = expectedType;
          scalarType.arrayDimensions = 0;
          if (!knownPgCategory(scalarType)) return;
          const auto &e = owner.expressions[id];
          auto expected = expectedType;
          // A PostgreSQL parameter identity is a base type OID. Declared
          // modifiers and mandatory nullability belong to its enclosing use.
          expected.nullability = Nullability::Unknown;
          expected.precision.reset();
          expected.scale.reset();
          expected.length.reset();
          expected.collation.clear();
          if (expected.family == TypeFamily::Decimal) expected.unsignedInteger = false;
          if (expected.arrayDimensions) expected.arrayDimensions = 1;
          auto [found, fresh] = candidates.emplace(e.value, Evidence{expected});
          if (!fresh && found->second.type != expected) found->second.conflicting = true;
        };
        const auto here = [&](ExprId id, const SqlType &expected) { observe(q, id, expected); };
        bool localParameters = false;
        for (const auto &e : q.expressions) {
          tick(0);
          localParameters |= parameter(e);
          const auto &args = e.operands;
          if (args.size() == 1 && e.kind == ExprKind::Cast) {
            here(args.front(), c.model.types.get(e.sqlType));
          } else if (e.kind==ExprKind::Subscript && !args.empty() && typeOf(args.front()).arrayDimensions) {
            for (auto i=std::next(args.begin());i!=args.end();++i) here(*i,integer);
          } else if (e.kind==ExprKind::In && !e.query && args.size()>1) {
            const auto &left=typeOf(args.front());
            // With an exact integer left side and only untyped parameters
            // and NULLs in the list, no right operand changes type selection.
            if (!left.arrayDimensions && left.opaqueName.empty() &&
                left.family==TypeFamily::Integer && left.unsignedInteger==false &&
                (left.bits==16 || left.bits==32 || left.bits==64) &&
                std::all_of(std::next(args.begin()),args.end(),[&](ExprId id) {
                  tick(0);
                  const auto &item=q.expressions.at(id);
                  return parameter(item) || item.kind==ExprKind::Null;
                }))
              for (auto i=std::next(args.begin());i!=args.end();++i) here(*i,left);
          } else if (e.kind==ExprKind::Quantified && !e.query && args.size()==2) {
            auto array=typeOf(args[1]);
            if (array.arrayDimensions) {
              array.arrayDimensions=0;
              here(args[0],array);
            } else {
              auto element=typeOf(args[0]);
              if (!element.arrayDimensions && knownPgCategory(element)) {
                element.arrayDimensions=1;
                here(args[1],element);
              }
            }
          } else if (e.kind == ExprKind::Binary && args.size() == 2 &&
                     std::find(std::begin(comparisons), std::end(comparisons), e.value) != std::end(comparisons)) {
            here(args[0], typeOf(args[1]));
            here(args[1], typeOf(args[0]));
          } else if (e.kind == ExprKind::Binary && args.size() == 2 && (e.value == "AND" || e.value == "OR")) {
            here(args[0], boolean);
            here(args[1], boolean);
          } else if (e.kind == ExprKind::Binary && args.size() == 2 &&
                     (e.value == "+" || e.value == "-" || e.value == "*" || e.value == "/" || e.value == "%")) {
            // A known native exact operand supplies a same-type
            // operator candidate. This can conflict with a later assignment
            // coercion; do not label that parameter with the target type alone.
            const auto observeExactOperand = [&](ExprId expression, ExprId other) {
              const auto &type = typeOf(other);
              const bool integer=type.family==TypeFamily::Integer && type.unsignedInteger==false &&
                  (type.bits==16 || type.bits==32 || type.bits==64);
              const bool numeric=type.family==TypeFamily::Decimal && type.flavor.empty();
              if ((integer || numeric) && !type.arrayDimensions && type.opaqueName.empty())
                here(expression, type);
            };
            observeExactOperand(args[0], args[1]);
            observeExactOperand(args[1], args[0]);
          } else if (e.kind == ExprKind::Unary && args.size() == 1 && e.value == "NOT") {
            here(args.front(), boolean);
          } else if (e.kind == ExprKind::Case && !args.empty()) {
            const auto expected = c.model.types.get(e.sqlType);
            const bool simple = e.value == "simple";
            for (std::size_t i = simple ? 1 : 0; i + 1 < args.size() - 1; i += 2) {
              tick(0);
              if (!simple) here(args[i], boolean);
              here(args[i + 1], expected);
            }
            here(args.back(), expected);
          } else if (e.kind==ExprKind::Call && !args.empty()) {
            const auto op = builtinOperator(c.dialect, e.name, e.nameQuoted, args.size());
            if (op && op->kind==RoutineKind::Window &&
                (!e.routineResolved || (e.routineSchema=="pg_catalog" && e.routineKind==RoutineKind::Window))) {
              if (op->name=="NTILE") here(args.front(),integer);
              else if (args.size()>=2 && (op->name=="NTH_VALUE" || op->name=="LAG" || op->name=="LEAD"))
                here(args[1],integer);
            }
            if (e.withinGroup && op && op->requiresWithinGroup && args.size()==2 &&
                (!e.routineResolved || e.routineSchema=="pg_catalog")) {
              SqlType fraction;
              fraction.family=TypeFamily::Floating;
              fraction.bits=64;
              here(args.front(),fraction);
            }
            const bool series=op && op->name=="GENERATE_SERIES" &&
                c.model.types.get(e.sqlType).family==TypeFamily::Integer &&
                (!e.routineResolved || e.routineSchema=="pg_catalog");
            if (series || (op && !e.routineResolved &&
                (op->name=="COALESCE" || op->name=="GREATEST" || op->name=="LEAST")))
              for (auto id : args) {
                tick(0);
                here(id, c.model.types.get(e.sqlType));
              }
          }
        }
        here(q.prewhere, boolean);
        here(q.where, boolean);
        here(q.qualify, boolean);
        here(q.having, boolean);
        here(q.conflict.targetWhere, boolean);
        here(q.conflict.where, boolean);
        here(q.mergeOn, boolean);
        for (const auto &clause : q.mergeClauses) here(clause.condition, boolean);
        if ((localParameters || q.insertSource == InsertSource::Query) && q.catalogChecked &&
            q.targetTable && *q.targetTable < q.tables.size()) {
          const auto &target = q.tables[*q.targetTable];
          if (target.definition && target.definition->complete && target.definition->arityKnown) {
            const auto &columns = target.definition->columns;
            std::map<std::string_view, std::size_t> ordinals;
            const auto targetType = [&](std::size_t ordinal) {
              return ordinal < columns.size() ? binding({*q.targetTable, ordinal, q.id}, 0) : SqlType{};
            };
            const auto positionType = [&](const auto &names, std::size_t position) {
              if (names.empty()) return targetType(position);
              if (position >= names.size()) return SqlType{};
              const auto found = ordinals.find(names[position]);
              return found != ordinals.end() ? targetType(found->second) : SqlType{};
            };
            for (std::size_t i = 0; i < columns.size(); ++i) {
              tick(0);
              ordinals.emplace(columns[i].name, i);
            }
            const auto row = [&](const auto &names, const auto &values) {
              for (std::size_t i = 0; i < values.size(); ++i) {
                tick(0);
                here(values[i], positionType(names, i));
              }
            };
            const auto assigned = [&](const std::vector<Assignment> &assignments) {
              const auto observeAssignment = [&](ExprId lhs, ExprId rhs) {
                const auto &column = q.expressions.at(lhs);
                if (column.kind==ExprKind::Subscript && !column.operands.empty()) {
                  const auto &base=q.expressions.at(column.operands.front());
                  if (base.binding && base.binding->scope==q.id && base.binding->table==*q.targetTable)
                    here(rhs,typeOf(lhs));
                } else if (column.binding && column.binding->scope == q.id && column.binding->table == *q.targetTable)
                  here(rhs, targetType(column.binding->column));
              };
              for (const auto &assignment : assignments) {
                tick(0);
                if (!assignment.value || assignment.targets.empty()) continue;
                if (assignment.targets.size() == 1) {
                  observeAssignment(assignment.targets.front(), assignment.value);
                  continue;
                }
                const auto &tuple = q.expressions.at(assignment.value);
                if (tuple.kind != ExprKind::Row || tuple.operands.size() != assignment.targets.size()) continue;
                for (std::size_t i = 0; i < tuple.operands.size(); ++i) {
                  tick(0);
                  observeAssignment(assignment.targets[i], tuple.operands[i]);
                }
              }
            };
            assigned(q.assignments);
            assigned(q.conflict.assignments);
            if (q.insertSource == InsertSource::Values)
              for (const auto &values : q.rows) row(q.columns, values);
            if (q.insertSource == InsertSource::Query && q.source > first && q.source <= end) {
              const auto &input = c.model.queries[q.source - 1];
              if (!input.setOperation && !input.valuesQuery)
                for (std::size_t i = 0; i < input.projections.size(); ++i) {
                  tick(0);
                  observe(input, input.projections[i].expression, positionType(q.columns, i));
                }
            }
            for (const auto &clause : q.mergeClauses) {
              assigned(clause.assignments);
              if (clause.action == MergeAction::Insert) row(clause.columns, clause.values);
            }
          }
        }
        here(q.limit, bigint);
        here(q.offset, bigint);
        if (q.valuesQuery) {
          for (const auto &row : q.rows)
            for (std::size_t column = 0; column < std::min(row.size(), q.projections.size()); ++column) {
              tick(0);
              here(row[column], typeOf(q.projections[column].expression));
            }
        }
        if (q.setOperation && !q.setOperation->recursive) {
          for (const auto arm : {q.setOperation->left, q.setOperation->right}) {
            if (arm <= first || arm > end) continue;
            const auto &input = c.model.queries[arm - 1];
            if (input.projections.size() != q.projections.size()) continue;
            for (std::size_t column = 0; column < input.projections.size(); ++column) {
              tick(0);
              observe(input, input.projections[column].expression,
                      typeOf(q.projections[column].expression));
            }
          }
        }
      }
      if (candidates.empty()) continue;
      for (auto index = std::size_t(first); index < end; ++index)
        for (auto &e : c.model.queries[index].expressions) {
          tick(0);
          if (!parameter(e)) continue;
          const auto found = candidates.find(e.value);
          if (found != candidates.end() && !found->second.conflicting) {
            e.sqlType = intern(found->second.type);
            changed |= e.sqlType != 0;
          }
        }
    }
    return changed;
  }
public:
  explicit Inference(Compilation &context):c(context),states(c.model.queries.size()),nullableTables(c.model.queries.size()){}
  void run() {
    if(c.names.locals.size()>8192)limit("SQL local type context symbol limit exceeded");
    for(const auto &symbol:c.names.locals) {
      auto inserted=localTypes.emplace(symbol.symbol,symbol.sqlType);
      if(!inserted.second && (!symbol.sqlType || !inserted.first->second || *symbol.sqlType!=*inserted.first->second))inserted.first->second.reset();
    }
    for(std::size_t i=0;i<c.model.queries.size();++i) {
      auto &q=c.model.queries[i];states[i].resize(q.expressions.size());
      declarations(q);outerNullable(i);
      if (c.dialect==Dialect::PostgreSQL)
        for (const auto &value:q.expressions) {
          tick(0);
          if (value.kind!=ExprKind::Cast || !value.castType || !value.castType->arrayDimensions ||
              value.operands.size()!=1 || q.expressions.at(value.operands.front()).kind!=ExprKind::ArrayConstructor ||
              q.expressions.at(value.operands.front()).query)
            continue;
          const auto declared=resolveDeclaredType(*value.castType,c.dialect,TypeUse::Cast);
          if (!declared.error.empty()) continue;
          std::vector<ExprId> arrays{value.operands.front()};
          while (!arrays.empty()) {
            tick(0);
            const auto id=arrays.back();
            arrays.pop_back();
            if (!arrayCastTypes.emplace(std::make_pair(q.id,id),declared.type).second) continue;
            for (auto part:q.expressions.at(id).operands) {
              const auto &child=q.expressions.at(part);
              if (child.kind==ExprKind::ArrayConstructor && !child.query) arrays.push_back(part);
            }
          }
        }
    }
    const auto inferExpressions = [&] {
      for (auto &q : c.model.queries)
        for (ExprId id = 1; id < q.expressions.size(); ++id) expression(q.id,id,0);
    };
    inferExpressions();
    for (auto &q:c.model.queries)
      for (std::size_t i=0;i<q.tables.size();++i)
        if (const auto &unpivot=q.tables[i].unpivot) {
          for (std::size_t field=0;field<unpivot->descriptors.size();++field) unpivotDescriptor(q,i,field,0);
          for (std::size_t field=0;field<unpivot->measures.size();++field) unpivotMeasure(q,i,field,0);
          auto &table=q.tables[i];
          if (c.dialect==Dialect::PostgreSQL && table.definition && table.definition->complete) {
            auto definition=*table.definition;
            const auto offset=unpivot->retainedOrdinals.size()+unpivot->descriptors.size();
            for (std::size_t field=0;field<unpivot->measures.size();++field) {
              auto &column=definition.columns.at(offset+field);
              auto type=unpivotMeasure(q,i,field,0);
              if (column.nonNullKnown) type.nullability=Nullability::NotNull;
              column.type=valueCategory(type);
              column.sqlType=std::make_shared<const SqlType>(std::move(type));
            }
            table.definition=std::make_shared<const TableDefinition>(std::move(definition));
          }
        }
    if (parameterTypes() && c.diagnostics.empty()) {
      // The same constraint can type a parameter in SET and another
      // occurrence inside RETURNING, a CTE or a scalar subquery.
      valuesTypes.clear();
      arrayShapes.clear();
      for (auto &queryStates : states) std::fill(queryStates.begin(), queryStates.end(), 0);
      inferExpressions();
    }
    if (c.dialect==Dialect::PostgreSQL)
      for (auto &q:c.model.queries)
        for (auto &t:q.tables)
          if (t.pivot) finalizePostgresPivot(q,t);
    for (const auto &q : c.model.queries) recursiveTypes(q);
    if (c.dialect==Dialect::PostgreSQL)
      for (const auto &q:c.model.queries)
        for (const auto id:{q.limit,q.offset}) if (id) {
          tick(0);
          const auto &bound=q.expressions.at(id);
          const auto &type=c.model.types.get(bound.sqlType);
          const bool numeric=type.family==TypeFamily::Integer || type.family==TypeFamily::Numeric ||
              type.family==TypeFamily::Decimal || type.family==TypeFamily::Floating;
          if (type.arrayDimensions || bound.kind==ExprKind::Row ||
              (!numeric && type.family!=TypeFamily::Unknown && type.family!=TypeFamily::Null))
            c.report(DiagnosticCode::InvalidQueryShape,bound.origin,
                     "PostgreSQL pagination requires a value coercible to bigint");
        }
    if (c.dialect==Dialect::SQLite)
      for (const auto &q:c.model.queries)
        for (const auto id:{q.limit,q.offset}) if (id) {
          tick(0);
          const auto &bound=q.expressions.at(id);
          if (c.model.types.get(bound.sqlType).family==TypeFamily::Null)
            c.report(DiagnosticCode::InvalidQueryShape,bound.origin,
                     "SQLite pagination requires a non-NULL integer value");
          const auto *literal=&bound;
          while (literal->kind==ExprKind::Unary && literal->operands.size()==1 &&
                 (literal->value=="+" || literal->value=="-"))
            literal=&q.expressions.at(literal->operands.front());
          // Integer-valued REAL bounds are legal. Check only finite, safely
          // represented literal fractions; defer dynamic input/range coercion.
          if (literal->kind==ExprKind::Number && literal->value.size()<=128 &&
              literal->value.find_first_of(".eE")!=std::string::npos) {
            try {
              std::size_t consumed=0;
              const auto number=std::stod(literal->value,&consumed);
              if (consumed==literal->value.size() && std::isfinite(number) &&
                  std::abs(number)<=9007199254740991.0 && std::floor(number)!=number)
                c.report(DiagnosticCode::InvalidQueryShape,bound.origin,
                         "SQLite pagination literal cannot be losslessly converted to an integer");
            } catch (const std::exception &) {
              // Overflow and platform-dependent input parsing need native facts.
            }
          }
        }
    for(const auto &q:c.model.queries)for(const auto &window:q.namedWindows) {
      tick(0);ModelExpression declaration;declaration.window=window.specification;declaration.origin=window.origin;
      declaration.operands.push_back(0);declaration.operands.insert(declaration.operands.end(),window.keys.begin(),window.keys.end());
      windowTypes(q,declaration);
    }
  }
};
}
void inferSqlTypes(Compilation &compilation) { Inference(compilation).run(); }
} // namespace navidog::sql::compiler
