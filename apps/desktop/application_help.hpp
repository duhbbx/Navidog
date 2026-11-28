#pragma once

#include <QObject>
#include <QPointer>
#include <QString>
#include <QUrl>

class QAction;
class QDialog;
class QMenu;
class QNetworkAccessManager;
class QNetworkReply;
class QWidget;
class StartupTiming;

struct ReleaseCheckResult {
  QString latestVersion;
  QUrl releasePage;
  QString error;
};

class ReleaseChecker final : public QObject {
  Q_OBJECT

public:
  ReleaseChecker(QString currentVersion, QUrl endpoint,
                 QObject *parent = nullptr);
  void check();

signals:
  void finished(const ReleaseCheckResult &result);

private:
  QString currentVersion_;
  QUrl endpoint_;
  QNetworkAccessManager *network_ = nullptr;
  QNetworkReply *reply_ = nullptr;
};

class ApplicationHelp final : public QObject {
public:
  ApplicationHelp(QMenu *menu, QWidget *owner, const QString &dataDirectory,
                  const StartupTiming *startupTiming);
  void checkForUpdates(bool manual);

private:
  void showTimingLog(bool operations);
  void showUpdateResult(const ReleaseCheckResult &result);

  QWidget *owner_;
  QString dataDirectory_;
  const StartupTiming *startupTiming_;
  ReleaseChecker checker_;
  QAction *checkAction_;
  QPointer<QDialog> logDialog_, operationLogDialog_, updateDialog_;
  bool manualCheck_ = false;
  QUrl lastOfferedRelease_;
};
