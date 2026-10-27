#pragma once

#include <QString>

namespace filemanager::ui {

struct AppearanceSettings
{
    qreal fontPointSize = 9.5;
    QString density = "Comfortable";
    bool animations = true;
};

AppearanceSettings loadAppearanceSettings();
void saveAppearanceSettings(const AppearanceSettings& settings);

} // namespace filemanager::ui
