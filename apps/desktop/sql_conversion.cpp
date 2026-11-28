#include "navidog/operation_timing.hpp"
#include "sql_conversion.hpp"
#include "components.hpp"
#include "editor.hpp"
#include <QApplication>
#include <QFormLayout>
#include <QJsonDocument>
#include <QLabel>
#include <QListWidget>
#include <QMessageBox>
#include <QPointer>
#include <QRunnable>
#include <QSaveFile>
#include <QSemaphore>
#include <QSplitter>
#include <QThreadPool>
#include <QVBoxLayout>
#include <atomic>
#include <chrono>
Q_DECLARE_METATYPE(navidog::sql::compiler::ConversionResult)
namespace {
namespace sc = navidog::sql::compiler;
QSemaphore conversionSlots(4);
QString changeReason(sc::ConversionChangeKind kind) {
  using Kind=sc::ConversionChangeKind;
  switch(kind) {
  case Kind::Qualifier: return "应用指定的数据库或 Schema 映射";
  case Kind::Type: return "使用目标方言中经过校验的类型";
  case Kind::NullOrdering: return "将源查询的 NULL 排序行为保留到目标查询";
  case Kind::Pagination: return "用目标方言的分页语法保留行数和偏移";
  case Kind::TableShorthand: return "展开关系读取简写以兼容目标方言";
  case Kind::DerivedValues: return "使用目标方言的派生 VALUES 构造";
  case Kind::CteSyntax: return "调整 CTE 声明以保留依赖顺序和列名";
  case Kind::WindowExclusion: return "目标窗口的默认行为与源端的显式设置相同";
  case Kind::SymmetricBounds: return "将已知整数上下界排好顺序以保留区间含义";
  case Kind::DateLiteral: return "使用不依赖会话日期格式的目标写法";
  case Kind::OneRowSource: return "按目标方言生成单行常量查询";
  case Kind::UpdateTarget: return "根据目标方言重写单表更新的目标引用";
  case Kind::DdlCommit: return "按授权保留目标数据库的 DDL 提交行为";
  case Kind::RoutineBody: return "用目标过程语言重新生成作用域、声明和控制流";
  case Kind::AggregateFilter: return "将过滤掉的行映射为 NULL，保留计数值和整数宽度";
  }
  return {};
}
QThreadPool &conversionPool() {
  static QThreadPool p;
  static bool init = [] {
    p.setMaxThreadCount(2);
    p.setExpiryTimeout(10000);
    return true;
  }();
  Q_UNUSED(init);
  return p;
}
class ConversionTask final : public QObject, public QRunnable {
  Q_OBJECT
public:
  sc::Dialect source, target;
  sc::FormatStyle style;
  QString text, fromQualifier, toQualifier;
  bool allowDdl = false;
  sc::NameResolutionContext names;
  navidog::MetadataSnapshotPtr snapshot;
  std::shared_ptr<std::atomic_bool> cancel =
      std::make_shared<std::atomic_bool>(false);
  void run() override {
    const navidog::OperationTimer timing(navidog::TimedOperation::SqlConvert);
    sc::ConversionResult result;
    try {
      sc::ConversionOptions options;
      options.names = names;
      navidog::MetadataCatalog catalog(snapshot);
      if (snapshot && snapshot->usable() &&
          snapshot->database.toStdString() == names.database &&
          snapshot->schema.toStdString() == names.schema &&
          sc::parseDialect(
              navidog::databaseKindKey(snapshot->driver).toStdString()) ==
              source)
        options.catalog = &catalog;
      options.language.limits.sourceBytes = 1024 * 1024;
      options.generation.maxBytes = 4 * 1024 * 1024;
      options.generation.compactExpressions = true;
      const auto deadline =
          std::chrono::steady_clock::now() + std::chrono::seconds(10);
      options.language.shouldStop = [flag = cancel, deadline] {
        return flag->load() || std::chrono::steady_clock::now() > deadline;
      };
      options.allowDdlTransactionChange = allowDdl;
      if (!fromQualifier.isEmpty())
        options.qualifiers.push_back(
            {{fromQualifier.toStdString()},
             toQualifier.isEmpty()
                 ? std::vector<std::string>{}
                 : std::vector<std::string>{toQualifier.toStdString()}});
      result =
          sc::convertSql(source, target, text.toUtf8().toStdString(), options);
      if(result.ok()) {
        sc::FormatOptions format;
        format.language=options.language;
        format.language.limits.sourceBytes=options.generation.maxBytes;
        format.maxOutputBytes=options.generation.maxBytes;
        format.shouldStop=options.language.shouldStop;
        auto formatted=sc::formatSql(target,result.sql,style,format);
        if(formatted.ok())result.sql=std::move(formatted.sql);
        else result.notes.emplace_back("目标 SQL 已验证，格式化超出时间或资源限制，保留生成的 SQL。");
      }
    } catch (const std::exception &e) {
      result.diagnostics.push_back(
          {sc::DiagnosticCode::InvalidQueryShape, {}, e.what()});
    }
    emit completed(result);
    conversionSlots.release();
  }
signals:
  void completed(const sc::ConversionResult &);
};
class ConversionDialog final : public navidog::ui::Dialog {
  sc::Dialect initial;
  sc::NameResolutionContext names;
  navidog::MetadataSnapshotPtr snapshot;
  sc::FormatStyle formatting;
  navidog::ui::Select *sourceDialect, *targetDialect;
  SqlEditor *source;
  SqlReadOnlyView *output;
  QPlainTextEdit *model;
  QListWidget *diagnostics, *changes;
  QLabel *status;
  navidog::ui::Button *convert, *cancel, *copy, *save;
  navidog::ui::TextInput *from, *to;
  QCheckBox *ddl;
  std::shared_ptr<std::atomic_bool> active;
  std::uint64_t revision = 0;
  void invalidate() {
    ++revision;
    if (active)
      active->store(true);
    active.reset();
    output->setSourceSql({});
    model->clear();
    diagnostics->clear();
    changes->clear();
    copy->setEnabled(false);
    save->setEnabled(false);
    cancel->setEnabled(false);
    convert->setEnabled(true);
    status->setText("更改后请重新转换；原查询不会被修改。");
  }
  void start() {
    invalidate();
    if (source->toPlainText().toUtf8().size() > 1024 * 1024) {
      status->setText("单次转换上限为 1 MiB，请选择较小的完整脚本。");
      return;
    }
    if (!conversionSlots.tryAcquire()) {
      status->setText("转换任务较多，请稍后再试。");
      return;
    }
    static const int registered = qRegisterMetaType<sc::ConversionResult>();
    Q_UNUSED(registered);
    auto *task = new ConversionTask;
    task->source =
        static_cast<sc::Dialect>(sourceDialect->currentData().toInt());
    task->target =
        static_cast<sc::Dialect>(targetDialect->currentData().toInt());
    task->text = source->toPlainText();
    task->style = formatting;
    task->names = names;
    task->snapshot = snapshot;
    task->allowDdl = ddl->isChecked();
    task->fromQualifier = from->text().trimmed();
    task->toQualifier = to->text().trimmed();
    active = task->cancel;
    auto flag = active;
    const auto stamp = revision;
    connect(this, &QObject::destroyed, [flag] { flag->store(true); });
    convert->setEnabled(false);
    cancel->setEnabled(true);
    status->setText("正在本地解析、绑定变量、转换并验证目标语法…");
    connect(
        task, &ConversionTask::completed, this,
        [this, stamp](const sc::ConversionResult &result) {
          if (stamp != revision)
            return;
          active.reset();
          convert->setEnabled(true);
          cancel->setEnabled(false);
          if (!result.ok()) {
            status->setText("转换失败：未生成部分结果。点击下方诊断定位原文。");
            const auto bytes = source->toPlainText().toUtf8();
            sc::SourceBuffer buffer(bytes.toStdString());
            for (const auto &d : result.diagnostics) {
              auto pos = buffer.position(d.range.begin);
              auto *item = new QListWidgetItem(
                  QString("第 %1 行，第 %2 字节列：%3")
                      .arg(qulonglong(pos.line))
                      .arg(qulonglong(pos.byteColumn))
                      .arg(QString::fromStdString(d.message)),
                  diagnostics);
              item->setData(Qt::UserRole, int(qMin<std::size_t>(d.range.begin,
                                                                bytes.size())));
              item->setData(Qt::UserRole + 1,
                            int(qMin<std::size_t>(d.range.end, bytes.size())));
            }
            return;
          }
          output->setSourceSql(QString::fromStdString(result.sql));
          auto json = QJsonDocument::fromJson(
              QByteArray::fromStdString(result.modelJson));
          model->setPlainText(
              json.isNull()
                  ? QString::fromStdString(result.modelJson)
                  : QString::fromUtf8(json.toJson(QJsonDocument::Indented)));
          copy->setEnabled(true);
          save->setEnabled(true);
          status->setText("目标语法验证通过；未执行。执行前仍须核对目标版本、字"
                          "段类型、排序规则和权限。");
          const auto bytes=source->toPlainText().toUtf8();
          sc::SourceBuffer buffer(bytes.toStdString());
          for(const auto& change:result.changes) {
            const auto line=buffer.position(change.origin.begin).line;
            auto* item=new QListWidgetItem(
              QString("语句 %1 · 第 %2 行\n%3 → %4\n%5")
                .arg(qulonglong(change.statement)).arg(qulonglong(line))
                .arg(QString::fromStdString(change.before),QString::fromStdString(change.after),
                     changeReason(change.kind)),changes);
            item->setData(Qt::UserRole,int(qMin<std::size_t>(change.origin.begin,bytes.size())));
            item->setData(Qt::UserRole+1,int(qMin<std::size_t>(change.origin.end,bytes.size())));
          }
          if(result.changes.empty())
            new QListWidgetItem("此脚本使用通用语法，仅按目标方言生成和排版。",changes);
          if(result.changesTruncated)
            new QListWidgetItem("变更较多，列表只显示前 2048 项。",changes);
          for (const auto &note : result.notes)
            new QListWidgetItem(QString::fromStdString(note), diagnostics);
        },
        Qt::QueuedConnection);
    conversionPool().start(task);
  }

public:
  ConversionDialog(QWidget *parent, sc::Dialect dialect, const QString &text,
                   sc::NameResolutionContext names,
                   navidog::MetadataSnapshotPtr snapshot, sc::FormatStyle style,
                   navidog::ui::ExportContext exportContext)
      : navidog::ui::Dialog(parent), initial(dialect), names(std::move(names)),
        snapshot(std::move(snapshot)), formatting(std::move(style)) {
    setObjectName("sqlConversionDialog");
    setWindowTitle("SQL 转换");
    resize(1240, 820);
    setMinimumSize(820, 600);
    auto *layout = new QVBoxLayout(this);
    auto *context = new QHBoxLayout;
    sourceDialect = new navidog::ui::Select;
    sourceDialect->setObjectName("conversionSourceDialect");
    targetDialect = new navidog::ui::Select;
    targetDialect->setObjectName("conversionTargetDialect");
    for (auto d :
         {sc::Dialect::MySQL, sc::Dialect::PostgreSQL, sc::Dialect::Oracle,
          sc::Dialect::SQLServer, sc::Dialect::SQLite, sc::Dialect::MariaDB,
          sc::Dialect::OceanBase, sc::Dialect::TiDB}) {
      auto label = QString::fromLatin1(sc::dialectName(d));
      sourceDialect->addItem(label, int(d));
      targetDialect->addItem(label, int(d));
    }
    sourceDialect->setCurrentIndex(sourceDialect->findData(int(initial)));
    targetDialect->setCurrentIndex(targetDialect->findData(
        int(initial == sc::Dialect::PostgreSQL ? sc::Dialect::MySQL
                                               : sc::Dialect::PostgreSQL)));
    context->addWidget(new QLabel("源方言"));
    context->addWidget(sourceDialect);
    context->addWidget(new QLabel("→ 目标方言"));
    context->addWidget(targetDialect);
    context->addStretch();
    convert = new navidog::ui::Button("转换 SQL", "convertSqlNow");
    cancel = new navidog::ui::Button("取消转换", "cancelSqlConversion");
    context->addWidget(convert);
    context->addWidget(cancel);
    layout->addLayout(context);
    auto *mappings = new QHBoxLayout;
    from = new navidog::ui::TextInput;
    to = new navidog::ui::TextInput;
    from->setObjectName("conversionFromQualifier");
    to->setObjectName("conversionToQualifier");
    from->setPlaceholderText("原数据库 / Schema（可选，名称须与解析结果一致）");
    to->setPlaceholderText("目标 Schema；留空则移除限定");
    mappings->addWidget(new QLabel("名称映射"));
    mappings->addWidget(from);
    mappings->addWidget(new QLabel("→"));
    mappings->addWidget(to);
    layout->addLayout(mappings);
    ddl = new QCheckBox(
        "允许 DDL 事务行为变化（隐式提交 / 可回滚不同，须人工审核）");
    ddl->setObjectName("conversionAllowDdlTransactionChange");
    layout->addWidget(ddl);
    auto *splitter = new QSplitter;
    auto *left = new QWidget;
    auto *leftLayout = new QVBoxLayout(left);
    leftLayout->setContentsMargins(0, 0, 0, 0);
    leftLayout->addWidget(new QLabel("源 SQL（选区或全文副本；不要求对象已存在）"));
    source = new SqlEditor;
    source->setObjectName("conversionSource");
    source->setDiagnosticContext(navidog::DatabaseKind::Unknown);
    source->setHighlightDialect(initial);
    source->setPlainText(text);
    leftLayout->addWidget(source);
    splitter->addWidget(left);
    auto *right = new QTabWidget;
    output = new SqlReadOnlyView;
    output->setDialect(static_cast<sc::Dialect>(targetDialect->currentData().toInt()));
    output->setObjectName("conversionOutput");
    output->setPlaceholderText(
        "全部语句转换并验证通过后，在这里显示目标 SQL。");
    model = new QPlainTextEdit;
    model->setReadOnly(true);
    model->setObjectName("conversionModel");
    output->setFormatStyle(formatting);
    right->addTab(output->withActions(false), "目标 SQL");
    changes = new QListWidget;
    changes->setObjectName("conversionChanges");
    changes->setWordWrap(true);
    right->addTab(changes, "变更说明");
    right->addTab(model, "统一模型 / 过程作用域");
    splitter->addWidget(right);
    splitter->setStretchFactor(0, 1);
    splitter->setStretchFactor(1, 1);
    layout->addWidget(splitter, 1);
    status = new QLabel;
    status->setWordWrap(true);
    status->setObjectName("conversionStatus");
    layout->addWidget(status);
    diagnostics = new QListWidget;
    diagnostics->setObjectName("conversionDiagnostics");
    diagnostics->setMaximumHeight(120);
    layout->addWidget(diagnostics);
    auto *footer = new QHBoxLayout;
    footer->addStretch();
    copy = new navidog::ui::Button("复制目标 SQL", "copyConvertedSql");
    save = new navidog::ui::Button("导出 SQL…", "saveConvertedSql");
    auto *close = new navidog::ui::Button("关闭", "closeSqlConversion");
    footer->addWidget(copy);
    footer->addWidget(save);
    footer->addWidget(close);
    layout->addLayout(footer);
    connect(convert, &QToolButton::clicked, this, [this] { start(); });
    connect(cancel, &QToolButton::clicked, this, [this] {
      invalidate();
      status->setText("转换已取消。");
    });
    connect(close, &QToolButton::clicked, this, &QDialog::reject);
    connect(copy, &QToolButton::clicked, output, &SqlReadOnlyView::copySql);
    connect(save, &QToolButton::clicked, this, [this, exportContext] {
      auto path = navidog::ui::chooseExportFile(this, "导出转换后的 SQL",
                                                exportContext, "SQL转换", "SQL (*.sql)");
      if (path.isEmpty())
        return;
      QSaveFile file(path);
      auto bytes = output->toPlainText().toUtf8();
      if (!file.open(QIODevice::WriteOnly) ||
          file.write(bytes) != bytes.size() || !file.commit())
        QMessageBox::warning(this, "导出失败", file.errorString());
    });
    connect(source, &QPlainTextEdit::textChanged, this,
            [this] { invalidate(); });
    connect(from, &QLineEdit::textChanged, this, [this] { invalidate(); });
    connect(to, &QLineEdit::textChanged, this, [this] { invalidate(); });
    connect(sourceDialect, qOverload<int>(&QComboBox::currentIndexChanged),
            this, [this] {
              source->setHighlightDialect(static_cast<sc::Dialect>(sourceDialect->currentData().toInt()));
              invalidate();
            });
    connect(targetDialect, qOverload<int>(&QComboBox::currentIndexChanged),
            this, [this] {
              output->setDialect(static_cast<sc::Dialect>(targetDialect->currentData().toInt()));
              invalidate();
            });
    connect(ddl, &QCheckBox::toggled, this, [this] { invalidate(); });
    const auto locate = [this](QListWidgetItem *item) {
          if (!item->data(Qt::UserRole).isValid())
            return;
          const auto bytes = source->toPlainText().toUtf8();
          auto cursor = source->textCursor();
          cursor.setPosition(
              QString::fromUtf8(bytes.left(item->data(Qt::UserRole).toInt()))
                  .size());
          cursor.setPosition(
              QString::fromUtf8(
                  bytes.left(item->data(Qt::UserRole + 1).toInt()))
                  .size(),
              QTextCursor::KeepAnchor);
          source->setTextCursor(cursor);
          source->setFocus();
        };
    connect(diagnostics,&QListWidget::itemClicked,this,locate);
    connect(changes,&QListWidget::itemClicked,this,locate);
    invalidate();
  }
  ~ConversionDialog() override {
    if (active)
      active->store(true);
  }
};
} // namespace
void showSqlConversion(QWidget *parent, sc::Dialect dialect,
                       const QString &source, sc::NameResolutionContext names,
                       navidog::MetadataSnapshotPtr snapshot, sc::FormatStyle formatting,
                       navidog::ui::ExportContext exportContext) {
  ConversionDialog dialog(parent, dialect, source, std::move(names),
                          std::move(snapshot),std::move(formatting),std::move(exportContext));
  dialog.exec();
}
#include "sql_conversion.moc"
