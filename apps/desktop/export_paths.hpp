#pragma once

#include <QDateTime>
#include <QFileDialog>
#include <QString>

namespace navidog {
struct ConnectionProfile;
struct QueryResult;
enum class DatabaseKind;

namespace ui {
struct ExportContext {
  QString connection, database, schema;
};

ExportContext exportContext(const ConnectionProfile &, QString database = {}, QString schema = {});
QString systemDownloadDirectory();
void setExportDirectory(QWidget &, const QString &customDirectory);
QString exportDirectory(const QWidget *);
QString exportStem(const ExportContext &, const QString &purpose,
                   const QDateTime &time = QDateTime::currentDateTime());
QString chooseExportFile(QWidget *, const QString &title, const ExportContext &,
                         const QString &purpose, const QString &filters,
                         QString *selectedFilter = nullptr, QFileDialog::Options = {});
QString exportCachedResult(QWidget *, const QueryResult &, const ExportContext &,
                           const QString &purpose, DatabaseKind, QString tableName);
} // namespace ui
} // namespace navidog
