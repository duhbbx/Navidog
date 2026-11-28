#pragma once
#include "table_design.hpp"
#include <QJsonObject>
namespace navidog {
enum class TableAction { Rename, Drop, DeleteRows, Truncate, CopyStructure, CopyData, CountRows, Analyze, Check, Optimize, Repair, ExportCsv, ExportSql, Dictionary, ImportCsv, GenerateData, GrantPrivileges, TablePrivileges, ReverseModel, BuildBI };
struct TableTask {
    CatalogNode node;
    TableAction action=TableAction::CountRows;
    QString targetName,path,option;
    QString grantee,granteeHost="%",dimension;
    QStringList privileges;
    QJsonObject generationRules;
    bool revoke=false;
    bool withData=true;
    int maxRows=100000;
};
struct TableTaskResult {
    QString error,report;
    QueryResult result;
    QJsonObject document;
    qint64 rows=0;
    bool applied=false,cancelled=false,rolledBack=false,partial=false,commitUnknown=false;
};
QString tableActionName(TableAction);
bool tableActionWrites(TableAction);
bool supportsTableAction(DatabaseKind,TableAction);
QString tableTaskSql(const ConnectionProfile&,const TableTask&);
TableTaskResult runTableTask(const ConnectionProfile&,const TableTask&,const Cancellation&);
}
Q_DECLARE_METATYPE(navidog::TableTaskResult)
