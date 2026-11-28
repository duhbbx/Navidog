#pragma once
#include "navidog/sql/compiler/formatter.hpp"
#include <QString>
#include <functional>
class QWidget;
class SqlEditor;
void setSqlPreviewFormattingPath(QWidget &, const QString &settingsPath);
void formatEditorAsync(SqlEditor *, navidog::sql::compiler::Dialect,
                       const QString &globalConfig, const QString &scriptPath,
                       std::function<bool()> contextStillCurrent,
                       std::function<void(const QString &)> finished);
