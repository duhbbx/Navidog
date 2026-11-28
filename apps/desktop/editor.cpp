#include "navidog/database_sql.hpp"
#include "editor.hpp"
#include "components.hpp"
#include "navidog/sql.hpp"
#include "navidog/operation_timing.hpp"
#include <QAbstractItemView>
#include <QFontDatabase>
#include <QHeaderView>
#include <QHelpEvent>
#include <QInputMethodEvent>
#include <QKeyEvent>
#include <QPainter>
#include <QPainterPath>
#include <QRegularExpression>
#include <QScrollBar>
#include <QScopedValueRollback>
#include <QSignalBlocker>
#include <QStandardItemModel>
#include <QSyntaxHighlighter>
#include <QTextBlock>
#include <QTextLayout>
#include <QToolTip>
#include <QTreeView>
#include <unordered_set>
#include <utility>

namespace {
class Gutter final : public QWidget {
  SqlEditor *editor_;

public:
  explicit Gutter(SqlEditor *editor) : QWidget(editor), editor_(editor) {}
  QSize sizeHint() const override { return {editor_->gutterWidth(), 0}; }
  void paintEvent(QPaintEvent *event) override { editor_->paintGutter(event); }
};
navidog::sql::Dialect displayDialect(navidog::sql::compiler::Dialect dialect) {
  namespace sc = navidog::sql::compiler;
  using View = navidog::sql::Dialect;
  switch (dialect) {
  case sc::Dialect::MySQL:
  case sc::Dialect::MariaDB:
  case sc::Dialect::TiDB:
  case sc::Dialect::OceanBase: return View::MySQL;
  case sc::Dialect::Oracle: return View::Oracle;
  case sc::Dialect::SQLServer: return View::SQLServer;
  case sc::Dialect::SQLite: return View::SQLite;
  case sc::Dialect::PostgreSQL: return View::PostgreSQL;
  }
  return View::PostgreSQL;
}
navidog::sql::compiler::Dialect displayDialect(navidog::DatabaseKind kind) {
  return navidog::compilerDialectFor(kind).value_or(navidog::sql::compiler::Dialect::PostgreSQL);
}
void setSqlTextStyle(QPlainTextEdit* view) {
  view->setProperty("sqlTextViewport", true);
  view->document()->setDocumentMargin(0);
  QFont font = QFontDatabase::systemFont(QFontDatabase::FixedFont);
  font.setStyleHint(QFont::Monospace);
  font.setPointSize(12);
  view->setFont(font);
  view->setTabStopDistance(view->fontMetrics().horizontalAdvance(' ') * 4);
  view->setLineWrapMode(QPlainTextEdit::NoWrap);
}
} // namespace

namespace {
template <typename Apply>
bool highlightSqlBlock(const QString &text, navidog::sql::Dialect dialect,
                       bool inComment, Apply apply) {
    int start = 0;
    if (inComment) {
      auto end = text.indexOf("*/");
      if (end < 0) {
        apply(0, text.size(), QColor("#8997a3"));
        return true;
      }
      start = end + 2;
      apply(0, start, QColor("#8997a3"));
    }
    const auto bytes = text.mid(start).toUtf8();
    const auto tokens =
        navidog::sql::lex(std::string_view(bytes.constData(), bytes.size()),
                         dialect);
    static const std::unordered_set<std::string> words = {
        "SELECT", "FROM",   "WHERE",   "JOIN",   "ON",    "AS",     "LEFT",
        "RIGHT",  "INNER",  "OUTER",   "GROUP",  "BY",    "HAVING", "ORDER",
        "DESC",   "ASC",    "LIMIT",   "OFFSET", "AND",   "OR",     "NOT",
        "NULL",   "IS",     "IN",      "WITH",   "UNION", "ALL",    "DISTINCT",
        "INSERT", "INTO",   "VALUES",  "UPDATE", "SET",   "DELETE", "CREATE",
        "TABLE",  "DROP",   "ALTER",   "CASE",   "WHEN",  "THEN",   "ELSE",
        "END",    "COUNT",  "SUM",     "ROUND",  "AVG",   "MIN",    "MAX",
        "BEGIN",  "COMMIT", "ROLLBACK", "DATABASE", "SCHEMA", "USER",
        "INDEX", "VIEW", "MATERIALIZED", "TRIGGER", "FUNCTION", "PROCEDURE",
        "PACKAGE", "DECLARE", "RETURN", "RETURNS", "LANGUAGE", "EXECUTE",
        "CALL", "IF", "EXISTS", "PRIMARY", "FOREIGN", "KEY", "REFERENCES",
        "DEFAULT", "CHECK", "CONSTRAINT", "UNIQUE", "COLLATE", "OWNER",
        "CHARACTER", "CHARSET", "ENGINE", "TABLESPACE", "ADD", "COLUMN",
        "RENAME", "CASCADE", "RESTRICT", "OVER", "PARTITION", "WINDOW",
        "RETURNING", "OUTPUT", "TOP", "FETCH", "NEXT", "ONLY", "CONFLICT",
        "REPLACE", "GRANT", "REVOKE", "INTEGER", "INT", "BIGINT", "VARCHAR",
        "VARCHAR2", "TEXT", "DECIMAL", "NUMERIC", "BOOLEAN", "TIMESTAMP"};
    std::size_t previousEnd = 0;
    int position = start;
    for (auto token : tokens.tokens) {
      position += QString::fromUtf8(bytes.constData() + previousEnd,
                                    token.begin - previousEnd).size();
      const auto spelling = QString::fromUtf8(bytes.constData() + token.begin,
                                              token.end - token.begin);
      QColor color;
      if (token.kind == navidog::sql::TokenKind::Comment)
        color = QColor("#8997a3");
      else if (token.kind == navidog::sql::TokenKind::String)
        color = QColor("#247867");
      else if (token.kind == navidog::sql::TokenKind::Number)
        color = QColor("#a76932");
      else if (token.kind == navidog::sql::TokenKind::Word) {
        const auto word = spelling.toUpper().toStdString();
        if (words.count(word) || (dialect == navidog::sql::Dialect::MySQL &&
                                  (word == "DIV" || word == "MOD" || word == "FOR" ||
                                   word == "LOCK" || word == "NOWAIT" || word == "LOCKED")))
          color = QColor("#315bb0");
      }
      if (color.isValid())
        apply(position, spelling.size(), color);
      position += spelling.size();
      previousEnd = token.end;
    }
    return std::any_of(tokens.diagnostics.begin(), tokens.diagnostics.end(),
                       [](const auto &diagnostic) {
                         return diagnostic.message.find("注释") != std::string::npos;
                       });
}
}

class SqlHighlighter final : public QSyntaxHighlighter {
public:
  explicit SqlHighlighter(QTextDocument *document)
      : QSyntaxHighlighter(document) {}
  void setDialect(navidog::sql::compiler::Dialect dialect) {
    const auto next = displayDialect(dialect);
    if (next == dialect_)
      return;
    dialect_ = next;
    if (document())
      rehighlight();
  }

  void setCommandMode(navidog::DatabaseKind kind) {
    if (commandKind_ == kind) return;
    commandKind_ = kind;
    rehighlight();
  }

protected:
  void highlightBlock(const QString &text) override {
    if (commandKind_ == navidog::DatabaseKind::MongoDb) {
      static const QRegularExpression strings(R"re("(?:[^"\\]|\\.)*")re");
      auto matches = strings.globalMatch(text);
      while (matches.hasNext()) {
        const auto match = matches.next();
        const bool key = text.mid(match.capturedEnd()).trimmed().startsWith(':');
        setFormat(match.capturedStart(), match.capturedLength(), QColor(key ? "#315bb0" : "#247867"));
      }
      setCurrentBlockState(0);
      return;
    }
    if (commandKind_ == navidog::DatabaseKind::Redis) {
      int first = 0;
      while (first < text.size() && text.at(first).isSpace()) ++first;
      int end = first;
      while (end < text.size() && !text.at(end).isSpace()) ++end;
      if (first < end) {
        if (text.at(first) == '#') setFormat(first, text.size() - first, QColor("#8997a3"));
        else setFormat(first, end - first, QColor("#315bb0"));
      }
      setCurrentBlockState(0);
      return;
    }
    setCurrentBlockState(highlightSqlBlock(text, dialect_, previousBlockState() == 1,
        [this](int start, int length, const QColor &color) { setFormat(start, length, color); }));
  }

private:
  navidog::sql::Dialect dialect_ = navidog::sql::Dialect::PostgreSQL;
  navidog::DatabaseKind commandKind_ = navidog::DatabaseKind::Unknown;
};
SqlReadOnlyView::SqlReadOnlyView(QWidget* parent)
    : QPlainTextEdit(parent), highlighter_(new SqlHighlighter(document())) {
  setSqlTextStyle(this);
  setReadOnly(true);
}
void SqlReadOnlyView::setDialect(navidog::sql::compiler::Dialect dialect) {
  if (dialect_ == dialect && extension_ == navidog::sql::compiler::SqlExtension::None)
    return;
  extension_ = navidog::sql::compiler::SqlExtension::None;
  dialect_ = dialect;
  ++formatContext_;
  highlighter_->setDialect(dialect);
}
void SqlReadOnlyView::setDatabaseKind(navidog::DatabaseKind kind) {
  const auto extension = navidog::compilerExtensionFor(kind);
  if (dialect_ == displayDialect(kind) && extension_ == extension) return;
  setDialect(displayDialect(kind));
  extension_ = extension;
  ++formatContext_;
}
void SqlReadOnlyView::setSourceSql(const QString &sql) {
  if (sourceSql_ == sql)
    return;
  sourceSql_ = sql;
  setPlainText(sourceSql_);
}
SqlEditor::SqlEditor(QWidget *parent)
    : QPlainTextEdit(parent), highlighter_(new SqlHighlighter(document())),
      gutter_(new Gutter(this)),
      completionList_(new QTreeView(this)) {
  highlighter_->setParent(this);
  setObjectName("sqlEditor");
  gutter_->setObjectName("sqlLineNumbers");
  setSqlTextStyle(this);
  connect(this, &QPlainTextEdit::updateRequest, this,
          [this](QRect rect, int dy) {
            if (dy)
              gutter_->scroll(0, dy);
            else
              gutter_->update(0, rect.y(), gutter_->width(), rect.height());
            if (rect.contains(viewport()->rect()))
              updateMargins();
          });
  connect(this, &QPlainTextEdit::cursorPositionChanged, this,
          &SqlEditor::updateEditorSelections);
  diagnosticCard_ = new navidog::ui::DiagnosticCard(this);
  diagnosticController_ = new SqlDiagnosticController(
      [this] {
        return toPlainText();
      },
      this);
  connect(diagnosticController_, &SqlDiagnosticController::updated, this,
          [this](const SqlCheckResult &result) {
            localDiagnostics_ = result.diagnostics;
            diagnosticStatus_ = result.status;
            const auto sync = property("metadataSyncState").toString();
            if (!largeDocument_ && !sync.isEmpty())
              diagnosticStatus_ += " · " + sync;
            refreshDiagnostics();
          });
  connect(this, &QPlainTextEdit::textChanged, this, [this] { setLiveSafetyMarks({}); });
  completionController_ = new SqlCompletionController(
      [this] {
        return std::pair<QString, int>{document()->characterCount() > sqlSelectionLimit + 1
                                           ? QString{} : toPlainText(),
                                       textCursor().position()};
      },
      this);
  connect(this, &QPlainTextEdit::cursorPositionChanged, this, [this] {
    if (!largeDocument_)
      completionController_->invalidate();
  });
  connect(completionController_, &SqlCompletionController::cleared, this, [this] {
    completionRevision_ = -1;
    completionList_->hide();
  });

  // The list is an editor child. It can receive clicks on a candidate without
  // opening a Qt::Popup, which grabs mouse events for the entire window.
  auto *popup = completionList_;
  popup->setObjectName("sqlCompletionPopup");
  popup->setAccessibleName("SQL 补全");
  popup->setFocusPolicy(Qt::NoFocus);
  popup->setRootIsDecorated(false);
  popup->setUniformRowHeights(true);
  popup->setHeaderHidden(true);
  popup->setItemsExpandable(false);
  popup->setSelectionMode(QAbstractItemView::SingleSelection);
  popup->setSelectionBehavior(QAbstractItemView::SelectRows);
  popup->setEditTriggers(QAbstractItemView::NoEditTriggers);
  popup->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
  popup->header()->setStretchLastSection(true);
  popup->setStyleSheet("QTreeView#sqlCompletionPopup { background: white; border: 1px solid #a9bfd4; }");
  auto *model = new QStandardItemModel(0, 2, popup);
  popup->setModel(model);
  popup->hide();
  installEventFilter(this);
  connect(
      completionController_, &SqlCompletionController::ready, this,
      [this, popup, model](const EditorCompletionResult &result) {
        if (isReadOnly() || !isVisible() || !hasFocus())
          return;
        setProperty("sqlCompletionStatus", result.status);
        if (result.items.isEmpty()) {
          popup->hide();
          return;
        }
        model->setRowCount(result.items.size());
        static const QMap<QString, QString> labels = {
            {"table", "表"},      {"view", "视图"},      {"column", "字段"},
            {"alias", "别名"},    {"function", "函数"},  {"procedure", "过程"}, {"cursor", "游标"},
            {"variable", "变量"}, {"keyword", "关键字"}, {"snippet", "模板"},
            {"type", "类型"},     {"schema", "模式"},{"database","数据库"},{"index","索引"},
            {"sequence","序列"},{"trigger","触发器"},{"materializedView","物化视图"},
            {"synonym","同义词"},{"package","包"},{"event","事件"}};
        int row = 0;
        for (const auto &item : result.items) {
          auto *name = new QStandardItem(item.label);
          name->setData(item.insertText, Qt::UserRole);
          name->setData(item.begin, Qt::UserRole + 1);
          name->setData(item.end, Qt::UserRole + 2);
          name->setData(item.cursor, Qt::UserRole + 3);
          name->setData(item.selection, Qt::UserRole + 4);
          name->setToolTip(item.detail.toHtmlEscaped());
          auto *detail = new QStandardItem(labels.value(item.kind, item.kind) +
                                           (item.detail.isEmpty()?QString{}:" · " + item.detail));
          detail->setForeground(QColor("#778995"));
          model->setItem(row,0,name);
          model->setItem(row++,1,detail);
        }
        completionRevision_ = document()->revision();
        popup->setCurrentIndex(model->index(0, 0));
        popup->scrollToTop();
        diagnosticCard_->hide();
        showCompletionList();
      });
  connect(popup, &QTreeView::clicked, this, [this](const QModelIndex &index) {
    acceptCompletion(index.sibling(index.row(), 0));
  });
  // Typing can scroll the caret into view after the edit event. Hide any
  // previous list while scrolling, but keep the request for the current text.
  connect(verticalScrollBar(), &QScrollBar::valueChanged, completionList_, &QWidget::hide);
  connect(horizontalScrollBar(), &QScrollBar::valueChanged, completionList_, &QWidget::hide);
  observeDocument();
  updateMargins();
}
void SqlEditor::acceptCompletion(const QModelIndex &index) {
  if (!index.isValid()) return;
  if (isReadOnly() || document()->revision() != completionRevision_)
    return;
  const auto insert = index.data(Qt::UserRole).toString();
  const auto begin = index.data(Qt::UserRole + 1).toInt(),
             end = index.data(Qt::UserRole + 2).toInt(),
             offset = index.data(Qt::UserRole + 3).toInt(),
             selection = index.data(Qt::UserRole + 4).toInt();
  if (begin < 0 || end < begin ||
      end > document()->characterCount() - 1 || offset < 0 ||
      offset + selection > insert.size())
    return;
  auto cursor = textCursor();
  cursor.beginEditBlock();
  cursor.setPosition(begin);
  cursor.setPosition(end, QTextCursor::KeepAnchor);
  cursor.insertText(insert);
  cursor.endEditBlock();
  cursor.setPosition(begin + offset);
  cursor.setPosition(begin + offset + selection, QTextCursor::KeepAnchor);
  setTextCursor(cursor);
  dismissCompletion();
}

void SqlEditor::dismissCompletion() {
  completionController_->invalidate();
  completionList_->hide();
}

void SqlEditor::showCompletionList() {
  const auto area = viewport()->geometry().adjusted(1, 1, -1, -1);
  const auto caret = cursorRect().translated(viewport()->pos());
  const int gap = qMax(24, fontMetrics().height() + 12);
  const int rowHeight = qMax(fontMetrics().height(), completionList_->sizeHintForRow(0));
  const int frameHeight = 2 * completionList_->frameWidth();
  const int desiredHeight = qMin(7, completionList_->model()->rowCount()) * rowHeight + frameHeight;
  const int below = area.bottom() - caret.bottom() - gap;
  const int above = caret.top() - gap - area.top();
  const bool showBelow = below >= desiredHeight || below >= above;
  const int height = qMin(desiredHeight, showBelow ? below : above);
  if (!area.contains(caret.center()) || height < rowHeight + frameHeight) {
    completionList_->hide();
    return;
  }
  const int width = qMin(560, area.width());
  const int x = qBound(area.left(), caret.left(), area.right() - width + 1);
  const int y = showBelow ? caret.bottom() + gap + 1 : caret.top() - gap - height;
  completionList_->setGeometry(x, y, width, height);
  completionList_->setColumnWidth(0, qMin(220, width / 2));
  completionList_->show();
  completionList_->raise();
}

int SqlEditor::gutterWidth() const {
  return 28 + fontMetrics().horizontalAdvance('9') *
                  QString::number(gutterBlockCountHint_ ? gutterBlockCountHint_ : blockCount()).size();
}
void SqlEditor::updateMargins() {
  const int width = gutterWidth();
  if (viewportMargins().left() != width)
    setViewportMargins(width, 0, 0, 0);
  const auto area = viewport()->geometry();
  gutter_->setGeometry(area.left() - width, area.top(), width, area.height());
}
void SqlEditor::resizeEvent(QResizeEvent *event) {
  QPlainTextEdit::resizeEvent(event);
  updateMargins();
  if (completionList_->isVisible()) showCompletionList();
}
void SqlEditor::paintGutter(QPaintEvent *event) {
  QPainter painter(gutter_);
  painter.fillRect(event->rect(), QColor("#f8fafc"));
  painter.setPen(QColor("#9aa8b6"));
  painter.setFont(font());
  auto block = firstVisibleBlock();
  int top =
      qRound(blockBoundingGeometry(block).translated(contentOffset()).top());
  while (block.isValid() && top <= event->rect().bottom()) {
    const int height = qRound(blockBoundingRect(block).height());
    if (block.isVisible() && top + height >= event->rect().top()) {
      bool problem = false,note=false,safety=false;
      for (const auto &mark : diagnostics_)
        if (mark.end > block.position() && mark.begin < block.position()+block.length()) {
          problem |= mark.error || mark.metadata;note |= !mark.error && !mark.metadata;
          safety |= mark.safety;
        }
      painter.setPen(QColor(problem ? "#d00020" : note?"#a56309":"#9aa8b6"));
      if (problem || note) {
        painter.setBrush(QColor(problem?"#d00020":"#b57516"));
        const auto center = top + fontMetrics().height() / 2.0;
        if (safety && !problem) {
          const QPointF triangle[] = {{6, center - 4}, {2, center + 3}, {10, center + 3}};
          painter.drawConvexPolygon(triangle, 3);
        } else {
          painter.drawEllipse(QPointF(6, center), 2.5, 2.5);
        }
      }
      painter.drawText(0, top, gutter_->width() - 12, fontMetrics().height(),
                       Qt::AlignRight,
                       QString::number(block.blockNumber() + 1));
    }
    top += height;
    block = block.next();
  }
}
void SqlEditor::paintEvent(QPaintEvent *event) {
  if (largeDocument_)
    highlightVisibleSql();
  QPlainTextEdit::paintEvent(event);
  if (diagnostics_.isEmpty())
    return;
  QPainter painter(viewport());
  painter.setClipRect(event->rect());
  painter.setRenderHint(QPainter::Antialiasing);
  painter.setPen(
      QPen(QColor("#d00020"), 2.0, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
  for (auto block = firstVisibleBlock(); block.isValid();
       block = block.next()) {
    const auto geometry =
        blockBoundingGeometry(block).translated(contentOffset());
    if (geometry.top() > viewport()->height())
      break;
    if (!block.isVisible() || geometry.bottom() < 0 || !block.layout())
      continue;
    for (const auto &mark : diagnostics_) {
      if (mark.end <= block.position() ||
          mark.begin >= block.position() + block.length())
        continue;
      const bool severe=mark.error || mark.metadata;
      QPen underline(QColor(severe?"#d00020":"#a56309"),severe?2.0:2.6,Qt::SolidLine,Qt::RoundCap,Qt::RoundJoin);
      if(!severe)underline.setDashPattern({2.0,0.8});
      painter.setPen(underline);
      for (int i = 0; i < block.layout()->lineCount(); ++i) {
        const auto line = block.layout()->lineAt(i);
        const int begin = qMax(mark.begin - block.position(), line.textStart());
        const int end = qMin(mark.end - block.position(),
                             line.textStart() + line.textLength());
        if (end <= begin)
          continue;
        const auto a = geometry.left() + line.cursorToX(begin),
                   b = geometry.left() + line.cursorToX(end);
        const auto left = qMax<qreal>(-3, qMin(a, b)),
                   right = qMin<qreal>(viewport()->width() + 3, qMax(a, b));
        if (right <= left)
          continue;
        const auto y = geometry.top() + line.y() +
                       qMin(line.height() - 2.3, line.ascent() + 2.3);
        QPainterPath wave;
        wave.moveTo(left, y);
        int direction = 1;
        for (qreal x = left; x < right; x += 4) {
          const auto next = qMin(x + 4, right);
          wave.quadTo((x + next) / 2, y + direction * 3.0, next, y);
          direction = -direction;
        }
        painter.drawPath(wave);
      }
    }
  }
}
void SqlEditor::keyPressEvent(QKeyEvent *event) {
  if (event->matches(QKeySequence::Find)) {
    showFind();
    return;
  }
  if (event->matches(QKeySequence::Replace)) {
    showFind(true);
    return;
  }
  if (event->matches(QKeySequence::FindNext)) {
    findNext();
    return;
  }
  if (event->matches(QKeySequence::FindPrevious)) {
    findNext(true);
    return;
  }
  if (isReadOnly()) {
    QPlainTextEdit::keyPressEvent(event);
    return;
  }
  if (event->key() == Qt::Key_Escape) {
    dismissCompletion();
    event->accept();
    return;
  }
  const auto commandModifiers = Qt::ControlModifier | Qt::AltModifier |
                                Qt::MetaModifier | Qt::ShiftModifier;
  if (completionList_->isVisible() && !(event->modifiers() & commandModifiers)) {
    const auto key = event->key();
    if (key == Qt::Key_Return || key == Qt::Key_Enter || key == Qt::Key_Tab) {
      const auto selected = completionList_->currentIndex();
      if (selected.isValid() && document()->revision() == completionRevision_) {
        acceptCompletion(selected);
        event->accept();
        return;
      }
      dismissCompletion();
    } else if (key == Qt::Key_Down || key == Qt::Key_Up ||
               key == Qt::Key_PageDown || key == Qt::Key_PageUp) {
      const int step = key == Qt::Key_PageDown || key == Qt::Key_PageUp ? 7 : 1;
      const int direction = key == Qt::Key_Down || key == Qt::Key_PageDown ? 1 : -1;
      const auto *model = completionList_->model();
      const int row = qBound(0, completionList_->currentIndex().row() + direction * step,
                             model->rowCount() - 1);
      const auto selected = model->index(row, 0);
      completionList_->setCurrentIndex(selected);
      completionList_->scrollTo(selected);
      event->accept();
      return;
    }
  }
  if (event->key() == Qt::Key_Space &&
      (event->modifiers() & Qt::ControlModifier)) {
    showCompletion();
    return;
  }
  if (event->key() == Qt::Key_Backtab ||
      (event->key() == Qt::Key_Tab &&
       event->modifiers().testFlag(Qt::ShiftModifier))) {
    indentLines(true);
    return;
  }
  if (event->key() == Qt::Key_Tab) {
    if (textCursor().hasSelection())
      indentLines();
    else
      insertPlainText("    ");
    return;
  }
  QPlainTextEdit::keyPressEvent(event);
  if (!(event->modifiers() &
        (Qt::ControlModifier | Qt::AltModifier | Qt::MetaModifier)))
    requestCompletionForText(event->text());
}
void SqlEditor::inputMethodEvent(QInputMethodEvent *event) {
  const auto committed = event->commitString();
  QPlainTextEdit::inputMethodEvent(event);
  requestCompletionForText(committed);
}
void SqlEditor::requestCompletionForText(const QString &inserted) {
  if (largeDocument_ || !completionEnabled_ || isReadOnly() || inserted.isEmpty() ||
      document()->characterCount() > sqlSelectionLimit + 1)
    return;
  if (inserted == "(" || inserted == ",") {
    completionController_->request(SqlCompletionController::Activation::ColumnList);
    return;
  }
  auto cursor = textCursor();
  cursor.select(QTextCursor::WordUnderCursor);
  const auto word = cursor.selectedText();
  const auto caret = textCursor();
  const auto spaceAfterToken = inserted == " " && caret.positionInBlock() >= 2 &&
      !caret.block().text().at(caret.positionInBlock() - 2).isSpace();
  if (inserted == "." ||
      (word.size() >= completionPrefix_ &&
       (inserted.back().isLetterOrNumber() || inserted.back() == '_' ||
        inserted.back() == '@' || inserted.back() == '$' || inserted.back() == '#')) ||
      spaceAfterToken)
    completionController_->request();
}

bool SqlEditor::eventFilter(QObject *watched, QEvent *event) {
  if (event->type() == QEvent::KeyPress || event->type() == QEvent::FocusOut ||
      event->type() == QEvent::Hide)
    diagnosticCard_->hide();
  if (event->type() == QEvent::FocusOut || event->type() == QEvent::WindowDeactivate)
    dismissCompletion();
  return QPlainTextEdit::eventFilter(watched, event);
}

void SqlEditor::contextMenuEvent(QContextMenuEvent *event) {
  dismissCompletion();
  navidog::ui::ContextMenu menu(this);
  menu.setObjectName("sqlEditorContextMenu");
  std::unique_ptr<QMenu> standard(createStandardContextMenu());
  menu.addActions(standard->actions());
  if (conversionEnabled_ || optimizationEnabled_) menu.addSeparator();
  if (conversionEnabled_) {
    auto *action=menu.addAction("SQL 转换…");
    action->setObjectName("editorConvertSql");
    connect(action,&QAction::triggered,this,[this]{
      QTimer::singleShot(0,this,[this]{emit conversionRequested();});
    });
  }
  if (optimizationEnabled_) {
    auto *action=menu.addAction("SQL 本地分析 / 优化…");
    action->setObjectName("editorOptimizeSql");
    connect(action,&QAction::triggered,this,[this]{
      QTimer::singleShot(0,this,[this]{emit optimizationRequested();});
    });
  }
  menu.execAt(event->globalPos(),this);
  event->accept();
}

void SqlEditor::setHighlightDialect(navidog::sql::compiler::Dialect dialect) {
  if (highlightDialect_ == dialect)
    return;
  highlightDialect_ = dialect;
  highlighter_->setDialect(dialect);
  highlightInvalidated_ = true;
  if (largeDocument_)
    viewport()->update();
  emit highlightDialectChanged();
}
void SqlEditor::setDiagnosticContext(navidog::DatabaseKind kind,
                                     const QString &database,
                                     const QString &schema,
                                     bool calciteExtensions) {
  extension_ = navidog::compilerExtensionFor(kind);
  setHighlightDialect(displayDialect(kind));
  highlighter_->setCommandMode(navidog::isCommandDatabase(kind) ? kind : navidog::DatabaseKind::Unknown);
  diagnosticController_->setContext(kind, database, schema, calciteExtensions);
  completionController_->setContext(kind, database, schema, calciteExtensions);
}
void SqlEditor::setQuerySafetyPolicy(const navidog::QuerySafetyPolicy& policy) {
  setLiveSafetyMarks({});
  diagnosticController_->setQuerySafetyPolicy(policy);
}
void SqlEditor::setLiveSafetyMarks(QVector<SqlEditorDiagnostic> marks) {
  if (liveSafetyMarks_.isEmpty() && marks.isEmpty()) return;
  liveSafetyMarks_ = std::move(marks);
  refreshDiagnostics();
}
void SqlEditor::refreshDiagnostics() {
  diagnostics_ = localDiagnostics_;
  for (const auto& mark : liveSafetyMarks_) {
    auto duplicate = std::find_if(diagnostics_.begin(), diagnostics_.end(), [&](const auto& old) {
      return old.safety && old.begin == mark.begin && old.end == mark.end;
    });
    if (duplicate == diagnostics_.end()) diagnostics_.push_back(mark);
    else *duplicate = mark;
  }
  const auto status = liveSafetyMarks_.isEmpty() ? diagnosticStatus_ :
      diagnosticStatus_ + QString(" · %1 处影响范围较大").arg(liveSafetyMarks_.size());
  QToolTip::hideText();
  diagnosticCard_->hide();
  scheduleEditorSelections();
  gutter_->update();
  setAccessibleDescription("SQL 本地检查：" + status);
  emit diagnosticsChanged();
  emit diagnosticStatusChanged(status);
}
void SqlEditor::scheduleEditorSelections() {
  if (selectionUpdatePending_)
    return;
  selectionUpdatePending_ = true;
  QTimer::singleShot(0, this, [this] {
    selectionUpdatePending_ = false;
    updateEditorSelections();
  });
}
void SqlEditor::updateEditorSelections() {
  QList<QTextEdit::ExtraSelection> selections;
  QTextEdit::ExtraSelection line;
  line.format.setBackground(QColor("#f3f7fc"));
  line.format.setProperty(QTextFormat::FullWidthSelection, true);
  line.cursor = textCursor();
  line.cursor.clearSelection();
  selections << line;
  for (const auto &diagnostic : diagnostics_) {
    QTextEdit::ExtraSelection mark;
    mark.cursor = QTextCursor(document());
    mark.cursor.setPosition(diagnostic.begin);
    mark.cursor.setPosition(diagnostic.end, QTextCursor::KeepAnchor);
    mark.format.setUnderlineStyle(diagnostic.error || diagnostic.metadata || diagnostic.safety
                                      ? QTextCharFormat::WaveUnderline
                                      : QTextCharFormat::DotLine);
    mark.format.setUnderlineColor(QColor(
        diagnostic.error || diagnostic.metadata ? "#d00020" : "#a56309"));
    mark.format.setBackground(QColor(diagnostic.error || diagnostic.metadata?"#fff0f2":"#fff4d6"));
    selections << mark;
  }
  setExtraSelections(selections);
}
QString SqlEditor::diagnosticAtPosition(int position) const {
  QStringList messages;
  for (const auto &mark : diagnostics_)
    if (mark.begin <= position && position < mark.end &&
        !messages.contains(mark.message)) {
      messages << (mark.error      ? "SQL 错误："
                   : mark.metadata ? "元数据提示："
                   : mark.safety   ? "写入风险："
                                   : "SQL 检查说明：") +
                      mark.message;
      if (messages.size() == 3)
        break;
    }
  return messages.join("\n\n");
}
bool SqlEditor::viewportEvent(QEvent *event) {
  if (event->type() == QEvent::ToolTip && !diagnostics_.isEmpty()) {
    const auto *help = static_cast<QHelpEvent *>(event);
    auto cursor = cursorForPosition(help->pos());
    const auto rect = cursorRect(cursor);
    int pos = cursor.position();
    if (help->pos().x() < rect.left() && pos > 0)
      --pos;
    const auto block = cursor.block();
    QTextCursor end(block);
    end.movePosition(QTextCursor::EndOfBlock);
    const auto endRect = cursorRect(end);
    const auto text = help->pos().y() >= rect.top() &&
                              help->pos().y() <= rect.bottom() &&
                              help->pos().x() <= endRect.right() + 2
                          ? diagnosticAtPosition(pos)
                          : QString{};
    QToolTip::hideText();
    if (!text.isEmpty() && !completionList_->isVisible()) {
      QStringList messages;bool error=false,metadata=false,safety=false;
      for(const auto &mark:diagnostics_)if(mark.begin<=pos && pos<mark.end) {
        error|=mark.error;metadata|=mark.metadata;safety|=mark.safety;
        if(!messages.contains(mark.message))messages<<mark.message.left(500);
        if(messages.size()==3)break;
      }
      const auto title=metadata?QString("对象与元数据检查"):error?QString("SQL 语法与语义错误"):safety?QString("写入风险"):QString("SQL 检查说明");
      const auto footer=QString("第 %1 行 · 第 %2 列%3").arg(cursor.blockNumber()+1).arg(cursor.positionInBlock()+1)
        .arg(metadata?" · 依据本地目录快照":error||safety?" · 执行前检查":" · 此语法可能尚未覆盖");
      diagnosticCard_->present(title,messages.join("\n\n"),footer,error,
        QRect(viewport()->mapToGlobal(rect.topLeft()),rect.size()));
    } else diagnosticCard_->hide();
    event->accept();
    return true;
  }
  if (event->type()==QEvent::Leave || event->type()==QEvent::Wheel || event->type()==QEvent::MouseButtonPress) {
    QToolTip::hideText();diagnosticCard_->hide();
    if (event->type() != QEvent::Leave) dismissCompletion();
  }
  return QPlainTextEdit::viewportEvent(event);
}

void SqlEditor::applyPreferences(int size, int indent, bool wrap,
                                 bool completion, int prefix) {
  completionEnabled_ = completion;
  completionPrefix_ = qBound(1, prefix, 4);
  completionController_->invalidate();
  auto f = font();
  f.setPointSize(qBound(8, size, 24));
  setFont(f);
  setStyleSheet(QString("QPlainTextEdit { font-family: Menlo, Consolas, "
                        "monospace; font-size: %1pt; }")
                    .arg(qBound(8, size, 24)));
  indentWidth_ = qBound(1, indent, 8);
  setTabStopDistance(fontMetrics().horizontalAdvance(' ') * indentWidth_);
  setLineWrapMode(wrap && !largeDocument_ ? QPlainTextEdit::WidgetWidth : QPlainTextEdit::NoWrap);
  updateMargins();
  viewport()->update();
}

void SqlEditor::setMetadataSyncState(const QString &state) {
  if (property("metadataSyncState").toString() == state)
    return;
  setProperty("metadataSyncState", state);
  if (!largeDocument_)
    diagnosticController_->invalidate();
}

void SqlEditor::setMetadataSnapshot(navidog::MetadataSnapshotPtr snapshot) {
  diagnosticController_->setMetadataSnapshot(snapshot);
  completionController_->setMetadataSnapshot(std::move(snapshot));
}

void SqlEditor::checkDocumentSize(int position) {
  if (!largeDocument_ && (document()->characterCount() > largeTextThreshold + 1 ||
      blockCount() > largeLineCount || document()->findBlock(position).length() > 512 * 1024 + 1))
    enableLargeDocument();
}

bool SqlEditor::hasBoundedSqlSelection() const {
  const auto cursor = textCursor();
  return cursor.selectionEnd() - cursor.selectionStart() > 0 &&
         cursor.selectionEnd() - cursor.selectionStart() <= sqlSelectionLimit;
}

void SqlEditor::enableLargeDocument() {
  if (largeDocument_)
    return;
  largeDocument_ = true;
  diagnosticController_->setSuspended(true);
  completionController_->invalidate();
  // Defer detaching after a live edit because Qt may still be notifying the
  // regular highlighter. Script loading detaches before inserting any text.
  QTimer::singleShot(0, this, [this] {
    if (highlighter_->document()) {
      highlighter_->setDocument(nullptr);
      viewport()->update();
    }
  });
  QTimer::singleShot(0, this, [this] { setLineWrapMode(QPlainTextEdit::NoWrap); });
  emit largeDocumentChanged(true);
}

namespace {
bool requiresLargeEditor(const QString &text) {
  if (text.size() > SqlEditor::largeTextThreshold)
    return true;
  int lines = 1;
  int start = 0;
  for (int i = 0; i < text.size(); ++i) {
    if (text.at(i) == '\n' || text.at(i) == QChar::ParagraphSeparator) {
      if (++lines > SqlEditor::largeLineCount)
        return true;
      start = i + 1;
    } else if (i - start >= 512 * 1024) {
      return true;
    }
  }
  return false;
}
}

void SqlEditor::setPlainText(const QString &text) {
  if (!largeDocument_ && requiresLargeEditor(text)) {
    enableLargeDocument();
    highlighter_->setDocument(nullptr);
  }
  QPlainTextEdit::setPlainText(text);
}

void SqlEditor::observeDocument() {
  connect(document(), &QTextDocument::blockCountChanged, this, &SqlEditor::updateMargins);
  connect(document(), &QTextDocument::contentsChange, this,
          [this](int position, int removed, int added) {
            if (!removed && !added)
              return;
            checkDocumentSize(position);
            if (largeDocument_) {
              highlightInvalidated_ = true;
              viewport()->update();
            }
          });
  connect(document(), &QTextDocument::contentsChanged, this, [this] {
    if (!largeDocument_) {
      diagnosticController_->invalidate();
      completionController_->invalidate();
    }
  });
}

void SqlEditor::installLargeDocument(QTextDocument *source) {
  const navidog::OperationTimer elapsed(navidog::TimedOperation::DisplaySqlFile);
  const int top = verticalScrollBar()->value();
  const int left = horizontalScrollBar()->value();
  const auto previousCursor = textCursor();
  const int anchor = previousCursor.anchor();
  const int position = previousCursor.position();
  enableLargeDocument();
  highlighter_->setDocument(nullptr);
  setLineWrapMode(QPlainTextEdit::NoWrap);
  highlightedBlocks_.clear();
  highlightInvalidated_ = true;
  source->setParent(this);
  // Qt invalidates cursors from the old document during setDocument().
  setExtraSelections({});
  {
    const QSignalBlocker blockEditorSignals(this);
    const QScopedValueRollback<int> gutterHint(gutterBlockCountHint_, source->blockCount());
    // Reserve horizontal space before attaching to avoid relayout when the
    // new line numbers and vertical scrollbar first become visible.
    setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOn);
    updateMargins();
    QPlainTextEdit::setDocument(source);
  }
  observeDocument();
  setUndoRedoEnabled(true);
  QTextCursor cursor(source);
  cursor.setPosition(qMin(anchor, source->characterCount() - 1));
  cursor.setPosition(qMin(position, source->characterCount() - 1), QTextCursor::KeepAnchor);
  setTextCursor(cursor);
  verticalScrollBar()->setValue(top);
  horizontalScrollBar()->setValue(left);
  updateEditorSelections();
  updateMargins();
  emit sourceDocumentReplaced();
}

void SqlEditor::highlightVisibleSql() {
  if (highlighter_->document())
    return;
  const auto first = firstVisibleBlock();
  auto last = first;
  if (!first.isValid())
    return;
  while (last.next().isValid() &&
         blockBoundingGeometry(last.next()).translated(contentOffset()).top() < viewport()->height())
    last = last.next();
  const int from = first.blockNumber(), through = last.blockNumber();
  if (!highlightInvalidated_ && highlightedBlocks_.size() == through - from + 1 &&
      highlightedBlocks_.contains(from) && highlightedBlocks_.contains(through))
    return;

  const bool wasInvalidated = std::exchange(highlightInvalidated_, false);
  QHash<int, HighlightedBlock> nowVisible;
  nowVisible.reserve(through - from + 1);
  auto start = first;
  bool comment = false;
  if (!wasInvalidated && highlightedBlocks_.contains(from)) {
    comment = highlightedBlocks_.value(from).commentBefore;
  } else if (!wasInvalidated && highlightedBlocks_.contains(from - 1)) {
    comment = highlightedBlocks_.value(from - 1).commentAfter;
  } else {
    for (int i = 0; i < 16 && start.previous().isValid(); ++i)
      start = start.previous();
  }
  const auto dialect = displayDialect(highlightDialect_);
  for (auto block = start; block.isValid() && block.blockNumber() <= through; block = block.next()) {
    const int line = block.blockNumber();
    const auto cached = highlightedBlocks_.constFind(line);
    if (!wasInvalidated && cached != highlightedBlocks_.cend() && cached->commentBefore == comment) {
      nowVisible.insert(line, *cached);
      comment = cached->commentAfter;
      continue;
    }
    QTextCursor prefix(block);
    prefix.setPosition(block.position() + qMin(highlightedLinePrefix, block.length() - 1),
                       QTextCursor::KeepAnchor);
    QVector<QTextLayout::FormatRange> formats;
    const bool visible = block.blockNumber() >= from && block.layout();
    const bool commentBefore = comment;
    comment = highlightSqlBlock(prefix.selectedText(), dialect, comment,
        [&formats, visible](int start, int length, const QColor &color) {
          if (visible) {
            QTextCharFormat format;
            format.setForeground(color);
            formats.append({start, length, format});
          }
        });
    if (visible) {
      block.layout()->setFormats(formats);
      nowVisible.insert(line, {block, commentBefore, comment});
    }
  }
  for (const auto &entry : highlightedBlocks_)
    if (entry.block.isValid() && entry.block.layout() && !nowVisible.contains(entry.block.blockNumber()))
      entry.block.layout()->clearFormats();
  highlightedBlocks_ = std::move(nowVisible);
}
