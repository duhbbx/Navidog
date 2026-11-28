#include "table_pane.hpp"
#include "editor.hpp"
#include "sql_outline.hpp"
#include "components.hpp"
#include <QTableView>
#include <QHeaderView>
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QPlainTextEdit>
#include <QSplitter>
#include <QStyledItemDelegate>
#include <QApplication>
#include <QJsonArray>
#include <QFormLayout>
#include <QCheckBox>
#include <QSignalBlocker>
#include <QTabWidget>
#include <QRegularExpression>
#include <algorithm>
namespace {
class FieldDelegate final : public QStyledItemDelegate {
public:
    navidog::DatabaseKind kind;
    explicit FieldDelegate(navidog::DatabaseKind k,QObject* p):QStyledItemDelegate(p),kind(k){}
    QWidget* createEditor(QWidget* p,const QStyleOptionViewItem&,const QModelIndex& index)const override {
        if(index.column()==10){auto* modes=new navidog::ui::Select(p);modes->setEditable(true);modes->addItems({"","VIRTUAL","STORED","ALWAYS","BY DEFAULT"});return modes;}
        if(index.column()!=1)return new navidog::ui::TextInput(p);
        auto* types=new navidog::ui::Select(p);types->setEditable(true);
        using K=navidog::DatabaseKind;QStringList options;
        if(kind==K::SQLite)options=QStringList{"INTEGER","TEXT","REAL","BLOB","NUMERIC"};
        else if(kind==K::Oracle)options=QStringList{"NUMBER","NUMBER(18,2)","VARCHAR2(255)","DATE","TIMESTAMP","CLOB","BLOB"};
        else if(kind==K::SQLServer)options=QStringList{"INT","BIGINT","NVARCHAR(255)","VARCHAR(255)","DECIMAL(18,2)","BIT","DATETIME2","VARBINARY(MAX)"};
        else if(navidog::usesNativePostgres(kind))options=QStringList{"integer","bigint","varchar(255)","text","numeric(18,2)","boolean","timestamp","date","jsonb","uuid","bytea"};
        else options=QStringList{"INT","BIGINT","VARCHAR(255)","TEXT","DECIMAL(18,2)","TINYINT","DATETIME","TIMESTAMP","DATE","JSON","BLOB"};
        types->addItems(options);return types;
    }
    void setEditorData(QWidget* editor,const QModelIndex& index)const override {
        if(auto* combo=qobject_cast<QComboBox*>(editor))combo->setCurrentText(index.data(Qt::EditRole).toString());
        else QStyledItemDelegate::setEditorData(editor,index);
    }
    void setModelData(QWidget* editor,QAbstractItemModel* model,const QModelIndex& index)const override {
        if(auto* combo=qobject_cast<QComboBox*>(editor))model->setData(index,combo->currentText(),Qt::EditRole);
        else QStyledItemDelegate::setModelData(editor,model,index);
    }
};
}
QVariant TableColumnModel::data(const QModelIndex& i,int role)const {
    if(!i.isValid() || i.row()>=columns.size())return {};const auto& c=columns[i.row()];
    if(role==Qt::ToolTipRole)return c.protectedReason;
    if(role==Qt::CheckStateRole){if(i.column()==2)return c.nullable?Qt::Unchecked:Qt::Checked;if(i.column()==3)return c.primaryKey?Qt::Checked:Qt::Unchecked;if(i.column()==7)return c.autoIncrement?Qt::Checked:Qt::Unchecked;}
    if(role!=Qt::DisplayRole && role!=Qt::EditRole)return {};
    switch(i.column()){case 0:return c.name;case 1:{if(role==Qt::EditRole)return c.type;auto type=c.type;type.remove(QRegularExpression("\\(\\s*(?:[0-9]+|MAX)(?:\\s*,\\s*[0-9]+)?\\s*\\)",QRegularExpression::CaseInsensitiveOption));return type;}case 4:return c.defaultExpression;case 5:return c.protectedReason.isEmpty()?c.attributes:c.protectedReason;case 6:return c.comment;case 8:return c.generatedExpression;case 9:return c.collation;case 10:return c.generation;case 11:case 12:{auto match=QRegularExpression("\\(\\s*([0-9]+|MAX)(?:\\s*,\\s*([0-9]+))?\\s*\\)",QRegularExpression::CaseInsensitiveOption).match(c.type);return match.hasMatch()?match.captured(i.column()==11?1:2):QString{};}default:return {};}
}
QVariant TableColumnModel::headerData(int s,Qt::Orientation orientation,int role)const {if(role!=Qt::DisplayRole)return {};if(orientation==Qt::Vertical)return s+1;return QStringList{"名称","类型","不是 Null","键","默认值表达式","其他属性","注释","自增 / 标识","生成表达式","排序规则","生成模式","长度","小数点"}.value(s);}
Qt::ItemFlags TableColumnModel::flags(const QModelIndex& i)const {auto f=QAbstractTableModel::flags(i);if(!i.isValid())return f;if(!columns[i.row()].protectedReason.isEmpty())return i.column()==6?f|Qt::ItemIsEditable:f;if(i.column()==2 || i.column()==3 || i.column()==7)return f|Qt::ItemIsUserCheckable;return i.column()==5?f:f|Qt::ItemIsEditable;}
bool TableColumnModel::setData(const QModelIndex& i,const QVariant& value,int role) {
    if(!i.isValid() || i.row()>=columns.size() || (!columns[i.row()].protectedReason.isEmpty() && i.column()!=6))return false;auto& c=columns[i.row()];
    if(role==Qt::CheckStateRole){bool yes=value.toInt()==Qt::Checked;if(i.column()==2){if(c.primaryKey && !yes)return false;c.nullable=!yes;}else if(i.column()==3){c.primaryKey=yes?i.row()+1:0;if(yes)c.nullable=false;}else if(i.column()==7){c.autoIncrement=yes;if(yes)c.nullable=false;}else return false;}
    else if(role==Qt::EditRole){if(i.column()==0)c.name=value.toString().trimmed();else if(i.column()==1)c.type=value.toString().trimmed();else if(i.column()==4)c.defaultExpression=value.toString().trimmed();else if(i.column()==6)c.comment=value.toString();else if(i.column()==8)c.generatedExpression=value.toString().trimmed();else if(i.column()==9)c.collation=value.toString().trimmed();else if(i.column()==10)c.generation=value.toString().trimmed();else if(i.column()==11 || i.column()==12){if(c.type.startsWith("ENUM",Qt::CaseInsensitive) || c.type.startsWith("SET(",Qt::CaseInsensitive))return false;auto re=QRegularExpression("\\(\\s*([0-9]+|MAX)(?:\\s*,\\s*([0-9]+))?\\s*\\)",QRegularExpression::CaseInsensitiveOption);auto match=re.match(c.type);auto length=i.column()==11?value.toString().trimmed():match.captured(1);auto scale=i.column()==12?value.toString().trimmed():match.captured(2);if(!length.isEmpty() && !QRegularExpression("^(?:[1-9][0-9]*|MAX)$",QRegularExpression::CaseInsensitiveOption).match(length).hasMatch())return false;if(!scale.isEmpty() && !QRegularExpression("^[0-9]+$").match(scale).hasMatch())return false;auto suffix=length.isEmpty()?QString{}:"("+length+(scale.isEmpty()?QString{}:","+scale)+")";if(match.hasMatch())c.type.replace(match.capturedStart(),match.capturedLength(),suffix);else c.type+=suffix;}else return false;}else return false;
    emit dataChanged(index(i.row(),0),index(i.row(),12));return true;
}
void TableColumnModel::resetColumns(QVector<navidog::TableColumn> values){beginResetModel();columns=std::move(values);endResetModel();}
void TableColumnModel::addColumn(navidog::DatabaseKind k){insertColumn(columns.size(),k);}
void TableColumnModel::insertColumn(int r,navidog::DatabaseKind k){if(columns.size()>=512)return;r=qBound(0,r,int(columns.size()));navidog::TableColumn c;int n=columns.size()+1;do{c.name="column_"+QString::number(n++);}while(std::any_of(columns.begin(),columns.end(),[&](auto a){return a.name==c.name;}));c.type=k==navidog::DatabaseKind::Oracle?"VARCHAR2(255)":k==navidog::DatabaseKind::SQLite?"TEXT":"VARCHAR(255)";beginInsertRows({},r,r);columns.insert(r,c);endInsertRows();}
void TableColumnModel::removeColumn(int r){if(r<0 || r>=columns.size())return;beginRemoveRows({},r,r);columns.removeAt(r);endRemoveRows();}
void TableColumnModel::moveColumn(int r,int delta){int dest=r+delta;if(r<0 || dest<0 || r>=columns.size() || dest>=columns.size())return;beginMoveRows({},r,r,{},delta>0?dest+1:dest);columns.move(r,dest);endMoveRows();}
namespace {
using O=navidog::TableObjectKind;using K=navidog::DatabaseKind;
class ObjectDelegate final:public QStyledItemDelegate {
public:
    using QStyledItemDelegate::QStyledItemDelegate;
    QWidget* createEditor(QWidget* p,const QStyleOptionViewItem&,const QModelIndex& index)const override{auto* model=dynamic_cast<const TableObjectModel*>(index.model());auto choices=model?model->choices(index.column()):QStringList{};if(choices.isEmpty())return new navidog::ui::TextInput(p);auto* combo=new navidog::ui::Select(p);combo->setEditable(true);combo->addItems(choices);return combo;}
    void setEditorData(QWidget* e,const QModelIndex& i)const override{if(auto* combo=qobject_cast<QComboBox*>(e))combo->setCurrentText(i.data(Qt::EditRole).toString());else QStyledItemDelegate::setEditorData(e,i);}
    void setModelData(QWidget* e,QAbstractItemModel* m,const QModelIndex& i)const override{if(auto* combo=qobject_cast<QComboBox*>(e))m->setData(i,combo->currentText());else QStyledItemDelegate::setModelData(e,m,i);}
};
// Cell selection also avoids the Qt Cocoa synthesized-row accessibility lifetime bug.
QTableView* grid(QWidget* parent,const QString& name){auto* v=new QTableView(parent);v->setObjectName(name);v->setAlternatingRowColors(true);v->setSelectionBehavior(QAbstractItemView::SelectItems);v->setSelectionMode(QAbstractItemView::SingleSelection);v->verticalHeader()->setDefaultSectionSize(29);v->horizontalHeader()->setStretchLastSection(true);v->setContextMenuPolicy(Qt::CustomContextMenu);return v;}
}
TablePane::TablePane(const navidog::ConnectionProfile& p,const navidog::CatalogNode& n,bool existing,QWidget* parent):WorkspacePane(parent),profile(p) {
    setObjectName("tableDesigner");kind=existing?navidog::WorkspaceTabKind::AlterTable:navidog::WorkspaceTabKind::CreateTable;profileId=p.id;database=n.database;baseline.node=n;baseline.exists=existing;
    auto* layout=new QVBoxLayout(this);layout->setContentsMargins(0,0,0,0);layout->setSpacing(0);auto* toolbar=new QWidget;toolbar->setObjectName("tableToolbar");auto* tools=new QHBoxLayout(toolbar);tools->setContentsMargins(10,4,10,4);tools->setSpacing(4);
    save=new navidog::ui::Button("保存","saveTable");save->setProperty("primary",true);tools->addWidget(save);add=new navidog::ui::Button("添加字段","addColumn");insert=new navidog::ui::Button("插入字段","insertColumn");remove=new navidog::ui::Button("删除字段","removeColumn");up=new navidog::ui::Button("上移","moveColumnUp");down=new navidog::ui::Button("下移","moveColumnDown");for(auto* b:{add,insert,remove,up,down})tools->addWidget(b);
    reload=new navidog::ui::Button("重新读取","reloadTable");reload->setVisible(existing);tools->addWidget(reload);stop=new navidog::ui::Button("停止","stopTable");stop->setEnabled(false);tools->addWidget(stop);tools->addStretch();layout->addWidget(toolbar);
    auto* tableIdentity=new QWidget;auto* row=new QHBoxLayout(tableIdentity);row->setContentsMargins(12,8,12,8);row->addWidget(new QLabel("表名"));name=new navidog::ui::TextInput;name->setObjectName("tableName");name->setPlaceholderText("输入表名");name->setText(n.object);name->setReadOnly(existing);row->addWidget(name,1);auto* context=new QLabel(p.name+" / "+(n.schema.isEmpty()?n.database:n.schema));context->setObjectName("muted");row->addWidget(context);layout->addWidget(tableIdentity);
    sections=new QTabWidget;sections->setObjectName("tableDesignSections");sections->setDocumentMode(true);layout->addWidget(sections,1);
    fields=grid(this,"tableFields");fields->setSelectionBehavior(QAbstractItemView::SelectItems);model=new TableColumnModel(fields);fields->setModel(model);fields->setItemDelegate(new FieldDelegate(p.driver,fields));fields->setColumnWidth(0,170);fields->setColumnWidth(1,180);fields->setColumnWidth(2,85);fields->setColumnWidth(3,55);fields->setColumnWidth(4,180);fields->setColumnWidth(5,180);fields->setColumnWidth(6,220);fields->setColumnWidth(7,95);fields->setColumnWidth(8,230);fields->setColumnWidth(9,140);fields->setColumnHidden(6,p.driver==K::SQLite);fields->setColumnHidden(9,p.driver==K::Oracle);fieldPage_=new QWidget;auto* fieldLayout=new QVBoxLayout(fieldPage_);fieldLayout->setContentsMargins(0,0,0,0);fieldLayout->setSpacing(0);fieldLayout->addWidget(fields,1);
    auto* properties=new QWidget;properties->setObjectName("fieldProperties");auto* propertyGrid=new QGridLayout(properties);propertyGrid->setContentsMargins(14,12,14,12);propertyGrid->setHorizontalSpacing(12);propertyGrid->setVerticalSpacing(8);
    auto* defaultEdit=new navidog::ui::TextInput;defaultEdit->setObjectName("columnDefault");auto* collationEdit=new navidog::ui::TextInput;collationEdit->setObjectName("columnCollation");auto* generatedEdit=new navidog::ui::TextInput;generatedEdit->setObjectName("columnExpression");auto* identity=new navidog::ui::CheckBox("自增 / 标识");identity->setObjectName("columnIdentity");auto* generation=new navidog::ui::Select;generation->setObjectName("columnGenerationMode");generation->addItems({"","VIRTUAL","STORED","ALWAYS","BY DEFAULT"});
    propertyGrid->addWidget(new QLabel("默认值表达式"),0,0);propertyGrid->addWidget(defaultEdit,0,1);propertyGrid->addWidget(new QLabel("排序规则"),0,2);propertyGrid->addWidget(collationEdit,0,3);propertyGrid->addWidget(identity,0,4);propertyGrid->addWidget(new QLabel("生成表达式"),1,0);propertyGrid->addWidget(generatedEdit,1,1,1,3);propertyGrid->addWidget(generation,1,4);propertyGrid->setColumnStretch(1,1);propertyGrid->setColumnStretch(3,1);fieldLayout->addWidget(properties);sections->addTab(fieldPage_,"字段");
    for(int c=0;c<model->columnCount();++c)fields->setColumnHidden(c,!QVector<int>{0,1,2,3,6,11,12}.contains(c));fields->setColumnHidden(6,p.driver==K::SQLite);auto order=QVector<int>{0,1,11,12,2,3,6};for(int pos=0;pos<order.size();++pos)fields->horizontalHeader()->moveSection(fields->horizontalHeader()->visualIndex(order[pos]),pos);fields->setColumnWidth(0,180);fields->setColumnWidth(1,170);fields->setColumnWidth(11,90);fields->setColumnWidth(12,70);fields->setColumnWidth(2,85);fields->setColumnWidth(3,55);fields->setColumnWidth(6,250);fields->horizontalHeader()->setStretchLastSection(false);
    auto sync=[this,properties,defaultEdit,collationEdit,generatedEdit,identity,generation]{int r=fields->currentIndex().row();bool valid=r>=0 && r<model->columns.size();properties->setEnabled(valid);if(!valid)return;auto c=model->columns[r];QSignalBlocker b1(defaultEdit),b2(collationEdit),b3(generatedEdit),b4(identity),b5(generation);defaultEdit->setText(c.defaultExpression);collationEdit->setText(c.collation);generatedEdit->setText(c.generatedExpression);identity->setChecked(c.autoIncrement);generation->setCurrentText(c.generation);bool editable=c.protectedReason.isEmpty();defaultEdit->setEnabled(editable);collationEdit->setEnabled(editable);generatedEdit->setEnabled(editable);identity->setEnabled(editable);generation->setEnabled(editable);};
    connect(fields->selectionModel(),&QItemSelectionModel::currentRowChanged,this,sync);connect(model,&QAbstractItemModel::dataChanged,this,sync);connect(model,&QAbstractItemModel::modelReset,this,sync);
    auto editProperty=[this](int column,const QVariant& value,int role=Qt::EditRole){auto r=fields->currentIndex().row();if(r>=0)model->setData(model->index(r,column),value,role);};connect(defaultEdit,&QLineEdit::editingFinished,this,[=]{editProperty(4,defaultEdit->text());});connect(collationEdit,&QLineEdit::editingFinished,this,[=]{editProperty(9,collationEdit->text());});connect(generatedEdit,&QLineEdit::editingFinished,this,[=]{editProperty(8,generatedEdit->text());});connect(identity,&QCheckBox::toggled,this,[=](bool checked){editProperty(7,checked?Qt::Checked:Qt::Unchecked,Qt::CheckStateRole);});connect(generation,QOverload<int>::of(&QComboBox::currentIndexChanged),this,[=]{editProperty(10,generation->currentText());});sync();
    for(auto type:{O::Index,O::ForeignKey,O::Trigger,O::Check}){auto* page=new QWidget;page->setProperty("objectKind",int(type));auto* pageLayout=new QVBoxLayout(page);pageLayout->setContentsMargins(0,0,0,0);pageLayout->setSpacing(0);auto* view=grid(page,"table_"+navidog::tableObjectKindKey(type));auto* m=new TableObjectModel(type,p.driver,view);view->setModel(m);view->setItemDelegate(new ObjectDelegate(view));pageLayout->addWidget(view,1);objectModels[int(type)]=m;objectViews[int(type)]=view;view->setColumnWidth(0,200);for(int c=1;c<m->columnCount();++c)view->setColumnWidth(c,c==1?280:150);
        if (type == O::Trigger) {
            auto *editor = new SqlEditor;
            triggerSql = editor;
            editor->setObjectName("tableTriggerSql");
            editor->setDiagnosticContext(p.driver, n.database, n.schema);
            editor->setPlaceholderText("选择触发器，在这里编辑完整 CREATE TRIGGER 定义");
            auto *outlineToggle = new navidog::ui::Button("结构导航", "toggleSqlOutline");
            auto *triggerArea = new QWidget;
            auto *triggerLayout = new QVBoxLayout(triggerArea);
            triggerLayout->setContentsMargins(0, 2, 0, 0);
            triggerLayout->setSpacing(0);
            triggerLayout->addWidget(outlineToggle, 0, Qt::AlignLeft);
            triggerLayout->addWidget(new SqlEditorArea(editor, outlineToggle, false), 1);
            auto *split = new QSplitter(Qt::Vertical);
            pageLayout->removeWidget(view);
            split->addWidget(view);
            split->addWidget(triggerArea);
            split->setSizes({180, 350});
            pageLayout->addWidget(split);
            connect(view->selectionModel(), &QItemSelectionModel::currentRowChanged,
                    this, [this] { syncTriggerEditor(); });
            connect(editor, &QPlainTextEdit::textChanged, this, [this, m, view] {
                const int row = view->currentIndex().row();
                if (row < 0 || row >= m->objects.size()) return;
                m->objects[row].sql = triggerSql->toPlainText();
                emit m->dataChanged(m->index(row, 1), m->index(row, 1));
            });
        }
        sections->addTab(page,navidog::tableObjectKindName(type));
        connect(view,&QWidget::customContextMenuRequested,this,[this,view](QPoint at){navidog::ui::ContextMenu menu;menu.addAction(add->text(),this,[this]{addItem();});menu.addAction(remove->text(),this,[this]{removeItem();})->setEnabled(view->currentIndex().isValid());menu.execAt(view->viewport()->mapToGlobal(at),view);});
    }
    auto* settings=new QWidget;auto* form=new QFormLayout(settings);form->setContentsMargins(22,22,22,22);form->setFieldGrowthPolicy(QFormLayout::FieldsStayAtSizeHint);form->setLabelAlignment(Qt::AlignRight);form->setVerticalSpacing(12);
    auto option=[&](QString key,QString label){auto* input=new navidog::ui::TextInput;input->setObjectName("tableOption_"+key);input->setMinimumWidth(270);options[key]=input;form->addRow(label,input);};
    if(navidog::isMySqlFamily(p.driver)){option("engine","引擎");option("charset","默认字符集");option("collation","默认排序规则");option("rowFormat","行格式");}
    if(navidog::usesNativePostgres(p.driver) || p.driver==K::Oracle)option("tablespace","表空间");
    withoutRowid=new navidog::ui::CheckBox("WITHOUT ROWID");withoutRowid->setObjectName("tableWithoutRowid");strict=new navidog::ui::CheckBox("STRICT 类型检查");strict->setObjectName("tableStrict");if(p.driver==K::SQLite){form->addRow(withoutRowid);form->addRow(strict);}else{withoutRowid->hide();strict->hide();}
    if(form->rowCount())sections->addTab(settings,"选项");else settings->deleteLater();comment=new QPlainTextEdit;comment->setObjectName("tableComment");comment->setPlaceholderText("表注释");if(p.driver!=K::SQLite)sections->addTab(comment,"注释");else comment->hide();
    auto* sqlPreview=new SqlReadOnlyView(this);sqlPreview->setDatabaseKind(p.driver);sql=sqlPreview;sql->setObjectName("tableSqlPreview");sections->addTab(sql->withActions(),"SQL 预览");message=new QLabel;message->setObjectName("tableDesignMessage");message->setWordWrap(true);message->setMargin(9);layout->addWidget(message);
    connect(save,&QToolButton::clicked,this,&TablePane::requestSave);connect(reload,&QToolButton::clicked,this,&TablePane::requestReload);connect(stop,&QToolButton::clicked,this,&TablePane::stopRequested);connect(add,&QToolButton::clicked,this,[this]{addItem();});connect(insert,&QToolButton::clicked,this,[this]{addItem(true);});connect(remove,&QToolButton::clicked,this,&TablePane::removeItem);connect(up,&QToolButton::clicked,this,[this]{auto r=fields->currentIndex().row();model->moveColumn(r,-1);fields->setCurrentIndex(model->index(qMax(0,r-1),0));});connect(down,&QToolButton::clicked,this,[this]{auto r=fields->currentIndex().row();model->moveColumn(r,1);fields->setCurrentIndex(model->index(qMin(model->rowCount()-1,r+1),0));});
    auto changed=[this]{updateSql();emit draftChanged();};for(auto* m:objectModels){connect(m,&QAbstractItemModel::dataChanged,this,changed);connect(m,&QAbstractItemModel::rowsInserted,this,changed);connect(m,&QAbstractItemModel::rowsRemoved,this,changed);}connect(model,&QAbstractItemModel::dataChanged,this,changed);connect(model,&QAbstractItemModel::rowsInserted,this,changed);connect(model,&QAbstractItemModel::rowsRemoved,this,changed);connect(model,&QAbstractItemModel::rowsMoved,this,changed);connect(name,&QLineEdit::textChanged,this,changed);connect(comment,&QPlainTextEdit::textChanged,this,changed);for(auto* input:options)connect(input,&QLineEdit::textChanged,this,changed);connect(withoutRowid,&QCheckBox::toggled,this,changed);connect(strict,&QCheckBox::toggled,this,changed);connect(sections,&QTabWidget::currentChanged,this,[this]{commitEditor();updateTools();});
    connect(fields,&QWidget::customContextMenuRequested,this,[this](QPoint at){navidog::ui::ContextMenu menu;menu.addAction("添加字段",this,[this]{addItem();});menu.addAction("插入字段",this,[this]{addItem(true);});menu.addAction("删除字段",this,[this]{removeItem();});menu.addSeparator();auto index=fields->currentIndex();menu.addAction("主键",this,[this,index]{if(index.isValid())model->setData(model->index(index.row(),3),model->columns[index.row()].primaryKey?Qt::Unchecked:Qt::Checked,Qt::CheckStateRole);})->setEnabled(index.isValid());menu.execAt(fields->viewport()->mapToGlobal(at),fields);});
    if(!existing){navidog::TableColumn c;c.name="id";c.type=p.driver==K::Oracle?"NUMBER":"INTEGER";c.primaryKey=1;c.nullable=false;model->resetColumns({c});fields->setCurrentIndex(model->index(0,0));loaded=true;baseline.detailsLoaded=true;}updateTools();updateSql();
}
navidog::TableDefinition TablePane::definition()const {auto d=baseline;d.node.object=d.node.name=name->text().trimmed();d.columns=model->columns;d.objects.clear();for(auto key:objectModels.keys())d.objects<<objectModels[key]->objects;d.comment=comment->toPlainText();if(options.contains("engine"))d.engine=options["engine"]->text().trimmed();if(options.contains("charset"))d.charset=options["charset"]->text().trimmed();if(options.contains("collation"))d.collation=options["collation"]->text().trimmed();if(options.contains("rowFormat"))d.rowFormat=options["rowFormat"]->text().trimmed();if(options.contains("tablespace"))d.tablespace=options["tablespace"]->text().trimmed();d.withoutRowid=withoutRowid->isChecked();d.strict=strict->isChecked();return d;}
bool TablePane::dirty()const {return !navidog::sameTableStructure(definition(),baseline) || name->text().trimmed()!=baseline.node.object;}
void TablePane::commitEditor(){save->setFocus();if(auto* focus=QApplication::focusWidget();focus && isAncestorOf(focus) && (focus->parent()==fields->viewport() || qobject_cast<QTableView*>(focus->parentWidget()))){focus->clearFocus();}for(auto* v:objectViews)v->setFocus();QApplication::sendPostedEvents();}
void TablePane::focusName() {
    name->setFocus();
}

bool TablePane::matchesTable(const QString &id, const navidog::CatalogNode &table,
                             bool wholeSchema) const {
    const auto &node = tableNode();
    return profileId == id && node.database == table.database &&
           node.schema == table.schema && (wholeSchema || node.object == table.object);
}

QString TablePane::tabTitle() const {
    return QString(baseline.exists ? "修改表 · " : "新建表 · ") +
           (name->text().isEmpty() ? "未命名" : name->text()) + (dirty() ? " *" : "");
}

bool TablePane::canSave() const {
    return save->isEnabled();
}

void TablePane::requestReload() {
    emit reloadRequested();
}

void TablePane::requestSave() {
    if (canSave())
        emit saveRequested();
}

void TablePane::setConnectionProfile(const navidog::ConnectionProfile &value) {
    profile = value;
    triggerSql->setDiagnosticContext(profile.driver, baseline.node.database, baseline.node.schema);
}

void TablePane::setDesignInProgress(bool running, const QString &status) {
    designInProgress_ = running;
    setBusy(workspaceBusy_);
    if (!status.isEmpty())
        message->setText(status);
}

void TablePane::invalidateTable(const QString &replacementName) {
    needsReload = true;
    if (!replacementName.isEmpty()) {
        baseline.node.name = baseline.node.object = replacementName;
        name->setText(replacementName);
    }
    updateSql();
    message->setText("表已变化，请重新读取结构。");
}

void TablePane::invalidateConnection() {
    needsReload = true;
    loaded = false;
    setBusy(workspaceBusy_);
    message->setText("连接配置已修改，请重新读取表结构。");
}

void TablePane::updateSql() {
    try {
        const auto changes = navidog::tableChangeSql(profile, baseline, definition());
        sql->setSourceSql(changes.join(";\n\n") + (changes.isEmpty() ? "" : ";"));
        if (!designInProgress_) {
            const bool autoCommit = navidog::isMySqlFamily(profile.driver) ||
                                    profile.driver == K::Oracle;
            message->setText(changes.isEmpty() ? QString("没有待保存的修改")
                : QString("%1 条待执行 SQL%2").arg(changes.size())
                      .arg(autoCommit ? "；此数据库的 DDL 会自动提交" : ""));
            if (needsReload)
                message->setText("需要重新读取完整表结构；重新读取前可复制 SQL 保留当前草稿。");
        }
        save->setEnabled(!workspaceBusy_ && !designInProgress_ && loaded &&
                         !needsReload && !profile.readOnly && !profile.http.enabled &&
                         !changes.isEmpty());
    } catch (const std::exception &error) {
        sql->setSourceSql({});
        if (!designInProgress_)
            message->setText(QString::fromUtf8(error.what()));
        save->setEnabled(false);
    }
}

void TablePane::setBusy(bool busy) {
    workspaceBusy_ = busy;
    const bool interactive = !busy && !designInProgress_;
    reload->setEnabled(interactive);
    for (auto *button : {add, insert, remove, up, down})
        button->setEnabled(interactive && loaded && !profile.readOnly);
    sections->setEnabled(interactive && loaded);
    name->setEnabled(interactive);
    stop->setEnabled(designInProgress_);
    save->setEnabled(false);
    if (interactive)
        updateSql();
}

QString TablePane::applyDesignResult(const navidog::TableDesignResult &result, bool saved) {
    QString detail;
    if (result.cancelled || !result.error.isEmpty()) {
        needsReload |= result.partial;
        updateSql();
        detail = result.error;
        if (result.rolledBack)
            detail += "\n本次表结构修改已回滚。";
        if (result.partial)
            detail += "\n已有部分 DDL 提交，请重新读取表结构后再修改。";
    } else {
        acceptDefinition(result.definition);
        detail = !result.refreshError.isEmpty()
            ? "表结构已保存，但刷新未完成，请重新读取。\n" + result.refreshError
            : saved ? "表结构已保存" : "已读取表结构";
        emit draftChanged();
    }
    message->setText(detail);
    return detail;
}
void TablePane::acceptDefinition(const navidog::TableDefinition& d){baseline=d;loaded=true;needsReload=d.exists && !d.detailsLoaded;kind=navidog::WorkspaceTabKind::AlterTable;name->setText(d.node.object);name->setReadOnly(true);model->resetColumns(d.columns);if(model->rowCount())fields->setCurrentIndex(model->index(0,0));for(auto* m:objectModels)m->resetObjects(d.objects);{QSignalBlocker block(comment);comment->setPlainText(d.comment);}auto values=QMap<QString,QString>{{"engine",d.engine},{"charset",d.charset},{"collation",d.collation},{"rowFormat",d.rowFormat},{"tablespace",d.tablespace}};for(auto key:options.keys()){QSignalBlocker block(options[key]);options[key]->setText(values[key]);}{QSignalBlocker block(withoutRowid);withoutRowid->setChecked(d.withoutRowid);}{QSignalBlocker block(strict);strict->setChecked(d.strict);}reload->show();updateTools();setBusy(workspaceBusy_);syncTriggerEditor();if(!pendingKind.isEmpty()){auto k=pendingKind,n=pendingName;auto op=pendingOperation;pendingKind.clear();focusObject(k,n,op);}}
QJsonObject TablePane::sessionState()const {return {{"kind",baseline.exists?"alterTable":"createTable"},{"profileId",profileId},{"database",database},{"schema",baseline.node.schema},{"object",baseline.node.object},{"tableName",name->text()},{"baseline",navidog::tableDefinitionJson(baseline)},{"draft",navidog::tableDefinitionJson(definition())},{"loaded",loaded},{"needsReload",needsReload},{"section",sections->currentIndex()}};}
void TablePane::restoreState(const QJsonObject& state){auto before=state["baseline"],draft=state["draft"];bool legacy=before.isArray();if(legacy){before=QJsonObject{{"columns",before}};draft=QJsonObject{{"columns",state["columns"]}};}auto savedNode=baseline.node;bool exists=baseline.exists;auto b=navidog::tableDefinitionFromJson(before.toObject());b.node=savedNode;b.exists=exists;auto after=navidog::tableDefinitionFromJson(draft.toObject());after.node=savedNode;after.node.object=after.node.name=state["tableName"].toString();after.exists=exists;acceptDefinition(after);baseline=b;kind=exists?navidog::WorkspaceTabKind::AlterTable:navidog::WorkspaceTabKind::CreateTable;name->setReadOnly(exists);reload->setVisible(exists);loaded=state["loaded"].toBool();needsReload=state["needsReload"].toBool() || (exists && !b.detailsLoaded);sections->setCurrentIndex(qBound(0,state["section"].toInt(),sections->count()-1));setBusy(workspaceBusy_);}
void TablePane::updateTools(){bool column=sections->currentWidget()==fieldPage_;auto value=sections->currentWidget()->property("objectKind");bool object=value.isValid();auto label=column?QString("字段"):object?navidog::tableObjectKindName(O(value.toInt())):QString{};add->setVisible(column || object);remove->setVisible(column || object);add->setText("添加"+label);remove->setText("删除"+label);bool ordered=column && (!baseline.exists || profile.driver==K::SQLite || navidog::isMySqlFamily(profile.driver));insert->setVisible(ordered);up->setVisible(ordered);down->setVisible(ordered);}
void TablePane::addItem(bool insertAtCurrent){if(sections->currentWidget()==fieldPage_){int r=insertAtCurrent?qMax(0,fields->currentIndex().row()):model->rowCount();model->insertColumn(r,profile.driver);fields->setCurrentIndex(model->index(r,0));fields->edit(model->index(r,0));return;}auto v=sections->currentWidget()->property("objectKind");if(!v.isValid())return;auto type=O(v.toInt());auto* m=objectModels[int(type)];navidog::TableObject o;o.kind=type;auto base=navidog::tableObjectKindKey(type)+"_";int n=m->rowCount()+1;do{o.name=base+QString::number(n++);}while(std::any_of(m->objects.begin(),m->objects.end(),[&](auto a){return a.name==o.name;}));if(type==O::ForeignKey)o.referenceSchema=navidog::isMySqlFamily(profile.driver)?baseline.node.database:baseline.node.schema;
    if(type==O::Trigger){auto d=definition();auto table=navidog::tableQualifiedName(profile,d.node);auto trigger=d.node;trigger.object=o.name;auto id=navidog::tableQualifiedName(profile,trigger);if(profile.driver==K::SQLite)o.sql="CREATE TRIGGER "+id+" AFTER INSERT ON "+navidog::quoteIdentifier(d.node.object,profile.driver)+" BEGIN\n    SELECT 1;\nEND";else if(navidog::usesNativePostgres(profile.driver))o.sql="CREATE TRIGGER "+navidog::quoteIdentifier(o.name,profile.driver)+" AFTER INSERT ON "+table+" FOR EACH ROW EXECUTE FUNCTION "+navidog::quoteIdentifier(d.node.schema,profile.driver)+".trigger_function()";else if(profile.driver==K::SQLServer)o.sql="CREATE TRIGGER "+id+" ON "+table+" AFTER INSERT AS\nBEGIN\n    SET NOCOUNT ON;\nEND";else if(profile.driver==K::Oracle)o.sql="CREATE OR REPLACE TRIGGER "+id+" AFTER INSERT ON "+table+" FOR EACH ROW\nBEGIN\n    NULL;\nEND;";else o.sql="CREATE TRIGGER "+id+" AFTER INSERT ON "+table+" FOR EACH ROW\nBEGIN\n    SET @navidog_trigger_seen = 1;\nEND";}
    m->append(o);auto* view=objectViews[int(type)];view->setCurrentIndex(m->index(m->rowCount()-1,0));view->edit(view->currentIndex());}
void TablePane::removeItem(){if(sections->currentWidget()==fieldPage_){model->removeColumn(fields->currentIndex().row());return;}auto v=sections->currentWidget()->property("objectKind");if(v.isValid()){auto* m=objectModels[v.toInt()];m->remove(objectViews[v.toInt()]->currentIndex().row());syncTriggerEditor();}}
void TablePane::syncTriggerEditor(){auto* m=objectModels[int(O::Trigger)];int r=objectViews[int(O::Trigger)]->currentIndex().row();QSignalBlocker block(triggerSql);bool valid=r>=0 && r<m->objects.size();triggerSql->setEnabled(valid && m->objects[r].protectedReason.isEmpty());triggerSql->setPlainText(valid?m->objects[r].sql:QString{});}
void TablePane::focusObject(const QString& key,const QString& object,int operation){if(!loaded){pendingKind=key;pendingName=object;pendingOperation=operation;return;}if(key=="primaryKey"){sections->setCurrentWidget(fieldPage_);if(operation==2){for(int i=0;i<model->rowCount();++i)model->setData(model->index(i,3),Qt::Unchecked,Qt::CheckStateRole);}return;}if(key=="column" || key=="columns"){sections->setCurrentWidget(fieldPage_);int r=-1;for(int i=0;i<model->rowCount();++i)if(model->columns[i].name==object)r=i;if(r>=0){fields->setCurrentIndex(model->index(r,0));fields->scrollTo(model->index(r,0));}if(operation==1)addItem();else if(operation==2 && r>=0)removeItem();return;}
    auto type=key=="index" || key=="indexes"?O::Index:key=="foreignKey" || key=="foreignKeys"?O::ForeignKey:key=="check" || key=="checks"?O::Check:O::Trigger;for(int i=0;i<sections->count();++i)if(sections->widget(i)->property("objectKind").isValid() && sections->widget(i)->property("objectKind").toInt()==int(type))sections->setCurrentIndex(i);auto* m=objectModels[int(type)];int r=-1;for(int i=0;i<m->rowCount();++i)if(m->objects[i].name==object)r=i;auto* view=objectViews[int(type)];if(r>=0){view->setCurrentIndex(m->index(r,0));view->scrollTo(m->index(r,0));}if(operation==1)addItem();else if(operation==2 && r>=0)removeItem();else if((operation==3 || operation==4) && r>=0)m->setData(m->index(r,2),operation==3?Qt::Checked:Qt::Unchecked,Qt::CheckStateRole);syncTriggerEditor();}
