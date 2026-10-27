#include "portabletagcrypto.h"

#include "tagcatalog.h"

#include <cstring>
#include <utility>

#ifdef Q_OS_WIN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <bcrypt.h>
#endif

namespace filemanager::tags {
namespace {

constexpr char Magic[8] = {'F', 'M', 'T', 'A', 'G', 'X', '0', '1'};
constexpr int HeaderSize = 48;
constexpr int SaltOffset = 16;
constexpr int SaltSize = 16;
constexpr int NonceOffset = SaltOffset + SaltSize;
constexpr int NonceSize = 12;
constexpr int LengthOffset = NonceOffset + NonceSize;
constexpr int TagSize = 16;
constexpr int KeySize = 32;
constexpr quint32 WriteIterations = 600'000;
constexpr quint32 MaximumReadIterations = 2'000'000;
constexpr qsizetype MaximumEnvelopeBytes =
    TagCatalog::MaximumPlainBytes + HeaderSize + TagSize;

PortableCryptoResult failure(const QString& message)
{
    return {{}, message};
}

void writeLe32(char* destination, quint32 value)
{
    for (int index = 0; index < 4; ++index)
        destination[index] = static_cast<char>((value >> (8 * index)) & 0xffU);
}

quint32 readLe32(const char* source)
{
    quint32 value = 0;
    for (int index = 0; index < 4; ++index)
        value |= static_cast<quint32>(static_cast<unsigned char>(source[index]))
            << (8 * index);
    return value;
}

#ifdef Q_OS_WIN
struct SensitiveBytes
{
    QByteArray bytes;
    ~SensitiveBytes()
    {
        if (!bytes.isEmpty())
            SecureZeroMemory(bytes.data(), static_cast<SIZE_T>(bytes.size()));
    }
};

struct SensitiveString
{
    QString value;
    ~SensitiveString()
    {
        value.detach();
        if (!value.isEmpty()) {
            SecureZeroMemory(value.data(),
                static_cast<SIZE_T>(value.size()) * sizeof(QChar));
        }
    }
};

struct Algorithm
{
    BCRYPT_ALG_HANDLE handle = nullptr;
    ~Algorithm()
    {
        if (handle)
            BCryptCloseAlgorithmProvider(handle, 0);
    }
};

struct SymmetricKey
{
    BCRYPT_KEY_HANDLE handle = nullptr;
    ~SymmetricKey()
    {
        if (handle)
            BCryptDestroyKey(handle);
    }
};

PUCHAR mutableBytes(QByteArray& bytes)
{
    return reinterpret_cast<PUCHAR>(bytes.data());
}

PUCHAR mutableBytes(const QByteArray& bytes)
{
    return reinterpret_cast<PUCHAR>(const_cast<char*>(bytes.constData()));
}

bool deriveKey(QStringView passphrase, const char* salt, quint32 iterations,
    SensitiveBytes* key)
{
    if (passphrase.size() < 12 || passphrase.size() > 1024)
        return false;
    SensitiveString normalized{passphrase.toString().normalized(
        QString::NormalizationForm_C)};
    SensitiveBytes password{normalized.value.toUtf8()};
    if (password.bytes.isEmpty() || password.bytes.size() > 4096)
        return false;
    Algorithm hmac;
    if (BCryptOpenAlgorithmProvider(&hmac.handle, BCRYPT_SHA256_ALGORITHM,
            nullptr, BCRYPT_ALG_HANDLE_HMAC_FLAG) < 0)
        return false;
    key->bytes.resize(KeySize);
    const NTSTATUS status = BCryptDeriveKeyPBKDF2(hmac.handle,
        mutableBytes(password.bytes), static_cast<ULONG>(password.bytes.size()),
        reinterpret_cast<PUCHAR>(const_cast<char*>(salt)), SaltSize,
        iterations, mutableBytes(key->bytes), KeySize, 0);
    return status >= 0;
}

bool createAesKey(const SensitiveBytes& rawKey, Algorithm* aes,
    SymmetricKey* key, SensitiveBytes* keyObject)
{
    if (BCryptOpenAlgorithmProvider(&aes->handle, BCRYPT_AES_ALGORITHM,
            nullptr, 0) < 0)
        return false;
    if (BCryptSetProperty(aes->handle, BCRYPT_CHAINING_MODE,
            reinterpret_cast<PUCHAR>(const_cast<wchar_t*>(BCRYPT_CHAIN_MODE_GCM)),
            sizeof(BCRYPT_CHAIN_MODE_GCM), 0) < 0)
        return false;
    DWORD objectSize = 0;
    ULONG received = 0;
    if (BCryptGetProperty(aes->handle, BCRYPT_OBJECT_LENGTH,
            reinterpret_cast<PUCHAR>(&objectSize), sizeof(objectSize),
            &received, 0) < 0 || received != sizeof(objectSize)
            || objectSize == 0 || objectSize > 65536)
        return false;
    keyObject->bytes.resize(static_cast<qsizetype>(objectSize));
    return BCryptGenerateSymmetricKey(aes->handle, &key->handle,
        mutableBytes(keyObject->bytes), objectSize,
        mutableBytes(rawKey.bytes), KeySize, 0) >= 0;
}

#endif

} // namespace

PortableCryptoResult sealPortableTagPayload(const QByteArray& plain,
    QStringView passphrase)
{
    if (plain.isEmpty() || plain.size() > TagCatalog::MaximumPlainBytes)
        return failure(QStringLiteral("Portable tag data is empty or oversized."));
    if (passphrase.size() < 12 || passphrase.size() > 1024)
        return failure(QStringLiteral("Use a passphrase of 12 to 1,024 characters."));
#ifdef Q_OS_WIN
    QByteArray envelope;
    envelope.resize(HeaderSize + plain.size() + TagSize);
    char* header = envelope.data();
    std::memcpy(header, Magic, sizeof(Magic));
    header[8] = 1; // Envelope version.
    header[9] = 1; // PBKDF2-HMAC-SHA-256.
    header[10] = 1; // AES-256-GCM.
    header[11] = 0;
    writeLe32(header + 12, WriteIterations);
    writeLe32(header + LengthOffset, static_cast<quint32>(plain.size()));
    if (BCryptGenRandom(nullptr,
            reinterpret_cast<PUCHAR>(header + SaltOffset), SaltSize,
            BCRYPT_USE_SYSTEM_PREFERRED_RNG) < 0
            || BCryptGenRandom(nullptr,
                reinterpret_cast<PUCHAR>(header + NonceOffset), NonceSize,
                BCRYPT_USE_SYSTEM_PREFERRED_RNG) < 0)
        return failure(QStringLiteral("Windows could not generate export randomness."));
    SensitiveBytes rawKey;
    if (!deriveKey(passphrase, header + SaltOffset, WriteIterations, &rawKey))
        return failure(QStringLiteral("Windows could not derive the export key."));
    Algorithm aes;
    SymmetricKey key;
    SensitiveBytes keyObject;
    if (!createAesKey(rawKey, &aes, &key, &keyObject))
        return failure(QStringLiteral("Windows could not create the export cipher."));
    BCRYPT_AUTHENTICATED_CIPHER_MODE_INFO info;
    BCRYPT_INIT_AUTH_MODE_INFO(info);
    info.pbNonce = reinterpret_cast<PUCHAR>(header + NonceOffset);
    info.cbNonce = NonceSize;
    info.pbAuthData = reinterpret_cast<PUCHAR>(header);
    info.cbAuthData = HeaderSize;
    info.pbTag = reinterpret_cast<PUCHAR>(
        header + HeaderSize + plain.size());
    info.cbTag = TagSize;
    ULONG written = 0;
    if (BCryptEncrypt(key.handle, mutableBytes(plain),
            static_cast<ULONG>(plain.size()), &info, nullptr, 0,
            reinterpret_cast<PUCHAR>(header + HeaderSize),
            static_cast<ULONG>(plain.size()), &written, 0) < 0
            || written != static_cast<ULONG>(plain.size())) {
        SecureZeroMemory(envelope.data(), static_cast<SIZE_T>(envelope.size()));
        return failure(QStringLiteral("Windows could not encrypt the portable tag data."));
    }
    return {std::move(envelope), {}};
#else
    Q_UNUSED(passphrase);
    return failure(QStringLiteral("Portable tag encryption requires Windows."));
#endif
}

PortableCryptoResult openPortableTagPayload(const QByteArray& envelope,
    QStringView passphrase)
{
    if (envelope.size() < HeaderSize + TagSize + 1
            || envelope.size() > MaximumEnvelopeBytes)
        return failure(QStringLiteral("The portable tag file is invalid or oversized."));
    const char* header = envelope.constData();
    if (std::memcmp(header, Magic, sizeof(Magic)) != 0
            || header[8] != 1 || header[9] != 1 || header[10] != 1
            || header[11] != 0)
        return failure(QStringLiteral("The portable tag format is unsupported."));
    const quint32 iterations = readLe32(header + 12);
    const quint32 cipherLength = readLe32(header + LengthOffset);
    if (iterations < WriteIterations || iterations > MaximumReadIterations
            || cipherLength == 0 || cipherLength > TagCatalog::MaximumPlainBytes
            || static_cast<qsizetype>(cipherLength) + HeaderSize + TagSize
                != envelope.size())
        return failure(QStringLiteral("The portable tag file has invalid limits."));
    if (passphrase.size() < 12 || passphrase.size() > 1024)
        return failure(QStringLiteral("Use a passphrase of 12 to 1,024 characters."));
#ifdef Q_OS_WIN
    SensitiveBytes rawKey;
    if (!deriveKey(passphrase, header + SaltOffset, iterations, &rawKey))
        return failure(QStringLiteral("Windows could not derive the export key."));
    Algorithm aes;
    SymmetricKey key;
    SensitiveBytes keyObject;
    if (!createAesKey(rawKey, &aes, &key, &keyObject))
        return failure(QStringLiteral("Windows could not open the export cipher."));
    QByteArray output;
    output.resize(cipherLength);
    BCRYPT_AUTHENTICATED_CIPHER_MODE_INFO info;
    BCRYPT_INIT_AUTH_MODE_INFO(info);
    info.pbNonce = reinterpret_cast<PUCHAR>(const_cast<char*>(header + NonceOffset));
    info.cbNonce = NonceSize;
    info.pbAuthData = reinterpret_cast<PUCHAR>(const_cast<char*>(header));
    info.cbAuthData = HeaderSize;
    info.pbTag = reinterpret_cast<PUCHAR>(
        const_cast<char*>(header + HeaderSize + cipherLength));
    info.cbTag = TagSize;
    ULONG written = 0;
    if (BCryptDecrypt(key.handle,
            reinterpret_cast<PUCHAR>(const_cast<char*>(header + HeaderSize)),
            cipherLength, &info, nullptr, 0, mutableBytes(output),
            cipherLength, &written, 0) < 0 || written != cipherLength) {
        SecureZeroMemory(output.data(), static_cast<SIZE_T>(output.size()));
        return failure(QStringLiteral("The passphrase is wrong or the export is damaged."));
    }
    return {std::move(output), {}};
#else
    Q_UNUSED(passphrase);
    return failure(QStringLiteral("Portable tag decryption requires Windows."));
#endif
}

} // namespace filemanager::tags
