#include "generation_rules.hpp"
#include "generation_regex.hpp"
#include <QDateTime>
#include <QRegularExpression>
#include <QTimeZone>
#include <QSet>
#include <algorithm>
#include <cmath>
#include <limits>

namespace navidog::generation {
[[noreturn]] void fail(const QString& message) {
    throw std::runtime_error(message.toStdString());
}

FieldType fieldType(const TableColumn& column) {
    const auto type = column.type.toLower().simplified();
    static const QRegularExpression modifiers(R"rx(\((\d+)(?:\s*,\s*(-?\d+))?(?:\s+(byte|char))?\))rx");
    const auto match = modifiers.match(type);
    FieldType out;
    if (match.hasMatch()) {
        out.width = match.captured(1).toInt();
        out.precision = out.width;
        out.scale = match.captured(2).toInt();
        out.widthInBytes = match.captured(3) == "byte";
    }
    out.name = type.section('(', 0, 0).trimmed();
    out.name.remove(QRegularExpression(" (unsigned|zerofill)$"));
    out.unsignedNumber = type.contains(QRegularExpression("\\bunsigned\\b"));
    out.timeZone = type.contains("with time zone") || out.name == "timestamptz";
    const auto base = out.name.section(' ', 0, 0);
    if (QStringList{"tinyint", "smallint", "mediumint", "int", "integer", "bigint", "int2",
                    "int4", "int8", "serial", "smallserial", "bigserial"}.contains(base))
        out.family = Family::Integer;
    else if (QStringList{"decimal", "numeric", "number", "real", "float", "double",
                         "binary_float", "binary_double", "money", "smallmoney"}.contains(base))
        out.family = Family::Decimal;
    else if (QStringList{"char", "character", "varchar", "varchar2", "nvarchar", "nvarchar2",
                         "nchar", "text", "tinytext", "mediumtext", "longtext", "ntext",
                         "clob", "nclob", "citext", "name"}.contains(base))
        out.family = Family::Text;
    else if (base == "date") out.family = Family::Date;
    else if (base == "time") out.family = Family::Time;
    else if (QStringList{"timestamp", "timestamptz", "datetime", "datetime2", "datetimeoffset",
                         "smalldatetime"}.contains(base))
        out.family = Family::DateTime;
    else if (base == "bool" || base == "boolean" || (base == "bit" && out.width <= 1))
        out.family = Family::Boolean;
    else if (QStringList{"blob", "binary", "varbinary", "bytea", "tinyblob", "mediumblob",
                         "longblob", "raw", "image"}.contains(base))
        out.family = Family::Binary;
    // A MySQL integer display width and a timestamp precision are not text length limits.
    if (out.family != Family::Text && out.family != Family::Binary) out.width = 0;
    return out;
}

bool readOnlyColumn(const TableColumn& column) {
    return column.autoIncrement || !column.generatedExpression.isEmpty() ||
           !column.generation.isEmpty() || column.protectedReason.contains("标识") ||
           column.protectedReason.contains("生成") || column.protectedReason.contains("计算");
}

IntegerBounds integerBounds(const FieldType& type, DatabaseKind driver) {
    if (driver == DatabaseKind::SQLite) return {};
    const auto name = type.name.section(' ', 0, 0);
    int bits = 64;
    if (name == "tinyint") bits = 8;
    else if (name == "smallint" || name == "int2" || name == "smallserial") bits = 16;
    else if (name == "mediumint") bits = 24;
    else if (name == "int" || name == "integer" || name == "int4" || name == "serial") bits = 32;
    if (type.unsignedNumber || (driver == DatabaseKind::SQLServer && name == "tinyint"))
        return {0, bits == 64 ? std::numeric_limits<qint64>::max() : (qint64(1) << bits) - 1};
    if (bits == 64) return {};
    const auto half = qint64(1) << (bits - 1);
    return {-half, half - 1};
}

IntegerBounds checkBounds(const TableDefinition& table, const QString& column, IntegerBounds bounds) {
    const auto escaped = QRegularExpression::escape(column);
    const auto field = QString("(?:%1|\"%1\"|`%1`|\\[%1\\])").arg(escaped);
    const QRegularExpression between("^" + field + "\\s+BETWEEN\\s+(-?\\d+)\\s+AND\\s+(-?\\d+)$",
                                     QRegularExpression::CaseInsensitiveOption);
    const QRegularExpression limit("^" + field + "\\s*(<=|>=|<|>|=)\\s*(-?\\d+)$",
                                   QRegularExpression::CaseInsensitiveOption);
    for (const auto& constraint : table.objects) {
        if (constraint.kind != TableObjectKind::Check || !constraint.enabled) continue;
        auto expression = constraint.expression;
        expression.remove('(');
        expression.remove(')');
        expression = expression.simplified();
        // Only infer bounds from simple conjunctions of literal comparisons.
        if (expression.contains(QRegularExpression("\\b(OR|NOT|CASE)\\b", QRegularExpression::CaseInsensitiveOption)))
            continue;
        const auto full = between.match(expression);
        if (full.hasMatch()) {
            bool okLow = false, okHigh = false;
            const auto low = full.captured(1).toLongLong(&okLow);
            const auto high = full.captured(2).toLongLong(&okHigh);
            if (okLow && okHigh) {
                bounds.min = std::max(bounds.min, low);
                bounds.max = std::min(bounds.max, high);
            }
            continue;
        }
        for (const auto& term : expression.split(QRegularExpression("\\s+AND\\s+", QRegularExpression::CaseInsensitiveOption))) {
            const auto match = limit.match(term);
            if (!match.hasMatch()) continue;
            bool ok = false;
            const auto number = match.captured(2).toLongLong(&ok);
            if (!ok) continue;
            const auto op = match.captured(1);
            if (op == ">=" || op == "=") bounds.min = std::max(bounds.min, number);
            if (op == "<=" || op == "=") bounds.max = std::min(bounds.max, number);
            if (op == ">" && number < std::numeric_limits<qint64>::max())
                bounds.min = std::max(bounds.min, number + 1);
            if (op == "<" && number > std::numeric_limits<qint64>::min())
                bounds.max = std::min(bounds.max, number - 1);
        }
    }
    return bounds;
}

namespace {
enum class Shape { Uniform, Low, High, Bell };
Shape shapeFrom(const QString& value) {
    const auto mode = value.trimmed().toLower();
    if (mode.isEmpty() || mode == "uniform" || mode == "均匀") return Shape::Uniform;
    if (mode == "low" || mode == "偏低") return Shape::Low;
    if (mode == "high" || mode == "偏高") return Shape::High;
    if (mode == "bell" || mode == "钟形") return Shape::Bell;
    fail("分布应为均匀、偏低、偏高、钟形，或填写区间权重");
}

struct Interval {
    qint64 low, high;
    double cumulative;
};
double weight(const QString& text) {
    bool ok = false;
    const auto value = text.toDouble(&ok);
    if (!ok || !std::isfinite(value) || value <= 0 || value > 1.e9)
        fail("权重须大于零且不超过 10 亿");
    return value;
}

qint64 integer(const QString& value) {
    bool ok = false;
    const auto result = value.trimmed().toLongLong(&ok);
    if (!ok) fail("需要有效的 64 位整数");
    return result;
}

int decimals(const FieldType& type) {
    if (type.precision && (type.name == "decimal" || type.name == "numeric" || type.name == "number"))
        return std::clamp(type.scale, 0, 9);
    return 2;
}

qint64 fixed(const QString& value, int scale) {
    static const QRegularExpression syntax(R"rx(^([+-]?)([0-9]+)(?:\.([0-9]+))?$)rx");
    const auto m = syntax.match(value.trimmed());
    if (!m.hasMatch() || m.captured(3).size() > scale) fail(QString("请输入最多 %1 位小数").arg(scale));
    auto digits = m.captured(2) + m.captured(3).leftJustified(scale, '0');
    return integer(m.captured(1) + digits);
}

QString decimalValue(qint64 fixed, int scale) {
    auto text = QString::number(fixed);
    if (!scale) return text;
    const bool negative = text.startsWith('-');
    if (negative) text.remove(0, 1);
    text = text.rightJustified(scale + 1, '0');
    text.insert(text.size() - scale, '.');
    if (negative) text.prepend('-');
    return text;
}

IntegerBounds decimalBounds(const FieldType& type, int scale) {
    IntegerBounds result;
    if (type.unsignedNumber) result.min = 0;
    if (!type.precision || type.scale < 0 ||
        (type.name != "numeric" && type.name != "decimal" && type.name != "number"))
        return result;
    const auto digits = std::clamp(type.precision - type.scale + scale, 0, 19);
    if (digits >= 19) return result;
    qint64 power = 1;
    for (int i = 0; i < digits; ++i) power *= 10;
    result.max = power - 1;
    if (!type.unsignedNumber) result.min = -result.max;
    return result;
}

template<class Parse>
QVector<Interval> ranges(const QString& text, Parse parse, IntegerBounds bounds = {}) {
    const auto parts = text.split(';', Qt::SkipEmptyParts);
    if (parts.isEmpty() || parts.size() > 128) fail("至少输入一个区间，最多 128 个");
    QVector<Interval> result;
    double sum = 0;
    for (const auto& part : parts) {
        const int at = part.lastIndexOf('@');
        if (parts.size() > 1 && at < 0) fail("多个区间需要分别填写 @权重");
        const auto limits = (at < 0 ? part : part.left(at)).split("..");
        if (limits.size() < 1 || limits.size() > 2) fail("区间格式为 起点..终点");
        const qint64 low = parse(limits.front().trimmed());
        const qint64 high = limits.size() == 1 ? low : parse(limits.back().trimmed());
        if (low > high || low < bounds.min || high > bounds.max) fail("区间超出字段类型或 CHECK 的范围，或起点大于终点");
        sum += at < 0 ? 1 : weight(part.mid(at + 1).trimmed());
        result << Interval{low, high, sum};
    }
    return result;
}

template<class T>
const T& weighted(const QVector<T>& entries, Random& random) {
    const auto draw = std::uniform_real_distribution<double>(0, entries.back().cumulative)(random);
    const auto entry = std::upper_bound(entries.begin(), entries.end(), draw,
                                         [](double x, const T& candidate) { return x < candidate.cumulative; });
    return entry == entries.end() ? entries.back() : *entry;
}
qint64 sample(const QVector<Interval>& intervals, Shape shape, Random& engine) {
    const auto& interval = weighted(intervals, engine);
    auto values = std::uniform_int_distribution<qint64>(interval.low, interval.high);
    const auto a = values(engine);
    if (shape == Shape::Uniform) return a;
    const auto b = values(engine);
    if (shape == Shape::Low) return std::min(a, b);
    if (shape == Shape::High) return std::max(a, b);
    const auto average = (static_cast<long double>(a) + b + values(engine)) / 3;
    if (average <= interval.low) return interval.low;
    if (average >= interval.high) return interval.high;
    return static_cast<qint64>(average);
}

struct Choice {
    QVariant value;
    double cumulative = 0;
};
QVariant scalar(const QString& text, const FieldType& type, DatabaseKind driver) {
    if (type.family == Family::Integer) {
        // Accept native unsigned BIGINT constants through the server's textual binder.
        if (type.unsignedNumber && type.name.startsWith("bigint")) {
            bool ok = false;
            auto unsignedValue = text.toULongLong(&ok);
            if (ok && unsignedValue > quint64(std::numeric_limits<qint64>::max()) && !text.startsWith('-'))
                return QString::number(unsignedValue);
        }
        const auto n = integer(text);
        const auto bounds = integerBounds(type, driver);
        if (n < bounds.min || n > bounds.max) fail("整数超出字段类型范围");
        return QVariant::fromValue(n);
    }
    if (type.family == Family::Boolean) {
        if (text.compare("true", Qt::CaseInsensitive) == 0 || text == "1") return true;
        if (text.compare("false", Qt::CaseInsensitive) == 0 || text == "0") return false;
        fail("布尔值应为 true、false、1 或 0");
    }
    if (type.family == Family::Binary) {
        if (!text.startsWith("hex:") || (text.size() - 4) % 2 != 0 ||
            text.mid(4).contains(QRegularExpression("[^0-9a-fA-F]")))
            fail("二进制固定值用 hex: 开头，后接成对的十六进制字符");
        return QByteArray::fromHex(text.mid(4).toLatin1());
    }
    return text.isNull() ? QStringLiteral("") : text;
}
QVector<Choice> choices(const QString& text, const FieldType& type, DatabaseKind driver) {
    const auto parts = text.split(';', Qt::SkipEmptyParts);
    if (parts.isEmpty() || parts.size() > 256) fail("填写 1 至 256 个候选值");
    double total = 0;
    QVector<Choice> values;
    for (const auto& part : parts) {
        const int at = part.lastIndexOf('@');
        total += at < 0 ? 1 : weight(part.mid(at + 1).trimmed());
        values << Choice{scalar((at < 0 ? part : part.left(at)).trimmed(), type, driver), total};
    }
    return values;
}

QDateTime utc(const QString& text) {
    const auto date = QDate::fromString(text, Qt::ISODate);
    if (date.isValid()) return QDateTime(date, QTime(0, 0), QTimeZone::utc());
    auto dateTime = QDateTime::fromString(text, Qt::ISODate);
    if (!dateTime.isValid()) fail("日期时间需使用 ISO 格式，如 2026-01-10T22:30:00Z");
    if (dateTime.timeSpec() == Qt::LocalTime)
        return QDateTime(dateTime.date(), dateTime.time(), QTimeZone::utc());
    return dateTime.toUTC();
}

Rule temporal(const QString& text, const QString& distribution, const FieldType& type) {
    const bool hourly = distribution.contains('@') || distribution.contains("..");
    const auto shape = hourly ? Shape::Uniform : shapeFrom(distribution);
    if (type.family == Family::Date) {
        if (hourly) fail("DATE 字段不能设置小时权重");
        const auto days = ranges(text, [](const QString& s) {
            const auto date = QDate::fromString(s, Qt::ISODate);
            if (!date.isValid()) fail("日期格式为 YYYY-MM-DD");
            return date.toJulianDay();
        });
        return {"temporal", [days, shape](qint64, Random& random) {
            return QDate::fromJulianDay(sample(days, shape, random)).toString(Qt::ISODate);
        }, {}};
    }
    if (type.family == Family::Time) {
        if (hourly) {
            if (text.trimmed() != "00:00:00..23:59:59")
                fail("小时权重需要完整时间范围 00:00:00..23:59:59");
            const auto hours = ranges(distribution, integer, {0, 23});
            return {"temporal", [hours](qint64, Random& random) {
                const auto hour = int(sample(hours, Shape::Uniform, random));
                return QTime(hour, std::uniform_int_distribution<int>(0, 59)(random),
                             std::uniform_int_distribution<int>(0, 59)(random)).toString("HH:mm:ss");
            }, {}};
        }
        const auto seconds = ranges(text, [](const QString& s) {
            const auto time = QTime::fromString(s, "HH:mm:ss");
            if (!time.isValid()) fail("时间格式为 HH:mm:ss");
            return qint64(QTime(0, 0).secsTo(time));
        });
        return {"temporal", [seconds, shape](qint64, Random& random) {
            return QTime(0, 0).addSecs(int(sample(seconds, shape, random))).toString("HH:mm:ss");
        }, {}};
    }
    if (type.family != Family::DateTime) fail("时间分布需要 DATE、TIME 或日期时间字段");
    const auto parts = text.split("..");
    if (parts.size() != 2) fail("日期时间范围需填写 起点..终点");
    const auto first = utc(parts.front().trimmed());
    auto end = utc(parts.back().trimmed());
    const auto dateOnly = [](const QString& value) { return QDate::fromString(value.trimmed(), Qt::ISODate).isValid(); };
    if (dateOnly(parts.back())) end = end.addSecs(86399);
    if (first > end) fail("时间范围的起点应早于或等于终点");
    if (hourly && (!dateOnly(parts.front()) || !dateOnly(parts.back())))
        fail("设置小时权重时，请用完整自然日范围 YYYY-MM-DD..YYYY-MM-DD");
    const QVector<Interval> seconds{{first.toSecsSinceEpoch(), end.toSecsSinceEpoch(), 1}};
    const auto hours = hourly ? ranges(distribution, integer, {0, 23}) : QVector<Interval>{};
    const auto countDays = first.date().daysTo(end.date());
    return {"temporal", [=](qint64, Random& random) {
        auto instant = QDateTime::fromSecsSinceEpoch(sample(seconds, shape, random), QTimeZone::utc());
        if (hourly) {
            const auto date = first.date().addDays(std::uniform_int_distribution<qint64>(0, countDays)(random));
            const auto hour = int(sample(hours, Shape::Uniform, random));
            instant = QDateTime(date, QTime(hour, 0).addSecs(std::uniform_int_distribution<int>(0, 3599)(random)), QTimeZone::utc());
        }
        auto value = instant.toString("yyyy-MM-dd HH:mm:ss");
        if (type.timeZone) value += "+00:00";
        return value;
    }, {}};
}
}

QString numericRuleExample(const FieldType& type) {
    if (type.family != Family::Decimal) return "1..100";
    const int scale = decimals(type);
    const auto limit = decimalBounds(type, scale).max;
    qint64 unit = 1;
    for (int i = 0; i < scale; ++i) unit *= 10;
    return decimalValue(std::min(unit, limit), scale) + ".." +
           decimalValue(std::min(100 * unit, limit), scale);
}

Rule prepareRule(const TableColumn& column, const FieldType& type, DatabaseKind driver,
                 IntegerBounds bounds, const QJsonObject& config, std::optional<qint64> nextId) {
    const auto kind = config.value("kind").toString("default");
    const auto value = config.value("value").toString();
    const auto distribution = config.value("distribution").toString();
    if (value.size() > 32768 || distribution.size() > 4096) fail("字段规则的参数过长");
    if (kind == "default") return {kind, {}, {}};
    if (readOnlyColumn(column)) fail("标识或计算字段需使用数据库默认值");
    if (kind == "foreign") return {kind, {}, {}};
    if (kind == "null") {
        if (!column.nullable || column.primaryKey) fail("字段不允许 NULL");
        return {kind, [](qint64, Random&) { return QVariant{}; }, {}};
    }
    if (kind == "constant") {
        const auto constant = scalar(value, type, driver);
        return {kind, [constant](qint64, Random&) { return constant; }, {}};
    }
    if (kind == "sequence") {
        if (type.family != Family::Integer && type.family != Family::Decimal) fail("递增整数需要数值字段");
        const auto start = nextId ? *nextId : integer(value);
        if (start < bounds.min || start > bounds.max) fail("起始值超出字段类型范围");
        return {kind, [start, bounds](qint64 row, Random&) {
            if (start > std::numeric_limits<qint64>::max() - row || start + row > bounds.max)
                fail("递增值超出字段类型范围");
            return QVariant::fromValue(start + row);
        }, start};
    }
    if (kind == "series") {
        if (type.family != Family::Text) fail("文本加序号需要字符字段");
        return {kind, [value](qint64 row, Random&) { return value + QString::number(quint64(row) + 1); }, {}};
    }
    if (kind == "number") {
        if (type.family != Family::Integer && type.family != Family::Decimal)
            fail("数值分布需要整数或数值字段");
        const auto shape = shapeFrom(distribution);
        if (type.family == Family::Integer) {
            const auto bins = ranges(value, integer, bounds);
            return {kind, [bins, shape](qint64, Random& random) {
                return QVariant::fromValue(sample(bins, shape, random));
            }, {}};
        }
        const int scale = decimals(type);
        const auto bins = ranges(value, [scale](const QString& s) { return fixed(s, scale); },
                                 decimalBounds(type, scale));
        return {kind, [bins, scale, shape](qint64, Random& random) {
            return decimalValue(sample(bins, shape, random), scale);
        }, {}};
    }
    if (kind == "choice") {
        const auto entries = choices(value, type, driver);
        return {kind, [entries](qint64, Random& random) { return weighted(entries, random).value; }, {}};
    }
    if (kind == "temporal") return temporal(value, distribution, type);
    if (kind == "regex") {
        if (type.family != Family::Text) fail("正则生成需要字符字段");
        const Regex pattern(value);
        return {kind, [pattern, type](qint64, Random& random) {
            return pattern.generate(random, type.width > 0 ? type.width : 8192);
        }, {}};
    }
    if (kind == "name") {
        if (type.family != Family::Text) fail("姓名生成需要字符字段");
        const bool chinese = value == "中文" || value.compare("zh", Qt::CaseInsensitive) == 0;
        if (!chinese && value != "英文" && value.compare("en", Qt::CaseInsensitive) != 0)
            fail("姓名参数可填中文或英文");
        return {kind, [chinese](qint64, Random& engine) {
            static const QStringList lastZh{"陈", "林", "周", "张", "王", "徐", "黄", "李", "杨", "郑", "刘", "喻"};
            static const QStringList firstZh{"可欣", "若宁", "子安", "欣然", "文远", "思雨", "明杰", "佳宜", "晓悦", "涵", "浩", "婧"};
            static const QStringList lastEn{"Smith", "Walker", "Brown", "Green", "Clark", "Lopez", "Wilson", "Patel"};
            static const QStringList firstEn{"Alex", "Jordan", "Sam", "Taylor", "Drew", "Casey", "Morgan", "Riley"};
            const auto choose = [&](const QStringList& names) {
                return names.at(std::uniform_int_distribution<int>(0, int(names.size()) - 1)(engine));
            };
            if (chinese) return choose(lastZh) + choose(firstZh);
            return choose(firstEn) + " " + choose(lastEn);
        }, {}};
    }
    fail("未知的生成规则");
}
}
