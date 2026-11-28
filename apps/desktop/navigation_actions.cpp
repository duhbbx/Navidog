#include "main_window.hpp"
#include "connection_controller.hpp"
#include "navigation_pane.hpp"
#include "navidog/node_actions.hpp"
#include "query_pane.hpp"
#include "source_pane.hpp"
#include "data_pane.hpp"
#include "components.hpp"
#include <QAction>
#include <QApplication>
#include <QClipboard>
#include <QJsonArray>
#include <QJsonDocument>
#include <QMenu>
#include <QMimeData>
#include <QTabWidget>
#include <QTimer>
#include <algorithm>

void MainWindow::populateNavigationMenu(QMenu *contextMenu, QTreeWidgetItem *item) {
  auto &menu = *contextMenu;
  const bool group = NavigationPane::isConnectionGroup(item);
  const auto node =
      item ? item->data(0, Qt::UserRole + 2).value<navidog::CatalogNode>()
           : navidog::CatalogNode{};
  const auto kind = !item  ? QString("root")
                    : group ? QString("group")
                    : item->data(0, Qt::UserRole + 2).isValid()
                        ? node.kind
                        : QString("note");
  const auto id = item ? item->data(0, Qt::UserRole).toString() : QString{};
  auto profile = std::find_if(profiles_.cbegin(), profiles_.cend(),
                              [&](const auto &p) { return p.id == id; });
  const auto driver = profile == profiles_.cend()
                          ? navidog::DatabaseKind::Unknown
                          : profile->driver;
  const bool idle = !service_.busy(),
             readOnly = profile != profiles_.cend() && profile->readOnly,
             sourceWritable = !readOnly && (profile == profiles_.cend() ||
                                            !profile->http.enabled);
  const bool opened =
      kind == "connection"
          ? service_.isConnected(id)
          : navigation_->namespaceOpen(item);
  using A = navidog::NodeAction;
  for (auto action : navidog::nodeActions(kind, opened, driver)) {
    QAction *added = nullptr;
    switch (action) {
    case A::GenerateSql: {
      auto *generate = menu.addMenu("生成 SQL");
      generate->setEnabled(idle);
      auto add = [&](const QString &title, navidog::StatementTemplate type) {
        generate->addAction(
            title, this, [this, item, type] { generateStatement(item, type); });
      };
      add("SELECT", navidog::StatementTemplate::Select);
      if (kind == "table") {
        add("INSERT", navidog::StatementTemplate::Insert);
        add("UPDATE", navidog::StatementTemplate::Update);
        add("DELETE", navidog::StatementTemplate::Delete);
      }
      break;
    }
    case A::Separator:
      menu.addSeparator();
      break;
    case A::NewConnection:
      menu.addAction("新建连接…", this, &MainWindow::addConnection)
          ->setEnabled(idle);
      break;
    case A::DuplicateConnection:
      menu.addAction("复制连接…", this,
                     [this, id] { connectionController_->duplicate(id); })
          ->setEnabled(idle);
      break;
    case A::NewGroup:
    case A::RenameGroup:
    case A::DeleteGroup:
    case A::MoveConnection:
    case A::ManageTableGroup:
    case A::RenameTableGroup:
    case A::DeleteTableGroup:
      navigation_->appendGroupAction(menu, action, item);
      break;
    case A::Connect:
      menu.addAction(connectAction_);
      break;
    case A::EditConnection:
      menu.addAction(editConnectionAction_);
      break;
    case A::Disconnect: {
      const QPersistentModelIndex index(navigation_->indexFor(item));
      auto *close =
          menu.addAction(disconnectAction_->text(), this, [this, index] {
            QTimer::singleShot(0, this, [this, index] {
              if (index.isValid())
                closeConnection(navigation_->itemFor(index));
            });
          });
      close->setEnabled(disconnectAction_->isEnabled());
      break;
    }
    case A::RenameConnection:
      menu.addAction(renameAction_);
      break;
    case A::DeleteConnection:
      menu.addAction(removeAction_);
      break;
    case A::OpenDatabase:
      added = menu.addAction(kind == "schema" ? "打开 Schema" : "打开数据库",
                             this, [this, item] { navigation_->openDatabase(item); });
      break;
    case A::CloseDatabase:
      added = menu.addAction(kind == "schema" ? "关闭 Schema" : "关闭数据库",
                             this, [this, item] { closeDatabase(item); });
      break;
    case A::NewDatabase:
      added = menu.addAction("新建数据库…", this, [this, item] {
        namespaceDialog(item, navidog::NamespaceKind::Database);
      });
      added->setObjectName("contextNewDatabase");
      added->setEnabled(!readOnly && idle);
      added = nullptr;
      break;
    case A::NewSchema:
      added = menu.addAction(
          driver == navidog::DatabaseKind::Oracle ? "新建用户 / Schema…"
                                                  : "新建 Schema…",
          this, [this, item] {
            namespaceDialog(item, navidog::NamespaceKind::Schema);
          });
      added->setObjectName("contextNewSchema");
      added->setEnabled(!readOnly && idle);
      added = nullptr;
      break;
    case A::DropNamespace:
      added = menu.addAction(kind == "database" ? "删除数据库…"
                             : driver == navidog::DatabaseKind::Oracle
                                 ? "删除用户 / Schema…"
                                 : "删除 Schema…",
                             this, [this, item] { dropNamespaceDialog(item); });
      added->setObjectName("contextDropNamespace");
      added->setEnabled(!readOnly && idle && !namespaceDeletion_);
      added = nullptr;
      break;
    case A::EditDatabase:
    case A::DatabaseConsole:
    case A::RunDatabaseSqlFile:
    case A::DatabaseDictionary:
    case A::SearchDatabase: {
      using Handler = void (MainWindow::*)(const QString &, const navidog::CatalogNode &);
      Handler handler;
      QString title, objectName;
      switch (action) {
      case A::EditDatabase:
        handler = &MainWindow::editDatabaseProperties;
        title = "编辑数据库…"; objectName = "contextEditDatabase";
        break;
      case A::DatabaseConsole:
        handler = &MainWindow::openDatabaseConsole;
        title = "命令列"; objectName = "contextDatabaseConsole";
        break;
      case A::RunDatabaseSqlFile:
        handler = &MainWindow::runDatabaseSqlFile;
        title = "运行 SQL 文件…"; objectName = "contextRunDatabaseSqlFile";
        break;
      case A::DatabaseDictionary:
        handler = &MainWindow::showDatabaseDictionary;
        title = "数据库字典…"; objectName = "contextDatabaseDictionary";
        break;
      default:
        handler = &MainWindow::openDatabaseSearch;
        title = kind == "connection" ? "在连接中查找对象…" :
                kind == "schema" ? "在 Schema 中查找…" : "在数据库中查找…";
        objectName = "contextDatabaseSearch";
        break;
      }
      added = menu.addAction(title, this, [this, id, node, handler] {
        QTimer::singleShot(0, this, [this, id, node, handler] { (this->*handler)(id, node); });
      });
      added->setObjectName(objectName);
      if (action == A::RunDatabaseSqlFile || action == A::EditDatabase)
        added->setEnabled(idle);
      break;
    }
    case A::DumpDatabaseSql: {
      auto *dump = menu.addMenu("转储 SQL 文件");
      dump->setObjectName("contextDumpDatabaseSql");
      dump->setEnabled(!quickBackup_);
      for (bool withData : {true, false})
        dump->addAction(withData ? "结构和数据…" : "仅结构…", this, [this, id, node, withData] {
          QTimer::singleShot(0, this, [this, id, node, withData] { dumpDatabaseSql(id, node, withData); });
        })->setObjectName(withData ? "dumpDatabaseWithData" : "dumpDatabaseSchemaOnly");
      break;
    }
    case A::NamespaceProperties:
      added =
          menu.addAction(kind == "database" ? "数据库属性" : "Schema 属性",
                         this, [this, item] { showNamespaceProperties(item); });
      added->setObjectName("contextNamespaceProperties");
      break;
    case A::NewQuery:
      added = menu.addAction("新建查询", this, [this, item] {
        auto n = item->data(0, Qt::UserRole + 2).value<navidog::CatalogNode>();
        newQuery({}, item->data(0, Qt::UserRole).toString(), n.database,
                 navidog::WorkspaceTabKind::Query, n.schema);
      });
      added->setShortcut(QKeySequence("Ctrl+Y"));
      added->setShortcutVisibleInContextMenu(true);
      break;
    case A::NewTableChild:
    case A::EditTableChild:
    case A::DeleteTableChild:
    case A::EnableTableChild:
    case A::DisableTableChild: {
      auto *table = item;
      while (
          table &&
          table->data(0, Qt::UserRole + 2).value<navidog::CatalogNode>().kind !=
              "table")
        table = table->parent();
      auto label = kind == "primaryKey"                    ? QString("主键")
                   : kind == "columns" || kind == "column" ? QString("字段")
                   : kind == "indexes" || kind == "index"  ? QString("索引")
                   : kind == "foreignKeys" || kind == "foreignKey"
                       ? QString("外键")
                   : kind == "checks" || kind == "check" ? QString("检查")
                                                         : QString("触发器");
      const int op = action == A::NewTableChild       ? 1
                     : action == A::DeleteTableChild  ? 2
                     : action == A::EnableTableChild  ? 3
                     : action == A::DisableTableChild ? 4
                                                      : 0;
      auto text = (op == 1   ? "新建"
                   : op == 2 ? "删除"
                   : op == 3 ? "启用"
                   : op == 4 ? "禁用"
                             : "设计") +
                  label + "…";
      auto *childAction = menu.addAction(
          text, this, [this, item, op] { editTableChild(item, op); });
      childAction->setObjectName(op == 1   ? "contextNewTableChild"
                                 : op == 2 ? "contextDeleteTableChild"
                                           : "contextEditTableChild");
      childAction->setEnabled(table && idle && (op == 0 || !readOnly));
      break;
    }
    case A::NewTable:
      added = menu.addAction("新建表…", this,
                             [this, item] { openTableDesigner(item, false); });
      added->setEnabled(!readOnly && idle);
      added = nullptr;
      break;
    case A::AlterTable:
      added = menu.addAction("修改表…", this,
                             [this, item] { openTableDesigner(item, true); });
      added->setEnabled(!readOnly && idle);
      added = nullptr;
      break;
    case A::Refresh:
      added = menu.addAction("刷新", this, [this, item] {
        auto *target = item;
        if (target && !target->data(0, Qt::UserRole + 2)
                           .value<navidog::CatalogNode>()
                           .expandable)
          target = target->parent();
        if (target) {
          navigation_->expandCatalog(target, true);
          target->setExpanded(true);
        }
      });
      added->setShortcut(QKeySequence("Ctrl+R"));
      added->setShortcutVisibleInContextMenu(true);
      break;
    case A::CopyName:
      menu.addAction("复制名称", this,
                     [node] { QApplication::clipboard()->setText(node.name); });
      break;
    case A::Preview:
      added = menu.addAction(kind == "table" ? "打开表" : kind == "redisKey" ? "查看键" : "预览前 100 行", this,
                             [this, item] { openObject(item, false); });
      break;
    case A::NewSource:
      added = menu.addAction(kind == "view" || kind == "views" ? "新建视图…"
                             : kind == "function" || kind == "functions"
                                 ? "新建函数…"
                                 : "新建存储过程…",
                             this, [this, item] { openSource(item, false); });
      added->setEnabled(idle && sourceWritable);
      added = nullptr;
      break;
    case A::EditSource:
      added = menu.addAction("设计 / 编辑定义…", this,
                             [this, item] { openSource(item, true); });
      break;
    case A::DropSource:
      added =
          menu.addAction("删除对象…", this, [this, item] { dropSource(item); });
      added->setEnabled(idle && sourceWritable);
      added = nullptr;
      break;
    case A::CallSource:
      added = menu.addAction("生成调用 SQL…", this, [this, item] {
        openSource(item, true);
        auto *p = dynamic_cast<SourcePane *>(tabs_->currentWidget());
        if (!p)
          return;
        if (p->loaded)
          callSource(p);
        else {
          auto id = p->requestId;
          auto connection = std::make_shared<QMetaObject::Connection>();
          *connection = connect(
              &service_, &navidog::QueryService::sourceDesignCompleted, p,
              [this, p, id,
               connection](const QString &done,
                           const navidog::SourceDesignResult &result) {
                if (done != id)
                  return;
                disconnect(*connection);
                if (result.error.isEmpty() && !result.cancelled)
                  QTimer::singleShot(0, p, [this, p] { callSource(p); });
              });
        }
      });
      break;
    case A::Definition:
      added = menu.addAction("查看定义 / 结构", this,
                             [this, item] { openObject(item, true); });
      break;
    case A::TableViews: {
      auto *views = menu.addMenu("通过配置文件打开表");
      views->setEnabled(idle);
      auto profiles = tableSettings_.value(NavigationPane::tableSettingsKey(id, node))
                          .toObject()
                          .value("profiles")
                          .toObject()
                          .value(node.object)
                          .toArray();
      for (auto value : profiles) {
        auto view = value.toObject();
        views->addAction(view["name"].toString(), this,
                         [this, item, view] { openTableView(item, view); });
      }
      if (!profiles.isEmpty())
        views->addSeparator();
      views->addAction("默认配置", this,
                       [this, item] { openObject(item, false); });
      if (!profiles.isEmpty()) {
        auto *remove = views->addMenu("删除配置");
        for (auto value : profiles) {
          auto name = value.toObject()["name"].toString();
          remove->addAction(name, this, [this, id, node, name] {
            auto key = NavigationPane::tableSettingsKey(id, node);
            auto before = tableSettings_;
            auto settings = tableSettings_[key].toObject();
            auto all = settings["profiles"].toObject();
            QJsonArray remaining;
            for (auto value : all[node.object].toArray())
              if (value.toObject()["name"].toString() != name)
                remaining << value;
            all[node.object] = remaining;
            settings["profiles"] = all;
            tableSettings_[key] = settings;
            if (!saveTableSettings(tableSettings_))
              tableSettings_ = before;
          });
        }
      }
      auto *saveView = views->addAction("保存当前筛选为配置…", this,
                                        [this, item] { saveTableView(item); });
      auto *pane = dynamic_cast<DataPane *>(tabs_->currentWidget());
      saveView->setEnabled(pane && pane->isLoaded() && pane->matchesTable(id, node));
      break;
    }
    case A::TablePermissions:
      if (navidog::supportsTableAction(driver,
                                       navidog::TableAction::GrantPrivileges)) {
        added = menu.addAction("设置权限…", this, [this, item] {
          tableTaskDialog(item, navidog::TableAction::GrantPrivileges);
        });
        added->setEnabled(idle && !readOnly);
        added = nullptr;
      }
      break;
    case A::GenerateTableData:
      added = menu.addAction("数据生成…", this, [this, item] {
        tableTaskDialog(item, navidog::TableAction::GenerateData);
      });
      added->setEnabled(idle && !readOnly);
      added = nullptr;
      break;
    case A::TableModel: {
      const QPersistentModelIndex index(navigation_->indexFor(item));
      added = menu.addAction("逆向到 ER 图…", this, [this, index] {
        QTimer::singleShot(0, this, [this, index] {
          if (index.isValid())
            importErProject(navigation_->itemFor(index));
        });
      });
      break;
    }
    case A::TableBI:
      added = menu.addAction("创建 BI 工作区…", this, [this, item] {
        tableTaskDialog(item, navidog::TableAction::BuildBI);
      });
      break;
    case A::ShareTable:
      added = menu.addAction("共享结构文档…", this, [this, item] {
        tableTaskDialog(item, navidog::TableAction::Dictionary);
      });
      break;
    case A::FastOpen:
      added = menu.addAction("打开表（快速）", this, [this, item] {
        auto n = item->data(0, Qt::UserRole + 2).value<navidog::CatalogNode>();
        auto id = item->data(0, Qt::UserRole).toString();
        for (const auto &p : profiles_)
          if (p.id == id) {
            newQuery(navidog::previewSql(p, n), id, n.database,
                     navidog::WorkspaceTabKind::Preview);
            tabs_->setTabText(tabs_->currentIndex(), n.object + " · 快速");
            runCurrent();
            break;
          }
      });
      break;
    case A::RenameTable:
      added = menu.addAction("重命名…", this, [this, item] {
        tableTaskDialog(item, navidog::TableAction::Rename);
      });
      added->setEnabled(idle && !readOnly);
      added = nullptr;
      break;
    case A::DropTable:
      added = menu.addAction("删除表…", this, [this, item] {
        tableTaskDialog(item, navidog::TableAction::Drop);
      });
      added->setEnabled(idle && !readOnly);
      added = nullptr;
      break;
    case A::DeleteRows:
      added = menu.addAction("清空表…", this, [this, item] {
        tableTaskDialog(item, navidog::TableAction::DeleteRows);
      });
      added->setEnabled(idle && !readOnly);
      added = nullptr;
      break;
    case A::TruncateTable:
      added = menu.addAction("截断表…", this, [this, item] {
        tableTaskDialog(item, navidog::TableAction::Truncate);
      });
      added->setEnabled(idle && !readOnly);
      added = nullptr;
      break;
    case A::CopyTable: {
      if (!navidog::supportsTableAction(driver, navidog::TableAction::CopyData))
        break;
      auto *copy = menu.addMenu("复制表");
      copy->setEnabled(idle && !readOnly);
      copy->addAction("结构和数据…", this, [this, item] {
        tableTaskDialog(item, navidog::TableAction::CopyData);
      });
      copy->addAction("仅结构…", this, [this, item] {
        tableTaskDialog(item, navidog::TableAction::CopyStructure);
      });
      break;
    }
    case A::ImportTable:
      added = menu.addAction("导入向导…", this, [this, item] {
        tableTaskDialog(item, navidog::TableAction::ImportCsv);
      });
      added->setEnabled(idle && !readOnly);
      added = nullptr;
      break;
    case A::ExportTable:
      added = menu.addAction("导出向导…", this, [this, item] {
        tableTaskDialog(item, navidog::TableAction::ExportCsv);
      });
      break;
    case A::Dictionary:
      added = menu.addAction("数据字典…", this, [this, item] {
        tableTaskDialog(item, navidog::TableAction::Dictionary);
      });
      break;
    case A::DumpTable: {
      if (!navidog::supportsTableAction(driver,
                                        navidog::TableAction::ExportSql))
        break;
      auto *dump = menu.addMenu("转储 SQL 文件");
      dump->setEnabled(idle);
      dump->addAction("结构和数据…", this, [this, item] {
        tableTaskDialog(item, navidog::TableAction::ExportSql, {}, true);
      });
      dump->addAction("仅结构…", this, [this, item] {
        tableTaskDialog(item, navidog::TableAction::ExportSql, {}, false);
      });
      break;
    }
    case A::MaintainTable: {
      auto *maintain = menu.addMenu("维护");
      using T = navidog::TableAction;
      auto taskAction = [&](QMenu *m, const QString &title, T a,
                            const QString &option = QString{}) {
        auto *action = m->addAction(title, this, [this, item, a, option] {
          tableTaskDialog(item, a, option);
        });
        action->setEnabled(idle &&
                           (!readOnly || !navidog::tableActionWrites(a)));
      };
      taskAction(maintain, "分析表…", T::Analyze);
      if (navidog::supportsTableAction(driver, T::Check)) {
        if (navidog::isMySqlFamily(driver)) {
          auto *checks = maintain->addMenu("检查表");
          for (auto pair :
               {qMakePair(QString("常规"), QString{}),
                qMakePair(QString("快速（Quick）"), QString("QUICK")),
                qMakePair(QString("快（Fast）"), QString("FAST")),
                qMakePair(QString("已更改"), QString("CHANGED")),
                qMakePair(QString("扩展"), QString("EXTENDED"))})
            taskAction(checks, pair.first, T::Check, pair.second);
        } else
          taskAction(maintain, "检查表…", T::Check);
      }
      if (navidog::supportsTableAction(driver, T::Optimize))
        taskAction(maintain,
                   driver == navidog::DatabaseKind::SQLite ? "整理数据库文件…"
                                                           : "优化表…",
                   T::Optimize);
      if (navidog::supportsTableAction(driver, T::Repair)) {
        auto *repairs = maintain->addMenu("修复表");
        taskAction(repairs, "快速（Quick）…", T::Repair, "QUICK");
        taskAction(repairs, "扩展…", T::Repair, "EXTENDED");
      }
      taskAction(maintain, "获取行的总数", T::CountRows);
      break;
    }
    case A::CopyTableReference:
      menu.addAction("复制", this, [this, node, id] {
        auto *mime = new QMimeData;
        QJsonObject reference{{"profile", id},
                              {"database", node.database},
                              {"schema", node.schema},
                              {"table", node.object}};
        mime->setData("application/x-navidog-table",
                      QJsonDocument(reference).toJson(QJsonDocument::Compact));
        mime->setText(node.object);
        QApplication::clipboard()->setMimeData(mime);
      });
      break;
    case A::PasteTable: {
      auto *mime = QApplication::clipboard()->mimeData();
      auto reference = mime ? QJsonDocument::fromJson(
                                  mime->data("application/x-navidog-table"))
                                  .object()
                            : QJsonObject{};
      auto *paste =
          menu.addAction("粘贴…", this, [this, item] { pasteTable(item); });
      paste->setEnabled(
          idle && !readOnly && reference["profile"].toString() == id &&
          reference["database"].toString() == node.database &&
          reference["schema"].toString() == node.schema &&
          navidog::supportsTableAction(driver, navidog::TableAction::CopyData));
      break;
    }
    }
    if (added)
      added->setEnabled(idle);
  }
}
