#pragma once

#include "types.hpp"

namespace navidog {
// A nonempty archive password encrypts the file and includes available credentials.
void exportConnectionArchive(const QString& path, const WorkspaceProfiles&,
                             const QString& archivePassword = {});
QByteArray readConnectionArchiveFile(const QString& path);
bool connectionArchiveIsEncrypted(const QByteArray&);
WorkspaceProfiles decodeConnectionArchive(const QByteArray&, const QString& archivePassword = {});

// New IDs keep imported credentials and session state separate from existing connections.
WorkspaceProfiles mergeConnectionArchive(WorkspaceProfiles current, WorkspaceProfiles imported,
                                         const QString& storePath);
}
