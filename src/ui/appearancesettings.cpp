#include "appearancesettings.h"

#include <QSettings>

namespace filemanager::ui {

AppearanceSettings loadAppearanceSettings()
{
    QSettings settings;
    AppearanceSettings result;
    result.fontPointSize = qBound<qreal>(8.0,
        settings.value("appearance/fontSize", 9.5).toDouble(), 18.0);
    result.density = settings.value("appearance/density", "Comfortable").toString();
    if (!QStringList{"Compact", "Comfortable", "Spacious"}.contains(result.density))
        result.density = "Comfortable";
    result.animations = settings.value("appearance/animations", true).toBool();
    return result;
}

void saveAppearanceSettings(const AppearanceSettings& settings)
{
    QSettings storage;
    storage.setValue("appearance/fontSize", qBound<qreal>(8.0, settings.fontPointSize, 18.0));
    storage.setValue("appearance/density", settings.density);
    storage.setValue("appearance/animations", settings.animations);
}

} // namespace filemanager::ui
