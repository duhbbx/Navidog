#pragma once
#include "namespace_design.hpp"
#include "table_design.hpp"
#include <functional>

namespace navidog {
enum class DatabaseSearchScope { Data, ObjectNames, ObjectDefinitions };
enum class DatabaseSearchMode { Contains, StartsWith, Exact };
struct DatabaseSearchRequest {
    QString schema, text;
    DatabaseSearchScope scope = DatabaseSearchScope::Data;
    DatabaseSearchMode mode = DatabaseSearchMode::Contains;
};
struct DatabaseSearchHit {
    CatalogNode node;
    qint64 matches = 0;
    QString previewSql;
    QString definition;
};
struct DatabaseSearchResult {
    QVector<DatabaseSearchHit> hits;
    QStringList warnings;
    int scanned = 0, skipped = 0, cachedScopes = 0, refreshedScopes = 0;
};
struct DatabaseProperties {
    QString charset, collation, owner;
    int userVersion = 0;
    NamespaceOptions options;
};
struct DatabaseDictionaryTable {
    QString name, comment;
    QVector<TableColumn> columns;
    QString kind, object;
    QVector<TableObject> objects;
};
struct DatabaseDictionarySection {
    int firstTable = 0;
    QString title, description;
};
struct DatabaseDictionary {
    QString scope;
    QVector<DatabaseDictionaryTable> tables;
    QVector<DatabaseDictionarySection> sections;
    QString serverVersion;
};
using DatabaseProgress = std::function<void(int, int)>;

bool supportsDatabaseEdit(DatabaseKind);
bool supportsDatabaseSqlDump(DatabaseKind, bool schema);
ConnectionProfile scopeDatabase(ConnectionProfile, const CatalogNode&);
DatabaseProperties loadDatabaseProperties(const ConnectionProfile&, const CatalogNode&, const Cancellation&);
QString alterDatabaseSql(const ConnectionProfile&, const CatalogNode&, const DatabaseProperties&);
void applyDatabaseProperties(const ConnectionProfile&, const CatalogNode&, const DatabaseProperties&, const Cancellation&);
DatabaseSearchResult searchDatabase(const ConnectionProfile&, const DatabaseSearchRequest&,
                                    const Cancellation&, const DatabaseProgress& = {});
DatabaseSearchResult searchDatabaseObjects(const ConnectionProfile&, const QString& database,
                                           const DatabaseSearchRequest&, const QString& cachePath,
                                           bool canRefresh, const Cancellation&);
DatabaseDictionary loadDatabaseDictionary(const ConnectionProfile&, const QString& schema, const Cancellation&);
QString databaseDictionaryHtml(const DatabaseDictionary&, const Cancellation&);
}
