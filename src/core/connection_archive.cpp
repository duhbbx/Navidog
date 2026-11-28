#include "navidog/connection_archive.hpp"
#include "navidog/profile_store.hpp"
#include "navidog/secret_store.hpp"
#include "secret_crypto.hpp"

#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QMessageAuthenticationCode>
#include <QSaveFile>
#include <QScopeGuard>
#include <QSet>
#include <QUuid>
#include <stdexcept>
#include <utility>

namespace navidog {
namespace {
constexpr char magic[] = "NDCA1";
constexpr int magicSize = sizeof(magic) - 1;
constexpr int blockSize = 16, tagSize = 32, saltSize = 16;
constexpr int headerSize = magicSize + saltSize + blockSize;
constexpr qint64 maxJsonBytes = 8 * 1024 * 1024;
constexpr qint64 maxFileBytes = maxJsonBytes + headerSize + blockSize + tagSize;
constexpr auto format = "navidog-connections";

[[noreturn]] void fail(const QString& message) { throw std::runtime_error(message.toStdString()); }

QByteArray encrypt(const QByteArray& plain, const QString& password) {
    const auto salt = secret_crypto::randomBytes(saltSize);
    const auto iv = secret_crypto::randomBytes(blockSize);
    auto utf8 = password.toUtf8();
    const auto wipePassword = qScopeGuard([&] { utf8.fill('\0'); });
    auto key = secret_crypto::archiveKey(utf8, salt);
    const auto wipeKey = qScopeGuard([&] { key.fill('\0'); });
    // NDCA1 fixes the KDF, frame format, and separate encryption and MAC keys.
    auto frame = QByteArray(magic) + salt + iv + secret_crypto::aes256Cbc(plain, key.left(32), iv, true);
    frame += QMessageAuthenticationCode::hash(frame, key.mid(32), QCryptographicHash::Sha256);
    return frame;
}

QByteArray decrypt(const QByteArray& frame, const QString& password) {
    const auto cipherSize = frame.size() - headerSize - tagSize;
    if (cipherSize <= 0 || cipherSize % blockSize != 0) fail("加密连接文件已损坏");
    if (password.isEmpty()) fail("请输入备份口令");
    auto utf8 = password.toUtf8();
    const auto wipePassword = qScopeGuard([&] { utf8.fill('\0'); });
    auto key = secret_crypto::archiveKey(utf8, frame.mid(magicSize, saltSize));
    const auto wipeKey = qScopeGuard([&] { key.fill('\0'); });
    const auto expected = QMessageAuthenticationCode::hash(frame.left(frame.size() - tagSize),
                                                           key.mid(32), QCryptographicHash::Sha256);
    if (!secret_crypto::equalTags(expected, frame.right(tagSize))) fail("备份口令不正确，或连接文件已损坏");
    return secret_crypto::aes256Cbc(frame.mid(headerSize, cipherSize), key.left(32),
                                    frame.mid(magicSize + saltSize, blockSize), false);
}

void useManualCredentials(ConnectionProfile& profile) {
    if (profile.credentialSource != CredentialSource::Passwordless)
        profile.credentialSource = CredentialSource::Prompt;
    profile.credentialContainer.clear();
    profile.credentialVariable.clear();
    profile.savePassword = profile.ssh.savePassword = profile.ssh.savePassphrase = false;
    profile.http.savePassword = profile.ssl.saveDamengKeystorePassword = false;
}

QJsonObject collectedSecrets(const ConnectionProfile& profile) {
    ConnectionProfile saved;
    if (hasRememberedSecrets(profile) && !profile.secretsLoaded) {
        saved = profile;
        try { loadConnectionSecrets(saved); }
        catch (const std::exception& error) {
            fail("无法读取连接「" + profile.name + "」的已保存密码：" + QString::fromUtf8(error.what()));
        }
    }
    QJsonObject secrets;
    const auto add = [&](const QString& key, const QString& active, const QString& persisted, bool remembered) {
        if (!active.isEmpty() || !persisted.isEmpty() || remembered)
            secrets.insert(key, active.isEmpty() ? persisted : active);
    };
    if (profile.credentialSource != CredentialSource::Passwordless)
        add("database", profile.password, saved.password, hasRememberedSecrets(profile) && profile.savePassword);
    add("sshPassword", profile.ssh.password, saved.ssh.password, hasRememberedSecrets(profile) && profile.ssh.savePassword);
    add("sshPassphrase", profile.ssh.passphrase, saved.ssh.passphrase,
        hasRememberedSecrets(profile) && profile.ssh.savePassphrase);
    add("httpPassword", profile.http.password, saved.http.password, hasRememberedSecrets(profile) && profile.http.savePassword);
    add("httpProxyPassword", profile.http.proxyPassword, saved.http.proxyPassword,
        hasRememberedSecrets(profile) && profile.http.savePassword);
    add("sslKeystorePassword", profile.ssl.damengKeystorePassword, saved.ssl.damengKeystorePassword,
        hasRememberedSecrets(profile) && profile.ssl.saveDamengKeystorePassword);
    return secrets;
}

void restoreSecrets(ConnectionProfile& profile, const QJsonObject& secrets) {
    const auto read = [&](const char* key, QString& target) {
        const auto value = secrets.value(key);
        if (value.isUndefined()) return false;
        if (!value.isString()) fail("连接文件的密码字段格式无效");
        target = value.toString();
        return true;
    };
    profile.savePassword = read("database", profile.password);
    profile.ssh.savePassword = read("sshPassword", profile.ssh.password);
    profile.ssh.savePassphrase = read("sshPassphrase", profile.ssh.passphrase);
    const bool http = read("httpPassword", profile.http.password);
    const bool proxy = read("httpProxyPassword", profile.http.proxyPassword);
    profile.http.savePassword = http || proxy;
    profile.ssl.saveDamengKeystorePassword = read("sslKeystorePassword", profile.ssl.damengKeystorePassword);
    profile.secretsLoaded = !secrets.isEmpty();
    if (profile.secretsLoaded && profile.credentialSource != CredentialSource::Passwordless)
        profile.credentialSource = CredentialSource::LocalEncrypted;
}
}

void exportConnectionArchive(const QString& path, const WorkspaceProfiles& workspace, const QString& archivePassword) {
    QJsonArray connections, groups;
    for (const auto& profile : workspace.connections) {
        auto copy = profile;
        useManualCredentials(copy);
        auto json = connectionProfileJson(copy);
        if (!archivePassword.isEmpty()) {
            const auto secrets = collectedSecrets(profile);
            if (!secrets.isEmpty()) json.insert("secrets", secrets);
        }
        connections.append(json);
    }
    for (const auto& group : workspace.groups)
        groups.append(QJsonObject{{"id", group.id}, {"name", group.name}});
    auto payload = QJsonDocument(QJsonObject{{"format", format}, {"version", 1},
                                             {"connections", connections}, {"groups", groups}})
                       .toJson(archivePassword.isEmpty() ? QJsonDocument::Indented : QJsonDocument::Compact);
    const auto wipePayload = qScopeGuard([&] { payload.fill('\0'); });
    if (payload.size() > maxJsonBytes) fail("连接备份超过 8 MiB");
    if (!archivePassword.isEmpty()) {
        const auto encrypted = encrypt(payload, archivePassword);
        payload.fill('\0');
        payload = encrypted;
    }
    QSaveFile file(path);
    if (!file.open(QIODevice::WriteOnly) ||
        !file.setPermissions(QFileDevice::ReadOwner | QFileDevice::WriteOwner) ||
        file.write(payload) != payload.size() || !file.commit())
        fail("无法保存连接文件：" + file.errorString());
}

QByteArray readConnectionArchiveFile(const QString& path) {
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) fail("无法打开连接文件：" + file.errorString());
    const auto bytes = file.read(maxFileBytes + 1);
    if (file.error() != QFileDevice::NoError) fail("无法读取连接文件：" + file.errorString());
    if (bytes.size() > maxFileBytes) fail("连接备份超过 8 MiB");
    return bytes;
}

bool connectionArchiveIsEncrypted(const QByteArray& data) { return data.startsWith(magic); }

WorkspaceProfiles decodeConnectionArchive(const QByteArray& data, const QString& archivePassword) {
    if (data.size() > maxFileBytes) fail("连接备份超过 8 MiB");
    const bool encrypted = connectionArchiveIsEncrypted(data);
    auto plain = encrypted ? decrypt(data, archivePassword) : data;
    const auto wipe = qScopeGuard([&] { if (encrypted) plain.fill('\0'); });
    if (plain.size() > maxJsonBytes) fail("连接备份超过 8 MiB");
    QJsonParseError error;
    const auto json = QJsonDocument::fromJson(plain, &error).object();
    const auto connections = json.value("connections");
    const auto groups = json.value("groups");
    if (error.error != QJsonParseError::NoError || json.value("format") != format ||
        json.value("version") != 1 || !connections.isArray() || !groups.isArray())
        fail("此文件不是受支持的 Navidog 连接备份");
    WorkspaceProfiles workspace;
    QSet<QString> groupIds, connectionIds;
    for (const auto& entry : groups.toArray()) {
        const auto object = entry.toObject();
        const auto id = object.value("id").toString();
        const auto name = object.value("name").toString();
        if (id.isEmpty() || name.trimmed().isEmpty() || groupIds.contains(id))
            fail("连接文件包含无效或重复的分组");
        groupIds.insert(id);
        workspace.groups.push_back({id, name});
    }
    for (const auto& entry : connections.toArray()) {
        const auto object = entry.toObject();
        auto profile = connectionProfileFromJson(object);
        if (profile.id.isEmpty() || connectionIds.contains(profile.id) || profile.name.trimmed().isEmpty() ||
            profile.driver == DatabaseKind::Unknown ||
            (isFileDatabase(profile.driver) && profile.database.isEmpty()) ||
            (!profile.groupId.isEmpty() && !groupIds.contains(profile.groupId)))
            fail("连接文件包含无效或重复的连接配置");
        connectionIds.insert(profile.id);
        useManualCredentials(profile);
        const auto secrets = object.value("secrets");
        if (!secrets.isUndefined()) {
            if (!encrypted || !secrets.isObject()) fail("密码必须放在加密连接文件中");
            restoreSecrets(profile, secrets.toObject());
        }
        workspace.connections.push_back(std::move(profile));
    }
    return workspace;
}

WorkspaceProfiles mergeConnectionArchive(WorkspaceProfiles current, WorkspaceProfiles imported,
                                         const QString& storePath) {
    QHash<QString, QString> groupNames, groupIds;
    for (const auto& group : std::as_const(current.groups))
        groupNames.insert(group.name.toCaseFolded(), group.id);
    for (auto& group : imported.groups) {
        const auto key = group.name.toCaseFolded();
        auto id = groupNames.value(key);
        if (id.isEmpty()) {
            id = QUuid::createUuid().toString(QUuid::Id128);
            groupNames.insert(key, id);
            current.groups.push_back({id, group.name});
        }
        groupIds.insert(group.id, id);
    }
    QSet<QString> names;
    for (const auto& connection : std::as_const(current.connections)) names.insert(connection.name.toCaseFolded());
    QHash<QString, int> nextSuffix;
    for (auto& connection : imported.connections) {
        const auto base = connection.name;
        auto& suffix = nextSuffix[base.toCaseFolded()];
        if (!suffix) suffix = 2;
        while (names.contains(connection.name.toCaseFolded()))
            connection.name = base + " (" + QString::number(suffix++) + ")";
        names.insert(connection.name.toCaseFolded());
        connection.id = QUuid::createUuid().toString(QUuid::Id128);
        connection.groupId = groupIds.value(connection.groupId);
        connection.credentialStorePath = storePath;
        connection.advanced.autoConnect = false;
        current.connections.push_back(std::move(connection));
    }
    return current;
}
}
