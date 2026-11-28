#include "data_pane.hpp"
#include "components.hpp"
#include "navidog/operation_timing.hpp"
#include "navidog/query_service.hpp"
#include "result_grid.hpp"
#include <QTableView>
#include <QHeaderView>
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QColor>
#include <QFont>
#include <QStyledItemDelegate>
#include <QMenu>
#include <QMessageBox>
#include <QPlainTextEdit>
#include <QJsonDocument>
#include <QRegularExpression>
#include <QShortcut>
#include <QClipboard>
#include <QApplication>
#include <QSignalBlocker>
#include <QUuid>
#include <utility>
namespace {
bool nullValue(const QVariant& v){return !v.isValid() || v.isNull();}
bool sameValue(const QVariant& a,const QVariant& b){return nullValue(a)||nullValue(b)?nullValue(a)&&nullValue(b):a.userType()==b.userType() && a==b;}
bool binary(const navidog::TableColumn& c){return c.type.contains("BLOB",Qt::CaseInsensitive)||c.type.compare("bytea",Qt::CaseInsensitive)==0;}
class CellDelegate final : public QStyledItemDelegate {
public:
    using QStyledItemDelegate::QStyledItemDelegate;
    QWidget* createEditor(QWidget* parent,const QStyleOptionViewItem&,const QModelIndex&)const override{return new navidog::ui::TextInput(parent);}
    void setEditorData(QWidget* editor,const QModelIndex& index)const override{static_cast<QLineEdit*>(editor)->setText(index.data(Qt::EditRole).toString());}
    void setModelData(QWidget* editor,QAbstractItemModel* model,const QModelIndex& index)const override{model->setData(index,static_cast<QLineEdit*>(editor)->text());}
};
}
QVariant DataGridModel::data(const QModelIndex& i,int role)const {
    if(!i.isValid() || i.row()>=rows.size() || i.column()>=schema.columns.size())return {};const auto& row=rows[i.row()];const auto& value=row.values[i.column()];bool defaultValue=row.state==navidog::DataRowState::Inserted && !row.touched.contains(i.column());
    if(role==Qt::EditRole)return value.userType()==QMetaType::QByteArray?QString::fromLatin1(value.toByteArray().toHex()):value;
    if(role==Qt::DisplayRole || role==Qt::ToolTipRole) {
        if(defaultValue)return QString("DEFAULT");if(nullValue(value))return QString("NULL");if(value.userType()==QMetaType::QByteArray)return QString("[BLOB: %1 bytes]").arg(value.toByteArray().size());auto text=value.toString();auto max=role==Qt::ToolTipRole?1024:150;return text.size()>max?text.left(max)+"…":text;
    }
    if(role==Qt::BackgroundRole){if(row.state==navidog::DataRowState::Deleted)return QColor("#fbe8e6");if(row.state==navidog::DataRowState::Inserted)return QColor("#e7f4ec");if(row.touched.contains(i.column()))return QColor("#fff2ce");}
    if(role==Qt::FontRole && row.state==navidog::DataRowState::Deleted){QFont f;f.setStrikeOut(true);return f;}
    if(role==Qt::ForegroundRole && (defaultValue || nullValue(value)))return QColor("#8291a1");
    return {};
}
QVariant DataGridModel::headerData(int index,Qt::Orientation o,int role)const {
    if(o==Qt::Horizontal && role==Qt::UserRole)return index>=0 && index<schema.columns.size()?QVariant(schema.columns[index].name):QVariant{};
    if(role!=Qt::DisplayRole)return {};
    if(o==Qt::Horizontal){if(index<0 || index>=schema.columns.size())return {};auto c=schema.columns[index];return c.name+"\n"+c.type;}
    if(index<0 || index>=rows.size())return {};auto state=rows[index].state;return QString(state==navidog::DataRowState::Inserted?"+ ":state==navidog::DataRowState::Deleted?"− ":state==navidog::DataRowState::Modified?"● ":"")+QString::number(index+1);
}
Qt::ItemFlags DataGridModel::flags(const QModelIndex& i)const {
    auto f=QAbstractTableModel::flags(i);if(i.isValid() && editable && rows[i.row()].state!=navidog::DataRowState::Deleted && schema.columns[i.column()].protectedReason.isEmpty() && schema.columns[i.column()].generatedExpression.isEmpty())f|=Qt::ItemIsEditable;return f;
}
bool DataGridModel::setData(const QModelIndex& i,const QVariant& input,int role) {
    if(role!=Qt::EditRole || !(flags(i)&Qt::ItemIsEditable))return false;auto value=input;
    if(binary(schema.columns[i.column()]) && value.userType()==QMetaType::QString){auto hex=value.toString();hex.remove(' ');if(!QRegularExpression("^(?:[0-9a-fA-F]{2})*$").match(hex).hasMatch())return false;value=QByteArray::fromHex(hex.toLatin1());}
    if(value.userType()==QMetaType::QByteArray?value.toByteArray().size()>65536:value.toString().size()>32768)return false;
    auto& row=rows[i.row()];row.values[i.column()]=value;row.touched.insert(i.column());
    if(row.state!=navidog::DataRowState::Inserted){if(sameValue(value,row.original[i.column()]))row.touched.remove(i.column());row.state=row.touched.isEmpty()?navidog::DataRowState::Clean:navidog::DataRowState::Modified;}
    emit dataChanged(i,i);emit headerDataChanged(Qt::Vertical,i.row(),i.row());emit pendingChanged();return true;
}
void DataGridModel::setPage(const navidog::DataPageResult& page){beginResetModel();schema=page.schema;editable=page.editable;rows.clear();for(int i=0;i<page.result.rows.size();++i){navidog::DataRowChange row;row.original=row.values=page.result.rows[i];row.version=page.versions.value(i);rows<<row;}endResetModel();emit pendingChanged();}
void DataGridModel::addRow(){if(!editable || rows.size()>=1000)return;int index=rows.size();beginInsertRows({},index,index);navidog::DataRowChange row;row.state=navidog::DataRowState::Inserted;row.values.resize(schema.columns.size());rows<<row;endInsertRows();emit pendingChanged();}
void DataGridModel::deleteRow(int index){if(!editable || index<0 || index>=rows.size())return;auto& row=rows[index];if(row.state==navidog::DataRowState::Inserted){beginRemoveRows({},index,index);rows.removeAt(index);endRemoveRows();}else{row.state=row.state==navidog::DataRowState::Deleted?(row.touched.isEmpty()?navidog::DataRowState::Clean:navidog::DataRowState::Modified):navidog::DataRowState::Deleted;emit dataChanged(this->index(index,0),this->index(index,columnCount()-1));emit headerDataChanged(Qt::Vertical,index,index);}emit pendingChanged();}
void DataGridModel::restoreCell(const QModelIndex& i){if(!i.isValid() || i.row()>=rows.size())return;auto& row=rows[i.row()];if(row.state==navidog::DataRowState::Inserted){useDefault(i);return;}setData(i,row.original[i.column()]);}
void DataGridModel::useDefault(const QModelIndex& i){if(!i.isValid() || i.row()>=rows.size() || rows[i.row()].state!=navidog::DataRowState::Inserted)return;rows[i.row()].touched.remove(i.column());rows[i.row()].values[i.column()]={};emit dataChanged(i,i);emit pendingChanged();}
void DataGridModel::discard(){beginResetModel();for(int i=rows.size()-1;i>=0;--i){if(rows[i].state==navidog::DataRowState::Inserted)rows.removeAt(i);else{rows[i].values=rows[i].original;rows[i].state=navidog::DataRowState::Clean;rows[i].touched.clear();}}endResetModel();emit pendingChanged();}
int DataGridModel::pendingCount()const{int n=0;for(auto row:rows)if(row.state!=navidog::DataRowState::Clean)++n;return n;}
QVector<navidog::DataRowChange> DataGridModel::changes()const{QVector<navidog::DataRowChange> out;for(auto row:rows)if(row.state!=navidog::DataRowState::Clean)out<<row;return out;}
navidog::QueryResult DataGridModel::exportResult()const{navidog::QueryResult result;for(auto c:schema.columns){result.columns<<c.name;result.columnTypes<<c.type;}for(auto row:rows)if(row.state!=navidog::DataRowState::Deleted)result.rows<<row.values;return result;}
DataPane::DataPane(const navidog::ConnectionProfile &p, const navidog::CatalogNode &n,
                   navidog::QueryService &service, ResolveProfile resolveProfile,
                   QWidget *parent)
    : WorkspacePane(parent), service_(service), resolveProfile_(std::move(resolveProfile)),
      exportContext_(navidog::ui::exportContext(p, n.database, n.schema)),
      driver_(p.driver), node(n) {
    kind=navidog::WorkspaceTabKind::TableData;profileId=p.id;database=n.database;page.request.node=n;pending.node=n;setObjectName("tableDataPane");
    auto* layout=new QVBoxLayout(this);layout->setContentsMargins(0,0,0,0);layout->setSpacing(0);
    auto* toolbar=new QWidget;toolbar->setObjectName("dataToolbar");auto* tools=new QHBoxLayout(toolbar);tools->setContentsMargins(8,3,8,3);tools->setSpacing(4);
    auto button=[&](const char* text,const char* name){auto* b=new navidog::ui::Button(text,name);tools->addWidget(b);return b;};
    refresh=button("刷新","refreshData");add=button("新增记录","insertDataRow");remove=button("删除记录","deleteDataRow");commit=button("提交修改","commitData");commit->setProperty("primary",true);discard=button("放弃修改","discardData");auto* cell=button("查看单元格","viewDataCell");stop=button("停止","stopData");tools->addStretch();layout->addWidget(toolbar);
    auto* filter=new QWidget;filter->setObjectName("dataFilterToolbar");auto* controls=new QHBoxLayout(filter);controls->setContentsMargins(8,3,8,3);controls->setSpacing(5);
    filterColumn=new navidog::ui::Select;filterColumn->setObjectName("dataFilterColumn");filterColumn->addItem("筛选字段",-1);controls->addWidget(filterColumn);
    filterOp=new navidog::ui::Select;filterOp->setObjectName("dataFilterOperator");
    using F=navidog::DataFilter;for(auto x:QVector<QPair<QString,F>>{{"不筛选",F::None},{"等于",F::Equal},{"不等于",F::NotEqual},{"包含",F::Contains},{"小于",F::Less},{"大于",F::Greater},{"为 NULL",F::IsNull},{"非 NULL",F::IsNotNull}})filterOp->addItem(x.first,int(x.second));controls->addWidget(filterOp);
    filterValue=new navidog::ui::TextInput;filterValue->setObjectName("dataFilterValue");filterValue->setPlaceholderText("筛选值");controls->addWidget(filterValue,1);
    sortColumn=new navidog::ui::Select;sortColumn->setObjectName("dataSortColumn");sortColumn->addItem("按主键排序",-1);controls->addWidget(sortColumn);
    direction=new navidog::ui::Select;direction->setObjectName("dataSortDirection");direction->addItems({"升序","降序"});controls->addWidget(direction);
    apply=new navidog::ui::Button("应用","applyDataFilter");controls->addWidget(apply);layout->addWidget(filter);
    grid=new navidog::ui::ObjectGrid;grid->setObjectName("dataGrid");model=new DataGridModel(grid);grid->setModel(model);grid->setItemDelegate(new CellDelegate(grid));grid->setAlternatingRowColors(true);grid->setSelectionMode(QAbstractItemView::ExtendedSelection);grid->setSelectionBehavior(QAbstractItemView::SelectItems);grid->setEditTriggers(QAbstractItemView::DoubleClicked|QAbstractItemView::EditKeyPressed);grid->verticalHeader()->setDefaultSectionSize(24);grid->horizontalHeader()->setDefaultSectionSize(170);grid->horizontalHeader()->setStretchLastSection(false);grid->horizontalHeader()->setDefaultAlignment(Qt::AlignLeft|Qt::AlignVCenter);grid->setWordWrap(false);layout->addWidget(grid,1);
    auto* footer=new QWidget;auto* nav=new QHBoxLayout(footer);nav->setContentsMargins(8,3,8,3);nav->setSpacing(5);
    first=new navidog::ui::Button("首页","firstDataPage");previous=new navidog::ui::Button("上一页","previousDataPage");next=new navidog::ui::Button("下一页","nextDataPage");nav->addWidget(first);nav->addWidget(previous);nav->addWidget(next);
    pageSize=new navidog::ui::Select;pageSize->setObjectName("dataPageSize");for(int size:{100,250,500,1000})pageSize->addItem(QString::number(size)+" 行 / 页",size);nav->addWidget(pageSize);summary=new QLabel("点击刷新加载数据");summary->setObjectName("dataSummary");nav->addWidget(summary,1);auto* exportButton=new navidog::ui::Button("导出…","exportDataCsv");nav->addWidget(exportButton);layout->addWidget(footer);
    transaction=new QLabel("修改暂存在当前页，点击提交后统一写入事务；失败时整批回滚。");transaction->setObjectName("dataTransactionStatus");transaction->setMargin(8);transaction->setWordWrap(true);layout->addWidget(transaction);
    connect(model,&DataGridModel::pendingChanged,this,[this]{updateActions();emit tabTitleChanged(tabTitle());});connect(grid->selectionModel(),&QItemSelectionModel::currentChanged,this,[this]{updateActions();});
    connect(add,&QToolButton::clicked,this,[this]{model->addRow();auto index=model->index(model->rowCount()-1,0);grid->setCurrentIndex(index);grid->scrollTo(index);});connect(remove,&QToolButton::clicked,this,[this]{model->deleteRow(grid->currentIndex().row());});
    connect(commit, &QToolButton::clicked, this, &DataPane::commitChanges);
    connect(discard, &QToolButton::clicked, model, &DataGridModel::discard);
    connect(stop, &QToolButton::clicked, this, [this] {
        if (operation_ != Operation::Idle)
            service_.cancel();
    });
    connect(cell, &QToolButton::clicked, this, &DataPane::showCell);
    connect(exportButton, &QToolButton::clicked, this, &DataPane::exportData);
    connect(refresh,&QToolButton::clicked,this,&DataPane::requestRefresh);
    connect(apply,&QToolButton::clicked,this,[this]{if(confirmDiscard())queue(controlsRequest(),{});});connect(filterValue,&QLineEdit::returnPressed,apply,&QToolButton::click);
    connect(first,&QToolButton::clicked,this,[this]{auto request=page.request;request.offset=0;request.afterKey.clear();queue(request,{});});
    connect(previous,&QToolButton::clicked,this,[this]{if(history_.isEmpty())return;auto history=history_;auto request=history.takeLast();queue(request,history);});
    connect(next,&QToolButton::clicked,this,[this]{auto request=page.request;request.offset+=page.result.rows.size();request.afterKey.clear();auto keys=navidog::primaryKeyColumns(page.schema);if(!keys.isEmpty() && !request.descending && (request.sortColumn==-1 || request.sortColumn==keys[0]) && !page.result.rows.isEmpty() && !page.result.cellTruncated){for(int key:keys)request.afterKey<<page.result.rows.last()[key];}auto history=history_;history<<page.request;queue(request,history);});
    connect(pageSize,QOverload<int>::of(&QComboBox::activated),this,[this]{if(confirmDiscard())queue(controlsRequest(),{});});
    connect(filterOp,QOverload<int>::of(&QComboBox::currentIndexChanged),this,[this]{auto op=navidog::DataFilter(filterOp->currentData().toInt());filterValue->setEnabled(op!=navidog::DataFilter::IsNull && op!=navidog::DataFilter::IsNotNull && !busy_);});
    grid->setContextMenuPolicy(Qt::CustomContextMenu);connect(grid,&QWidget::customContextMenuRequested,this,[this](QPoint point){auto index=grid->indexAt(point);if(!index.isValid())return;grid->setCurrentIndex(index);navidog::ui::ContextMenu menu(this);menu.addAction("查看单元格",this,&DataPane::showCell);menu.addAction("复制并包含列名",this,[this]{try{auto text=navidog::ui::resultSelectionText(grid,true,Qt::EditRole);if(!text.isNull())QApplication::clipboard()->setText(text);}catch(const std::exception& e){summary->setText(QString::fromUtf8(e.what()));}});if(model->flags(index)&Qt::ItemIsEditable){menu.addAction("设为 NULL",this,[this,index]{model->setData(index,QVariant{});});menu.addAction("还原单元格",this,[this,index]{model->restoreCell(index);});if(model->rows[index.row()].state==navidog::DataRowState::Inserted)menu.addAction("使用默认值",this,[this,index]{model->useDefault(index);});}menu.execAt(grid->viewport()->mapToGlobal(point),grid);});
    auto* copy=new QShortcut(QKeySequence::Copy,grid);copy->setContext(Qt::WidgetWithChildrenShortcut);connect(copy,&QShortcut::activated,this,[this]{try{auto text=navidog::ui::resultSelectionText(grid,false,Qt::EditRole);if(!text.isNull())QApplication::clipboard()->setText(text);}catch(const std::exception& e){summary->setText(QString::fromUtf8(e.what()));}});
    connect(&service_, &navidog::QueryService::dataPageCompleted,
            this, &DataPane::finishPage);
    connect(&service_, &navidog::QueryService::dataCommitCompleted,
            this, &DataPane::finishCommit);
    setBusy(service_.busy());
}

void DataPane::updateActions() {
    const bool dirty = this->dirty();
    const bool writable = loaded && model->editable && !busy_ && !blocked;
    const bool canFilter = loaded && !busy_;
    add->setEnabled(writable && model->rowCount() < 1000);
    remove->setEnabled(writable && grid->currentIndex().isValid());
    commit->setEnabled(writable && dirty);
    discard->setEnabled(!busy_ && dirty);
    refresh->setEnabled(!busy_);
    stop->setEnabled(operation_ != Operation::Idle);
    first->setEnabled(!busy_ && loaded && !dirty && page.request.offset > 0);
    previous->setEnabled(!busy_ && !dirty && !history_.isEmpty());
    next->setEnabled(!busy_ && loaded && !dirty && page.hasMore && !page.result.truncated);
    for (auto *control : QList<QWidget *>{apply, filterColumn, filterOp, filterValue,
                                          sortColumn, direction, pageSize})
        control->setEnabled(canFilter);
    grid->setEnabled(!busy_);
    if (loaded)
        transaction->setText(!page.editable ? page.readOnlyReason
                           : blocked ? "提交结果待核对，请重新加载确认后再编辑"
                           : dirty ? QString("%1 行待提交；修改尚未写入数据库").arg(model->pendingCount())
                                   : "修改暂存在当前页，提交时使用独立事务；失败整批回滚。");
}

void DataPane::setBusy(bool busy) {
    busy_ = busy || operation_ != Operation::Idle;
    updateActions();
}

std::optional<navidog::ConnectionProfile> DataPane::targetProfile() {
    auto target = resolveProfile_();
    if (target) {
        if (target->driver != navidog::DatabaseKind::SQLite && !database.isEmpty())
            target->database = database;
        exportContext_ = navidog::ui::exportContext(*target, node.database, node.schema);
        driver_ = target->driver;
    }
    return target;
}

bool DataPane::loadPage() {
    if (service_.busy()) {
        emit statusChanged("请等待当前数据库操作完成");
        return false;
    }
    const auto profile = targetProfile();
    const auto id = QUuid::createUuid().toString(QUuid::Id128);
    if (!profile || !service_.dataPage(*profile, pending, id))
        return false;
    startOperation(Operation::Load, id, "正在读取表数据…");
    return true;
}

void DataPane::commitChanges() {
    if (service_.busy() || !dirty() || blocked)
        return;
    grid->setFocus();
    QApplication::sendPostedEvents();
    const auto profile = targetProfile();
    const auto id = QUuid::createUuid().toString(QUuid::Id128);
    if (profile && service_.commitData(*profile, model->schema, model->changes(), id))
        startOperation(Operation::Commit, id, "正在事务中提交修改…");
}

void DataPane::startOperation(Operation operation, const QString &id, const QString &message) {
    operation_ = operation;
    requestId_ = id;
    setBusy(true);
    emit activityChanged();
    emit statusChanged(message);
}

void DataPane::finishOperation() {
    operation_ = Operation::Idle;
    requestId_.clear();
    setBusy(service_.busy());
    emit activityChanged();
}

void DataPane::finishPage(const QString &id, const navidog::DataPageResult &result) {
    if (operation_ != Operation::Load || requestId_ != id)
        return;
    finishOperation();
    emit statusChanged(applyPageResult(result));
    if (!result.error.isEmpty() && !result.cancelled)
        emit errorOccurred("数据加载失败", result.error);
    emit operationFinished();
}

void DataPane::finishCommit(const QString &id, const navidog::DataCommitResult &result) {
    if (operation_ != Operation::Commit || requestId_ != id)
        return;
    finishOperation();
    const auto detail = applyCommitResult(result);
    if (!result.error.isEmpty() || result.cancelled) {
        emit statusChanged(result.cancelled ? "数据提交已停止" : "数据提交失败");
        if (!result.cancelled)
            emit errorOccurred("数据提交失败", detail);
    } else {
        emit statusChanged(detail);
        if (loadPage())
            return;
    }
    emit operationFinished();
}

void DataPane::finishCellEditing() {
    if (auto *focused = QApplication::focusWidget(); focused && grid->isAncestorOf(focused))
        grid->setFocus(Qt::OtherFocusReason);
}

void DataPane::cancelLoad() {
    if (operation_ != Operation::Load)
        return;
    service_.cancel();
    finishOperation();
}

QString DataPane::applyPageResult(const navidog::DataPageResult &result) {
    if (result.cancelled || !result.error.isEmpty()) {
        const auto status = result.cancelled ? QString("数据加载已停止") : QString("数据加载失败");
        summary->setText(status);
        return status;
    }
    acceptPage(result);
    return result.editable ? QString("数据已加载，可双击单元格编辑")
                           : "数据已加载 · " + result.readOnlyReason;
}

QString DataPane::applyCommitResult(const navidog::DataCommitResult &result) {
    if (result.cancelled || !result.error.isEmpty()) {
        blocked |= result.commitUnknown;
        updateActions();
        auto detail = result.error;
        if (result.rolledBack)
            detail += "\n本批修改已全部回滚，当前页草稿仍保留。";
        if (result.commitUnknown)
            detail += "\n提交结果无法确认，请重新加载核对，避免重复提交。";
        transaction->setText(detail);
        return detail;
    }

    model->discard();
    blocked = true;
    updateActions();
    prepareFirstPage();
    return QString("已提交：新增 %1、修改 %2、删除 %3")
        .arg(result.inserted).arg(result.updated).arg(result.deleted);
}

void DataPane::preventEditingAfterSchemaChange() {
    blocked = true;
    model->editable = false;
    updateActions();
    summary->setText("表结构已更新，请刷新数据后再编辑。");
}

void DataPane::preventEditingAfterConnectionChange() {
    blocked = true;
    model->editable = false;
    updateActions();
    transaction->setText("连接配置已修改，请刷新数据后再编辑。");
}

bool DataPane::matchesTable(const QString &id, const navidog::CatalogNode &table,
                            bool wholeSchema) const {
    return profileId == id && node.database == table.database &&
           node.schema == table.schema &&
           (wholeSchema || node.object == table.object);
}

QString DataPane::tabTitle() const {
    return node.object + " · 数据" + (dirty() ? " *" : "");
}

QJsonObject DataPane::viewSettings() const {
    const auto &request = page.request;
    return {{"filterColumn", request.filterColumn},
            {"filter", int(request.filter)},
            {"value", QJsonValue::fromVariant(request.filterValue)},
            {"sortColumn", request.sortColumn},
            {"descending", request.descending},
            {"pageSize", request.pageSize}};
}

void DataPane::openSavedView(const QJsonObject &view) {
    auto request = page.request;
    request.offset = 0;
    request.afterKey.clear();
    request.filterColumn = view["filterColumn"].toInt(-1);
    request.filter = static_cast<navidog::DataFilter>(view["filter"].toInt());
    request.filterValue = view["value"].toVariant();
    request.sortColumn = view["sortColumn"].toInt(-1);
    request.descending = view["descending"].toBool();
    request.pageSize = qBound(10, view["pageSize"].toInt(100), 1000);
    if (!busy_ && confirmDiscard())
        queue(request, {});
}

void DataPane::exportData() {
    const auto result = cachedRows();
    if (result.columns.isEmpty()) {
        emit statusChanged("请先加载数据再导出");
        return;
    }
    try {
        const auto path = navidog::ui::exportCachedResult(
            this, result, exportContext_, "表数据-" + node.object, driver_, node.object);
        if (!path.isEmpty())
            emit statusChanged("当前缓存结果已导出：" + path);
    } catch (const std::exception &error) {
        emit errorOccurred("导出失败", QString::fromUtf8(error.what()));
    }
}

bool DataPane::confirmDiscard(){if(!dirty())return true;if(QMessageBox::question(this,"未提交修改","丢弃当前页尚未提交的数据修改？",QMessageBox::Discard|QMessageBox::Cancel,QMessageBox::Cancel)!=QMessageBox::Discard)return false;model->discard();return true;}
navidog::DataPageRequest DataPane::controlsRequest()const{auto request=page.request;request.node=node;request.pageSize=pageSize->currentData().toInt();request.offset=0;request.afterKey.clear();request.filterColumn=filterColumn->currentData().toInt();request.filter=navidog::DataFilter(filterOp->currentData().toInt());request.filterValue=filterValue->text();request.sortColumn=sortColumn->currentData().toInt();request.descending=direction->currentIndex()==1;return request;}
void DataPane::queue(const navidog::DataPageRequest &request,
                     QVector<navidog::DataPageRequest> history) {
    if (busy_)
        return;
    pending = request;
    pendingHistory_ = std::move(history);
    loadPage();
}

void DataPane::requestRefresh() {
    if (!busy_ && confirmDiscard())
        queue(page.request, history_);
}

void DataPane::prepareFirstPage(){auto request=page.request;request.offset=0;request.afterKey.clear();pending=request;pendingHistory_.clear();}
void DataPane::invalidateTable(const QString &replacementName) {
    if (!replacementName.isEmpty())
        node.name = node.object = replacementName;
    page.request.node=node;page.schema.node=node;model->schema.node=node;
    page.request.offset=0;page.request.afterKey.clear();pending=page.request;history_.clear();pendingHistory_.clear();
    blocked=true;loaded=false;model->editable=false;updateActions();transaction->setText("表已变化，请刷新后继续编辑。");
    emit tabTitleChanged(tabTitle());
}
void DataPane::acceptPage(const navidog::DataPageResult& result){
    const navidog::OperationTimer timing(navidog::TimedOperation::ApplyTableData);
    page=result;loaded=true;blocked=false;history_=pendingHistory_;QSignalBlocker filterBlock(filterColumn),sortBlock(sortColumn),opBlock(filterOp),sizeBlock(pageSize);filterColumn->clear();filterColumn->addItem("筛选字段",-1);sortColumn->clear();sortColumn->addItem("按主键排序",-1);for(int i=0;i<result.schema.columns.size();++i){auto c=result.schema.columns[i];filterColumn->addItem(c.name,i);sortColumn->addItem(c.name,i);}filterColumn->setCurrentIndex(filterColumn->findData(result.request.filterColumn));filterOp->setCurrentIndex(filterOp->findData(int(result.request.filter)));filterValue->setText(result.request.filterValue.toString());sortColumn->setCurrentIndex(sortColumn->findData(result.request.sortColumn));direction->setCurrentIndex(result.request.descending?1:0);pageSize->setCurrentIndex(pageSize->findData(result.request.pageSize));model->setPage(result);summary->setText(QString("第 %1–%2 行%3").arg(result.result.rows.isEmpty()?0:result.request.offset+1).arg(result.request.offset+result.result.rows.size()).arg(result.hasMore?"，还有更多":""));updateActions();}
void DataPane::showCell(){auto index=grid->currentIndex();if(!index.isValid())return;auto value=model->rows[index.row()].values[index.column()];navidog::ui::Dialog dialog(this);dialog.setWindowTitle(model->schema.columns[index.column()].name);dialog.resize(760,530);QVBoxLayout layout(&dialog);QPlainTextEdit text; text.setReadOnly(true);QString content;if(nullValue(value))content="NULL";else if(value.userType()==QMetaType::QByteArray)content=QString::fromLatin1(value.toByteArray().toHex(' '));else{content=value.toString();QJsonParseError error;auto json=QJsonDocument::fromJson(content.toUtf8(),&error);if(error.error==QJsonParseError::NoError)content=QString::fromUtf8(json.toJson(QJsonDocument::Indented));}text.setPlainText(content);layout.addWidget(&text);navidog::ui::PushButton close("关闭");layout.addWidget(&close);connect(&close,&QPushButton::clicked,&dialog,&QDialog::accept);dialog.exec();}
QJsonObject DataPane::sessionState()const{return {{"kind","data"},{"profileId",profileId},{"database",database},{"schema",node.schema},{"object",node.object},{"nodeKind",node.kind},{"pageSize",page.request.pageSize},{"filterColumn",page.request.filterColumn},{"filterOp",int(page.request.filter)},{"filterValue",page.request.filterValue.toString()},{"sortColumn",page.request.sortColumn},{"descending",page.request.descending}};}
void DataPane::restoreState(const QJsonObject& state){page.request.pageSize=state["pageSize"].toInt(100);page.request.filterColumn=state["filterColumn"].toInt(-1);page.request.filter=navidog::DataFilter(state["filterOp"].toInt());page.request.filterValue=state["filterValue"].toString();page.request.sortColumn=state["sortColumn"].toInt(-1);page.request.descending=state["descending"].toBool();pending=page.request;summary->setText("点击刷新恢复此数据页");}
