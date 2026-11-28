#pragma once

#include <QPlainTextEdit>
#include <QPointer>
#include <functional>

class SqlEditor;

struct QueryMessageSource {
    QPointer<QTextDocument> document;
    int revision = -1;
    int start = 0;
    int length = 0;
};

class QueryMessages final : public QPlainTextEdit {
public:
    explicit QueryMessages(SqlEditor& editor);
    void appendSql(const QString& heading, const QString& displaySql,
                   const QueryMessageSource& = {}, int begin = 0, int end = 0);
    std::function<void(const QString&)> statusChanged;

protected:
    void mouseReleaseEvent(QMouseEvent*) override;
    void keyPressEvent(QKeyEvent*) override;
    void contextMenuEvent(QContextMenuEvent*) override;

private:
    bool locate(const QTextBlock&);
    SqlEditor& editor_;
};
