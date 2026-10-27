#include "platform/knownfolders.h"

#include <QStandardPaths>

#ifdef Q_OS_WIN
#include <windows.h>
#include <shlobj.h>
#endif

namespace filemanager::platform {

QString knownFolderPath(KnownFolder folder)
{
#ifdef Q_OS_WIN
    const KNOWNFOLDERID* id = nullptr;
    switch (folder) {
    case KnownFolder::Desktop: id = &FOLDERID_Desktop; break;
    case KnownFolder::Documents: id = &FOLDERID_Documents; break;
    case KnownFolder::Downloads: id = &FOLDERID_Downloads; break;
    case KnownFolder::Music: id = &FOLDERID_Music; break;
    case KnownFolder::Pictures: id = &FOLDERID_Pictures; break;
    case KnownFolder::Videos: id = &FOLDERID_Videos; break;
    }
    PWSTR nativePath = nullptr;
    const HRESULT result = SHGetKnownFolderPath(*id, KF_FLAG_DEFAULT, nullptr, &nativePath);
    if (SUCCEEDED(result) && nativePath) {
        const QString path = QString::fromWCharArray(nativePath);
        CoTaskMemFree(nativePath);
        return path;
    }
    if (nativePath)
        CoTaskMemFree(nativePath);
#endif
    QStandardPaths::StandardLocation fallback = QStandardPaths::HomeLocation;
    switch (folder) {
    case KnownFolder::Desktop: fallback = QStandardPaths::DesktopLocation; break;
    case KnownFolder::Documents: fallback = QStandardPaths::DocumentsLocation; break;
    case KnownFolder::Downloads: fallback = QStandardPaths::DownloadLocation; break;
    case KnownFolder::Music: fallback = QStandardPaths::MusicLocation; break;
    case KnownFolder::Pictures: fallback = QStandardPaths::PicturesLocation; break;
    case KnownFolder::Videos: fallback = QStandardPaths::MoviesLocation; break;
    }
    return QStandardPaths::writableLocation(fallback);
}

} // namespace filemanager::platform
