#include "export_paths.hpp"
#include "navidog/operation_timing.hpp"
#include "sql_formatting.hpp"
#include "application_settings.hpp"
#include "components.hpp"
#include "editor.hpp"
#include <QApplication>
#include <QClipboard>
#include <QContextMenuEvent>
#include <QDir>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QFormLayout>
#include <QLabel>
#include <QListWidget>
#include <QMessageBox>
#include <QMenu>
#include <QPointer>
#include <QRunnable>
#include <QSaveFile>
#include <QScrollArea>
#include <QSemaphore>
#include <QSplitter>
#include <QStackedWidget>
#include <QTextCursor>
#include <QTextDocument>
#include <QThreadPool>
#include <QTimer>
#include <atomic>
#include <chrono>
#include <map>
#include <memory>
Q_DECLARE_METATYPE(navidog::sql::compiler::FormatResult)
namespace {
namespace sc = navidog::sql::compiler;
constexpr auto previewFormattingPath = "navidogSqlPreviewFormattingPath";
QSemaphore formatSlots(4);
QThreadPool &pool() {
  static QThreadPool p;
  static const bool init = [] {
    p.setMaxThreadCount(2);
    p.setExpiryTimeout(10000);
    return true;
  }();
  Q_UNUSED(init);
  return p;
}
QByteArray readConfig(const QString &path) {
  QFile file(path);
  if (!file.open(QIODevice::ReadOnly))
    throw std::runtime_error(file.errorString().toStdString());
  if (file.size() > 65536)
    throw std::runtime_error("配置文件超过 64 KiB");
  auto bytes = file.read(65537);
  if (bytes.size() > 65536 || file.error() != QFileDevice::NoError)
    throw std::runtime_error("无法完整读取格式配置");
  return bytes;
}
sc::FormatStyle styleFor(const QString &global, const QString &script) {
  sc::FormatStyle style;
  if ((global.endsWith("settings.json") || global.endsWith(".sqlite")))
    style = loadApplicationSettings(global).formatting;
  else if (QFileInfo::exists(global))
    style = sc::parseFormatStyle(readConfig(global).toStdString());
  Q_UNUSED(script);
  return style;
}
class FormatTask final : public QObject, public QRunnable {
  Q_OBJECT
public:
  sc::Dialect dialect;
  sc::SqlExtension extension = sc::SqlExtension::None;
  QString source, global, script;
  std::optional<sc::FormatStyle> style;
  std::shared_ptr<std::atomic_bool> cancel =
      std::make_shared<std::atomic_bool>(false);
  void run() override {
    const navidog::OperationTimer timing(navidog::TimedOperation::SqlFormat);
    sc::FormatResult result;
    try {
      auto options = sc::FormatOptions{};
      options.language.extension = extension;
      const auto deadline =
          std::chrono::steady_clock::now() + std::chrono::seconds(5);
      options.shouldStop = [flag = cancel, deadline] {
        return flag->load() || std::chrono::steady_clock::now() > deadline;
      };
      options.language.limits.sourceBytes = 1024 * 1024;
      const auto bytes = source.toUtf8();
      result =
          sc::formatSql(dialect, bytes.toStdString(),
                        style ? *style : styleFor(global, script), options);
    } catch (const std::exception &error) {
      result.diagnostics.push_back(
          {sc::DiagnosticCode::InvalidQueryShape, {}, error.what()});
    }
    emit completed(result);
    formatSlots.release();
  }
signals:
  void completed(const sc::FormatResult &);
};
QString resultError(const sc::FormatResult &r) {
  return r.diagnostics.empty()
             ? QString{}
             : QString::fromStdString(r.diagnostics.front().message).left(1024);
}
void registerResult() {
  static const int registered = qRegisterMetaType<sc::FormatResult>();
  Q_UNUSED(registered);
}
QString example(sc::Dialect dialect, int type) {
  if (type == 1)
    return "select t.id,t.name,case when t.score>=60 then '通过' else '待提高' "
           "end as result from students t left join classes c on "
           "c.id=t.class_id where t.active=1 and t.score>0 order by t.id;";
  if (type == 2)
    return "create table students(id integer primary key,name "
           "varchar(80),score decimal(6,2),constraint ck_score "
           "check(score>=0));insert into students(id,name,score) "
           "values(1,'示例',90),(2,'预览',80);update students set "
           "name='示例',score=95 where id=1;";
  switch (dialect) {
  case sc::Dialect::MySQL:
  case sc::Dialect::MariaDB:
  case sc::Dialect::TiDB:
  case sc::Dialect::OceanBase:
    return "DELIMITER $$\nCREATE PROCEDURE calculate_score(IN score INT,OUT "
           "result INT) BEGIN DECLARE bonus INT DEFAULT 5; IF score>=60 THEN "
           "SET result=score+bonus; ELSE SET result=score; END IF; "
           "END$$\nDELIMITER ;";
  case sc::Dialect::Oracle:
    return "CREATE OR REPLACE FUNCTION calculate_score(score IN NUMBER) RETURN "
           "NUMBER AS result NUMBER; BEGIN IF score>=60 THEN result:=score+5; "
           "ELSE result:=score; END IF; RETURN result; END;\n/";
  case sc::Dialect::SQLServer:
    return "CREATE PROCEDURE calculate_score @score int AS BEGIN IF @score>=60 "
           "BEGIN SELECT @score+5 AS result; END ELSE BEGIN SELECT @score AS "
           "result; END; END;\nGO";
  case sc::Dialect::PostgreSQL:
    return "create function calculate_score(score int) returns int language "
           "plpgsql as $body$declare result int; begin if score>=60 then "
           "result:=score+5; else result:=score; end if; return result; "
           "end;$body$;";
  default:
    return "create trigger update_score after insert on students begin update "
           "totals set score=score+new.score where id=new.id; end;";
  }
}
QString label(const QString &key) {
  static const QMap<QString, QString> names = {
      {"IndentWidth", "缩进宽度"},
      {"ContinuationIndentWidth", "续行缩进"},
      {"TabWidth", "Tab 宽度"},
      {"ColumnLimit", "目标行宽（0 为不限）"},
      {"KeywordCase", "关键字大小写"},
      {"UseTabs", "使用 Tab 缩进"},
      {"FinalNewline", "文件末尾换行"},
      {"LineEnding", "换行符"},
      {"BlankLinesBetweenStatements", "语句间空行"},
      {"MaxEmptyLines", "保留空行上限"},
      {"SelectList", "SELECT 字段"},
      {"InsertColumns", "INSERT 字段"},
      {"ValuesRows", "VALUES 行"},
      {"UpdateAssignments", "UPDATE 赋值"},
      {"DdlColumns", "DDL 字段"},
      {"RoutineParameters", "函数 / 过程参数"},
      {"FunctionArguments", "函数调用参数"},
      {"CommaPosition", "逗号位置"},
      {"BreakBeforeClauses", "子句另起一行"},
      {"BreakBeforeJoin", "JOIN 另起一行"},
      {"BreakBeforeBooleanOperators", "AND / OR 另起一行"},
      {"IndentSubqueries", "缩进子查询"},
      {"IndentCaseBranches", "缩进 CASE 分支"},
      {"SpacesAroundOperators", "运算符两侧空格"},
      {"SpaceAfterComma", "逗号后空格"},
      {"SpacesInParentheses", "括号内空格"},
      {"AlignAliases", "对齐 AS 别名"},
      {"AlignAssignments", "对齐赋值符号"},
      {"FormatRoutineBodies", "格式化已识别的 PostgreSQL 美元引号函数体"}};
  return names.value(key, key);
}
} // namespace

void setSqlPreviewFormattingPath(QWidget &window, const QString &settingsPath) {
  window.setProperty(previewFormattingPath, settingsPath);
}

void SqlReadOnlyView::setFormatStyle(const sc::FormatStyle &style) {
  formatStyle_ = style;
  ++formatContext_;
}

void SqlReadOnlyView::setFormattingEnabled(bool enabled) {
  if (formattingEnabled_ == enabled)
    return;
  formattingEnabled_ = enabled;
  ++formatContext_;
  emit formatAvailabilityChanged();
}

bool SqlReadOnlyView::canFormat() const {
  return formattingEnabled_ && !formatting_ && !document()->isEmpty();
}

QWidget *SqlReadOnlyView::withActions(bool showCopy) {
  auto *panel = new QWidget(parentWidget());
  auto *layout = new QVBoxLayout(panel);
  layout->setContentsMargins(0, 0, 0, 0);
  layout->setSpacing(0);
  auto *toolbar = new QHBoxLayout;
  toolbar->setContentsMargins(4, 2, 4, 2);
  auto *format = new navidog::ui::Button("格式化 SQL", "formatSqlPreview");
  format->setToolTip("按当前方言和格式设置重新排版预览");
  toolbar->addWidget(format);
  connect(format, &QToolButton::clicked, this, &SqlReadOnlyView::formatSql);
  if (showCopy) {
    auto *copy = new navidog::ui::Button("复制 SQL", "copySqlPreview");
    toolbar->addWidget(copy);
    connect(copy, &QToolButton::clicked, this, &SqlReadOnlyView::copySql);
    connect(this, &QPlainTextEdit::textChanged, copy,
            [this, copy] { copy->setEnabled(!document()->isEmpty()); });
    copy->setEnabled(!document()->isEmpty());
  }
  auto *status = new QLabel;
  status->setObjectName("sqlPreviewStatus");
  status->setTextFormat(Qt::PlainText);
  status->setWordWrap(true);
  toolbar->addWidget(status, 1);
  connect(this, &SqlReadOnlyView::formatStatusChanged, status, &QLabel::setText);
  connect(this, &QPlainTextEdit::textChanged, status, &QLabel::clear);
  const auto updateFormat = [this, format] { format->setEnabled(canFormat()); };
  connect(this, &SqlReadOnlyView::formatAvailabilityChanged, format, updateFormat);
  connect(this, &QPlainTextEdit::textChanged, format, updateFormat);
  updateFormat();
  layout->addLayout(toolbar);
  layout->addWidget(this, 1);
  return panel;
}

void SqlReadOnlyView::copySql() {
  if (!document()->isEmpty())
    QApplication::clipboard()->setText(toPlainText());
}

void SqlReadOnlyView::contextMenuEvent(QContextMenuEvent *event) {
  std::unique_ptr<QMenu> menu(createStandardContextMenu());
  menu->addSeparator();
  auto *format = menu->addAction("格式化 SQL", this, &SqlReadOnlyView::formatSql);
  auto *copy = menu->addAction("复制全部 SQL", this, &SqlReadOnlyView::copySql);
  format->setEnabled(canFormat());
  copy->setEnabled(!document()->isEmpty());
  menu->exec(event->globalPos());
}

void SqlReadOnlyView::formatSql() {
  if (!canFormat())
    return;
  const auto input = toPlainText();
  if (input.toUtf8().size() > 1024 * 1024) {
    emit formatStatusChanged("预览格式化上限为 1 MiB");
    return;
  }
  if (!formatSlots.tryAcquire()) {
    emit formatStatusChanged("格式化任务较多，请稍后重试");
    return;
  }
  registerResult();
  const auto revision = document()->revision();
  const auto context = formatContext_;
  auto *task = new FormatTask;
  task->dialect = dialect_;
  task->extension = extension_;
  task->source = input;
  task->style = formatStyle_;
  for (auto *ancestor = this->parentWidget(); ancestor; ancestor = ancestor->parentWidget()) {
    const auto path = ancestor->property(previewFormattingPath);
    if (path.isValid()) {
      task->global = path.toString();
      break;
    }
  }
  const auto death = connect(this, &QObject::destroyed,
                             [flag = task->cancel] { flag->store(true); });
  formatting_ = true;
  emit formatAvailabilityChanged();
  emit formatStatusChanged("正在格式化…");
  connect(task, &FormatTask::completed, this, [this, revision, context, death](const sc::FormatResult &result) {
    disconnect(death);
    formatting_ = false;
    emit formatAvailabilityChanged();
    if (document()->revision() != revision || formatContext_ != context) {
      emit formatStatusChanged({});
      return;
    }
    if (!result.ok()) {
      emit formatStatusChanged("格式化失败：" + resultError(result));
      return;
    }
    const auto output = QString::fromStdString(result.sql);
    if (output != toPlainText())
      setPlainText(output);
    emit formatStatusChanged("SQL 已格式化");
  }, Qt::QueuedConnection);
  pool().start(task);
}

void formatEditorAsync(SqlEditor *editor, sc::Dialect dialect,
                       const QString &globalConfig, const QString &scriptPath,
                       std::function<bool()> contextStillCurrent,
                       std::function<void(const QString &)> finished) {
  if (editor->isReadOnly()) {
    finished("当前编辑器为只读，无法格式化。");
    return;
  }
  if (editor->largeDocument() && !editor->hasBoundedSqlSelection()) {
    finished("大文件模式下，请选中较小的完整 SQL 语句再格式化。");
    return;
  }
  if (editor->property("sqlFormatting").toBool()) {
    finished("正在本地格式化，请稍候。");
    return;
  }
  if (!formatSlots.tryAcquire()) {
    finished("格式化任务较多，请稍后再试。");
    return;
  }
  registerResult();
  auto cursor = editor->textCursor();
  const int begin = cursor.hasSelection() ? cursor.selectionStart() : 0;
  const int textLength = editor->document()->characterCount() - 1;
  const int end = cursor.hasSelection() ? cursor.selectionEnd() : textLength;
  if (end - begin > 1024 * 1024) {
    formatSlots.release();
    finished("格式化上限为 1 MiB，请选中较小的完整语句。");
    return;
  }
  const auto selected = editor->selectedSqlOrAll();
  if (selected.toUtf8().size() > 1024 * 1024) {
    formatSlots.release();
    finished("格式化上限为 1 MiB，请选中较小的完整语句。");
    return;
  }
  const auto revision = editor->document()->revision();
  const auto originalPosition = cursor.position();
  auto *task = new FormatTask;
  task->dialect = dialect;
  task->extension = editor->sqlExtension();
  task->source = selected;
  task->global = globalConfig;
  task->script = scriptPath;
  auto flag = task->cancel;
  const auto death = QObject::connect(editor, &QObject::destroyed,
                                      [flag] { flag->store(true); });
  editor->setProperty("sqlFormatting", true);
  QObject::connect(
      task, &FormatTask::completed, editor,
      [=](const sc::FormatResult &result) {
        QObject::disconnect(death);
        editor->setProperty("sqlFormatting", false);
        if (editor->document()->revision() != revision ||
            !contextStillCurrent()) {
          finished("编辑内容或连接已变化，已忽略旧的格式化结果。");
          return;
        }
        if (!result.ok()) {
          finished("格式化未修改原文：" + resultError(result));
          return;
        }
        const auto output = QString::fromStdString(result.sql);
        if (output == selected) {
          finished("SQL 已符合当前格式。");
          return;
        }
        int newPosition = begin;
        const auto byte =
            selected.left(qMax(0, originalPosition - begin)).toUtf8().size();
        for (const auto &mapping : result.mapping) {
          if (mapping.before.end >= std::size_t(byte)) {
            const auto offset = std::min<std::size_t>(
                byte > int(mapping.before.begin) ? byte - mapping.before.begin
                                                 : 0,
                mapping.after.end - mapping.after.begin);
            newPosition =
                begin + QString::fromUtf8(result.sql.data(),
                                          int(mapping.after.begin + offset))
                            .size();
            break;
          }
        }
        auto edit = editor->textCursor();
        edit.beginEditBlock();
        edit.setPosition(begin);
        edit.setPosition(end, QTextCursor::KeepAnchor);
        edit.insertText(output);
        edit.endEditBlock();
        edit.setPosition(
            qBound(begin, newPosition, begin + int(output.size())));
        editor->setTextCursor(edit);
        finished(
            QString("已格式化%1，可一次撤销恢复。%2")
                .arg(begin == 0 && end == textLength ? "全文" : "选区",
                     result.syntaxSupported
                         ? ""
                         : "复杂对象使用保留原文的排版模式。"));
      },
      Qt::QueuedConnection);
  finished("正在本地格式化…");
  pool().start(task);
}
bool editApplicationSettings(QWidget *parent, const QString &globalConfig,
                             sc::Dialect dialect, const QString &sample,
                             int category) {
  registerResult();
  navidog::ui::Dialog dialog(parent);
  dialog.setWindowTitle("设置");
  dialog.setObjectName("applicationSettings");
  dialog.resize(940, 720);
  ApplicationSettings settings;
  try {
    settings = loadApplicationSettings(globalConfig);
  } catch (const std::exception &e) {
    QMessageBox::warning(parent, "无法读取设置", QString::fromUtf8(e.what()));
    return false;
  }
  auto *root = new QVBoxLayout(&dialog);
  auto *all = new QSplitter;
  auto *categories = new navidog::ui::CategoryList;
  categories->setObjectName("settingsCategories");
  categories->setMinimumWidth(145);
  categories->setMaximumWidth(190);
  categories->addItems({"常规", "SQL 编辑器", "查询与结果", "SQL 格式化",
                        "元数据", "SQL 分析与优化", "SQL 执行安全"});
  auto *settingsPages = new QStackedWidget;
  settingsPages->setObjectName("settingsPages");
  all->addWidget(categories);
  all->addWidget(settingsPages);
  all->setStretchFactor(1, 1);
  root->addWidget(all, 1);
  auto formPage = [&](const QString &title) {
    auto *page = new QWidget;
    auto *vertical = new QVBoxLayout(page);
    auto *heading = new QLabel(title);
    heading->setObjectName("sectionTitle");
    vertical->addWidget(heading);
    auto *form = new QFormLayout;
    form->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);
    vertical->addLayout(form);
    vertical->addStretch();
    settingsPages->addWidget(page);
    return form;
  };
  auto *general = formPage("常规");
  auto *restore = new navidog::ui::CheckBox("启动时恢复上次的标签和 SQL 草稿");
  restore->setObjectName("settingsRestoreWorkspace");
  restore->setChecked(settings.restoreWorkspace);
  general->addRow(restore);
  auto *updates = new navidog::ui::CheckBox("启动完成后检查更新");
  updates->setObjectName("settingsAutomaticUpdateCheck");
  updates->setChecked(settings.automaticUpdateCheck);
  general->addRow(updates);
  auto *directoryRow = new QWidget;
  auto *directoryLayout = new QHBoxLayout(directoryRow);
  directoryLayout->setContentsMargins(0, 0, 0, 0);
  auto *exportDirectory = new navidog::ui::TextInput(settings.exportDirectory);
  exportDirectory->setObjectName("settingsExportDirectory");
  exportDirectory->setPlaceholderText(QDir::toNativeSeparators(navidog::ui::systemDownloadDirectory()));
  auto *chooseDirectory = new navidog::ui::PushButton("选择…");
  auto *resetDirectory = new navidog::ui::PushButton("使用系统默认");
  resetDirectory->setObjectName("settingsResetExportDirectory");
  directoryLayout->addWidget(exportDirectory, 1);
  directoryLayout->addWidget(chooseDirectory);
  directoryLayout->addWidget(resetDirectory);
  general->addRow("导出与下载保存目录", directoryRow);
  QObject::connect(chooseDirectory, &QPushButton::clicked, &dialog, [&, exportDirectory] {
    const auto current = exportDirectory->text().trimmed();
    const auto chosen = QFileDialog::getExistingDirectory(&dialog, "选择默认保存目录",
        QFileInfo(current).isDir() ? current : navidog::ui::systemDownloadDirectory());
    if (!chosen.isEmpty())
      exportDirectory->setText(QDir::toNativeSeparators(chosen));
  });
  QObject::connect(resetDirectory, &QPushButton::clicked, exportDirectory, &QLineEdit::clear);
  auto *editor = formPage("SQL 编辑器");
  auto *font = new navidog::ui::NumberInput;
  font->setObjectName("settingsEditorFontSize");
  font->setRange(8, 24);
  font->setValue(settings.editorFontSize);
  editor->addRow("字体大小", font);
  auto *indent = new navidog::ui::NumberInput;
  indent->setObjectName("settingsEditorIndentWidth");
  indent->setRange(1, 8);
  indent->setValue(settings.editorIndentWidth);
  editor->addRow("Tab / 编辑缩进宽度", indent);
  auto *wrap = new navidog::ui::CheckBox("自动换行");
  wrap->setObjectName("settingsEditorWordWrap");
  wrap->setChecked(settings.editorWordWrap);
  editor->addRow(wrap);
  auto *completion = new navidog::ui::CheckBox(
      "输入时自动显示上下文补全（Ctrl+Space 可手动触发）");
  completion->setObjectName("settingsEditorCompletion");
  completion->setChecked(settings.editorCompletion);
  editor->addRow(completion);
  auto *prefix = new navidog::ui::NumberInput;
  prefix->setObjectName("settingsCompletionMinPrefix");
  prefix->setRange(1, 4);
  prefix->setValue(settings.completionMinPrefix);
  editor->addRow("自动补全的最短前缀", prefix);
  editor->addRow(new QLabel("保存后应用于已打开和之后新建的 SQL 编辑器。"));
  auto *query = formPage("查询与结果");
  auto *rowLimit = new navidog::ui::Select;
  rowLimit->setObjectName("settingsQueryRowLimit");
  for (int n : {1000, 5000, 10000})
    rowLimit->addItem(QString::number(n) + " 行", n);
  rowLimit->setCurrentIndex(rowLimit->findData(settings.queryRowLimit));
  query->addRow("新查询默认缓存行数", rowLimit);
  auto *resultLayout = new navidog::ui::Select;
  resultLayout->setObjectName("settingsResultLayout");
  resultLayout->addItems({"结果在下方", "结果在右侧"});
  resultLayout->setCurrentIndex(settings.resultsBeside ? 1 : 0);
  query->addRow("新查询默认结果布局", resultLayout);
  query->addRow(new QLabel("仅影响之后新建的查询，保留已打开标签各自的选择。"));
  auto *formatting = new QWidget;
  auto *layout = new QVBoxLayout(formatting);
  layout->setContentsMargins(0, 0, 0, 0);
  settingsPages->addWidget(formatting);
  auto *metadata = formPage("元数据");
  auto *autoMetadata = new navidog::ui::CheckBox("自动更新已连接数据库的元数据");
  autoMetadata->setObjectName("settingsAutoMetadata");
  autoMetadata->setChecked(settings.autoMetadata);
  metadata->addRow(autoMetadata);
  auto *metadataNote =
      new QLabel("仅针对已打开的连接。关闭后不自动刷新元数据。\n缓存过期后停止"
                 "缺失对象判断，不会使用旧缓存断言表或字段不存在。");
  metadataNote->setWordWrap(true);
  metadata->addRow(metadataNote);
  QObject::connect(categories, &QListWidget::currentRowChanged, settingsPages,
                   &QStackedWidget::setCurrentIndex);
  auto *cost = formPage("SQL 分析与优化");
  auto numberSetting = [&](const QString &label, const QString &key, int min,
                           int max, int value) {
    auto *input = new navidog::ui::NumberInput;
    input->setObjectName(key);
    input->setRange(min, max);
    input->setValue(value);
    cost->addRow(label, input);
    return input;
  };
  auto *assumed =
      numberSetting("缺少统计时的表行数", "settingsOptimizerAssumedRows", 0,
                    1000000000, settings.optimizerAssumedRows);
  auto *operatorMemory =
      numberSetting("算子内存预算（MiB）", "settingsOptimizerMemoryMiB", 1,
                    4096, settings.optimizerMemoryMiB);
  auto *planningTime =
      numberSetting("搜索时间上限（ms）", "settingsOptimizerTimeMs", 50, 10000,
                    settings.optimizerTimeMs);
  auto *groups = numberSetting("Memo 等价组上限", "settingsOptimizerGroups", 32,
                               8192, settings.optimizerGroups);
  auto *memoMemory =
      numberSetting("搜索空间预算（MiB）", "settingsOptimizerMemoMiB", 1, 256,
                    settings.optimizerMemoMiB);
  auto weightSetting = [&](const QString &label, const QString &key,
                           double value) {
    auto *input = new navidog::ui::DecimalInput;
    input->setObjectName(key);
    input->setRange(0, 1000000);
    input->setDecimals(6);
    input->setValue(value);
    cost->addRow(label, input);
    return input;
  };
  auto *cpuWeight = weightSetting("CPU 相对权重", "settingsOptimizerCpuWeight",
                                  settings.optimizerCpuWeight);
  auto *ioWeight = weightSetting("页 I/O 相对权重", "settingsOptimizerIoWeight",
                                 settings.optimizerIoWeight);
  auto *memoryWeight =
      weightSetting("内存相对权重 / 字节", "settingsOptimizerMemoryWeight",
                    settings.optimizerMemoryWeight);
  auto ruleSetting = [&](const QString &label, const QString &key, bool value) {
    auto *input = new navidog::ui::CheckBox(label);
    input->setObjectName(key);
    input->setChecked(value);
    cost->addRow(input);
    return input;
  };
  auto *filterRule = ruleSetting("过滤优化", "settingsOptimizerFilterPushdown",
                                 settings.optimizerFilterPushdown);
  filterRule->setToolTip("过滤下推、恒真条件移除和受限的常量空结果化简。");
  auto *commuteRule = ruleSetting("内连接交换", "settingsOptimizerJoinCommute",
                                  settings.optimizerJoinCommute);
  auto *associateRule =
      ruleSetting("内连接结合", "settingsOptimizerJoinAssociate",
                  settings.optimizerJoinAssociate);
  auto *hepRule=ruleSetting("启发式规则预处理（Hep）","settingsOptimizerHep",settings.optimizerHep);
  hepRule->setToolTip("有界规则程序：合并过滤、移除恒等投影 / 重复排序、合并常量分页、按已知唯一键移除冗余 DISTINCT。");
  auto *costNote = new QLabel(
      "这些默认值应用于之后打开的本地优化窗口。成本是通用相对值，不是毫秒或服务"
      "端 "
      "COST；达到预算会保留已完成的计划。目录统计在已连接时随元数据静默更新。");
  costNote->setWordWrap(true);
  cost->addRow(costNote);
  auto *safety = formPage("SQL 执行安全");
  const auto safetyToggle = [&](const QString &text, const char* name, bool checked) {
    auto *box = new navidog::ui::CheckBox(text);
    box->setObjectName(name);
    box->setChecked(checked);
    safety->addRow(box);
    return box;
  };
  const auto safetyNumber = [&](const QString &label, const char* name, int value,
                                int minimum, int maximum, const QString &suffix) {
    auto *input = new navidog::ui::NumberInput;
    input->setObjectName(name);
    input->setRange(minimum, maximum);
    input->setValue(value);
    input->setSuffix(suffix);
    safety->addRow(label, input);
    return input;
  };
  const auto &risk = settings.querySafety;
  auto *safetyEnabled = safetyToggle("启用写入风险检查", "settingsSqlSafetyEnabled", risk.enabled);
  auto *safetyWarnings = safetyToggle("在编辑器显示波浪线和行号标记", "settingsSqlSafetyWarnings", risk.warnings);
  auto *safetyConfirm = safetyToggle("危险语句执行前要求确认", "settingsSqlSafetyConfirm", risk.confirm);
  auto *safetyBeforeRun = safetyToggle("执行前只读预查影响范围", "settingsSqlSafetyBeforeRun", risk.previewBeforeRun);
  auto *safetyWhileEditing = safetyToggle("停止输入后预查当前已连接的查询", "settingsSqlSafetyWhileEditing", risk.previewWhileEditing);
  auto *safetyUnmeasured = safetyToggle("范围无法预查时也要求确认", "settingsSqlSafetyUnmeasured", risk.confirmUnmeasured);
  auto *safetyRows = safetyNumber("匹配行数达到", "settingsSqlSafetyRows", risk.rowThreshold, 1, 10000, " 行");
  auto *safetyPercent = safetyNumber("匹配比例达到", "settingsSqlSafetyPercent", risk.percentThreshold, 50, 100, " %");
  auto *safetyTimeout = safetyNumber("本次预查最多等待", "settingsSqlSafetyTimeout", risk.timeoutMs, 250, 10000, " ms");
  auto *safetyNote = new QLabel("预查使用独立会话，结果可能随并发写入变化。含前序语句、临时对象或未提交事务时，只做能确认的静态检查。");
  safetyNote->setWordWrap(true);
  safety->addRow(safetyNote);
  const auto updateSafety = [=] {
    const bool enabled = safetyEnabled->isChecked();
    const bool probe = enabled && (safetyBeforeRun->isChecked() || safetyWhileEditing->isChecked());
    for (auto *control : {safetyWarnings, safetyConfirm, safetyBeforeRun, safetyWhileEditing})
      control->setEnabled(enabled);
    safetyUnmeasured->setEnabled(enabled && safetyConfirm->isChecked() && safetyBeforeRun->isChecked());
    safetyRows->setEnabled(enabled);
    safetyPercent->setEnabled(probe);
    safetyTimeout->setEnabled(probe);
  };
  for (auto *box : {safetyEnabled, safetyConfirm, safetyBeforeRun, safetyWhileEditing})
    QObject::connect(box, &QCheckBox::toggled, &dialog, updateSafety);
  updateSafety();
  categories->setCurrentRow(qBound(0, category, settingsPages->count() - 1));

  auto *top = new QHBoxLayout;
  auto *preset = new navidog::ui::Select;
  preset->addItem("易读", "Readable");
  preset->addItem("紧凑", "Compact");
  preset->addItem("展开", "Expanded");
  preset->setObjectName("formatPreset");
  top->addWidget(new QLabel("预设"));
  top->addWidget(preset);
  auto *reset = new navidog::ui::Button("应用预设");
  top->addWidget(reset);
  top->addStretch();
  auto *previewSource = new navidog::ui::Select;
  previewSource->setObjectName("formatPreviewSource");
  previewSource->addItems(
      {"当前 SQL", "查询示例", "DML / DDL 示例", "函数 / 存储过程示例"});
  if (sample.trimmed().isEmpty())
    previewSource->setCurrentIndex(1);
  top->addWidget(new QLabel("预览内容"));
  top->addWidget(previewSource);
  layout->addLayout(top);
  auto *right = new QSplitter(Qt::Vertical);
  auto *pages = new QTabWidget;
  pages->setObjectName("formatCategoryTabs");
  right->addWidget(pages);
  QVector<QFormLayout *> forms;
  for (int i = 0; i < 4; ++i) {
    auto *page = new QWidget;
    auto *form = new QFormLayout(page);
    form->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);
    forms.append(form);
    auto *area = new QScrollArea;
    area->setWidgetResizable(true);
    area->setFrameShape(QFrame::NoFrame);
    area->setWidget(page);
    pages->addTab(area, QStringList{"基本样式", "字段与列表", "换行与间距",
                                    "函数与存储过程"}[i]);
  }
  auto *preview = new SqlReadOnlyView;
  preview->setObjectName("formatPreview");
  preview->setDialect(dialect);
  right->addWidget(preview->withActions());
  right->setSizes({390, 220});
  layout->addWidget(right, 1);
  auto *status = new QLabel;
  status->setWordWrap(true);
  status->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Fixed);
  layout->addWidget(status);
  auto *note = new QLabel("保存后自动应用于 SQL 格式化。预览不执行 "
                          "SQL；字符串、动态 SQL 和未知语言函数体保持原样。");
  note->setWordWrap(true);
  note->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Fixed);
  layout->addWidget(note);
  auto *buttons = new navidog::ui::FormButtons;
  root->addWidget(buttons);
  QMap<QString, QWidget *> controls;
  auto configLines =
      QString::fromStdString(sc::dumpFormatStyle({})).split('\n');
  for (const auto &line : configLines) {
    if (line.startsWith('#') || !line.contains(':') ||
        line.startsWith("BasedOnStyle:"))
      continue;
    const auto key = line.section(':', 0, 0),
               value = line.section(':', 1).trimmed();
    QWidget *control = nullptr;
    if (value == "true" || value == "false")
      control = new navidog::ui::CheckBox;
    else if (!value.isEmpty() && value.front().isDigit()) {
      auto *spin = new navidog::ui::NumberInput;
      spin->setRange(key == "IndentWidth" || key == "TabWidth" ? 1 : 0,
                     key == "ColumnLimit"               ? 240
                     : key == "ContinuationIndentWidth" ? 16
                     : key.contains("Lines")            ? 3
                                                        : 8);
      control = spin;
    } else {
      auto *combo = new navidog::ui::Select;
      if (key == "KeywordCase") {
        combo->addItem("保留原样", "Preserve");
        combo->addItem("大写", "Upper");
        combo->addItem("小写", "Lower");
      } else if (key == "CommaPosition") {
        combo->addItem("行末", "Trailing");
        combo->addItem("行首", "Leading");
      } else if (key == "LineEnding") {
        combo->addItem("保留原样", "Preserve");
        combo->addItem("LF", "LF");
        combo->addItem("CRLF", "CRLF");
      } else {
        combo->addItem("同一行", "Inline");
        combo->addItem("按行宽换行", "Fit");
        combo->addItem("每项一行", "Vertical");
      }
      control = combo;
    }
    control->setObjectName("format" + key);
    controls.insert(key, control);
    int tab = key == "RoutineParameters" || key == "FormatRoutineBodies" ? 3
              : (key.contains("List") || key.contains("Columns") ||
                 key == "ValuesRows" || key == "UpdateAssignments" ||
                 key == "FunctionArguments" || key == "CommaPosition")
                  ? 1
              : (key.startsWith("Break") || key.startsWith("IndentS") ||
                 key.startsWith("IndentC") || key.startsWith("Space") ||
                 key.startsWith("Align"))
                  ? 2
                  : 0;
    forms[tab]->addRow(label(key), control);
  }
  auto setStyle = [&](const sc::FormatStyle &style) {
    for (const auto &line :
         QString::fromStdString(sc::dumpFormatStyle(style)).split('\n')) {
      auto *c = controls.value(line.section(':', 0, 0));
      if (!c)
        continue;
      auto v = line.section(':', 1).trimmed();
      if (auto *check = qobject_cast<QCheckBox *>(c))
        check->setChecked(v == "true");
      else if (auto *spin = qobject_cast<QSpinBox *>(c))
        spin->setValue(v.toInt());
      else {
        auto *combo = qobject_cast<QComboBox *>(c);
        combo->setCurrentIndex(combo->findData(v));
      }
    }
  };
  auto getStyle = [&] {
    QString config;
    for (auto it = controls.begin(); it != controls.end(); ++it) {
      auto *c = it.value();
      QString v;
      if (auto *check = qobject_cast<QCheckBox *>(c))
        v = check->isChecked() ? "true" : "false";
      else if (auto *spin = qobject_cast<QSpinBox *>(c))
        v = QString::number(spin->value());
      else
        v = qobject_cast<QComboBox *>(c)->currentData().toString();
      config += it.key() + ": " + v + "\n";
    }
    return sc::parseFormatStyle(config.toStdString());
  };
  try {
    setStyle(settings.formatting);
  } catch (const std::exception &error) {
    setStyle({});
    status->setText(QString::fromUtf8(error.what()));
  }
  QTimer debounce;
  debounce.setSingleShot(true);
  debounce.setInterval(200);
  quint64 generation = 0;
  std::shared_ptr<std::atomic_bool> cancel;
  auto refresh = [&] {
    ++generation;
    if (cancel)
      cancel->store(true);
    debounce.start();
  };
  QObject::connect(&debounce, &QTimer::timeout, &dialog, [&] {
    try {
      auto style = getStyle();
      if (!formatSlots.tryAcquire()) {
        debounce.start();
        return;
      }
      auto *task = new FormatTask;
      task->dialect = dialect;
      task->style = style;
      task->source = previewSource->currentIndex() == 0
                         ? sample.left(12000)
                         : example(dialect, previewSource->currentIndex());
      if (task->source.trimmed().isEmpty())
        task->source = "select a,b from items where a=1 and b>2;";
      cancel = task->cancel;
      auto current = generation;
      QObject::connect(
          task, &FormatTask::completed, &dialog,
          [&, current, style](const sc::FormatResult &r) {
            if (current != generation)
              return;
            if (r.ok()) {
              preview->setFormatStyle(style);
              preview->setSourceSql(QString::fromStdString(r.sql));
              status->setText("预览已更新");
            } else
              status->setText(resultError(r));
          },
          Qt::QueuedConnection);
      pool().start(task);
    } catch (const std::exception &e) {
      status->setText(QString::fromUtf8(e.what()));
    }
  });
  QObject::connect(previewSource,
                   qOverload<int>(&QComboBox::currentIndexChanged), &dialog,
                   refresh);
  for (auto *c : controls) {
    if (auto *check = qobject_cast<QCheckBox *>(c))
      QObject::connect(check, &QCheckBox::toggled, &dialog, refresh);
    else if (auto *spin = qobject_cast<QSpinBox *>(c))
      QObject::connect(spin, qOverload<int>(&QSpinBox::valueChanged), &dialog,
                       refresh);
    else
      QObject::connect(qobject_cast<QComboBox *>(c),
                       &QComboBox::currentTextChanged, &dialog, refresh);
  }
  QObject::connect(reset, &QToolButton::clicked, &dialog, [&] {
    setStyle(sc::formatPreset(preset->currentData().toString().toStdString()));
    refresh();
  });
  QObject::connect(buttons, &QDialogButtonBox::accepted, &dialog, [&] {
    try {
      auto directory = QDir::fromNativeSeparators(exportDirectory->text().trimmed());
      if (directory.startsWith("~/"))
        directory = QDir::home().filePath(directory.mid(2));
      if (!directory.isEmpty()) {
        const QFileInfo info(directory);
        if (!info.isAbsolute() || (!info.isDir() && directory != settings.exportDirectory))
          throw std::runtime_error("请选择一个现有的保存目录，或恢复为系统默认目录。");
        directory = QDir::cleanPath(directory);
      }
      settings.exportDirectory = directory;
      settings.restoreWorkspace = restore->isChecked();
      settings.automaticUpdateCheck = updates->isChecked();
      settings.editorFontSize = font->value();
      settings.editorIndentWidth = indent->value();
      settings.editorWordWrap = wrap->isChecked();
      settings.editorCompletion = completion->isChecked();
      settings.completionMinPrefix = prefix->value();
      settings.queryRowLimit = rowLimit->currentData().toInt();
      settings.resultsBeside = resultLayout->currentIndex() == 1;
      settings.autoMetadata = autoMetadata->isChecked();
      settings.querySafety = {safetyEnabled->isChecked(), safetyConfirm->isChecked(),
          safetyWarnings->isChecked(), safetyBeforeRun->isChecked(), safetyWhileEditing->isChecked(),
          safetyUnmeasured->isChecked(), safetyRows->value(), safetyPercent->value(), safetyTimeout->value()};
      settings.optimizerAssumedRows = assumed->value();
      settings.optimizerMemoryMiB = operatorMemory->value();
      settings.optimizerTimeMs = planningTime->value();
      settings.optimizerGroups = groups->value();
      settings.optimizerMemoMiB = memoMemory->value();
      settings.optimizerCpuWeight = cpuWeight->value();
      settings.optimizerIoWeight = ioWeight->value();
      settings.optimizerMemoryWeight = memoryWeight->value();
      settings.optimizerFilterPushdown = filterRule->isChecked();
      settings.optimizerJoinCommute = commuteRule->isChecked();
      settings.optimizerJoinAssociate = associateRule->isChecked();
      settings.optimizerHep = hepRule->isChecked();
      settings.formatting = getStyle();
      saveApplicationSettings(globalConfig, settings);
      dialog.accept();
    } catch (const std::exception &e) {
      QMessageBox::warning(&dialog, "设置未保存", QString::fromUtf8(e.what()));
    }
  });
  QObject::connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
  refresh();
  const auto accepted = dialog.exec() == QDialog::Accepted;
  if (cancel)
    cancel->store(true);
  return accepted;
}
#include "sql_formatting.moc"
