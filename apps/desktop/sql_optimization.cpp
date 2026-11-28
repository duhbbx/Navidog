#include "navidog/operation_timing.hpp"
#include "sql_optimization.hpp"
#include "components.hpp"
#include "editor.hpp"
#include "navidog/local_store.hpp"
#include "navidog/sql/compiler/formatter.hpp"
#include "navidog/sql/optimizer/optimizer.hpp"
#include "navidog/sql/optimizer/plan_sql.hpp"
#include <QApplication>
#include <QClipboard>
#include <QDoubleSpinBox>
#include <QFormLayout>
#include <QHeaderView>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLabel>
#include <QListWidget>
#include <QMessageBox>
#include <QRunnable>
#include <QSaveFile>
#include <QSemaphore>
#include <QSignalBlocker>
#include <QSplitter>
#include <QStandardItemModel>
#include <QThreadPool>
#include <QVBoxLayout>
#include <algorithm>
#include <atomic>
#include <cmath>
Q_DECLARE_METATYPE(navidog::sql::optimizer::OptimizationResult)
Q_DECLARE_METATYPE(navidog::sql::optimizer::MemoryStatistics)
namespace {
namespace sc = navidog::sql::compiler;
namespace opt = navidog::sql::optimizer;
QSemaphore optimizationSlots(4);
QThreadPool &pool() {
  static QThreadPool p;
  static const bool initialized = [] {
    p.setMaxThreadCount(2);
    p.setExpiryTimeout(10000);
    return true;
  }();
  Q_UNUSED(initialized);
  return p;
}
QString name(const std::string &s) { return QString::fromStdString(s); }
QJsonObject tableJson(const opt::TableStatistics &t) {
  return {{"database", name(t.database)},
          {"schema", name(t.schema)},
          {"name", name(t.name)},
          {"rows", t.rows},
          {"rowBytes", t.averageRowBytes}};
}
opt::MemoryStatistics readManual(const QString &path, const QString &key) {
  opt::MemoryStatistics stats;
  if (path.isEmpty() || key.isEmpty())
    return stats;
  navidog::LocalStore store(path);
  const auto bytes = store.document(key);
  if (!bytes)
    return stats;
  if (bytes->size() > 256 * 1024)
    throw std::runtime_error("本地统计配置超过 256 KiB");
  const auto document = QJsonDocument::fromJson(*bytes);
  if (!document.isObject())
    throw std::runtime_error("本地统计配置格式不正确");
  const auto root = document.object();
  if (root["version"].toInt() != 1)
    throw std::runtime_error("本地统计配置版本不支持");
  const auto tables = root["tables"].toArray();
  if (tables.size() > 256)
    throw std::runtime_error("本地手工统计超过 256 张表");
  for (const auto &v : tables) {
    const auto o = v.toObject();
    opt::TableStatistics t;
    t.database = o["database"].toString().toStdString();
    t.schema = o["schema"].toString().toStdString();
    t.name = o["name"].toString().toStdString();
    t.rows = o["rows"].toDouble(-1);
    t.averageRowBytes = o["rowBytes"].toDouble(-1);
    t.source = opt::StatisticsSource::Manual;
    t.provenance = "用户在本地配置的表行数/平均行宽";
    if (t.name.empty() || !std::isfinite(t.rows) || t.rows < 0 ||
        t.rows > 1e15 || !std::isfinite(t.averageRowBytes) ||
        t.averageRowBytes <= 0 || t.averageRowBytes > 1e9)
      throw std::runtime_error("本地手工统计数值无效");
    stats.tables.push_back(std::move(t));
  }
  return stats;
}
struct Request {
  sc::Dialect dialect;
  sc::NameResolutionContext context;
  QString sql, storePath, cacheKey;
  navidog::MetadataSnapshotPtr metadata;
  opt::MemoryStatistics manual;
  opt::PlannerOptions options;
  sc::FormatStyle formatting;
  bool load = false, save = false, calciteSyntax = false;
};
class Task final : public QObject, public QRunnable {
  Q_OBJECT
public:
  Request request;
  std::shared_ptr<std::atomic_bool> cancel =
      std::make_shared<std::atomic_bool>(false);
  void run() override {
    const navidog::OperationTimer timing(navidog::TimedOperation::SqlOptimize);
    opt::OptimizationResult result;
    auto stats = request.manual;
    QString planSql, planSqlError;
    try {
      if (request.load)
        stats = readManual(request.storePath, request.cacheKey);
      if (request.save) {
        QJsonArray tables;
        for (const auto &t : stats.tables)
          tables.append(tableJson(t));
        const auto bytes =
            QJsonDocument(QJsonObject{{"version", 1}, {"tables", tables}})
                .toJson(QJsonDocument::Compact);
        if (bytes.size() > 256 * 1024)
          throw std::runtime_error("手工统计超过 256 KiB");
        if (cancel->load())
          throw std::runtime_error("操作已取消");
        navidog::LocalStore store(request.storePath);
        store.putDocument(request.cacheKey, bytes);
      }
      auto combined = stats;
      if (request.metadata && request.metadata->usable() &&
          request.metadata->statistics &&
          request.metadata->statistics->refreshedMs <=
              QDateTime::currentMSecsSinceEpoch() &&
          QDateTime::currentMSecsSinceEpoch() -
                  request.metadata->statistics->refreshedMs <
              navidog::MetadataSnapshot::ttlMs) {
        combined.tables = request.metadata->statistics->tables;
        for (const auto &manual : stats.tables) {
          auto found = std::find_if(combined.tables.begin(),
                                    combined.tables.end(), [&](const auto &t) {
                                      return t.database == manual.database &&
                                             t.schema == manual.schema &&
                                             t.name == manual.name;
                                    });
          if (found == combined.tables.end())
            combined.tables.push_back(manual);
          else {
            found->rows = manual.rows;
            found->averageRowBytes = manual.averageRowBytes;
            found->rowBytesAssumed = false;
            found->source = opt::StatisticsSource::Manual;
            found->provenance = "手工行数/行宽，索引与列分布来自当前目录缓存";
            for (auto &col : found->columns)
              if (col.distinct)
                col.distinct = std::min(*col.distinct, found->rows);
          }
        }
      }
      const auto deadline =
          std::chrono::steady_clock::now() + std::chrono::milliseconds(10000);
      sc::LanguageOptions language;
      language.calciteExtensions=request.calciteSyntax;
      language.limits.sourceBytes = 512 * 1024;
      language.shouldStop = [flag = cancel, deadline] {
        return flag->load() || std::chrono::steady_clock::now() > deadline;
      };
      request.options.shouldStop = language.shouldStop;
      navidog::MetadataCatalog catalog(request.metadata);
      auto c = sc::compile(request.dialect, request.sql.toUtf8().toStdString(),
                           language, request.metadata ? &catalog : nullptr,
                           request.context);
      if (!c->ok())
        result.diagnostics = c->diagnostics;
      else {
        result = opt::optimize(c->model, &combined, request.options);
        if (result.ok() && request.dialect==sc::Dialect::PostgreSQL) {
          sc::GenerationOptions generation;
          generation.maxBytes=256*1024;
          generation.shouldStop=language.shouldStop;
          const auto root=result.heuristicRoot?result.heuristicRoot:result.logical.root;
          auto preview=opt::generatePlanSql(result.logical,root,generation);
          if (preview.ok()) {
            sc::FormatOptions format;
            format.language.limits.sourceBytes=generation.maxBytes;
            format.maxOutputBytes=generation.maxBytes;
            format.shouldStop=language.shouldStop;
            auto formatted=sc::formatSql(request.dialect,preview.sql,request.formatting,format);
            planSql=name(formatted.ok()?formatted.sql:preview.sql);
          } else {
            planSqlError=name(preview.diagnostics.front().message);
          }
        }
      }
    } catch (const std::exception &e) {
      result.diagnostics.push_back(
          {sc::DiagnosticCode::InvalidQueryShape, {}, e.what()});
    }
    emit completed(result, stats, planSql, planSqlError);
    optimizationSlots.release();
  }
signals:
  void completed(const opt::OptimizationResult &,
                 const opt::MemoryStatistics &, const QString &planSql,
                 const QString &planSqlError);
};
class Dialog final : public navidog::ui::Dialog {
  Request base;
  SqlEditor *source;
  QLabel *status;
  navidog::ui::Button *analyze, *cancel, *save, *copy, *exportReport;
  navidog::ui::NumberInput *rows, *memory, *milliseconds;
  QCheckBox *pushdown, *commute, *associate, *heuristic;
  QCheckBox *calciteSyntax=nullptr;
  QTreeWidget *best, *baseline;
  QListWidget *advice, *rules;
  QStandardItemModel *statistics;
  QPlainTextEdit *report;
  SqlReadOnlyView *planSql=nullptr;
  std::shared_ptr<std::atomic_bool> active;
  std::uint64_t revision = 0;
  bool loaded = false;
  QByteArray reportBytes;
  std::vector<opt::TableStatistics> displayed;
  void configureEditor() {
    const auto kind=base.metadata?base.metadata->driver:
        base.dialect==sc::Dialect::PostgreSQL?navidog::DatabaseKind::PostgreSQL:
        navidog::databaseKindFromKey(QString::fromLatin1(sc::dialectName(base.dialect)));
    source->setDiagnosticContext(kind,name(base.context.database),name(base.context.schema),
                                 calciteSyntax && calciteSyntax->isChecked());
    source->setMetadataSnapshot(base.metadata);
  }
  void invalidate() {
    ++revision;
    if (active)
      active->store(true);
    active.reset();
    analyze->setEnabled(true);
    cancel->setEnabled(false);
    copy->setEnabled(false);
    exportReport->setEnabled(false);
    best->clear();
    baseline->clear();
    advice->clear();
    rules->clear();
    report->clear();
    if (planSql) {
      planSql->setSourceSql({});
      planSql->setPlaceholderText("分析后显示可生成的 PostgreSQL 计划 SQL。");
    }
    reportBytes.clear();
    status->setText(
        "更改后请重新分析。成本为通用相对单位，缺少统计时使用假设值。");
  }
  opt::MemoryStatistics manual() {
    auto out = base.manual;
    for (int i = 0; i < statistics->rowCount(); ++i) {
      auto t = displayed.at(std::size_t(i));
      if (t.source != opt::StatisticsSource::Manual)
        continue;
      t.rowBytesAssumed = false;
      bool okRows = false, okBytes = false;
      t.rows = statistics->item(i, 1)->text().toDouble(&okRows);
      t.averageRowBytes = statistics->item(i, 2)->text().toDouble(&okBytes);
      if (!okRows || !okBytes || !std::isfinite(t.rows) || t.rows < 0 ||
          t.rows > 1e15 || !std::isfinite(t.averageRowBytes) ||
          t.averageRowBytes <= 0 || t.averageRowBytes > 1e9)
        throw std::runtime_error(
            "表行数须为 0–10¹⁵，平均行宽须大于 0 且不超过 10⁹ 字节。");
      auto found = std::find_if(
          out.tables.begin(), out.tables.end(), [&](const auto &v) {
            return v.database == t.database && v.schema == t.schema &&
                   v.name == t.name;
          });
      if (found == out.tables.end())
        out.tables.push_back(t);
      else
        *found = t;
    }
    if (out.tables.size() > 256)
      throw std::runtime_error("手工统计最多保存 256 张表。");
    return out;
  }
  void tree(QTreeWidget *widget, const opt::PhysicalPlan &plan,
            const opt::CostWeights &weights) {
    widget->clear();
    struct Work {
      opt::PlanId id;
      QTreeWidgetItem *parent;
      std::size_t depth;
    };
    std::vector<Work> work;
    if (plan.root)
      work.push_back({plan.root, nullptr, 0});
    std::size_t count = 0;
    while (!work.empty() && count++ < 1000) {
      auto w = work.back();
      work.pop_back();
      if (!w.id || w.id >= plan.nodes.size() || w.depth > 128)
        continue;
      const auto &n = plan.nodes[w.id];
      auto *item = new QTreeWidgetItem(QStringList{
          name(opt::physicalKindName(n.kind)),
          name(n.object) + (n.index.empty() ? "" : " / " + name(n.index)),
          QString::number(n.rows, 'g', 7),
          QString::number(n.total.score(weights), 'g', 7),
          QString::number(n.total.memory / 1048576, 'g', 5)});
      item->setToolTip(
          0, name(n.description) +
                 (n.assumedStatistics ? "\n含缺省统计或选择率假设" : ""));
      if (n.assumedStatistics)
        item->setForeground(2, QColor("#956315"));
      if (w.parent)
        w.parent->addChild(item);
      else
        widget->addTopLevelItem(item);
      for (auto it = n.inputs.rbegin(); it != n.inputs.rend(); ++it)
        work.push_back({*it, item, w.depth + 1});
    }
    widget->expandToDepth(4);
  }
  void showResult(const opt::OptimizationResult &result, const QString &preview,
                  const QString &previewError) {
    active.reset();
    analyze->setEnabled(true);
    cancel->setEnabled(false);
    if (!result.ok()) {
      status->setText("无法完成本地优化；点击诊断可定位源 SQL。");
      sc::SourceBuffer buffer(source->toPlainText().toUtf8().toStdString());
      for (const auto &d : result.diagnostics) {
        const auto pos = buffer.position(d.range.begin);
        auto *item = new QListWidgetItem(QString("第 %1 行：%2")
                                             .arg(qulonglong(pos.line))
                                             .arg(name(d.message)),
                                         advice);
        item->setData(Qt::UserRole, qulonglong(d.range.begin));
        item->setData(Qt::UserRole + 1, qulonglong(d.range.end));
      }
      return;
    }
    loaded = true;
    tree(best, result.best, result.weights);
    tree(baseline, result.baseline, result.weights);
    if (planSql) {
      planSql->setSourceSql(preview);
      if (!previewError.isEmpty())
        planSql->setPlaceholderText("该逻辑计划暂时无法生成 PostgreSQL SQL。\n"+previewError);
    }

    {
      QSignalBlocker blocker(statistics);
      statistics->setRowCount(0);
      displayed.clear();
      for (const auto &t : result.statistics) {
        if (displayed.size() >= 256)
          break;
        const auto found = std::find_if(
            displayed.begin(), displayed.end(), [&](const auto &v) {
              return v.database == t.database && v.schema == t.schema &&
                     v.name == t.name;
            });
        if (found != displayed.end())
          continue;
        displayed.push_back(t);
        auto *object = new QStandardItem(
            (t.database.empty() ? "" : name(t.database) + ".") +
            (t.schema.empty() ? "" : name(t.schema) + ".") + name(t.name));
        object->setEditable(false);
        auto *origin =
            new QStandardItem(name(opt::statisticsSourceName(t.source)));
        origin->setEditable(false);
        statistics->appendRow(
            {object, new QStandardItem(QString::number(t.rows, 'g', 15)),
             new QStandardItem(QString::number(t.averageRowBytes, 'g', 10)),
             origin});
      }
    }
    for (const auto &a : result.advice) {
      auto *item =
          new QListWidgetItem(name(a.title) + "\n" + name(a.detail), advice);
      item->setData(Qt::UserRole, qulonglong(a.origin.begin));
      item->setData(Qt::UserRole + 1, qulonglong(a.origin.end));
    }
    if (result.advice.empty())
      new QListWidgetItem("当前规则未发现明显问题；这不表示查询一定最优。",
                          advice);
    for (const auto &r : result.rules) {
      if (rules->count() >= 1000)
        break;
      auto *item =
          new QListWidgetItem(name(r.rule) + QString(" · %1 · %2 → %3")
                                                 .arg(r.heuristic?QString("HEP"):QString("G%1").arg(r.group))
                                                 .arg(r.before)
                                                 .arg(r.after),
                              rules);
      item->setToolTip(name(r.reason));
    }
    QString reportWarning;
    try {
      reportBytes = QByteArray::fromStdString(opt::dumpOptimization(result));
      report->setPlainText(
          QString::fromUtf8(reportBytes.left(512 * 1024)) +
          (reportBytes.size() > 512 * 1024
               ? "\n…界面仅显示前 512 KiB，导出可取得完整报告。"
               : ""));
      copy->setEnabled(true);
      exportReport->setEnabled(true);
    } catch (const std::exception &e) {
      reportBytes.clear();
      copy->setEnabled(false);
      exportReport->setEnabled(false);
      reportWarning = " · 报告超出限额";
      report->setPlainText(QString::fromUtf8(e.what()));
    }
    const auto before = result.baseline.nodes[result.baseline.root].total.score(
                   result.weights),
               after = result.best.nodes[result.best.root].total.score(
                   result.weights);
    status->setText(
        QString(
            "基线 %1 → 候选 %2（相对成本） · %3 组 / %4 个逻辑候选 · %5 ms%6")
            .arg(before, 0, 'g', 7)
            .arg(after, 0, 'g', 7)
            .arg(qulonglong(result.memo.groups))
            .arg(qulonglong(result.memo.logicalExpressions))
            .arg(result.elapsedMs, 0, 'f', 1)
            .arg(result.searchComplete ? ""
                                       : " · 搜索达到预算，保留已完成方案") +
        reportWarning);
  }
  void start(bool persist = false) {
    Request request = base;
    try {
      request.manual = manual();
    } catch (const std::exception &e) {
      status->setText(QString::fromUtf8(e.what()));
      return;
    }
    invalidate();
    if (source->toPlainText().toUtf8().size() > 512 * 1024) {
      status->setText("单次分析上限 512 KiB，请选择一条完整 SQL。");
      return;
    }
    if (!optimizationSlots.tryAcquire()) {
      status->setText("分析任务较多，请稍后再试。");
      return;
    }
    request.sql = source->toPlainText();
    request.load = !loaded;
    request.save = persist;
    request.options.assumedRows = rows->value();
    request.options.memoryBytes = memory->value() * 1048576.;
    request.options.maxTimeMs = milliseconds->value();
    request.options.filterPushdown = pushdown->isChecked();
    request.options.joinCommutation = commute->isChecked();
    request.options.joinAssociation = associate->isChecked();
    request.options.heuristic.enabled=heuristic->isChecked();
    request.calciteSyntax=calciteSyntax && calciteSyntax->isChecked();
    static const int registered = qRegisterMetaType<opt::OptimizationResult>();
    static const int registeredStats =
        qRegisterMetaType<opt::MemoryStatistics>();
    Q_UNUSED(registered);
    Q_UNUSED(registeredStats);
    auto *task = new Task;
    task->request = std::move(request);
    active = task->cancel;
    auto flag = active;
    const auto stamp = revision;
    connect(this, &QObject::destroyed, [flag] { flag->store(true); });
    connect(
        task, &Task::completed, this,
        [this, stamp](const opt::OptimizationResult &r,
                      const opt::MemoryStatistics &stats, const QString &preview,
                      const QString &previewError) {
          if (stamp == revision) {
            base.manual = stats;
            showResult(r,preview,previewError);
          }
        },
        Qt::QueuedConnection);
    analyze->setEnabled(false);
    cancel->setEnabled(true);
    status->setText(persist ? "正在保存手工统计并重新分析…"
                            : "正在本地解析、建立关系代数并搜索候选…");
    pool().start(task);
  }

public:
  Dialog(QWidget *parent, Request request, navidog::ui::ExportContext exportContext)
      : navidog::ui::Dialog(parent), base(std::move(request)) {
    setWindowTitle("SQL 本地优化 · Memo / 成本分析");
    setObjectName("sqlOptimizationDialog");
    resize(1240, 820);
    auto *layout = new QVBoxLayout(this);
    auto *intro = new QLabel("在数据库之外比较通用物理方案，不连接或执行 "
                             "SQL。成本使用独立模型；不能与数据库 EXPLAIN 的 "
                             "COST 或耗时直接比较。统计不足时会明确标记。");
    intro->setWordWrap(true);
    layout->addWidget(intro);
    auto *controls = new QHBoxLayout;
    rows = new navidog::ui::NumberInput;
    rows->setRange(0, 1000000000);
    rows->setValue(int(base.options.assumedRows));
    memory = new navidog::ui::NumberInput;
    memory->setRange(1, 4096);
    memory->setValue(int(base.options.memoryBytes / 1048576));
    milliseconds = new navidog::ui::NumberInput;
    milliseconds->setRange(50, 10000);
    milliseconds->setValue(int(base.options.maxTimeMs));
    controls->addWidget(new QLabel("缺省表行数"));
    controls->addWidget(rows);
    controls->addWidget(new QLabel("算子内存 MiB"));
    controls->addWidget(memory);
    controls->addWidget(new QLabel("搜索预算 ms"));
    controls->addWidget(milliseconds);
    analyze = new navidog::ui::Button("分析 SQL", "optimizeSqlNow");
    cancel = new navidog::ui::Button("取消分析", "cancelSqlOptimization");
    controls->addStretch();
    controls->addWidget(analyze);
    controls->addWidget(cancel);
    layout->addLayout(controls);
    auto *toggles = new QHBoxLayout;
    pushdown = new QCheckBox("过滤优化");
    pushdown->setToolTip("过滤下推、恒真条件移除和受限的常量空结果化简；规则不"
                         "会执行或改写原 SQL。");
    commute = new QCheckBox("内连接交换");
    associate = new QCheckBox("内连接结合");
    for (auto *box : {pushdown, commute, associate}) {
      box->setChecked(box == pushdown  ? base.options.filterPushdown
                      : box == commute ? base.options.joinCommutation
                                       : base.options.joinAssociation);
      toggles->addWidget(box);
    }
    heuristic=new QCheckBox("启发式规则");heuristic->setObjectName("optimizationHepEnabled");
    heuristic->setChecked(base.options.heuristic.enabled);
    heuristic->setToolTip("先运行有界 Hep 规则程序，再由 Memo / Volcano 比较完整基线和候选成本；不改写或执行 SQL。");
    toggles->addWidget(heuristic);
    if (base.dialect==sc::Dialect::PostgreSQL) {
      calciteSyntax=new QCheckBox("Calcite 扩展语法");
      calciteSyntax->setObjectName("optimizationCalciteSyntax");
      calciteSyntax->setToolTip("在本地分析中解析 UNPIVOT、WITHIN DISTINCT 等扩展语法。");
      toggles->addWidget(calciteSyntax);
    }
    toggles->addStretch();
    layout->addLayout(toggles);
    auto *splitter = new QSplitter;
    auto *left = new QSplitter(Qt::Vertical);
    source = new SqlEditor;
    source->setObjectName("optimizationSource");
    configureEditor();
    source->setPlainText(base.sql);
    left->addWidget(source);
    auto *statsPane = new QWidget;
    auto *statsLayout = new QVBoxLayout(statsPane);
    statsLayout->setContentsMargins(0, 0, 0, 0);
    auto *statsIntro = new QLabel("使用的统计：首次分析后可修改行数 / "
                                  "平均行宽（字节），再次分析使用手工估计。");
    statsIntro->setWordWrap(true);
    statsLayout->addWidget(statsIntro);
    auto *grid = new navidog::ui::ObjectGrid;
    statistics = new QStandardItemModel(grid);
    statistics->setHorizontalHeaderLabels({"表", "行数", "平均行宽", "来源"});
    grid->setModel(statistics);
    grid->horizontalHeader()->setSectionResizeMode(0, QHeaderView::Stretch);
    grid->setObjectName("optimizationStatistics");
    statsLayout->addWidget(grid);
    left->addWidget(statsPane);
    left->setSizes({480, 220});
    splitter->addWidget(left);
    auto *tabs = new QTabWidget;
    auto makeTree = [] {
      auto *t = new QTreeWidget;
      t->setHeaderLabels(
          {"算子", "对象 / 索引", "估计行数", "累计相对成本", "峰值 MiB"});
      t->setUniformRowHeights(true);
      t->setColumnWidth(0, 155);
      t->setColumnWidth(1, 145);
      return t;
    };
    best = makeTree();
    best->setObjectName("optimizationBestPlan");
    baseline = makeTree();
    advice = new QListWidget;
    advice->setWordWrap(true);
    advice->setObjectName("optimizationAdvice");
    rules = new QListWidget;
    report = new QPlainTextEdit;
    report->setReadOnly(true);
    tabs->addTab(best, "候选计划");
    tabs->addTab(baseline, "基线计划");
    tabs->addTab(advice, "建议 / 诊断");
    tabs->addTab(rules, "规则轨迹");
    if (base.dialect==sc::Dialect::PostgreSQL) {
      planSql=new SqlReadOnlyView;
      planSql->setDialect(sc::Dialect::PostgreSQL);
      planSql->setObjectName("optimizationPlanSql");
      planSql->setFormatStyle(base.formatting);
      const auto sqlTab = tabs->addTab(planSql->withActions(),"计划 SQL");
      tabs->setTabToolTip(sqlTab,"显示启发式改写后的逻辑计划 SQL。");
    }
    tabs->addTab(report, "报告 JSON");
    splitter->addWidget(tabs);
    splitter->setStretchFactor(0, 1);
    splitter->setStretchFactor(1, 1);
    layout->addWidget(splitter, 1);
    status = new QLabel;
    status->setWordWrap(true);
    status->setObjectName("optimizationStatus");
    layout->addWidget(status);
    auto *footer = new QHBoxLayout;
    save = new navidog::ui::Button("保存手工统计并分析",
                                  "saveOptimizationStatistics");
    save->setEnabled(!base.storePath.isEmpty() && !base.cacheKey.isEmpty());
    footer->addWidget(save);
    footer->addStretch();
    copy = new navidog::ui::Button("复制报告", "copyOptimizationReport");
    exportReport =
        new navidog::ui::Button("导出报告…", "exportOptimizationReport");
    auto *close = new navidog::ui::Button("关闭", "closeOptimization");
    footer->addWidget(copy);
    footer->addWidget(exportReport);
    footer->addWidget(close);
    layout->addLayout(footer);
    connect(analyze, &QToolButton::clicked, this, [this] { start(); });
    connect(save, &QToolButton::clicked, this, [this] { start(true); });
    connect(cancel, &QToolButton::clicked, this, [this] {
      invalidate();
      status->setText("本地分析已取消。");
    });
    connect(close, &QToolButton::clicked, this, &QDialog::reject);
    connect(source, &QPlainTextEdit::textChanged, this,
            [this] { invalidate(); });
    for (auto *box : {pushdown, commute, associate})
      connect(box, &QCheckBox::toggled, this, [this] { invalidate(); });
    if (calciteSyntax)
      connect(calciteSyntax,&QCheckBox::toggled,this,[this] {
        configureEditor();
        invalidate();
      });
    for (auto *input : {rows, memory, milliseconds})
      connect(input, qOverload<int>(&QSpinBox::valueChanged), this,
              [this] { invalidate(); });
    connect(statistics, &QStandardItemModel::itemChanged, this,
            [this](QStandardItem *item) {
              if (item->column() != 1 && item->column() != 2)
                return;
              const auto row = item->row();
              if (row < 0 || std::size_t(row) >= displayed.size())
                return;
              displayed[std::size_t(row)].source =
                  opt::StatisticsSource::Manual;
              {
                QSignalBlocker b(statistics);
                statistics->item(row, 3)->setText("manual");
              }
              invalidate();
            });
    connect(
        advice, &QListWidget::itemClicked, this, [this](QListWidgetItem *item) {
          if (!item->data(Qt::UserRole).isValid())
            return;
          const auto bytes = source->toPlainText().toUtf8();
          auto cursor = source->textCursor();
          cursor.setPosition(
              QString::fromUtf8(
                  bytes.left(int(qMin<qulonglong>(
                      bytes.size(), item->data(Qt::UserRole).toULongLong()))))
                  .size());
          cursor.setPosition(
              QString::fromUtf8(
                  bytes.left(int(qMin<qulonglong>(
                      bytes.size(),
                      item->data(Qt::UserRole + 1).toULongLong()))))
                  .size(),
              QTextCursor::KeepAnchor);
          source->setTextCursor(cursor);
          source->setFocus();
        });
    connect(copy, &QToolButton::clicked, this, [this] {
      QApplication::clipboard()->setText(QString::fromUtf8(reportBytes));
    });
    connect(exportReport, &QToolButton::clicked, this, [this, exportContext] {
      auto path = navidog::ui::chooseExportFile(
          this, "导出本地优化报告", exportContext, "SQL优化报告", "JSON (*.json)");
      if (path.isEmpty())
        return;
      QSaveFile file(path);
      if (!file.open(QIODevice::WriteOnly) ||
          file.write(reportBytes) != reportBytes.size() || !file.commit())
        QMessageBox::warning(this, "导出失败", file.errorString());
    });
    invalidate();
  }
};
} // namespace
void showSqlOptimization(QWidget *parent, sc::Dialect dialect,
                         const QString &sql,
                         const sc::NameResolutionContext &context,
                         navidog::MetadataSnapshotPtr metadata,
                         const QString &path, const QString &key,
                         const ApplicationSettings &settings, navidog::ui::ExportContext exportContext) {
  Request r;
  r.dialect = dialect;
  r.sql = sql;
  r.context = context;
  r.metadata = std::move(metadata);
  r.storePath = path;
  r.cacheKey = key;
  r.options = optimizationOptions(settings);
  r.formatting = settings.formatting;
  Dialog dialog(parent, std::move(r), std::move(exportContext));
  dialog.exec();
}
#include "sql_optimization.moc"
