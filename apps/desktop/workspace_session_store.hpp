#pragma once

#include <QByteArray>
#include <QJsonArray>
#include <QString>
#include <optional>

class QMainWindow;
class QTabWidget;

struct WorkspaceSession {
    std::optional<QJsonArray> tabs;
    int activeTab = 0;
    QByteArray geometry, splitterState;
    bool sidebarVisible = true;
};

class WorkspaceSessionStore final {
public:
    explicit WorkspaceSessionStore(const QString &dataDirectory);
    std::optional<WorkspaceSession> load() const;
    QString sqlDraftPath(const QString &id) const;
    void save(const QMainWindow &, const QTabWidget &, bool sidebarVisible) const;

private:
    QString path_, legacyPath_;
};
