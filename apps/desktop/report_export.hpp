#pragma once

#include "navidog/er_model.hpp"
#include "dictionary_document.hpp"
#include "export_paths.hpp"

namespace navidog { struct DatabaseDictionary; }

namespace navidog::ui {
enum class ReportDocumentFormat { Pdf, Word };
void exportDatabaseDictionary(const DatabaseDictionary&, const QString& path, ReportDocumentFormat,
                              const Cancellation&);
void exportDatabaseDictionary(const DatabaseDictionary&, const DictionaryLayout&, const QString& path,
                              ReportDocumentFormat, const Cancellation&);
}

namespace navidog::ui::er {
enum ExportFormat { Markdown = 1, Pdf = 2, Word = 4 };
QString exportReport(const navidog::er::Project&, const QString& destination, int formats,
                     const navidog::Cancellation&, const std::function<void(int,int)>& progress = {},
                     const ExportContext& context = {});
}
