#pragma once

#include "navidog/background_tasks.hpp"
#include <QPointer>
#include <QWidget>

class QDialog;
class QProgressBar;
class QStatusBar;
class QTableView;
class QLabel;
class MainWindow;

namespace navidog::ui {
class PushButton;
class Select;
class TaskListModel;

BackgroundTasks* backgroundTasksFor(const QWidget*);
void rememberTaskDialog(const QString&, QDialog&);

class TaskCenter final : public QObject {
    Q_OBJECT
public:
    TaskCenter(BackgroundTasks&, QStatusBar&, QWidget& window);
    void show();
    void rememberDialog(const QString&, QDialog&);

private:
    BackgroundTasks& tasks_;
    PushButton* entry_;
    QDialog* dialog_;
    QTableView* grid_;
    TaskListModel* model_;
    QLabel* detail_;
    QProgressBar* progress_;
    Select* artifacts_;
    PushButton *reopen_, *stop_, *openFile_, *openFolder_, *clear_;
    QHash<QString, QPointer<QDialog>> dialogs_;

    QString selectedId() const;
    void refresh(const QString& id);
    void refreshSelection();
    void openArtifact(bool directory);
    void reopen();
};

}
