#pragma once
#include "application_settings.hpp"
#include "navidog/profile_store.hpp"
#include "navidog/query_service.hpp"
#include "navidog/statement_templates.hpp"
#include <QJsonObject>
#include <QMainWindow>
#include <QPointer>
#include <functional>
#include <memory>
#include <optional>
#include <utility>

class ApplicationHelp;
class ConnectionController;
class EditorSqlTools;
class MetadataController;
class QuerySessionController;
class NavigationPane;
class QTreeWidgetItem;
class QTabWidget;
class QComboBox;
class QToolButton;
class QLabel;
class QAction;
class QMenu;
class QCloseEvent;
class QPaintEvent;
class QueryPane;
enum class QueryContextPart;
class TablePane;
class DataPane;
class WorkspacePane;
class SourcePane;
class ObjectsPane;
class StartupTiming;
class ErPane;
class QThread;
class QTimer;
namespace navidog::ui {
class QuickEntryBar;
}
class MainWindow final : public QMainWindow {
    Q_OBJECT
  public:
    explicit MainWindow(const QString &dataDirectory, QWidget *parent = nullptr,
                        StartupTiming *startup = nullptr);
    ~MainWindow() override;
    void runCurrent();
    void analyzeCurrent();
    void scheduleUpdateCheck();
    bool restoredSession() const { return restoredSession_; }
  signals:
    void queryFinished();

  private:
    ApplicationSettings preferences_;
    navidog::ProfileStore store_;
    QVector<navidog::ConnectionProfile> profiles_;
    navidog::QueryService service_;
    std::unique_ptr<MetadataController> metadata_;
    std::unique_ptr<EditorSqlTools> editorSqlTools_;
    ConnectionController *connectionController_;
    QuerySessionController *querySessions_;
    NavigationPane *navigation_;
    QTabWidget *tabs_;
    QComboBox *connection_;
    QLabel *status_, *connectionCount_;
    QPointer<SourcePane> sourcePane_;
    QString closingConnectionStatusId_;
    void handleQueryCatalog(const QString &, const navidog::CatalogResult &);
    void handleNavigationCatalog(const QString &, const navidog::CatalogNode &,
                                 const navidog::CatalogResult &);
    struct NamespaceDeletion {
        navidog::ConnectionProfile profile;
        navidog::NamespaceKind kind;
        QString name, request;
        bool started = false;
    };
    std::optional<NamespaceDeletion> namespaceDeletion_;
    void dropNamespaceDialog(QTreeWidgetItem *);
    void startPendingNamespaceDrop();
    void showNamespaceProperties(QTreeWidgetItem *);
    std::optional<navidog::ConnectionProfile>
    databaseToolProfile(const QString &, const navidog::CatalogNode &);
    void editDatabaseProperties(const QString &, const navidog::CatalogNode &);
    void openDatabaseConsole(const QString &, const navidog::CatalogNode &);
    bool databaseConsoleHasTransaction(QWidget *) const;
    void openDatabaseSearch(const QString &, const navidog::CatalogNode &);
    void runDatabaseSqlFile(const QString &, const navidog::CatalogNode &);
    void dumpDatabaseSql(const QString &, const navidog::CatalogNode &,
                         bool withData);
    void showDatabaseDictionary(const QString &, const navidog::CatalogNode &);
    struct QueryCatalogRequest {
        QPointer<QueryPane> pane;
        QString profileId, database;
        bool schema = false;
    };
    QHash<QString, QueryCatalogRequest> queryCatalogRequests_;
    QHash<QString, QStringList> queryDatabases_, querySchemas_;
    void configureQueryContext(QueryPane *, bool preserveInput = false);
    void requestQueryCatalog(QueryPane *, bool schema);
    bool changeQueryContext(QueryPane *, QueryContextPart part);
    void cacheQueryCatalog(const QString &, const navidog::CatalogNode &,
                           const navidog::CatalogResult &);
    navidog::ui::QuickEntryBar *quickEntries_;
    QToolButton *removeConnectionButton_;
    QAction *newConnectionAction_ = nullptr, *renameAction_ = nullptr,
            *removeAction_ = nullptr, *editConnectionAction_ = nullptr;
    QAction *connectAction_ = nullptr, *disconnectAction_ = nullptr,
            *refreshAction_ = nullptr;
    QAction *newTableAction_ = nullptr, *alterTableAction_ = nullptr,
            *runAction_ = nullptr, *formatAction_ = nullptr,
            *exportAction_ = nullptr;
    QString dataDirectory_;
    StartupTiming *startupTiming_ = nullptr;
    ApplicationHelp *applicationHelp_ = nullptr;
    QJsonObject tableSettings_;
    bool tableSettingsWritable_ = true;
    bool saveTableSettings(const QJsonObject &);
    void saveTableView(QTreeWidgetItem *);
    void openTableView(QTreeWidgetItem *, const QJsonObject &);
    bool restoredSession_ = false;
    void restoreSession();
    void setupMenus();
    navidog::CatalogNode quickEntryContext(QString &profileId) const;
    void openQueryLibrary(const QString &id = {}, const QString &database = {},
                          const QString &schema = {});
    void saveQueryInLibrary(QueryPane *);
    void browseServerObjects(bool events);
    void showQuickBi();
    void showQuickBackup();
    void showQuickAutomation();
    void loadQuickAutomation();
    void runScheduledBackup(bool now = false);
    void startSqliteBackup(const QString &source, const QString &destination,
                           bool scheduled = false);
    void startBackupOperation(std::function<QString()> operation,
                              const QString &destination, bool scheduled);
    QPointer<QThread> quickBackup_;
    QTimer *quickBackupTimer_ = nullptr;
    QJsonObject quickBackupSchedule_;
    bool saveQuickBackupSchedule(const QJsonObject &);
    void editFocused(int operation);
    void updateNodeActions();
    void updateWorkspaceActions();
    void updateServerInfo(const QString &, const QString &,
                          navidog::DatabaseVersion);
    void closeDatabase(QTreeWidgetItem *);
    void populateNavigationMenu(QMenu *, QTreeWidgetItem *);
    ObjectsPane *createObjectsPane(const navidog::ConnectionProfile &,
                                   const navidog::CatalogNode &);
    void openObjects(QTreeWidgetItem *, bool activate = true);
    void refreshObjectsPanes();
    void generateStatement(QTreeWidgetItem *, navidog::StatementTemplate);
    void openSqlFile();
    void saveCurrentDocument(bool saveAs = false);
    ErPane *createErPane();
    void newErProject();
    void openErProject();
    void importErProject(QTreeWidgetItem *context = nullptr);
    bool saveWorkspace();
    void openObject(QTreeWidgetItem *, bool definition);
    void
    newQuery(const QString &sql = {}, const QString &profileId = {},
             const QString &database = {},
             navidog::WorkspaceTabKind kind = navidog::WorkspaceTabKind::Query,
             const QString &schema = {});
    void openTableDesigner(QTreeWidgetItem *, bool existing);
    TablePane *createTablePane(const navidog::ConnectionProfile &,
                               const navidog::CatalogNode &, bool existing);
    void editTableChild(QTreeWidgetItem *, int operation = 0);
    bool hasTableDataDraft(const QString &, const navidog::CatalogNode &) const;
    void handleTableStructureSaved(const QString &, const navidog::CatalogNode &);
    DataPane *createDataPane(const navidog::ConnectionProfile &,
                             const navidog::CatalogNode &);
    SourcePane *createSourcePane(const navidog::ConnectionProfile &,
                                 const navidog::CatalogNode &, bool existing);
    void openSource(QTreeWidgetItem *, bool existing);
    void loadSource(SourcePane *);
    void saveSource(SourcePane *, bool remove = false);
    void dropSource(QTreeWidgetItem *);
    void callSource(SourcePane *);
    void showError(const QString &, const QString &);
    bool promptPassword(navidog::ConnectionProfile &);
    void syncConnections();
    void closeConnection(QTreeWidgetItem *);
    void refreshConnectionStates();
    bool hasConnectionDrafts(const QString &) const;
    void addConnection();
    void editConnection();
    void handleConnectionSaved(const QString &id, bool edited, bool endpointChanged);
    void namespaceDialog(QTreeWidgetItem *, navidog::NamespaceKind);
    void tableTaskDialog(QTreeWidgetItem *, navidog::TableAction,
                         const QString &option = {}, bool withData = true);
    void pasteTable(QTreeWidgetItem *);
    int addWorkspaceTab(WorkspacePane *, const QString &);
    QVector<QPointer<WorkspacePane>> pendingCloseTabs_;
    QString pendingCloseEdit_, pendingEditAfterClose_;
    bool workspaceCloseFlow_ = false;
    void requestCloseWorkspaceTab(WorkspacePane *);
    bool prepareWorkspaceClose(WorkspacePane *, bool transactionConfirmed);
    void closeWorkspacePanes(const QVector<QPointer<WorkspacePane>> &,
                             const QString &editAfter = {},
                             bool transactionConfirmed = false);
    void finishPendingWorkspaceClose();
    void resumeConnectionEdit();
    bool confirmConnectionTabClosure(const QString &);
    void disposeWorkspacePane(WorkspacePane *);
    void removeConnection();
    void formatCurrent();
    void showSettings(int category = 0);
    void applyEditorSettings();
    void exportCurrent();
    QueryPane *currentPane() const;
    void refreshWorkspaceActivity();
    void closeEvent(QCloseEvent *) override;
    void paintEvent(QPaintEvent *) override;
};
