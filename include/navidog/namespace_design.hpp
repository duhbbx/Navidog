#pragma once
#include "catalog.hpp"
namespace navidog {
enum class NamespaceKind { Database, Schema };
struct NamespaceDraft {
    NamespaceKind kind=NamespaceKind::Database;
    QString name, owner, charset, collation, filePath;
    // Runtime only. Never serialize this draft or include the password in previews.
    QString password, defaultTablespace, temporaryTablespace;
    int quotaMiB=0;
    bool grantSession=true, accountLocked=false;
};
struct NamespaceOptions {
    QStringList owners, charsets, collations;
    QStringList tablespaces, temporaryTablespaces;
    // Parallel list for filtering collations by character set.
    QStringList collationCharsets;
    QString error;
};
struct NamespaceResult { QString error; bool applied=false, cancelled=false, partial=false; };
bool supportsNamespace(DatabaseKind,NamespaceKind);
bool supportsNamespaceDrop(DatabaseKind,NamespaceKind);
// SQL preview only; credentials are always redacted.
QString namespaceSql(const ConnectionProfile&,const NamespaceDraft&);
QString dropNamespaceSql(const ConnectionProfile&,NamespaceKind,const QString& name);
NamespaceOptions loadNamespaceOptions(const ConnectionProfile&,NamespaceKind,const Cancellation&);
// Independent connection: never commits a query tab's transaction.
NamespaceResult createNamespace(const ConnectionProfile&,const NamespaceDraft&,const Cancellation&);
NamespaceResult dropNamespace(const ConnectionProfile&,NamespaceKind,const QString& name,const Cancellation&);
}
Q_DECLARE_METATYPE(navidog::NamespaceOptions)
Q_DECLARE_METATYPE(navidog::NamespaceResult)
