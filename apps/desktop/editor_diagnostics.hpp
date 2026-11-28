#pragma once
#include "navidog/database_types.hpp"
#include "navidog/metadata_cache.hpp"
#include <QObject>
#include <QTimer>
#include <QVector>
#include <atomic>
#include <functional>
#include <memory>

struct SqlEditorDiagnostic {
  int begin = 0, end = 0; // UTF-16 document positions, half-open
  QString message;
  bool error = true;
  bool metadata = false;
};
struct SqlCheckResult {
  QVector<SqlEditorDiagnostic> diagnostics;
  QString status;
};
Q_DECLARE_METATYPE(SqlCheckResult)

// Local compiler only: no driver, metadata connection or SQL execution.
SqlCheckResult checkEditorSql(const QString &, navidog::DatabaseKind,
                              const QString &database, const QString &schema,
                              const std::shared_ptr<std::atomic_bool> &cancel,
                              navidog::MetadataSnapshotPtr snapshot = {},
                              bool calciteExtensions=false);
class SqlDiagnosticController final : public QObject {
  Q_OBJECT
public:
  SqlDiagnosticController(std::function<QString()> source, QObject *parent);
  ~SqlDiagnosticController() override;
  void setContext(navidog::DatabaseKind, const QString &database,
                  const QString &schema, bool calciteExtensions);
  void invalidate();
  void setSuspended(bool suspended);
  void setMetadataSnapshot(navidog::MetadataSnapshotPtr);
signals:
  void updated(const SqlCheckResult &);

private:
  void launch();
  std::function<QString()> source_;
  QTimer timer_, expiry_;
  navidog::MetadataSnapshotPtr snapshot_;
  navidog::DatabaseKind kind_ = navidog::DatabaseKind::Unknown;
  QString database_, schema_;
  bool calciteExtensions_ = false;
  quint64 revision_ = 0;
  bool running_ = false;
  std::shared_ptr<std::atomic_bool> cancel_;
  bool suspended_ = false;
};
