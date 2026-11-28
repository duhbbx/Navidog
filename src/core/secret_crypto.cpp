#include "secret_crypto.hpp"

#include <QScopeGuard>
#include <stdexcept>
#ifdef Q_OS_MACOS
#include <CommonCrypto/CommonCryptor.h>
#include <CommonCrypto/CommonKeyDerivation.h>
#include <stdlib.h>
#elif defined(Q_OS_WIN)
#include <windows.h>
#include <bcrypt.h>
#else
#include <openssl/evp.h>
#include <openssl/rand.h>
#endif

namespace navidog::secret_crypto {
namespace {
constexpr int rounds = 600000;
[[noreturn]] void fail() { throw std::runtime_error("无法完成加密或校验凭据"); }

#ifdef Q_OS_WIN
struct Algorithm {
    BCRYPT_ALG_HANDLE handle{};
    Algorithm(const wchar_t* id, ULONG flags = 0) {
        if (BCryptOpenAlgorithmProvider(&handle, id, nullptr, flags) != 0) fail();
    }
    ~Algorithm() { BCryptCloseAlgorithmProvider(handle, 0); }
};
PUCHAR bytes(const QByteArray& data) {
    return reinterpret_cast<PUCHAR>(const_cast<char*>(data.constData()));
}
#endif
}

QByteArray randomBytes(int size) {
    QByteArray data(size, '\0');
#ifdef Q_OS_MACOS
    arc4random_buf(data.data(), size);
#elif defined(Q_OS_WIN)
    if (BCryptGenRandom(nullptr, reinterpret_cast<PUCHAR>(data.data()), ULONG(size),
                        BCRYPT_USE_SYSTEM_PREFERRED_RNG) != 0) fail();
#else
    if (RAND_bytes(reinterpret_cast<unsigned char*>(data.data()), size) != 1) fail();
#endif
    return data;
}

QByteArray aes256Cbc(const QByteArray& input, const QByteArray& key, const QByteArray& iv, bool encrypt) {
    QByteArray output(input.size() + 16, '\0');
#ifdef Q_OS_MACOS
    size_t written = 0;
    if (CCCrypt(encrypt ? kCCEncrypt : kCCDecrypt, kCCAlgorithmAES, kCCOptionPKCS7Padding,
                key.constData(), 32, iv.constData(), input.constData(), input.size(),
                output.data(), output.size(), &written) != kCCSuccess) fail();
    output.resize(int(written));
#elif defined(Q_OS_WIN)
    Algorithm algorithm(BCRYPT_AES_ALGORITHM);
    if (BCryptSetProperty(algorithm.handle, BCRYPT_CHAINING_MODE,
                          reinterpret_cast<PUCHAR>(const_cast<wchar_t*>(BCRYPT_CHAIN_MODE_CBC)),
                          sizeof(BCRYPT_CHAIN_MODE_CBC), 0) != 0) fail();
    DWORD objectLength = 0, read = 0;
    if (BCryptGetProperty(algorithm.handle, BCRYPT_OBJECT_LENGTH, reinterpret_cast<PUCHAR>(&objectLength),
                          sizeof(objectLength), &read, 0) != 0) fail();
    QByteArray keyObject(int(objectLength), '\0');
    BCRYPT_KEY_HANDLE handle{};
    if (BCryptGenerateSymmetricKey(algorithm.handle, &handle, reinterpret_cast<PUCHAR>(keyObject.data()),
                                   objectLength, bytes(key), ULONG(key.size()), 0) != 0) fail();
    const auto release = qScopeGuard([&] { BCryptDestroyKey(handle); keyObject.fill('\0'); });
    auto mutableIv = iv;
    ULONG written = 0;
    const auto crypt = encrypt ? BCryptEncrypt : BCryptDecrypt;
    if (crypt(handle, bytes(input), ULONG(input.size()), nullptr, reinterpret_cast<PUCHAR>(mutableIv.data()),
              ULONG(mutableIv.size()), reinterpret_cast<PUCHAR>(output.data()), ULONG(output.size()),
              &written, BCRYPT_BLOCK_PADDING) != 0) fail();
    output.resize(int(written));
#else
    auto* context = EVP_CIPHER_CTX_new();
    if (!context) fail();
    const auto release = qScopeGuard([&] { EVP_CIPHER_CTX_free(context); });
    int first = 0, last = 0;
    auto* destination = reinterpret_cast<unsigned char*>(output.data());
    if (EVP_CipherInit_ex(context, EVP_aes_256_cbc(), nullptr,
                          reinterpret_cast<const unsigned char*>(key.constData()),
                          reinterpret_cast<const unsigned char*>(iv.constData()), encrypt ? 1 : 0) != 1 ||
        EVP_CipherUpdate(context, destination, &first,
                         reinterpret_cast<const unsigned char*>(input.constData()), int(input.size())) != 1 ||
        EVP_CipherFinal_ex(context, destination + first, &last) != 1) fail();
    output.resize(first + last);
#endif
    return output;
}

QByteArray archiveKey(const QByteArray& password, const QByteArray& salt) {
    QByteArray key(64, '\0');
#ifdef Q_OS_MACOS
    if (CCKeyDerivationPBKDF(kCCPBKDF2, password.constData(), password.size(),
                            reinterpret_cast<const uint8_t*>(salt.constData()), salt.size(),
                            kCCPRFHmacAlgSHA256, rounds, reinterpret_cast<uint8_t*>(key.data()),
                            key.size()) != kCCSuccess) fail();
#elif defined(Q_OS_WIN)
    Algorithm algorithm(BCRYPT_SHA256_ALGORITHM, BCRYPT_ALG_HANDLE_HMAC_FLAG);
    if (BCryptDeriveKeyPBKDF2(algorithm.handle, bytes(password), ULONG(password.size()),
                              bytes(salt), ULONG(salt.size()), rounds,
                              reinterpret_cast<PUCHAR>(key.data()), ULONG(key.size()), 0) != 0) fail();
#else
    if (PKCS5_PBKDF2_HMAC(password.constData(), int(password.size()),
                          reinterpret_cast<const unsigned char*>(salt.constData()), int(salt.size()),
                          rounds, EVP_sha256(), int(key.size()),
                          reinterpret_cast<unsigned char*>(key.data())) != 1) fail();
#endif
    return key;
}

bool equalTags(const QByteArray& a, const QByteArray& b) {
    if (a.size() != b.size()) return false;
    unsigned difference = 0;
    for (qsizetype i = 0; i < a.size(); ++i)
        difference |= static_cast<unsigned char>(a.constData()[i]) ^ static_cast<unsigned char>(b.constData()[i]);
    return difference == 0;
}
}
