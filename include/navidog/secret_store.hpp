#pragma once
#include "types.hpp"
namespace navidog {
class LocalStore;
bool systemSecretStoreAvailable();
bool localSecretStoreAvailable();
bool hasRememberedSecrets(const ConnectionProfile&);
void saveConnectionSecrets(const ConnectionProfile&, LocalStore* transaction=nullptr);
void loadConnectionSecrets(ConnectionProfile&);
void removeConnectionSecrets(const QString& profileId);
}
