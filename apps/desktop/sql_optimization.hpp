#pragma once
#include "export_paths.hpp"
#include "application_settings.hpp"
#include "navidog/metadata_cache.hpp"
class QWidget;
void showSqlOptimization(QWidget *, navidog::sql::compiler::Dialect,
                         const QString &,
                         const navidog::sql::compiler::NameResolutionContext &,
                         navidog::MetadataSnapshotPtr, const QString &storePath,
                         const QString &cacheKey,
                         const ApplicationSettings &settings = {},
                         navidog::ui::ExportContext = {});
