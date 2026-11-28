#include "extended_catalog.hpp"
#include "navidog/document_commands.hpp"
#include <QHash>
#include <QJsonDocument>
#include <QSet>

namespace navidog {
namespace {
QString literal(QString value, DatabaseKind kind) {
    if (isAnalyticalMySql(kind) || kind == DatabaseKind::ClickHouse) {
        value.replace('\\', "\\\\");
        value.replace('\'', "''");
    } else value.replace('\'', "''");
    return "'" + value + "'";
}
QString scopeName(const ConnectionProfile& profile, const CatalogNode& node) {
    return (usesPostgresCompatibleSql(profile.driver) || profile.driver == DatabaseKind::DuckDb) ?
        node.schema : (node.database.isEmpty() ? profile.database : node.database);
}
QString tableListSql(DatabaseKind kind, const QString& scope) {
    const auto match = literal(scope, kind);
    if (kind == DatabaseKind::ClickHouse)
        return "SELECT name, if(positionCaseInsensitive(engine,'view') > 0, 'VIEW', 'TABLE'), engine "
               "FROM system.tables WHERE database = " + match + " ORDER BY name";
    const QString catalogFilter = kind == DatabaseKind::DuckDb ? " AND table_catalog = current_database()" : "";
    return "SELECT table_name, table_type FROM information_schema.tables WHERE table_schema = " +
        match + catalogFilter + " ORDER BY table_name";
}
QString columnsSql(DatabaseKind kind, const QString& scope, const QString& object = {}) {
    const auto name = literal(scope, kind);
    if (kind == DatabaseKind::ClickHouse)
        return "SELECT table, name, type, position FROM system.columns WHERE database = " + name +
            (object.isEmpty() ? QString{} : " AND table = " + literal(object, kind)) + " ORDER BY table, position";
    const QString catalogFilter = kind == DatabaseKind::DuckDb ? " AND table_catalog = current_database()" : "";
    return "SELECT table_name, column_name, data_type, ordinal_position FROM information_schema.columns "
        "WHERE table_schema = " + name + catalogFilter +
        (object.isEmpty() ? QString{} : " AND table_name = " + literal(object, kind)) +
        " ORDER BY table_name, ordinal_position";
}
QString indexSql(const ConnectionProfile& profile, const CatalogNode& node, bool definition) {
    const auto kind = profile.driver;
    const auto scope = literal(scopeName(profile, node), kind);
    const auto table = literal(node.object, kind);
    const auto index = literal(node.name, kind);
    if (kind == DatabaseKind::DuckDb)
        return "SELECT index_name, sql FROM duckdb_indexes() WHERE database_name=current_database() "
               "AND schema_name=" + scope + " AND table_name=" + table +
               (definition ? " AND index_name=" + index : " ORDER BY index_name");
    if (kind == DatabaseKind::ClickHouse)
        return "SELECT name, type_full AS type, expr AS expression, granularity FROM system.data_skipping_indices "
               "WHERE database=" + scope + " AND table=" + table +
               (definition ? " AND name=" + index : " ORDER BY name");
    if (isAnalyticalMySql(kind))
        return "SELECT INDEX_NAME, INDEX_TYPE, COLUMN_NAME, SEQ_IN_INDEX, INDEX_COMMENT "
               "FROM information_schema.statistics WHERE TABLE_SCHEMA=" + scope + " AND TABLE_NAME=" + table +
               (definition ? " AND INDEX_NAME=" + index : QString{}) + " ORDER BY INDEX_NAME, SEQ_IN_INDEX";
    return "SELECT indexname, indexdef AS definition FROM pg_catalog.pg_indexes WHERE schemaname=" +
           scope + " AND tablename=" + table + (definition ? " AND indexname=" + index : " ORDER BY indexname");
}
QString constraintSql(const ConnectionProfile& profile, const CatalogNode& node, bool definition) {
    const auto kind = profile.driver;
    const auto scope = literal(scopeName(profile, node), kind);
    const auto table = literal(node.object, kind);
    const auto name = literal(node.name, kind);
    if (kind == DatabaseKind::DuckDb)
        return "SELECT constraint_name, constraint_type, constraint_text AS definition FROM duckdb_constraints() "
               "WHERE database_name=current_database() AND schema_name=" + scope + " AND table_name=" + table +
               (definition ? " AND constraint_name=" + name : " ORDER BY constraint_name");
    const QString property = definition ? "pg_get_constraintdef(c.oid) AS definition" :
        "CASE c.contype WHEN 'p' THEN 'PRIMARY KEY' WHEN 'f' THEN 'FOREIGN KEY' "
        "WHEN 'u' THEN 'UNIQUE' WHEN 'c' THEN 'CHECK' WHEN 'x' THEN 'EXCLUDE' "
        "WHEN 'n' THEN 'NOT NULL' ELSE c.contype::text END AS constraint_type";
    return "SELECT c.conname AS constraint_name, " + property +
           " FROM pg_catalog.pg_constraint c JOIN pg_catalog.pg_class t ON t.oid=c.conrelid "
           "JOIN pg_catalog.pg_namespace n ON n.oid=t.relnamespace WHERE n.nspname=" + scope +
           " AND t.relname=" + table + (definition ? " AND c.conname=" + name : " ORDER BY c.conname");
}
QueryResult checked(Database& db, const QString& sql, int rows, const Cancellation& token) {
    auto result = db.execute(sql, {rows, 8 * 1024 * 1024, 8192}, token);
    if (!result.error.isEmpty()) throw std::runtime_error(result.error.toStdString());
    return result;
}
QString qualified(const ConnectionProfile& profile, const CatalogNode& node) {
    const auto quote = [&](const QString& name) { return quoteIdentifier(name, profile.driver); };
    const auto scope = scopeName(profile, node);
    return (scope.isEmpty() ? QString{} : quote(scope) + '.') + quote(node.object);
}
}
CatalogResult loadExtendedCatalog(Database& db, const ConnectionProfile& profile, const CatalogNode& parent,
                                  const Cancellation& cancel) {
    const auto kind = profile.driver;
    CatalogResult result;
    auto add = [&](const QString& type, const QString& name, const QString& detail = {}, bool expandable = true) {
        if (name.isEmpty()) return;
        auto node = parent;
        node.kind = type;
        node.name = name;
        node.detail = detail;
        node.expandable = expandable;
        if (type == "database") { node.database = name; node.schema.clear(); }
        else if (type == "schema") { node.schema = name; if (node.database.isEmpty()) node.database = profile.database; }
        else if (type == "table" || type == "view" || type == "mongoCollection") node.object = name;
        result.nodes << std::move(node);
    };
    auto rows = [&](const QString& sql) {
        const auto query = checked(db, sql, 10000, cancel);
        result.cancelled |= query.cancelled;
        result.truncated |= query.truncated || query.cellTruncated;
        return query.rows;
    };
    const auto& node = parent.kind;
    if (kind == DatabaseKind::MongoDb) {
        QString command;
        if (node == "connection") command = "listDatabases";
        else if (node == "database") command = "listCollections";
        else if (node == "mongoCollection") command = "listIndexes";
        if (command.isEmpty()) return result;
        for (const auto& row : rows(mongoCatalogCommand(command, parent.object))) {
            const auto doc = QJsonDocument::fromJson(row.value(0).toString().toUtf8()).object();
            const auto name = doc.value("name").toString();
            if (node == "connection") add("database", name);
            else if (node == "database") add("mongoCollection", name, doc.value("type").toString(),
                                              doc.value("type").toString() != "view");
            else add("index", name, {}, false);
        }
        return result;
    }
    const bool schemaLayer = usesNativePostgres(kind) || kind == DatabaseKind::DuckDb;
    if (node == "connection") {
        if (kind == DatabaseKind::DuckDb)
            for (const auto& row : rows("SELECT schema_name FROM information_schema.schemata WHERE catalog_name=current_database() ORDER BY schema_name"))
                add("schema", row.value(0).toString());
        else {
            const auto sql = kind == DatabaseKind::ClickHouse ? QString("SELECT name FROM system.databases ORDER BY name") :
                schemaLayer ? QString("SELECT datname FROM pg_catalog.pg_database ORDER BY datname") : QString("SHOW DATABASES");
            for (const auto& row : rows(sql)) add("database", row.value(0).toString());
        }
    } else if (node == "database" && schemaLayer) {
        for (const auto& row : rows("SELECT schema_name FROM information_schema.schemata ORDER BY schema_name"))
            add("schema", row.value(0).toString());
    } else if (node == "database" || node == "schema") {
        add("tables", "表");
        add("views", "视图");
    } else if (node == "tables" || node == "views") {
        for (const auto& row : rows(tableListSql(kind, scopeName(profile,parent)))) {
            const auto view = row.value(1).toString().contains("VIEW", Qt::CaseInsensitive);
            if (view == (node == "views"))
                add(view ? "view" : "table", row.value(0).toString(), row.value(2).toString());
        }
    } else if (node == "table" || node == "view") {
        add("columns", "字段");
        if (node == "table") {
            add("indexes", "索引");
            if (kind == DatabaseKind::DuckDb || usesPostgresCompatibleSql(kind))
                add("constraints", "约束");
        }
    } else if (node == "indexes") {
        QSet<QString> names;
        for (const auto& row : rows(indexSql(profile,parent,false))) {
            const auto name = row.value(0).toString();
            if (names.contains(name)) continue;
            names.insert(name);
            add("index", name, row.value(1).toString(), false);
        }
    } else if (node == "constraints") {
        for (const auto& row : rows(constraintSql(profile,parent,false)))
            add("constraint", row.value(0).toString(), row.value(1).toString(), false);
    } else if (node == "columns")
        for (const auto& row : rows(columnsSql(kind, scopeName(profile,parent), parent.object)))
            add("column", row.value(1).toString(), row.value(2).toString(), false);
    return result;
}
QString extendedPreview(const ConnectionProfile& p, const CatalogNode& node) {
    if (p.driver == DatabaseKind::MongoDb) return mongoCatalogCommand("find", node.object);
    return "SELECT * FROM " + qualified(p,node) + " LIMIT 100";
}
QString extendedDefinition(const ConnectionProfile& p, const CatalogNode& node) {
    if (p.driver == DatabaseKind::MongoDb)
        return mongoCatalogCommand(node.kind == "index" ? "listIndexes" : "collectionInfo", node.object);
    if (node.kind == "index") return indexSql(p,node,true);
    if (node.kind == "constraint") return constraintSql(p,node,true);
    const auto scope = literal(scopeName(p,node),p.driver);
    const auto name = literal(node.object,p.driver);
    if (node.kind == "schema")
        return "SELECT catalog_name, schema_name, schema_owner FROM information_schema.schemata WHERE schema_name=" + scope;
    if (node.kind == "database") {
        if (p.driver == DatabaseKind::ClickHouse)
            return "SELECT name, engine, comment FROM system.databases WHERE name=" + literal(node.database,p.driver);
        if (isAnalyticalMySql(p.driver))
            return "SELECT schema_name, default_character_set_name, default_collation_name FROM information_schema.schemata WHERE schema_name=" + scope;
        return "SELECT datname, pg_get_userbyid(datdba) AS owner FROM pg_catalog.pg_database WHERE datname=" +
               literal(node.database,p.driver);
    }
    if (p.driver == DatabaseKind::DuckDb)
        return "SELECT sql FROM " + QString(node.kind == "view" ? "duckdb_views()" : "duckdb_tables()") +
            " WHERE database_name=current_database() AND schema_name=" + scope +
            (node.kind == "view" ? " AND view_name=" : " AND table_name=") + name;
    if ((p.driver == DatabaseKind::YugabyteDb || p.driver == DatabaseKind::OpenGauss) && node.kind == "view")
        return "SELECT pg_get_viewdef(" + literal(qualified(p,node),p.driver) + "::regclass, true) AS definition";
    if (p.driver == DatabaseKind::OpenGauss && (node.kind == "function" || node.kind == "procedure"))
        return "SELECT routine_name, routine_type, routine_definition AS definition "
               "FROM information_schema.routines WHERE routine_schema=" + scope + " AND routine_name=" + name;
    if (p.driver == DatabaseKind::ClickHouse || p.driver == DatabaseKind::CockroachDb || isAnalyticalMySql(p.driver))
        return "SHOW CREATE " + QString(node.kind == "view" && p.driver != DatabaseKind::ClickHouse ? "VIEW " : "TABLE ") + qualified(p,node);
    return columnsSql(p.driver, scopeName(p,node), node.object);
}
MetadataSnapshot collectExtendedMetadata(Database& db, const ConnectionProfile& p, const QString& schema,
                                         const Cancellation& cancel) {
    MetadataSnapshot out;
    out.profileId = p.id;
    out.identity = metadataIdentity(p);
    out.key = metadataKey(p,p.database,schema);
    out.driver = p.driver;
    out.database = p.database;
    out.schema = schema;
    out.serverVersion = db.serverVersion();
    const auto kind = p.driver;
    const bool schemaLayer = usesNativePostgres(kind) || kind == DatabaseKind::DuckDb;
    auto scope = schemaLayer ? schema : p.database;
    if (scope.isEmpty()) {
        const auto query = checked(db, kind == DatabaseKind::ClickHouse ? "SELECT currentDatabase()" :
                                    schemaLayer ? "SELECT current_schema()" : "SELECT DATABASE()", 1, cancel);
        scope = query.rows.value(0).value(0).toString();
    }
    if (scope.isEmpty() || cancel->load()) return out;
    out.searchPath = QStringList{scope};
    out.caseSensitive = kind != DatabaseKind::DuckDb;
    const auto tables = checked(db,tableListSql(kind,scope),15000,cancel);
    const auto columns = checked(db,columnsSql(kind,scope),50000,cancel);
    QHash<QString,int> indexes;
    for (const auto& row : tables.rows) {
        MetadataObject object;
        object.database = p.database;
        if (schemaLayer) object.schema = scope;
        else object.database = scope;
        object.kind = row.value(1).toString().contains("VIEW",Qt::CaseInsensitive) ? "view" : "table";
        object.name = row.value(0).toString();
        object.key = scope + ':' + object.kind + ':' + object.name;
        indexes.insert(object.name,out.objects.size());
        out.objects << std::move(object);
    }
    for (const auto& row : columns.rows) {
        const auto it = indexes.constFind(row.value(0).toString());
        if (it != indexes.cend())
            out.objects[*it].columns << MetadataColumn{row.value(1).toString(),row.value(2).toString(),{},row.value(3).toInt()};
    }
    out.tablesComplete = !tables.truncated && !tables.cellTruncated && !tables.cancelled;
    out.columnsComplete = !columns.truncated && !columns.cellTruncated && !columns.cancelled;
    return out;
}
}
