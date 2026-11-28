#pragma once
#include "types.hpp"
#include <QJsonArray>
namespace navidog {
// Invoked only by the database worker / explicit discovery command, never the GUI thread.
bool useSessionCredentialsOnly(ConnectionProfile&);
ConnectionProfile resolveCredentials(const ConnectionProfile&);
QJsonArray localDockerContainers();
}
