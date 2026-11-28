#include "jdbc_catalog.hpp"
#include <QHash>

namespace navidog {
namespace {
class Rows {
public:
    QueryResult result;
    explicit Rows(QueryResult value) : result(std::move(value)) {
        if (!result.error.isEmpty())
            throw std::runtime_error(result.error.toStdString());
        for (int i = 0; i < result.columns.size(); ++i)
            offsets_.insert(result.columns[i].toUpper(), i);
    }
    QVariant get(const QVector<QVariant>& row, const QString& column) const {
        return row.value(offsets_.value(column, -1));
    }
private:
    QHash<QString, int> offsets_;
};
QString tableType(QString type) {
    return type.contains("VIEW", Qt::CaseInsensitive) ? "view" : "table";
}
QString columnType(const Rows& rows, const QVector<QVariant>& row) {
    auto type = rows.get(row, "TYPE_NAME").toString();
    const auto size = rows.get(row, "COLUMN_SIZE").toInt();
    const auto scale = rows.get(row, "DECIMAL_DIGITS").toInt();
    if (type.contains('(') || size < 1)
        return type;
    switch (rows.get(row, "DATA_TYPE").toInt()) {
    case 1: case 12: case -15: case -9: case -2: case -3:
        if (size < 1000000000)
            type += QString("(%1)").arg(size);
        break;
    case 2: case 3:
        if (size < 100000)
            type += QString("(%1,%2)").arg(size).arg(scale);
        break;
    default:
        break;
    }
    return type;
}
QString activeSchema(Database& db, DatabaseKind kind, const Cancellation& cancel) {
    auto result = db.execute(kind == DatabaseKind::Db2 ? "VALUES CURRENT SCHEMA" : "SELECT CURRENT_SCHEMA", {}, cancel);
    if (result.cancelled)
        return {};
    if (!result.error.isEmpty())
        throw std::runtime_error(result.error.toStdString());
    return result.rows.value(0).value(0).toString().trimmed();
}
bool systemSchema(const QString& name) {
    return name.startsWith("PG_") || name.startsWith("SYS") || name == "INFORMATION_SCHEMA";
}
Rows query(Database& db, JdbcMetadata kind, const QString& schema, const QString& object,
           int maxRows, const Cancellation& cancel) {
    return Rows(db.jdbcMetadata(kind, schema, object, {maxRows, 8 * 1024 * 1024, 8192}, cancel));
}
}

CatalogResult loadJdbcCatalog(Database& db, const ConnectionProfile& profile, const CatalogNode& parent, const Cancellation& cancel) {
    CatalogResult out;
    auto add = [&](const QString& kind, const QString& name, const QString& detail = {}, bool expandable = true) {
        if (name.isEmpty())
            return;
        auto node = parent;
        node.kind = kind;
        node.name = name;
        node.detail = detail;
        node.expandable = expandable;
        if (kind == "schema")
            node.schema = name;
        else if (kind == "table" || kind == "view" || kind == "function" || kind == "procedure")
            node.object = name;
        out.nodes << std::move(node);
    };
    auto load = [&](JdbcMetadata kind) {
        auto rows = query(db, kind, parent.schema, parent.object, 10000, cancel);
        out.truncated |= rows.result.truncated;
        out.cancelled |= rows.result.cancelled;
        return rows;
    };
    if (parent.kind == "connection") {
        const auto rows = load(JdbcMetadata::Schemas);
        for (const auto& row : rows.result.rows)
            add("schema", rows.get(row, "TABLE_SCHEM").toString());
    } else if (parent.kind == "schema") {
        add("tables", "表");
        add("views", "视图");
        add("functions", "函数");
        add("procedures", "存储过程");
    } else if (parent.kind == "tables" || parent.kind == "views") {
        const auto rows = load(JdbcMetadata::Tables);
        for (const auto& row : rows.result.rows) {
            const auto nativeType = rows.get(row, "TABLE_TYPE").toString();
            const auto type = tableType(nativeType);
            if ((type == "table" && parent.kind == "tables") || (type == "view" && parent.kind == "views"))
                add(type, rows.get(row, "TABLE_NAME").toString(), nativeType);
        }
    } else if (parent.kind == "table" || parent.kind == "view") {
        add("columns", "字段");
        if (profile.driver == DatabaseKind::OpenGauss && parent.kind == "table") {
            add("indexes", "索引");
            add("constraints", "约束");
        }
    } else if (parent.kind == "columns") {
        const auto rows = load(JdbcMetadata::Columns);
        for (const auto& row : rows.result.rows)
            add("column", rows.get(row, "COLUMN_NAME").toString(), columnType(rows, row), false);
    } else if (parent.kind == "functions" || parent.kind == "procedures") {
        const bool functions = parent.kind == "functions";
        const auto rows = load(functions ? JdbcMetadata::Functions : JdbcMetadata::Procedures);
        for (const auto& row : rows.result.rows) {
            const auto name = rows.get(row, functions ? "FUNCTION_NAME" : "PROCEDURE_NAME").toString();
            add(functions ? "function" : "procedure", name, {}, false);
        }
    }
    return out;
}

TableDefinition readJdbcTable(Database& db, const CatalogNode& node, const Cancellation& cancel) {
    TableDefinition out;
    out.node = node;
    out.exists = true;
    const auto columns = query(db, JdbcMetadata::Columns, node.schema, node.object, 512, cancel);
    const auto keys = query(db, JdbcMetadata::PrimaryKeys, node.schema, node.object, 512, cancel);
    for (auto result : {&columns.result, &keys.result})
        if (result->truncated || result->cellTruncated || result->cancelled)
            throw std::runtime_error("JDBC 表结构读取不完整");
    if (columns.result.rows.isEmpty())
        throw std::runtime_error("未读取到表字段");
    QHash<QString, int> primary;
    for (const auto& row : keys.result.rows) {
        primary.insert(keys.get(row, "COLUMN_NAME").toString(), keys.get(row, "KEY_SEQ").toInt());
        out.primaryKeyName = keys.get(row, "PK_NAME").toString();
    }
    for (const auto& row : columns.result.rows) {
        TableColumn field;
        field.originalName = field.name = columns.get(row, "COLUMN_NAME").toString();
        field.type = columnType(columns, row);
        field.primaryKey = primary.value(field.name);
        field.nullable = !field.primaryKey && columns.get(row, "NULLABLE").toInt() != 0;
        field.defaultExpression = columns.get(row, "COLUMN_DEF").toString();
        field.comment = columns.get(row, "REMARKS").toString();
        field.autoIncrement = columns.get(row, "IS_AUTOINCREMENT").toString() == "YES";
        if (field.autoIncrement || columns.get(row, "IS_GENERATEDCOLUMN").toString() == "YES")
            field.protectedReason = "已有标识或生成字段请通过 SQL 编辑";
        out.columns << std::move(field);
    }
    out.detailsLoaded = true;
    return out;
}

MetadataSnapshot collectJdbcMetadata(Database& db, const ConnectionProfile& p, const QString& schema,
                                    const Cancellation& cancel, bool allSchemas) {
    MetadataSnapshot out;
    out.profileId = p.id;
    out.identity = metadataIdentity(p);
    out.database = p.database;
    out.schema = schema;
    out.driver = p.driver;
    out.key = metadataKey(p, p.database, schema);
    out.serverVersion = db.serverVersion();
    const auto current = schema.isEmpty() ? activeSchema(db, p.driver, cancel) : schema;
    if (current.isEmpty() || cancel->load())
        return out;
    out.searchPath = QStringList{current};
    out.caseSensitive = false;
    const auto scope = allSchemas ? QString{} : current;
    auto tables = query(db, JdbcMetadata::Tables, scope, {}, 20000, cancel);
    auto columns = query(db, JdbcMetadata::Columns, scope, {}, 50000, cancel);
    out.tablesComplete = !tables.result.truncated && !tables.result.cancelled && !tables.result.cellTruncated;
    out.columnsComplete = !columns.result.truncated && !columns.result.cancelled && !columns.result.cellTruncated;
    QHash<QPair<QString, QString>, int> relations;
    for (const auto& row : tables.result.rows) {
        MetadataObject object;
        object.database = p.database;
        object.schema = tables.get(row, "TABLE_SCHEM").toString();
        if (allSchemas && systemSchema(object.schema.toUpper()) && object.schema != current)
            continue;
        object.name = tables.get(row, "TABLE_NAME").toString();
        object.kind = tableType(tables.get(row, "TABLE_TYPE").toString());
        object.key = object.kind + ":" + object.schema + ":" + object.name;
        relations.insert({object.schema, object.name}, out.objects.size());
        out.objects << std::move(object);
    }
    for (const auto& row : columns.result.rows) {
        const auto location = relations.constFind({columns.get(row, "TABLE_SCHEM").toString(),
                                                   columns.get(row, "TABLE_NAME").toString()});
        if (location != relations.cend())
            out.objects[*location].columns << MetadataColumn{columns.get(row, "COLUMN_NAME").toString(),
                columnType(columns, row), {}, columns.get(row, "ORDINAL_POSITION").toInt()};
    }
    // Driver support for routine metadata varies. Include positive results without
    // making missing-routine claims in an incomplete catalog.
    for (auto kind : {JdbcMetadata::Functions, JdbcMetadata::Procedures}) {
        try {
            const auto rows = query(db, kind, scope, {}, 5000, cancel);
            const bool function = kind == JdbcMetadata::Functions;
            for (const auto& row : rows.result.rows) {
                MetadataObject object;
                object.database = p.database;
                object.kind = function ? "function" : "procedure";
                object.schema = rows.get(row, function ? "FUNCTION_SCHEM" : "PROCEDURE_SCHEM").toString();
                object.name = rows.get(row, function ? "FUNCTION_NAME" : "PROCEDURE_NAME").toString();
                object.key = object.kind + ":" + object.schema + ":" + rows.get(row, "SPECIFIC_NAME").toString();
                object.variadic = true;
                object.maxArgs = 2147483647;
                out.objects << std::move(object);
            }
        } catch (const std::exception&) {
            out.objectWarning = "JDBC 驱动未提供完整的例程目录";
        }
    }
    return out;
}
}
