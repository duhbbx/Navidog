#pragma once
#include "navidog/data_generation.hpp"
#include <limits>
#include <optional>
#include <random>

namespace navidog::generation {
using Random = std::mt19937_64;

enum class Family { Integer, Decimal, Text, Date, Time, DateTime, Boolean, Binary, Other };
struct FieldType {
    Family family = Family::Other;
    QString name;
    int width = 0;
    int precision = 0;
    int scale = 0;
    bool widthInBytes = false;
    bool unsignedNumber = false;
    bool timeZone = false;
};

struct Rule {
    QString kind;
    std::function<QVariant(qint64, Random&)> generate;
    std::optional<qint64> sequenceStart;
};

struct IntegerBounds {
    qint64 min = std::numeric_limits<qint64>::min();
    qint64 max = std::numeric_limits<qint64>::max();
};

[[noreturn]] void fail(const QString&);
FieldType fieldType(const TableColumn&);
bool readOnlyColumn(const TableColumn&);
QString numericRuleExample(const FieldType&);
IntegerBounds integerBounds(const FieldType&, DatabaseKind);
IntegerBounds checkBounds(const TableDefinition&, const QString& column, IntegerBounds);
Rule prepareRule(const TableColumn&, const FieldType&, DatabaseKind, IntegerBounds, const QJsonObject&,
                 std::optional<qint64> nextId = {});
}
