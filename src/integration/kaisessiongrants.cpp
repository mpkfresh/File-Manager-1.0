#include "kaisessiongrants.h"

#include <QDir>
#include <QFileInfo>

#ifdef Q_OS_WIN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace filemanager::integration
{
namespace
{

QString normalizedFolder(const QString& folder)
{
    if (!QDir::isAbsolutePath(folder))
        return {};
    const QString normalized =
        QDir::fromNativeSeparators(QDir::cleanPath(QFileInfo(folder).absoluteFilePath()));
    if (normalized.startsWith(QStringLiteral("//?/")) ||
        normalized.startsWith(QStringLiteral("//./")) || normalized.size() > 32767)
        return {};
    return normalized;
}

bool safeDirectoryIdentity(const files::SourceSnapshot& snapshot)
{
#ifdef Q_OS_WIN
    return snapshot.valid && (snapshot.attributes & FILE_ATTRIBUTE_DIRECTORY) != 0 &&
           (snapshot.attributes & FILE_ATTRIBUTE_REPARSE_POINT) == 0;
#else
    Q_UNUSED(snapshot);
    return false;
#endif
}

bool sameDirectory(const files::SourceSnapshot& left, const files::SourceSnapshot& right)
{
    return safeDirectoryIdentity(left) && safeDirectoryIdentity(right) &&
           left.volumeSerial == right.volumeSerial && left.fileIndex == right.fileIndex;
}

} // namespace

void KaiSessionGrants::setEnabled(bool enabled)
{
    if (!enabled)
        grants_.clear();
    enabled_ = enabled;
}

bool KaiSessionGrants::enabled() const { return enabled_; }

bool KaiSessionGrants::grant(const QString& folder, KaiCapability capability, QString* error)
{
    if (error)
        error->clear();
    if (!enabled_)
    {
        if (error)
            *error = QStringLiteral("Kai UI integration is off.");
        return false;
    }
    const QString path = normalizedFolder(folder);
    if (path.isEmpty())
    {
        if (error)
            *error = QStringLiteral("The selected folder is not an absolute path.");
        return false;
    }
    const auto identity = files::captureSourceSnapshot(path);
    if (!safeDirectoryIdentity(identity))
    {
        if (error)
            *error = QStringLiteral("The selected folder is not a safe existing directory.");
        return false;
    }
    const QString key = path.toCaseFolded();
    auto current = grants_.value(key);
    if (!current.folder.isEmpty() && !sameDirectory(current.identity, identity))
    {
        if (error)
            *error = QStringLiteral("The selected folder changed since consent.");
        grants_.remove(key);
        return false;
    }
    current.folder = path;
    current.identity = identity;
    if (capability == KaiCapability::ReadNavigation)
        current.read = true;
    else
        current.createOnly = true;
    grants_.insert(key, current);
    return true;
}

bool KaiSessionGrants::allows(const QString& folder, KaiCapability capability) const
{
    if (!enabled_)
        return false;
    const QString path = normalizedFolder(folder);
    if (path.isEmpty())
        return false;
    const auto entry = grants_.constFind(path.toCaseFolded());
    if (entry == grants_.cend() ||
        !sameDirectory(entry->identity, files::captureSourceSnapshot(path)))
        return false;
    return capability == KaiCapability::ReadNavigation ? entry->read : entry->createOnly;
}

void KaiSessionGrants::revoke(const QString& folder, KaiCapability capability)
{
    const QString path = normalizedFolder(folder);
    if (path.isEmpty())
        return;
    auto entry = grants_.find(path.toCaseFolded());
    if (entry == grants_.end())
        return;
    if (capability == KaiCapability::ReadNavigation)
        entry->read = false;
    else
        entry->createOnly = false;
    if (!entry->read && !entry->createOnly)
        grants_.erase(entry);
}

void KaiSessionGrants::revokeAll() { grants_.clear(); }

} // namespace filemanager::integration
