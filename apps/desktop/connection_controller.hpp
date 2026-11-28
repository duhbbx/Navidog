#pragma once

#include "navidog/types.hpp"
#include <QObject>
#include <functional>
#include <optional>

class QWidget;

namespace navidog {
class ProfileStore;
class QueryService;
}

class ConnectionController final : public QObject {
    Q_OBJECT

public:
    struct WorkspaceAccess {
        std::function<QVector<navidog::ConnectionGroup>()> groups;
        std::function<bool()> closingTabs;
        std::function<bool(const QString &)> prepareEdit;
    };

    ConnectionController(navidog::QueryService &, const navidog::ProfileStore &,
                         QVector<navidog::ConnectionProfile> &, QString storePath,
                         WorkspaceAccess, QWidget *owner);

    void create(const QString &groupId);
    void edit(const QString &id);
    void duplicate(const QString &id);
    void importConnections();
    void exportConnections(const QString& preferredId = {});
    bool promptPassword(navidog::ConnectionProfile &);
    std::optional<navidog::ConnectionProfile> authenticatedProfile(const QString &id);

signals:
    void connectionSaved(const QString &id, bool edited, bool endpointChanged);
    void connectionsImported(QVector<navidog::ConnectionGroup> groups);
    void statusChanged(const QString &message);

private:
    enum class Mode { Create, Edit, Duplicate };
    void open(Mode, const QString &id, const QString &groupId = {});
    void prepareDuplicate(navidog::ConnectionProfile &) const;

    navidog::QueryService &service_;
    const navidog::ProfileStore &store_;
    QVector<navidog::ConnectionProfile> &profiles_;
    const QString storePath_;
    WorkspaceAccess workspace_;
    QWidget *owner_;
};
