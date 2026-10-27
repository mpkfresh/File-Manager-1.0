#pragma once

#include <QString>

namespace filemanager::platform {

struct WinEStatus {
    bool enabled = false;
    bool atSignIn = false;
    bool manageable = true;
    QString error;
};
struct WinEResult { bool success = false; QString error; };

// Nonempty fixture roots must be disposable WinE_<32 hex> trees under FileManagerTests.
// Fixture operations never launch a helper, install a hook, or touch the real Run key.
WinEStatus winEStatus(const QString& applicationPath, const QString& fixtureRoot = {});
WinEResult setWinEIntegration(const QString& applicationPath, bool enabled, bool atSignIn,
    const QString& fixtureRoot = {});
WinEResult startConsentedWinEHelper(const QString& applicationPath);
int runWinEHelper(const QString& applicationPath);

} // namespace filemanager::platform
