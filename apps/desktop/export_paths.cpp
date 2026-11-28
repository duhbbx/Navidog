#include "export_paths.hpp"
#include "navidog/result_export.hpp"
#include "navidog/types.hpp"
#include <QDir>
#include <QFileInfo>
#include <QInputDialog>
#include <QMessageBox>
#include <QRegularExpression>
#include <QSaveFile>
#include <QStandardPaths>
#include <QVariant>
#include <QStringList>
#include <stdexcept>

namespace navidog::ui {
namespace {
constexpr auto directoryProperty = "navidogExportDirectory";

QString namePart(QString part) {
  static const QRegularExpression unsafe(R"re([<>:"/\\|?*\x{0000}-\x{001f}\x{007f}])re");
  part = part.trimmed().replace(unsafe, "_");
  while (part.startsWith('.'))
    part.remove(0, 1);
  part.truncate(48);
  while (part.toUtf8().size() > 48)
    part.chop(1);
  while (!part.isEmpty()) {
    const auto last = part.at(part.size() - 1);
    if (last != '.' && !last.isSpace() && !last.isHighSurrogate())
      break;
    part.chop(1);
  }
  return part;
}

QString suffixFor(const QString &filter) {
  static const QRegularExpression extension(R"(\*\.([a-zA-Z0-9.]+))");
  return extension.match(filter).captured(1);
}

QString filename(const QString &stem, const QString &suffix) {
  return suffix.isEmpty() ? stem : stem + '.' + suffix;
}
} // namespace

ExportContext exportContext(const ConnectionProfile &profile, QString database, QString schema) {
  if (database.isEmpty())
    database = profile.database;
  if (profile.driver == DatabaseKind::SQLite) {
    if (database == "main" || database == profile.database)
      database = QFileInfo(profile.database).completeBaseName();
    if (schema == "main")
      schema.clear();
  } else if (isMySqlFamily(profile.driver) && database == schema) {
    schema.clear();
  }
  return {profile.name, database, schema};
}

QString systemDownloadDirectory() {
  const auto path = QStandardPaths::writableLocation(QStandardPaths::DownloadLocation);
  return path.isEmpty() ? QDir::home().filePath("Downloads") : path;
}

void setExportDirectory(QWidget &widget, const QString &customDirectory) {
  widget.setProperty(directoryProperty, customDirectory);
}

QString exportDirectory(const QWidget *widget) {
  for (; widget; widget = widget->parentWidget()) {
    const auto setting = widget->property(directoryProperty);
    if (!setting.isValid())
      continue;
    const auto directory = setting.toString();
    if (!directory.isEmpty() && QFileInfo(directory).isDir())
      return directory;
    break;
  }
  return systemDownloadDirectory();
}

QString exportStem(const ExportContext &context, const QString &purpose, const QDateTime &time) {
  auto connection = namePart(context.connection);
  if (connection.isEmpty())
    connection = "Navidog";
  QStringList parts{connection};
  for (const auto &scope : {context.database, context.schema}) {
    const auto name = namePart(scope);
    if (!name.isEmpty())
      parts << name;
  }
  const auto action = namePart(purpose);
  parts << (action.isEmpty() ? QString("导出") : action) << time.toString("yyyyMMdd-HHmmss");
  auto stem = parts.join('-');
  static const QRegularExpression reserved(
      R"(^(CON|PRN|AUX|NUL|COM[1-9¹²³]|LPT[1-9¹²³])(?:\.|$))",
      QRegularExpression::CaseInsensitiveOption);
  if (reserved.match(stem).hasMatch())
    stem.prepend('_');
  return stem;
}

QString chooseExportFile(QWidget *parent, const QString &title, const ExportContext &context,
                         const QString &purpose, const QString &filters, QString *selectedFilter,
                         QFileDialog::Options options) {
  QFileDialog picker(parent, title, exportDirectory(parent), filters);
  picker.setOptions(options);
  picker.setAcceptMode(QFileDialog::AcceptSave);
  picker.setFileMode(QFileDialog::AnyFile);
  if (selectedFilter && !selectedFilter->isEmpty())
    picker.selectNameFilter(*selectedFilter);
  const auto stem = exportStem(context, purpose);
  auto suffix = suffixFor(picker.selectedNameFilter());
  picker.setDefaultSuffix(suffix);
  picker.selectFile(filename(stem, suffix));
  QObject::connect(&picker, &QFileDialog::filterSelected, &picker, [&](const QString &filter) {
    const auto current = QFileInfo(picker.selectedFiles().value(0)).fileName();
    const auto previous = filename(stem, suffix);
    suffix = suffixFor(filter);
    picker.setDefaultSuffix(suffix);
    if (current == previous || current == stem)
      picker.selectFile(filename(stem, suffix));
  });
  if (picker.exec() != QDialog::Accepted)
    return {};
  auto path = picker.selectedFiles().value(0);
  const auto filter = picker.selectedNameFilter();
  const auto extension = suffixFor(filter);
  // Native dialogs can defer reporting the selected filter until acceptance.
  if (!path.isEmpty() && extension != suffix &&
      QFileInfo(path).fileName() == filename(stem, suffix)) {
    const auto corrected = QFileInfo(path).dir().filePath(filename(stem, extension));
    if (!(options & QFileDialog::DontConfirmOverwrite) && QFileInfo::exists(corrected) &&
        QMessageBox::question(parent, "替换文件", "是否替换已有文件？\n" + corrected,
                              QMessageBox::Yes | QMessageBox::Cancel,
                              QMessageBox::Cancel) != QMessageBox::Yes)
      return {};
    path = corrected;
  }
  if (selectedFilter)
    *selectedFilter = filter;
  return path;
}

QString exportCachedResult(QWidget *parent, const QueryResult &result,
                           const ExportContext &context, const QString &purpose,
                           DatabaseKind kind, QString tableName) {
  QString filter = "CSV (*.csv)";
  const auto path = chooseExportFile(parent, "导出当前缓存结果", context, purpose,
                                     "CSV (*.csv);;JSON (*.json);;SQL INSERT (*.sql)", &filter);
  if (path.isEmpty())
    return {};
  const auto format = filter.startsWith("JSON") ? ResultExportFormat::Json
                    : filter.startsWith("SQL") ? ResultExportFormat::Sql
                                                : ResultExportFormat::Csv;
  if (format == ResultExportFormat::Sql) {
    bool accepted = false;
    tableName = QInputDialog::getText(parent, "SQL 目标表",
        "目标表名称（不含 Schema，按当前数据库方言生成）：",
        QLineEdit::Normal, tableName, &accepted);
    if (!accepted)
      return {};
  }
  const auto bytes = exportResult(result, format, kind, tableName);
  QSaveFile file(path);
  if (!file.open(QIODevice::WriteOnly) ||
      file.write(bytes) != bytes.size() || !file.commit())
    throw std::runtime_error(file.errorString().toStdString());
  return path;
}
} // namespace navidog::ui
