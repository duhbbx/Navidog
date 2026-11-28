#include "main_window.hpp"
#include "connection_controller.hpp"
#include "components.hpp"
#include "editor.hpp"
#include "navigation_pane.hpp"
#include "query_pane.hpp"
#include "query_session_controller.hpp"
#include "database_job.hpp"
#include "dictionary_wizard.hpp"
#include "workspace_pane.hpp"
#include "navidog/database_tools.hpp"
#include "navidog/local_store.hpp"
#include "navidog/script_document.hpp"

#include <QFileDialog>
#include <QFileInfo>
#include <QFormLayout>
#include <QHeaderView>
#include <QMessageBox>
#include <QStandardItemModel>
#include <QUuid>
#include <algorithm>
#include <limits>
#include <optional>

namespace {
using namespace navidog;
using K = DatabaseKind;
QString siteName(const ConnectionProfile& profile, const CatalogNode& node) {
    if (profile.driver == K::SQLite) return profile.name;
    if (!node.schema.isEmpty()) return node.schema;
    return node.database;
}

QString relationName(const CatalogNode& node) {
    if (node.kind == "schema" || node.kind == "database" || node.schema.isEmpty() || node.schema == "main")
        return node.object;
    return node.schema + "." + node.object;
}

QString searchObjectKind(const QString& kind) {
    static const QHash<QString, QString> labels{
        {"table", "表"}, {"view", "视图"}, {"materializedView", "物化视图"}, {"function", "函数"},
        {"procedure", "存储过程"}, {"database", "数据库"}, {"schema", "Schema"}, {"index", "索引"},
        {"sequence", "序列"}, {"trigger", "触发器"}, {"event", "事件"}, {"type", "类型"},
        {"synonym", "同义词"}, {"package", "包"}};
    return labels.value(kind, kind);
}

class ConsoleInput final : public navidog::ui::TextInput {
public:
    std::function<void(int)> browseHistory;
protected:
    void keyPressEvent(QKeyEvent* event) override {
        if ((event->key() == Qt::Key_Up || event->key() == Qt::Key_Down) && browseHistory) {
            browseHistory(event->key() == Qt::Key_Up ? -1 : 1);
            return;
        }
        TextInput::keyPressEvent(event);
    }
};

class DatabaseConsole final : public WorkspacePane {
public:
    QString session = QUuid::createUuid().toString(QUuid::Id128);
    TransactionState transactionState = TransactionState::Idle;
    bool sessionContextChanged = false;
    QString schema;
    std::function<void(const QString&)> execute;
    std::function<void()> stop;

    DatabaseConsole(const ConnectionProfile& profile, const CatalogNode& node) {
        profileId = profile.id;
        database = node.database;
        if (node.kind == "schema") schema = node.schema;
        setObjectName("databaseConsole");
        auto *layout = new QVBoxLayout(this);
        auto *tools = new QHBoxLayout;
        tools->addWidget(new QLabel(profile.name + " · " + siteName(profile, node)));
        tools->addStretch();
        auto *clear = new ui::Button("清空输出");
        stop_ = new ui::Button("停止");
        stop_->setEnabled(false);
        tools->addWidget(clear);
        tools->addWidget(stop_);
        layout->addLayout(tools);
        output_ = new QPlainTextEdit;
        output_->setObjectName("databaseConsoleOutput");
        output_->setReadOnly(true);
        output_->setLineWrapMode(QPlainTextEdit::NoWrap);
        output_->setMaximumBlockCount(500);
        layout->addWidget(output_, 1);
        auto *inputRow = new QHBoxLayout;
        prompt_ = profile.driver == K::SQLite ? "sqlite> " :
                  isMySqlFamily(profile.driver) ? "mysql> " : "sql> ";
        inputRow->addWidget(new QLabel(prompt_));
        input_ = new ConsoleInput;
        input_->setObjectName("databaseConsoleInput");
        input_->setPlaceholderText("输入 SQL，按回车执行");
        input_->setMaxLength(1024 * 1024);
        inputRow->addWidget(input_, 1);
        layout->addLayout(inputRow);
        connect(clear, &QToolButton::clicked, output_, &QPlainTextEdit::clear);
        connect(stop_, &QToolButton::clicked, this, [this] { if (stop) stop(); });
        connect(input_, &QLineEdit::returnPressed, this, [this] {
            const auto sql = input_->text().trimmed();
            if (sql.isEmpty() || !execute) return;
            input_->setEnabled(false);
            stop_->setEnabled(true);
            execute(sql);
        });
        input_->browseHistory = [this](int step) {
            historyIndex_ = qBound(0, historyIndex_ + step, int(history_.size()));
            input_->setText(history_.value(historyIndex_));
        };
    }

    void submitted(const QString& sql) {
        output_->appendPlainText(prompt_ + queryTraceText(sql));
        if (history_.size() == 100) history_.removeFirst();
        if (sql.size() <= 65536) history_.push_back(sql);
        historyIndex_ = history_.size();
        input_->clear();
    }
    void previewSubmitted(ImpactQueryKind kind, const QString& command) {
        output_->appendPlainText(kind == ImpactQueryKind::MatchingRows ? "执行前预查 · 匹配行数" : "执行前预查 · 目标总行数");
        output_->appendPlainText(command);
    }

    void ready() {
        input_->setEnabled(true);
        stop_->setEnabled(false);
        if (isVisible()) input_->setFocus();
    }

    void accept(const QueryOutcome& outcome) {
        transactionState = outcome.transactionState;
        sessionContextChanged |= outcome.sessionContextChanged;
        for (const auto& result : outcome.results) {
            if (result.columns.isEmpty()) continue;
            QStringList lines{result.columns.join(" | ")};
            for (const auto& row : result.rows) {
                QStringList fields;
                for (const auto& field : row) {
                    auto text = field.isNull() ? QString("NULL") : field.toString().left(300);
                    fields << text.replace('\n', "\\n").replace('\r', "\\r");
                }
                lines << fields.join(" | ");
            }
            output_->appendPlainText(lines.join('\n').left(256 * 1024));
            if (result.truncated) output_->appendPlainText("仅显示结果的前 100 行。");
        }
        if (outcome.result.cancelled) output_->appendPlainText("已停止");
        else if (!outcome.result.error.isEmpty()) output_->appendPlainText(outcome.result.error.left(4096));
        else output_->appendPlainText(QString("完成 · %1 ms · 影响行数 %2")
                                      .arg(outcome.result.elapsedMs).arg(outcome.result.affectedRows));
        ready();
    }

    QJsonObject sessionState() const override {
        return {{"kind","databaseConsole"},{"profileId",profileId},{"database",database},{"schema",schema}};
    }

private:
    QString prompt_;
    QStringList history_;
    int historyIndex_ = 0;
    QPlainTextEdit *output_;
    ConsoleInput *input_;
    ui::Button *stop_;
};

class DatabaseSearchPane final : public WorkspacePane {
public:
    using ProfileLookup = std::function<std::optional<ConnectionProfile>(const QString&, const CatalogNode&, bool)>;
    using NamespaceLookup = std::function<QVector<CatalogNode>(const QString&)>;
    std::function<void(const QString&, const DatabaseSearchHit&)> openMatch;

    DatabaseSearchPane(const QVector<ConnectionProfile>& profiles, const QString& id,
                       const CatalogNode& node, QString cachePath, ProfileLookup lookup, NamespaceLookup namespaces)
        : lookup_(std::move(lookup)), namespaces_(std::move(namespaces)), cachePath_(std::move(cachePath)) {
        setObjectName("databaseSearchPane");
        profileId = id;
        database = node.database;
        auto *layout = new QHBoxLayout(this);
        auto *criteria = new QWidget;
        criteria->setMaximumWidth(390);
        auto *form = new QFormLayout(criteria);
        connection_ = new ui::Select;
        connection_->setObjectName("databaseSearchConnection");
        for (const auto& profile : profiles) {
            connection_->addItem(profile.name, profile.id);
            drivers_.insert(profile.id, profile.driver);
        }
        connection_->setCurrentIndex(connection_->findData(id));
        database_ = new ui::Select;
        database_->setEditable(true);
        database_->setObjectName("databaseSearchDatabase");
        database_->setInsertPolicy(QComboBox::NoInsert);
        database_->lineEdit()->setPlaceholderText("留空查找所有数据库");
        schema_ = new ui::Select;
        schema_->setEditable(true);
        schema_->setObjectName("databaseSearchSchema");
        schema_->setInsertPolicy(QComboBox::NoInsert);
        schema_->lineEdit()->setPlaceholderText("留空查找所有 Schema");
        term_ = new ui::TextInput;
        term_->setObjectName("databaseSearchTerm");
        term_->setMaxLength(512);
        scope_ = new ui::Select;
        scope_->setObjectName("databaseSearchScope");
        scope_->addItem("表数据", int(DatabaseSearchScope::Data));
        scope_->addItem("对象和字段名称", int(DatabaseSearchScope::ObjectNames));
        scope_->addItem("视图、函数和存储过程定义", int(DatabaseSearchScope::ObjectDefinitions));
        if (node.kind == "connection" || (node.database.isEmpty() && node.schema.isEmpty()))
            scope_->setCurrentIndex(scope_->findData(int(DatabaseSearchScope::ObjectNames)));
        mode_ = new ui::Select;
        mode_->addItem("包含", int(DatabaseSearchMode::Contains));
        mode_->addItem("开头匹配", int(DatabaseSearchMode::StartsWith));
        mode_->addItem("完全匹配", int(DatabaseSearchMode::Exact));
        form->addRow("连接", connection_);
        form->addRow("数据库", database_);
        form->addRow("Schema", schema_);
        databaseLabel_ = form->labelForField(database_);
        schemaLabel_ = form->labelForField(schema_);
        form->addRow("查找", term_);
        form->addRow("范围", scope_);
        form->addRow("匹配", mode_);
        auto *buttons = new QHBoxLayout;
        search_ = new ui::Button("查找", "runDatabaseSearch");
        search_->setProperty("primary", true);
        stop_ = new ui::Button("停止", "stopDatabaseSearch");
        buttons->addWidget(search_);
        buttons->addWidget(stop_);
        form->addRow(buttons);

        auto *results = new QVBoxLayout;
        status_ = new QLabel("输入查找内容开始搜索");
        status_->setObjectName("databaseSearchStatus");
        status_->setWordWrap(true);
        results->addWidget(status_);
        grid_ = new ui::ObjectGrid;
        grid_->setObjectName("databaseSearchResults");
        grid_->setSelectionBehavior(QAbstractItemView::SelectRows);
        grid_->setEditTriggers(QAbstractItemView::NoEditTriggers);
        grid_->horizontalHeader()->setStretchLastSection(true);
        model_ = new QStandardItemModel(grid_);
        grid_->setModel(model_);
        resetResults();
        results->addWidget(grid_, 1);
        layout->addWidget(criteria);
        layout->addLayout(results, 1);

        loadScope(node.database, node.schema);
        updateControls();
        connect(connection_, QOverload<int>::of(&QComboBox::currentIndexChanged),
                this, [this] { loadScope({}, {}); resetResults(); updateControls(); });
        connect(database_, &QComboBox::currentTextChanged, this, [this] {
            updateSchemas({});
            resetResults();
        });
        connect(schema_, &QComboBox::currentTextChanged, this, &DatabaseSearchPane::resetResults);
        connect(scope_, QOverload<int>::of(&QComboBox::currentIndexChanged), this, [this] {
            resetResults();
            updateControls();
        });
        connect(term_, &QLineEdit::textChanged, this, [this] { updateControls(); });
        connect(term_, &QLineEdit::returnPressed, this, &DatabaseSearchPane::search);
        connect(search_, &QToolButton::clicked, this, &DatabaseSearchPane::search);
        connect(stop_, &QToolButton::clicked, this, [this] {
            if (cancel_) cancel_->store(true);
            stop_->setEnabled(false);
            status_->setText("正在停止…");
        });
        connect(grid_, &QTableView::doubleClicked, this, [this](const QModelIndex& index) {
            grid_->afterDoubleClick(index, [this](const QModelIndex& ready) {
                if (!openMatch || ready.row() >= hits_.size()) return;
                const auto& hit = hits_[ready.row()];
                openMatch(connection_->currentData().toString(), hit);
            });
        });
    }

    ~DatabaseSearchPane() override { if (cancel_) cancel_->store(true); }
    QJsonObject sessionState() const override {
        return {{"kind","databaseSearch"},{"profileId",profileId},{"database",database},
                {"schema",schema_->currentText()}};
    }

private:
    void updateSchemas(const QString& selected) {
        const auto kind = drivers_.value(profileId);
        const QSignalBlocker blocked(schema_);
        schema_->clear();
        schema_->addItem(QString{});
        for (const auto& node : namespaces_(profileId))
            if (node.kind == "schema" && (usesSchemaAtConnection(kind) || database_->currentText().isEmpty() ||
                node.database == database_->currentText()) && schema_->findText(node.schema) < 0)
                schema_->addItem(node.schema);
        schema_->setEditText(selected);
        database = database_->currentText();
    }

    void loadScope(const QString& selected, const QString& schema) {
        profileId = connection_->currentData().toString();
        {
            const QSignalBlocker blocked(database_);
            database_->clear();
            database_->addItem(QString{});
            for (const auto& node : namespaces_(profileId))
                if (node.kind == "database") database_->addItem(node.database);
            database_->setEditText(selected);
        }
        updateSchemas(schema);
    }

    void resetResults() {
        hits_.clear();
        model_->clear();
        model_->setHorizontalHeaderLabels({"名称", "匹配数量", "类型", "数据库", "Schema"});
    }

    void updateControls() {
        const auto kind = drivers_.value(profileId);
        const bool active = bool(cancel_);
        connection_->setEnabled(!active);
        term_->setEnabled(!active);
        scope_->setEnabled(!active);
        mode_->setEnabled(!active);
        const bool hasDatabase = kind != K::SQLite && kind != K::Unknown && !usesSchemaAtConnection(kind);
        const bool hasSchema = kind == K::PostgreSQL || kind == K::SQLServer || usesSchemaAtConnection(kind);
        database_->setVisible(hasDatabase);
        databaseLabel_->setVisible(hasDatabase);
        schema_->setVisible(hasSchema);
        schemaLabel_->setVisible(hasSchema);
        database_->setEnabled(!active && hasDatabase);
        schema_->setEnabled(!active && hasSchema);
        const bool objectSearch = DatabaseSearchScope(scope_->currentData().toInt()) != DatabaseSearchScope::Data;
        database_->lineEdit()->setPlaceholderText(objectSearch ? "留空查找所有数据库" : "留空使用连接默认数据库");
        search_->setEnabled(!active && !profileId.isEmpty() && !term_->text().isEmpty());
        stop_->setEnabled(active);
    }

    void search() {
        if (!search_->isEnabled()) return;
        CatalogNode node;
        const auto type = DatabaseSearchScope(scope_->currentData().toInt());
        const auto kind = drivers_.value(profileId);
        const bool objectSearch = type != DatabaseSearchScope::Data;
        const auto databaseName = database_->isVisible() ? database_->currentText().trimmed() : QString{};
        if (!objectSearch) node.database = databaseName;
        auto profile = lookup_(profileId, node, !objectSearch);
        if (!profile) return;
        if (!objectSearch && kind != K::SQLite && !usesSchemaAtConnection(kind) && profile->database.isEmpty()) {
            status_->setText("请选择或输入要搜索的数据库名称");
            return;
        }
        resetResults();
        const DatabaseSearchRequest request{schema_->isEnabled() ? schema_->currentText() : QString{}, term_->text(),
            type, DatabaseSearchMode(mode_->currentData().toInt())};
        const auto cancel = cancel_ = cancellation();
        const bool canRefresh = kind == K::SQLite || !profile->password.isEmpty() || profile->savePassword ||
            profile->credentialSource == CredentialSource::Passwordless ||
            (usesOracleSql(kind) && profile->oracle.externalAuthentication);
        updateControls();
        status_->setText("正在读取目录和搜索…");
        runDatabaseJob(this, [profile = *profile, request, cancel, objectSearch, databaseName,
                              canRefresh, cachePath = cachePath_] {
            if (objectSearch) return searchDatabaseObjects(profile, databaseName, request, cachePath, canRefresh, cancel);
            return searchDatabase(profile, request, cancel);
        }, [this, cancel, objectSearch](std::optional<DatabaseSearchResult> result, const QString& error) {
            cancel_.reset();
            updateControls();
            if (!result) {
                status_->setText(cancel->load() ? "搜索已停止" : error);
                return;
            }
            hits_ = std::move(result->hits);
            for (const auto& hit : std::as_const(hits_)) {
                model_->appendRow({new QStandardItem(relationName(hit.node)),
                                   new QStandardItem(QString::number(hit.matches)), new QStandardItem(searchObjectKind(hit.node.kind)),
                                   new QStandardItem(hit.node.database), new QStandardItem(hit.node.schema)});
            }
            auto summary = QString("%1 · 已检查 %2 个对象 · 匹配 %3 个 · 跳过 %4 个")
                .arg(cancel->load() ? "已停止" : "搜索完成").arg(result->scanned)
                .arg(hits_.size()).arg(result->skipped);
            if (objectSearch) summary += QString(" · 缓存 %1 个范围 · 实时读取 %2 个范围")
                .arg(result->cachedScopes).arg(result->refreshedScopes);
            if (!result->warnings.isEmpty()) summary += "\n" + result->warnings.front();
            status_->setText(summary);
            status_->setToolTip(result->warnings.join('\n'));
            grid_->resizeColumnToContents(0);
        });
    }

    ProfileLookup lookup_;
    NamespaceLookup namespaces_;
    QString cachePath_;
    QHash<QString, K> drivers_;
    Cancellation cancel_;
    QVector<DatabaseSearchHit> hits_;
    ui::Select *connection_, *database_, *schema_, *scope_, *mode_;
    ui::TextInput *term_;
    ui::Button *search_, *stop_;
    QLabel *status_;
    QWidget *databaseLabel_, *schemaLabel_;
    ui::ObjectGrid *grid_;
    QStandardItemModel *model_;
};
}

std::optional<navidog::ConnectionProfile> MainWindow::databaseToolProfile(const QString& id,
                                                                        const navidog::CatalogNode& node) {
    const auto profile = connectionController_->authenticatedProfile(id);
    if (!profile) return {};
    return navidog::scopeDatabase(*profile, node);
}

void MainWindow::openDatabaseConsole(const QString& id, const navidog::CatalogNode& node) {
    const auto profile = std::find_if(profiles_.cbegin(), profiles_.cend(), [&](const auto& p) { return p.id == id; });
    if (profile == profiles_.cend()) return;
    auto *pane = new DatabaseConsole(*profile, node);
    addWorkspaceTab(pane, siteName(*profile, node) + " · 命令列");
    tabs_->setCurrentWidget(pane);
    auto scoped = navidog::scopeDatabase(*profile, node);
    const auto database = scoped.database;
    const auto session = pane->session;
    const auto pending = std::make_shared<navidog::Cancellation>();
    pane->execute = [this, id, node, session, pending, guard = QPointer<DatabaseConsole>(pane)](const QString& sql) {
        const auto target = databaseToolProfile(id, node);
        if (!guard) return;
        if (!target) { guard->ready(); return; }
        navidog::QueryExecutionOptions options;
        options.sessionId = session;
        if (node.kind == "schema" && !target->http.enabled &&
            (target->driver == K::PostgreSQL || usesSchemaAtConnection(target->driver)))
            options.schema = node.schema;
        navidog::QueryLimits limit;
        limit.maxRows = 100;
        limit.maxBytes = 2 * 1024 * 1024;
        const auto policy = preferences_.querySafety;
        const auto token = *pending = navidog::cancellation();
        querySessions_->safetyController().assess(*target, options.schema, sql, {}, policy,
            policy.previewBeforeRun && !guard->sessionContextChanged &&
                (guard->transactionState == TransactionState::Idle || guard->transactionState == TransactionState::Unavailable),
            false, token, [this, guard, pending, token, target = *target, options, limit, sql, policy]
            (QuerySafetyController::Findings findings) {
                if (!guard || *pending != token) return;
                const auto confirmed = QuerySafetyController::confirm(this, target, options.schema, policy, findings);
                if (!guard || *pending != token) return;
                pending->reset();
                if (confirmed && !workspaceCloseFlow_ && !guard->property("closingWorkspace").toBool() &&
                    service_.run(target, sql, limit, options)) {
                    guard->submitted(sql);
                    refreshWorkspaceActivity();
                    return;
                }
                guard->ready();
                status_->setText(confirmed ? "当前无法启动命令，请等待正在运行的数据库操作" : "已取消危险语句执行");
            }, [guard](int, ImpactQueryKind kind, const QString& command) {
                if (guard) guard->previewSubmitted(kind, command);
            });
    };
    pane->stop = [this, pane, id, database, session, pending] {
        if (*pending) {
            (*pending)->store(true);
            pending->reset();
            pane->ready();
            status_->setText("已停止写入风险检查");
        } else {
            service_.cancelQuery(id, database, session);
        }
    };
    connect(&service_, &navidog::QueryService::completed, pane, [pane, session](const auto& result) {
        if (result.sessionId == session) pane->accept(result);
    });
    connect(pane, &QObject::destroyed, &service_, [this, id, database, session, pending] {
        if (*pending) (*pending)->store(true);
        service_.disconnectSession(id, database, session);
    });
}

void MainWindow::openDatabaseSearch(const QString& id, const navidog::CatalogNode& node) {
    auto *pane = new DatabaseSearchPane(profiles_, id, node, navidog::localStorePath(dataDirectory_),
        [this](const QString& profile, const navidog::CatalogNode& scope, bool authenticate)
            -> std::optional<navidog::ConnectionProfile> {
            if (authenticate) return databaseToolProfile(profile, scope);
            const auto found = std::find_if(profiles_.cbegin(), profiles_.cend(),
                                           [&](const auto& p) { return p.id == profile; });
            if (found == profiles_.cend()) return {};
            return *found;
        }, [this](const QString& profile) { return navigation_->namespaces(profile); });
    pane->openMatch = [this, pane](const QString& profile, const navidog::DatabaseSearchHit& hit) {
        const auto target = std::find_if(profiles_.cbegin(), profiles_.cend(), [&](const auto& p) { return p.id == profile; });
        if (target == profiles_.cend()) return;
        const auto& relation = hit.node;
        const bool relationType = relation.kind == "table" || relation.kind == "view" || relation.kind == "materializedView";
        if (!hit.definition.isEmpty() || !relationType) {
            auto *dialog = new navidog::ui::Dialog(pane);
            dialog->setAttribute(Qt::WA_DeleteOnClose);
            dialog->setWindowTitle("对象定义 · " + relationName(relation));
            dialog->resize(780, 520);
            auto *layout = new QVBoxLayout(dialog);
            if (hit.definition.isEmpty()) {
                const auto location = QStringList{relation.database, relation.schema}.join(" / ");
                const auto member = relation.detail.isEmpty() ? QString{} : "\n所属对象  " + relation.detail;
                auto *details = new QLabel(searchObjectKind(relation.kind) + "  " + relation.object + "\n" +
                                           location + member, dialog);
                details->setTextFormat(Qt::PlainText);
                layout->addWidget(details);
            } else {
                auto *source = new SqlReadOnlyView(dialog);
                source->setDatabaseKind(target->driver);
                source->setSourceSql(hit.definition);
                layout->addWidget(source->withActions(), 1);
            }
            auto *buttons = new QDialogButtonBox(QDialogButtonBox::Close, dialog);
            connect(buttons, &QDialogButtonBox::rejected, dialog, &QDialog::close);
            layout->addWidget(buttons);
            dialog->show();
        } else if (hit.previewSql.isEmpty()) {
            newQuery(navidog::previewSql(*target, relation), profile, relation.database,
                     navidog::WorkspaceTabKind::Query, relation.schema);
        } else {
            newQuery(hit.previewSql, profile, relation.database, navidog::WorkspaceTabKind::Query, relation.schema);
            runCurrent();
        }
    };
    addWorkspaceTab(pane, "在数据库或 Schema 中查找");
    tabs_->setCurrentWidget(pane);
}

void MainWindow::runDatabaseSqlFile(const QString& id, const navidog::CatalogNode& node) {
    const auto path = QFileDialog::getOpenFileName(this, "选择要运行的 SQL 文件", {}, "SQL 文件 (*.sql);;所有文件 (*)");
    if (path.isEmpty()) return;
    try {
        const auto document = navidog::readScriptDocument(path);
        if (document.text.trimmed().isEmpty()) { status_->setText("SQL 文件没有内容"); return; }
        newQuery(document.text, id, node.database, navidog::WorkspaceTabKind::Query, node.schema);
        currentPane()->setScriptDocument(document);
        if (QMessageBox::question(this, "运行 SQL 文件",
                QString("将在 %1 执行 %2。继续执行？").arg(node.name, QFileInfo(path).fileName()),
                QMessageBox::Yes | QMessageBox::Cancel, QMessageBox::Cancel) == QMessageBox::Yes)
            runCurrent();
        else status_->setText("SQL 文件已加载，等待执行");
    } catch (const std::exception& e) {
        showError("读取 SQL 文件失败", QString::fromUtf8(e.what()));
    }
}

void MainWindow::showDatabaseDictionary(const QString& id, const navidog::CatalogNode& node) {
    navidog::ui::showDictionaryWizard(this,profiles_,id,node,navidog::localStorePath(dataDirectory_),
        [this](const QString& profile,const navidog::CatalogNode& scope) { return databaseToolProfile(profile,scope); },
        [this](const QString& profile) { return navigation_->namespaces(profile); });
}

void MainWindow::editDatabaseProperties(const QString& id, const navidog::CatalogNode& node) {
    const auto profile = databaseToolProfile(id, node);
    if (!profile || !navidog::supportsDatabaseEdit(profile->driver)) return;
    const auto driver = profile->driver;
    auto *dialog = new navidog::ui::Dialog(this);
    dialog->setObjectName("databasePropertiesEditor");
    dialog->setWindowTitle("编辑数据库 · " + siteName(*profile, node));
    dialog->setAttribute(Qt::WA_DeleteOnClose);
    dialog->resize(640, 350);
    auto *form = new QFormLayout(dialog);
    auto *primary = new navidog::ui::Select;
    primary->setEditable(true);
    primary->setObjectName("databasePropertyPrimary");
    auto *secondary = new navidog::ui::Select;
    secondary->setEditable(true);
    secondary->setObjectName("databasePropertyCharset");
    auto *version = new navidog::ui::NumberInput;
    version->setRange(0, std::numeric_limits<int>::max());
    version->setObjectName("databasePropertyVersion");
    if (driver == navidog::DatabaseKind::SQLite) {
        form->addRow("用户版本", version);
        primary->hide();
        secondary->hide();
        primary->setParent(dialog);
        secondary->setParent(dialog);
    } else {
        form->addRow(driver == navidog::DatabaseKind::PostgreSQL ? "所有者" : "排序规则", primary);
        if (navidog::isMySqlFamily(driver))
            form->addRow("默认字符集", secondary);
        else {
            secondary->hide();
            secondary->setParent(dialog);
        }
        version->hide();
        version->setParent(dialog);
    }
    auto *status = new QLabel("正在读取数据库属性…");
    status->setWordWrap(true);
    form->addRow(status);
    auto *preview = new SqlReadOnlyView;
    preview->setDatabaseKind(driver);
    preview->setObjectName("databasePropertiesSql");
    form->addRow("SQL 预览", preview->withActions());
    auto *buttons = new navidog::ui::FormButtons(dialog);
    form->addRow(buttons);
    auto *save = buttons->button(QDialogButtonBox::Save);
    save->setEnabled(false);
    primary->setEnabled(false);
    secondary->setEnabled(false);
    version->setEnabled(false);
    auto cancel = navidog::cancellation();
    connect(dialog, &QObject::destroyed, this, [cancel] { cancel->store(true); });
    connect(buttons, &QDialogButtonBox::rejected, dialog, &QDialog::close);
    dialog->show();
    runDatabaseJob(dialog, [target = *profile, node, cancel] {
        return navidog::loadDatabaseProperties(target, node, cancel);
    }, [this, dialog, profile = *profile, node, driver, cancel, primary, secondary, version, preview, status, buttons, save]
       (std::optional<navidog::DatabaseProperties> original, const QString& error) {
        if (!original) { status->setText(error); return; }
        const auto& options = original->options;
        if (driver == navidog::DatabaseKind::PostgreSQL) {
            primary->addItems(options.owners);
            primary->setEditText(original->owner);
        } else {
            primary->addItems(options.collations);
            primary->setEditText(original->collation);
            secondary->addItems(options.charsets);
            secondary->setEditText(original->charset);
        }
        version->setValue(original->userVersion);
        version->setEnabled(!profile.readOnly);
        primary->setEnabled(!profile.readOnly);
        secondary->setEnabled(!profile.readOnly);
        status->setText(profile.readOnly ? "当前连接为只读" :
            !options.error.isEmpty() ? "无法读取候选项，可以直接输入：" + options.error :
            driver == navidog::DatabaseKind::PostgreSQL ? "编码：" + original->charset :
            navidog::isMySqlFamily(driver) ? "修改数据库默认设置，现有表保留各自的字符集和排序规则。" : QString{});

        const auto draft = [driver, primary, secondary, version] {
            navidog::DatabaseProperties value;
            value.charset = secondary->currentText().trimmed();
            value.collation = primary->currentText().trimmed();
            if (driver == navidog::DatabaseKind::PostgreSQL) value.owner = value.collation;
            value.userVersion = version->value();
            return value;
        };
        const auto update = [profile, node, original = *original, draft, preview, save] {
            if (profile.readOnly) return;
            try {
                const auto text = navidog::alterDatabaseSql(profile, node, draft());
                preview->setSourceSql(text);
                save->setEnabled(text != navidog::alterDatabaseSql(profile, node, original));
            } catch (const std::exception& e) {
                preview->setSourceSql("-- " + QString::fromUtf8(e.what()).replace('\n', ' '));
                save->setEnabled(false);
            }
        };
        connect(primary, &QComboBox::currentTextChanged, dialog, update);
        connect(secondary, &QComboBox::currentTextChanged, dialog, update);
        connect(version, QOverload<int>::of(&QSpinBox::valueChanged), dialog, update);
        update();
        connect(buttons, &QDialogButtonBox::accepted, dialog,
            [this, dialog, profile, node, draft, cancel, save, status, primary, secondary, version, update] {
                if (!save->isEnabled()) return;
                const auto value = draft();
                save->setEnabled(false);
                primary->setEnabled(false);
                secondary->setEnabled(false);
                version->setEnabled(false);
                status->setText("正在保存…");
                runDatabaseJob(dialog, [profile, node, value, cancel] {
                    navidog::applyDatabaseProperties(profile, node, value, cancel);
                    return true;
                }, [this, dialog, profile, status, primary, secondary, version, update]
                   (std::optional<bool> applied, const QString& operationError) {
                    if (!applied) {
                        status->setText(operationError);
                        primary->setEnabled(true);
                        secondary->setEnabled(true);
                        version->setEnabled(true);
                        update();
                        return;
                    }
                    service_.invalidateMetadata(profile.id, profile.database);
                    navigation_->queueRefresh(profile.id);
                    status_->setText("数据库属性已更新");
                    dialog->close();
                });
            });
    });
}

bool MainWindow::databaseConsoleHasTransaction(QWidget* widget) const {
    auto *pane = dynamic_cast<DatabaseConsole*>(widget);
    return pane && (pane->transactionState == navidog::TransactionState::Active ||
                    pane->transactionState == navidog::TransactionState::Failed);
}
