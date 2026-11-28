#pragma once
#include "workspace_pane.hpp"
#include "components.hpp"
#include "editor.hpp"
#include "sql_outline.hpp"
#include "result_model.hpp"
#include "navidog/script_document.hpp"
#include "navidog/query_analysis.hpp"
#include "navidog/sql.hpp"
#include "sql_ast_view.hpp"
#include "estimated_plan_view.hpp"
#include "export_paths.hpp"
#include <QCryptographicHash>
#include <QApplication>
#include <QClipboard>
#include <QHeaderView>
#include <QHBoxLayout>
#include <QVBoxLayout>
#include <QLabel>
#include <QTableView>
#include "result_grid.hpp"
#include <QTabWidget>
#include <QSplitter>
#include <QShortcut>
#include <QUuid>
#include <algorithm>
enum class QueryContextPart { Connection, Database, Schema };
class EditorSqlTools;

class QueryPane : public WorkspacePane {
    Q_OBJECT
    static QToolButton* button(const QString& text,const QString& name={}){return new navidog::ui::Button(text,name);}
    void connectControls();
    void requestConversion();
    void requestOptimization();
    void requestDatabaseChange();
    void requestSchemaChange();
    void toggleAutoCommit(bool enabled);
    void showReturnedDefinition(const navidog::QueryOutcome &);
    navidog::DatabaseKind statementDriver_ = navidog::DatabaseKind::Unknown;
    QWidget *generatedSqlPage_;
    bool transactionsSupported_ = true;
    const EditorSqlTools *sqlTools_ = nullptr;
    QLabel *scriptPolicy_;
    SqlEditorArea *editorArea_;
    bool loadingScript_ = false;
    bool restoringScript_ = false;
    QString pendingReadPath_;
    QString draftId_;
    bool restoreModified_ = false;
    void startReadingScript();
    void finishScriptLoad();
    void updateScriptPolicy(const QString &progress = {});
    void updateSqlActions();
    Activity activity_{false, false};
public:
    navidog::ScriptDocument script;
    bool dirty()const;
    void restoreOutlineState(const QJsonObject &state) { editorArea_->setOutlineOpen(state["outlineOpen"].toBool()); }
    bool saveSqlFile(bool saveAs=false);
    navidog::ui::ExportContext scriptExportContext() const;
    navidog::ui::ExportContext selectedExportContext, resultExportContext;
    navidog::DatabaseKind resultExportDriver = navidog::DatabaseKind::SQLite;
    void setScriptDocument(const navidog::ScriptDocument&);
    void openScriptFile(const QString &path);
    void restoreLargeScript(const QJsonObject &state, const QString &draftPath);
    bool scriptReady() const { return !loadingScript_ && !restoringScript_; }
    bool allowSqlAction();
    bool externalScriptState() const;
    bool needsExternalDraft() const;
    void restoreScriptState(const QJsonObject&);
    void setContextEditable(bool database, bool schema);
    void setConnectionCapabilities(navidog::DatabaseKind driver, bool transactionsSupported);
    bool canRun(bool exclusiveOperation) const;
    void updateWorkspaceActivity(Activity activity) override;
    QString statementAtCursor() const;
    void explainCurrent();
    static QString resultSummary(const navidog::QueryResult &);
    bool hasActiveTransaction() const;
    void resetSession();
    void startExecution(bool analyzeOnly, navidog::TransactionAction);
    void setSqlTools(const EditorSqlTools &tools) { sqlTools_ = &tools; }
    void openSqlTool(EditorSqlTool) override;
    void applyOutcome(const navidog::QueryOutcome &, const QString &summaryText,
                      const navidog::ConnectionProfile *profile);
    SqlEditor* editor;
    ResultModel* model;
    navidog::ui::ObjectGrid* table;
    QPlainTextEdit* messages;
    QPlainTextEdit* analysis;
    SqlReadOnlyView* generatedSql;
    SqlAstView* astView;
    EstimatedPlanView* planView;
    QToolButton* parseAst;
    bool sessionContextChanged=false;
    QLabel* summary;
    QTabWidget* resultTabs;
    QString sessionId=QUuid::createUuid().toString(QUuid::Id128),sessionDatabase;
    QString schema;
    navidog::TransactionState transactionState=navidog::TransactionState::Idle;
    bool executing=false,editorFocus=false,changingContext=false;
    QSplitter* resultSplitter;
    QWidget* resultArea;
    QToolButton* focusEditor;
    QCheckBox* autoCommit;
    QToolButton *beginTransaction,*commitTransaction,*rollbackTransaction,*runStatement;
    QLabel* transactionStatus;
    QComboBox *connection,*limit;
    navidog::ui::ContextSelect *databaseContext,*schemaContext;
    QToolButton *run,*stop,*format,*exportButton,*explain,*conversion,*optimize;
    explicit QueryPane(const QVector<navidog::ConnectionProfile>& profiles,navidog::WorkspaceTabKind tabKind,QWidget* parent = nullptr) : WorkspacePane(parent) {
        kind=tabKind;setObjectName("queryPane");
        auto* layout = new QVBoxLayout(this); layout->setContentsMargins(0, 0, 0, 0); layout->setSpacing(0);
        auto* viewToolbar=new navidog::ui::OverflowStrip;viewToolbar->setObjectName("queryViewToolbar");viewToolbar->setSizePolicy(QSizePolicy::Ignored,QSizePolicy::Fixed);auto* views=viewToolbar->row();views->setContentsMargins(6,2,6,2);views->setSpacing(2);
        auto* openScript=button("打开 SQL…","openSqlFile");auto* saveScript=button("保存 SQL","saveSqlFile");auto* saveToLibrary=button("保存到查询库…","saveSqlToLibrary");views->addWidget(openScript);views->addWidget(saveScript);views->addWidget(saveToLibrary);connect(openScript,&QToolButton::clicked,this,&QueryPane::openSqlRequested);connect(saveScript,&QToolButton::clicked,this,[this]{saveSqlFile();});connect(saveToLibrary,&QToolButton::clicked,this,&QueryPane::saveInLibraryRequested);
        format=button("格式化 SQL","formatQuery");views->addWidget(format);conversion=button("转换 SQL…","convertQuery");views->addWidget(conversion);optimize=button("本地优化…","optimizeQuery");views->addWidget(optimize);connect(optimize,&QToolButton::clicked,this,[this]{ requestOptimization(); });connect(conversion,&QToolButton::clicked,this,[this]{ requestConversion(); });parseAst=button("解析 AST","parseAst");parseAst->setToolTip("仅本地解析选中或全部 SQL，不连接数据库、不执行语句。");views->addWidget(parseAst);connect(parseAst,&QToolButton::clicked,this,&QueryPane::analyzeRequested);auto* findSql=button("查找","findSql");views->addWidget(findSql);connect(findSql,&QToolButton::clicked,this,[this]{editor->showFind();});
        auto *outlineToggle = button("结构导航", "toggleSqlOutline");
        views->addWidget(outlineToggle);
        explain=button("解释","explainQuery");
        explain->setToolTip("连接当前数据库并发送 EXPLAIN / SHOWPLAN，读取服务端估计计划，不执行原语句。使用独立会话，无法查看本页的临时表或未提交结构。");
        views->addWidget(explain);
        views->addStretch();auto* below=button("结果在下方","resultsBelow");auto* beside=button("结果在右侧","resultsBeside");focusEditor=button("专注编辑","focusQueryEditor");views->addWidget(below);views->addWidget(beside);views->addWidget(focusEditor);viewToolbar->setVisible(kind==navidog::WorkspaceTabKind::Query);layout->addWidget(viewToolbar);
        connect(below,&QToolButton::clicked,this,[this]{setResultsLayout(false);});connect(beside,&QToolButton::clicked,this,[this]{setResultsLayout(true);});connect(focusEditor,&QToolButton::clicked,this,[this]{setEditorFocus(!editorFocus);});
        auto* toolbar=new QWidget;toolbar->setObjectName("queryToolbar");toolbar->setSizePolicy(QSizePolicy::Preferred,QSizePolicy::Fixed);auto* tools=new QHBoxLayout(toolbar);tools->setContentsMargins(8,3,8,3);tools->setSpacing(7);
        connection=new navidog::ui::Select;connection->setObjectName("activeConnection");connection->setMinimumWidth(180);for(const auto& p:profiles)connection->addItem(p.name,p.id);tools->addWidget(connection);
        databaseContext=new navidog::ui::ContextSelect;databaseContext->setObjectName("queryDatabase");databaseContext->setAccessibleName("数据库");databaseContext->setEditable(true);databaseContext->setMinimumWidth(130);databaseContext->setMaximumWidth(200);tools->addWidget(databaseContext);
        schemaContext=new navidog::ui::ContextSelect;schemaContext->setObjectName("querySchema");schemaContext->setAccessibleName("Schema");schemaContext->setEditable(true);schemaContext->setMinimumWidth(120);schemaContext->setMaximumWidth(180);tools->addWidget(schemaContext);
        run=button(kind==navidog::WorkspaceTabKind::Preview?"刷新数据":kind==navidog::WorkspaceTabKind::Definition?"读取定义":"▶ 运行","runQuery");tools->addWidget(run);
        runStatement=button("运行当前语句","runStatement");runStatement->setVisible(kind==navidog::WorkspaceTabKind::Query);tools->addWidget(runStatement);
        stop=button("■ 停止","stopQuery");stop->setEnabled(false);tools->addWidget(stop);
        tools->addStretch();
        auto* cap=new QLabel("缓存上限");cap->setObjectName("muted");tools->addWidget(cap);
        limit=new navidog::ui::Select;limit->setObjectName("rowLimit");for(int n:{1000,5000,10000})limit->addItem(QString::number(n)+" 行",n);tools->addWidget(limit);
        exportButton=button("导出…","exportCsv");tools->addWidget(exportButton);layout->addWidget(toolbar);
        auto* transactionTools=new QWidget;transactionTools->setObjectName("queryTransactionToolbar");transactionTools->setSizePolicy(QSizePolicy::Preferred,QSizePolicy::Fixed);auto* transactionLayout=new QHBoxLayout(transactionTools);transactionLayout->setContentsMargins(12,3,12,4);transactionLayout->setSpacing(8);autoCommit=new navidog::ui::CheckBox("自动提交");autoCommit->setObjectName("queryAutoCommit");autoCommit->setChecked(true);transactionLayout->addWidget(autoCommit);beginTransaction=button("开始事务","beginTransaction");commitTransaction=button("提交","commitTransaction");rollbackTransaction=button("回滚","rollbackTransaction");transactionLayout->addWidget(beginTransaction);transactionLayout->addWidget(commitTransaction);transactionLayout->addWidget(rollbackTransaction);transactionStatus=new QLabel("无活动事务");transactionStatus->setObjectName("queryTransactionStatus");transactionLayout->addStretch();transactionLayout->addWidget(transactionStatus);transactionTools->setVisible(kind==navidog::WorkspaceTabKind::Query);layout->addWidget(transactionTools);
        scriptPolicy_ = new QLabel;
        scriptPolicy_->setObjectName("sqlScriptPolicy");
        scriptPolicy_->setTextFormat(Qt::PlainText);
        scriptPolicy_->setWordWrap(true);
        scriptPolicy_->setContentsMargins(10, 5, 10, 5);
        scriptPolicy_->setStyleSheet("QLabel#sqlScriptPolicy { background: palette(alternate-base); color: palette(text); }");
        scriptPolicy_->hide();
        layout->addWidget(scriptPolicy_);
        auto* split = new QSplitter(Qt::Vertical);resultSplitter=split;split->setObjectName("queryResultSplitter");split->setChildrenCollapsible(false);
        editor = new SqlEditor; editor->setSqlToolsEnabled(kind==navidog::WorkspaceTabKind::Query,kind==navidog::WorkspaceTabKind::Query); editor->setMinimumHeight(140); editorArea_ = new SqlEditorArea(editor, outlineToggle, false); split->addWidget(editorArea_);connect(editor,&QPlainTextEdit::textChanged,this,[this]{if(scriptReady())emit documentChanged();});connect(editor,&QPlainTextEdit::modificationChanged,this,[this]{if(scriptReady())emit documentChanged();});
        auto* checkStatus=new QLabel("等待检查…");checkStatus->setObjectName("sqlCheckStatus");checkStatus->setToolTip("本地实时语法检查，不连接数据库。加粗红色波浪线表示语法或当前目录中的对象问题；浅红底色和行号红点帮助定位；琥珀色加粗虚线波浪、浅黄底色和行号标记表示解析器暂未覆盖的检查说明。悬停可区分语法错误和可刷新的元数据提示。元数据同步后参与检查；过期 / 不完整缓存不能断言对象不存在，完整类型与权限仍由服务器校验。");
        checkStatus->setMaximumWidth(240);views->insertWidget(6,checkStatus);connect(editor,&SqlEditor::diagnosticStatusChanged,checkStatus,&QLabel::setText);viewToolbar->complete({openScript,saveScript,saveToLibrary,format,conversion,optimize,parseAst,findSql,outlineToggle,explain,below,beside,focusEditor},checkStatus);
        editorArea_->setVisible(kind==navidog::WorkspaceTabKind::Query);editor->setReadOnly(kind!=navidog::WorkspaceTabKind::Query);
        auto* results = new QWidget;resultArea=results;results->setObjectName("queryResultArea");auto* resultLayout = new QVBoxLayout(results); resultLayout->setContentsMargins(0, 0, 0, 0); resultLayout->setSpacing(0);
        auto* resultHeader = new QWidget; resultHeader->setObjectName("resultHeader"); auto* headerLayout = new QHBoxLayout(resultHeader); headerLayout->setContentsMargins(10, 4, 10, 4);
        auto* title = new QLabel("查询结果"); title->setObjectName("sectionTitle"); headerLayout->addWidget(title);
        summary = new QLabel("运行 SQL 后查看结果"); summary->setObjectName("muted"); headerLayout->addStretch(); headerLayout->addWidget(summary); resultLayout->addWidget(resultHeader);
        resultTabs = new QTabWidget; resultTabs->setObjectName("resultTabs");
        table = new navidog::ui::ObjectGrid; table->setObjectName("resultTable"); model = new ResultModel(table); table->setModel(model);
        navidog::ui::configureResultGrid(table);
        messages = new QPlainTextEdit; messages->setObjectName("queryMessages"); messages->setReadOnly(true); messages->setMaximumBlockCount(200);
        messages->setPlaceholderText("执行耗时、数据库错误和停止状态会显示在这里。");
        analysis=new QPlainTextEdit;analysis->setObjectName("queryAnalysis");analysis->setReadOnly(true);analysis->setMaximumBlockCount(400);analysis->setPlaceholderText("运行 SQL 后查看本地方言解析结果。");
        resultTabs->addTab(table, "数据"); resultTabs->addTab(messages, "消息"); resultTabs->addTab(analysis,"SQL 分析");astView=new SqlAstView;resultTabs->addTab(astView,"语法树");generatedSql=new SqlReadOnlyView;generatedSql->setObjectName("generatedSql");generatedSql->setReadOnly(true);generatedSql->setPlaceholderText("解析 AST 或运行后，查看从统一 IR 生成的同方言规范 SQL。此处不会执行 SQL。");generatedSqlPage_=generatedSql->withActions();resultTabs->addTab(generatedSqlPage_,"规范 SQL");planView=new EstimatedPlanView;resultTabs->addTab(planView,"执行计划视图");
        astView->sourceActivated=[this](int begin,int end,const QByteArray& hash){
            if (editor->largeDocument() || !scriptReady()) return;
            const auto bytes=editor->toPlainText().toUtf8();if(hash!=QCryptographicHash::hash(bytes,QCryptographicHash::Sha256) || begin<0 || end<begin || end>bytes.size()) {astView->setToolTip("编辑器内容已变化，或本次只解析了选区；请重新解析全部 SQL 后定位。");return;}
            auto cursor=editor->textCursor();cursor.setPosition(QString::fromUtf8(bytes.left(begin)).size());cursor.setPosition(QString::fromUtf8(bytes.left(end)).size(),QTextCursor::KeepAnchor);editor->setTextCursor(cursor);editor->ensureCursorVisible();
        };
        resultLayout->addWidget(resultTabs);
        results->setMinimumHeight(180); split->addWidget(results); split->setSizes({360, 350}); layout->addWidget(split,1);
        connectControls();
    }
    void updateTransaction(bool busy){
        const bool supported = transactionsSupported_;

        const bool active=transactionState==navidog::TransactionState::Active || transactionState==navidog::TransactionState::Failed;autoCommit->setEnabled(!busy && supported);beginTransaction->setEnabled(!busy && supported && !active);commitTransaction->setEnabled(!busy && supported && transactionState==navidog::TransactionState::Active);rollbackTransaction->setEnabled(!busy && supported && active);
        transactionStatus->setText(!supported?(statementDriver_==navidog::DatabaseKind::Redis?"Redis 命令会话":"HTTP 独立会话"):transactionState==navidog::TransactionState::Failed?"事务已失败 · 请回滚":active?"事务进行中 · 尚未提交":autoCommit->isChecked()?"无活动事务":"手动提交 · 等待执行");
    }
    void setEditorFocus(bool value){editorFocus=value;resultArea->setVisible(!value);focusEditor->setText(value?"恢复结果":"专注编辑");}
    void setResultsLayout(bool horizontal){setEditorFocus(false);resultSplitter->setOrientation(horizontal?Qt::Horizontal:Qt::Vertical);resultSplitter->setSizes(horizontal?QList<int>{600,600}:QList<int>{360,350});}
private:
    void showResults(const navidog::QueryOutcome& outcome){
        if(editorFocus)setEditorFocus(false);
        analysis->setPlainText(navidog::queryAnalysisText(outcome));astView->setReport(outcome.analysis);
        QString generatedText;for(const auto& statement:outcome.analysis.statements){generatedText+=QString("-- 第 %1 条 · %2 · 同方言规范输出\n").arg(statement.index+1).arg(statement.kind);generatedText+=statement.generatedSql.isEmpty()?"-- 未生成："+QString(statement.generationDetail.isEmpty()?statement.detail:statement.generationDetail).replace('\n',' '):statement.generatedSql;generatedText+="\n\n";}generatedSql->setDatabaseKind(statementDriver_);generatedSql->setSourceSql(generatedText);
        if(outcome.analyzedOnly){resultTabs->setCurrentWidget(astView);return;}
        for(int i=resultTabs->count()-1;i>=0;--i){auto* widget=resultTabs->widget(i);if(widget!=table && widget!=messages && widget!=analysis && widget!=astView && widget!=generatedSqlPage_ && widget!=planView){resultTabs->removeTab(i);widget->deleteLater();}}
        planView->setExportContext(resultExportContext);
        planView->setPlan(outcome.explained?outcome.estimatedPlan:navidog::EstimatedPlan{});
        model->setResult({});int count=0;for(const auto& result:outcome.results){if(result.columns.isEmpty())continue;if(count>=32)break;if(count==0){model->setResult(result);resultTabs->setTabText(resultTabs->indexOf(table),outcome.explained?"执行计划":"结果 1");}else{auto* grid=new navidog::ui::ObjectGrid;grid->setObjectName("resultTable");auto* data=new ResultModel(grid);data->setResult(result);grid->setModel(data);navidog::ui::configureResultGrid(grid);resultTabs->insertTab(resultTabs->indexOf(messages),grid,QString("结果 %1").arg(count+1));}++count;}
        if(outcome.results.isEmpty() && outcome.result.error.isEmpty())model->setResult(outcome.result);
        resultTabs->setCurrentWidget(!outcome.result.error.isEmpty() || outcome.result.cancelled || count==0?static_cast<QWidget*>(messages):static_cast<QWidget*>(table));if(outcome.explained && outcome.estimatedPlan.complete)resultTabs->setCurrentWidget(planView);
        showReturnedDefinition(outcome);
    }
public:
    QJsonObject sessionState() const override;
protected:
    void showEvent(QShowEvent *) override;
signals:
    void documentChanged();
    void openSqlRequested();
    void saveInLibraryRequested();
    void explainRequested(const QString &sql);
    void statusRequested(const QString &message);
    void errorRequested(const QString &title, const QString &detail);
    void analyzeRequested();
    void runRequested();
    void runStatementRequested();
    void stopRequested();
    void formatRequested();
    void exportRequested();
    void contextChangeRequested(QueryContextPart);
    void namespaceListRequested(bool schema);
    void transactionRequested(navidog::TransactionAction);
    void autoCommitChanged();
};
