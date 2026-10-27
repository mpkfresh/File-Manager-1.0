#pragma once

#include <QFileInfo>
#include <QString>
#include <QStringList>

#include <atomic>
#include <functional>

namespace filemanager::files {

struct ScopeEnumerationResult
{
    bool cancelled = false;
    int skippedBoundaries = 0;
    QString error;
    qsizetype entriesVisited = 0;
    bool limitReached = false;
};

struct ScopeEnumerationOptions
{
    QStringList excludedDirectories;
    qsizetype maxEntries = 0; // Zero preserves the existing unbounded callers.
};

// Read-only boundary check; does not pin directories against concurrent changes.
bool isRegularDirectoryChain(QString path, bool allowMissing = false);

ScopeEnumerationResult enumerateRegularTree(const QString& root, bool includeDirectories,
    const std::atomic_bool* cancelled, const std::function<bool(const QFileInfo&)>& visitor,
    const ScopeEnumerationOptions& options = {});

} // namespace filemanager::files
