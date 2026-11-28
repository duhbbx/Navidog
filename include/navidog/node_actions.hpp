#pragma once
#include "catalog.hpp"
#include "namespace_design.hpp"
#include "database_tools.hpp"
#include "source_design.hpp"
namespace navidog {
enum class NodeAction { GenerateSql, Separator, NewConnection, DuplicateConnection, NewGroup, Connect, Disconnect, EditConnection, RenameConnection, MoveConnection, DeleteConnection, RenameGroup, DeleteGroup, OpenDatabase, CloseDatabase, NewDatabase, NewSchema, DropNamespace, NamespaceProperties, EditDatabase, DatabaseConsole, RunDatabaseSqlFile, DumpDatabaseSql, DatabaseDictionary, SearchDatabase, NewQuery, NewTable, AlterTable, Refresh, CopyName, Preview, Definition, FastOpen, RenameTable, DropTable, DeleteRows, TruncateTable, CopyTable, ImportTable, ExportTable, DumpTable, Dictionary, MaintainTable, CopyTableReference, PasteTable, TablePermissions, GenerateTableData, TableModel, TableBI, ShareTable, ManageTableGroup, TableViews, RenameTableGroup, DeleteTableGroup, NewTableChild, EditTableChild, DeleteTableChild, EnableTableChild, DisableTableChild, NewSource, EditSource, DropSource, CallSource };
inline QVector<NodeAction> nodeActions(const QString& kind,bool opened=false,DatabaseKind driver=DatabaseKind::Unknown) {
    using A=NodeAction;
    if(kind=="root") return {A::NewConnection,A::NewGroup};
    if(kind=="group") return {A::NewConnection,A::NewGroup,A::Separator,A::RenameGroup,A::DeleteGroup};
    if(kind=="connection") {
        QVector<A> a={opened?A::Disconnect:A::Connect,A::Separator,A::EditConnection,A::NewConnection,A::DeleteConnection,A::DuplicateConnection,A::RenameConnection,A::Separator};
        if(supportsNamespace(driver,NamespaceKind::Database))a<<A::NewDatabase;
        if(driver==DatabaseKind::Oracle)a<<A::NewSchema;
        a<<A::NewQuery;
        if(driver!=DatabaseKind::Unknown && driver!=DatabaseKind::Redis)a<<A::SearchDatabase;
        return a<<A::Separator<<A::MoveConnection<<A::Separator<<A::Refresh;
    }
    if(driver==DatabaseKind::Redis) {
        if(kind=="redisKeys")return {A::NewQuery,A::Refresh};
        if(kind=="redisKey")return {A::Preview,A::CopyName};
        return {};
    }
    if(usesJdbcCatalog(driver)) {
        if(kind=="schema")return {opened?A::CloseDatabase:A::OpenDatabase,A::NewQuery,A::NewTable,A::Separator,A::Refresh,A::CopyName};
        if(kind=="table")return {A::Preview,A::AlterTable,A::Separator,A::Refresh,A::CopyName};
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
        if(kind=="schema")a<<A::NamespaceProperties;
        if(a.last()!=A::Separator)a<<A::Separator;
        a<<A::NewQuery;
        // PostgreSQL / SQL Server tables belong to a schema, not directly to a database.
        if(kind=="schema" || driver==DatabaseKind::SQLite || navidog::isMySqlFamily(driver))a<<A::NewTable;
        a<<A::Separator<<A::DatabaseConsole<<A::RunDatabaseSqlFile;
        if(supportsDatabaseSqlDump(driver,kind=="schema"))a<<A::DumpDatabaseSql;
        return a<<A::DatabaseDictionary<<A::TableModel<<A::SearchDatabase<<A::Separator<<A::Refresh<<A::CopyName;
    }
    if(kind=="tables" || kind=="tableGroup"){QVector<A> a={A::NewTable,A::Separator,A::ImportTable,A::ExportTable,A::GenerateTableData,A::Dictionary,A::TableModel,A::Separator,A::ManageTableGroup};if(kind=="tableGroup")a<<A::RenameTableGroup<<A::DeleteTableGroup;return a<<A::PasteTable<<A::Refresh;}
    if(kind=="table") return {A::Preview,A::FastOpen,A::TableViews,A::Separator,A::AlterTable,A::NewTable,A::DropTable,A::DeleteRows,A::TruncateTable,A::CopyTable,A::Separator,A::TablePermissions,A::ImportTable,A::ExportTable,A::GenerateTableData,A::Dictionary,A::DumpTable,A::GenerateSql,A::MaintainTable,A::TableModel,A::TableBI,A::Separator,A::ManageTableGroup,A::CopyTableReference,A::PasteTable,A::ShareTable,A::RenameTable,A::Refresh,A::Definition,A::CopyName};
    if(QStringList{"columns","indexes","foreignKeys","checks","triggers"}.contains(kind))return {A::NewTableChild,A::Separator,A::Refresh,A::CopyName};
    if(kind=="primaryKey")return {A::EditTableChild,A::DeleteTableChild,A::Separator,A::Refresh,A::CopyName};
    if(QStringList{"column","index","foreignKey","check","trigger"}.contains(kind)){
        QVector<A> a={A::EditTableChild,A::NewTableChild,A::DeleteTableChild};
        if(kind=="trigger" && (driver==DatabaseKind::PostgreSQL || driver==DatabaseKind::Oracle || driver==DatabaseKind::SQLServer))a<<A::Separator<<A::EnableTableChild<<A::DisableTableChild;
        return a<<A::Separator<<A::Refresh<<A::CopyName;
    }
    if(kind=="view") return {A::Preview,A::GenerateSql,A::EditSource,A::NewSource,A::DropSource,A::Separator,A::Definition,A::Refresh,A::CopyName};
    if(kind=="function" || kind=="procedure") return {A::EditSource,A::CallSource,A::NewSource,A::DropSource,A::Separator,A::Definition,A::Refresh,A::CopyName};
    if(kind=="views" || ((kind=="functions" || kind=="procedures") && driver!=DatabaseKind::SQLite))return {A::NewSource,A::Separator,A::Refresh,A::CopyName};
    if(kind=="views" || kind=="columns" || kind=="indexes" || kind=="functions" || kind=="procedures" || kind=="parameters" || kind=="variables") return {A::Refresh,A::CopyName};
    if(kind=="note" || kind.isEmpty()) return {};
    return {A::CopyName};
}
}
