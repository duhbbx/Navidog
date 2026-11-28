#include "components.hpp"
#include "main_window.hpp"
#include "navigation_pane.hpp"
#include "objects_pane.hpp"
#include "query_pane.hpp"
#include <QLabel>
#include <QSignalBlocker>
#include <QTabWidget>
#include <algorithm>
ObjectsPane *
MainWindow::createObjectsPane(const navidog::ConnectionProfile &profile,
                              const navidog::CatalogNode &folder) {
  auto *pane = new ObjectsPane;
  pane->setContext(profile, folder);
  const int index = addWorkspaceTab(pane, "对象");
  tabs_->setTabIcon(index, navidog::ui::objectIcon(folder.kind));
  tabs_->setTabToolTip(index, profile.name + " · " + folder.name);
  connect(pane, &ObjectsPane::contextRequested, this,
          [this, pane](const navidog::CatalogNode &node, const QPoint &global) {
            if (auto *item = navigation_->catalogItem(pane->profileId, node))
              navigation_->showContextMenu(item, global);
          });
  connect(pane, &ObjectsPane::actionRequested, this,
          [this, pane](const QString &action, const navidog::CatalogNode &node) {
            if (service_.busy()) {
              status_->setText("请等待当前操作完成");
              return;
            }
            auto *item = navigation_->catalogItem(pane->profileId, node);
            if (!item) {
              status_->setText("请先从导航树打开此对象目录");
              return;
            }
            navigation_->selectItem(item);
            const bool table = node.kind == "table";
            if (action == "refresh") {
              navigation_->expandCatalog(item, true);
              item->setExpanded(true);
            } else if (action == "create") {
              if (node.kind == "tables" || node.kind == "tableGroup")
                openTableDesigner(item, false);
              else
                openSource(item, false);
            } else if (action == "design") {
              if (pane->definitionOnly())
                openObject(item, true);
              else if (table)
                openTableDesigner(item, true);
              else
                openSource(item, true);
            } else if (action == "open") {
              if (table || node.kind == "view" || node.kind == "redisKey")
                openObject(item, false);
              else if (node.kind == "tableGroup")
                openObjects(item);
              else
                openObject(item, true);
            } else if (action == "delete") {
              if (table)
                tableTaskDialog(item, navidog::TableAction::Drop);
              else
                dropSource(item);
            }
          });
  pane->setBusy(service_.busy());
  return pane;
}
void MainWindow::openObjects(QTreeWidgetItem *item, bool activate) {
  if (!item)
    return;
  auto node = item->data(0, Qt::UserRole + 2).value<navidog::CatalogNode>();
  if (!QStringList{"tables", "tableGroup", "views", "functions", "procedures"}
           .contains(node.kind))
    return;
  const auto id = item->data(0, Qt::UserRole).toString();
  auto profile = std::find_if(profiles_.begin(), profiles_.end(),
                              [&](const auto &p) { return p.id == id; });
  if (profile == profiles_.end())
    return;
  ObjectsPane *pane = nullptr;
  for (int i = 0; i < tabs_->count(); ++i)
    if ((pane = dynamic_cast<ObjectsPane *>(tabs_->widget(i))))
      break;
  if (!pane)
    pane = createObjectsPane(*profile, node);
  else
    pane->setContext(*profile, node);
  QVector<navidog::CatalogNode> nodes;
  bool truncated = false;
  for (int i = 0; i < item->childCount(); ++i) {
    auto *child = item->child(i);
    if (child->data(0, Qt::UserRole + 2).isValid())
      nodes << child->data(0, Qt::UserRole + 2).value<navidog::CatalogNode>();
    else if (!NavigationPane::isConnectionGroup(child))
      truncated = true;
  }
  pane->setNodes(nodes,
                 item->data(0, Qt::UserRole + 3).toBool() ||
                     node.kind == "tableGroup",
                 truncated);
  pane->setBusy(service_.busy());
  tabs_->setTabToolTip(tabs_->indexOf(pane), profile->name + " · " + node.name);
  if (activate)
    tabs_->setCurrentWidget(pane);
  if (activate) {
    item->setExpanded(true);
    navigation_->requestLoad(item);
  }
}

void MainWindow::refreshObjectsPanes() {
  for (int i = 0; i < tabs_->count(); ++i)
    if (auto *pane = dynamic_cast<ObjectsPane *>(tabs_->widget(i))) {
      auto *item = navigation_->catalogItem(pane->profileId, pane->folder);
      if (item)
        openObjects(item, false);
      else
        pane->setNodes({}, false);
    }
}

void MainWindow::handleNavigationCatalog(const QString &profileId,
                                         const navidog::CatalogNode &loadedNode,
                                         const navidog::CatalogResult &result) {
  cacheQueryCatalog(profileId, loadedNode, result);
  if (loadedNode.kind == "connection") {
    for (int i = 0; i < tabs_->count(); ++i)
      if (auto *p = dynamic_cast<QueryPane *>(tabs_->widget(i));
          p && p->profileId == profileId &&
          p->databaseContext->property("databaseSelectable").toBool()) {
        QSignalBlocker blocker(p->databaseContext);
        auto current = p->databaseContext->currentText();
        for (auto node : result.nodes)
          if (node.kind == "database" &&
              p->databaseContext->findText(node.database) < 0)
            p->databaseContext->addItem(node.database);
        p->databaseContext->setCurrentText(current);
      }
  }
  refreshObjectsPanes();
}
