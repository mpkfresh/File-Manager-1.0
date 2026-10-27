#include "portabletagfile.h"

#include "files/filenamevalidator.h"
#include "files/scopeenumerator.h"
#include "platform/nativefilecopy.h"
#include "tags/tagcatalog.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QTemporaryFile>

#ifdef Q_OS_WIN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace filemanager::tags {
namespace {

constexpr qsizetype EnvelopeOverhead = 64; // Authenticated header and GCM tag.

PortableFileWriteResult writeFailure(const QString& error)
{
    return {false, error};
}

PortableCryptoResult readFailure(const QString& error)
{
    return {{}, error};
}

QString checkedAbsolutePath(const QString& candidate)
{
    if (!QDir::isAbsolutePath(candidate))
        return {};
    const auto validation = filemanager::files::validateWindowsPath(candidate);
    return validation.valid ? validation.normalizedName : QString{};
}

} // namespace

PortableFileWriteResult writeNewPortableTagFile(const QString& destination,
    const QByteArray& plain, QStringView passphrase)
{
    const QString path = checkedAbsolutePath(destination);
    if (path.isEmpty())
        return writeFailure(QStringLiteral("Choose a valid absolute export file path."));
    const QString folder = QFileInfo(path).absolutePath();
    if (!filemanager::files::isRegularDirectoryChain(folder)
            || !QFileInfo(folder).isDir()) {
        return writeFailure(QStringLiteral(
            "Choose an existing regular folder for the export."));
    }
    if (filemanager::platform::nativePathKind(path)
            != filemanager::platform::NativePathKind::Missing) {
        return writeFailure(QStringLiteral(
            "The export file already exists. Choose a new name."));
    }

    const PortableCryptoResult encrypted = sealPortableTagPayload(plain, passphrase);
    if (!encrypted.valid())
        return writeFailure(encrypted.error);

    QTemporaryFile staging(QDir(folder).filePath(QStringLiteral(
        ".filemanager-tags-XXXXXX.tmp")));
    if (!staging.open())
        return writeFailure(QStringLiteral(
            "Could not create encrypted export staging in the chosen folder."));
    qsizetype offset = 0;
    while (offset < encrypted.bytes.size()) {
        const qint64 written = staging.write(encrypted.bytes.constData() + offset,
            encrypted.bytes.size() - offset);
        if (written <= 0)
            return writeFailure(QStringLiteral("Could not write the encrypted export."));
        offset += written;
    }
    if (!staging.flush())
        return writeFailure(QStringLiteral("Could not flush the encrypted export."));
    if (!filemanager::files::isRegularDirectoryChain(folder))
        return writeFailure(QStringLiteral("The export folder changed before commit."));
#ifdef Q_OS_WIN
    // QTemporaryFile keeps its native handle open after close(). Its own rename
    // handles that lifetime and guarantees a same-volume atomic, no-replace move.
    if (!staging.rename(path)) {
        if (filemanager::platform::nativePathKind(path)
                != filemanager::platform::NativePathKind::Missing) {
            return writeFailure(QStringLiteral(
                "The export file appeared before commit. Choose a new name."));
        }
        return writeFailure(QStringLiteral(
            "Could not commit the encrypted export in the chosen folder: %1")
                .arg(staging.errorString()));
    }
    staging.setAutoRemove(false);
    return {true, {}};
#else
    return writeFailure(QStringLiteral("Portable tag export requires Windows."));
#endif
}

PortableCryptoResult readPortableTagFile(const QString& source,
    QStringView passphrase)
{
    const QString path = checkedAbsolutePath(source);
    if (path.isEmpty()
            || !filemanager::files::isRegularDirectoryChain(QFileInfo(path).absolutePath())
            || filemanager::platform::nativePathKind(path)
                != filemanager::platform::NativePathKind::Regular
            || !QFileInfo(path).isFile()) {
        return readFailure(QStringLiteral(
            "Choose a locally available regular portable tag file."));
    }
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly))
        return readFailure(QStringLiteral("Could not open the portable tag file."));
    const qsizetype maximum = TagCatalog::MaximumPlainBytes + EnvelopeOverhead;
    if (file.size() < EnvelopeOverhead + 1 || file.size() > maximum)
        return readFailure(QStringLiteral("The portable tag file is invalid or oversized."));
    const QByteArray encrypted = file.read(maximum + 1);
    if (encrypted.size() != file.size() || encrypted.size() > maximum)
        return readFailure(QStringLiteral("Could not read the complete portable tag file."));
    return openPortableTagPayload(encrypted, passphrase);
}

} // namespace filemanager::tags
