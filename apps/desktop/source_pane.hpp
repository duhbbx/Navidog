#pragma once
#include "workspace_pane.hpp"
#include "navidog/source_design.hpp"
class SqlEditor;
class SqlEditorArea;
class QLineEdit;
class QToolButton;
class QTableView;
class QStandardItemModel;
class QLabel;
class EditorSqlTools;
class SourcePane final:public WorkspacePane {
    Q_OBJECT
public:
    navidog::ConnectionProfile profile;
    navidog::SourceDefinition baseline;
    navidog::CatalogNode node;
    QString requestId;
    bool loaded=false,loading=false,saving=false,removing=false,needsReload=false;
    SqlEditor* editor;
    QLineEdit* name;
    QToolButton *save,*reload,*stop,*copy,*run;
    QStandardItemModel* parameters;
    QLabel* message;
    SourcePane(const navidog::ConnectionProfile&,const navidog::CatalogNode&,bool existing,QWidget* parent=nullptr);
    bool dirty()const;
    navidog::CatalogNode desiredNode()const;
    void acceptDefinition(const navidog::SourceDefinition&);
    void setBusy(bool) override;
    void setSqlTools(const EditorSqlTools &tools) { sqlTools_ = &tools; }
    void openSqlTool(EditorSqlTool) override;
    QJsonObject sessionState()const override;
    void restoreState(const QJsonObject&);
signals:
    void saveRequested();
    void reloadRequested();
    void stopRequested();
    void callRequested();
    void draftChanged();
    void formatRequested();
private:
    SqlEditorArea *editorArea_;
    const EditorSqlTools *sqlTools_ = nullptr;
};
