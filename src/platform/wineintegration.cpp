#include "wineintegration.h"
#include "winekeystate.h"

#include <QCryptographicHash>
#include <QDir>
#include <QFileInfo>
#include <QProcess>
#include <QSettings>
#include <vector>
#include <algorithm>

#ifdef Q_OS_WIN
#include <windows.h>
#include <sddl.h>
#endif

namespace filemanager::platform {
namespace {
constexpr auto consentRoot = "HKEY_CURRENT_USER\\Software\\Kiatami\\File Manager\\WinEIntegration";
constexpr auto runRoot = "HKEY_CURRENT_USER\\Software\\Microsoft\\Windows\\CurrentVersion\\Run";
constexpr auto runValue = "Kiatami.FileManager.WinE";

#ifdef Q_OS_WIN
enum class RegistryKeyState { Missing, Present, Unreadable };

RegistryKeyState registryKeyState(const QString& root)
{
    constexpr auto prefix = "HKEY_CURRENT_USER\\";
    if (!root.startsWith(QLatin1String(prefix))) return RegistryKeyState::Unreadable;
    const QString relative = root.mid(sizeof("HKEY_CURRENT_USER\\") - 1);
    HKEY key = nullptr;
    const LSTATUS result = RegOpenKeyExW(HKEY_CURRENT_USER,
        reinterpret_cast<LPCWSTR>(relative.utf16()), 0, KEY_READ, &key);
    if (result == ERROR_FILE_NOT_FOUND || result == ERROR_PATH_NOT_FOUND)
        return RegistryKeyState::Missing;
    if (result != ERROR_SUCCESS) return RegistryKeyState::Unreadable;
    RegCloseKey(key);
    return RegistryKeyState::Present;
}
#endif

bool validFixtureRoot(const QString& root)
{
    if (root.isEmpty()) return true;
    const QString prefix = "HKEY_CURRENT_USER\\Software\\FileManagerTests\\WinE_";
    if (!root.startsWith(prefix)) return false;
    const QByteArray suffix = root.mid(prefix.size()).toLatin1();
    return suffix.size() == 32 && std::all_of(suffix.cbegin(), suffix.cend(), [](char c) {
        return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
    });
}

QString normalized(const QString& path)
{
    return QDir::toNativeSeparators(QFileInfo(path).absoluteFilePath());
}
QString helperPath(const QString& applicationPath)
{
    return QDir(QFileInfo(applicationPath).absolutePath()).filePath("FileManagerMaintenance.exe");
}
QString startupCommand(const QString& applicationPath)
{
    return '"' + normalized(helperPath(applicationPath)) + "\" --win-e-helper";
}
bool ownsConsent(QSettings& settings, const QString& applicationPath)
{
    return settings.value("ownerPath").toString().compare(normalized(applicationPath),
        Qt::CaseInsensitive) == 0;
}
bool permissionEnabled(QSettings& settings)
{
    const QString value = settings.value("enabled").toString();
    return value == "true" || value == "1";
}
bool consented(const QString& applicationPath)
{
#ifdef Q_OS_WIN
    // QSettings can create an empty native key when it is opened for a read.
    // Merely launching File Manager must not create an integration key.
    if (registryKeyState(QString::fromLatin1(consentRoot)) != RegistryKeyState::Present)
        return false;
#endif
    QSettings settings(consentRoot, QSettings::NativeFormat);
    settings.setFallbacksEnabled(false);
    return ownsConsent(settings, applicationPath)
        && permissionEnabled(settings) && settings.status() == QSettings::NoError;
}

#ifdef Q_OS_WIN
class Handle final {
public:
    explicit Handle(HANDLE handle = nullptr) : value(handle) {}
    ~Handle() { if (value) CloseHandle(value); }
    Handle(const Handle&) = delete;
    Handle& operator=(const Handle&) = delete;
    HANDLE value;
};
QString objectPrefix(const QString& applicationPath)
{
    HANDLE rawToken = nullptr;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &rawToken)) return {};
    Handle token(rawToken);
    DWORD size = 0;
    GetTokenInformation(token.value, TokenUser, nullptr, 0, &size);
    if (!size || size > 65536) return {};
    std::vector<unsigned char> buffer(size);
    if (!GetTokenInformation(token.value, TokenUser, buffer.data(), size, &size)) return {};
    LPWSTR sid = nullptr;
    if (!ConvertSidToStringSidW(reinterpret_cast<TOKEN_USER*>(buffer.data())->User.Sid, &sid))
        return {};
    const QString user = QString::fromWCharArray(sid);
    LocalFree(sid);
    const QString installation = applicationPath.isEmpty()
        ? QStringLiteral("shared-settings") : normalized(applicationPath).toCaseFolded();
    const QByteArray identity = (user + '|' + installation).toUtf8();
    return "Local\\Kiatami.FileManager.WinE." + QString::fromLatin1(
        QCryptographicHash::hash(identity, QCryptographicHash::Sha256).toHex());
}
HANDLE createEvent(const QString& name, bool manual)
{
    return CreateEventW(nullptr, manual, FALSE, reinterpret_cast<LPCWSTR>(name.utf16()));
}
WinEResult stopHelper(const QString& applicationPath)
{
    const QString prefix = objectPrefix(applicationPath);
    if (prefix.isEmpty()) return {false, "The shortcut helper's user identity is unavailable."};
    const QString stopName = prefix + ".stop";
    Handle stop(OpenEventW(EVENT_MODIFY_STATE, FALSE,
        reinterpret_cast<LPCWSTR>(stopName.utf16())));
    if (stop.value) SetEvent(stop.value);
    const QString mutexName = prefix + ".instance";
    Handle instance(OpenMutexW(SYNCHRONIZE | MUTEX_MODIFY_STATE, FALSE,
        reinterpret_cast<LPCWSTR>(mutexName.utf16())));
    if (!instance.value) return {true, {}};
    const DWORD wait = WaitForSingleObject(instance.value, 2500);
    if (wait == WAIT_OBJECT_0 || wait == WAIT_ABANDONED) {
        ReleaseMutex(instance.value);
        return {true, {}};
    }
    return {false, "Shortcut permission was revoked, but helper shutdown could not be confirmed."};
}

thread_local WinEKeyState keyState;
thread_local bool launchQueued = false;
constexpr UINT launchMessage = WM_APP + 42;

bool markWindowsChordUsed()
{
    // Suppressing E leaves Windows seeing a lone Win press, which opens Start
    // when Win is released. F24 consumes the chord without typing a character.
    INPUT keys[2]{};
    keys[0].type = INPUT_KEYBOARD;
    keys[0].ki.wVk = VK_F24;
    keys[1] = keys[0];
    keys[1].ki.dwFlags = KEYEVENTF_KEYUP;
    const UINT sent = SendInput(2, keys, sizeof(INPUT));
    if (sent == 1) SendInput(1, &keys[1], sizeof(INPUT));
    return sent == 2;
}

LRESULT CALLBACK keyboardHook(int code, WPARAM message, LPARAM parameter)
{
    if (code != HC_ACTION) return CallNextHookEx(nullptr, code, message, parameter);
    const auto* key = reinterpret_cast<const KBDLLHOOKSTRUCT*>(parameter);
    const bool down = message == WM_KEYDOWN || message == WM_SYSKEYDOWN;
    const bool up = message == WM_KEYUP || message == WM_SYSKEYUP;
    if (!down && !up) return CallNextHookEx(nullptr, code, message, parameter);
    auto decision = keyState.update(key->vkCode, down, (key->flags & LLKHF_INJECTED) != 0);
    if (decision.launch) {
        // Only the opt-in physical Win+E path reaches this point. If input
        // injection fails, let E pass so Windows keeps its normal shortcut.
        if (!markWindowsChordUsed()) {
            keyState.abandonCapture();
            decision.suppress = false;
        } else if (!launchQueued) {
            if (PostThreadMessageW(GetCurrentThreadId(), launchMessage, 0, 0)) launchQueued = true;
            else { keyState.abandonCapture(); decision.suppress = false; }
        }
    }
    return decision.suppress ? 1 : CallNextHookEx(nullptr, code, message, parameter);
}
#endif
} // namespace

WinEStatus winEStatus(const QString& applicationPath, const QString& fixtureRoot)
{
#ifdef Q_OS_WIN
    if (!validFixtureRoot(fixtureRoot)) return {false, false, false, "Invalid Win+E fixture scope."};
    const QString consentPath = fixtureRoot.isEmpty() ? QString::fromLatin1(consentRoot)
        : fixtureRoot + "\\Consent";
    const QString runPath = fixtureRoot.isEmpty() ? QString::fromLatin1(runRoot)
        : fixtureRoot + "\\Run";
    const auto consentKey = registryKeyState(consentPath);
    const auto runKey = registryKeyState(runPath);
    if (consentKey == RegistryKeyState::Unreadable || runKey == RegistryKeyState::Unreadable)
        return {false, false, false, "Win+E settings could not be read."};

    bool enabled = false;
    if (consentKey == RegistryKeyState::Present) {
        QSettings settings(consentPath, QSettings::NativeFormat);
        settings.setFallbacksEnabled(false);
        const bool owned = ownsConsent(settings, applicationPath);
        if (!settings.allKeys().isEmpty() && !owned)
            return {false, false, false, "Win+E settings belong to another installation or need manual review."};
        if (settings.status() != QSettings::NoError)
            return {false, false, false, "Win+E settings could not be read."};
        enabled = owned && permissionEnabled(settings);
    }
    bool hasStartup = false;
    QString command;
    if (runKey == RegistryKeyState::Present) {
        QSettings run(runPath, QSettings::NativeFormat);
        run.setFallbacksEnabled(false);
        hasStartup = run.contains(runValue);
        command = run.value(runValue).toString();
        if (run.status() != QSettings::NoError)
            return {false, false, false, "Win+E settings could not be read."};
    }
    const bool foreignStartup = hasStartup && command != startupCommand(applicationPath);
    return {enabled, !foreignStartup && hasStartup, true,
        foreignStartup ? "The sign-in entry belongs to another command and will be preserved." : QString{}};
#else
    Q_UNUSED(applicationPath); Q_UNUSED(fixtureRoot);
    return {false, false, false, "Win+E integration is available only on Windows."};
#endif
}

WinEResult startConsentedWinEHelper(const QString& applicationPath)
{
#ifdef Q_OS_WIN
    if (!consented(applicationPath)) return {true, {}};
    const QString prefix = objectPrefix(applicationPath);
    if (prefix.isEmpty()) return {false, "The shortcut helper's user identity is unavailable."};
    Handle ready(createEvent(prefix + ".ready", true));
    if (!ready.value) return {false, "The shortcut helper's readiness event is unavailable."};
    // An active helper owns this event and sets it only after installing its hook.
    if (WaitForSingleObject(ready.value, 0) == WAIT_OBJECT_0) return {true, {}};
    if (!QProcess::startDetached(helperPath(applicationPath), {"--win-e-helper"},
            QFileInfo(applicationPath).absolutePath()))
        return {false, "The shortcut helper could not start."};
    if (WaitForSingleObject(ready.value, 3000) != WAIT_OBJECT_0)
        return {false, "The shortcut helper did not confirm that Win+E is ready."};
    return {true, {}};
#else
    Q_UNUSED(applicationPath);
    return {false, "Win+E integration is available only on Windows."};
#endif
}

WinEResult setWinEIntegration(const QString& applicationPath, bool enabled, bool atSignIn,
    const QString& fixtureRoot)
{
#ifdef Q_OS_WIN
    if (!validFixtureRoot(fixtureRoot)) return {false, "Invalid Win+E fixture scope."};
    const QFileInfo executable(applicationPath);
    if (!executable.isAbsolute() || applicationPath.contains('"'))
        return {false, "The application path is invalid."};
    // Serialize changes across app windows/installations for this user.
    const QString lockPrefix = objectPrefix({});
    if (lockPrefix.isEmpty()) return {false, "The shortcut helper's user identity is unavailable."};
    const QString lockName = lockPrefix + ".settings";
    Handle lock(CreateMutexW(nullptr, FALSE, reinterpret_cast<LPCWSTR>(lockName.utf16())));
    if (!lock.value) return {false, "Win+E settings could not be locked."};
    const DWORD acquired = WaitForSingleObject(lock.value, 3000);
    if (acquired != WAIT_OBJECT_0 && acquired != WAIT_ABANDONED)
        return {false, "Another window is updating Win+E settings."};
    struct Unlock { HANDLE value; ~Unlock() { ReleaseMutex(value); } } unlock{lock.value};
    const auto status = winEStatus(applicationPath, fixtureRoot);
    if (!status.manageable) return {false, status.error};
    if (enabled && (!executable.isFile() || !QFileInfo(helperPath(applicationPath)).isFile()))
        return {false, "The installed app or shortcut helper is unavailable."};
    QSettings settings(fixtureRoot.isEmpty() ? QString::fromLatin1(consentRoot)
        : fixtureRoot + "\\Consent", QSettings::NativeFormat);
    settings.setFallbacksEnabled(false);
    QSettings run(fixtureRoot.isEmpty() ? QString::fromLatin1(runRoot)
        : fixtureRoot + "\\Run", QSettings::NativeFormat);
    run.setFallbacksEnabled(false);
    const QString command = startupCommand(applicationPath);
    const bool foreignStartup = run.contains(runValue) && run.value(runValue).toString() != command;
    if (enabled && atSignIn && foreignStartup) return {false, status.error};

    // Revoke first, including on partial failure; helper polls this independently of stop signals.
    settings.setValue("ownerPath", normalized(applicationPath));
    settings.setValue("enabled", false);
    settings.sync();
    if (settings.status() != QSettings::NoError) {
        if (fixtureRoot.isEmpty()) stopHelper(applicationPath);
        return {false, "Win+E permission could not be saved."};
    }
    const auto stopped = fixtureRoot.isEmpty() ? stopHelper(applicationPath) : WinEResult{true, {}};
    if (!stopped.success) return stopped;
    if (!foreignStartup) {
        if (enabled && atSignIn) run.setValue(runValue, command);
        else run.remove(runValue);
        run.sync();
        if (run.status() != QSettings::NoError)
            return {false, "Win+E is disabled, but its sign-in setting could not be updated."};
    }
    if (!enabled) {
        settings.remove("");
        settings.sync();
        if (settings.status() != QSettings::NoError)
            return {false, "Win+E is disabled, but retained settings could not be cleared."};
        return {true, {}};
    }
    settings.setValue("enabled", true);
    settings.sync();
    WinEResult started = settings.status() == QSettings::NoError
        ? (fixtureRoot.isEmpty() ? startConsentedWinEHelper(applicationPath) : WinEResult{true, {}})
        : WinEResult{false, "Win+E permission could not be enabled."};
    if (!started.success) {
        settings.setValue("enabled", false);
        settings.sync();
        if (fixtureRoot.isEmpty()) stopHelper(applicationPath);
        if (!foreignStartup && run.value(runValue).toString() == command) {
            run.remove(runValue);
            run.sync();
        }
    }
    return started;
#else
    Q_UNUSED(applicationPath); Q_UNUSED(enabled); Q_UNUSED(atSignIn); Q_UNUSED(fixtureRoot);
    return {false, "Win+E integration is available only on Windows."};
#endif
}

int runWinEHelper(const QString& applicationPath)
{
#ifdef Q_OS_WIN
    if (!consented(applicationPath) || !QFileInfo(applicationPath).isFile()) return 0;
    const QString prefix = objectPrefix(applicationPath);
    if (prefix.isEmpty()) return 1;
    const QString instanceName = prefix + ".instance";
    Handle instance(CreateMutexW(nullptr, FALSE, reinterpret_cast<LPCWSTR>(instanceName.utf16())));
    if (!instance.value) return 2;
    const DWORD acquired = WaitForSingleObject(instance.value, 0);
    if (acquired != WAIT_OBJECT_0 && acquired != WAIT_ABANDONED) return 0;
    struct Unlock { HANDLE value; ~Unlock() { ReleaseMutex(value); } } unlock{instance.value};
    Handle stop(createEvent(prefix + ".stop", true));
    Handle ready(createEvent(prefix + ".ready", true));
    if (!stop.value || !ready.value) return 3;
    ResetEvent(stop.value);
    ResetEvent(ready.value);
    MSG message{};
    PeekMessageW(&message, nullptr, 0, 0, PM_NOREMOVE);
    keyState.reset();
    launchQueued = false;
    HHOOK hook = SetWindowsHookExW(WH_KEYBOARD_LL, keyboardHook, GetModuleHandleW(nullptr), 0);
    if (!hook) return 4;
    // Sample held modifiers outside the callback; low-level events use left/right key codes.
    for (const unsigned key : {0x5Bu, 0x5Cu, 0xA0u, 0xA1u, 0xA2u, 0xA3u, 0xA4u, 0xA5u, 0x45u})
        keyState.update(key, (GetAsyncKeyState(static_cast<int>(key)) & 0x8000) != 0);
    keyState.abandonCapture();
    if (!consented(applicationPath)) { UnhookWindowsHookEx(hook); return 0; }
    SetEvent(ready.value);
    bool running = true;
    while (running) {
        const DWORD wait = MsgWaitForMultipleObjects(1, &stop.value, FALSE, 250,
            QS_ALLINPUT);
        if (wait == WAIT_OBJECT_0 || wait == WAIT_FAILED || !consented(applicationPath)) break;
        // Bound each drain so revocation remains responsive during sustained keyboard input.
        for (int count = 0; count < 64 && PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE); ++count) {
            if (message.message == WM_QUIT) { running = false; break; }
            if (message.message == launchMessage) {
                launchQueued = false;
                if (!consented(applicationPath)) { running = false; break; }
                if (!QProcess::startDetached(applicationPath, {}, QFileInfo(applicationPath).absolutePath())) {
                    running = false;
                    break;
                }
            } else { TranslateMessage(&message); DispatchMessageW(&message); }
        }
    }
    ResetEvent(ready.value);
    UnhookWindowsHookEx(hook);
    keyState.reset();
    return 0;
#else
    Q_UNUSED(applicationPath);
    return 1;
#endif
}
} // namespace filemanager::platform
