#include "sql_ast_view.hpp"
#include "components.hpp"
#include <QAccessible>
#include <QApplication>
#include <QClipboard>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QItemSelectionModel>
#include <QJsonArray>
#include <QJsonDocument>
#include <QLabel>
#include <QStandardItemModel>
#include <QTreeView>
#include <QVBoxLayout>
namespace {
QString label(const QString &key) {
  static const QHash<QString, QString> labels = {
      {"queries", "语句 / 子查询池"},
      {"roots", "顶层语句 ID"},
      {"expressions", "表达式池"},
      {"types", "规范类型池（按索引引用）"},
      {"sqlType", "类型 ID"},
      {"castType", "CAST 结构化类型"},
      {"literalType", "字面量结构化类型"},
      {"intervalLeading", "区间起始单位"}, {"intervalTrailing", "区间结束单位"}, {"intervalUnit", "区间单位"}, {"intervalLeadingPrecision", "区间起始精度"},
      {"intervalType", "区间类型限定"}, {"interval", "区间限定"}, {"leading", "起始单位"}, {"trailing", "结束单位"}, {"mysqlUnit", "MySQL 区间单位"},
      {"leadingPrecision", "起始精度"}, {"fractionalPrecision", "秒小数精度"}, {"prefixPrecision", "前置精度"},
      {"nullability", "可空性"},
      {"precision", "精度"},
      {"scale", "标度"},
      {"bits", "位宽"},
      {"lengthUnit", "长度单位"},
      {"tables", "表来源"},
      {"relations", "JOIN 关系树"},
      {"target", "写入目标"},
      {"assignments", "赋值"},
      {"rows", "VALUES 行"},
      {"returning", "RETURNING / OUTPUT"},
      {"projections", "投影"},
      {"ctes", "WITH / CTE"},
      {"scope", "作用域"},
      {"scopes", "程序作用域树"},
      {"symbols", "参数 / 局部变量符号"},
      {"code", "绑定后的表达式 / SQL 模型"},
      {"nodes", "程序节点"},
      {"mergeClauses", "MERGE 分支"},
      {"semanticEnvironment", "语义环境"},
      {"columns", "字段"},
      {"source", "来源查询 ID"},
      {"query", "子查询 ID"},
      {"from", "FROM 关系 ID"},
      {"where", "WHERE 表达式 ID"},
      {"on", "ON 表达式 ID"},
      {"using", "USING 字段"},
      {"conflict", "冲突处理"}};
  return labels.value(key, key);
}
QString preview(const QJsonValue &value) {
  if (value.isObject())
    return QString("{%1 项}").arg(value.toObject().size());
  if (value.isArray())
    return QString("[%1 项]").arg(value.toArray().size());
  if (value.isString())
    return value.toString().left(300);
  if (value.isNull())
    return "null";
  if (value.isBool())
    return value.toBool() ? "true" : "false";
  return QString::number(value.toDouble(), 'g', 16);
}
} // namespace
SqlAstView::SqlAstView(QWidget *parent) : QWidget(parent) {
  setObjectName("queryAstPage");
  auto *layout = new QVBoxLayout(this);
  layout->setContentsMargins(0, 0, 0, 0);
  layout->setSpacing(0);
  auto *bar = new QHBoxLayout;
  bar->setContentsMargins(8, 3, 8, 3);
  status_ = new QLabel("点击“解析 AST”，无需执行 SQL。");
  bar->addWidget(status_, 1);
  auto *copy = new navidog::ui::Button("复制 JSON", "copyUnifiedAst");
  bar->addWidget(copy);
  auto *collapse = new navidog::ui::Button("收起", "collapseUnifiedAst");
  bar->addWidget(collapse);
  layout->addLayout(bar);
  tree_ = new QTreeView;
  tree_->setObjectName("queryAstTree");
  tree_->setAccessibleName("统一 AST 解析结果");
  tree_->setUniformRowHeights(true);
  tree_->setAlternatingRowColors(true);
  tree_->setEditTriggers(QAbstractItemView::NoEditTriggers);
  model_ = new QStandardItemModel(this);
  model_->setHorizontalHeaderLabels({"节点 / 属性", "值"});
  tree_->setModel(model_);
  tree_->setColumnWidth(0, 320);
  tree_->header()->setStretchLastSection(true);
  layout->addWidget(tree_);
  connect(copy, &QToolButton::clicked, this, [this] {
    if (!json_.isEmpty())
      QApplication::clipboard()->setText(QString::fromUtf8(json_));
  });
  connect(collapse, &QToolButton::clicked, tree_, &QTreeView::collapseAll);
  connect(tree_, &QTreeView::doubleClicked, this,
          [this](const QModelIndex &index) {
            const auto range =
                index.sibling(index.row(), 0).data(Qt::UserRole).toList();
            if (range.size() == 2 && sourceActivated)
              sourceActivated(range[0].toInt(), range[1].toInt(), sourceHash_);
          });
}
void SqlAstView::prepareForClose() {
  tree_->clearSelection();
  tree_->setCurrentIndex({});
  tree_->clearFocus();
}
SqlAstView::~SqlAstView() {
  // Detach while both the selection model and data model are still alive. On
  // macOS the accessibility bridge may retain a tree selection during tab
  // close.
  prepareForClose();
  tree_->setModel(nullptr);
  delete tree_;
  tree_ = nullptr;
}
void SqlAstView::reset() {
  prepareForClose();
  model_->removeRows(0, model_->rowCount());
  json_.clear();
  sourceHash_.clear();
  status_->setText("正在后台解析…");
}
void SqlAstView::setReport(const navidog::SqlExecutionAnalysis &report) {
  reset();
  sourceHash_ = report.sourceHash;
  QJsonArray exported;
  int nodes = 0;
  bool truncated = false;
  struct Work {
    QStandardItem *parent;
    QString key;
    QJsonValue value;
    int offset;
  };
  for (const auto &statement : report.statements)
    exported.append(QJsonObject{{"index", statement.index},
                                {"sourceBegin", statement.sourceBegin},
                                {"kind", statement.kind},
                                {"state", int(statement.state)},
                                {"detail", statement.detail},
                                {"astTruncated", statement.astTruncated},
                                {"unifiedAst", statement.unifiedAst}});
  for (const auto &statement : report.statements) {
    auto *title = new QStandardItem(
        QString("第 %1 条 · %2")
            .arg(statement.index + 1)
            .arg(statement.kind.isEmpty() ? "SQL" : statement.kind));
    auto *state = new QStandardItem(
        statement.unifiedAst.isEmpty()
            ? statement.astTruncated       ? "AST 超出展示大小上限"
              : statement.detail.isEmpty() ? "未能解析 / 超限 / 停止"
                                           : statement.detail
            : "统一语句模型 · 双击有 origin 的节点定位源码");
    model_->appendRow({title, state});
    ++nodes;

    std::vector<Work> work{
        {title, "统一 AST", statement.unifiedAst, statement.sourceBegin}};
    while (!work.empty()) {
      if (nodes >= 2500) {
        truncated = true;
        break;
      }
      auto entry = std::move(work.back());
      work.pop_back();
      auto *key = new QStandardItem(label(entry.key));
      auto *value = new QStandardItem(preview(entry.value));
      entry.parent->appendRow({key, value});
      ++nodes;
      if (entry.value.isObject()) {
        const auto object = entry.value.toObject();
        const auto range = object.value("origin").toArray();
        if (range.size() == 2)
          key->setData(QVariantList{entry.offset + range[0].toInt(),
                                    entry.offset + range[1].toInt()},
                       Qt::UserRole);
        const auto keys = object.keys();
        for (auto it = keys.crbegin(); it != keys.crend(); ++it)
          work.push_back({key, *it, object.value(*it), entry.offset});
      } else if (entry.value.isArray()) {
        const auto array = entry.value.toArray();
        for (int i = array.size() - 1; i >= 0; --i)
          work.push_back({key, QString("[%1]").arg(i), array[i], entry.offset});
      }
    }
    if (truncated)
      break;
  }
  json_ = QJsonDocument(exported).toJson();
  tree_->expandToDepth(1);
  status_->setText(QString("统一 AST · %1 条%2%3")
                       .arg(report.statements.size())
                       .arg(truncated ? " · 树已截断至 2500 节点" : "")
                       .arg(report.truncated ? " · 部分语句未分析" : ""));
}
