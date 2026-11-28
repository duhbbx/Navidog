#include "navidog/database_sql.hpp"
#include "navidog/redis_commands.hpp"
#include "navidog/document_commands.hpp"
#include <QRegularExpression>
#include "editor_completion.hpp"
#include <QRunnable>
#include <QSemaphore>
#include <QThreadPool>
#include <chrono>
#include <algorithm>
#include <unordered_map>
namespace {
namespace sc = navidog::sql::compiler;
QSemaphore completionSlots(4);
QThreadPool &pool() {
  static QThreadPool p;
  static bool init = [] {
    p.setMaxThreadCount(2);
    p.setExpiryTimeout(10000);
    return true;
  }();
  Q_UNUSED(init);
  return p;
}
class CompletionTask final : public QObject, public QRunnable {
  Q_OBJECT
public:
  QString text, database, schema;
  int cursor = 0;
  navidog::DatabaseKind kind;
  bool calciteExtensions = false;
  navidog::MetadataSnapshotPtr metadata;
  SqlCompletionController::Activation activation = SqlCompletionController::Activation::Automatic;
  std::shared_ptr<std::atomic_bool> cancel;
  void run() override;
signals:
  void completed(const EditorCompletionResult &);
};

void CompletionTask::run() {
  EditorCompletionResult result;
  result.explicitRequest = activation == SqlCompletionController::Activation::Manual;
  if (kind == navidog::DatabaseKind::MongoDb) {
    const auto source = text.left(cursor);
    static const QRegularExpression key(QStringLiteral("([{,])\\s*(\"?)([a-zA-Z0-9_$]*)$"));
    static const QRegularExpression first(QStringLiteral("^\\s*\\{\\s*\"([a-zA-Z]+)\"\\s*:"));
    const auto match = key.match(source);
    if (match.hasMatch()) {
      int depth = 0;
      bool inString = false, escaped = false;
      for (int i = 0; i < match.capturedStart(1); ++i) {
        const auto c = source.at(i);
        if (inString) {
          if (escaped) escaped = false;
          else if (c == '\\') escaped = true;
          else if (c == '"') inString = false;
        } else if (c == '"') inString = true;
        else if (c == '{' || c == '[') ++depth;
        else if (c == '}' || c == ']') --depth;
      }
      const bool initial = depth == 0 && match.captured(1) == "{";
      if (!inString && (initial || (depth == 1 && match.captured(1) == ","))) {
        auto candidates = initial ? navidog::mongoCommandNames()
            : navidog::mongoCommandOptions(first.match(source).captured(1));
        const auto prefix = match.captured(3);
        auto end = cursor;
        while (end < text.size() && (text.at(end).isLetterOrNumber() || text.at(end) == '_')) ++end;
        const bool closingQuote = end < text.size() && text.at(end) == '"';
        for (const auto& name : candidates) {
          if (!name.startsWith(prefix, Qt::CaseInsensitive)) continue;
          EditorCompletionItem item;
          item.label = name;
          item.insertText = (match.captured(2).isEmpty() ? QString("\"") : QString{}) +
              name + (closingQuote ? QString{} : QString("\""));
          if (!closingQuote && !text.mid(end).trimmed().startsWith(':')) item.insertText += ": ";
          item.begin = match.capturedStart(3);
          item.end = end;
          item.cursor = item.insertText.size();
          item.kind = "keyword";
          result.items << item;
        }
      }
    }
    result.status = "MongoDB 命令与参数补全";
    emit completed(result);
    completionSlots.release();
    return;
  }
  if (kind == navidog::DatabaseKind::Redis) {
    const auto begin = text.left(cursor).lastIndexOf('\n') + 1;
    const auto prefix = text.mid(begin, cursor - begin);
    const auto word = prefix.trimmed();
    if ((!word.isEmpty() || result.explicitRequest) && !word.startsWith('#') &&
        !word.contains(' ') && !word.contains('\t') && !prefix.endsWith(' ')) {
      auto end = cursor;
      while (end < text.size() && !text.at(end).isSpace()) ++end;
      for (const auto& name : navidog::redisCommandNames()) {
        if (!name.startsWith(word, Qt::CaseInsensitive)) continue;
        EditorCompletionItem item;
        item.label = name;
        item.insertText = name + ' ';
        item.begin = word.isEmpty() ? cursor : cursor - int(word.size());
        item.end = end;
        item.cursor = int(item.insertText.size());
        item.kind = "keyword";
        result.items << item;
      }
    }
    result.status = "Redis 命令补全";
    emit completed(result);
    completionSlots.release();
    return;
  }
  try {
    const auto dialect = navidog::compilerDialectFor(kind).value_or(sc::Dialect::SQLite);
    sc::CompletionOptions options;
    options.language.calciteExtensions=calciteExtensions;
    options.language.extension=navidog::compilerExtensionFor(kind);
    options.language.limits.sourceBytes = 256 * 1024;
    options.language.limits.tokens = 20000;
    options.maxItems = 128;
    options.columnListOnly = activation == SqlCompletionController::Activation::ColumnList;
    const auto deadline =
        std::chrono::steady_clock::now() + std::chrono::milliseconds(200);
    options.language.shouldStop = [flag = cancel, deadline] {
      return flag->load() || std::chrono::steady_clock::now() > deadline;
    };
    const auto bytes = text.toUtf8();
    const auto byteCursor = text.left(cursor).toUtf8().size();
    navidog::MetadataCatalog catalog(metadata);
    sc::NameResolutionContext context{database.toStdString(),
                                      schema.toStdString(), dialect};
    auto completed =
        sc::completeSql(dialect, bytes.toStdString(), std::size_t(byteCursor),
                        metadata ? &catalog : nullptr, context, options);
    std::unordered_map<std::size_t,int> positions{{0,0}};
    const auto documentPosition=[&](std::size_t offset) {
      auto [entry,inserted]=positions.try_emplace(offset);
      if (inserted)
        entry->second=int(QString::fromUtf8(bytes.constData(),qsizetype(offset)).size());
      return entry->second;
    };
    for (const auto &item : completed.items) {
      if (item.replacement.end > std::size_t(bytes.size()) ||
          item.replacement.begin > item.replacement.end ||
          item.cursor > item.insertText.size() ||
          item.selection > item.insertText.size() - item.cursor)
        continue;
      EditorCompletionItem out;
      out.label = QString::fromStdString(item.label);
      out.insertText = QString::fromStdString(item.insertText);
      out.detail = QString::fromStdString(item.detail);
      out.kind = QString::fromLatin1(sc::completionKindName(item.kind));
      out.begin=documentPosition(item.replacement.begin);
      out.end=documentPosition(item.replacement.end);
      out.cursor =
          QString::fromUtf8(item.insertText.data(), int(item.cursor)).size();
      out.selection = QString::fromUtf8(item.insertText.data() + item.cursor,
                                        int(item.selection))
                          .size();
      result.items.push_back(std::move(out));
    }
    if (!completed.suppressed && activation != SqlCompletionController::Activation::ColumnList) {
      using K = navidog::DatabaseKind;
      QStringList additions;
      switch (kind) {
      case K::DuckDb: additions = QStringList{"QUALIFY","read_parquet","read_csv","arg_max","list","count_if"}; break;
      case K::ClickHouse: additions = QStringList{"PREWHERE","QUALIFY","arrayJoin","uniqExact","toStartOfDay","countIf"}; break;
      case K::Doris: case K::StarRocks: additions = QStringList{"BITMAP_UNION","DATE_TRUNC","APPROX_COUNT_DISTINCT"}; break;
      case K::OpenGauss: additions = QStringList{"LISTAGG","GROUPING","NVL"}; break;
      case K::CockroachDb: additions = QStringList{"unique_rowid","crdb_internal"}; break;
      case K::YugabyteDb: additions = QStringList{"yb_servers","yb_table_properties"}; break;
      default: break;
      }
      int begin = cursor, end = cursor;
      while (begin > 0 && (text.at(begin-1).isLetterOrNumber() || text.at(begin-1) == '_')) --begin;
      while (end < text.size() && (text.at(end).isLetterOrNumber() || text.at(end) == '_')) ++end;
      const auto prefix = text.mid(begin, cursor - begin);
      if (!prefix.isEmpty()) for (const auto& word : additions) {
        if (!word.startsWith(prefix, Qt::CaseInsensitive)) continue;
        const bool exists = std::any_of(result.items.cbegin(),result.items.cend(),[&](const auto& option) {
            return option.label.compare(word,Qt::CaseInsensitive) == 0;
        });
        if (exists) continue;
        EditorCompletionItem item;
        item.label = item.insertText = word;
        item.detail = navidog::databaseKindName(kind);
        item.begin = begin;
        item.end = end;
        item.cursor = word.size();
        item.kind = "keyword";
        result.items << item;
      }
    }
    if (!completed.diagnostics.empty())
      result.status =
          QString::fromStdString(completed.diagnostics.front().message);
    else if (completed.suppressed)
      result.status = "字符串或注释内不补全";
    else if (metadata && !completed.metadataFresh)
      result.status = "目录候选来自本地缓存，尚未验证";
    else
      result.status =
          completed.parsedScope ? "语法与作用域补全" : "局部语法补全";
    if(metadata && !metadata->objectWarning.isEmpty())result.status+=" · "+metadata->objectWarning;
  } catch (const std::exception &e) {
    result.status = QString::fromUtf8(e.what());
  }
  emit completed(result);
  completionSlots.release();
}
} // namespace
SqlCompletionController::SqlCompletionController(Read read, QObject *parent)
    : QObject(parent), read_(std::move(read)) {
  static const int registered = qRegisterMetaType<EditorCompletionResult>();
  Q_UNUSED(registered);
  timer_.setSingleShot(true);
  timer_.setInterval(70);
  connect(&timer_, &QTimer::timeout, this, &SqlCompletionController::start);
}
SqlCompletionController::~SqlCompletionController() {
  if (cancel_)
    cancel_->store(true);
}
void SqlCompletionController::setContext(navidog::DatabaseKind kind,
                                         const QString &database,
                                         const QString &schema,
                                         bool calciteExtensions) {
  const bool sameScope=kind_==kind && database_==database && schema_==schema;
  if (sameScope && calciteExtensions_==calciteExtensions)
    return;
  kind_ = kind;
  database_ = database;
  schema_ = schema;
  calciteExtensions_=calciteExtensions;
  if (!sameScope) metadata_.reset();
  invalidate();
}
void SqlCompletionController::setMetadataSnapshot(
    navidog::MetadataSnapshotPtr metadata) {
  if (metadata_ == metadata)
    return;
  if (metadata &&
      (metadata->driver != kind_ || metadata->database != database_))
    metadata.reset();
  // Refresh the active request against the new snapshot without requiring
  // another keystroke. A request already invalidated by focus or an edit
  // is not reopened by a background cache update.
  const bool refresh = requested_ || (running_ && generation_ == runningGeneration_);
  const auto activation = activation_;
  metadata_ = std::move(metadata);
  invalidate();
  if (refresh)
    request(activation);
}
void SqlCompletionController::invalidate() {
  ++generation_;
  timer_.stop();
  requested_ = false;
  if (cancel_)
    cancel_->store(true);
  emit cleared();
}
void SqlCompletionController::request(Activation activation) {
  requested_ = true;
  activation_ = activation;
  if (!running_)
    timer_.start(activation == Activation::Manual ? 0 : 70);
}
void SqlCompletionController::start() {
  if (!requested_ || running_)
    return;
  if (!completionSlots.tryAcquire()) {
    timer_.start(80);
    return;
  }
  auto snapshot = read_();
  if (snapshot.first.toUtf8().size() > 256 * 1024) {
    completionSlots.release();
    requested_ = false;
    emit cleared();
    return;
  }
  auto *task = new CompletionTask;
  task->text = std::move(snapshot.first);
  task->cursor = snapshot.second;
  task->kind = kind_;
  task->calciteExtensions=calciteExtensions_;
  task->database = database_;
  task->schema = schema_;
  task->metadata = metadata_;
  task->activation = activation_;
  task->cancel = std::make_shared<std::atomic_bool>(false);
  cancel_ = task->cancel;
  running_ = true;
  requested_ = false;
  const auto stamp = runningGeneration_ = generation_;
  connect(
      task, &CompletionTask::completed, this,
      [this, stamp](const EditorCompletionResult &result) {
        running_ = false;
        cancel_.reset();
        if (stamp == generation_)
          emit ready(result);
        if (requested_)
          timer_.start(0);
      },
      Qt::QueuedConnection);
  pool().start(task);
}
#include "editor_completion.moc"
