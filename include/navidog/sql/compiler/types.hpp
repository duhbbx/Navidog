#pragma once
#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <tuple>
#include <vector>

namespace navidog::sql::compiler {
enum class Dialect;
enum class ValueType;
using TypeId = std::uint32_t;
enum class TypeFamily { Unknown, Null, Boolean, Integer, Numeric, Decimal, Floating, String, Bytes,
                        Character, VarCharacter, Binary, VarBinary,
                        Date, Time, Timestamp, Interval };
enum class Nullability { Unknown, Nullable, NotNull };
inline constexpr std::size_t postgresArrayMaxDimensions=6;
enum class TimeZoneKind { Unknown, Without, With, Local };
enum class LengthUnit { Unknown, Characters, Bytes, Utf16CodeUnits };
enum class IntervalField { None, Year, Month, Day, Hour, Minute, Second };
enum class IntervalUnit {
  None, Year, Quarter, Month, Week, Day, Hour, Minute, Second, Microsecond,
  YearMonth, DayHour, DayMinute, DaySecond, HourMinute, HourSecond, MinuteSecond,
  DayMicrosecond, HourMicrosecond, MinuteMicrosecond, SecondMicrosecond
};

// Missing attributes mean unknown, not a default silently invented by a consumer.
// Canonical descriptor equality is equality of known facts, not a proof that
// vendor domains, collations, rounding or implicit conversions are equivalent.
struct SqlType {
  TypeFamily family = TypeFamily::Unknown;
  Nullability nullability = Nullability::Unknown;
  std::optional<int> precision, scale, bits;
  std::optional<std::uint64_t> length;
  std::optional<bool> unsignedInteger, nationalCharacter;
  TimeZoneKind timeZone = TimeZoneKind::Unknown;
  LengthUnit lengthUnit = LengthUnit::Unknown;
  IntervalField intervalLeading=IntervalField::None,intervalTrailing=IntervalField::None;
  IntervalUnit intervalUnit=IntervalUnit::None;
  std::optional<int> intervalLeadingPrecision;
  int arrayDimensions = 0;
  bool sqliteAffinity = false;
  std::vector<std::string> opaqueName, opaqueArguments, opaqueModifiers;
  std::vector<std::string> characterSet, collation;
  std::string flavor;
  auto identity() const {
    return std::tie(family,nullability,precision,scale,bits,length,unsignedInteger,
                    nationalCharacter,timeZone,lengthUnit,intervalLeading,intervalTrailing,intervalUnit,intervalLeadingPrecision,arrayDimensions,sqliteAffinity,
                    opaqueName,opaqueArguments,opaqueModifiers,characterSet,collation,flavor);
  }
  bool operator<(const SqlType &other) const { return identity()<other.identity(); }
  bool operator==(const SqlType &other) const { return identity()==other.identity(); }
  bool operator!=(const SqlType &other) const { return !(*this==other); }
};
// Operation-owned arena. IDs survive copies/moves of the owning ModelProgram;
// they must never be exchanged between independent arenas without re-interning.
class TypeContext {
  std::vector<SqlType> types_;
  std::map<SqlType,TypeId> ids_;
  std::size_t bytes_ = 0;
public:
  TypeContext();
  TypeId intern(SqlType, std::size_t limit = 8192);
  TypeId withNullability(TypeId, Nullability, std::size_t limit = 8192);
  const SqlType &get(TypeId) const;
  const std::vector<SqlType> &all() const { return types_; }
  std::size_t size() const { return types_.size(); }
  std::size_t accountedBytes() const { return bytes_; }
};
const char *typeFamilyName(TypeFamily);
const char *nullabilityName(Nullability);
const char *timeZoneName(TimeZoneKind);
const char *lengthUnitName(LengthUnit);
SqlType categoryType(ValueType, Nullability = Nullability::Unknown);
ValueType valueCategory(const SqlType &);
} // namespace navidog::sql::compiler
