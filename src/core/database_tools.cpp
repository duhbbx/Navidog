#include "navidog/database_tools.hpp"
#include "navidog/er_model.hpp"
#include "navidog/metadata_cache.hpp"
#include "navidog/operation_timing.hpp"
#include "table_design_internal.hpp"

#include <QRegularExpression>
#include <QMap>
#include <algorithm>

namespace navidog {
namespace {
using K = DatabaseKind;
constexpr int maxRelations = 20000;

void require(bool condition, const QString& message) {
    if (!condition) design::fail(message);
}

QueryResult read(Database& db, const QString& sql, const Cancellation& cancel, int maxRows = 1) {
    QueryLimits limits;
    limits.maxRows = maxRows;
    auto result = db.execute(sql, limits, cancel);
    require(!result.cancelled && !cancel->load(), "操作已停止");
    require(result.error.isEmpty(), result.error);
    require(!result.truncated && !result.cellTruncated, "数据库目录结果超过读取上限");
    return result;
}

QStringList databaseSchemas(Database& db, const ConnectionProfile& profile, QString schema,
                            const Cancellation& cancel) {
    if (!schema.isEmpty()) return {schema};
    if (profile.driver != K::PostgreSQL && profile.driver != K::SQLServer)
        return {QString{}};

    const auto query = profile.driver == K::PostgreSQL
        ? "SELECT schema_name FROM information_schema.schemata WHERE schema_name <> 'information_schema' "
          "AND left(schema_name, 3) <> 'pg_' ORDER BY schema_name"
        : "SELECT name FROM sys.schemas WHERE name NOT IN ('sys','INFORMATION_SCHEMA') "
          "AND schema_id < 16384 ORDER BY name";
    QStringList names;
    for (const auto& row : read(db, query, cancel, 4096).rows)
        names << row.value(0).toString();
    return names;
}

QVector<TableDefinition> inventory(Database& db, const ConnectionProfile& profile,
                                   const QString& schema, bool views, const Cancellation& cancel) {
    QVector<TableDefinition> relations;
    for (const auto& name : databaseSchemas(db, profile, schema, cancel)) {
        er::ImportOptions options;
        options.schema = name;
        options.includeViews = views;
        auto imported = er::reverse(db, profile, options, cancel);
        require(relations.size() + imported.project.entities.size() <= maxRelations,
                        "对象超过 20000 个，请选择单个 Schema 或缩小范围");
        for (auto& entity : imported.project.entities)
            relations.push_back(std::move(entity.table));
    }
    return relations;
}

QString qualified(const CatalogNode& node, K driver) {
    const auto prefix = isMySqlFamily(driver) ? node.database : node.schema;
    return (prefix.isEmpty() ? QString{} : quoteIdentifier(prefix, driver) + ".") +
           quoteIdentifier(node.object, driver);
}

bool searchable(const QString& declaredType, K driver) {
    const auto type = declaredType.toUpper();
    if (driver == K::SQLite)
        return !type.contains("BLOB");
    static const QRegularExpression scalar(
        R"(^(?:[N]?CHAR\w*|[N]?VARCHAR\w*|TINYTEXT|MEDIUMTEXT|LONGTEXT|[N]?TEXT|CLOB|NCLOB|UUID|UNIQUEIDENTIFIER|CITEXT|ENUM|SET|UROWID|ROWID|JSONB?|DATE|TIME\w*|SMALLDATETIME|DATETIME\w*|INT\w*|BIGINT|MEDIUMINT|SMALLINT|TINYINT|DECIMAL|NUMERIC|NUMBER|FLOAT|REAL|DOUBLE\w*|BINARY_FLOAT|BINARY_DOUBLE|MONEY|SMALLMONEY|BOOL\w*|BIT)(?:\b|\())");
    return scalar.match(type).hasMatch();
}

QString dataPredicate(const TableColumn& column, K driver, const DatabaseSearchRequest& request) {
    const auto name = quoteIdentifier(column.name, driver);
    const auto type = driver == K::SQLServer ? "NVARCHAR(MAX)" :
                      isMySqlFamily(driver) ? "CHAR CHARACTER SET utf8mb4" :
                      usesOracleSql(driver) ? "VARCHAR2(4000)" : "TEXT";
    const bool lob = usesOracleSql(driver) &&
        (column.type.compare("CLOB", Qt::CaseInsensitive) == 0 ||
         column.type.compare("NCLOB", Qt::CaseInsensitive) == 0);
    const auto value = lob ? "LOWER(" + name + ")" : "LOWER(CAST(" + name + " AS " + type + "))";
    const auto needle = "LOWER(" + design::lit(request.text, driver) + ")";
    QString predicate;
    if (request.mode == DatabaseSearchMode::Exact) {
        predicate = lob ? "DBMS_LOB.COMPARE(" + value + ", TO_CLOB(" + needle + ")) = 0"
                        : value + " = " + needle;
    } else {
        const auto position = (isMySqlFamily(driver) ? "LOCATE(" + needle + "," + value
            : driver == K::SQLServer ? "CHARINDEX(" + needle + "," + value
            : driver == K::PostgreSQL ? "STRPOS(" + value + "," + needle
            : "INSTR(" + value + "," + needle) + ")";
        predicate = position + (request.mode == DatabaseSearchMode::StartsWith ? " = 1" : " > 0");
    }
    if (driver == K::SQLite)
        return "(typeof(" + name + ") <> 'blob' AND " + predicate + ")";
    return predicate;
}

bool nameMatches(const QString& text, const DatabaseSearchRequest& request) {
    if (request.mode == DatabaseSearchMode::Exact)
        return text.compare(request.text, Qt::CaseInsensitive) == 0;
    if (request.mode == DatabaseSearchMode::StartsWith)
        return text.startsWith(request.text, Qt::CaseInsensitive);
    return text.contains(request.text, Qt::CaseInsensitive);
}

QString optionName(const QString& value) {
    static const QRegularExpression valid(QStringLiteral("\\A[A-Za-z0-9_]+\\z"));
    require(valid.match(value).hasMatch(), "字符集或排序规则名称无效");
    return value;
}
}

bool supportsDatabaseEdit(DatabaseKind kind) {
    return isMySqlFamily(kind) || kind == K::PostgreSQL || kind == K::SQLServer || kind == K::SQLite;
}

bool supportsDatabaseSqlDump(DatabaseKind kind, bool schema) {
    return kind == K::PostgreSQL || (!schema && (kind == K::SQLite || isMySqlFamily(kind)));
}

ConnectionProfile scopeDatabase(ConnectionProfile profile, const CatalogNode& node) {
    if (profile.driver != K::SQLite && !usesOracleSql(profile.driver) && !node.database.isEmpty())
        profile.database = node.database;
    return profile;
}

DatabaseProperties loadDatabaseProperties(const ConnectionProfile& profile, const CatalogNode& node,
                                          const Cancellation& cancel) {
    auto target = scopeDatabase(profile, node);
    target.advanced.initialSql.clear();
    auto db = connectDatabase(target);
    DatabaseProperties properties;
    if (target.driver == K::SQLite) {
        properties.userVersion = read(*db, "PRAGMA main.user_version", cancel).rows.value(0).value(0).toInt();
        properties.charset = read(*db, "PRAGMA main.encoding", cancel).rows.value(0).value(0).toString();
        return properties;
    }

    const auto rows = read(*db, definitionSql(target, node), cancel).rows;
    require(!rows.isEmpty(), "未找到所选数据库的属性");
    const auto& row = rows.front();
    if (isMySqlFamily(target.driver)) {
        properties.charset = row.value(1).toString();
        properties.collation = row.value(2).toString();
    } else if (target.driver == K::PostgreSQL) {
        properties.owner = row.value(1).toString();
        properties.charset = row.value(2).toString();
        properties.collation = row.value(3).toString();
    } else if (target.driver == K::SQLServer) {
        properties.collation = row.value(2).toString();
    }
    if (!target.readOnly)
        properties.options = loadNamespaceOptions(target, NamespaceKind::Database, cancel);
    return properties;
}

QString alterDatabaseSql(const ConnectionProfile& profile, const CatalogNode& node,
                         const DatabaseProperties& properties) {
    require(!profile.readOnly && supportsDatabaseEdit(profile.driver), "此连接不能修改数据库属性");
    require(node.kind == "database", "请选择数据库节点");
    if (profile.driver == K::SQLite) {
        require(properties.userVersion >= 0, "用户版本必须是非负整数");
        return "PRAGMA main.user_version = " + QString::number(properties.userVersion) + ";";
    }
    require(!node.database.isEmpty() && !node.database.contains(QChar::Null), "数据库名称无效");
    const auto root = "ALTER DATABASE " + quoteIdentifier(node.database, profile.driver);
    if (isMySqlFamily(profile.driver))
        return root + " DEFAULT CHARACTER SET " + optionName(properties.charset) +
               " COLLATE " + optionName(properties.collation) + ";";
    if (profile.driver == K::SQLServer)
        return root + " COLLATE " + optionName(properties.collation) + ";";
    require(!properties.owner.isEmpty() && !properties.owner.contains(QChar::Null),
                    "所有者名称无效");
    return root + " OWNER TO " + quoteIdentifier(properties.owner, profile.driver) + ";";
}

void applyDatabaseProperties(const ConnectionProfile& profile, const CatalogNode& node,
                             const DatabaseProperties& properties, const Cancellation& cancel) {
    const auto sql = alterDatabaseSql(profile, node, properties);
    auto target = scopeDatabase(profile, node);
    target.advanced.initialSql.clear();
    if (target.driver == K::SQLServer)
        target.database = "master";
    auto db = connectDatabase(target);
    read(*db, sql, cancel);
}

DatabaseSearchResult searchDatabase(const ConnectionProfile& profile, const DatabaseSearchRequest& request,
                                    const Cancellation& cancel, const DatabaseProgress& progress) {
    const OperationTimer timing(TimedOperation::LoadCatalog);
    require(!request.text.isEmpty() && request.text.size() <= 512 &&
                    !request.text.contains(QChar::Null), "请输入长度不超过 512 的查找内容");
    require(request.scope != DatabaseSearchScope::ObjectDefinitions, "对象定义请使用元数据查找");
    auto target = profile;
    target.readOnly = true;
    target.advanced.initialSql.clear();
    auto db = connectDatabase(target);
    const auto tables = inventory(*db, target, request.schema, request.scope == DatabaseSearchScope::ObjectNames, cancel);
    DatabaseSearchResult out;
    for (const auto& table : tables) {
        if (cancel->load()) break;
        const auto& node = table.node;
        if (progress) progress(++out.scanned, int(tables.size()));
        else ++out.scanned;
        if (request.scope == DatabaseSearchScope::ObjectNames) {
            qint64 matches = nameMatches(node.object, request) ? 1 : 0;
            for (const auto& column : table.columns)
                if (nameMatches(column.name, request)) ++matches;
            if (matches) out.hits.push_back({node, matches, {}, {}});
            continue;
        }

        QStringList predicates;
        for (const auto& column : table.columns)
            if (searchable(column.type, target.driver))
                predicates << dataPredicate(column, target.driver, request);
        if (predicates.isEmpty()) {
            ++out.skipped;
            continue;
        }
        const auto from = " FROM " + qualified(node, target.driver) + " WHERE " + predicates.join(" OR ");
        const auto count = db->execute((target.driver == K::SQLServer
            ? "SELECT COUNT_BIG(*)" : "SELECT COUNT(*)") + from, {}, cancel);
        if (count.cancelled || cancel->load()) break;
        if (!count.error.isEmpty()) {
            ++out.skipped;
            if (out.warnings.size() < 10)
                out.warnings << qualified(node, target.driver) + " · " + count.error.left(500);
            continue;
        }
        const auto matches = count.rows.value(0).value(0).toLongLong();
        if (!matches) continue;
        auto sql = (target.driver == K::SQLServer ? "SELECT TOP (100) *" : "SELECT *") + from;
        if (usesOracleSql(target.driver)) sql = "SELECT * FROM (" + sql + ") WHERE ROWNUM <= 100";
        else if (target.driver != K::SQLServer) sql += " LIMIT 100";
        out.hits.push_back({node, matches, sql + ";", {}});
    }
    return out;
}

DatabaseSearchResult searchDatabaseObjects(const ConnectionProfile& profile, const QString& database,
                                           const DatabaseSearchRequest& request, const QString& cachePath,
                                           bool canRefresh, const Cancellation& cancel) {
    const OperationTimer timing(TimedOperation::LoadCatalog);
    require(request.scope != DatabaseSearchScope::Data && !request.text.isEmpty() && request.text.size() <= 512 &&
            !request.text.contains(QChar::Null), "请输入长度不超过 512 的对象查找内容");
    DatabaseSearchResult out;
    const auto warn = [&](const QString& message) {
        ++out.skipped;
        if (out.warnings.size() < 10) out.warnings.push_back(message.left(500));
    };
    std::unique_ptr<MetadataStore> cache;
    try {
        if (!cachePath.isEmpty()) cache = std::make_unique<MetadataStore>(cachePath);
    } catch (const std::exception&) { warn("本地元数据缓存暂不可用"); }
    auto target = profile;
    target.readOnly = true;
    target.advanced.initialSql.clear();
    target.advanced.connectTimeout = qBound(1, target.advanced.connectTimeout, 5);
    const bool oracle = usesOracleSql(profile.driver);
    const bool sqlite = profile.driver == K::SQLite;
    const auto schemaFilter = isMySqlFamily(profile.driver) || sqlite ? QString{} : request.schema;
    using Scope = QPair<QString, QString>;
    QMap<Scope, QStringList> candidates;
    const auto addScope = [&](const QString& db, const QString& schema) {
        auto& variants = candidates[{db, oracle ? schema : QString{}}];
        if (!variants.contains(schema)) variants << schema;
    };
    if (cache) try {
        for (const auto& scope : cache->searchScopes(profile)) {
            if ((oracle || sqlite) && scope.first != profile.database) continue;
            if (!oracle && !sqlite && !database.isEmpty() && scope.first != database) continue;
            if (oracle && !schemaFilter.isEmpty() && scope.second != schemaFilter) continue;
            // The default Oracle snapshot represents one implicit owner, not the whole service.
            addScope(scope.first, oracle && scope.second.isEmpty() ? profile.user.toUpper() : scope.second);
            if (oracle && scope.second.isEmpty()) candidates[{scope.first, profile.user.toUpper()}] << QString{};
        }
    } catch (const std::exception&) { warn("无法读取本地对象索引"); }
    const bool wholeConnection = oracle ? schemaFilter.isEmpty() : !sqlite && database.isEmpty();
    std::unique_ptr<Database> discovery;
    if (!wholeConnection) {
        const auto db = oracle || sqlite ? profile.database : database;
        auto& variants = candidates[{db, oracle ? schemaFilter : QString{}}];
        if (variants.isEmpty()) variants << (oracle ? schemaFilter : QString{});
    } else if (canRefresh && !cancel->load()) {
        try {
            discovery = connectDatabase(target);
            if (oracle) {
                for (const auto& row : read(*discovery, "SELECT DISTINCT owner FROM all_objects WHERE object_type "
                    "IN('TABLE','VIEW','FUNCTION','PROCEDURE','INDEX','SEQUENCE','TRIGGER','TYPE','SYNONYM','PACKAGE') "
                    "ORDER BY owner", cancel, 4096).rows) {
                    const auto owner = row.value(0).toString();
                    if (!candidates.contains({profile.database, owner})) addScope(profile.database, owner);
                }
            } else {
                const auto namespaces = loadCatalog(*discovery, target, {}, cancel);
                require(namespaces.error.isEmpty(), namespaces.error);
                if (namespaces.truncated) warn("连接目录已达到读取上限");
                for (const auto& node : namespaces.nodes) {
                    if (node.kind != "database") continue;
                    if (profile.driver == K::PostgreSQL && node.database == "template1") continue;
                    if (!candidates.contains({node.database, {}})) addScope(node.database, {});
                }
            }
        } catch (const std::exception& e) { warn("无法读取连接目录：" + QString::fromUtf8(e.what())); }
    }
    auto scopes = candidates.keys();
    const Scope preferred{profile.database, oracle ? profile.user.toUpper() : QString{}};
    if (scopes.removeAll(preferred)) scopes.prepend(preferred);
    qint64 resultBytes = 0;
    constexpr int hitLimit = 2000;
    for (const auto& scope : scopes) {
        if (cancel->load()) break;
        const auto name = oracle ? scope.second : sqlite ? profile.name : scope.first;
        MetadataSnapshotPtr snapshot;
        target.database = scope.first;
        if (cache) try {
            for (const auto& variant : candidates.value(scope)) {
                const auto key = metadataKey(target, target.database, variant);
                for (const auto& candidate : {key, key + "-search"}) {
                    auto read = cache->load(candidate);
                    const auto now = QDateTime::currentMSecsSinceEpoch();
                    if (!read || !read->definitionsRead || read->invalidated || read->refreshedMs > now ||
                        now - read->refreshedMs >= MetadataSnapshot::ttlMs) continue;
                    if (!snapshot || read->refreshedMs > snapshot->refreshedMs) snapshot = std::move(read);
                }
            }
        } catch (const std::exception&) { warn(name + " · 无法读取缓存"); }
        if (snapshot) ++out.cachedScopes;
        else if (canRefresh) {
            try {
                const auto generation = cache ? cache->generation(profile.id) : -1;
                auto db = discovery && (oracle || isMySqlFamily(profile.driver))
                    ? std::unique_ptr<Database>{} : connectDatabase(target);
                auto fresh = collectMetadata(db ? *db : *discovery, target, scope.second, cancel, true);
                fresh.key += "-search";
                snapshot = std::make_shared<const MetadataSnapshot>(std::move(fresh));
                ++out.refreshedScopes;
                if (cache) try { cache->replace(*snapshot, generation); }
                    catch (const std::exception&) { warn(name + " · 缓存未保存"); }
            } catch (const std::exception& e) { warn(name + " · " + QString::fromUtf8(e.what())); }
        }
        if (!snapshot) continue;
        if (!snapshot->tablesComplete || !snapshot->routinesComplete || !snapshot->columnsComplete ||
            (request.scope == DatabaseSearchScope::ObjectDefinitions && !snapshot->definitionsComplete) ||
            (request.scope == DatabaseSearchScope::ObjectNames && !snapshot->objectWarning.isEmpty()))
            warn(name + " · 部分元数据超过读取限制或服务器未返回定义");
        for (const auto& object : snapshot->objects) {
            if (cancel->load()) break;
            const bool relation = object.kind == "table" || object.kind == "view" || object.kind == "materializedView";
            const bool routine = object.kind == "function" || object.kind == "procedure";
            if (request.scope == DatabaseSearchScope::ObjectDefinitions && !relation && !routine) continue;
            if (object.kind == "database" && object.name != scope.first) continue;
            const auto owner = object.kind == "schema" ? object.name : object.schema;
            if (!schemaFilter.isEmpty() && owner != schemaFilter) continue;
            if (oracle && owner != scope.second) continue;
            if (wholeConnection && profile.driver == K::PostgreSQL &&
                (object.schema == "pg_catalog" || object.schema == "information_schema")) continue;
            if (sqlite && routine) continue; // Built-in SQLite functions have no database object identity.
            ++out.scanned;
            qint64 matches = 0;
            if (request.scope == DatabaseSearchScope::ObjectDefinitions) {
                matches = !object.definition.isEmpty() && nameMatches(object.definition, request) ? 1 : 0;
            } else {
                matches = nameMatches(object.name, request) ? 1 : 0;
                if (relation) for (const auto& column : object.columns)
                    matches += nameMatches(column.name, request) ? 1 : 0;
            }
            if (!matches) continue;
            resultBytes += 2LL * (object.definition.size() + object.name.size() + object.schema.size() + object.database.size());
            if (out.hits.size() >= hitLimit || resultBytes > 8 * 1024 * 1024) {
                warn("结果达到显示上限，请缩小搜索范围");
                return out;
            }
            CatalogNode node;
            node.kind = object.kind;
            node.name = node.object = object.name;
            node.database = sqlite || oracle ? QString{} : object.database;
            node.schema = isMySqlFamily(profile.driver) ? QString{} : owner;
            if (!relation && !routine) node.detail = object.type;
            out.hits.push_back({node, matches, {}, object.definition});
        }
    }
    if (!out.cachedScopes && !out.refreshedScopes && !cancel->load() && !canRefresh)
        warn("此范围没有可用的对象缓存，请先打开连接再查找");
    else if (wholeConnection && !canRefresh && !cancel->load())
        warn("当前仅查找已有缓存的范围，打开连接后可查找其余对象");
    return out;
}

DatabaseDictionary loadDatabaseDictionary(const ConnectionProfile& profile, const QString& schema,
                                          const Cancellation& cancel) {
    auto target = profile;
    target.readOnly = true;
    target.advanced.initialSql.clear();
    auto db = connectDatabase(target);
    auto tables = inventory(*db, target, schema, true, cancel);
    DatabaseDictionary dictionary;
    dictionary.serverVersion = db->serverVersion();
    dictionary.scope = target.driver == K::SQLite ? target.name : target.database;
    if (!schema.isEmpty() && target.driver != K::SQLite && !isMySqlFamily(target.driver)) {
        if (!dictionary.scope.isEmpty()) dictionary.scope += " / ";
        dictionary.scope += schema;
    }
    dictionary.tables.reserve(tables.size());
    for (auto& table : tables) {
        require(!cancel->load(), "操作已停止");
        dictionary.tables.push_back({qualified(table.node, target.driver), std::move(table.comment),
                                     std::move(table.columns), table.node.kind, table.node.object,
                                     std::move(table.objects)});
    }
    return dictionary;
}

QString databaseDictionaryHtml(const DatabaseDictionary& dictionary, const Cancellation& cancel) {
    const auto& tables = dictionary.tables;
    const auto formattedComment = [](QString text) {
        return text.replace("\r\n", "\n").replace('\r', '\n').toHtmlEscaped().replace("\n", "<br>");
    };
    QString html = "<!doctype html><html lang='zh'><head><meta charset='utf-8'>"
        "<title>数据库字典</title><style>body{font:15px system-ui;margin:2rem;color:#253548}"
        "table{border-collapse:collapse;margin-bottom:2rem;width:100%}td,th{border:1px solid #d6e0ea;padding:6px 10px}"
        "th{background:#edf3f8;text-align:left}</style></head><body>";
    html += "<h1>数据库字典</h1><p>" + dictionary.scope.toHtmlEscaped() + "</p><ol>";
    for (int i = 0; i < tables.size(); ++i)
        html += "<li><a href='#object-" + QString::number(i) + "'>" +
                tables[i].name.toHtmlEscaped() + "</a></li>";
    html += "</ol>";
    for (int i = 0; i < tables.size(); ++i) {
        require(!cancel->load(), "操作已停止");
        const auto& table = tables[i];
        html += "<h2 id='object-" + QString::number(i) + "'>" +
                table.name.toHtmlEscaped() + "</h2>";
        if (!table.comment.isEmpty()) html += "<p>" + formattedComment(table.comment) + "</p>";
        html += "<table><thead><tr><th>字段</th><th>类型</th><th>允许空值</th>"
                "<th>主键</th><th>默认值</th><th>注释</th></tr></thead><tbody>";
        for (const auto& column : table.columns)
            html += "<tr><td>" + column.name.toHtmlEscaped() + "</td><td>" +
                    column.type.toHtmlEscaped() + "</td><td>" + (column.nullable ? "是" : "否") +
                    "</td><td>" + (column.primaryKey ? "是" : "") + "</td><td>" +
                    column.defaultExpression.toHtmlEscaped() + "</td><td>" + formattedComment(column.comment) +
                    "</td></tr>";
        html += "</tbody></table>";
        require(html.size() <= 8 * 1024 * 1024, "字典超过 8 MiB，请按 Schema 导出");
    }
    return html + "</body></html>";
}
}
