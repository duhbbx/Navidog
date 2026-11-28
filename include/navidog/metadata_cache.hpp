#pragma once
#include "database.hpp"
#include "planner_statistics.hpp"
#include "sql/compiler/compiler.hpp"
#include <QDateTime>
#include <QStringList>
#include <memory>
#include <unordered_map>
#include <map>
namespace navidog {
struct MetadataColumn {
  QString name, type, mode;
  int ordinal = 0;
  bool input = true, hasDefault = false, variadic = false;
  bool nonNullKnown=false; // Verified immediate constraint on an ordinary table column.
};
struct MetadataObject {
  QString key, kind, database, schema, name, type;
  QVector<MetadataColumn> columns;
  int minArgs = 0, maxArgs = 0;
  bool variadic = false;
  sql::compiler::RoutineKind routineKind=sql::compiler::RoutineKind::Unknown;
  sql::compiler::FunctionVolatility volatility=sql::compiler::FunctionVolatility::Unknown;
  QStringList primaryKey;
  bool noDescendantsKnown = false;
  bool rowSecurityDisabledKnown = false;
  QVector<QStringList> strictUniqueKeys; // Verified secondary keys with immediate integer equality.
  QString definition;
};
struct MetadataLookupIndex;
class MetadataLookupSlot {
  friend class MetadataCatalog;
  mutable std::shared_ptr<const MetadataLookupIndex> index_;

public:
  MetadataLookupSlot() = default;
  // A copied snapshot can be invalidated or rebuilt before publication.
  MetadataLookupSlot(const MetadataLookupSlot &) noexcept {}
  MetadataLookupSlot &operator=(const MetadataLookupSlot &) noexcept {
    reset();
    return *this;
  }
  void reset() noexcept {
    std::atomic_store(&index_,std::shared_ptr<const MetadataLookupIndex>{});
  }
};
struct MetadataSnapshot {
  QString key, profileId, identity, database, schema, serverVersion;
  DatabaseKind driver = DatabaseKind::Unknown;
  QStringList searchPath;
  QVector<MetadataObject> objects;
  std::shared_ptr<const PlannerStatisticsSnapshot> statistics;
  // Derived once on the worker from native type strings; not persisted or rebuilt per keystroke.
  std::map<std::string,sql::compiler::SqlType> typeFacts;
  MetadataLookupSlot lookup;
  QString statisticsWarning,objectWarning;
  qint64 refreshedMs = 0;
  bool tablesComplete = false, columnsComplete = false,
       routinesComplete = false;
  bool definitionsRead = false, definitionsComplete = false;
  bool caseSensitive = true, invalidated = false, verified = false;
  static constexpr qint64 ttlMs = 5 * 60 * 1000;
  bool usable(qint64 now = QDateTime::currentMSecsSinceEpoch()) const {
    return verified && !invalidated && refreshedMs <= now &&
           now - refreshedMs < ttlMs;
  }
};
using MetadataSnapshotPtr = std::shared_ptr<const MetadataSnapshot>;
void prepareMetadataTypeFacts(MetadataSnapshot &,const Cancellation & = {});
struct MetadataResult {
  MetadataSnapshotPtr snapshot;
  QString error;
  bool cancelled = false, fromDisk = false;
};
struct CachedObjectName {
  QString database, schema, kind, name, parent;
};
QString metadataIdentity(const ConnectionProfile &);
QString metadataKey(const ConnectionProfile &, const QString &database,
                    const QString &schema);
MetadataSnapshot collectMetadata(Database &, const ConnectionProfile &,
                                 const QString &schema, const Cancellation &, bool searchOnly = false);
// Each instance belongs to its caller's worker thread. SQLite handles never
// cross threads.
class MetadataStore {
public:
  explicit MetadataStore(const QString &path);
  ~MetadataStore();
  MetadataStore(const MetadataStore &) = delete;
  MetadataSnapshotPtr load(const QString &key);
  QVector<QPair<QString, QString>> searchScopes(const ConnectionProfile&);
  QVector<CachedObjectName> searchObjectNames(const QString &profileId,
                                              const QString &identity,
                                              const QString &term, int limit);
  qint64 generation(const QString& profileId);
  void replace(const MetadataSnapshot &, qint64 expectedGeneration = -1);
  void invalidate(const QString &profileId, const QString &database = {});
  void removeProfile(const QString &profileId);

private:
  struct Impl;
  std::unique_ptr<Impl> d;
};
class MetadataCatalog final : public sql::compiler::CatalogSnapshot {
public:
  explicit MetadataCatalog(MetadataSnapshotPtr);
  std::vector<sql::compiler::TableDefinition> listRelations(const sql::compiler::NameResolutionContext&,std::size_t)const override;
  std::vector<sql::compiler::RoutineDefinition> listRoutines(const sql::compiler::NameResolutionContext&,bool,std::size_t)const override;
  std::vector<sql::compiler::TableDefinition> matchingRelations(const sql::compiler::NameResolutionContext&,
      std::size_t,const sql::compiler::CatalogNameFilter &) const override;
  std::vector<sql::compiler::RoutineDefinition> matchingRoutines(const sql::compiler::NameResolutionContext&,
      bool,std::size_t,const sql::compiler::CatalogNameFilter &) const override;
  std::vector<sql::compiler::CatalogObjectDefinition> listObjects(const sql::compiler::NameResolutionContext&,sql::compiler::CatalogObjectKind,std::size_t) const override;
  bool current()const override {return snapshot_ && snapshot_->usable();}
  std::optional<sql::compiler::TableDefinition>
  findTable(const sql::compiler::NameResolutionContext &,
            const std::vector<std::string> &) const override;
  bool relationNameEquals(std::string_view, std::string_view) const override;
  bool columnNameEquals(std::string_view, std::string_view) const override;
  bool tableLookupComplete(const sql::compiler::NameResolutionContext &,
                           const std::vector<std::string> &) const override;
  std::vector<sql::compiler::RoutineDefinition>
  findRoutines(const sql::compiler::NameResolutionContext &,
               const std::vector<std::string> &, bool procedure) const override;
  bool routineLookupComplete(const sql::compiler::NameResolutionContext &,
                             const std::vector<std::string> &,
                             bool procedure) const override;

private:
  MetadataSnapshotPtr snapshot_;
  std::shared_ptr<const MetadataLookupIndex> lookup_;
  std::vector<std::string>
  resolvedSchemas(const sql::compiler::NameResolutionContext &,
                  const std::vector<std::string> &) const;
  std::string normalized(const std::string &) const;
};
} // namespace navidog
Q_DECLARE_METATYPE(navidog::MetadataSnapshotPtr)
Q_DECLARE_METATYPE(navidog::MetadataResult)
