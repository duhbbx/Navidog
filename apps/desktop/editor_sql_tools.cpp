#include "editor_sql_tools.hpp"
#include "navidog/database_sql.hpp"
#include "editor.hpp"
#include "metadata_controller.hpp"
#include "sql_conversion.hpp"
#include "sql_optimization.hpp"

#include <algorithm>
#include <utility>

EditorSqlTools::EditorSqlTools(const QVector<navidog::ConnectionProfile> &profiles,
                               const MetadataController &metadata,
                               const ApplicationSettings &settings, QString storePath)
    : profiles_(profiles), metadata_(metadata), settings_(settings), storePath_(std::move(storePath)) {}

void EditorSqlTools::open(QWidget &pane, const SqlEditor &editor, EditorSqlTool tool,
                          const QString &profileId, const QString &database,
                          const QString &schema) const {
    namespace sql = navidog::sql::compiler;
    const auto found = std::find_if(profiles_.cbegin(), profiles_.cend(),
        [&](const auto &profile) { return profile.id == profileId; });
    sql::NameResolutionContext context;
    context.dialect = sql::Dialect::SQLite;
    context.database = database.toStdString();
    context.schema = schema.toStdString();
    navidog::MetadataSnapshotPtr snapshot;
    navidog::ui::ExportContext output;
    QString statisticsKey;
    if (found != profiles_.cend()) {
        context.dialect = navidog::compilerDialectFor(found->driver)
                              .value_or(sql::Dialect::SQLite);
        const auto effectiveDatabase = database.isEmpty() ||
                                       found->driver == navidog::DatabaseKind::SQLite ||
                                       navidog::usesOracleSql(found->driver)
                                           ? found->database : database;
        context.database = effectiveDatabase.toStdString();
        snapshot = metadata_.snapshot(*found, effectiveDatabase, schema);
        output = navidog::ui::exportContext(*found, database, schema);
        if (tool == EditorSqlTool::Optimize)
            statisticsKey = "optimizer.statistics." + navidog::metadataKey(*found, effectiveDatabase, schema);
    }
    const auto sqlText = editor.selectedSqlOrAll();
    if (tool == EditorSqlTool::Convert)
        showSqlConversion(&pane, context.dialect, sqlText, context, snapshot, settings_.formatting, output);
    else
        showSqlOptimization(&pane, context.dialect, sqlText, context, snapshot, storePath_,
                            statisticsKey, settings_, output);
}
