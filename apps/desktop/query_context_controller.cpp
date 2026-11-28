#include "components.hpp"
#include "main_window.hpp"
#include "query_session_controller.hpp"
#include "metadata_controller.hpp"
#include "navigation_pane.hpp"
#include "query_pane.hpp"
#include <QFileInfo>
#include <QLabel>
#include <QLineEdit>
#include <QScopedValueRollback>
#include <QSignalBlocker>
#include <QTabWidget>
#include <QUuid>
#include <algorithm>

namespace {
QString schemaKey(const QString &id, const QString &db) {
  return id + QChar(0x1f) + db;
}
QString selectedValue(QComboBox *combo) {
  const auto value = combo->currentData();
  return combo->currentIndex() >= 0 && value.isValid() &&
                 combo->itemText(combo->currentIndex()) == combo->currentText()
             ? value.toString()
             : combo->currentText().trimmed();
}
void fill(QComboBox *combo, const QString &current, QStringList values,
          const QString &defaultLabel) {
  QSignalBlocker block(combo);
  combo->clear();
  combo->addItem(defaultLabel, QString{});
  values.removeAll(QString{});
  values.removeDuplicates();
  values.sort(Qt::CaseInsensitive);
  for (const auto &value : values)
    combo->addItem(value, value);
  if (!current.isEmpty() && combo->findData(current) < 0)
    combo->addItem(current, current);
  combo->setCurrentIndex(qMax(0, combo->findData(current)));
}
} // namespace

void MainWindow::configureQueryContext(QueryPane *pane, bool preserveInput) {
  const bool pendingDatabase = preserveInput &&
                               pane->databaseContext->lineEdit() &&
                               pane->databaseContext->lineEdit()->isModified();
  const auto databaseInput = pane->databaseContext->currentText();
  const bool pendingSchema = preserveInput && pane->schemaContext->lineEdit() &&
                             pane->schemaContext->lineEdit()->isModified();
  const auto schemaInput = pane->schemaContext->currentText();
  auto it =
      std::find_if(profiles_.begin(), profiles_.end(),
                   [&](const auto &p) { return p.id == pane->profileId; });
  pane->setConnectionCapabilities(
      it == profiles_.end() ? navidog::DatabaseKind::Unknown : it->driver,
      it == profiles_.end() || (!it->http.enabled && it->driver!=navidog::DatabaseKind::Redis));
  if (it == profiles_.end())
    return;
  pane->setProperty("configuringQueryContext", true);
  const auto &p = *it;
  if (p.http.enabled || (!navidog::usesNativePostgres(p.driver) &&
                         !navidog::usesSchemaAtConnection(p.driver)))
    pane->schema.clear();
  const auto database = pane->database.isEmpty() ? p.database : pane->database;
  pane->selectedExportContext = navidog::ui::exportContext(p, database, pane->schema);
  pane->editor->setDiagnosticContext(pane->kind ==
                                             navidog::WorkspaceTabKind::Query
                                         ? p.driver
                                         : navidog::DatabaseKind::Unknown,
                                     database, pane->schema);
  metadata_->updateContext(*pane);
  auto databases = queryDatabases_.value(p.id),
       schemas = querySchemas_.value(schemaKey(p.id, database));
  if (!p.database.isEmpty())
    databases << p.database;
  for (const auto &n : navigation_->namespaces(p.id)) {
    if (n.kind == "database")
      databases << n.database;
    else if (n.kind == "schema" && (navidog::usesSchemaAtConnection(p.driver) ||
                                    n.database == database))
      schemas << n.schema;
  }
  const bool query = pane->kind == navidog::WorkspaceTabKind::Query;
  const bool dbSelectable = query && p.driver != navidog::DatabaseKind::SQLite && p.driver != navidog::DatabaseKind::Redis &&
                            !navidog::usesSchemaAtConnection(p.driver);
  const bool schemaSelectable = query && !p.http.enabled &&
                                (navidog::usesNativePostgres(p.driver) ||
                                 navidog::usesSchemaAtConnection(p.driver));
  pane->setContextEditable(dbSelectable, schemaSelectable);
  if (p.driver == navidog::DatabaseKind::SQLite) {
    QSignalBlocker block(pane->databaseContext);
    pane->databaseContext->clear();
    pane->databaseContext->addItem(QFileInfo(p.database).fileName(),
                                   p.database);
  } else
    fill(pane->databaseContext, database, databases, "默认数据库");
  if (navidog::usesNativePostgres(p.driver) ||
      navidog::usesSchemaAtConnection(p.driver))
    fill(pane->schemaContext, pane->schema, schemas, "默认 Schema");
  else {
    QSignalBlocker block(pane->schemaContext);
    pane->schemaContext->clear();
    pane->schemaContext->addItem(
        navidog::isMySqlFamily(p.driver)
            ? (database.isEmpty() ? "同数据库" : database)
        : p.driver == navidog::DatabaseKind::SQLite ? "main"
                                                   : "默认 Schema",
        QString{});
  }
  pane->databaseContext->setToolTip(
      p.driver == navidog::DatabaseKind::Dameng ? "达梦在连接内按 Schema 组织对象"
      : navidog::usesOracleSql(p.driver)
          ? "Oracle 服务 / SID / TNS 由连接决定，请切换连接来改变服务"
      : p.driver == navidog::DatabaseKind::SQLite
          ? p.database
          : "数据库；展开列表读取可访问数据库，也可以输入名称。切换会重建本标签"
            "会话。");
  pane->schemaContext->setToolTip(
      p.http.enabled ? "HTTP 不保留会话，请在 SQL 中使用 Schema 限定名称"
      : navidog::isMySqlFamily(p.driver)
          ? "MySQL / MariaDB 的 Schema 与数据库相同，使用左侧数据库列表切换"
      : schemaSelectable ? "默认 "
                           "Schema；展开读取列表，也可以输入名称。切换会重建本"
                           "标签会话，并核对未提交事务。"
                         : "此引擎的默认 Schema 不能独立切换，请使用限定名称");
  if (pendingDatabase && dbSelectable) {
    QSignalBlocker block(pane->databaseContext);
    pane->databaseContext->setEditText(databaseInput);
  }
  if (pendingSchema && schemaSelectable) {
    QSignalBlocker block(pane->schemaContext);
    pane->schemaContext->setEditText(schemaInput);
  }
  pane->setProperty("configuringQueryContext", false);
  pane->updateWorkspaceActivity({service_.busy(), service_.operationBusy()});
}

bool MainWindow::changeQueryContext(QueryPane *pane, QueryContextPart part) {
  if (workspaceCloseFlow_ || pane->property("closingWorkspace").toBool() ||
      pane->property("configuringQueryContext").toBool() ||
      pane->changingContext)
    return false;
  const auto id =
      part == QueryContextPart::Connection ? pane->connection->currentData().toString()
                                           : pane->profileId;
  auto profile = std::find_if(profiles_.begin(), profiles_.end(),
                              [&](const auto &p) { return p.id == id; });
  if (profile == profiles_.end())
    return false;
  const auto database =
      part == QueryContextPart::Database ? selectedValue(pane->databaseContext)
                                        : pane->database;
  const auto schema =
      part == QueryContextPart::Schema ? selectedValue(pane->schemaContext)
                                      : pane->schema;
  if ((part == QueryContextPart::Connection && id == pane->profileId) ||
      (part == QueryContextPart::Database &&
       database == (pane->database.isEmpty() ? profile->database : pane->database)) ||
      (part == QueryContextPart::Schema && schema == pane->schema))
    return true;
  QScopedValueRollback<bool> changing(pane->changingContext, true);
  if (pane->executing || service_.operationBusy() ||
      !querySessions_->release(*pane)) {
    {
      QSignalBlocker block(pane->connection);
      pane->connection->setCurrentIndex(
          pane->connection->findData(pane->profileId));
    }
    configureQueryContext(pane);
    return false;
  }
  if (part == QueryContextPart::Connection) {
    pane->profileId = id;
    pane->database = profile->database;
    pane->schema.clear();
  } else if (part == QueryContextPart::Database) {
    pane->database = database;
    pane->schema.clear();
  } else
    pane->schema = schema;
  configureQueryContext(pane);
  {
    QSignalBlocker block(connection_);
    connection_->setCurrentIndex(connection_->findData(pane->profileId));
  }
  const auto label =
      pane->database.isEmpty() ? QString("查询") : pane->database + " · 查询";
  pane->setProperty("queryBaseTitle", label);
  emit pane->documentChanged();
  const auto index = tabs_->indexOf(pane);
  if (index >= 0 && pane->script.path.isEmpty())
    tabs_->setTabToolTip(
        index, profile->name + " / " + pane->database +
                   (pane->schema.isEmpty() ? QString{} : " / " + pane->schema));
  status_->setText("查询上下文已切换，SQL 草稿已保留");
  return true;
}

void MainWindow::cacheQueryCatalog(const QString &id,
                                   const navidog::CatalogNode &node,
                                   const navidog::CatalogResult &result) {
  if (!result.error.isEmpty() || result.cancelled)
    return;
  QStringList databases, schemas;
  for (const auto &n : result.nodes) {
    if (n.kind == "database")
      databases << n.database;
    if (n.kind == "schema")
      schemas << n.schema;
  }
  if (node.kind == "connection") {
    auto profile = std::find_if(profiles_.begin(), profiles_.end(),
                                [&](const auto &p) { return p.id == id; });
    if (profile != profiles_.end() &&
        navidog::usesSchemaAtConnection(profile->driver))
      querySchemas_[schemaKey(id, profile->database)] = schemas;
    else
      queryDatabases_[id] = databases;
  } else if (node.kind == "database")
    querySchemas_[schemaKey(id, node.database)] = schemas;
  for (int i = 0; i < tabs_->count(); ++i)
    if (auto *p = dynamic_cast<QueryPane *>(tabs_->widget(i));
        p && p->profileId == id && !p->databaseContext->hasFocus() &&
        !p->schemaContext->hasFocus())
      configureQueryContext(p, true);
}

void MainWindow::requestQueryCatalog(QueryPane *pane, bool schemas) {
  if (pane->executing || service_.operationBusy())
    return;
  auto profile =
      std::find_if(profiles_.begin(), profiles_.end(),
                   [&](const auto &p) { return p.id == pane->profileId; });
  if (profile == profiles_.end())
    return;
  if (service_.catalogBusy()) {
    status_->setText(
        "正在读取其他对象，请稍后重新展开列表；可继续编辑 SQL 或新建查询");
    return;
  }
  if (!promptPassword(*profile))
    return;
  auto p = *profile;
  const auto database = pane->database.isEmpty() ? p.database : pane->database;
  navidog::CatalogNode node;
  node.kind = schemas && !navidog::usesSchemaAtConnection(p.driver)
                  ? "database"
                  : "connection";
  node.database = database;
  const auto request = QUuid::createUuid().toString();
  if (service_.catalog(p, node, request)) {
    queryCatalogRequests_.insert(request, {pane, p.id, database, schemas});
    refreshWorkspaceActivity();
    status_->setText(schemas ? "正在读取 Schema 列表…" : "正在读取数据库列表…");
  } else
    status_->setText("正在处理其他请求，请稍后重新展开列表");
}

void MainWindow::handleQueryCatalog(const QString &request,
                                    const navidog::CatalogResult &result) {
  auto requested = queryCatalogRequests_.take(request);
  navidog::CatalogNode node;
  node.database = requested.database;
  auto profile =
      std::find_if(profiles_.begin(), profiles_.end(), [&](const auto &p) {
        return p.id == requested.profileId;
      });
  node.kind = requested.schema && profile != profiles_.end() &&
                      !navidog::usesSchemaAtConnection(profile->driver)
                  ? "database"
                  : "connection";
  cacheQueryCatalog(requested.profileId, node, result);
  auto currentDatabase =
      requested.pane ? requested.pane->database : QString{};
  if (currentDatabase.isEmpty() && profile != profiles_.end())
    currentDatabase = profile->database;
  if (requested.pane && requested.pane->profileId == requested.profileId &&
      currentDatabase == requested.database)
    configureQueryContext(requested.pane, true);
  if (!result.error.isEmpty())
    showError("读取查询上下文失败", result.error);
  status_->setText(result.cancelled         ? "对象加载已停止"
                   : result.error.isEmpty() ? "查询上下文列表已更新"
                                            : "读取列表失败；可重新展开重试");
}
