#include "navidog/object_management.hpp"
#include "navidog/source_design.hpp"
#include "navidog/table_design.hpp"
#include <QHash>
#include <QIODevice>
#include <QSet>
#include <algorithm>

namespace navidog {
namespace {
using K = DatabaseKind;
using Change = CatalogObjectChange;

[[noreturn]] void fail(const QString& message) { throw std::runtime_error(message.toStdString()); }

bool routine(const QString& kind) { return kind == "function" || kind == "procedure"; }
bool tableChild(const QString& kind) {
    return kind == "column" || kind == "index" || kind == "primaryKey" ||
           kind == "foreignKey" || kind == "check" || kind == "trigger";
}
bool tableMetadataSupported(K kind) {
    return kind == K::SQLite || kind == K::SQLServer || kind == K::Oracle ||
           isMySqlFamily(kind) || usesNativePostgres(kind) || kind == K::OpenGauss || usesJdbcCatalog(kind);
}

QString literal(QString value, K driver) {
    if (isMySqlFamily(driver))
        return value.isEmpty() ? QString("''") :
            "CONVERT(X'" + QString::fromLatin1(value.toUtf8().toHex()) + "' USING utf8mb4)";
    if (usesNativePostgres(driver)) value.replace("\\", "\\\\");
    value.replace("'", "''");
    return (usesNativePostgres(driver) ? "E'" : driver == K::SQLServer ? "N'" : "'") + value + "'";
}

QueryResult read(Database& db, const QString& sql, const Cancellation& token, int rows = 250) {
    if (token->load()) fail("已停止");
    QueryLimits limits;
    limits.maxRows = rows;
    limits.maxCellBytes = 1024 * 1024;
    limits.maxBytes = 4 * 1024 * 1024;
    auto result = db.execute(sql, limits, token);
    if (result.cancelled) fail("已停止");
    if (!result.error.isEmpty()) fail(result.error);
    if (result.truncated || result.cellTruncated) fail("目录信息超过读取上限");
    return result;
}

QString keyword(const QString& kind) {
    return kind == "materializedView" ? "MATERIALIZED VIEW" : kind.toUpper();
}

void add(QVector<QPair<QString, QString>>& properties, const QString& name, const QString& value) {
    if (!value.isEmpty()) properties.push_back(qMakePair(name, value));
}
void addRow(CatalogObjectDetails& details, const QueryResult& rows) {
    if (rows.rows.size() != 1) return;
    const auto& row = rows.rows.front();
    for (int i = 0; i < row.size() && i < rows.columns.size(); ++i)
        add(details.properties, rows.columns[i], row[i].toString());
}

QString oraclePredicate(const ConnectionProfile& profile, const CatalogNode& node, const QString& nameField,
                        const QString& typeField) {
    const auto name = tableChild(node.kind) ? node.name : node.object;
    return " owner=" + literal(node.schema, profile.driver) + " AND " + nameField + "=" +
           literal(name, profile.driver) + " AND " + typeField + "=" + literal(keyword(node.kind), profile.driver);
}
QString compileMessages(Database& db, const ConnectionProfile& profile, const CatalogNode& node,
                        const Cancellation& cancel) {
    const auto result = read(db, "SELECT attribute,line,position,text FROM all_errors WHERE" +
        oraclePredicate(profile, node, "name", "type") + " ORDER BY sequence", cancel);
    QStringList messages;
    for (const auto& row : result.rows)
        messages << QString("%1 %2:%3  %4").arg(row[0].toString(), row[1].toString(), row[2].toString(), row[3].toString().trimmed());
    return messages.join('\n');
}

void verifyRoutineIdentity(Database& db, const ConnectionProfile& profile, const CatalogNode& node,
                           const Cancellation& cancel) {
    if (!usesNativePostgres(profile.driver) || !routine(node.kind)) return;
    const auto identity = read(db, "SELECT pg_get_function_identity_arguments(p.oid) FROM pg_proc p "
        "JOIN pg_namespace n ON n.oid=p.pronamespace WHERE p.oid=" + literal(node.key, profile.driver) +
        "::oid AND n.nspname=" + literal(node.schema, profile.driver) +
        " AND p.proname=" + literal(node.object, profile.driver) +
        (node.kind == "procedure" ? " AND p.prokind='p'" : " AND p.prokind IN ('f','w')"), cancel, 2);
    if (identity.rows.size() != 1 || identity.rows[0][0].toString() != node.detail)
        fail("例程签名已变化，请刷新后重新选择");
}

bool constraint(const QString& kind) { return kind=="primaryKey" || kind=="foreignKey" || kind=="check"; }

QString renameSql(const ConnectionProfile& profile, const CatalogNode& node, const QString& newName) {
    const auto driver = profile.driver;
    if (!supportsObjectRename(driver, node.kind)) fail("此数据库未提供这个对象的重命名操作");
    const bool child = tableChild(node.kind);
    const auto oldName = child ? node.name : node.object;
    const int size = usesNativePostgres(driver) || driver==K::Oracle ? newName.toUtf8().size() : newName.size();
    const int maximum = usesNativePostgres(driver) ? 63 : driver==K::Oracle &&
        (profile.version==DatabaseVersion::Oracle11 || profile.detectedVersion==DatabaseVersion::Oracle11) ? 30 :
        isMySqlFamily(driver) ? 64 : 128;
    if (newName.trimmed().isEmpty() || size > maximum || newName == oldName ||
        std::any_of(newName.cbegin(), newName.cend(), [](QChar ch) {return ch.unicode() < 32 || ch.unicode() == 127;}))
        fail(QString("请输入不同的有效名称，长度上限为 %1").arg(maximum));
    const auto quote = [&](const QString& value) {return quoteIdentifier(value, driver);};
    const auto replacement = quote(newName);
    const auto table = tableQualifiedName(profile, node);
    auto childTarget = node;
    childTarget.object = oldName;
    const auto qualified = tableQualifiedName(profile, childTarget);
    if (node.kind=="column") {
        if (driver==K::SQLServer)
            return "EXEC sys.sp_rename " + literal(table+"."+quote(oldName),driver) + ", " +
                   literal(newName,driver) + ", 'COLUMN'";
        return "ALTER TABLE " + table + " RENAME COLUMN " + quote(oldName) + " TO " + replacement;
    }
    if (node.kind=="index") {
        if (driver==K::SQLServer)
            return "EXEC sys.sp_rename " + literal(table+"."+quote(oldName),driver) + ", " +
                   literal(newName,driver) + ", 'INDEX'";
        if (isMySqlFamily(driver)) return "ALTER TABLE " + table + " RENAME INDEX " + quote(oldName) + " TO " + replacement;
        return "ALTER INDEX " + qualified + " RENAME TO " + replacement;
    }
    if (constraint(node.kind))
        return "ALTER TABLE " + table + " RENAME CONSTRAINT " + quote(oldName) + " TO " + replacement;
    if (node.kind=="trigger")
        return driver==K::Oracle ? "ALTER TRIGGER " + qualified + " RENAME TO " + replacement :
            "ALTER TRIGGER " + quote(oldName) + " ON " + table + " RENAME TO " + replacement;
    if (driver==K::Oracle)
        return "RENAME " + quote(oldName) + " TO " + replacement; // Verified against the session and current schema.
    if (isMySqlFamily(driver)) {
        auto target = node;
        target.object = newName;
        return "RENAME TABLE " + table + " TO " + tableQualifiedName(profile, target);
    }
    const auto signature = routine(node.kind) ? sourceDropSql(profile,node).mid(6 + node.kind.size()) : table;
    return "ALTER " + keyword(node.kind) + " " + signature + " RENAME TO " + replacement;
}

void checkBatch(const QVector<CatalogNode>& nodes) {
    if (nodes.size()<2 || nodes.size()>128) fail("请选择同一目录的 2–128 个对象");
    const auto& first = nodes.front();
    QSet<QPair<QString,QString>> objects;
    for (const auto& node : nodes) {
        const auto identity = qMakePair(node.object, node.key);
        if (node.database!=first.database || node.schema!=first.schema || node.kind!=first.kind ||
            node.object.isEmpty() || objects.contains(identity))
            fail("批量操作必须使用同一目录中不同的对象");
        objects.insert(identity);
    }
}

void describeTable(Database& db, const ConnectionProfile& profile, const CatalogNode& node,
                   const Cancellation& cancel, CatalogObjectDetails& details) {
    auto table = node;
    table.kind = "table";
    table.name = table.object;
    const auto definition = readTableDefinition(db, profile, table, cancel, node.kind != "column");
    auto& fields = details.properties;
    if (node.kind == "table") {
        add(fields, "字段数", QString::number(definition.columns.size()));
        add(fields, "主键", definition.primaryKeyName);
        add(fields, "注释", definition.comment);
        add(fields, "存储引擎", definition.engine);
        add(fields, "字符集", definition.charset);
        add(fields, "排序规则", definition.collation);
        add(fields, "表空间", definition.tablespace);
        details.sql = definition.rawSql;
        if (details.sql.isEmpty()) {
            add(fields, "SQL 来源", "根据目录中的表结构生成");
            details.sql = createTableSql(profile, definition);
        }
        return;
    }
    if (node.kind == "column") {
        const auto field = std::find_if(definition.columns.cbegin(), definition.columns.cend(),
                                        [&](const auto& item) { return item.name == node.name; });
        if (field == definition.columns.cend()) fail("字段已变化，请刷新导航树");
        add(fields, "数据类型", field->type);
        add(fields, "可空", field->nullable ? "是" : "否");
        add(fields, "默认值", field->defaultExpression);
        add(fields, "生成表达式", field->generatedExpression);
        add(fields, "排序规则", field->collation);
        add(fields, "注释", field->comment);
        if (field->primaryKey) add(fields, "主键顺序", QString::number(field->primaryKey));
        return;
    }
    if (node.kind == "primaryKey") {
        QStringList names;
        for (const auto& column : definition.columns)
            if (column.primaryKey) names << column.name;
        add(fields, "字段", names.join(", "));
        return;
    }
    const auto object = std::find_if(definition.objects.cbegin(), definition.objects.cend(), [&](const auto& item) {
        return tableObjectKindKey(item.kind) == node.kind && item.name == node.name;
    });
    if (object == definition.objects.cend()) fail("表对象已变化，请刷新导航树");
    add(fields, "字段", formatIdentifierList(object->columns, profile.driver));
    add(fields, "方法", object->method);
    add(fields, "表达式", object->expression);
    add(fields, "筛选条件", object->predicate);
    add(fields, "注释", object->comment);
    if (object->kind == TableObjectKind::Index) add(fields, "唯一", object->unique ? "是" : "否");
    if (object->kind == TableObjectKind::Trigger) add(fields, "启用", object->enabled ? "是" : "否");
    if (object->kind == TableObjectKind::ForeignKey) {
        add(fields, "引用表", object->referenceSchema.isEmpty() ? object->referenceTable :
            object->referenceSchema + "." + object->referenceTable);
        add(fields, "引用字段", formatIdentifierList(object->referenceColumns, profile.driver));
        add(fields, "删除规则", object->onDelete);
        add(fields, "更新规则", object->onUpdate);
    }
    details.sql = object->sql;
    if (details.sql.isEmpty()) {
        TableDefinition target;
        target.node = table;
        target.objects << *object;
        details.sql = tableObjectCreates(profile, {}, target).join(";\n");
    }
}
}

QString catalogObjectTypeName(const QString& kind) {
    static const QHash<QString, QString> names{
        {"table", "表"}, {"view", "视图"}, {"materializedView", "物化视图"}, {"function", "函数"},
        {"procedure", "存储过程"}, {"column", "字段"}, {"trigger", "触发器"}, {"index", "索引"},
        {"primaryKey", "主键"}, {"foreignKey", "外键"}, {"check", "检查约束"}, {"schema", "Schema"},
        {"database", "数据库"},
    };
    return names.value(kind, kind);
}
bool supportsObjectProperties(K driver, const QString& kind) {
    return supportsSourceDesign(driver, kind) ||
           (tableMetadataSupported(driver) && (kind == "table" || tableChild(kind))) ||
           (!isCommandDatabase(driver) && driver != K::Unknown && (kind == "database" || kind == "schema"));
}
bool supportsObjectCascade(K driver, const QString& kind) {
    if (driver == K::Oracle) return kind == "table" || kind == "view" || kind == "schema";
    return usesNativePostgres(driver) && (routine(kind) || kind == "table" || kind == "view" ||
        (kind == "materializedView" && supportsSourceDesign(driver, kind)) || kind == "schema");
}
bool supportsObjectCompile(K driver, const QString& kind) {
    return driver == K::Oracle && (routine(kind) || kind == "view" || kind == "materializedView" || kind == "trigger");
}
bool supportsObjectRename(K driver, const QString& kind) {
    if (usesNativePostgres(driver))
        return kind=="view" || kind=="materializedView" || routine(kind) || tableChild(kind);
    if (driver==K::Oracle) return kind=="view" || tableChild(kind);
    if (driver==K::SQLServer) return kind=="column" || kind=="index";
    if (driver==K::SQLite) return kind=="column";
    return (driver==K::MySQL || driver==K::MariaDB || driver==K::PolarDbMySql) &&
        (kind=="view" || kind=="index");
}
bool supportsObjectBatchDrop(K driver, const QString& kind) {
    return supportsSourceDesign(driver,kind) || (kind=="table" &&
        (tableMetadataSupported(driver) || isAnalyticalMySql(driver) || driver==K::Dameng || driver==K::DuckDb));
}
bool supportsObjectBatchSql(K driver, const QString& kind, std::optional<StatementTemplate> statement) {
    if (!statement) return supportsSourceDesign(driver,kind) || (kind=="table" && tableMetadataSupported(driver));
    if (kind=="table" && tableMetadataSupported(driver)) return true;
    return isSourceRelation(kind) && *statement==StatementTemplate::Select;
}
QString catalogObjectChangeSql(const ConnectionProfile& profile, const CatalogNode& node, Change change,
                               const QString& newName) {
    const auto kind = profile.driver;
    const auto name = tableChild(node.kind) ? node.name : node.object;
    if (name.isEmpty() || name.contains(QChar(0))) fail("请选择有效的数据库对象");
    if (profile.readOnly || profile.http.enabled) fail("此操作需要可写的直连或 SSH 连接");
    if ((usesOracleSql(kind) || usesNativePostgres(kind) || kind == K::SQLServer) && node.schema.isEmpty())
        fail("请刷新导航树并重新选择带有 Schema 的对象");
    const auto schema = node.schema.toLower();
    const auto db = node.database.isEmpty() ? profile.database.toLower() : node.database.toLower();
    if ((kind == K::Oracle && (schema == "sys" || schema == "system")) ||
        (usesNativePostgres(kind) && (schema.startsWith("pg_") || schema == "information_schema")) ||
        (isMySqlFamily(kind) && QStringList{"mysql","sys","information_schema","performance_schema","oceanbase"}.contains(db)) ||
        (kind == K::SQLServer && (schema == "sys" || schema == "information_schema")))
        fail("此菜单只操作用户对象");
    if (change == Change::Rename) return renameSql(profile,node,newName);
    if (change == Change::Compile) {
        if (!supportsObjectCompile(kind, node.kind) || node.schema.isEmpty()) fail("此对象不支持独立编译");
        auto target = node;
        target.object = name;
        return "ALTER " + keyword(node.kind) + " " + tableQualifiedName(profile, target) + " COMPILE" +
               ((routine(node.kind) || node.kind == "trigger") ? " REUSE SETTINGS" : "");
    }
    const bool cascade = change == Change::DropCascade;
    if (cascade && !supportsObjectCascade(kind, node.kind)) fail("此对象不支持级联删除");
    QString sql;
    if (node.kind == "table") sql = "DROP TABLE " + tableQualifiedName(profile, node);
    else if (supportsSourceDesign(kind, node.kind)) sql = sourceDropSql(profile, node);
    else fail("此对象不支持删除");
    if (cascade) sql += kind == K::Oracle ? " CASCADE CONSTRAINTS" : " CASCADE";
    return sql;
}

CatalogObjectDetails readCatalogObjectDetails(Database& db, const ConnectionProfile& profile,
                                              const CatalogNode& node, const Cancellation& cancel) {
    CatalogObjectDetails details;
    auto& props = details.properties;
    const auto database = node.database.isEmpty() ? profile.database : node.database;
    const auto schema = node.schema.isEmpty() && profile.driver == K::SQLite ? QString("main") :
        node.schema.isEmpty() && isMySqlFamily(profile.driver) ? database : node.schema;
    add(props, "名称", tableChild(node.kind) || node.kind == "database" || node.kind == "schema" ? node.name : node.object);
    add(props, "对象类型", catalogObjectTypeName(node.kind));
    add(props, "连接", profile.name);
    add(props, "数据库", database.isEmpty() ? "—" : database);
    add(props, "Schema", schema.isEmpty() ? "—" : schema);
    if (tableChild(node.kind)) add(props, "所属表", node.object);
    if (routine(node.kind) && usesNativePostgres(profile.driver)) add(props, "参数签名", "(" + node.detail + ")");
    try {
        if (supportsObjectCompile(profile.driver, node.kind)) {
            addRow(details, read(db, "SELECT status AS \"状态\", created AS \"创建时间\", "
                "last_ddl_time AS \"最近 DDL\" FROM all_objects WHERE" +
                oraclePredicate(profile, node, "object_name", "object_type") + " AND subobject_name IS NULL", cancel, 2));
            details.warning = compileMessages(db, profile, node, cancel);
        }
        if (tableMetadataSupported(profile.driver) && (node.kind == "table" || tableChild(node.kind))) {
            describeTable(db, profile, node, cancel, details);
            return details;
        }
        if (node.kind == "database" || node.kind == "schema") {
            if (profile.driver != K::SQLite) addRow(details, read(db, definitionSql(profile, node), cancel, 2));
            return details;
        }
        if (usesNativePostgres(profile.driver) && routine(node.kind)) {
            addRow(details, read(db, "SELECT pg_get_userbyid(p.proowner) AS \"所有者\",l.lanname AS \"语言\", "
                "obj_description(p.oid,'pg_proc') AS \"注释\" FROM pg_proc p JOIN pg_language l ON l.oid=p.prolang "
                "JOIN pg_namespace n ON n.oid=p.pronamespace WHERE p.oid=" + literal(node.key, profile.driver) +
                "::oid AND n.nspname=" + literal(node.schema, profile.driver) +
                " AND p.proname=" + literal(node.object, profile.driver), cancel, 2));
        }
        if (isMySqlFamily(profile.driver)) {
            const auto name = literal(node.object, profile.driver);
            const auto databaseName = literal(database, profile.driver);
            if (routine(node.kind)) {
                addRow(details, read(db, "SELECT DEFINER, SECURITY_TYPE, IS_DETERMINISTIC, SQL_DATA_ACCESS, "
                    "CREATED, LAST_ALTERED, ROUTINE_COMMENT FROM information_schema.ROUTINES WHERE ROUTINE_SCHEMA=" +
                    databaseName + " AND ROUTINE_NAME=" + name + " AND ROUTINE_TYPE=" +
                    literal(keyword(node.kind), profile.driver), cancel, 2));
            } else {
                addRow(details, read(db, "SELECT DEFINER, SECURITY_TYPE, IS_UPDATABLE, CHECK_OPTION "
                    "FROM information_schema.VIEWS WHERE TABLE_SCHEMA=" + databaseName + " AND TABLE_NAME=" +
                    name, cancel, 2));
            }
        } else if (profile.driver == K::SQLServer) {
            addRow(details, read(db, "SELECT o.type_desc, o.create_date, o.modify_date, m.uses_ansi_nulls, "
                "m.uses_quoted_identifier, m.is_schema_bound FROM sys.objects o "
                "LEFT JOIN sys.sql_modules m ON m.object_id=o.object_id WHERE o.object_id=OBJECT_ID(" +
                literal(tableQualifiedName(profile, node), profile.driver) + ")", cancel, 2));
        } else if (usesNativePostgres(profile.driver) && isSourceRelation(node.kind)) {
            addRow(details, read(db, "SELECT pg_get_userbyid(c.relowner) AS \"所有者\", "
                "obj_description(c.oid,'pg_class') AS \"注释\", "
                "CASE WHEN c.relkind='m' THEN c.relispopulated::text END AS \"已填充\" "
                "FROM pg_class c JOIN pg_namespace n ON n.oid=c.relnamespace WHERE n.nspname=" +
                literal(node.schema, profile.driver) + " AND c.relname=" + literal(node.object, profile.driver), cancel, 2));
        }
        const auto definition = readSourceDefinition(db, profile, node, cancel);
        details.sql = definition.sql;
        if (node.kind == "materializedView" &&
            (usesNativePostgres(profile.driver) || definition.sql.startsWith("-- 从 Oracle 目录")))
            add(props, "SQL 来源", "根据目录中的查询定义生成");
        add(props, "SQL 模式", definition.sqlMode);
        if (routine(node.kind)) {
            for (const auto& parameter : definition.parameters) add(props, parameter.name, parameter.detail);
        }
    } catch (const std::exception& error) {
        details.cancelled = cancel->load();
        if (!details.cancelled) {
            if (!details.warning.isEmpty()) details.warning += '\n';
            details.warning += QString::fromUtf8(error.what());
        }
    }
    return details;
}

CatalogObjectChangeResult changeCatalogObject(const ConnectionProfile& profile, const CatalogNode& node,
                                              Change change, const Cancellation& cancel, const QString& newName) {
    CatalogObjectChangeResult result;
    try {
        const auto sql = catalogObjectChangeSql(profile, node, change, newName);
        if (cancel->load()) { result.cancelled = true; return result; }
        auto db = connectDatabase(profile);
        if (change != Change::Compile) verifyRoutineIdentity(*db, profile, node, cancel);
        if (change==Change::Rename && profile.driver==K::Oracle && node.kind=="view") {
            const auto scope = read(*db, "SELECT SYS_CONTEXT('USERENV','SESSION_USER'),"
                "SYS_CONTEXT('USERENV','CURRENT_SCHEMA') FROM dual", cancel, 2);
            if (scope.rows.size()!=1 || scope.rows[0][0].toString()!=node.schema || scope.rows[0][1].toString()!=node.schema)
                fail("请用视图所属 Schema 的账号连接，在该 Schema 下重命名");
        }
        read(*db, sql, cancel);
        result.applied = true;
        if (change == Change::Compile) {
            result.diagnostics = compileMessages(*db, profile, node, cancel);
            const auto status = read(*db, "SELECT status FROM all_objects WHERE" +
                oraclePredicate(profile, node, "object_name", "object_type") + " AND subobject_name IS NULL", cancel, 2);
            if (status.rows.size() != 1 || status.rows[0][0].toString() != "VALID")
                result.error = "服务器编译后对象仍处于无效状态";
        }
    } catch (const std::exception& error) {
        result.cancelled = cancel->load();
        if (!result.cancelled) result.error = QString::fromUtf8(error.what());
    }
    return result;
}
QStringList catalogBatchDropSql(const ConnectionProfile& profile, const QVector<CatalogNode>& nodes, bool cascade) {
    checkBatch(nodes);
    const auto kind = nodes.front().kind;
    if (!supportsObjectBatchDrop(profile.driver,kind)) fail("此对象暂不支持批量删除");
    const auto change = cascade ? Change::DropCascade : Change::Drop;
    if (usesNativePostgres(profile.driver)) {
        const auto prefix = "DROP " + keyword(kind) + " ";
        const auto suffix = cascade ? QString(" CASCADE") : QString{};
        QStringList names;
        for (const auto& node : nodes) {
            const auto sql = catalogObjectChangeSql(profile,node,change);
            names << sql.mid(prefix.size(), sql.size()-prefix.size()-suffix.size());
        }
        return {prefix + names.join(", ") + suffix};
    }
    QStringList sql;
    for (const auto& node : nodes) sql << catalogObjectChangeSql(profile,node,change);
    return sql;
}

CatalogBatchDropResult dropCatalogBatch(const ConnectionProfile& profile, const QVector<CatalogNode>& nodes,
                                        bool cascade, const TaskContext& context) {
    CatalogBatchDropResult result;
    std::unique_ptr<Database> db;
    bool transaction = false;
    const auto driver = profile.driver;
    try {
        const auto commands = catalogBatchDropSql(profile,nodes,cascade);
        if (context.cancel->load()) { result.cancelled=true; return result; }
        db = connectDatabase(profile);
        if (driver==K::SQLite || driver==K::SQLServer) {
            read(*db, driver==K::SQLite ? "BEGIN IMMEDIATE" : "BEGIN TRANSACTION", context.cancel);
            transaction = true;
        }
        if (context.report) context.report({0,nodes.size(),TaskUnit::Items,"正在核对对象"});
        for (const auto& node : nodes) verifyRoutineIdentity(*db, profile, node, context.cancel);
        if (usesNativePostgres(driver)) {
            read(*db, commands.front(), context.cancel);
            result.removed = nodes;
        } else {
            for (int i = 0; i < commands.size(); ++i) {
                try { read(*db, commands[i], context.cancel); }
                catch (const std::exception& error) {
                    fail(nodes[i].name + " · " + QString::fromUtf8(error.what()));
                }
                result.removed << nodes[i];
                if (context.report) context.report({i+1,nodes.size(),TaskUnit::Items,"正在删除对象"});
            }
        }
        if (transaction) {
            read(*db,"COMMIT",context.cancel);
            transaction=false;
        }
        if (context.report) context.report({nodes.size(),nodes.size(),TaskUnit::Items,"删除完成"});
    } catch (const std::exception& error) {
        result.cancelled=context.cancel->load();
        if (!result.cancelled) result.error=QString::fromUtf8(error.what());
        if (transaction && db) {
            try {
                read(*db,"ROLLBACK",cancellation());
                result.rolledBack=true;
                result.removed.clear();
            } catch (const std::exception&) {
                result.error += "\n无法确认回滚结果，请刷新目录核对。";
            }
        }
    }
    return result;
}

void writeCatalogBatchSql(const ConnectionProfile& profile, const QVector<CatalogNode>& nodes,
                          std::optional<StatementTemplate> statement, QIODevice& output, const TaskContext& task, qint64 byteLimit) {
    checkBatch(nodes);
    if (!supportsObjectBatchSql(profile.driver,nodes.front().kind,statement)) fail("此对象不支持所选 SQL 模板");
    if (task.cancel->load()) fail("已停止");
    auto db = connectDatabase(profile);
    qint64 written = 0;
    for (int i=0; i<nodes.size(); ++i) {
        const auto& node=nodes[i];
        if (task.cancel->load()) fail("已停止");
        QString sql;
        if (statement) {
            sql = node.kind=="table" ? statementTemplate(profile,readTableDefinition(*db,profile,node,task.cancel,false),*statement)
                                     : previewSql(profile,node);
        } else {
            const auto details = readCatalogObjectDetails(*db,profile,node,task.cancel);
            if (details.sql.isEmpty()) fail(node.name + " · " +
                (details.warning.isEmpty() ? QString("无法读取 SQL 定义") : details.warning));
            sql = details.sql.trimmed();
        }
        sql = sql.trimmed();
        if (!statement && isMySqlFamily(profile.driver) && routine(node.kind)) {
            QString delimiter;
            for (const auto* candidate : {"$$","//","%%","//NAVIDOG//"})
                if (!sql.contains(candidate)) {delimiter=candidate;break;}
            if (delimiter.isEmpty()) fail(node.name + " · 无法为 MySQL 定义选择结束符");
            sql="DELIMITER "+delimiter+"\n"+sql+delimiter+"\nDELIMITER ;";
        } else {
            if (!sql.endsWith(';')) sql+=';';
            if (!statement && usesOracleSql(profile.driver) && routine(node.kind)) sql+="\n/";
            if (!statement && profile.driver==K::SQLServer) sql+="\nGO";
        }
        const auto bytes=(sql+"\n\n").toUtf8();
        if (byteLimit && written+bytes.size()>byteLimit) fail("SQL 草稿超过 4 MiB，请使用导出 SQL");
        if (output.write(bytes)!=bytes.size()) fail(output.errorString());
        written += bytes.size();
        if (task.report) task.report({i+1,nodes.size(),TaskUnit::Items,"正在读取并生成 SQL"});
    }
}

}
