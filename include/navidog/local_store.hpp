#pragma once
#include <QString>
#include <QByteArray>
#include <QStringList>
#include <functional>
#include <memory>
#include <optional>
namespace navidog {
// One short-lived SQLite handle per caller/thread; no database drivers or network.
class LocalStore {
public:
 explicit LocalStore(const QString& path);
 ~LocalStore();
 std::optional<QByteArray> document(const QString& key)const;
 void putDocument(const QString& key,const QByteArray& value);
 std::optional<QByteArray> secret(const QString& profile)const;
 void putSecret(const QString& profile,const QByteArray& cipher);
 void removeSecret(const QString& profile);
 void retainSecrets(const QStringList& profiles);
 void transaction(const std::function<void()>&);
private:struct Impl;std::unique_ptr<Impl> d;
};
QString localStorePath(const QString& directory);
// Read/import a legacy document once. The old file is kept; SQLite then wins.
std::optional<QByteArray> localDocument(const QString& database,const QString& key,const QString& legacy={});
}
