#include "settings/startuppreferences.h"

#include <QDir>
#include <QFileInfo>
#include <QSettings>

namespace filemanager::settings {
namespace {

constexpr auto HomeIdentity = "filemanager:home";
constexpr auto ThisPcIdentity = "filemanager:shell:this-pc";
constexpr auto SettingKey = "navigation/openTo";

QString storedValue(StartupDestination destination)
{
    switch (destination) {
    case StartupDestination::ThisPc:
        return QStringLiteral("this-pc");
    case StartupDestination::LastFolder:
        return QStringLiteral("last-folder");
    case StartupDestination::Home:
        return QStringLiteral("home");
    }
    return QStringLiteral("home");
}

} // namespace

StartupDestination loadStartupDestination(const QSettings& settings)
{
    const QString value = settings.value(SettingKey, QStringLiteral("home")).toString();
    if (value == QLatin1String("this-pc"))
        return StartupDestination::ThisPc;
    if (value == QLatin1String("last-folder"))
        return StartupDestination::LastFolder;
    return StartupDestination::Home;
}

void saveStartupDestination(QSettings& settings, StartupDestination destination)
{
    settings.setValue(SettingKey, storedValue(destination));
}

QString startupLocation(const QSettings& settings)
{
    switch (loadStartupDestination(settings)) {
    case StartupDestination::ThisPc:
        return QLatin1String(ThisPcIdentity);
    case StartupDestination::LastFolder: {
        const QString candidate = settings.value("navigation/recents").toStringList().value(0);
        const QString path = QDir::cleanPath(QDir::fromNativeSeparators(candidate));
        if (QDir::isAbsolutePath(path) && QFileInfo(path).isDir())
            return path;
        return QLatin1String(HomeIdentity);
    }
    case StartupDestination::Home:
        return QLatin1String(HomeIdentity);
    }
    return QLatin1String(HomeIdentity);
}

} // namespace filemanager::settings
