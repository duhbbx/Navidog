#include "estimated_plan_view.hpp"
#include "editor.hpp"
#include "components.hpp"
#include "result_grid.hpp"
#include <QAbstractTableModel>
#include <QApplication>
#include <QButtonGroup>
#include <QClipboard>
#include <QFontDatabase>
#include <QHeaderView>
#include <QItemSelectionModel>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QPlainTextEdit>
#include <QSaveFile>
#include <QSet>
#include <QSortFilterProxyModel>
#include <QSplitter>
#include <QStackedWidget>
#include <QStandardItemModel>
#include <QVBoxLayout>
namespace {
constexpr int nodeRole=Qt::UserRole+3;
QString number(const std::optional<double> &value) {
  return value?QString::number(*value,'g',10):QString::fromUtf8("—");
}
QString serverId(const navidog::EstimatedPlanNode &n) {
  for(auto it=n.properties.begin();it!=n.properties.end();++it)
    if(it.key().compare("NodeId",Qt::CaseInsensitive)==0 || it.key().compare("ID",Qt::CaseInsensitive)==0)return it.value();
  return {};
}
QString searchable(const navidog::EstimatedPlanNode &n) {
  auto text=n.operation+"\n"+n.object+"\n"+n.index+"\n"+n.alias;
  for(auto it=n.properties.begin();it!=n.properties.end() && text.size()<32768;++it)text+="\n"+it.key()+":"+it.value();
  return text.left(32768);
}
QString preview(const QByteArray &bytes) {
  auto text=QString::fromUtf8(bytes.left(256*1024));
  if(bytes.size()>256*1024)text+="\n\n……界面只预览前 256 KiB，导出原始文档可保留完整内容。";
  return text;
}
} // namespace
// One virtual row per visible operator; no per-cell widgets or copies of plans.
class FlatPlanModel final:public QAbstractTableModel {
  const navidog::EstimatedPlan *plan_;
  QVector<int> ids_,parents_;
  QVector<QString> search_;
public:
  FlatPlanModel(const navidog::EstimatedPlan *plan,QObject *owner):QAbstractTableModel(owner),plan_(plan){}
  void reset(QVector<int> ids={},QVector<int> parents={},QVector<QString> search={}) {
    beginResetModel();ids_=std::move(ids);parents_=std::move(parents);search_=std::move(search);endResetModel();
  }
  int rowCount(const QModelIndex &parent={})const override{return parent.isValid()?0:ids_.size();}
  int columnCount(const QModelIndex &parent={})const override{return parent.isValid()?0:12;}
  QVariant headerData(int section,Qt::Orientation orientation,int role)const override {
    if(role!=Qt::DisplayRole && role!=Qt::UserRole)return {};
    static const QStringList labels={"算子","对象","估计行数","服务端 COST","估计时间（μs）","行宽（字节）","索引","别名","启动成本","节点 ID","父节点 ID","服务端 ID"};
    return orientation==Qt::Horizontal && section>=0 && section<labels.size()?QVariant(labels[section]):QVariant(section+1);
  }
  QVariant data(const QModelIndex &index,int role)const override {
    if(!index.isValid() || index.row()<0 || index.row()>=ids_.size() || ids_[index.row()]>=plan_->nodes.size())return {};
    const auto id=ids_[index.row()];const auto &n=plan_->nodes[id];
    if(role==nodeRole)return id;
    if(role==Qt::UserRole+1 || role==Qt::ToolTipRole)return search_.value(index.row());
    if(role==Qt::TextAlignmentRole && (index.column()==2 || index.column()==3 || index.column()==4 || index.column()==5 || index.column()>=8))return int(Qt::AlignRight|Qt::AlignVCenter);
    if(role!=Qt::DisplayRole && role!=Qt::UserRole && role!=Qt::UserRole+2)return {};
    const bool display=role==Qt::DisplayRole;
    auto numeric=[&](std::optional<double> value)->QVariant{return display?QVariant(number(value)):value?QVariant(*value):QVariant{};};
    switch(index.column()) {
    case 0:return n.operation;
    case 1:return n.object;
    case 2:return numeric(n.rows);
    case 3:return numeric(n.totalCost);
    case 4:return numeric(n.estimatedTimeUs);
    case 5:return numeric(n.rowBytes);
    case 6:return n.index;
    case 7:return n.alias;
    case 8:return numeric(n.startupCost);
    case 9:return id;
    case 10:return parents_.value(index.row(),-1)<0?QVariant{}:QVariant(parents_[index.row()]);
    case 11:return serverId(n);
    }
    return {};
  }
};
EstimatedPlanView::EstimatedPlanView(QWidget *parent):QWidget(parent) {
  setObjectName("estimatedPlanView");
  auto *layout=new QVBoxLayout(this);layout->setContentsMargins(0,0,0,0);
  auto *bar=new QHBoxLayout;
  status_=new QLabel;status_->setWordWrap(true);status_->setTextFormat(Qt::PlainText);status_->setObjectName("estimatedPlanStatus");bar->addWidget(status_,1);
  copy_=new navidog::ui::Button("复制计划 JSON","copyEstimatedPlan");
  save_=new navidog::ui::Button("导出计划…","exportEstimatedPlan");
  raw_=new navidog::ui::Button("导出原始文档…","exportRawEstimatedPlan");
  bar->addWidget(copy_);bar->addWidget(save_);bar->addWidget(raw_);layout->addLayout(bar);
  source_=new QLabel;source_->setWordWrap(true);source_->setTextFormat(Qt::PlainText);source_->setObjectName("estimatedPlanSource");source_->setTextInteractionFlags(Qt::TextSelectableByMouse);layout->addWidget(source_);
  auto *tools=new QHBoxLayout;auto *modes=new QButtonGroup(this);modes->setExclusive(true);
  const QStringList names={"树形","表形","原文","计划请求"},ids={"planTreeMode","planTableMode","planRawMode","planRequestMode"};
  for(int i=0;i<names.size();++i) {
    auto *button=new navidog::ui::Button(names[i],ids[i]);button->setCheckable(true);button->setChecked(i==0);modes->addButton(button,i);tools->addWidget(button);
    connect(button,&QToolButton::clicked,this,[this,i]{
      pages_->setCurrentIndex(i);details_->setVisible(i<2 && detailsToggle_->isChecked());detailsToggle_->setEnabled(i<2);search_->setEnabled(i<2);expand_->setEnabled(i==0);collapse_->setEnabled(i==0);
      if(i<2)showNode(selected_);
    });
  }
  search_=new navidog::ui::TextInput;search_->setObjectName("estimatedPlanSearch");search_->setAccessibleName("搜索执行计划");search_->setPlaceholderText("搜索算子、对象、索引或条件…");search_->setClearButtonEnabled(true);tools->addWidget(search_,1);
  expand_=new navidog::ui::Button("展开","expandEstimatedPlan");collapse_=new navidog::ui::Button("收起","collapseEstimatedPlan");tools->addWidget(expand_);tools->addWidget(collapse_);
  detailsToggle_=new navidog::ui::Button("详情","estimatedPlanToggleDetails");detailsToggle_->setCheckable(true);detailsToggle_->setToolTip("显示所选算子的详细属性");tools->addWidget(detailsToggle_);
  connect(detailsToggle_,&QToolButton::toggled,this,[this](bool show){details_->setVisible(show && pages_->currentIndex()<2);});
  layout->addLayout(tools);
  split_=new QSplitter;pages_=new QStackedWidget;pages_->setObjectName("estimatedPlanModes");
  tree_=new QTreeView;tree_->setObjectName("estimatedPlanTree");tree_->setUniformRowHeights(true);tree_->setAlternatingRowColors(true);tree_->setEditTriggers(QAbstractItemView::NoEditTriggers);
  model_=new QStandardItemModel(tree_);model_->setHorizontalHeaderLabels({"算子","对象 / 索引","估计行数","服务端 COST","行宽（字节）","估计时间（μs）"});
  proxy_=new QSortFilterProxyModel(tree_);proxy_->setSourceModel(model_);proxy_->setRecursiveFilteringEnabled(true);proxy_->setFilterCaseSensitivity(Qt::CaseInsensitive);proxy_->setFilterKeyColumn(0);proxy_->setFilterRole(Qt::UserRole+1);proxy_->setSortRole(Qt::UserRole+2);
  tree_->setModel(proxy_);tree_->setSortingEnabled(true);tree_->sortByColumn(-1,Qt::AscendingOrder);tree_->header()->resizeSection(0,230);tree_->header()->resizeSection(1,180);pages_->addWidget(tree_);
  table_=new navidog::ui::ObjectGrid;table_->setObjectName("estimatedPlanTable");flat_=new FlatPlanModel(&plan_,table_);
  flatProxy_=new QSortFilterProxyModel(table_);flatProxy_->setSourceModel(flat_);flatProxy_->setFilterCaseSensitivity(Qt::CaseInsensitive);flatProxy_->setFilterKeyColumn(0);flatProxy_->setFilterRole(Qt::UserRole+1);flatProxy_->setSortRole(Qt::UserRole+2);
  table_->setModel(flatProxy_);navidog::ui::configureResultGrid(table_);table_->setSortingEnabled(true);table_->sortByColumn(-1,Qt::AscendingOrder);table_->setEditTriggers(QAbstractItemView::NoEditTriggers);
  table_->horizontalHeader()->setMaximumSectionSize(480);
  const int widths[]={220,180,100,110,125,110,160,100,110,85,85,100};
  for(int i=0;i<12;++i)table_->setColumnWidth(i,widths[i]);
  pages_->addWidget(table_);
  auto document=[&](const QString &name){auto *edit=new QPlainTextEdit;edit->setObjectName(name);edit->setReadOnly(true);edit->setLineWrapMode(QPlainTextEdit::NoWrap);edit->setFont(QFontDatabase::systemFont(QFontDatabase::FixedFont));pages_->addWidget(edit);return edit;};
  rawDocument_ = document("estimatedPlanRaw");
  request_ = new SqlReadOnlyView;
  request_->setObjectName("estimatedPlanRequest");
  pages_->addWidget(request_->withActions());
  split_->addWidget(pages_);
  details_=new QPlainTextEdit;details_->setReadOnly(true);details_->setObjectName("estimatedPlanDetails");details_->setPlaceholderText("选择算子查看服务端属性；节点 ID 是本界面编号，服务端 ID 单独保留。");split_->addWidget(details_);split_->setStretchFactor(0,3);split_->setStretchFactor(1,2);details_->hide();layout->addWidget(split_,1);
  connect(search_,&QLineEdit::textChanged,this,[this](const QString &text){proxy_->setFilterFixedString(text);flatProxy_->setFilterFixedString(text);if(!text.isEmpty())tree_->expandAll();});
  connect(expand_,&QToolButton::clicked,tree_,&QTreeView::expandAll);connect(collapse_,&QToolButton::clicked,tree_,&QTreeView::collapseAll);
  connect(tree_->selectionModel(),&QItemSelectionModel::currentChanged,this,[this](const QModelIndex &i){if(pages_->currentIndex()==0)showNode(i.isValid()?i.sibling(i.row(),0).data(Qt::UserRole).toInt():-1);});
  connect(table_->selectionModel(),&QItemSelectionModel::currentChanged,this,[this](const QModelIndex &i){if(pages_->currentIndex()==1)showNode(i.isValid()?i.data(nodeRole).toInt():-1);});
  connect(copy_,&QToolButton::clicked,this,[this]{try{QApplication::clipboard()->setText(QString::fromUtf8(navidog::estimatedPlanJson(plan_)));}catch(const std::exception &e){QMessageBox::warning(this,"复制失败",QString::fromUtf8(e.what()));}});
  auto exportData=[this](bool raw){try{
    auto bytes=raw?plan_.raw:navidog::estimatedPlanJson(plan_);const bool xml=raw && plan_.format.contains("xml");
    auto path=navidog::ui::chooseExportFile(this,"导出数据库估计计划",exportContext_,raw?"原始执行计划":"执行计划",xml?"SQL Server 计划 (*.sqlplan);;XML (*.xml)":"JSON (*.json)");
    if(path.isEmpty())return;QSaveFile file(path);if(!file.open(QIODevice::WriteOnly) || file.write(bytes)!=bytes.size() || !file.commit())throw std::runtime_error(file.errorString().toStdString());
  }catch(const std::exception &e){QMessageBox::warning(this,"导出失败",QString::fromUtf8(e.what()));}};
  connect(save_,&QToolButton::clicked,this,[exportData]{exportData(false);});connect(raw_,&QToolButton::clicked,this,[exportData]{exportData(true);});setPlan({});
}
void EstimatedPlanView::showNode(int id) {
  selected_=id;details_->clear();if(id<0 || id>=plan_.nodes.size())return;
  const auto &n=plan_.nodes[id];auto text=QString("节点 ID：%1\n服务端 ID：%2\n%3\n对象：%4\n索引：%5\n别名：%6\n估计行数：%7\n启动成本：%8\n累计成本：%9\n行宽：%10\n")
    .arg(id).arg(serverId(n)).arg(n.operation,n.object,n.index,n.alias,number(n.rows),number(n.startupCost),number(n.totalCost),number(n.rowBytes));
  if(n.estimatedTimeUs)text+="\n估计时间（μs）："+number(n.estimatedTimeUs);
  for(auto it=n.properties.begin();it!=n.properties.end() && text.size()<262144;++it)text+="\n"+it.key()+"："+it.value();details_->setPlainText(text.left(262144));
}
void EstimatedPlanView::setPlan(const navidog::EstimatedPlan &plan) {
  flat_->reset();model_->removeRows(0,model_->rowCount());plan_=plan;search_->clear();details_->clear();selected_=-1;
  rawDocument_->setPlainText(preview(plan_.raw));copy_->setEnabled(plan.complete);save_->setEnabled(plan.complete);raw_->setEnabled(!plan.raw.isEmpty());
  const auto &r=plan_.request;
  QString source;
  if(r.issued)source=QString("来源：%1数据库计划请求 · 连接：%2 · 数据库：%3%4 · 隔离会话 · %5 ms%6")
    .arg(navidog::databaseKindName(plan.driver),r.connectionName,r.database,r.schema.isEmpty()?QString{}:" · Schema："+r.schema).arg(r.elapsedMs).arg(r.received?" · 已收到响应":" · 未取得完整响应");
  else source=plan.raw.isEmpty()?"尚未取得数据库执行计划":"来源：已解析的计划文档，未附在线请求记录";
  source_->setText(source);
  QString request=source+"\n服务器版本："+r.serverVersion+"\n请求时间（UTC）："+r.requestedAtUtc+"\n\n以下为实际交给驱动的计划请求语句；连接初始化 / Schema 设置不在此列表。原 SQL 未作为普通 DML 执行。\n";
  if(!r.issued)request="没有在线计划请求记录；不能根据导入的文档推断已经连接过数据库。\n";
  request = "-- " + request.replace('\n', "\n-- ");
  bool truncated = false;
  int i = 0;
  for (const auto &command : r.commands) {
    if (request.size() > 256 * 1024) {
      request += "\n-- 界面预览达到上限，完整记录在计划 JSON 导出中。";
      truncated = true;
      break;
    }
    truncated |= command.size() > 256 * 1024;
    request += "\n-- 计划请求 " + QString::number(++i) + "\n" + command.left(256 * 1024) + "\n";
  }
  request_->setDatabaseKind(plan.driver);
  request_->setSourceSql(request.left(512 * 1024));
  request_->setFormattingEnabled(!truncated && request.size() <= 512 * 1024 && !r.commands.isEmpty());
  if(!plan.complete){status_->setText(plan.warning.isEmpty()?"点击“解释”，读取当前数据库的估计执行计划。":plan.warning+"；可切换原文或原始结果查看。");return;}
  struct Work {int id,parentId,depth;QStandardItem *parent;};QVector<Work> work;
  for(auto it=plan.roots.rbegin();it!=plan.roots.rend();++it)work.append({*it,-1,0,model_->invisibleRootItem()});
  QSet<int> seen;QVector<int> ids,parents;QVector<QString> searches;
  while(!work.isEmpty() && seen.size()<1000) {
    const auto w=work.takeLast();if(w.id<0 || w.id>=plan.nodes.size() || w.depth>128 || seen.contains(w.id))continue;seen.insert(w.id);
    const auto &n=plan.nodes[w.id];const auto search=searchable(n);ids.append(w.id);parents.append(w.parentId);searches.append(search);
    QList<QStandardItem*> row;
    for(const auto &text:QStringList{n.operation,n.object+(n.index.isEmpty()?"":" / "+n.index),number(n.rows),number(n.totalCost),number(n.rowBytes),number(n.estimatedTimeUs)}) {
      auto *item=new QStandardItem(text);item->setData(w.id,Qt::UserRole);item->setData(search,Qt::UserRole+1);item->setData(text,Qt::UserRole+2);item->setToolTip(search);row.append(item);
    }
    if(n.rows)row[2]->setData(*n.rows,Qt::UserRole+2);if(n.totalCost)row[3]->setData(*n.totalCost,Qt::UserRole+2);if(n.rowBytes)row[4]->setData(*n.rowBytes,Qt::UserRole+2);if(n.estimatedTimeUs)row[5]->setData(*n.estimatedTimeUs,Qt::UserRole+2);
    w.parent->appendRow(row);for(auto it=n.children.rbegin();it!=n.children.rend();++it)work.append({*it,w.id,w.depth+1,row[0]});
  }
  flat_->reset(std::move(ids),std::move(parents),std::move(searches));
  status_->setText(QString("%1 · %2 个算子 · 数据库估计值，COST 不跨库比较%3").arg(navidog::databaseKindName(plan.driver)).arg(plan.nodes.size()).arg(seen.size()<plan.nodes.size()?" · 界面显示前 1000 个，导出保留完整结果":""));
  tree_->expandToDepth(3);if(model_->rowCount())tree_->setCurrentIndex(proxy_->index(0,0));
}
