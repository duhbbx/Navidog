#pragma once
#include "export_paths.hpp"
#include "workspace_pane.hpp"
#include "navidog/data_edit.hpp"
#include <QAbstractTableModel>
#include <functional>
#include <optional>

namespace navidog { class QueryService; }

class QTableView;
class QComboBox;
class QLineEdit;
class QToolButton;
class QLabel;
class DataGridModel final : public QAbstractTableModel {
    Q_OBJECT
public:
    navidog::TableDefinition schema;
    QVector<navidog::DataRowChange> rows;
    bool editable=false;
    using QAbstractTableModel::QAbstractTableModel;
    int rowCount(const QModelIndex& p={})const override{return p.isValid()?0:rows.size();}
    int columnCount(const QModelIndex& p={})const override{return p.isValid()?0:schema.columns.size();}
    QVariant data(const QModelIndex&,int role)const override;
    QVariant headerData(int,Qt::Orientation,int role)const override;
    Qt::ItemFlags flags(const QModelIndex&)const override;
    bool setData(const QModelIndex&,const QVariant&,int role=Qt::EditRole)override;
    void setPage(const navidog::DataPageResult&);
    void addRow();
    void deleteRow(int);
    void restoreCell(const QModelIndex&);
    void useDefault(const QModelIndex&);
    void discard();
    int pendingCount()const;
    QVector<navidog::DataRowChange> changes()const;
    navidog::QueryResult exportResult()const;
signals:
    void pendingChanged();
};
class DataPane final : public WorkspacePane {
    Q_OBJECT
public:
    using ResolveProfile = std::function<std::optional<navidog::ConnectionProfile>()>;
    DataPane(const navidog::ConnectionProfile &, const navidog::CatalogNode &,
             navidog::QueryService &, ResolveProfile, QWidget *parent = nullptr);
    bool dirty() const { return model->pendingCount() > 0; }
    bool isLoaded() const { return loaded; }
    bool isCommitting() const { return operation_ == Operation::Commit; }
    bool matchesTable(const QString &id, const navidog::CatalogNode &table,
                      bool wholeSchema = false) const;
    QString tabTitle() const;
    QJsonObject viewSettings() const;
    navidog::QueryResult cachedRows() const { return model->exportResult(); }
    navidog::ui::ExportContext exportContext() const { return exportContext_; }
    void setBusy(bool) override;
    void finishCellEditing();
    void cancelLoad();
    void preventEditingAfterSchemaChange();
    void preventEditingAfterConnectionChange();
    void requestRefresh();
    void openSavedView(const QJsonObject &);
    void invalidateTable(const QString &replacementName = {});
    void exportData();
    QJsonObject sessionState() const override;
    void restoreState(const QJsonObject &);

signals:
    void activityChanged();
    void operationFinished();
    void statusChanged(const QString &message);
    void errorOccurred(const QString &title, const QString &detail);
    void tabTitleChanged(const QString &title);

private:
    enum class Operation { Idle, Load, Commit };
    navidog::QueryService &service_;
    ResolveProfile resolveProfile_;
    navidog::ui::ExportContext exportContext_;
    navidog::DatabaseKind driver_;
    navidog::CatalogNode node;
    navidog::DataPageResult page;
    navidog::DataPageRequest pending;
    QString requestId_;
    Operation operation_ = Operation::Idle;
    bool busy_ = false, loaded = false, blocked = false;

    DataGridModel *model;
    QTableView *grid;
    QToolButton *refresh, *add, *remove, *commit, *discard, *stop, *first,
        *previous, *next, *apply;
    QComboBox *filterColumn, *filterOp, *sortColumn, *direction, *pageSize;
    QLineEdit *filterValue;
    QLabel *summary, *transaction;

    std::optional<navidog::ConnectionProfile> targetProfile();
    bool loadPage();
    void commitChanges();
    void finishPage(const QString &, const navidog::DataPageResult &);
    void finishCommit(const QString &, const navidog::DataCommitResult &);
    void startOperation(Operation, const QString &id, const QString &message);
    void finishOperation();
    QString applyPageResult(const navidog::DataPageResult &);
    QString applyCommitResult(const navidog::DataCommitResult &);
    void updateActions();
    void showCell();
    void acceptPage(const navidog::DataPageResult&);
    void prepareFirstPage();
    QVector<navidog::DataPageRequest> history_,pendingHistory_;
    bool confirmDiscard();
    navidog::DataPageRequest controlsRequest()const;
    void queue(const navidog::DataPageRequest&,QVector<navidog::DataPageRequest>);
};
