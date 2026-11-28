#include "connection_controller.hpp"
#include "components.hpp"
#include "export_paths.hpp"
#include "navidog/connection_archive.hpp"
#include "navidog/profile_store.hpp"

#include <QEventLoop>
#include <QFormLayout>
#include <QInputDialog>
#include <QMessageBox>
#include <QProgressDialog>
#include <QSet>
#include <QThread>
#include <exception>
#include <utility>

namespace {
using namespace navidog;
using namespace navidog::ui;

class ExportConnectionsDialog final : public Dialog {
public:
    ExportConnectionsDialog(const QVector<ConnectionProfile>& profiles,
                            const QVector<ConnectionGroup>& groups,
                            const QString& preferredId, QWidget* parent)
        : Dialog(parent) {
        setObjectName("exportConnectionsDialog");
        setWindowTitle("导出连接");
        resize(540, 500);
        auto* layout = new QVBoxLayout(this);
        layout->addWidget(new QLabel("选择要导出的连接", this));
        auto* list = new CategoryList(this);
        list->setObjectName("exportConnectionList");
        QHash<QString, QString> groupNames;
        for (const auto& group : groups) groupNames.insert(group.id, group.name);
        for (const auto& profile : profiles) {
            const auto group = groupNames.value(profile.groupId);
            const auto label = profile.name + " · " + databaseKindName(profile.driver);
            auto* item = new QListWidgetItem(group.isEmpty() ? label : group + " / " + label, list);
            item->setData(Qt::UserRole, profile.id);
            item->setCheckState(preferredId.isEmpty() || profile.id == preferredId ? Qt::Checked : Qt::Unchecked);
            if (item->checkState() == Qt::Checked) ids_.insert(profile.id);
        }
        layout->addWidget(list, 1);
        auto* include = new CheckBox("包含密码并加密备份", this);
        include->setObjectName("includeConnectionPasswords");
        layout->addWidget(include);
        auto* credentials = new QWidget(this);
        auto* fields = new QFormLayout(credentials);
        fields->setContentsMargins(0, 0, 0, 0);
        password_ = new TextInput(credentials);
        password_->setObjectName("connectionArchivePassword");
        password_->setEchoMode(QLineEdit::Password);
        password_->setMaxLength(1024);
        auto* confirm = new TextInput(credentials);
        confirm->setObjectName("confirmConnectionArchivePassword");
        confirm->setEchoMode(QLineEdit::Password);
        confirm->setMaxLength(1024);
        fields->addRow("备份口令", password_);
        fields->addRow("确认口令", confirm);
        auto* hint = new QLabel("导入时需要此口令。已保存和本次会话中可用的密码会一起加密。", credentials);
        hint->setWordWrap(true);
        fields->addRow(hint);
        layout->addWidget(credentials);
        credentials->hide();
        auto* buttons = new QDialogButtonBox(QDialogButtonBox::Save | QDialogButtonBox::Cancel, this);
        auto* save = buttons->button(QDialogButtonBox::Save);
        save->setText("导出…");
        save->setProperty("primary", true);
        buttons->button(QDialogButtonBox::Cancel)->setText("取消");
        layout->addWidget(buttons);
        const auto update = [this, include, confirm, save] {
            save->setEnabled(!ids_.isEmpty() && (!include->isChecked() ||
                             (!password_->text().isEmpty() && password_->text() == confirm->text())));
        };
        connect(list, &QListWidget::itemChanged, this, [this, update](QListWidgetItem* item) {
            const auto id = item->data(Qt::UserRole).toString();
            if (item->checkState() == Qt::Checked) ids_.insert(id);
            else ids_.remove(id);
            update();
        });
        connect(include, &CheckBox::toggled, this, [this, credentials, update](bool enabled) {
            includePasswords_ = enabled;
            credentials->setVisible(enabled);
            if (enabled) password_->setFocus();
            update();
        });
        connect(password_, &QLineEdit::textChanged, this, update);
        connect(confirm, &QLineEdit::textChanged, this, update);
        connect(buttons, &QDialogButtonBox::accepted, this, &QDialog::accept);
        connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
        update();
    }

    const QSet<QString>& ids() const { return ids_; }
    QString password() const { return includePasswords_ ? password_->text() : QString{}; }

private:
    TextInput* password_;
    QSet<QString> ids_;
    bool includePasswords_ = false;
};

void archiveOperation(QWidget* owner, const QString& label, const std::function<void()>& work) {
    QProgressDialog progress(label, QString{}, 0, 0, owner);
    progress.setWindowModality(Qt::WindowModal);
    progress.setCancelButton(nullptr);
    progress.setWindowFlag(Qt::WindowCloseButtonHint, false);
    progress.setMinimumDuration(0);
    std::exception_ptr failure;
    QEventLoop done;
    std::unique_ptr<QThread> thread(QThread::create([&] {
        try { work(); }
        catch (...) { failure = std::current_exception(); }
    }));
    QObject::connect(thread.get(), &QThread::finished, &done, &QEventLoop::quit, Qt::QueuedConnection);
    progress.show();
    thread->start();
    done.exec();
    thread->wait();
    if (failure) std::rethrow_exception(failure);
}
}

void ConnectionController::exportConnections(const QString& preferredId) {
    if (profiles_.isEmpty()) {
        emit statusChanged("没有可以导出的连接");
        return;
    }
    const auto groups = workspace_.groups();
    ExportConnectionsDialog dialog(profiles_, groups, preferredId, owner_);
    if (dialog.exec() != QDialog::Accepted) return;
    WorkspaceProfiles backup;
    QSet<QString> groupIds;
    for (const auto& profile : std::as_const(profiles_)) {
        if (!dialog.ids().contains(profile.id)) continue;
        backup.connections.push_back(profile);
        groupIds.insert(profile.groupId);
    }
    if (backup.connections.isEmpty()) return;
    const bool all = backup.connections.size() == profiles_.size();
    QSet<QString> exportedGroupIds;
    for (const auto& group : groups)
        if (all || groupIds.contains(group.id)) {
            backup.groups.push_back(group);
            exportedGroupIds.insert(group.id);
        }
    for (auto& profile : backup.connections)
        if (!exportedGroupIds.contains(profile.groupId)) profile.groupId.clear();
    const auto password = dialog.password();
    const auto context = backup.connections.size() == 1 ? exportContext(backup.connections.front()) : ExportContext{};
    const auto path = chooseExportFile(owner_, "保存连接备份", context, "连接备份",
                                       password.isEmpty() ? "Navidog 连接 (*.json)"
                                                          : "Navidog 加密连接 (*.ndconn)");
    if (path.isEmpty()) return;
    try {
        archiveOperation(owner_, "正在导出连接…", [&] { exportConnectionArchive(path, backup, password); });
        emit statusChanged(QString("已导出 %1 个连接").arg(backup.connections.size()));
    } catch (const std::exception& error) {
        QMessageBox::warning(owner_, "导出连接失败", QString::fromUtf8(error.what()));
    }
}

void ConnectionController::importConnections() {
    const auto path = QFileDialog::getOpenFileName(owner_, "导入连接", exportDirectory(owner_),
                                                   "Navidog 连接 (*.json *.ndconn)");
    if (path.isEmpty()) return;
    try {
        const auto bytes = readConnectionArchiveFile(path);
        QString password;
        if (connectionArchiveIsEncrypted(bytes)) {
            bool accepted = false;
            password = QInputDialog::getText(owner_, "解锁连接备份", "备份口令", QLineEdit::Password,
                                             {}, &accepted);
            if (!accepted) return;
        }
        WorkspaceProfiles imported;
        archiveOperation(owner_, "正在读取连接备份…", [&] { imported = decodeConnectionArchive(bytes, password); });
        const auto count = imported.connections.size();
        if (!count && imported.groups.isEmpty()) {
            emit statusChanged("备份中没有连接或分组");
            return;
        }
        auto updated = mergeConnectionArchive({profiles_, workspace_.groups()}, std::move(imported), storePath_);
        store_.saveWorkspace(updated);
        profiles_ = std::move(updated.connections);
        emit connectionsImported(std::move(updated.groups));
        emit statusChanged(QString("已导入 %1 个连接").arg(count));
    } catch (const std::exception& error) {
        QMessageBox::warning(owner_, "导入连接失败", QString::fromUtf8(error.what()));
    }
}
