#pragma once

#include "platform/wineintegration.h"
#include <QCoreApplication>
#include <QDir>
#include <QSettings>
#include <QUuid>
#include <cstdio>
#ifdef Q_OS_WIN
#include <windows.h>
#endif

inline bool runWinEIntegrationTests()
{
#ifdef Q_OS_WIN
    using namespace filemanager::platform;
    const QString leaf = "Software\\FileManagerTests\\WinE_"
        + QUuid::createUuid().toString(QUuid::Id128);
    HKEY key = nullptr;
    DWORD disposition = 0;
    const auto created = RegCreateKeyExW(HKEY_CURRENT_USER,
        reinterpret_cast<LPCWSTR>(leaf.utf16()), 0, nullptr, 0, KEY_ALL_ACCESS,
        nullptr, &key, &disposition);
    if (key) RegCloseKey(key);
    if (created != ERROR_SUCCESS || disposition != REG_CREATED_NEW_KEY) {
        std::fprintf(stderr,
            "Win+E registry fixture could not claim a new disposable key "
            "(RegCreateKeyExW=%ld, disposition=%lu).\n", created, disposition);
        return false;
    }
    struct Cleanup {
        QString leaf;
        ~Cleanup() { RegDeleteTreeW(HKEY_CURRENT_USER, reinterpret_cast<LPCWSTR>(leaf.utf16())); }
    } cleanup{leaf};
    const QString root = "HKEY_CURRENT_USER\\" + leaf;
    const QString app = QCoreApplication::applicationFilePath();
    QSettings startup(root + "\\Run", QSettings::NativeFormat);
    startup.setFallbacksEnabled(false);
    constexpr auto name = "Kiatami.FileManager.WinE";
    bool passed = true;
    const auto check = [&passed](bool condition, const char* label) {
        if (!condition) { std::fprintf(stderr, "Win+E registry: %s\n", label); passed = false; }
    };
    const auto initial = winEStatus(app, root);
    check(initial.manageable && !initial.enabled && !initial.atSignIn,
        "disposable settings start disabled");
    HKEY unreadConsent = nullptr;
    const QString consentLeaf = leaf + "\\Consent";
    const auto consentRead = RegOpenKeyExW(HKEY_CURRENT_USER,
        reinterpret_cast<LPCWSTR>(consentLeaf.utf16()), 0, KEY_READ, &unreadConsent);
    if (unreadConsent) RegCloseKey(unreadConsent);
    check(consentRead == ERROR_FILE_NOT_FOUND || consentRead == ERROR_PATH_NOT_FOUND,
        "reading disabled shortcut status must not create a consent key");
    {
        QSettings consent(root + "\\Consent", QSettings::NativeFormat);
        consent.setFallbacksEnabled(false);
        consent.setValue("ownerPath", QDir::toNativeSeparators(app));
        consent.setValue("enabled", "corrupt permission");
        consent.sync();
        check(consent.status() == QSettings::NoError,
            "malformed-permission fixture must be written successfully");
        check(!winEStatus(app, root).enabled, "malformed permission values fail closed");
        consent.clear();
        consent.sync();
    }
    check(!setWinEIntegration(app, true, true, root + "\\invalid").success,
        "fixture scope cannot escape the exact disposable root");
    check(setWinEIntegration(app, true, false, root).success,
        "permission can be enabled without sign-in startup");
    check(winEStatus(app, root).enabled && !winEStatus(app, root).atSignIn,
        "startup is separately optional");
    check(setWinEIntegration(app, true, true, root).success,
        "sign-in registers only the fixture Run entry");
    startup.sync();
    const QString ownedCommand = startup.value(name).toString();
    check(ownedCommand.endsWith("\" --win-e-helper") && ownedCommand.startsWith('"')
        && winEStatus(app, root).atSignIn, "helper launch is quoted and uses its exact mode");
    check(setWinEIntegration(app, true, true, root).success,
        "applying the same configuration again is safe");
    check(setWinEIntegration(app, false, false, root).success,
        "revocation removes its owned startup value");
    startup.sync();
    {
        QSettings consent(root + "\\Consent", QSettings::NativeFormat);
        consent.setFallbacksEnabled(false);
        check(!startup.contains(name) && consent.allKeys().isEmpty(),
            "revocation clears only owned consent and startup state");
    }
    startup.setValue(name, "foreign command");
    startup.setValue("UnrelatedApplication", "preserve this");
    startup.sync();
    check(!setWinEIntegration(app, true, true, root).success,
        "another application's startup value is never replaced");
    check(setWinEIntegration(app, true, false, root).success,
        "foreign startup does not prevent session-only permission");
    check(setWinEIntegration(app, false, false, root).success,
        "foreign startup does not prevent revocation");
    startup.sync();
    check(startup.value(name).toString() == "foreign command"
        && startup.value("UnrelatedApplication").toString() == "preserve this",
        "revocation preserves foreign and unrelated startup values");
    // Revocation deletes the key: reopen it instead of writing through a cached HKEY.
    const QString foreignOwner = "C:\\AnotherInstallation\\FileManager.exe";
    {
        QSettings consent(root + "\\Consent", QSettings::NativeFormat);
        consent.setFallbacksEnabled(false);
        consent.setValue("ownerPath", foreignOwner);
        consent.setValue("enabled", true);
        consent.sync();
        check(consent.status() == QSettings::NoError,
            "foreign-owner fixture must be written successfully");
    }
    {
        QSettings verify(root + "\\Consent", QSettings::NativeFormat);
        verify.setFallbacksEnabled(false);
        const bool fixtureReady = verify.value("ownerPath").toString() == foreignOwner
            && verify.value("enabled").toBool() && verify.status() == QSettings::NoError;
        check(fixtureReady, "foreign-owner fixture must be readable before exercising protection");
        if (!fixtureReady) return false;
    }
    check(!winEStatus(app, root).manageable
        && !setWinEIntegration(app, false, false, root).success,
        "another installation's consent cannot be rewritten");
    {
        QSettings consent(root + "\\Consent", QSettings::NativeFormat);
        consent.setFallbacksEnabled(false);
        check(consent.value("enabled").toBool()
            && consent.value("ownerPath").toString() == foreignOwner,
            "foreign consent remains unchanged");
    }
    return passed;
#else
    return true;
#endif
}
