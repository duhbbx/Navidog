#pragma once

#include "catalog.hpp"
#include "statement_templates.hpp"
#include "background_tasks.hpp"

class QIODevice;
#include <QPair>

namespace navidog {
enum class CatalogObjectChange { Drop, DropCascade, Compile, Rename };
struct CatalogObjectDetails {
    QVector<QPair<QString, QString>> properties;
    QString sql, warning, error;
    bool cancelled = false;
};
struct CatalogObjectChangeResult {
    QString error, diagnostics;
    bool applied = false, cancelled = false;
};
QString catalogObjectTypeName(const QString& kind);
bool supportsObjectProperties(DatabaseKind, const QString& kind);
bool supportsObjectCascade(DatabaseKind, const QString& kind);
bool supportsObjectCompile(DatabaseKind, const QString& kind);
bool supportsObjectRename(DatabaseKind, const QString& kind);
bool supportsObjectBatchDrop(DatabaseKind, const QString& kind);
bool supportsObjectBatchSql(DatabaseKind, const QString& kind, std::optional<StatementTemplate>);
QString catalogObjectChangeSql(const ConnectionProfile&, const CatalogNode&, CatalogObjectChange, const QString& newName = {});
CatalogObjectDetails readCatalogObjectDetails(Database&, const ConnectionProfile&, const CatalogNode&, const Cancellation&);
CatalogObjectChangeResult changeCatalogObject(const ConnectionProfile&, const CatalogNode&, CatalogObjectChange,
                                              const Cancellation&, const QString& newName = {});
QStringList catalogBatchDropSql(const ConnectionProfile&, const QVector<CatalogNode>&, bool cascade);
struct CatalogBatchDropResult {
    QVector<CatalogNode> removed;
    QString error;
    bool cancelled = false, rolledBack = false;
};
CatalogBatchDropResult dropCatalogBatch(const ConnectionProfile&, const QVector<CatalogNode>&, bool cascade,
                                        const TaskContext&);
void writeCatalogBatchSql(const ConnectionProfile&, const QVector<CatalogNode>&,
                          std::optional<StatementTemplate>, QIODevice&, const TaskContext&, qint64 byteLimit = 0);
}
Q_DECLARE_METATYPE(navidog::CatalogObjectDetails)
Q_DECLARE_METATYPE(navidog::CatalogObjectChangeResult)
Q_DECLARE_METATYPE(navidog::CatalogBatchDropResult)
