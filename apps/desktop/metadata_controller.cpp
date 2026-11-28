#include "metadata_controller.hpp"
#include "navidog/query_service.hpp"
#include "query_pane.hpp"
#include "source_pane.hpp"
#include <QTabWidget>
#include <QUuid>
#include <algorithm>

namespace {
QString databaseFor(const navidog::ConnectionProfile &profile,
                    const QString &selected) {
  // Match the database used by QueryService when reading or refreshing a snapshot.
  if (selected.isEmpty() || profile.driver == navidog::DatabaseKind::SQLite ||
      navidog::usesOracleSql(profile.driver))
    return profile.database;
  return selected;
}

qint64 snapshotBytes(const navidog::MetadataSnapshot &s) {
  qint64 bytes = sizeof(s);
  for (const auto &o : s.objects) {
    bytes += sizeof(o) + 2 * (o.key.size() + o.kind.size() + o.database.size() +
                              o.schema.size() + o.name.size() + o.type.size() +
                              o.definition.size());
    for (const auto &c : o.columns)
      bytes += sizeof(c) + 2 * (c.name.size() + c.type.size() + c.mode.size());
  }
  if (s.statistics)
    for (const auto &t : s.statistics->tables) {
      bytes += sizeof(t) + 2 * (t.name.size() + t.schema.size() +
                                t.database.size() + t.provenance.size());
      for (const auto &col : t.columns)
        bytes += sizeof(col) + 2 * col.name.size();
      for (const auto &index : t.indexes) {
        bytes += sizeof(index) + 2 * index.name.size();
        for (const auto &col : index.columns)
          bytes += sizeof(col) + 2 * col.size();
      }
    }
  return bytes;
}
} // namespace

MetadataController::MetadataController(
    navidog::QueryService &service, QTabWidget &tabs,
    const QVector<navidog::ConnectionProfile> &profiles,
    const QString &cachePath, bool autoRefresh)
    : service_(service), tabs_(tabs), profiles_(profiles), autoRefresh_(autoRefresh) {
  service_.setMetadataCachePath(cachePath);

  refreshDelay_.setSingleShot(true);
  refreshDelay_.setInterval(100);
  connect(&refreshDelay_, &QTimer::timeout, this, &MetadataController::refreshCurrent);
  refreshTimer_.setInterval(15000);
  connect(&refreshTimer_, &QTimer::timeout, this, &MetadataController::refreshCurrent);
  refreshTimer_.start();

  connect(&tabs_, &QTabWidget::currentChanged, this, &MetadataController::scheduleRefresh);
  connect(&service_, &navidog::QueryService::connectionStatesChanged,
          this, &MetadataController::scheduleRefresh);
  connect(&service_, &navidog::QueryService::metadataInvalidated,
          this, &MetadataController::invalidate);
  connect(&service_, &navidog::QueryService::metadataCompleted,
          this, &MetadataController::complete);
  connect(&service_, &navidog::QueryService::completed, this, [this] {
    // The window updates each query's session and transaction state on completion.
    QTimer::singleShot(0, this, [this] {
      for (int i = 0; i < tabs_.count(); ++i)
        if (auto *pane = dynamic_cast<WorkspacePane *>(tabs_.widget(i)))
          updateContext(*pane);
      refreshCurrent();
    });
  });
  connect(&service_, &navidog::QueryService::catalogCompleted, this, [this] {
    QTimer::singleShot(0, this, &MetadataController::refreshCurrent);
  });
}

void MetadataController::setAutoRefreshEnabled(bool enabled) {
  autoRefresh_ = enabled;
  scheduleRefresh();
}

MetadataController::Context MetadataController::contextFor(const WorkspacePane &pane) const {
  Context context;
  if (auto *query = dynamic_cast<const QueryPane *>(&pane);
      query && query->kind == navidog::WorkspaceTabKind::Query) {
    context.editor = query->editor;
    context.schema = query->schema;
    context.sessionLocal = query->sessionContextChanged;
  } else if (auto *source = dynamic_cast<const SourcePane *>(&pane)) {
    context.editor = source->editor;
    context.schema = source->node.schema;
  } else {
    return context;
  }

  const auto profile = std::find_if(profiles_.cbegin(), profiles_.cend(),
                                    [&](const auto &p) { return p.id == pane.profileId; });
  if (profile != profiles_.cend() && profile->driver != navidog::DatabaseKind::Redis) {
    context.profile = &*profile;
    context.database = databaseFor(*profile, pane.database);
    context.key = navidog::metadataKey(*profile, context.database, context.schema);
  }
  return context;
}

navidog::MetadataSnapshotPtr MetadataController::snapshot(
    const navidog::ConnectionProfile &profile, const QString &database,
    const QString &schema) const {
  return snapshot(navidog::metadataKey(profile, databaseFor(profile, database), schema));
}

navidog::MetadataSnapshotPtr MetadataController::snapshot(const QString &key) const {
  const auto entry = cache_.constFind(key);
  return entry == cache_.cend() ? navidog::MetadataSnapshotPtr{} : entry->snapshot;
}

void MetadataController::removeProfile(const QString &id) {
  for (auto it = cache_.begin(); it != cache_.end();) {
    if (it->profileId == id) {
      snapshotOrder_.removeAll(it.key());
      it = cache_.erase(it);
    } else {
      ++it;
    }
  }
  for (auto it = requests_.begin(); it != requests_.end();) {
    if (it->profileId == id)
      it = requests_.erase(it);
    else
      ++it;
  }
  for (int i = 0; i < tabs_.count(); ++i) {
    auto *pane = dynamic_cast<WorkspacePane *>(tabs_.widget(i));
    if (!pane || pane->profileId != id)
      continue;
    if (auto *editor = contextFor(*pane).editor) {
      pane->setProperty("metadataKey", {});
      editor->setMetadataSnapshot({});
      editor->setMetadataSyncState({});
    }
  }
  service_.removeMetadata(id);
}

WorkspacePane *MetadataController::currentPane() const {
  return dynamic_cast<WorkspacePane *>(tabs_.currentWidget());
}

bool MetadataController::transactionOpen(const QString &id, const QString &database) const {
  for (int i = 0; i < tabs_.count(); ++i)
    if (auto *pane = dynamic_cast<QueryPane *>(tabs_.widget(i));
        pane && pane->profileId == id &&
        (pane->database == database || pane->sessionDatabase == database) &&
        (pane->executing ||
         pane->transactionState == navidog::TransactionState::Active ||
         pane->transactionState == navidog::TransactionState::Failed))
      return true;
  return false;
}

void MetadataController::updateContext(WorkspacePane &pane) {
  const auto context = contextFor(pane);
  if (!context.editor || pane.property("closingWorkspace").toBool())
    return;
  if (dynamic_cast<QueryPane *>(&pane))
    connect(context.editor, &QPlainTextEdit::textChanged, this,
            &MetadataController::editorChanged, Qt::UniqueConnection);
  if (pane.property("metadataKey").toString() != context.key)
    context.editor->setMetadataSyncState({});
  pane.setProperty("metadataKey", context.key);
  context.editor->setMetadataSnapshot(
      context.profile && !context.sessionLocal &&
              !transactionOpen(context.profile->id, context.database)
          ? snapshot(context.key)
          : navidog::MetadataSnapshotPtr{});
  if (context.profile && !cache_.contains(context.key))
    request(context, false);
}

bool MetadataController::request(const Context &context, bool refresh) {
  for (const auto &pending : requests_)
    if (pending.key == context.key)
      return false;
  const auto id = QUuid::createUuid().toString();
  if (!service_.metadata(*context.profile, context.database, context.schema, id, refresh))
    return false;
  requests_.insert(id, {context.key, context.profile->id, context.database, refresh});
  return true;
}

void MetadataController::complete(const QString &requestId, const navidog::MetadataResult &result) {
  const auto it = requests_.find(requestId);
  if (it == requests_.end())
    return;
  const auto request = it.value();
  requests_.erase(it);

  auto &entry = cache_[request.key];
  entry.profileId = request.profileId;
  if (result.snapshot) {
    entry.snapshot = result.snapshot;
    snapshotOrder_.removeAll(request.key);
    snapshotOrder_ << request.key;
  }
  if (!request.refresh) {
    scheduleRefresh();
  } else if (!result.error.isEmpty() || result.cancelled || !result.snapshot) {
    entry.failures = std::min(5, entry.failures + 1);
    entry.retryAt = QDateTime::currentMSecsSinceEpoch() +
                    std::min(300, 15 * (1 << entry.failures)) * 1000LL;
  } else {
    entry.failures = 0;
    entry.retryAt = 0;
  }

  if (result.snapshot)
    evictSnapshots();
  publish(request.key, request.database, request.refresh ? &result : nullptr);
}

void MetadataController::invalidate(const QString &id, const QString &database) {
  for (auto it = cache_.begin(); it != cache_.end(); ++it) {
    const auto &snapshot = it->snapshot;
    if (!snapshot || snapshot->profileId != id || snapshot->invalidated ||
        (!database.isEmpty() && snapshot->database != database))
      continue;
    auto invalidated = std::make_shared<navidog::MetadataSnapshot>(*snapshot);
    invalidated->invalidated = true;
    it->snapshot = std::move(invalidated);
    publish(it.key(), it->snapshot->database);
  }
}

void MetadataController::evictSnapshots() {
  qint64 bytes = 0;
  for (const auto &key : snapshotOrder_)
    bytes += snapshotBytes(*snapshot(key));
  while (snapshotOrder_.size() > 4 || bytes > 32 * 1024 * 1024) {
    const auto key = snapshotOrder_.takeFirst();
    const auto entry = cache_.take(key);
    bytes -= snapshotBytes(*entry.snapshot);
    publish(key, entry.snapshot->database);
  }
}

void MetadataController::publish(const QString &key, const QString &database,
                                 const navidog::MetadataResult *refreshResult) {
  const auto cached = snapshot(key);
  const bool transaction = cached && transactionOpen(cached->profileId, database);
  for (int i = 0; i < tabs_.count(); ++i) {
    auto *pane = dynamic_cast<WorkspacePane *>(tabs_.widget(i));
    if (!pane || pane->property("metadataKey").toString() != key)
      continue;
    const auto context = contextFor(*pane);
    if (!context.editor)
      continue;
    if (refreshResult) {
      context.editor->setProperty("metadataSyncError", refreshResult->error);
      context.editor->setMetadataSyncState(refreshResult->error.isEmpty()
                                               ? QString{}
                                               : "后台校验失败，将自动重试");
    }
    context.editor->setMetadataSnapshot(
        context.sessionLocal || transaction ? navidog::MetadataSnapshotPtr{} : cached);
  }
}

void MetadataController::editorChanged() {
  const auto *pane = dynamic_cast<const QueryPane *>(tabs_.currentWidget());
  if (pane && pane->editor == sender())
    scheduleRefresh();
}

void MetadataController::scheduleRefresh() {
  if (autoRefresh_ && !refreshDelay_.isActive())
    refreshDelay_.start();
}

void MetadataController::refreshCurrent() {
  auto *pane = currentPane();
  if (!autoRefresh_ || !pane || pane->property("closingWorkspace").toBool())
    return;
  const auto context = contextFor(*pane);
  if (!context.editor || !context.profile)
    return;
  if (pane->property("metadataKey").toString() != context.key) {
    updateContext(*pane);
    return;
  }
  if (context.sessionLocal || transactionOpen(context.profile->id, context.database)) {
    context.editor->setMetadataSnapshot({});
    return;
  }
  const auto entry = cache_.constFind(context.key);
  if (entry != cache_.cend()) {
    if (entry->retryAt > QDateTime::currentMSecsSinceEpoch())
      return;
    if (entry->snapshot && entry->snapshot->usable()) {
      context.editor->setMetadataSnapshot(entry->snapshot);
      context.editor->setMetadataSyncState({});
      return;
    }
  }
  if (!service_.isConnected(context.profile->id)) {
    context.editor->setMetadataSyncState("连接后自动校验对象");
    return;
  }
  if (!service_.busy() && request(context, true))
    context.editor->setMetadataSyncState("正在后台校验对象");
}
