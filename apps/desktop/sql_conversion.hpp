#pragma once
#include "export_paths.hpp"
#include "navidog/metadata_cache.hpp"
#include "navidog/sql/compiler/conversion.hpp"
#include "navidog/sql/compiler/formatter.hpp"
#include <QString>
class QWidget;
// Offline, modal conversion workspace. Never changes or executes the source.
void showSqlConversion(QWidget *, navidog::sql::compiler::Dialect,
                       const QString &,
                       navidog::sql::compiler::NameResolutionContext = {},
                       navidog::MetadataSnapshotPtr = {},
                       navidog::sql::compiler::FormatStyle = {},
                       navidog::ui::ExportContext = {},
                       navidog::DatabaseKind sourceKind = navidog::DatabaseKind::Unknown);
