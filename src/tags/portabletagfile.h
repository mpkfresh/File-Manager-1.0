#pragma once

#include "tags/portabletagcrypto.h"

#include <QByteArray>
#include <QString>
#include <QStringView>

namespace filemanager::tags {

struct PortableFileWriteResult
{
    bool written = false;
    QString error;
};

// Writes an opaque caller-selected payload as encrypted bytes only. The
// destination must be a new file in an existing regular folder. An encrypted
// staging file is committed without replacing a file that appeared meanwhile.
// This does not choose catalog fields, show consent UI, or mutate the catalog.
PortableFileWriteResult writeNewPortableTagFile(const QString& destination,
    const QByteArray& plain, QStringView passphrase);

// Reads only an explicitly selected locally available regular file, bounded
// by the portable envelope limit. No catalog import or path remapping occurs.
PortableCryptoResult readPortableTagFile(const QString& source,
    QStringView passphrase);

} // namespace filemanager::tags
