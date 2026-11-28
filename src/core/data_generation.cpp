#include "data_generation_internal.hpp"
#include "generation_rules.hpp"
#include "navidog/data_edit.hpp"
#include <QDate>
#include <QElapsedTimer>
#include <QLocale>
#include <QMap>
#include <QRegularExpression>
#include <QSet>
#include <QTextCodec>

#include <algorithm>
#include <cmath>
#include <limits>

namespace navidog {
namespace {
using K = DatabaseKind;
using generation::Family;
using generation::fieldType;

const QVector<QPair<QString, QString>>& ruleOptions() {
    static const QVector<QPair<QString, QString>> choices{
        {"default", "数据库默认值"}, {"sequence", "递增整数"}, {"number", "数值分布"},
        {"name", "随机姓名"}, {"regex", "正则文本"}, {"temporal", "日期 / 时间分布"},
        {"choice", "加权候选值"}, {"foreign", "引用已有外键"}, {"series", "文本加序号"},
        {"constant", "固定值"}, {"null", "NULL"}
    };
    return choices;
}

QString integerRange(qint64 low, qint64 high) {
    return QString::number(low) + ".." + QString::number(high);
}
bool sqliteRowId(const TableDefinition& table, const TableColumn& column, K kind) {
    return kind == K::SQLite && !table.withoutRowid && column.primaryKey == 1 &&
           column.type.compare("INTEGER", Qt::CaseInsensitive) == 0 &&
           primaryKeyColumns(table).size() == 1;
}
}

QStringList generationRuleLabels() {
    QStringList out;
    for (const auto& option : ruleOptions()) out << option.second;
    return out;
}

QStringList generationRuleKeys() {
    QStringList out;
    for (const auto& option : ruleOptions()) out << option.first;
    return out;
}

QString generationRuleExample(const QString& kind, const TableColumn& column) {
    const auto type = fieldType(column);
    if (kind == "sequence") return "auto";
    if (kind == "regex") return type.width == 1 ? "[A-Z]" : "[A-Z0-9]{2,8}";
    if (kind == "name") return "中文";
    if (kind == "number") {
        const auto name = column.name.toLower();
        return type.family == Family::Integer && (name == "age" || name == "年龄")
            ? QString("18..80") : generation::numericRuleExample(type);
    }
    if (kind == "temporal") {
        if (type.family == Family::Time) return "00:00:00..23:59:59";
        const auto end = QDate::currentDate();
        return end.addDays(-29).toString(Qt::ISODate) + ".." + end.toString(Qt::ISODate);
    }
    if (kind == "choice") {
        if (type.family == Family::Boolean) return "true@50;false@50";
        if (type.family == Family::Integer || type.family == Family::Decimal) return "1@70;2@30";
        return "A@70;B@30";
    }
    if (kind == "series") return !type.width || type.width >= 8 ? "sample_" : QString{};
    if (kind == "constant") {
        if (type.family == Family::Binary) return "hex:00";
        if (type.family == Family::Boolean) return "true";
        if (type.family == Family::Date) return QDate::currentDate().toString(Qt::ISODate);
        if (type.family == Family::Time) return "12:00:00";
        if (type.family == Family::DateTime) return QDate::currentDate().toString(Qt::ISODate) + " 12:00:00";
        if (type.family == Family::Integer || type.family == Family::Decimal) return "1";
        return type.width && type.width < 6 ? QString("a") : QString("sample");
    }
    return {};
}

QJsonObject suggestedGenerationRules(const TableDefinition& table, K kind) {
    QSet<QString> foreign;
    for (const auto& constraint : table.objects)
        if (constraint.kind == TableObjectKind::ForeignKey &&
            (constraint.enabled || usesNativePostgres(kind)))
            for (const auto& column : constraint.columns) foreign.insert(column.toCaseFolded());
    QJsonObject result;
    for (const auto& column : table.columns) {
        const auto type = fieldType(column);
        const auto name = column.name.toLower();
        const bool defaultValue = generation::readOnlyColumn(column) ||
                                  sqliteRowId(table, column, kind) || !column.defaultExpression.isEmpty();
        QString rule = "default";
        if (foreign.contains(name) && !generation::readOnlyColumn(column)) rule = "foreign";
        else if (!defaultValue) {
            if (type.family == Family::Integer && (column.primaryKey || name == "id")) rule = "sequence";
            else if (type.family == Family::Integer || type.family == Family::Decimal) rule = "number";
            else if (type.family == Family::Date || type.family == Family::Time || type.family == Family::DateTime) rule = "temporal";
            else if (type.family == Family::Boolean) rule = "choice";
            else if (type.family == Family::Text) {
                if (column.primaryKey) rule = "series";
                else if ((name == "name" || name == "full_name" || name == "姓名") &&
                         (!type.width || type.width >= 3)) rule = "name";
                else rule = "regex";
            } else if (column.nullable) rule = "null";
        }
        auto value = generationRuleExample(rule, column);
        if (rule == "name" && isMySqlFamily(kind)) {
            const auto charset = column.collation.isEmpty() ? table.charset.toLower()
                : column.collation.section('_', 0, 0).toLower();
            if (!charset.isEmpty() && !charset.startsWith("utf") && !charset.startsWith("gb")) {
                value = "英文";
                if (type.width && type.width < 14) {
                    rule = "regex";
                    value = generationRuleExample(rule, column);
                }
            }
        }
        if (rule == "number" && type.family == Family::Integer) {
            const auto bounds = generation::checkBounds(table, column.name, generation::integerBounds(type, kind));
            const qint64 low = (name == "age" || name == "年龄") ? 18 : 1;
            const qint64 high = low == 18 ? 80 : 100;
            if (bounds.min <= bounds.max)
                value = integerRange(std::clamp(low, bounds.min, bounds.max),
                                     std::clamp(high, bounds.min, bounds.max));
        }
        result.insert(column.name, QJsonObject{{"kind", rule}, {"value", value}});
    }
    return result;
}

namespace generation {
namespace {
constexpr int referencePoolLimit = 4096;
constexpr qsizetype referenceBytesLimit = 24 * 1024 * 1024;
constexpr qsizetype batchBytesLimit = 1024 * 1024;
constexpr qsizetype previewBytesLimit = 256 * 1024;
constexpr qsizetype uniqueBytesLimit = 32 * 1024 * 1024;

QueryResult execute(Database& db, const QString& sql, const Cancellation& cancel, QueryLimits limits = {},
                    const QVector<QVariant>& parameters = {}) {
    if (cancel->load()) fail("操作已停止");
    const auto result = parameters.isEmpty() ? db.execute(sql, limits, cancel)
                                            : db.executeBound(sql, parameters, limits, cancel);
    if (result.cancelled) fail("操作已停止");
    if (!result.error.isEmpty()) fail(result.error);
    if (result.cellTruncated) fail("外键或元数据的单元格超过可完整读取的大小");
    return result;
}

QString dbCharset(Database& db, K driver, const TableDefinition& table, const Cancellation& cancel) {
    if (!table.charset.isEmpty()) return table.charset.toLower();
    QString sql;
    if (driver == K::SQLite) sql = "PRAGMA encoding";
    else if (usesNativePostgres(driver)) sql = "SHOW server_encoding";
    else if (driver == K::Oracle) sql = "SELECT VALUE FROM NLS_DATABASE_PARAMETERS WHERE PARAMETER='NLS_CHARACTERSET'";
    else if (isMySqlFamily(driver)) sql = "SELECT @@character_set_database";
    if (sql.isEmpty()) return {};
    const auto result = execute(db, sql, cancel);
    return result.rows.isEmpty() ? QString{} : result.rows.front().front().toString().toLower();
}

QString codecName(const QString& charset) {
    if (charset == "utf8mb4" || charset == "utf8mb3" || charset == "utf8" || charset == "al32utf8")
        return "UTF-8";
    if (charset == "latin1" || charset == "we8mswin1252" || charset == "win1252") return "Windows-1252";
    if (charset == "sql_ascii" || charset == "us7ascii") return "US-ASCII";
    if (charset == "utf16" || charset == "ucs2") return "UTF-16BE";
    if (charset == "utf16le") return "UTF-16LE";
    if (charset == "sjis") return "Shift_JIS";
    return charset;
}

struct Field {
    const TableColumn* definition = nullptr;
    FieldType type;
    Rule rule;
    QTextCodec* codec = nullptr;
    QString charset;
    IntegerBounds bounds;
    bool omitted = false;
    bool foreignUnavailable = false;
    bool unique = false;
    QSet<QString> seen;
};
struct ForeignPool {
    QVector<int> fields;
    QVector<QVector<QVariant>> rows;
    QString name, reference;
    bool unique = false;
};

int fieldIndex(const TableDefinition& table, const QString& name, K driver) {
    const auto sensitivity = driver == K::SQLite || isMySqlFamily(driver) || driver == K::SQLServer
        ? Qt::CaseInsensitive : Qt::CaseSensitive;
    for (int i = 0; i < table.columns.size(); ++i)
        if (table.columns[i].name.compare(name, sensitivity) == 0) return i;
    return -1;
}

QVector<TableObject> readForeignKeys(Database& db, const ConnectionProfile& p, const TableDefinition& table,
                                    const Cancellation& cancel, const std::function<QString(const QVariant&, K)>& literal) {
    const auto driver = p.driver;
    QString sql;
    const auto quoted = [&](const QString& value) { return literal(value, driver); };
    if (driver == K::SQLite) {
        const auto schema = table.node.schema.isEmpty() ? QString("main") : table.node.schema;
        sql = "SELECT id,[from]," + quoted(schema) + ",[table],[to] FROM pragma_foreign_key_list(" +
              quoted(table.node.object) + "," + quoted(schema) + ") ORDER BY id,seq";
    } else if (usesNativePostgres(driver) && !isDistributedPostgres(driver)) {
        sql = "SELECT c.conname,a.attname,n.nspname,r.relname,b.attname FROM pg_constraint c"
              " JOIN pg_class r ON r.oid=c.confrelid JOIN pg_namespace n ON n.oid=r.relnamespace"
              " JOIN LATERAL generate_subscripts(c.conkey,1) AS ord(i) ON true"
              " JOIN pg_attribute a ON a.attrelid=c.conrelid AND a.attnum=c.conkey[ord.i]"
              " JOIN pg_attribute b ON b.attrelid=c.confrelid AND b.attnum=c.confkey[ord.i]"
              " WHERE c.contype='f' AND c.conrelid=" + quoted(tableQualifiedName(p, table.node)) +
              "::regclass ORDER BY c.conname,ord.i";
    } else {
        QVector<TableObject> out;
        for (const auto& object : table.objects)
            if (object.kind == TableObjectKind::ForeignKey &&
                (object.enabled || usesNativePostgres(driver))) out << object;
        return out;
    }
    QueryLimits limits;
    limits.maxRows = 10000;
    const auto records = execute(db, sql, cancel, limits);
    if (records.truncated) fail("外键元数据未完整读取");
    QVector<TableObject> out;
    for (const auto& row : records.rows) {
        const auto key = row[0].toString();
        if (out.isEmpty() || out.back().name != key) {
            TableObject object;
            object.kind = TableObjectKind::ForeignKey;
            object.name = driver == K::SQLite ? key : row[0].toString();
            object.referenceSchema = row[2].toString();
            object.referenceTable = row[3].toString();
            out << object;
        }
        out.back().columns << row[1].toString();
        out.back().referenceColumns << row[4].toString();
    }
    return out;
}

QVector<int> indexFields(const TableDefinition& table, const TableObject& object, K driver) {
    QStringList columns = object.columns;
    if (columns.isEmpty()) {
        try { columns = parseIdentifierList(object.expression); }
        catch (const std::exception&) { return {}; }
    }
    QVector<int> fields;
    for (const auto& column : columns) {
        const auto field = fieldIndex(table, column, driver);
        if (field < 0) return {};
        fields << field;
    }
    return fields;
}

QVector<QVector<int>> uniqueKeys(const TableDefinition& table, K driver) {
    QVector<QVector<int>> out;
    const auto primary = primaryKeyColumns(table);
    if (!primary.isEmpty()) out << primary;
    for (const auto& object : table.objects) {
        if (object.kind != TableObjectKind::Index || !object.unique || !object.predicate.isEmpty()) continue;
        const auto fields = indexFields(table, object, driver);
        if (!fields.isEmpty() && !out.contains(fields)) out << fields;
    }
    return out;
}

QVector<QVector<int>> estimatedIndexes(const TableDefinition& table, K driver, int& skipped) {
    QVector<QVector<int>> out;
    const auto primary = primaryKeyColumns(table);
    if (!primary.isEmpty() && !isMySqlFamily(driver) &&
        !(driver == K::SQLite && (table.withoutRowid || sqliteRowId(table, table.columns[primary.front()], driver))))
        out << primary;
    for (const auto& object : table.objects) {
        if (object.kind != TableObjectKind::Index) continue;
        const auto fields = indexFields(table, object, driver);
        if (!object.predicate.isEmpty() || fields.isEmpty() ||
            (!object.method.isEmpty() && object.method.compare("btree", Qt::CaseInsensitive) != 0)) {
            ++skipped;
            continue;
        }
        out << fields;
    }
    return out;
}
bool containsUniqueKey(const QVector<QVector<int>>& keys, const QVector<int>& fields) {
    return std::any_of(keys.begin(), keys.end(), [&](const auto& key) {
        return std::all_of(key.begin(), key.end(), [&](int index) { return fields.contains(index); });
    });
}

QVector<ForeignPool> foreignPools(Database& db, const ConnectionProfile& p, const TableDefinition& table,
                                 const QVector<TableObject>& constraints, const QVector<QVector<int>>& keys,
                                 QVector<Field>& fields, bool preview, const Cancellation& cancel) {
    QVector<ForeignPool> pools;
    QSet<int> used;
    qsizetype bytesLeft = referenceBytesLimit;
    const auto driver = p.driver;
    const auto identifier = [&](const QString& field) { return quoteIdentifier(field, driver); };
    for (const auto& constraint : constraints) {
        QVector<int> members;
        for (const auto& column : constraint.columns) {
            const int field = fieldIndex(table, column, driver);
            if (field < 0) fail("外键字段元数据不完整：" + column);
            members << field;
        }
        if (std::none_of(members.begin(), members.end(), [&](int index) { return fields[index].rule.kind == "foreign"; }))
            continue;
        if (std::any_of(members.begin(), members.end(), [&](int index) { return fields[index].rule.kind != "foreign"; }))
            fail("复合外键所有字段都需同时选择引用已有外键：" + constraint.columns.join(", "));
        if (std::any_of(members.begin(), members.end(), [&](int index) { return used.contains(index); }))
            fail("同一字段参与多个自动外键，请为重叠字段指定一致的固定值：" + constraint.columns.join(", "));
        for (const int field : members) used.insert(field);
        if (constraint.referenceTable.isEmpty()) fail("外键的目标表不可见");
        auto reference = table.node;
        reference.kind = "table";
        reference.object = constraint.referenceTable;
        if (!constraint.referenceSchema.isEmpty()) {
            if (isMySqlFamily(driver)) reference.database = constraint.referenceSchema;
            else reference.schema = constraint.referenceSchema;
        }
        auto references = constraint.referenceColumns;
        if (references.isEmpty() || std::any_of(references.begin(), references.end(), [](const auto& s) { return s.isEmpty(); })) {
            const auto parent = readTableDefinition(db, p, reference, cancel, false);
            references.clear();
            for (const int key : primaryKeyColumns(parent)) references << parent.columns[key].name;
        }
        if (references.size() != members.size()) fail("外键的目标列数和本地列数不一致：" + constraint.name);
        QStringList columns, predicates;
        for (const auto& ref : references) {
            const auto key = "parent_key." + identifier(ref);
            columns << key;
            predicates << key + " IS NOT NULL";
        }
        ForeignPool pool;
        pool.fields = members;
        pool.name = constraint.name;
        pool.reference = tableQualifiedName(p, reference);
        // Each UNIQUE subset of the local columns must be new, and must
        // stay unique even when the corresponding parent tuple is wider.
        QVector<QVector<int>> uniqueParts;
        QStringList keyPredicates;
        for (const auto& key : keys) {
            QVector<int> positions;
            QStringList comparisons;
            for (const auto index : key) {
                const int at = members.indexOf(index);
                if (at < 0) break;
                positions << at;
                comparisons << "existing_row." + identifier(fields[index].definition->name) + " = " + columns[at];
            }
            if (positions.size() == key.size()) {
                uniqueParts << std::move(positions);
                keyPredicates << "(" + comparisons.join(" AND ") + ")";
            }
        }
        pool.unique = !uniqueParts.isEmpty();
        if (pool.unique)
            predicates << "NOT EXISTS (SELECT 1 FROM " + tableQualifiedName(p, table.node) +
                          " existing_row WHERE " + keyPredicates.join(" OR ") + ")";
        const auto limit = QString::number(referencePoolLimit);
        const auto prefix = driver == K::SQLServer ? "SELECT TOP (" + limit + ") " : QString("SELECT ");
        auto sql = prefix + columns.join(", ") + " FROM " + tableQualifiedName(p, reference) +
                   " parent_key WHERE " + predicates.join(" AND ") + " ORDER BY " + columns.join(", ");
        if (driver == K::Oracle)
            sql = "SELECT * FROM (" + sql + ") WHERE ROWNUM <= " + limit;
        else if (driver != K::SQLServer) sql += " LIMIT " + limit;
        if (bytesLeft < 1024) fail("外键采样超过总内存上限");
        QueryLimits limits;
        limits.maxRows = referencePoolLimit;
        limits.maxBytes = std::min<qsizetype>(4 * 1024 * 1024, bytesLeft);
        limits.rejectInvalidTextEncoding = true;
        auto result = execute(db, sql, cancel, limits);
        bytesLeft -= result.cachedBytes;
        if (pool.unique) {
            QVector<QSet<QString>> seen(uniqueParts.size());
            for (auto& row : result.rows) {
                QStringList rowKeys;
                bool duplicate = false;
                for (int i = 0; i < uniqueParts.size(); ++i) {
                    QString key;
                    for (const int column : uniqueParts[i]) {
                        const auto value = row[column].toString();
                        key += QString::number(value.size()) + ':' + value;
                    }
                    if (seen[i].contains(key)) {
                        duplicate = true;
                        break;
                    }
                    rowKeys << std::move(key);
                }
                if (duplicate) continue;
                for (int i = 0; i < rowKeys.size(); ++i) seen[i].insert(std::move(rowKeys[i]));
                pool.rows << std::move(row);
            }
        } else pool.rows = std::move(result.rows);
        if (pool.rows.isEmpty()) {
            if (!preview)
                fail("目标表没有可用引用键，请先填充父表或修改外键规则：" + pool.reference);
            for (const int index : members) fields[index].foreignUnavailable = true;
        }
        pools << std::move(pool);
    }
    for (int i = 0; i < fields.size(); ++i)
        if (fields[i].rule.kind == "foreign" && !used.contains(i))
            fail("字段没有可用的外键元数据：" + fields[i].definition->name);
    return pools;
}

QTextCodec* storageCodec(const Field& field) {
    if (field.codec) return field.codec;
    return QTextCodec::codecForName("UTF-8");
}
int textBytes(const QString& value, const Field& field) {
    if (field.charset == "utf8mb3" || field.charset == "ucs2")
        for (const QChar ch : value) if (ch.isHighSurrogate()) fail("字段字符集不支持四字节 Unicode 字符");
    QTextCodec::ConverterState state(QTextCodec::IgnoreHeader);
    const auto bytes = storageCodec(field)->fromUnicode(value.constData(), value.size(), &state);
    if (state.invalidChars) fail("字段字符集 " + field.charset + " 无法表示生成内容");
    return int(bytes.size());
}

int columnCharacters(const QString& value) {
    int characters = 0;
    for (const QChar ch : value)
        if (!ch.isLowSurrogate()) ++characters;
    return characters;
}
void validateValue(const Field& field, const QVariant& value, K kind) {
    try {
        if (!value.isValid() || value.isNull()) {
            if (!field.definition->nullable || field.definition->primaryKey) fail("字段不允许 NULL");
            return;
        }
        if (field.type.family == Family::Text) {
            const auto text = value.toString();
            const int bytes = textBytes(text, field);
            if (bytes > 65536) fail("文本超过每个生成字段 64 KiB 上限");
            const auto limit = field.type.width;
            auto count = field.type.widthInBytes ? bytes : columnCharacters(text);
            if (kind == K::SQLServer && field.type.name.startsWith('n')) count = int(text.size());
            if (kind == K::Oracle && text.isEmpty() && !field.definition->nullable)
                fail("Oracle 将空字符串视为 NULL");
            if (kind != K::SQLite && limit && count > limit)
                fail(QString("生成内容长度 %1 超出声明长度 %2").arg(count).arg(limit));
        } else if (field.type.family == Family::Integer) {
            bool ok = false;
            const auto n = value.toLongLong(&ok);
            if (ok && (n < field.bounds.min || n > field.bounds.max))
                fail("整数超出声明类型或简单 CHECK 的范围");
        } else if (field.type.family == Family::Binary && value.userType() == QMetaType::QByteArray &&
                   field.type.width && kind != K::SQLite && value.toByteArray().size() > field.type.width)
            fail("二进制内容超出字段长度");
    } catch (const std::exception& e) {
        fail(field.definition->name + "：" + QString::fromUtf8(e.what()));
    }
}

int typedBytes(const Field& field, const QVariant& value, K driver) {
    if (!field.omitted && !field.foreignUnavailable && (!value.isValid() || value.isNull())) return 0;
    const auto& type = field.type;
    if (type.family == Family::Boolean) return 1;
    if (type.family == Family::Integer) {
        if (driver == K::SQLite && !field.omitted) {
            const auto n = value.toLongLong();
            if (n == 0 || n == 1) return 0;
            for (const int bytes : {1, 2, 3, 4, 6}) {
                const auto half = qint64(1) << (bytes * 8 - 1);
                if (n >= -half && n < half) return bytes;
            }
            return 8;
        }
        if (type.name == "tinyint") return 1;
        if (type.name == "smallint" || type.name == "int2" || type.name == "smallserial") return 2;
        if (type.name == "mediumint") return 3;
        if (type.name == "bigint" || type.name == "int8" || type.name == "bigserial" || driver == K::SQLite)
            return 8;
        return 4;
    }
    if (type.family == Family::Date || type.family == Family::DateTime || type.family == Family::Time) {
        if (driver == K::SQLite && !field.omitted) return textBytes(value.toString(), field);
        return type.family == Family::Date ? 4 : 8;
    }
    if (type.family == Family::Decimal) {
        if (type.precision && (type.name == "decimal" || type.name == "numeric" || type.name == "number")) {
            if (isMySqlFamily(driver)) {
                const auto groupBytes = [](int digits) { return (digits / 9) * 4 + ((digits % 9) + 1) / 2; };
                const int scale = std::clamp(type.scale, 0, type.precision);
                return groupBytes(type.precision - scale) + groupBytes(scale);
            }
            return 4 + (type.precision + 3) / 4 * 2;
        }
        return type.name == "float" || type.name == "real" ? 4 : 8;
    }
    if (type.name == "uuid" || type.name == "uniqueidentifier") return 16;
    if (field.omitted || field.foreignUnavailable) {
        // Server-owned defaults and computed values have no local sample.
        if (!field.foreignUnavailable && field.definition->nullable && field.definition->defaultExpression.isEmpty() &&
            field.definition->generatedExpression.isEmpty()) return 0;
        return std::min(32, type.width ? type.width : 32) * (field.charset.startsWith("utf-16") ? 2 : 1) + 2;
    }
    int bytes = value.userType() == QMetaType::QByteArray ? int(value.toByteArray().size()) :
                textBytes(value.toString(), field);
    const bool fixed = type.name == "char" || type.name == "character" || type.name == "nchar" || type.name == "binary";
    if (fixed && type.width > 0)
        bytes += std::max(0, type.width - columnCharacters(value.toString())) *
                 (field.charset.startsWith("utf-16") || field.charset == "utf16le" ? 2 : 1);
    if (isMySqlFamily(driver)) {
        if (fixed) return bytes;
        if (type.name == "tinytext" || type.name == "tinyblob") return bytes + 1;
        if (type.name == "mediumtext" || type.name == "mediumblob") return bytes + 3;
        if (type.name == "longtext" || type.name == "longblob") return bytes + 4;
        if (type.width && (type.name == "varchar" || type.name == "varbinary")) {
            const int width = field.charset == "utf8mb4" ? 4 : field.charset.startsWith("utf8") ? 3 :
                              field.charset == "latin1" || type.name == "varbinary" ? 1 : 4;
            return bytes + (qint64(type.width) * width <= 255 ? 1 : 2);
        }
        return bytes + 2;
    }
    if (driver == K::SQLite) return bytes;
    return bytes + (bytes <= 126 ? 1 : 4);
}

qint64 rowCount(QString text) {
    static const QRegularExpression grouped("^[0-9]{1,3}(?:,[0-9]{3})+$");
    text = text.trimmed().replace(QChar(0xff0c), ',');
    if (grouped.match(text).hasMatch()) text.remove(',');
    bool ok = false;
    const auto rows = text.toLongLong(&ok);
    if (!ok || rows <= 0) fail("行数须为正整数，例如 100,000,000,000,000");
    return rows;
}
long double capacity(const QString& text) {
    static const QRegularExpression syntax(R"rx(^\s*([0-9]+(?:\.[0-9]+)?)\s*(B|[KMGTPE]I?B)?\s*$)rx",
                                           QRegularExpression::CaseInsensitiveOption);
    const auto match = syntax.match(text);
    if (!match.hasMatch()) fail("容量格式示例为 20 GB、1.5 TiB 或 500000 B");
    auto bytes = static_cast<long double>(match.captured(1).toDouble());
    const auto unit = match.captured(2).toUpper();
    if (unit.size() >= 2) {
        const int power = QString("KMGTPE").indexOf(unit.front()) + 1;
        for (int i = 0; i < power; ++i) bytes *= unit.contains('I') ? 1024 : 1000;
    }
    if (!std::isfinite(bytes) || bytes <= 0) fail("容量须为有限正数");
    return bytes;
}
QString sizeText(long double bytes) {
    const QStringList units{"B", "KiB", "MiB", "GiB", "TiB", "PiB", "EiB", "ZiB", "YiB"};
    int unit = 0;
    while (bytes >= 1024 && unit + 1 < units.size()) { bytes /= 1024; ++unit; }
    return QLocale().toString(double(bytes), 'f', unit ? 2 : 0) + ' ' + units[unit];
}
QString rowText(qint64 count) { return QLocale().toString(count); }

int rowOverhead(K driver, int fields) {
    const auto nullBytes = (fields + 7) / 8;
    if (driver == K::SQLite) return 8 + fields;
    if (usesNativePostgres(driver)) return 28 + nullBytes;
    if (isMySqlFamily(driver)) return 20 + nullBytes;
    return 24 + nullBytes;
}

QVector<QVariant> makeRow(QVector<Field>& fields, const QVector<ForeignPool>& pools, K driver,
                           qint64 row, Random& random, bool enforceUnique, qsizetype& uniqueBytes) {
    QVector<QVariant> values(fields.size());
    for (const auto& pool : pools) {
        if (pool.rows.isEmpty()) continue;
        const auto key = pool.unique && enforceUnique ? row :
            std::uniform_int_distribution<qint64>(0, pool.rows.size() - 1)(random);
        const auto& entry = pool.rows[int(key)];
        for (int i = 0; i < pool.fields.size(); ++i) values[pool.fields[i]] = entry[i];
    }
    for (int i = 0; i < fields.size(); ++i) {
        auto& field = fields[i];
        if (field.omitted || field.foreignUnavailable) continue;
        auto& value = values[i];
        if (field.rule.generate) {
            const bool distinct = enforceUnique && field.unique && field.rule.kind != "sequence" &&
                                  field.rule.kind != "series" && field.rule.kind != "null";
            for (int attempt = 0; ; ++attempt) {
                try { value = field.rule.generate(row, random); }
                catch (const std::exception& e) { fail(field.definition->name + "：" + QString::fromUtf8(e.what())); }
                if (!distinct) break;
                const auto key = value.toString();
                if (!field.seen.contains(key)) {
                    uniqueBytes += key.size() * 2 + 80;
                    if (uniqueBytes > uniqueBytesLimit)
                        fail("随机唯一值超过内存预算，请改用递增序列或减少本批行数：" + field.definition->name);
                    field.seen.insert(key);
                    break;
                }
                if (attempt >= 63) fail("无法继续生成符合唯一约束的值，请扩大取值范围：" + field.definition->name);
            }
        }
        validateValue(field, value, driver);
    }
    return values;
}

QString parameter(int ordinal, K driver, const FieldType& type, bool notNull) {
    if (usesNativePostgres(driver)) return "$" + QString::number(ordinal);
    if (driver != K::Oracle) return "?" + QString::number(ordinal);
    const auto bind = ":" + QString::number(ordinal);
    if (!notNull) return bind;
    if (type.family == Family::Date)
        return "TO_DATE(" + bind + ", 'YYYY-MM-DD')";
    if (type.family == Family::DateTime) {
        if (type.timeZone) return "TO_TIMESTAMP_TZ(" + bind + ", 'YYYY-MM-DD HH24:MI:SS TZH:TZM')";
        return "TO_TIMESTAMP(" + bind + ", 'YYYY-MM-DD HH24:MI:SS')";
    }
    return bind;
}
}

void generateTable(Database& db, const ConnectionProfile& profile, const TableTask& task,
                   const Cancellation& cancel, TableTaskResult& outcome, const GenerationProgress& progress,
                   const std::function<QString(const QVariant&, K)>& literal) {
    const auto driver = profile.driver;
    const auto table = readTableDefinition(db, profile, task.node, cancel);
    const auto tableName = tableQualifiedName(profile, task.node);
    const auto format = task.generation.size;
    const auto storage = task.generation.sizeMode == GenerationSizeMode::Capacity;
    if (!task.generation.preview && !storage && (format.isEmpty() ? task.maxRows : rowCount(format)) > maximumGenerationRows)
        fail(QString("每次最多写入 %1 行。更大规模可使用预览估算。").arg(rowText(maximumGenerationRows)));
    if (!task.generation.preview && isMySqlFamily(driver) && table.engine.compare("InnoDB", Qt::CaseInsensitive) != 0)
        fail("写入整批数据需要 InnoDB 事务表");
    const auto charset = dbCharset(db, driver, table, cancel);
    const auto foreign = readForeignKeys(db, profile, table, cancel, literal);
    const auto keys = uniqueKeys(table, driver);
    QVector<Field> fields;
    QSet<QString> known;
    for (const auto& column : table.columns) known.insert(column.name);
    for (auto it = task.generationRules.begin(); it != task.generationRules.end(); ++it)
        if (!known.contains(it.key())) fail("规则字段已不存在：" + it.key());

    QSet<QString> charsets;
    bool fallback = false;
    QStringList insertColumns;
    QVector<int> insertIndices;
    for (const auto& column : table.columns) {
        Field field;
        field.definition = &column;
        field.type = fieldType(column);
        field.bounds = field.type.family == Family::Integer ?
            checkBounds(table, column.name, integerBounds(field.type, driver)) : IntegerBounds{};
        field.charset = charset;
        if (isMySqlFamily(driver) && !column.collation.isEmpty()) field.charset = column.collation.section('_', 0, 0).toLower();
        if (driver == K::SQLServer && field.type.name.startsWith('n')) field.charset = "utf-16le";
        if (driver == K::SQLServer && column.attributes.contains("_UTF8", Qt::CaseInsensitive)) field.charset = "utf8";
        field.codec = field.charset.isEmpty() ? nullptr : QTextCodec::codecForName(codecName(field.charset).toLatin1());
        if (field.type.family == Family::Text) {
            if (field.codec) charsets.insert(field.charset);
            else fallback = true;
        }
        const auto config = task.generationRules.value(column.name).toObject();
        std::optional<qint64> start;
        try {
            if (config.value("kind") == "sequence" &&
                config.value("value").toString().compare("auto", Qt::CaseInsensitive) == 0) {
                const auto q = execute(db, "SELECT MAX(" + quoteIdentifier(column.name, driver) +
                                           ") FROM " + tableName, cancel);
                if (q.rows.size() != 1 || q.rows.front().isEmpty()) fail("未读取到目标字段当前最大值");
                const auto current = q.rows.front().front();
                if (!current.isValid() || current.isNull()) start = std::max<qint64>(1, field.bounds.min);
                else {
                    bool ok = false;
                    const auto last = current.toLongLong(&ok);
                    if (!ok || last == std::numeric_limits<qint64>::max()) fail("当前最大值无法继续递增");
                    start = std::max(last + 1, field.bounds.min);
                }
            }
            field.rule = prepareRule(column, field.type, driver, field.bounds, config, start);
        } catch (const std::exception& e) {
            fail(column.name + "：" + QString::fromUtf8(e.what()));
        }
        field.omitted = !field.rule.generate && field.rule.kind != "foreign";
        field.unique = containsUniqueKey(keys, {int(fields.size())});
        if (field.omitted && !task.generation.preview && !column.nullable && column.defaultExpression.isEmpty() &&
            !readOnlyColumn(column) && !sqliteRowId(table, column, driver))
            fail("非空字段没有数据库默认值，请指定生成规则：" + column.name);
        if (!field.omitted) {
            insertColumns << quoteIdentifier(column.name, driver);
            insertIndices << int(fields.size());
        }
        fields << std::move(field);
    }

    auto pools = foreignPools(db, profile, table, foreign, keys, fields, task.generation.preview, cancel);
    auto count = storage ? qint64(0) : format.isEmpty() ? qint64(task.maxRows) : rowCount(format);
    if (!storage && count <= 0) fail("行数须大于零");
    const int samples = storage ? 128 : int(std::min<qint64>(count, 128));
    const int overhead = rowOverhead(driver, int(fields.size()));
    int skippedIndexes = 0;
    const auto indexes = estimatedIndexes(table, driver, skippedIndexes);
    const auto clustered = isMySqlFamily(driver) ? primaryKeyColumns(table) : QVector<int>{};
    long double dataBytes = 0, indexBytes = 0;
    QVector<QVector<QVariant>> sampleRows;
    qsizetype sampleBytes = 0, uniqueBytes = 0;
    Random previewRandom(task.generation.seed);
    for (int i = 0; i < samples; ++i) {
        if (cancel->load()) fail("操作已停止");
        auto values = makeRow(fields, pools, driver, i, previewRandom, false, uniqueBytes);
        dataBytes += overhead;
        qsizetype bytes = 0;
        QVector<int> sizes;
        sizes.reserve(fields.size());
        for (int j = 0; j < fields.size(); ++j) {
            const auto size = typedBytes(fields[j], values[j], driver);
            sizes << size;
            dataBytes += size;
            bytes += values[j].userType() == QMetaType::QByteArray ? values[j].toByteArray().size() :
                     values[j].toString().size() * 2;
        }
        int rowLink = 12;
        for (const int field : clustered) rowLink += sizes[field];
        for (const auto& index : indexes) {
            indexBytes += rowLink;
            for (const int field : index) indexBytes += sizes[field];
        }
        if (task.generation.preview && sampleRows.size() < 12 && sampleBytes + bytes <= previewBytesLimit) {
            sampleBytes += bytes;
            sampleRows << std::move(values);
        }
    }
    const auto average = (dataBytes + indexBytes) / samples;
    if (storage) {
        const auto planned = std::floor(capacity(format) / average);
        if (planned < 1) fail("指定容量不足以容纳一行");
        if (planned >= static_cast<long double>(std::numeric_limits<qint64>::max()))
            fail("估算行数超出 64 位整数范围");
        count = static_cast<qint64>(planned);
    }
    if (task.generation.preview) {
        const auto encoding = QStringList(charsets.values()).join(", ");
        outcome.report = QString("估算 %1 行，共约 %2，平均 %3 B/行。数据约 %4，常见索引约 %5。")
            .arg(rowText(count), sizeText(average * count), QLocale().toString(double(average), 'f', 1),
                 sizeText(dataBytes / samples * count), sizeText(indexBytes / samples * count));
        if (!encoding.isEmpty()) outcome.report += " 字符集 " + encoding + "。";
        if (fallback) outcome.report += " 未知字符集按 UTF-8 估算。";
        outcome.report += "\n按生成样本和常见行、索引开销估算。事务日志、压缩和实际空页另计。";
        if (skippedIndexes)
            outcome.report += QString(" 有 %1 个复杂索引未计入。").arg(skippedIndexes);
        if (count > maximumGenerationRows)
            outcome.report += QString(" 此规模仅估算，单次最多写入 %1 行。").arg(rowText(maximumGenerationRows));
        for (const auto& pool : pools) {
            if (pool.rows.isEmpty())
                outcome.report += "\n" + pool.reference + " 没有可用父键，外键仅按类型估算。写入前需准备父表数据。";
            else if (pool.unique && count > pool.rows.size())
                outcome.report += QString("\n自动外键 %1 仅采到 %2 个可用候选键，无法满足整批唯一性。")
                    .arg(pool.name, rowText(pool.rows.size()));
        }
        for (int i = 0; i < fields.size(); ++i)
            if (fields[i].omitted) outcome.result.columns << fields[i].definition->name + "（数据库默认）";
            else if (fields[i].foreignUnavailable) outcome.result.columns << fields[i].definition->name + "（无父键）";
            else outcome.result.columns << fields[i].definition->name;
        outcome.result.rows = std::move(sampleRows);
        outcome.rows = count;
        return;
    }
    if (count > maximumGenerationRows)
        fail(QString("估算目标约 %1 行。单次最多写入 %2 行。").arg(rowText(count), rowText(maximumGenerationRows)));
    for (const auto& field : fields) {
        if (field.rule.sequenceStart) {
            const auto start = *field.rule.sequenceStart;
            if (start > std::numeric_limits<qint64>::max() - (count - 1) ||
                start + (count - 1) > field.bounds.max)
                fail("递增规则超出字段类型或 CHECK 范围：" + field.definition->name);
        }
        if (count > 1 && field.unique && field.rule.kind == "constant")
            fail("唯一字段不能为多行使用同一个固定值：" + field.definition->name);
    }

    Random random(task.generation.seed);
    for (auto& pool : pools) {
        if (pool.unique && count > pool.rows.size()) fail("自动外键的可用唯一键数量不足：" + pool.name);
        std::shuffle(pool.rows.begin(), pool.rows.end(), random);
    }
    const QString prefix = "INSERT INTO " + tableName;
    if (insertColumns.isEmpty() && driver == K::Oracle)
        fail("Oracle 至少需要一列显式值才能使用本工具写入");
    const auto header = prefix + " (" + insertColumns.join(", ") + ") VALUES ";
    const int batchSize = driver == K::Oracle || insertIndices.isEmpty() ? 1 :
        std::max(1, std::min(250, 900 / int(insertIndices.size())));
    QVector<QVariant> arguments;
    QStringList tuples;
    qsizetype batchBytes = 0;
    QElapsedTimer elapsed;
    elapsed.start();
    const auto flush = [&] {
        if (tuples.isEmpty()) return;
        if (insertIndices.isEmpty())
            execute(db, prefix + (isMySqlFamily(driver) ? " () VALUES ()" : " DEFAULT VALUES"), cancel);
        else
            execute(db, header + tuples.join(", "), cancel, {}, arguments);
        outcome.rows += tuples.size();
        tuples.clear();
        arguments.clear();
        batchBytes = 0;
        if (progress && (elapsed.elapsed() >= 500 || outcome.rows == count)) {
            progress(outcome.rows, count);
            elapsed.restart();
        }
    };
    for (qint64 row = 0; row < count; ++row) {
        if (cancel->load()) fail("操作已停止");
        auto values = makeRow(fields, pools, driver, row, random, true, uniqueBytes);
        qsizetype bytes = 0;
        for (const int index : insertIndices) {
            const auto& value = values[index];
            bytes += (value.userType() == QMetaType::QByteArray ? value.toByteArray().size() :
                      value.toString().size() * 2) + 24;
        }
        if (bytes > batchBytesLimit) fail("生成的单行数据超过写入内存上限");
        if (tuples.size() >= batchSize || batchBytes + bytes > batchBytesLimit) flush();
        QStringList markers;
        for (const int index : insertIndices) {
            auto value = std::move(values[index]);
            if (driver == K::SQLServer) {
                markers << literal(value, driver);
            } else {
                markers << parameter(int(arguments.size()) + 1, driver, fields[index].type, value.isValid() && !value.isNull());
                arguments << std::move(value);
            }
        }
        batchBytes += bytes;
        tuples << "(" + markers.join(",") + ")";
    }
    flush();
}
}
}
