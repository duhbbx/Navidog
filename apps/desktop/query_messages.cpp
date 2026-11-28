#include "query_messages.hpp"
#include "editor.hpp"

#include <QContextMenuEvent>
#include <QDateTime>
#include <QFontDatabase>
#include <QKeyEvent>
#include <QMenu>
#include <QMouseEvent>
#include <QTextBlock>
#include <memory>

namespace {
struct Location : QTextBlockUserData {
    QueryMessageSource source;
    int begin, end;
    Location(const QueryMessageSource& s, int from, int to) : source(s), begin(from), end(to) {}
};
}

QueryMessages::QueryMessages(SqlEditor& editor) : editor_(editor) {
    setObjectName("queryMessages");
    setReadOnly(true);
    setMaximumBlockCount(2500);
    setFont(QFontDatabase::systemFont(QFontDatabase::FixedFont));
    setPlaceholderText("执行与预查语句、耗时和错误会显示在这里。");
    setAccessibleDescription("点击带有「定位 SQL」的标题，或将光标移到标题后按回车，定位到编辑器中的原语句。");
}

void QueryMessages::appendSql(const QString& heading, const QString& displaySql,
                              const QueryMessageSource& source, int begin, int end) {
    const bool linked = source.document && begin >= 0 && end > begin && end <= source.length;
    auto label = QDateTime::currentDateTime().toString("HH:mm:ss") + "  " + heading;
    if (linked) {
        if (source.document == editor_.document() && source.revision == editor_.document()->revision()) {
            const auto block = editor_.document()->findBlock(source.start + begin);
            if (block.isValid()) label += QString(" · 第 %1 行").arg(block.blockNumber() + 1);
        }
        label += " · 定位 SQL";
    }
    appendPlainText(label);
    const auto header = document()->lastBlock();
    if (linked) {
        auto block = header;
        block.setUserData(new Location(source, begin, end));
        QTextCursor cursor(block);
        cursor.movePosition(QTextCursor::EndOfBlock, QTextCursor::KeepAnchor);
        QTextCharFormat appearance;
        appearance.setForeground(palette().color(QPalette::Link));
        appearance.setFontUnderline(true);
        cursor.mergeCharFormat(appearance);
    }
    appendPlainText(displaySql);
}

bool QueryMessages::locate(const QTextBlock& block) {
    const auto* location = dynamic_cast<const Location*>(block.userData());
    if (!location) return false;
    auto* document = editor_.document();
    const int start = location->source.start + location->begin;
    const int end = location->source.start + location->end;
    if (location->source.document != document || location->source.revision != document->revision() ||
        start < 0 || end >= document->characterCount()) {
        if (statusChanged) statusChanged("SQL 编辑器内容已变化，请重新执行或在编辑器中查找该语句。");
        return true;
    }
    QTextCursor cursor(document);
    cursor.setPosition(end);
    cursor.setPosition(start, QTextCursor::KeepAnchor);
    editor_.setTextCursor(cursor);
    editor_.setFocus();
    editor_.ensureCursorVisible();
    return true;
}

void QueryMessages::mouseReleaseEvent(QMouseEvent* event) {
    QPlainTextEdit::mouseReleaseEvent(event);
    if (event->button() == Qt::LeftButton && !textCursor().hasSelection())
        locate(cursorForPosition(event->pos()).block());
}

void QueryMessages::keyPressEvent(QKeyEvent* event) {
    if ((event->key() == Qt::Key_Return || event->key() == Qt::Key_Enter) &&
        locate(textCursor().block())) {
        event->accept();
        return;
    }
    QPlainTextEdit::keyPressEvent(event);
}

void QueryMessages::contextMenuEvent(QContextMenuEvent* event) {
    const auto block = event->reason() == QContextMenuEvent::Keyboard ? textCursor().block() :
        cursorForPosition(event->pos()).block();
    std::unique_ptr<QMenu> menu(createStandardContextMenu(event->pos()));
    if (dynamic_cast<const Location*>(block.userData())) {
        auto* action = new QAction("定位到 SQL", menu.get());
        connect(action, &QAction::triggered, this, [this, block] { locate(block); });
        menu->insertAction(menu->actions().value(0), action);
    }
    menu->exec(event->globalPos());
}
