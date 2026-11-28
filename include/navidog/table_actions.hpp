#pragma once
#include "data_generation.hpp"
#include "background_tasks.hpp"
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
    GenerationRequest generation;
    bool revoke=false;
    bool withData=true, cascade=false;
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
bool tableActionRunsInBackground(TableAction);
bool supportsTableAction(DatabaseKind,TableAction);
QString tableTaskSql(const ConnectionProfile&,const TableTask&);
TableTaskResult runTableTask(const ConnectionProfile&,const TableTask&,const Cancellation&,
                             const GenerationProgress& progress = {},
                             const TaskProgressCallback& backgroundProgress = {});
}
Q_DECLARE_METATYPE(navidog::TableTaskResult)
