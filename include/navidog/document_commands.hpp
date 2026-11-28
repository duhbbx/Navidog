#pragma once
#include <QJsonObject>
#include <QStringList>
namespace navidog {
struct DocumentCommand {
    QString name, error;
    QJsonObject options;
    int position = 0;
};
DocumentCommand parseDocumentCommand(const QString&);
QStringList mongoCommandNames();
QStringList mongoCommandOptions(const QString& command);
QString mongoCatalogCommand(const QString& command, const QString& collection = {});
QString mongoCommandDraft(const QString& command, const QString& collection,
                          const QString& index = {});
}
