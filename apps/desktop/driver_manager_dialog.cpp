#include "driver_manager_dialog.hpp"
#include "components.hpp"
#include "navidog/database.hpp"
#include "navidog/driver_packages.hpp"
#include <QCryptographicHash>
#include <QDesktopServices>
#include <QDir>
#include <QFileInfo>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QProcess>
#include <QProgressBar>
#include <QSaveFile>
#include <QScrollArea>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QVBoxLayout>

namespace {
using navidog::DriverDownload;
using navidog::DriverPackage;
using navidog::DatabaseKind;

class DriverManager final : public navidog::ui::Dialog {
    QNetworkAccessManager network_;
    QNetworkReply* reply_ = nullptr;
    QProcess extract_;
    std::unique_ptr<QTemporaryDir> stage_;
    std::unique_ptr<QSaveFile> downloadFile_;
    QCryptographicHash hash_{QCryptographicHash::Sha256};
    DriverPackage package_ = DriverPackage::Java;
    DriverDownload download_;
    QVBoxLayout* rows_;
    QLabel* status_;
    QProgressBar* progress_;
    QVector<QWidget*> rowWidgets_;
    qint64 downloaded_ = 0;
    const DatabaseKind selected_;

    QString assetPath() const { return stage_->filePath("download.tar.gz"); }

    void reset(const QString& message) {
        stage_.reset();
        downloadFile_.reset();
        progress_->hide();
        for (auto* row : rowWidgets_) row->setEnabled(true);
        status_->setText(message);
        updateRows();
    }

    QNetworkReply* request(const QUrl& url) {
        QNetworkRequest request(url);
        request.setRawHeader("User-Agent", "Navidog-DriverManager");
        request.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::NoLessSafeRedirectPolicy);
        request.setTransferTimeout(30000);
        reply_ = network_.get(request);
        reply_->setReadBufferSize(128 * 1024);
        return reply_;
    }

    void readRelease() {
        const QUrl url(QStringLiteral("https://api.github.com/repos/duhbbx/Navidog/releases/tags/v" NAVIDOG_VERSION));
        auto* reply = request(url);
        auto metadata = std::make_shared<QByteArray>();
        connect(reply, &QIODevice::readyRead, this, [reply, metadata] {
            metadata->append(reply->readAll());
            if (metadata->size() > 1024 * 1024) reply->abort();
        });
        connect(reply, &QNetworkReply::finished, this, [this, reply, metadata] {
            metadata->append(reply->readAll());
            reply_ = nullptr;
            reply->deleteLater();
            if (reply->error() != QNetworkReply::NoError) {
                reset("无法读取此版本的 GitHub Release。请检查网络或稍后重试。");
                return;
            }
            auto asset = navidog::githubDriverAsset(download_, *metadata);
            if (asset.url.isEmpty()) {
                reset("此版本尚未发布与当前平台匹配的组件。");
                return;
            }
            download_ = std::move(asset);
            startDownload();
        });
    }

    void startDownload() {
        const auto target = navidog::driverPackageDirectory(package_);
        if (!QDir().mkpath(QFileInfo(target).absolutePath())) { reset("无法创建组件目录"); return; }
        if (download_.archive) {
            stage_ = std::make_unique<QTemporaryDir>(QFileInfo(target).absolutePath() + "/.install-XXXXXX");
            if (!stage_->isValid()) { reset("无法创建临时组件目录"); return; }
        } else if (!QDir().mkpath(target)) { reset("无法创建 JDBC 驱动目录"); return; }
        const auto file = download_.archive ? assetPath() : QDir(target).filePath(download_.asset);
        downloadFile_ = std::make_unique<QSaveFile>(file);
        if (!downloadFile_->open(QIODevice::WriteOnly)) { reset("无法写入组件文件"); return; }
        hash_.reset();
        downloaded_ = 0;
        status_->setText("正在下载 " + download_.asset);
        auto* reply = request(download_.url);
        const auto consume = [this, reply] {
            const auto bytes = reply->readAll();
            downloaded_ += bytes.size();
            if (downloaded_ > download_.maxBytes ||
                downloadFile_->write(bytes) != bytes.size()) { reply->abort(); return; }
            hash_.addData(bytes);
        };
        connect(reply, &QIODevice::readyRead, this, consume);
        connect(reply, &QNetworkReply::downloadProgress, this, [this](qint64 received, qint64 total) {
            if (total > 0 && total <= download_.maxBytes) {
                progress_->setRange(0, 100);
                progress_->setValue(int(received * 100 / total));
            }
        });
        connect(reply, &QNetworkReply::finished, this, [this, reply, consume] {
            // A network error or an abort may complete synchronously during a read.
            if (reply->error() == QNetworkReply::NoError && reply->bytesAvailable() > 0) consume();
            reply_ = nullptr;
            reply->deleteLater();
            if (reply->error() != QNetworkReply::NoError || downloaded_ > download_.maxBytes ||
                hash_.result() != download_.sha256) {
                reset("组件下载未完成，或 SHA-256 校验失败。");
                return;
            }
            if (!downloadFile_->commit()) { reset("无法保存已校验的组件"); return; }
            downloadFile_.reset();
            if (download_.archive) unpack();
            else reset("JDBC 驱动已安装，可以返回连接窗口测试。");
        });
    }

    void unpack() {
        const auto payload = stage_->filePath("payload");
        if (!QDir().mkdir(payload)) { reset("无法创建组件解压目录"); return; }
#ifdef Q_OS_WIN
        const auto tar = QStandardPaths::findExecutable("tar.exe", {qEnvironmentVariable("SystemRoot") + "/System32"});
#else
        const auto tar = QStandardPaths::findExecutable("tar", {"/usr/bin", "/bin"});
#endif
        if (tar.isEmpty()) { reset("系统缺少 tar，无法解压组件"); return; }
        status_->setText("下载校验通过，正在解压");
        progress_->setRange(0, 0);
        extract_.start(tar, {"-xzf", assetPath(), "--no-same-owner", "-C", payload});
    }

    void install(DriverPackage package) {
        package_ = package;
        download_ = navidog::driverDownload(package);
        if (download_.asset.isEmpty()) return;
        for (auto* row : rowWidgets_) row->setEnabled(false);
        status_->setText("正在读取组件信息");
        progress_->setRange(0, 0);
        progress_->show();
        if (download_.archive) readRelease();
        else startDownload();
    }

    void updateRows() {
        for (auto* widget : rowWidgets_) {
            rows_->removeWidget(widget);
            widget->hide();
            widget->deleteLater();
        }
        rowWidgets_.clear();

        auto add = [this](const QString& title, const QString& summary, const QString& action,
                         const std::function<void()>& run = {}) {
            auto* row = new QWidget(this);
            auto* layout = new QHBoxLayout(row);
            layout->setContentsMargins(0, 4, 0, 4);
            auto* text = new QVBoxLayout;
            auto* name = new QLabel(title, row);
            name->setTextFormat(Qt::PlainText);
            auto font = name->font();
            font.setBold(true);
            name->setFont(font);
            auto* detail = new QLabel(summary, row);
            detail->setTextFormat(Qt::PlainText);
            detail->setWordWrap(true);
            text->addWidget(name);
            text->addWidget(detail);
            layout->addLayout(text, 1);
            if (run) {
                auto* button = new navidog::ui::PushButton(action, row);
                layout->addWidget(button, 0, Qt::AlignVCenter);
                connect(button, &QPushButton::clicked, this, run);
            }
            rows_->addWidget(row);
            rowWidgets_ << row;
        };

        auto packageRow = [&](DriverPackage package, const QString& name, bool supported, bool installed) {
            const auto spec = navidog::driverDownload(package);
            if (!supported || spec.asset.isEmpty()) add(name, "此构建未提供下载组件", {});
            else if (installed) add(name, "已安装 · " + spec.asset, {});
            else add(name, spec.asset, "下载安装", [this, package] { install(package); });
        };
        const bool jdbc = navidog::jdbcBridgeSupported();
        const auto java = QFileInfo(navidog::javaPackageExecutable());
        if (selected_ == DatabaseKind::Unknown || navidog::usesJdbcDriver(selected_)) {
            packageRow(DriverPackage::Java, "Java 25 运行时", jdbc, java.isFile() && java.isExecutable());
            const std::pair<DatabaseKind, DriverPackage> jars[] = {
                {DatabaseKind::H2, DriverPackage::H2}, {DatabaseKind::Dameng, DriverPackage::Dameng},
                {DatabaseKind::Db2, DriverPackage::Db2}, {DatabaseKind::Kingbase, DriverPackage::Kingbase},
                {DatabaseKind::GaussDb, DriverPackage::GaussDb}
            };
            for (const auto& entry : jars) {
                const auto kind = entry.first;
                if (selected_ == DatabaseKind::Unknown || selected_ == kind)
                    packageRow(entry.second, navidog::databaseKindName(kind) + " JDBC", jdbc,
                               !navidog::managedJdbcJar(kind).isEmpty());
            }
        }
        if (selected_ == DatabaseKind::Unknown || selected_ == DatabaseKind::Redis) {
            const auto library = QDir(navidog::driverPackageDirectory(DriverPackage::Redis))
                                     .filePath(navidog::redisPackageLibraryName());
            if (navidog::managedRedisSupported())
                packageRow(DriverPackage::Redis, "Redis 原生驱动", true, QFileInfo(library).isFile());
            else if (navidog::availableDrivers().contains(DatabaseKind::Redis))
                add("Redis 原生驱动", "使用此构建的系统 hiredis", {});
        }
        struct Vendor { DatabaseKind kind; const char* label; const char* help; const char* url; };
        const Vendor vendors[] = {
            {DatabaseKind::UxDb, "优炫 JDBC", "在连接设置的高级页选择厂商 JAR", "https://license.uxsino.com/download"},
            {DatabaseKind::Oracle, "Oracle Instant Client", "解压后在连接的高级页选择客户端目录",
                "https://www.oracle.com/database/technologies/instant-client/downloads.html"},
            {DatabaseKind::SQLServer, "SQL Server ODBC", "安装微软客户端后，Navidog 会检测驱动",
                "https://learn.microsoft.com/sql/connect/odbc/download-odbc-driver-for-sql-server"},
        };
        for (const auto& vendor : vendors)
            if (selected_ == DatabaseKind::Unknown || selected_ == vendor.kind)
                add(vendor.label, vendor.help, "官方下载", [url=QUrl(vendor.url)] { QDesktopServices::openUrl(url); });
        for (const auto& driver : navidog::driverRegistry()) {
            const bool defaultNative = driver.id == DatabaseKind::SQLite || driver.id == DatabaseKind::MySQL ||
                driver.id == DatabaseKind::PostgreSQL || driver.id == DatabaseKind::OceanBaseOracle;
            const bool selectedNative = driver.id == selected_ && !navidog::usesJdbcDriver(selected_) &&
                selected_ != DatabaseKind::Redis && selected_ != DatabaseKind::Oracle && selected_ != DatabaseKind::SQLServer;
            if ((selected_ == DatabaseKind::Unknown && defaultNative) || selectedNative)
                add(driver.name + " · " + driver.provider, driver.status, {});
        }
    }

public:
    explicit DriverManager(QWidget* parent, DatabaseKind selected) : Dialog(parent), selected_(selected) {
        setObjectName("driverManager");
        setWindowTitle("驱动与运行时");
        resize(650, 555);
        auto* layout = new QVBoxLayout(this);
        auto* scroll = new QScrollArea(this);
        scroll->setWidgetResizable(true);
        auto* content = new QWidget(scroll);
        rows_ = new QVBoxLayout(content);
        rows_->setAlignment(Qt::AlignTop);
        scroll->setWidget(content);
        layout->addWidget(scroll, 1);
        status_ = new QLabel(this);
        status_->setTextFormat(Qt::PlainText);
        status_->setWordWrap(true);
        layout->addWidget(status_);
        progress_ = new QProgressBar(this);
        progress_->hide();
        layout->addWidget(progress_);
        auto* close = new navidog::ui::PushButton("关闭", this);
        layout->addWidget(close, 0, Qt::AlignRight);
        connect(close, &QPushButton::clicked, this, &QDialog::reject);
        connect(&extract_, &QProcess::errorOccurred, this, [this](QProcess::ProcessError error) {
            if (error == QProcess::FailedToStart) reset("无法运行系统 tar");
        });
        connect(&extract_, qOverload<int,QProcess::ExitStatus>(&QProcess::finished), this,
                [this](int code, QProcess::ExitStatus exit) {
            if (!stage_) return;
            const QDir root(stage_->filePath("payload"));
            const auto target = navidog::driverPackageDirectory(package_);
            const bool java = package_ == DriverPackage::Java;
            const QString installedFile = java ? QDir(target).relativeFilePath(navidog::javaPackageExecutable())
                                               : navidog::redisPackageLibraryName();
            const QFileInfo file(root.filePath(installedFile));
            if (code || exit != QProcess::NormalExit || !file.isFile() ||
                !file.canonicalFilePath().startsWith(root.canonicalPath() + '/') ||
                (java && (!file.isExecutable() || !QFileInfo(root.filePath("release")).isFile()))) {
                reset("组件解压失败，或组件内容与平台不匹配。");
                return;
            }
            if (!QDir().rename(root.absolutePath(), target)) { reset("无法安装组件，请检查目录权限。"); return; }
            reset(java ? "Java 运行时已安装。" : "Redis 原生驱动已安装。");
        });
        updateRows();
    }

    ~DriverManager() override {
        if (reply_) { reply_->disconnect(this); reply_->abort(); }
        if (extract_.state() != QProcess::NotRunning) {
            extract_.disconnect(this);
            extract_.kill();
            extract_.waitForFinished(1000);
        }
    }
};
}

void showDriverManager(QWidget* parent, DatabaseKind selected) {
    DriverManager dialog(parent, selected);
    dialog.exec();
}
