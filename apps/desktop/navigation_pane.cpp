#include "navigation_pane.hpp"
#include "components.hpp"
#include "database_job.hpp"
#include "navidog/metadata_cache.hpp"
#include "navidog/node_actions.hpp"
#include "navidog/operation_timing.hpp"
#include "navidog/profile_store.hpp"
#include "navidog/query_service.hpp"
#include <QCache>
#include <QDrag>
#include <QDropEvent>
#include <QHeaderView>
#include <QInputDialog>
#include <QJsonArray>
#include <QLabel>
#include <QMessageBox>
#include <QMimeData>
#include <QResizeEvent>
#include <QScopeGuard>
#include <QSignalBlocker>
#include <QStyledItemDelegate>
#include <QTimer>
#include <QTreeWidgetItemIterator>
#include <QUuid>
#include <QVBoxLayout>
#include <algorithm>
#include <map>
#include <tuple>

namespace {
enum ItemRole {
    ProfileRole = Qt::UserRole,
    CatalogRole = Qt::UserRole + 2,
    LoadedRole,
    RequestRole,
    GroupRole,
    ConnectedRole,
    PreFilterExpansionRole,
    MutedIconRole,
    NavigationOrderRole
};

QIcon locateIcon(const QColor &color) {
    QPixmap pixmap(32, 32);
    pixmap.fill(Qt::transparent);
    QPainter painter(&pixmap);
    painter.setRenderHint(QPainter::Antialiasing);
    painter.setPen(QPen(color, 2.5, Qt::SolidLine, Qt::RoundCap));
    painter.drawEllipse(QPointF(16, 16), 9, 9);
    painter.drawLine(16, 3, 16, 9);
    painter.drawLine(16, 23, 16, 29);
    painter.drawLine(3, 16, 9, 16);
    painter.drawLine(23, 16, 29, 16);
    painter.setPen(Qt::NoPen);
    painter.setBrush(color);
    painter.drawEllipse(QPointF(16, 16), 2.5, 2.5);
    return QIcon(pixmap);
}

bool isConnectionRow(const QTreeWidgetItem *item) {
    return item && !item->data(0, ProfileRole).toString().isEmpty() &&
           item->data(0, CatalogRole).value<navidog::CatalogNode>().kind ==
               "connection";
}

class NavigationOrderItem final : public QTreeWidgetItem {
  public:
    using QTreeWidgetItem::QTreeWidgetItem;

    bool operator<(const QTreeWidgetItem &other) const override {
        return data(0, NavigationOrderRole).toInt() <
               other.data(0, NavigationOrderRole).toInt();
    }
};

class NavigationTree final : public navidog::ui::ObjectTree {
  public:
    using Reorder =
        std::function<void(QTreeWidgetItem *source, QTreeWidgetItem *parent,
                           const QTreeWidgetItem *anchor, bool after)>;

    NavigationTree(QWidget *parent, std::function<bool()> canReorder,
                   Reorder reorder)
        : ObjectTree(parent), canReorder_(std::move(canReorder)),
          reorder_(std::move(reorder)) {
        setDragDropMode(QAbstractItemView::DragDrop);
        setDefaultDropAction(Qt::MoveAction);
        setDropIndicatorShown(false);
    }

  protected:
    bool viewportEvent(QEvent *event) override {
        if (event->type() == QEvent::Resize)
            header()->setMinimumSectionSize(
                static_cast<QResizeEvent *>(event)->size().width());
        return ObjectTree::viewportEvent(event);
    }

    QStringList mimeTypes() const override { return {mimeType_}; }

#if QT_VERSION >= QT_VERSION_CHECK(6, 0, 0)
    QMimeData *mimeData(const QList<QTreeWidgetItem *> &items) const override {
#else
    QMimeData *mimeData(const QList<QTreeWidgetItem *> items) const override {
#endif
        if (items.size() != 1)
            return nullptr;
        const auto key = dragKey(items.front());
        if (key.isEmpty())
            return nullptr;
        auto *data = new QMimeData;
        data->setData(mimeType_, key);
        return data;
    }

    void startDrag(Qt::DropActions) override {
        auto *source = currentItem();
        if (!source || !canReorder_())
            return;
        auto *data = mimeData({source});
        if (!data)
            return;
        auto *drag = new QDrag(this);
        drag->setMimeData(data);
        auto preview = visualItemRect(source).intersected(viewport()->rect());
        preview.setWidth(qMin(preview.width(), 240));
        if (!preview.isEmpty()) {
            drag->setPixmap(viewport()->grab(preview));
            drag->setHotSpot({8, preview.height() / 2});
        }
        drag->exec(Qt::MoveAction);
        clearDrop();
    }

    void dragEnterEvent(QDragEnterEvent *event) override {
        setMoveAccepted(event, isOurDrag(event));
    }

    void dragMoveEvent(QDragMoveEvent *event) override {
        const auto drop = destination(event);
        auto *marker = drop.anchor;
        dropIntoGroup_ =
            drop.parent && !marker && drop.parent != invisibleRootItem();
        if (dropIntoGroup_)
            marker = drop.parent;
        if (drop.parent && !marker) {
            for (int i = topLevelItemCount() - 1; i >= 0; --i)
                if (!topLevelItem(i)->isHidden()) {
                    marker = topLevelItem(i);
                    break;
                }
        }
        dropIndex_ = marker ? QPersistentModelIndex(indexFromItem(marker))
                            : QPersistentModelIndex{};
        dropAfter_ = drop.after || !drop.anchor;
        const auto y = dropPoint(event).y();
        if (isOurDrag(event) && hasAutoScroll() &&
            (y < autoScrollMargin() ||
             y >= viewport()->height() - autoScrollMargin()))
            startAutoScroll();
        else
            stopAutoScroll();
        setMoveAccepted(event, drop.parent);
        viewport()->update();
    }

    void dragLeaveEvent(QDragLeaveEvent *event) override {
        clearDrop();
        ObjectTree::dragLeaveEvent(event);
    }

    void dropEvent(QDropEvent *event) override {
        const auto drop = destination(event);
        clearDrop();
        setMoveAccepted(event, drop.parent);
        if (drop.parent)
            reorder_(currentItem(), drop.parent, drop.anchor, drop.after);
    }

    void paintEvent(QPaintEvent *event) override {
        ObjectTree::paintEvent(event);
        if (!dropIndex_.isValid())
            return;
        auto *item = itemFromIndex(dropIndex_);
        QPainter painter(viewport());
        const auto highlight = palette().highlight().color();
        painter.setPen(QPen(highlight, 2));
        if (dropIntoGroup_) {
            auto highlightFill = highlight;
            highlightFill.setAlpha(42);
            painter.setBrush(highlightFill);
            painter.drawRect(visualItemRect(item).adjusted(1, 1, -2, -2));
            return;
        }
        const int left = visualItemRect(item).left();
        // The insertion line goes below the expanded row and its visible
        // children.
        while (dropAfter_ && item->isExpanded() && item->childCount()) {
            int last = item->childCount() - 1;
            while (last >= 0 && item->child(last)->isHidden())
                --last;
            if (last < 0)
                break;
            item = item->child(last);
        }
        const auto row = visualItemRect(item);
        const int y = qBound(1, dropAfter_ ? row.bottom() : row.top(),
                             viewport()->height() - 1);
        painter.drawLine(qMax(0, left), y, viewport()->width() - 1, y);
    }

  private:
    struct Destination {
        QTreeWidgetItem *parent = nullptr;
        QTreeWidgetItem *anchor = nullptr;
        bool after = false;
    };
    static constexpr auto mimeType_ = "application/x-navidog-navigation-row";
    const QByteArray dragPrefix_ = QUuid::createUuid().toByteArray();
    std::function<bool()> canReorder_;
    Reorder reorder_;
    QPersistentModelIndex dropIndex_;
    bool dropAfter_ = false;
    bool dropIntoGroup_ = false;

    QByteArray dragKey(const QTreeWidgetItem *item) const {
        if (NavigationPane::isConnectionGroup(item))
            return dragPrefix_ + 'g' +
                   item->data(0, GroupRole).toString().toUtf8();
        if (isConnectionRow(item))
            return dragPrefix_ + 'c' +
                   item->data(0, ProfileRole).toString().toUtf8();
        return {};
    }

    bool isOurDrag(const QDropEvent *event) const {
        const auto key = dragKey(currentItem());
        return !key.isEmpty() && canReorder_() &&
               (event->possibleActions() & Qt::MoveAction) &&
               event->mimeData()->data(mimeType_) == key;
    }

    static QPoint dropPoint(const QDropEvent *event) {
#if QT_VERSION >= QT_VERSION_CHECK(6, 0, 0)
        return event->position().toPoint();
#else
        return event->pos();
#endif
    }

    Destination destination(const QDropEvent *event) const {
        if (!isOurDrag(event))
            return {};
        const auto pos = dropPoint(event);
        const auto *source = currentItem();
        auto *target = itemAt(pos);
        const bool targetGroup = NavigationPane::isConnectionGroup(target);
        auto *root = invisibleRootItem();
        if (target == source)
            return {};
        if (NavigationPane::isConnectionGroup(source)) {
            if (targetGroup)
                return {root, target,
                        pos.y() > visualItemRect(target).center().y()};
            return {};
        }
        if (targetGroup)
            return {target};
        if (isConnectionRow(target))
            return {target->parent() ? target->parent() : root, target,
                    pos.y() > visualItemRect(target).center().y()};
        if (!target && viewport()->rect().contains(pos))
            return {root};
        return {};
    }

    static void setMoveAccepted(QDropEvent *event, bool accepted) {
        if (accepted) {
            event->setDropAction(Qt::MoveAction);
            event->accept();
        } else {
            event->ignore();
        }
    }

    void clearDrop() {
        stopAutoScroll();
        dropIndex_ = QPersistentModelIndex{};
        viewport()->update();
    }
};

class NavigationIconDelegate final : public QStyledItemDelegate {
  public:
    using QStyledItemDelegate::QStyledItemDelegate;

  protected:
    void initStyleOption(QStyleOptionViewItem *option,
                         const QModelIndex &index) const override {
        QStyledItemDelegate::initStyleOption(option, index);
        if (!index.data(MutedIconRole).toBool() || option->icon.isNull())
            return;

        const auto key = option->icon.cacheKey();
        if (const auto *muted = mutedIcons_.object(key)) {
            option->icon = *muted;
            return;
        }

        // Composite the original artwork over the row background at half
        // opacity.
        const auto source =
            option->icon.pixmap(option->icon.availableSizes().value(0));
        if (source.isNull())
            return;
        QPixmap muted(source.size());
        muted.setDevicePixelRatio(source.devicePixelRatio());
        muted.fill(Qt::transparent);
        {
            QPainter painter(&muted);
            painter.setOpacity(0.5);
            painter.drawPixmap(0, 0, source);
        }
        option->icon = QIcon(muted);
        mutedIcons_.insert(key, new QIcon(option->icon));
    }

  private:
    mutable QCache<qint64, QIcon> mutedIcons_;
};

template <class Entry>
bool reorderEntries(QVector<Entry> &entries, const QString &id,
                    const QString &targetId, bool after) {
    const auto from =
        std::find_if(entries.begin(), entries.end(),
                     [&](const auto &entry) { return entry.id == id; });
    const auto target =
        std::find_if(entries.begin(), entries.end(),
                     [&](const auto &entry) { return entry.id == targetId; });
    if (from == entries.end() || target == entries.end() || from == target)
        return false;
    const auto to = target + (after ? 1 : 0);
    if (from == to || from + 1 == to)
        return false;
    if (from < to)
        std::rotate(from, from + 1, to);
    else
        std::rotate(to, from, from + 1);
    return true;
}
} // namespace

NavigationPane::NavigationPane(navidog::QueryService &service,
                               QVector<navidog::ConnectionProfile> &profiles,
                               const navidog::ProfileStore &profileStore,
                               QString metadataPath,
                               QJsonObject &tableSettings,
                               SaveTableSettings saveTableSettings,
                               CredentialPrompt prompt, QWidget *parent)
    : QWidget(parent), service_(service), profiles_(profiles),
      profileStore_(profileStore), prompt_(std::move(prompt)),
      tree_(new NavigationTree(
          this,
          [this] { return !service_.busy() && !dialogOpen_ && !menuOpen_; },
          [this](auto *source, auto *parent, const auto *anchor, bool after) {
              reorderNavigation(source, parent, anchor, after);
          })),
      search_(new navidog::ui::TextInput(this)),
      locate_(new navidog::ui::Button("在完整导航树中定位", "locateNavigationItem", this)),
      metadataPath_(std::move(metadataPath)), cachedResults_(new QWidget(this)),
      cachedResultsTitle_(new QLabel("未打开连接的缓存对象", cachedResults_)),
      cachedTree_(new navidog::ui::ObjectTree(cachedResults_)),
      tableSettings_(tableSettings),
      saveTableSettings_(std::move(saveTableSettings)) {
    setObjectName("navigationPane");
    setMinimumWidth(210);
    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(6, 6, 5, 5);
    layout->setSpacing(4);
    auto *header = new QHBoxLayout;
    auto *title = new QLabel("我的连接");
    title->setObjectName("sectionTitle");
    header->addWidget(title);
    header->addStretch();
    layout->addLayout(header);

    tree_->setObjectName("connectionTree");
    tree_->setHeaderHidden(true);
    tree_->setItemDelegate(new NavigationIconDelegate(tree_));
    tree_->setIndentation(17);
    tree_->setExpandsOnDoubleClick(false);
    tree_->setTextElideMode(Qt::ElideNone);
    tree_->header()->setStretchLastSection(false);
    tree_->header()->setSectionResizeMode(0, QHeaderView::ResizeToContents);
    tree_->setContextMenuPolicy(Qt::CustomContextMenu);
    layout->addWidget(tree_, 1);

    auto *cachedLayout = new QVBoxLayout(cachedResults_);
    cachedLayout->setContentsMargins(0, 0, 0, 0);
    cachedLayout->setSpacing(4);
    cachedLayout->addWidget(cachedResultsTitle_);
    cachedTree_->setObjectName("cachedNavigationResults");
    cachedTree_->setHeaderHidden(true);
    cachedTree_->setUniformRowHeights(true);
    cachedTree_->setIndentation(17);
    cachedTree_->setExpandsOnDoubleClick(false);
    cachedLayout->addWidget(cachedTree_, 1);
    layout->addWidget(cachedResults_, 1);
    cachedResults_->hide();

    cachedSearchDelay_.setSingleShot(true);
    cachedSearchDelay_.setInterval(150);
    connect(&cachedSearchDelay_, &QTimer::timeout, this,
            &NavigationPane::searchCachedObjects);
    connect(&service_, &navidog::QueryService::connectionStatesChanged, this,
            &NavigationPane::scheduleCachedSearch);
    connect(&service_, &navidog::QueryService::catalogCompleted, this,
            [this](const QString &request, const navidog::CatalogResult &result) {
                if (request == catalogRequest_ && !catalogRequest_.isEmpty())
                    applyCatalogResult(request, result);
                else
                    scheduleRefresh();
            });
    connect(cachedTree_, &QTreeWidget::itemDoubleClicked, this,
            [this](QTreeWidgetItem *item) {
                cachedTree_->afterDoubleClick(item, [this](QTreeWidgetItem *hit) {
                    if (!hit->parent()) return;
                    auto *connection = connectionItem(hit->data(0, ProfileRole).toString());
                    if (!connection) return;
                    search_->clear();
                    {
                        const QSignalBlocker block(tree_);
                        for (auto *parent = connection->parent(); parent; parent = parent->parent())
                            parent->setExpanded(true);
                    }
                    selectItem(connection, true);
                    focusTree();
                });
            });

    search_->setObjectName("connectionSearch");
    search_->setPlaceholderText("搜索连接或对象…");
    search_->setToolTip("搜索导航树和未打开连接的本地元数据缓存");
    search_->setClearButtonEnabled(true);
    auto *searchRow = new QHBoxLayout;
    searchRow->setSpacing(2);
    searchRow->addWidget(search_, 1);
    locate_->setIcon(locateIcon(palette().color(QPalette::ButtonText)));
    locate_->setToolButtonStyle(Qt::ToolButtonIconOnly);
    locate_->setToolTip("清除筛选并在完整导航树中定位所选节点");
    locate_->setEnabled(false);
    searchRow->addWidget(locate_);
    auto *collapse = new navidog::ui::Button("收起", "collapseNavigation");
    collapse->setToolTip("折叠全部导航节点");
    searchRow->addWidget(collapse);
    layout->addLayout(searchRow);

    connect(collapse, &QToolButton::clicked, tree_, &QTreeWidget::collapseAll);
    connect(locate_, &QToolButton::clicked, this, &NavigationPane::locateFilteredItem);
    connect(search_, &QLineEdit::textChanged, this,
            [this](const QString &text) {
                applyFilter(text);
                scheduleCachedSearch();
            });
    connect(tree_, &QTreeWidget::itemSelectionChanged, this,
            &NavigationPane::selectionChanged);
    connect(tree_, &QTreeWidget::itemSelectionChanged, this,
            &NavigationPane::updateLocateAvailability);
    connect(tree_, &QTreeWidget::currentItemChanged, this,
            &NavigationPane::cancelFolderNavigation);
    connect(tree_, &QTreeWidget::currentItemChanged, this,
            &NavigationPane::updateLocateAvailability);
    connect(tree_, &QTreeWidget::itemExpanded, this,
            &NavigationPane::requestLoad);
    connect(tree_, &QTreeWidget::itemCollapsed, this,
            [this](QTreeWidgetItem *item) {
                if (pendingFolder_ &&
                    pendingFolder_->parent == tree_->indexFromItem(item))
                    cancelFolderNavigation();
            });
    connect(tree_, &QTreeWidget::itemDoubleClicked, this,
            [this](QTreeWidgetItem *item) {
                tree_->afterDoubleClick(item,
                                        [this](QTreeWidgetItem *activated) {
                                            activate(activated);
                                        });
            });
    connect(tree_, &QWidget::customContextMenuRequested, this,
            [this](const QPoint &point) {
                showContextMenu(tree_->itemAt(point),
                                tree_->viewport()->mapToGlobal(point));
            });
}

void NavigationPane::activate(QTreeWidgetItem *item) {
    if (!item->data(0, GroupRole).toString().isEmpty()) {
        item->setExpanded(!item->isExpanded());
        return;
    }
    const auto data = item->data(0, CatalogRole);
    if (!data.isValid())
        return;
    const auto node = data.value<navidog::CatalogNode>();
    if (node.kind == "table" || node.kind == "view" || node.kind == "redisKey")
        emit previewRequested(item);
    else if (QStringList{"column", "index", "foreignKey", "check", "trigger",
                         "primaryKey"}
                 .contains(node.kind))
        emit tableChildRequested(item);
    else if (node.kind == "function" || node.kind == "procedure")
        emit sourceRequested(item);
    else if (node.expandable) {
        const bool expanding = !item->isExpanded();
        item->setExpanded(expanding);
        if (expanding)
            emit objectsRequested(item);
    }
}

void NavigationPane::showContextMenu(QTreeWidgetItem *item,
                                     const QPoint &global) {
    if (menuOpen_)
        return;
    menuOpen_ = true;
    const auto menuClosed = qScopeGuard([this] {
        menuOpen_ = false;
        const auto results = std::exchange(deferredResults_, {});
        for (const auto &[request, result] : results)
            applyCatalogResult(request, result);
        scheduleRefresh();
    });
    tree_->setCurrentItem(item);
    emit selectionChanged();
    navidog::ui::ContextMenu menu(this);
    menu.setObjectName("nodeContextMenu");
    emit populateContextMenu(&menu, item);
    if (!menu.isEmpty())
        menu.execAt(global, tree_);
}

void NavigationPane::appendGroupAction(QMenu &menu, navidog::NodeAction action,
                                       QTreeWidgetItem *item) {
    const bool enabled = !service_.busy() && !dialogOpen_;
    using A = navidog::NodeAction;
    if (action == A::NewGroup) {
        menu.addAction("新建分组…", this, &NavigationPane::addConnectionGroup)
            ->setEnabled(enabled);
        return;
    }
    if (!item)
        return;

    const auto id = item->data(0, ProfileRole).toString();
    const auto groupId = item->data(0, GroupRole).toString();
    const auto node = item->data(0, CatalogRole).value<navidog::CatalogNode>();
    switch (action) {
    case A::RenameGroup:
        menu.addAction("重命名分组…", this,
                       [this, groupId] { renameConnectionGroup(groupId); })
            ->setEnabled(enabled);
        break;
    case A::DeleteGroup:
        menu.addAction("删除分组（保留连接）", this,
                       [this, groupId] { deleteConnectionGroup(groupId); })
            ->setEnabled(enabled);
        break;
    case A::MoveConnection: {
        const auto profile =
            std::find_if(profiles_.cbegin(), profiles_.cend(),
                         [&](const auto &p) { return p.id == id; });
        if (profile == profiles_.cend())
            return;
        auto *manage = menu.addMenu("管理组");
        manage->setEnabled(enabled);
        manage->addAction("新建组…", this, &NavigationPane::addConnectionGroup);
        auto *move = manage->addMenu("移动到分组");
        for (const auto &group : groups_) {
            auto *target = move->addAction(group.name, this,
                                           [this, id, targetId = group.id] {
                                               moveConnection(id, targetId);
                                           });
            target->setCheckable(true);
            target->setChecked(group.id == profile->groupId);
        }
        move->setEnabled(!groups_.isEmpty());
        manage
            ->addAction("从组中排除", this,
                        [this, id] { moveConnection(id, {}); })
            ->setEnabled(!profile->groupId.isEmpty());
        break;
    }
    case A::ManageTableGroup: {
        auto *manage = menu.addMenu("管理组");
        manage->setEnabled(enabled);
        manage->addAction("新建组…", this, [this, id, node] {
            manageTableGroup(id, node, TableGroupAction::Create);
        });
        auto *move = manage->addMenu("添加到组");
        const auto settings =
            tableSettings_.value(tableSettingsKey(id, node)).toObject();
        const auto names = settings["groups"].toArray();
        for (auto name : names) {
            auto label = name.toString();
            move->addAction(label, this, [this, id, node, label] {
                manageTableGroup(id, node, TableGroupAction::Assign, label);
            });
        }
        move->setEnabled(node.kind == "table" && !names.isEmpty());
        manage
            ->addAction("从组中排除", this,
                        [this, id, node] {
                            manageTableGroup(id, node,
                                             TableGroupAction::Remove);
                        })
            ->setEnabled(node.kind == "table" &&
                         settings["members"].toObject().contains(node.object));
        break;
    }
    case A::RenameTableGroup:
        menu.addAction("重命名组…", this,
                       [this, id, node] {
                           manageTableGroup(id, node, TableGroupAction::Rename);
                       })
            ->setEnabled(enabled);
        break;
    case A::DeleteTableGroup:
        menu.addAction("删除组（保留表）", this,
                       [this, id, node] {
                           manageTableGroup(id, node, TableGroupAction::Delete);
                       })
            ->setEnabled(enabled);
        break;
    default:
        break;
    }
}

const QVector<navidog::ConnectionGroup> &
NavigationPane::connectionGroups() const {
    return groups_;
}

void NavigationPane::setConnectionGroups(
    QVector<navidog::ConnectionGroup> groups) {
    groups_ = std::move(groups);
}

bool NavigationPane::isConnectionGroup(const QTreeWidgetItem *item) {
    return item && !item->data(0, GroupRole).toString().isEmpty();
}

QString NavigationPane::selectedConnectionGroup() const {
    auto *item = currentItem();
    if (!item || !item->isSelected())
        return {};
    for (; item; item = item->parent())
        if (isConnectionGroup(item))
            return item->data(0, GroupRole).toString();
    return {};
}

QString NavigationPane::askName(const QString &title, const QString &label,
                                const QString &current) {
    if (service_.busy() || dialogOpen_)
        return {};
    setDialogOpen(true);
    const auto dialogClosed = qScopeGuard([this] { setDialogOpen(false); });
    bool accepted = false;
    const auto name =
        QInputDialog::getText(this, title, label, QLineEdit::Normal, current,
                              &accepted)
            .trimmed();
    return accepted ? name : QString{};
}

bool NavigationPane::saveConnectionLayout(
    QVector<navidog::ConnectionProfile> profiles,
    QVector<navidog::ConnectionGroup> groups) {
    if (!persistConnectionLayout(std::move(profiles), std::move(groups)))
        return false;
    rebuild();
    return true;
}

bool NavigationPane::persistConnectionLayout(
    QVector<navidog::ConnectionProfile> profiles,
    QVector<navidog::ConnectionGroup> groups) {
    navidog::WorkspaceProfiles workspace{std::move(profiles),
                                         std::move(groups)};
    try {
        profileStore_.saveWorkspace(workspace);
    } catch (const std::exception &e) {
        QMessageBox::warning(this, "无法保存", QString::fromUtf8(e.what()));
        return false;
    }
    profiles_ = std::move(workspace.connections);
    groups_ = std::move(workspace.groups);
    return true;
}

void NavigationPane::reorderNavigation(QTreeWidgetItem *source,
                                       QTreeWidgetItem *parent,
                                       const QTreeWidgetItem *anchor,
                                       bool after) {
    auto profiles = profiles_;
    auto groups = groups_;
    auto *oldParent =
        source->parent() ? source->parent() : tree_->invisibleRootItem();
    const bool moved = oldParent != parent;
    bool changed = false;
    if (isConnectionGroup(source)) {
        changed = reorderEntries(groups, source->data(0, GroupRole).toString(),
                                 anchor->data(0, GroupRole).toString(), after);
    } else {
        const auto id = source->data(0, ProfileRole).toString();
        const auto groupId = parent->data(0, GroupRole).toString();
        const auto profile =
            std::find_if(profiles.begin(), profiles.end(),
                         [&](const auto &entry) { return entry.id == id; });
        if (profile == profiles.end())
            return;
        changed = profile->groupId != groupId;
        profile->groupId = groupId;
        if (anchor) {
            changed = reorderEntries(profiles, id,
                                     anchor->data(0, ProfileRole).toString(),
                                     after) ||
                      changed;
        } else {
            const auto last = std::find_if(
                profiles.crbegin(), profiles.crend(), [&](const auto &entry) {
                    return entry.groupId == groupId && entry.id != id;
                });
            if (last != profiles.crend()) {
                const auto lastId = last->id;
                changed = reorderEntries(profiles, id, lastId, true) || changed;
            }
        }
    }
    if (!changed ||
        !persistConnectionLayout(std::move(profiles), std::move(groups)))
        return;

    const QSignalBlocker block(tree_);
    if (moved)
        reparentConnection(source, oldParent, parent);
    // A sort of the affected siblings preserves existing nodes and indexes.
    QHash<QString, int> groupOrder, profileOrder;
    int rank = 0;
    for (const auto &group : groups_)
        groupOrder.insert(group.id, rank++);
    for (const auto &profile : profiles_)
        profileOrder.insert(profile.id, rank++);
    for (int i = 0; i < parent->childCount(); ++i) {
        auto *row = parent->child(i);
        const auto group = row->data(0, GroupRole).toString();
        row->setData(0, NavigationOrderRole,
                     group.isEmpty() ? profileOrder.value(
                                           row->data(0, ProfileRole).toString())
                                     : groupOrder.value(group));
    }
    parent->sortChildren(0, Qt::AscendingOrder);
    if (moved) {
        applyFilter(search_->text(), false);
        if (!source->isHidden()) {
            tree_->setCurrentItem(source);
            tree_->scrollToItem(source);
        }
        emit selectionChanged();
    }
}

void NavigationPane::reparentConnection(QTreeWidgetItem *source,
                                        QTreeWidgetItem *oldParent,
                                        QTreeWidgetItem *parent) {
    QList<QTreeWidgetItem *> subtree{source}, expanded;
    for (int i = 0; i < subtree.size(); ++i) {
        auto *item = subtree.at(i);
        if (item->isExpanded())
            expanded.append(item);
        for (int child = 0; child < item->childCount(); ++child)
            subtree.append(item->child(child));
    }

    // Reparenting changes the model path. Recover queued work using the same
    // items.
    auto capture = [this](const QList<QPersistentModelIndex> &indexes) {
        QList<QTreeWidgetItem *> items;
        for (const auto &index : indexes)
            if (auto *item = itemFor(index))
                items.append(item);
        return items;
    };
    const auto pendingItems = capture(pendingLoads_);
    const auto refreshItems = capture(refreshItems_);
    auto *folderParent =
        pendingFolder_ ? itemFor(pendingFolder_->parent) : nullptr;

    parent->addChild(oldParent->takeChild(oldParent->indexOfChild(source)));
    for (auto *item : expanded)
        item->setExpanded(true);
    if (parent != tree_->invisibleRootItem())
        parent->setExpanded(true);
    auto restore = [this](const QList<QTreeWidgetItem *> &items) {
        QList<QPersistentModelIndex> indexes;
        for (auto *item : items)
            indexes.append(indexFor(item));
        return indexes;
    };
    pendingLoads_ = restore(pendingItems);
    refreshItems_ = restore(refreshItems);
    if (folderParent)
        pendingFolder_->parent = indexFor(folderParent);
}

void NavigationPane::addConnectionGroup() {
    const auto name = askName("新建连接分组", "分组名称");
    if (name.isEmpty())
        return;
    auto groups = groups_;
    groups.push_back({QUuid::createUuid().toString(QUuid::Id128), name});
    saveConnectionLayout(profiles_, std::move(groups));
}

void NavigationPane::renameSelection() {
    const auto *item = currentItem();
    if (isConnectionGroup(item))
        renameConnectionGroup(item->data(0, GroupRole).toString());
    else if (item &&
             item->data(0, CatalogRole).value<navidog::CatalogNode>().kind ==
                 "connection")
        renameConnection(item->data(0, ProfileRole).toString());
}

void NavigationPane::renameConnectionGroup(const QString &id) {
    const auto group = std::find_if(groups_.cbegin(), groups_.cend(),
                                    [&](const auto &g) { return g.id == id; });
    if (group == groups_.cend())
        return;
    const auto name = askName("重命名", "名称", group->name);
    if (name.isEmpty())
        return;
    auto groups = groups_;
    for (auto &g : groups)
        if (g.id == id)
            g.name = name;
    saveConnectionLayout(profiles_, std::move(groups));
}

void NavigationPane::renameConnection(const QString &id) {
    const auto profile =
        std::find_if(profiles_.cbegin(), profiles_.cend(),
                     [&](const auto &p) { return p.id == id; });
    if (profile == profiles_.cend())
        return;
    const auto name = askName("重命名", "名称", profile->name);
    if (name.isEmpty())
        return;
    if (std::any_of(profiles_.cbegin(), profiles_.cend(), [&](const auto &p) {
            return p.id != id && p.name.compare(name, Qt::CaseInsensitive) == 0;
        })) {
        QMessageBox::information(this, "连接名称重复",
                                 "已有同名连接，请使用其他名称。");
        return;
    }
    auto profiles = profiles_;
    for (auto &p : profiles)
        if (p.id == id)
            p.name = name;
    if (saveConnectionLayout(std::move(profiles), groups_))
        emit connectionRenamed(id, name);
}

void NavigationPane::deleteConnectionGroup(const QString &id) {
    if (service_.busy() || dialogOpen_)
        return;
    auto groups = groups_;
    const auto removed =
        std::remove_if(groups.begin(), groups.end(),
                       [&](const auto &g) { return g.id == id; });
    if (removed == groups.end())
        return;
    groups.erase(removed, groups.end());
    auto profiles = profiles_;
    for (auto &p : profiles)
        if (p.groupId == id)
            p.groupId.clear();
    if (saveConnectionLayout(std::move(profiles), std::move(groups)))
        emit statusChanged("分组已删除，连接已移至根目录");
}

void NavigationPane::moveConnection(const QString &id, const QString &groupId) {
    if (service_.busy() || dialogOpen_)
        return;
    auto profiles = profiles_;
    const auto profile =
        std::find_if(profiles.begin(), profiles.end(),
                     [&](const auto &p) { return p.id == id; });
    if (profile == profiles.end() || profile->groupId == groupId)
        return;
    profile->groupId = groupId;
    if (saveConnectionLayout(std::move(profiles), groups_))
        emit statusChanged("连接已移动");
}

QTreeWidgetItem *NavigationPane::currentItem() const {
    return tree_->currentItem();
}

QString NavigationPane::selectedObjectCategory() const {
    static const QStringList categories{"tables", "views", "functions", "procedures"};
    for (auto *item = currentItem(); item; item = item->parent()) {
        const auto kind = item->data(0, CatalogRole).value<navidog::CatalogNode>().kind;
        if (categories.contains(kind))
            return kind;
    }
    return {};
}

void NavigationPane::selectItem(QTreeWidgetItem *item, bool reveal) {
    tree_->setCurrentItem(item);
    if (item && reveal)
        tree_->scrollToItem(item);
}

void NavigationPane::focusTree() { tree_->setFocus(Qt::OtherFocusReason); }

QPersistentModelIndex NavigationPane::indexFor(QTreeWidgetItem *item) const {
    return QPersistentModelIndex(tree_->indexFromItem(item));
}

QTreeWidgetItem *
NavigationPane::itemFor(const QPersistentModelIndex &index) const {
    return index.isValid() ? tree_->itemFromIndex(index) : nullptr;
}

void NavigationPane::setDialogOpen(bool open) {
    dialogOpen_ = open;
    if (!open)
        scheduleRefresh();
}

void NavigationPane::cancelFolderNavigation() { pendingFolder_.reset(); }

QTreeWidgetItem *NavigationPane::connectionItem(const QString &id) const {
    return findCatalogItem(
        id, [](const auto &node) { return node.kind == "connection"; });
}

QTreeWidgetItem *
NavigationPane::findCatalogItem(const QString &id,
                                const NodePredicate &matches) const {
    for (QTreeWidgetItemIterator it(tree_); *it; ++it) {
        auto *item = *it;
        const auto data = item->data(0, CatalogRole);
        if (item->data(0, ProfileRole).toString() == id && data.isValid() &&
            matches(data.value<navidog::CatalogNode>()))
            return item;
    }
    return nullptr;
}

QVector<navidog::CatalogNode>
NavigationPane::namespaces(const QString &id) const {
    QVector<navidog::CatalogNode> result;
    const auto *connection = connectionItem(id);
    if (!connection)
        return result;
    for (int i = 0; i < connection->childCount(); ++i) {
        const auto *child = connection->child(i);
        const auto node =
            child->data(0, CatalogRole).value<navidog::CatalogNode>();
        if (node.kind == "schema") {
            result << node;
        } else if (node.kind == "database") {
            result << node;
            for (int j = 0; j < child->childCount(); ++j) {
                const auto schema = child->child(j)
                                        ->data(0, CatalogRole)
                                        .value<navidog::CatalogNode>();
                if (schema.kind == "schema")
                    result << schema;
            }
        }
    }
    return result;
}

bool NavigationPane::namespaceOpen(QTreeWidgetItem *item) const {
    return item && openedDatabases_.contains(databaseKey(item));
}

void NavigationPane::updateConnectionTooltip(
    const navidog::ConnectionProfile &profile) {
    if (auto *item = connectionItem(profile.id))
        item->setToolTip(
            0, profile.name + " · " +
                   navidog::databaseKindName(profile.driver) + " · " +
                   (service_.isConnected(profile.id) ? "已打开" : "已关闭") +
                   "\n" + profile.serverVersion + "\n" +
                   (profile.driver == navidog::DatabaseKind::SQLite
                        ? profile.database
                        : profile.host + ":" + QString::number(profile.port)));
}

void NavigationPane::invalidateTable(const QString &id,
                                     const navidog::CatalogNode &table) {
    for (QTreeWidgetItemIterator it(tree_); *it; ++it) {
        auto *item = *it;
        const auto node =
            item->data(0, CatalogRole).value<navidog::CatalogNode>();
        if (item->data(0, ProfileRole).toString() == id &&
            node.database == table.database && node.schema == table.schema &&
            (node.kind == "tables" || node.object == table.object))
            item->setData(0, LoadedRole, false);
    }
}

void NavigationPane::refreshFolder(const QString &id,
                                   const navidog::CatalogNode &context,
                                   const QString &kind, bool onlyExpanded) {
    auto *item = findCatalogItem(id, [&](const auto &node) {
        return node.kind == kind && node.database == context.database &&
               node.schema == context.schema;
    });
    if (item && (!onlyExpanded || item->isExpanded()))
        expandCatalog(item, true, true);
}

void NavigationPane::rebuild() {
    const QSignalBlocker blocker(tree_);
    auto *selected = tree_->currentItem();
    const auto selectedId =
        selected ? selected->data(0, ProfileRole).toString() : QString{};
    const auto selectedGroupId =
        selected ? selected->data(0, GroupRole).toString() : QString{};
    QHash<QString, QTreeWidgetItem *> items, groupItems;
    QHash<QString, bool> expandedGroups;
    for (QTreeWidgetItemIterator it(tree_); *it; ++it) {
        auto *item = *it;
        const auto id = item->data(0, ProfileRole).toString();
        if (!id.isEmpty() &&
            item->data(0, CatalogRole).value<navidog::CatalogNode>().kind ==
                "connection")
            items[id] = item;
        const auto group = item->data(0, GroupRole).toString();
        if (!group.isEmpty())
            expandedGroups[group] = item->isExpanded();
    }
    for (auto *item : items) {
        if (item->parent())
            item->parent()->takeChild(item->parent()->indexOfChild(item));
        else
            tree_->takeTopLevelItem(tree_->indexOfTopLevelItem(item));
    }
    tree_->clear();
    for (const auto &group : groups_) {
        auto *item = new NavigationOrderItem(tree_, {group.name});
        item->setData(0, GroupRole, group.id);
        item->setIcon(0, style()->standardIcon(QStyle::SP_DirIcon));
        item->setExpanded(expandedGroups.value(group.id, true));
        groupItems[group.id] = item;
    }
    for (const auto &profile : profiles_) {
        auto *parent = groupItems.value(profile.groupId);
        auto *item = items.take(profile.id);
        navidog::CatalogNode node;
        node.name = profile.name;
        if (item) {
            if (parent)
                parent->addChild(item);
            else
                tree_->addTopLevelItem(item);
            item->setText(0, profile.name);
            item->setData(0, CatalogRole, QVariant::fromValue(node));
        } else
            item = addCatalogItem(parent, profile.id, node);
        item->setIcon(0, navidog::ui::databaseIcon(profile.driver));
    }
    qDeleteAll(items);
    if (!selectedId.isEmpty() &&
        std::any_of(profiles_.begin(), profiles_.end(),
                    [&](const auto &p) { return p.id == selectedId; }))
        tree_->setCurrentItem(selected);
    else if (!selectedGroupId.isEmpty())
        tree_->setCurrentItem(groupItems.value(selectedGroupId));
    cancelFolderNavigation();
    applyFilter(search_->text());
    scheduleCachedSearch();
    emit treeRebuilt();
}
void NavigationPane::clearConnectionCatalog(QTreeWidgetItem *item) {
    const auto id = item->data(0, ProfileRole).toString();
    const QSignalBlocker blocker(tree_);
    item->setExpanded(false);
    qDeleteAll(item->takeChildren());
    item->setData(0, LoadedRole, false);
    item->setChildIndicatorPolicy(QTreeWidgetItem::ShowIndicator);
    const QPersistentModelIndex index(tree_->indexFromItem(item));
    pendingLoads_.removeAll(index);
    refreshItems_.removeAll(index);
    if (pendingFolder_ && !pendingFolder_->parent.isValid())
        cancelFolderNavigation();
    item->setData(0, RequestRole, QUuid::createUuid().toString());
    for (auto it = openedDatabases_.begin(); it != openedDatabases_.end();)
        if (it->startsWith(id + QChar(0x1f)))
            it = openedDatabases_.erase(it);
        else
            ++it;
}
int NavigationPane::refreshConnectionStates(
    const QString &disconnectingProfile) {
    int opened = 0;
    for (const auto &p : profiles_) {
        const bool connected = service_.isConnected(p.id);
        if (connected)
            ++opened;
        if (auto *item = connectionItem(p.id)) {
            if (!connected && item->data(0, ConnectedRole).toBool() &&
                item->data(0, LoadedRole).toBool() &&
                disconnectingProfile != p.id)
                clearConnectionCatalog(item);
            item->setData(0, ConnectedRole, connected);
            item->setData(0, MutedIconRole, !connected);
            auto font = item->font(0);
            font.setBold(connected);
            item->setFont(0, font);
            item->setToolTip(
                0, p.name + " · " + navidog::databaseKindName(p.driver) +
                       " · " + (connected ? "已打开" : "已关闭") + "\n" +
                       (p.driver == navidog::DatabaseKind::SQLite
                            ? p.database
                            : p.host + ":" + QString::number(p.port)) +
                       (!p.serverVersion.isEmpty() ? "\n" + p.serverVersion
                                                   : QString{}));
        }
    }
    return opened;
}
void NavigationPane::openConnection(QTreeWidgetItem *item) {
    if (!item ||
        item->data(0, CatalogRole).value<navidog::CatalogNode>().kind !=
            "connection")
        return;
    item->setExpanded(true);
    requestLoad(item);
}
QTreeWidgetItem *
NavigationPane::addCatalogItem(QTreeWidgetItem *parent, const QString &id,
                               const navidog::CatalogNode &node) {
    auto *item = node.kind == "connection"
                     ? (parent ? new NavigationOrderItem(parent, {node.name})
                               : new NavigationOrderItem(tree_, {node.name}))
                     : (parent ? new QTreeWidgetItem(parent, {node.name})
                               : new QTreeWidgetItem(tree_, {node.name}));
    if (node.kind != "connection")
        item->setFlags(item->flags() & ~Qt::ItemIsDragEnabled);
    item->setData(0, ProfileRole, id);
    item->setData(0, CatalogRole, QVariant::fromValue(node));
    item->setData(0, RequestRole, QUuid::createUuid().toString());
    if ((node.kind == "function" || node.kind == "procedure") &&
        !node.key.isEmpty())
        item->setText(0, node.name + "(" + node.detail + ")");
    if (node.kind == "column" && !node.detail.isEmpty())
        item->setText(0, node.name + ' ' + node.detail);
    item->setToolTip(0, node.detail);
    if (node.expandable)
        item->setChildIndicatorPolicy(QTreeWidgetItem::ShowIndicator);
    if (node.kind == "database" || node.kind == "schema")
        item->setData(0, MutedIconRole, true);
    if (node.kind == "column")
        return item;
    if (QStringList{"connection", "database",  "schema",     "table",
                    "tables",     "view",      "views", "redisKeys", "redisKey",
                    "index",      "indexes",   "foreignKey", "foreignKeys",
                    "check",      "checks",    "trigger",    "triggers",
                    "primaryKey", "functions", "function",   "procedures",
                    "procedure"}
            .contains(node.kind))
        item->setIcon(0, navidog::ui::objectIcon(node.kind));
    else
        item->setIcon(0, style()->standardIcon(node.expandable
                                                   ? QStyle::SP_DirIcon
                                                   : QStyle::SP_FileIcon));
    return item;
}
void NavigationPane::expandCatalog(QTreeWidgetItem *item, bool refresh,
                                   bool isolated) {
    if (!item || dialogOpen_ || !item->data(0, GroupRole).toString().isEmpty())
        return;
    auto node = item->data(0, CatalogRole).value<navidog::CatalogNode>();
    if (node.kind == "tableGroup") {
        if (refresh)
            expandCatalog(item->parent(), true, isolated);
        return;
    }
    if (!node.expandable || (!refresh && item->data(0, LoadedRole).toBool()))
        return;
    if (service_.operationBusy() || service_.catalogBusy()) {
        if (catalogRequest_ != item->data(0, RequestRole).toString())
            emit statusChanged("正在处理其他请求，请稍后重试");
        return;
    }
    const auto id = item->data(0, ProfileRole).toString();
    auto it = std::find_if(profiles_.begin(), profiles_.end(),
                           [&](auto &p) { return p.id == id; });
    if (it == profiles_.end())
        return;
    const QPersistentModelIndex index(tree_->indexFromItem(item));
    setDialogOpen(true);
    const auto dialogClosed = qScopeGuard([this] { setDialogOpen(false); });
    if (!prompt_(*it) || !index.isValid()) {
        if (pendingFolder_ && pendingFolder_->parent == index)
            pendingFolder_.reset();
        return;
    }
    if (service_.catalog(*it, node, item->data(0, RequestRole).toString(),
                         isolated)) {
        catalogRequest_ = item->data(0, RequestRole).toString();
        emit catalogActivityChanged();
        emit statusChanged("正在读取数据库对象…");
    } else if (!refresh && item->isExpanded())
        requestLoad(item);
}
QString NavigationPane::tableSettingsKey(const QString &id,
                                         const navidog::CatalogNode &n) {
    return id + QChar(0x1f) + n.database + QChar(0x1f) + n.schema;
}
QString NavigationPane::databaseKey(QTreeWidgetItem *item) const {
    return tableSettingsKey(
        item->data(0, ProfileRole).toString(),
        item->data(0, CatalogRole).value<navidog::CatalogNode>());
}
void NavigationPane::openDatabase(QTreeWidgetItem *item) {
    if (!item)
        return;
    auto n = item->data(0, CatalogRole).value<navidog::CatalogNode>();
    if (n.kind != "database" && n.kind != "schema")
        return;
    item->setExpanded(true);
    requestLoad(item);
}
void NavigationPane::clearNamespace(QTreeWidgetItem *item) {
    const auto n = item->data(0, CatalogRole).value<navidog::CatalogNode>();
    const auto key = databaseKey(item);
    for (auto it = openedDatabases_.begin(); it != openedDatabases_.end();)
        if (*it == key ||
            (n.kind == "database" &&
             it->startsWith(item->data(0, ProfileRole).toString() +
                            QChar(0x1f) + n.database + QChar(0x1f))))
            it = openedDatabases_.erase(it);
        else
            ++it;
    item->setExpanded(false);
    qDeleteAll(item->takeChildren());
    item->setData(0, LoadedRole, false);
    item->setChildIndicatorPolicy(QTreeWidgetItem::ShowIndicator);
    item->setData(0, MutedIconRole, true);
    auto font = item->font(0);
    font.setBold(false);
    item->setFont(0, font);
    item->setToolTip(0, "已关闭");
}

namespace {
bool same(const navidog::CatalogNode &a, const navidog::CatalogNode &b) {
    return a.kind == b.kind && a.database == b.database &&
           a.schema == b.schema && a.object == b.object && a.key == b.key;
}
auto childIdentity(const navidog::CatalogNode &node) {
    return std::make_tuple(node.kind, node.database, node.schema, node.object,
                           node.key, node.name);
}
} // namespace
void NavigationPane::reconcileCatalogChildren(
    QTreeWidgetItem *parent, const QVector<navidog::CatalogNode> &nodes,
    bool truncated) {

    using Identity = decltype(childIdentity(navidog::CatalogNode{}));
    std::multimap<Identity, QTreeWidgetItem *> current;
    for (int i = parent->childCount() - 1; i >= 0; --i) {
        auto *child = parent->child(i);
        const auto data = child->data(0, CatalogRole);
        if (data.isValid())
            current.emplace(childIdentity(data.value<navidog::CatalogNode>()),
                            child);
        else
            delete child;
    }
    const auto id = parent->data(0, ProfileRole).toString();
    for (const auto &node : nodes) {
        const auto entry = current.find(childIdentity(node));
        if (entry == current.end()) {
            addCatalogItem(parent, id, node);
            continue;
        }
        auto *child = entry->second;
        const auto previous =
            child->data(0, CatalogRole).value<navidog::CatalogNode>();
        if (previous.detail != node.detail) {
            child->setData(0, CatalogRole, QVariant::fromValue(node));
            child->setToolTip(0, node.detail);
            if (node.kind == "column")
                child->setText(0, node.name + (node.detail.isEmpty()
                                                   ? QString{}
                                                   : ' ' + node.detail));
            else if ((node.kind == "function" || node.kind == "procedure") &&
                     !node.key.isEmpty())
                child->setText(0, node.name + "(" + node.detail + ")");
        }
        current.erase(entry);
    }
    if (truncated)
        return;
    for (const auto &entry : current) {
        auto *child = entry.second;
        std::function<void(QTreeWidgetItem *)> forget =
            [&](QTreeWidgetItem *item) {
                const auto node =
                    item->data(0, CatalogRole).value<navidog::CatalogNode>();
                if (node.kind == "database" || node.kind == "schema")
                    openedDatabases_.remove(databaseKey(item));
                for (int i = 0; i < item->childCount(); ++i)
                    forget(item->child(i));
            };
        forget(child);
        delete child;
    }
}

void NavigationPane::queueRefresh(const QString &profileId) {
    const auto profile =
        std::find_if(profiles_.cbegin(), profiles_.cend(),
                     [&](const auto &p) { return p.id == profileId; });
    if (profile == profiles_.cend())
        return;
    const bool schemaList =
        profile->driver == navidog::DatabaseKind::PostgreSQL ||
        profile->driver == navidog::DatabaseKind::SQLServer;
    const QStringList folders{
        "tables",  "views",       "functions", "procedures", "columns",
        "indexes", "foreignKeys", "checks",    "triggers",   "parameters"};
    for (QTreeWidgetItemIterator it(tree_); *it; ++it) {
        auto *item = *it;
        if (item->data(0, ProfileRole).toString() != profileId ||
            !item->data(0, LoadedRole).toBool())
            continue;
        const auto node =
            item->data(0, CatalogRole).value<navidog::CatalogNode>();
        if (!folders.contains(node.kind) &&
            !(node.kind == "connection" &&
              profile->driver != navidog::DatabaseKind::SQLite) &&
            !(node.kind == "database" && schemaList))
            continue;
        const QPersistentModelIndex index(tree_->indexFromItem(item));
        if (!refreshItems_.contains(index))
            refreshItems_ << index;
    }
    scheduleRefresh();
}

void NavigationPane::requestLoad(QTreeWidgetItem *item) {
    if (!item || !item->data(0, CatalogRole).isValid() ||
        !item->data(0, CatalogRole).value<navidog::CatalogNode>().expandable ||
        item->data(0, LoadedRole).toBool())
        return;
    const QPersistentModelIndex index(tree_->indexFromItem(item));
    if (!pendingLoads_.contains(index))
        pendingLoads_ << index;
    scheduleRefresh();
}

void NavigationPane::scheduleRefresh() {
    if (refreshScheduled_ || menuOpen_ || dialogOpen_ ||
        service_.operationBusy() || service_.catalogBusy() ||
        !refreshRequest_.isEmpty() ||
        (pendingLoads_.isEmpty() && refreshItems_.isEmpty()))
        return;
    refreshScheduled_ = true;
    QTimer::singleShot(pendingLoads_.isEmpty() ? 50 : 0, this, [this] {
        refreshScheduled_ = false;
        refreshNextItem();
    });
}

void NavigationPane::refreshNextItem() {
    if (service_.operationBusy() || service_.catalogBusy() || menuOpen_ ||
        dialogOpen_ || !refreshRequest_.isEmpty())
        return;
    // Keep explicit expansions queued while the single catalog worker is busy.
    while (!pendingLoads_.isEmpty()) {
        const auto index = pendingLoads_.takeFirst();
        if (!index.isValid())
            continue;
        auto *item = tree_->itemFromIndex(index);
        if (!item->isExpanded())
            continue;
        expandCatalog(item);
        if (service_.operationBusy() || service_.catalogBusy())
            return;
    }
    while (!refreshItems_.isEmpty()) {
        const auto index = refreshItems_.takeFirst();
        if (!index.isValid())
            continue;
        auto *item = tree_->itemFromIndex(index);
        const auto id = item->data(0, ProfileRole).toString();
        const auto profile =
            std::find_if(profiles_.cbegin(), profiles_.cend(),
                         [&](const auto &p) { return p.id == id; });
        if (profile == profiles_.cend())
            continue;
        const auto request = item->data(0, RequestRole).toString();
        auto node = item->data(0, CatalogRole).value<navidog::CatalogNode>();
        auto connection = *profile;
        if (node.kind == "connection") {
            node.database.clear();
            if (navidog::isMySqlFamily(connection.driver))
                connection.database.clear();
            else if (connection.driver == navidog::DatabaseKind::PostgreSQL)
                connection.database = "postgres";
            else if (connection.driver == navidog::DatabaseKind::SQLServer)
                connection.database = "master";
        }
        if (service_.catalog(connection, node, request, true)) {
            refreshRequest_ = request;
            catalogRequest_ = request;
            emit catalogActivityChanged();
            return;
        }
        refreshItems_.prepend(index);
        return;
    }
}
QTreeWidgetItem *
NavigationPane::catalogItem(const QString &id,
                            const navidog::CatalogNode &node) const {
    for (QTreeWidgetItemIterator it(tree_); *it; ++it)
        if ((*it)->data(0, ProfileRole).toString() == id &&
            same((*it)->data(0, CatalogRole).value<navidog::CatalogNode>(),
                 node))
            return *it;
    return nullptr;
}
void NavigationPane::renderTableFolder(
    QTreeWidgetItem *folder, const QVector<navidog::CatalogNode> &nodes,
    bool truncated) {
    const auto node =
        folder->data(0, CatalogRole).value<navidog::CatalogNode>();
    const auto settings = tableSettings_.value(databaseKey(folder)).toObject();
    const auto members = settings["members"].toObject();
    QVector<navidog::CatalogNode> top;
    QHash<QString, QVector<navidog::CatalogNode>> groups;
    for (auto value : settings["groups"].toArray()) {
        auto group = node;
        group.kind = "tableGroup";
        group.name = value.toString();
        group.object.clear();
        groups.insert(group.name, {});
        top << group;
    }
    for (const auto &table : nodes) {
        auto group = groups.find(members[table.object].toString());
        if (group == groups.end())
            top << table;
        else
            group.value() << table;
    }
    reconcileCatalogChildren(folder, top, truncated);
    for (int i = 0; i < folder->childCount(); ++i) {
        auto *item = folder->child(i);
        const auto group =
            item->data(0, CatalogRole).value<navidog::CatalogNode>();
        if (group.kind != "tableGroup" || !groups.contains(group.name))
            continue;
        item->setData(0, LoadedRole, true);
        reconcileCatalogChildren(item, groups.value(group.name), truncated);
    }
}
void NavigationPane::manageTableGroup(const QString &id,
                                      const navidog::CatalogNode &node,
                                      TableGroupAction action, QString name) {
    if (service_.busy() || dialogOpen_)
        return;
    const bool renaming = action == TableGroupAction::Rename;
    const bool naming = action == TableGroupAction::Create || renaming;
    if (naming) {
        name = askName(renaming ? "重命名表组" : "新建表组", "组名",
                       renaming ? node.name : QString{});
        if (name.isEmpty())
            return;
        if (name.size() > 64) {
            QMessageBox::information(this, "名称过长", "组名最多 64 个字符。");
            return;
        }
    }

    const auto key = tableSettingsKey(id, node);
    auto settings = tableSettings_.value(key).toObject();
    auto names = settings["groups"].toArray();
    auto members = settings["members"].toObject();
    if (naming && names.contains(name) && !(renaming && name == node.name)) {
        QMessageBox::information(this, "名称重复", "该表组已存在。");
        return;
    }

    switch (action) {
    case TableGroupAction::Create:
        names.append(name);
        if (node.kind == "table")
            members[node.object] = name;
        break;
    case TableGroupAction::Assign:
        members[node.object] = name;
        break;
    case TableGroupAction::Remove:
        members.remove(node.object);
        break;
    case TableGroupAction::Rename:
    case TableGroupAction::Delete:
        for (qsizetype i = 0; i < names.size(); ++i) {
            if (names.at(i).toString() != node.name)
                continue;
            if (renaming)
                names.replace(i, name);
            else
                names.removeAt(i);
            break;
        }
        for (auto member = members.begin(); member != members.end();) {
            if (member.value().toString() != node.name) {
                ++member;
            } else if (renaming) {
                member.value() = name;
                ++member;
            } else {
                member = members.erase(member);
            }
        }
        break;
    }

    settings["groups"] = names;
    settings["members"] = members;
    auto updated = tableSettings_;
    updated[key] = settings;
    if (!saveTableSettings_(updated))
        return;
    tableSettings_ = std::move(updated);
    regroupTables(id, node);
}

void NavigationPane::updateTableSettingsForObjectChange(
    const QString &id, const navidog::CatalogNode &node,
    const QString &newName) {
    const auto key = tableSettingsKey(id, node);
    auto settings = tableSettings_.value(key).toObject();
    for (const auto &field : {"members", "profiles"}) {
        auto entries = settings[field].toObject();
        const auto previous = entries.take(node.object);
        if (!newName.isEmpty() && !previous.isUndefined())
            entries[newName] = previous;
        settings[field] = entries;
    }
    tableSettings_[key] = settings;
    saveTableSettings_(tableSettings_);
}

void NavigationPane::regroupTables(const QString &id,
                                   const navidog::CatalogNode &node) {
    for (QTreeWidgetItemIterator i(tree_); *i; ++i) {
        auto n = (*i)->data(0, CatalogRole).value<navidog::CatalogNode>();
        if ((*i)->data(0, ProfileRole).toString() != id || n.kind != "tables" ||
            n.database != node.database || n.schema != node.schema ||
            !(*i)->data(0, LoadedRole).toBool())
            continue;
        auto *folder = *i;
        QVector<navidog::CatalogNode> nodes;
        std::function<void(QTreeWidgetItem *)> collect =
            [&](QTreeWidgetItem *p) {
                for (int j = 0; j < p->childCount(); ++j) {
                    auto *child = p->child(j);
                    auto n = child->data(0, CatalogRole)
                                 .value<navidog::CatalogNode>();
                    if (n.kind == "table")
                        nodes << n;
                    else if (n.kind == "tableGroup")
                        collect(child);
                }
            };
        collect(folder);
        qDeleteAll(folder->takeChildren());
        renderTableFolder(folder, nodes);
        tree_->setCurrentItem(folder);
        return;
    }
}
void NavigationPane::applyCatalogResult(const QString &request,
                                        const navidog::CatalogResult &result) {
    // Menu handlers can keep item pointers across the nested menu event loop.
    // Apply completed navigation reads after those handlers have returned.
    if (menuOpen_) {
        deferredResults_.push_back({request, result});
        return;
    }
    const navidog::OperationTimer timing(navidog::TimedOperation::ApplyCatalog);
    const bool background = request == refreshRequest_;
    if (background)
        refreshRequest_.clear();
    emit catalogActivityChanged();
    if (catalogRequest_ == request)
        catalogRequest_.clear();
    for (QTreeWidgetItemIterator it(tree_); *it; ++it) {
        auto *item = *it;
        if (item->data(0, RequestRole).toString() != request)
            continue;
        QString requestedFolder;
        if (pendingFolder_ &&
            pendingFolder_->parent == tree_->indexFromItem(item)) {
            requestedFolder = std::move(pendingFolder_->kind);
            pendingFolder_.reset();
        }
        emit serverInfoReceived(item->data(0, ProfileRole).toString(),
                                result.serverVersion, result.detectedVersion);
        if (!result.error.isEmpty() || result.cancelled) {
            if (background) {
                item->setToolTip(0, result.cancelled ? "自动刷新已停止"
                                                     : result.error);
                break;
            }
            reconcileCatalogChildren(item, {}, false);
            item->setData(0, LoadedRole, false);
            item->setChildIndicatorPolicy(QTreeWidgetItem::ShowIndicator);
            auto *note = new QTreeWidgetItem(
                item, {result.cancelled ? "已停止，右键刷新重试"
                                        : "加载失败，右键刷新重试"});
            note->setToolTip(0, result.error);
            emit statusChanged(result.cancelled ? "对象加载已停止"
                                                : "对象加载失败");
            if (!result.cancelled)
                emit errorOccurred("对象加载失败", result.error);
            return;
        }
        item->setData(0, LoadedRole, true);
        auto loadedNode =
            item->data(0, CatalogRole).value<navidog::CatalogNode>();
        if (loadedNode.kind == "database" || loadedNode.kind == "schema") {
            openedDatabases_.insert(databaseKey(item));
            auto font = item->font(0);
            font.setBold(true);
            item->setFont(0, font);
            item->setToolTip(0, "已打开");
            item->setData(0, MutedIconRole, false);
        }
        const auto profileId = item->data(0, ProfileRole).toString();
        if (loadedNode.kind == "tables")
            renderTableFolder(item, result.nodes, result.truncated);
        else
            reconcileCatalogChildren(item, result.nodes, result.truncated);
        if (result.truncated) {
            auto *more =
                new QTreeWidgetItem(item, {"仅显示前 1000 项，请使用查询筛选"});
            more->setFlags(Qt::ItemIsEnabled);
        }
        item->setChildIndicatorPolicy(
            QTreeWidgetItem::DontShowIndicatorWhenChildless);
        if (!background)
            emit statusChanged(
                QString("已加载 %1 个对象").arg(result.nodes.size()));
        emit catalogLoaded(profileId, loadedNode, result);
        applyFilter(search_->text(), false);
        if (!requestedFolder.isEmpty() && item->isExpanded() &&
            !item->isHidden() && !openNavigationFolder(item, requestedFolder))
            emit statusChanged(
                "当前目录下没有对应的对象分类，请检查所选数据库或 Schema");
        break;
    }
}
void NavigationPane::navigateFolder(const QString &kind) {
    pendingFolder_.reset();
    auto *item = tree_->currentItem();
    while (item) {
        auto n = item->data(0, CatalogRole).value<navidog::CatalogNode>();
        if (n.kind == kind) {
            tree_->scrollToItem(item);
            item->setExpanded(true);
            emit objectsRequested(item);
            return;
        }
        if (n.kind == "database" || n.kind == "schema")
            break;
        item = item->parent();
    }
    if (!item) {
        emit statusChanged("请先选择数据库或 schema");
        return;
    }
    if (openNavigationFolder(item, kind))
        return;
    if (item->data(0, LoadedRole).toBool()) {
        item->setExpanded(true);
        emit statusChanged(
            "当前目录下没有对应的对象分类，请检查所选数据库或 Schema");
        return;
    }
    pendingFolder_ = FolderNavigation{tree_->indexFromItem(item), kind};
    item->setExpanded(true);
    requestLoad(item);
    emit statusChanged("正在等待对象目录加载…");
}

bool NavigationPane::openNavigationFolder(QTreeWidgetItem *parent,
                                          const QString &kind) {
    for (int i = 0; i < parent->childCount(); ++i) {
        auto *child = parent->child(i);
        if (child->data(0, CatalogRole).value<navidog::CatalogNode>().kind !=
            kind)
            continue;
        tree_->setCurrentItem(child);
        tree_->scrollToItem(child);
        emit objectsRequested(child);
        return true;
    }
    return false;
}

void NavigationPane::applyFilter(const QString &text, bool revealMatches) {
    // Expanding search matches must never open a new database connection.
    const QSignalBlocker block(tree_);
    std::function<bool(QTreeWidgetItem *, bool)> filter =
        [&](QTreeWidgetItem *item, bool ancestorMatch) {
            if (!text.isEmpty() &&
                !item->data(0, PreFilterExpansionRole).isValid())
                item->setData(0, PreFilterExpansionRole, item->isExpanded());
            const bool match =
                text.isEmpty() || ancestorMatch ||
                item->text(0).contains(text, Qt::CaseInsensitive);
            bool visible = match, childVisible = false;
            for (int i = 0; i < item->childCount(); ++i)
                childVisible = filter(item->child(i), match) || childVisible;
            visible |= childVisible;
            item->setHidden(!visible);
            if (text.isEmpty()) {
                if (item->data(0, PreFilterExpansionRole).isValid()) {
                    item->setExpanded(
                        item->data(0, PreFilterExpansionRole).toBool());
                    item->setData(0, PreFilterExpansionRole, QVariant{});
                }
            } else if (childVisible && revealMatches)
                item->setExpanded(true);
            return visible;
        };
    for (int i = 0; i < tree_->topLevelItemCount(); ++i) {
        filter(tree_->topLevelItem(i), false);
    }
    updateLocateAvailability();
}

void NavigationPane::updateLocateAvailability() {
    const auto *item = tree_->currentItem();
    locate_->setEnabled(!search_->text().isEmpty() && item &&
                        item->isSelected() && !item->isHidden());
}

void NavigationPane::locateFilteredItem() {
    auto *item = tree_->currentItem();
    if (!locate_->isEnabled() || !item)
        return;
    search_->clear();
    {
        // These nodes are already loaded. Only restore their visible path.
        const QSignalBlocker block(tree_);
        for (auto *parent = item->parent(); parent; parent = parent->parent())
            parent->setExpanded(true);
    }
    tree_->setCurrentItem(item);
    tree_->scrollToItem(item, QAbstractItemView::PositionAtCenter);
    focusTree();
}

void NavigationPane::scheduleCachedSearch() {
    ++cachedSearchRevision_;
    cachedResults_->hide();
    cachedTree_->clear();
    tree_->show();
    const auto term = search_->text().trimmed();
    if (term.isEmpty() || term.size() > 512)
        cachedSearchDelay_.stop();
    else
        cachedSearchDelay_.start();
}

void NavigationPane::searchCachedObjects() {
    const auto term = search_->text().trimmed();
    if (cachedSearchRunning_ || term.isEmpty() || term.size() > 512)
        return;
    QVector<QPair<QString, QString>> connections;
    for (const auto &profile : profiles_)
        if (!service_.isConnected(profile.id))
            connections.push_back({profile.id, navidog::metadataIdentity(profile)});
    if (connections.isEmpty())
        return;

    constexpr int displayLimit = 200;
    using Matches = QVector<QPair<QString, QVector<navidog::CachedObjectName>>>;
    cachedSearchRunning_ = true;
    const auto revision = cachedSearchRevision_;
    runDatabaseJob(this, [path = metadataPath_, term,
                          connections = std::move(connections)] {
        navidog::MetadataStore store(path);
        Matches result;
        int remaining = displayLimit + 1;
        for (const auto &connection : connections) {
            auto names = store.searchObjectNames(connection.first, connection.second,
                                                 term, remaining);
            remaining -= int(names.size());
            if (!names.isEmpty())
                result.push_back({connection.first, std::move(names)});
            if (remaining == 0)
                break;
        }
        return result;
    }, [this, revision](std::optional<Matches> result, const QString &error) {
        cachedSearchRunning_ = false;
        if (revision != cachedSearchRevision_) {
            if (!cachedSearchDelay_.isActive())
                searchCachedObjects();
            return;
        }
        if (!result) {
            if (!error.isEmpty())
                emit statusChanged("无法读取本地元数据缓存");
            return;
        }

        int count = 0;
        for (const auto &entry : *result) {
            auto *connection = connectionItem(entry.first);
            if (!connection)
                continue;
            const auto node = connection->data(0, CatalogRole).value<navidog::CatalogNode>();
            auto *group = new QTreeWidgetItem(cachedTree_, {connection->text(0)});
            group->setIcon(0, connection->icon(0));
            for (const auto &match : entry.second) {
                if (count == displayLimit)
                    break;
                QStringList context;
                if (!match.database.isEmpty() && match.database != node.database &&
                    match.kind != "database")
                    context << match.database;
                if (!match.schema.isEmpty() && match.schema != match.database &&
                    match.kind != "schema")
                    context << match.schema;
                if (!match.parent.isEmpty())
                    context << match.parent;
                auto *item = new QTreeWidgetItem(group, {context.isEmpty() ? match.name :
                    match.name + "  ·  " + context.join('.')});
                if (match.kind != "column")
                    item->setIcon(0, navidog::ui::objectIcon(match.kind));
                item->setData(0, ProfileRole, entry.first);
                item->setToolTip(0, (context << match.name).join('.') +
                                    "\n来自本地缓存，双击定位到所属连接");
                ++count;
            }
            if (group->childCount() == 0)
                delete group;
            else
                group->setExpanded(true);
        }
        if (!count)
            return;
        cachedResultsTitle_->setText(count == displayLimit ?
            "未打开连接的缓存对象 · 最多 200 项" : "未打开连接的缓存对象");
        cachedResults_->show();
        bool visible = false;
        for (int i = 0; i < tree_->topLevelItemCount(); ++i)
            visible |= !tree_->topLevelItem(i)->isHidden();
        tree_->setVisible(visible);
    });
}
