#include "navidog/profile_store.hpp"
#include "navidog/credentials.hpp"
#include "navidog/local_store.hpp"
#include "navidog/secret_store.hpp"
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSaveFile>
#include <stdexcept>

namespace navidog {
ConnectionProfile connectionProfileFromJson(const QJsonObject& o) {
    ConnectionProfile p;
    p.groupId = o["groupId"].toString();
    p.id = o["id"].toString();
    p.name = o["name"].toString();
    p.driver = databaseKindFromKey(o["driver"].toString());
    p.version = versionFromKey(o["targetVersion"].toString());
    if (!versionBelongsTo(p.version, p.driver))
        p.version = DatabaseVersion::Auto;
    p.detectedVersion =
        versionFromKey(o["detectedVersion"].toString("unknown"));
    p.serverVersion = o["serverVersion"].toString();
    p.credentialSource =
        o["credentialSource"].toString() == "localEncrypted"
            ? CredentialSource::LocalEncrypted
        : o["credentialSource"].toString() == "system"
            ? CredentialSource::SystemStore
        : o["credentialSource"].toString() == "localDocker"
            ? CredentialSource::LocalDocker
        : o["credentialSource"].toString() == "passwordless"
            ? CredentialSource::Passwordless
            : CredentialSource::Prompt;
    p.credentialContainer = o["credentialContainer"].toString();
    p.credentialVariable = o["credentialVariable"].toString();
    p.host = o["host"].toString();
    p.database = o["database"].toString();
    p.user = o["user"].toString();
    p.port = o["port"].toInt();
    p.readOnly = o["readOnly"].toBool();
    p.tls = o["tls"].toBool(true);
    readConnectionOptions(p, o["options"].toObject());
    return p;
}

QJsonObject connectionProfileJson(const ConnectionProfile& p) {
    return {
        {"groupId", p.groupId},
        {"id", p.id},
        {"name", p.name},
        {"driver", databaseKindKey(p.driver)},
        {"targetVersion", versionKey(p.version)},
        {"detectedVersion", versionKey(p.detectedVersion)},
        {"serverVersion", p.serverVersion},
        {"credentialSource",
         p.credentialSource == CredentialSource::LocalEncrypted
             ? "localEncrypted"
         : p.credentialSource == CredentialSource::SystemStore ? "system"
         : p.credentialSource == CredentialSource::LocalDocker
             ? "localDocker"
         : p.credentialSource == CredentialSource::Passwordless
             ? "passwordless"
             : "prompt"},
        {"credentialContainer", p.credentialContainer},
        {"credentialVariable", p.credentialVariable},
        {"host", p.host},
        {"database", p.database},
        {"user", p.user},
        {"port", p.port},
        {"readOnly", p.readOnly},
        {"tls", p.tls},
        {"options", connectionOptionsJson(p)}};
}

QVector<ConnectionProfile> ProfileStore::load() const {
    return loadWorkspace().connections;
}
WorkspaceProfiles ProfileStore::loadWorkspace() const {
    QByteArray bytes;
    const bool sqlite = path_.endsWith(".sqlite");
    if (sqlite) {
        auto value =
            localDocument(path_, "connections",
                          QFileInfo(path_).dir().filePath("connections.json"));
        if (!value)
            return {};
        bytes = *value;
    } else {
        QFile file(path_);
        if (!file.exists())
            return {};
        if (!file.open(QIODevice::ReadOnly) || file.size() > 8 * 1024 * 1024)
            throw std::runtime_error("无法读取连接配置");
        bytes = file.readAll();
    }
    QJsonParseError error;
    auto doc = QJsonDocument::fromJson(bytes, &error);
    if (error.error != QJsonParseError::NoError ||
        (!doc.isArray() && !doc.isObject()))
        throw std::runtime_error("连接配置损坏，请备份后检查 JSON 文件");
    WorkspaceProfiles workspace;
    const auto array =
        doc.isArray() ? doc.array() : doc.object()["connections"].toArray();
    for (auto entry : doc.object()["groups"].toArray()) {
        auto o = entry.toObject();
        auto id = o["id"].toString(), name = o["name"].toString();
        if (!id.isEmpty() && !name.isEmpty())
            workspace.groups.push_back({id, name});
    }
    for (auto entry : array) {
        auto o = entry.toObject();
        auto p = connectionProfileFromJson(o);
        p.credentialStorePath = sqlite ? path_ : QString{};
        if (!p.id.isEmpty() && !p.name.isEmpty() &&
            (p.driver != DatabaseKind::SQLite || !p.database.isEmpty()))
            workspace.connections.push_back(p);
    }
    return workspace;
}
void ProfileStore::save(const QVector<ConnectionProfile> &profiles) const {
    auto workspace = loadWorkspace();
    workspace.connections = profiles;
    saveWorkspace(workspace);
}
void ProfileStore::saveWorkspace(const WorkspaceProfiles &workspace) const {
    QJsonArray array;
    for (auto p : workspace.connections) {
        useSessionCredentialsOnly(p);
        array.append(connectionProfileJson(p));
    }
    if (!QDir().mkpath(QFileInfo(path_).absolutePath()))
        throw std::runtime_error("无法创建配置目录");

    QJsonArray groups;
    for (const auto &group : workspace.groups)
        groups.append(QJsonObject{{"id", group.id}, {"name", group.name}});
    auto bytes = QJsonDocument(QJsonObject{{"version", 3},
                                           {"connections", array},
                                           {"groups", groups}})
                     .toJson();
    if (path_.endsWith(".sqlite")) {
        LocalStore db(path_);
        db.transaction([&] {
            db.putDocument("connections", bytes);
            QStringList ids;
            for (auto p : workspace.connections) {
                p.credentialStorePath = path_;
                useSessionCredentialsOnly(p);
                saveConnectionSecrets(p, &db);
                ids << p.id;
            }
            db.retainSecrets(ids);
        });
        return;
    }
    QSaveFile file(path_);
    if (!file.open(QIODevice::WriteOnly))
        throw std::runtime_error("无法保存连接配置");
    file.setPermissions(QFileDevice::ReadOwner | QFileDevice::WriteOwner);
    if (file.write(bytes) != bytes.size() || !file.commit())
        throw std::runtime_error("连接配置保存失败");
}
} // namespace navidog
