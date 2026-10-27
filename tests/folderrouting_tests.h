#pragma once
#include "platform/folderrouting.h"
#include <QCoreApplication>
#include <QJsonDocument>
#include <QJsonObject>
#include <QUuid>
#include <cstdio>
#ifdef Q_OS_WIN
#include <windows.h>
#endif

inline bool runFolderRoutingTests()
{
#ifdef Q_OS_WIN
    using namespace filemanager::platform;
    const QString root = "Software\\FileManagerTests\\FolderRouting_"
        + QUuid::createUuid().toString(QUuid::Id128);
    const auto wide = [](const QString& value) { return reinterpret_cast<LPCWSTR>(value.utf16()); };
    HKEY initial = nullptr;
    DWORD disposition = 0;
    const auto claimed = RegCreateKeyExW(HKEY_CURRENT_USER, wide(root), 0, nullptr, 0,
        KEY_ALL_ACCESS, nullptr, &initial, &disposition);
    if (initial) RegCloseKey(initial);
    if (claimed != ERROR_SUCCESS || disposition != REG_CREATED_NEW_KEY) return false;
    struct Cleanup {
        QString root;
        ~Cleanup() { RegDeleteTreeW(HKEY_CURRENT_USER, reinterpret_cast<LPCWSTR>(root.utf16())); }
    } cleanup{root};
    bool passed = true;
    const auto check = [&passed](bool result, const char* label) {
        if (!result) { std::fprintf(stderr, "Folder routing: %s\n", label); passed = false; }
    };
    const auto put = [&](const QString& relative, const QString& name, DWORD type, const QByteArray& bytes) {
        HKEY key = nullptr;
        const QString path = root + '\\' + relative;
        if (RegCreateKeyExW(HKEY_CURRENT_USER, wide(path), 0, nullptr, 0,
            KEY_SET_VALUE, nullptr, &key, nullptr) != ERROR_SUCCESS) return false;
        const auto result = RegSetValueExW(key, wide(name), 0, type,
            reinterpret_cast<const BYTE*>(bytes.constData()), static_cast<DWORD>(bytes.size()));
        RegCloseKey(key);
        return result == ERROR_SUCCESS;
    };
    const auto read = [&](const QString& relative, const QString& name, DWORD* type) {
        HKEY key = nullptr;
        const QString path = root + '\\' + relative;
        if (RegOpenKeyExW(HKEY_CURRENT_USER, wide(path), 0, KEY_QUERY_VALUE, &key) != ERROR_SUCCESS)
            return QByteArray{};
        DWORD size = 0;
        if (RegQueryValueExW(key, wide(name), nullptr, type, nullptr, &size) != ERROR_SUCCESS || size > 1024 * 1024) {
            RegCloseKey(key); return QByteArray{};
        }
        QByteArray bytes(size, Qt::Uninitialized);
        const auto result = RegQueryValueExW(key, wide(name), nullptr, type,
            reinterpret_cast<BYTE*>(bytes.data()), &size);
        RegCloseKey(key);
        return result == ERROR_SUCCESS ? bytes : QByteArray{};
    };
    const auto stringBytes = [](const QString& value) {
        return QByteArray(reinterpret_cast<const char*>(value.utf16()),
            (value.size() + 1) * static_cast<qsizetype>(sizeof(wchar_t)));
    };
    const QString app = QCoreApplication::applicationFilePath();
    const QString directory = "Classes\\Directory\\shell";
    const QString drive = "Classes\\Drive\\shell";
    const QString handler = directory + "\\FileManager.DefaultFolder";
    const auto off = folderRoutingStatus(app, root);
    check(off.state == FolderRoutingState::Off && off.canEnable, "new fixture starts off");
    check(!setFolderRouting(true, app, root + "\\escape").success, "fixture scope rejects descendants");
    check(folderRoutingCommand(app).contains("--open-folder -- \"%1\\.\""), "command preserves strict mode and drive-root quoting");

    // Use a non-string raw default: recovery must preserve type and embedded NULs exactly.
    const QByteArray prior = QByteArray::fromHex("0001ff000302");
    check(put(directory, {}, REG_BINARY, prior), "raw prior default setup");
    const auto enabled = setFolderRouting(true, app, root);
    if (!enabled.success) std::fprintf(stderr, "Folder routing enable: %s\n", qPrintable(enabled.error));
    check(enabled.success && folderRoutingStatus(app, root).state == FolderRoutingState::Enabled,
        "enable registers both defaults with an owned recovery record");
    check(setFolderRouting(true, app, root).success, "enabling an already active installation is idempotent");
    const QString other = "C:/Other File Manager/FileManager.exe";
    check(folderRoutingStatus(other, root).state == FolderRoutingState::OtherInstallation
            && !setFolderRouting(false, other, root).success, "another installation cannot restore this record");
    check(setFolderRouting(false, app, root).success, "disable restores owned defaults");
    DWORD type = 0;
    check(read(directory, {}, &type) == prior && type == REG_BINARY, "restore retains exact raw type and bytes");
    HKEY missing = nullptr;
    const auto absent = RegOpenKeyExW(HKEY_CURRENT_USER, wide(root + '\\' + drive), 0, KEY_READ, &missing);
    if (missing) RegCloseKey(missing);
    check(absent == ERROR_FILE_NOT_FOUND, "new drive shell keys are removed after absence restoration");

    check(setFolderRouting(true, app, root).success, "second independent fixture transaction enables");
    check(put(directory, {}, REG_SZ, stringBytes("AnotherApplication")), "later choice setup");
    check(!setFolderRouting(false, app, root).success, "later default blocks destructive restoration");
    check(read(directory, {}, &type) == stringBytes("AnotherApplication"), "later application choice remains unchanged");
    check(!read(handler + "\\command", {}, &type).isEmpty(), "handler retained while a default conflicts");
    check(folderRoutingStatus(app, root).state == FolderRoutingState::RecoveryNeeded, "conflict retains a recoverable state");
    check(put(directory, {}, REG_SZ, stringBytes("FileManager.DefaultFolder")), "return owned fixture default for cleanup");
    check(setFolderRouting(false, app, root).success, "partially restored state completes when conflict is resolved");

    check(setFolderRouting(true, app, root).success, "interruption fixture enables");
    auto journal = QJsonDocument::fromJson(read("Journal", "Journal", &type)).object();
    journal.insert("phase", "applying");
    check(put("Journal", "Journal", REG_BINARY, QJsonDocument(journal).toJson(QJsonDocument::Compact)), "persist interrupted phase");
    check(put(directory, {}, REG_BINARY, prior), "simulate one default never applied/already restored");
    check(folderRoutingStatus(app, root).state == FolderRoutingState::RecoveryNeeded
            && setFolderRouting(false, app, root).success, "interrupted apply restores remaining values without guessing");

    check(put(handler, "Foreign", REG_SZ, stringBytes("keep")), "foreign handler fixture");
    check(!setFolderRouting(true, app, root).success
        && read(handler, "Foreign", &type) == stringBytes("keep"), "foreign handler cannot be adopted or removed");
    check(put("Journal", "Journal", REG_BINARY, "not-json"), "corrupt record fixture");
    check(folderRoutingStatus(app, root).state == FolderRoutingState::Unavailable
        && !setFolderRouting(false, app, root).success, "corrupt recovery record fails closed");
    check(read(directory, {}, &type) == prior && type == REG_BINARY, "invalid recovery never rewrites prior defaults");
    return passed;
#else
    return true;
#endif
}
