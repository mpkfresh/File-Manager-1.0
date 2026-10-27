#pragma once

#include "files/sourcesnapshot.h"

#include <QString>
#include <QStringList>

#include <functional>

namespace filemanager::platform {

enum class NativePathKind { Regular, SymbolicLink, CloudPlaceholder, UnsupportedReparse, Missing };

struct NativeCopyResult
{
    bool ok = false;
    bool cancelled = false;
    QString error;
    quint32 nativeCode = 0;
    bool metadataConfirmationRequired = false;
    QStringList metadataDegradations;
    files::SourceSnapshot committedSnapshot;
};

struct NativeVolumeCapabilities
{
    bool known = false;
    bool namedStreams = false;
    bool persistentAcls = false;
    bool sparseFiles = false;
    bool encryption = false;
    QString fileSystemName;
};

struct NativeMetadataReview
{
    bool inspected = false;
    QString error;
    quint32 nativeCode = 0;
    QStringList degradations;
    bool compatible() const { return inspected && error.isEmpty() && degradations.isEmpty(); }
};

using NativeMetadataProbe = std::function<NativeMetadataReview(const QString&, const QString&)>;

NativePathKind nativePathKind(const QString& path);
QString extendedLengthPath(const QString& path);
NativeVolumeCapabilities nativeVolumeCapabilities(const QString& path);
QStringList unsupportedNativeMetadata(bool encrypted, bool hasNamedStreams, bool sparse,
    bool sourceHasPersistentAcls, const NativeVolumeCapabilities& destination);
// Read-only metadata capability review. destinationLocation must already exist and be on the
// filesystem that would receive the copy.
NativeMetadataReview reviewNativeCopyMetadata(const QString& source,
    const QString& destinationLocation);
// When approvedMetadataDegradations is supplied, every newly observed loss
// must be present in that exact reviewed detail list. Legacy callers may omit
// the list and retain their existing boolean-consent behavior.
NativeCopyResult copyFileAtomically(const QString& source, const QString& destination,
    bool replaceExisting, const std::function<bool(qint64, qint64)>& progress = {},
    bool allowMetadataDegradation = false,
    const std::function<NativeCopyResult(const QString&, const files::SourceSnapshot&)>&
        stagingCommit = {},
    const QStringList* approvedMetadataDegradations = nullptr,
    const NativeMetadataProbe& additionalMetadataProbe = {});

} // namespace filemanager::platform
