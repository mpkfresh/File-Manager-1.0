#include "tagmoveprepare.h"

#include "files/filenamevalidator.h"
#include "files/scopeenumerator.h"
#include "platform/nativefilecopy.h"

#include <QDir>
#include <QFileInfo>
#include <QHash>
#include <QSet>

#include <utility>

namespace filemanager::tags {
namespace {

QString key(const QString& path)
{
    return QDir::cleanPath(QDir::fromNativeSeparators(path)).toCaseFolded();
}

bool stillReady(const TagMoveItem& item, QString* reason)
{
    if (item.state != TagMoveState::Proposed || item.source.isEmpty()
            || item.destination.isEmpty() || item.destinationDirectory.isEmpty()
            || !item.sourceSnapshot.valid || !item.destinationSnapshot.valid
            || !filemanager::files::validateWindowsPath(item.destination).valid
            || !filemanager::files::isRegularDirectoryChain(
                QFileInfo(item.source).absolutePath())
            || !filemanager::files::isRegularDirectoryChain(item.destinationDirectory)
            || filemanager::files::isPathInside(item.source,
                item.destinationDirectory)) {
        *reason = QStringLiteral("The retained mapping or its directory boundary is unavailable.");
        return false;
    }
    if (filemanager::platform::nativePathKind(item.source)
            != filemanager::platform::NativePathKind::Regular
            || filemanager::files::captureSourceSnapshot(item.source)
                != item.sourceSnapshot) {
        *reason = QStringLiteral("Source changed after preview; make a fresh preview before moving it.");
        return false;
    }
    if (filemanager::files::captureSourceSnapshot(item.destinationDirectory)
            != item.destinationSnapshot) {
        *reason = QStringLiteral("Destination folder changed after preview; make a fresh preview.");
        return false;
    }
    if (filemanager::platform::nativePathKind(item.destination)
            != filemanager::platform::NativePathKind::Missing) {
        *reason = QStringLiteral("The proposed destination is now occupied; review it again.");
        return false;
    }
    return true;
}

} // namespace

PreparedTagMoves prepareSelectedTagMoves(const TagMovePlan& preview,
    const QStringList& includedSources, const std::atomic_bool* cancelled)
{
    PreparedTagMoves prepared;
    if (!preview.complete() || includedSources.isEmpty()
            || includedSources.size() > 4096) {
        prepared.errors.append(QStringLiteral("A complete preview and 1 to 4,096 included files are required."));
        return prepared;
    }
    QHash<QString, const TagMoveItem*> rows;
    for (const TagMoveItem& item : preview.items) {
        if (rows.contains(key(item.source))) {
            prepared.errors.append(QStringLiteral("The retained preview has duplicate sources."));
            return prepared;
        }
        rows.insert(key(item.source), &item);
    }
    QSet<QString> requested;
    for (const QString& source : includedSources) {
        if (!QDir::isAbsolutePath(source) || requested.contains(key(source))
                || !rows.contains(key(source))) {
            prepared.errors.append(QStringLiteral("The included selection differs from the retained preview."));
            return prepared;
        }
        requested.insert(key(source));
        if (rows.value(key(source))->state != TagMoveState::Proposed) {
            prepared.errors.append(QStringLiteral("Only reviewed, unambiguous proposals may be included."));
            return prepared;
        }
    }
    QHash<QString, int> groupIndexes;
    for (const QString& source : includedSources) {
        if (cancelled && cancelled->load(std::memory_order_relaxed)) {
            prepared.cancelled = true;
            prepared.requests.clear();
            return prepared;
        }
        const TagMoveItem& item = *rows.value(key(source));
        QString reason;
        if (!stillReady(item, &reason)) {
            prepared.skipped.append({item.source, reason});
            continue;
        }
        const QString rootKey = key(item.destinationDirectory);
        int index = groupIndexes.value(rootKey, -1);
        if (index < 0) {
            filemanager::files::TransferRequest request;
            request.destinationDirectory = item.destinationDirectory;
            request.reviewedDestination = item.destinationSnapshot;
            request.operation = filemanager::files::TransferOperation::Move;
            request.reviewedMove = true;
            request.reviewedSkipStaleItems = true;
            index = static_cast<int>(prepared.requests.size());
            groupIndexes.insert(rootKey, index);
            prepared.requests.append(std::move(request));
        } else if (prepared.requests.at(index).reviewedDestination
                != item.destinationSnapshot) {
            prepared.skipped.append({item.source,
                QStringLiteral("Destination identity differs within one reviewed group.")});
            continue;
        }
        auto& request = prepared.requests[index];
        request.sources.append(item.source);
        request.reviewedMoves.append({item.source, item.destination,
            item.sourceSnapshot});
    }
    return prepared;
}

} // namespace filemanager::tags
