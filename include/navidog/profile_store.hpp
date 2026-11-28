#pragma once
#include "types.hpp"

namespace navidog {
QJsonObject connectionProfileJson(const ConnectionProfile&);
ConnectionProfile connectionProfileFromJson(const QJsonObject&);

class ProfileStore {
public:
    explicit ProfileStore(QString path) : path_(std::move(path)) {}
    QVector<ConnectionProfile> load() const;
    WorkspaceProfiles loadWorkspace() const;
    void saveWorkspace(const WorkspaceProfiles&) const;
    void save(const QVector<ConnectionProfile>&) const;
private:
    QString path_;
};
}
