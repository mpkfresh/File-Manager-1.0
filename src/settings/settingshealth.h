#pragma once

#include <QStringList>

class QSettings;

namespace filemanager::settings {

struct SettingsHealthReport
{
    QStringList resetCategories;

    [[nodiscard]] bool repaired() const { return !resetCategories.isEmpty(); }
    [[nodiscard]] int resetCount() const { return resetCategories.size(); }
};

SettingsHealthReport repairSettings(QSettings& settings);
SettingsHealthReport repairSettings();

} // namespace filemanager::settings
