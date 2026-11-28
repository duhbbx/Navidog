#pragma once
#include "catalog.hpp"
#include <QJsonObject>
namespace navidog {
enum class WorkspaceTabKind { Query, Preview, Definition, CreateTable, AlterTable, TableData, ErModel };
struct TableColumn {
    QString originalName, name, type, defaultExpression, attributes, protectedReason;
    QString comment, generatedExpression, generation, collation, defaultConstraint;
    bool autoIncrement=false;
    bool nullable=true;
    int primaryKey=0;
    bool operator==(const TableColumn& c)const{return std::tie(originalName,name,type,defaultExpression,attributes,protectedReason,comment,generatedExpression,generation,collation,defaultConstraint,autoIncrement,nullable,primaryKey)==std::tie(c.originalName,c.name,c.type,c.defaultExpression,c.attributes,c.protectedReason,c.comment,c.generatedExpression,c.generation,c.collation,c.defaultConstraint,c.autoIncrement,c.nullable,c.primaryKey);}
    bool operator!=(const TableColumn& c)const{return !(*this==c);}
};
enum class TableObjectKind { Index, ForeignKey, Check, Trigger };
struct TableObject {
    TableObjectKind kind=TableObjectKind::Index;
    QString originalName,name,expression,method,predicate,comment;
    QStringList columns,referenceColumns;
    QString referenceSchema,referenceTable,onDelete="NO ACTION",onUpdate="NO ACTION";
    QString sql,sqlMode,protectedReason;
    bool unique=false,constraint=false,deferrable=false,initiallyDeferred=false,enabled=true;
    bool operator==(const TableObject& o)const {
        return std::tie(kind,originalName,name,expression,method,predicate,comment,columns,referenceColumns,referenceSchema,referenceTable,onDelete,onUpdate,sql,sqlMode,protectedReason,unique,constraint,deferrable,initiallyDeferred,enabled)==std::tie(o.kind,o.originalName,o.name,o.expression,o.method,o.predicate,o.comment,o.columns,o.referenceColumns,o.referenceSchema,o.referenceTable,o.onDelete,o.onUpdate,o.sql,o.sqlMode,o.protectedReason,o.unique,o.constraint,o.deferrable,o.initiallyDeferred,o.enabled);
    }
    bool operator!=(const TableObject& o)const{return !(*this==o);}
};
struct TableDefinition {
    CatalogNode node;
    QVector<TableColumn> columns;
    QVector<TableObject> objects;
    QString comment,primaryKeyName,engine,charset,collation,rowFormat,tablespace,rawSql,sqlMode;
    bool exists=false,withoutRowid=false,strict=false,detailsLoaded=false;
};
bool sameTableStructure(const TableDefinition&,const TableDefinition&);
void readTableDetails(Database&,const ConnectionProfile&,TableDefinition&,const Cancellation&);
QStringList tableObjectDrops(const ConnectionProfile&,const TableDefinition&,const TableDefinition&);
QStringList tableObjectCreates(const ConnectionProfile&,const TableDefinition&,const TableDefinition&);
QStringList tableConstraints(const ConnectionProfile&,const TableDefinition&);
QStringList tablePropertySql(const ConnectionProfile&,const TableDefinition&,const TableDefinition&);
QString tableOptionsSql(const ConnectionProfile&,const TableDefinition&);
QString createTableSql(const ConnectionProfile&,const TableDefinition&);
QStringList sqliteRebuildSql(const ConnectionProfile&,const TableDefinition&,const TableDefinition&);
bool requiresSqliteRebuild(const TableDefinition&,const TableDefinition&);
QString tableObjectKindKey(TableObjectKind);
QString tableObjectKindName(TableObjectKind);
QStringList parseIdentifierList(const QString&);
QString formatIdentifierList(const QStringList&,DatabaseKind);
QJsonObject tableDefinitionJson(const TableDefinition&);
TableDefinition tableDefinitionFromJson(const QJsonObject&);
struct TableDesignResult {
    TableDefinition definition;
    QString error,refreshError;
    int appliedStatements=0;
    bool cancelled=false, rolledBack=false, partial=false;
};
TableDefinition readTableDefinition(Database&,const ConnectionProfile&,const CatalogNode&,const Cancellation&,bool details=true);
QStringList tableChangeSql(const ConnectionProfile&,const TableDefinition& before,const TableDefinition& after);
// Uses a separate connection so DDL cannot commit/roll back a query tab's transaction.
TableDesignResult applyTableChanges(const ConnectionProfile&,const TableDefinition&,const TableDefinition&,const Cancellation&);
QString tableQualifiedName(const ConnectionProfile&,const CatalogNode&);
}
Q_DECLARE_METATYPE(navidog::TableDesignResult)
