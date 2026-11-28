#pragma once
#include "navidog/query_safety.hpp"
#include "navidog/sql/compiler/formatter.hpp"
#include "navidog/sql/optimizer/optimizer.hpp"
#include <QString>
class QWidget;
struct ApplicationSettings {
  QString exportDirectory; // Empty follows the operating system download directory.


  bool restoreWorkspace = true, editorWordWrap = false, resultsBeside = false,
       autoMetadata = true, editorCompletion = true, automaticUpdateCheck = true;
  int completionMinPrefix = 1;
  int editorFontSize = 12, editorIndentWidth = 4, queryRowLimit = 1000;
  navidog::QuerySafetyPolicy querySafety;

  int optimizerAssumedRows = 10000, optimizerMemoryMiB = 64,
      optimizerTimeMs = 2000, optimizerGroups = 2048, optimizerMemoMiB = 64;

  double optimizerCpuWeight = 1, optimizerIoWeight = 100,
         optimizerMemoryWeight = 0;

  bool optimizerFilterPushdown = true, optimizerJoinCommute = true,
       optimizerJoinAssociate = true;

  bool optimizerHep = true;

  navidog::sql::compiler::FormatStyle formatting;
};

navidog::sql::optimizer::PlannerOptions

optimizationOptions(const ApplicationSettings &);

ApplicationSettings loadApplicationSettings(const QString &path);

void saveApplicationSettings(const QString &path, const ApplicationSettings &);

bool editApplicationSettings(QWidget *, const QString &path,
                             navidog::sql::compiler::Dialect,
                             const QString &sample, int category = 0);
