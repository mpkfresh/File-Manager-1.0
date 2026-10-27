#pragma once

#include <QByteArray>
#include <QString>
#include <QStringView>

namespace filemanager::tags {

struct PortableCryptoResult
{
    QByteArray bytes;
    QString error;
    bool valid() const { return error.isEmpty() && !bytes.isEmpty(); }
};

// Encrypts bounded opaque bytes for a manually requested portable tag export.
// No catalog fields, destination path, file I/O or import mutation live here.
// The envelope authenticates its versioned header and payload together.
PortableCryptoResult sealPortableTagPayload(const QByteArray& plain,
    QStringView passphrase);

// Returns plaintext only after authenticated decryption. A wrong passphrase
// and a damaged envelope share the same user-facing error.
PortableCryptoResult openPortableTagPayload(const QByteArray& envelope,
    QStringView passphrase);

} // namespace filemanager::tags
