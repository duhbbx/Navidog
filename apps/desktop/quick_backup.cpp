#include "navidog/operation_timing.hpp"
#include "main_window.hpp"
#include "export_paths.hpp"
#include "components.hpp"
#include "navidog/database.hpp"
#include "navidog/database_tools.hpp"
#include "navidog/local_store.hpp"

#include <QDateTime>
#include <QDir>
#include <QElapsedTimer>
#include <QFileDialog>
#include <QFileInfo>
#include <QFormLayout>
#include <QJsonDocument>
#include <QMessageBox>
#include <QProcess>
#include <QProcessEnvironment>
#include <QStandardPaths>
#include <QTemporaryFile>
#include <QTimer>
#include <algorithm>
#include <sqlite3.h>

namespace {
using navidog::DatabaseKind;
constexpr auto scheduleKey = "quick-backup-schedule";

QString copySqliteDatabase(const QString& source, const QString& destination, const navidog::TaskContext& task) {
    if (!QFileInfo(source).isFile()) return "源 SQLite 数据库文件不存在";
    if (QFileInfo::exists(destination)) return "目标文件已存在，请选择一个新文件名";
    const QFileInfo output(destination);
    QTemporaryFile temporary(QDir(output.absolutePath()).filePath(".navidog-backup-XXXXXX"));
    if (!temporary.open()) return temporary.errorString();
    temporary.close();
    using Handle = std::unique_ptr<sqlite3, decltype(&sqlite3_close)>;
    sqlite3* raw = nullptr;
    const int sourceStatus = sqlite3_open_v2(source.toUtf8().constData(), &raw, SQLITE_OPEN_READONLY, nullptr);
    Handle input(raw, &sqlite3_close);
    if (sourceStatus != SQLITE_OK) return raw ? QString::fromUtf8(sqlite3_errmsg(raw)) : "无法读取 SQLite 文件";
    raw = nullptr;
    const int targetStatus = sqlite3_open_v2(temporary.fileName().toUtf8().constData(), &raw, SQLITE_OPEN_READWRITE, nullptr);
    Handle target(raw, &sqlite3_close);
    if (targetStatus != SQLITE_OK) return raw ? QString::fromUtf8(sqlite3_errmsg(raw)) : "无法写入 SQLite 文件";
    auto* backup = sqlite3_backup_init(target.get(), "main", input.get(), "main");
    if (!backup) return QString::fromUtf8(sqlite3_errmsg(target.get()));
    QElapsedTimer busy;
    int result = SQLITE_OK;
    while (result == SQLITE_OK || result == SQLITE_BUSY || result == SQLITE_LOCKED) {
        if (task.cancel->load()) {
            sqlite3_backup_finish(backup);
            return "备份或恢复已停止";
        }
        result = sqlite3_backup_step(backup, 128);
        const auto pages = sqlite3_backup_pagecount(backup);
        task.report({pages - sqlite3_backup_remaining(backup), pages, navidog::TaskUnit::Pages, "正在复制数据库"});
        if (result == SQLITE_BUSY || result == SQLITE_LOCKED) {
            if (!busy.isValid()) busy.start();
            if (busy.elapsed() >= 5000) break;
            QThread::msleep(40);
        } else busy.invalidate();
    }
    const auto finish = sqlite3_backup_finish(backup);
    if (result != SQLITE_DONE) return QString::fromUtf8(sqlite3_errstr(result));
    if (finish != SQLITE_OK) return QString::fromUtf8(sqlite3_errstr(finish));
    target.reset();
    input.reset();
    if (task.cancel->load()) return "任务已停止";
    if (!temporary.rename(output.absoluteFilePath()))
        return "无法保存备份。请检查写入权限以及目标是否已存在。";
    temporary.setAutoRemove(false);
    return {};
}

QString backupClient(const QString& tool) {
    auto path = QStandardPaths::findExecutable(tool);
#ifdef Q_OS_MACOS
    if (path.isEmpty()) path = QStandardPaths::findExecutable(tool, {"/opt/homebrew/bin", "/usr/local/bin"});
#endif
    return path;
}

QString runBackupClient(const QString& executable, const QStringList& arguments,
                         const QProcessEnvironment& environment, QTemporaryFile* output, const navidog::TaskContext& task) {
    if (executable.isEmpty()) return "找不到数据库官方客户端工具";
    QProcess process;
    process.setProcessEnvironment(environment);
    process.setStandardInputFile(QProcess::nullDevice());
    process.start(executable, arguments, QIODevice::ReadOnly);
    if (!process.waitForStarted(5000)) return process.errorString();
    QByteArray diagnostics;
    const auto drain = [&] {
        diagnostics = (diagnostics + process.readAllStandardError()).right(16384);
        while (process.bytesAvailable() > 0) {
            const auto bytes = process.read(64 * 1024);
            if (output && output->write(bytes) != bytes.size()) return false;
        }
        if (output) task.report({output->pos(), 0, navidog::TaskUnit::Bytes, "正在写入备份或转储文件"});
        return true;
    };
    while (process.state() != QProcess::NotRunning) {
        if (task.cancel->load() || !drain()) {
            process.kill();
            process.waitForFinished(3000);
            return task.cancel->load() ? "任务已停止" : output->errorString();
        }
        process.waitForReadyRead(100);
    }
    if (!drain()) return output->errorString();
    if (process.exitStatus() != QProcess::NormalExit || process.exitCode() != 0)
        return diagnostics.isEmpty() ? QString("客户端退出代码 %1").arg(process.exitCode())
                                     : QString::fromUtf8(diagnostics).trimmed();
    return {};
}

QString pgConnectionArgument(QString database) {
    database.replace("\\", "\\\\").replace("'", "\\'");
    return "dbname='" + database + "'";
}

QProcessEnvironment pgEnvironment(const navidog::ConnectionProfile& profile) {
    auto env = QProcessEnvironment::systemEnvironment();
    for (const auto* key : {"PGSERVICE", "PGSERVICEFILE", "PGOPTIONS", "PGHOSTADDR", "PGSSLSNI", "PGSSLNEGOTIATION"})
        env.remove(key);
    env.insert("PGGSSENCMODE", "disable");
    env.insert("PGPASSWORD", profile.password);
    env.insert("PGHOST", profile.advanced.socketFile.isEmpty() ? profile.host : profile.advanced.socketFile);
    env.insert("PGPORT", QString::number(profile.port));
    env.insert("PGUSER", profile.user);
    env.insert("PGCONNECT_TIMEOUT", QString::number(profile.advanced.connectTimeout));
    env.insert("PGAPPNAME", "Navidog backup");
    env.insert("PGSSLMODE", !profile.tls ? "disable" :
        profile.ssl.verification == navidog::SslVerification::Identity ? "verify-full" :
        profile.ssl.verification == navidog::SslVerification::CertificateAuthority ? "verify-ca" : "require");
    for (const auto& pair : {std::pair<QString, QString>{"PGSSLROOTCERT", profile.ssl.caFile},
                            {"PGSSLCERT", profile.ssl.certificateFile}, {"PGSSLKEY", profile.ssl.keyFile}}) {
        if (pair.second.isEmpty()) env.remove(pair.first);
        else env.insert(pair.first, pair.second);
    }
    return env;
}

QString mysqlOption(QString text) {
    text.replace("\\", "\\\\").replace("\"", "\\\"").replace("\n", "\\n").replace("\r", "\\r").replace("\t", "\\t");
    return "\"" + text + "\"";
}

QString exportServerDatabase(const navidog::ConnectionProfile& profile, const QString& destination,
                             const navidog::TaskContext& task, bool plain = false, bool withData = true, const QString& schema = {}) {
    if (QFileInfo::exists(destination)) return "备份目标已存在，请选择一个新文件名";
    QTemporaryFile output(QDir(QFileInfo(destination).absolutePath()).filePath(".navidog-backup-XXXXXX"));
    if (!output.open()) return output.errorString();
    QString result;
    if (profile.driver == DatabaseKind::SQLite) {
        result = runBackupClient(backupClient("sqlite3"),
            {"-batch", "-bail", "-readonly", "-init", QProcess::nullDevice(),
             QFileInfo(profile.database).absoluteFilePath(), withData ? ".dump" : ".schema --nosys"},
            QProcessEnvironment::systemEnvironment(), &output, task);
    } else if (profile.driver == DatabaseKind::PostgreSQL) {
        QStringList args{plain ? "--format=plain" : "--format=custom", "--no-password",
                         "--dbname", pgConnectionArgument(profile.database)};
        if (plain) {
            args << "--no-owner" << "--no-privileges";
            args << (withData ? "--inserts" : "--schema-only");
            if (!schema.isEmpty())
                args << "--schema" << navidog::quoteIdentifier(schema, profile.driver);
        }
        result = runBackupClient(backupClient("pg_dump"), args, pgEnvironment(profile), &output, task);
    } else {
        // Use a per-run owner-only file so the password stays out of command arguments.
        QTemporaryFile options;
        if (!options.open()) return options.errorString();
        const bool maria = profile.driver == DatabaseKind::MariaDB;
        const auto mode = !profile.tls ? "DISABLED" :
            profile.ssl.verification == navidog::SslVerification::Identity ? "VERIFY_IDENTITY" :
            profile.ssl.verification == navidog::SslVerification::CertificateAuthority ? "VERIFY_CA" : "REQUIRED";
        auto settings = QString("[client]\nhost=%1\nuser=%2\nport=%3\npassword=%4\n")
            .arg(mysqlOption(profile.host), mysqlOption(profile.user), QString::number(profile.port),
                 mysqlOption(profile.password));
        if (!maria) settings += "ssl-mode=" + QString::fromLatin1(mode) + "\n";
        const auto settingsBytes = settings.toUtf8();
        if (options.write(settingsBytes) != settingsBytes.size() || !options.flush()) return options.errorString();
        options.close();
        QStringList args{"--defaults-file=" + options.fileName()};
        if (!maria) args << "--no-login-paths";
        else {
            args << (profile.tls ? "--ssl" : "--skip-ssl");
            if (profile.tls && profile.ssl.verification != navidog::SslVerification::Encryption)
                args << "--ssl-verify-server-cert";
        }
        if (profile.advanced.socketFile.isEmpty()) args << "--protocol=TCP";
        else args << "--protocol=SOCKET" << "--socket=" + profile.advanced.socketFile;
        if (profile.tls) {
            if (!profile.ssl.caFile.isEmpty()) args << "--ssl-ca=" + profile.ssl.caFile;
            if (!profile.ssl.certificateFile.isEmpty()) args << "--ssl-cert=" + profile.ssl.certificateFile;
            if (!profile.ssl.keyFile.isEmpty()) args << "--ssl-key=" + profile.ssl.keyFile;
            if (!profile.ssl.tlsVersions.isEmpty()) args << "--tls-version=" + profile.ssl.tlsVersions;
            if (!profile.ssl.ciphers.isEmpty()) args << "--ssl-cipher=" + profile.ssl.ciphers;
        }
        args << "--single-transaction" << "--quick" << "--events" << "--routines" << "--triggers"
             << "--no-tablespaces" << "--skip-add-drop-table" << "--skip-add-drop-trigger" << "--hex-blob";
        if (!maria) args << "--set-gtid-purged=OFF";
        if (!withData) args << "--no-data";
        args << "--" << profile.database;
        auto env = QProcessEnvironment::systemEnvironment();
        env.remove("MYSQL_PWD");
        result = runBackupClient(backupClient(maria ? "mariadb-dump" : "mysqldump"), args, env, &output, task);
    }
    if (!result.isEmpty()) return result;
    if (!output.flush() || output.size() == 0) return "备份文件为空或无法写入";
    output.close();
    if (task.cancel->load()) return "任务已停止";
    if (!output.rename(QFileInfo(destination).absoluteFilePath())) return output.errorString();
    output.setAutoRemove(false);
    return {};
}

QString restorePostgres(const navidog::ConnectionProfile& profile, const QString& archive,
                        const navidog::TaskContext& task) {
    auto catalog = navidog::connectDatabase(profile);
    const auto check = catalog->execute(
        "SELECT 1 FROM pg_catalog.pg_namespace WHERE nspname NOT IN ('public','information_schema') "
        "AND left(nspname, 3) <> 'pg_' "
        "UNION ALL SELECT 1 FROM pg_catalog.pg_class c JOIN pg_catalog.pg_namespace n ON n.oid=c.relnamespace "
        "WHERE n.nspname='public' "
        "UNION ALL SELECT 1 FROM pg_catalog.pg_proc p JOIN pg_catalog.pg_namespace n ON n.oid=p.pronamespace "
        "WHERE n.nspname='public' "
        "UNION ALL SELECT 1 FROM pg_catalog.pg_type t JOIN pg_catalog.pg_namespace n ON n.oid=t.typnamespace "
        "WHERE n.nspname='public' "
        "UNION ALL SELECT 1 FROM pg_catalog.pg_extension WHERE extname <> 'plpgsql' LIMIT 1", {}, navidog::cancellation());
    if (!check.error.isEmpty()) return check.error;
    if (!check.rows.isEmpty()) return "请先新建一个空 PostgreSQL 数据库作为恢复目标";
    catalog.reset();
    if (task.cancel->load()) return "任务已停止";
    task.report({0, 0, navidog::TaskUnit::Items, "正在恢复 PostgreSQL 数据库"});
    return runBackupClient(backupClient("pg_restore"),
        {"--no-password", "--single-transaction", "--no-owner", "--no-privileges",
         "--dbname", pgConnectionArgument(profile.database), "--", archive}, pgEnvironment(profile), nullptr, task);
}
}

void MainWindow::dumpDatabaseSql(const QString& id, const navidog::CatalogNode& node, bool withData) {
    if (!quickBackup_.isEmpty()) { status_->setText("已有一项备份或转储正在运行"); return; }
    auto target = databaseToolProfile(id, node);
    if (!target || !navidog::supportsDatabaseSqlDump(target->driver, node.kind == "schema")) return;
    if (target->ssh.enabled || target->http.enabled || !target->ssl.serverName.isEmpty() ||
        !target->advanced.initialSql.isEmpty() ||
        (target->driver == DatabaseKind::PostgreSQL &&
         (!target->ssl.tlsVersions.isEmpty() || !target->ssl.ciphers.isEmpty()))) {
        showError("此连接暂不能使用 SQL 转储", "SQL 转储使用数据库客户端工具，需要直接连接及可由该工具应用的连接设置。");
        return;
    }
    const QString tool = target->driver == DatabaseKind::SQLite ? "sqlite3" :
                         target->driver == DatabaseKind::PostgreSQL ? "pg_dump" :
                         target->driver == DatabaseKind::MariaDB ? "mariadb-dump" : "mysqldump";
    if (backupClient(tool).isEmpty()) {
        showError("缺少数据库客户端", "请在 PATH 中提供 " + tool);
        return;
    }
    const auto destination = navidog::ui::chooseExportFile(this, "转储 SQL 文件",
        navidog::ui::exportContext(*target, node.database, node.kind == "schema" ? node.schema : QString{}),
        withData ? "结构与数据转储" : "结构转储", "SQL 文件 (*.sql)",
        nullptr, QFileDialog::DontConfirmOverwrite);
    if (!destination.isEmpty())
        startBackupOperation([target = *target, destination, withData,
                              schema = node.kind == "schema" ? node.schema : QString{}](const navidog::TaskContext& task) {
            return exportServerDatabase(target, destination, task, true, withData, schema);
        }, "转储数据库 SQL", destination, false,
        target->driver == DatabaseKind::SQLite ? QFileInfo(target->database).canonicalFilePath() :
                                                target->id + "/" + target->database);
}

void MainWindow::showQuickBackup() {
    QString id;
    const auto context = quickEntryContext(id);
    navidog::ui::Dialog dialog(this);
    dialog.setObjectName("quickBackupDialog");
    dialog.setWindowTitle("备份与恢复");
    dialog.resize(550, 185);
    auto* layout = new QVBoxLayout(&dialog);
    auto* form = new QFormLayout;
    auto* connection = new navidog::ui::Select;
    connection->setObjectName("backupConnection");
    for (const auto& p : profiles_)
        if (p.driver == DatabaseKind::SQLite || p.driver == DatabaseKind::MySQL || p.driver == DatabaseKind::PostgreSQL)
            connection->addItem(p.name + " · " + navidog::databaseKindName(p.driver), p.id);
    connection->setCurrentIndex(connection->findData(id));
    auto* database = new navidog::ui::TextInput;
    database->setObjectName("backupDatabase");
    form->addRow("连接", connection);
    form->addRow("数据库", database);
    layout->addLayout(form);
    auto* description = new QLabel;
    description->setWordWrap(true);
    layout->addWidget(description);
    auto* tools = new QHBoxLayout;
    tools->addStretch();
    auto* backup = new navidog::ui::PushButton("创建备份…");
    auto* restore = new navidog::ui::PushButton("恢复…");
    auto* close = new navidog::ui::PushButton("取消");
    auto* stop = new navidog::ui::PushButton("停止当前任务");
    backup->setObjectName("createSqliteBackup");
    restore->setObjectName("restoreSqliteBackup");
    for (auto* button : {backup, restore, stop, close}) tools->addWidget(button);
    stop->setVisible(!quickBackup_.isEmpty());
    layout->addLayout(tools);
    connect(close, &QPushButton::clicked, &dialog, &QDialog::reject);
    connect(stop, &QPushButton::clicked, &dialog, [this, &dialog] {
        if (!quickBackup_.isEmpty()) tasks_->cancel(quickBackup_);
        dialog.reject();
    });
    const auto update = [&] {
        const auto profile = std::find_if(profiles_.cbegin(), profiles_.cend(), [&](const auto& p) {
            return p.id == connection->currentData().toString();
        });
        const auto kind = profile != profiles_.cend() ? profile->driver : DatabaseKind::Unknown;
        database->setEnabled(kind == DatabaseKind::PostgreSQL || kind == DatabaseKind::MySQL);
        database->setText(profile == profiles_.cend() || kind == DatabaseKind::SQLite ? QString{} :
                          profile->id == id && !context.database.isEmpty() ? context.database : profile->database);
        backup->setEnabled(kind != DatabaseKind::Unknown && quickBackup_.isEmpty());
        restore->setEnabled((kind == DatabaseKind::SQLite || kind == DatabaseKind::PostgreSQL) && quickBackup_.isEmpty());
        description->setText(kind == DatabaseKind::PostgreSQL ? "用 pg_dump 创建数据库归档。恢复需要一个空数据库。"
            : kind == DatabaseKind::MySQL ? "用 mysqldump 导出结构、数据、例程和事件。事务表使用一致性快照。"
            : kind == DatabaseKind::SQLite ? "创建 SQLite 快照，或将备份恢复为新的数据库文件。"
            : "请选择 SQLite、MySQL 或 PostgreSQL 连接。");
    };
    connect(connection, QOverload<int>::of(&QComboBox::currentIndexChanged), &dialog, update);
    update();
    connect(backup, &QPushButton::clicked, &dialog, [&dialog] { dialog.done(2); });
    connect(restore, &QPushButton::clicked, &dialog, [&dialog] { dialog.done(3); });
    const auto action = dialog.exec();
    if (action != 2 && action != 3) return;
    auto selected = std::find_if(profiles_.begin(), profiles_.end(), [&](const auto& p) {
        return p.id == connection->currentData().toString();
    });
    if (selected == profiles_.end()) return;
    if (selected->driver != DatabaseKind::SQLite) {
        auto profile = *selected;
        profile.database = database->text().trimmed();
        if (profile.database.isEmpty() || profile.database.contains(QChar::Null)) {
            showError("请选择数据库", "填写要备份或恢复的数据库名称。"); return;
        }
        if (profile.ssh.enabled || profile.http.enabled || !profile.ssl.serverName.isEmpty() || !profile.advanced.initialSql.isEmpty() ||
            (profile.driver == DatabaseKind::PostgreSQL && (!profile.ssl.tlsVersions.isEmpty() || !profile.ssl.ciphers.isEmpty()))) {
            showError("连接不支持此备份工具", "请使用直接连接，并保留数据库需要的证书验证设置。"); return;
        }
        const bool pg = profile.driver == DatabaseKind::PostgreSQL;
        const bool restoring = action == 3;
        if (restoring && (!pg || profile.readOnly)) {
            showError("无法恢复", "请选择可写的 PostgreSQL 连接。"); return;
        }
        const QString tool = restoring ? "pg_restore" : pg ? "pg_dump" : "mysqldump";
        if (backupClient(tool).isEmpty()) { showError("缺少数据库客户端", "请安装并在 PATH 中提供 " + tool); return; }
        const auto file = restoring
            ? QFileDialog::getOpenFileName(this, "打开 PostgreSQL 归档", {}, "PostgreSQL 归档 (*.dump *.backup);;所有文件 (*)")
            : navidog::ui::chooseExportFile(this, "保存数据库备份", navidog::ui::exportContext(profile),
                "数据库备份", pg ? "PostgreSQL 归档 (*.dump)" : "SQL 文件 (*.sql)",
                nullptr, QFileDialog::DontConfirmOverwrite);
        if (file.isEmpty()) return;
        if (restoring && QMessageBox::question(this, "恢复 PostgreSQL 数据库",
                QString("将恢复到 %1 / %2。目标库须为空，且请确认归档来自可信来源。").arg(profile.name, profile.database),
                QMessageBox::Yes | QMessageBox::Cancel, QMessageBox::Cancel) != QMessageBox::Yes) return;
        if (!promptPassword(*selected)) return;
        profile.password = selected->password;
        startBackupOperation([profile, file, restoring](const navidog::TaskContext& task) {
            return restoring ? restorePostgres(profile, file, task) : exportServerDatabase(profile, file, task);
        }, restoring ? QString("恢复 PostgreSQL 数据库") : QString("备份数据库"),
           restoring ? profile.database : file, false, profile.id + "/" + profile.database, !restoring);
        return;
    }
    const auto source = action == 2 ? selected->database :
        QFileDialog::getOpenFileName(this, "选择 SQLite 备份", {}, "SQLite 文件 (*.sqlite *.sqlite3 *.db);;所有文件 (*)");
    if (source.isEmpty()) return;
    const auto destination = navidog::ui::chooseExportFile(this,
        action == 2 ? "保存 SQLite 备份" : "恢复为新的 SQLite 数据库",
        navidog::ui::exportContext(*selected), action == 2 ? "数据库备份" : "数据库恢复",
        "SQLite 文件 (*.sqlite *.sqlite3 *.db)", nullptr, QFileDialog::DontConfirmOverwrite);
    if (!destination.isEmpty()) startSqliteBackup(source, destination);
}

void MainWindow::startSqliteBackup(const QString& source, const QString& destination, bool scheduled) {
    startBackupOperation([source, destination](const navidog::TaskContext& task) {
        return copySqliteDatabase(source, destination, task);
    }, scheduled ? QString("定时备份 SQLite") : QString("复制 SQLite 数据库"),
       destination, scheduled, QFileInfo(source).canonicalFilePath());
}

void MainWindow::startBackupOperation(std::function<QString(const navidog::TaskContext&)> operation,
                                      const QString& title, const QString& destination, bool scheduled,
                                      const QString& resource, bool fileOutput) {
    if (!quickBackup_.isEmpty()) {
        if (!scheduled) status_->setText("已有一项备份或恢复正在运行");
        return;
    }
    const auto output = fileOutput ? QFileInfo(destination).absoluteFilePath() : QString{};
    QString rejection;
    quickBackup_ = tasks_->submit({title, destination, resource, output},
        [operation = std::move(operation), output](const navidog::TaskContext& task) {
            const navidog::OperationTimer timing(navidog::TimedOperation::BackupOrRestore);
            task.report({0, 0, navidog::TaskUnit::Items, "正在准备数据库客户端"});
            navidog::TaskResult result;
            result.error = operation(task);
            result.cancelled = !result.error.isEmpty() && task.cancel->load();
            if (result.error.isEmpty()) {
                result.detail = "操作已完成";
                if (!output.isEmpty()) result.artifacts << output;
            }
            return result;
        }, &rejection);
    if (quickBackup_.isEmpty()) {
        status_->setText(rejection);
        return;
    }
    auto connection = std::make_shared<QMetaObject::Connection>();
    *connection = connect(tasks_, &navidog::BackgroundTasks::finished, this,
            [this, connection, destination, scheduled](const QString& id, const navidog::TaskResult& result) {
        if (id != quickBackup_) return;
        QObject::disconnect(*connection);
        quickBackup_.clear();
        const auto error = result.cancelled && result.error.isEmpty() ? QString("任务已取消") : result.error;
        if (scheduled) {
            auto state = quickBackupSchedule_;
            state["lastFinished"] = QDateTime::currentDateTimeUtc().toString(Qt::ISODate);
            state["lastResult"] = error.isEmpty() ? QFileInfo(destination).fileName() : error;
            if (!saveQuickBackupSchedule(state)) quickBackupTimer_->stop();
        }
        status_->setText(error.isEmpty() ? "操作完成，可在任务列表查看结果。" :
                         result.cancelled ? "备份或恢复已取消" : "备份或恢复失败，可在任务列表查看详情。");
    });
    status_->setText("已提交到后台任务列表。");
}

bool MainWindow::saveQuickBackupSchedule(const QJsonObject& state) {
    try {
        navidog::LocalStore store(navidog::localStorePath(dataDirectory_));
        store.putDocument(scheduleKey, QJsonDocument(state).toJson(QJsonDocument::Compact));
        quickBackupSchedule_ = state;
        if (state["enabled"].toBool()) quickBackupTimer_->start();
        else quickBackupTimer_->stop();
        return true;
    } catch (const std::exception& e) {
        showError("无法保存自动运行设置", QString::fromUtf8(e.what()));
        return false;
    }
}

void MainWindow::loadQuickAutomation() {
    quickBackupTimer_ = new QTimer(this);
    quickBackupTimer_->setInterval(30000);
    connect(quickBackupTimer_, &QTimer::timeout, this, [this] { runScheduledBackup(); });
    try {
        navidog::LocalStore store(navidog::localStorePath(dataDirectory_));
        if (const auto state = store.document(scheduleKey)) {
            quickBackupSchedule_ = QJsonDocument::fromJson(*state).object();
            if (quickBackupSchedule_["enabled"].toBool()) quickBackupTimer_->start();
        }
    } catch (const std::exception& e) {
        status_->setText(QString("无法读取自动备份设置  %1").arg(QString::fromUtf8(e.what())));
    }
}

void MainWindow::runScheduledBackup(bool now) {
    if (!quickBackup_.isEmpty()) {
        if (now) status_->setText("已有一项备份或恢复正在运行");
        return;
    }
    const auto& state = quickBackupSchedule_;
    if (state.isEmpty() || (!now && !state["enabled"].toBool())) return;
    const auto last = QDateTime::fromString(state["lastFinished"].toString(), Qt::ISODate);
    const auto interval = std::clamp(state["interval"].toInt(60), 1, 1440);
    const auto current = QDateTime::currentDateTimeUtc();
    if (!now && last.isValid() && current < last.addSecs(interval * 60)) return;
    const auto id = state["connection"].toString();
    const auto source = state["source"].toString();
    const auto directory = state["directory"].toString();
    const auto profile = std::find_if(profiles_.cbegin(), profiles_.cend(), [&](const auto& p) { return p.id == id; });
    if (profile == profiles_.cend() || profile->driver != DatabaseKind::SQLite ||
        profile->database != source || !QFileInfo(directory).isDir()) {
        auto disabled = state;
        disabled["enabled"] = false;
        disabled["lastResult"] = "连接或备份目录已变化。请重新选择目标。";
        if (!saveQuickBackupSchedule(disabled)) quickBackupTimer_->stop();
        status_->setText(disabled["lastResult"].toString());
        return;
    }
    const auto scope = navidog::ui::exportContext(*profile);
    const auto timestamp = current.toLocalTime();
    const QDir output(directory);
    auto name = navidog::ui::exportStem(scope, "自动备份", timestamp) + ".sqlite";
    for (int copy = 2; output.exists(name); ++copy)
        name = navidog::ui::exportStem(scope, QString("自动备份-%1").arg(copy), timestamp) + ".sqlite";
    const auto file = output.filePath(name);
    startSqliteBackup(source, file, true);
}

void MainWindow::showQuickAutomation() {
    if (!quickBackup_.isEmpty()) { status_->setText("请等当前备份或恢复结束后再编辑自动运行设置"); return; }
    navidog::ui::Dialog dialog(this);
    dialog.setObjectName("quickAutomationDialog");
    dialog.setWindowTitle("自动运行");
    dialog.resize(570, 280);
    auto* layout = new QVBoxLayout(&dialog);
    auto* headline = new QLabel("SQLite 定时备份");
    headline->setObjectName("sectionTitle");
    layout->addWidget(headline);
    auto* form = new QFormLayout;
    auto* connection = new navidog::ui::Select;
    connection->setObjectName("automationConnection");
    for (const auto& p : profiles_)
        if (p.driver == DatabaseKind::SQLite) {
            connection->addItem(p.name, p.id);
            connection->setItemData(connection->count() - 1, p.database, Qt::UserRole + 1);
        }
    if (const auto index = connection->findData(quickBackupSchedule_["connection"].toString()); index >= 0)
        connection->setCurrentIndex(index);
    form->addRow("SQLite 连接", connection);
    auto* folderRow = new QHBoxLayout;
    auto* folder = new navidog::ui::TextInput;
    folder->setObjectName("automationBackupFolder");
    folder->setText(quickBackupSchedule_["directory"].toString(navidog::ui::exportDirectory(this)));
    auto* choose = new navidog::ui::PushButton("选择…");
    folderRow->addWidget(folder, 1); folderRow->addWidget(choose);
    form->addRow("备份目录", folderRow);
    auto* interval = new navidog::ui::NumberInput;
    interval->setRange(1, 1440);
    interval->setValue(quickBackupSchedule_["interval"].toInt(60));
    interval->setSuffix(" 分钟");
    form->addRow("执行间隔", interval);
    auto* enabled = new navidog::ui::CheckBox("在 Navidog 运行时自动备份");
    enabled->setChecked(quickBackupSchedule_["enabled"].toBool());
    form->addRow({}, enabled);
    layout->addLayout(form);
    auto* last = new QLabel(quickBackupSchedule_["lastResult"].toString("还没有运行记录"));
    last->setTextFormat(Qt::PlainText);
    last->setWordWrap(true);
    last->setObjectName("muted");
    layout->addWidget(last);
    auto* buttons = new navidog::ui::FormButtons;
    buttons->setStandardButtons(QDialogButtonBox::Save | QDialogButtonBox::Cancel);
    auto* run = buttons->addButton("保存并运行一次", QDialogButtonBox::ActionRole);
    run->setEnabled(quickBackup_.isEmpty());
    layout->addWidget(buttons);
    connect(choose, &QPushButton::clicked, &dialog, [&] {
        const auto path = QFileDialog::getExistingDirectory(&dialog, "选择备份目录", folder->text());
        if (!path.isEmpty()) folder->setText(path);
    });
    connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    const auto save = [&](bool now) {
        const auto output = QFileInfo(folder->text()).canonicalFilePath();
        if (connection->currentIndex() < 0 || output.isEmpty() || !QFileInfo(output).isDir()) {
            QMessageBox::warning(&dialog, "备份配置未保存", "请选择 SQLite 连接和现有的备份目录。"); return;
        }
        auto state = quickBackupSchedule_;
        state["connection"] = connection->currentData().toString();
        state["source"] = connection->currentData(Qt::UserRole + 1).toString();
        state["directory"] = output;
        state["interval"] = interval->value();
        state["enabled"] = enabled->isChecked();
        if (state["enabled"].toBool() && state["lastFinished"].toString().isEmpty())
            state["lastFinished"] = QDateTime::currentDateTimeUtc().toString(Qt::ISODate);
        if (saveQuickBackupSchedule(state)) {
            dialog.accept();
            if (now) runScheduledBackup(true);
        }
    };
    connect(buttons, &QDialogButtonBox::accepted, &dialog, [&] { save(false); });
    connect(run, &QPushButton::clicked, &dialog, [&] { save(true); });
    dialog.exec();
}
