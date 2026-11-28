#include "navidog/database_sql.hpp"
#include "query_pane.hpp"
#include "editor_sql_tools.hpp"
#include "navidog/operation_timing.hpp"
#include <algorithm>
#include <QDateTime>
#include <QMessageBox>
#include <QFileInfo>
#include <QSignalBlocker>
#include <QRunnable>
#include <QThreadPool>
#include <QThread>
#include <atomic>
#include <utility>

namespace {
constexpr int draftByteLimit = navidog::editableScriptBytes * 2;
constexpr int maxEditableSqlLines = 2'000'000;
constexpr int maxEditableSqlLineLength = 2 * 1024 * 1024;

bool validateEditorSize(const QString &source) {
    int lines = 1;
    int lineStart = 0;
    bool longLine = false;
    for (int i = 0; i < source.size(); ++i) {
        if (source.at(i) == QChar('\n') || source.at(i) == QChar::ParagraphSeparator) {
            if (++lines > maxEditableSqlLines)
                throw std::runtime_error("SQL 文件超过编辑器的 200 万行上限");
            lineStart = i + 1;
        } else if (i - lineStart >= maxEditableSqlLineLength) {
            throw std::runtime_error("SQL 文件有单行超过 2 Mi 字符，无法在当前编辑器中流畅显示");
        } else if (i - lineStart >= 512 * 1024) {
            longLine = true;
        }
    }
    return longLine || lines > SqlEditor::largeLineCount ||
           source.size() > SqlEditor::largeTextThreshold;
}

struct ScriptReadResult {
    navidog::ScriptDocument document;
    std::unique_ptr<QTextDocument, void (*)(QTextDocument *)> prepared{
        nullptr, [](QTextDocument *doc) { doc->deleteLater(); }};
    QString error;
};

class ScriptReadTask final : public QObject, public QRunnable {
    Q_OBJECT
public:
    QString path;
    int byteLimit = navidog::editableScriptBytes;
    QThread *deliveryThread = nullptr;
    QFont editorFont;
    QTextOption editorTextOptions;
    std::shared_ptr<std::atomic_bool> canceled = std::make_shared<std::atomic_bool>(false);
    std::shared_ptr<ScriptReadResult> result = std::make_shared<ScriptReadResult>();

    void run() override {
        if (canceled->load())
            return;
        try {
            const navidog::OperationTimer elapsed(navidog::TimedOperation::ReadSqlFile);
            const auto size = QFileInfo(path).size();
            if (size > SqlEditor::largeTextThreshold) {
                const auto prefix = navidog::readScriptPrefix(path);
                if (!prefix.isEmpty())
                    emit previewReady(prefix);
            }
            auto source = navidog::readScriptDocument(path, byteLimit);
            const bool large = validateEditorSize(source.text) ||
                               source.byteSize > SqlEditor::largeTextThreshold;
            if (canceled->load())
                return;
            if (large) {
                auto document = std::make_unique<QTextDocument>();
                document->setDocumentMargin(0);
                document->setUndoRedoEnabled(false);
                document->setDefaultFont(editorFont);
                document->setDefaultTextOption(editorTextOptions);
                document->setPlainText(source.text);
                source.text.clear();
                // Allocate native block layouts before QPlainTextEdit's first relayout.
                // This prepares text structures without shaping every line.
                document->setDocumentLayout(new QPlainTextDocumentLayout(document.get()));
                for (auto block = document->firstBlock(); block.isValid(); block = block.next())
                    block.layout();
                if (canceled->load())
                    return;
                // The document remains detached from widgets until it reaches the GUI.
                document->moveToThread(deliveryThread);
                result->prepared.reset(document.release());
            }
            result->document = std::move(source);
        } catch (const std::exception &error) {
            result->error = QString::fromUtf8(error.what());
        }
        emit finished();
    }
signals:
    void previewReady(const QString &prefix);
    void finished();
};

QThreadPool &scriptReadPool() {
    static const auto pool = [] {
        // Keep the operation log alive until the file worker has stopped.
        navidog::operationLogSnapshot();
        auto worker = std::make_unique<QThreadPool>();
        worker->setMaxThreadCount(1);
        return worker;
    }();
    return *pool;
}

navidog::sql::Dialect dialectFor(navidog::DatabaseKind driver) {
    return navidog::scriptDialect(driver);
}
} // namespace

void QueryPane::connectControls() {
    connect(editor, &SqlEditor::largeDocumentChanged, this, [this] {
        updateScriptPolicy();
        updateSqlActions();
    });
    connect(editor, &QPlainTextEdit::selectionChanged, this, &QueryPane::updateSqlActions);
    connect(run, &QToolButton::clicked, this, &QueryPane::runRequested);
    connect(runStatement, &QToolButton::clicked, this, &QueryPane::runStatementRequested);
    connect(stop, &QToolButton::clicked, this, [this] {
        stop->setEnabled(false);
        emit stopRequested();
    });
    connect(format, &QToolButton::clicked, this, &QueryPane::formatRequested);
    connect(exportButton, &QToolButton::clicked, this, &QueryPane::exportRequested);
    connect(explain, &QToolButton::clicked, this, &QueryPane::explainCurrent);
    connect(editor, &SqlEditor::conversionRequested, this,
            &QueryPane::requestConversion);
    connect(editor, &SqlEditor::optimizationRequested, this,
            &QueryPane::requestOptimization);
    connect(connection, QOverload<int>::of(&QComboBox::currentIndexChanged), this,
            [this] { emit contextChangeRequested(QueryContextPart::Connection); });
    connect(databaseContext, QOverload<int>::of(&QComboBox::activated), this,
            &QueryPane::requestDatabaseChange);
    connect(schemaContext, QOverload<int>::of(&QComboBox::activated), this,
            &QueryPane::requestSchemaChange);
    databaseContext->opening = [this] {
        if (databaseContext->property("databaseSelectable").toBool())
            emit namespaceListRequested(false);
    };
    schemaContext->opening = [this] {
        if (schemaContext->property("schemaSelectable").toBool())
            emit namespaceListRequested(true);
    };
    connect(beginTransaction, &QToolButton::clicked, this, [this] {
        {
            const QSignalBlocker blocker(autoCommit);
            autoCommit->setChecked(false);
        }
        emit transactionRequested(navidog::TransactionAction::Begin);
    });
    connect(commitTransaction, &QToolButton::clicked, this,
            [this] { emit transactionRequested(navidog::TransactionAction::Commit); });
    connect(rollbackTransaction, &QToolButton::clicked, this,
            [this] { emit transactionRequested(navidog::TransactionAction::Rollback); });
    connect(autoCommit, &QCheckBox::toggled, this, &QueryPane::toggleAutoCommit);
}

void QueryPane::requestConversion() { openSqlTool(EditorSqlTool::Convert); }

void QueryPane::requestOptimization() { openSqlTool(EditorSqlTool::Optimize); }

void QueryPane::openSqlTool(EditorSqlTool tool) {
    if (sqlTools_ && kind == navidog::WorkspaceTabKind::Query &&
        !property("closingWorkspace").toBool() && allowSqlAction())
        sqlTools_->open(*this, *editor, tool, profileId, database, schema);
}

void QueryPane::requestDatabaseChange() {
    emit contextChangeRequested(QueryContextPart::Database);
}

void QueryPane::requestSchemaChange() {
    emit contextChangeRequested(QueryContextPart::Schema);
}

void QueryPane::setContextEditable(bool databaseEditable, bool schemaEditable) {
    databaseContext->setProperty("databaseSelectable", databaseEditable);
    databaseContext->setEditable(databaseEditable);
    databaseContext->setInsertPolicy(QComboBox::NoInsert);
    schemaContext->setVisible(schemaEditable);
    schemaContext->setProperty("schemaSelectable", schemaEditable);
    schemaContext->setEditable(schemaEditable);
    schemaContext->setInsertPolicy(QComboBox::NoInsert);
    // setEditable can replace the line edit when a connection changes dialect.
    if (auto *edit = databaseContext->lineEdit())
        connect(edit, &QLineEdit::editingFinished, this,
                &QueryPane::requestDatabaseChange, Qt::UniqueConnection);
    if (auto *edit = schemaContext->lineEdit())
        connect(edit, &QLineEdit::editingFinished, this,
                &QueryPane::requestSchemaChange, Qt::UniqueConnection);
}

void QueryPane::toggleAutoCommit(bool enabled) {
    const auto active = transactionState == navidog::TransactionState::Active ||
                        transactionState == navidog::TransactionState::Failed;
    if (enabled && active) {
        const auto answer = QMessageBox::question(
            window(), "切换自动提交",
            "启用自动提交前是否提交当前事务？选择“否”将回滚。",
            QMessageBox::Yes | QMessageBox::No | QMessageBox::Cancel,
            QMessageBox::Cancel);
        if (answer == QMessageBox::Cancel) {
            const QSignalBlocker blocker(autoCommit);
            autoCommit->setChecked(false);
            return;
        }
        emit transactionRequested(answer == QMessageBox::Yes
            ? navidog::TransactionAction::Commit : navidog::TransactionAction::Rollback);
    }
    emit autoCommitChanged();
}

void QueryPane::setConnectionCapabilities(navidog::DatabaseKind driver,
                                          bool transactionsSupported) {
    statementDriver_ = driver;
    transactionsSupported_ = transactionsSupported;
}

bool QueryPane::canRun(bool exclusiveOperation) const {
    return scriptReady() && !exclusiveOperation && !executing && !safetyReviewPending_ &&
        !property("closingWorkspace").toBool();
}

void QueryPane::setSafetyReviewPending(bool pending) {
    safetyReviewPending_ = pending;
    updateWorkspaceActivity(activity_);
}

void QueryPane::updateWorkspaceActivity(Activity activity) {
    activity_ = activity;
    const bool interactive = canRun(activity.exclusiveOperation);
    databaseContext->setEnabled(interactive && databaseContext->property("databaseSelectable").toBool());
    schemaContext->setEnabled(interactive && schemaContext->property("schemaSelectable").toBool());
    connection->setEnabled(interactive && kind == navidog::WorkspaceTabKind::Query);
    stop->setEnabled(executing || safetyReviewPending_);
    updateTransaction(!interactive);
    updateSqlActions();
}

void QueryPane::updateSqlActions() {
    const bool selected = !editor->largeDocument() || editor->hasBoundedSqlSelection();
    const bool sourceAvailable = scriptReady() && selected;
    const bool interactive = canRun(activity_.exclusiveOperation);
    run->setEnabled(interactive && sourceAvailable);
    runStatement->setEnabled(interactive && !editor->largeDocument());
    const bool command = navidog::isCommandDatabase(statementDriver_);
    const bool hasCompiler = !command && statementDriver_ != navidog::DatabaseKind::Dameng;
    parseAst->setEnabled(interactive && sourceAvailable && hasCompiler);
    explain->setEnabled(interactive && sourceAvailable &&
                        (!command || statementDriver_ == navidog::DatabaseKind::MongoDb));
    format->setEnabled(sourceAvailable && hasCompiler);
    conversion->setEnabled(sourceAvailable && hasCompiler);
    optimize->setEnabled(sourceAvailable && !command);
    if (auto *save = findChild<QToolButton *>("saveSqlFile"))
        save->setEnabled(scriptReady());
    if (auto *library = findChild<QToolButton *>("saveSqlToLibrary"))
        library->setEnabled(scriptReady() && !editor->largeDocument());
}

bool QueryPane::allowSqlAction() {
    if (!scriptReady()) {
        emit statusRequested("请等待 SQL 文件装入完毕");
        return false;
    }
    if (editor->largeDocument() && !editor->hasBoundedSqlSelection()) {
        emit statusRequested("大文件模式下，请选中至多 256 Ki 字符的 SQL 再操作");
        return false;
    }
    return true;
}

void QueryPane::updateScriptPolicy(const QString &progress) {
    if (!progress.isEmpty()) {
        scriptPolicy_->setText(progress);
        scriptPolicy_->show();
    } else if (!scriptReady()) {
        scriptPolicy_->setText("正在读取 SQL 文件…");
        scriptPolicy_->show();
    } else if (editor->largeDocument()) {
        scriptPolicy_->setText("大文件模式 · 可见行快速高亮 · 已暂停实时检查、补全和自动换行 · 运行与分析仅处理选区");
        scriptPolicy_->setToolTip("保留编辑、撤销、查找和保存。超长行仅高亮前 8192 个字符；格式化、转换、解释和运行需先选中至多 256 Ki 字符。");
        scriptPolicy_->show();
    } else {
        scriptPolicy_->hide();
    }
}

QString QueryPane::statementAtCursor(int *documentStart) const {
    if (documentStart) *documentStart = 0;
    if (statementDriver_ == navidog::DatabaseKind::MongoDb) {
        if (documentStart) *documentStart = editor->textCursor().selectionStart();
        return editor->selectedSqlOrAll();
    }
    if (!scriptReady() || editor->largeDocument())
        return {};
    if (statementDriver_ == navidog::DatabaseKind::Redis) {
        auto cursor = editor->textCursor();
        cursor.select(QTextCursor::LineUnderCursor);
        if (documentStart) *documentStart = cursor.selectionStart();
        return cursor.selectedText();
    }
    const auto sql = editor->toPlainText();
    const auto bytes = sql.toUtf8();
    const auto spans = navidog::sql::splitStatements(bytes.toStdString(),
                                                     dialectFor(statementDriver_));
    if (spans.empty())
        return {};
    const auto offset = std::size_t(sql.left(editor->textCursor().position()).toUtf8().size());
    const auto span = std::find_if(spans.begin(), spans.end(), [offset](const auto &entry) {
        return offset <= entry.end;
    });
    const auto &current = span == spans.end() ? spans.back() : *span;
    if (documentStart) *documentStart = int(QString::fromUtf8(bytes.constData(), int(current.begin)).size());
    return QString::fromUtf8(bytes.mid(current.begin, current.end - current.begin));
}

void QueryPane::explainCurrent() {
    if (kind != navidog::WorkspaceTabKind::Query ||
        statementDriver_ == navidog::DatabaseKind::Unknown || !allowSqlAction())
        return;

    auto sql = editor->textCursor().selectedText();
    int begin = editor->textCursor().selectionStart();
    sql.replace(QChar::ParagraphSeparator, '\n');
    if (sql.trimmed().isEmpty()) {
        try {
            sql = statementAtCursor(&begin);
        } catch (const std::exception &error) {
            emit errorRequested("SQL 无法解释", QString::fromUtf8(error.what()));
            return;
        }
    }
    if (sql.isEmpty()) {
        emit statusRequested("当前位置没有可解释的 SQL");
        return;
    }
    emit explainRequested(sql, begin);
}

QString QueryPane::resultSummary(const navidog::QueryResult &result) {
    if (result.cancelled)
        return "查询已停止";
    if (!result.error.isEmpty())
        return "执行失败：" + result.error;
    if (result.columns.isEmpty())
        return QString("执行成功 · 影响 %1 行 · %2 ms")
            .arg(result.affectedRows).arg(result.elapsedMs);
    return QString("%1 行 · %2 ms%3%4")
        .arg(result.rows.size()).arg(result.elapsedMs)
        .arg(result.truncated ? " · 已到缓存上限" : "")
        .arg(result.cellTruncated ? " · 部分单元格已截断" : "");
}

bool QueryPane::hasActiveTransaction() const {
    return transactionState == navidog::TransactionState::Active ||
           transactionState == navidog::TransactionState::Failed;
}

void QueryPane::resetSession() {
    sessionId = QUuid::createUuid().toString(QUuid::Id128);
    sessionDatabase.clear();
    sessionContextChanged = false;
    transactionState = navidog::TransactionState::Idle;
    executing = false;
    updateTransaction(false);
}

void QueryPane::startExecution(bool analyzeOnly, navidog::TransactionAction action) {
    executing = true;
    if (action != navidog::TransactionAction::None)
        return;
    if (!analyzeOnly)
        summary->setText("正在执行…");
    analysis->setPlainText(analyzeOnly ? "正在本地解析 · SQL 未执行" : "正在解析和执行…");
    astView->reset();
}

void QueryPane::showReturnedDefinition(const navidog::QueryOutcome &outcome) {
    if (kind != navidog::WorkspaceTabKind::Definition || outcome.result.cancelled ||
        !outcome.result.error.isEmpty())
        return;
    const auto &result = outcome.results.isEmpty() ? outcome.result : outcome.results.front();
    if (result.rows.size() != 1)
        return;
    for (int i = 0; i < result.columns.size(); ++i) {
        const auto column = result.columns.at(i);
        if (column.compare("sql", Qt::CaseInsensitive) != 0 &&
            column.compare("definition", Qt::CaseInsensitive) != 0 &&
            column.compare("create_statement", Qt::CaseInsensitive) != 0 &&
            !(statementDriver_ == navidog::DatabaseKind::ClickHouse && column == "statement") &&
            !column.startsWith("Create ", Qt::CaseInsensitive))
            continue;
        const auto sql = result.rows.front().value(i).toString();
        if (sql.trimmed().isEmpty())
            continue;
        auto *preview = new SqlReadOnlyView;
        preview->setObjectName("returnedDefinitionSql");
        preview->setDatabaseKind(statementDriver_);
        preview->setSourceSql(sql);
        preview->setFormattingEnabled(!result.cellTruncated);
        auto *page = preview->withActions();
        resultTabs->insertTab(0, page, "对象 SQL");
        resultTabs->setCurrentWidget(page);
        return;
    }
}

void QueryPane::applyOutcome(const navidog::QueryOutcome &outcome,
                             const QString &summaryText,
                             const navidog::ConnectionProfile *profile,
                             const QueryMessageSource& source) {
    executing = false;
    sessionContextChanged |= outcome.sessionContextChanged;
    if (outcome.analyzedOnly) {
        showResults(outcome);
        messages->appendPlainText(outcome.result.cancelled
            ? "本地解析已停止；SQL 未执行" : "本地 AST 解析完成；SQL 未执行");
        return;
    }

    const bool hasResults = outcome.transactionAction == navidog::TransactionAction::None;
    for (const auto& statement : outcome.statementTrace)
        messages->appendSql(QString("执行 SQL · 第 %1 条").arg(statement.index + 1),
                            statement.sql, source, statement.begin, statement.end);
    if (outcome.explained)
        for (const auto& command : outcome.estimatedPlan.request.commands)
            messages->appendSql("执行计划请求", navidog::queryTraceText(command), source, 0, source.length);
    if (hasResults)
        summary->setText(summaryText);
    messages->appendPlainText(QDateTime::currentDateTime().toString("HH:mm:ss") +
                              "  " + summaryText);
    if (hasResults) {
        resultExportContext = profile ? navidog::ui::exportContext(*profile, outcome.database, schema)
                                      : scriptExportContext();
        resultExportDriver = profile ? profile->driver : navidog::DatabaseKind::SQLite;
        showResults(outcome);
    }
    transactionState = outcome.transactionState;
    if (transactionState == navidog::TransactionState::Active ||
        transactionState == navidog::TransactionState::Failed) {
        const QSignalBlocker blocker(autoCommit);
        autoCommit->setChecked(false);
    }
    if (outcome.executedStatements > 1)
        messages->appendPlainText(QString("已执行 %1 条语句 · %2 个结果")
            .arg(outcome.executedStatements).arg(outcome.results.size()));
    if (outcome.sessionClosed)
        messages->appendPlainText("此查询会话已关闭；未提交事务已终止。提交时断线的结果请重新查询确认。");
}

bool QueryPane::dirty() const {
    if (!scriptReady())
        return restoreModified_;
    return editor->document()->isModified() ||
        (!editor->largeDocument() && !script.path.isEmpty() && script.text != editor->toPlainText());
}

void QueryPane::setScriptDocument(const navidog::ScriptDocument &document) {
    script = document;
    editor->setPlainText(document.text);
    finishScriptLoad();
}

void QueryPane::finishScriptLoad() {
    loadingScript_ = false;
    restoringScript_ = false;
    editor->document()->setModified(restoreModified_);
    restoreModified_ = false;
    draftId_.clear();
    editor->setReadOnly(false);
    editor->setPlaceholderText({});
    emit documentChanged();
    updateScriptPolicy();
    updateWorkspaceActivity(activity_);
    emit statusRequested("已打开 SQL 文件，尚未执行");
}

void QueryPane::openScriptFile(const QString &path) {
    script.path = path;
    pendingReadPath_ = path;
    restoringScript_ = true;
    editor->setReadOnly(true);
    editor->clear();
    updateScriptPolicy();
    updateWorkspaceActivity(activity_);
    emit documentChanged();
    if (isVisible())
        startReadingScript();
}

void QueryPane::restoreLargeScript(const QJsonObject &state, const QString &draftPath) {
    restoreScriptState(state);
    draftId_ = state["draft"].toString();
    restoreModified_ = state["file"].toObject()["modified"].toBool();
    restoringScript_ = true;
    pendingReadPath_ = draftId_.isEmpty() ? script.path : draftPath;
    editor->setReadOnly(true);
    editor->setPlaceholderText("切换到此标签后加载 SQL 文件");
    updateScriptPolicy(pendingReadPath_.isEmpty() ? "无法找到 SQL 草稿"
                                                  : "大文件 · 切换到此标签后加载");
    updateWorkspaceActivity(activity_);
    if (isVisible())
        startReadingScript();
}

void QueryPane::showEvent(QShowEvent *event) {
    WorkspacePane::showEvent(event);
    startReadingScript();
}

void QueryPane::startReadingScript() {
    if (pendingReadPath_.isEmpty() || loadingScript_)
        return;
    loadingScript_ = true;
    const auto path = std::exchange(pendingReadPath_, {});
    updateScriptPolicy("正在读取 SQL 文件…");
    auto *task = new ScriptReadTask;
    task->path = path;
    task->deliveryThread = thread();
    task->editorFont = editor->font();
    task->editorTextOptions = editor->document()->defaultTextOption();
    task->editorTextOptions.setWrapMode(QTextOption::NoWrap);
    if (!draftId_.isEmpty())
        task->byteLimit = draftByteLimit;
    const auto lifetime = connect(this, &QObject::destroyed, [flag = task->canceled] { flag->store(true); });
    connect(task, &ScriptReadTask::previewReady, this, [this](const QString &prefix) {
        editor->setLineWrapMode(QPlainTextEdit::NoWrap);
        editor->setPlainText(prefix);
        updateScriptPolicy("已显示文件开头 · 正在后台装入全文，完成后可编辑");
    }, Qt::QueuedConnection);
    connect(task, &ScriptReadTask::finished, this, [this, result = task->result, lifetime] {
        disconnect(lifetime);
        if (!result->error.isEmpty()) {
            loadingScript_ = false;
            updateScriptPolicy((editor->document()->isEmpty() ? QString("SQL 文件未能打开 · ")
                                                             : QString("仅预览文件开头 · ")) + result->error);
            return;
        }
        if (result->prepared) {
            if (draftId_.isEmpty())
                script = std::move(result->document);
            editor->installLargeDocument(result->prepared.release());
            finishScriptLoad();
        } else if (draftId_.isEmpty()) {
            setScriptDocument(result->document);
        } else {
            editor->setPlainText(result->document.text);
            finishScriptLoad();
        }
    }, Qt::QueuedConnection);
    scriptReadPool().start(task);
}

void QueryPane::restoreScriptState(const QJsonObject &state) {
    const auto file = state["file"].toObject();
    script.path = file["path"].toString();
    script.text = editor->toPlainText();
    script.fingerprint = QByteArray::fromBase64(file["fingerprint"].toString().toLatin1());
    script.encoding = static_cast<navidog::ScriptEncoding>(std::clamp(file["encoding"].toInt(), 0, 3));
    script.crlf = file["crlf"].toBool();
    editor->document()->setModified(file["modified"].toBool());
    emit documentChanged();
}

bool QueryPane::externalScriptState() const {
    return editor->largeDocument() || !scriptReady();
}

bool QueryPane::needsExternalDraft() const {
    return scriptReady() && editor->largeDocument() && (dirty() || script.path.isEmpty());
}

QJsonObject QueryPane::sessionState() const {
    QJsonObject state{
        {"file", QJsonObject{{"path", script.path},
                             {"fingerprint", QString::fromLatin1(script.fingerprint.toBase64())},
                             {"encoding", static_cast<int>(script.encoding)},
                             {"crlf", script.crlf}, {"modified", dirty()}}},
        {"resultLayout", resultSplitter->orientation() == Qt::Horizontal ? "beside" : "below"},
        {"editorFocus", editorFocus}, {"outlineOpen", editorArea_->outlineOpen()},
        {"autoCommit", autoCommit->isChecked()},
        {"kind", kind == navidog::WorkspaceTabKind::Preview ? "preview" :
                 kind == navidog::WorkspaceTabKind::Definition ? "definition" : "query"},
        {"profileId", profileId}, {"database", database}, {"schema", schema},
        {"rowLimit", limit->currentData().toInt()}};
    if (externalScriptState()) {
        state["externalSql"] = true;
        if (!draftId_.isEmpty())
            state["draft"] = draftId_;
    } else {
        state["sql"] = editor->toPlainText();
    }
    return state;
}
navidog::ui::ExportContext QueryPane::scriptExportContext() const {
    auto context = selectedExportContext;
    context.connection = connection->itemText(connection->findData(profileId));
    return context;
}
bool QueryPane::saveSqlFile(bool saveAs){
    if (kind != navidog::WorkspaceTabKind::Query || !scriptReady()) return false;
    QString path=script.path;bool chosen=saveAs || path.isEmpty();if(chosen){path=navidog::ui::chooseExportFile(this,"保存 SQL 文件",scriptExportContext(),"查询脚本","SQL 文件 (*.sql);;所有文件 (*)");if(path.isEmpty())return false;}
    try{
        try{script=navidog::writeScriptDocument(script,path,editor->toPlainText(),chosen,navidog::editableScriptBytes);}
        catch(const navidog::ScriptConflict&){if(QMessageBox::question(this,"SQL 文件已改变","磁盘上的文件已被其他程序修改或删除。是否用当前编辑内容覆盖？",QMessageBox::Yes|QMessageBox::Cancel,QMessageBox::Cancel)!=QMessageBox::Yes)return false;script=navidog::writeScriptDocument(script,path,editor->toPlainText(),true,navidog::editableScriptBytes);}
        if (editor->largeDocument()) script.text.clear();
        editor->document()->setModified(false);emit documentChanged();return true;
    }catch(const std::exception& e){QMessageBox::warning(this,"保存 SQL 失败",QString::fromUtf8(e.what()));return false;}
}

#include "query_pane.moc"
