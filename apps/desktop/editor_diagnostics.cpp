#include "navidog/database_sql.hpp"
#include "navidog/redis_commands.hpp"
#include "navidog/document_commands.hpp"
#include "editor_diagnostics.hpp"
#include "navidog/sql.hpp"
#include "navidog/sql/compiler/compiler.hpp"
#include "navidog/sql/compiler/conversion.hpp"
#include <QRunnable>
#include <QThreadPool>
#include <algorithm>
#include <chrono>
#include <stdexcept>

namespace {
using namespace navidog::sql::compiler;
Dialect compilerDialect(navidog::DatabaseKind kind) {
  if (auto selected = navidog::compilerDialectFor(kind))
    return *selected;
  throw std::invalid_argument("unsupported database kind");
}
QThreadPool &checkPool() {
  static QThreadPool pool;
  static const bool configured = [] {
    pool.setMaxThreadCount(2);
    pool.setExpiryTimeout(10000);
    return true;
  }();
  Q_UNUSED(configured);
  return pool;
}
QString message(const Diagnostic &d) {
  const auto detail = QString::fromStdString(d.message).left(512);
  switch (d.code) {
  case DiagnosticCode::ExpectedToken:
    if (detail.startsWith("疑似中文"))
      return detail;
    if (detail == "expected expression")
      return "缺少表达式，请检查字段、运算符或条件是否完整。";
    if (detail.startsWith("expected '") && detail.endsWith("'"))
      return "此处需要“" + detail.mid(10, detail.size() - 11) + "”。";
    if (detail.startsWith("expected identifier"))
      return "此处需要表名、字段名等标识符；保留字名称需要引用符。";
    if (detail.startsWith("unrecognized SQL statement start: "))
      return "无法识别 SQL 语句开头，请检查关键字拼写：" + detail.mid(34);
    if (detail.startsWith("ON CONFLICT / EXCLUDED"))
      return "当前连接为 MySQL：ON CONFLICT / EXCLUDED 属于 PostgreSQL "
             "写法，请使用 MySQL 的 ON DUPLICATE KEY UPDATE。";
    return "语法不完整或位置不正确：" + detail;
  case DiagnosticCode::UnknownTable:
    return "未找到该表或视图。";
  case DiagnosticCode::UnknownColumn:
    if (detail.startsWith("undeclared procedural variable: "))
      return "未声明的过程变量：" + detail.mid(32);
    return "字段未找到或不在当前作用域：" + detail;
  case DiagnosticCode::UnknownRoutine:
    return "未找到该函数或存储过程。";
  case DiagnosticCode::RoutineArgumentCount:
    return "参数个数与已缓存的所有重载均不匹配。";
  case DiagnosticCode::CursorRuntime:
    return "游标调用时可能失败：" + detail;
  case DiagnosticCode::PreparedRuntime:
    if (detail.startsWith("EXECUTE USING argument count"))
      return "EXECUTE USING 的参数个数与 PREPARE 的占位符数量不一致。";
    if (detail.startsWith("prepared statement has been deallocated"))
      return "该预处理语句已经释放。";
    return detail;
  case DiagnosticCode::LexicalError:
    return "字符串、引用符或词法错误：" + detail;
  case DiagnosticCode::InvalidEncoding:
    return "文本编码错误：" + detail;
  case DiagnosticCode::DuplicateAlias:
    return "名称重复：" + detail;
  case DiagnosticCode::InvalidQueryShape:
    if (detail ==
        "condition requires a boolean/predicate expression in this dialect")
      return "此处需要布尔条件，例如“字段 = "
             "值”；当前方言不接受这个已知类型作为条件。";
    return "语句结构不正确：" + detail;
  case DiagnosticCode::UnsupportedSyntax:
    return "本地解析器暂未覆盖或无法确认此语法；这不表示数据库一定不支持。\n" +
           detail;
  default:
    return detail;
  }
}
class CheckTask final : public QObject, public QRunnable {
  Q_OBJECT
public:
  QString source, database, schema;
  navidog::DatabaseKind kind;
  bool calciteExtensions = false;
  quint64 revision;
  navidog::MetadataSnapshotPtr snapshot;
  std::shared_ptr<std::atomic_bool> cancel;
  void run() override {
    emit checked(revision, checkEditorSql(source, kind, database, schema,
                                          cancel, snapshot, calciteExtensions));
  }
signals:
  void checked(quint64 revision, const SqlCheckResult &result);
};
} // namespace
SqlCheckResult checkEditorSql(const QString &text, navidog::DatabaseKind kind,
                              const QString &database, const QString &schema,
                              const std::shared_ptr<std::atomic_bool> &cancel,
                              navidog::MetadataSnapshotPtr snapshot,
                              bool calciteExtensions) {
  SqlCheckResult out;
  constexpr int maxBytes = 1024 * 1024;
  if (text.size() > maxBytes) {
    out.status = "超出实时检查上限（1 MiB）";
    return out;
  }
  if (kind == navidog::DatabaseKind::MongoDb) {
    const auto document = navidog::parseDocumentCommand(text);
    if (!document.error.isEmpty() && !text.isEmpty()) {
      const auto start = qBound(0, document.position, int(text.size())-1);
      out.diagnostics << SqlEditorDiagnostic{start, start+1, document.error};
    }
    out.status = text.trimmed().isEmpty() ? "等待 MongoDB 命令"
        : out.diagnostics.isEmpty() ? "JSON 和常用命令结构已检查" : "MongoDB 命令有错误";
    return out;
  }
  if (kind == navidog::DatabaseKind::Redis) {
    int start = 0;
    for (const auto& line : text.split('\n')) {
      if (cancel->load() || out.diagnostics.size() >= 100) break;
      const auto parsed = navidog::parseRedisCommand(line);
      if (!parsed.error.isEmpty()) out.diagnostics << SqlEditorDiagnostic{start, start + int(line.size()), parsed.error};
      start += int(line.size()) + 1;
    }
    out.status = out.diagnostics.isEmpty() ? "Redis 命令已做本地检查 · 参数与键类型由服务器校验"
                                          : QString("Redis 命令有 %1 处错误").arg(out.diagnostics.size());
    return out;
  }
  // Some Qt encoders drop isolated UTF-16 surrogates. Replace them in the
  // check snapshot so source positions retain one UTF-16 unit per character.
  QString normalized = text;
  for (int i = 0; i < normalized.size(); ++i) {
    if (normalized.at(i).isHighSurrogate()) {
      if (i + 1 < normalized.size() && normalized.at(i + 1).isLowSurrogate())
        ++i;
      else
        normalized[i] = QChar(0xfffd);
    } else if (normalized.at(i).isLowSurrogate())
      normalized[i] = QChar(0xfffd);
  }
  auto bytes = normalized.toUtf8();
  if (bytes.size() > maxBytes) {
    out.status = "超出实时检查上限（1 MiB）";
    return out;
  }
  if (text.trimmed().isEmpty()) {
    out.status = "等待 SQL";
    return out;
  }
  try {
    LanguageOptions options;
    options.calciteExtensions=calciteExtensions;
    options.extension=navidog::compilerExtensionFor(kind);
    options.limits.sourceBytes = maxBytes;
    options.limits.tokens = 40000;
    options.limits.nodes = 20000;
    options.limits.nesting = 64;
    options.limits.statements = 128;
    options.limits.diagnostics = 32;
    const auto deadline =
        std::chrono::steady_clock::now() + std::chrono::milliseconds(150);
    options.shouldStop = [cancel, deadline] {
      return (cancel && cancel->load()) ||
             std::chrono::steady_clock::now() >= deadline;
    };
    auto c = compile(compilerDialect(kind),
                     std::string(bytes.constData(), bytes.size()), options,
                     nullptr, {database.toStdString(), schema.toStdString()});
    const auto lexicalKind =
        kind == navidog::DatabaseKind::PostgreSQL
            ? navidog::sql::Dialect::PostgreSQL
        : navidog::usesOracleSql(kind) ? navidog::sql::Dialect::Oracle
        : kind == navidog::DatabaseKind::SQLServer
            ? navidog::sql::Dialect::SQLServer
        : kind == navidog::DatabaseKind::SQLite ? navidog::sql::Dialect::SQLite
                                               : navidog::sql::Dialect::MySQL;
    const auto source = std::string(bytes.constData(), bytes.size());
    const auto originalTokens = navidog::sql::lex(source, lexicalKind);
    auto catalogSource = source;
    std::vector<SourceRange> repairedPunctuation;
    // This is an editor lint, not a claim that every dialect prohibits Unicode
    // in identifiers. Quoted names, strings and comments are deliberately
    // opaque.
    for (const auto &token : originalTokens.tokens) {
      if (options.shouldStop())
        break;
      if (token.kind != navidog::sql::TokenKind::Word)
        continue;
      std::size_t offset = token.begin;
      while (offset < token.end) {
        if ((offset & 4095u) == 0 && options.shouldStop())
          break;
        const auto lead = static_cast<unsigned char>(source[offset]);
        const std::size_t n = lead < 0x80   ? 1
                              : lead < 0xe0 ? 2
                              : lead < 0xf0 ? 3
                                            : 4;
        if (offset + n > token.end)
          break;
        ushort code = 0;
        if (n == 3)
          code = ushort(
              ((lead & 15) << 12) |
              ((static_cast<unsigned char>(source[offset + 1]) & 63) << 6) |
              (static_cast<unsigned char>(source[offset + 2]) & 63));
        const QChar ch(code);
        QChar ascii;
        switch (ch.unicode()) {
        case 0xff0c:
        case 0x3001:
          ascii = ',';
          break;
        case 0xff1b:
          ascii = ';';
          break;
        case 0xff08:
          ascii = '(';
          break;
        case 0xff09:
          ascii = ')';
          break;
        case 0xff0e:
          ascii = '.';
          break;
        case 0xff1a:
          ascii = ':';
          break;
        case 0xff1d:
          ascii = '=';
          break;
        case 0xff0b:
          ascii = '+';
          break;
        case 0xff0d:
          ascii = '-';
          break;
        case 0xff0a:
          ascii = '*';
          break;
        case 0xff0f:
          ascii = '/';
          break;
        default:
          break;
        }
        if (!ascii.isNull()) {
          const SourceRange range{offset, offset + n};
          if (c->diagnostics.size() < options.limits.diagnostics)
            c->diagnostics.push_back(
                {DiagnosticCode::ExpectedToken, range,
                 QString("疑似中文/"
                         "全角标点“%1”（U+%2），若为分隔符或运算符请改为英文“%"
                         "3”；若属于名称，请使用当前方言的引用符包裹名称。")
                     .arg(QString(ch))
                     .arg(QString::number(uint(ch.unicode()), 16)
                              .rightJustified(4, QChar('0'))
                              .toUpper())
                     .arg(QString(ascii))
                     .toStdString()});
          // Only comma recovery is needed for the projection-list case. Keep
          // byte offsets stable and never modify the editor or executed SQL.
          if (ascii == ',') {
            catalogSource[offset] = ',';
            for (std::size_t j = 1; j < n; ++j)
              catalogSource[offset + j] = ' ';
            repairedPunctuation.push_back(range);
          }
        }
        offset += n;
      }
    }
    bool metadataUsed = false, metadataTableOnly = false;
    bool procedural = false, declarations = false;
    std::vector<QString> words;
    for (const auto &token : originalTokens.tokens) {
      if (token.kind == navidog::sql::TokenKind::Comment)
        continue;
      if (token.kind == navidog::sql::TokenKind::Word)
        words.push_back(QString::fromUtf8(source.data() + token.begin,
                                          int(token.end - token.begin))
                            .toUpper());
      else
        words.push_back({});
    }
    for (const auto &word : words)
      if (word == "CREATE" || word == "ALTER" || word == "DROP" ||
          word == "RENAME")
        declarations = true;
    const auto usesScriptContext = [&] { return procedural || declarations; };
    for (std::size_t i = 0; i < words.size(); ++i)
      if (words[i] == "CREATE") {
        auto j = i + 1;
        if (j + 1 < words.size() && words[j] == "OR" &&
            words[j + 1] == "REPLACE")
          j += 2;
        if (j < words.size() &&
            (words[j] == "FUNCTION" || words[j] == "PROCEDURE" ||
             words[j] == "PROC")) {
          procedural = true;
          break;
        }
      }
    const bool usableCatalog = snapshot && snapshot->driver == kind &&
                               snapshot->database == database &&
                               snapshot->schema == schema && snapshot->usable();
    if (usesScriptContext()) {
      navidog::MetadataCatalog catalog(snapshot);
      auto checked = checkSqlScript(compilerDialect(kind), source, options,
                                    usableCatalog ? &catalog : nullptr,
                                    {database.toStdString(),
                                     schema.toStdString(),
                                     compilerDialect(kind),
                                     {}});
      for (const auto &d : c->diagnostics)
        if (d.message.rfind("疑似中文", 0) == 0)
          checked.diagnostics.push_back(d);
      c->diagnostics = std::move(checked.diagnostics);
      c->diagnostics.insert(c->diagnostics.end(), checked.runtimeDiagnostics.begin(),
                            checked.runtimeDiagnostics.end());
      metadataUsed = checked.metadataUsed;
    }
    std::vector<bool> catalogMarks(c->diagnostics.size(), false);
    std::vector<bool> recoveredMarks(c->diagnostics.size(), false);
    if (usesScriptContext() && usableCatalog)
      for (std::size_t i = 0; i < c->diagnostics.size(); ++i) {
        const auto &d = c->diagnostics[i];
        catalogMarks[i] =
            d.code == DiagnosticCode::UnknownTable ||
            d.code == DiagnosticCode::UnknownRoutine ||
            (d.code == DiagnosticCode::UnknownColumn &&
             d.message.find("procedural variable") == std::string::npos);
      }
    if (!usesScriptContext() && snapshot && snapshot->driver == kind &&
        snapshot->database == database && snapshot->schema == schema &&
        snapshot->usable()) {
      const auto compilerKind = compilerDialect(kind);
      auto spans = navidog::sql::splitStatements(source, lexicalKind);
      bool safe = spans.size() <= 128;
      std::vector<bool> checkable;
      bool unknownContext = false;
      for (const auto &span : spans) {
        if (span.compound) {
          safe = false;
          break;
        }
        const auto part =
            std::string_view(source).substr(span.begin, span.end - span.begin);
        auto lex = navidog::sql::lex(part, lexicalKind);
        bool first = true;
        bool dml = false;
        for (const auto &token : lex.tokens)
          if (token.kind == navidog::sql::TokenKind::Word) {
            auto word = QString::fromUtf8(part.data() + token.begin,
                                          int(token.end - token.begin))
                            .toUpper();
            if (first) {
              dml = word == "SELECT" || word == "WITH" || word == "INSERT" ||
                    word == "UPDATE" || word == "DELETE" || word == "MERGE";
              // Known context-changing commands still invalidate the whole
              // batch, including reads before a later CREATE/SET/transaction.
              if (QStringList{"SET", "USE", "RENAME", "TRUNCATE", "COMMENT",
                              "GRANT", "REVOKE", "DISCARD", "DO", "BEGIN",
                              "START", "COMMIT", "ROLLBACK", "SAVEPOINT",
                              "RELEASE", "END", "LOCK", "UNLOCK"}
                      .contains(word))
                safe = false;
            }
            first = false;
            if (word == "SET_CONFIG" || word == "CREATE" || word == "ALTER" ||
                word == "DROP" || word == "CALL" || word == "EXECUTE" ||
                word == "EXEC" || word == "DECLARE")
              safe = false;
          }
        // A broken/unknown statement is a barrier for later statements, not
        // a reason to discard catalog checks for a complete preceding SELECT.
        checkable.push_back(!unknownContext && dml && lex.diagnostics.empty());
        if (!dml || !lex.diagnostics.empty())
          unknownContext = true;
      }
      if (safe) {
        navidog::MetadataCatalog catalog(snapshot);
        for (std::size_t i = 0; i < spans.size(); ++i) {
          if (!checkable[i])
            continue;
          const auto &span = spans[i];
          const bool mayRecover = std::any_of(
              repairedPunctuation.begin(), repairedPunctuation.end(),
              [&](const auto &range) {
                return range.begin >= span.begin && range.begin < span.end;
              });
          bool recovered = false;
          auto checked =
              compile(compilerKind,
                      std::string(navidog::sql::statementSource(source, span,
                                                               lexicalKind)),
                      options, &catalog,
                      {database.toStdString(), schema.toStdString()});
          if (!checked->syntaxValid && mayRecover) {
            checked = compile(compilerKind,
                              std::string(navidog::sql::statementSource(
                                  catalogSource, span, lexicalKind)),
                              options, &catalog,
                              {database.toStdString(), schema.toStdString()});
            recovered = true;
          }
          if (!checked->syntaxValid)
            continue;
          metadataUsed = true;
          metadataTableOnly |= recovered;
          for (auto diagnostic : checked->diagnostics) {
            if (recovered && diagnostic.code != DiagnosticCode::UnknownTable)
              continue;
            if (diagnostic.code != DiagnosticCode::UnknownTable &&
                diagnostic.code != DiagnosticCode::UnknownColumn &&
                diagnostic.code != DiagnosticCode::AmbiguousColumn &&
                diagnostic.code != DiagnosticCode::UnknownRoutine &&
                diagnostic.code != DiagnosticCode::RoutineArgumentCount)
              continue;
            diagnostic.range.begin += span.begin;
            diagnostic.range.end += span.begin;
            // A recovered relation name must retain the original token's exact
            // boundaries. Do not turn FROM `odd，name` / unquoted odd，name
            // into spurious missing-table errors for odd and name.
            if (recovered && diagnostic.code == DiagnosticCode::UnknownTable) {
              const auto nameToken = [](const auto &token) {
                return token.kind == navidog::sql::TokenKind::Word ||
                       token.kind == navidog::sql::TokenKind::Identifier;
              };
              const bool begin = std::any_of(
                  originalTokens.tokens.begin(), originalTokens.tokens.end(),
                  [&](const auto &token) {
                    return nameToken(token) &&
                           token.begin == diagnostic.range.begin;
                  });
              const bool end = std::any_of(
                  originalTokens.tokens.begin(), originalTokens.tokens.end(),
                  [&](const auto &token) {
                    return nameToken(token) &&
                           token.end == diagnostic.range.end;
                  });
              if (!begin || !end)
                continue;
            }
            const bool duplicate = std::any_of(
                c->diagnostics.begin(), c->diagnostics.end(),
                [&](const auto &old) {
                  return old.code == diagnostic.code &&
                         old.range.begin == diagnostic.range.begin &&
                         old.range.end == diagnostic.range.end &&
                         old.message == diagnostic.message;
                });
            if (!duplicate &&
                c->diagnostics.size() < options.limits.diagnostics) {
              c->diagnostics.push_back(std::move(diagnostic));
              catalogMarks.push_back(true);
              recoveredMarks.push_back(recovered);
            }
          }
        }
      }
    }
    // One linear UTF-8 -> UTF-16 boundary map, never a prefix scan for every
    // mark.
    QVector<int> offsets(bytes.size() + 1);
    int byte = 0, position = 0;
    while (byte < bytes.size()) {
      const auto lead = static_cast<unsigned char>(bytes[byte]);
      const int count = lead < 0x80 ? 1 : lead < 0xe0 ? 2 : lead < 0xf0 ? 3 : 4;
      for (int j = 0; j < count && byte < bytes.size(); ++j)
        offsets[byte++] = position;
      position += count == 4 ? 2 : 1;
      offsets[byte] = position;
    }
    bool limited = false;
    int errors=0,unsupported=0,metadataIssues=0,cursorWarnings=0,preparedWarnings=0;
    for (std::size_t index = 0; index < c->diagnostics.size(); ++index) {
      const auto &d = c->diagnostics[index];
      if (d.code == DiagnosticCode::Stopped ||
          d.code == DiagnosticCode::ResourceLimit) {
        limited = true;
        continue;
      }
      SqlEditorDiagnostic mark;
      mark.begin =
          offsets[int(std::min<std::size_t>(d.range.begin, bytes.size()))];
      mark.end = offsets[int(std::min<std::size_t>(d.range.end, bytes.size()))];
      mark.error = d.code != DiagnosticCode::UnsupportedSyntax &&
                   d.code != DiagnosticCode::CursorRuntime &&
                   d.code != DiagnosticCode::PreparedRuntime;
      mark.metadata = catalogMarks[index];
      if (mark.metadata)
        mark.error = false;

      mark.message = message(d);
      if (d.code == DiagnosticCode::UnknownTable)
        mark.message += "：" + QString::fromUtf8(
                                   bytes.mid(int(d.range.begin),
                                             int(d.range.end - d.range.begin)))
                                   .left(128);
      if (mark.metadata)
        mark.message = "当前元数据快照：" + mark.message +
                       "\n对象可能不存在、无目录权限或已在服务器改变。后台会自"
                       "动刷新复查；临时对象不在共享目录内。";
      if (recoveredMarks[index])
        mark.message += "\n本条含中文逗号，仅用分析副本辅助核对原始表名；字段未"
                        "校验，原文和执行 SQL 未修改。";

      if (mark.end == mark.begin) {
        // EOF/missing token has no glyph: underline the nearest complete code
        // point.
        if (mark.begin < text.size())
          mark.end =
              mark.begin + (text.at(mark.begin).isHighSurrogate() ? 2 : 1);
        else if (mark.begin > 0) {
          --mark.begin;
          if (text.at(mark.begin).isLowSurrogate() && mark.begin > 0)
            --mark.begin;
        }
      }
      if (mark.end > mark.begin)
        out.diagnostics << mark;
      if (mark.metadata)
        ++metadataIssues;
      else if (mark.error)
        ++errors;
      else if (d.code == DiagnosticCode::CursorRuntime)
        ++cursorWarnings;
      else if (d.code == DiagnosticCode::PreparedRuntime)
        ++preparedWarnings;
      else
        ++unsupported;
    }
    if (errors)
      out.status = QString("%1 处本地错误").arg(errors);
    else if (metadataIssues)
      out.status = QString("%1 处元数据提示").arg(metadataIssues);
    else if (unsupported)
      out.status = "部分语法未覆盖";
    else if (cursorWarnings)
      out.status = QString("%1 处游标运行提示").arg(cursorWarnings);
    else if (preparedWarnings)
      out.status = QString("%1 处预处理语句运行提示").arg(preparedWarnings);
    else
      out.status = "已覆盖语法未发现问题";
    if (errors && metadataIssues)
      out.status += QString(" · %1 处对象提示").arg(metadataIssues);
    if (cursorWarnings && (errors || metadataIssues || unsupported))
      out.status += QString(" · %1 处游标运行提示").arg(cursorWarnings);
    if (preparedWarnings && (errors || metadataIssues || unsupported || cursorWarnings))
      out.status += QString(" · %1 处预处理语句运行提示").arg(preparedWarnings);
    if (limited)
      out.status += " · 检查未完成（上限）";
    if (!snapshot)
      out.status += " · 元数据未同步（表/字段未校验）";
    else if (!snapshot->verified)
      out.status += " · 表/字段未校验（缓存待验证）";
    else if (!snapshot->usable())
      out.status += " · 表/字段未校验（元数据已失效）";
    else if (metadataUsed)
      out.status += snapshot->tablesComplete && snapshot->columnsComplete &&
                            snapshot->routinesComplete
                        ? " · 元数据有效"
                        : " · 元数据部分完成";
    else
      out.status += procedural     ? " · 过程作用域已检查"
                    : declarations ? " · 脚本声明上下文已检查"
                                   : " · 本批仅语法检查";
    if (metadataTableOnly)
      out.status += " · 部分语句仅核对表名（中文标点待修正）";

  } catch (const std::exception &) {
    out.status = "本地检查未完成";
  }
  return out;
}
SqlDiagnosticController::SqlDiagnosticController(
    std::function<QString()> source, QObject *parent)
    : QObject(parent), source_(std::move(source)) {
  qRegisterMetaType<SqlCheckResult>();
  timer_.setSingleShot(true);
  timer_.setInterval(350);
  connect(&timer_, &QTimer::timeout, this, &SqlDiagnosticController::launch);
  expiry_.setSingleShot(true);
  connect(&expiry_, &QTimer::timeout, this,
          &SqlDiagnosticController::invalidate);
}
SqlDiagnosticController::~SqlDiagnosticController() {
  if (cancel_)
    cancel_->store(true);
}
void SqlDiagnosticController::setContext(navidog::DatabaseKind kind,
                                         const QString &database,
                                         const QString &schema,
                                         bool calciteExtensions) {
  const bool sameScope=kind_==kind && database_==database && schema_==schema;
  if (sameScope && calciteExtensions_==calciteExtensions)
    return;
  if (!sameScope) {
    snapshot_.reset();
    expiry_.stop();
  }
  kind_ = kind;
  database_ = database;
  schema_ = schema;
  calciteExtensions_=calciteExtensions;
  invalidate();
}
void SqlDiagnosticController::setSuspended(bool suspended) {
  if (suspended_ == suspended)
    return;
  suspended_ = suspended;
  invalidate();
}
void SqlDiagnosticController::invalidate() {
  ++revision_;
  if (cancel_)
    cancel_->store(true);
  if (kind_ == navidog::DatabaseKind::Dameng) {
    timer_.stop();
    emit updated({{}, "达梦 SQL 由服务器校验"});
    return;
  }
  if (suspended_) {
    timer_.stop();
    emit updated({{}, "大文件模式"});
    return;
  }
  emit updated({{},
                kind_ == navidog::DatabaseKind::Unknown ? "未选择检查方言"
                                                       : "等待检查…"});
  if (kind_ == navidog::DatabaseKind::Unknown)
    timer_.stop();
  else
    timer_.start(350);
}
void SqlDiagnosticController::launch() {
  if (suspended_ || running_ || kind_ == navidog::DatabaseKind::Unknown || kind_ == navidog::DatabaseKind::Dameng)
    return;
  auto *task = new CheckTask;
  task->source = source_();
  task->kind = kind_;
  task->calciteExtensions=calciteExtensions_;
  task->database = database_;
  task->schema = schema_;
  task->revision = revision_;
  task->cancel = std::make_shared<std::atomic_bool>(false);
  task->snapshot = snapshot_;
  connect(
      task, &CheckTask::checked, this,
      [this](quint64 revision, const SqlCheckResult &result) {
        running_ = false;
        if (revision == revision_)
          emit updated(result);
        else if (!suspended_ && !timer_.isActive() && kind_ != navidog::DatabaseKind::Unknown && kind_ != navidog::DatabaseKind::Dameng)
          timer_.start(0);
      },
      Qt::QueuedConnection);
  // Never queue unbounded source copies; retry after a slot becomes available.
  cancel_ = task->cancel;
  running_ = true;
  if (!checkPool().tryStart(task)) {
    running_ = false;
    delete task;
    timer_.start(100);
  }
}
void SqlDiagnosticController::setMetadataSnapshot(
    navidog::MetadataSnapshotPtr snapshot) {
  if (snapshot_ == snapshot)
    return;
  snapshot_ = std::move(snapshot);
  expiry_.stop();
  if (snapshot_ && snapshot_->usable())
    expiry_.start(int(std::max<qint64>(
        1, snapshot_->refreshedMs + navidog::MetadataSnapshot::ttlMs -
               QDateTime::currentMSecsSinceEpoch())));
  invalidate();
}
#include "editor_diagnostics.moc"
