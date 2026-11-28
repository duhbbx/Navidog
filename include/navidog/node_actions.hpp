#pragma once
#include "catalog.hpp"
#include "namespace_design.hpp"
#include "database_tools.hpp"
#include "source_design.hpp"
#include "object_management.hpp"
namespace navidog {
enum class NodeAction { GenerateSql, Separator, NewConnection, DuplicateConnection, NewGroup, Connect, Disconnect, EditConnection, RenameConnection, MoveConnection, DeleteConnection, RenameGroup, DeleteGroup, OpenDatabase, CloseDatabase, NewDatabase, NewSchema, DropNamespace, NamespaceProperties, EditDatabase, DatabaseConsole, RunDatabaseSqlFile, DumpDatabaseSql, DatabaseDictionary, SearchDatabase, NewQuery, NewTable, AlterTable, Refresh, CopyName, Preview, Definition, FastOpen, RenameTable, DropTable, DeleteRows, TruncateTable, CopyTable, ImportTable, ExportTable, DumpTable, Dictionary, MaintainTable, CopyTableReference, PasteTable, TablePermissions, GenerateTableData, TableModel, TableBI, ShareTable, ManageTableGroup, TableViews, RenameTableGroup, DeleteTableGroup, NewTableChild, EditTableChild, DeleteTableChild, EnableTableChild, DisableTableChild, NewSource, NewSourceList, EditSource, DropSource, DropCascade, CompileObject, RenameObject, ObjectProperties, CallSource, MongoCreateCollection, MongoInsertDocument, MongoCreateIndex, MongoDropIndex, MongoDropCollection };
inline QVector<NodeAction> nodeActions(const QString& kind,bool opened=false,DatabaseKind driver=DatabaseKind::Unknown) {
    using A=NodeAction;
    const auto details = [&](QVector<A> actions) {
        if (supportsObjectProperties(driver, kind)) actions << A::ObjectProperties;
        return actions;
    };
    if(kind=="root") return {A::NewConnection,A::NewGroup};
    if(kind=="group") return {A::NewConnection,A::NewGroup,A::Separator,A::RenameGroup,A::DeleteGroup};
    if(kind=="connection") {
        QVector<A> a={opened?A::Disconnect:A::Connect,A::Separator,A::EditConnection,A::NewConnection,A::DeleteConnection,A::DuplicateConnection,A::RenameConnection,A::Separator};
        if(supportsNamespace(driver,NamespaceKind::Database))a<<A::NewDatabase;
        if(driver==DatabaseKind::Oracle || driver==DatabaseKind::DuckDb || driver==DatabaseKind::OpenGauss)a<<A::NewSchema;
        a<<A::NewQuery;
        if(driver!=DatabaseKind::Unknown && !isCommandDatabase(driver))a<<A::SearchDatabase;
        return a<<A::Separator<<A::MoveConnection<<A::Separator<<A::Refresh;
    }
    if(driver==DatabaseKind::Redis) {
        if(kind=="redisKeys")return {A::NewQuery,A::Refresh};
        if(kind=="redisKey")return {A::Preview,A::CopyName};
        return {};
    }
    if(driver==DatabaseKind::MongoDb) {
        if(kind=="database") return {opened?A::CloseDatabase:A::OpenDatabase,A::NewQuery,
                                    A::MongoCreateCollection,A::Separator,A::Refresh,A::CopyName};
        if(kind=="mongoCollection") return {A::Preview,A::Definition,A::NewQuery,A::Separator,
            A::MongoInsertDocument,A::MongoCreateIndex,A::MongoDropCollection,A::Separator,A::Refresh,A::CopyName};
        if(kind=="index") return {A::Definition,A::MongoDropIndex,A::Separator,A::CopyName};
        return {A::CopyName};
    }
    if(usesExtendedCatalog(driver) || driver==DatabaseKind::OpenGauss) {
        if(kind=="database" || kind=="schema") {
            QVector<A> actions{opened?A::CloseDatabase:A::OpenDatabase,A::NamespaceProperties,A::NewQuery,A::Separator};
            if(kind=="database" && supportsNamespace(driver,NamespaceKind::Database)) actions << A::NewDatabase;
            if(supportsNamespace(driver,NamespaceKind::Schema)) actions << A::NewSchema;
            if(supportsNamespaceDrop(driver,kind=="database"?NamespaceKind::Database:NamespaceKind::Schema)) actions << A::DropNamespace;
            if(supportsObjectCascade(driver,kind)) actions<<A::DropCascade;
            return actions << A::Refresh << A::CopyName;
        }
        if(kind=="table" || kind=="view") {
            QVector<A> actions{A::Preview,A::Definition,A::NewQuery};
            if(kind=="table" && driver==DatabaseKind::OpenGauss) actions << A::AlterTable;
            if(supportsObjectCascade(driver,kind) && kind=="table") actions << A::DropTable << A::DropCascade;
            return details(actions << A::Separator << A::Refresh << A::CopyName);
        }
        if(kind=="tables" && driver==DatabaseKind::OpenGauss)
            return {A::NewTable,A::Separator,A::Refresh,A::CopyName};
        if(QStringList{"index","constraint","function","procedure"}.contains(kind))
            return {A::Definition,A::NewQuery,A::CopyName};
        if(QStringList{"tables","views","columns","indexes","constraints","functions","procedures"}.contains(kind))
            return {A::Refresh,A::CopyName};
        return {A::CopyName};
    }
    if(usesJdbcCatalog(driver)) {
        if(kind=="schema")return {opened?A::CloseDatabase:A::OpenDatabase,A::NewQuery,A::NewTable,A::Separator,A::Refresh,A::CopyName};
        if(kind=="table")return details({A::Preview,A::AlterTable,A::Separator,A::Refresh,A::CopyName});
        if(kind=="view")return {A::Preview,A::Separator,A::Refresh,A::CopyName};
        if(kind=="tables")return {A::NewTable,A::Separator,A::Refresh,A::CopyName};
        if(QStringList{"views","columns","functions","procedures"}.contains(kind))return {A::Refresh,A::CopyName};
        return {A::CopyName};
    }
    if(driver==DatabaseKind::Dameng) {
        if(kind=="schema")
            return {opened?A::CloseDatabase:A::OpenDatabase,A::NamespaceProperties,
                    A::Separator,A::NewQuery,A::DatabaseConsole,A::RunDatabaseSqlFile,
                    A::SearchDatabase,A::Separator,A::Refresh,A::CopyName};
        if(kind=="table" || kind=="view")
            return {A::Preview,A::Definition,A::Separator,A::Refresh,A::CopyName};
        if(kind=="function" || kind=="procedure")
            return {A::Definition,A::Separator,A::Refresh,A::CopyName};
        if(QStringList{"tables","views","columns","functions","procedures"}.contains(kind))
            return {A::Refresh,A::CopyName};
        return {A::CopyName};
    }
    if(kind=="database" || kind=="schema") {
        QVector<A> a={opened?A::CloseDatabase:A::OpenDatabase,A::Separator};
        const auto target=kind=="database"?NamespaceKind::Database:NamespaceKind::Schema;
        if(kind=="database" && supportsDatabaseEdit(driver))a<<A::EditDatabase;
        if(supportsNamespace(driver,NamespaceKind::Database))a<<A::NewDatabase;
        if(supportsNamespace(driver,NamespaceKind::Schema))a<<A::NewSchema;
        if(supportsNamespaceDrop(driver,target))a<<A::DropNamespace;
        if(supportsObjectCascade(driver,kind))a<<A::DropCascade;
        a<<A::NamespaceProperties;
        if(a.last()!=A::Separator)a<<A::Separator;
        a<<A::NewQuery;
        // PostgreSQL / SQL Server tables belong to a schema, not directly to a database.
        if(kind=="schema" || driver==DatabaseKind::SQLite || navidog::isMySqlFamily(driver)) {
            a<<A::NewTable;
            if(supportsSourceDesign(driver,"view"))a<<A::NewSourceList;
        }
        a<<A::Separator<<A::DatabaseConsole<<A::RunDatabaseSqlFile;
        if(supportsDatabaseSqlDump(driver,kind=="schema"))a<<A::DumpDatabaseSql;
        return a<<A::DatabaseDictionary<<A::TableModel<<A::SearchDatabase<<A::Separator<<A::Refresh<<A::CopyName;
    }
    if(kind=="tables" || kind=="tableGroup"){QVector<A> a={A::NewTable,A::Separator,A::ImportTable,A::ExportTable,A::GenerateTableData,A::Dictionary,A::TableModel,A::Separator,A::ManageTableGroup};if(kind=="tableGroup")a<<A::RenameTableGroup<<A::DeleteTableGroup;return a<<A::PasteTable<<A::Refresh;}
    if(kind=="table") {
        QVector<A> actions{A::Preview,A::FastOpen,A::TableViews,A::Separator,A::AlterTable,A::NewTable,A::DropTable};
        if(supportsObjectCascade(driver,kind))actions<<A::DropCascade;
        actions<<A::DeleteRows<<A::TruncateTable<<A::CopyTable<<A::Separator<<A::TablePermissions
               <<A::ImportTable<<A::ExportTable<<A::GenerateTableData<<A::Dictionary<<A::DumpTable
               <<A::GenerateSql<<A::MaintainTable<<A::TableModel<<A::TableBI<<A::Separator<<A::ManageTableGroup
               <<A::CopyTableReference<<A::PasteTable<<A::ShareTable<<A::RenameTable<<A::Refresh<<A::Definition<<A::CopyName;
        return details(actions);
    }
    if(QStringList{"columns","indexes","foreignKeys","checks","triggers"}.contains(kind))return {A::NewTableChild,A::Separator,A::Refresh,A::CopyName};
    if(kind=="primaryKey")return details({A::EditTableChild,A::DeleteTableChild,A::Separator,A::Refresh,A::CopyName});
    if(QStringList{"column","index","foreignKey","check","trigger"}.contains(kind)){
        QVector<A> a={A::EditTableChild,A::NewTableChild,A::DeleteTableChild};
        if(kind=="trigger" && (driver==DatabaseKind::PostgreSQL || driver==DatabaseKind::Oracle || driver==DatabaseKind::SQLServer))a<<A::Separator<<A::EnableTableChild<<A::DisableTableChild;
        if(supportsObjectCompile(driver,kind))a<<A::CompileObject;
        return details(a<<A::Separator<<A::Refresh<<A::CopyName);
    }
    if(isSourceRelation(kind) || isRoutineObject(kind)) {
        QVector<A> actions;
        if(isSourceRelation(kind))actions<<A::Preview;
        if(kind=="view")actions<<A::GenerateSql;
        if(supportsSourceDesign(driver,kind)) {
            if(kind!="materializedView")actions<<A::EditSource;
            if(isRoutineObject(kind))actions<<A::CallSource;
            if(supportsObjectCompile(driver,kind))actions<<A::CompileObject;
            actions<<A::NewSource<<A::DropSource;
            if(supportsObjectCascade(driver,kind))actions<<A::DropCascade;
            actions<<A::Separator;
        }
        if(kind!="materializedView")actions<<A::Definition;
        return details(actions<<A::Refresh<<A::CopyName);
    }
    if(kind=="views" || kind=="materializedViews" || kind=="functions" || kind=="procedures") {
        auto object=kind;
        object.chop(1);
        if(supportsSourceDesign(driver,object))return {A::NewSource,A::Separator,A::Refresh,A::CopyName};
    }
    if(kind=="views" || kind=="columns" || kind=="indexes" || kind=="functions" || kind=="procedures" || kind=="parameters" || kind=="variables") return {A::Refresh,A::CopyName};
    if(kind=="note" || kind.isEmpty()) return {};
    return {A::CopyName};
}
}
