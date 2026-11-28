#include "application_help.hpp"
#include "components.hpp"
#include "startup_timing.hpp"
#include "navidog/operation_timing.hpp"

#include <QCoreApplication>
#include <QDesktopServices>
#include <QFile>
#include <QFontDatabase>
#include <QFontInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QMessageBox>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QRegularExpression>
#include <QVersionNumber>
#include <algorithm>
#include <functional>
#include <utility>

namespace {
const auto latestReleaseApi =
    "https://api.github.com/repos/duhbbx/Navidog/releases/latest";
const auto releasePages = "https://github.com/duhbbx/Navidog/releases/tag/";
constexpr int maxReleaseBytes = 512 * 1024;
constexpr int maxLogBytes = 128 * 1024;

QVersionNumber stableVersion(const QString &text) {
  static const QRegularExpression pattern(
      QStringLiteral("\\Av?([0-9]+\\.[0-9]+\\.[0-9]+)\\z"));
  const auto match = pattern.match(text);
  if (!match.hasMatch())
    return {};
  const auto version = QVersionNumber::fromString(match.captured(1));
  return version.segmentCount() == 3 ? version : QVersionNumber{};
}

QString alignedTimingLog(const QString &text, int columns) {
  auto lines = text.split('\n');
  QVector<qsizetype> widths(columns - 1, 0);
  for (const auto &line : std::as_const(lines)) {
    const auto fields = line.split('\t');
    if (fields.size() == columns)
      for (int column = 0; column < widths.size(); ++column)
        widths[column] = std::max<qsizetype>(widths[column], fields[column].size());
  }
  for (auto &line : lines) {
    const auto fields = line.split('\t');
    if (fields.size() != columns)
      continue;
    line.clear();
    for (int column = 0; column < widths.size(); ++column)
      line += fields[column].leftJustified(widths[column] + 2);
    line += fields.back();
  }
  return lines.join('\n');
}

class TimingLogDialog final : public navidog::ui::Dialog {
public:
  TimingLogDialog(QWidget *owner, bool operations, const QString &directory,
                  std::function<QString()> readReport)
      : Dialog(owner), readReport_(std::move(readReport)), columns_(operations ? 4 : 3) {
    setObjectName(operations ? "operationLogDialog" : "startupLogDialog");
    setWindowTitle(operations ? "运行耗时日志" : "启动日志");
    setAttribute(Qt::WA_DeleteOnClose);
    resize(operations ? 840 : 720, 430);

    auto *layout = new QVBoxLayout(this);
    if (operations)
      layout->addWidget(new QLabel(
          QString("记录达到 %1 ms 的操作。ui 为界面线程，worker 为后台线程。")
              .arg(navidog::operationLogThresholdMs), this));
    text_ = new QPlainTextEdit(this);
    text_->setObjectName(operations ? "operationLogText" : "startupLogText");
    text_->setReadOnly(true);
    text_->setLineWrapMode(QPlainTextEdit::NoWrap);
    auto font = QFontDatabase::systemFont(QFontDatabase::FixedFont);
    font.setStyleHint(QFont::Monospace);
    if (!QFontInfo(font).fixedPitch())
      font.setFamily("monospace");
    // Use the resolved platform font so the app's global font rule cannot
    // override the fixed pitch needed to align the report columns.
    text_->setStyleSheet(QStringLiteral("font-family: \"%1\"; font-size: 13px;")
                            .arg(QFontInfo(font).family()));
    layout->addWidget(text_, 1);

    auto *buttons = new QDialogButtonBox(this);
    if (operations) {
      auto *refreshButton = buttons->addButton("刷新", QDialogButtonBox::ActionRole);
      refreshButton->setObjectName("refreshOperationLog");
      connect(refreshButton, &QPushButton::clicked, this, &TimingLogDialog::refresh);
    }
    auto *copy = buttons->addButton("复制日志", QDialogButtonBox::ActionRole);
    auto *folder = buttons->addButton("打开日志目录", QDialogButtonBox::ActionRole);
    auto *close = buttons->addButton("关闭", QDialogButtonBox::RejectRole);
    layout->addWidget(buttons);
    connect(copy, &QPushButton::clicked, this, [this] {
      QApplication::clipboard()->setText(report_);
    });
    connect(folder, &QPushButton::clicked, this, [directory] {
      QDesktopServices::openUrl(QUrl::fromLocalFile(directory));
    });
    connect(close, &QPushButton::clicked, this, &QDialog::close);
    refresh();
  }

private:
  void refresh() {
    report_ = readReport_();
    auto display = alignedTimingLog(report_, columns_);
    if (columns_ == 4 && report_.count('\n') == 1)
      display += "\n暂无达到阈值的记录。";
    text_->setPlainText(display);
    if (columns_ == 4)
      text_->moveCursor(QTextCursor::End);
  }

  std::function<QString()> readReport_;
  int columns_;
  QPlainTextEdit *text_;
  QString report_;
};
}

ReleaseChecker::ReleaseChecker(QString currentVersion, QUrl endpoint,
                               QObject *parent)
    : QObject(parent), currentVersion_(std::move(currentVersion)),
      endpoint_(std::move(endpoint)) {}

void ReleaseChecker::check() {
  if (reply_)
    return;
  if (!network_)
    network_ = new QNetworkAccessManager(this);
  QNetworkRequest request(endpoint_);
  request.setAttribute(QNetworkRequest::RedirectPolicyAttribute,
                       QNetworkRequest::ManualRedirectPolicy);
  request.setRawHeader("Accept", "application/vnd.github+json");
  request.setRawHeader("User-Agent", "Navidog-UpdateChecker");
  auto *reply = reply_ = network_->get(request);
  reply->setReadBufferSize(maxReleaseBytes + 1);
  connect(reply, &QIODevice::readyRead, this, [reply] {
    if (reply->bytesAvailable() > maxReleaseBytes)
      reply->abort();
  });
  QTimer::singleShot(8000, reply, [reply] {
    if (reply->isRunning()) {
      reply->setProperty("updateCheckTimedOut", true);
      reply->abort();
    }
  });
  connect(reply, &QNetworkReply::finished, this, [this, reply] {
    reply_ = nullptr;
    reply->deleteLater();
    ReleaseCheckResult result;
    const auto status =
        reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
    if (reply->property("updateCheckTimedOut").toBool()) {
      result.error = "连接 GitHub 超时，请稍后重试。";
    } else if (reply->bytesAvailable() > maxReleaseBytes) {
      result.error = "GitHub 发布信息超过大小限制。";
    } else if (status == 403 || status == 429) {
      result.error = "GitHub 暂时限制了请求，请稍后重试。";
    } else if (status == 404) {
      result.error = "GitHub 上暂时没有正式发布的版本。";
    } else if (status != 200 || reply->error() != QNetworkReply::NoError) {
      result.error = status ? QString("GitHub 请求失败（HTTP %1）。").arg(status)
                            : "无法连接 GitHub，请检查网络后重试。";
    } else {
      const auto object = QJsonDocument::fromJson(reply->readAll()).object();
      const auto tag = object.value("tag_name").toString();
      const auto latest = stableVersion(tag);
      const auto current = stableVersion(currentVersion_);
      if (latest.isNull() || current.isNull() || object.value("draft").toBool() ||
          object.value("prerelease").toBool()) {
        result.error = "无法识别 GitHub 返回的正式版本。";
      } else {
        result.latestVersion = latest.toString();
        if (latest > current)
          result.releasePage = QUrl(QString::fromLatin1(releasePages) + tag);
      }
    }
    emit finished(result);
  });
}

ApplicationHelp::ApplicationHelp(QMenu *menu, QWidget *owner,
                                 const QString &dataDirectory,
                                 const StartupTiming *startupTiming)
    : QObject(owner), owner_(owner),
      dataDirectory_(dataDirectory),
      startupTiming_(startupTiming),
      checker_(QCoreApplication::applicationVersion(), QUrl(latestReleaseApi)) {
  checkAction_ = menu->addAction("检查更新…", this, [this] {
    checkForUpdates(true);
  });
  checkAction_->setObjectName("menuCheckForUpdates");
  connect(&checker_, &ReleaseChecker::finished, this,
          &ApplicationHelp::showUpdateResult);
  menu->addAction("查看启动日志…", this, [this] { showTimingLog(false); })
      ->setObjectName("menuStartupLog");
  menu->addAction("查看运行耗时日志…", this, [this] { showTimingLog(true); })
      ->setObjectName("menuOperationLog");
}

void ApplicationHelp::checkForUpdates(bool manual) {
  if (updateDialog_) {
    if (manual) {
      updateDialog_->raise();
      updateDialog_->activateWindow();
    }
    return;
  }
  manualCheck_ |= manual;
  checkAction_->setText("正在检查更新…");
  checker_.check();
}

void ApplicationHelp::showUpdateResult(const ReleaseCheckResult &result) {
  const bool manual = std::exchange(manualCheck_, false);
  checkAction_->setText("检查更新…");
  if (!manual &&
      (result.releasePage.isEmpty() || result.releasePage == lastOfferedRelease_))
    return;

  auto *dialog = new QMessageBox(owner_);
  updateDialog_ = dialog;
  dialog->setObjectName("updateCheckResult");
  dialog->setAttribute(Qt::WA_DeleteOnClose);
  dialog->setAttribute(Qt::WA_ShowWithoutActivating, !manual);
  dialog->setWindowTitle("检查更新");
  dialog->setWindowModality(Qt::NonModal);
  dialog->setTextFormat(Qt::PlainText);
  dialog->setIcon(result.error.isEmpty() ? QMessageBox::Information
                                        : QMessageBox::Warning);
  const auto current = QCoreApplication::applicationVersion();
  if (!result.error.isEmpty()) {
    dialog->setText(result.error);
  } else if (result.releasePage.isEmpty()) {
    dialog->setText(result.latestVersion == current
        ? QString("当前版本 %1 已是最新版本。").arg(current)
        : QString("当前版本 %1。GitHub 最新正式版本为 %2。")
              .arg(current, result.latestVersion));
  } else {
    lastOfferedRelease_ = result.releasePage;
    dialog->setText(QString("Navidog %1 已发布。当前版本 %2。")
                        .arg(result.latestVersion, current));
    auto *release = dialog->addButton("查看 GitHub 发布", QMessageBox::ActionRole);
    connect(release, &QPushButton::clicked, dialog, [page = result.releasePage] {
      QDesktopServices::openUrl(page);
    });
  }
  dialog->addButton("关闭", QMessageBox::RejectRole);
  dialog->show();
}

void ApplicationHelp::showTimingLog(bool operations) {
  auto &dialog = operations ? operationLogDialog_ : logDialog_;
  if (dialog) {
    dialog->raise();
    dialog->activateWindow();
    return;
  }
  dialog = new TimingLogDialog(owner_, operations, dataDirectory_, [this, operations] {
    if (operations)
      return QString::fromUtf8(navidog::operationLogSnapshot().text);
    if (startupTiming_)
      return QString::fromUtf8(startupTiming_->report());
    QFile file(QDir(dataDirectory_).filePath("startup.log"));
    return file.open(QIODevice::ReadOnly)
        ? QString::fromUtf8(file.read(maxLogBytes))
        : QString("无法读取启动日志。\n%1").arg(file.errorString());
  });
  dialog->show();
}
