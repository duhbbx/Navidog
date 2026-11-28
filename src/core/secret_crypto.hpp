#pragma once

#include <QByteArray>

namespace navidog::secret_crypto {
QByteArray randomBytes(int size);
QByteArray aes256Cbc(const QByteArray& input, const QByteArray& key, const QByteArray& iv, bool encrypt);
QByteArray archiveKey(const QByteArray& password, const QByteArray& salt);
bool equalTags(const QByteArray& a, const QByteArray& b);
}
