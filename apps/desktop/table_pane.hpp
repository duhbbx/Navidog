#pragma once
#include "workspace_pane.hpp"
#include "table_object_model.hpp"
#include <QMap>
#include <QTabWidget>
#include <QAbstractTableModel>
class QTableView;
class QLineEdit;
class QPlainTextEdit;
class SqlReadOnlyView;
class SqlEditor;
class QLabel;
class QToolButton;
class QCheckBox;
class TableColumnModel final : public QAbstractTableModel {
    Q_OBJECT
public:
    QVector<navidog::TableColumn> columns;
    explicit TableColumnModel(QObject* parent=nullptr):QAbstractTableModel(parent){}
    int rowCount(const QModelIndex& parent={})const override {return parent.isValid()?0:columns.size();}
    int columnCount(const QModelIndex& parent={})const override {return parent.isValid()?0:13;}
    QVariant data(const QModelIndex&,int role)const override;
    QVariant headerData(int,Qt::Orientation,int role)const override;
    Qt::ItemFlags flags(const QModelIndex&)const override;
    bool setData(const QModelIndex&,const QVariant&,int role)override;
    void resetColumns(QVector<navidog::TableColumn>);
    void addColumn(navidog::DatabaseKind);
    void removeColumn(int);
    void insertColumn(int,navidog::DatabaseKind);
    void moveColumn(int,int);
};
class TablePane final : public WorkspacePane {
    Q_OBJECT
public:
    TableColumnModel* model;
    QTableView* fields;
    QLineEdit* name;
    SqlReadOnlyView* sql;
    QLabel* message;
    QToolButton *save,*reload,*add,*remove,*stop;
    QTabWidget* sections;
    QMap<int,TableObjectModel*> objectModels;
    QMap<int,QTableView*> objectViews;
    SqlEditor *triggerSql;
    QPlainTextEdit *comment;
    QMap<QString,QLineEdit*> options;
    QCheckBox *withoutRowid,*strict;
    QToolButton *insert,*up,*down;
    void focusObject(const QString& kind,const QString& name={},int operation=0);
    void focusName();
    void commitEditor();
    explicit TablePane(const navidog::ConnectionProfile&,const navidog::CatalogNode&,bool existing,QWidget* parent=nullptr);
    navidog::TableDefinition definition()const;
    const navidog::TableDefinition &originalDefinition() const { return baseline; }
    const navidog::CatalogNode &tableNode() const { return baseline.node; }
    bool matchesTable(const QString &id, const navidog::CatalogNode &, bool wholeSchema = false) const;
    bool isLoaded() const { return loaded; }
    bool designInProgress() const { return designInProgress_; }
    bool canSave() const;
    bool dirty()const;
    QString tabTitle() const;
    void setBusy(bool) override;
    void requestReload();
    void requestSave();
    void setConnectionProfile(const navidog::ConnectionProfile &);
    void setDesignInProgress(bool, const QString &status = {});
    void invalidateTable(const QString &replacementName = {});
    void invalidateConnection();
    void acceptDefinition(const navidog::TableDefinition&);
    QString applyDesignResult(const navidog::TableDesignResult &, bool saved);
    QJsonObject sessionState()const override;
    void restoreState(const QJsonObject&);
private:
    navidog::ConnectionProfile profile;
    navidog::TableDefinition baseline;
    bool designInProgress_ = false, workspaceBusy_ = false;
    bool loaded = false, needsReload = false;
    QString pendingKind, pendingName;
    int pendingOperation = 0;
    void updateSql();
    QWidget* fieldPage_;
    void addItem(bool insertAtCurrent=false);
    void removeItem();
    void updateTools();
    void syncTriggerEditor();
signals:
    void saveRequested();
    void reloadRequested();
    void stopRequested();
    void draftChanged();
};
