#include "objects_pane.hpp"
#include "components.hpp"
#include <QApplication>
#include <QClipboard>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QShortcut>
#include <QSortFilterProxyModel>
#include <QSplitter>
#include <QStandardItemModel>
#include <QTableView>
#include <QVBoxLayout>
namespace {
QString kindTitle(const QString &kind) {
  if (kind == "mongoCollection")
    return "集合";
  if (kind == "redisKey")
    return "Redis 键";
  if (kind == "table")
    return "表";
  if (kind == "materializedView")
    return "物化视图";
  if (kind == "view")
    return "视图";
  if (kind == "function")
    return "函数";
  if (kind == "procedure")
    return "存储过程";
  if (kind == "tableGroup")
    return "表分组";
  if (kind == "database")
    return "数据库";
  if (kind == "schema")
    return "Schema";
  return kind;
}
} // namespace
ObjectsPane::ObjectsPane(QWidget *parent) : WorkspacePane(parent) {
  setObjectName("objectsBrowser");
  auto *layout = new QVBoxLayout(this);
  layout->setContentsMargins(0, 0, 0, 0);
  layout->setSpacing(0);
  auto *toolbar = new QWidget;
  toolbar->setObjectName("objectsToolbar");
  auto *tools = new QHBoxLayout(toolbar);
  tools->setContentsMargins(6, 3, 6, 3);
  tools->setSpacing(2);
  auto control = [&](QString label, QString name) {
    auto *value = new navidog::ui::Button(label, name);
    tools->addWidget(value);
    return value;
  };
  open_ = control("打开", "openBrowserObject");
  design_ = control("设计", "designBrowserObject");
  create_ = control("新建对象", "newBrowserObject");
  remove_ = control("删除", "deleteBrowserObject");
  refresh_ = control("刷新", "refreshBrowserObjects");
  tools->addStretch();
  auto *filter = new navidog::ui::TextInput;
  filter->setObjectName("objectListFilter");
  filter->setPlaceholderText("筛选对象名称…");
  filter->setClearButtonEnabled(true);
  filter->setMaximumWidth(240);
  tools->addWidget(filter);
  layout->addWidget(toolbar);
  context_ = new QLabel;
  context_->setTextFormat(Qt::PlainText);
  context_->setObjectName("objectBrowserContext");
  context_->setContentsMargins(12, 7, 12, 7);
  layout->addWidget(context_);
  auto *split = new QSplitter;
  split->setChildrenCollapsible(false);
  list_ = new navidog::ui::ObjectGrid;
  list_->setObjectName("objectList");
  model_ = new QStandardItemModel(this);
  model_->setHorizontalHeaderLabels(
      {"名称", "类型", "Schema / 数据库", "说明 / 参数"});
  proxy_ = new QSortFilterProxyModel(this);
  proxy_->setSourceModel(model_);
  proxy_->setFilterKeyColumn(0);
  proxy_->setFilterCaseSensitivity(Qt::CaseInsensitive);
  list_->setModel(proxy_);
  list_->setSelectionBehavior(QAbstractItemView::SelectItems);
  list_->setSelectionMode(QAbstractItemView::SingleSelection);
  list_->setEditTriggers(QAbstractItemView::NoEditTriggers);
  list_->setAlternatingRowColors(true);
  list_->setSortingEnabled(true);
  list_->sortByColumn(0, Qt::AscendingOrder);
  list_->verticalHeader()->hide();
  list_->verticalHeader()->setDefaultSectionSize(24);
  list_->horizontalHeader()->setDefaultAlignment(Qt::AlignLeft |
                                                 Qt::AlignVCenter);
  list_->horizontalHeader()->setSectionResizeMode(QHeaderView::Interactive);
  list_->horizontalHeader()->setStretchLastSection(true);
  list_->setColumnWidth(0, 260);
  list_->setColumnWidth(1, 100);
  list_->setColumnWidth(2, 160);
  split->addWidget(list_);
  details_ = new QLabel("选择对象查看信息");
  details_->setTextFormat(Qt::PlainText);
  details_->setWordWrap(true);
  details_->setTextInteractionFlags(Qt::TextSelectableByMouse);
  details_->setAlignment(Qt::AlignLeft | Qt::AlignTop);
  details_->setContentsMargins(16, 16, 16, 16);
  details_->setMinimumWidth(160);
  details_->setObjectName("objectBrowserDetails");
  split->addWidget(details_);
  split->setSizes({850, 210});
  layout->addWidget(split, 1);
  count_ = new QLabel("请选择导航树中的表、视图、函数或存储过程目录");
  count_->setObjectName("objectBrowserCount");
  count_->setContentsMargins(12, 6, 12, 6);
  layout->addWidget(count_);
  connect(filter, &QLineEdit::textChanged, proxy_,
          &QSortFilterProxyModel::setFilterFixedString);
  connect(proxy_, &QAbstractItemModel::rowsRemoved, this, [this] {
    updateSelection();
    updateCount();
  });
  connect(proxy_, &QAbstractItemModel::rowsInserted, this, [this] {
    updateSelection();
    updateCount();
  });
  connect(list_->selectionModel(), &QItemSelectionModel::currentChanged, this,
          [this] { updateSelection(); });
  connect(list_, &QTableView::doubleClicked, this,
          [this](const QModelIndex &index) {
            list_->afterDoubleClick(
                index, [this](const QModelIndex &activated) {
                  const auto node = activated.siblingAtColumn(0)
                                        .data(Qt::UserRole)
                                        .value<navidog::CatalogNode>();
                  if (open_->isEnabled() && !node.object.isEmpty())
                    emit actionRequested("open", node);
                });
          });
  for (auto pair : {qMakePair(open_, QString("open")),
                    qMakePair(design_, QString("design")),
                    qMakePair(remove_, QString("delete"))})
    connect(pair.first, &QToolButton::clicked, this,
            [this, action = pair.second] {
              emit actionRequested(action, selectedNode());
            });
  connect(create_, &QToolButton::clicked, this,
          [this] { emit actionRequested("create", folder); });
  connect(refresh_, &QToolButton::clicked, this,
          [this] { emit actionRequested("refresh", folder); });
  list_->setContextMenuPolicy(Qt::CustomContextMenu);
  connect(list_, &QWidget::customContextMenuRequested, this,
          [this](QPoint position) {
            auto index = list_->indexAt(position);
            if (index.isValid())
              list_->setCurrentIndex(index);
            emit contextRequested(index.isValid() ? selectedNode() : folder,
                                  list_->viewport()->mapToGlobal(position));
          });
  auto *copy = new QShortcut(QKeySequence::Copy, list_);
  connect(copy, &QShortcut::activated, this, [this] {
    auto n = selectedNode();
    if (!n.object.isEmpty())
      QApplication::clipboard()->setText(n.object);
  });
  auto *enter = new QShortcut(QKeySequence(Qt::Key_Return), list_);
  connect(enter, &QShortcut::activated, this, [this] {
    if (open_->isEnabled())
      emit actionRequested("open", selectedNode());
  });
  updateSelection();
}
void ObjectsPane::setContext(const navidog::ConnectionProfile &p,
                             const navidog::CatalogNode &node) {
  const bool changed = profileId != p.id || folder.kind != node.kind ||
                       folder.database != node.database ||
                       folder.schema != node.schema || folder.key != node.key;
  profileId = p.id;
  database = node.database;
  folder = node;
  definitionOnly_ = p.driver == navidog::DatabaseKind::Dameng || navidog::usesExtendedCatalog(p.driver);
  writable_ = !p.readOnly && !definitionOnly_ && !navidog::usesExtendedCatalog(p.driver) && !navidog::isCommandDatabase(p.driver);
  design_->setText(definitionOnly_ ? "定义" : "设计");
  if (changed)
    findChild<QLineEdit *>("objectListFilter")->clear();
  context_->setText(
      p.name + (!node.database.isEmpty() ? " / " + node.database : QString{}) +
      (!node.schema.isEmpty() && node.schema != node.database
           ? " / " + node.schema
           : QString{}) +
      " / " + (node.name.isEmpty() ? node.kind : node.name));
  create_->setText(node.kind == "tables" || node.kind == "tableGroup" ? "新建表"
                   : node.kind == "views"     ? "新建视图"
                   : node.kind == "materializedViews" ? "新建物化视图"
                   : node.kind == "functions" ? "新建函数"
                                              : "新建存储过程");
  updateSelection();
}
void ObjectsPane::setNodes(const QVector<navidog::CatalogNode> &nodes,
                           bool loaded, bool truncated) {
  loaded_ = loaded;
  truncated_ = truncated;
  total_ = nodes.size();
  const auto previous = selectedNode();
  model_->removeRows(0, model_->rowCount());
  int selected = -1;
  for (auto node : nodes) {
    auto *name =
        new QStandardItem(navidog::ui::objectIcon(node.kind), node.name);
    name->setData(QVariant::fromValue(node), Qt::UserRole);
    model_->appendRow(
        {name, new QStandardItem(kindTitle(node.kind)),
         new QStandardItem(node.schema.isEmpty() ? node.database : node.schema),
         new QStandardItem(node.detail)});
    if (node.kind == previous.kind && node.object == previous.object &&
        node.key == previous.key)
      selected = model_->rowCount() - 1;
  }
  if (selected >= 0)
    list_->setCurrentIndex(proxy_->mapFromSource(model_->index(selected, 0)));
  updateCount();
  updateSelection();
}
navidog::CatalogNode ObjectsPane::selectedNode() const {
  auto index = list_->currentIndex();
  if (!index.isValid())
    return {};
  auto source = proxy_->mapToSource(index);
  return model_->index(source.row(), 0)
      .data(Qt::UserRole)
      .value<navidog::CatalogNode>();
}
void ObjectsPane::setBusy(bool value) {
  busy_ = value;
  updateSelection();
}
void ObjectsPane::updateSelection() {
  const auto node = selectedNode();
  const bool selected =
      QStringList{"table", "view", "materializedView", "function", "procedure", "mongoCollection", "redisKey"}.contains(node.kind);
  open_->setEnabled(!busy_ && selected);
  design_->setText(node.kind == "materializedView" ? "属性" : definitionOnly_ ? "定义" : "设计");
  design_->setEnabled(!busy_ && selected && (writable_ || definitionOnly_ || node.kind == "materializedView"));
  remove_->setEnabled(!busy_ && writable_ && selected);
  create_->setEnabled(!busy_ && writable_ && !profileId.isEmpty());
  refresh_->setEnabled(!busy_ && !profileId.isEmpty());
  details_->setText(
      !selected
          ? "选择对象查看信息"
          : node.name + "\n\n类型\n" + kindTitle(node.kind) +
                "\n\nSchema / 数据库\n" +
                (node.schema.isEmpty() ? node.database : node.schema) +
                (node.detail.isEmpty() ? QString{}
                                       : "\n\n说明 / 参数\n" + node.detail));
}
QJsonObject ObjectsPane::sessionState() const {
  return {{"kind", "objects"},         {"profileId", profileId},
          {"database", database},      {"folderKind", folder.kind},
          {"folderName", folder.name}, {"schema", folder.schema},
          {"key", folder.key}};
}

void ObjectsPane::updateCount() {
  count_->setText(!loaded_
                      ? "目录尚未加载；点击刷新读取对象"
                      : QString("显示 %1 / %2 个对象%3")
                            .arg(proxy_->rowCount())
                            .arg(total_)
                            .arg(truncated_ ? " · 目录已达到显示上限" : ""));
}
