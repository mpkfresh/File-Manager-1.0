#pragma once

#include <QtGlobal>

class QApplication;

namespace filemanager::ui {

struct SystemAccessibilityPreferences
{
    bool highContrast = false;
    bool clientAreaAnimations = true;
};

SystemAccessibilityPreferences systemAccessibilityPreferences();

inline bool effectiveAnimationsEnabled(
    bool userEnabled, const SystemAccessibilityPreferences& system)
{
    return userEnabled && system.clientAreaAnimations && !system.highContrast;
}

inline qreal effectiveFontPointSize(qreal configured, qreal systemPointSize)
{
    return qMax(configured, systemPointSize);
}

void applyApplicationTheme(QApplication& application);
bool applicationAnimationsEnabled();
bool applicationHighContrastEnabled();

} // namespace filemanager::ui
