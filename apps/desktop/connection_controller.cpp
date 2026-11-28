#include "connection_controller.hpp"
#include "connection_dialog.hpp"
#include "navidog/credentials.hpp"
#include "navidog/profile_store.hpp"
#include "navidog/secret_store.hpp"
#include <QInputDialog>
#include <QMessageBox>
#include <QUuid>
#include <algorithm>
#include <utility>

ConnectionController::ConnectionController(
    navidog::QueryService &service, const navidog::ProfileStore &store,
    QVector<navidog::ConnectionProfile> &profiles, QString storePath,
    WorkspaceAccess workspace, QWidget *owner)
    : QObject(owner), service_(service), store_(store), profiles_(profiles),
      storePath_(std::move(storePath)), workspace_(std::move(workspace)), owner_(owner) {}

void ConnectionController::create(const QString &groupId) {
    open(Mode::Create, {}, groupId);
}

void ConnectionController::edit(const QString &id) {
    open(Mode::Edit, id);
}

void ConnectionController::duplicate(const QString &id) {
    open(Mode::Duplicate, id);
}

void ConnectionController::prepareDuplicate(navidog::ConnectionProfile &profile) const {
    const auto base = profile.name + " 副本";
    auto name = base;
    int suffix = 2;
    while (std::any_of(profiles_.cbegin(), profiles_.cend(), [&](const auto &other) {
        return other.name.compare(name, Qt::CaseInsensitive) == 0;
    }))
        name = base + ' ' + QString::number(suffix++);

    profile.id.clear();
    profile.name = name;
    profile.password.clear();
    profile.savePassword = false;
    profile.credentialSource = navidog::CredentialSource::Prompt;
    profile.ssh.password.clear();
    profile.ssh.passphrase.clear();
    profile.http.password.clear();
    profile.http.proxyPassword.clear();
    profile.ssl.damengKeystorePassword.clear();
    profile.ssl.saveDamengKeystorePassword = false;
    profile.ssh.savePassword = false;
    profile.ssh.savePassphrase = false;
    profile.http.savePassword = false;
    profile.serverVersion.clear();
    profile.detectedVersion = navidog::DatabaseVersion::Unknown;
}

void ConnectionController::open(Mode mode, const QString &id, const QString &groupId) {
    if (service_.operationBusy() || service_.catalogBusy()) {
        QMessageBox::information(owner_, "编辑连接",
            "当前正在读取或保存数据库对象，请等待完成或停止该操作后再编辑连接。");
        return;
    }
    if (workspace_.closingTabs()) {
        emit statusChanged("正在关闭标签并准备连接设置，请稍候");
        return;
    }

    const bool editing = mode == Mode::Edit;
    navidog::ConnectionProfile original;
    if (mode == Mode::Create) {
        original.groupId = groupId;
    } else {
        auto found = std::find_if(profiles_.cbegin(), profiles_.cend(),
                                  [&](const auto &profile) { return profile.id == id; });
        if (found == profiles_.cend())
            return;
        original = *found;
        if (editing && !workspace_.prepareEdit(id))
            return;
    }
    if (mode == Mode::Duplicate)
        prepareDuplicate(original);
    original.credentialStorePath = storePath_;
    if (editing && navidog::hasRememberedSecrets(original) && !original.secretsLoaded) {
        try {
            navidog::loadConnectionSecrets(original);
        } catch (const std::exception &error) {
            emit statusChanged(QString::fromUtf8(error.what()));
        }
    }

    ConnectionDialog dialog(original, editing, workspace_.groups(), service_,
                            owner_, mode == Mode::Create);
    while (dialog.exec() == QDialog::Accepted) {
        auto profile = dialog.profile();
        profile.id = editing ? id : QUuid::createUuid().toString(QUuid::Id128);
        profile.credentialStorePath = storePath_;
        if (editing && (profile.driver != original.driver || profile.host != original.host ||
                        profile.port != original.port || profile.database != original.database ||
                        profile.user != original.user)) {
            profile.serverVersion.clear();
            profile.detectedVersion = navidog::DatabaseVersion::Unknown;
        }
        const bool nameTaken = std::any_of(profiles_.cbegin(), profiles_.cend(),
            [&](const auto &other) {
                return other.id != profile.id && other.name.compare(profile.name, Qt::CaseInsensitive) == 0;
            });
        if (nameTaken) {
            QMessageBox::information(&dialog, "连接名称重复", "已有同名连接，请使用其他名称。");
            continue;
        }

        navidog::WorkspaceProfiles updated{profiles_, workspace_.groups()};
        if (editing) {
            auto found = std::find_if(updated.connections.begin(), updated.connections.end(),
                                      [&](const auto &entry) { return entry.id == id; });
            if (found == updated.connections.end())
                return;
            *found = profile;
        } else {
            updated.connections.push_back(profile);
        }
        try {
            store_.saveWorkspace(updated);
        } catch (const std::exception &error) {
            QMessageBox::warning(&dialog, "无法保存", QString::fromUtf8(error.what()));
            continue;
        }
        profiles_ = std::move(updated.connections);

        auto comparable = profile;
        comparable.name = original.name;
        comparable.groupId = original.groupId;
        comparable.notes = original.notes;
        const bool endpointChanged = editing && comparable != original;
        emit connectionSaved(profile.id, editing, endpointChanged);
        emit statusChanged(endpointChanged ? "连接已更新，旧会话已关闭；对象页需重新读取。"
                           : editing ? "连接已更新" : "连接已保存，双击即可打开。");
        return;
    }
}

std::optional<navidog::ConnectionProfile> ConnectionController::authenticatedProfile(const QString &id) {
    const auto profile = std::find_if(profiles_.begin(), profiles_.end(),
        [&](const auto &entry) { return entry.id == id; });
    if (profile == profiles_.end() || !promptPassword(*profile))
        return {};
    return *profile;
}

bool ConnectionController::promptPassword(navidog::ConnectionProfile &p) {
    navidog::useSessionCredentialsOnly(p);
    if (navidog::hasRememberedSecrets(p) && !p.secretsLoaded) {
        try {
            navidog::loadConnectionSecrets(p);
        } catch (const std::exception &e) {
            emit statusChanged(QString::fromUtf8(e.what()));
            p.secretsLoaded = true;
        }
    }
    if (p.ssh.enabled && p.ssh.password.isEmpty() &&
        (p.ssh.authentication == navidog::SshAuthentication::Password ||
         p.ssh.authentication == navidog::SshAuthentication::PasswordAndKey)) {
        bool ok = false;
        auto password = QInputDialog::getText(owner_, "SSH 密码",
                                              p.ssh.user + "@" + p.ssh.host,
                                              QLineEdit::Password, {}, &ok);
        if (!ok)
            return false;
        p.ssh.password = password;
    }
    if (p.http.enabled && p.http.authentication && p.http.password.isEmpty()) {
        bool ok = false;
        auto password = QInputDialog::getText(owner_, "HTTP 验证", p.http.user,
                                              QLineEdit::Password, {}, &ok);
        if (!ok)
            return false;
        p.http.password = password;
    }
    if (p.driver == navidog::DatabaseKind::Dameng && p.tls && p.ssl.damengKeystorePassword.isEmpty()) {
        bool ok = false;
        const auto password = QInputDialog::getText(owner_, "达梦 SSL 密钥库",
            "请输入 .keystore 的密码，留空使用默认密码。", QLineEdit::Password, {}, &ok);
        if (!ok)
            return false;
        p.ssl.damengKeystorePassword = password.isEmpty() ? QStringLiteral("changeit") : password;
    }
    if (p.driver == navidog::DatabaseKind::SQLite ||
        p.credentialSource == navidog::CredentialSource::Passwordless ||
        (p.driver == navidog::DatabaseKind::Oracle &&
         p.oracle.externalAuthentication) ||
        !p.password.isEmpty())
        return true;
    bool ok = false;
    auto password = QInputDialog::getText(owner_, "连接密码", p.name + " 的密码",
                                          QLineEdit::Password, {}, &ok);
    if (ok)
        p.password = password;
    return ok;
}
