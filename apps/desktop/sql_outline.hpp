#pragma once

#include <QMetaObject>
#include <QSplitter>
#include <QTimer>
#include <QVector>
#include <atomic>
#include <memory>

class QLabel;
class QShowEvent;
class QHideEvent;
class QToolButton;
class QTreeWidgetItem;
class SqlEditor;
namespace navidog::ui { class ObjectTree; }
namespace navidog::sql::compiler { struct SqlOutline; }

class SqlEditorArea final : public QSplitter {
    Q_OBJECT
public:
    SqlEditorArea(SqlEditor *editor, QToolButton *outlineToggle, bool open,
                  QWidget *parent = nullptr);
    ~SqlEditorArea() override;
    bool outlineOpen() const;
    void setOutlineOpen(bool open);

protected:
    void showEvent(QShowEvent *) override;
    void hideEvent(QHideEvent *) override;

private:
    void observeDocument();
    void refresh();
    void launch();
    void showOutline(const navidog::sql::compiler::SqlOutline &result, const QVector<int> &offsets);
    void nodeMenu(const QPoint &position);
    void locate(QTreeWidgetItem *, bool select);
    void copy(QTreeWidgetItem *);
    QString unavailableReason() const;
    bool current() const;

    SqlEditor *editor_;
    QToolButton *toggle_;
    QWidget *outline_;
    navidog::ui::ObjectTree *tree_;
    QLabel *status_;
    QTimer debounce_;
    QMetaObject::Connection documentChanged_;
    std::shared_ptr<std::atomic_bool> cancel_;
    quint64 generation_ = 0, shownGeneration_ = 0;
    bool running_ = false;
};
