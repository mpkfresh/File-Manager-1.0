#include "windowswindowchrome.h"

#include <QWidget>

#ifdef Q_OS_WIN
#include <windows.h>
#include <dwmapi.h>
#endif

namespace filemanager::platform {

void applyWindowChrome(QWidget* window)
{
#ifdef Q_OS_WIN
    if (!window)
        return;
    const HWND handle = reinterpret_cast<HWND>(window->winId());
    constexpr DWORD UseImmersiveDarkMode = 20;
    constexpr DWORD WindowCornerPreference = 33;
    constexpr DWORD SystemBackdropType = 38;
    const BOOL dark = TRUE;
    const DWORD rounded = 2; // DWMWCP_ROUND
    const DWORD backdrop = 2; // DWMSBT_MAINWINDOW
    DwmSetWindowAttribute(handle, UseImmersiveDarkMode, &dark, sizeof(dark));
    DwmSetWindowAttribute(handle, WindowCornerPreference, &rounded, sizeof(rounded));
    DwmSetWindowAttribute(handle, SystemBackdropType, &backdrop, sizeof(backdrop));
#else
    Q_UNUSED(window);
#endif
}

} // namespace filemanager::platform
