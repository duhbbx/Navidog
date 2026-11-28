#pragma once

#include "navidog/types.hpp"
#include <QString>

struct ApplicationSettings;
class MetadataController;
class SqlEditor;
class QWidget;

enum class EditorSqlTool { Convert, Optimize };

class EditorSqlTools final {
public:
    EditorSqlTools(const QVector<navidog::ConnectionProfile> &, const MetadataController &,
                   const ApplicationSettings &, QString storePath);
    void open(QWidget &pane, const SqlEditor &, EditorSqlTool, const QString &profileId,
              const QString &database, const QString &schema) const;

private:
    const QVector<navidog::ConnectionProfile> &profiles_;
    const MetadataController &metadata_;
    const ApplicationSettings &settings_;
    QString storePath_;
};
