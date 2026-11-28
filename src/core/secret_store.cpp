#include "navidog/secret_store.hpp"
#include "navidog/local_store.hpp"
#include "navidog/metadata_cache.hpp"
#include "secret_crypto.hpp"
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLockFile>
#include <QMessageAuthenticationCode>
#include <QSaveFile>
#include <stdexcept>
#ifdef Q_OS_WIN
#include <windows.h>
#include <wincrypt.h>
#endif
namespace navidog {
namespace {
[[noreturn]] void fail() {
  throw std::runtime_error("本机加密凭据无法读取或校验，请重新输入并保存密码");
}
#ifndef Q_OS_WIN
QByteArray keyFor(const QString &db, bool create, LocalStore &store) {
  const auto path = db + ".key";
  QLockFile lock(path + ".lock");
  if (!lock.tryLock(1500))
    fail();
  QFile file(path);
  if (QFileInfo(path).isSymLink())
    fail();
  if (!file.exists()) {
    if (!create || store.document("vault-key-created"))
      fail();
    auto bytes = secret_crypto::randomBytes(64);
    QSaveFile out(path);
    if (!out.open(QIODevice::WriteOnly))
      fail();
    if(!out.setPermissions(QFileDevice::ReadOwner | QFileDevice::WriteOwner))fail();
    if (out.write(bytes) != bytes.size() || !out.commit())
      fail();
    store.putDocument("vault-key-created", "1");
    return bytes;
  }
  if (!file.open(QIODevice::ReadOnly) || file.size() != 64)
    fail();
  const auto forbidden = QFileDevice::ReadGroup | QFileDevice::WriteGroup |
                         QFileDevice::ReadOther | QFileDevice::WriteOther;
  if (file.permissions() & forbidden)
    fail();
  return file.readAll();
}
#endif
QByteArray crypt(const QByteArray &bytes, const ConnectionProfile &p,
                 bool encrypt, LocalStore &store) {
#ifdef Q_OS_WIN
  auto entropy = p.id.toUtf8();
  DATA_BLOB in{DWORD(bytes.size()),
               reinterpret_cast<BYTE *>(const_cast<char *>(bytes.constData()))},
      extra{DWORD(entropy.size()), reinterpret_cast<BYTE *>(entropy.data())},
      out{};
  bool ok = encrypt ? CryptProtectData(&in, L"Navidog", &extra, nullptr,
                                       nullptr, CRYPTPROTECT_UI_FORBIDDEN, &out)
                    : CryptUnprotectData(&in, nullptr, &extra, nullptr, nullptr,
                                         CRYPTPROTECT_UI_FORBIDDEN, &out);
  if (!ok)
    fail();
  QByteArray result(reinterpret_cast<char *>(out.pbData), int(out.cbData));
  SecureZeroMemory(out.pbData, out.cbData);
  LocalFree(out.pbData);
  return result;
#else
  auto key = keyFor(p.credentialStorePath, encrypt, store);
  if (encrypt) {
    auto iv = secret_crypto::randomBytes(16);
    auto payload = QByteArray("NDV1") + iv + secret_crypto::aes256Cbc(bytes, key.left(32), iv, true);
    auto tag = QMessageAuthenticationCode::hash(
        payload + p.id.toUtf8(), key.mid(32), QCryptographicHash::Sha256);
    key.fill('\0');
    return payload + tag;
  }
  if (bytes.size() < 68 || bytes.left(4) != "NDV1")
    fail();
  auto payload = bytes.left(bytes.size() - 32);
  auto expected = QMessageAuthenticationCode::hash(
      payload + p.id.toUtf8(), key.mid(32), QCryptographicHash::Sha256);
  if (!secret_crypto::equalTags(expected, bytes.right(32)))
    fail();
  auto result = secret_crypto::aes256Cbc(payload.mid(20), key.left(32), payload.mid(4, 16), false);
  key.fill('\0');
  return result;
#endif
}
bool remembering(const ConnectionProfile &p) {
  return (p.credentialSource == CredentialSource::LocalEncrypted ||
          p.credentialSource == CredentialSource::Passwordless) &&
         (p.savePassword || p.ssh.savePassword || p.ssh.savePassphrase ||
          p.http.savePassword || p.ssl.saveDamengKeystorePassword);
}
} // namespace
bool systemSecretStoreAvailable() { return false; }
bool localSecretStoreAvailable() { return true; }
bool hasRememberedSecrets(const ConnectionProfile &p) { return remembering(p); }
void saveConnectionSecrets(const ConnectionProfile &p,
                           LocalStore *transaction) {
  if (p.credentialStorePath.isEmpty()) {
    if (remembering(p))
      throw std::runtime_error("加密凭据未绑定本地工作区");
    return;
  }
  std::unique_ptr<LocalStore> owned;
  if (!transaction) {
    owned = std::make_unique<LocalStore>(p.credentialStorePath);
    transaction = owned.get();
  }
  if (!remembering(p)) {
    transaction->removeSecret(p.id);
    return;
  }
  if (!p.secretsLoaded && p.password.isEmpty() && p.ssh.password.isEmpty() &&
      p.ssh.passphrase.isEmpty() && p.http.password.isEmpty() && p.ssl.damengKeystorePassword.isEmpty()) {
    if (transaction->secret(p.id))
      return;
    throw std::runtime_error("请先输入要记住的密码");
  }
  QJsonObject root{
      {"version", 1},
      {"identity", metadataIdentity(p)},
      {"password", p.savePassword ? p.password : QString{}},
      {"sshPassword", p.ssh.savePassword ? p.ssh.password : QString{}},
      {"sshPassphrase", p.ssh.savePassphrase ? p.ssh.passphrase : QString{}},
      {"damengKeystorePassword", p.ssl.saveDamengKeystorePassword ? p.ssl.damengKeystorePassword : QString{}},
      {"httpPassword", p.http.savePassword ? p.http.password : QString{}},
      {"proxyPassword",
       p.http.savePassword ? p.http.proxyPassword : QString{}}};
  auto plain = QJsonDocument(root).toJson(QJsonDocument::Compact);
  if (plain.size() > 60000)
    throw std::runtime_error("凭据长度超过上限");
  auto cipher = crypt(plain, p, true, *transaction);
  plain.fill('\0');
  transaction->putSecret(p.id, cipher);
}
void loadConnectionSecrets(ConnectionProfile &p) {
  if (p.credentialSource == CredentialSource::SystemStore)
    throw std::runtime_error("系统钥匙串已停用，请重新输入数据库密码");
  if (!remembering(p)) {
    p.secretsLoaded = true;
    return;
  }
  if (p.credentialStorePath.isEmpty())
    fail();
  LocalStore store(p.credentialStorePath);
  auto cipher = store.secret(p.id);
  if (!cipher)
    fail();
  auto plain = crypt(*cipher, p, false, store);
  auto doc = QJsonDocument::fromJson(plain);
  plain.fill('\0');
  auto root = doc.object();
  if (root["version"].toInt() != 1 ||
      root["identity"].toString() != metadataIdentity(p))
    fail();
  if (p.savePassword)
    p.password = root["password"].toString();
  if (p.ssh.savePassword)
    p.ssh.password = root["sshPassword"].toString();
  if (p.ssh.savePassphrase)
    p.ssh.passphrase = root["sshPassphrase"].toString();
  if (p.ssl.saveDamengKeystorePassword)
    p.ssl.damengKeystorePassword = root["damengKeystorePassword"].toString();
  if (p.http.savePassword) {
    p.http.password = root["httpPassword"].toString();
    p.http.proxyPassword = root["proxyPassword"].toString();
  }
  p.secretsLoaded = true;
}
void removeConnectionSecrets(
    const QString &) { /* Legacy API cannot address a workspace; no OS access.
                          ProfileStore prunes SQLite rows atomically. */
}
} // namespace navidog
