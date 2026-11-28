#include "main_window.hpp"
#include "query_session_controller.hpp"
#include "export_paths.hpp"
#include "navigation_pane.hpp"
#include "data_pane.hpp"
#include "query_pane.hpp"

#include <QCryptographicHash>
#include <QDateTime>
#include <QDir>
#include <QFileDialog>
#include <QInputDialog>
#include <QJsonArray>
#include <QJsonDocument>
#include <QMessageBox>
#include <QRegularExpression>
#include <QSaveFile>
#include <QStandardItemModel>
#include <cmath>
#include <functional>

namespace {
using navidog::DatabaseKind;

QDir queryDirectory(const QString& workspace, const QString& id, const QString& database, const QString& schema) {
    const auto scope = QJsonDocument(QJsonArray{id, database, schema}).toJson(QJsonDocument::Compact);
    const auto key = QCryptographicHash::hash(scope, QCryptographicHash::Sha256).toHex();
    return QDir(QDir(workspace).filePath("queries/" + QString::fromLatin1(key)));
}

QString mysqlEventScript(const QString& statement) {
    const auto delimiter = "$ND" + QUuid::createUuid().toString(QUuid::Id128).left(8) + "$";
    return "DELIMITER " + delimiter + "\n" + statement + "\n" + delimiter + "\nDELIMITER ;\n";
}

QString askQueryName(QWidget* parent, const QString& initial = {}) {
    bool accepted = false;
    auto name = QInputDialog::getText(parent, "查询名称", "名称", QLineEdit::Normal, initial, &accepted).trimmed();
    if (!accepted || name.isEmpty()) return {};
    if (name.endsWith(".sql", Qt::CaseInsensitive)) name.chop(4);
    if (name.isEmpty() || name.startsWith('.') || name.size() > 140 ||
        name.contains(QRegularExpression("[\\\\/:*?\"<>|\\x00-\\x1f]"))) {
        QMessageBox::warning(parent, "查询名称无效", "请输入不包含路径或保留字符的名称。");
        return {};
    }
    return name + ".sql";
}

class QueryLibrary final : public WorkspacePane {
public:
    QueryLibrary(const QString& id, const QString& db, const QString& schemaName, QDir directory)
        : schema(schemaName), folder(std::move(directory)) {
        profileId = id;
        database = db;
        setObjectName("queryLibrary");
    }
    QJsonObject sessionState() const override {
        return {{"kind", "queryLibrary"}, {"profileId", profileId}, {"database", database}, {"schema", schema}};
    }
    void refresh() {
        model->removeRows(0, model->rowCount());
        for (const auto& file : folder.entryInfoList({"*.sql"}, QDir::Files | QDir::NoSymLinks, QDir::Name | QDir::IgnoreCase)) {
            if (!file.fileName().contains(filter->text().trimmed(), Qt::CaseInsensitive)) continue;
            auto* name = new QStandardItem(file.completeBaseName());
            name->setData(file.fileName(), Qt::UserRole);
            model->appendRow({name, new QStandardItem(file.lastModified().toString("yyyy-MM-dd HH:mm"))});
        }
        if (model->rowCount()) grid->selectRow(0);
        hint->setText(model->rowCount() ? QString("%1 条查询").arg(model->rowCount())
                                       : "没有匹配的查询。可以从编辑器保存 SQL，或导入 SQL 文件。");
    }
    QString selectedName() const { return grid->currentIndex().siblingAtColumn(0).data(Qt::UserRole).toString(); }
    QString schema;
    QDir folder;
    QStandardItemModel* model = nullptr;
    navidog::ui::ObjectGrid* grid = nullptr;
    navidog::ui::TextInput* filter = nullptr;
    QLabel* hint = nullptr;
};

class ResultChart final : public QWidget {
public:
    explicit ResultChart(navidog::QueryResult result) : values(std::move(result)) { setMinimumSize(520, 310); }
    void setColumns(int category, int metric) { labelColumn = category; valueColumn = metric; update(); }
protected:
    void paintEvent(QPaintEvent*) override {
        QPainter painter(this);
        painter.setRenderHint(QPainter::Antialiasing);
        painter.fillRect(rect(), QColor("#ffffff"));
        struct Bar { QString label; double value; };
        QVector<Bar> bars;
        double low = 0, high = 0;
        for (const auto& row : values.rows) {
            const auto cell = row.value(valueColumn);
            bool ok = false;
            const auto number = cell.toDouble(&ok);
            if (ok && !cell.isNull() && std::isfinite(number)) {
                const auto label = row.value(labelColumn);
                bars.push_back({label.isNull() ? QString("NULL") : label.toString(), number});
                low = std::min(low, number); high = std::max(high, number);
            }
            if (bars.size() == 24) break;
        }
        painter.setPen(QColor("#344d64"));
        if (bars.isEmpty()) {
            painter.drawText(rect(), Qt::AlignCenter, "所选数值列没有可绘制的数据");
            return;
        }
        const auto left = 158.0, right = double(width() - 78);
        const auto extent = std::max(std::abs(low), std::abs(high));
        const auto scaledLow = extent > 0 ? low / extent : 0.0;
        const auto span = extent > 0 ? high / extent - scaledLow : 1.0;
        const auto position = [&](double value) {
            return left + ((extent > 0 ? value / extent : 0.0) - scaledLow) / span * (right - left);
        };
        const auto zero = position(0);
        const auto step = (height() - 20.0) / bars.size();
        painter.setPen(QPen(QColor("#cbd7e3"), 1));
        painter.drawLine(QLineF(zero, 6, zero, height() - 6));
        for (int i = 0; i < bars.size(); ++i) {
            const auto& bar = bars[i];
            const auto y = 10 + i * step, x = position(bar.value);
            painter.setPen(QColor("#344d64"));
            painter.drawText(QRectF(8, y, 140, step), Qt::AlignRight | Qt::AlignVCenter,
                             fontMetrics().elidedText(bar.label, Qt::ElideRight, 140));
            painter.fillRect(QRectF(std::min(zero, x), y + step * .18, std::max(1.0, std::abs(zero - x)), step * .64),
                             bar.value < 0 ? QColor("#9c5391") : QColor("#167f7a"));
            painter.drawText(QRectF(right + 8, y, 62, step), Qt::AlignRight | Qt::AlignVCenter,
                             QString::number(bar.value, 'g', 6));
        }
    }
private:
    navidog::QueryResult values;
    int labelColumn = 0, valueColumn = 0;
};
}

navidog::CatalogNode MainWindow::quickEntryContext(QString& profileId) const {
    navidog::CatalogNode context;
    if (auto* selected = navigation_->currentItem()) {
        profileId = selected->data(0, Qt::UserRole).toString();
        context = selected->data(0, Qt::UserRole + 2).value<navidog::CatalogNode>();
        for (auto* item = selected->parent(); item; item = item->parent()) {
            const auto parent = item->data(0, Qt::UserRole + 2).value<navidog::CatalogNode>();
            if (context.database.isEmpty()) context.database = parent.database;
            if (context.schema.isEmpty()) context.schema = parent.schema;
        }
    }
    if (profileId.isEmpty()) {
        if (const auto* current = dynamic_cast<WorkspacePane*>(tabs_->currentWidget())) {
            profileId = current->profileId;
            context.database = current->database;
            if (const auto* query = dynamic_cast<const QueryPane*>(current)) context.schema = query->schema;
        }
        if (profileId.isEmpty()) profileId = connection_->currentData().toString();
    }
    const auto profile = std::find_if(profiles_.cbegin(), profiles_.cend(), [&](const auto& p) { return p.id == profileId; });
    if (profile != profiles_.cend() && profile->driver != DatabaseKind::SQLite && context.database.isEmpty())
        context.database = profile->database;
    if (profile != profiles_.cend() && profile->driver == DatabaseKind::SQLite) {
        // The connection already identifies the file; the tree may display a friendly database name.
        context.database.clear();
        context.schema.clear();
    }
    return context;
}

void MainWindow::openQueryLibrary(const QString& requestedId, const QString& requestedDb, const QString& requestedSchema) {
    auto id = requestedId;
    auto db = requestedDb;
    auto schema = requestedSchema;
    if (id.isEmpty()) {
        const auto context = quickEntryContext(id);
        db = context.database;
        schema = context.schema;
    }
    if (id.isEmpty()) { status_->setText("请先选择数据库连接"); return; }
    for (int i = 0; i < tabs_->count(); ++i) {
        if (auto* pane = dynamic_cast<QueryLibrary*>(tabs_->widget(i));
            pane && pane->profileId == id && pane->database == db && pane->schema == schema) {
            pane->refresh();
            tabs_->setCurrentIndex(i);
            return;
        }
    }
    auto* pane = new QueryLibrary(id, db, schema, queryDirectory(dataDirectory_, id, db, schema));
    auto* layout = new QVBoxLayout(pane);
    auto* tools = new QHBoxLayout;
    auto* open = new navidog::ui::Button("打开", "openSavedQuery");
    auto* import = new navidog::ui::Button("导入 SQL…", "importSavedQuery");
    auto* rename = new navidog::ui::Button("重命名…", "renameSavedQuery");
    auto* remove = new navidog::ui::Button("移到废纸篓", "removeSavedQuery");
    auto* create = new navidog::ui::Button("新建查询", "newLibraryQuery");
    for (auto* button : {open, create, import, rename, remove}) tools->addWidget(button);
    tools->addStretch();
    pane->filter = new navidog::ui::TextInput;
    pane->filter->setPlaceholderText("搜索查询名称");
    tools->addWidget(pane->filter);
    layout->addLayout(tools);
    pane->grid = new navidog::ui::ObjectGrid;
    pane->grid->setObjectName("savedQueriesGrid");
    pane->grid->setSelectionBehavior(QAbstractItemView::SelectRows);
    pane->grid->setSelectionMode(QAbstractItemView::SingleSelection);
    pane->grid->setEditTriggers(QAbstractItemView::NoEditTriggers);
    pane->model = new QStandardItemModel(pane->grid);
    pane->model->setHorizontalHeaderLabels({"名称", "修改时间"});
    pane->grid->setModel(pane->model);
    pane->grid->horizontalHeader()->setSectionResizeMode(0, QHeaderView::Stretch);
    pane->grid->setColumnWidth(1, 168);
    layout->addWidget(pane->grid);
    pane->hint = new QLabel;
    pane->hint->setObjectName("muted");
    layout->addWidget(pane->hint);
    const auto openSelected = [this, pane, id, db, schema] {
        const auto name = pane->selectedName();
        if (name.isEmpty()) return;
        try {
            const auto path = pane->folder.filePath(name);
            const auto canonical = QFileInfo(path).canonicalFilePath();
            for (int i = 0; i < tabs_->count(); ++i)
                if (auto* query = dynamic_cast<QueryPane*>(tabs_->widget(i)); query && !canonical.isEmpty() && query->script.path == canonical) {
                    tabs_->setCurrentIndex(i); return;
                }
            newQuery({}, id, db, navidog::WorkspaceTabKind::Query, schema);
            currentPane()->openScriptFile(canonical.isEmpty() ? path : canonical);
        } catch (const std::exception& e) { showError("打开查询失败", QString::fromUtf8(e.what())); }
    };
    const auto editableSelection = [this, pane] {
        const auto name = pane->selectedName();
        if (name.isEmpty()) return QString{};
        const auto path = QFileInfo(pane->folder.filePath(name)).canonicalFilePath();
        for (int i = 0; i < tabs_->count(); ++i)
            if (auto* query = dynamic_cast<QueryPane*>(tabs_->widget(i)); query && !path.isEmpty() && query->script.path == path) {
                status_->setText("请先关闭正在编辑该查询的标签页");
                return QString{};
            }
        return name;
    };
    connect(open, &QToolButton::clicked, pane, openSelected);
    connect(pane->grid, &QTableView::doubleClicked, pane, [pane, openSelected](const QModelIndex& index) {
        pane->grid->afterDoubleClick(index, [openSelected](const QModelIndex&) { openSelected(); });
    });
    connect(create, &QToolButton::clicked, pane, [this, id, db, schema] {
        newQuery({}, id, db, navidog::WorkspaceTabKind::Query, schema);
    });
    connect(pane->filter, &QLineEdit::textChanged, pane, [pane] { pane->refresh(); });
    connect(import, &QToolButton::clicked, pane, [this, pane] {
        const auto path = QFileDialog::getOpenFileName(this, "导入 SQL 到查询库", {}, "SQL 文件 (*.sql)");
        if (path.isEmpty()) return;
        const auto name = askQueryName(this, QFileInfo(path).completeBaseName());
        if (name.isEmpty()) return;
        try {
            const auto document = navidog::readScriptDocument(path);
            if (!pane->folder.mkpath(".")) throw std::runtime_error("无法创建查询库目录");
            const auto target = pane->folder.filePath(name);
            if (QFileInfo::exists(target)) throw std::runtime_error("查询库中已有同名文件");
            navidog::writeScriptDocument(document, target, document.text, false);
            pane->refresh();
        } catch (const std::exception& e) { showError("导入查询失败", QString::fromUtf8(e.what())); }
    });
    connect(rename, &QToolButton::clicked, pane, [this, pane, editableSelection] {
        const auto old = editableSelection();
        if (old.isEmpty()) return;
        const auto name = askQueryName(this, QFileInfo(old).completeBaseName());
        if (name.isEmpty() || name == old) return;
        if (!pane->folder.rename(old, name)) showError("重命名失败", "同名查询已存在或查询库不可写。");
        pane->refresh();
    });
    connect(remove, &QToolButton::clicked, pane, [this, pane, editableSelection] {
        const auto name = editableSelection();
        if (name.isEmpty()) return;
#if QT_VERSION >= QT_VERSION_CHECK(5, 15, 0)
        if (QMessageBox::question(this, "移除查询", QString("将 %1 移到废纸篓？").arg(name),
                QMessageBox::Yes | QMessageBox::Cancel, QMessageBox::Cancel) != QMessageBox::Yes) return;
        QFile file(pane->folder.filePath(name));
        if (!file.moveToTrash()) showError("移除失败", file.errorString());
#endif
        pane->refresh();
    });
    pane->refresh();
    QStringList scope{connection_->itemText(connection_->findData(id))};
    if (!db.isEmpty()) scope << db;
    if (!schema.isEmpty() && schema != db) scope << schema;
    tabs_->setCurrentIndex(addWorkspaceTab(pane, "查询库 · " + scope.join(" / ")));
}

void MainWindow::saveQueryInLibrary(QueryPane* query) {
    if (!query || query->kind != navidog::WorkspaceTabKind::Query || query->profileId.isEmpty()) {
        status_->setText("请先打开带数据库连接的查询编辑器"); return;
    }
    if (!query->scriptReady() || query->editor->largeDocument()) {
        status_->setText("请使用保存 SQL 将大文件写入磁盘");
        return;
    }
    const auto profile = std::find_if(profiles_.cbegin(), profiles_.cend(), [query](const auto& p) { return p.id == query->profileId; });
    const bool sqlite = profile != profiles_.cend() && profile->driver == DatabaseKind::SQLite;
    const auto folder = queryDirectory(dataDirectory_, query->profileId, sqlite ? QString{} : query->database,
                                       sqlite ? QString{} : query->schema);
    const auto name = askQueryName(this, QFileInfo(query->script.path).completeBaseName());
    if (name.isEmpty()) return;
    try {
        if (!folder.mkpath(".")) throw std::runtime_error("无法创建查询库目录");
        const auto path = folder.filePath(name);
        const auto document = navidog::writeScriptDocument(query->script, path, query->editor->toPlainText(), false);
        query->script = document;
        query->editor->document()->setModified(false);
        emit query->documentChanged();
        status_->setText("已保存到查询库");
    } catch (const std::exception& e) { showError("保存到查询库失败", QString::fromUtf8(e.what())); }
}

void MainWindow::browseServerObjects(bool events) {
    QString id;
    const auto context = quickEntryContext(id);
    const auto profile = std::find_if(profiles_.cbegin(), profiles_.cend(), [&](const auto& p) { return p.id == id; });
    if (profile == profiles_.cend()) { status_->setText("请先选择数据库连接"); return; }
    QString sql;
    switch (profile->driver) {
    case DatabaseKind::MySQL:
    case DatabaseKind::MariaDB:
        sql = events ? "SELECT EVENT_SCHEMA, EVENT_NAME, STATUS, EVENT_TYPE, EXECUTE_AT, "
                       "INTERVAL_VALUE, INTERVAL_FIELD, LAST_EXECUTED FROM information_schema.EVENTS "
                       "WHERE EVENT_SCHEMA = DATABASE() ORDER BY EVENT_NAME"
                     : "SELECT User AS user_name, Host AS account_host FROM mysql.user ORDER BY User, Host";
        if (events && context.database.isEmpty()) { status_->setText("请先选择要查看事件的数据库"); return; }
        break;
    case DatabaseKind::PostgreSQL:
        if (!events) sql = "SELECT rolname, rolcanlogin, rolcreatedb, rolcreaterole, rolsuper "
                           "FROM pg_catalog.pg_roles ORDER BY rolname";
        break;
    case DatabaseKind::Oracle:
        sql = events ? "SELECT owner, job_name, enabled, state, next_run_date FROM all_scheduler_jobs ORDER BY owner, job_name"
                     : "SELECT username, user_id, created FROM all_users ORDER BY username";
        break;
    case DatabaseKind::SQLServer:
        if (!events) sql = "SELECT name, type_desc, authentication_type_desc, create_date "
                           "FROM sys.database_principals WHERE principal_id > 4 ORDER BY name";
        break;
    default: break;
    }
    const QString title = events ? "事件" : "用户";
    if (sql.isEmpty()) { status_->setText(navidog::databaseKindName(profile->driver) + " 暂无 " + title + " 列表入口"); return; }
    const auto kind = profile->driver;
    const auto attachActions = [this, id, context, kind, events, sql](QueryPane* list) {
        if (list->findChild<QWidget*>("serverObjectActions")) return;
        auto* actions = new QWidget(list);
        actions->setObjectName("serverObjectActions");
        auto* tools = new QHBoxLayout(actions);
        tools->setContentsMargins(8, 3, 8, 3);
        const auto add = [actions, tools](const QString& title, const QString& name,
                                          std::function<void()> handler) {
            auto* button = new navidog::ui::Button(title, name);
            tools->addWidget(button);
            QObject::connect(button, &QToolButton::clicked, actions, std::move(handler));
        };
        const auto runChange = [this, id, context, sql, guard = QPointer<QueryPane>(list)](const QString& statement) {
            newQuery(statement, id, context.database, navidog::WorkspaceTabKind::Query, context.schema);
            auto* operation = currentPane();
            querySessions_->run(*operation, statement);
            if (!operation->executing) return;
            const auto session = operation->sessionId;
            auto completion = std::make_shared<QMetaObject::Connection>();
            *completion = connect(&service_, &navidog::QueryService::completed, operation,
                [this, session, guard, sql, completion](const navidog::QueryOutcome& result) {
                    if (result.sessionId != session) return;
                    disconnect(*completion);
                    if (guard && result.result.error.isEmpty() && !result.result.cancelled)
                        QTimer::singleShot(0, guard, [this, guard, sql] { if (guard) querySessions_->run(*guard, sql); });
                });
        };
        if (events && (kind == DatabaseKind::MySQL || kind == DatabaseKind::MariaDB)) {
            const auto selectedEvent = [this, list, kind] {
                const auto selection = list->table->currentIndex();
                const auto result = list->model->result();
                if (!selection.isValid() || selection.row() >= result.rows.size()) {
                    status_->setText("请先选中一个事件"); return QString{};
                }
                const auto row = result.rows.at(selection.row());
                if (row.size() < 2 || row[0].toString().isEmpty() || row[1].toString().isEmpty()) return QString{};
                return navidog::quoteIdentifier(row[0].toString(), kind) + "." +
                       navidog::quoteIdentifier(row[1].toString(), kind);
            };
            add("新建事件", "newServerEvent", [this, id, context, kind] {
                bool accepted = false;
                const auto name = QInputDialog::getText(this, "新建事件", "事件名称", QLineEdit::Normal, {}, &accepted).trimmed();
                if (!accepted || name.isEmpty() || name.size() > 64 || name.contains(QChar::Null)) return;
                const auto qualified = navidog::quoteIdentifier(context.database, kind) + "." + navidog::quoteIdentifier(name, kind);
                newQuery(mysqlEventScript("CREATE EVENT " + qualified + "\nON SCHEDULE EVERY 1 DAY\nDISABLE\nDO SELECT 1"),
                         id, context.database, navidog::WorkspaceTabKind::Query, context.schema);
                status_->setText("编辑计划和事件体，运行 SQL 创建停用状态的事件");
            });
            const auto loadDefinition = [this, id, context, selectedEvent](bool edit) {
                const auto name = selectedEvent();
                if (name.isEmpty()) return;
                const auto definition = "SHOW CREATE EVENT " + name;
                newQuery(definition, id, context.database, navidog::WorkspaceTabKind::Definition, context.schema);
                tabs_->setTabText(tabs_->currentIndex(), "事件定义");
                auto* viewer = currentPane();
                querySessions_->run(*viewer, definition);
                if (!edit || !viewer->executing) return;
                const auto session = viewer->sessionId;
                auto completion = std::make_shared<QMetaObject::Connection>();
                *completion = connect(&service_, &navidog::QueryService::completed, viewer,
                    [this, id, context, session, completion](const navidog::QueryOutcome& result) {
                        if (result.sessionId != session) return;
                        disconnect(*completion);
                        if (!result.result.error.isEmpty() || result.result.cancelled) return;
                        const auto page = result.results.isEmpty() ? result.result : result.results.front();
                        const auto col = page.columns.indexOf("Create Event");
                        auto statement = col < 0 ? QString{} : page.rows.value(0).value(col).toString().trimmed();
                        if (!statement.startsWith("CREATE ", Qt::CaseInsensitive)) {
                            showError("无法编辑事件", "服务器未返回可编辑的事件定义。");
                            return;
                        }
                        statement.replace(0, 6, "ALTER");
                        newQuery(mysqlEventScript(statement), id, context.database, navidog::WorkspaceTabKind::Query, context.schema);
                        status_->setText("修改事件定义后运行 ALTER EVENT，保持修改前请先核对计划");
                    });
            };
            add("查看定义", "viewServerEventDefinition", [loadDefinition] { loadDefinition(false); });
            add("编辑事件…", "editServerEvent", [loadDefinition] { loadDefinition(true); });
            for (const auto& operation : {std::pair<QString, QString>{"启用", "ENABLE"}, {"停用", "DISABLE"}, {"删除", "DROP"}}) {
                add(operation.first, "event" + operation.second, [this, selectedEvent, runChange, operation] {
                    const auto name = selectedEvent();
                    if (name.isEmpty()) return;
                    const auto note = operation.second == "ENABLE" ? "。启用后将按已有计划运行。" : "？";
                    if (QMessageBox::question(this, operation.first + "事件",
                            operation.first + "事件 " + name + note,
                            QMessageBox::Yes | QMessageBox::Cancel, QMessageBox::Cancel) != QMessageBox::Yes) return;
                    runChange(operation.second == "DROP" ? "DROP EVENT " + name :
                              "ALTER EVENT " + name + " " + operation.second);
                });
            }
        } else if (!events && kind == DatabaseKind::Oracle) {
            add("新建用户 / Schema…", "createOracleUserQuick", [this, id] {
                if (auto *item = navigation_->connectionItem(id))
                    namespaceDialog(item, navidog::NamespaceKind::Schema);
            });
        } else if (!events && (kind == DatabaseKind::PostgreSQL || kind == DatabaseKind::MySQL ||
                                kind == DatabaseKind::MariaDB || kind == DatabaseKind::SQLServer)) {
            add("新建角色…", "createServerRole", [this, kind, runChange] {
                bool accepted = false;
                const auto name = QInputDialog::getText(this, "新建角色", "角色名称", QLineEdit::Normal, {}, &accepted).trimmed();
                if (!accepted || name.isEmpty() || name.size() > 128 || name.contains(QChar::Null)) return;
                const auto statement = "CREATE ROLE " + navidog::quoteIdentifier(name, kind);
                runChange(kind == DatabaseKind::PostgreSQL ? statement + " NOLOGIN" : statement);
            });
        }
        tools->addStretch();
        if (tools->count() > 1)
            static_cast<QVBoxLayout*>(list->layout())->insertWidget(2, actions);
        else actions->hide();
    };
    for (int i = 0; i < tabs_->count(); ++i) {
        auto* query = dynamic_cast<QueryPane*>(tabs_->widget(i));
        if (query && query->kind == navidog::WorkspaceTabKind::Definition && query->profileId == id &&
            query->database == context.database && query->schema == context.schema && query->editor->toPlainText() == sql) {
            tabs_->setCurrentIndex(i);
            query->run->setText("刷新列表");
            attachActions(query);
            querySessions_->run(*query, sql);
            return;
        }
    }
    newQuery(sql, id, context.database, navidog::WorkspaceTabKind::Definition, context.schema);
    auto* query = currentPane();
    query->run->setText("刷新列表");
    query->setProperty("queryBaseTitle", title);
    attachActions(query);
    tabs_->setTabText(tabs_->currentIndex(), title);
    querySessions_->run(*query, sql);
}

void MainWindow::showQuickBi() {
    navidog::QueryResult result;
    navidog::ui::ExportContext context;
    if (const auto* query = currentPane()) {
        const auto* grid = qobject_cast<QTableView*>(query->resultTabs->currentWidget());
        const auto* model = grid ? dynamic_cast<ResultModel*>(grid->model()) : query->model;
        if (model) result = model->result();
        context = query->resultExportContext;
    } else if (const auto* data = dynamic_cast<DataPane*>(tabs_->currentWidget())) {
        result = data->cachedRows();
        context = data->exportContext();
    }
    if (result.rows.isEmpty()) {
        auto* item = navigation_->currentItem();
        if (item && item->data(0, Qt::UserRole + 2).value<navidog::CatalogNode>().kind == "table")
            tableTaskDialog(item, navidog::TableAction::BuildBI);
        else status_->setText("请先运行查询、加载数据页或选择一张表");
        return;
    }
    auto* dialog = new navidog::ui::Dialog(this);
    dialog->setObjectName("quickBiDialog");
    dialog->setWindowTitle("BI 图表");
    dialog->setAttribute(Qt::WA_DeleteOnClose);
    dialog->resize(790, 620);
    auto* layout = new QVBoxLayout(dialog);
    auto* tools = new QHBoxLayout;
    auto* category = new navidog::ui::Select;
    auto* metric = new navidog::ui::Select;
    category->setObjectName("biCategory");
    metric->setObjectName("biMetric");
    category->addItems(result.columns);
    metric->addItems(result.columns);
    for (int col = 0; col < result.columns.size(); ++col) {
        if (std::any_of(result.rows.cbegin(), result.rows.cend(), [col](const auto& row) {
            bool ok = false;
            const auto cell = row.value(col);
            const auto value = cell.toDouble(&ok);
            return ok && !cell.isNull() && std::isfinite(value);
        })) { metric->setCurrentIndex(col); break; }
    }
    tools->addWidget(new QLabel("分类列")); tools->addWidget(category, 1);
    tools->addWidget(new QLabel("数值列")); tools->addWidget(metric, 1);
    auto* save = new navidog::ui::Button("导出 PNG…", "exportQuickBi");
    tools->addWidget(save);
    layout->addLayout(tools);
    auto* chart = new ResultChart(std::move(result));
    chart->setObjectName("quickResultChart");
    layout->addWidget(chart, 1);
    auto* info = new QLabel("图表使用当前缓存结果，最多绘制前 24 条有数值的记录。");
    info->setObjectName("muted");
    layout->addWidget(info);
    const auto update = [chart, category, metric] { chart->setColumns(category->currentIndex(), metric->currentIndex()); };
    connect(category, QOverload<int>::of(&QComboBox::currentIndexChanged), dialog, update);
    connect(metric, QOverload<int>::of(&QComboBox::currentIndexChanged), dialog, update);
    connect(save, &QToolButton::clicked, dialog, [this, dialog, chart, context] {
        const auto path = navidog::ui::chooseExportFile(dialog, "导出 BI 图表", context, "BI图表", "PNG (*.png)");
        if (path.isEmpty()) return;
        QSaveFile file(path);
        if (!file.open(QIODevice::WriteOnly) || !chart->grab().save(&file, "PNG") || !file.commit())
            showError("图表导出失败", file.errorString());
    });
    update();
    dialog->show();
}
