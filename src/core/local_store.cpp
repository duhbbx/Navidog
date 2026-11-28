#include "navidog/local_store.hpp"
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QSet>
#include <sqlite3.h>
#include <stdexcept>
namespace navidog {
namespace {
void check(int rc, sqlite3 *db) {
  if (rc != SQLITE_OK && rc != SQLITE_ROW && rc != SQLITE_DONE)
    throw std::runtime_error(sqlite3_errmsg(db));
}
struct Statement {
  sqlite3 *db;
  sqlite3_stmt *s = nullptr;
  Statement(sqlite3 *db, const char *sql) : db(db) {
    check(sqlite3_prepare_v2(db, sql, -1, &s, nullptr), db);
  }
  ~Statement() { sqlite3_finalize(s); }
  void text(int n, const QString &value) {
    auto b = value.toUtf8();
    check(sqlite3_bind_text(s, n, b.constData(), b.size(), SQLITE_TRANSIENT),
          db);
  }
  void blob(int n, const QByteArray &value) {
    check(sqlite3_bind_blob(s, n, value.constData(), value.size(),
                            SQLITE_TRANSIENT),
          db);
  }
  bool row() {
    auto rc = sqlite3_step(s);
    check(rc, db);
    return rc == SQLITE_ROW;
  }
  QByteArray bytes(int n) {
    int len = sqlite3_column_bytes(s, n);
    if (len > 8 * 1024 * 1024)
      throw std::runtime_error("本地配置超过 8 MiB");
    return QByteArray(static_cast<const char *>(sqlite3_column_blob(s, n)),
                      len);
  }
};
} // namespace
struct LocalStore::Impl {
  sqlite3 *db = nullptr;
  ~Impl() {
    if (db)
      sqlite3_close(db);
  }
  void exec(const char *sql) {
    char *error = nullptr;
    auto rc = sqlite3_exec(db, sql, nullptr, nullptr, &error);
    sqlite3_free(error);
    check(rc, db);
  }
};
QString localStorePath(const QString &directory) {
  return QDir(directory).filePath("navidog.sqlite");
}
LocalStore::LocalStore(const QString &path) : d(std::make_unique<Impl>()) {
  if (QFileInfo(path).isSymLink())
    throw std::runtime_error("本地配置数据库不能是符号链接");
  if (!QDir().mkpath(QFileInfo(path).absolutePath()))
    throw std::runtime_error("无法创建本地配置目录");
  auto bytes = QFile::encodeName(path);
  check(sqlite3_open_v2(bytes.constData(), &d->db,
                        SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE |
                            SQLITE_OPEN_NOMUTEX,
                        nullptr),
        d->db);
  sqlite3_busy_timeout(d->db, 1500);
  {
    Statement s(d->db, "PRAGMA user_version");
    if (s.row() && sqlite3_column_int(s.s, 0) > 1)
      throw std::runtime_error("本地数据库版本较新，未覆盖");
  }
  if (!QFile::setPermissions(path,
                             QFileDevice::ReadOwner | QFileDevice::WriteOwner))
    throw std::runtime_error("无法设置本地配置文件权限");
  d->exec("PRAGMA journal_mode=WAL; PRAGMA synchronous=FULL; PRAGMA "
          "foreign_keys=ON; PRAGMA secure_delete=ON; PRAGMA "
          "journal_size_limit=8388608; PRAGMA max_page_count=16384;");
  d->exec("CREATE TABLE IF NOT EXISTS app_documents(key TEXT PRIMARY KEY,value "
          "BLOB NOT NULL CHECK(length(value)<=8388608));CREATE TABLE IF NOT "
          "EXISTS connection_secrets(profile_id TEXT PRIMARY KEY,cipher BLOB "
          "NOT NULL CHECK(length(cipher)<=65536));PRAGMA user_version=1;");
}
LocalStore::~LocalStore() = default;
std::optional<QByteArray> LocalStore::document(const QString &key) const {
  Statement s(d->db, "SELECT value FROM app_documents WHERE key=?");
  s.text(1, key);
  if (!s.row())
    return {};
  return s.bytes(0);
}
void LocalStore::putDocument(const QString &key, const QByteArray &bytes) {
  if (bytes.size() > 8 * 1024 * 1024)
    throw std::runtime_error("本地配置超过 8 MiB");
  Statement s(d->db, "INSERT INTO app_documents(key,value) VALUES(?,?) ON "
                     "CONFLICT(key) DO UPDATE SET value=excluded.value");
  s.text(1, key);
  s.blob(2, bytes);
  s.row();
}
std::optional<QByteArray> LocalStore::secret(const QString &key) const {
  Statement s(d->db,
              "SELECT cipher FROM connection_secrets WHERE profile_id=?");
  s.text(1, key);
  if (!s.row())
    return {};
  return s.bytes(0);
}
void LocalStore::putSecret(const QString &key, const QByteArray &bytes) {
  if (bytes.size() > 65536)
    throw std::runtime_error("加密凭据超过上限");
  Statement s(d->db,
              "INSERT INTO connection_secrets(profile_id,cipher) VALUES(?,?) "
              "ON CONFLICT(profile_id) DO UPDATE SET cipher=excluded.cipher");
  s.text(1, key);
  s.blob(2, bytes);
  s.row();
}
void LocalStore::removeSecret(const QString &key) {
  Statement s(d->db, "DELETE FROM connection_secrets WHERE profile_id=?");
  s.text(1, key);
  s.row();
}
void LocalStore::retainSecrets(const QStringList &profiles) {
  Statement s(d->db, "SELECT profile_id FROM connection_secrets");
  const QSet<QString> retained(profiles.begin(),profiles.end());
  QStringList remove;
  while (s.row()) {
    auto id = QString::fromUtf8(
        reinterpret_cast<const char *>(sqlite3_column_text(s.s, 0)));
    if (!retained.contains(id))
      remove << id;
  }
  for (const auto &id : remove)
    removeSecret(id);
}
void LocalStore::transaction(const std::function<void()> &fn) {
  d->exec("BEGIN IMMEDIATE");
  try {
    fn();
    d->exec("COMMIT");
  } catch (...) {
    try {
      d->exec("ROLLBACK");
    } catch (...) {
    }
    throw;
  }
}
std::optional<QByteArray> localDocument(const QString &database,
                                        const QString &key,
                                        const QString &legacy) {
  LocalStore store(database);
  if (auto bytes = store.document(key))
    return bytes;
  if (legacy.isEmpty() || !QFileInfo::exists(legacy))
    return {};
  QFile file(legacy);
  if (!file.open(QIODevice::ReadOnly) || file.size() > 8 * 1024 * 1024)
    throw std::runtime_error("无法读取旧配置");
  auto bytes = file.read(8 * 1024 * 1024 + 1);
  QJsonParseError error;
  auto json = QJsonDocument::fromJson(bytes, &error);
  if (bytes.size() > 8 * 1024 * 1024 ||
      error.error != QJsonParseError::NoError ||
      (!json.isObject() && !json.isArray()))
    throw std::runtime_error("旧配置损坏，未导入或覆盖");
  store.transaction([&] {
    if (!store.document(key))
      store.putDocument(key, bytes);
  });
  return store.document(key);
}
} // namespace navidog
