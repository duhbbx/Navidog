#pragma once
#include "database.hpp"
namespace navidog {
// Names are separate components; a dot inside a quoted identifier is never
// split.
struct CatalogNode {
  QString kind = "connection", name, database, schema, object, key, detail;
  bool expandable = true;
};
struct CatalogResult {
  QString serverVersion;
  DatabaseVersion detectedVersion = DatabaseVersion::Unknown;
  QVector<CatalogNode> nodes;
  QString error;
  bool truncated = false, cancelled = false;
};
CatalogResult loadCatalog(Database &, const ConnectionProfile &,
                          const CatalogNode &, const Cancellation &);
QString quoteIdentifier(QString, navidog::DatabaseKind driver);
QString previewSql(const ConnectionProfile &, const CatalogNode &);
QString definitionSql(const ConnectionProfile &, const CatalogNode &);
} // namespace navidog
Q_DECLARE_METATYPE(navidog::CatalogNode)
Q_DECLARE_METATYPE(navidog::CatalogResult)
