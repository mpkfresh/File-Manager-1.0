#pragma once

#include <QString>

class QSettings;

namespace filemanager::settings {

enum class StartupDestination
{
    Home,
    ThisPc,
    LastFolder
};

StartupDestination loadStartupDestination(const QSettings& settings);
void saveStartupDestination(QSettings& settings, StartupDestination destination);
QString startupLocation(const QSettings& settings);

} // namespace filemanager::settings
