#pragma once

#include "navidog/metadata_cache.hpp"
#include <QHash>
#include <QObject>
#include <QTimer>

class QTabWidget;
class SqlEditor;
class WorkspacePane;
namespace navidog {
class QueryService;
}

class MetadataController final : public QObject {
public:
  MetadataController(navidog::QueryService &, QTabWidget &,
                     const QVector<navidog::ConnectionProfile> &,
                     const QString &cachePath, bool autoRefresh);

  void updateContext(WorkspacePane &);
  void setAutoRefreshEnabled(bool enabled);
  navidog::MetadataSnapshotPtr snapshot(const navidog::ConnectionProfile &,
                                        const QString &database,
                                        const QString &schema) const;
  void removeProfile(const QString &id);

private:
  struct Context {
    SqlEditor *editor = nullptr;
    const navidog::ConnectionProfile *profile = nullptr;
    QString key, database, schema;
    bool sessionLocal = false;
  };
  struct Request {
    QString key, profileId, database;
    bool refresh;
  };
  struct CacheEntry {
    QString profileId;
    navidog::MetadataSnapshotPtr snapshot;
    qint64 retryAt = 0;
    int failures = 0;
  };

  navidog::QueryService &service_;
  QTabWidget &tabs_;
  const QVector<navidog::ConnectionProfile> &profiles_;
  bool autoRefresh_;
  QTimer refreshDelay_, refreshTimer_;
  QHash<QString, Request> requests_;
  QHash<QString, CacheEntry> cache_;
  QStringList snapshotOrder_;

  Context contextFor(const WorkspacePane &) const;
  navidog::MetadataSnapshotPtr snapshot(const QString &key) const;
  WorkspacePane *currentPane() const;
  bool transactionOpen(const QString &id, const QString &database) const;
  bool request(const Context &, bool refresh);
  void complete(const QString &requestId, const navidog::MetadataResult &);
  void invalidate(const QString &id, const QString &database);
  void evictSnapshots();
  void publish(const QString &key, const QString &database,
               const navidog::MetadataResult *refreshResult = nullptr);
  void editorChanged();
  void scheduleRefresh();
  void refreshCurrent();
};
