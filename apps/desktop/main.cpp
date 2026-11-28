#include "main_window.hpp"
#include "operation_monitor.hpp"
#include "startup_timing.hpp"
#include <QApplication>
#include <QCommandLineParser>
#include <QStandardPaths>
#include <QTimer>
#include <QDir>
#include <QIcon>
#include <QLockFile>
#include <QMessageBox>
#include <QTranslator>
#include <QLibraryInfo>

int main(int argc, char** argv) {
    StartupTiming startup;
    QApplication app(argc, argv);
    startup.mark("qt_application");
    app.setApplicationName("Navidog");
    app.setOrganizationName("Navidog");
    app.setApplicationDisplayName("Navidog");
    app.setApplicationVersion(NAVIDOG_VERSION);
#ifndef Q_OS_MACOS
    app.setWindowIcon(QIcon(QStringLiteral(":/branding/navidog.png")));
#endif
#ifdef Q_OS_LINUX
    app.setDesktopFileName(QStringLiteral("navidog"));
#endif

    QTranslator qtChinese;
#if QT_VERSION >= QT_VERSION_CHECK(6,0,0)
    const auto translationPath=QLibraryInfo::path(QLibraryInfo::TranslationsPath);
#else
    const auto translationPath=QLibraryInfo::location(QLibraryInfo::TranslationsPath);
#endif
    const auto appDirectory=QCoreApplication::applicationDirPath();
    if (qtChinese.load("qtbase_zh_CN",QDir(appDirectory).filePath("../Resources/translations")) ||
        qtChinese.load("qtbase_zh_CN",QDir(appDirectory).filePath("translations")) ||
        qtChinese.load("qtbase_zh_CN",translationPath))
        app.installTranslator(&qtChinese);
    startup.mark("translation");

    QCommandLineParser parser;
    parser.setApplicationDescription("Navidog — 跨平台数据库工作台");
    parser.addHelpOption();
    parser.addVersionOption();

    parser.addOption({"data-dir", "Local workspace directory", "path"});
    parser.addOption({"screenshot", "Run demo, save a screenshot and exit", "path"});
    parser.process(app);

    const auto legacy=QDir(QStandardPaths::writableLocation(QStandardPaths::GenericDataLocation)).filePath("Harbor/HarborDB");
    // Renaming the app must preserve the existing workspace, profiles and drafts.
    const auto data = parser.isSet("data-dir") ? parser.value("data-dir") :
    (QFileInfo::exists(QDir(legacy).filePath("connections.json")) || QFileInfo::exists(QDir(legacy).filePath("navidog.sqlite")))
    ? legacy : QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation);

    QDir().mkpath(data);
    QLockFile workspaceLock(QDir(data).filePath("harbor.lock"));
    workspaceLock.setStaleLockTime(0);
    if (!workspaceLock.tryLock()) {
        QMessageBox::information(nullptr,"Navidog","此工作区已在运行，请切换到现有窗口。");
        return 1;
    }
    startup.mark("workspace_path_and_lock");

    OperationMonitor operations(data);
    MainWindow window(data, nullptr, &startup);
    startup.mark("window_ready_to_show");
    QObject::connect(&app, &QCoreApplication::aboutToQuit, &window, [&] { startup.write(data); });
    window.show();
    startup.mark("window_show_returned");
    QTimer::singleShot(1500, &window, [&] { startup.write(data); });
    if (!parser.isSet("screenshot")) {
        window.scheduleUpdateCheck();
        QTimer::singleShot(1000, &operations, &OperationMonitor::start);
    }
    if (parser.isSet("screenshot")) {
        const auto path = parser.value("screenshot");
        QObject::connect(&window, &MainWindow::queryFinished, &app, [&window, &app, path] {
            QTimer::singleShot(250, &app, [&window, &app, path] { const bool ok = window.grab().save(path); app.exit(ok ? 0 : 2); });
        });
        QTimer::singleShot(30000, &app, [&app] {
            app.exit(3);
        });
    }
    if (!window.restoredSession()) {
        QTimer::singleShot(100, &window, &MainWindow::runCurrent);
    }

    return app.exec();
}
