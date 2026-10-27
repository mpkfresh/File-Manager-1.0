#pragma once

#include <QString>

namespace filemanager::platform {

enum class KnownFolder { Desktop, Documents, Downloads, Music, Pictures, Videos };

QString knownFolderPath(KnownFolder folder);

} // namespace filemanager::platform
