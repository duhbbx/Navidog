#pragma once
#include "navidog/table_design.hpp"
#include <QWidget>
#include <QJsonObject>
enum class EditorSqlTool;
class WorkspacePane : public QWidget {
public:
    struct Activity {
        bool busy;
        bool exclusiveOperation;
    };
    navidog::WorkspaceTabKind kind=navidog::WorkspaceTabKind::Query;
    QString profileId,database;
    using QWidget::QWidget;
    virtual void setBusy(bool) {}
    virtual void updateWorkspaceActivity(Activity activity) { setBusy(activity.busy); }
    virtual void openSqlTool(EditorSqlTool) {}
    virtual QJsonObject sessionState() const=0;
};
