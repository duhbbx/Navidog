#include "navidog/metadata_cache.hpp"
#include "metadata_relation_proofs.hpp"
#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QHash>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QUrl>
#include <sqlite3.h>
#include <stdexcept>
namespace navidog {
namespace {
void check(int code, sqlite3 *db) {
  if (code != SQLITE_OK && code != SQLITE_DONE && code != SQLITE_ROW)
    throw std::runtime_error(sqlite3_errmsg(db));
}
struct Stmt {
  sqlite3 *db;
  sqlite3_stmt *s = nullptr;
  Stmt(sqlite3 *db, const char *sql) : db(db) {
    check(sqlite3_prepare_v2(db, sql, -1, &s, nullptr), db);
  }
  ~Stmt() { sqlite3_finalize(s); }
  void bind(int i, const QString &value) {
    const auto bytes = value.toUtf8();
    check(sqlite3_bind_text(s, i, bytes.constData(), bytes.size(),
                            SQLITE_TRANSIENT),
          db);
  }
  void bind(int i, qint64 value) { check(sqlite3_bind_int64(s, i, value), db); }
  bool row() {
    auto code = sqlite3_step(s);
    check(code, db);
    return code == SQLITE_ROW;
  }
  QString text(int i) {
    if (sqlite3_column_bytes(s, i) > 65536)
      throw std::runtime_error("metadata cache cell limit exceeded");
    return QString::fromUtf8(
        reinterpret_cast<const char *>(sqlite3_column_text(s, i)));
  }
  qint64 number(int i) { return sqlite3_column_int64(s, i); }
  void done() {
    check(sqlite3_step(s), db);
    check(sqlite3_reset(s), db);
    sqlite3_clear_bindings(s);
  }
};
QString hash(const QJsonArray &parts) {
  return QString::fromLatin1(
      QCryptographicHash::hash(
          QJsonDocument(parts).toJson(QJsonDocument::Compact),
          QCryptographicHash::Sha256)
          .toHex());
}
} // namespace
QString metadataIdentity(const ConnectionProfile &p) {
  // Deliberately excludes all secrets, SQL text and transient tunnel ports.
  return hash({databaseKindKey(p.driver), p.host, p.port, p.user,
               p.advanced.socketFile, p.ssh.enabled, p.ssh.host, p.ssh.port,
               p.ssh.user, p.http.enabled,
               QUrl(p.http.url)
                   .adjusted(QUrl::RemoveUserInfo | QUrl::RemoveQuery |
                             QUrl::RemoveFragment)
                   .toString(),
               int(p.oracle.addressKind), int(p.oracle.role),
               p.oracle.configDirectory, p.oracle.externalAuthentication});
}
QString metadataKey(const ConnectionProfile &p, const QString &database,
                    const QString &schema) {
  return hash({p.id, metadataIdentity(p), database, schema});
}
struct MetadataStore::Impl {
  sqlite3 *db = nullptr;
  QString path;
  void exec(const char *sql) {
    char *error = nullptr;
    auto code = sqlite3_exec(db, sql, nullptr, nullptr, &error);
    QString detail = QString::fromUtf8(error ? error : "");
    sqlite3_free(error);
    if (code != SQLITE_OK)
      throw std::runtime_error(detail.toStdString());
  }
  ~Impl() {
    if (db)
      sqlite3_close(db);
  }
};
MetadataStore::MetadataStore(const QString &path)
    : d(std::make_unique<Impl>()) {
  d->path = path;
  auto parent = QFileInfo(path).absolutePath();
  if (!QDir().mkpath(parent))
    throw std::runtime_error("cannot create metadata cache directory");
  if (QFileInfo(parent).fileName() == "metadata")
    QFile::setPermissions(parent, QFileDevice::ReadOwner |
                                      QFileDevice::WriteOwner |
                                      QFileDevice::ExeOwner);
  auto bytes = QFile::encodeName(path);
  check(sqlite3_open_v2(bytes.constData(), &d->db,
                        SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE |
                            SQLITE_OPEN_NOMUTEX,
                        nullptr),
        d->db);
  sqlite3_busy_timeout(d->db, 2000);
  {
    Stmt version(d->db, "PRAGMA user_version");
    version.row();
    if (version.number(0) > 1)
      throw std::runtime_error(
          "newer metadata cache format; cache left unchanged");
  }
  int pageSize = 4096;
  {
    Stmt pages(d->db, "PRAGMA page_size");
    pages.row();
    pageSize = int(pages.number(0));
  }
  if (pageSize < 512 || pageSize > 65536)
    throw std::runtime_error("invalid metadata cache page size");
  const auto pageLimit = (64 * 1024 * 1024) / pageSize;
  {
    Stmt pages(d->db, "PRAGMA page_count");
    pages.row();
    if (pages.number(0) > pageLimit)
      throw std::runtime_error("metadata cache exceeds disk budget");
  }
  const auto pragmas =
      std::string("PRAGMA foreign_keys=ON;PRAGMA journal_mode=WAL;PRAGMA "
                  "journal_size_limit=8388608;PRAGMA max_page_count=") +
      std::to_string(pageLimit) + ";";
  d->exec(pragmas.c_str());
  d->exec("CREATE TABLE IF NOT EXISTS scopes(cache_key TEXT PRIMARY "
          "KEY,profile_id TEXT NOT NULL,identity TEXT NOT NULL,database_name "
          "TEXT,schema_name TEXT,driver TEXT,server_version TEXT,refreshed_ms "
          "INTEGER,invalidated INTEGER,tables_complete "
          "INTEGER,columns_complete INTEGER,routines_complete "
          "INTEGER,case_sensitive INTEGER,search_path TEXT,last_used INTEGER);"
          "CREATE INDEX IF NOT EXISTS scope_profile ON "
          "scopes(profile_id,database_name);"
          "CREATE TABLE IF NOT EXISTS objects(scope TEXT NOT NULL REFERENCES "
          "scopes(cache_key) ON DELETE CASCADE,object_key TEXT NOT NULL,kind "
          "TEXT,database_name TEXT,schema_name TEXT,name TEXT,type "
          "TEXT,min_args INTEGER,max_args INTEGER,variadic INTEGER,PRIMARY "
          "KEY(scope,object_key)) WITHOUT ROWID;"
          "CREATE INDEX IF NOT EXISTS object_lookup ON "
          "objects(scope,kind,schema_name,name);"
          "CREATE TABLE IF NOT EXISTS members(scope TEXT NOT NULL,object_key "
          "TEXT NOT NULL,ordinal INTEGER,name TEXT,type TEXT,mode "
          "TEXT,is_input INTEGER,has_default INTEGER,variadic INTEGER,PRIMARY "
          "KEY(scope,object_key,ordinal),FOREIGN KEY(scope,object_key) "
          "REFERENCES objects(scope,object_key) ON DELETE CASCADE) WITHOUT "
          "ROWID;"
          "CREATE TABLE IF NOT EXISTS planner_stat_scopes(scope TEXT PRIMARY "
          "KEY REFERENCES scopes(cache_key) ON DELETE CASCADE,refreshed_ms "
          "INTEGER,complete INTEGER,warning TEXT);"
          "CREATE TABLE IF NOT EXISTS planner_stat_tables(scope TEXT NOT NULL "
          "REFERENCES scopes(cache_key) ON DELETE CASCADE,database_name TEXT "
          "NOT NULL,schema_name TEXT NOT NULL,name TEXT NOT NULL,payload TEXT "
          "NOT NULL CHECK(length(payload)<=262144),PRIMARY "
          "KEY(scope,database_name,schema_name,name)) WITHOUT ROWID;"
          "CREATE TABLE IF NOT EXISTS routine_properties(scope TEXT NOT NULL,object_key TEXT NOT NULL,"
          "routine_kind INTEGER NOT NULL,volatility INTEGER NOT NULL,PRIMARY KEY(scope,object_key),"
          "FOREIGN KEY(scope,object_key) REFERENCES objects(scope,object_key) ON DELETE CASCADE) WITHOUT ROWID;"
          "CREATE TABLE IF NOT EXISTS member_constraints(scope TEXT NOT NULL,object_key TEXT NOT NULL,ordinal INTEGER NOT NULL,"
          "not_null_known INTEGER NOT NULL CHECK(not_null_known IN(0,1)),PRIMARY KEY(scope,object_key,ordinal),"
          "FOREIGN KEY(scope,object_key,ordinal) REFERENCES members(scope,object_key,ordinal) ON DELETE CASCADE) WITHOUT ROWID;"
          "CREATE TABLE IF NOT EXISTS table_proofs(scope TEXT NOT NULL,object_key TEXT NOT NULL,"
          "no_descendants_known INTEGER NOT NULL CHECK(no_descendants_known IN(0,1)),"
          "primary_key_json TEXT NOT NULL CHECK(length(primary_key_json)<=65536),"
          "PRIMARY KEY(scope,object_key),FOREIGN KEY(scope,object_key) "
          "REFERENCES objects(scope,object_key) ON DELETE CASCADE) WITHOUT ROWID;"
          "CREATE TABLE IF NOT EXISTS row_security_disabled(scope TEXT NOT NULL,object_key TEXT NOT NULL,"
          "PRIMARY KEY(scope,object_key),FOREIGN KEY(scope,object_key) "
          "REFERENCES objects(scope,object_key) ON DELETE CASCADE) WITHOUT ROWID;"
          "CREATE TABLE IF NOT EXISTS definition_scopes(scope TEXT PRIMARY KEY REFERENCES scopes(cache_key) "
          "ON DELETE CASCADE,complete INTEGER NOT NULL);"
          "CREATE TABLE IF NOT EXISTS object_definitions(scope TEXT NOT NULL,object_key TEXT NOT NULL,"
          "source TEXT NOT NULL CHECK(length(source)<=65536),PRIMARY KEY(scope,object_key),"
          "FOREIGN KEY(scope,object_key) REFERENCES objects(scope,object_key) ON DELETE CASCADE) WITHOUT ROWID;"
          "CREATE TABLE IF NOT EXISTS metadata_generations(profile_id TEXT PRIMARY KEY,generation INTEGER NOT NULL);"
          "PRAGMA user_version=1;");
  for (const auto &suffix : {QString{}, QString("-wal"), QString("-shm")})
    QFile::setPermissions(path + suffix,
                          QFileDevice::ReadOwner | QFileDevice::WriteOwner);
}
MetadataStore::~MetadataStore() = default;
MetadataSnapshotPtr MetadataStore::load(const QString &key) {
  auto out = std::make_shared<MetadataSnapshot>();
  Stmt scope(
      d->db,
      "SELECT "
      "profile_id,identity,database_name,schema_name,driver,server_version,"
      "refreshed_ms,invalidated,tables_complete,columns_complete,routines_"
      "complete,case_sensitive,search_path FROM scopes WHERE cache_key=?");
  scope.bind(1, key);
  if (!scope.row())
    return {};
  out->key = key;
  out->profileId = scope.text(0);
  out->identity = scope.text(1);
  out->database = scope.text(2);
  out->schema = scope.text(3);
  out->driver = databaseKindFromKey(scope.text(4));
  out->serverVersion = scope.text(5);
  out->refreshedMs = scope.number(6);
  out->invalidated = scope.number(7);
  out->tablesComplete = scope.number(8);
  out->columnsComplete = scope.number(9);
  out->routinesComplete = scope.number(10);
  out->caseSensitive = scope.number(11);
  for (const auto &value :
       QJsonDocument::fromJson(scope.text(12).toUtf8()).array())
    out->searchPath << value.toString();
  Stmt objects(d->db,"SELECT o.object_key,o.kind,o.database_name,o.schema_name,o.name,o.type,o.min_args,o.max_args,"
             "o.variadic,p.routine_kind,p.volatility,t.no_descendants_known,t.primary_key_json,s.object_key IS NOT NULL "
             "FROM objects o LEFT JOIN routine_properties p ON p.scope=o.scope AND p.object_key=o.object_key "
             "LEFT JOIN table_proofs t ON t.scope=o.scope AND t.object_key=o.object_key "
             "LEFT JOIN row_security_disabled s ON s.scope=o.scope AND s.object_key=o.object_key "
             "WHERE o.scope=? ORDER BY o.object_key");
  objects.bind(1, key);
  QHash<QString, int> positions;
  qint64 loadedBytes = 0;
  while (objects.row()) {
    if (out->objects.size() >= 20000)
      throw std::runtime_error("metadata cache object limit exceeded");
    MetadataObject o;
    o.key = objects.text(0);
    o.kind = objects.text(1);
    o.database = objects.text(2);
    o.schema = objects.text(3);
    o.name = objects.text(4);
    o.type = objects.text(5);
    o.minArgs = objects.number(6);
    o.maxArgs = objects.number(7);
    o.variadic = objects.number(8);
    const auto kind=objects.number(9),volatility=objects.number(10);
    if(kind>=0 && kind<=static_cast<int>(sql::compiler::RoutineKind::TableFunction))o.routineKind=static_cast<sql::compiler::RoutineKind>(kind);
    if(volatility>=0 && volatility<=static_cast<int>(sql::compiler::FunctionVolatility::Volatile))o.volatility=static_cast<sql::compiler::FunctionVolatility>(volatility);
    if (o.kind == "table") {
      o.noDescendantsKnown = objects.number(11) == 1;
      o.rowSecurityDisabledKnown = objects.number(13) == 1;
      const auto keyPayload = objects.text(12);
      loadedBytes += 2LL * keyPayload.size();
      auto keys=metadata_detail::decodeTableKeys(keyPayload);
      o.primaryKey=std::move(keys.primary);
      o.strictUniqueKeys=std::move(keys.secondary);
    }
    positions.insert(o.key, out->objects.size());
    loadedBytes += 2LL * (o.key.size() + o.kind.size() + o.database.size() +
                          o.schema.size() + o.name.size() + o.type.size());
    if (loadedBytes > 16 * 1024 * 1024)
      throw std::runtime_error("metadata cache snapshot byte limit exceeded");
    out->objects << o;
  }
  Stmt members(
      d->db,
      "SELECT m.object_key,m.ordinal,m.name,m.type,m.mode,m.is_input,m.has_default,m.variadic,COALESCE(c.not_null_known,0) "
      "FROM members m LEFT JOIN member_constraints c ON c.scope=m.scope AND c.object_key=m.object_key AND c.ordinal=m.ordinal "
      "WHERE m.scope=? ORDER BY m.object_key,m.ordinal");
  members.bind(1, key);
  int count = 0;
  while (members.row()) {
    if (++count > 100000)
      throw std::runtime_error("metadata cache member limit exceeded");
    auto found = positions.constFind(members.text(0));
    if (found == positions.cend())
      continue;
    MetadataColumn c;
    c.ordinal = members.number(1);
    c.name = members.text(2);
    c.type = members.text(3);
    c.mode = members.text(4);
    c.input = members.number(5);
    c.hasDefault = members.number(6);
    c.variadic = members.number(7);
    c.nonNullKnown=members.number(8)==1;
    loadedBytes += 2LL * (c.name.size() + c.type.size() + c.mode.size());
    if (loadedBytes > 16 * 1024 * 1024)
      throw std::runtime_error("metadata cache snapshot byte limit exceeded");
    out->objects[*found].columns << c;
  }
  {
    Stmt definitions(d->db, "SELECT complete FROM definition_scopes WHERE scope=?");
    definitions.bind(1, key);
    if (definitions.row()) {
      out->definitionsRead = true;
      out->definitionsComplete = definitions.number(0);
      Stmt source(d->db, "SELECT object_key,source FROM object_definitions WHERE scope=?");
      source.bind(1, key);
      while (source.row()) {
        const auto index = positions.value(source.text(0), -1);
        if (index < 0) continue;
        auto text = source.text(1);
        loadedBytes += 2LL * text.size();
        if (loadedBytes > 24 * 1024 * 1024)
          throw std::runtime_error("metadata definitions exceed cache byte limit");
        out->objects[index].definition = std::move(text);
      }
    }
  }
  try {
    Stmt statScope(d->db, "SELECT refreshed_ms,complete,warning FROM "
                          "planner_stat_scopes WHERE scope=?");
    statScope.bind(1, key);
    if (statScope.row()) {
      auto stats = std::make_shared<PlannerStatisticsSnapshot>();
      stats->refreshedMs = statScope.number(0);
      stats->complete = statScope.number(1);
      stats->warning = statScope.text(2);
      out->statisticsWarning = stats->warning;
      Stmt tables(d->db, "SELECT payload FROM planner_stat_tables WHERE "
                         "scope=? ORDER BY database_name,schema_name,name");
      tables.bind(1, key);
      qint64 bytes = 0;
      while (tables.row()) {
        const auto payload = tables.text(0).toUtf8();
        bytes += payload.size();
        if (bytes > 2 * 1024 * 1024 || stats->tables.size() >= 4096)
          throw std::runtime_error("local planner statistics budget exceeded");
        auto document = QJsonDocument::fromJson(payload);
        if (!document.isObject())
          throw std::runtime_error("invalid planner statistics JSON");
        stats->tables.push_back(plannerTableFromJson(document.object()));
      }
      out->statistics = stats;
    }
  } catch (const std::exception &e) {
    out->statistics.reset();
    out->statisticsWarning = QString::fromUtf8(e.what()).left(1024);
  }
  // Release the read snapshot before starting the LRU write. A concurrent
  // workspace write can make upgrading the WAL read snapshot fail immediately.
  check(sqlite3_reset(scope.s), d->db);
  Stmt touch(d->db, "UPDATE scopes SET last_used=? WHERE cache_key=?");
  touch.bind(1, QDateTime::currentMSecsSinceEpoch());
  touch.bind(2, key);
  touch.done();
  prepareMetadataTypeFacts(*out);
  return out; // disk snapshots intentionally unverified
}
QVector<QPair<QString, QString>> MetadataStore::searchScopes(const ConnectionProfile& profile) {
  Stmt query(d->db,"SELECT DISTINCT s.database_name,s.schema_name FROM scopes s "
                    "JOIN definition_scopes d ON d.scope=s.cache_key WHERE s.profile_id=? AND s.identity=? "
                    "AND s.invalidated=0 AND s.refreshed_ms>=? AND s.refreshed_ms<=? "
                    "ORDER BY s.database_name,s.schema_name");
  const auto now = QDateTime::currentMSecsSinceEpoch();
  query.bind(1, profile.id);
  query.bind(2, metadataIdentity(profile));
  query.bind(3, now - MetadataSnapshot::ttlMs);
  query.bind(4, now);
  QVector<QPair<QString, QString>> scopes;
  while (query.row()) scopes.push_back({query.text(0), query.text(1)});
  return scopes;
}
QVector<CachedObjectName> MetadataStore::searchObjectNames(
    const QString &profileId, const QString &identity, const QString &term,
    int limit) {
  if (term.isEmpty() || term.size() > 512 || limit <= 0)
    return {};
  // Read names directly from disk without loading definitions, statistics or types.
  // Older snapshots can still be displayed as navigation hints until invalidated.
  Stmt matches(d->db, R"sql(
    WITH objects_in_profile AS (
      SELECT o.scope, o.object_key, o.database_name, o.schema_name, o.kind, o.name
      FROM scopes s JOIN objects o ON o.scope=s.cache_key
      WHERE s.profile_id=?1 AND s.identity=?2 AND s.invalidated=0
    )
    SELECT CASE kind WHEN 'database' THEN name ELSE database_name END AS database_name,
           CASE kind WHEN 'schema' THEN name ELSE schema_name END AS schema_name,
           kind, name, '' AS parent
    FROM objects_in_profile
    WHERE instr(lower(name),lower(?3))>0
    UNION
    SELECT o.database_name, o.schema_name, 'column', m.name, o.name
    FROM objects_in_profile o JOIN members m ON m.scope=o.scope AND m.object_key=o.object_key
    WHERE o.kind IN ('table','view','materializedView') AND instr(lower(m.name),lower(?3))>0
    ORDER BY name, database_name, schema_name, kind, parent
    LIMIT ?4
  )sql");
  matches.bind(1, profileId);
  matches.bind(2, identity);
  matches.bind(3, term);
  matches.bind(4, qMin(limit, 201));
  QVector<CachedObjectName> result;
  while (matches.row())
    result.push_back({matches.text(0), matches.text(1), matches.text(2),
                      matches.text(3), matches.text(4)});
  return result;
}
qint64 MetadataStore::generation(const QString& profileId) {
  Stmt query(d->db,"SELECT generation FROM metadata_generations WHERE profile_id=?");
  query.bind(1, profileId);
  return query.row() ? query.number(0) : 0;
}
void MetadataStore::replace(const MetadataSnapshot &s, qint64 expectedGeneration) {
  d->exec("BEGIN IMMEDIATE");
  try {
    if (expectedGeneration >= 0 && generation(s.profileId) != expectedGeneration)
      throw std::runtime_error("metadata invalidated during collection");
    Stmt remove(d->db, "DELETE FROM scopes WHERE cache_key=?");
    remove.bind(1, s.key);
    remove.done();
    Stmt scope(d->db,
               "INSERT INTO scopes VALUES(?,?,?,?,?,?,?,?,?,?,?,?,?,?,?)");
    scope.bind(1, s.key);
    scope.bind(2, s.profileId);
    scope.bind(3, s.identity);
    scope.bind(4, s.database);
    scope.bind(5, s.schema);
    scope.bind(6, databaseKindKey(s.driver));
    scope.bind(7, s.serverVersion);
    scope.bind(8, s.refreshedMs);
    scope.bind(9, s.invalidated);
    scope.bind(10, s.tablesComplete);
    scope.bind(11, s.columnsComplete);
    scope.bind(12, s.routinesComplete);
    scope.bind(13, s.caseSensitive);
    QJsonArray path;
    for (const auto &p : s.searchPath)
      path << p;
    scope.bind(14, QString::fromUtf8(
                       QJsonDocument(path).toJson(QJsonDocument::Compact)));
    scope.bind(15, QDateTime::currentMSecsSinceEpoch());
    scope.done();
    if (s.definitionsRead) {
      Stmt definitions(d->db, "INSERT INTO definition_scopes VALUES(?,?)");
      definitions.bind(1, s.key);
      definitions.bind(2, s.definitionsComplete);
      definitions.done();
    }
    Stmt object(d->db, "INSERT INTO objects VALUES(?,?,?,?,?,?,?,?,?,?)"),
        member(d->db, "INSERT INTO members VALUES(?,?,?,?,?,?,?,?,?)"),
        property(d->db,"INSERT INTO routine_properties VALUES(?,?,?,?)"),
        constraint(d->db,"INSERT INTO member_constraints VALUES(?,?,?,1)"),
        tableProof(d->db,"INSERT INTO table_proofs VALUES(?,?,?,?)"),
        rowSecurity(d->db,"INSERT INTO row_security_disabled VALUES(?,?)"),
        source(d->db,"INSERT INTO object_definitions VALUES(?,?,?)");
    if (s.objects.size() > 20000)
      throw std::runtime_error("metadata cache object limit exceeded");
    int count = 0;
    qint64 bytes = 0;
    for (const auto &o : s.objects) {
      bytes += (o.name.size() + o.type.size() + o.key.size() + o.schema.size() +
                o.database.size() + o.kind.size()) *
               2LL;
      if (bytes > 16 * 1024 * 1024)
        throw std::runtime_error("metadata cache snapshot byte limit exceeded");
      object.bind(1, s.key);
      object.bind(2, o.key);
      object.bind(3, o.kind);
      object.bind(4, o.database);
      object.bind(5, o.schema);
      object.bind(6, o.name);
      object.bind(7, o.type);
      object.bind(8, o.minArgs);
      object.bind(9, o.maxArgs);
      object.bind(10, o.variadic);
      object.done();
      if (!o.definition.isEmpty() && s.definitionsRead) {
        source.bind(1, s.key);
        source.bind(2, o.key);
        source.bind(3, o.definition);
        source.done();
      }
      if(o.routineKind!=sql::compiler::RoutineKind::Unknown || o.volatility!=sql::compiler::FunctionVolatility::Unknown) {
        property.bind(1,s.key);property.bind(2,o.key);property.bind(3,static_cast<int>(o.routineKind));property.bind(4,static_cast<int>(o.volatility));property.done();
      }
      if (o.kind == "table" && (o.noDescendantsKnown || !o.primaryKey.isEmpty())) {
        const auto payload=metadata_detail::encodeTableKeys(o.primaryKey,{});
        bytes += 2LL * payload.size();
        if (payload.size() > 65536 || bytes > 16 * 1024 * 1024)
          throw std::runtime_error("metadata table key proofs exceed cache budget");
        tableProof.bind(1, s.key);
        tableProof.bind(2, o.key);
        tableProof.bind(3, o.noDescendantsKnown);
        tableProof.bind(4, payload);
        tableProof.done();
      }
      if (o.kind == "table" && o.rowSecurityDisabledKnown) {
        rowSecurity.bind(1, s.key);
        rowSecurity.bind(2, o.key);
        rowSecurity.done();
      }
      for (const auto &c : o.columns) {
        if (++count > 100000)
          throw std::runtime_error("metadata cache member limit exceeded");
        bytes += (c.name.size() + c.type.size() + c.mode.size()) * 2;
        if (bytes > 16 * 1024 * 1024)
          throw std::runtime_error(
              "metadata cache snapshot byte limit exceeded");
        member.bind(1, s.key);
        member.bind(2, o.key);
        member.bind(3, c.ordinal);
        member.bind(4, c.name);
        member.bind(5, c.type);
        member.bind(6, c.mode);
        member.bind(7, c.input);
        member.bind(8, c.hasDefault);
        member.bind(9, c.variadic);
        member.done();
        if(c.nonNullKnown && o.kind=="table") {constraint.bind(1,s.key);constraint.bind(2,o.key);constraint.bind(3,c.ordinal);constraint.done();}
      }
    }
    // Store optional proofs only after all required names and fields fit.
    // A large secondary index collection cannot invalidate the snapshot.
    if (s.driver==DatabaseKind::PostgreSQL) {
      Stmt secondaryProof(d->db,"INSERT OR REPLACE INTO table_proofs VALUES(?,?,?,?)");
      for (const auto &table:s.objects) {
        if (table.kind!="table" || table.strictUniqueKeys.isEmpty()) continue;
        const auto payload=metadata_detail::encodeTableKeys(table.primaryKey,table.strictUniqueKeys);
        if (payload.size()>65536 || bytes+2LL*payload.size()>16*1024*1024) continue;
        bytes+=2LL*payload.size();
        secondaryProof.bind(1,s.key);
        secondaryProof.bind(2,table.key);
        secondaryProof.bind(3,table.noDescendantsKnown);
        secondaryProof.bind(4,payload);
        secondaryProof.done();
      }
    }
    if (s.statistics || !s.statisticsWarning.isEmpty()) {
      Stmt summary(d->db, "INSERT INTO planner_stat_scopes VALUES(?,?,?,?)");
      summary.bind(1, s.key);
      summary.bind(2, s.statistics ? s.statistics->refreshedMs : 0);
      summary.bind(3, qint64(s.statistics && s.statistics->complete));
      summary.bind(4, s.statisticsWarning.left(1024));
      summary.done();
      Stmt table(d->db, "INSERT INTO planner_stat_tables VALUES(?,?,?,?,?)");
      qint64 statBytes = 0;
      bool omitted = false;
      if (s.statistics)
        for (const auto &t : s.statistics->tables) {
          auto payload =
              QJsonDocument(plannerTableJson(t)).toJson(QJsonDocument::Compact);
          statBytes += payload.size();
          // Statistics are optional. Oversize entries are omitted atomically
          // with the metadata generation; no partial index definition is
          // serialized.
          if (payload.size() > 65536 || statBytes > 2 * 1024 * 1024) {
            omitted = true;
            continue;
          }
          table.bind(1, s.key);
          table.bind(2, QString::fromStdString(t.database));
          table.bind(3, QString::fromStdString(t.schema));
          table.bind(4, QString::fromStdString(t.name));
          table.bind(5, QString::fromUtf8(payload));
          table.done();
        }
      if (omitted) {
        Stmt partial(d->db, "UPDATE planner_stat_scopes SET "
                            "complete=0,warning=? WHERE scope=?");
        partial.bind(1, "部分目录统计超过本地缓存限额，未存储不完整的索引定义");
        partial.bind(2, s.key);
        partial.done();
      }
    }
    d->exec("DELETE FROM scopes WHERE cache_key NOT IN(SELECT cache_key FROM "
            "scopes ORDER BY last_used DESC LIMIT 12);COMMIT;");
  } catch (...) {
    d->exec("ROLLBACK");
    throw;
  }
}
void MetadataStore::invalidate(const QString &profile,
                               const QString &database) {
  d->exec("BEGIN IMMEDIATE");
  try {
    Stmt generation(d->db, "INSERT INTO metadata_generations VALUES(?,1) "
                           "ON CONFLICT(profile_id) DO UPDATE SET generation=generation+1");
    generation.bind(1, profile);
    generation.done();
    Stmt s(d->db, "UPDATE scopes SET invalidated=1 WHERE profile_id=? AND (?='' OR database_name=?)");
    s.bind(1, profile);
    s.bind(2, database);
    s.bind(3, database);
    s.done();
    d->exec("COMMIT");
  } catch (...) {
    d->exec("ROLLBACK");
    throw;
  }
}
void MetadataStore::removeProfile(const QString &profile) {
  Stmt s(d->db, "DELETE FROM scopes WHERE profile_id=?");
  s.bind(1, profile);
  s.done();
}
} // namespace navidog
