#pragma once

#include "navidog/catalog.hpp"
#include <QJsonObject>
#include <QList>
#include <QPersistentModelIndex>
#include <QSet>
#include <QTimer>
#include <QWidget>
#include <functional>
#include <optional>
#include <utility>

class QMenu;
class QLabel;
class QTreeWidgetItem;
namespace navidog {
struct ConnectionGroup;
struct ConnectionProfile;
class ProfileStore;
class QueryService;
enum class NodeAction;
namespace ui {
class ObjectTree;
class TextInput;
class Button;
} // namespace ui
} // namespace navidog

class NavigationPane final : public QWidget {
    Q_OBJECT

  public:
    using CredentialPrompt = std::function<bool(navidog::ConnectionProfile &)>;
    using NodePredicate = std::function<bool(const navidog::CatalogNode &)>;
    using SaveTableSettings = std::function<bool(const QJsonObject &)>;

    NavigationPane(navidog::QueryService &service,
                   QVector<navidog::ConnectionProfile> &profiles,
                   const navidog::ProfileStore &profileStore,
                   QString metadataPath,
                   QJsonObject &tableSettings,
                   SaveTableSettings saveTableSettings, CredentialPrompt prompt,
                   QWidget *parent = nullptr);

    QTreeWidgetItem *currentItem() const;
    QTreeWidgetItem *connectionItem(const QString &id) const;
    QTreeWidgetItem *catalogItem(const QString &id,
                                 const navidog::CatalogNode &node) const;
    QTreeWidgetItem *findCatalogItem(const QString &id,
                                     const NodePredicate &matches) const;
    QVector<navidog::CatalogNode> namespaces(const QString &id) const;
    void selectItem(QTreeWidgetItem *, bool reveal = false);
    void focusTree();
    QPersistentModelIndex indexFor(QTreeWidgetItem *) const;
    QTreeWidgetItem *itemFor(const QPersistentModelIndex &) const;

    const QVector<navidog::ConnectionGroup> &connectionGroups() const;
    void setConnectionGroups(QVector<navidog::ConnectionGroup> groups);
    static bool isConnectionGroup(const QTreeWidgetItem *);
    QString selectedConnectionGroup() const;
    QString selectedObjectCategory() const;
    void addConnectionGroup();
    void renameSelection();
    void appendGroupAction(QMenu &, navidog::NodeAction, QTreeWidgetItem *);
    static QString tableSettingsKey(const QString &,
                                    const navidog::CatalogNode &);
    void updateTableSettingsForObjectChange(const QString &id,
                                            const navidog::CatalogNode &,
                                            const QString &newName = {});
    void removeTableSettings(const QString &, const QVector<navidog::CatalogNode> &);
    void rebuild();
    int refreshConnectionStates(const QString &disconnectingProfile);
    void updateConnectionTooltip(const navidog::ConnectionProfile &profile);
    void clearConnectionCatalog(QTreeWidgetItem *);
    bool namespaceOpen(QTreeWidgetItem *) const;
    void clearNamespace(QTreeWidgetItem *);

    void openConnection(QTreeWidgetItem *);
    void openDatabase(QTreeWidgetItem *);
    void requestLoad(QTreeWidgetItem *);
    void expandCatalog(QTreeWidgetItem *, bool refresh = false,
                       bool isolated = false);
    void queueRefresh(const QString &profileId);
    void scheduleRefresh();
    void invalidateTable(const QString &id, const navidog::CatalogNode &table);
    void refreshFolder(const QString &id, const navidog::CatalogNode &context,
                       const QString &kind, bool onlyExpanded = false);

    void navigateFolder(const QString &kind);
    void cancelFolderNavigation();
    void showContextMenu(QTreeWidgetItem *, const QPoint &global);
    bool dialogOpen() const { return dialogOpen_; }
    void setDialogOpen(bool open);

  signals:
    void selectionChanged();
    void treeRebuilt();
    void connectionRenamed(const QString &id, const QString &name);
    void objectsRequested(QTreeWidgetItem *item);
    void previewRequested(QTreeWidgetItem *item);
    void tableChildRequested(QTreeWidgetItem *item);
    void sourceRequested(QTreeWidgetItem *item);
    void populateContextMenu(QMenu *menu, QTreeWidgetItem *item);
    void populateBatchContextMenu(QMenu *menu, const QString &profileId, const QVector<navidog::CatalogNode>&);
    void catalogActivityChanged();
    void serverInfoReceived(const QString &id, const QString &version,
                            navidog::DatabaseVersion detected);
    void catalogLoaded(const QString &id, const navidog::CatalogNode &node,
                       const navidog::CatalogResult &result);
    void statusChanged(const QString &message);
    void errorOccurred(const QString &title, const QString &detail);

  private:
    navidog::QueryService &service_;
    QVector<navidog::ConnectionProfile> &profiles_;
    const navidog::ProfileStore &profileStore_;
    QVector<navidog::ConnectionGroup> groups_;
    CredentialPrompt prompt_;
    navidog::ui::ObjectTree *tree_;
    navidog::ui::TextInput *search_;
    navidog::ui::Button *locate_;
    const QString metadataPath_;
    QWidget *cachedResults_;
    QLabel *cachedResultsTitle_;
    navidog::ui::ObjectTree *cachedTree_;
    QTimer cachedSearchDelay_;
    quint64 cachedSearchRevision_ = 0;
    bool cachedSearchRunning_ = false;
    QJsonObject &tableSettings_;
    SaveTableSettings saveTableSettings_;
    QSet<QString> openedDatabases_;
    QString catalogRequest_, refreshRequest_;
    QList<QPersistentModelIndex> pendingLoads_, refreshItems_;
    struct FolderNavigation {
        QPersistentModelIndex parent;
        QString kind;
    };
    std::optional<FolderNavigation> pendingFolder_;
    bool refreshScheduled_ = false;
    bool menuOpen_ = false;
    bool dialogOpen_ = false;
    QList<std::pair<QString, navidog::CatalogResult>> deferredResults_;

    enum class TableGroupAction { Create, Assign, Remove, Rename, Delete };
    void renameConnection(const QString &id);
    void renameConnectionGroup(const QString &id);
    void deleteConnectionGroup(const QString &id);
    void moveConnection(const QString &id, const QString &groupId);
    void reorderNavigation(QTreeWidgetItem *source, QTreeWidgetItem *parent,
                           const QTreeWidgetItem *anchor, bool after);
    void reparentConnection(QTreeWidgetItem *source, QTreeWidgetItem *oldParent,
                            QTreeWidgetItem *parent);
    bool saveConnectionLayout(QVector<navidog::ConnectionProfile> profiles,
                              QVector<navidog::ConnectionGroup> groups);
    bool persistConnectionLayout(QVector<navidog::ConnectionProfile> profiles,
                                 QVector<navidog::ConnectionGroup> groups);
    QString askName(const QString &title, const QString &label,
                    const QString &current = {});
    void manageTableGroup(const QString &id, const navidog::CatalogNode &,
                          TableGroupAction, QString name = {});
    void regroupTables(const QString &id, const navidog::CatalogNode &);
    void activate(QTreeWidgetItem *);
    void applyFilter(const QString &text, bool revealMatches = true);
    void updateLocateAvailability();
    void locateFilteredItem();
    void scheduleCachedSearch();
    void searchCachedObjects();
    void refreshNextItem();
    void applyCatalogResult(const QString &request, const navidog::CatalogResult &);
    bool openNavigationFolder(QTreeWidgetItem *parent, const QString &kind);
    QTreeWidgetItem *addCatalogItem(QTreeWidgetItem *, const QString &,
                                    const navidog::CatalogNode &);
    void reconcileCatalogChildren(QTreeWidgetItem *,
                                  const QVector<navidog::CatalogNode> &,
                                  bool truncated);
    void renderTableFolder(QTreeWidgetItem *,
                           const QVector<navidog::CatalogNode> &,
                           bool truncated = false);
    QString databaseKey(QTreeWidgetItem *) const;
};
