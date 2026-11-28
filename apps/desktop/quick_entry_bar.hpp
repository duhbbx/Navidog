#pragma once

#include "components.hpp"
#include <QHash>

namespace navidog::ui {

class QuickEntryBar final : public OverflowStrip {
    Q_OBJECT

public:
    explicit QuickEntryBar(QWidget *parent = nullptr);

    void setNewConnectionEnabled(bool enabled);
    void setCatalogBusy(bool busy);
    void setSelectedCategory(const QString &kind);
    void setModelActions(const QList<QAction *> &actions);

signals:
    void newConnectionRequested();
    void newQueryRequested();
    void browseFolderRequested(const QString &kind);
    void serverObjectsRequested(bool events);
    void queryLibraryRequested();
    void backupRequested();
    void automationRequested();
    void biRequested();
    void stopCatalogRequested();

private:
    QToolButton *connectionButton_;
    QToolButton *stopCatalogButton_;
    QMenu *modelMenu_;
    QHash<QString, QToolButton *> categoryButtons_;
};

} // namespace navidog::ui
