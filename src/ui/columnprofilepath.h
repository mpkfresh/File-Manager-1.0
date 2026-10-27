#pragma once

#include <QDir>

namespace filemanager::ui {

// Lexical folder classification for presentation preferences, not a filesystem
// access boundary. Both paths come from resolved absolute navigation locations.
inline bool columnProfileContainsPath(const QString& root, const QString& path)
{
    if (root.isEmpty() || path.isEmpty()
            || !QDir::isAbsolutePath(root) || !QDir::isAbsolutePath(path))
        return false;
    const QString relative = QDir(root).relativeFilePath(QDir::fromNativeSeparators(path));
    // Different Windows drives can produce an absolute path here, not "../...".
    return QDir::isRelativePath(relative)
        && (relative == "." || (relative != ".." && !relative.startsWith("../")));
}

} // namespace filemanager::ui
