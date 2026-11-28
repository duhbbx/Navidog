#include "driver_manager_dialog.hpp"
#include "navidog/database_sql.hpp"
#include "main_window.hpp"
#include "export_paths.hpp"
#include "application_help.hpp"
#include "components.hpp"
#include "connection_controller.hpp"
#include "data_pane.hpp"
#include "editor.hpp"
#include "editor_sql_tools.hpp"
#include "er_pane.hpp"
#include "metadata_controller.hpp"
#include "namespace_dialog.hpp"
#include "navidog/credentials.hpp"
#include "navidog/database.hpp"
#include "navidog/local_store.hpp"
#include "navigation_pane.hpp"
#include "objects_pane.hpp"
#include "query_pane.hpp"
#include "query_session_controller.hpp"
#include "quick_action_icons.hpp"
#include "quick_entry_bar.hpp"
#include "result_model.hpp"
#include "source_pane.hpp"
#include "sql_formatting.hpp"
#include "startup_timing.hpp"
#include "table_design_controller.hpp"
#include "table_pane.hpp"
#include "table_report_dialog.hpp"
#include "table_task_dialog.hpp"
#include "workspace_session_store.hpp"
#include <QAction>
#include <QApplication>
#include <QCheckBox>
#include <QClipboard>
#include <QCloseEvent>
#include <QComboBox>
#include <QDialog>
#include <QDir>
#include <QFile>
#include <QFileDialog>
#include <QFormLayout>
#include <QInputDialog>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLabel>
#include <QLineEdit>
#include <QMenuBar>
#include <QMessageBox>
#include <QMimeData>
#include <QScopeGuard>
#include <QShortcut>
#include <QSignalBlocker>
#include <QSpinBox>
#include <QSplitter>
#include <QStatusBar>
#include <QTabBar>
#include <QTabWidget>
#include <QTableView>
#include <QTimer>
#include <QToolButton>
#include <QTreeWidget>
#include <QUuid>
#include <QVBoxLayout>
#include <algorithm>
#include <functional>
#include <utility>

namespace {
QString sampleQuery() {
    return "-- Navidog 示例库 · 100,000 条订单\n"
           "-- 选中一条 SQL 运行，或运行当前编辑器全部内容\n\n"
           "SELECT\n    o.order_no,\n    c.name AS customer,\n    c.city,\n    "
           "o.status,\n"
           "    ROUND(o.total_cents / 100.0, 2) AS amount,\n    o.created_at\n"
           "FROM orders AS o\nJOIN customers AS c ON c.id = o.customer_id\n"
           "WHERE o.status = 'paid'\nORDER BY o.id DESC\nLIMIT 100;";
}
QToolButton *button(const QString &text, const QString &objectName = {}) {
    return new navidog::ui::Button(text, objectName);
}
} // namespace

MainWindow::MainWindow(const QString &dataDirectory, QWidget *parent,
                       StartupTiming *startup)
    : QMainWindow(parent), store_(navidog::localStorePath(dataDirectory)),
      service_(this), dataDirectory_(dataDirectory), startupTiming_(startup) {

    // 记录启动的时候时间消耗
    const auto mark = [startup](const char *stage) {
        if (startup)
            startup->mark(stage);
    };

    mark("window_and_workers");

    // 获取本地的首选项
    QString preferencesWarning;
    try {
        preferences_ =
            loadApplicationSettings(navidog::localStorePath(dataDirectory_));
    } catch (const std::exception &e) {
        preferencesWarning = QString::fromUtf8(e.what());
    }

    navidog::ui::setExportDirectory(*this, preferences_.exportDirectory);
    setSqlPreviewFormattingPath(*this, navidog::localStorePath(dataDirectory_));
    mark("application_settings");

    // 设置界面的宽高
    setWindowTitle("Navidog — 数据库工作台");
    setMinimumSize(980, 680);
    resize(1380, 900);

    // 设置主题
    navidog::ui::applyTheme(this);
    auto *root = new QWidget;
    auto *rootLayout = new QVBoxLayout(root);
    rootLayout->setContentsMargins(0, 0, 0, 0);
    rootLayout->setSpacing(0);
    quickEntries_ = new navidog::ui::QuickEntryBar(root);
    rootLayout->addWidget(quickEntries_);

    auto *split = new QSplitter(Qt::Horizontal);
    split->setObjectName("workspaceSplitter");
    split->setChildrenCollapsible(false);
    navigation_ = new NavigationPane(
        service_, profiles_, store_, navidog::localStorePath(dataDirectory_),
        tableSettings_,
        [this](const QJsonObject &settings) {
            return saveTableSettings(settings);
        },
        [this](navidog::ConnectionProfile &profile) {
            return promptPassword(profile);
        },
        split);
    split->addWidget(navigation_);
    removeConnectionButton_ = button("删除", "removeConnection");
    removeConnectionButton_->setParent(navigation_);
    removeConnectionButton_->setEnabled(false);
    removeConnectionButton_->hide();

    // 工作区域
    auto *workspace = new QWidget;
    auto *work = new QVBoxLayout(workspace);
    work->setContentsMargins(0, 0, 0, 0);
    work->setSpacing(0);
    // Profile registry is not a shared toolbar: each query tab owns its context
    // controls.
    connection_ = new navidog::ui::Select(this);
    connection_->setObjectName("profileRegistry");
    connection_->hide();
    tabs_ = new navidog::ui::WorkspaceTabs;
    tabs_->setObjectName("queryTabs");
    metadata_ = std::make_unique<MetadataController>(
        service_, *tabs_, profiles_, navidog::localStorePath(dataDirectory_),
        preferences_.autoMetadata);
    work->addWidget(tabs_);

    // 添加到工作区域
    split->addWidget(workspace);
    split->setSizes({260, 1120});
    rootLayout->addWidget(split);
    setCentralWidget(root);
    status_ = new QLabel("就绪");
    statusBar()->addWidget(status_, 1);
    connectionCount_ = new QLabel;
    statusBar()->addPermanentWidget(connectionCount_);
    statusBar()->setSizeGripEnabled(false);
    connectionController_ = new ConnectionController(
        service_, store_, profiles_, navidog::localStorePath(dataDirectory_),
        {[this] { return navigation_->connectionGroups(); },
         [this] {
             return workspaceCloseFlow_ || !pendingCloseTabs_.isEmpty() ||
                    !pendingEditAfterClose_.isEmpty();
         },
         [this](const QString &id) { return confirmConnectionTabClosure(id); }},
        this);
    connect(connectionController_, &ConnectionController::connectionSaved, this,
            &MainWindow::handleConnectionSaved);
    connect(connectionController_, &ConnectionController::statusChanged, status_,
            &QLabel::setText);
    editorSqlTools_ = std::make_unique<EditorSqlTools>(
        profiles_, *metadata_, preferences_, navidog::localStorePath(dataDirectory_));
    querySessions_ = new QuerySessionController(
        service_, *connectionController_, profiles_, *tabs_,
        {[this] { return workspaceCloseFlow_; },
         [this](QueryPane &pane) {
             const QPointer<QueryPane> guarded(&pane);
             if (pane.databaseContext->property("databaseSelectable").toBool() &&
                 !changeQueryContext(&pane, QueryContextPart::Database))
                 return false;
             return guarded &&
                    (!pane.schemaContext->property("schemaSelectable").toBool() ||
                     changeQueryContext(&pane, QueryContextPart::Schema));
         }},
        this);
    connect(querySessions_, &QuerySessionController::activityChanged,
            this, &MainWindow::refreshWorkspaceActivity);
    connect(querySessions_, &QuerySessionController::operationFinished,
            this, &MainWindow::queryFinished);
    connect(querySessions_, &QuerySessionController::statusChanged, status_, &QLabel::setText);
    connect(querySessions_, &QuerySessionController::errorOccurred, this, &MainWindow::showError);
    connect(querySessions_, &QuerySessionController::serverInfoReceived, this, &MainWindow::updateServerInfo);
    connect(querySessions_, &QuerySessionController::catalogChanged, navigation_,
            &NavigationPane::queueRefresh);
    mark("window_widgets_and_theme");

    // 加载之前的工作空间
    bool profilesLoaded = false;
    try {
        auto workspace = store_.loadWorkspace();
        profiles_ = workspace.connections;
        navigation_->setConnectionGroups(std::move(workspace.groups));
        bool migrated = false;
        for (auto &p : profiles_)
            if (p.credentialSource == navidog::CredentialSource::LocalDocker) {
                p.credentialSource = navidog::CredentialSource::Prompt;
                p.credentialContainer.clear();
                p.credentialVariable.clear();
                migrated = true;
            }
        for (auto &p : profiles_)
            migrated = navidog::useSessionCredentialsOnly(p) || migrated;
        if (migrated)
            store_.saveWorkspace({profiles_, navigation_->connectionGroups()});
        profilesLoaded = true;
    } catch (const std::exception &e) {
        status_->setText(QString::fromUtf8(e.what()));
    }

    mark("profiles_and_groups");
    if (profilesLoaded) {
        try {
            const auto path = navidog::ensureDemoDatabase(dataDirectory);
            bool found = false;
            for (auto &p : profiles_)
                if (p.id == "harbor-demo") {
                    p.database = path;
                    if (p.name == "Harbor 示例库")
                        p.name = "Navidog 示例库";
                    found = true;
                }
            if (!found) {
                navidog::ConnectionProfile p;
                p.id = "harbor-demo";
                p.name = "Navidog 示例库";
                p.database = path;
                profiles_.prepend(p);
                store_.save(profiles_);
            }
        } catch (const std::exception &e) {
            status_->setText(QString::fromUtf8(e.what()));
        }
    }

    mark(profilesLoaded ? "demo_database" : "demo_database_skipped");
    try {
        auto bytes = navidog::localDocument(
            navidog::localStorePath(dataDirectory_), "table-views",
            QDir(dataDirectory_).filePath("table-settings.json"));
        if (bytes) {
            auto doc = QJsonDocument::fromJson(*bytes);
            if (!doc.isObject())
                throw std::runtime_error("表视图设置格式无效");
            tableSettings_ = doc.object();
        }
    } catch (const std::exception &e) {
        tableSettingsWritable_ = false;
        status_->setText(QString::fromUtf8(e.what()));
    }
    mark("table_view_settings");
    for (const auto &p : profiles_)
        connection_->addItem(p.name, p.id);
    connection_->setCurrentIndex(connection_->findData("harbor-demo"));
    connect(navigation_, &NavigationPane::selectionChanged, this,
            &MainWindow::updateNodeActions);
    connect(navigation_, &NavigationPane::treeRebuilt, this,
            &MainWindow::refreshConnectionStates);
    connect(navigation_, &NavigationPane::connectionRenamed, this,
            [this](const QString &id, const QString &name) {
                const auto index = connection_->findData(id);
                if (index >= 0)
                    connection_->setItemText(index, name);
                syncConnections();
            });
    connect(navigation_, &NavigationPane::objectsRequested, this,
            [this](QTreeWidgetItem *item) { openObjects(item); });
    connect(navigation_, &NavigationPane::previewRequested, this,
            [this](QTreeWidgetItem *item) { openObject(item, false); });
    connect(navigation_, &NavigationPane::tableChildRequested, this,
            [this](QTreeWidgetItem *item) { editTableChild(item); });
    connect(navigation_, &NavigationPane::sourceRequested, this,
            [this](QTreeWidgetItem *item) { openObject(item, true); });
    connect(navigation_, &NavigationPane::populateContextMenu, this,
            &MainWindow::populateNavigationMenu);
    connect(navigation_, &NavigationPane::catalogActivityChanged, this,
            &MainWindow::refreshWorkspaceActivity);
    connect(navigation_, &NavigationPane::catalogLoaded, this,
            &MainWindow::handleNavigationCatalog);
    connect(navigation_, &NavigationPane::serverInfoReceived, this,
            &MainWindow::updateServerInfo);
    connect(navigation_, &NavigationPane::statusChanged, status_,
            &QLabel::setText);
    connect(navigation_, &NavigationPane::errorOccurred, this,
            &MainWindow::showError);
    connect(
        &service_, &navidog::QueryService::catalogCompleted, this,
        [this](const QString &request, const navidog::CatalogResult &result) {
            if (queryCatalogRequests_.contains(request)) {
                refreshWorkspaceActivity();
                handleQueryCatalog(request, result);
            }
        });
    navigation_->rebuild();
    mark("connection_tree");
    using QuickEntryBar = navidog::ui::QuickEntryBar;
    connect(quickEntries_, &QuickEntryBar::newConnectionRequested, this,
            &MainWindow::addConnection);
    connect(quickEntries_, &QuickEntryBar::newQueryRequested, this,
            [this] { newQuery(); });
    connect(quickEntries_, &QuickEntryBar::browseFolderRequested, navigation_,
            &NavigationPane::navigateFolder);
    connect(quickEntries_, &QuickEntryBar::serverObjectsRequested, this,
            &MainWindow::browseServerObjects);
    connect(quickEntries_, &QuickEntryBar::queryLibraryRequested, this,
            [this] { openQueryLibrary(); });
    connect(quickEntries_, &QuickEntryBar::backupRequested, this,
            &MainWindow::showQuickBackup);
    connect(quickEntries_, &QuickEntryBar::automationRequested, this,
            &MainWindow::showQuickAutomation);
    connect(quickEntries_, &QuickEntryBar::biRequested, this,
            &MainWindow::showQuickBi);
    connect(quickEntries_, &QuickEntryBar::stopCatalogRequested, this, [this] {
        service_.cancelCatalog();
        status_->setText("正在停止对象加载…");
    });
    connect(removeConnectionButton_, &QToolButton::clicked, this,
            &MainWindow::removeConnection);
    connect(tabs_, &QTabWidget::currentChanged, this, [this] {
        navigation_->cancelFolderNavigation();
        if (auto *pane =
                dynamic_cast<WorkspacePane *>(tabs_->currentWidget())) {
            const QSignalBlocker blocker(connection_);
            connection_->setCurrentIndex(
                connection_->findData(pane->profileId));
        }
        updateWorkspaceActions();
    });
    connect(tabs_, &QTabWidget::tabCloseRequested, this, [this](int index) {
        QPointer<WorkspacePane> pane =
            dynamic_cast<WorkspacePane *>(tabs_->widget(index));
        QTimer::singleShot(0, this, [this, pane] {
            if (pane)
                requestCloseWorkspaceTab(pane);
        });
    });
    connect(this, &MainWindow::queryFinished, this, [this] {
        if (!pendingCloseTabs_.isEmpty())
            QTimer::singleShot(0, this,
                               &MainWindow::finishPendingWorkspaceClose);
    });
    connect(&service_, &navidog::QueryService::sessionCountChanged, this,
            [this](int count) {
                connectionCount_->setText(QString("%1 个连接 · %2 个会话")
                                              .arg(profiles_.size())
                                              .arg(count));
            });
    connect(&service_, &navidog::QueryService::connectionStatesChanged, this,
            [this] {
                refreshConnectionStates();
                refreshWorkspaceActivity();
                startPendingNamespaceDrop();
                if (!closingConnectionStatusId_.isEmpty() &&
                    !service_.isConnected(closingConnectionStatusId_)) {
                    closingConnectionStatusId_.clear();
                    if (status_->text() == "正在关闭连接…")
                        status_->setText("连接已关闭");
                }
            });
    connect(&service_, &navidog::QueryService::namespaceOptionsCompleted, this,
            &MainWindow::refreshWorkspaceActivity);
    connect(&service_, &navidog::QueryService::namespaceCompleted, this,
            [this](const QString &id, const navidog::NamespaceResult &result) {
                refreshWorkspaceActivity();
                if (!namespaceDeletion_ || namespaceDeletion_->request != id)
                    return;
                const auto operation = std::move(*namespaceDeletion_);
                namespaceDeletion_.reset();
                if (result.applied) {
                    navigation_->queueRefresh(operation.profile.id);
                    status_->setText(QString("已删除 %1").arg(operation.name));
                } else if (result.cancelled)
                    status_->setText("删除已停止，请刷新目录核对结果");
                else {
                    status_->setText("删除失败");
                    showError("删除数据库对象失败", result.error);
                }
            });
    connect(&service_, &navidog::QueryService::tableTaskCompleted, this,
            &MainWindow::refreshWorkspaceActivity);
    auto *startupConnections = new QTimer(this);
    startupConnections->setInterval(350);
    QStringList startupIds;
    for (const auto &p : profiles_)
        if (p.advanced.autoConnect)
            startupIds << p.id;
    connect(startupConnections, &QTimer::timeout, this,
            [this, startupConnections, ids = startupIds]() mutable {
                if (service_.busy())
                    return;
                if (ids.isEmpty()) {
                    startupConnections->stop();
                    startupConnections->deleteLater();
                    return;
                }
                auto id = ids.takeFirst();
                auto profile =
                    std::find_if(profiles_.begin(), profiles_.end(),
                                 [&](const auto &p) { return p.id == id; });
                if (profile == profiles_.end())
                    return;
                bool credentials =
                    profile->driver == navidog::DatabaseKind::SQLite ||
                    profile->savePassword ||
                    profile->credentialSource ==
                        navidog::CredentialSource::Passwordless ||
                    (profile->driver == navidog::DatabaseKind::Oracle &&
                     profile->oracle.externalAuthentication);
                if (!credentials ||
                    (profile->ssh.enabled && !profile->ssh.savePassword &&
                     (profile->ssh.authentication ==
                          navidog::SshAuthentication::Password ||
                      profile->ssh.authentication ==
                          navidog::SshAuthentication::PasswordAndKey))) {
                    status_->setText(profile->name +
                                     " 需要手动输入凭据，已跳过自动连接");
                    return;
                }
                navigation_->openConnection(navigation_->connectionItem(id));
            });
    if (!startupIds.isEmpty())
        startupConnections->start();
    else
        startupConnections->deleteLater();
    setupMenus();
    updateWorkspaceActions();
    using QuickIcon = navidog::ui::QuickActionIcon;
    newTableAction_->setIcon(navidog::ui::quickActionIcon(QuickIcon::NewTable));
    alterTableAction_->setIcon(
        navidog::ui::quickActionIcon(QuickIcon::EditTable));
    connect(
        &service_, &navidog::QueryService::sourceDesignCompleted, this,
        [this](const QString &id, const navidog::SourceDesignResult &result) {
            auto *pane = sourcePane_.data();
            if (!pane || pane->requestId != id) {
                refreshWorkspaceActivity();
                return;
            }
            const bool saved = pane->saving;
            pane->loading = pane->saving = false;
            sourcePane_.clear();
            refreshWorkspaceActivity();
            if (!result.error.isEmpty() || result.cancelled) {
                pane->needsReload |= result.partial;
                auto message =
                    result.cancelled ? QString("定义操作已停止") : result.error;
                if (result.rolledBack)
                    message += "\n本次修改已回滚。";
                if (result.restored)
                    message += "\n原例程及授权已恢复。";
                if (result.partial)
                    message += "\n对象可能已改变，请重新读取核对后再操作。";
                if (!result.warning.isEmpty())
                    message += "\n" + result.warning;
                pane->message->setText(message);
                pane->setBusy(false);
                if (saved && !result.cancelled)
                    showError("对象操作失败", message);
                else
                    status_->setText(result.cancelled
                                         ? "定义操作已停止"
                                         : "读取定义失败，可在标签内重试");
            } else if (result.deleted) {
                auto index = tabs_->indexOf(pane);
                if (index >= 0)
                    tabs_->removeTab(index);
                pane->deleteLater();
                status_->setText("对象已删除");
            } else {
                pane->acceptDefinition(result.definition);
                pane->needsReload = !result.refreshError.isEmpty();
                pane->message->setText(
                    !result.refreshError.isEmpty()
                        ? "定义已提交，但刷新失败：" + result.refreshError
                    : !result.warning.isEmpty()
                        ? "定义已保存，服务器编译信息：\n" + result.warning
                    : saved ? "对象定义已保存"
                            : "完整定义已读取");
                pane->setBusy(false);
                status_->setText(saved ? "对象已保存" : "定义已读取");
            }
            if (saved && result.applied) {
                auto profileId = pane->profileId;
                auto node = result.definition.node;
                QTimer::singleShot(0, this, [this, profileId, node] {
                    if (!service_.busy())
                        navigation_->refreshFolder(profileId, node,
                                                   node.kind + "s");
                });
            }
            if (tabs_->count() == 0)
                newQuery();
            emit queryFinished();
        });
    // A cancelled read can finish after its data tab has closed.
    connect(&service_, &navidog::QueryService::dataPageCompleted, this,
            &MainWindow::refreshWorkspaceActivity, Qt::QueuedConnection);
    auto *runShortcut = new QShortcut(QKeySequence("Ctrl+Return"), this);
    connect(runShortcut, &QShortcut::activated, this, &MainWindow::runCurrent);
    auto *runF5 = new QShortcut(QKeySequence(Qt::Key_F5), this);
    connect(runF5, &QShortcut::activated, this, &MainWindow::runCurrent);
    mark("menus_and_actions");
    if (preferences_.restoreWorkspace)
        restoreSession();
    mark("restore_workspace");
    if (!restoredSession_) {
        newQuery(sampleQuery());
        mark("initial_query_tab");
    }
    if (!preferencesWarning.isEmpty())
        status_->setText(preferencesWarning);
    loadQuickAutomation();
    auto *newShortcut = new QShortcut(QKeySequence::AddTab, this);
    connect(newShortcut, &QShortcut::activated, this, [this] { newQuery(); });
}
MainWindow::~MainWindow() = default;

void MainWindow::paintEvent(QPaintEvent *event) {
    QMainWindow::paintEvent(event);
    if (auto *startup = std::exchange(startupTiming_, nullptr)) {
        // The queued callback follows the paint pass for the window and its
        // children.
        QTimer::singleShot(0, this, [this, startup] {
            startup->mark("first_paint_processed");
            startup->write(dataDirectory_);
        });
    }
}
QueryPane *MainWindow::currentPane() const {
    return dynamic_cast<QueryPane *>(tabs_->currentWidget());
}
int MainWindow::addWorkspaceTab(WorkspacePane *pane, const QString &title) {
    for (auto *editor : pane->findChildren<SqlEditor *>())
        editor->applyPreferences(
            preferences_.editorFontSize, preferences_.editorIndentWidth,
            preferences_.editorWordWrap, preferences_.editorCompletion,
            preferences_.completionMinPrefix);
    const auto index = tabs_->addTab(pane, title);
    if (auto *close = tabs_->tabBar()->tabButton(index, QTabBar::RightSide)) {
        close->setObjectName("closeWorkspaceTab");
        close->setAccessibleName("关闭标签页");
        close->setToolTip("关闭此标签页");
    }
    return index;
}
void MainWindow::newQuery(const QString &sql, const QString &explicitProfile,
                          const QString &explicitDatabase,
                          navidog::WorkspaceTabKind kind,
                          const QString &explicitSchema) {
    auto id = explicitProfile.isEmpty() ? connection_->currentData().toString()
                                        : explicitProfile;
    QString database = explicitDatabase, schema = explicitSchema;
    if (explicitProfile.isEmpty() && sql.isEmpty()) {
        auto *item = navigation_->currentItem();
        if (item && !item->data(0, Qt::UserRole).toString().isEmpty()) {
            id = item->data(0, Qt::UserRole).toString();
            for (auto *ancestor = item; ancestor;
                 ancestor = ancestor->parent()) {
                auto n = ancestor->data(0, Qt::UserRole + 2)
                             .value<navidog::CatalogNode>();
                if (database.isEmpty())
                    database = n.database;
                if (schema.isEmpty())
                    schema = n.schema;
            }
        } else if (auto *pane = currentPane()) {
            id = pane->profileId;
            database = pane->database;
            schema = pane->schema;
        }
    }
    auto *pane = new QueryPane(profiles_, kind);
    pane->setSqlTools(*editorSqlTools_);
    pane->limit->setCurrentIndex(
        pane->limit->findData(preferences_.queryRowLimit));
    pane->setResultsLayout(preferences_.resultsBeside);
    pane->profileId = id;
    pane->database = database;
    pane->connection->setCurrentIndex(pane->connection->findData(id));
    pane->connection->setEnabled(kind == navidog::WorkspaceTabKind::Query);
    pane->schema = schema;
    configureQueryContext(pane);
    connect(pane, &QueryPane::namespaceListRequested, this,
            [this, pane](bool schemaList) { requestQueryCatalog(pane, schemaList); });
    connect(pane, &QueryPane::contextChangeRequested, this,
            [this, pane](QueryContextPart part) { changeQueryContext(pane, part); });

    querySessions_->bind(*pane);
    connect(pane, &QueryPane::formatRequested, this, &MainWindow::formatCurrent);
    connect(pane, &QueryPane::exportRequested, this, &MainWindow::exportCurrent);
    auto profile = std::find_if(profiles_.begin(), profiles_.end(),
                                [&](const auto &p) { return p.id == id; });
    const auto defaultSql =
        profile != profiles_.end() && profile->driver==navidog::DatabaseKind::Redis ? "PING" :
        profile != profiles_.end() && (navidog::usesOracleSql(profile->driver) || profile->driver==navidog::DatabaseKind::Dameng)
            ? "SELECT 1 FROM dual" : "SELECT 1;";
    pane->editor->setPlainText(sql.isEmpty() ? defaultSql : sql);
    auto label = database.isEmpty() ? QString("查询 %1").arg(tabs_->count() + 1)
                                    : database + " · 查询";
    auto index = addWorkspaceTab(pane, label);
    pane->setProperty("queryBaseTitle", label);
    connect(pane, &QueryPane::openSqlRequested, this, &MainWindow::openSqlFile);
    connect(pane, &QueryPane::statusRequested, status_, &QLabel::setText);
    connect(pane, &QueryPane::errorRequested, this, &MainWindow::showError);
    connect(pane, &QueryPane::documentChanged, this, [this, pane] {
        auto index = tabs_->indexOf(pane);
        if (index < 0)
            return;
        auto title = pane->script.path.isEmpty()
                         ? pane->property("queryBaseTitle").toString()
                         : QFileInfo(pane->script.path).fileName();
        tabs_->setTabText(index, title + (pane->dirty() ? " *" : ""));
        if (!pane->script.path.isEmpty())
            tabs_->setTabToolTip(index, pane->script.path);
    });
    tabs_->setCurrentIndex(index);
    pane->editor->setFocus();
    refreshWorkspaceActivity();
    connect(pane, &QueryPane::saveInLibraryRequested, this,
            [this, pane] { saveQueryInLibrary(pane); });
}
void MainWindow::refreshWorkspaceActivity() {
    navigation_->scheduleRefresh();
    quickEntries_->setCatalogBusy(service_.catalogBusy());
    const WorkspacePane::Activity activity{service_.busy(), service_.operationBusy()};
    for (int i = 0; i < tabs_->count(); ++i)
        if (auto *pane = dynamic_cast<WorkspacePane *>(tabs_->widget(i)))
            pane->updateWorkspaceActivity(activity);
    updateNodeActions();
    updateWorkspaceActions();
}
void MainWindow::analyzeCurrent() {
    if (auto *pane = currentPane())
        querySessions_->analyzeEditor(*pane);
}

void MainWindow::runCurrent() {
    if (auto *pane = currentPane())
        querySessions_->runEditor(*pane);
}

void MainWindow::refreshConnectionStates() {
    const auto opened = navigation_->refreshConnectionStates(
        namespaceDeletion_ ? namespaceDeletion_->profile.id : QString{});
    connectionCount_->setText(QString("%1 个连接 · %2 已打开 · %3 个会话")
                                  .arg(profiles_.size())
                                  .arg(opened)
                                  .arg(service_.sessionCount()));
    updateNodeActions();
}

void MainWindow::closeConnection(QTreeWidgetItem *item) {
    if (!item || service_.busy() || navigation_->dialogOpen() ||
        item->data(0, Qt::UserRole + 2).value<navidog::CatalogNode>().kind !=
            "connection")
        return;
    const auto id = item->data(0, Qt::UserRole).toString();
    const QPersistentModelIndex index(navigation_->indexFor(item));
    navigation_->setDialogOpen(true);
    const auto dialogClosed =
        qScopeGuard([this] { navigation_->setDialogOpen(false); });
    if (service_.isConnected(id) &&
        QMessageBox::question(this, "关闭连接",
                              "关闭此连接的所有会话？未提交的 SQL "
                              "事务会回滚，查询文本和本地编辑草稿会保留。",
                              QMessageBox::Yes | QMessageBox::Cancel,
                              QMessageBox::Cancel) != QMessageBox::Yes)
        return;
    if (!index.isValid())
        return;
    if (service_.busy()) {
        status_->setText("当前有数据库操作正在执行，请稍后关闭连接");
        return;
    }
    querySessions_->disconnectProfile(id);
    navigation_->clearConnectionCatalog(navigation_->itemFor(index));
    refreshObjectsPanes();
    navigation_->focusTree();
    closingConnectionStatusId_ = id;
    status_->setText("正在关闭连接…");
    refreshWorkspaceActivity();
}
void MainWindow::openObject(QTreeWidgetItem *item, bool definition) {
    if (service_.busy()) {
        status_->setText("请等待当前操作完成");
        return;
    }
    auto node = item->data(0, Qt::UserRole + 2).value<navidog::CatalogNode>();
    auto id = item->data(0, Qt::UserRole).toString();
    for (const auto &p : profiles_)
        if (p.id == id) {
            if (definition && navidog::supportsSourceDesign(p.driver, node.kind)) {
                openSource(item, true);
                return;
            }
            if (!definition && navidog::supportsDataEditing(p.driver)) {
                createDataPane(p, node)->requestRefresh();
                return;
            }
            newQuery(definition ? navidog::definitionSql(p, node)
                                : navidog::previewSql(p, node),
                     id, node.database,
                     definition ? navidog::WorkspaceTabKind::Definition
                                : navidog::WorkspaceTabKind::Preview);
            if (p.driver == navidog::DatabaseKind::Redis)
                currentPane()->setProperty("redisPreviewKey", node.key);
            tabs_->setTabText(tabs_->currentIndex(),
                              node.name + (definition ? " · 定义" : " · 数据"));
            tabs_->setTabToolTip(tabs_->currentIndex(),
                                 p.name + " · " + node.database + " · " +
                                     node.schema);
            runCurrent();
            break;
        }
}
DataPane *MainWindow::createDataPane(const navidog::ConnectionProfile &p,
                                     const navidog::CatalogNode &node) {
    auto *pane = new DataPane(p, node, service_, [this, id = p.id] {
        return connectionController_->authenticatedProfile(id);
    });
    const auto index = addWorkspaceTab(pane, pane->tabTitle());
    tabs_->setTabToolTip(index,
                         p.name + " / " + node.database + " / " + node.schema);
    tabs_->setCurrentIndex(index);
    connect(pane, &DataPane::activityChanged, this, &MainWindow::refreshWorkspaceActivity);
    connect(pane, &DataPane::operationFinished, this, &MainWindow::queryFinished);
    connect(pane, &DataPane::statusChanged, status_, &QLabel::setText);
    connect(pane, &DataPane::errorOccurred, this, &MainWindow::showError);
    connect(pane, &DataPane::tabTitleChanged, this, [this, pane](const QString &title) {
        if (const auto index = tabs_->indexOf(pane); index >= 0)
            tabs_->setTabText(index, title);
    });
    return pane;
}
bool MainWindow::promptPassword(navidog::ConnectionProfile &profile) {
    return connectionController_->promptPassword(profile);
}
void MainWindow::showError(const QString &title, const QString &detail) {
    auto *box = dynamic_cast<navidog::ui::DatabaseErrorDialog *>(
        findChild<QDialog *>("databaseErrorDialog"));
    if (!box)
        box = new navidog::ui::DatabaseErrorDialog(this);
    box->setError(title, detail);
    box->show();
    box->raise();
    box->activateWindow();
}

void MainWindow::syncConnections() {
    queryDatabases_.clear();
    querySchemas_.clear();
    for (int i = 0; i < tabs_->count(); ++i)
        if (auto *pane = dynamic_cast<QueryPane *>(tabs_->widget(i))) {
            QSignalBlocker blocker(pane->connection);
            pane->connection->clear();
            for (auto p : profiles_)
                pane->connection->addItem(p.name, p.id);
            pane->connection->setCurrentIndex(
                pane->connection->findData(pane->profileId));
        }
}
TablePane *MainWindow::createTablePane(const navidog::ConnectionProfile &profile,
                                       const navidog::CatalogNode &node,
                                       bool existing) {
    auto *pane = new TablePane(profile, node, existing);
    auto *controller = new TableDesignController(
        *pane, service_,
        [this, pane] { return databaseToolProfile(pane->profileId, pane->tableNode()); },
        [this](const QString &id, const navidog::CatalogNode &table) {
            return hasTableDataDraft(id, table);
        });
    connect(controller, &TableDesignController::activityChanged, this,
            &MainWindow::refreshWorkspaceActivity);
    connect(controller, &TableDesignController::statusChanged, status_, &QLabel::setText);
    connect(controller, &TableDesignController::errorOccurred, this, &MainWindow::showError);
    connect(controller, &TableDesignController::operationFinished, this, &MainWindow::queryFinished);
    connect(controller, &TableDesignController::structureSaved, this,
            &MainWindow::handleTableStructureSaved);
    connect(pane, &TablePane::draftChanged, this, [this, pane] {
        if (const auto index = tabs_->indexOf(pane); index >= 0)
            tabs_->setTabText(index, pane->tabTitle());
    });

    const auto index = addWorkspaceTab(pane, existing ? pane->tabTitle() : "新建表");
    tabs_->setCurrentIndex(index);
    pane->setBusy(service_.busy());
    return pane;
}

bool MainWindow::hasTableDataDraft(const QString &id, const navidog::CatalogNode &node) const {
    for (int i = 0; i < tabs_->count(); ++i)
        if (auto *pane = dynamic_cast<DataPane *>(tabs_->widget(i));
            pane && pane->matchesTable(id, node) && pane->dirty())
            return true;
    return false;
}

void MainWindow::handleTableStructureSaved(const QString &id, const navidog::CatalogNode &node) {
    for (int i = 0; i < tabs_->count(); ++i)
        if (auto *pane = dynamic_cast<DataPane *>(tabs_->widget(i));
            pane && pane->matchesTable(id, node))
            pane->preventEditingAfterSchemaChange();
    navigation_->invalidateTable(id, node);
    QTimer::singleShot(0, this, [this, id, node] {
        if (!service_.busy())
            navigation_->refreshFolder(id, node, "tables", true);
    });
}

void MainWindow::openTableDesigner(QTreeWidgetItem *item, bool existing) {
    if (!item)
        return;
    auto n = item->data(0, Qt::UserRole + 2).value<navidog::CatalogNode>();
    auto id = item->data(0, Qt::UserRole).toString();
    if (existing ? n.kind != "table"
                 : (n.kind != "schema" && n.kind != "database" &&
                    n.kind != "tables" && n.kind != "tableGroup" &&
                    n.kind != "table"))
        return;
    auto it = std::find_if(profiles_.begin(), profiles_.end(),
                           [&](auto p) { return p.id == id; });
    if (it == profiles_.end() || it->driver == navidog::DatabaseKind::Dameng)
        return;
    if (existing)
        for (int i = 0; i < tabs_->count(); ++i)
            if (auto *p = dynamic_cast<TablePane *>(tabs_->widget(i));
                p && p->originalDefinition().exists && p->matchesTable(id, n)) {
                tabs_->setCurrentIndex(i);
                return;
            }
    if (!existing) {
        n.kind = "table";
        n.object.clear();
        n.name.clear();
        if (it->driver == navidog::DatabaseKind::PostgreSQL &&
            n.schema.isEmpty())
            n.schema = "public";
        if (it->driver == navidog::DatabaseKind::SQLServer &&
            n.schema.isEmpty())
            n.schema = "dbo";
    }
    auto *pane = createTablePane(*it, n, existing);
    if (existing)
        pane->requestReload();
    else
        pane->focusName();
}
void MainWindow::editTableChild(QTreeWidgetItem *item, int operation) {
    if (!item)
        return;
    auto child = item->data(0, Qt::UserRole + 2).value<navidog::CatalogNode>();
    auto *table = item;
    while (
        table &&
        table->data(0, Qt::UserRole + 2).value<navidog::CatalogNode>().kind !=
            "table")
        table = table->parent();
    if (!table)
        return;
    openTableDesigner(table, true);
    auto *pane = dynamic_cast<TablePane *>(tabs_->currentWidget());
    if (pane && pane->tableNode().object == child.object &&
        pane->profileId == item->data(0, Qt::UserRole).toString())
        pane->focusObject(child.kind, child.name, operation);
}
void MainWindow::addConnection() {
    connectionController_->create(navigation_->selectedConnectionGroup());
}
void MainWindow::editConnection() {
    auto *item = navigation_->currentItem();
    if (!item ||
        item->data(0, Qt::UserRole + 2).value<navidog::CatalogNode>().kind !=
            "connection")
        return;
    connectionController_->edit(item->data(0, Qt::UserRole).toString());
}
bool MainWindow::hasConnectionDrafts(const QString &id) const {
    for (int i = 0; i < tabs_->count(); ++i) {
        auto *pane = dynamic_cast<WorkspacePane *>(tabs_->widget(i));
        if (!pane || pane->profileId != id)
            continue;
        if (auto *query = dynamic_cast<QueryPane *>(pane);
            query &&
            query->hasActiveTransaction())
            return true;
        if (auto *source = dynamic_cast<SourcePane *>(pane);
            source && source->dirty())
            return true;
        if (auto *data = dynamic_cast<DataPane *>(pane); data && data->dirty())
            return true;
        if (auto *table = dynamic_cast<TablePane *>(pane);
            table && table->dirty())
            return true;
    }
    return false;
}
void MainWindow::handleConnectionSaved(const QString &id, bool edited, bool endpointChanged) {
    const auto profile = std::find_if(profiles_.cbegin(), profiles_.cend(),
                                      [&](const auto &entry) { return entry.id == id; });
    if (profile == profiles_.cend())
        return;
    const auto name = profile->name;
    if (endpointChanged) {
        metadata_->removeProfile(id);
        querySessions_->disconnectProfile(id);
        if (auto *item = navigation_->connectionItem(id))
            navigation_->clearConnectionCatalog(item);
        for (int i = 0; i < tabs_->count(); ++i) {
            if (auto *query = dynamic_cast<QueryPane *>(tabs_->widget(i));
                query && query->profileId == id)
                query->database.clear();
            if (auto *data = dynamic_cast<DataPane *>(tabs_->widget(i));
                data && data->profileId == id)
                data->preventEditingAfterConnectionChange();
            if (auto *table = dynamic_cast<TablePane *>(tabs_->widget(i));
                table && table->profileId == id)
                table->invalidateConnection();
            if (auto *source = dynamic_cast<SourcePane *>(tabs_->widget(i));
                source && source->profileId == id) {
                source->needsReload = true;
                source->loaded = false;
                source->setBusy(false);
                source->message->setText(
                    "连接配置已修改，请重新读取对象定义。");
            }
        }
    }
    if (edited)
        connection_->setItemText(connection_->findData(id), name);
    else {
        connection_->addItem(name, id);
        connection_->setCurrentIndex(connection_->count() - 1);
    }
    syncConnections();
    navigation_->rebuild();
    navigation_->selectItem(navigation_->connectionItem(id), true);
}
void MainWindow::namespaceDialog(QTreeWidgetItem *item,
                                 navidog::NamespaceKind kind) {
    if (!item || service_.busy())
        return;
    const auto node =
        item->data(0, Qt::UserRole + 2).value<navidog::CatalogNode>();
    const auto id = item->data(0, Qt::UserRole).toString();
    auto it = std::find_if(profiles_.begin(), profiles_.end(),
                           [&](const auto &p) { return p.id == id; });
    if (it == profiles_.end())
        return;
    if (!navidog::supportsNamespace(it->driver, kind) || it->readOnly)
        return;
    if (kind == navidog::NamespaceKind::Schema && node.kind != "database" &&
        node.kind != "schema" &&
        !(it->driver == navidog::DatabaseKind::Oracle &&
          node.kind == "connection"))
        return;
    if (!promptPassword(*it))
        return;
    auto target = *it;
    if (kind == navidog::NamespaceKind::Schema &&
        target.driver != navidog::DatabaseKind::Oracle)
        target.database = node.database;
    NamespaceDialog dialog(target, kind, service_, this);
    const bool accepted = dialog.exec() == QDialog::Accepted;
    refreshWorkspaceActivity();
    if (!accepted && !dialog.partialResult())
        return;
    const auto draft = dialog.draft();
    if (target.driver == navidog::DatabaseKind::SQLite) {
        navidog::ConnectionProfile created;
        created.id = QUuid::createUuid().toString(QUuid::Id128);
        created.name = draft.name.trimmed();
        created.database = draft.filePath;
        created.groupId = target.groupId;
        const auto base = created.name;
        int suffix = 2;
        while (
            std::any_of(profiles_.begin(), profiles_.end(), [&](const auto &p) {
                return p.name.compare(created.name, Qt::CaseInsensitive) == 0;
            }))
            created.name = base + " " + QString::number(suffix++);
        profiles_.push_back(created);
        if (!saveWorkspace()) {
            profiles_.pop_back();
            showError(
                "数据库已创建",
                QString("数据库文件已保存到 "
                        "%1，但连接配置保存失败。可通过新建连接打开该文件。")
                    .arg(draft.filePath));
            return;
        }
        connection_->addItem(created.name, created.id);
        syncConnections();
        navigation_->rebuild();
        navigation_->selectItem(navigation_->connectionItem(created.id), true);
    } else {
        auto *targetItem =
            kind == navidog::NamespaceKind::Database ||
                    target.driver == navidog::DatabaseKind::Oracle
                ? navigation_->connectionItem(id)
                : navigation_->findCatalogItem(id, [&](const auto &node) {
                      return node.kind == "database" &&
                             node.database == target.database;
                  });
        if (targetItem) {
            navigation_->expandCatalog(targetItem, true, true);
            targetItem->setExpanded(true);
        }
    }
    status_->setText(
        dialog.partialResult()
            ? QString("用户已创建，后续授权未完成：") + draft.name
            : (target.driver == navidog::DatabaseKind::Oracle ? "用户 / Schema"
               : kind == navidog::NamespaceKind::Schema       ? "Schema"
                                                              : "数据库") +
                  QString("已创建：") + draft.name);
}
void MainWindow::showNamespaceProperties(QTreeWidgetItem *item) {
    const auto node =
        item->data(0, Qt::UserRole + 2).value<navidog::CatalogNode>();
    const auto id = item->data(0, Qt::UserRole).toString();
    const auto profile =
        std::find_if(profiles_.cbegin(), profiles_.cend(),
                     [&](const auto &p) { return p.id == id; });
    if (profile == profiles_.cend())
        return;
    const auto sql = navidog::definitionSql(*profile, node);
    newQuery(sql, id, node.kind == "schema" ? node.database : QString{},
             navidog::WorkspaceTabKind::Definition);
    tabs_->setTabText(tabs_->currentIndex(), node.name + " · 属性");
    runCurrent();
}
void MainWindow::dropNamespaceDialog(QTreeWidgetItem *item) {
    if (!item || service_.busy() || namespaceDeletion_)
        return;
    const auto node =
        item->data(0, Qt::UserRole + 2).value<navidog::CatalogNode>();
    if (node.kind != "database" && node.kind != "schema")
        return;
    const auto id = item->data(0, Qt::UserRole).toString();
    auto saved = std::find_if(profiles_.begin(), profiles_.end(),
                              [&](const auto &p) { return p.id == id; });
    if (saved == profiles_.end() || saved->readOnly)
        return;
    const bool database = node.kind == "database";
    const auto kind = database ? navidog::NamespaceKind::Database
                               : navidog::NamespaceKind::Schema;
    if (!navidog::supportsNamespaceDrop(saved->driver, kind))
        return;
    const auto name = database ? node.database : node.schema;
    auto profile = *saved;
    if (!database && profile.driver != navidog::DatabaseKind::Oracle)
        profile.database = node.database;
    QString sql;
    try {
        sql = navidog::dropNamespaceSql(profile, kind, name);
    } catch (const std::exception &e) {
        showError("无法生成删除语句", QString::fromUtf8(e.what()));
        return;
    }
    NamespaceDropDialog dialog(profile.driver, kind, name, sql, this);
    if (dialog.exec() != QDialog::Accepted)
        return;
    saved = std::find_if(profiles_.begin(), profiles_.end(),
                         [&](const auto &p) { return p.id == id; });
    if (saved == profiles_.end() || saved->driver != profile.driver ||
        !promptPassword(*saved))
        return;
    profile = *saved;
    if (!database && profile.driver != navidog::DatabaseKind::Oracle)
        profile.database = node.database;
    namespaceDeletion_ =
        NamespaceDeletion{profile, kind, name, QUuid::createUuid().toString()};
    if (database)
        querySessions_->disconnectDatabase(id, name);
    startPendingNamespaceDrop();
}
void MainWindow::startPendingNamespaceDrop() {
    if (!namespaceDeletion_ || namespaceDeletion_->started || service_.busy())
        return;
    auto &deletion = *namespaceDeletion_;
    if (service_.dropNamespace(deletion.profile, deletion.kind, deletion.name,
                               deletion.request)) {
        deletion.started = true;
        refreshWorkspaceActivity();
        status_->setText(QString("正在删除 %1…").arg(deletion.name));
    }
}
bool MainWindow::saveTableSettings(const QJsonObject &settings) {
    if (!tableSettingsWritable_) {
        QMessageBox::warning(
            this, "设置未保存",
            "原表视图设置文件无法读取；请先修复该文件，程序未覆盖它。");
        return false;
    }
    auto bytes = QJsonDocument(settings).toJson();
    if (bytes.size() > 4 * 1024 * 1024) {
        QMessageBox::warning(this, "设置未保存", "表视图设置超过上限");
        return false;
    }
    try {
        navidog::LocalStore db(navidog::localStorePath(dataDirectory_));
        db.putDocument("table-views", bytes);
        return true;
    } catch (const std::exception &e) {
        QMessageBox::warning(this, "设置未保存", QString::fromUtf8(e.what()));
        return false;
    }
}
void MainWindow::saveTableView(QTreeWidgetItem *item) {
    if (!item)
        return;
    const auto id = item->data(0, Qt::UserRole).toString();
    const auto node =
        item->data(0, Qt::UserRole + 2).value<navidog::CatalogNode>();
    auto *pane = dynamic_cast<DataPane *>(tabs_->currentWidget());
    if (!pane || !pane->isLoaded() || !pane->matchesTable(id, node)) {
        QMessageBox::information(this, "保存配置",
                                 "请先打开该表的数据页并应用筛选 / 排序。");
        return;
    }
    bool ok = false;
    auto name = QInputDialog::getText(this, "保存表视图配置", "配置名称",
                                      QLineEdit::Normal, {}, &ok)
                    .trimmed();
    if (!ok || name.isEmpty())
        return;
    auto key = NavigationPane::tableSettingsKey(id, node);
    auto before = tableSettings_;
    auto settings = tableSettings_[key].toObject();
    auto all = settings["profiles"].toObject();
    auto views = all[node.object].toArray();
    for (auto view : views)
        if (view.toObject()["name"].toString() == name) {
            QMessageBox::information(this, "名称重复", "请使用新的配置名称。");
            return;
        }
    auto view = pane->viewSettings();
    view["name"] = name;
    views << view;
    all[node.object] = views;
    settings["profiles"] = all;
    tableSettings_[key] = settings;
    if (!saveTableSettings(tableSettings_))
        tableSettings_ = before;
}
void MainWindow::openTableView(QTreeWidgetItem *item, const QJsonObject &view) {
    if (!item || service_.busy())
        return;
    const auto id = item->data(0, Qt::UserRole).toString();
    const auto n =
        item->data(0, Qt::UserRole + 2).value<navidog::CatalogNode>();
    for (auto p : profiles_)
        if (p.id == id) {
            createDataPane(p, n)->openSavedView(view);
            return;
        }
}
void MainWindow::tableTaskDialog(QTreeWidgetItem *item,
                                 navidog::TableAction action,
                                 const QString &option, bool withData) {
    if (!item || service_.busy())
        return;
    const auto id = item->data(0, Qt::UserRole).toString();
    auto node = item->data(0, Qt::UserRole + 2).value<navidog::CatalogNode>();
    auto profile = std::find_if(profiles_.begin(), profiles_.end(),
                                [&](const auto &p) { return p.id == id; });
    if (profile == profiles_.end() ||
        !navidog::supportsTableAction(profile->driver, action))
        return;
    if (navidog::tableActionWrites(action))
        for (int i = 0; i < tabs_->count(); ++i) {
            if (auto *pane = dynamic_cast<DataPane *>(tabs_->widget(i));
                pane && pane->matchesTable(id, node, node.object.isEmpty()) &&
                pane->dirty()) {
                QMessageBox::information(this, "未提交编辑",
                                         "请先处理该表的数据草稿。");
                return;
            }
            if (auto *pane = dynamic_cast<TablePane *>(tabs_->widget(i));
                pane && pane->matchesTable(id, node, node.object.isEmpty()) &&
                pane->dirty()) {
                QMessageBox::information(this, "未保存结构",
                                         "请先处理该表的结构草稿。");
                return;
            }
        }
    if (!promptPassword(*profile))
        return;
    auto target = *profile;
    if (target.driver != navidog::DatabaseKind::SQLite &&
        !navidog::usesOracleSql(target.driver) && !node.database.isEmpty())
        target.database = node.database;
    navidog::TableTask task;
    task.node = node;
    task.action = action;
    task.option = option;
    task.withData = withData;
    if (action == navidog::TableAction::GenerateData)
        task.maxRows = 100;
    TableTaskDialog dialog(target, task, service_, this);
    dialog.exec();
    refreshWorkspaceActivity();
    if (!dialog.succeeded())
        return;
    task = dialog.task();
    node = task.node;
    using T = navidog::TableAction;
    if (action == T::ReverseModel || action == T::BuildBI) {
        auto *report = new TableReportDialog(task, dialog.outcome(), this,
            navidog::ui::exportContext(target, task.node.database, task.node.schema));
        report->show();
    }
    if (action == T::Rename || action == T::Drop)
        navigation_->updateTableSettingsForObjectChange(
            id, node, action == T::Rename ? task.targetName : QString{});
    if (navidog::tableActionWrites(action)) {
        for (int i = tabs_->count() - 1; i >= 0; --i) {
            auto *data = dynamic_cast<DataPane *>(tabs_->widget(i));
            auto *table = dynamic_cast<TablePane *>(tabs_->widget(i));
            if ((data && data->matchesTable(id, node)) ||
                (table && table->matchesTable(id, node))) {
                if (action == T::Drop) {
                    auto *pane = tabs_->widget(i);
                    tabs_->removeTab(i);
                    pane->deleteLater();
                    continue;
                }
                if (action != T::CopyData && action != T::CopyStructure) {
                    const auto renamedTo = action == T::Rename ? task.targetName : QString{};
                    if (data)
                        data->invalidateTable(renamedTo);
                    if (table)
                        table->invalidateTable(renamedTo);
                }
            }
        }
        if (tabs_->count() == 0)
            newQuery();
        navigation_->refreshFolder(id, node, "tables");
    }
    status_->setText(dialog.outcome().report);
}
void MainWindow::pasteTable(QTreeWidgetItem *item) {
    if (!item || service_.busy())
        return;
    auto *mime = QApplication::clipboard()->mimeData();
    if (!mime)
        return;
    auto ref =
        QJsonDocument::fromJson(mime->data("application/x-navidog-table"))
            .object();
    auto target = item->data(0, Qt::UserRole + 2).value<navidog::CatalogNode>();
    const auto id = item->data(0, Qt::UserRole).toString();
    if (ref["profile"].toString() != id ||
        ref["database"].toString() != target.database ||
        ref["schema"].toString() != target.schema)
        return;
    auto source = target;
    source.kind = "table";
    source.name = source.object = ref["table"].toString();
    QTreeWidgetItem fixture;
    fixture.setData(0, Qt::UserRole, id);
    fixture.setData(0, Qt::UserRole + 2, QVariant::fromValue(source));
    tableTaskDialog(&fixture, navidog::TableAction::CopyData);
}
void MainWindow::removeConnection() {
    if (service_.busy()) {
        status_->setText("请先停止查询");
        return;
    }
    auto *item = navigation_->currentItem();
    if (!item || NavigationPane::isConnectionGroup(item) ||
        item->data(0, Qt::UserRole + 2).value<navidog::CatalogNode>().kind !=
            "connection")
        return;
    const auto id = item->data(0, Qt::UserRole).toString();
    if (id.isEmpty() || id == "harbor-demo") {
        status_->setText("示例连接保留用于本地体验");
        return;
    }
    if (hasConnectionDrafts(id)) {
        QMessageBox::information(this, "删除连接",
                                 "请先处理此连接下的未提交事务和编辑草稿。");
        return;
    }
    int tabCount = 0;
    for (int i = 0; i < tabs_->count(); ++i)
        if (auto *pane = dynamic_cast<WorkspacePane *>(tabs_->widget(i));
            pane && pane->profileId == id)
            ++tabCount;
    if (QMessageBox::question(
            this, "删除连接",
            QString(
                "删除连接“%"
                "1”？\n仅删除本地配置，数据库文件与数据不受影响。\n将关闭此连"
                "接的 %2 个标签；未提交的 SQL 事务会回滚。")
                .arg(item->text(0))
                .arg(tabCount),
            QMessageBox::Yes | QMessageBox::Cancel,
            QMessageBox::Cancel) != QMessageBox::Yes)
        return;
    auto updated = profiles_;
    updated.erase(std::remove_if(updated.begin(), updated.end(),
                                 [&](const auto &p) { return p.id == id; }),
                  updated.end());
    try {
        store_.save(updated);
    } catch (const std::exception &e) {
        QMessageBox::warning(this, "无法保存", QString::fromUtf8(e.what()));
        return;
    }
    metadata_->removeProfile(id);
    querySessions_->disconnectProfile(id);
    navigation_->clearConnectionCatalog(item);
    profiles_ = updated;
    connection_->removeItem(connection_->findData(id));
    for (int i = tabs_->count() - 1; i >= 0; --i)
        if (auto *pane = dynamic_cast<WorkspacePane *>(tabs_->widget(i));
            pane && pane->profileId == id) {
            tabs_->removeTab(i);
            pane->deleteLater();
        }
    syncConnections();
    navigation_->rebuild();
    if (tabs_->count() == 0)
        newQuery();
    status_->setText("连接已删除，数据库数据未改变。");
}
void MainWindow::formatCurrent() {
    auto *query = currentPane();
    auto *source = dynamic_cast<SourcePane *>(tabs_->currentWidget());
    auto *workspace = dynamic_cast<WorkspacePane *>(tabs_->currentWidget());
    if (!workspace || workspace->property("closingWorkspace").toBool() ||
        (!source &&
         (!query || query->kind != navidog::WorkspaceTabKind::Query)))
        return;
    auto *editor = source ? source->editor : query->editor;
    if (query && !query->allowSqlAction())
        return;
    if (source && (source->loading || source->saving || !source->loaded))
        return;
    const auto id = workspace->profileId;
    auto p = std::find_if(profiles_.begin(), profiles_.end(),
                          [&](const auto &p) { return p.id == id; });
    const auto dialect =
        navidog::compilerDialectFor(p == profiles_.end() ? navidog::DatabaseKind::SQLite : p->driver)
            .value_or(navidog::sql::compiler::Dialect::SQLite);
    if (p != profiles_.end() && p->driver == navidog::DatabaseKind::Redis)
        return;
    const auto db = query ? query->sessionDatabase : workspace->database,
               schema = query ? query->schema : source->node.schema;
    QPointer<MainWindow> self(this);
    QPointer<WorkspacePane> guarded(workspace);
    formatEditorAsync(
        editor, dialect, navidog::localStorePath(dataDirectory_), {},
        [guarded, id, db, schema] {
            if (!guarded || guarded->property("closingWorkspace").toBool() ||
                guarded->profileId != id)
                return false;
            if (auto *q = dynamic_cast<QueryPane *>(guarded.data()))
                return q->sessionDatabase == db && q->schema == schema;
            auto *s = dynamic_cast<SourcePane *>(guarded.data());
            return s && s->database == db && s->node.schema == schema &&
                   !s->loading && !s->saving;
        },
        [self](const QString &message) {
            if (self)
                self->status_->setText(message);
        });
}
void MainWindow::applyEditorSettings() {
    for (auto *editor : findChildren<SqlEditor *>())
        editor->applyPreferences(
            preferences_.editorFontSize, preferences_.editorIndentWidth,
            preferences_.editorWordWrap, preferences_.editorCompletion,
            preferences_.completionMinPrefix);
}
void MainWindow::showSettings(int category) {
    auto *pane = currentPane();
    auto *source = dynamic_cast<SourcePane *>(tabs_->currentWidget());
    auto *workspace = dynamic_cast<WorkspacePane *>(tabs_->currentWidget());
    auto p =
        std::find_if(profiles_.begin(), profiles_.end(), [&](const auto &p) {
            return workspace && p.id == workspace->profileId;
        });
    const auto dialect =
        navidog::compilerDialectFor(p == profiles_.end() ? navidog::DatabaseKind::SQLite : p->driver)
            .value_or(navidog::sql::compiler::Dialect::SQLite);
    const auto *settingsEditor = source ? source->editor : pane ? pane->editor : nullptr;
    const auto sample = settingsEditor && !settingsEditor->largeDocument()
        ? settingsEditor->toPlainText() : QString{};
    if (editApplicationSettings(this, navidog::localStorePath(dataDirectory_),
                                dialect, sample, category)) {
        preferences_ =
            loadApplicationSettings(navidog::localStorePath(dataDirectory_));
        navidog::ui::setExportDirectory(*this, preferences_.exportDirectory);
        applyEditorSettings();
        metadata_->setAutoRefreshEnabled(preferences_.autoMetadata);
        status_->setText("设置已保存。");
    }
}
void MainWindow::exportCurrent() {
    if (auto *pane = dynamic_cast<DataPane *>(tabs_->currentWidget())) {
        pane->exportData();
        return;
    }
    navidog::QueryResult result;
    navidog::ui::ExportContext context;
    auto kind = navidog::DatabaseKind::SQLite;
    if (auto *pane = currentPane()) {
        auto *grid = qobject_cast<QTableView *>(pane->resultTabs->currentWidget());
        auto *model = grid ? dynamic_cast<ResultModel *>(grid->model()) : pane->model;
        if (model)
            result = model->result();
        context = pane->resultExportContext;
        kind = pane->resultExportDriver;
    }
    if (result.columns.isEmpty()) {
        status_->setText("请先加载数据再导出");
        return;
    }
    try {
        const auto path = navidog::ui::exportCachedResult(
            this, result, context, "查询结果", kind, "exported_result");
        if (!path.isEmpty())
            status_->setText("当前缓存结果已导出：" + path);
    } catch (const std::exception &e) {
        showError("导出失败", QString::fromUtf8(e.what()));
    }
}

bool MainWindow::saveWorkspace() {
    try {
        store_.saveWorkspace({profiles_, navigation_->connectionGroups()});
        return true;
    } catch (const std::exception &e) {
        QMessageBox::warning(this, "无法保存", QString::fromUtf8(e.what()));
        return false;
    }
}
void MainWindow::closeDatabase(QTreeWidgetItem *item) {
    if (!item || service_.busy())
        return;
    auto n = item->data(0, Qt::UserRole + 2).value<navidog::CatalogNode>();
    if (n.kind != "database" && n.kind != "schema")
        return;
    const auto id = item->data(0, Qt::UserRole).toString();
    auto actualDatabase = n.database;
    for (const auto &profile : profiles_)
        if (profile.id == id && profile.driver == navidog::DatabaseKind::SQLite)
            actualDatabase = profile.database;
    if (n.kind == "database" && querySessions_->hasActiveTransaction(id, &actualDatabase) &&
        QMessageBox::question(
            this, "关闭数据库",
            "该数据库的查询标签仍有未提交事务，关闭后将回滚。继续？",
            QMessageBox::Yes | QMessageBox::Cancel,
            QMessageBox::Cancel) != QMessageBox::Yes)
        return;
    navigation_->clearNamespace(item);
    if (n.kind == "database")
        querySessions_->disconnectDatabase(id, actualDatabase);
    status_->setText(n.kind == "schema"
                         ? "Schema 已收起"
                         : "数据库已关闭；该数据库会话中未提交事务会回滚");
}
void MainWindow::updateNodeActions() {
    auto *item = navigation_->currentItem();
    const auto node =
        item ? item->data(0, Qt::UserRole + 2).value<navidog::CatalogNode>()
             : navidog::CatalogNode{};
    const bool group = NavigationPane::isConnectionGroup(item);
    const bool connection = item && !group && node.kind == "connection" &&
                            !item->data(0, Qt::UserRole).toString().isEmpty();
    const bool idle = !service_.busy();
    const bool connected =
        connection &&
        service_.isConnected(item->data(0, Qt::UserRole).toString());
    quickEntries_->setNewConnectionEnabled(idle);
    removeConnectionButton_->setEnabled(
        idle && connection &&
        item->data(0, Qt::UserRole).toString() != "harbor-demo");
    if (newConnectionAction_)
        newConnectionAction_->setEnabled(idle);
    if (renameAction_)
        renameAction_->setEnabled(idle && (group || connection));
    if (removeAction_)
        removeAction_->setEnabled(idle && connection &&
                                  item->data(0, Qt::UserRole).toString() !=
                                      "harbor-demo");
    if (connectAction_)
        connectAction_->setEnabled(idle && connection && !connected);
    if (editConnectionAction_)
        editConnectionAction_->setEnabled(
            connection && !service_.operationBusy() &&
            !service_.catalogBusy() && !workspaceCloseFlow_ &&
            pendingCloseTabs_.isEmpty() && pendingEditAfterClose_.isEmpty());
    if (disconnectAction_)
        disconnectAction_->setEnabled(idle && connection && connected);
    if (refreshAction_)
        refreshAction_->setEnabled(idle && item && !group && node.expandable);
    const bool create =
        item && (node.kind == "schema" || node.kind == "tables" ||
                 node.kind == "tableGroup" || node.kind == "table" ||
                 node.kind == "database");
    auto p =
        std::find_if(profiles_.cbegin(), profiles_.cend(), [&](const auto &p) {
            return item && p.id == item->data(0, Qt::UserRole).toString();
        });
    const bool writable = idle && p != profiles_.cend() && !p->readOnly &&
                          p->driver != navidog::DatabaseKind::Dameng;
    if (newTableAction_)
        newTableAction_->setEnabled(create && writable);
    if (alterTableAction_)
        alterTableAction_->setEnabled(item && node.kind == "table" && writable);
    quickEntries_->setSelectedCategory(navigation_->selectedObjectCategory());
}

void MainWindow::updateWorkspaceActions() {
    auto *active = tabs_->currentWidget();
    auto *query = currentPane();
    const bool editableQuery =
        query && query->kind == navidog::WorkspaceTabKind::Query &&
        !query->property("closingWorkspace").toBool();
    const bool canRun = query && query->canRun(service_.operationBusy());
    const bool source = dynamic_cast<SourcePane *>(active) != nullptr;
    if (runAction_)
        runAction_->setEnabled(canRun);
    if (formatAction_)
        formatAction_->setEnabled(editableQuery || source);
    if (exportAction_)
        exportAction_->setEnabled(query || dynamic_cast<DataPane *>(active));
    if (auto *action = findChild<QAction *>("menuSaveSqlAs"))
        action->setEnabled(editableQuery);
    if (auto *action = findChild<QAction *>("menuSaveDocument"))
        action->setEnabled(
            editableQuery || dynamic_cast<ErPane *>(active) ||
            (!service_.busy() && (dynamic_cast<TablePane *>(active) || source)));
    if (auto *action = findChild<QAction *>("menuExplainSql"))
        action->setEnabled(editableQuery && canRun);
    if (auto *action = findChild<QAction *>("menuStopCurrentQuery"))
        action->setEnabled(query && query->executing);

}
void MainWindow::updateServerInfo(const QString &id, const QString &raw,
                                  navidog::DatabaseVersion detected) {
    if (raw.isEmpty())
        return;
    for (auto &p : profiles_)
        if (p.id == id) {
            if (p.serverVersion != raw || p.detectedVersion != detected) {
                p.serverVersion = raw;
                p.detectedVersion = detected;
                try {
                    store_.saveWorkspace(
                        {profiles_, navigation_->connectionGroups()});
                } catch (const std::exception &e) {
                    status_->setText(QString::fromUtf8(e.what()));
                }
            }
            navigation_->updateConnectionTooltip(p);
            break;
        }
}
void MainWindow::editFocused(int operation) {
    for (auto *target = QApplication::focusWidget(); target && target != this;
         target = target->parentWidget()) {
        if (auto *text = qobject_cast<QPlainTextEdit *>(target)) {
            if (operation == 0)
                text->undo();
            else if (operation == 1)
                text->redo();
            else if (operation == 2)
                text->cut();
            else if (operation == 3)
                text->copy();
            else if (operation == 4)
                text->paste();
            return;
        }
        if (auto *text = qobject_cast<QLineEdit *>(target)) {
            if (operation == 0)
                text->undo();
            else if (operation == 1)
                text->redo();
            else if (operation == 2)
                text->cut();
            else if (operation == 3)
                text->copy();
            else if (operation == 4)
                text->paste();
            return;
        }
        if (auto *grid = qobject_cast<QTableView *>(target)) {
            if (operation == 3)
                try {
                    const auto role =
                        dynamic_cast<DataGridModel *>(grid->model())
                            ? Qt::EditRole
                        : dynamic_cast<ResultModel *>(grid->model())
                            ? Qt::UserRole
                            : Qt::DisplayRole;
                    auto text =
                        navidog::ui::resultSelectionText(grid, false, role);
                    if (!text.isNull())
                        QApplication::clipboard()->setText(text);
                } catch (const std::exception &e) {
                    showError("复制失败", QString::fromUtf8(e.what()));
                }
            return;
        }
    }
}
void MainWindow::setupMenus() {
    auto *file = menuBar()->addMenu("文件");
    newConnectionAction_ =
        file->addAction("新建连接…", this, &MainWindow::addConnection);
    newConnectionAction_->setObjectName("menuNewConnection");
    file->addAction("新建连接分组…", navigation_,
                    &NavigationPane::addConnectionGroup)
        ->setObjectName("menuNewGroup");
    file->addSeparator();
    file->addAction("新建查询", this, [this] { newQuery(); });
    auto *openSql =
        file->addAction("打开 SQL 文件…", this, &MainWindow::openSqlFile);
    openSql->setObjectName("menuOpenSql");
    openSql->setShortcut(QKeySequence::Open);
    auto *saveSql =
        file->addAction("保存", this, [this] { saveCurrentDocument(); });
    saveSql->setObjectName("menuSaveDocument");
    saveSql->setShortcut(QKeySequence::Save);
    auto *saveAs = file->addAction("SQL 另存为…", this,
                                   [this] { saveCurrentDocument(true); });
    saveAs->setObjectName("menuSaveSqlAs");
    saveAs->setShortcut(QKeySequence::SaveAs);
    exportAction_ =
        file->addAction("导出查询结果…", this, &MainWindow::exportCurrent);
    file->addSeparator();
    auto *closeWindow = file->addAction("关闭窗口", this, &QWidget::close);
    closeWindow->setShortcut(QKeySequence("Ctrl+Shift+W"));
    auto *closeTab = file->addAction("关闭标签", this, [this] {
        if (tabs_->currentIndex() >= 0)
            emit tabs_->tabCloseRequested(tabs_->currentIndex());
    });
    closeTab->setObjectName("menuCloseTab");
    closeTab->setShortcut(QKeySequence::Close);
    auto *edit = menuBar()->addMenu("编辑");
    edit->addAction("撤销", this, [this] { editFocused(0); });
    edit->addAction("重做", this, [this] { editFocused(1); });
    edit->addSeparator();
    edit->addAction("剪切", this, [this] { editFocused(2); });
    edit->addAction("复制", this, [this] { editFocused(3); });
    edit->addAction("粘贴", this, [this] { editFocused(4); });
    edit->addSeparator();
    auto searchEditor = [this]() -> SqlEditor * {
        auto *widget = tabs_->currentWidget();
        return widget ? widget->findChild<SqlEditor *>() : nullptr;
    };
    auto *find = edit->addAction("查找…", this, [searchEditor] {
        if (auto *editor = searchEditor())
            editor->showFind();
    });
    find->setObjectName("menuFindSql");
    find->setShortcut(QKeySequence::Find);
    auto *replace = edit->addAction("替换…", this, [searchEditor] {
        if (auto *editor = searchEditor())
            editor->showFind(true);
    });
    replace->setObjectName("menuReplaceSql");
    replace->setShortcut(QKeySequence::Replace);
    auto *next = edit->addAction("查找下一个", this, [searchEditor] {
        if (auto *editor = searchEditor())
            editor->findNext();
    });
    next->setShortcut(QKeySequence::FindNext);
    auto *previous = edit->addAction("查找上一个", this, [searchEditor] {
        if (auto *editor = searchEditor())
            editor->findNext(true);
    });
    previous->setShortcut(QKeySequence::FindPrevious);
    auto searchActions = [searchEditor, find, replace, next, previous] {
        auto *editor = searchEditor();
        find->setEnabled(editor);
        replace->setEnabled(editor && !editor->isReadOnly());
        next->setEnabled(editor);
        previous->setEnabled(editor);
    };
    connect(tabs_, &QTabWidget::currentChanged, this,
            [searchActions](int) { searchActions(); });
    connect(edit, &QMenu::aboutToShow, this, searchActions);
    searchActions();
    edit->addSeparator();
    auto *indent = edit->addAction("增加缩进", this, [searchEditor] {
        if (auto *editor = searchEditor())
            editor->indentLines();
    });
    indent->setObjectName("menuIndentSql");
    auto *outdent = edit->addAction("减少缩进", this, [searchEditor] {
        if (auto *editor = searchEditor())
            editor->indentLines(true);
    });
    outdent->setObjectName("menuOutdentSql");
    auto *comment = edit->addAction("切换行注释", this, [searchEditor] {
        if (auto *editor = searchEditor())
            editor->toggleLineComment();
    });
    comment->setObjectName("menuCommentSql");
    comment->setShortcut(QKeySequence("Ctrl+/"));
    auto *goLine = edit->addAction("转到行…", this, [searchEditor] {
        if (auto *editor = searchEditor())
            editor->goToLine();
    });
    goLine->setObjectName("menuGoToSqlLine");
    goLine->setShortcut(QKeySequence("Ctrl+Alt+L"));
    auto *complete = edit->addAction("补全 SQL", this, [searchEditor] {
        if (auto *editor = searchEditor())
            QTimer::singleShot(0, editor,
                               [editor] { editor->showCompletion(); });
    });
    complete->setObjectName("menuCompleteSql");
    auto commandActions = [searchEditor, indent, outdent, comment, goLine,
                           complete] {
        auto *editor = searchEditor();
        const bool writable = editor && !editor->isReadOnly();
        indent->setEnabled(writable);
        outdent->setEnabled(writable);
        comment->setEnabled(writable);
        goLine->setEnabled(editor);
        complete->setEnabled(writable);
    };
    connect(tabs_, &QTabWidget::currentChanged, this,
            [commandActions](int) { commandActions(); });
    connect(edit, &QMenu::aboutToShow, this, commandActions);
    commandActions();
    auto *view = menuBar()->addMenu("视图");
    auto *sidebar = view->addAction("连接资源");
    sidebar->setObjectName("menuConnectionResources");
    sidebar->setCheckable(true);
    sidebar->setChecked(true);
    connect(sidebar, &QAction::toggled, navigation_, &QWidget::setVisible);
    refreshAction_ = view->addAction("刷新选中对象", this, [this] {
        navigation_->expandCatalog(navigation_->currentItem(), true);
    });
    refreshAction_->setShortcut(QKeySequence("Ctrl+R"));
    auto *connections = menuBar()->addMenu("连接");
    connectAction_ = connections->addAction("打开连接", this, [this] {
        navigation_->openConnection(navigation_->currentItem());
    });
    connectAction_->setObjectName("menuOpenConnection");
    editConnectionAction_ =
        connections->addAction("编辑连接…", this, &MainWindow::editConnection);
    editConnectionAction_->setObjectName("menuEditConnection");
    disconnectAction_ = connections->addAction("关闭连接", this, [this] {
        closeConnection(navigation_->currentItem());
    });
    disconnectAction_->setObjectName("menuCloseConnection");
    connections->addSeparator();
    renameAction_ = connections->addAction("重命名…", navigation_,
                                           &NavigationPane::renameSelection);
    renameAction_->setObjectName("menuRenameConnection");
    renameAction_->setShortcut(QKeySequence("F2"));
    removeAction_ = connections->addAction("删除连接…", this,
                                           &MainWindow::removeConnection);
    removeAction_->setObjectName("menuDeleteConnection");
    auto *table = menuBar()->addMenu("表");
    newTableAction_ = table->addAction("新建表…", this, [this] {
        openTableDesigner(navigation_->currentItem(), false);
    });
    newTableAction_->setObjectName("menuNewTable");
    alterTableAction_ = table->addAction("修改表…", this, [this] {
        openTableDesigner(navigation_->currentItem(), true);
    });
    alterTableAction_->setObjectName("menuAlterTable");
    auto *query = menuBar()->addMenu("查询");
    auto *newSql = query->addAction("新建查询", this, [this] { newQuery(); });
    newSql->setObjectName("menuNewQuery");
    newSql->setShortcut(QKeySequence("Ctrl+Y"));
    query->addSeparator();
    runAction_ = query->addAction("运行", this, &MainWindow::runCurrent);
    query->addAction("查询库", this, [this] { openQueryLibrary(); })
        ->setObjectName("menuQueryLibrary");
    query
        ->addAction("保存到查询库…", this,
                    [this] { saveQueryInLibrary(currentPane()); })
        ->setObjectName("menuSaveQueryInLibrary");
    query->addAction("解释当前语句", this, [this] {
        if (auto *pane = currentPane())
            pane->explainCurrent();
    })
        ->setObjectName("menuExplainSql");
    query
        ->addAction("停止当前查询", this,
                    [this] {
                        if (auto *pane = currentPane())
                            querySessions_->cancel(*pane);
                    })
        ->setObjectName("menuStopCurrentQuery");
    query->addAction("转换 SQL…", this, [this] {
        if (auto *pane = dynamic_cast<WorkspacePane *>(tabs_->currentWidget()))
            pane->openSqlTool(EditorSqlTool::Convert);
    })->setObjectName("menuConvertSql");
    query->addAction("本地优化…", this, [this] {
        if (auto *pane = dynamic_cast<WorkspacePane *>(tabs_->currentWidget()))
            pane->openSqlTool(EditorSqlTool::Optimize);
    })->setObjectName("menuOptimizeSql");
    formatAction_ =
        query->addAction("格式化 SQL", this, &MainWindow::formatCurrent);
    formatAction_->setShortcut(QKeySequence("Ctrl+Shift+F"));
    auto *tools = menuBar()->addMenu("工具");
    auto *settings =
        tools->addAction("设置…", this, [this] { showSettings(); });
    settings->setObjectName("applicationSettingsAction");
    settings->setMenuRole(QAction::PreferencesRole);
    settings->setShortcut(QKeySequence("Ctrl+,"));
    tools->addSeparator();
    tools->addAction("驱动与运行时…", this, [this] { showDriverManager(this); })
        ->setObjectName("menuDriverStatus");
    updateNodeActions();
    auto *modeling = menuBar()->addMenu("建模");
    auto *newModel = modeling->addAction("新建 ER 模型…", this, &MainWindow::newErProject);
    newModel->setObjectName("menuNewErModel");
    auto *openModel = modeling->addAction("打开 ER 模型…", this, &MainWindow::openErProject);
    openModel->setObjectName("menuOpenErModel");
    auto *reverseModel = modeling->addAction(
        "从数据库逆向导入…", this,
        [this] { importErProject(navigation_->currentItem()); });
    reverseModel->setObjectName("menuReverseErModel");
    quickEntries_->setModelActions({newModel, openModel, reverseModel});
    auto *help = menuBar()->addMenu("帮助");
    applicationHelp_ =
        new ApplicationHelp(help, this, dataDirectory_, startupTiming_);
    help->addSeparator();
    help->addAction("关于 Navidog", this, [this] {
        QMessageBox::about(
            this, "Navidog",
            QString(
                "Navidog %1\nC++20 · Qt 原生桌面数据库工作台\n原生连接 · SSL · "
                "SSH · HTTP · 表设计")
                .arg(QCoreApplication::applicationVersion()));
    });
}
void MainWindow::scheduleUpdateCheck() {
    if (!preferences_.automaticUpdateCheck)
        return;
    QTimer::singleShot(5000, this, [this] {
        if (preferences_.automaticUpdateCheck && isVisible())
            applicationHelp_->checkForUpdates(false);
    });
}

void MainWindow::closeEvent(QCloseEvent *event) {
    if (quickBackup_) {
        status_->setText("请等待备份或恢复结束后再退出");
        event->ignore();
        return;
    }
    if (workspaceCloseFlow_ || !pendingCloseTabs_.isEmpty()) {
        status_->setText("正在关闭标签，请稍候再退出");
        event->ignore();
        return;
    }
    if (service_.busy()) {
        status_->setText("请等待当前数据库请求完成或停止后再退出");
        event->ignore();
        return;
    }
    for (int i = 0; i < tabs_->count(); ++i)
        if (auto *pane = dynamic_cast<ErPane *>(tabs_->widget(i));
            pane && pane->busy()) {
            tabs_->setCurrentWidget(pane);
            status_->setText("请等待 ER 工作区操作完成，或停止后再退出");
            event->ignore();
            return;
        }
    bool activeTransaction = querySessions_->hasActiveTransaction();
    for (int i = 0; i < tabs_->count(); ++i)
        activeTransaction |= databaseConsoleHasTransaction(tabs_->widget(i));
    if (activeTransaction &&
        QMessageBox::question(
            this, "未提交事务",
            "退出会关闭查询会话，未提交事务将回滚。仍要退出？",
            QMessageBox::Yes | QMessageBox::Cancel,
            QMessageBox::Cancel) != QMessageBox::Yes) {
        event->ignore();
        return;
    }
    bool dirty = false;
    for (int i = 0; i < tabs_->count(); ++i)
        if (auto *pane = dynamic_cast<DataPane *>(tabs_->widget(i));
            pane && pane->dirty())
            dirty = true;
    if (dirty &&
        QMessageBox::question(this, "未提交数据",
                              "数据页仍有未提交修改。丢弃这些修改并退出？",
                              QMessageBox::Discard | QMessageBox::Cancel,
                              QMessageBox::Cancel) != QMessageBox::Discard) {
        event->ignore();
        return;
    }
    for (int i = 0; i < tabs_->count(); ++i)
        if (auto *diagram = dynamic_cast<ErPane *>(tabs_->widget(i)))
            try {
                diagram->persistDraft();
            } catch (const std::exception &e) {
                showError("ER 草稿保存失败", QString::fromUtf8(e.what()));
                event->ignore();
                return;
            }
    try {
        WorkspaceSessionStore(dataDirectory_).save(*this, *tabs_, !navigation_->isHidden());
    } catch (const std::exception &e) {
        status_->setText("查询标签保存失败：" + QString::fromUtf8(e.what()));
        event->ignore();
        return;
    }
    event->accept();
}
void MainWindow::restoreSession() {
    std::optional<WorkspaceSession> session;
    try {
        session = WorkspaceSessionStore(dataDirectory_).load();
    } catch (const std::exception &e) {
        status_->setText(QString::fromUtf8(e.what()));
        return;
    }
    if (!session)
        return;
    if (!session->geometry.isEmpty())
        restoreGeometry(session->geometry);
    if (!session->splitterState.isEmpty())
        if (auto *splitter = findChild<QSplitter *>("workspaceSplitter"))
            splitter->restoreState(session->splitterState);
    if (auto *sidebar = findChild<QAction *>("menuConnectionResources"))
        sidebar->setChecked(session->sidebarVisible);
    if (!session->tabs)
        return;
    const auto &queries = *session->tabs;
    if (queries.isEmpty()) {
        restoredSession_ = true;
        return;
    }
    for (auto item : queries) {
        auto q = item.toObject();
        auto id = q["profileId"].toString();
        auto kind = q["kind"].toString("query");
        if (kind != "erModel" && kind != "queryLibrary" &&
            std::none_of(profiles_.begin(), profiles_.end(),
                         [&](auto &p) { return p.id == id; }))
            continue;
        if (kind == "erModel") {
            createErPane()->restore(q);
        } else if (kind == "queryLibrary") {
            openQueryLibrary(id, q["database"].toString(),
                             q["schema"].toString());
        } else if (kind == "databaseSearch" || kind == "databaseConsole") {
            navidog::CatalogNode scope;
            scope.database = q["database"].toString();
            scope.schema = q["schema"].toString();
            scope.kind = scope.schema.isEmpty() ? "database" : "schema";
            if (kind == "databaseSearch")
                openDatabaseSearch(id, scope);
            else
                openDatabaseConsole(id, scope);
        } else if (kind == "objects") {
            auto p = *std::find_if(profiles_.begin(), profiles_.end(),
                                   [&](const auto &p) { return p.id == id; });
            navidog::CatalogNode folder;
            folder.kind = q["folderKind"].toString();
            folder.name = q["folderName"].toString();
            folder.database = q["database"].toString();
            folder.schema = q["schema"].toString();
            folder.key = q["key"].toString();
            auto *restoredObjects = createObjectsPane(p, folder);
            tabs_->setCurrentWidget(restoredObjects);
            restoredObjects->setNodes({}, false);
        } else if (kind == "data") {
            auto p = *std::find_if(profiles_.begin(), profiles_.end(),
                                   [&](auto p) { return p.id == id; });
            navidog::CatalogNode n;
            n.kind = q["nodeKind"].toString("table");
            n.object = n.name = q["object"].toString();
            n.database = q["database"].toString();
            n.schema = q["schema"].toString();
            createDataPane(p, n)->restoreState(q);
        } else if (kind == "source") {
            auto p = *std::find_if(profiles_.begin(), profiles_.end(),
                                   [&](auto p) { return p.id == id; });
            auto value = q["node"].toObject();
            navidog::CatalogNode n;
            n.kind = value["kind"].toString();
            n.object = n.name = value["object"].toString();
            n.database = value["database"].toString();
            n.schema = value["schema"].toString();
            n.key = value["key"].toString();
            n.detail = value["detail"].toString();
            createSourcePane(p, n, q["exists"].toBool())->restoreState(q);
        } else if (kind == "createTable" || kind == "alterTable") {
            auto p = *std::find_if(profiles_.begin(), profiles_.end(),
                                   [&](auto p) { return p.id == id; });
            navidog::CatalogNode n;
            n.kind = "table";
            n.name = n.object = q["object"].toString();
            n.database = q["database"].toString();
            n.schema = q["schema"].toString();
            auto *pane = createTablePane(p, n, kind == "alterTable");
            pane->restoreState(q);
        } else {
            const auto sql = q["sql"].toString();
            newQuery(sql, id, q["database"].toString(),
                     kind == "preview" ? navidog::WorkspaceTabKind::Preview
                     : kind == "definition"
                         ? navidog::WorkspaceTabKind::Definition
                         : navidog::WorkspaceTabKind::Query,
                     q["schema"].toString());
            auto *pane = currentPane();
            if (sql.isEmpty())
                pane->editor->clear();
            if (q["externalSql"].toBool())
                pane->restoreLargeScript(q, WorkspaceSessionStore(dataDirectory_).sqlDraftPath(q["draft"].toString()));
            else
                pane->restoreScriptState(q);
            pane->setResultsLayout(q["resultLayout"].toString() == "beside");
            pane->setEditorFocus(q["editorFocus"].toBool());
            pane->restoreOutlineState(q);
            auto limit = pane->limit->findData(q["rowLimit"].toInt(1000));
            pane->limit->setCurrentIndex(limit < 0 ? 0 : limit);
            {
                QSignalBlocker blocker(pane->autoCommit);
                pane->autoCommit->setChecked(q["autoCommit"].toBool(true));
            }
        }
        tabs_->setTabText(tabs_->currentIndex(), q["title"].toString("查询"));
    }
    if (tabs_->count()) {
        tabs_->setCurrentIndex(
            std::clamp(session->activeTab, 0, tabs_->count() - 1));
        restoredSession_ = true;
    }
}

void MainWindow::openSqlFile() {
    const auto path = QFileDialog::getOpenFileName(
        this, "打开 SQL 文件", {}, "SQL 文件 (*.sql);;所有文件 (*)");
    if (path.isEmpty())
        return;
    QString id = connection_->currentData().toString(), database, schema;
    if (auto *current = dynamic_cast<WorkspacePane *>(tabs_->currentWidget())) {
        id = current->profileId;
        database = current->database;
        if (auto *query = dynamic_cast<QueryPane *>(current))
            schema = query->schema;
    }
    newQuery({}, id, database, navidog::WorkspaceTabKind::Query, schema);
    currentPane()->openScriptFile(path);
}
void MainWindow::saveCurrentDocument(bool saveAs) {
    if (auto *widget = tabs_->currentWidget();
        widget && widget->property("closingWorkspace").toBool())
        return;
    if (auto *pane = dynamic_cast<ErPane *>(tabs_->currentWidget())) {
        pane->saveFile(saveAs);
        return;
    }
    if (auto *pane = currentPane()) {
        pane->saveSqlFile(saveAs);
        return;
    }
    if (saveAs)
        return;
    if (auto *pane = dynamic_cast<TablePane *>(tabs_->currentWidget()))
        pane->requestSave();
    else if (auto *pane = dynamic_cast<SourcePane *>(tabs_->currentWidget());
             pane && pane->save->isEnabled())
        saveSource(pane);
}
