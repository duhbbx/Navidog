#include "source_pane.hpp"
#include "navidog/object_management.hpp"
#include "components.hpp"
#include "editor.hpp"
#include "sql_outline.hpp"
#include "editor_sql_tools.hpp"
#include <QHBoxLayout>
#include <QVBoxLayout>
#include <QTableView>
#include <QTabWidget>
#include <QStandardItemModel>
#include <QHeaderView>
#include <QClipboard>
#include <QApplication>
#include <QLabel>
#include <QSignalBlocker>
#include <QUuid>
namespace {
QJsonObject nodeJson(const navidog::CatalogNode& n){return {{"kind",n.kind},{"name",n.name},{"object",n.object},{"database",n.database},{"schema",n.schema},{"key",n.key},{"detail",n.detail}};}
navidog::CatalogNode readNode(QJsonObject v){navidog::CatalogNode n;n.kind=v["kind"].toString();n.name=v["name"].toString();n.object=v["object"].toString();n.database=v["database"].toString();n.schema=v["schema"].toString();n.key=v["key"].toString();n.detail=v["detail"].toString();return n;}
}
SourcePane::SourcePane(const navidog::ConnectionProfile& p,const navidog::CatalogNode& n,bool existing,QWidget* parent):WorkspacePane(parent),profile(p),node(n){
    setObjectName("sourceDesigner");profileId=p.id;database=n.database;requestId=QUuid::createUuid().toString(QUuid::Id128);baseline.node=n;baseline.exists=existing;loaded=!existing;kind=navidog::WorkspaceTabKind::Definition;
    auto* layout=new QVBoxLayout(this);layout->setContentsMargins(0,0,0,0);layout->setSpacing(0);auto* tools=new QWidget;auto* toolbar=new QHBoxLayout(tools);toolbar->setContentsMargins(10,6,10,6);save=new navidog::ui::Button("保存","saveSource");reload=new navidog::ui::Button("重新读取","reloadSource");run=new navidog::ui::Button(navidog::isSourceRelation(n.kind)?"查看数据":"生成调用 SQL","callSource");stop=new navidog::ui::Button("停止","stopSource");copy=new navidog::ui::Button("复制定义","copySource");toolbar->addWidget(save);toolbar->addWidget(reload);toolbar->addWidget(run);toolbar->addWidget(stop);auto* format=new navidog::ui::Button("格式化 SQL","formatSource");toolbar->addWidget(format);auto* convert=new navidog::ui::Button("转换 SQL…","convertSource");toolbar->addWidget(convert);auto *outlineToggle = new navidog::ui::Button("结构导航", "toggleSqlOutline");toolbar->addWidget(outlineToggle);connect(convert,&QToolButton::clicked,this,[this]{openSqlTool(EditorSqlTool::Convert);});connect(format,&QToolButton::clicked,this,&SourcePane::formatRequested);toolbar->addStretch();toolbar->addWidget(copy);layout->addWidget(tools);
    auto* identity=new QWidget;auto* row=new QHBoxLayout(identity);row->setContentsMargins(12,5,12,8);row->addWidget(new QLabel(navidog::catalogObjectTypeName(n.kind)+"名称"));name=new navidog::ui::TextInput;name->setObjectName("sourceName");name->setMaximumWidth(300);name->setText(n.object);name->setReadOnly(existing);row->addWidget(name);auto* context=new QLabel(p.name+(!n.database.isEmpty()?" / "+n.database:QString{})+(!n.schema.isEmpty() && n.schema!=n.database?" / "+n.schema:QString{}));context->setObjectName("muted");row->addWidget(context);row->addStretch();layout->addWidget(identity);
    auto* tabs=new QTabWidget;tabs->setObjectName("sourceSections");editor=new SqlEditor;editor->setSqlToolsEnabled(true,true);connect(editor,&SqlEditor::conversionRequested,this,[this]{openSqlTool(EditorSqlTool::Convert);});connect(editor,&SqlEditor::optimizationRequested,this,[this]{openSqlTool(EditorSqlTool::Optimize);});editor->setDiagnosticContext(p.driver,n.database.isEmpty()?p.database:n.database,n.schema);editor->setObjectName("sourceEditor");editorArea_ = new SqlEditorArea(editor, outlineToggle, true);
    tabs->addTab(editorArea_, "定义");
    connect(outlineToggle, &QToolButton::toggled, tabs, [tabs, area = editorArea_](bool open) {
        if (open) tabs->setCurrentWidget(area);
    });auto* parameterPage=new QWidget(tabs);auto* parameterLayout=new QVBoxLayout(parameterPage);parameterLayout->setContentsMargins(0,0,0,0);auto* table=new QTableView;parameters=new QStandardItemModel(table);parameters->setHorizontalHeaderLabels({"参数","类型 / 模式"});table->setModel(parameters);table->setSelectionBehavior(QAbstractItemView::SelectItems);table->setEditTriggers(QAbstractItemView::NoEditTriggers);table->setAlternatingRowColors(true);table->horizontalHeader()->setStretchLastSection(true);table->horizontalHeader()->setDefaultSectionSize(220);parameterLayout->addWidget(table);auto* help=new QLabel("在完整定义中修改参数，保存后重新读取。例程内部变量可在定义中查看和编辑。");help->setContentsMargins(12,8,12,8);parameterLayout->addWidget(help);if(navidog::isRoutineObject(n.kind))tabs->addTab(parameterPage,"参数");else parameterPage->hide();layout->addWidget(tabs,1);
    message=new QLabel(existing?"点击重新读取以加载完整定义":"编辑完整定义后保存");message->setObjectName("sourceMessage");message->setWordWrap(true);message->setTextFormat(Qt::PlainText);message->setTextInteractionFlags(Qt::TextSelectableByMouse);message->setContentsMargins(12,8,12,8);layout->addWidget(message);if((navidog::isMySqlFamily(p.driver)) && existing && n.kind!="view")message->setText("MySQL 修改例程需要删除后重建；保存时核对原定义和授权，失败会尝试恢复。保存前将展示确认。");
    if(p.http.enabled)message->setText("HTTP 连接可读取定义。保存对象请使用直连或 SSH 连接。");
    if(!existing)editor->setPlainText(navidog::sourceTemplate(p,n));
    connect(save,&QToolButton::clicked,this,&SourcePane::saveRequested);connect(reload,&QToolButton::clicked,this,&SourcePane::reloadRequested);connect(stop,&QToolButton::clicked,this,&SourcePane::stopRequested);connect(run,&QToolButton::clicked,this,&SourcePane::callRequested);connect(copy,&QToolButton::clicked,this,[this]{QApplication::clipboard()->setText(editor->toPlainText());});connect(editor,&QPlainTextEdit::textChanged,this,[this]{setBusy(false);emit draftChanged();});connect(name,&QLineEdit::textChanged,this,[this]{node.object=node.name=name->text().trimmed();if(!baseline.exists){try{auto text=navidog::rewriteSourceTarget(editor->toPlainText(),profile,node);QSignalBlocker blocker(editor);editor->setPlainText(text);}catch(...){} }setBusy(false);emit draftChanged();});setBusy(false);
}
bool SourcePane::dirty()const{return !baseline.exists?!editor->toPlainText().trimmed().isEmpty():editor->toPlainText()!=baseline.sql;}
navidog::CatalogNode SourcePane::desiredNode()const{auto n=node;n.object=n.name=name->text().trimmed();return n;}
void SourcePane::acceptDefinition(const navidog::SourceDefinition& d){baseline=d;node=d.node;{QSignalBlocker block(name);name->setText(d.node.object);name->setReadOnly(d.exists);}{QSignalBlocker block(editor);editor->setPlainText(d.sql);}parameters->removeRows(0,parameters->rowCount());for(auto n:d.parameters)parameters->appendRow({new QStandardItem(n.name),new QStandardItem(n.detail)});loaded=true;needsReload=false;setBusy(false);emit draftChanged();}
void SourcePane::setBusy(bool busy){save->setEnabled(!busy && loaded && !needsReload && !profile.readOnly && !profile.http.enabled && dirty());reload->setEnabled(!busy && baseline.exists);stop->setEnabled(busy && (loading || saving));run->setEnabled(!busy && baseline.exists && !dirty());copy->setEnabled(!editor->toPlainText().isEmpty());editor->setReadOnly(busy || (baseline.exists && !loaded));name->setEnabled(!busy);}
QJsonObject SourcePane::sessionState()const{return {{"kind","source"},{"profileId",profileId},{"database",database},{"node",nodeJson(desiredNode())},{"baselineNode",nodeJson(baseline.node)},{"exists",baseline.exists},{"baselineSql",baseline.sql},{"sqlMode",baseline.sqlMode},{"outlineOpen",editorArea_->outlineOpen()},{"sql",editor->toPlainText()}};}
void SourcePane::restoreState(const QJsonObject& state){node=readNode(state["node"].toObject());baseline.node=readNode(state["baselineNode"].toObject());baseline.exists=state["exists"].toBool();baseline.sql=state["baselineSql"].toString();baseline.sqlMode=state["sqlMode"].toString();{QSignalBlocker block(name);name->setText(node.object);name->setReadOnly(baseline.exists);}{QSignalBlocker block(editor);editor->setPlainText(state["sql"].toString());}editorArea_->setOutlineOpen(state["outlineOpen"].toBool(true));loaded=true;setBusy(false);message->setText("已恢复本地草稿；保存时会重新核对服务器定义。");}

void SourcePane::openSqlTool(EditorSqlTool tool) {
    if (sqlTools_ && !property("closingWorkspace").toBool())
        sqlTools_->open(*this, *editor, tool, profileId, database, node.schema);
}
