#pragma once
#include "table_design.hpp"
#include <QSet>
namespace navidog {
enum class DataFilter { None, Equal, NotEqual, Contains, Less, Greater, IsNull, IsNotNull };
enum class DataRowState { Clean, Inserted, Modified, Deleted };
struct DataPageRequest {
    CatalogNode node;
    int pageSize=100,filterColumn=-1,sortColumn=-1;
    qint64 offset=0;
    DataFilter filter=DataFilter::None;
    QVariant filterValue;
    bool descending=false;
    QVector<QVariant> afterKey;
};
struct DataRowChange {
    DataRowState state=DataRowState::Clean;
    QVector<QVariant> original,values;
    QSet<int> touched;
    QString version;
};
struct DataPageResult {
    TableDefinition schema;
    DataPageRequest request;
    QueryResult result;
    QVector<QString> versions;
    bool hasMore=false,editable=false,keyset=false;
    QString readOnlyReason,error;
    bool cancelled=false;
};
struct DataCommitResult {
    QString error;
    int inserted=0,updated=0,deleted=0;
    bool cancelled=false,rolledBack=false,commitUnknown=false;
};
bool supportsDataEditing(DatabaseKind);
QVector<int> primaryKeyColumns(const TableDefinition&);
DataPageResult readDataPage(const ConnectionProfile&,const DataPageRequest&,const Cancellation&);
DataCommitResult commitDataChanges(const ConnectionProfile&,const TableDefinition&,const QVector<DataRowChange>&,const Cancellation&);
}
Q_DECLARE_METATYPE(navidog::DataPageResult)
Q_DECLARE_METATYPE(navidog::DataCommitResult)
