#include "navidog/operation_timing.hpp"
#include "er_pane.hpp"
#include "components.hpp"
#include "editor.hpp"
#include "report_export.hpp"
#include "task_center.hpp"
#include "table_pane.hpp"
#include "navidog/source_design.hpp"
#include <QAbstractListModel>
#include <QApplication>
#include <QClipboard>
#include <QDialog>
#include <QDialogButtonBox>
#include <QDir>
#include <QFileDialog>
#include <QFormLayout>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QInputDialog>
#include <QLabel>
#include <QListView>
#include <QMessageBox>
#include <QPointer>
#include <QSaveFile>
#include <QSignalBlocker>
#include <QSplitter>
#include <QThread>
#include <QTimer>
#include <QUuid>
#include <QVBoxLayout>
#include <algorithm>
#include <optional>
#include <stdexcept>
#include <type_traits>

namespace m = navidog::er;
namespace ui = navidog::ui;
using K = navidog::DatabaseKind;

class ErTableList final : public QAbstractListModel {
public:
    explicit ErTableList(QObject* parent) : QAbstractListModel(parent) {}
    int rowCount(const QModelIndex& parent = {}) const override {
        return parent.isValid() ? 0 : int(rows_.size());
    }
    QVariant data(const QModelIndex& index,int role) const override {
        if (!index.isValid() || index.row() >= rows_.size()) return {};
        const int i = rows_[index.row()];
        const auto& table = project_->entities[i].table;
        if (role == Qt::DisplayRole) return table.node.object;
        if (role == Qt::DecorationRole) return table.node.kind == "view" ? view_ : table_;
        if (role == Qt::ToolTipRole)
            return m::displayName(table.node,project_->driver)+" · "+m::groupName(*project_,project_->entities[i]);
        if (role == Qt::UserRole) return i;
        return {};
    }
    void setSource(const m::Project& project,const m::Graph& graph) {
        project_ = &project;
        all_ = graph.active;
        view_ = ui::objectIcon("view");
        table_ = ui::objectIcon("table");
        std::sort(all_.begin(),all_.end(),[&](int left,int right) {
            const auto& a = project.entities[left].table.node;
            const auto& b = project.entities[right].table.node;
            const int order = a.object.compare(b.object,Qt::CaseInsensitive);
            return order ? order < 0 : std::tie(a.schema,a.database,left) < std::tie(b.schema,b.database,right);
        });
        names_.clear();
        names_.reserve(project.entities.size());
        for (const auto& entity : project.entities)
            names_.push_back(m::displayName(entity.table.node,project.driver).toCaseFolded());
    }
    void filter(const QString& text,const QString& group) {
        const auto needle = text.trimmed().toCaseFolded();
        beginResetModel();
        rows_.clear();
        inverse_.clear();
        if (project_) {
            inverse_.fill(-1,project_->entities.size());
            for (int i : all_) {
                if (!names_[i].contains(needle)) continue;
                if (!group.isEmpty() && m::groupName(*project_,project_->entities[i]) != group) continue;
                inverse_[i] = int(rows_.size());
                rows_.push_back(i);
            }
        }
        endResetModel();
    }
    QModelIndex object(int i) const {
        if (i >= 0 && i < inverse_.size() && inverse_[i] >= 0) return index(inverse_[i],0);
        return {};
    }
private:
    const m::Project* project_ = nullptr;
    QVector<int> all_, rows_, inverse_;
    QVector<QString> names_;
    QIcon view_, table_;
};

namespace {
QString ns(const navidog::CatalogNode& node,K kind) {
    return navidog::isMySqlFamily(kind) ? node.database : node.schema;
}
navidog::ConnectionProfile modelProfile(const m::Project& p) {
    navidog::ConnectionProfile profile;
    profile.driver = p.driver;
    profile.version = p.version;
    profile.database = p.database;
    profile.name = databaseKindName(p.driver);
    return profile;
}
QStringList fieldsOf(const navidog::TableDefinition& table) {
    QStringList fields;
    for (const auto& col : table.columns) fields << col.name;
    return fields;
}
QStringList primaryOf(const navidog::TableDefinition& table) {
    QMap<int,QString> fields;
    for (const auto& col : table.columns) if (col.primaryKey) fields[col.primaryKey] = col.name;
    return fields.values();
}
void sqlDialect(SqlEditor& editor,K kind) {
    using D = navidog::sql::compiler::Dialect;
    const auto dialect = navidog::sql::compiler::parseDialect(navidog::databaseKindKey(kind).toStdString());
    editor.setHighlightDialect(dialect.value_or(D::SQLite));
}
}

ErPane::ErPane(const QString& draftDirectory,QWidget* parent)
    : WorkspacePane(parent), draftDirectory_(draftDirectory), draftId_(QUuid::createUuid().toString(QUuid::Id128)) {
    setObjectName("erWorkspace");
    kind = navidog::WorkspaceTabKind::ErModel;
    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(0,0,0,0);
    layout->setSpacing(0);
    auto* toolbar = new QWidget;
    toolbar->setObjectName("erToolbar");
    toolbar->setStyleSheet("QWidget#erToolbar{background:#eaf1f5;border-bottom:1px solid #cbd8e4}");
    auto* row = new QHBoxLayout(toolbar);
    row->setContentsMargins(8,5,8,5);
    auto button = [&](const QString& label,const QString& name,auto slot) {
        auto* control = new ui::Button(label,name);
        row->addWidget(control);
        connect(control,&QToolButton::clicked,this,slot);
        return control;
    };
    button("逆向导入…","erReverse",[this] { if (reverseRequested) reverseRequested(); });
    button("新建表","erNewTable",[this] { editTable(-1,false); });
    button("新建视图","erNewView",[this] { editTable(-1,true); });
    edit_ = button("编辑","erEdit",[this] { if (selected_ >= 0) editTable(selected_,project_.entities[selected_].table.node.kind=="view"); });
    link_ = button("新建关系","erNewRelation",[this] { addRelation(); });
    remove_ = button("移除","erRemove",[this] { removeSelected(); });
    row->addStretch();
    save_ = button("保存模型","erSave",[this] { saveFile(); });
    ddl_ = button("生成 SQL","erDDL",[this] { showDDL(); });
    ddl_->setProperty("primary",true);
    export_ = button("导出…","erExport",[this] { exportDocuments(); });
    stop_ = button("停止","erStop",[this] { if (cancellation_) cancellation_->store(true); });
    layout->addWidget(toolbar);

    auto* split = new QSplitter(Qt::Horizontal);
    split->setChildrenCollapsible(false);
    split->setHandleWidth(5);
    auto* left = new QWidget;
    left->setMinimumWidth(195);
    auto* navigation = new QVBoxLayout(left);
    navigation->setContentsMargins(8,9,7,8);
    navigation->setSpacing(6);
    search_ = new ui::TextInput;
    search_->setObjectName("erSearch");
    search_->setClearButtonEnabled(true);
    search_->setPlaceholderText("筛选表名、Schema");
    navigation->addWidget(search_);
    groups_ = new ui::Select;
    groups_->setObjectName("erGroups");
    navigation->addWidget(groups_);
    auto* objectList = new ui::ObjectList;
    list_ = objectList;
    list_->setObjectName("erTableList");
    list_->setUniformItemSizes(true);
    list_->setAlternatingRowColors(true);
    list_->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    list_->setEditTriggers(QAbstractItemView::NoEditTriggers);
    list_->setSelectionMode(QAbstractItemView::SingleSelection);
    list_->setVerticalScrollMode(QAbstractItemView::ScrollPerPixel);
    list_->setLayoutMode(QListView::Batched);
    list_->setBatchSize(512);
    list_->setIconSize({15,15});
    tableList_ = new ErTableList(list_);
    list_->setModel(tableList_);
    navigation->addWidget(list_,1);
    selectedInfo_ = new QLabel("未选择对象");
    selectedInfo_->setWordWrap(true);
    navigation->addWidget(selectedInfo_);
    auto* actions = new QHBoxLayout;
    auto* group = new ui::Button("设置分组","erAssignGroup");
    connect(group,&QToolButton::clicked,this,[this] { groupTable(); });
    details_ = new ui::Button("读取完整结构","erDetails");
    connect(details_,&QToolButton::clicked,this,[this] {
        if (selected_ >= 0 && detailsRequested) detailsRequested(selected_);
    });
    actions->addWidget(group);
    actions->addWidget(details_);
    navigation->addLayout(actions);
    split->addWidget(left);

    auto* right = new QWidget;
    auto* drawing = new QVBoxLayout(right);
    drawing->setContentsMargins(0,0,0,0);
    drawing->setSpacing(0);
    auto* viewTools = new QHBoxLayout;
    viewTools->setContentsMargins(10,6,10,6);
    grouping_ = new ui::Select;
    grouping_->setObjectName("erGrouping");
    grouping_->addItems({"按前缀分组","按 Schema 分组","自定义分组"});
    layouts_ = new ui::Select;
    layouts_->setObjectName("erLayouts");
    layouts_->addItems({"关系重力＋网格","名称网格","依赖分层"});
    viewTools->addWidget(grouping_);
    viewTools->addWidget(layouts_);
    auto* reflow = new ui::Button("重排","erReflow");
    viewTools->addWidget(reflow);
    auto* annotations = new ui::Button("显示注释","erToggleComments");
    annotations->setCheckable(true);
    annotations->setToolTip("显示或隐藏所选表或视图的表注释和字段注释");
    viewTools->addWidget(annotations);
    viewTools->addStretch();
    auto* zoomOut = new ui::Button("−","erZoomOut");
    auto* zoomIn = new ui::Button("+","erZoomIn");
    auto* fit = new ui::Button("适合画布","erFit");
    zoom_ = new QLabel("100%");
    zoom_->setMinimumWidth(45);
    viewTools->addWidget(fit);
    viewTools->addWidget(zoomOut);
    viewTools->addWidget(zoom_);
    viewTools->addWidget(zoomIn);
    drawing->addLayout(viewTools);
    canvas_ = new ui::er::Canvas;
    drawing->addWidget(canvas_,1);
    split->addWidget(right);
    split->setStretchFactor(0,0);
    split->setStretchFactor(1,1);
    split->setSizes({235,900});
    layout->addWidget(split,1);
    status_ = new QLabel;
    status_->setObjectName("erStatus");
    status_->setMargin(6);
    status_->setTextFormat(Qt::PlainText);
    layout->addWidget(status_);

    filterTimer_ = new QTimer(this);
    filterTimer_->setInterval(95);
    filterTimer_->setSingleShot(true);
    connect(filterTimer_,&QTimer::timeout,this,&ErPane::refreshList);
    connect(search_,&QLineEdit::textChanged,this,[this]{filterTimer_->start();});
    connect(groups_,QOverload<int>::of(&QComboBox::currentIndexChanged),this,&ErPane::refreshList);
    connect(list_->selectionModel(),&QItemSelectionModel::currentChanged,this,[this](const QModelIndex& current) {
        if (!syncing_ && current.isValid()) select(current.data(Qt::UserRole).toInt(),true,false);
    });
    connect(objectList,&QListView::doubleClicked,this,[this,objectList](const QModelIndex& item) {
        objectList->afterDoubleClick(item,[this](const QModelIndex& activated) {
            if (!activated.isValid()) return;
            const int i = activated.data(Qt::UserRole).toInt();
            if (i >= 0 && i < project_.entities.size())
                editTable(i,project_.entities[i].table.node.kind=="view");
        });
    });
    canvas_->selected = [this](int i){select(i,false,true);};
    canvas_->opened = [this](int i){if(i < project_.entities.size()) editTable(i,project_.entities[i].table.node.kind=="view");};
    canvas_->moved = [this](int i,const QPointF& point){project_.entities[i].position = point;changed();};
    canvas_->zoomed = [this](qreal scale){zoom_->setText(QString::number(scale*100,'f',scale<.05?1:0)+"%");};
    canvas_->annotationStateChanged = [this,annotations] {
        const bool visible = canvas_->annotationsVisible();
        annotations->setEnabled(canvas_->canShowAnnotations());
        annotations->setText(visible ? "隐藏注释" : "显示注释");
        annotations->setChecked(visible);
    };
    connect(annotations,&QToolButton::clicked,canvas_,&ui::er::Canvas::setAnnotationsVisible);
    connect(zoomOut,&QToolButton::clicked,this,[this]{canvas_->zoom(.8);});
    connect(zoomIn,&QToolButton::clicked,this,[this]{canvas_->zoom(1.25);});
    connect(fit,&QToolButton::clicked,canvas_,&ui::er::Canvas::fit);
    auto relayout = [this] {
        for (auto& table : project_.entities) table.position.reset();
        changed();
        rebuild(true);
    };
    connect(reflow,&QToolButton::clicked,this,relayout);
    connect(layouts_,QOverload<int>::of(&QComboBox::currentIndexChanged),this,[this,relayout](int n) {
        if (!syncing_) { project_.layout = m::LayoutKind(n); relayout(); }
    });
    connect(grouping_,QOverload<int>::of(&QComboBox::currentIndexChanged),this,[this,relayout](int n) {
        if (!syncing_) { project_.grouping = m::GroupKind(n); relayout(); }
    });
    rebuild(true);
}

ErPane::~ErPane() { if (cancellation_) cancellation_->store(true); }

template<class Work,class Done>
void ErPane::start(const QString& label,Work work,Done done) {
    if (busy()) return;
    cancellation_ = navidog::cancellation();
    auto cancel = cancellation_;
    status_->setText(label);
    updateActions();
    const QPointer<ErPane> guard(this);
    const auto progress = [guard](const QString& text) {
        QMetaObject::invokeMethod(qApp,[guard,text] { if (guard && guard->busy()) guard->status_->setText(text); },Qt::QueuedConnection);
    };
    auto* thread = QThread::create([guard,cancel,progress,work=std::move(work),done=std::move(done)]() mutable {
        using Result = std::invoke_result_t<Work,const navidog::Cancellation&,const Progress&>;
        std::optional<Result> result;
        QString error;
        try { result = work(cancel,progress); }
        catch (const std::exception& e) { error = QString::fromUtf8(e.what()); }
        QMetaObject::invokeMethod(qApp,[guard,cancel,done=std::move(done),result=std::move(result),error]() mutable {
            if (!guard) return;
            guard->cancellation_.reset();
            if (cancel->load()) guard->status_->setText("操作已停止");
            else if (result) done(std::move(*result));
            else guard->showError("ER 工作区操作失败",error);
            guard->updateActions();
        },Qt::QueuedConnection);
    });
    connect(thread,&QThread::finished,thread,&QObject::deleteLater);
    thread->start();
}

void ErPane::useProject(m::Project value) {
    auto diagram = prepareDiagram(value);
    useProject(std::move(value),std::move(diagram));
}

void ErPane::useProject(m::Project value,PreparedDiagram diagram) {
    project_ = std::move(value);
    profileId = project_.profileId;
    database = project_.database;
    selected_ = -1;
    hasProject_ = true;
    showDiagram(std::move(diagram),true,false);
    emit titleChanged(project_.title + (dirty_ ? " *" : ""));
}

void ErPane::rebuild(bool fit) {
    showDiagram(prepareDiagram(project_),fit,true);
}

ErPane::PreparedDiagram ErPane::prepareDiagram(const m::Project& project) {
    const navidog::OperationTimer timing(navidog::TimedOperation::ErLayout);
    PreparedDiagram diagram;
    diagram.graph = m::makeGraph(project);
    diagram.scene = m::arrange(project,diagram.graph);
    diagram.index.reset(diagram.scene);
    return diagram;
}

void ErPane::showDiagram(PreparedDiagram diagram,bool fit,bool keepCardState) {
    const navidog::OperationTimer timing(navidog::TimedOperation::ErDisplay);
    graph_ = std::move(diagram.graph);
    auto& scene = diagram.scene;
    tableList_->setSource(project_,graph_);
    {
        const QSignalBlocker groups(groups_);
        const QSignalBlocker groupMode(grouping_);
        const QSignalBlocker layoutMode(layouts_);
        const auto previous = groups_->currentData().toString();
        groups_->clear();
        groups_->addItem("全部分组",QString{});
        for (const auto& group : scene.groups) groups_->addItem(group.name,group.name);
        groups_->setCurrentIndex(std::max(0,groups_->findData(previous)));
        grouping_->setCurrentIndex(int(project_.grouping));
        layouts_->setCurrentIndex(int(project_.layout));
    }
    canvas_->reset(project_,graph_,std::move(scene),std::move(diagram.index),keepCardState);
    if (fit) canvas_->fit();
    refreshList();
    select(selected_,false,false);
    status_->setText(QString("%1  ·  %2 个对象  ·  %3 条关系  ·  %4 个分组")
        .arg(navidog::databaseKindName(project_.driver)).arg(graph_.active.size())
        .arg(graph_.edges.size()).arg(canvas_->arrangement().groups.size()));
}

void ErPane::refreshList() {
    syncing_ = true;
    tableList_->filter(search_->text(),groups_->currentData().toString());
    if (const auto index = tableList_->object(selected_); index.isValid()) list_->setCurrentIndex(index);
    syncing_ = false;
}

void ErPane::select(int index,bool locate,bool reveal) {
    selected_ = index >= 0 && index < project_.entities.size() && !project_.entities[index].removed ? index : -1;
    const auto item = tableList_->object(selected_);
    if (reveal && selected_ >= 0 && !item.isValid()) {
        const QSignalBlocker search(search_);
        const QSignalBlocker group(groups_);
        search_->clear();
        groups_->setCurrentIndex(0);
        refreshList();
    }
    const auto current = tableList_->object(selected_);
    if (current.isValid()) {
        syncing_ = true;
        list_->setCurrentIndex(current);
        list_->scrollTo(current,QAbstractItemView::EnsureVisible);
        syncing_ = false;
    }
    canvas_->select(selected_,locate);
    if (selected_ < 0) selectedInfo_->setText("未选择对象");
    else {
        const auto& selected = project_.entities[selected_].table;
        selectedInfo_->setText(m::displayName(selected.node,project_.driver)+"\n"+
            QString("%1 个字段 · %2 条关系").arg(selected.columns.size()).arg(graph_.incidents[selected_].size()));
    }
    updateActions();
}

void ErPane::updateActions() {
    const bool editable = hasProject_ && !busy();
    const bool selected = selected_ >= 0;
    edit_->setEnabled(editable && selected);
    remove_->setEnabled(editable && selected);
    link_->setEnabled(editable && selected && project_.entities[selected_].table.node.kind == "table" && !graph_.active.isEmpty());
    details_->setEnabled(editable && selected && !profileId.isEmpty() && bool(project_.entities[selected_].baseline));
    for (const auto* objectName : {"erNewTable","erNewView","erAssignGroup","erReflow"})
        if (auto* button = findChild<QToolButton*>(objectName)) button->setEnabled(editable);
    save_->setEnabled(editable);
    ddl_->setEnabled(editable && !project_.entities.isEmpty());
    export_->setEnabled(editable && !graph_.active.isEmpty());
    stop_->setVisible(busy());
}

void ErPane::changed() {
    dirty_ = true;
    emit titleChanged(project_.title+" *");
}

void ErPane::showError(const QString& title,const QString& message) {
    status_->setText(title);
    auto* dialog = new ui::DatabaseErrorDialog(this);
    dialog->setAttribute(Qt::WA_DeleteOnClose);
    dialog->setError(title,message);
    dialog->show();
}

bool ErPane::editTable(int index,bool view) {
    if (busy() || !hasProject_) return false;
    navidog::TableDefinition table;
    const bool existing = index >= 0;
    if (existing) table = project_.entities[index].table;
    else {
        table.node.kind = view ? "view" : "table";
        table.node.database = project_.database;
        table.node.schema = project_.schema;
        table.node.name = table.node.object = view ? "new_view" : "new_table";
        table.detailsLoaded = true;
        if (!view) {
            navidog::TableColumn key;
            key.name = "id";
            key.type = project_.driver == K::Oracle ? "NUMBER" : "INTEGER";
            key.primaryKey = 1;
            key.nullable = false;
            table.columns << key;
        }
    }
    ui::Dialog dialog(this);
    dialog.setWindowTitle(existing?"编辑模型对象":view?"新建视图":"新建表");
    dialog.resize(view?790:1020,view?510:700);
    auto* layout = new QVBoxLayout(&dialog);
    auto* form = new QFormLayout;
    auto* schema = new ui::TextInput;
    schema->setText(ns(table.node,project_.driver));
    schema->setReadOnly(existing && bool(project_.entities[index].baseline));
    form->addRow(navidog::isMySqlFamily(project_.driver)?"数据库":"Schema",schema);
    auto* custom = new ui::TextInput;
    if (existing) custom->setText(project_.entities[index].group);
    custom->setPlaceholderText("仅在自定义分组模式使用");
    form->addRow("分组",custom);
    layout->addLayout(form);
    TablePane* designer = nullptr;
    ui::TextInput* viewName = nullptr;
    SqlEditor* query = nullptr;
    QHash<QString,QString> baselineColumns;
    QString previousViewBody;
    if (view) {
        viewName = new ui::TextInput;
        viewName->setText(table.node.object);
        form->addRow("视图名",viewName);
        query = new SqlEditor;
        sqlDialect(*query,project_.driver);
        query->setPlaceholderText("输入视图的 SELECT 查询");
        try { previousViewBody = m::viewBody(table.rawSql,project_.driver); }
        catch (...) { previousViewBody = table.rawSql; }
        query->setPlainText(previousViewBody);
        layout->addWidget(query,1);
    } else {
        designer = new TablePane(modelProfile(project_),table.node,table.exists);
        auto working = table;
        for (auto& col : working.columns) {
            baselineColumns.insert(col.name,col.originalName);
            col.originalName = col.name;
        }
        designer->acceptDefinition(working);
        designer->name->setReadOnly(false);
        designer->save->hide();
        designer->reload->hide();
        designer->stop->hide();
        designer->message->hide();
        for (int i = designer->sections->count()-1; i >= 0; --i) {
            if (designer->sections->widget(i)->isAncestorOf(designer->sql))
                designer->sections->removeTab(i);
        }
        layout->addWidget(designer,1);
    }
    auto* buttons = new ui::FormButtons;
    buttons->button(QDialogButtonBox::Save)->setText("保存到模型");
    layout->addWidget(buttons);
    connect(buttons,&QDialogButtonBox::rejected,&dialog,&QDialog::reject);
    connect(buttons,&QDialogButtonBox::accepted,&dialog,[&] {
        QHash<QString,QString> renames;
        if (designer) {
            designer->commitEditor();
            table = designer->definition();
            for (auto& col : table.columns) {
                const auto from = baselineColumns.constFind(col.originalName);
                if (from == baselineColumns.cend()) continue;
                if (col.originalName != col.name) renames.insert(col.originalName,col.name);
                col.originalName = from.value();
            }
        }
        if (viewName) table.node.object = viewName->text().trimmed();
        table.node.name = table.node.object;
        if (navidog::isMySqlFamily(project_.driver)) table.node.database = schema->text().trimmed();
        else table.node.schema = schema->text().trimmed();
        if (project_.driver == K::SQLite && table.node.schema.isEmpty()) table.node.schema = "main";
        if (query && (!existing || query->toPlainText().trimmed() != previousViewBody))
            table.rawSql = query->toPlainText().trimmed();
        try {
            if (table.node.object.trimmed().isEmpty()) throw std::runtime_error("请填写对象名称");
            const auto key = m::nameKey(table.node,project_.driver);
            if (graph_.byName.contains(key) && graph_.byName.value(key) != index) throw std::runtime_error("模型中已存在同名对象");
            if (query && (!existing || table.rawSql != project_.entities[index].table.rawSql) && !table.rawSql.isEmpty())
                m::viewBody(table.rawSql,project_.driver);
            if (query && !existing && table.rawSql.isEmpty()) throw std::runtime_error("请填写视图查询");
            if (designer && table.columns.isEmpty()) throw std::runtime_error("至少需要一个字段");
            if (existing) m::replaceEntity(project_,index,table,std::move(renames));
            else { project_.add(table); index = int(project_.entities.size())-1; }
            project_.entities[index].group = custom->text().trimmed();
            dialog.accept();
        } catch (const std::exception& e) {
            QMessageBox::warning(&dialog,"不能保存模型对象",QString::fromUtf8(e.what()));
        }
    });
    if (dialog.exec() != QDialog::Accepted) return false;
    changed();
    rebuild();
    select(index,true,true);
    return true;
}

void ErPane::addRelation() {
    if (selected_ < 0 || busy()) return;
    const int source = selected_;
    const auto& first = project_.entities[source].table;
    if (first.node.kind != "table") return;
    ui::Dialog dialog(this);
    dialog.setWindowTitle("新建表关系");
    dialog.resize(600,300);
    auto* form = new QFormLayout(&dialog);
    auto* filter = new ui::TextInput;
    filter->setPlaceholderText("输入目标表名筛选");
    auto* target = new ui::Select;
    auto* from = new ui::TextInput;
    auto* to = new ui::TextInput;
    auto* name = new ui::TextInput;
    name->setText("fk_"+QString::number(first.objects.size()+1));
    auto* onDelete = new ui::Select;
    auto* onUpdate = new ui::Select;
    onDelete->addItems({"NO ACTION","CASCADE","SET NULL","RESTRICT","SET DEFAULT"});
    onUpdate->addItems({"NO ACTION","CASCADE","SET NULL","RESTRICT","SET DEFAULT"});
    if (project_.driver == K::Oracle) { onUpdate->setEnabled(false); onDelete->clear();onDelete->addItems({"NO ACTION","CASCADE","SET NULL"}); }
    auto* relation = new ui::Select;
    relation->addItems({"多对一","一对一","多对多（建立中间表）"});
    form->addRow("源表",new QLabel(m::displayName(first.node,project_.driver)));
    form->addRow("源列",from);
    from->setPlaceholderText("按顺序用逗号分隔");
    if (first.columns.size()==1) from->setText(navidog::formatIdentifierList(fieldsOf(first),project_.driver));
    form->addRow("查找目标表",filter);
    form->addRow("目标表",target);
    form->addRow("目标列",to);
    form->addRow("关系名 / 中间表名",name);
    form->addRow("ON DELETE",onDelete);
    form->addRow("ON UPDATE",onUpdate);
    form->addRow("关系类型",relation);
    auto* buttons = new ui::FormButtons;
    form->addRow(buttons);
    const auto fill = [&] {
        const QSignalBlocker blocker(target);
        const auto previous = target->currentData();
        target->clear();
        for (int i : graph_.active) {
            const auto& t = project_.entities[i].table;
            const auto label = m::displayName(t.node,project_.driver);
            if (t.node.kind == "table" && label.contains(filter->text(),Qt::CaseInsensitive))
                target->addItem(label,i);
        }
        const int restored = target->findData(previous);
        if (restored >= 0) target->setCurrentIndex(restored);
    };
    const auto targetChanged = [&] {
        if (!target->currentData().isValid()) { to->clear(); return; }
        const auto& table = project_.entities[target->currentData().toInt()].table;
        const auto fields = primaryOf(table);
        to->setText(navidog::formatIdentifierList(fields,project_.driver));
        to->setToolTip(navidog::formatIdentifierList(fieldsOf(table),project_.driver));
    };
    connect(target,QOverload<int>::of(&QComboBox::currentIndexChanged),&dialog,targetChanged);
    QTimer throttle(&dialog);
    throttle.setInterval(140);
    throttle.setSingleShot(true);
    connect(filter,&QLineEdit::textChanged,&throttle,QOverload<>::of(&QTimer::start));
    connect(&throttle,&QTimer::timeout,&dialog,[&]{fill();targetChanged();});
    fill();
    targetChanged();
    connect(relation,QOverload<int>::of(&QComboBox::currentIndexChanged),&dialog,[&](int mode) {
        if (mode == 2) {
            from->setText(navidog::formatIdentifierList(primaryOf(first),project_.driver));
            name->setText(first.node.object+"_link");
        }
    });
    from->setToolTip(navidog::formatIdentifierList(fieldsOf(first),project_.driver));
    connect(buttons,&QDialogButtonBox::rejected,&dialog,&QDialog::reject);
    connect(buttons,&QDialogButtonBox::accepted,&dialog,[&] {
        try {
            if (!target->currentData().isValid()) throw std::runtime_error("请选择目标表");
            const auto& destination = project_.entities[target->currentData().toInt()].table;
            navidog::TableObject fk;
            fk.kind = navidog::TableObjectKind::ForeignKey;
            fk.name = name->text().trimmed();
            fk.columns = navidog::parseIdentifierList(from->text());
            fk.referenceColumns = navidog::parseIdentifierList(to->text());
            fk.referenceTable = destination.node.object;
            fk.referenceSchema = ns(destination.node,project_.driver);
            fk.onDelete = onDelete->currentText();
            fk.onUpdate = onUpdate->currentText();
            const int mode = relation->currentIndex();
            if (fk.name.isEmpty() || fk.columns.isEmpty() || fk.referenceColumns.isEmpty() ||
                (mode != 2 && fk.columns.size() != fk.referenceColumns.size()))
                throw std::runtime_error("请填写约束名并提供数量相同的源字段和目标字段");
            const auto contains = [](const auto& table,const QStringList& columns) {
                const auto names = fieldsOf(table);
                return std::all_of(columns.cbegin(),columns.cend(),[&](const auto& c){return names.contains(c);});
            };
            if (!contains(first,fk.columns) || !contains(destination,fk.referenceColumns))
                throw std::runtime_error("关系引用了不存在的字段");
            if (mode == 2) {
                navidog::TableDefinition junction;
                junction.node = first.node;
                junction.node.object = junction.node.name = fk.name;
                junction.detailsLoaded = true;
                if (graph_.byName.contains(m::nameKey(junction.node,project_.driver)))
                    throw std::runtime_error("中间表名已存在");
                if (fk.onDelete == "SET NULL" || fk.onUpdate == "SET NULL")
                    throw std::runtime_error("中间表引用列同时属于主键，不能使用 SET NULL");
                const auto addSide = [&](const auto& table,const QStringList& referenced,const QString& side) {
                    auto cross = fk;
                    cross.name = "fk_"+side;
                    cross.columns.clear();
                    cross.referenceColumns = referenced;
                    cross.referenceTable = table.node.object;
                    cross.referenceSchema = ns(table.node,project_.driver);
                    for (const auto& key : referenced) {
                        const auto original = std::find_if(table.columns.cbegin(),table.columns.cend(),[&](const auto& c){ return c.name==key; });
                        navidog::TableColumn col;
                        col.name = side+"_"+QString::number(cross.columns.size()+1);
                        col.type = original->type;
                        col.collation = original->collation;
                        col.nullable = false;
                        col.primaryKey = junction.columns.size()+1;
                        junction.columns << col;
                        cross.columns << col.name;
                    }
                    junction.objects << cross;
                };
                addSide(first,fk.columns,"source");
                addSide(destination,fk.referenceColumns,"target");
                project_.add(std::move(junction));
                dialog.accept();
                return;
            }
            auto copy = first;
            copy.objects << fk;
            if (mode == 1 && primaryOf(copy) != fk.columns) {
                navidog::TableObject index;
                index.name = "uq_"+fk.name;
                index.columns = fk.columns;
                index.unique = true;
                copy.objects << index;
            }
            if (std::any_of(first.objects.cbegin(),first.objects.cend(),[&](const auto& o){return o.name == fk.name && o.kind == fk.kind;}))
                throw std::runtime_error("外键约束名称重复");
            m::replaceEntity(project_,source,std::move(copy));
            dialog.accept();
        } catch (const std::exception& e) {
            QMessageBox::warning(&dialog,"不能建立关系",QString::fromUtf8(e.what()));
        }
    });
    if (dialog.exec() == QDialog::Accepted) { changed(); rebuild(); select(source,true,true); }
}

void ErPane::groupTable() {
    if (busy() || selected_ < 0) return;
    bool ok = false;
    const auto group = QInputDialog::getText(this,"自定义分组","分组名",QLineEdit::Normal,project_.entities[selected_].group,&ok);
    if (!ok) return;
    project_.entities[selected_].group = group.trimmed();
    if (project_.grouping != m::GroupKind::Custom) {
        project_.grouping = m::GroupKind::Custom;
        for (auto& e : project_.entities) e.position.reset();
    }
    changed();
    rebuild();
}

void ErPane::removeSelected() {
    if (busy() || selected_ < 0) return;
    const int index = selected_;
    const auto& table = project_.entities[index].table;
    for (int e : graph_.incidents[index]) {
        if (graph_.edges[e].to == index && graph_.edges[e].from != index) {
            showError("该对象仍被引用","请先在引用表中删除对应外键，再移除此对象。");
            return;
        }
    }
    if (QMessageBox::question(this,"从模型移除",QString("移除 %1？导入对象会在差异 SQL 中生成 DROP。").arg(table.node.object),
                               QMessageBox::Yes|QMessageBox::Cancel,QMessageBox::Cancel) != QMessageBox::Yes)
        return;
    project_.entities[index].removed = true;
    selected_ = -1;
    changed();
    rebuild();
}

void ErPane::showDDL() {
    if (busy()) return;
    ui::Dialog dialog(this);
    dialog.setWindowTitle("ER 模型 SQL");
    dialog.resize(950,660);
    auto* layout = new QVBoxLayout(&dialog);
    auto* controls = new QHBoxLayout;
    auto* mode = new ui::Select;
    mode->setObjectName("erDdlMode");
    mode->addItems({"差异 SQL（ALTER）","完整建模 SQL（CREATE）"});
    controls->addWidget(mode);
    controls->addStretch();
    auto* copy = new ui::Button("复制 SQL","erCopyDDL");
    auto* saveSql = new ui::Button("保存 SQL…","erSaveDDL");
    auto* open = new ui::Button("送到查询页","erExecuteDDL");
    controls->addWidget(copy);
    controls->addWidget(saveSql);
    controls->addWidget(open);
    layout->addLayout(controls);
    auto* code = new SqlReadOnlyView;
    code->setDatabaseKind(project_.driver);
    code->setObjectName("erDdlPreview");
    layout->addWidget(code->withActions(false),1);
    auto* info = new QLabel;
    info->setWordWrap(true);
    layout->addWidget(info);
    struct Preview { QString sql, error; int statements = 0; bool truncated = false; };
    const auto preview = std::make_shared<Preview>();
    const QPointer<QDialog> guard(&dialog);
    auto generate = [&] {
        const bool changesOnly = mode->currentIndex()==0;
        mode->setEnabled(false);
        code->setFormattingEnabled(false);
        for (auto* action : {copy,saveSql,open}) action->setEnabled(false);
        info->setText("正在生成和检查 SQL…");
        start("正在生成模型 SQL…",[project=project_,changesOnly](const auto& cancel,const Progress&) {
            Preview result;
            try {
                const auto statements = m::ddl(project,changesOnly,cancel);
                result.statements = int(statements.size());
                if (!statements.isEmpty()) result.sql = statements.join(";\n\n")+";\n";
            } catch (const std::exception& e) { result.error = QString::fromUtf8(e.what()); }
            return result;
        },[this,guard,preview,mode,copy,saveSql,open,info,code](Preview result) {
            if (!guard) return;
            *preview = std::move(result);
            constexpr int maxPreviewText = 220000;
            const bool large = preview->sql.size() > maxPreviewText;
            preview->truncated = large;
            code->setSourceSql(large ? preview->sql.left(maxPreviewText) : preview->sql);
            code->setFormattingEnabled(!large);
            info->setText(!preview->error.isEmpty() ? preview->error : preview->sql.isEmpty() ? "模型结构没有变更" :
                QString("%1 条语句，请核对数据库上下文和变更范围%2").arg(preview->statements)
                .arg(large ? "。预览显示前 22 万字，完整 SQL 可保存。" : ""));
            mode->setEnabled(true);
            copy->setEnabled(!preview->sql.isEmpty());
            saveSql->setEnabled(!preview->sql.isEmpty());
            open->setEnabled(!preview->sql.isEmpty() && !large && bool(openSqlRequested));
            status_->setText(preview->error.isEmpty() ? "模型 SQL 已生成" : "模型 SQL 检查未通过");
        });
    };
    connect(mode,QOverload<int>::of(&QComboBox::currentIndexChanged),&dialog,generate);
    const auto outputSql = [&] { return preview->truncated ? preview->sql : code->toPlainText(); };
    connect(copy,&QToolButton::clicked,&dialog,[&]{QApplication::clipboard()->setText(outputSql());});
    connect(saveSql,&QToolButton::clicked,&dialog,[&] {
        const auto path = ui::chooseExportFile(&dialog,"保存模型 SQL",modelExportContext(),"ER模型SQL","SQL (*.sql)");
        if (path.isEmpty()) return;
        QSaveFile file(path);
        const auto bytes = outputSql().toUtf8();
        if (!file.open(QIODevice::WriteOnly) || file.write(bytes) != bytes.size() || !file.commit())
            QMessageBox::warning(&dialog,"SQL 保存失败",file.errorString());
    });
    connect(open,&QToolButton::clicked,&dialog,[&] {
        if (openSqlRequested && openSqlRequested(outputSql())) dialog.accept();
    });
    connect(&dialog,&QDialog::finished,this,[this]{if (cancellation_) cancellation_->store(true);});
    {
        const QSignalBlocker block(mode);
        if (std::none_of(project_.entities.cbegin(),project_.entities.cend(),[](const auto& e){return bool(e.baseline);}))
            mode->setCurrentIndex(1);
    }
    generate();
    dialog.exec();
}

navidog::ui::ExportContext ErPane::modelExportContext() const {
    navidog::ConnectionProfile profile;
    profile.name = connectionName ? connectionName(project_.profileId) : QString{};
    profile.driver = project_.driver;
    profile.database = project_.database;
    return ui::exportContext(profile, project_.database, project_.schema);
}

void ErPane::exportDocuments() {
    if (busy()) return;
    bool ok = false;
    const auto type = QInputDialog::getItem(this,"导出 ER 图与文档","文件格式",
                          {"PDF + Word + Markdown","PDF","Word","Markdown"},0,false,&ok);
    if (!ok) return;
    const int formats = type == "PDF" ? ui::er::Pdf : type == "Word" ? ui::er::Word :
        type == "Markdown" ? ui::er::Markdown : ui::er::Pdf|ui::er::Word|ui::er::Markdown;
    const auto destination = QFileDialog::getExistingDirectory(
        this,"选择文档和分组 PNG 保存目录",ui::exportDirectory(this));
    if (destination.isEmpty()) return;
    if (auto* tasks = ui::backgroundTasksFor(this)) {
        QString rejection;
        const auto id = tasks->submit({"导出 ER 图与文档", project_.title + " / " + destination, {}, {}},
            [value = project_, destination, formats, context = modelExportContext()](const navidog::TaskContext& task) {
                const navidog::OperationTimer timing(navidog::TimedOperation::ErExport);
                task.report({0, 0, navidog::TaskUnit::Items, "准备文档和分组图片"});
                const auto path = ui::er::exportReport(value, destination, formats, task.cancel, [&](int done, int total) {
                    task.report({done, total, navidog::TaskUnit::Items, "正在导出文档与图片"});
                }, context);
                navidog::TaskResult result;
                result.detail = "ER 图与文档已导出";
                const QDir folder(path);
                for (const auto& file : folder.entryList({"*.pdf", "*.docx", "*.md"}, QDir::Files))
                    result.artifacts << folder.absoluteFilePath(file);
                if (result.artifacts.isEmpty()) result.artifacts << path;
                return result;
            }, &rejection);
        status_->setText(id.isEmpty() ? rejection : "已提交导出任务，可在右下角查看。");
        return;
    }
    start("正在生成分组 ER 图和说明…",[value=project_,destination,formats,context=modelExportContext()](const auto& cancel,const Progress& report) {
        const navidog::OperationTimer timing(navidog::TimedOperation::ErExport);
        return ui::er::exportReport(value,destination,formats,cancel,[&](int done,int total) {
            if (done == total || done%5 == 0) report(QString("正在导出文档（%1 / %2）").arg(done).arg(total));
        },context);
    },[this](QString path){status_->setText("已导出到 "+path);});
}

void ErPane::importFrom(navidog::ConnectionProfile profile,m::ImportOptions settings) {
    start("正在读取数据库目录…",[profile=std::move(profile),settings=std::move(settings)](const auto& cancel,const Progress& report) {
        const navidog::OperationTimer timing(navidog::TimedOperation::ErImport);
        auto db = navidog::connectDatabase(profile);
        auto imported = m::reverse(*db,profile,settings,cancel,[&](int count) {
            if (count == 1 || count%256==0) report(QString("已读取 %1 个表和视图").arg(count));
        });
        PreparedDiagram diagram;
        if (!cancel->load()) {
            report("正在生成 ER 图布局…");
            diagram = prepareDiagram(imported.project);
        }
        return std::make_pair(std::move(imported),std::move(diagram));
    },[this](std::pair<m::ImportResult,PreparedDiagram> result) {
        auto& [imported,diagram] = result;
        dirty_ = true;
        useProject(std::move(imported.project),std::move(diagram));
        if (!imported.notices.isEmpty()) status_->setText(status_->text()+" · "+imported.notices.join(" · "));
    });
}

void ErPane::loadDetails(int index,navidog::ConnectionProfile profile) {
    if (busy() || index < 0 || index >= project_.entities.size()) return;
    const auto entity = std::as_const(project_.entities)[index];
    if (!entity.baseline) return;
    if (m::nameKey(entity.table.node,project_.driver) != m::nameKey(entity.baseline->node,project_.driver) ||
        !navidog::sameTableStructure(entity.table,*entity.baseline)) {
        showError("对象已有本地修改","读取服务器完整结构会覆盖此对象的草稿。可在另一个 ER 工作区导入完整结构，或先保存模型。");
        return;
    }
    start("正在读取完整对象结构…",[profile=std::move(profile),node=entity.baseline->node](const auto& cancel,const Progress&) {
        auto db = navidog::connectDatabase(profile);
        if (node.kind=="view") {
            auto source = navidog::readSourceDefinition(*db,profile,node,cancel);
            auto t = navidog::readTableDefinition(*db,profile,node,cancel,false);
            t.rawSql = source.sql;
            return t;
        }
        return navidog::readTableDefinition(*db,profile,node,cancel);
    },[this,id=entity.id](navidog::TableDefinition table) {
        const int index = graph_.byId.value(id,-1);
        if (index < 0) return;
        project_.entities[index].table = table;
        project_.entities[index].baseline = std::move(table);
        changed();
        rebuild();
        select(index,false,true);
    });
}

QString ErPane::draftPath() const { return QDir(draftDirectory_).filePath(draftId_+".json"); }

void ErPane::persistDraft() const {
    if (!hasProject_ || (!draftOrigin_.isEmpty() && !dirty_)) return;
    if (!QDir().mkpath(draftDirectory_)) throw std::runtime_error("无法创建 ER 模型草稿目录");
    m::save(project_,draftPath());
}

QJsonObject ErPane::sessionState() const {
    return {{"kind","erModel"},{"profileId",profileId},{"draft",draftId_},{"file",filePath_},{"dirty",dirty_}};
}

void ErPane::readProject(const QString& path,bool userFile) {
    hasProject_ = false;
    start("正在打开模型…",[path](const auto& cancel,const Progress&) {
        auto value = m::open(path);
        auto diagram = cancel->load() ? PreparedDiagram{} : prepareDiagram(value);
        return std::make_pair(std::move(value),std::move(diagram));
    },[this,path,userFile](std::pair<m::Project,PreparedDiagram> result) {
            if (userFile) filePath_ = path;
            if (userFile) dirty_ = false;
            useProject(std::move(result.first),std::move(result.second));
        });
}

void ErPane::openFile(const QString& path) { if (!path.isEmpty()) readProject(path,true); }

void ErPane::restore(const QJsonObject& state) {
    const auto id = state["draft"].toString();
    if (QUuid(id).isNull() && QUuid("{"+id.left(8)+"-"+id.mid(8,4)+"-"+id.mid(12,4)+"-"+id.mid(16,4)+"-"+id.mid(20,12)+"}").isNull())
        return;
    if (id.size() != 32) return;
    draftId_ = id;
    dirty_ = state["dirty"].toBool();
    filePath_ = state["file"].toString();
    draftOrigin_ = draftPath();
    if (QFile::exists(draftOrigin_)) readProject(draftOrigin_,false);
    else if (!filePath_.isEmpty()) readProject(filePath_,true);
}

bool ErPane::saveFile(bool choosePath) {
    if (busy() || !hasProject_) return false;
    QString path = filePath_;
    if (path.isEmpty() || choosePath) {
        path = ui::chooseExportFile(this,"保存 ER 模型",modelExportContext(),"ER模型","Navidog ER (*.naver.json)");
        if (path.isEmpty()) return false;
        if (!path.endsWith(".json",Qt::CaseInsensitive)) path += ".naver.json";
    }
    try {
        m::save(project_,path);
        filePath_ = path;
        dirty_ = false;
        draftOrigin_.clear();
        emit titleChanged(project_.title);
        status_->setText("模型已保存到 "+path);
        return true;
    } catch (const std::exception& e) {
        showError("模型保存失败",QString::fromUtf8(e.what()));
        return false;
    }
}

bool ErPane::prepareClose() {
    if (busy()) {
        cancellation_->store(true);
        status_->setText("正在停止操作，结束后可关闭模型");
        return false;
    }
    if (dirty_) {
        const auto choice = QMessageBox::question(this,"保存 ER 模型","关闭前是否保存模型修改？",
                    QMessageBox::Save|QMessageBox::Discard|QMessageBox::Cancel,QMessageBox::Cancel);
        if (choice == QMessageBox::Cancel || (choice == QMessageBox::Save && !saveFile())) return false;
    }
    return true;
}

void ErPane::forgetDraft() { QFile::remove(draftPath()); }
