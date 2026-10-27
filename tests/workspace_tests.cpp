#include "workspace/workspaceview.h"
#include "winekeystate_tests.h"
#include "wineintegration_tests.h"
#include "folderrouting_tests.h"
#include "commands/commandregistry.h"
#include "ui/tabbedpanel.h"
#include "ui/filepane.h"
#include "ui/presentationtiming.h"
#include "ui/localizedformat.h"
#include "ui/columnprofilepath.h"
#include "search/searchservice.h"
#include "search/windowssearch.h"
#include "search/metadataindex.h"
#include "search/contentsearchservice.h"
#include "search/sessionsearchhistory.h"
#include "search/savedsearchstore.h"
#include "files/transfercontroller.h"
#include "files/transferqueue.h"
#include "files/operationjournal.h"
#include "files/sessionundo.h"
#include "files/analysisservice.h"
#include "files/duplicateservice.h"
#include "files/networkdiscoveryservice.h"
#include "files/ziparchive.h"
#include "tags/tagcatalog.h"
#include "files/scopeenumerator.h"
#include "preview/previewservice.h"
#include "preview/previewcontroller.h"
#include "preview/sandboxedpreviewprocess.h"
#include "preview/installcleanup.h"
#include "preview/thumbnailprovider.h"
#include "ui/localdirectorymodel.h"
#include "platform/directorywatcher.h"
#include <QPersistentModelIndex>
#include "preview/imagemetadataprovider.h"
#include "platform/fileicon.h"
#include "ui/fileiconimage.h"
#include "preview/documentmetadataprovider.h"
#include "preview/mediametadataprovider.h"
#include "preview/shellmetadataprovider.h"
#include "preview/cloudstatusprovider.h"
#include "settings/metadatapermissions.h"
#include "settings/shellpermissions.h"
#include "settings/contextmenupermissions.h"
#include "settings/sessionstore.h"
#include "settings/startuppreferences.h"
#include "platform/singleinstancecoordinator.h"
#include "platform/folderlaunch.h"
#include "platform/knownfolders.h"
#include "platform/shellfileoperation.h"
#include "platform/shellclipboard.h"
#include "platform/shellitem.h"
#include "platform/helperoutput.h"
#include "platform/helperinput.h"
#include "platform/nativefilecopy.h"
#include "platform/cloudfilestatus.h"

#include <iostream>
#include "platform/recyclebinoperation.h"
#include "platform/systemopen.h"
#include "platform/windowscontextmenu.h"
#include "platform/archivehandoff.h"
#include "platform/windowsshare.h"
#include "platform/windowsfolderintegration.h"
#include "ui/appearancesettings.h"
#include "ui/conflictdialog.h"
#include "ui/contentsearchdialog.h"
#include "ui/filedetailsdialog.h"
#include "ui/filesearchdialog.h"
#include "ui/operationcenterdialog.h"
#include "ui/shellnamespacebrowser.h"
#include "ui/searchresultbar.h"

#include <QApplication>
#include <QCheckBox>
#include <QClipboard>
#include <QComboBox>
#include <QCryptographicHash>
#include <QDebug>
#include <QDataStream>
#include <QDir>
#include <QEventLoop>
#include <QElapsedTimer>
#include <QDropEvent>
#include <QDragEnterEvent>
#include <QFile>
#include <QFileInfo>
#include <QFileSystemWatcher>
#include <QHeaderView>
#include <QImage>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QListView>
#include <QListWidget>
#include <QLineEdit>
#include <QLabel>
#include <QProcess>
#include <QPushButton>
#include <QSettings>
#include <QTimeZone>
#include <QStandardItemModel>
#include <QTemporaryDir>
#include <QMimeData>
#include <QTimer>
#include <QToolButton>
#include <QTreeView>
#include <QUuid>
#include <QSqlDatabase>
#include <QSqlQuery>
#include <QTableWidget>
#include <QTabWidget>
#include <QThread>
#include <QThreadPool>
#include <QtEndian>
#include <cstdio>
#include <cstddef>
#include <cstring>
#include <memory>
#include <utility>

#ifdef Q_OS_WIN
#include <windows.h>
#include <winioctl.h>
#include <ole2.h>
#include <shlobj.h>
#include <float.h>
#endif

namespace {

QByteArray helperOutputFixture()
{
    QByteArray bytes(2 * 1024 * 1024, Qt::Uninitialized);
    for (qsizetype index = 0; index < bytes.size(); ++index)
        bytes[index] = static_cast<char>(index % 251);
    return bytes;
}

#ifdef Q_OS_WIN
bool sameWindowsShellItem(const QByteArray& left, const QByteArray& right)
{
    if (!filemanager::platform::ShellItem::isValidSerializedIdentity(left)
            || !filemanager::platform::ShellItem::isValidSerializedIdentity(right))
        return false;
    const HRESULT initialized = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    if (FAILED(initialized))
        return false;
    IShellItem* first = nullptr;
    IShellItem* second = nullptr;
    int order = 1;
    const bool equal = SUCCEEDED(SHCreateItemFromIDList(
            reinterpret_cast<PCIDLIST_ABSOLUTE>(left.constData()), IID_PPV_ARGS(&first)))
        && SUCCEEDED(SHCreateItemFromIDList(
            reinterpret_cast<PCIDLIST_ABSOLUTE>(right.constData()), IID_PPV_ARGS(&second)))
        && first->Compare(second, SICHINT_CANONICAL, &order) == S_OK && order == 0;
    if (second) second->Release();
    if (first) first->Release();
    CoUninitialize();
    return equal;
}

unsigned iconFloatingControl()
{
    unsigned control = 0;
    return _controlfp_s(&control, 0, 0) == 0 ? control : 0xffffffffU;
}

QByteArray iconRasterHash(const QImage& image)
{
    QCryptographicHash hash(QCryptographicHash::Sha256);
    for (int y = 0; y < image.height(); ++y)
        hash.addData(QByteArrayView(reinterpret_cast<const char*>(image.constScanLine(y)), image.width() * 4));
    return hash.result().toHex();
}

class TemporaryRegistryTree final
{
public:
    TemporaryRegistryTree()
        : root(QString("Software\\FileManagerTests\\FolderIntegration\\%1")
              .arg(QUuid::createUuid().toString(QUuid::WithoutBraces)))
    {
    }

    ~TemporaryRegistryTree()
    {
        if (root.startsWith("Software\\FileManagerTests\\FolderIntegration\\"))
            RegDeleteTreeW(HKEY_CURRENT_USER,
                reinterpret_cast<const wchar_t*>(root.utf16()));
    }

    QString root;
};

bool registryKeyExists(const QString& path)
{
    HKEY key = nullptr;
    const LSTATUS status = RegOpenKeyExW(HKEY_CURRENT_USER,
        reinterpret_cast<const wchar_t*>(path.utf16()), 0, KEY_READ, &key);
    if (key)
        RegCloseKey(key);
    return status == ERROR_SUCCESS;
}

bool registryDefaultValueMissing(const QString& path)
{
    wchar_t value[2]{};
    DWORD bytes = sizeof(value);
    const LSTATUS status = RegGetValueW(HKEY_CURRENT_USER,
        reinterpret_cast<const wchar_t*>(path.utf16()), nullptr,
        RRF_RT_REG_SZ, nullptr, value, &bytes);
    return status == ERROR_FILE_NOT_FOUND;
}

bool registryNamedValueMissing(const QString& path, const wchar_t* name)
{
    DWORD bytes = 0;
    const LSTATUS status = RegGetValueW(HKEY_CURRENT_USER,
        reinterpret_cast<const wchar_t*>(path.utf16()), name,
        RRF_RT_ANY, nullptr, nullptr, &bytes);
    return status == ERROR_FILE_NOT_FOUND;
}

bool createJunction(const QString& linkPath, const QString& targetPath)
{
    if (!QDir().mkpath(linkPath))
        return false;
    const QString nativeLink = filemanager::platform::extendedLengthPath(linkPath);
    const HANDLE handle = CreateFileW(reinterpret_cast<LPCWSTR>(nativeLink.utf16()),
        GENERIC_WRITE, 0, nullptr, OPEN_EXISTING,
        FILE_FLAG_OPEN_REPARSE_POINT | FILE_FLAG_BACKUP_SEMANTICS, nullptr);
    if (handle == INVALID_HANDLE_VALUE) {
        QDir().rmdir(linkPath);
        return false;
    }
    const QString printName = QDir::toNativeSeparators(QFileInfo(targetPath).absoluteFilePath());
    const QString substituteName = QStringLiteral("\\??\\") + printName;
    const int substituteBytes = substituteName.size() * static_cast<int>(sizeof(wchar_t));
    const int printBytes = printName.size() * static_cast<int>(sizeof(wchar_t));
    struct MountPointBuffer {
        ULONG tag;
        USHORT dataLength;
        USHORT reserved;
        USHORT substituteOffset;
        USHORT substituteLength;
        USHORT printOffset;
        USHORT printLength;
        wchar_t path[1];
    };
    const int pathBytes = substituteBytes + static_cast<int>(sizeof(wchar_t))
        + printBytes + static_cast<int>(sizeof(wchar_t));
    QByteArray storage(static_cast<int>(offsetof(MountPointBuffer, path)) + pathBytes, '\0');
    auto* reparse = reinterpret_cast<MountPointBuffer*>(storage.data());
    reparse->tag = IO_REPARSE_TAG_MOUNT_POINT;
    reparse->dataLength = static_cast<USHORT>(8 + pathBytes);
    reparse->substituteOffset = 0;
    reparse->substituteLength = static_cast<USHORT>(substituteBytes);
    reparse->printOffset = static_cast<USHORT>(substituteBytes + sizeof(wchar_t));
    reparse->printLength = static_cast<USHORT>(printBytes);
    std::memcpy(reparse->path, substituteName.utf16(), substituteBytes);
    std::memcpy(reinterpret_cast<char*>(reparse->path) + reparse->printOffset,
        printName.utf16(), printBytes);
    DWORD returned = 0;
    const bool created = DeviceIoControl(handle, FSCTL_SET_REPARSE_POINT,
        reparse, 8 + reparse->dataLength, nullptr, 0, &returned, nullptr) != FALSE;
    CloseHandle(handle);
    if (!created)
        QDir().rmdir(linkPath);
    return created;
}

class NativeJunctionGuard final
{
public:
    NativeJunctionGuard(QString path, bool active)
        : path_(std::move(path)), active_(active)
    {
    }

    ~NativeJunctionGuard()
    {
        if (!active_)
            return;
        const QString nativePath = filemanager::platform::extendedLengthPath(path_);
        RemoveDirectoryW(reinterpret_cast<LPCWSTR>(nativePath.utf16()));
    }

private:
    QString path_;
    bool active_ = false;
};
#endif

bool expect(bool condition, const char* message)
{
    if (!condition)
        std::fprintf(stderr, "FAIL: %s\n", message);
    return condition;
}

bool runOptionalIntegrationCleanupTests()
{
#ifdef Q_OS_WIN
    using namespace filemanager::platform;
    const QString app = QCoreApplication::applicationFilePath();
    bool passed = true;
    enum Scenario { Matching, OtherInstallation, ChangedCommand, MalformedOwner,
        ExtraData, PartialCommand, SelectedDefault, MalformedDefault, MissingIcon };
    for (const auto scenario : {Matching, OtherInstallation, ChangedCommand, MalformedOwner,
             ExtraData, PartialCommand, SelectedDefault, MalformedDefault, MissingIcon}) {
        TemporaryRegistryTree tree;
        const QString classes = tree.root + "\\Classes";
        const QString paths = tree.root + "\\App Paths";
        const QString directory = classes + "\\Directory\\shell\\FileManager.Open";
        const QString driveShell = classes + "\\Drive\\shell";
        const QString drive = driveShell + "\\FileManager.Open";
        const QString appKey = paths + '\\' + QFileInfo(app).fileName();
        const auto wide = [](const QString& value) { return reinterpret_cast<LPCWSTR>(value.utf16()); };
        const auto put = [&](const QString& keyPath, const wchar_t* name,
                             const QString& value, DWORD type = REG_SZ) {
            HKEY key = nullptr;
            if (RegCreateKeyExW(HKEY_CURRENT_USER, wide(keyPath), 0, nullptr, 0,
                    KEY_SET_VALUE, nullptr, &key, nullptr) != ERROR_SUCCESS)
                return false;
            const auto status = RegSetValueExW(key, name, 0, type,
                reinterpret_cast<const BYTE*>(value.utf16()),
                static_cast<DWORD>((value.size() + 1) * sizeof(wchar_t)));
            RegCloseKey(key);
            return status == ERROR_SUCCESS;
        };
        const auto read = [&](const QString& keyPath, const wchar_t* name) {
            HKEY key = nullptr;
            if (RegOpenKeyExW(HKEY_CURRENT_USER, wide(keyPath), 0, KEY_READ, &key) != ERROR_SUCCESS)
                return QByteArray{};
            DWORD size = 0;
            RegQueryValueExW(key, name, nullptr, nullptr, nullptr, &size);
            QByteArray bytes(size, Qt::Uninitialized);
            const auto status = RegQueryValueExW(key, name, nullptr, nullptr,
                reinterpret_cast<BYTE*>(bytes.data()), &size);
            RegCloseKey(key);
            return status == ERROR_SUCCESS ? bytes : QByteArray{};
        };
        const auto erase = [&](const QString& keyPath, const wchar_t* name) {
            HKEY key = nullptr;
            if (RegOpenKeyExW(HKEY_CURRENT_USER, wide(keyPath), 0, KEY_SET_VALUE, &key) != ERROR_SUCCESS)
                return false;
            const auto status = RegDeleteValueW(key, name);
            RegCloseKey(key);
            return status == ERROR_SUCCESS;
        };
        if (!expect(setWindowsFolderCommandRegistered(true, app, classes).success
                && setWindowsAppPathsRegistered(true, app, paths).success,
                "Optional cleanup fixture registrations must be created.")) {
            passed = false;
            continue;
        }
        if (scenario == ChangedCommand)
            passed &= expect(put(directory + "\\command", nullptr, "foreign.exe %1"),
                "Foreign command fixture must be written.");
        if (scenario == MalformedOwner) {
            passed &= expect(put(directory, L"FileManagerOwner", "Kiatami.FileManager", REG_BINARY)
                    && put(appKey, L"FileManagerOwner", "Kiatami.FileManager", REG_BINARY),
                "Malformed cleanup ownership fixture must be written.");
        }
        if (scenario == ExtraData) {
            passed &= expect(put(directory, nullptr, "My custom label")
                    && put(directory, L"Foreign", "retain")
                    && put(directory + "\\command\\ForeignChild", nullptr, "retain child")
                    && put(appKey, L"Path", "C:\\Custom tools")
                    && put(appKey + "\\ForeignChild", nullptr, "retain app child"),
                "Later-added registry fixture data must be written.");
        }
        if (scenario == PartialCommand)
            passed &= expect(erase(directory + "\\command", nullptr),
                "Interrupted cleanup fixture removes only its command value.");
        if (scenario == MissingIcon)
            passed &= expect(erase(directory, L"Icon"), "Partial registration fixture omits its icon.");
        if (scenario == SelectedDefault || scenario == MalformedDefault)
            passed &= expect(put(driveShell, nullptr, "FileManager.Open",
                    scenario == SelectedDefault ? REG_SZ : REG_BINARY),
                "Unjournaled default fixture must be written.");
        const QByteArray priorCommand = read(directory + "\\command", nullptr);
        const QByteArray priorOwner = read(directory, L"FileManagerOwner");
        const QByteArray priorApp = read(appKey, nullptr);
        const QByteArray priorAppOwner = read(appKey, L"FileManagerOwner");
        const QByteArray priorDefault = read(driveShell, nullptr);
        const QByteArray priorLabel = read(directory, nullptr);
        const QByteArray priorExtra = read(directory, L"Foreign");
        const QByteArray priorChild = read(directory + "\\command\\ForeignChild", nullptr);
        const QByteArray priorPath = read(appKey, L"Path");
        const QByteArray priorAppChild = read(appKey + "\\ForeignChild", nullptr);
        const QString uninstallApp = scenario == OtherInstallation
            ? "C:/Different installation/" + QFileInfo(app).fileName() : app;
        const auto result = removeWindowsIntegrationForInstallation(uninstallApp, tree.root);
        const bool blocked = scenario == SelectedDefault || scenario == MalformedDefault;
        passed &= expect(result.success != blocked, "Cleanup must refuse an unresolved selected or malformed default.");
        if (blocked || scenario == OtherInstallation || scenario == ChangedCommand
                || scenario == MalformedOwner) {
            passed &= expect(read(directory + "\\command", nullptr) == priorCommand
                    && read(directory, L"FileManagerOwner") == priorOwner,
                "Foreign, malformed or blocked folder registrations must be retained byte-for-byte.");
        } else {
            passed &= expect(registryDefaultValueMissing(directory + "\\command"),
                "An owned optional command must be removed on uninstall.");
            passed &= expect(scenario == ExtraData || !registryKeyExists(directory),
                "Empty owned folder keys must be removed, including partial registrations.");
        }
        if (blocked || scenario == OtherInstallation || scenario == MalformedOwner) {
            passed &= expect(read(appKey, nullptr) == priorApp
                    && read(appKey, L"FileManagerOwner") == priorAppOwner,
                "Blocked or foreign App Paths data must remain unchanged.");
        } else {
            passed &= expect(registryDefaultValueMissing(appKey)
                    && (scenario == ExtraData || !registryKeyExists(appKey)),
                "Only this installation's App Paths launch target must be removed.");
        }
        passed &= expect(read(driveShell, nullptr) == priorDefault,
            "Optional cleanup must never change the Windows default selection.");
        passed &= expect(registryKeyExists(drive) == (blocked || scenario == OtherInstallation),
            "Each folder kind must use its own installation ownership.");
        if (scenario == ExtraData) {
            passed &= expect(read(directory, nullptr) == priorLabel
                    && read(directory, L"Foreign") == priorExtra
                    && read(directory + "\\command\\ForeignChild", nullptr) == priorChild
                    && read(appKey, L"Path") == priorPath
                    && read(appKey + "\\ForeignChild", nullptr) == priorAppChild,
                "Unknown values, custom labels and nested data must survive uninstall cleanup.");
        }
        if (!blocked)
            passed &= expect(removeWindowsIntegrationForInstallation(uninstallApp, tree.root).success,
                "Repeated optional cleanup must be harmless after successful or partial removal.");
        passed &= expect(!removeWindowsIntegrationForInstallation(app, tree.root + "\\escape").success
                && !removeWindowsIntegrationForInstallation("relative.exe", tree.root).success,
            "Cleanup must reject an escaped fixture scope and relative executable path.");
    }
    return passed;
#else
    return true;
#endif
}

bool createSparseSizedFile(const QString& path, qint64 size)
{
#ifdef Q_OS_WIN
    const QString nativePath = filemanager::platform::extendedLengthPath(path);
    const HANDLE handle = CreateFileW(reinterpret_cast<LPCWSTR>(nativePath.utf16()),
        GENERIC_WRITE, FILE_SHARE_READ, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (handle == INVALID_HANDLE_VALUE)
        return false;
    DWORD returned = 0;
    const bool markedSparse = DeviceIoControl(handle, FSCTL_SET_SPARSE, nullptr, 0,
        nullptr, 0, &returned, nullptr) != FALSE;
    LARGE_INTEGER end{};
    end.QuadPart = size;
    const bool sized = markedSparse
        && SetFilePointerEx(handle, end, nullptr, FILE_BEGIN) != FALSE
        && SetEndOfFile(handle) != FALSE;
    CloseHandle(handle);
    return sized;
#else
    QFile file(path);
    return file.open(QIODevice::WriteOnly) && file.resize(size);
#endif
}

bool runLoopWithTimeout(QEventLoop& loop, int timeoutMs = 5000)
{
    QTimer timeout;
    timeout.setSingleShot(true);
    QObject::connect(&timeout, &QTimer::timeout, &loop, &QEventLoop::quit);
    timeout.start(timeoutMs);
    loop.exec();
    return timeout.isActive();
}

void appendLittleEndian16(QByteArray& bytes, quint16 value)
{
    bytes.append(static_cast<char>(value & 0xff));
    bytes.append(static_cast<char>((value >> 8) & 0xff));
}

void appendLittleEndian32(QByteArray& bytes, quint32 value)
{
    appendLittleEndian16(bytes, static_cast<quint16>(value & 0xffff));
    appendLittleEndian16(bytes, static_cast<quint16>(value >> 16));
}

quint32 zipCrc32(const QByteArray& bytes)
{
    quint32 crc = 0xffffffffU;
    for (const unsigned char byte : bytes) {
        crc ^= byte;
        for (int bit = 0; bit < 8; ++bit)
            crc = (crc >> 1) ^ (0xedb88320U & (0U - (crc & 1U)));
    }
    return ~crc;
}

bool writeStoredZip(const QString& path,
    const QList<QPair<QByteArray, QByteArray>>& entries)
{
    struct CentralEntry { QByteArray name; quint32 crc; quint32 size; quint32 offset; };
    QList<CentralEntry> centralEntries;
    QByteArray archive;
    for (const auto& [name, contents] : entries) {
        const quint32 offset = static_cast<quint32>(archive.size());
        const quint32 crc = zipCrc32(contents);
        appendLittleEndian32(archive, 0x04034b50);
        appendLittleEndian16(archive, 20);
        appendLittleEndian16(archive, 0);
        appendLittleEndian16(archive, 0);
        appendLittleEndian16(archive, 0);
        appendLittleEndian16(archive, 0);
        appendLittleEndian32(archive, crc);
        appendLittleEndian32(archive, static_cast<quint32>(contents.size()));
        appendLittleEndian32(archive, static_cast<quint32>(contents.size()));
        appendLittleEndian16(archive, static_cast<quint16>(name.size()));
        appendLittleEndian16(archive, 0);
        archive.append(name);
        archive.append(contents);
        centralEntries.append({name, crc, static_cast<quint32>(contents.size()), offset});
    }
    const quint32 centralOffset = static_cast<quint32>(archive.size());
    for (const CentralEntry& entry : centralEntries) {
        appendLittleEndian32(archive, 0x02014b50);
        appendLittleEndian16(archive, 20);
        appendLittleEndian16(archive, 20);
        appendLittleEndian16(archive, 0);
        appendLittleEndian16(archive, 0);
        appendLittleEndian16(archive, 0);
        appendLittleEndian16(archive, 0);
        appendLittleEndian32(archive, entry.crc);
        appendLittleEndian32(archive, entry.size);
        appendLittleEndian32(archive, entry.size);
        appendLittleEndian16(archive, static_cast<quint16>(entry.name.size()));
        appendLittleEndian16(archive, 0);
        appendLittleEndian16(archive, 0);
        appendLittleEndian16(archive, 0);
        appendLittleEndian16(archive, 0);
        appendLittleEndian32(archive, 0);
        appendLittleEndian32(archive, entry.offset);
        archive.append(entry.name);
    }
    const quint32 centralSize = static_cast<quint32>(archive.size()) - centralOffset;
    appendLittleEndian32(archive, 0x06054b50);
    appendLittleEndian16(archive, 0);
    appendLittleEndian16(archive, 0);
    appendLittleEndian16(archive, static_cast<quint16>(centralEntries.size()));
    appendLittleEndian16(archive, static_cast<quint16>(centralEntries.size()));
    appendLittleEndian32(archive, centralSize);
    appendLittleEndian32(archive, centralOffset);
    appendLittleEndian16(archive, 0);
    QFile file(path);
    return file.open(QIODevice::WriteOnly) && file.write(archive) == archive.size();
}

bool runCloudPreviewNoHydrationTests()
{
#ifdef Q_OS_WIN
    const auto progress = [](const char* step) {
        std::fprintf(stdout, "Cloud preview phase: %s\n", step);
        std::fflush(stdout);
    };
    progress("entered");
    QTemporaryDir root;
    if (!expect(root.isValid(), "Cloud preview fixture directory should open."))
        return false;
    const QString path = QDir(root.path()).filePath("offline-preview.png");
    QFile source(path);
    const bool written = source.open(QIODevice::WriteOnly)
        && source.write("synthetic image placeholder") == 27;
    source.close();
    if (!expect(written, "Cloud preview fixture should be written."))
        return false;
    const bool localReady = filemanager::platform::isLocallyPreviewableFile(path);
    progress("fixture_written");
    const DWORD original = GetFileAttributesW(reinterpret_cast<LPCWSTR>(path.utf16()));
    const bool marked = original != INVALID_FILE_ATTRIBUTES
        && SetFileAttributesW(reinterpret_cast<LPCWSTR>(path.utf16()),
            original | FILE_ATTRIBUTE_OFFLINE);
    if (!expect(marked, "Cloud preview fixture should carry the offline attribute."))
        return false;
    progress("offline_marked");
    const auto kind = filemanager::platform::nativePathKind(path);
    const auto cloudStatus = filemanager::platform::inspectCloudFileStatus(path);
    const bool offlineReady = filemanager::platform::isLocallyPreviewableFile(path);
    progress("kind_checked");
    const auto preview = filemanager::preview::runSandboxedPreviewHost(
        path, "preview", 1000, 1024 * 1024);
    progress("broker_returned");
    const bool restored = SetFileAttributesW(reinterpret_cast<LPCWSTR>(path.utf16()),
        original) != FALSE;
    progress("attributes_restored");
    const bool fileRefused = expect(localReady && !offlineReady
            && kind == filemanager::platform::NativePathKind::CloudPlaceholder
            && cloudStatus.inspected && cloudStatus.providerBacked
            && !preview.success && preview.error.contains("cloud placeholder") && restored,
        "An offline provider item must show provider status and be refused before "
        "the preview helper opens its data.");
    const QString folder = QDir(root.path()).filePath("offline-folder");
    if (!expect(QDir().mkpath(folder), "Offline folder preview fixture should be created."))
        return false;
    QFile child(QDir(folder).filePath("local-note.txt"));
    const bool childWritten = child.open(QIODevice::WriteOnly) && child.write("local") == 5;
    child.close();
    const auto localFolderPreview = filemanager::preview::PreviewController::loadIsolated(folder, 1000);
    if (!expect(childWritten && localFolderPreview.success
            && localFolderPreview.preview.kind == filemanager::preview::PreviewKind::Folder
            && localFolderPreview.preview.text.startsWith(
                filemanager::ui::localizedItems(1)),
        "A regular local folder should show its entry count before the offline guard is applied."))
        return false;
    const DWORD originalFolder = GetFileAttributesW(
        reinterpret_cast<LPCWSTR>(folder.utf16()));
    const bool folderMarked = originalFolder != INVALID_FILE_ATTRIBUTES
        && SetFileAttributesW(reinterpret_cast<LPCWSTR>(folder.utf16()),
            originalFolder | FILE_ATTRIBUTE_OFFLINE);
    if (!expect(folderMarked, "Folder preview fixture should carry the offline attribute."))
        return false;
    const auto folderPreview = filemanager::preview::PreviewController::loadIsolated(folder, 1000);
    const bool folderRestored = SetFileAttributesW(
        reinterpret_cast<LPCWSTR>(folder.utf16()), originalFolder) != FALSE;
    const bool folderRefused = expect(!folderPreview.success
            && folderPreview.error.contains("cloud placeholder") && folderRestored,
        "Selecting an offline provider folder must not enumerate it for a preview count.");
    return fileRefused && folderRefused;
#else
    return true;
#endif
}

bool runFfmpegCandidateTests()
{
#ifdef Q_OS_WIN
    if (!expect(qEnvironmentVariable("QT_MEDIA_BACKEND") == "ffmpeg",
            "The candidate media check must select Qt's FFmpeg backend explicitly."))
        return false;
    QTemporaryDir fixture;
    if (!expect(fixture.isValid(), "The synthetic FFmpeg fixture directory should open."))
        return false;
    // The .wave suffix deliberately bypasses the preview host's .wav-only
    // parser. RIFF/WAVE bytes must go through QMediaPlayer in its sandbox.
    const QString path = fixture.filePath("synthetic-tone.wave");
    QFile file(path);
    if (!expect(file.open(QIODevice::WriteOnly),
            "The synthetic FFmpeg media fixture should open."))
        return false;
    QDataStream stream(&file);
    stream.setByteOrder(QDataStream::LittleEndian);
    constexpr quint32 sampleRate = 8000;
    constexpr quint32 sampleCount = 800;
    constexpr quint32 dataBytes = sampleCount * 2;
    stream.writeRawData("RIFF", 4);
    stream << quint32(36 + dataBytes);
    stream.writeRawData("WAVEfmt ", 8);
    stream << quint32(16) << quint16(1) << quint16(1) << sampleRate
           << quint32(sampleRate * 2) << quint16(2) << quint16(16);
    stream.writeRawData("data", 4);
    stream << dataBytes;
    for (quint32 sample = 0; sample < sampleCount; ++sample)
        stream << qint16(0);
    file.close();
    if (!expect(stream.status() == QDataStream::Ok && file.size() == 44 + dataBytes,
            "The synthetic FFmpeg fixture should contain a complete 100 ms RIFF file."))
        return false;

    const auto result = filemanager::preview::runSandboxedPreviewHost(
        path, QStringLiteral("media"), 9000, 1024 * 1024);
    QJsonParseError parseError;
    const auto reply = QJsonDocument::fromJson(result.standardOutput, &parseError).object();
    const bool pluginLoaded = reply.value(QStringLiteral("ffmpegPluginLoaded")).toBool();
    const bool librariesLoaded = reply.value(QStringLiteral("ffmpegLibrariesLoaded")).toBool();
    const qint64 duration = reply.value(QStringLiteral("durationMs")).toInteger();
    const bool valid = result.success && result.sandboxed && result.jobRestricted
        && parseError.error == QJsonParseError::NoError
        && pluginLoaded && librariesLoaded && duration >= 90 && duration <= 110;
    if (!valid) {
        // Keep local diagnostics useful without recording the temporary path,
        // source media bytes or any provider data.
        std::fprintf(stderr,
            "FFmpeg candidate: success=%d sandboxed=%d job=%d exit=%d "
            "json=%d plugin=%d libraries=%d durationMs=%lld outputBytes=%lld errorPresent=%d\n",
            result.success, result.sandboxed, result.jobRestricted, result.exitCode,
            static_cast<int>(parseError.error), pluginLoaded, librariesLoaded,
            static_cast<long long>(duration),
            static_cast<long long>(result.standardOutput.size()), !result.error.isEmpty());
    }
    if (!expect(valid,
            "The restricted preview helper should extract duration through Qt's FFmpeg backend."))
        return false;

    const QString malformedPath = fixture.filePath("malformed.wave");
    QFile malformedFile(malformedPath);
    constexpr char malformedBytes[] = {'R', 'I', 'F', 'F', 4, 0, 0, 0,
        'b', 'r', 'o', 'k', 'e', 'n'};
    constexpr qint64 malformedBytesCount = static_cast<qint64>(sizeof(malformedBytes));
    const bool malformedWritten = malformedFile.open(QIODevice::WriteOnly)
        && malformedFile.write(malformedBytes, malformedBytesCount)
            == malformedBytesCount;
    malformedFile.close();
    if (!expect(malformedWritten, "The malformed media fixture should be written."))
        return false;
    const auto malformed = filemanager::preview::runSandboxedPreviewHost(
        malformedPath, QStringLiteral("media"), 9000, 1024 * 1024);
    QJsonParseError malformedParseError;
    const auto malformedReply = QJsonDocument::fromJson(
        malformed.standardOutput, &malformedParseError).object();
    return expect(malformed.success && malformed.sandboxed && malformed.jobRestricted
            && malformedParseError.error == QJsonParseError::NoError
            && malformedReply.value(QStringLiteral("ffmpegPluginLoaded")).toBool()
            && malformedReply.value(QStringLiteral("ffmpegLibrariesLoaded")).toBool()
            && malformedReply.value(QStringLiteral("durationMs")).toInteger() == 0,
        "Malformed local media should receive a bounded empty FFmpeg reply in the restricted helper.");
#else
    return true;
#endif
}

bool runFfmpegCandidateVideoTest()
{
#ifdef Q_OS_WIN
    if (!expect(qEnvironmentVariable("QT_MEDIA_BACKEND") == "ffmpeg",
            "The synthetic video check must select Qt's FFmpeg backend."))
        return false;
    QTemporaryDir fixture;
    if (!expect(fixture.isValid(), "The synthetic video fixture directory should open."))
        return false;
    const QString path = fixture.filePath("synthetic-video.avi");
    // Two 2x2 uncompressed RGB frames at 2 fps. This is an AVI RIFF fixture,
    // generated here so no personal or third-party media enters the test.
    auto appendDword = [](QByteArray& target, quint32 value) {
        for (int shift = 0; shift < 32; shift += 8)
            target.append(static_cast<char>((value >> shift) & 0xff));
    };
    auto appendWord = [](QByteArray& target, quint16 value) {
        target.append(static_cast<char>(value & 0xff));
        target.append(static_cast<char>(value >> 8));
    };
    auto chunk = [&](const char* name, const QByteArray& body) {
        QByteArray output(name, 4);
        appendDword(output, static_cast<quint32>(body.size()));
        output += body;
        if (body.size() & 1) output.append(char(0));
        return output;
    };
    auto list = [&](const char* name, const QByteArray& body) {
        return chunk("LIST", QByteArray(name, 4) + body);
    };
    QByteArray avih;
    for (quint32 value : {500000u, 32u, 0u, 0u, 2u, 0u, 1u,
            16u, 2u, 2u, 0u, 0u, 0u, 0u})
        appendDword(avih, value);
    QByteArray strh("vidsDIB ", 8);
    appendDword(strh, 0);
    appendWord(strh, 0);
    appendWord(strh, 0);
    for (quint32 value : {0u, 1u, 2u, 0u, 2u, 16u, 0xffffffffu, 0u})
        appendDword(strh, value);
    for (quint16 edge : {quint16(0), quint16(0), quint16(2), quint16(2)})
        appendWord(strh, edge);
    QByteArray strf;
    appendDword(strf, 40);
    appendDword(strf, 2);
    appendDword(strf, 2);
    appendWord(strf, 1);
    appendWord(strf, 24);
    for (quint32 value : {0u, 16u, 0u, 0u, 0u, 0u})
        appendDword(strf, value);
    const QByteArray hdrl = list("hdrl", chunk("avih", avih)
        + list("strl", chunk("strh", strh) + chunk("strf", strf)));
    const QByteArray black(16, char(0));
    const QByteArray white(16, char(-1));
    const QByteArray movi = list("movi", chunk("00db", black)
        + chunk("00db", white));
    const QByteArray avi = chunk("RIFF", QByteArray("AVI ", 4) + hdrl + movi);
    QFile file(path);
    const bool written = file.open(QIODevice::WriteOnly)
        && file.write(avi) == avi.size();
    file.close();
    if (!expect(written && avih.size() == 56 && strh.size() == 56
            && strf.size() == 40 && avi.size() == 272,
            "The synthetic video should contain complete AVI headers and two frames."))
        return false;
    const auto result = filemanager::preview::runSandboxedPreviewHost(
        path, QStringLiteral("media"), 9000, 1024 * 1024);
    QJsonParseError parseError;
    const auto reply = QJsonDocument::fromJson(result.standardOutput, &parseError).object();
    const qint64 duration = reply.value(QStringLiteral("durationMs")).toInteger();
    const bool valid = result.success && result.sandboxed && result.jobRestricted
        && parseError.error == QJsonParseError::NoError
        && reply.value(QStringLiteral("ffmpegPluginLoaded")).toBool()
        && reply.value(QStringLiteral("ffmpegLibrariesLoaded")).toBool()
        && duration >= 900 && duration <= 1100
        && reply.value(QStringLiteral("width")).toInt() == 2
        && reply.value(QStringLiteral("height")).toInt() == 2;
    if (!valid) {
        std::fprintf(stderr,
            "FFmpeg synthetic video: success=%d sandboxed=%d job=%d exit=%d "
            "json=%d durationMs=%lld width=%d height=%d outputBytes=%lld\n",
            result.success, result.sandboxed, result.jobRestricted, result.exitCode,
            static_cast<int>(parseError.error), static_cast<long long>(duration),
            reply.value(QStringLiteral("width")).toInt(),
            reply.value(QStringLiteral("height")).toInt(),
            static_cast<long long>(result.standardOutput.size()));
    }
    return expect(valid,
        "The restricted FFmpeg helper should read duration and dimensions from synthetic video.");
#else
    return true;
#endif
}

bool runFfmpegCompressedVideoTest(const QString& path, const QString& expectedSha256)
{
#ifdef Q_OS_WIN
    if (!expect(qEnvironmentVariable("QT_MEDIA_BACKEND") == "ffmpeg",
            "The compressed-video check must select Qt's FFmpeg backend."))
        return false;
    const QFileInfo info(path);
    if (!expect(info.isAbsolute() && info.isFile() && info.size() > 0
            && info.size() <= 1024 * 1024,
            "The reviewed compressed-video fixture must be a small local file."))
        return false;
    QFile file(path);
    if (!expect(file.open(QIODevice::ReadOnly),
            "The reviewed compressed-video fixture must be readable."))
        return false;
    const QByteArray bytes = file.readAll();
    const QByteArray digest = QCryptographicHash::hash(bytes,
        QCryptographicHash::Sha256).toHex();
    if (!expect(bytes.size() == info.size()
            && digest == expectedSha256.toLatin1().toLower()
            && bytes.mid(4, 4) == "ftyp",
            "The compressed-video fixture must match its approved MP4 hash."))
        return false;
    file.close();
    const auto result = filemanager::preview::runSandboxedPreviewHost(
        path, QStringLiteral("media"), 9000, 1024 * 1024);
    QJsonParseError parseError;
    const auto reply = QJsonDocument::fromJson(result.standardOutput, &parseError).object();
    const qint64 duration = reply.value(QStringLiteral("durationMs")).toInteger();
    const int width = reply.value(QStringLiteral("width")).toInt();
    const int height = reply.value(QStringLiteral("height")).toInt();
    const bool pluginLoaded = reply.value(QStringLiteral("ffmpegPluginLoaded")).toBool();
    const bool librariesLoaded = reply.value(QStringLiteral("ffmpegLibrariesLoaded")).toBool();
    const bool valid = result.success && result.sandboxed && result.jobRestricted
        && parseError.error == QJsonParseError::NoError
        && pluginLoaded && librariesLoaded
        && duration >= 3000 && duration <= 3500 && width > 0 && height > 0;
    if (!valid) {
        std::fprintf(stderr,
            "FFmpeg compressed video: success=%d sandboxed=%d job=%d exit=%d "
            "json=%d plugin=%d libraries=%d durationMs=%lld width=%d height=%d outputBytes=%lld\n",
            result.success, result.sandboxed, result.jobRestricted, result.exitCode,
            static_cast<int>(parseError.error), pluginLoaded, librariesLoaded,
            static_cast<long long>(duration),
            width, height, static_cast<long long>(result.standardOutput.size()));
    } else {
        std::fprintf(stdout,
            "compressed_video_ok sandboxed=1 job=1 plugin=1 libraries=1 durationMs=%lld width=%d height=%d\n",
            static_cast<long long>(duration), width, height);
    }
    return expect(valid,
        "The restricted FFmpeg helper should read compressed MP4 metadata.");
#else
    Q_UNUSED(path);
    Q_UNUSED(expectedSha256);
    return true;
#endif
}

} // namespace

bool runReviewedTransferTests();
bool runGuardedFileRenameTests();
int runGuardedBatchRenameInterruptionFixture(const QString& root);
bool runGuardedDirectoryTests();
bool runGuardedCrossVolumeTests();

int main(int argc, char* argv[])
{
    // Keep helper-mode stdout unchanged. Flush main-run progress so a timeout
    // identifies the last reached phase instead of losing buffered diagnostics.
    const auto phase = [enabled = argc == 1](const char* name) {
        if (enabled) {
            std::fprintf(stdout, "Workspace phase: %s\n", name);
            std::fflush(stdout);
        }
    };
    phase("main_entered");
    QTemporaryDir isolatedHome;
    phase("isolated_home_created");
    QApplication application(argc, argv);
    phase("application_ready");
#ifdef Q_OS_WIN
    const unsigned initialIconFloatingControl = iconFloatingControl();
#endif
    const QStringList arguments = application.arguments();
    if (arguments == QStringList{arguments.constFirst(), "--helper-output-fixture"})
        return filemanager::platform::writeHelperReply(helperOutputFixture()) ? 0 : 21;
    if (arguments == QStringList{arguments.constFirst(), "--helper-echo-fixture"}) {
        QByteArray received;
        char buffer[4096];
        for (;;) {
            const qint64 count = filemanager::platform::readHelperInput(buffer, sizeof(buffer));
            if (count < 0)
                return 22;
            if (count == 0)
                break;
            received.append(buffer, count);
            if (received.size() > 2 * 1024 * 1024)
                return 23;
        }
        return received == helperOutputFixture()
                && filemanager::platform::writeHelperReply(received) ? 0 : 24;
    }
#ifdef Q_OS_WIN
    if (arguments.size() == 3 && arguments.at(1) == "--single-instance-delayed-ack") {
        if (!arguments.at(2).startsWith("FileManagerTests."))
            return 30;
        const QString name = QStringLiteral("\\\\.\\pipe\\") + arguments.at(2);
        const HANDLE pipe = CreateFileW(reinterpret_cast<LPCWSTR>(name.utf16()),
            GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, 0, nullptr);
        if (pipe == INVALID_HANDLE_VALUE)
            return 31;
        const QByteArray payload = QJsonDocument(QJsonArray{QStringLiteral("C:/Delayed")})
            .toJson(QJsonDocument::Compact);
        QByteArray frame(sizeof(quint32), Qt::Uninitialized);
        qToBigEndian<quint32>(static_cast<quint32>(payload.size()),
            reinterpret_cast<uchar*>(frame.data()));
        frame.append(payload);
        DWORD written = 0;
        const bool sent = WriteFile(pipe, frame.constData(), static_cast<DWORD>(frame.size()),
            &written, nullptr) && written == static_cast<DWORD>(frame.size());
        // Do not post a pipe read until the primary has emitted pathsReceived.
        // QLocalSocket's automatic reader would hide this disconnect-before-read race.
        char release = 0;
        if (!sent || filemanager::platform::readHelperInput(&release, 1) != 1 || release != 'R') {
            CloseHandle(pipe);
            return 32;
        }
        char acknowledgement = 0;
        DWORD received = 0;
        const bool acknowledged = ReadFile(pipe, &acknowledgement, 1, &received, nullptr)
            && received == 1 && acknowledgement == '\x06';
        CloseHandle(pipe);
        return acknowledged ? 0 : 33;
    }
#endif
    if (arguments.size() >= 3 && (arguments.at(1) == "--single-instance-forward"
            || arguments.at(1) == "--single-instance-folders")) {
        filemanager::platform::SingleInstanceCoordinator instance(arguments.at(2));
        QString error;
        const auto role = instance.startOrForward(arguments.mid(3), &error,
            arguments.at(1) == "--single-instance-folders");
        if (role != filemanager::platform::SingleInstanceCoordinator::Role::Secondary)
            std::fprintf(stderr, "IPC helper role=%d error=%s\n", static_cast<int>(role),
                error.toUtf8().constData());
        return role == filemanager::platform::SingleInstanceCoordinator::Role::Secondary ? 0 : 2;
    }
    if (arguments.size() == 3 && arguments.at(1) == "--guarded-batch-rename-interruption")
        return runGuardedBatchRenameInterruptionFixture(arguments.at(2));
    application.setOrganizationName("FileManagerTests");
    application.setApplicationName("WorkspaceTests");
    QTemporaryDir settingsStore;
    QSettings::setDefaultFormat(QSettings::IniFormat);
    QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, settingsStore.path());
    QSettings().clear();
    if (arguments == QStringList{arguments.constFirst(), "--guarded-cross-volume-only"})
        return runGuardedCrossVolumeTests() ? 0 : 1;
    if (arguments == QStringList{arguments.constFirst(), "--cloud-preview-only"})
        return runCloudPreviewNoHydrationTests() ? 0 : 1;
    if (arguments == QStringList{arguments.constFirst(), "--ffmpeg-candidate-only"})
        return runFfmpegCandidateTests() ? 0 : 1;
    if (arguments == QStringList{arguments.constFirst(), "--ffmpeg-video-only"})
        return runFfmpegCandidateVideoTest() ? 0 : 1;
    if (arguments.size() == 4 && arguments.at(1) == "--ffmpeg-compressed-video-only")
        return runFfmpegCompressedVideoTest(arguments.at(2), arguments.at(3)) ? 0 : 1;
    bool passed = expect(isolatedHome.isValid(), "The isolated filesystem-model home should open.");
    passed &= runCloudPreviewNoHydrationTests();
    passed &= runWinEKeyStateTests();
    passed &= runWinEIntegrationTests();
    if (arguments == QStringList{arguments.constFirst(), "--win-e-integration-only"})
        return passed ? 0 : 1;
    passed &= runFolderRoutingTests();
    passed &= runOptionalIntegrationCleanupTests();
    passed &= expect(settingsStore.isValid(), "The isolated settings store should open.");
#ifdef Q_OS_WIN
    phase("helper_output_begin");
    for (const bool echo : {false, true}) {
        // Exceed QProcess's native pipe buffer and delay the consumer. Verify all
        // binary bytes, including NUL/CR/LF, across pending/chunked writes.
        QProcess writer;
        writer.setProgram(QCoreApplication::applicationFilePath());
        writer.setArguments({echo ? QStringLiteral("--helper-echo-fixture")
                                 : QStringLiteral("--helper-output-fixture")});
        writer.start(echo ? QIODevice::ReadWrite : QIODevice::ReadOnly);
        const bool started = writer.waitForStarted(1000);
        QThread::msleep(75);
        const QByteArray expected = helperOutputFixture();
        const bool queued = !echo || (started && writer.write(expected) == expected.size());
        if (echo)
            writer.closeWriteChannel();
        QByteArray received;
        QElapsedTimer deadline;
        deadline.start();
        while (started && writer.state() != QProcess::NotRunning && deadline.elapsed() < 5000) {
            writer.waitForFinished(25);
            received.append(writer.readAllStandardOutput());
            if (received.size() > 2 * 1024 * 1024)
                break;
        }
        const bool finished = writer.state() == QProcess::NotRunning;
        if (!finished) {
            writer.kill();
            writer.waitForFinished(1000);
        }
        received.append(writer.readAllStandardOutput());
        passed &= expect(started && queued && finished && writer.exitStatus() == QProcess::NormalExit
                && writer.exitCode() == 0 && received == expected,
            echo ? "A helper must receive a complete binary request and EOF, then return every byte through overlapped pipes."
                 : "A helper must finish its complete binary reply through an overlapped Windows pipe before exiting.");
    }
#endif
    phase("reviewed_transfer_begin");
    passed &= runReviewedTransferTests();
    phase("guarded_file_rename_begin");
    passed &= runGuardedFileRenameTests();
    phase("guarded_directory_begin");
    passed &= runGuardedDirectoryTests();
    phase("guarded_cross_volume_begin");
    passed &= runGuardedCrossVolumeTests();
    phase("settings_and_permissions_begin");
    passed &= expect(!filemanager::settings::loadContextMenuPermissions().allowWindowsExtensions,
        "Windows context-menu extensions should be denied by default.");
    filemanager::settings::saveContextMenuPermissions({true});
    passed &= expect(filemanager::settings::loadContextMenuPermissions().allowWindowsExtensions,
        "The explicit isolated Windows context-menu permission should persist.");
    filemanager::settings::saveContextMenuPermissions({false});
    passed &= expect(!filemanager::settings::loadContextMenuPermissions().allowWindowsExtensions,
        "Revoking Windows context-menu permission should persist.");
    passed &= expect(!filemanager::platform::showWindowsContextMenuIsolated({}, 0, 0).success
            && !filemanager::platform::showWindowsContextMenuIsolated(
                {settingsStore.filePath("missing.txt")}, 0, 0).success,
        "The isolated Windows command client should reject empty and unavailable selections.");

    filemanager::settings::SessionStore sessionStore;
    passed &= expect(!sessionStore.restorationEnabled(),
        "Detailed workspace restoration should be off by default.");
    sessionStore.setRestorationEnabled(true);
    passed &= expect(sessionStore.restorationEnabled(),
        "The user should be able to enable detailed workspace restoration explicitly.");
    const QString privateWorkspacePath = QStringLiteral("C:/Private/Workspace/Distinctive.txt");
    filemanager::settings::Session savedSession;
    savedSession.workspaceLayoutJson = QString::fromUtf8(QJsonDocument(
        filemanager::workspace::LayoutNode::panelNode({{privateWorkspacePath}, 0}).toJson())
        .toJson(QJsonDocument::Compact));
    savedSession.geometry = QByteArray("geometry");
    savedSession.splitterState = QByteArray("splitter");
    savedSession.inspectorVisible = true;
    QString sessionError;
    const bool savedWorkspaceSession = sessionStore.save(savedSession, &sessionError);
    if (!savedWorkspaceSession)
        std::fprintf(stderr, "Workspace session save error: %s\n",
            sessionError.toUtf8().constData());
    passed &= expect(savedWorkspaceSession,
        "An explicitly enabled workspace session should save successfully.");
    QSettings rawSessionSettings;
    const QByteArray protectedWorkspace =
        rawSessionSettings.value("layout/workspaceProtected").toByteArray();
    passed &= expect(!protectedWorkspace.isEmpty()
            && !protectedWorkspace.contains(privateWorkspacePath.toUtf8())
            && !rawSessionSettings.contains("layout/workspaceJson")
            && sessionStore.hasSavedSession(),
        "Saved workspace paths should exist only in the per-user encrypted payload.");
    sessionError.clear();
    const auto restoredSession = sessionStore.load(&sessionError);
    passed &= expect(sessionError.isEmpty()
            && restoredSession.workspaceLayoutJson == savedSession.workspaceLayoutJson
            && restoredSession.geometry == savedSession.geometry
            && restoredSession.splitterState == savedSession.splitterState
            && restoredSession.inspectorVisible,
        "The encrypted detailed workspace session should round-trip exactly.");

    QString namedId;
    sessionError.clear();
    passed &= expect(sessionStore.saveNamedWorkspace(
            "Research", savedSession, &namedId, &sessionError) && !namedId.isEmpty(),
        "An explicitly named workspace should save in the encrypted layout store.");
    const QByteArray protectedNamed =
        rawSessionSettings.value("layout/namedWorkspacesProtected").toByteArray();
    passed &= expect(!protectedNamed.isEmpty()
            && !protectedNamed.contains(privateWorkspacePath.toUtf8())
            && !protectedNamed.contains("Research")
            && sessionStore.activeNamedWorkspaceId() == namedId,
        "Named layout names and paths should not appear in plaintext settings.");
    sessionError.clear();
    auto namedWorkspaces = sessionStore.namedWorkspaces(&sessionError);
    passed &= expect(sessionError.isEmpty() && namedWorkspaces.size() == 1
            && namedWorkspaces.first().id == namedId
            && namedWorkspaces.first().name == "Research"
            && namedWorkspaces.first().session.workspaceLayoutJson
                == savedSession.workspaceLayoutJson,
        "The encrypted named workspace should round-trip exactly.");
    filemanager::settings::Session revisedNamedSession = savedSession;
    revisedNamedSession.inspectorVisible = false;
    QString replacedId;
    sessionError.clear();
    passed &= expect(sessionStore.saveNamedWorkspace(
            "research", revisedNamedSession, &replacedId, &sessionError)
            && replacedId == namedId
            && sessionStore.namedWorkspaces().size() == 1
            && !sessionStore.namedWorkspaces().first().session.inspectorVisible,
        "Case-insensitive duplicate names should replace one stable named layout.");
    sessionError.clear();
    passed &= expect(sessionStore.removeNamedWorkspace(namedId, &sessionError)
            && sessionStore.activeNamedWorkspaceId() != namedId
            && sessionStore.namedWorkspaces().isEmpty(),
        "Deleting a named layout should remove its encrypted record and active marker.");
    rawSessionSettings.setValue("layout/namedWorkspacesProtected", QByteArray("corrupted"));
    sessionError.clear();
    passed &= expect(sessionStore.namedWorkspaces(&sessionError).isEmpty()
            && !sessionError.isEmpty()
            && !rawSessionSettings.contains("layout/namedWorkspacesProtected")
            && rawSessionSettings.value("recovery/workspace/reason").toString()
                == "named_store_decryption_failed",
        "A corrupted named-layout store should be removed from the active load path.");
    rawSessionSettings.setValue("layout/workspaceProtected", QByteArray("corrupted"));
    sessionError.clear();
    const auto corruptedSession = sessionStore.load(&sessionError);
    passed &= expect(!sessionError.isEmpty() && corruptedSession.workspaceLayoutJson.isEmpty()
            && !rawSessionSettings.contains("layout/workspaceProtected")
            && !sessionStore.restorationEnabled()
            && rawSessionSettings.value("recovery/workspace/reason").toString()
                == "workspace_decryption_failed",
        "A corrupted protected workspace should reset safely and disable repeated restoration.");
    sessionError.clear();
    passed &= expect(sessionStore.clear(&sessionError) && !sessionStore.hasSavedSession(),
        "Revoking workspace persistence should remove all saved workspace state.");
    rawSessionSettings.setValue("schemaVersion", filemanager::settings::Session::CurrentVersion);
    rawSessionSettings.setValue("layout/workspaceJson", savedSession.workspaceLayoutJson);
    rawSessionSettings.setValue("layout/geometry", savedSession.geometry);
    rawSessionSettings.setValue("layout/shellSizes", savedSession.splitterState);
    rawSessionSettings.setValue("layout/inspectorVisible", savedSession.inspectorVisible);
    sessionError.clear();
    const auto migratedPlaintextSession = sessionStore.load(&sessionError);
    passed &= expect(sessionError.isEmpty()
            && migratedPlaintextSession.workspaceLayoutJson == savedSession.workspaceLayoutJson
            && rawSessionSettings.contains("layout/workspaceProtected")
            && !rawSessionSettings.contains("layout/workspaceJson"),
        "The released settings schema should migrate legacy plaintext paths into DPAPI storage.");
    passed &= expect(sessionStore.clear(&sessionError),
        "The migrated settings fixture should clear without leaving recovery state.");
    sessionStore.setRestorationEnabled(false);

    QSettings startupSettings;
    startupSettings.remove("navigation/openTo");
    startupSettings.remove("navigation/recents");
    passed &= expect(filemanager::settings::startupLocation(startupSettings)
            == QLatin1String(FilePane::HomeIdentity),
        "Home should be the safe default startup destination.");
    filemanager::settings::saveStartupDestination(startupSettings,
        filemanager::settings::StartupDestination::ThisPc);
    passed &= expect(filemanager::settings::startupLocation(startupSettings)
            == QStringLiteral("filemanager:shell:this-pc"),
        "The startup preference should preserve the stable This PC identity.");
    TabbedPanel startupThisPc{QStringLiteral("filemanager:shell:this-pc")};
    passed &= expect(startupThisPc.currentPane()
            && startupThisPc.currentPane()->builtInShellLocation()
                == filemanager::platform::BuiltInShellLocation::ThisPc,
        "A tab panel should preserve the selected This PC startup identity.");
    filemanager::settings::saveStartupDestination(startupSettings,
        filemanager::settings::StartupDestination::LastFolder);
    startupSettings.setValue("navigation/recents", QStringList{settingsStore.path()});
    passed &= expect(filemanager::settings::startupLocation(startupSettings)
            == QDir::cleanPath(settingsStore.path()),
        "Last folder should resolve the most recent folder while it remains available.");
    startupSettings.setValue("navigation/recents",
        QStringList{settingsStore.filePath("missing-folder")});
    passed &= expect(filemanager::settings::startupLocation(startupSettings)
            == QLatin1String(FilePane::HomeIdentity),
        "Last folder should fall back to Home when no recent folder is still available.");
    TabbedPanel startupHome{QLatin1String(FilePane::HomeIdentity)};
    passed &= expect(startupHome.currentPane() && startupHome.currentPane()->isHomeLocation(),
        "A tab panel should preserve the selected Home startup identity.");
    startupSettings.remove("navigation/openTo");
    startupSettings.remove("navigation/recents");

    phase("archive_begin");
    QTemporaryDir archiveRoot;
    passed &= expect(archiveRoot.isValid(), "The archive test root should open.");
    for (const QString& extension : {QStringLiteral("rar"), QStringLiteral("7z"),
             QStringLiteral("tar"), QStringLiteral("zip")}) {
        QFile fixture(archiveRoot.filePath(QStringLiteral("native-handoff.") + extension));
        passed &= expect(fixture.open(QIODevice::WriteOnly) && fixture.write("archive") == 7,
            "Native archive handoff fixtures should be created.");
        fixture.close();
    }
    const auto rarHandoff = filemanager::platform::validateNativeArchiveHandoff(
        archiveRoot.filePath("native-handoff.rar"));
    const auto sevenZipHandoff = filemanager::platform::validateNativeArchiveHandoff(
        archiveRoot.filePath("native-handoff.7z"));
    const auto tarHandoff = filemanager::platform::validateNativeArchiveHandoff(
        archiveRoot.filePath("native-handoff.tar"));
    passed &= expect(rarHandoff.valid
            && rarHandoff.format == filemanager::platform::NativeArchiveFormat::Rar
            && sevenZipHandoff.valid
            && sevenZipHandoff.format == filemanager::platform::NativeArchiveFormat::SevenZip
            && filemanager::platform::nativeArchiveFormat(QStringLiteral("EXAMPLE.7Z"))
                == filemanager::platform::NativeArchiveFormat::SevenZip
            && tarHandoff.valid
            && tarHandoff.format == filemanager::platform::NativeArchiveFormat::Tar,
        "Windows archive handoff should accept RAR, 7z, and TAR case-insensitively.");
    passed &= expect(!filemanager::platform::validateNativeArchiveHandoff(
            archiveRoot.filePath("native-handoff.zip")).valid
            && !filemanager::platform::validateNativeArchiveHandoff(
                archiveRoot.filePath("missing.rar")).valid
            && !filemanager::platform::validateNativeArchiveHandoff(archiveRoot.path()).valid,
        "Native archive handoff should keep ZIP guarded and reject missing files and folders.");
    const QString contextHost = QDir(QCoreApplication::applicationDirPath()).filePath(
#ifdef Q_OS_WIN
        "FileManagerContextMenuHost.exe");
#else
        "FileManagerContextMenuHost");
#endif
    QProcess contextHostProbe;
    contextHostProbe.start(contextHost, {"--validate"}, QIODevice::ReadOnly);
    passed &= expect(contextHostProbe.waitForStarted(1000)
            && contextHostProbe.waitForFinished(3000)
            && contextHostProbe.exitStatus() == QProcess::NormalExit
            && contextHostProbe.exitCode() == 0,
        "The isolated Windows context-menu helper should start and validate its protocol mode.");
    const QString contextFirst = archiveRoot.filePath("context-first.txt");
    const QString contextOtherFolder = archiveRoot.filePath("other-parent");
    QDir().mkdir(contextOtherFolder);
    const QString contextSecond = QDir(contextOtherFolder).filePath("context-second.txt");
    QFile firstContextFile(contextFirst);
    QFile secondContextFile(contextSecond);
    passed &= expect(firstContextFile.open(QIODevice::WriteOnly)
            && secondContextFile.open(QIODevice::WriteOnly),
        "The cross-parent Windows context-menu fixtures should be created.");
    firstContextFile.close();
    secondContextFile.close();
    const QStringList settingsBeforeShareValidation = QSettings().allKeys();
    const auto validShare = filemanager::platform::validateWindowsShareSelection(
        {contextFirst, contextFirst, contextOtherFolder});
    QStringList tooManyShareItems;
    for (int item = 0; item < 33; ++item)
        tooManyShareItems.append(contextFirst);
    passed &= expect(validShare.valid && validShare.absolutePaths.size() == 2
            && validShare.providerBackedItems == 0,
        "Share validation should accept available files and folders and remove duplicates.");
#ifdef Q_OS_WIN
    const DWORD shareOriginalAttributes = GetFileAttributesW(
        reinterpret_cast<LPCWSTR>(contextFirst.utf16()));
    const bool shareOfflineFixture = shareOriginalAttributes != INVALID_FILE_ATTRIBUTES
        && SetFileAttributesW(reinterpret_cast<LPCWSTR>(contextFirst.utf16()),
            shareOriginalAttributes | FILE_ATTRIBUTE_OFFLINE);
    if (shareOfflineFixture) {
        const auto providerShare = filemanager::platform::validateWindowsShareSelection(
            {contextFirst, contextFirst});
        passed &= expect(providerShare.valid && providerShare.absolutePaths.size() == 1
                && providerShare.providerBackedItems == 1,
            "Share preflight should classify a provider-backed selection once without reading content.");
        SetFileAttributesW(reinterpret_cast<LPCWSTR>(contextFirst.utf16()),
            shareOriginalAttributes);
    }
#endif
    passed &= expect(!filemanager::platform::validateWindowsShareSelection({}).valid
            && !filemanager::platform::validateWindowsShareSelection({"relative.txt"}).valid
            && !filemanager::platform::validateWindowsShareSelection(
                {settingsStore.filePath("missing-share.txt")}).valid
            && !filemanager::platform::validateWindowsShareSelection(tooManyShareItems).valid,
        "Share validation should enforce availability, absolute paths, and the 32-item limit.");
    passed &= expect(QSettings().allKeys() == settingsBeforeShareValidation,
        "Share validation should not persist selection or sharing metadata.");
#ifdef Q_OS_WIN
    QString shareCapabilityError;
    passed &= expect(filemanager::platform::windowsShareContractAvailable(&shareCapabilityError),
        "The supported Windows desktop Share contract should be available.");
    if (!shareCapabilityError.isEmpty())
        std::fprintf(stderr, "Windows Share capability error: %s\n",
            shareCapabilityError.toUtf8().constData());

    // Malformed shared-registry values must never be treated as our ownership.
    // In particular, an odd byte count must not overrun a wchar_t-sized buffer.
    const QString registryOwner = QStringLiteral("Kiatami.FileManager");
    const QByteArray ownerBytes(reinterpret_cast<const char*>(registryOwner.utf16()),
        registryOwner.size() * static_cast<qsizetype>(sizeof(wchar_t)));
    const QList<std::pair<DWORD, QByteArray>> malformedOwners = {
        {REG_SZ, QByteArray{}}, {REG_SZ, QByteArray::fromHex("410042")},
        {REG_SZ, ownerBytes}, {REG_BINARY, ownerBytes + QByteArray(2, '\0')},
        {REG_SZ, QByteArray(65538, '\0')}};
    for (const auto& malformed : malformedOwners) {
        for (bool appPaths : {false, true}) {
            TemporaryRegistryTree malformedRegistry;
            const QString root = malformedRegistry.root + (appPaths ? "\\App Paths" : "\\Classes");
            const QString executable = QCoreApplication::applicationFilePath();
            const QString keyPath = root + (appPaths ? '\\' + QFileInfo(executable).fileName()
                : QStringLiteral("\\Directory\\shell\\FileManager.Open"));
            HKEY key = nullptr;
            const LSTATUS created = RegCreateKeyExW(HKEY_CURRENT_USER,
                reinterpret_cast<LPCWSTR>(keyPath.utf16()), 0, nullptr, 0, KEY_WRITE, nullptr, &key, nullptr);
            // R20 read back an extra terminator for the unterminated payload.
            // Avoid implicit zero backing bytes immediately after the
            // deliberately malformed payload, without including them in cbData.
            const QByteArray writeBuffer = malformed.second + QByteArray::fromHex("41004100");
            const LSTATUS written = created == ERROR_SUCCESS
                ? RegSetValueExW(key, L"FileManagerOwner", 0, malformed.first,
                    reinterpret_cast<const BYTE*>(writeBuffer.constData()),
                    static_cast<DWORD>(malformed.second.size())) : created;
            if (key) RegCloseKey(key);
            // Read back the stored bytes before invoking the public API. Windows
            // may normalize a supplied string; a setup problem must be distinct
            // from an ownership-reader or mutation failure.
            HKEY observedKey = nullptr;
            DWORD storedType = 0;
            DWORD storedSize = 0;
            LSTATUS observed = RegOpenKeyExW(HKEY_CURRENT_USER,
                reinterpret_cast<LPCWSTR>(keyPath.utf16()), 0, KEY_QUERY_VALUE, &observedKey);
            QByteArray storedBytes;
            if (observed == ERROR_SUCCESS) {
                observed = RegQueryValueExW(observedKey, L"FileManagerOwner", nullptr,
                    &storedType, nullptr, &storedSize);
                if (observed == ERROR_SUCCESS && storedSize <= 128 * 1024) {
                    storedBytes.resize(storedSize);
                    observed = RegQueryValueExW(observedKey, L"FileManagerOwner", nullptr,
                        &storedType, reinterpret_cast<BYTE*>(storedBytes.data()), &storedSize);
                    if (observed == ERROR_SUCCESS)
                        storedBytes.resize(storedSize);
                } else if (observed == ERROR_SUCCESS) {
                    observed = ERROR_MORE_DATA;
                }
                RegCloseKey(observedKey);
            }
            bool manageable = false;
            bool removalSucceeded = false;
            QString statusDetail;
            QString removalError;
            if (appPaths) {
                const auto status = filemanager::platform::windowsAppPathsStatus(executable, root);
                manageable = status.manageable;
                statusDetail = status.detail;
                const auto removal = filemanager::platform::setWindowsAppPathsRegistered(false, executable, root);
                removalSucceeded = removal.success;
                removalError = removal.error;
            } else {
                const auto status = filemanager::platform::windowsFolderCommandStatus(executable, root);
                manageable = status.manageable;
                statusDetail = status.detail;
                const auto removal = filemanager::platform::setWindowsFolderCommandRegistered(false, executable, root);
                removalSucceeded = removal.success;
                removalError = removal.error;
            }
            const bool retained = registryKeyExists(keyPath);
            const bool storedAsWritten = observed == ERROR_SUCCESS && storedType == malformed.first
                && storedBytes == malformed.second;
            passed &= expect(storedAsWritten,
                "Malformed registry fixture must store exactly the requested type and bytes.");
            const bool protectedValue = created == ERROR_SUCCESS && written == ERROR_SUCCESS
                && !manageable && !removalSucceeded && retained;
            if (!protectedValue || !storedAsWritten) {
                std::fprintf(stderr, "Registry ownership fixture: app_paths=%d requested_type=%lu requested_bytes=%lld "
                    "created=%ld written=%ld read=%ld stored_type=%lu stored_bytes=%lu exact_write=%d "
                    "manageable=%d removal_success=%d retained=%d stored_prefix=%s detail=%s removal=%s\n",
                    appPaths, malformed.first, static_cast<long long>(malformed.second.size()), created,
                    written, observed, storedType, storedSize, storedAsWritten, manageable, removalSucceeded,
                    retained, storedBytes.left(64).toHex().constData(), qPrintable(statusDetail), qPrintable(removalError));
            }
            passed &= expect(protectedValue,
                "Malformed registry ownership must be rejected and preserved for folder commands and App Paths.");
        }
    }

    TemporaryRegistryTree folderRegistry;
    const QString testClassesRoot = folderRegistry.root + "\\Classes";
    const QString testExecutable = QCoreApplication::applicationFilePath();
    const auto initialFolderCommand = filemanager::platform::windowsFolderCommandStatus(
        testExecutable, testClassesRoot);
    passed &= expect(!initialFolderCommand.registered && initialFolderCommand.manageable,
        "The per-user Windows folder command should be off when no owned verb exists.");
    const auto registeredFolderCommand =
        filemanager::platform::setWindowsFolderCommandRegistered(
            true, testExecutable, testClassesRoot);
    const auto activeFolderCommand = filemanager::platform::windowsFolderCommandStatus(
        testExecutable, testClassesRoot);
    if (!registeredFolderCommand.success || !activeFolderCommand.current) {
        std::fprintf(stderr, "Folder registration error=%s status=%s registered=%d current=%d manageable=%d\n",
            registeredFolderCommand.error.toUtf8().constData(),
            activeFolderCommand.detail.toUtf8().constData(), activeFolderCommand.registered,
            activeFolderCommand.current, activeFolderCommand.manageable);
    }
    passed &= expect(registeredFolderCommand.success && activeFolderCommand.registered
            && activeFolderCommand.current && activeFolderCommand.manageable,
        "The owned per-user folder and drive commands should register with the current executable.");
    passed &= expect(filemanager::platform::windowsFolderCommandLine(testExecutable)
            == QStringLiteral("\"") + QDir::toNativeSeparators(testExecutable)
                + QStringLiteral("\" \"%1\""),
        "The folder command should quote both the executable and the Windows path placeholder.");
    passed &= expect(registryDefaultValueMissing(testClassesRoot + "\\Directory"),
        "Folder-command registration must not replace the Directory default association.");
    const auto removedFolderCommand = filemanager::platform::setWindowsFolderCommandRegistered(
        false, testExecutable, testClassesRoot);
    if (!removedFolderCommand.success) {
        std::fprintf(stderr, "Folder removal error=%s\n",
            removedFolderCommand.error.toUtf8().constData());
    }
    passed &= expect(removedFolderCommand.success
            && !registryKeyExists(testClassesRoot + "\\Directory\\shell\\FileManager.Open")
            && !registryKeyExists(testClassesRoot + "\\Drive\\shell\\FileManager.Open"),
        "Disabling integration should remove both app-owned verbs completely.");

    const QString foreignVerb =
        testClassesRoot + "\\Directory\\shell\\FileManager.Open";
    HKEY foreignKey = nullptr;
    DWORD foreignDisposition = 0;
    const LSTATUS foreignCreated = RegCreateKeyExW(HKEY_CURRENT_USER,
        reinterpret_cast<const wchar_t*>(foreignVerb.utf16()), 0, nullptr,
        REG_OPTION_NON_VOLATILE, KEY_WRITE, nullptr, &foreignKey, &foreignDisposition);
    Q_UNUSED(foreignDisposition);
    if (foreignKey)
        RegCloseKey(foreignKey);
    const auto foreignStatus = filemanager::platform::windowsFolderCommandStatus(
        testExecutable, testClassesRoot);
    const auto foreignRemoval = filemanager::platform::setWindowsFolderCommandRegistered(
        false, testExecutable, testClassesRoot);
    passed &= expect(foreignCreated == ERROR_SUCCESS && !foreignStatus.manageable
            && !foreignRemoval.success && registryKeyExists(foreignVerb),
        "File Manager must not overwrite or remove a foreign registry verb with the same name.");

    const QByteArray environmentPathBefore = qgetenv("PATH");
    const QString testAppPathsRoot = folderRegistry.root + "\\App Paths";
    const QString appPathsKey = testAppPathsRoot + QLatin1Char('\\')
        + QFileInfo(testExecutable).fileName();
    const auto initialAppPaths = filemanager::platform::windowsAppPathsStatus(
        testExecutable, testAppPathsRoot);
    passed &= expect(!initialAppPaths.registered && initialAppPaths.manageable,
        "The per-user App Paths registration should be absent by default.");
    const auto registeredAppPaths = filemanager::platform::setWindowsAppPathsRegistered(
        true, testExecutable, testAppPathsRoot);
    const auto activeAppPaths = filemanager::platform::windowsAppPathsStatus(
        testExecutable, testAppPathsRoot);
    passed &= expect(registeredAppPaths.success && activeAppPaths.registered
            && activeAppPaths.current && activeAppPaths.manageable,
        "An owned App Paths entry should map the executable name to its exact current path.");
    passed &= expect(registryNamedValueMissing(appPathsKey, L"Path")
            && qgetenv("PATH") == environmentPathBefore,
        "App Paths registration must omit its Path value and leave the process PATH unchanged.");
    const auto removedAppPaths = filemanager::platform::setWindowsAppPathsRegistered(
        false, testExecutable, testAppPathsRoot);
    passed &= expect(removedAppPaths.success && !registryKeyExists(appPathsKey)
            && qgetenv("PATH") == environmentPathBefore,
        "Disabling App Paths integration should remove only its owned key without changing PATH.");

    HKEY foreignAppPathsKey = nullptr;
    DWORD foreignAppPathsDisposition = 0;
    const LSTATUS foreignAppPathsCreated = RegCreateKeyExW(HKEY_CURRENT_USER,
        reinterpret_cast<const wchar_t*>(appPathsKey.utf16()), 0, nullptr,
        REG_OPTION_NON_VOLATILE, KEY_WRITE, nullptr,
        &foreignAppPathsKey, &foreignAppPathsDisposition);
    Q_UNUSED(foreignAppPathsDisposition);
    if (foreignAppPathsKey)
        RegCloseKey(foreignAppPathsKey);
    const auto foreignAppPathsStatus = filemanager::platform::windowsAppPathsStatus(
        testExecutable, testAppPathsRoot);
    const auto foreignAppPathsRemoval = filemanager::platform::setWindowsAppPathsRegistered(
        false, testExecutable, testAppPathsRoot);
    passed &= expect(foreignAppPathsCreated == ERROR_SUCCESS
            && !foreignAppPathsStatus.manageable && !foreignAppPathsRemoval.success
            && registryKeyExists(appPathsKey) && qgetenv("PATH") == environmentPathBefore,
        "File Manager must preserve a foreign App Paths key and never alter PATH while refusing it.");
#endif

    QListWidget searchResultList;
    auto* firstSearchResult = new QListWidgetItem("First", &searchResultList);
    firstSearchResult->setData(Qt::UserRole, contextFirst);
    auto* duplicateSearchResult = new QListWidgetItem("Duplicate", &searchResultList);
    duplicateSearchResult->setData(Qt::UserRole, contextFirst);
    auto* folderSearchResult = new QListWidgetItem("Folder", &searchResultList);
    folderSearchResult->setData(Qt::UserRole, contextOtherFolder);
    auto* staleSearchResult = new QListWidgetItem("Stale", &searchResultList);
    staleSearchResult->setData(Qt::UserRole, archiveRoot.filePath("removed-result.txt"));
    filemanager::ui::SearchResultBar searchResultActions(&searchResultList);
    searchResultList.setCurrentItem(firstSearchResult);
    firstSearchResult->setSelected(true);
    duplicateSearchResult->setSelected(true);
    folderSearchResult->setSelected(true);
    staleSearchResult->setSelected(true);
    const QStringList actionableSearchPaths = searchResultActions.selectedPaths();
    passed &= expect(actionableSearchPaths.size() == 3
            && actionableSearchPaths.contains(QFileInfo(contextFirst).absoluteFilePath())
            && actionableSearchPaths.contains(QFileInfo(contextOtherFolder).absoluteFilePath())
            && actionableSearchPaths.contains(
                QFileInfo(archiveRoot.filePath("removed-result.txt")).absoluteFilePath()),
        "Search-result operations should deduplicate absolute selections without blocking filesystem probes.");
    QStringList clipboardSearchPaths;
    bool clipboardSearchCut = false;
    QObject::connect(&searchResultActions,
        &filemanager::ui::SearchResultBar::clipboardRequested,
        [&](const QStringList& paths, bool cut) {
            clipboardSearchPaths = paths;
            clipboardSearchCut = cut;
        });
    if (QPushButton* cutSearchResults =
            searchResultActions.findChild<QPushButton*>("SearchResultCut")) {
        cutSearchResults->click();
    }
    passed &= expect(clipboardSearchCut && clipboardSearchPaths == actionableSearchPaths,
        "The search-result Cut action should emit the complete bounded live selection.");
    QString revealedSearchPath;
    QObject::connect(&searchResultActions,
        &filemanager::ui::SearchResultBar::revealRequested,
        [&](const QString& path) { revealedSearchPath = path; });
    if (QPushButton* revealSearchResult =
            searchResultActions.findChild<QPushButton*>("SearchResultReveal")) {
        revealSearchResult->click();
    }
    passed &= expect(revealedSearchPath == QFileInfo(contextFirst).absoluteFilePath(),
        "Show in folder should target the current result deterministically.");
    const auto singleDetails = filemanager::ui::FileDetailsInspector::inspect({contextFirst});
    const auto hasDetail = [](const auto& snapshot, const QString& label) {
        return std::any_of(snapshot.rows.cbegin(), snapshot.rows.cend(),
            [&](const auto& row) { return row.label == label && !row.value.isEmpty(); });
    };
    passed &= expect(singleDetails.available && singleDetails.paths.size() == 1
            && hasDetail(singleDetails, "Full path") && hasDetail(singleDetails, "Type")
            && hasDetail(singleDetails, "Size") && hasDetail(singleDetails, "Permissions")
            && hasDetail(singleDetails, "Attributes")
            && hasDetail(singleDetails, "Storage/link status")
            && hasDetail(singleDetails, "Cloud Files status"),
        "The read-only details inspector should expose bounded common and operation-relevant metadata.");
    const QString tagDetailsStore = archiveRoot.filePath("details-tags.dat");
    filemanager::tags::TagCatalog detailsCatalog(tagDetailsStore);
    QString detailsTagError;
    const auto detailsIdentity = filemanager::files::captureSourceSnapshot(contextFirst);
    passed &= expect(detailsIdentity.valid && detailsCatalog.load(&detailsTagError)
            && detailsCatalog.assign(
                {{contextFirst, detailsIdentity, {QStringLiteral("Project Alpha")}}},
                &detailsTagError),
        "Create a disposable file tag for read-only Details inspection.");
    const auto taggedDetails = filemanager::ui::FileDetailsInspector::inspect(
        {contextFirst}, tagDetailsStore);
    const auto taggedRow = std::find_if(taggedDetails.rows.cbegin(),
        taggedDetails.rows.cend(), [](const auto& row) {
            return row.label == QStringLiteral("File Manager tags");
        });
    passed &= expect(taggedRow != taggedDetails.rows.cend()
            && taggedRow->value == QStringLiteral("Project Alpha"),
        "Details shows labels from the current native file identity.");
    const auto multiDetails = filemanager::ui::FileDetailsInspector::inspect(
        {contextFirst, contextOtherFolder});
    passed &= expect(multiDetails.available && multiDetails.paths.size() == 2
            && hasDetail(multiDetails, "Files") && hasDetail(multiDetails, "Folders")
            && multiDetails.notice.contains("not traversed"),
        "Multi-item details should summarize selected items without traversing folder contents.");
    const auto staleDetails = filemanager::ui::FileDetailsInspector::inspect(
        {archiveRoot.filePath("missing-details.txt")});
    passed &= expect(!staleDetails.available && hasDetail(staleDetails, "Status"),
        "The details inspector should report an unavailable item without opening or mutating it.");
    filemanager::ui::FileDetailsDialog detailsDialog({contextFirst});
    QEventLoop detailsWait;
    QTimer detailsTimeout;
    detailsTimeout.setSingleShot(true);
    QObject::connect(&detailsDialog, &filemanager::ui::FileDetailsDialog::detailsReady,
        &detailsWait, &QEventLoop::quit);
    QObject::connect(&detailsTimeout, &QTimer::timeout, &detailsWait, &QEventLoop::quit);
    detailsTimeout.start(3000);
    detailsWait.exec();
    auto* detailsTable = detailsDialog.findChild<QTableWidget*>("FileDetailsTable");
    passed &= expect(detailsTable && detailsTable->rowCount() >= 10
            && detailsTable->editTriggers() == QAbstractItemView::NoEditTriggers
            && detailsDialog.findChild<QPushButton*>("CopyFileDetails")
            && detailsDialog.findChild<QPushButton*>("WindowsFileProperties"),
        "The accessible details dialog should render read-only rows and explicit copy/native handoff actions.");
    QProcess nativeContextProbe;
    nativeContextProbe.start(contextHost, {"--probe", contextFirst}, QIODevice::ReadOnly);
    const bool nativeContextStarted = nativeContextProbe.waitForStarted(1000);
    const bool nativeContextFinished = nativeContextStarted
        && nativeContextProbe.waitForFinished(10000);
    const bool nativeContextPassed = nativeContextStarted && nativeContextFinished
        && nativeContextProbe.exitStatus() == QProcess::NormalExit
        && nativeContextProbe.exitCode() == 0;
    if (!nativeContextPassed) {
        std::cerr << "Native context probe diagnostic: started=" << nativeContextStarted
                  << " finished=" << nativeContextFinished
                  << " exit=" << nativeContextProbe.exitCode()
                  << " process_error=" << nativeContextProbe.errorString().toStdString()
                  << " stdout=" << nativeContextProbe.readAllStandardOutput().toStdString()
                  << " stderr=" << nativeContextProbe.readAllStandardError().toStdString()
                  << '\n';
    }
    passed &= expect(nativeContextPassed,
        "The isolated helper should build a real native IContextMenu for a filesystem selection.");
    const auto crossParentContext = filemanager::platform::showWindowsContextMenuIsolated(
        {contextFirst, contextSecond}, 0, 0, 5000);
    passed &= expect(!crossParentContext.success
            && crossParentContext.error.contains("one folder", Qt::CaseInsensitive),
        "The isolated Windows context-menu helper should refuse ambiguous cross-folder selections.");
    passed &= expect(!filemanager::platform::showWindowsContextMenuIsolated(
            {contextFirst}, 0, 0, 999).success,
        "The isolated Windows context-menu client should enforce its timeout policy before launch.");
    const QString safeZip = archiveRoot.filePath("safe.zip");
    passed &= expect(writeStoredZip(safeZip, {{"folder/readme.txt", "safe contents"}}),
        "The safe ZIP fixture should be created.");
    const QString safeDestination = archiveRoot.filePath("safe-output");
    passed &= expect(QDir().mkdir(safeDestination), "The safe extraction destination should be created.");
    const auto safeExtraction = filemanager::files::extractZipSafely(safeZip, safeDestination);
    if (!safeExtraction.success) std::fprintf(stderr, "Safe extraction error: %s\n", safeExtraction.error.toUtf8().constData());
    QFile extracted(QDir(safeDestination).filePath("folder/readme.txt"));
    const bool extractedOpened = extracted.open(QIODevice::ReadOnly);
    const QByteArray extractedBytes = extractedOpened ? extracted.readAll() : QByteArray{};
    if (!extractedOpened || extractedBytes != "safe contents")
        std::fprintf(stderr, "Safe output exists=%d bytes=%s\n", QFileInfo::exists(extracted.fileName()), extractedBytes.constData());
    passed &= expect(safeExtraction.success && extractedOpened && extractedBytes == "safe contents",
        "Bounded extraction should stream a valid ZIP entry to its destination.");
    const auto safeArchivePreview = filemanager::preview::PreviewService{}.load(safeZip);
    passed &= expect(safeArchivePreview.kind == filemanager::preview::PreviewKind::Text
            && safeArchivePreview.text.contains("folder/readme.txt")
            && safeArchivePreview.metadata.contains("1 entry"),
        "The inspector should show a bounded contents summary for a valid ZIP.");
    const auto isolatedArchivePreview = filemanager::preview::PreviewController::loadIsolated(safeZip);
    if (!isolatedArchivePreview.success)
        std::fprintf(stderr, "Isolated archive preview error: %s\n",
            isolatedArchivePreview.error.toUtf8().constData());
    passed &= expect(isolatedArchivePreview.success
            && isolatedArchivePreview.preview.text.contains("folder/readme.txt"),
        "The preview helper should return a valid bounded archive summary across its protocol.");
#ifdef Q_OS_WIN
    phase("preview_cleanup_begin");
    QTemporaryDir cleanupParent;
    QTemporaryDir cleanupOutside;
    const QString cleanupRuntime = QDir(cleanupParent.path()).filePath("PreviewRuntime");
    const QString cleanupNested = QDir(cleanupRuntime).filePath("plugins/nested");
    passed &= expect(cleanupParent.isValid() && cleanupOutside.isValid()
            && QDir().mkpath(cleanupNested),
        "The preview cleanup fixture should create its bounded private runtime tree.");
    QFile cleanupPayload(QDir(cleanupNested).filePath("payload.dll"));
    QFile outsideSentinel(QDir(cleanupOutside.path()).filePath("must-survive.txt"));
    passed &= expect(cleanupPayload.open(QIODevice::WriteOnly)
            && cleanupPayload.write("runtime") == 7
            && outsideSentinel.open(QIODevice::WriteOnly)
            && outsideSentinel.write("outside") == 7,
        "The preview cleanup fixture should create inside and outside sentinel files.");
    cleanupPayload.close();
    outsideSentinel.close();
    const QString cleanupBoundary = QDir(cleanupRuntime).filePath("outside-junction");
    const bool cleanupBoundaryCreated = createJunction(cleanupBoundary, cleanupOutside.path());
    NativeJunctionGuard cleanupBoundaryGuard(cleanupBoundary, cleanupBoundaryCreated);
    QString cleanupError;
    passed &= expect(cleanupBoundaryCreated
            && filemanager::preview::removePreviewRuntimeTree(cleanupRuntime, &cleanupError)
            && !QFileInfo::exists(cleanupRuntime)
            && QFileInfo::exists(outsideSentinel.fileName()),
        "Preview cleanup should remove only the bounded PreviewRuntime tree without following a "
        "directory reparse point.");
    passed &= expect(!filemanager::preview::removePreviewRuntimeTree(
            QDir(cleanupParent.path()).filePath("NotOwned"), &cleanupError),
        "Preview cleanup should refuse a path that is not the exact PreviewRuntime leaf.");

    const auto sandboxProbe = filemanager::preview::runSandboxedPreviewHost(
        safeZip, "probe", 3000, 1024 * 1024);
    QJsonParseError sandboxParseError;
    const QJsonDocument sandboxDocument = QJsonDocument::fromJson(
        sandboxProbe.standardOutput, &sandboxParseError);
    const QJsonObject sandboxEvidence = sandboxDocument.object();
    passed &= expect(sandboxProbe.success && sandboxProbe.sandboxed
            && sandboxProbe.jobRestricted
            && sandboxParseError.error == QJsonParseError::NoError
            && sandboxEvidence.value("appContainer").toBool()
            && sandboxEvidence.value("job").toBool()
            && sandboxEvidence.value("capabilityCount").toInteger(-1) == 0
            && sandboxEvidence.value("readable").toBool()
            && sandboxEvidence.value("writeDenied").toBool(),
        "The real preview host should run in a zero-capability AppContainer and bounded Job "
        "with only a readable, non-writable brokered source handle.");
#endif

    const QString helperFixture = QDir(QCoreApplication::applicationDirPath()).filePath(
#ifdef Q_OS_WIN
        "FileManagerPreviewHelperFixture.exe");
#else
        "FileManagerPreviewHelperFixture");
#endif
    const auto makeProtocolFixture = [&](const QString& name) {
        const QString path = archiveRoot.filePath(name);
        QFile file(path);
        if (file.open(QIODevice::WriteOnly)) file.close();
        return path;
    };
    const auto reportProtocolResult = [](const char* fixture,
            const filemanager::preview::PreviewController::IsolatedResult& result,
            qint64 elapsedMs, int timeoutMs) {
        // Fixed categories only: retain failure evidence without printing
        // source paths, helper output or arbitrary diagnostic text.
        const char* category = result.success ? "unexpected_success"
            : result.error.contains("malformed") ? "malformed"
            : result.error.contains("timed out") ? "timed_out"
            : result.error.contains("could not start") ? "start_failed"
            : result.error.contains("failed or crashed") ? "failed_or_crashed"
            : result.error.contains("exceeded") ? "output_limit"
            : result.error.contains("cancelled") ? "cancelled" : "other_rejection";
        std::printf("preview_protocol case=%s success=%d category=%s elapsed_ms=%lld timeout_ms=%d\n",
            fixture, result.success, category, static_cast<long long>(elapsedMs), timeoutMs);
        std::fflush(stdout);
    };
    QElapsedTimer protocolTimer;
    protocolTimer.start();
    const auto malformedReply = filemanager::preview::PreviewController::loadIsolated(
        makeProtocolFixture("malformed.fixture"), 1000, helperFixture);
    reportProtocolResult("malformed", malformedReply, protocolTimer.elapsed(), 1000);
    passed &= expect(!malformedReply.success && malformedReply.error.contains("malformed"),
        "The main app should reject a successful helper process that returns malformed JSON.");
    protocolTimer.restart();
    const auto timedOutReply = filemanager::preview::PreviewController::loadIsolated(
        makeProtocolFixture("hang.fixture"), 100, helperFixture);
    reportProtocolResult("hang", timedOutReply, protocolTimer.elapsed(), 100);
    passed &= expect(!timedOutReply.success && timedOutReply.error.contains("timed out"),
        "The main app should terminate and reject a hung preview helper.");
    protocolTimer.restart();
    const auto oversizedReply = filemanager::preview::PreviewController::loadIsolated(
        makeProtocolFixture("oversize.fixture"), 3000, helperFixture);
    reportProtocolResult("oversize", oversizedReply, protocolTimer.elapsed(), 3000);
    passed &= expect(!oversizedReply.success && (oversizedReply.error.contains("exceeded")
            || oversizedReply.error.contains("timed out")),
        "The main app should terminate and reject an oversized preview helper reply.");
    std::atomic_bool cancelledPreview = true;
    const auto cancelledReply = filemanager::preview::PreviewController::loadIsolated(
        safeZip, 1000, {}, &cancelledPreview);
    passed &= expect(!cancelledReply.success && cancelledReply.error.contains("cancelled"),
        "Preview cancellation should prevent a helper from starting.");

    const QString malformedZip = archiveRoot.filePath("malformed.zip");
    QFile malformedArchive(malformedZip);
    passed &= expect(malformedArchive.open(QIODevice::WriteOnly)
            && malformedArchive.write("not a ZIP") == 9,
        "The malformed archive preview fixture should be created.");
    malformedArchive.close();
    passed &= expect(filemanager::preview::PreviewService{}.load(malformedZip).kind
            == filemanager::preview::PreviewKind::Error,
        "The inspector should fail closed for a malformed ZIP.");

    const QString oversizedZip = archiveRoot.filePath("oversized.zip");
    passed &= expect(createSparseSizedFile(oversizedZip,
            filemanager::preview::PreviewService::MaximumArchivePreviewSourceBytes + 1),
        "The oversized archive preview fixture should be created sparsely.");
    passed &= expect(filemanager::preview::PreviewService{}.load(oversizedZip).kind
            == filemanager::preview::PreviewKind::Error,
        "The inspector should reject a ZIP above its source-byte ceiling before parsing.");

    const QString traversalZip = archiveRoot.filePath("traversal.zip");
    passed &= expect(writeStoredZip(traversalZip, {{"../escape.txt", "blocked"}}),
        "The traversal ZIP fixture should be created.");
    const QString traversalDestination = archiveRoot.filePath("traversal-output");
    passed &= expect(QDir().mkdir(traversalDestination)
            && !filemanager::files::extractZipSafely(traversalZip, traversalDestination).success
            && !QFileInfo::exists(archiveRoot.filePath("escape.txt")),
        "Extraction should reject traversal before writing outside the destination.");

    const QString collisionZip = archiveRoot.filePath("collision.zip");
    passed &= expect(writeStoredZip(collisionZip, {{"Report.txt", "one"}, {"report.TXT", "two"}}),
        "The collision ZIP fixture should be created.");
    const QString collisionDestination = archiveRoot.filePath("collision-output");
    passed &= expect(QDir().mkdir(collisionDestination)
            && !filemanager::files::extractZipSafely(collisionZip, collisionDestination).success
            && QDir(collisionDestination).isEmpty(),
        "Extraction should reject Windows case collisions before writing.");

    const QString adsZip = archiveRoot.filePath("ads.zip");
    passed &= expect(writeStoredZip(adsZip, {{"document.txt:hidden", "blocked"}}),
        "The alternate-data-stream ZIP fixture should be created.");
    const QString adsDestination = archiveRoot.filePath("ads-output");
    passed &= expect(QDir().mkdir(adsDestination)
            && !filemanager::files::extractZipSafely(adsZip, adsDestination).success
            && QDir(adsDestination).isEmpty(),
        "Extraction should reject NTFS alternate-data-stream names before writing.");

    const QString prefixZip = archiveRoot.filePath("prefix.zip");
    passed &= expect(writeStoredZip(prefixZip, {{"item/child.txt", "child"}, {"item", "file"}}),
        "The file/directory collision ZIP fixture should be created.");
    const QString prefixDestination = archiveRoot.filePath("prefix-output");
    passed &= expect(QDir().mkdir(prefixDestination)
            && !filemanager::files::extractZipSafely(prefixZip, prefixDestination).success
            && QDir(prefixDestination).isEmpty(),
        "Extraction should reject file/directory prefix collisions before writing.");

    const QString overwriteZip = archiveRoot.filePath("overwrite.zip");
    passed &= expect(writeStoredZip(overwriteZip, {{"existing.txt", "replacement"}}),
        "The overwrite ZIP fixture should be created.");
    const QString overwriteDestination = archiveRoot.filePath("overwrite-output");
    passed &= expect(QDir().mkdir(overwriteDestination), "The overwrite destination should be created.");
    QFile existing(QDir(overwriteDestination).filePath("existing.txt"));
    passed &= expect(existing.open(QIODevice::WriteOnly) && existing.write("original") == 8,
        "The existing extraction target should be created.");
    existing.close();
    const auto overwriteResult = filemanager::files::extractZipSafely(overwriteZip, overwriteDestination);
    passed &= expect(!overwriteResult.success && existing.open(QIODevice::ReadOnly)
            && existing.readAll() == "original",
        "Extraction should never overwrite an existing item.");

    std::atomic_bool preCancelled = true;
    const QString cancelledDestination = archiveRoot.filePath("cancelled-output");
    passed &= expect(QDir().mkdir(cancelledDestination)
            && filemanager::files::extractZipSafely(safeZip, cancelledDestination, &preCancelled).cancelled
            && QDir(cancelledDestination).isEmpty(),
        "A cancelled extraction should leave no created items behind.");

    const QString createSource = archiveRoot.filePath("native-source.txt");
    QFile sourceFile(createSource);
    passed &= expect(sourceFile.open(QIODevice::WriteOnly) && sourceFile.write("native ZIP") == 10,
        "The native ZIP source should be created.");
    sourceFile.close();
    const QString cancelledZip = archiveRoot.filePath("cancelled-create.zip");
    const auto cancelledCreation = filemanager::files::createZipWithWindowsTar(
        {createSource}, cancelledZip, &preCancelled);
    passed &= expect(cancelledCreation.cancelled && !QFileInfo::exists(cancelledZip),
        "A pre-cancelled archive creation should not start or commit a ZIP.");
    const QString nativeZip = archiveRoot.filePath("native.zip");
    const auto nativeCreation = filemanager::files::createZipWithWindowsTar({createSource}, nativeZip);
    const QString nativeDestination = archiveRoot.filePath("native-output");
    const bool nativeDestinationCreated = QDir().mkdir(nativeDestination);
    const auto nativeExtraction = nativeCreation.success
        ? filemanager::files::extractZipSafely(nativeZip, nativeDestination)
        : filemanager::files::ZipOperationResult{};
    if (!nativeCreation.success) std::fprintf(stderr, "Native creation error: %s\n", nativeCreation.error.toUtf8().constData());
    if (!nativeExtraction.success) std::fprintf(stderr, "Native extraction error: %s\n", nativeExtraction.error.toUtf8().constData());
    passed &= expect(nativeDestinationCreated && nativeCreation.success && nativeExtraction.success,
        "Windows tar creation should round-trip through bounded extraction.");
    passed &= expect(!filemanager::settings::loadShellPermissions().allowThirdPartyNamespaces,
        "Third-party Shell namespace extensions should be denied by default.");
    filemanager::settings::saveShellPermissions({true});
    passed &= expect(filemanager::settings::loadShellPermissions().allowThirdPartyNamespaces,
        "The explicit third-party namespace permission should persist.");
    filemanager::settings::saveShellPermissions({false});
    passed &= expect(!filemanager::settings::loadShellPermissions().allowThirdPartyNamespaces,
        "Revoking third-party namespace permission should persist.");

    passed &= expect(!filemanager::platform::knownFolderPath(
            filemanager::platform::KnownFolder::Documents).isEmpty()
            && !filemanager::platform::knownFolderPath(
                filemanager::platform::KnownFolder::Desktop).isEmpty(),
        "Windows Known Folder APIs should resolve redirected user locations.");

    QMimeData copiedFiles;
    filemanager::platform::ShellClipboard::populateFileTransfer(&copiedFiles,
        {QStringLiteral("C:/interop/copy.txt")},
        filemanager::platform::ShellTransferEffect::Copy);
    passed &= expect(copiedFiles.hasUrls()
            && copiedFiles.hasFormat(
                filemanager::platform::ShellClipboard::preferredDropEffectMimeType())
            && filemanager::platform::ShellClipboard::preferredEffect(&copiedFiles)
                == filemanager::platform::ShellTransferEffect::Copy,
        "Copied file URLs should advertise the Windows Shell preferred copy effect.");
    QMimeData movedFiles;
    filemanager::platform::ShellClipboard::populateFileTransfer(&movedFiles,
        {QStringLiteral("C:/interop/move.txt")},
        filemanager::platform::ShellTransferEffect::Move);
    passed &= expect(filemanager::platform::ShellClipboard::preferredEffect(&movedFiles)
            == filemanager::platform::ShellTransferEffect::Move,
        "Cut file URLs should advertise the Windows Shell preferred move effect.");
    QMimeData externalShellCut;
    externalShellCut.setUrls({QUrl::fromLocalFile(QStringLiteral("C:/interop/external.txt"))});
    externalShellCut.setData(
        filemanager::platform::ShellClipboard::preferredDropEffectMimeType(),
        QByteArray::fromHex("02000000"));
    passed &= expect(filemanager::platform::ShellClipboard::preferredEffect(&externalShellCut)
            == filemanager::platform::ShellTransferEffect::Move,
        "A Windows Explorer Preferred DropEffect payload should be recognized as a cut.");
    QMimeData malformedShellEffect;
    malformedShellEffect.setData(
        filemanager::platform::ShellClipboard::preferredDropEffectMimeType(), QByteArray("x"));
    passed &= expect(!filemanager::platform::ShellClipboard::preferredEffect(
            &malformedShellEffect).has_value(),
        "Malformed Shell drop-effect data should not be treated as a destructive move.");
#ifdef Q_OS_WIN
    auto* completionClipboard = new QMimeData;
    filemanager::platform::ShellClipboard::populateFileTransfer(completionClipboard,
        {QStringLiteral("C:/interop/completed.txt")},
        filemanager::platform::ShellTransferEffect::Copy);
    QApplication::clipboard()->setMimeData(completionClipboard);
    QApplication::processEvents();
    const quint64 completionSequence =
        filemanager::platform::ShellClipboard::sequenceNumber();
    QString completionError;
    passed &= expect(completionSequence != 0
            && filemanager::platform::ShellClipboard::publishPerformedEffect(
                filemanager::platform::ShellTransferEffect::Copy,
                completionSequence, &completionError),
        "A successful paste should publish native Performed DropEffect and Paste Succeeded data.");
    QApplication::clipboard()->setText("new clipboard owner");
    QApplication::processEvents();
    completionError.clear();
    passed &= expect(!filemanager::platform::ShellClipboard::publishPerformedEffect(
            filemanager::platform::ShellTransferEffect::Move,
            completionSequence, &completionError)
            && completionError.contains("ownership changed"),
        "A completed transfer must not publish effects to a newer clipboard owner.");
#endif

    phase("shell_items_begin");
    QTemporaryDir shellItemRoot;
    QFile shellItemFixture(shellItemRoot.filePath("shell-item.txt"));
    passed &= expect(shellItemFixture.open(QIODevice::WriteOnly)
            && shellItemFixture.write("shell item") == 10,
        "The Shell-item filesystem fixture should be created.");
    shellItemFixture.close();
    const auto fileShellItem = filemanager::platform::ShellItem::resolve(
        shellItemFixture.fileName());
    passed &= expect(fileShellItem.valid && fileShellItem.fileSystem
            && !fileShellItem.idList.isEmpty() && !fileShellItem.parsingName.isEmpty()
            && QFileInfo(fileShellItem.fileSystemPath) == QFileInfo(shellItemFixture.fileName()),
        "A filesystem path should resolve to an absolute Shell item identity.");
    const auto fileShellParent = filemanager::platform::ShellItem::parent(fileShellItem);
    passed &= expect(fileShellParent.valid && fileShellParent.folder
            && fileShellParent.fileSystem
            && QFileInfo(fileShellParent.fileSystemPath) == QFileInfo(shellItemRoot.path()),
        "A Shell-item identity should navigate to its native parent without parsing its name.");
    QFile secondShellItemFixture(shellItemRoot.filePath("second-shell-item.txt"));
    passed &= expect(secondShellItemFixture.open(QIODevice::WriteOnly)
            && secondShellItemFixture.write("second") == 6,
        "The bounded Shell enumeration fixture should be created.");
    secondShellItemFixture.close();
    const auto shellFolderItem = filemanager::platform::ShellItem::resolve(shellItemRoot.path());
    const auto boundedShellChildren =
        filemanager::platform::ShellItem::enumerateChildren(shellFolderItem, 1);
    passed &= expect(shellFolderItem.valid && shellFolderItem.folder
            && boundedShellChildren.success && boundedShellChildren.items.size() == 1
            && boundedShellChildren.truncated && boundedShellChildren.items.constFirst().valid,
        "Shell child enumeration should return stable identities and enforce its item cap.");
    passed &= expect(!filemanager::platform::ShellItem::enumerateChildren(
            shellFolderItem, 0).success,
        "Shell child enumeration should reject an unbounded or empty item allowance.");
    // Native integration cases opt in explicitly; temporary/default-off cases are separate.
    filemanager::settings::saveShellPermissions({true});
    filemanager::ui::ShellNamespaceBrowser shellBrowser(shellFolderItem);
    shellBrowser.show();
    auto* shellBrowserItems = shellBrowser.findChild<QListWidget*>("ShellNamespaceItems");
    bool browserFinished = false;
    QString browserError;
    QObject::connect(shellBrowser.findChild<FilePane*>(), &FilePane::shellLoadFinished, &shellBrowser,
        [&browserFinished, &browserError](bool, bool, const QString& error) {
            browserFinished = true;
            browserError = error;
        });
    QElapsedTimer shellBrowserWait;
    shellBrowserWait.start();
    while (shellBrowserItems && !browserFinished
            && shellBrowserWait.elapsed() < 7000) {
        QApplication::processEvents(QEventLoop::AllEvents, 50);
    }
    std::fprintf(stderr, "shell_diagnostic browser enabled=%d count=%d elapsed_ms=%lld error=%s\n",
        shellBrowserItems && shellBrowserItems->isEnabled(),
        shellBrowserItems ? shellBrowserItems->count() : -1,
        static_cast<long long>(shellBrowserWait.elapsed()), browserError.toUtf8().constData());
    passed &= expect(shellBrowserItems && shellBrowserItems->isEnabled()
            && shellBrowserItems->count() == 2 && shellBrowserItems->dragEnabled(),
        "The in-app Shell namespace browser should asynchronously render bounded children.");
    const auto shellBrowserButtons = shellBrowser.findChildren<QPushButton*>();
    passed &= expect(std::any_of(shellBrowserButtons.cbegin(), shellBrowserButtons.cend(),
            [](const QPushButton* button) {
                return button->text().startsWith("Copy to current folder");
            }),
        "The Shell browser should expose an explicit keyboard-accessible copy action.");
    shellBrowser.close();
    const QString shellHost = QDir(QCoreApplication::applicationDirPath()).filePath(
        QStringLiteral("FileManagerShellHost.exe"));
    const auto isolatedShellChildren =
        filemanager::platform::ShellItem::enumerateChildrenIsolated(
            shellFolderItem, 1, 3000, shellHost);
    passed &= expect(isolatedShellChildren.success
            && isolatedShellChildren.items.size() == 1 && isolatedShellChildren.truncated,
        "The isolated Shell host should return bounded child identities over validated JSON.");
    passed &= expect(!filemanager::platform::ShellItem::enumerateChildrenIsolated(
            shellFolderItem, 1, 50, shellHost).success,
        "The isolated Shell host should reject a timeout below the containment policy.");
    passed &= expect(!filemanager::platform::ShellItem::enumerateChildrenIsolated(
            shellFolderItem, 1, 1000, shellHost + QStringLiteral(".missing")).success,
        "A missing Shell helper must be contained and reported without in-process fallback.");
    const auto isolatedParent = filemanager::platform::ShellItem::parentIsolated(
        fileShellItem, 3000, shellHost);
    passed &= expect(isolatedParent.valid && isolatedParent.folder && isolatedParent.fileSystem
            && isolatedParent.idList == fileShellParent.idList
            && QFileInfo(isolatedParent.fileSystemPath) == QFileInfo(shellItemRoot.path()),
        "Isolated parent resolution should return the exact native parent without reparsing a name.");
    auto rootShellIdentity = fileShellItem;
    rootShellIdentity.idList = QByteArray(2, '\0');
    auto malformedParentSource = fileShellItem;
    malformedParentSource.idList = QByteArray::fromHex("ffff0000");
    passed &= expect(filemanager::platform::ShellItem::hasParentIdentity(fileShellItem)
            && !filemanager::platform::ShellItem::hasParentIdentity(rootShellIdentity)
            && !filemanager::platform::ShellItem::hasParentIdentity(malformedParentSource)
            && !filemanager::platform::ShellItem::parentIsolated(rootShellIdentity).valid
            && !filemanager::platform::ShellItem::parentIsolated(malformedParentSource).valid,
        "Parent availability must use structural identity checks and reject root/malformed PIDLs.");
    passed &= expect(!filemanager::platform::ShellItem::parentIsolated(
            fileShellItem, 50, shellHost).valid
            && !filemanager::platform::ShellItem::parentIsolated(
                fileShellItem, 1000, shellHost + QStringLiteral(".missing")).valid,
        "Isolated parent resolution must enforce timeout policy and refuse missing-helper fallback.");
    const auto parentProtocolFixture = [](const QByteArray& mode) {
        filemanager::platform::ShellItemReference item;
        item.valid = true;
        item.idList.append(static_cast<char>(mode.size() + 2));
        item.idList.append('\0');
        item.idList.append(mode);
        item.idList.append(QByteArray(2, '\0'));
        return item;
    };
    const auto verifyParentRejection = [&](const QByteArray& mode, int timeoutMilliseconds,
                                          const char* expectedError, const char* failureMessage) {
        QElapsedTimer elapsed;
        elapsed.start();
        const auto reply = filemanager::platform::ShellItem::parentIsolated(
            parentProtocolFixture(mode), timeoutMilliseconds, helperFixture);
        const qint64 elapsedMilliseconds = elapsed.elapsed();
        const bool rejected = !reply.valid && reply.error.contains(QLatin1String(expectedError));
        if (!rejected) {
            std::cerr << "Parent helper fixture: mode=" << mode.constData()
                      << " timeout_ms=" << timeoutMilliseconds
                      << " elapsed_ms=" << elapsedMilliseconds
                      << " valid=" << reply.valid
                      << " error=" << reply.error.toStdString() << '\n';
        }
        return expect(rejected, failureMessage);
    };
    passed &= verifyParentRejection("invalid-parent", 3000, "invalid item",
        "Parent helper containment must reject a malformed reply identity.");
    passed &= verifyParentRejection("wrong-parent", 3000, "requested parent",
        "Parent helper containment must reject an unrelated reply identity.");
    passed &= verifyParentRejection("hang-parent", 100, "timed out",
        "Parent helper containment must time out and stop a hung host.");
    const auto isolatedResolvedFolder = filemanager::platform::ShellItem::resolveIsolated(
        shellItemRoot.path(), 3000, shellHost);
    std::fprintf(stderr, "shell_diagnostic resolve valid=%d folder=%d native=%lld identity_equal=%d path_equal=%d error=%s\n",
        isolatedResolvedFolder.valid, isolatedResolvedFolder.folder,
        static_cast<long long>(isolatedResolvedFolder.nativeCode),
        isolatedResolvedFolder.idList == shellFolderItem.idList,
        QFileInfo(isolatedResolvedFolder.fileSystemPath) == QFileInfo(shellItemRoot.path()),
        isolatedResolvedFolder.error.toUtf8().constData());
    if (isolatedResolvedFolder.valid && filemanager::platform::ShellItem::isValidSerializedIdentity(isolatedResolvedFolder.idList)) {
        std::fprintf(stderr, "shell_diagnostic native_identity_equal=%d\n",
            ILIsEqual(reinterpret_cast<PCIDLIST_ABSOLUTE>(isolatedResolvedFolder.idList.constData()),
                reinterpret_cast<PCIDLIST_ABSOLUTE>(shellFolderItem.idList.constData())));
    }
    for (const bool resolveDiagnostic : {false, true}) {
        QProcess protocolDiagnostic;
        protocolDiagnostic.setProgram(shellHost);
        protocolDiagnostic.setArguments(resolveDiagnostic
            ? QStringList{QStringLiteral("--resolve"), shellItemRoot.path()}
            : QStringList{QString::fromLatin1(shellFolderItem.idList.toBase64(
                QByteArray::Base64UrlEncoding | QByteArray::OmitTrailingEquals)), QStringLiteral("1")});
        protocolDiagnostic.start(QIODevice::ReadOnly);
        const bool completed = protocolDiagnostic.waitForStarted(1000)
            && protocolDiagnostic.waitForFinished(3000);
        if (!completed && protocolDiagnostic.state() != QProcess::NotRunning) {
            protocolDiagnostic.kill();
            protocolDiagnostic.waitForFinished(1000);
        }
        const QByteArray reply = protocolDiagnostic.readAllStandardOutput();
        QJsonParseError error;
        const auto document = QJsonDocument::fromJson(reply, &error);
        std::fprintf(stderr, "shell_diagnostic direct mode=%s complete=%d exit=%d bytes=%lld parse=%d offset=%lld object=%d first=%s last=%s\n",
            resolveDiagnostic ? "resolve" : "enumerate", completed, protocolDiagnostic.exitCode(),
            static_cast<long long>(reply.size()), static_cast<int>(error.error),
            static_cast<long long>(error.offset), document.isObject(),
            reply.first(16).toHex().constData(), reply.last(16).toHex().constData());
    }
    // Equivalent native items can have different serialized PIDLs on supported Windows.
    passed &= expect(isolatedResolvedFolder.valid && isolatedResolvedFolder.folder
            && sameWindowsShellItem(isolatedResolvedFolder.idList, shellFolderItem.idList)
            && QFileInfo(isolatedResolvedFolder.fileSystemPath) == QFileInfo(shellItemRoot.path()),
        "Isolated arbitrary resolution must preserve the native folder identity.");
    QTemporaryDir unrelatedShellRoot;
    const auto unrelatedShellFolder = filemanager::platform::ShellItem::resolve(unrelatedShellRoot.path());
    passed &= expect(unrelatedShellRoot.isValid() && unrelatedShellFolder.valid
            && !sameWindowsShellItem(isolatedResolvedFolder.idList, unrelatedShellFolder.idList),
        "Native identity comparison must reject a different disposable folder.");
    auto cancelledShellRequest = std::make_shared<std::atomic_bool>(true);
    passed &= expect(filemanager::platform::ShellItem::resolveIsolated(
            shellItemRoot.path(), 3000, shellHost + ".missing", cancelledShellRequest).error.contains("cancelled")
            && filemanager::platform::ShellItem::enumerateChildrenIsolated(
                shellFolderItem, 1, 3000, shellHost + ".missing", cancelledShellRequest).error.contains("cancelled"),
        "A revoked request must be refused before attempting to launch a helper.");
    {
        auto cancellation = std::make_shared<std::atomic_bool>(false);
        QThread* revoker = QThread::create([cancellation] {
            QThread::msleep(150);
            cancellation->store(true);
        });
        revoker->start();
        const auto stopped = filemanager::platform::ShellItem::parentIsolated(
            parentProtocolFixture("hang-parent"), 5000, helperFixture, cancellation);
        revoker->wait();
        delete revoker;
        passed &= expect(!stopped.valid && stopped.error.contains("cancelled"),
            "Cancellation during helper execution must terminate the request instead of waiting for its deadline.");
    }
    {
        filemanager::settings::saveShellPermissions({false});
        FilePane consentPane(shellItemRoot.path());
        consentPane.resize(850, 650);
        consentPane.show();
        int providerRequests = 0;
        bool safeLoadDone = false;
        QObject::connect(&consentPane, &FilePane::shellProviderRequestStarted, &consentPane,
            [&providerRequests] { ++providerRequests; });
        auto safeConnection = QObject::connect(&consentPane, &FilePane::shellLoadFinished, &consentPane,
            [&safeLoadDone](bool success, bool, const QString&) { safeLoadDone = success; });
        consentPane.navigateBuiltInShell(filemanager::platform::BuiltInShellLocation::ThisPc);
        QElapsedTimer consentWait;
        consentWait.start();
        while (!safeLoadDone && consentWait.elapsed() < 7000)
            QApplication::processEvents(QEventLoop::AllEvents, 50);
        passed &= expect(safeLoadDone && providerRequests == 0 && consentPane.visibleItemCount() > 0
                && consentPane.currentShellLocation().idList.isEmpty()
                && consentPane.shellNavigationItems().isEmpty()
                && !consentPane.hasTemporaryShellVisit(),
            "Default-off This PC must list known paths without resolving or enumerating a namespace.");
        QObject::disconnect(safeConnection);
        consentPane.navigateShellParsingName(shellItemRoot.path());
        auto* allow = consentPane.findChild<QPushButton*>("AllowShellVisit");
        auto* handoff = consentPane.findChild<QPushButton*>("OpenShellInWindows");
        auto* cancel = consentPane.findChild<QPushButton*>("CancelShellVisit");
        passed &= expect(providerRequests == 0 && consentPane.visibleItemCount() == 0
                && consentPane.shellNavigationItems().isEmpty()
                && allow && allow->isVisible() && handoff && cancel,
            "An unresolved virtual destination must offer consent before any provider request.");
        QString handedOff;
        QObject::connect(&consentPane, &FilePane::shellItemOpenRequested, &consentPane,
            [&handedOff](const QString& target) { handedOff = target; });
        if (handoff)
            handoff->click();
        passed &= expect(handedOff == shellItemRoot.path() && providerRequests == 0
                && !consentPane.hasTemporaryShellVisit(),
            "Windows handoff must not grant access or resolve the destination in File Manager.");
        bool visitLoaded = false;
        QString visitLoadError;
        QObject::connect(&consentPane, &FilePane::shellLoadFinished, &consentPane,
            [&visitLoaded, &visitLoadError](bool success, bool, const QString& error) {
                visitLoaded = success;
                visitLoadError = error;
            });
        if (allow)
            allow->click();
        consentWait.restart();
        while (!visitLoaded && consentWait.elapsed() < 12000)
            QApplication::processEvents(QEventLoop::AllEvents, 50);
        std::fprintf(stderr, "shell_diagnostic visit loaded=%d requests=%d granted=%d count=%d tree=%lld error=%s\n",
            visitLoaded, providerRequests, consentPane.hasTemporaryShellVisit(),
            consentPane.visibleItemCount(), static_cast<long long>(consentPane.shellNavigationItems().size()),
            visitLoadError.toUtf8().constData());
        passed &= expect(visitLoaded && providerRequests == 1 && consentPane.hasTemporaryShellVisit()
                && consentPane.visibleItemCount() == 2
                && !filemanager::settings::loadShellPermissions().allowThirdPartyNamespaces,
            "One-visit consent must enable isolated browsing without persisting permission.");
        const QByteArray visitIdentity = consentPane.currentShellLocation().idList;
        passed &= expect(consentPane.shellNavigationItems().size() == 1
                && consentPane.navigateShellVisitItem(visitIdentity)
                && !consentPane.navigateShellVisitItem(QByteArray::fromHex("020002000000"))
                && providerRequests == 1,
            "The tree snapshot must omit files and reject unknown identities without provider work.");
        QPushButton* visitUp = nullptr;
        for (auto* button : consentPane.findChildren<QPushButton*>())
            if (button->accessibleName() == "Up one folder")
                visitUp = button;
        passed &= expect(visitUp && !visitUp->isEnabled(),
            "Up must not resolve a parent outside the temporary visit's granted root.");
        consentPane.navigateHome();
        passed &= expect(consentPane.shellNavigationItems().isEmpty()
                && !consentPane.navigateShellVisitItem(visitIdentity),
            "Leaving a visit must invalidate its tree identities immediately.");
        QPushButton* consentBack = nullptr;
        for (auto* button : consentPane.findChildren<QPushButton*>())
            if (button->accessibleName() == "Back")
                consentBack = button;
        if (consentBack)
            consentBack->click();
        passed &= expect(consentBack && !consentPane.hasTemporaryShellVisit()
                && providerRequests == 1,
            "History must not restore a temporary grant after leaving a virtual visit.");
        consentPane.navigateShellParsingName(shellItemRoot.path());
        auto revokeConnection = QObject::connect(&consentPane, &FilePane::shellProviderRequestStarted,
            &consentPane, [] { filemanager::settings::saveShellPermissions({false}); });
        if (allow)
            allow->click();
        passed &= expect(providerRequests == 2 && !consentPane.hasTemporaryShellVisit()
                && consentPane.visibleItemCount() == 0,
            "Revocation at dispatch must cancel the grant and keep provider results empty.");
        QObject::disconnect(revokeConnection);
        if (cancel)
            cancel->click();
        passed &= expect(consentPane.isHomeLocation() && !consentPane.hasTemporaryShellVisit(),
            "Cancel must return to Home without provider access.");

        TabbedPanel consentTabs(shellItemRoot.path());
        auto virtualFixture = shellFolderItem;
        virtualFixture.fileSystem = false;
        auto* firstVisit = consentTabs.currentPane();
        firstVisit->navigateShellItem(virtualFixture);
        firstVisit->findChild<QPushButton*>("AllowShellVisit")->click();
        auto* duplicateVisit = consentTabs.duplicateCurrentTab();
        passed &= expect(firstVisit->hasTemporaryShellVisit() && duplicateVisit
                && !duplicateVisit->hasTemporaryShellVisit() && duplicateVisit->visibleItemCount() == 0,
            "Duplicating a tab must copy its destination without copying the visit grant.");
        consentTabs.closeCurrentTab();
        auto* reopenedVisit = consentTabs.reopenLastClosedTab();
        passed &= expect(reopenedVisit && !reopenedVisit->hasTemporaryShellVisit()
                && reopenedVisit->visibleItemCount() == 0,
            "Reopening a virtual tab must require a new visit grant.");
        QWidget otherWindow;
        FilePane otherVisit(shellItemRoot.path(), &otherWindow);
        otherVisit.navigateShellItem(virtualFixture);
        otherVisit.findChild<QPushButton*>("AllowShellVisit")->click();
        filemanager::settings::saveShellPermissions({false});
        passed &= expect(!firstVisit->hasTemporaryShellVisit() && !otherVisit.hasTemporaryShellVisit()
                && firstVisit->visibleItemCount() == 0 && otherVisit.visibleItemCount() == 0,
            "Revocation must end temporary visits and clear provider results in every window.");
    }
    filemanager::settings::saveShellPermissions({true});
    QByteArray virtualShellIdListPayload;
#ifdef Q_OS_WIN
    using filemanager::platform::BuiltInShellLocation;
    const auto thisPcShellItem = filemanager::platform::ShellItem::resolveBuiltIn(
        BuiltInShellLocation::ThisPc);
    const auto librariesShellItem = filemanager::platform::ShellItem::resolveBuiltIn(
        BuiltInShellLocation::Libraries);
    const auto recycleBinShellItem = filemanager::platform::ShellItem::resolveBuiltIn(
        BuiltInShellLocation::RecycleBin);
    passed &= expect(thisPcShellItem.valid && !thisPcShellItem.fileSystem
            && thisPcShellItem.folder && !thisPcShellItem.idList.isEmpty()
            && librariesShellItem.valid && !librariesShellItem.fileSystem
            && librariesShellItem.folder && !librariesShellItem.idList.isEmpty()
            && recycleBinShellItem.valid && !recycleBinShellItem.fileSystem
            && recycleBinShellItem.folder && !recycleBinShellItem.idList.isEmpty(),
        "Documented Known Folder identities should resolve This PC, Libraries and Recycle Bin.");
    const QString thisPcIdentity = filemanager::platform::ShellItem::builtInIdentity(
        BuiltInShellLocation::ThisPc);
    const QString librariesIdentity = filemanager::platform::ShellItem::builtInIdentity(
        BuiltInShellLocation::Libraries);
    const QString recycleBinIdentity = filemanager::platform::ShellItem::builtInIdentity(
        BuiltInShellLocation::RecycleBin);
    passed &= expect(filemanager::platform::ShellItem::builtInLocationFromIdentity(thisPcIdentity)
                == BuiltInShellLocation::ThisPc
            && filemanager::platform::ShellItem::builtInLocationFromIdentity(librariesIdentity)
                == BuiltInShellLocation::Libraries
            && filemanager::platform::ShellItem::builtInLocationFromIdentity(recycleBinIdentity)
                == BuiltInShellLocation::RecycleBin
            && filemanager::platform::ShellItem::builtInLocationFromIdentity(
                QStringLiteral("shell:unknown")) == BuiltInShellLocation::None,
        "Built-in destinations should use narrow stable app identities for persistence.");
    FilePane shellPane(shellItemRoot.path());
    shellPane.resize(800, 600);
    shellPane.show();
    bool integratedShellFinished = false;
    bool integratedShellSucceeded = false;
    QObject::connect(&shellPane, &FilePane::shellLoadFinished, &shellPane,
        [&integratedShellFinished, &integratedShellSucceeded](bool success, bool,
            const QString&) {
            integratedShellFinished = true;
            integratedShellSucceeded = success;
        });
    const bool openedThisPc = shellPane.navigateBuiltInShell(BuiltInShellLocation::ThisPc);
    auto* integratedShellItems =
        shellPane.findChild<QListWidget*>("ShellNamespacePaneItems");
    QElapsedTimer integratedShellWait;
    integratedShellWait.start();
    while (integratedShellItems && !integratedShellFinished
            && integratedShellWait.elapsed() < 7000) {
        QApplication::processEvents(QEventLoop::AllEvents, 50);
    }
    const bool integratedShellPassed = openedThisPc && !shellPane.isFileSystemLocation()
        && shellPane.currentPath().isEmpty()
        && shellPane.builtInShellLocation() == BuiltInShellLocation::ThisPc
        && shellPane.serializedLocation() == thisPcIdentity
        && shellPane.view() == integratedShellItems && integratedShellItems
        && integratedShellFinished && integratedShellSucceeded
        && integratedShellItems->isEnabled() && integratedShellItems->dragEnabled()
        && shellPane.visibleItemCount() <= 1024;
    if (!integratedShellPassed) {
        std::cerr << "Integrated Shell pane diagnostic: opened=" << openedThisPc
                  << " filesystem=" << shellPane.isFileSystemLocation()
                  << " path_empty=" << shellPane.currentPath().isEmpty()
                  << " built_in=" << (shellPane.builtInShellLocation() == BuiltInShellLocation::ThisPc)
                  << " identity=" << (shellPane.serializedLocation() == thisPcIdentity)
                  << " view=" << (shellPane.view() == integratedShellItems)
                  << " items_present=" << (integratedShellItems != nullptr)
                  << " finished=" << integratedShellFinished
                  << " success=" << integratedShellSucceeded
                  << " enabled=" << (integratedShellItems && integratedShellItems->isEnabled())
                  << " drag=" << (integratedShellItems && integratedShellItems->dragEnabled())
                  << " count=" << shellPane.visibleItemCount() << '\n';
    }
    passed &= expect(integratedShellPassed,
        "A pane should report completion and adopt a virtual Shell identity through the isolated "
        "bounded surface without fabricating a filesystem destination.");
    const auto integratedSelection = shellPane.selectedShellItems();
    passed &= expect(integratedSelection.size() <= 1
            && (integratedSelection.isEmpty() || integratedSelection.constFirst().valid),
        "Integrated Shell-pane selection should expose only bounded stable item identities.");
    QPushButton* integratedBack = nullptr;
    for (QPushButton* button : shellPane.findChildren<QPushButton*>()) {
        if (button->accessibleName() == QStringLiteral("Back")) {
            integratedBack = button;
            break;
        }
    }
    if (integratedBack)
        integratedBack->click();
    QApplication::processEvents();
    passed &= expect(integratedBack && shellPane.isFileSystemLocation()
            && QFileInfo(shellPane.currentPath()) == QFileInfo(shellItemRoot.path()),
        "Pane history should cross safely from a virtual Shell item back to the fast filesystem model.");
    {
        // Present a known temporary folder through the virtual surface to exercise real helper
        // navigation with a deterministic filesystem parent, independent of installed providers.
        FilePane parentPane(shellItemRoot.path());
        auto virtualFolder = shellFolderItem;
        virtualFolder.fileSystem = false;
        parentPane.navigateShellItem(virtualFolder);
        QPushButton* parentUp = nullptr;
        for (auto* button : parentPane.findChildren<QPushButton*>())
            if (button->accessibleName() == QStringLiteral("Up one folder"))
                parentUp = button;
        int parentCompletions = 0;
        bool parentNavigated = false;
        QString parentError;
        QObject::connect(&parentPane, &FilePane::shellParentNavigationFinished, &parentPane,
            [&parentCompletions, &parentNavigated, &parentError](bool navigated, const QString& error) {
                ++parentCompletions;
                parentNavigated = navigated;
                parentError = error;
            });
        if (parentUp)
            parentUp->click();
        passed &= expect(parentUp && !parentUp->isEnabled() && !parentPane.isFileSystemLocation(),
            "Shell Up should return before helper completion and disable duplicate requests.");
        if (parentUp)
            parentUp->click();
        QElapsedTimer parentWait;
        parentWait.start();
        while (parentCompletions == 0 && parentWait.elapsed() < 7000)
            QApplication::processEvents(QEventLoop::AllEvents, 50);
        std::fprintf(stderr, "shell_diagnostic up completions=%d navigated=%d filesystem=%d path_equal=%d error=%s\n",
            parentCompletions, parentNavigated, parentPane.isFileSystemLocation(),
            QFileInfo(parentPane.currentPath()) == QFileInfo(QFileInfo(shellItemRoot.path()).absolutePath()),
            parentError.toUtf8().constData());
        passed &= expect(parentCompletions == 1 && parentNavigated && parentPane.isFileSystemLocation()
                && QFileInfo(parentPane.currentPath()) == QFileInfo(QFileInfo(shellItemRoot.path()).absolutePath()),
            "One asynchronous Up request should navigate to the exact resolved parent.");
        parentPane.navigateShellItem(virtualFolder);
        const int beforeStaleRequest = parentCompletions;
        if (parentUp)
            parentUp->click();
        parentPane.navigateHome();
        parentWait.restart();
        while (parentCompletions == beforeStaleRequest && parentWait.elapsed() < 7000)
            QApplication::processEvents(QEventLoop::AllEvents, 50);
        passed &= expect(parentCompletions == beforeStaleRequest + 1 && !parentNavigated
                && parentPane.isHomeLocation() && parentUp && !parentUp->isEnabled(),
            "A late Shell parent reply must not replace a newer Home destination.");
    }
    TabbedPanel shellSessionPanel(shellItemRoot.path());
    shellSessionPanel.currentPane()->navigateBuiltInShell(BuiltInShellLocation::ThisPc);
    FilePane* duplicatedBuiltIn = shellSessionPanel.duplicateCurrentTab();
    const bool duplicatedBuiltInOk = duplicatedBuiltIn
        && duplicatedBuiltIn->builtInShellLocation() == BuiltInShellLocation::ThisPc;
    shellSessionPanel.closeCurrentTab();
    FilePane* reopenedBuiltIn = shellSessionPanel.reopenLastClosedTab();
    TabbedPanel restoredBuiltIns(shellItemRoot.path(), nullptr, false);
    restoredBuiltIns.restoreTabs({thisPcIdentity, librariesIdentity, recycleBinIdentity}, 1);
    passed &= expect(shellSessionPanel.paths().contains(thisPcIdentity)
            && duplicatedBuiltInOk
            && reopenedBuiltIn && reopenedBuiltIn->builtInShellLocation()
                == BuiltInShellLocation::ThisPc
            && restoredBuiltIns.count() == 3
            && restoredBuiltIns.panes().at(0)->builtInShellLocation()
                == BuiltInShellLocation::ThisPc
            && restoredBuiltIns.panes().at(1)->builtInShellLocation()
                == BuiltInShellLocation::Libraries
            && restoredBuiltIns.panes().at(2)->builtInShellLocation()
                == BuiltInShellLocation::RecycleBin,
        "Built-in Windows destinations should survive duplicate, close/reopen and layout restore.");
    TabbedPanel arbitraryShellSessionPanel(shellItemRoot.path());
    arbitraryShellSessionPanel.currentPane()->navigateShellItem(thisPcShellItem);
    passed &= expect(arbitraryShellSessionPanel.paths() == QStringList{shellItemRoot.path()},
        "Arbitrary provider namespaces should still restore to the safe filesystem default.");
    shellSessionPanel.currentPane()->navigate(shellItemRoot.path());
    IDataObject* shellClipboardData = nullptr;
    const quint16 desktopRootTerminator = 0;
    PCUIDLIST_RELATIVE clipboardIdLists[] = {
        reinterpret_cast<PCUIDLIST_RELATIVE>(thisPcShellItem.idList.constData())};
    const HRESULT clipboardObjectStatus = CIDLData_CreateFromIDArray(
        reinterpret_cast<PCIDLIST_ABSOLUTE>(&desktopRootTerminator),
        1, clipboardIdLists, &shellClipboardData);
    const CLIPFORMAT shellIdListFormat = static_cast<CLIPFORMAT>(
        RegisterClipboardFormatW(CFSTR_SHELLIDLIST));
    FORMATETC shellIdListDescription{
        shellIdListFormat, nullptr, DVASPECT_CONTENT, -1, TYMED_HGLOBAL};
    const HRESULT directShellIdListStatus = shellClipboardData
        ? shellClipboardData->QueryGetData(&shellIdListDescription) : E_POINTER;
    if (SUCCEEDED(directShellIdListStatus)) {
        STGMEDIUM shellIdListMedium{};
        if (SUCCEEDED(shellClipboardData->GetData(
                &shellIdListDescription, &shellIdListMedium))) {
            const SIZE_T bytes = GlobalSize(shellIdListMedium.hGlobal);
            const void* locked = GlobalLock(shellIdListMedium.hGlobal);
            if (locked && bytes <= 16U * 1024U * 1024U)
                virtualShellIdListPayload = QByteArray(
                    static_cast<const char*>(locked), static_cast<qsizetype>(bytes));
            if (locked)
                GlobalUnlock(shellIdListMedium.hGlobal);
            ReleaseStgMedium(&shellIdListMedium);
        }
    }
    const HRESULT shellClipboardStatus = SUCCEEDED(clipboardObjectStatus)
        ? OleSetClipboard(shellClipboardData) : clipboardObjectStatus;
    const auto virtualClipboardItems = filemanager::platform::ShellClipboard::shellItems();
    if (shellClipboardData)
        shellClipboardData->Release();
    const bool virtualSnapshotOk = SUCCEEDED(shellClipboardStatus)
        && SUCCEEDED(directShellIdListStatus)
        && virtualClipboardItems.available && virtualClipboardItems.error.isEmpty()
        && virtualClipboardItems.items.size() == 1
        && virtualClipboardItems.items.constFirst().valid
        && !virtualClipboardItems.items.constFirst().fileSystem
        && !virtualClipboardItems.items.constFirst().idList.isEmpty()
        && !virtualClipboardItems.items.constFirst().parsingName.isEmpty();
    if (!virtualSnapshotOk) {
        std::fprintf(stderr, "Virtual clipboard snapshot: hr=0x%08lx query=0x%08lx available=%d error=%s count=%lld\n",
            static_cast<unsigned long>(shellClipboardStatus),
            static_cast<unsigned long>(directShellIdListStatus), virtualClipboardItems.available,
            qPrintable(virtualClipboardItems.error),
            static_cast<long long>(virtualClipboardItems.items.size()));
        if (!virtualClipboardItems.items.isEmpty()) {
            const auto& snapshot = virtualClipboardItems.items.constFirst();
            std::fprintf(stderr, "item valid=%d filesystem=%d folder=%d parsing=%s path=%s bytes=%lld\n",
                snapshot.valid, snapshot.fileSystem, snapshot.folder,
                qPrintable(snapshot.parsingName), qPrintable(snapshot.fileSystemPath),
                static_cast<long long>(snapshot.idList.size()));
        }
    }
    passed &= expect(virtualSnapshotOk,
        "The OLE clipboard adapter should snapshot a virtual Shell selection as an absolute PIDL.");
    passed &= expect(!filemanager::platform::ShellClipboard::shellItems(0).available,
        "The OLE clipboard adapter should reject an item limit outside policy.");
    QMimeData virtualShellMime;
    virtualShellMime.setData(
        filemanager::platform::ShellClipboard::shellIdListMimeType(),
        virtualShellIdListPayload);
    const auto bridgedVirtualItems =
        filemanager::platform::ShellClipboard::shellItems(&virtualShellMime);
    passed &= expect(bridgedVirtualItems.available && bridgedVirtualItems.error.isEmpty()
            && bridgedVirtualItems.items.size() == 1
            && !bridgedVirtualItems.items.constFirst().fileSystem,
        "The Qt OLE bridge should decode the same bounded virtual Shell ID-list payload on drop.");
    QMimeData publishedVirtualShellMime;
    QString publishedVirtualShellError;
    const bool publishedVirtualShell =
        filemanager::platform::ShellClipboard::populateShellItems(
            &publishedVirtualShellMime, {thisPcShellItem}, &publishedVirtualShellError);
    const auto publishedVirtualItems =
        filemanager::platform::ShellClipboard::shellItems(&publishedVirtualShellMime);
    passed &= expect(publishedVirtualShell && publishedVirtualShellError.isEmpty()
            && publishedVirtualShellMime.hasFormat(
                filemanager::platform::ShellClipboard::shellIdListMimeType())
            && filemanager::platform::ShellClipboard::preferredEffect(
                &publishedVirtualShellMime)
                == filemanager::platform::ShellTransferEffect::Copy
            && publishedVirtualItems.available && publishedVirtualItems.error.isEmpty()
            && publishedVirtualItems.items.size() == 1
            && !publishedVirtualItems.items.constFirst().fileSystem,
        "In-app virtual selections should publish a bounded Explorer-compatible copy data object.");
    passed &= expect(!filemanager::platform::ShellClipboard::populateShellItems(
            &publishedVirtualShellMime, {}, &publishedVirtualShellError)
            && !publishedVirtualShellError.isEmpty(),
        "Publishing an empty virtual Shell drag should fail before creating transfer data.");
    QMimeData malformedVirtualShellMime;
    malformedVirtualShellMime.setData(
        filemanager::platform::ShellClipboard::shellIdListMimeType(),
        QByteArray::fromHex("01000000ffffffff"));
    const auto malformedVirtualItems =
        filemanager::platform::ShellClipboard::shellItems(&malformedVirtualShellMime);
    passed &= expect(malformedVirtualItems.available && !malformedVirtualItems.error.isEmpty()
            && malformedVirtualItems.items.isEmpty(),
        "A malformed virtual Shell ID-list drop should fail closed before binding an item.");
    QApplication::clipboard()->setText("clipboard test complete");
    QApplication::processEvents();
#endif
    passed &= expect(!filemanager::platform::ShellItem::resolve({}).valid,
        "An empty Shell-item identity should be rejected safely.");
    filemanager::settings::saveShellPermissions({false});
    auto malformedShellIdentity = shellFolderItem;
    malformedShellIdentity.idList = QByteArray::fromHex("0400ff00");
    passed &= expect(!filemanager::platform::ShellItem::parent(malformedShellIdentity).valid
            && !filemanager::platform::ShellItem::enumerateChildren(
                malformedShellIdentity, 1).success
            && !filemanager::platform::ShellItem::enumerateChildrenIsolated(
                malformedShellIdentity, 1, 1000, shellHost).success,
        "Parent and enumeration entry points should reject a malformed public PIDL value before "
        "calling the Shell or helper process.");

    {
    phase("pane_drop_begin");
    QTemporaryDir paneDropRoot;
    const QString paneDropSourceDirectory = paneDropRoot.filePath("source");
    const QString paneDropDestination = paneDropRoot.filePath("destination");
    QDir().mkpath(paneDropSourceDirectory);
    QDir().mkpath(paneDropDestination);
    const QString paneDropSource = QDir(paneDropSourceDirectory).filePath("queued-drop.txt");
    QFile paneDropFixture(paneDropSource);
    passed &= expect(paneDropFixture.open(QIODevice::WriteOnly)
            && paneDropFixture.write("queued drop") == 11,
        "The controlled pane-drop fixture should be created.");
    paneDropFixture.close();
    FilePane paneDropTarget(isolatedHome.path());
    paneDropTarget.navigate(paneDropDestination);
    paneDropTarget.resize(800, 600);
    paneDropTarget.show();
    QApplication::processEvents();
    QStringList emittedDropPaths;
    QString emittedDropDestination;
    bool emittedDropMove = false;
    bool emittedDropConfirmation = true;
    bool emittedLinkRequest = true;
    QList<filemanager::platform::ShellItemReference> emittedShellDropItems;
    QString emittedShellDropDestination;
    bool emittedShellMoveRequest = false;
    QString emittedShellDropError;
    QObject::connect(&paneDropTarget, &FilePane::filesDropped,
        [&](const QStringList& paths, const QString& destination, bool move,
            bool confirmationRequired, bool linkRequested) {
            emittedDropPaths = paths;
            emittedDropDestination = destination;
            emittedDropMove = move;
            emittedDropConfirmation = confirmationRequired;
            emittedLinkRequest = linkRequested;
        });
    QObject::connect(&paneDropTarget, &FilePane::shellItemsDropped,
        [&](const QList<filemanager::platform::ShellItemReference>& items,
            const QString& destination, bool moveRequested, const QString& error) {
            emittedShellDropItems = items;
            emittedShellDropDestination = destination;
            emittedShellMoveRequest = moveRequested;
            emittedShellDropError = error;
        });
    QMimeData paneDropMime;
    filemanager::platform::ShellClipboard::populateFileTransfer(&paneDropMime,
        {paneDropSource}, filemanager::platform::ShellTransferEffect::Move);
    QDragEnterEvent paneDragEnter(QPoint(700, 500), Qt::CopyAction | Qt::MoveAction,
        &paneDropMime, Qt::LeftButton, Qt::NoModifier);
    QApplication::sendEvent(paneDropTarget.view()->viewport(), &paneDragEnter);
    QDropEvent paneDropEvent(QPointF(700, 500), Qt::CopyAction | Qt::MoveAction,
        &paneDropMime, Qt::LeftButton, Qt::NoModifier);
    QApplication::sendEvent(paneDropTarget.view()->viewport(), &paneDropEvent);
    passed &= expect(emittedDropPaths == QStringList{paneDropSource}
            && QDir::cleanPath(emittedDropDestination) == QDir::cleanPath(paneDropDestination)
            && emittedDropMove && !emittedDropConfirmation && !emittedLinkRequest,
        "A Shell move drop should be routed to the pane transfer request.");
    passed &= expect(QFileInfo::exists(paneDropSource)
            && !QFileInfo::exists(QDir(paneDropDestination).filePath("queued-drop.txt")),
        "The pane must not let QFileSystemModel execute a dropped operation outside the queue.");
    QMimeData ambiguousDropMime;
    ambiguousDropMime.setUrls({QUrl::fromLocalFile(paneDropSource)});
    QDragEnterEvent ambiguousDragEnter(QPoint(700, 500), Qt::CopyAction | Qt::MoveAction,
        &ambiguousDropMime, Qt::LeftButton, Qt::NoModifier);
    QApplication::sendEvent(paneDropTarget.view()->viewport(), &ambiguousDragEnter);
    QDropEvent ambiguousDropEvent(QPointF(700, 500), Qt::CopyAction | Qt::MoveAction,
        &ambiguousDropMime, Qt::LeftButton, Qt::NoModifier);
    QApplication::sendEvent(paneDropTarget.view()->viewport(), &ambiguousDropEvent);
    passed &= expect(emittedDropConfirmation && !emittedLinkRequest,
        "A drop without modifiers or Shell effect metadata should require user confirmation.");
    QDragEnterEvent linkDragEnter(QPoint(700, 500), Qt::CopyAction | Qt::LinkAction,
        &ambiguousDropMime, Qt::LeftButton, Qt::ControlModifier | Qt::ShiftModifier);
    QApplication::sendEvent(paneDropTarget.view()->viewport(), &linkDragEnter);
    QDropEvent linkDropEvent(QPointF(700, 500), Qt::CopyAction | Qt::LinkAction,
        &ambiguousDropMime, Qt::LeftButton, Qt::ControlModifier | Qt::ShiftModifier);
    QApplication::sendEvent(paneDropTarget.view()->viewport(), &linkDropEvent);
    passed &= expect(emittedLinkRequest,
        "A requested link drop should be reported explicitly and never reinterpreted as transfer.");

    QMimeData virtualPaneDropMime;
    virtualPaneDropMime.setData(
        filemanager::platform::ShellClipboard::shellIdListMimeType(),
        virtualShellIdListPayload);
    virtualPaneDropMime.setData(
        filemanager::platform::ShellClipboard::preferredDropEffectMimeType(),
        QByteArray::fromHex("02000000"));
    QDragEnterEvent virtualPaneDragEnter(QPoint(700, 500), Qt::CopyAction | Qt::MoveAction,
        &virtualPaneDropMime, Qt::LeftButton, Qt::NoModifier);
    QApplication::sendEvent(paneDropTarget.view()->viewport(), &virtualPaneDragEnter);
    QDropEvent virtualPaneDrop(QPointF(700, 500), Qt::CopyAction | Qt::MoveAction,
        &virtualPaneDropMime, Qt::LeftButton, Qt::NoModifier);
    QApplication::sendEvent(paneDropTarget.view()->viewport(), &virtualPaneDrop);
    passed &= expect(virtualPaneDragEnter.isAccepted() && virtualPaneDrop.isAccepted()
            && emittedShellDropError.isEmpty() && emittedShellDropItems.size() == 1
            && !emittedShellDropItems.constFirst().fileSystem && emittedShellMoveRequest
            && QDir::cleanPath(emittedShellDropDestination)
                == QDir::cleanPath(paneDropDestination),
        "A virtual Shell pane drop should enumerate only on Drop and request a confirmed native copy.");

    phase("shell_operations_begin");
    QTemporaryDir shellOperationRoot;
    const QString shellCopySourceDirectory = shellOperationRoot.filePath("copy-source");
    const QString shellMoveSourceDirectory = shellOperationRoot.filePath("move-source");
    const QString shellDestinationDirectory = shellOperationRoot.filePath("destination");
    QDir().mkpath(shellCopySourceDirectory);
    QDir().mkpath(shellMoveSourceDirectory);
    QDir().mkpath(shellDestinationDirectory);
    QFile shellCopySource(QDir(shellCopySourceDirectory).filePath("native-copy.txt"));
    QFile shellMoveSource(QDir(shellMoveSourceDirectory).filePath("native-move.txt"));
    passed &= expect(shellCopySource.open(QIODevice::WriteOnly)
            && shellCopySource.write("native copy") == 11,
        "The native-copy fixture should be created.");
    shellCopySource.close();
    passed &= expect(shellMoveSource.open(QIODevice::WriteOnly)
            && shellMoveSource.write("native move") == 11,
        "The native-move fixture should be created.");
    shellMoveSource.close();
    const auto nativeCopySourceItem = filemanager::platform::ShellItem::resolve(
        shellCopySource.fileName());
    const auto nativeDestinationItem = filemanager::platform::ShellItem::resolve(
        shellDestinationDirectory);
    const auto nativeCopy = filemanager::platform::ShellFileOperation::perform(
        filemanager::platform::ShellFileOperationKind::Copy, nativeCopySourceItem,
        nativeDestinationItem);
    if (!nativeCopy.success)
        std::fprintf(stderr, "Native copy failed: 0x%08lx %s\n",
            static_cast<unsigned long>(nativeCopy.nativeCode),
            nativeCopy.error.toUtf8().constData());
    passed &= expect(nativeCopy.success && QFileInfo::exists(
            QDir(shellDestinationDirectory).filePath("native-copy.txt"))
            && QFileInfo::exists(shellCopySource.fileName()),
        "IFileOperation should copy an absolute Shell-item identity through a dedicated STA.");
    const auto nativeCollisionCopy = filemanager::platform::ShellFileOperation::perform(
        filemanager::platform::ShellFileOperationKind::Copy, nativeCopySourceItem,
        nativeDestinationItem);
    const QStringList collisionCopies = QDir(shellDestinationDirectory).entryList(
        {QStringLiteral("native-copy*.txt")}, QDir::Files);
    QFile originalNativeCopy(QDir(shellDestinationDirectory).filePath("native-copy.txt"));
    passed &= expect(originalNativeCopy.open(QIODevice::ReadOnly)
            && originalNativeCopy.readAll() == QByteArrayLiteral("native copy")
            && nativeCollisionCopy.success && collisionCopies.size() == 2,
        "A native Shell collision should keep the existing file and create a separate copy.");
    QFile elevationSource(QDir(shellCopySourceDirectory).filePath("elevation-copy.txt"));
    passed &= expect(elevationSource.open(QIODevice::WriteOnly)
            && elevationSource.write("elevation copy") == 14,
        "The native elevation-policy fixture should be created.");
    elevationSource.close();
    const auto elevationSourceItem =
        filemanager::platform::ShellItem::resolve(elevationSource.fileName());
    const auto elevationPolicyCopy = filemanager::platform::ShellFileOperation::perform(
        filemanager::platform::ShellFileOperationKind::Copy, {elevationSourceItem},
        nativeDestinationItem, {true, 0});
    passed &= expect(elevationPolicyCopy.success
            && QFileInfo::exists(
                QDir(shellDestinationDirectory).filePath("elevation-copy.txt")),
        "The copy-only native elevation-prompt policy should retain normal Shell copy behavior "
        "when no UAC prompt is needed.");
    const auto refusedElevatedMove = filemanager::platform::ShellFileOperation::perform(
        filemanager::platform::ShellFileOperationKind::Move, {elevationSourceItem},
        nativeDestinationItem, {true, 0});
    QList<filemanager::platform::ShellItemReference> oversizedElevatedSelection;
    for (int index = 0; index < 33; ++index)
        oversizedElevatedSelection.append(elevationSourceItem);
    const auto refusedOversizedElevation = filemanager::platform::ShellFileOperation::perform(
        filemanager::platform::ShellFileOperationKind::Copy, oversizedElevatedSelection,
        nativeDestinationItem, {true, 0});
    passed &= expect(!refusedElevatedMove.success
            && refusedElevatedMove.error.contains("non-destructive copies")
            && !refusedOversizedElevation.success,
        "The elevation adapter should reject moves and selections above 32 before entering Windows.");
    const auto nativeMove = filemanager::platform::ShellFileOperation::perform(
        filemanager::platform::ShellFileOperationKind::Move, shellMoveSource.fileName(),
        shellDestinationDirectory);
    if (!nativeMove.success)
        std::fprintf(stderr, "Native move failed: 0x%08lx %s\n",
            static_cast<unsigned long>(nativeMove.nativeCode),
            nativeMove.error.toUtf8().constData());
    passed &= expect(nativeMove.success && QFileInfo::exists(
            QDir(shellDestinationDirectory).filePath("native-move.txt"))
            && !QFileInfo::exists(shellMoveSource.fileName()),
        "IFileOperation should move a local file through a dedicated STA.");
    const auto missingNativeSource = filemanager::platform::ShellFileOperation::perform(
        filemanager::platform::ShellFileOperationKind::Copy,
        shellOperationRoot.filePath("missing.txt"), shellDestinationDirectory);
    passed &= expect(!missingNativeSource.success && !missingNativeSource.error.isEmpty(),
        "The native adapter should return a typed failure for an unavailable source.");
    auto malformedShellSource = nativeCopySourceItem;
    malformedShellSource.idList = QByteArray::fromHex("04000100");
    const auto malformedNativeIdentity = filemanager::platform::ShellFileOperation::perform(
        filemanager::platform::ShellFileOperationKind::Copy, malformedShellSource,
        nativeDestinationItem);
    passed &= expect(!malformedNativeIdentity.success
            && malformedNativeIdentity.error.contains("identity is invalid"),
        "The native adapter should reject a malformed PIDL before entering the Shell.");

    QTemporaryDir permanentDeleteRoot;
    const QString permanentlyDeletedFile =
        permanentDeleteRoot.filePath("permanent-delete-file.txt");
    const QString permanentlyDeletedFolder =
        permanentDeleteRoot.filePath("permanent-delete-folder");
    passed &= expect(QDir().mkpath(permanentlyDeletedFolder),
        "The permanent-delete folder fixture should be created.");
    QFile permanentFile(permanentlyDeletedFile);
    QFile permanentChild(QDir(permanentlyDeletedFolder).filePath("child.txt"));
    passed &= expect(permanentFile.open(QIODevice::WriteOnly)
            && permanentFile.write("permanent file") == 14
            && permanentChild.open(QIODevice::WriteOnly)
            && permanentChild.write("permanent child") == 15,
        "Permanent-delete file and recursive folder fixtures should be created.");
    permanentFile.close();
    permanentChild.close();
    const auto permanentDelete = filemanager::platform::RecycleBinOperation::permanentlyDelete(
        {permanentlyDeletedFile, permanentlyDeletedFolder});
    if (!permanentDelete.success) {
        std::fprintf(stderr, "Native permanent delete failed: 0x%08lx deleted=%lld remaining=%lld file=%d folder=%d %s\n",
            static_cast<unsigned long>(permanentDelete.nativeCode),
            static_cast<long long>(permanentDelete.deletedPaths.size()),
            static_cast<long long>(permanentDelete.remainingPaths.size()),
            QFileInfo::exists(permanentlyDeletedFile),
            QFileInfo::exists(permanentlyDeletedFolder),
            permanentDelete.error.toUtf8().constData());
    }
    passed &= expect(permanentDelete.success && permanentDelete.deletedPaths.size() == 2
            && permanentDelete.remainingPaths.isEmpty()
            && !QFileInfo::exists(permanentlyDeletedFile)
            && !QFileInfo::exists(permanentlyDeletedFolder),
        "IFileOperation should permanently delete a bounded file/folder selection in one STA batch.");

    QTemporaryDir recycleRoot;
    const QString recycledFirstPath = recycleRoot.filePath("recycle-undo-first.txt");
    const QString recycledSecondPath = recycleRoot.filePath("recycle-undo-second.txt");
    QFile recycledFirst(recycledFirstPath);
    QFile recycledSecond(recycledSecondPath);
    passed &= expect(recycledFirst.open(QIODevice::WriteOnly)
            && recycledFirst.write("first recycled payload") == 22,
        "The first Recycle Bin undo fixture should be created.");
    recycledFirst.close();
    passed &= expect(recycledSecond.open(QIODevice::WriteOnly)
            && recycledSecond.write("second recycled payload") == 23,
        "The second Recycle Bin undo fixture should be created.");
    recycledSecond.close();
    const auto recycledFirstResult = filemanager::files::moveToTrashDetailed(recycledFirstPath);
    const auto recycledSecondResult = filemanager::files::moveToTrashDetailed(recycledSecondPath);
    passed &= expect(recycledFirstResult.ok && recycledSecondResult.ok
            && !recycledFirstResult.recycledItemId.isEmpty()
            && !recycledSecondResult.recycledItemId.isEmpty()
            && !QFileInfo::exists(recycledFirstPath) && !QFileInfo::exists(recycledSecondPath),
        "Native deletion should capture exact Recycle Bin identities for every selected item.");
    filemanager::files::SessionUndoStack recycleUndo;
    passed &= expect(recycleUndo.recordTrash({recycledFirstResult, recycledSecondResult})
            && recycleUndo.canUndo(),
        "A verified multi-item Recycle Bin deletion should enter session undo history.");
    const auto recycleUndoResult = recycleUndo.undoLast();
    passed &= expect(recycleUndoResult.ok && QFileInfo::exists(recycledFirstPath)
            && QFileInfo::exists(recycledSecondPath)
            && QFile(recycledFirstPath).open(QIODevice::ReadOnly),
        "Recycle Bin undo should restore each exact item to its original path.");

    const QString refusedRecyclePath = recycleRoot.filePath("recycle-undo-refused.txt");
    QFile refusedRecycleFixture(refusedRecyclePath);
    passed &= expect(refusedRecycleFixture.open(QIODevice::WriteOnly)
            && refusedRecycleFixture.write("original recycled payload") == 25,
        "The Recycle Bin collision fixture should be created.");
    refusedRecycleFixture.close();
    const auto refusedRecycleResult = filemanager::files::moveToTrashDetailed(refusedRecyclePath);
    filemanager::files::SessionUndoStack refusedRecycleUndo;
    passed &= expect(refusedRecycleResult.ok
            && refusedRecycleUndo.recordTrash({refusedRecycleResult}),
        "The Recycle Bin collision fixture should enter session undo history.");
    QFile occupiedRecycleOriginal(refusedRecyclePath);
    passed &= expect(occupiedRecycleOriginal.open(QIODevice::WriteOnly)
            && occupiedRecycleOriginal.write("replacement") == 11,
        "The deleted item's original path should be occupiable before undo.");
    occupiedRecycleOriginal.close();
    const auto refusedRecycleUndoResult = refusedRecycleUndo.undoLast();
    QFile replacementContents(refusedRecyclePath);
    const bool replacementOpened = replacementContents.open(QIODevice::ReadOnly);
    const QByteArray replacementBytes = replacementOpened ? replacementContents.readAll() : QByteArray{};
    replacementContents.close();
    passed &= expect(!refusedRecycleUndoResult.ok
            && replacementBytes == QByteArray("replacement"),
        "Recycle Bin undo refuses an occupied original path without replacing it.");
    QFile::remove(refusedRecyclePath);
    const auto refusedRecycleCleanup = filemanager::platform::RecycleBinOperation::restore(
        {refusedRecycleResult.originalPath, refusedRecycleResult.recycledItemId});
    passed &= expect(refusedRecycleCleanup.success && QFileInfo::exists(refusedRecyclePath),
        "The same captured identity remains restorable after refusal, and the test leaves no Recycle Bin item.");

    const QString selectedRestoreName = QStringLiteral("recycle-selected-restore-%1.txt")
        .arg(QUuid::createUuid().toString(QUuid::WithoutBraces));
    const QString selectedRestorePath = recycleRoot.filePath(selectedRestoreName);
    QFile selectedRestoreFixture(selectedRestorePath);
    passed &= expect(selectedRestoreFixture.open(QIODevice::WriteOnly)
            && selectedRestoreFixture.write("selected restore payload") == 24,
        "The selected-item restore fixture should be created.");
    selectedRestoreFixture.close();
    const auto selectedRestoreTrash =
        filemanager::files::moveToTrashDetailed(selectedRestorePath);
    filemanager::files::SessionUndoStack selectedRestoreUndo;
    passed &= expect(selectedRestoreTrash.ok
            && selectedRestoreUndo.recordTrash({selectedRestoreTrash}),
        "The selected-item fixture should have an exact Recycle Bin identity and undo entry.");
    filemanager::platform::ShellItemReference selectedRestoreIdentity;
    selectedRestoreIdentity.valid = true;
    selectedRestoreIdentity.idList = selectedRestoreTrash.recycledItemId;
    const auto selectedRestore = filemanager::platform::RecycleBinOperation::
        restoreSelectedShellItemsIsolated({selectedRestoreIdentity}, 0, 30000, shellHost);
    selectedRestoreUndo.discardTrashEntriesContaining({selectedRestoreTrash.recycledItemId});
    if (!selectedRestore.success || selectedRestore.restoredItems != 1
            || !QFileInfo::exists(selectedRestorePath) || selectedRestoreUndo.canUndo()) {
        std::fprintf(stderr,
            "Selected restore diagnostic: success=%d aborted=%d code=0x%08lx requested=%d "
            "restored=%d exists=%d undo=%d error=%s\n",
            selectedRestore.success, selectedRestore.aborted,
            static_cast<unsigned long>(selectedRestore.nativeCode),
            selectedRestore.requestedItems, selectedRestore.restoredItems,
            QFileInfo::exists(selectedRestorePath), selectedRestoreUndo.canUndo(),
            selectedRestore.error.toUtf8().constData());
    }
    passed &= expect(selectedRestore.success && selectedRestore.restoredItems == 1
            && QFileInfo::exists(selectedRestorePath) && !selectedRestoreUndo.canUndo(),
        "The isolated Windows Restore verb should restore the exact selected item and invalidate "
        "only its now-obsolete trash undo entry.");
    if (!QFileInfo::exists(selectedRestorePath) && selectedRestoreTrash.ok) {
        const auto selectedRestoreCleanup = filemanager::platform::RecycleBinOperation::restore(
            {selectedRestoreTrash.originalPath, selectedRestoreTrash.recycledItemId});
        passed &= expect(selectedRestoreCleanup.success,
            "A failed selected-item restore test must recover its exact Recycle Bin fixture.");
    }
    const auto recycleSummary = filemanager::platform::RecycleBinOperation::summary();
    passed &= expect(recycleSummary.success,
        "The read-only native Recycle Bin summary should be available without emptying user data.");

    filemanager::files::NetworkDiscoveryService networkDiscovery;
    passed &= expect(!networkDiscovery.isRunning(),
        "Creating the network-discovery service must not start a scan.");
    passed &= expect(filemanager::files::NetworkDiscoveryService::MaximumResources == 256
            && filemanager::files::NetworkDiscoveryService::MaximumDepth == 3,
        "One-time network scans should retain explicit resource and recursion bounds.");

    const QString instanceName = "FileManagerTests."
        + QUuid::createUuid().toString(QUuid::WithoutBraces);
    filemanager::platform::SingleInstanceCoordinator primaryInstance(instanceName);
    passed &= expect(primaryInstance.startOrForward({})
            == filemanager::platform::SingleInstanceCoordinator::Role::Primary,
        "The first user-scoped instance should become primary.");
    QStringList forwardedPaths;
    QEventLoop forwardingWait;
    QTimer forwardingTimeout;
    forwardingTimeout.setSingleShot(true);
    QObject::connect(&forwardingTimeout, &QTimer::timeout,
        &forwardingWait, &QEventLoop::quit);
    QObject::connect(&primaryInstance,
        &filemanager::platform::SingleInstanceCoordinator::pathsReceived,
        &forwardingWait, [&](const QStringList& paths) {
            forwardedPaths = paths;
        });
    QProcess secondaryProcess;
    QObject::connect(&secondaryProcess, &QProcess::finished,
        &forwardingWait, &QEventLoop::quit);
    forwardingTimeout.start(5000);
    secondaryProcess.start(QCoreApplication::applicationFilePath(),
        {"--single-instance-forward", instanceName, "C:/One", "D:/Two"});
    forwardingWait.exec();
    forwardingTimeout.stop();
    const bool secondaryCompleted = secondaryProcess.state() == QProcess::NotRunning;
    if (!secondaryCompleted) {
        secondaryProcess.kill();
        secondaryProcess.waitForFinished(1000);
    }
    const QByteArray helperError = secondaryProcess.readAllStandardError();
    if (!helperError.isEmpty())
        std::fprintf(stderr, "%s", helperError.constData());
    if (secondaryProcess.state() != QProcess::NotRunning
            || secondaryProcess.exitStatus() != QProcess::NormalExit
            || secondaryProcess.exitCode() != 0)
        qWarning() << "Single-instance helper failed:" << secondaryProcess.errorString()
                   << secondaryProcess.state() << secondaryProcess.exitStatus()
                   << secondaryProcess.exitCode()
                   << helperError;
    passed &= expect(secondaryCompleted
            && secondaryProcess.exitStatus() == QProcess::NormalExit
            && secondaryProcess.exitCode() == 0,
        "A later instance should forward startup paths instead of opening another window.");
    passed &= expect(forwardedPaths == QStringList{"C:/One", "D:/Two"},
        "Single-instance IPC should preserve every forwarded startup path.");

    QTemporaryDir launchDirectory;
    passed &= expect(launchDirectory.isValid(), "The secondary launch directory should open.");
    if (launchDirectory.isValid()) {
        QStringList forwardedFolders;
        QProcess folderSecondary;
        QObject::connect(&primaryInstance,
            &filemanager::platform::SingleInstanceCoordinator::foldersReceived,
            &folderSecondary, [&](const QStringList& folders) { forwardedFolders = folders; });
        QObject::connect(&folderSecondary, &QProcess::finished,
            &forwardingWait, &QEventLoop::quit);
        folderSecondary.setWorkingDirectory(launchDirectory.path());
        forwardedPaths.clear();
        forwardingTimeout.start(5000);
        folderSecondary.start(QCoreApplication::applicationFilePath(),
            {"--single-instance-folders", instanceName, "folder with spaces"});
        forwardingWait.exec();
        forwardingTimeout.stop();
        const bool folderCompleted = folderSecondary.state() == QProcess::NotRunning;
        if (!folderCompleted) {
            folderSecondary.kill();
            folderSecondary.waitForFinished(1000);
        }
        passed &= expect(folderCompleted && folderSecondary.exitStatus() == QProcess::NormalExit
                && folderSecondary.exitCode() == 0 && forwardedPaths.isEmpty()
                && forwardedFolders == QStringList{launchDirectory.filePath("folder with spaces")},
            "Strict folder forwarding must preserve its mode and the sender's directory without using ordinary file launch handling.");

        // Deliberately absent paths: binding the sender's working directory must
        // not depend on the requested item existing or canonicalizing its target.
        const QString relativeFolder = QStringLiteral("folder with spaces");
        const QString unicodeFolder = QString::fromUtf8("book-\xE6\x9C\xAC");
        const QString literalVariable = QStringLiteral("%FILE_MANAGER_UNEXPANDED%");
        const QString absolutePath = launchDirectory.filePath("absolute-folder");
        forwardedPaths.clear();
        QProcess relativeSecondary;
        relativeSecondary.setWorkingDirectory(launchDirectory.path());
        QObject::connect(&relativeSecondary, &QProcess::finished,
            &forwardingWait, &QEventLoop::quit);
        forwardingTimeout.start(5000);
        relativeSecondary.start(QCoreApplication::applicationFilePath(),
            {"--single-instance-forward", instanceName, relativeFolder,
                unicodeFolder, literalVariable, absolutePath});
        forwardingWait.exec();
        forwardingTimeout.stop();
        const bool completed = relativeSecondary.state() == QProcess::NotRunning;
        if (!completed) {
            relativeSecondary.kill();
            relativeSecondary.waitForFinished(1000);
        }
        passed &= expect(completed && relativeSecondary.exitStatus() == QProcess::NormalExit
                && relativeSecondary.exitCode() == 0,
            "A secondary launched from another working directory should forward successfully.");
        passed &= expect(forwardedPaths == QStringList{
                launchDirectory.filePath(relativeFolder), launchDirectory.filePath(unicodeFolder),
                launchDirectory.filePath(literalVariable), absolutePath},
            "Forwarded relative paths must retain the sender directory, spaces, Unicode and literal percent names, including missing targets.");
    }

#ifdef Q_OS_WIN
    {
        QProcess delayedReader;
        forwardedPaths.clear();
        QObject::connect(&primaryInstance,
            &filemanager::platform::SingleInstanceCoordinator::pathsReceived,
            &delayedReader, [&](const QStringList& paths) {
                if (paths == QStringList{QStringLiteral("C:/Delayed")}) {
                    delayedReader.write("R", 1);
                    delayedReader.closeWriteChannel();
                }
            });
        QObject::connect(&delayedReader, &QProcess::finished,
            &forwardingWait, &QEventLoop::quit);
        forwardingTimeout.start(5000);
        delayedReader.start(QCoreApplication::applicationFilePath(),
            {"--single-instance-delayed-ack", instanceName});
        forwardingWait.exec();
        forwardingTimeout.stop();
        const bool completed = delayedReader.state() == QProcess::NotRunning;
        if (!completed) {
            delayedReader.kill();
            delayedReader.waitForFinished(1000);
        }
        passed &= expect(completed && delayedReader.exitStatus() == QProcess::NormalExit
                && delayedReader.exitCode() == 0
                && forwardedPaths == QStringList{QStringLiteral("C:/Delayed")},
            "The primary must retain its acknowledgement until a client reads it after path delivery.");
    }
#endif

    {
        QTemporaryDir folders;
        passed &= expect(folders.isValid(), "Folder-launch fixtures must be available.");
        const QString good = folders.filePath("Book with spaces");
        passed &= expect(QDir().mkpath(good), "The requested folder fixture should be created.");
        QFile ordinaryFile(folders.filePath("not-a-folder.txt"));
        passed &= expect(ordinaryFile.open(QIODevice::WriteOnly), "The file fixture should open.");
        ordinaryFile.close();
        const auto valid = filemanager::platform::resolveFolderLaunch({good, good});
        passed &= expect(valid.error.isEmpty() && valid.folders == QStringList{good},
            "Strict folder launches preserve targets and deduplicate repeated folders.");
#ifdef Q_OS_WIN
        const QString nativeFolder = QDir::toNativeSeparators(good);
        wchar_t shortName[32768]{};
        const DWORD shortLength = GetShortPathNameW(reinterpret_cast<LPCWSTR>(nativeFolder.utf16()),
            shortName, static_cast<DWORD>(std::size(shortName)));
        if (shortLength > 0 && shortLength < std::size(shortName)) {
            const auto alias = filemanager::platform::resolveFolderLaunch({QString::fromWCharArray(shortName)});
            passed &= expect(alias.error.isEmpty() && alias.folders.size() == 1
                    && alias.folders.first().compare(good, Qt::CaseInsensitive) == 0,
                "A Shell short-name alias must open with the folder's long name.");
        }
#endif
        for (const QStringList& invalid : {QStringList{}, QStringList{QString{}},
                 QStringList{ordinaryFile.fileName()}, QStringList{good, folders.filePath("missing")}}) {
            const auto rejected = filemanager::platform::resolveFolderLaunch(invalid);
            passed &= expect(!rejected.error.isEmpty() && rejected.folders.isEmpty(),
                "Strict folder requests reject files and unavailable targets without partial startup fallback.");
        }
    }

    phase("workspace_begin");
    filemanager::workspace::WorkspaceView workspace(isolatedHome.path());
    passed &= expect(workspace.panels().size() == 1, "A workspace starts with one panel.");
    passed &= expect(workspace.currentPane() != nullptr, "The initial panel has an active tab.");
    passed &= expect(workspace.currentPane()->hasPathAutocomplete(),
        "The path editor exposes filesystem autocomplete.");
    FilePane* modePane = workspace.currentPane();
    modePane->setViewMode(FilePane::ViewMode::List);
    passed &= expect(qobject_cast<QListView*>(modePane->view()) != nullptr,
        "List mode uses the list presentation surface.");
    modePane->setViewMode(FilePane::ViewMode::LargeIcons);
    auto* iconView = qobject_cast<QListView*>(modePane->view());
    passed &= expect(iconView && iconView->viewMode() == QListView::IconMode
            && iconView->iconSize().width() == 64,
        "Large-icons mode uses a wrapping icon grid with large thumbnails.");
    modePane->setViewMode(FilePane::ViewMode::ExtraLargeIcons);
    iconView = qobject_cast<QListView*>(modePane->view());
    passed &= expect(iconView && iconView->viewMode() == QListView::IconMode
            && iconView->iconSize().width() == 96,
        "Extra-large mode uses the largest bounded thumbnail grid.");
    modePane->setViewMode(FilePane::ViewMode::MediumIcons);
    iconView = qobject_cast<QListView*>(modePane->view());
    passed &= expect(iconView && iconView->viewMode() == QListView::IconMode
            && iconView->iconSize().width() == 32,
        "Medium mode uses a compact thumbnail grid.");
    modePane->setViewMode(FilePane::ViewMode::SmallIcons);
    iconView = qobject_cast<QListView*>(modePane->view());
    passed &= expect(iconView && iconView->viewMode() == QListView::ListMode
            && iconView->iconSize().width() == 16,
        "Small-icons mode uses a dense list presentation.");
    modePane->setViewMode(FilePane::ViewMode::Details);
    passed &= expect(qobject_cast<QListView*>(modePane->view()) == nullptr,
        "Details mode restores the column presentation surface.");
    passed &= expect(modePane->view()->model()->columnCount() == 12
            && modePane->view()->model()->headerData(4, Qt::Horizontal).toString()
                == "Date created"
            && modePane->view()->model()->headerData(5, Qt::Horizontal).toString()
                == "Extension"
            && modePane->view()->model()->headerData(6, Qt::Horizontal).toString()
                == "Dimensions"
            && modePane->view()->model()->headerData(7, Qt::Horizontal).toString()
                == "Duration"
            && modePane->view()->model()->headerData(8, Qt::Horizontal).toString()
                == "Title"
            && modePane->view()->model()->headerData(9, Qt::Horizontal).toString()
                == "Artists"
            && modePane->view()->model()->headerData(10, Qt::Horizontal).toString()
                == "Pages"
            && modePane->view()->model()->headerData(11, Qt::Horizontal).toString()
                == "Availability"
            && !modePane->columnVisible(11),
        "Details mode exposes bounded media/document metadata and an opt-in availability column.");

    phase("live_directory_begin");
    QTemporaryDir liveDirectoryFixture;
    const QString liveFirstPath = liveDirectoryFixture.filePath("first.txt");
    const QString liveSecondPath = liveDirectoryFixture.filePath("second.txt");
    QFile liveFirst(liveFirstPath);
    passed &= expect(liveDirectoryFixture.isValid()
            && liveFirst.open(QIODevice::WriteOnly) && liveFirst.write("first") == 5,
        "The live-directory fixture should be created.");
    liveFirst.close();
    FilePane liveDirectoryPane(liveDirectoryFixture.path());
    auto* cloudStatusProvider =
        liveDirectoryPane.findChild<filemanager::preview::CloudStatusProvider*>();
    int cloudStatusInspections = 0;
    bool cloudStatusReady = false;
    if (cloudStatusProvider) {
        QObject::connect(cloudStatusProvider,
            &filemanager::preview::CloudStatusProvider::inspectionStarted,
            [&](const QString&) { ++cloudStatusInspections; });
        QObject::connect(cloudStatusProvider,
            &filemanager::preview::CloudStatusProvider::statusReady,
            [&](const QString& path) {
                if (QFileInfo(path).absoluteFilePath()
                        == QFileInfo(liveFirstPath).absoluteFilePath())
                    cloudStatusReady = true;
            });
    }
    liveDirectoryPane.resize(800, 600);
    liveDirectoryPane.show();
    const auto waitForLiveCount = [&liveDirectoryPane](int expected) {
        QElapsedTimer timer;
        timer.start();
        while (timer.elapsed() < 5000) {
            QApplication::processEvents(QEventLoop::AllEvents, 25);
            if (liveDirectoryPane.visibleItemCount() == expected)
                return true;
            QThread::msleep(5);
        }
        return false;
    };
    passed &= expect(waitForLiveCount(1),
        "The bounded directory model should publish its initial item.");
    passed &= expect(cloudStatusProvider && cloudStatusInspections == 0,
        "The hidden Availability column must not inspect provider metadata during folder load.");
    liveDirectoryPane.setColumnVisible(11, true);
    const QModelIndex availabilityIndex = liveDirectoryPane.view()->model()->index(
        0, 11, liveDirectoryPane.view()->rootIndex());
    availabilityIndex.data(Qt::DisplayRole);
    QElapsedTimer cloudStatusWait;
    cloudStatusWait.start();
    while (!cloudStatusReady && cloudStatusWait.elapsed() < 5000) {
        QApplication::processEvents(QEventLoop::AllEvents, 25);
        QThread::msleep(5);
    }
    passed &= expect(cloudStatusReady && cloudStatusInspections == 1
            && availabilityIndex.data(Qt::DisplayRole).toString().isEmpty(),
        "Enabling Availability should inspect visible metadata asynchronously and keep local rows quiet.");
    liveDirectoryPane.revealPath(liveFirstPath);
    QApplication::processEvents();
    QFile liveSecond(liveSecondPath);
    passed &= expect(liveSecond.open(QIODevice::WriteOnly) && liveSecond.write("second") == 6,
        "The watched-directory insertion fixture should be created.");
    liveSecond.close();
    passed &= expect(waitForLiveCount(2)
            && liveDirectoryPane.selectedPath() == liveFirstPath,
        "A watched insertion should appear without refresh and preserve the live selection.");
    const QModelIndex liveSelected = liveDirectoryPane.view()->currentIndex();
    std::unique_ptr<QMimeData> liveDragData(
        liveDirectoryPane.view()->model()->mimeData({liveSelected}));
    passed &= expect(liveDragData && liveDragData->hasUrls()
            && liveDragData->urls() == QList<QUrl>{QUrl::fromLocalFile(liveFirstPath)},
        "A local model drag should expose the exact selected path as a file URL.");
    passed &= expect(QFile::remove(liveSecondPath) && waitForLiveCount(1)
            && liveDirectoryPane.selectedPath() == liveFirstPath,
        "A watched removal should disappear without refresh and preserve unrelated selection.");

    phase("image_metadata_and_icons_begin");
    QTemporaryDir imageMetadataFixture;
    const QString metadataImagePath = imageMetadataFixture.filePath("dimensions.png");
    QImage metadataImage(37, 23, QImage::Format_ARGB32);
    metadataImage.fill(Qt::magenta);
    passed &= expect(metadataImage.save(metadataImagePath),
        "Image-metadata fixture should save.");
    const auto isolatedDimensions = filemanager::preview::runSandboxedPreviewHost(
        metadataImagePath, QStringLiteral("image-metadata"), 6000, 16 * 1024);
    const QJsonObject dimensionReply = QJsonDocument::fromJson(
        isolatedDimensions.standardOutput).object();
    passed &= expect(isolatedDimensions.success && isolatedDimensions.sandboxed
            && isolatedDimensions.jobRestricted
            && dimensionReply.value(QStringLiteral("width")).toInt() == 37
            && dimensionReply.value(QStringLiteral("height")).toInt() == 23,
        "Image dimensions should be parsed by the restricted handle-only helper.");
    filemanager::preview::ImageMetadataProvider imageMetadata;
    passed &= expect(!imageMetadata.requestDimensions(metadataImagePath).has_value(),
        "Image dimensions should not block the requesting thread.");
    QEventLoop metadataWait;
    QTimer metadataTimeout;
    metadataTimeout.setSingleShot(true);
    QObject::connect(&metadataTimeout, &QTimer::timeout, &metadataWait, &QEventLoop::quit);
    QObject::connect(&imageMetadata,
        &filemanager::preview::ImageMetadataProvider::metadataReady,
        &metadataWait, &QEventLoop::quit);
    metadataTimeout.start(10000);
    metadataWait.exec();
    passed &= expect(imageMetadata.requestDimensions(metadataImagePath) == QSize(37, 23),
        "Image dimensions should become available from the bounded async cache.");

    const QByteArray iconToken(32, 'a');
    QImage premultipliedPixels(256, 256, QImage::Format_ARGB32_Premultiplied);
    for (int alpha = 0; alpha < 256; ++alpha)
        for (int channel = 0; channel < 256; ++channel)
            premultipliedPixels.setPixel(channel, alpha,
                qRgba(channel % (alpha + 1), alpha / 2, alpha, alpha));
    // Captured Windows HICON pixels that the old ARGB32->RGBA8888->ARGB32
    // premultiplied conversion changed by one channel value.
    premultipliedPixels.setPixel(1, 1, 0xdf696564U);
    premultipliedPixels.setPixel(2, 1, 0xfef3b005U);
    const QByteArray premultipliedToken(32, 'p');
    const QByteArray premultipliedReply = filemanager::platform::encodeFileIconReply(
        premultipliedPixels, premultipliedToken);
    const QImage decodedPremultiplied = filemanager::platform::decodeFileIconReply(
        premultipliedReply, premultipliedToken);
    passed &= expect(premultipliedReply.startsWith("FMICON03")
            && decodedPremultiplied.format() == QImage::Format_ARGB32_Premultiplied
            && decodedPremultiplied == premultipliedPixels
            && decodedPremultiplied.convertToFormat(QImage::Format_RGBA8888)
                == premultipliedPixels.convertToFormat(QImage::Format_RGBA8888),
        "Native premultiplied channels must survive transport exactly at every alpha value.");
    QImage iconPixels(3, 2, QImage::Format_RGBA8888);
    iconPixels.fill(QColor(17, 99, 231, 113));
    iconPixels.setPixelColor(2, 1, QColor(201, 13, 57, 255));
    const QByteArray iconReply = filemanager::platform::encodeFileIconReply(iconPixels, iconToken);
    passed &= expect(filemanager::platform::decodeFileIconReply(iconReply, iconToken) == iconPixels
            && iconReply.size() == 48 + 3 * 2 * 4,
        "Icon transport should preserve nonsquare RGBA pixels and alpha with a fixed-size header.");
    QByteArray oversizedIconDimensions = iconReply;
    oversizedIconDimensions.replace(40, 4, QByteArray::fromHex("ffffffff"));
    passed &= expect(filemanager::platform::decodeFileIconReply(iconReply, QByteArray(32, 'b')).isNull()
            && filemanager::platform::decodeFileIconReply(iconReply.left(47), iconToken).isNull()
            && filemanager::platform::decodeFileIconReply(iconReply.chopped(1), iconToken).isNull()
            && filemanager::platform::decodeFileIconReply(iconReply + 'x', iconToken).isNull()
            && filemanager::platform::decodeFileIconReply(oversizedIconDimensions, iconToken).isNull(),
        "Icon transport must reject unrelated, truncated, trailing and oversized replies before allocation.");
    passed &= expect(!filemanager::platform::validFileIconRequest("relative.txt", 16)
            && !filemanager::platform::validFileIconRequest(metadataImagePath, 0)
            && !filemanager::platform::validFileIconRequest(metadataImagePath, 513)
            && !filemanager::platform::validFileIconRequest(metadataImagePath + QChar::Null, 16),
        "Icon requests must reject relative paths, embedded NULs and invalid dimensions.");
#ifdef Q_OS_WIN
    const QString iconHelperFixture = QDir(QCoreApplication::applicationDirPath())
        .filePath(QStringLiteral("FileManagerPreviewHelperFixture.exe"));
    using IconStatus = filemanager::platform::FileIconStatus;
    const QList<std::pair<QString, IconStatus>> iconFailureCases{
        {QStringLiteral("icon-malformed.fixture"), IconStatus::InvalidReply},
        {QStringLiteral("icon-wrong-token.fixture"), IconStatus::InvalidReply},
        {QStringLiteral("icon-oversize.fixture"), IconStatus::OutputLimit},
        {QStringLiteral("icon-stderr.fixture"), IconStatus::StandardError},
        {QStringLiteral("icon-exit.fixture"), IconStatus::HelperFailed}};
    for (const auto& [mode, expectedStatus] : iconFailureCases) {
        const auto rejected = filemanager::platform::readFileIconIsolated(
            imageMetadataFixture.filePath(mode), 16, 3000, {}, iconHelperFixture);
        const bool correctFailure = rejected.status == expectedStatus && rejected.image.isNull()
            && rejected.processStarted && rejected.processStopped
            && (mode != QStringLiteral("icon-exit.fixture") || rejected.helperExitCode == 17);
        if (!correctFailure)
            std::fprintf(stderr, "Icon rejection: fixture=%s expected=%s actual=%s "
                "elapsed_ms=%lld started=%d stopped=%d exit=%d\n", qPrintable(mode),
                filemanager::platform::fileIconStatusName(expectedStatus),
                filemanager::platform::fileIconStatusName(rejected.status),
                static_cast<long long>(rejected.elapsedMilliseconds), rejected.processStarted,
                rejected.processStopped, rejected.helperExitCode);
        passed &= expect(correctFailure,
            "The icon parent must classify the exercised failure and stop its helper.");
    }
    const auto missingIconHelper = filemanager::platform::readFileIconIsolated(metadataImagePath,
        16, 3000, {}, iconHelperFixture + QStringLiteral(".missing"));
    const auto invalidIconRequest = filemanager::platform::readFileIconIsolated("relative.txt", 16);
    passed &= expect(missingIconHelper.status == IconStatus::StartFailed
            && !missingIconHelper.processStarted && missingIconHelper.processStopped
            && invalidIconRequest.status == IconStatus::InvalidRequest
            && !invalidIconRequest.processStarted && invalidIconRequest.processStopped,
        "Icon startup failure and invalid input must remain distinct and leave no child process.");
    QElapsedTimer iconDeadline;
    iconDeadline.start();
    const auto timedOutIcon = filemanager::platform::readFileIconIsolated(
        imageMetadataFixture.filePath("icon-hang.fixture"), 16, 300, {}, iconHelperFixture);
    passed &= expect(timedOutIcon.status == IconStatus::TimedOut && timedOutIcon.image.isNull()
            && timedOutIcon.processStarted && timedOutIcon.processStopped && iconDeadline.elapsed() < 1800,
        "A hung icon helper must be terminated within the request deadline and cleanup allowance.");
    const auto cancelledIcon = std::make_shared<std::atomic_bool>(true);
    const auto preCancelledIcon = filemanager::platform::readFileIconIsolated(metadataImagePath,
        16, 5000, cancelledIcon);
    passed &= expect(preCancelledIcon.status == IconStatus::Cancelled
            && !preCancelledIcon.processStarted && preCancelledIcon.processStopped
            && preCancelledIcon.image.isNull(), "A pre-cancelled icon request must not start a helper.");
    const auto runningIconCancellation = std::make_shared<std::atomic_bool>(false);
    QThread* iconRevoker = QThread::create([runningIconCancellation] {
        QThread::msleep(150);
        runningIconCancellation->store(true);
    });
    iconRevoker->start();
    iconDeadline.restart();
    const auto cancelledRunningIcon = filemanager::platform::readFileIconIsolated(
        imageMetadataFixture.filePath("icon-hang.fixture"), 16, 5000,
        runningIconCancellation, iconHelperFixture);
    const qint64 cancelledIconElapsed = iconDeadline.elapsed();
    iconRevoker->wait();
    delete iconRevoker;
    passed &= expect(cancelledRunningIcon.status == IconStatus::Cancelled
            && cancelledRunningIcon.processStarted && cancelledRunningIcon.processStopped
            && cancelledRunningIcon.image.isNull() && cancelledIconElapsed < 1800,
        "Cancellation during icon retrieval must terminate the helper without waiting for its deadline.");
    QFileIconProvider directIconProvider;
    // Windows can rasterize the same Shell icon in different processes with
    // one- or two-level RGB rounding differences. Preserve an exact alpha
    // mask and dimensions, while allowing only that observed channel noise.
    const auto equivalentShellRaster = [](const QImage& first, const QImage& second) {
        if (first.isNull() || second.isNull() || first.size() != second.size())
            return false;
        for (int y = 0; y < first.height(); ++y) {
            for (int x = 0; x < first.width(); ++x) {
                const QRgb a = first.pixel(x, y);
                const QRgb b = second.pixel(x, y);
                if (qAlpha(a) != qAlpha(b) || std::abs(qRed(a) - qRed(b)) > 2
                        || std::abs(qGreen(a) - qGreen(b)) > 2
                        || std::abs(qBlue(a) - qBlue(b)) > 2)
                    return false;
            }
        }
        return true;
    };
    for (const QString& path : {metadataImagePath, imageMetadataFixture.path()}) {
        const QIcon directIcon = directIconProvider.icon(QFileInfo(path));
        for (int extent : {16, 32, 48, 256}) {
            const unsigned beforeIsolatedControl = iconFloatingControl();
            QElapsedTimer iconReadTime;
            iconReadTime.start();
            const auto isolatedIcon = filemanager::platform::readFileIconIsolated(path, extent);
            const qint64 isolatedMilliseconds = iconReadTime.elapsed();
            const unsigned afterIsolatedControl = iconFloatingControl();
            const QImage directImage = directIcon.pixmap(QSize(extent, extent), 1.0).toImage();
            const QImage directPixels = directImage.convertToFormat(QImage::Format_RGBA8888);
            const QImage isolatedPixels = isolatedIcon.image.convertToFormat(QImage::Format_RGBA8888);
            // Compare transport before GUI conversion, and artwork after the same
            // QPixmap conversion used by the application. Native HICON images are
            // premultiplied; a raw image and a GUI pixmap need not have identical
            // straight-alpha channel values after their different conversions.
            const unsigned beforeNativeControl = iconFloatingControl();
            const QImage nativeImage = filemanager::platform::readFileIconInProcess(path, extent);
            const unsigned afterNativeControl = iconFloatingControl();
            const QImage nativePixels = nativeImage.convertToFormat(QImage::Format_RGBA8888);
            const QImage displayedPixels = filemanager::ui::fileIconFromImage(
                isolatedIcon.image, directIcon.availableSizes()).pixmap(QSize(extent, extent), 1.0)
                .toImage().convertToFormat(QImage::Format_RGBA8888);
            const bool sameTransport = equivalentShellRaster(isolatedPixels, nativePixels);
            const bool sameArtwork = isolatedIcon.status == IconStatus::Ready
                && isolatedIcon.processStarted && isolatedIcon.processStopped
                && equivalentShellRaster(displayedPixels, directPixels);
            const QImage canonicalIsolated = isolatedIcon.image.convertToFormat(QImage::Format_ARGB32_Premultiplied);
            const QImage canonicalNative = nativeImage.convertToFormat(QImage::Format_ARGB32_Premultiplied);
            std::fprintf(stdout, "Icon extraction state: kind=%s requested=%d initial_fp=%08x "
                "before_isolated_fp=%08x after_isolated_fp=%08x before_native_fp=%08x after_native_fp=%08x "
                "isolated_sha256=%s native_sha256=%s provider_sha256=%s\n",
                path == metadataImagePath ? "file" : "folder", extent, initialIconFloatingControl,
                beforeIsolatedControl, afterIsolatedControl, beforeNativeControl, afterNativeControl,
                iconRasterHash(canonicalIsolated).constData(), iconRasterHash(canonicalNative).constData(),
                iconRasterHash(directImage.convertToFormat(QImage::Format_ARGB32_Premultiplied)).constData());
            if (canonicalIsolated.size() == canonicalNative.size() && canonicalIsolated != canonicalNative) {
                int differences = 0;
                for (int y = 0; y < canonicalNative.height(); ++y) {
                    for (int x = 0; x < canonicalNative.width(); ++x) {
                        const QRgb actual = canonicalIsolated.pixel(x, y);
                        const QRgb expected = canonicalNative.pixel(x, y);
                        if (actual == expected)
                            continue;
                        if (differences++ < 16)
                            std::fprintf(stdout, "Icon premultiplied difference: kind=%s requested=%d x=%d y=%d "
                                "isolated_argb=%08x native_argb=%08x\n",
                                path == metadataImagePath ? "file" : "folder", extent, x, y,
                                static_cast<unsigned>(actual), static_cast<unsigned>(expected));
                    }
                }
                std::fprintf(stdout, "Icon premultiplied difference count: kind=%s requested=%d pixels=%d\n",
                    path == metadataImagePath ? "file" : "folder", extent, differences);
            }
            const QImage localRoundTrip = filemanager::platform::decodeFileIconReply(
                filemanager::platform::encodeFileIconReply(nativeImage, iconToken), iconToken);
            const QImage canonicalDisplay = filemanager::ui::fileIconFromImage(canonicalIsolated,
                directIcon.availableSizes()).pixmap(QSize(extent, extent), 1.0).toImage()
                .convertToFormat(QImage::Format_RGBA8888);
            std::fprintf(stdout, "Icon conversion boundaries: kind=%s requested=%d premultiplied_equal=%d "
                "local_roundtrip_straight_equal=%d canonical_straight_equal=%d canonical_display_equal=%d\n",
                path == metadataImagePath ? "file" : "folder", extent, canonicalIsolated == canonicalNative,
                localRoundTrip.convertToFormat(QImage::Format_RGBA8888) == nativePixels,
                canonicalIsolated.convertToFormat(QImage::Format_RGBA8888) == nativePixels,
                canonicalDisplay == directPixels);
            std::fprintf(stdout, "Icon contracts: kind=%s requested=%d native_format=%d "
                "isolated_format=%d raw_shell_equivalent=%d display_shell_equivalent=%d "
                "raw_matches_display=%d\n",
                path == metadataImagePath ? "file" : "folder", extent,
                static_cast<int>(nativeImage.format()), static_cast<int>(isolatedIcon.image.format()),
                sameTransport, sameArtwork, isolatedPixels == directPixels);
            if (!sameArtwork || !sameTransport
                    || !equivalentShellRaster(isolatedPixels, directPixels)) {
                // Keep the actual rasters from this comparison, without another Shell
                // request that could change the evidence. These paths contain only
                // this test's synthetic file/folder icons.
                const QString evidence = QDir(QCoreApplication::applicationDirPath())
                    .filePath(QStringLiteral("out/icon-fidelity"));
                QDir().mkpath(evidence);
                const QString prefix = QDir(evidence).filePath(QStringLiteral("%1-%2")
                    .arg(path == metadataImagePath ? QStringLiteral("file") : QStringLiteral("folder"))
                    .arg(extent));
                const bool rastersSaved = canonicalIsolated.save(prefix + "-isolated.png")
                    && canonicalNative.save(prefix + "-native.png")
                    && directPixels.save(prefix + "-provider.png")
                    && displayedPixels.save(prefix + "-displayed.png");
                std::fprintf(stderr, "Icon fidelity artifacts: prefix=%s saved=%d\n",
                    qPrintable(prefix), rastersSaved);
                int differentPixels = 0;
                int firstX = -1;
                int firstY = -1;
                QRgb firstActual = 0;
                QRgb firstExpected = 0;
                for (int y = 0; y < qMin(isolatedPixels.height(), directPixels.height()); ++y) {
                    for (int x = 0; x < qMin(isolatedPixels.width(), directPixels.width()); ++x) {
                        if (isolatedPixels.pixel(x, y) != directPixels.pixel(x, y)) {
                            ++differentPixels;
                            if (firstX < 0) {
                                firstX = x;
                                firstY = y;
                                firstActual = isolatedPixels.pixel(x, y);
                                firstExpected = directPixels.pixel(x, y);
                            }
                        }
                    }
                }
                std::fprintf(stderr, "Icon fidelity: kind=%s requested=%d status=%s exit=%d "
                    "started=%d stopped=%d elapsed_ms=%lld "
                    "actual=%dx%d expected=%dx%d differing_overlap_pixels=%d "
                    "first_diff=%d,%d actual_argb=%08x expected_argb=%08x\n",
                    path == metadataImagePath ? "file" : "folder", extent,
                    filemanager::platform::fileIconStatusName(isolatedIcon.status),
                    isolatedIcon.helperExitCode, isolatedIcon.processStarted, isolatedIcon.processStopped,
                    static_cast<long long>(isolatedMilliseconds), isolatedPixels.width(),
                    isolatedPixels.height(), directPixels.width(), directPixels.height(),
                    differentPixels, firstX, firstY, static_cast<unsigned>(firstActual),
                    static_cast<unsigned>(firstExpected));
            }
            passed &= expect(sameTransport,
                "Isolated icon transport must preserve native artwork within Windows Shell raster rounding.");
            passed &= expect(sameArtwork,
                "Displayed isolated file/folder icons must preserve native artwork at all four source sizes.");
        }
    }
#endif

    // More requests than the provider's worker limit must all complete, including
    // rejections. Repaints must neither duplicate jobs nor repeatedly retry failures.
    phase("queued_image_metadata_begin");
    filemanager::preview::ImageMetadataProvider queuedImageMetadata;
    QHash<QString, int> imageCompletions;
    QObject::connect(&queuedImageMetadata,
        &filemanager::preview::ImageMetadataProvider::metadataReady,
        &queuedImageMetadata, [&imageCompletions](const QString& path) {
            ++imageCompletions[path];
        });
    QStringList rejectedImagePaths;
    for (int i = 0; i < 40; ++i) {
        const QString path = imageMetadataFixture.filePath(
            QStringLiteral("rejected-%1.%2").arg(i).arg(i % 2 ? "png" : "txt"));
        QFile rejected(path);
        passed &= expect(rejected.open(QIODevice::WriteOnly),
            "Rejected-image fixture should open.");
        rejected.write("not an image");
        rejected.close();
        rejectedImagePaths.append(path);
        passed &= expect(!queuedImageMetadata.requestDimensions(path)
                && !queuedImageMetadata.requestDimensions(path),
            "Unsupported/corrupt image requests should return asynchronously without dimensions.");
    }
    queuedImageMetadata.requestDimensions(metadataImagePath);
    const auto waitForImageCompletions = [&](int expectedCount) {
        QElapsedTimer deadline;
        deadline.start();
        while (imageCompletions.size() < expectedCount && deadline.elapsed() < 20000) {
            QApplication::processEvents(QEventLoop::AllEvents, 25);
            QThread::msleep(1);
        }
        return imageCompletions.size() == expectedCount;
    };
    passed &= expect(waitForImageCompletions(41)
            && queuedImageMetadata.requestDimensions(metadataImagePath) == QSize(37, 23),
        "Queued image requests should drain without dropping the valid image behind rejections.");
    for (const QString& path : rejectedImagePaths) {
        passed &= expect(imageCompletions.value(path) == 1
                && !queuedImageMetadata.requestDimensions(path),
            "Duplicate requests should complete once and negative cache entries must stay empty.");
    }
    QEventLoop rejectionCacheWait;
    QTimer::singleShot(50, &rejectionCacheWait, &QEventLoop::quit);
    rejectionCacheWait.exec();
    bool noRejectionRetries = true;
    for (const QString& path : rejectedImagePaths)
        noRejectionRetries &= imageCompletions.value(path) == 1;
    passed &= expect(noRejectionRetries,
        "Reading cached unsupported/corrupt dimensions should not enqueue another job.");

    const QString repairedImagePath = rejectedImagePaths.at(1);
    imageCompletions.remove(repairedImagePath);
    passed &= expect(metadataImage.save(repairedImagePath)
            && !queuedImageMetadata.requestDimensions(repairedImagePath)
            && waitForImageCompletions(41)
            && queuedImageMetadata.requestDimensions(repairedImagePath) == QSize(37, 23),
        "Replacing a corrupt image with a different-sized valid file should invalidate rejection.");

    const QString changedDimensionsPath = imageMetadataFixture.filePath("same-size-dimensions.bmp");
    qint64 dimensionFileBytes = -1;
    int dimensionVersion = 0;
    for (const QSize dimensions : {QSize(12, 8), QSize(8, 12)}) {
        QImage changedImage(dimensions, QImage::Format_RGB32);
        changedImage.fill(Qt::blue);
        const bool saved = changedImage.save(changedDimensionsPath);
        QFile changedFile(changedDimensionsPath);
        const QDateTime modified(QDate(2024, 11, 3), QTime(5 + dimensionVersion, 30), QTimeZone::UTC);
        const bool timestampSet = changedFile.open(QIODevice::ReadWrite)
            && changedFile.setFileTime(modified, QFileDevice::FileModificationTime);
        changedFile.close();
        const QFileInfo changedInfo(changedDimensionsPath);
        if (dimensionFileBytes < 0)
            dimensionFileBytes = changedInfo.size();
        passed &= expect(saved && timestampSet && dimensionFileBytes > 0
                && changedInfo.size() == dimensionFileBytes
                && changedInfo.lastModified(QTimeZone::UTC) == modified,
            "Dimension replacement fixture keeps byte size while changing its UTC modification time.");
        imageCompletions.remove(changedDimensionsPath);
        passed &= expect(!queuedImageMetadata.requestDimensions(changedDimensionsPath)
                && waitForImageCompletions(42)
                && queuedImageMetadata.requestDimensions(changedDimensionsPath) == dimensions,
            "Fresh dimension requests invalidate a same-size cached image when its modification time changes.");
        ++dimensionVersion;
    }

    const QString metadataPdfPath = imageMetadataFixture.filePath("document.pdf");
    QFile metadataPdf(metadataPdfPath);
    passed &= expect(metadataPdf.open(QIODevice::WriteOnly),
        "PDF-metadata fixture should open.");
    metadataPdf.write("%PDF-1.7\n1 0 obj<</Type /Page>>endobj\n"
                      "2 0 obj<</Type /Page>>endobj\n"
                      "3 0 obj<</Title (Roadmap) /Author (Example Author)>>endobj\n%%EOF");
    metadataPdf.close();
    const auto isolatedDocument = filemanager::preview::runSandboxedPreviewHost(
        metadataPdfPath, QStringLiteral("document-metadata"), 6000, 128 * 1024);
    const QJsonObject documentReply = QJsonDocument::fromJson(
        isolatedDocument.standardOutput).object();
    passed &= expect(isolatedDocument.success && isolatedDocument.sandboxed
            && isolatedDocument.jobRestricted
            && documentReply.value(QStringLiteral("pageCount")).toInt() == 2
            && documentReply.value(QStringLiteral("title")).toString() == QStringLiteral("Roadmap"),
        "Document fields should be parsed by the restricted handle-only helper.");
    filemanager::preview::DocumentMetadataProvider documentMetadata;
    passed &= expect(!documentMetadata.request(metadataPdfPath).has_value(),
        "Document metadata should not block the requesting thread.");
    QEventLoop documentMetadataWait;
    QTimer documentMetadataTimeout;
    documentMetadataTimeout.setSingleShot(true);
    QObject::connect(&documentMetadataTimeout, &QTimer::timeout,
        &documentMetadataWait, &QEventLoop::quit);
    QObject::connect(&documentMetadata,
        &filemanager::preview::DocumentMetadataProvider::metadataReady,
        &documentMetadataWait, &QEventLoop::quit);
    documentMetadataTimeout.start(10000);
    documentMetadataWait.exec();
    const auto pdfMetadata = documentMetadata.request(metadataPdfPath);
    passed &= expect(pdfMetadata && pdfMetadata->pageCount == 2
            && pdfMetadata->title == "Roadmap" && pdfMetadata->authors == "Example Author",
        "The bounded PDF provider should return pages, title and author metadata.");

    const QString metadataDocxPath = imageMetadataFixture.filePath("document.docx");
    passed &= expect(writeStoredZip(metadataDocxPath, {
            {"[Content_Types].xml", "<Types/>"},
            {"docProps/core.xml",
                "<cp:coreProperties xmlns:cp='urn:cp' xmlns:dc='urn:dc'>"
                "<dc:title>Verified plan</dc:title><dc:creator>Example Author</dc:creator>"
                "</cp:coreProperties>"},
            {"docProps/app.xml", "<Properties><Pages>7</Pages></Properties>"}}),
        "Office Open XML metadata fixture should save as a valid stored ZIP.");
    filemanager::preview::DocumentMetadataProvider officeMetadata;
    passed &= expect(!officeMetadata.request(metadataDocxPath).has_value(),
        "Office metadata should not block the requesting thread.");
    QEventLoop officeMetadataWait;
    QTimer officeMetadataTimeout;
    officeMetadataTimeout.setSingleShot(true);
    QObject::connect(&officeMetadataTimeout, &QTimer::timeout,
        &officeMetadataWait, &QEventLoop::quit);
    QObject::connect(&officeMetadata,
        &filemanager::preview::DocumentMetadataProvider::metadataReady,
        &officeMetadataWait, &QEventLoop::quit);
    officeMetadataTimeout.start(10000);
    officeMetadataWait.exec();
    const auto docxMetadata = officeMetadata.request(metadataDocxPath);
    passed &= expect(docxMetadata && docxMetadata->pageCount == 7
            && docxMetadata->title == "Verified plan" && docxMetadata->authors == "Example Author",
        "The bounded Office provider should return pages, title and author metadata.");
    passed &= expect(filemanager::preview::DocumentMetadataProvider::MaximumArchiveEntries == 256
            && filemanager::preview::DocumentMetadataProvider::MaximumMetadataTotalBytes
                == 512 * 1024
            && filemanager::preview::DocumentMetadataProvider::MaximumExpansionRatio == 64,
        "Office metadata extraction should retain explicit archive bomb limits.");
    QList<QPair<QByteArray, QByteArray>> oversizedArchiveEntries;
    oversizedArchiveEntries.emplaceBack(QByteArray("docProps/core.xml"),
        QByteArray("<coreProperties><title>Must not load</title></coreProperties>"));
    for (int entry = 0;
         entry < filemanager::preview::DocumentMetadataProvider::MaximumArchiveEntries;
         ++entry) {
        oversizedArchiveEntries.emplaceBack(
            QByteArray("padding/") + QByteArray::number(entry), QByteArray());
    }
    const QString oversizedDocxPath = imageMetadataFixture.filePath("too-many-entries.docx");
    passed &= expect(writeStoredZip(oversizedDocxPath, oversizedArchiveEntries),
        "Oversized Office archive fixture should save.");
    filemanager::preview::DocumentMetadataProvider guardedOfficeMetadata;
    guardedOfficeMetadata.request(oversizedDocxPath);
    QEventLoop guardedMetadataWait;
    QTimer guardedMetadataTimeout;
    guardedMetadataTimeout.setSingleShot(true);
    QObject::connect(&guardedMetadataTimeout, &QTimer::timeout,
        &guardedMetadataWait, &QEventLoop::quit);
    QObject::connect(&guardedOfficeMetadata,
        &filemanager::preview::DocumentMetadataProvider::metadataReady,
        &guardedMetadataWait, &QEventLoop::quit);
    guardedMetadataTimeout.start(10000);
    guardedMetadataWait.exec();
    const auto rejectedMetadata = guardedOfficeMetadata.request(oversizedDocxPath);
    passed &= expect(rejectedMetadata && rejectedMetadata->title.isEmpty()
            && rejectedMetadata->authors.isEmpty() && rejectedMetadata->pageCount == 0,
        "Office archives exceeding the entry ceiling should be rejected before XML parsing.");

    const auto defaultMetadataPermissions = filemanager::settings::loadMetadataPermissions();
    passed &= expect(!defaultMetadataPermissions.allowWindowsShellHandlers,
        "Windows Shell metadata handlers should be denied by default.");
    filemanager::settings::saveMetadataPermissions({true});
    passed &= expect(filemanager::settings::loadMetadataPermissions().allowWindowsShellHandlers,
        "The explicit Windows Shell metadata permission should persist.");
    filemanager::settings::saveMetadataPermissions({false});
    passed &= expect(!filemanager::settings::loadMetadataPermissions().allowWindowsShellHandlers,
        "Revoking Windows Shell metadata permission should persist.");

    filemanager::preview::MediaMetadata formattedMedia{3723000};
    passed &= expect(formattedMedia.durationText() == "1:02:03",
        "Long media durations should use hours, minutes and seconds.");
    formattedMedia.durationMs = 65000;
    passed &= expect(formattedMedia.durationText() == "1:05",
        "Short media durations should use minutes and seconds.");

    QTemporaryDir mediaMetadataFixture;
    const QString wavePath = mediaMetadataFixture.filePath("tone.wav");
    QFile waveFile(wavePath);
    passed &= expect(waveFile.open(QIODevice::WriteOnly), "WAV metadata fixture should open.");
    QDataStream wave(&waveFile);
    wave.setByteOrder(QDataStream::LittleEndian);
    constexpr quint32 sampleRate = 8000;
    constexpr quint16 channels = 1;
    constexpr quint16 bitsPerSample = 16;
    constexpr quint32 sampleCount = 800;
    constexpr quint32 waveDataBytes = sampleCount * channels * bitsPerSample / 8;
    wave.writeRawData("RIFF", 4);
    wave << quint32(36 + waveDataBytes);
    wave.writeRawData("WAVEfmt ", 8);
    wave << quint32(16) << quint16(1) << channels << sampleRate
         << quint32(sampleRate * channels * bitsPerSample / 8)
         << quint16(channels * bitsPerSample / 8) << bitsPerSample;
    wave.writeRawData("data", 4);
    wave << waveDataBytes;
    for (quint32 sample = 0; sample < sampleCount; ++sample)
        wave << qint16(0);
    waveFile.close();
    filemanager::preview::MediaMetadataProvider mediaMetadata;
    passed &= expect(!mediaMetadata.request(wavePath).has_value(),
        "Built-in media metadata should load asynchronously.");
    QEventLoop mediaWait;
    QTimer mediaTimeout;
    mediaTimeout.setSingleShot(true);
    QObject::connect(&mediaTimeout, &QTimer::timeout, &mediaWait, &QEventLoop::quit);
    QObject::connect(&mediaMetadata, &filemanager::preview::MediaMetadataProvider::metadataReady,
        &mediaWait, &QEventLoop::quit);
    mediaTimeout.start(6000);
    mediaWait.exec();
    const auto waveMetadata = mediaMetadata.request(wavePath);
    passed &= expect(waveMetadata && waveMetadata->durationMs >= 90
            && waveMetadata->durationMs <= 110,
        "The built-in provider should extract bounded WAV duration metadata.");

#ifdef Q_OS_WIN
    {
        using filemanager::ui::columnProfileContainsPath;
        const QString documents = "C:/Users/Example/Documents";
        passed &= expect(columnProfileContainsPath(documents, documents)
                && columnProfileContainsPath(documents, "c:/users/example/documents/Series")
                && columnProfileContainsPath(documents, "C:\\Users\\Example\\Documents\\Series"),
            "Column profiles recognize their root and children across Windows case and separator forms.");
        passed &= expect(!columnProfileContainsPath(documents, "D:/Users/Example/Documents/Series")
                && !columnProfileContainsPath(documents, "//server/share/Documents/Series")
                && !columnProfileContainsPath(documents, "C:/Users/Example/Documents-Archive")
                && !columnProfileContainsPath(documents, "C:/Users/Example/Documents/../Pictures")
                && !columnProfileContainsPath({}, documents),
            "Column profiles reject different drives, UNC roots, prefix siblings and parent escapes.");
        passed &= expect(columnProfileContainsPath("//server/share/Documents", "//server/share/Documents/Series")
                && !columnProfileContainsPath("//server/share/Documents", "//other/share/Documents/Series")
                && !columnProfileContainsPath("//server/share/Documents", "//server/other/Documents/Series"),
            "Column profile containment distinguishes UNC servers and shares without accessing them.");
    }
#endif
    modePane->navigate(QDir::tempPath());
    using namespace filemanager::ui::diagnostics;
    static int columnProfileLookups = 0;
    static int columnProfileSaves = 0;
    columnProfileLookups = 0;
    columnProfileSaves = 0;
    const auto previousColumnObserver = presentationObserver;
    presentationObserver = [](PresentationStage stage, int, int, qint64) {
        if (stage == PresentationStage::ColumnProfileLookup)
            ++columnProfileLookups;
        if (stage == PresentationStage::PaneColumnsSave)
            ++columnProfileSaves;
    };
    auto* details = qobject_cast<QTreeView*>(modePane->view());
    details->header()->resizeSection(0, 333);
    details->header()->moveSection(details->header()->visualIndex(2), 4);
    modePane->setSort(1, Qt::DescendingOrder);
    modePane->setColumnVisible(6, false);
    modePane->setColumnVisible(0, false);
    QEventLoop columnPersistenceDelay;
    QTimer::singleShot(350, &columnPersistenceDelay, &QEventLoop::quit);
    columnPersistenceDelay.exec();
    presentationObserver = previousColumnObserver;
    passed &= expect(columnProfileSaves > 0 && columnProfileLookups == 0,
        "Column edits and timer saves reuse the displayed profile without known-folder lookups.");
    {
        const QSettings persistedColumnSettings;
        const QByteArray state = persistedColumnSettings.value("view/columnProfiles/general").toByteArray();
        const int version = persistedColumnSettings.value("view/columnProfileVersions/general").toInt();
        // Test the stored preferences through a fresh Qt header, independently
        // of FilePane's live profile cache and transient serialized header fields.
        QStandardItemModel storedColumns(0, details->model()->columnCount());
        QTreeView storedView;
        storedView.setModel(&storedColumns);
        auto* storedHeader = storedView.header();
        const bool restored = !state.isEmpty() && storedHeader->restoreState(state);
        passed &= expect(restored && version == 2
                && storedHeader->sectionSize(0) == 333
                && !storedHeader->isSectionHidden(0) && storedHeader->isSectionHidden(6)
                && storedHeader->visualIndex(2) == 4
                && storedHeader->sortIndicatorSection() == 1
                && storedHeader->sortIndicatorOrder() == Qt::DescendingOrder,
            "Stored version-2 column preferences restore width, visibility, order and sorting in a fresh header.");
        std::printf("column_profile_storage bytes=%lld version=%d restored=%d name_width=%d "
            "name_hidden=%d dimensions_hidden=%d type_position=%d sort_column=%d descending=%d\n",
            static_cast<long long>(state.size()), version, restored, storedHeader->sectionSize(0),
            storedHeader->isSectionHidden(0), storedHeader->isSectionHidden(6), storedHeader->visualIndex(2),
            storedHeader->sortIndicatorSection(), storedHeader->sortIndicatorOrder() == Qt::DescendingOrder);
    }
    std::printf("column_profile_refresh saves=%d folder_lookups=%d\n",
        columnProfileSaves, columnProfileLookups);
    FilePane persistedColumns(isolatedHome.path());
    persistedColumns.navigate(QDir::tempPath());
    auto* persistedDetails = qobject_cast<QTreeView*>(persistedColumns.view());
    passed &= expect(persistedDetails && persistedDetails->header()->sectionSize(0) == 333
            && persistedDetails->header()->visualIndex(2) == 4
            && persistedColumns.columnVisible(0) && !persistedColumns.columnVisible(6)
            && persistedColumns.sortColumn() == 1
            && persistedColumns.sortOrder() == Qt::DescendingOrder,
        "General-folder column visibility, widths and sort state persist across independent tabs.");
    persistedColumns.setColumnVisible(6, true);
    persistedColumns.setSort(0, Qt::AscendingOrder);

    filemanager::ui::saveAppearanceSettings({12, "Spacious", false});
    const auto savedAppearance = filemanager::ui::loadAppearanceSettings();
    passed &= expect(savedAppearance.fontPointSize == 12
            && savedAppearance.density == "Spacious" && !savedAppearance.animations,
        "Appearance preferences round-trip font, density and animation state.");

    TabbedPanel lifecyclePanel(isolatedHome.path());
    FilePane* lifecycleSource = lifecyclePanel.currentPane();
    const QString historyRoot = lifecycleSource->currentPath();
    lifecycleSource->navigate(QDir::tempPath());
    const auto normalizedPath = [](const QString& path) {
        return QDir::cleanPath(QDir::fromNativeSeparators(path)).toCaseFolded();
    };
    QStringList normalizedHistory;
    for (const QString& path : lifecycleSource->navigationHistory())
        normalizedHistory.append(normalizedPath(path));
    passed &= expect(normalizedHistory.contains(normalizedPath(historyRoot))
            && normalizedHistory.last() == normalizedPath(QDir::tempPath()),
        "Each tab retains its own navigation history for back/forward menus.");
    lifecycleSource->setViewMode(FilePane::ViewMode::LargeIcons);
    lifecycleSource->setShowHidden(true);
    lifecycleSource->setFolderPlacement(FilePane::FolderPlacement::Mixed);
    lifecycleSource->setShowFileExtensions(false);
    lifecycleSource->setGroupBy(FilePane::GroupBy::Type);
    FilePane* duplicatedTab = lifecyclePanel.duplicateCurrentTab();
    passed &= expect(duplicatedTab && lifecyclePanel.count() == 2
            && duplicatedTab->viewMode() == FilePane::ViewMode::LargeIcons
            && duplicatedTab->showHidden()
            && duplicatedTab->folderPlacement() == FilePane::FolderPlacement::Mixed
            && !duplicatedTab->showFileExtensions()
            && duplicatedTab->groupBy() == FilePane::GroupBy::Type,
        "Duplicating a tab preserves its path and view state.");
    const QString duplicatedPath = duplicatedTab->currentPath();
    lifecyclePanel.closeCurrentTab();
    passed &= expect(lifecyclePanel.count() == 1 && lifecyclePanel.hasClosedTabs(),
        "Closing a tab records it in the bounded panel history.");
    FilePane* reopenedTab = lifecyclePanel.reopenLastClosedTab();
    passed &= expect(reopenedTab && reopenedTab->currentPath() == duplicatedPath
            && reopenedTab->viewMode() == FilePane::ViewMode::LargeIcons
            && reopenedTab->showHidden()
            && reopenedTab->folderPlacement() == FilePane::FolderPlacement::Mixed
            && !reopenedTab->showFileExtensions()
            && reopenedTab->groupBy() == FilePane::GroupBy::Type
            && !lifecyclePanel.hasClosedTabs(),
        "Reopening a closed tab restores its path and view state.");

    TabbedPanel keyboardReorderPanel(isolatedHome.path());
    keyboardReorderPanel.addTab(QDir::tempPath());
    FilePane* reorderedPane = keyboardReorderPanel.addTab(QCoreApplication::applicationDirPath());
    const QStringList pathsBeforeReorder = keyboardReorderPanel.paths();
    passed &= expect(keyboardReorderPanel.moveCurrentTab(-1)
            && keyboardReorderPanel.currentPane() == reorderedPane
            && keyboardReorderPanel.paths().at(1) == pathsBeforeReorder.at(2),
        "The keyboard tab-reorder operation should move the current tab left and preserve focus.");
    passed &= expect(keyboardReorderPanel.moveCurrentTab(1)
            && keyboardReorderPanel.currentPane() == reorderedPane
            && keyboardReorderPanel.paths() == pathsBeforeReorder,
        "The keyboard tab-reorder operation should move the current tab right deterministically.");

    TabbedPanel finalTabPanel(isolatedHome.path());
    int finalTabCloseRequests = 0;
    QObject::connect(&finalTabPanel, &TabbedPanel::lastTabCloseRequested,
        [&] { ++finalTabCloseRequests; });
    const auto finalTabButtons = finalTabPanel.findChildren<QToolButton*>();
    const auto finalTabCloseButton = std::find_if(finalTabButtons.cbegin(), finalTabButtons.cend(),
        [](const QToolButton* button) { return button->toolTip() == QStringLiteral("Close tab"); });
    if (finalTabCloseButton != finalTabButtons.cend())
        (*finalTabCloseButton)->click();
    passed &= expect(finalTabCloseButton != finalTabButtons.cend()
            && finalTabCloseRequests == 1 && finalTabPanel.count() == 1,
        "Clicking the visible close button on a final tab emits an owner-level close request.");

    filemanager::workspace::WorkspaceView closeLifecycleWorkspace(isolatedHome.path());
    closeLifecycleWorkspace.splitActive(Qt::Horizontal);
    TabbedPanel* closingPanel = closeLifecycleWorkspace.activePanel();
    const auto panelCloseButtons = closingPanel->findChildren<QToolButton*>();
    const auto panelCloseButton = std::find_if(panelCloseButtons.cbegin(), panelCloseButtons.cend(),
        [](const QToolButton* button) { return button->toolTip() == QStringLiteral("Close tab"); });
    if (panelCloseButton != panelCloseButtons.cend())
        (*panelCloseButton)->click();
    passed &= expect(panelCloseButton != panelCloseButtons.cend()
            && closeLifecycleWorkspace.panels().size() == 1,
        "Closing the final tab in one of several panels closes that panel.");
    int finalWorkspaceCloseRequests = 0;
    QObject::connect(&closeLifecycleWorkspace,
        &filemanager::workspace::WorkspaceView::finalTabCloseRequested,
        [&] { ++finalWorkspaceCloseRequests; });
    const auto windowCloseButtons =
        closeLifecycleWorkspace.activePanel()->findChildren<QToolButton*>();
    const auto windowCloseButton = std::find_if(
        windowCloseButtons.cbegin(), windowCloseButtons.cend(),
        [](const QToolButton* button) { return button->toolTip() == QStringLiteral("Close tab"); });
    if (windowCloseButton != windowCloseButtons.cend())
        (*windowCloseButton)->click();
    passed &= expect(windowCloseButton != windowCloseButtons.cend()
            && finalWorkspaceCloseRequests == 1
            && closeLifecycleWorkspace.panels().size() == 1,
        "Closing the final tab in the final panel requests application-window closure.");

    QWidget connectionWindow;
    TabbedPanel connectionSource(isolatedHome.path(), &connectionWindow);
    TabbedPanel connectionDestination(isolatedHome.path(), &connectionWindow);
    FilePane* transferredPane = connectionSource.currentPane();
    int oldPanelChanges = 0;
    int newPanelChanges = 0;
    QObject::connect(&connectionSource, &TabbedPanel::tabsChanged,
        [&] { ++oldPanelChanges; });
    QObject::connect(&connectionDestination, &TabbedPanel::tabsChanged,
        [&] { ++newPanelChanges; });
    passed &= expect(connectionDestination.transferTabFrom(&connectionSource, 0, 1),
        "A pane should transfer between panel owners.");
    const int oldChangesAfterTransfer = oldPanelChanges;
    const int newChangesAfterTransfer = newPanelChanges;
    transferredPane->setShowHidden(!transferredPane->showHidden());
    passed &= expect(oldPanelChanges == oldChangesAfterTransfer
            && newPanelChanges > newChangesAfterTransfer,
        "Transferred panes notify only their new panel owner.");

    {
        filemanager::workspace::WorkspaceView dragWorkspace(isolatedHome.path());
        dragWorkspace.resize(1000, 700);
        dragWorkspace.show();
        QApplication::processEvents();
        TabbedPanel* source = dragWorkspace.activePanel();
        FilePane* original = source->currentPane();
        original->setShowHidden(true);
        source->addTab(isolatedHome.path(), false);
        const QByteArray livePayload = source->tabDragPayload(0);
        source->moveCurrentTab(1);
        int resolvedIndex = -1;
        passed &= expect(TabbedPanel::resolveTabDrag(livePayload, &dragWorkspace, &resolvedIndex)
                == source && resolvedIndex == 1,
            "A drag retains the exact live pane when its tab index changes.");

        TabbedPanel foreignWindow(isolatedHome.path());
        passed &= expect(!foreignWindow.transferTabFrom(source, resolvedIndex, 0)
                && !TabbedPanel::resolveTabDrag(livePayload, &foreignWindow, nullptr)
                && source->panes().contains(original) && foreignWindow.count() == 1,
            "Cross-window transfer is refused without losing live pane or operation ownership.");
        source->addTab(isolatedHome.path());
        const QByteArray closedPayload = source->tabDragPayload(source->currentIndex());
        source->closeCurrentTab();
        QByteArray destroyedPayload;
        {
            TabbedPanel destroyed(isolatedHome.path(), &dragWorkspace);
            destroyedPayload = destroyed.tabDragPayload(0);
        }
        const QPoint dropPoint = source->mapTo(&dragWorkspace,
            QPoint(source->width() - 5, source->height() / 2));
        const auto sendDrop = [&](const QByteArray& payload, Qt::DropAction action) {
            QMimeData mime;
            mime.setData(TabbedPanel::TabMimeType, payload);
            QDragEnterEvent enter(dropPoint, action, &mime, Qt::LeftButton, Qt::NoModifier);
            QApplication::sendEvent(&dragWorkspace, &enter);
            QDropEvent drop(QPointF(dropPoint), action, &mime, Qt::LeftButton, Qt::NoModifier);
            QApplication::sendEvent(&dragWorkspace, &drop);
            return enter.isAccepted() && drop.isAccepted() && drop.dropAction() == Qt::MoveAction;
        };
        for (const auto& invalid : QList<QByteArray>{"123:0", livePayload + ":extra",
                 QByteArray(1024, 'x'), closedPayload, destroyedPayload,
                 foreignWindow.tabDragPayload(0)}) {
            passed &= expect(!sendDrop(invalid, Qt::MoveAction)
                    && dragWorkspace.panels().size() == 1 && source->count() == 2,
                "Malformed, closed, destroyed or foreign-window drags must not create a split.");
        }
        passed &= expect(!sendDrop(livePayload, Qt::CopyAction)
                && dragWorkspace.panels().size() == 1,
            "A tab copy-only payload must not silently move a live pane.");
        passed &= expect(sendDrop(livePayload, Qt::MoveAction)
                && dragWorkspace.panels().size() == 2
                && dragWorkspace.currentPane() == original && original->showHidden()
                && !source->panes().contains(original),
            "A valid UUID split-edge drop moves the exact live pane and preserves its state.");
        dragWorkspace.close();
    }

    workspace.splitActive(Qt::Horizontal);
    passed &= expect(workspace.panels().size() == 2, "A horizontal split creates a panel.");
    FilePane* movedPane = workspace.currentPane();
    passed &= expect(workspace.moveActiveTabToAdjacent(), "An active tab moves to an adjacent panel.");
    passed &= expect(workspace.currentPane() == movedPane, "Moving a tab preserves its live pane state.");
    const QList<TabbedPanel*> splitPanels = workspace.panels();
    splitPanels.first()->addTab("C:/", false);
    const int sourceCount = splitPanels.first()->count();
    const int destinationCount = splitPanels.last()->count();
    passed &= expect(splitPanels.last()->transferTabFrom(splitPanels.first(), 0, destinationCount),
        "A drag-style indexed transfer succeeds between panels.");
    passed &= expect(splitPanels.first()->count() == sourceCount - 1
            && splitPanels.last()->count() == destinationCount + 1,
        "An indexed transfer moves exactly one live tab.");
    workspace.splitActive(Qt::Vertical);
    passed &= expect(workspace.panels().size() == 3, "A nested vertical split creates a panel.");
    workspace.currentPane()->setViewMode(FilePane::ViewMode::ExtraLargeIcons);
    workspace.currentPane()->setShowHidden(true);
    workspace.currentPane()->setFolderPlacement(FilePane::FolderPlacement::Last);
    workspace.currentPane()->setGroupBy(FilePane::GroupBy::Size);
    workspace.currentPane()->setShowFileExtensions(false);

    QString error;
    const auto state = workspace.layoutState();
    passed &= expect(state.isValid(&error), "The live nested widget tree serializes as valid state.");

    filemanager::workspace::WorkspaceView restored(isolatedHome.path());
    passed &= expect(restored.restoreLayout(state, &error), "A valid nested workspace restores.");
    passed &= expect(restored.panels().size() == 3, "Restore preserves every panel.");
    passed &= expect(restored.layoutState() == state, "Workspace state round-trips exactly.");
    bool restoredViewState = false;
    for (FilePane* pane : restored.panes())
        restoredViewState |= pane->viewMode() == FilePane::ViewMode::ExtraLargeIcons && pane->showHidden()
            && pane->folderPlacement() == FilePane::FolderPlacement::Last
            && !pane->showFileExtensions()
            && pane->groupBy() == FilePane::GroupBy::Size;
    passed &= expect(restoredViewState,
        "Workspace restore preserves per-tab view mode and hidden-item state.");

    restored.closeActivePanel();
    passed &= expect(restored.panels().size() == 2, "Closing a panel collapses its split.");
    passed &= expect(restored.layoutState().isValid(&error), "Collapsed workspace state remains valid.");

    filemanager::commands::CommandRegistry commands;
    commands.add({"first", "First", "Test", {}}, QKeySequence("Ctrl+1"));
    commands.add({"second", "Second", "Test", {}}, QKeySequence("Ctrl+2"));
    QString conflict;
    passed &= expect(!commands.setShortcut("second", QKeySequence("Ctrl+1"), &conflict),
        "Duplicate shortcuts are rejected.");
    passed &= expect(conflict == "first", "Shortcut conflicts identify the owning command.");
    passed &= expect(commands.setShortcut("second", QKeySequence("Ctrl+3")),
        "An unused shortcut can be assigned.");
    }

    phase("search_begin");
    QTemporaryDir outsideSearchScope;
    QTemporaryDir searchRoot;
    QFile alphaFile(searchRoot.filePath("alpha-report.txt"));
    QFile unrelatedFile(searchRoot.filePath("unrelated.bin"));
    passed &= expect(alphaFile.open(QIODevice::WriteOnly), "Search fixture should be created.");
    passed &= expect(unrelatedFile.open(QIODevice::WriteOnly), "Filtered fixture should be created.");
    alphaFile.close();
    unrelatedFile.close();
    QFile sizedFile(searchRoot.filePath("sized.dat"));
    passed &= expect(sizedFile.open(QIODevice::WriteOnly), "Sized search fixture should open.");
    sizedFile.write(QByteArray(2048, 's'));
    sizedFile.close();
    QDir().mkpath(searchRoot.filePath("matching-folder"));
    QFile outsideSecret(outsideSearchScope.filePath("escape-only-secret.txt"));
    passed &= expect(outsideSecret.open(QIODevice::WriteOnly)
            && outsideSecret.write("outside-scope-content-marker") == 28,
        "Outside-scope traversal fixture should be created.");
    outsideSecret.close();
    const QString scopeBoundary = searchRoot.filePath("linked-outside");
#ifdef Q_OS_WIN
    const bool scopeBoundaryCreated = createJunction(
        scopeBoundary, outsideSearchScope.path());
    NativeJunctionGuard scopeBoundaryGuard(scopeBoundary, scopeBoundaryCreated);
#else
    const bool scopeBoundaryCreated = QFile::link(outsideSearchScope.path(), scopeBoundary);
#endif
    passed &= expect(scopeBoundaryCreated,
        "A directory reparse boundary should be available for scope-containment tests.");
    const QStringList settingsBeforeSessionHistory = QSettings().allKeys();
    filemanager::search::SessionSearchHistory sessionSearchHistory;
    for (int query = 0; query < 25; ++query)
        sessionSearchHistory.record(QString("query-%1").arg(query));
    sessionSearchHistory.record("  QUERY-20  ");
    passed &= expect(sessionSearchHistory.entries().size()
                == filemanager::search::SessionSearchHistory::MaximumEntries
            && sessionSearchHistory.entries().first() == "QUERY-20"
            && !sessionSearchHistory.entries().contains("query-0")
            && QSettings().allKeys() == settingsBeforeSessionHistory,
        "Filename history should be bounded, deduplicated and memory-only without settings writes.");
    sessionSearchHistory.clear();
    passed &= expect(sessionSearchHistory.entries().isEmpty(),
        "The session filename history should clear immediately on request.");

    QTemporaryDir savedSearchStorageRoot;
    passed &= expect(savedSearchStorageRoot.isValid(),
        "The isolated saved-search storage root should open.");
    const QString savedSearchPath = savedSearchStorageRoot.filePath("saved-searches.dat");
    filemanager::search::SavedSearchStore savedSearches(savedSearchPath);
    filemanager::search::SearchFilters savedFilters;
    savedFilters.name = "alpha";
    savedFilters.extension = "txt";
    savedFilters.itemType = filemanager::search::SearchItemType::Files;
    QString savedSearchError;
    passed &= expect(savedSearches.save(
            {"saved-alpha", "Alpha reports", searchRoot.path(), savedFilters, 2, 3},
            &savedSearchError),
        "An explicitly named filename/metadata search should save successfully.");
    QFile encryptedSavedSearch(savedSearchPath);
    passed &= expect(encryptedSavedSearch.open(QIODevice::ReadOnly),
        "The encrypted saved-search file should be readable as opaque data.");
    const QByteArray encryptedSavedSearchBytes = encryptedSavedSearch.readAll();
    encryptedSavedSearch.close();
    passed &= expect(!encryptedSavedSearchBytes.contains("Alpha reports")
            && !encryptedSavedSearchBytes.contains("alpha-report")
            && !encryptedSavedSearchBytes.startsWith('{'),
        "Saved-search names, scopes and queries must not appear in plaintext storage.");
    filemanager::search::SavedSearchStore reloadedSavedSearches(savedSearchPath);
    passed &= expect(reloadedSavedSearches.load(&savedSearchError)
            && reloadedSavedSearches.entries().size() == 1
            && reloadedSavedSearches.entries().first().name == "Alpha reports"
            && reloadedSavedSearches.entries().first().filters.name == "alpha"
            && reloadedSavedSearches.entries().first().sizePreset == 2
            && reloadedSavedSearches.entries().first().datePreset == 3,
        "Saved filename filters should round-trip through bounded per-user encryption.");
    passed &= expect(reloadedSavedSearches.clear(&savedSearchError)
            && reloadedSavedSearches.entries().isEmpty()
            && !QFileInfo::exists(savedSearchPath),
        "Clearing saved searches should remove both memory and encrypted storage.");
    QFile corruptSavedSearch(savedSearchPath);
    passed &= expect(corruptSavedSearch.open(QIODevice::WriteOnly)
            && corruptSavedSearch.write("not protected saved-search data") == 31,
        "The malformed saved-search fixture should be created.");
    corruptSavedSearch.close();
    filemanager::search::SavedSearchStore rejectedSavedSearches(savedSearchPath);
    passed &= expect(!rejectedSavedSearches.load(&savedSearchError)
            && rejectedSavedSearches.entries().isEmpty(),
        "Malformed or unprotected saved-search storage should fail closed without records.");

    sessionSearchHistory.record("prior-session-query");
    filemanager::ui::FileSearchDialog fileSearchDialog(searchRoot.path(),
        &sessionSearchHistory, nullptr,
        savedSearchStorageRoot.filePath("dialog-saved-searches.dat"),
        savedSearchStorageRoot.filePath("dialog-metadata-index.sqlite"));
    auto* fileSearchQuery =
        fileSearchDialog.findChild<QLineEdit*>("FileSearchQuery");
    auto* fileSearchResults =
        fileSearchDialog.findChild<QListWidget*>("FileSearchResults");
    auto* fileSearchClearRecent =
        fileSearchDialog.findChild<QPushButton*>("FileSearchClearRecent");
    auto* fileSearchManageIndex =
        fileSearchDialog.findChild<QPushButton*>("FileSearchManageIndex");
    auto* fileSearchSaved =
        fileSearchDialog.findChild<QComboBox*>("SavedSearches");
    passed &= expect(fileSearchDialog.objectName() == "FileSearchDialog"
            && fileSearchQuery && fileSearchResults && fileSearchClearRecent
            && fileSearchManageIndex && fileSearchSaved
            && fileSearchResults->selectionMode() == QAbstractItemView::ExtendedSelection
            && fileSearchManageIndex->text() == "Index this folder...",
        "The extracted filename Search dialog exposes stable controls and safe index defaults.");
    if (fileSearchQuery && fileSearchResults && fileSearchClearRecent) {
        fileSearchClearRecent->click();
        passed &= expect(sessionSearchHistory.entries().isEmpty()
                && !fileSearchClearRecent->isEnabled(),
            "The filename Search dialog clears session-only query history immediately.");
        fileSearchQuery->setText("alpha");
        QEventLoop fileSearchDialogLoop;
        QTimer::singleShot(1500, &fileSearchDialogLoop, &QEventLoop::quit);
        fileSearchDialogLoop.exec();
        passed &= expect(fileSearchResults->count() == 1
                && fileSearchResults->item(0)->data(Qt::UserRole).toString()
                    == alphaFile.fileName(),
            "The extracted filename Search dialog presents bounded asynchronous results.");
        fileSearchDialog.reject();
        passed &= expect(sessionSearchHistory.entries().first() == "alpha",
            "Closing an executed filename search records only its query in session memory.");
    }

    filemanager::search::SessionSearchHistory aqsHistory;
    filemanager::ui::FileSearchDialog aqsDialog(searchRoot.path(), &aqsHistory, nullptr,
        savedSearchStorageRoot.filePath("aqs-saved-searches.dat"),
        savedSearchStorageRoot.filePath("aqs-metadata-index.sqlite"));
    auto* aqsToggle = aqsDialog.findChild<QCheckBox*>("FileSearchUseAqs");
    auto* aqsQuery = aqsDialog.findChild<QLineEdit*>("FileSearchQuery");
    auto* aqsExtension = aqsDialog.findChild<QLineEdit*>("FileSearchExtension");
    auto* aqsScope = aqsDialog.findChild<QLabel*>("FileSearchScope");
    auto* aqsDisclosure = aqsDialog.findChild<QLabel*>("FileSearchAqsDisclosure");
    auto* aqsSave = aqsDialog.findChild<QPushButton*>("SaveSearch");
    passed &= expect(aqsToggle && aqsQuery && aqsExtension && aqsScope
            && aqsDisclosure && aqsSave
            && aqsScope->text().contains(QDir::toNativeSeparators(searchRoot.path())),
        "AQS mode should expose its exact displayed-folder scope and privacy notice.");
    if (aqsToggle && aqsQuery && aqsExtension && aqsDisclosure && aqsSave) {
        aqsToggle->setChecked(true);
        QApplication::processEvents();
        passed &= expect(aqsQuery->completer() == nullptr && !aqsExtension->isEnabled()
                && !aqsSave->isEnabled() && !aqsDisclosure->isHidden()
                && aqsQuery->placeholderText().contains("AQS"),
            "AQS mode should disable local filters, history completion and saved-search creation.");
        aqsQuery->setText("kind:document datemodified:this month");
        QEventLoop aqsStart;
        QTimer::singleShot(250, &aqsStart, &QEventLoop::quit);
        aqsStart.exec();
        aqsDialog.reject();
        passed &= expect(aqsHistory.entries().isEmpty(),
            "AQS text should not be added to session filename history when the dialog closes.");
    }

    using filemanager::search::SearchHit;
    using filemanager::search::searchHitPrecedes;
    passed &= expect(searchHitPrecedes({"C:/report.txt", "report.txt", false, 90},
                         {"C:/older.txt", "older.txt", false, 70})
            && searchHitPrecedes({"C:/file2.txt", "file2.txt", false, 50},
                {"C:/file10.txt", "file10.txt", false, 50})
            && searchHitPrecedes({"C:/A/shared.txt", "shared.txt", false, 50},
                {"C:/B/shared.txt", "shared.txt", false, 50}),
        "Search ranking should use score, natural-name and path tie breakers deterministically.");
    filemanager::search::SearchService search;
    QEventLoop searchLoop;
    QList<filemanager::search::SearchHit> hits;
    QString searchBackend;
    QObject::connect(&search, &filemanager::search::SearchService::backendSelected,
        [&](quint64, const QString& backend) { searchBackend = backend; });
    QObject::connect(&search, &filemanager::search::SearchService::resultFound,
        [&hits](quint64, const auto& hit) { hits.append(hit); });
    QObject::connect(&search, &filemanager::search::SearchService::finished,
        &searchLoop, &QEventLoop::quit);
    const auto waitForSearch = [&] {
        if (runLoopWithTimeout(searchLoop))
            return true;
        search.cancel();
        runLoopWithTimeout(searchLoop, 1000);
        return false;
    };
    search.start(searchRoot.path(), "alpha", "txt");
    passed &= expect(waitForSearch(), "Direct filename search should finish before its timeout.");
    passed &= expect(hits.size() == 1 && hits.first().name == "alpha-report.txt",
        "Background search returns filtered fuzzy results.");
    passed &= expect(searchBackend == "Direct folder scan",
        "An unindexed temporary scope should fall back to direct link-safe traversal.");
#ifdef Q_OS_WIN
    std::atomic_bool cancelledNativeSearch{true};
    const auto cancelledNative = filemanager::search::WindowsSearch::query(
        filemanager::platform::knownFolderPath(filemanager::platform::KnownFolder::Documents),
        QStringLiteral("cancelled"), 10, &cancelledNativeSearch);
    passed &= expect(cancelledNative.cancelled && !cancelledNative.usedIndex,
        "A native indexed search should honor cancellation before contacting SystemIndex.");
    const auto documentsProbe = filemanager::search::WindowsSearch::query(
        filemanager::platform::knownFolderPath(filemanager::platform::KnownFolder::Documents),
        QStringLiteral("unlikely-file-manager-probe"), 10);
    passed &= expect(documentsProbe.usedIndex || !documentsProbe.diagnostic.isEmpty(),
        "Windows Search capability detection should either execute a bounded read-only query or "
        "return a typed fallback reason.");
    const auto aqsPolicyFailure = filemanager::search::WindowsSearch::queryAqs(
        searchRoot.path(), QString(4097, QLatin1Char('a')), 10);
    passed &= expect(!aqsPolicyFailure.usedIndex && !aqsPolicyFailure.diagnostic.isEmpty(),
        "Windows AQS should reject oversized input before contacting SystemIndex.");
    const auto aqsDocumentsProbe = filemanager::search::WindowsSearch::queryAqs(
        filemanager::platform::knownFolderPath(filemanager::platform::KnownFolder::Documents),
        QStringLiteral("System.ItemName:unlikely-file-manager-aqs-probe"), 10);
    passed &= expect(aqsDocumentsProbe.usedIndex || !aqsDocumentsProbe.diagnostic.isEmpty(),
        "AQS should either execute through the existing Windows index or return a typed reason.");
#endif
    hits.clear();
    filemanager::search::SearchFilters structuredFilters;
    structuredFilters.itemType = filemanager::search::SearchItemType::Files;
    structuredFilters.minimumSize = 1024;
    structuredFilters.maximumSize = 4096;
    structuredFilters.modifiedAfter = QDateTime::currentDateTime().addDays(-1);
    search.start(searchRoot.path(), structuredFilters);
    passed &= expect(waitForSearch(), "Structured filename search should finish before its timeout.");
    passed &= expect(hits.size() == 1 && hits.first().name == "sized.dat",
        "Background search combines item-type, size and modified-date filters.");
    if (scopeBoundaryCreated) {
        hits.clear();
        search.start(searchRoot.path(), "escape-only-secret", "txt");
        passed &= expect(waitForSearch(), "Boundary filename search should finish before its timeout.");
        passed &= expect(hits.isEmpty(),
            "Filename search cannot escape its selected scope through a reparse directory.");
        const auto containedStatistics = filemanager::files::measurePath(searchRoot.path());
        const auto containedDuplicates = filemanager::files::findDuplicates(searchRoot.path());
        passed &= expect(containedStatistics.files == 3
                && containedDuplicates.filesScanned == 3,
            "Analysis and duplicate hashing skip every outside-scope reparse boundary.");
    }

    passed &= expect(alphaFile.open(QIODevice::WriteOnly | QIODevice::Truncate),
        "Content-search fixture should reopen for writing.");
    alphaFile.write("first line\napproved private-memory search phrase\nlast line");
    alphaFile.close();
    filemanager::search::ContentSearchService contentSearch;
    QEventLoop contentLoop;
    QList<filemanager::search::ContentSearchHit> contentHits;
    QObject::connect(&contentSearch, &filemanager::search::ContentSearchService::resultFound,
        [&](quint64, const auto& hit) { contentHits.append(hit); });
    QObject::connect(&contentSearch, &filemanager::search::ContentSearchService::finished,
        &contentLoop, &QEventLoop::quit);
    const auto waitForContentSearch = [&] {
        if (runLoopWithTimeout(contentLoop))
            return true;
        contentSearch.cancel();
        runLoopWithTimeout(contentLoop, 1000);
        return false;
    };
    contentSearch.start(searchRoot.path(), "private-memory", "txt");
    passed &= expect(waitForContentSearch(), "Content search should finish before its timeout.");
    passed &= expect(contentHits.size() == 1 && contentHits.first().line == 2
            && contentHits.first().path == alphaFile.fileName(),
        "On-demand content search returns an in-memory line-aware result.");
    filemanager::ui::ContentSearchDialog contentSearchDialog(searchRoot.path());
    auto* contentSearchQuery =
        contentSearchDialog.findChild<QLineEdit*>("ContentSearchQuery");
    auto* contentSearchStart =
        contentSearchDialog.findChild<QPushButton*>("ContentSearchStart");
    auto* contentSearchResults =
        contentSearchDialog.findChild<QListWidget*>("ContentSearchResults");
    auto* contentSearchDisclosure =
        contentSearchDialog.findChild<QLabel*>("ContentSearchDisclosure");
    passed &= expect(contentSearchDialog.objectName() == "ContentSearchDialog"
            && contentSearchQuery && contentSearchStart && contentSearchResults
            && contentSearchDisclosure
            && contentSearchDisclosure->text().contains("not indexed or saved")
            && contentSearchResults->selectionMode() == QAbstractItemView::ExtendedSelection,
        "The extracted content Search dialog exposes stable privacy and operation contracts.");
    if (contentSearchQuery && contentSearchStart && contentSearchResults) {
        contentSearchQuery->setText("private-memory");
        contentSearchStart->click();
        QEventLoop contentSearchDialogLoop;
        QTimer::singleShot(1500, &contentSearchDialogLoop, &QEventLoop::quit);
        contentSearchDialogLoop.exec();
        passed &= expect(contentSearchResults->count() == 1
                && contentSearchResults->item(0)->data(Qt::UserRole).toString()
                    == alphaFile.fileName(),
            "The extracted content Search dialog presents only its in-memory scoped result.");
        contentSearchDialog.removeResults({alphaFile.fileName()});
        passed &= expect(contentSearchResults->count() == 0,
            "The content Search dialog removes successfully recycled results from its surface.");
    }
    if (scopeBoundaryCreated) {
        contentHits.clear();
        contentSearch.start(searchRoot.path(), "outside-scope-content-marker", "txt");
        passed &= expect(waitForContentSearch(), "Boundary content search should finish before its timeout.");
        passed &= expect(contentHits.isEmpty(),
            "Content search never opens a file beyond a reparse scope boundary.");
    }

    const QString indexDatabase = searchRoot.filePath("metadata.sqlite");
    passed &= expect(QDir().mkpath(searchRoot.filePath("user-databases")),
        "User database fixture directory should be created.");
    const QStringList userDatabasePaths{searchRoot.filePath("notes.sqlite"),
        searchRoot.filePath("notes.sqlite-journal"), searchRoot.filePath("notes.sqlite-wal"),
        searchRoot.filePath("notes.sqlite-shm"), searchRoot.filePath("metadata.sqlite.backup"),
        searchRoot.filePath("user-databases/metadata.sqlite")};
    for (const QString& path : userDatabasePaths) {
        QFile fixture(path);
        passed &= expect(fixture.open(QIODevice::WriteOnly) && fixture.write("user file") == 9,
            "Unrelated database-shaped files should be created.");
    }
    const QStringList ownedIndexPaths{indexDatabase, indexDatabase + "-journal",
        indexDatabase + "-wal", indexDatabase + "-shm"};
    constexpr int IndexStageCount = int(filemanager::search::MetadataIndex::RebuildStage::Count);
    struct IndexTrace {
        std::atomic<qint64> elapsed[IndexStageCount];
        std::atomic_int count[IndexStageCount];
        IndexTrace() {
            for (int i = 0; i < IndexStageCount; ++i) { elapsed[i] = -1; count[i] = -1; }
        }
    };
    const auto indexTrace = std::make_shared<IndexTrace>();
    filemanager::search::MetadataIndex metadataIndex;
    QEventLoop indexLoop;
    QString indexError;
    quint64 expectedIndexGeneration = 0;
    bool indexFinished = false;
    bool indexCancelled = false;
    int indexItemCount = 0;
    QObject::connect(&metadataIndex, &filemanager::search::MetadataIndex::finished,
        [&](quint64 generation, bool cancelled, const QString& error, int itemCount) {
            if (generation != expectedIndexGeneration)
                return;
            indexFinished = true;
            indexCancelled = cancelled;
            indexItemCount = itemCount;
            indexError = error;
            indexLoop.quit();
        });
    const auto waitForIndex = [&](quint64 generation, const char* operation) {
        expectedIndexGeneration = generation;
        indexFinished = false;
        indexCancelled = false;
        indexItemCount = 0;
        indexError.clear();
        QElapsedTimer elapsed;
        elapsed.start();
        const bool inTime = runLoopWithTimeout(indexLoop) && indexFinished;
        if (!inTime) {
            metadataIndex.cancel();
            if (!indexFinished)
                runLoopWithTimeout(indexLoop, 1000);
        }
        const bool success = inTime && !indexCancelled && indexError.isEmpty();
        if (!success) {
            std::fprintf(stderr,
                "Metadata index operation=%s generation=%llu elapsed_ms=%lld "
                "in_time=%d finished=%d cancelled=%d items=%d error=%s\n",
                operation, static_cast<unsigned long long>(generation),
                static_cast<long long>(elapsed.elapsed()), inTime, indexFinished,
                indexCancelled, indexItemCount, indexError.toUtf8().constData());
        }
        return success;
    };
    const auto recordIndexStage = [indexTrace](filemanager::search::MetadataIndex::RebuildStage stage,
                                      qint64 elapsedMs, int count) {
        indexTrace->count[int(stage)].store(count);
        indexTrace->elapsed[int(stage)].store(elapsedMs);
    };
    passed &= expect(waitForIndex(metadataIndex.rebuild(indexDatabase, searchRoot.path(), recordIndexStage), "rebuild"),
        "Metadata index rebuild should finish before its timeout.");
    constexpr const char* indexStageNames[]{"worker_started", "connection_created", "parent_directory_checked",
        "database_opened", "storage_paths_resolved", "transaction_started", "schema_prepared",
        "enumeration_finished", "transaction_finished", "connection_closed", "connection_removed"};
    static_assert(std::size(indexStageNames) == IndexStageCount);
    qint64 previousIndexStageTime = 0;
    for (int stage = 0; stage < IndexStageCount; ++stage) {
        const qint64 elapsed = indexTrace->elapsed[stage].load();
        std::printf("Metadata index stage=%s elapsed_ms=%lld interval_ms=%lld items=%d\n", indexStageNames[stage],
            static_cast<long long>(elapsed), static_cast<long long>(elapsed < 0 ? -1 : elapsed - previousIndexStageTime),
            indexTrace->count[stage].load());
        if (elapsed >= 0)
            previousIndexStageTime = elapsed;
    }
    std::fflush(stdout);
    if (!indexError.isEmpty())
        std::fprintf(stderr, "Metadata index fixture error: %s\n",
            indexError.toUtf8().constData());
    passed &= expect(indexError.isEmpty(), "Metadata-only index builds successfully.");
    QList<filemanager::search::SearchHit> indexedHits;
    QObject::connect(&metadataIndex, &filemanager::search::MetadataIndex::resultFound,
        [&](quint64 generation, const auto& hit) {
            if (generation == expectedIndexGeneration)
                indexedHits.append(hit);
        });
    const QString storageCheckConnection = "metadata-storage-exclusion-test";
    {
        QSqlDatabase database = QSqlDatabase::addDatabase("QSQLITE", storageCheckConnection);
        database.setDatabaseName(indexDatabase);
        passed &= expect(database.open(), "Built index should be inspectable for storage exclusions.");
        QSqlQuery rows("SELECT path FROM metadata", database);
        QSet<QString> storedPaths;
        while (rows.next()) storedPaths.insert(rows.value(0).toString());
        for (const QString& path : ownedIndexPaths)
            passed &= expect(!storedPaths.contains(path), "Rebuild must not store its own database or sidecars.");
        for (const QString& path : userDatabasePaths)
            passed &= expect(storedPaths.contains(path), "Rebuild must retain unrelated user database files.");
        // Inject the rows older builds could have stored, including now-missing
        // sidecars. Search must suppress them without requiring a rebuild first.
        QSqlQuery legacy(database);
        legacy.prepare("INSERT OR REPLACE INTO metadata "
            "(root, path, name, extension, size, modified, is_directory) VALUES (?, ?, ?, ?, 0, 0, 0)");
        for (const QString& path : ownedIndexPaths) {
            legacy.bindValue(0, QDir::cleanPath(searchRoot.path()));
            legacy.bindValue(1, path);
            legacy.bindValue(2, QFileInfo(path).fileName());
            legacy.bindValue(3, QFileInfo(path).suffix());
            passed &= expect(legacy.exec(), "Legacy self-indexed rows should be seeded.");
        }
        database.close();
    }
    QSqlDatabase::removeDatabase(storageCheckConnection);
    passed &= expect(waitForIndex(metadataIndex.query(indexDatabase, searchRoot.path(), "", ""),
        "storage exclusion query"), "Storage-exclusion query should finish before its timeout.");
    QSet<QString> returnedPaths;
    for (const auto& hit : indexedHits) returnedPaths.insert(hit.path);
    for (const QString& path : ownedIndexPaths)
        passed &= expect(!returnedPaths.contains(path), "Search must hide legacy database/sidecar entries.");
    for (const QString& path : userDatabasePaths)
        passed &= expect(returnedPaths.contains(path), "Search must retain unrelated database files and backups.");
    indexedHits.clear();
    // The index uses rollback journaling; a user directory occupying the unused
    // shared-memory pathname is not a SQLite storage file and must remain visible.
    const QString sidecarNamedDirectory = indexDatabase + "-shm";
    const QString sidecarDirectoryChild = QDir(sidecarNamedDirectory).filePath("user-document.txt");
    passed &= expect(QDir().mkpath(sidecarNamedDirectory), "Sidecar-named user directory should be created.");
    {
        QFile fixture(sidecarDirectoryChild);
        passed &= expect(fixture.open(QIODevice::WriteOnly) && fixture.write("user document") == 13,
            "Sidecar-named directory should contain a user document.");
    }
    passed &= expect(waitForIndex(metadataIndex.rebuild(indexDatabase, searchRoot.path()),
        "sidecar-named directory rebuild"), "Directory-preservation rebuild should finish before its timeout.");
    passed &= expect(waitForIndex(metadataIndex.query(indexDatabase, searchRoot.path(), "", ""),
        "sidecar-named directory query"), "Directory-preservation query should finish before its timeout.");
    bool directoryRetained = false;
    bool childRetained = false;
    for (const auto& hit : indexedHits) {
        if (hit.path == sidecarNamedDirectory) directoryRetained = hit.directory;
        if (hit.path == sidecarDirectoryChild) childRetained = !hit.directory;
    }
    passed &= expect(directoryRetained && childRetained,
        "A folder sharing an unused sidecar name and its contents must remain searchable.");
    indexedHits.clear();
    // A timed-out prior worker can complete after the next request starts. Its completion
    // must neither release the current wait nor contaminate that request's result list.
    const quint64 priorIndexGeneration = expectedIndexGeneration;
    QTimer::singleShot(0, &metadataIndex, [&metadataIndex, priorIndexGeneration] {
        emit metadataIndex.finished(priorIndexGeneration, true, QStringLiteral("obsolete operation"), 0);
        emit metadataIndex.resultFound(priorIndexGeneration,
            {QStringLiteral("obsolete.txt"), QStringLiteral("obsolete.txt"), false, 0});
    });
    passed &= expect(waitForIndex(metadataIndex.query(indexDatabase, searchRoot.path(), "alpha", "txt"),
        "fuzzy query"), "Metadata index query should finish before its timeout.");
    passed &= expect(indexedHits.size() == 1 && indexedHits.first().name == "alpha-report.txt",
        "Metadata index returns fuzzy filtered results.");
    indexedHits.clear();
    passed &= expect(waitForIndex(metadataIndex.query(indexDatabase, searchRoot.path(), structuredFilters),
        "structured query"), "Structured index query should finish before its timeout.");
    passed &= expect(indexedHits.size() == 1 && indexedHits.first().name == "sized.dat",
        "Metadata-index search applies the same type, size and date filters.");
    if (scopeBoundaryCreated) {
        indexedHits.clear();
        passed &= expect(waitForIndex(metadataIndex.query(indexDatabase, searchRoot.path(),
            "escape-only-secret", "txt"), "boundary query"),
            "Boundary index query should finish before its timeout.");
        passed &= expect(indexedHits.isEmpty(),
            "Persistent metadata indexing cannot escape through a reparse directory.");
    }
    const QString schemaConnection = "metadata-schema-test";
    {
        QSqlDatabase database = QSqlDatabase::addDatabase("QSQLITE", schemaConnection);
        database.setDatabaseName(indexDatabase);
        passed &= expect(database.open(), "Rollback fixture database should open.");
        QSqlQuery trigger(database);
        passed &= expect(trigger.exec("CREATE TRIGGER reject_fixture_insert BEFORE INSERT ON metadata "
            "BEGIN SELECT RAISE(ABORT, 'fixture insert rejected'); END"),
            "Rollback fixture should reject replacement inserts.");
        database.close();
    }
    QSqlDatabase::removeDatabase(schemaConnection);
    passed &= expect(!waitForIndex(metadataIndex.rebuild(indexDatabase, searchRoot.path()),
            "expected insertion failure") && !indexCancelled && indexError.contains("fixture insert rejected"),
        "An insertion error must fail the rebuild without reporting success.");
    indexedHits.clear();
    passed &= expect(waitForIndex(metadataIndex.query(indexDatabase, searchRoot.path(), "alpha", "txt"),
            "query after rollback") && indexedHits.size() == 1 && indexedHits.first().name == "alpha-report.txt",
        "A failed replacement must roll back its delete and preserve the prior searchable index.");
    {
        QSqlDatabase database = QSqlDatabase::addDatabase("QSQLITE", schemaConnection);
        database.setDatabaseName(indexDatabase);
        passed &= expect(database.open(), "Rollback fixture cleanup database should open.");
        QSqlQuery query(database);
        passed &= expect(query.exec("DROP TRIGGER reject_fixture_insert"), "Rollback trigger should be removed.");
        database.close();
    }
    QSqlDatabase::removeDatabase(schemaConnection);
    const QString failedNewDatabase = searchRoot.filePath("failed-new-index.sqlite");
    passed &= expect(!waitForIndex(metadataIndex.rebuild(failedNewDatabase, searchRoot.filePath("missing-root")),
            "expected missing-root failure") && !indexCancelled && !indexError.isEmpty(),
        "A new index with an unavailable root must report an error.");
    {
        QSqlDatabase database = QSqlDatabase::addDatabase("QSQLITE", schemaConnection);
        database.setDatabaseName(failedNewDatabase);
        passed &= expect(database.open(), "Failed new index should be inspectable.");
        QSqlQuery query("SELECT count(*) FROM sqlite_master WHERE name IN ('metadata', 'metadata_root_name')", database);
        passed &= expect(query.next() && query.value(0).toInt() == 0,
            "A failed initial build must roll back both schema objects with its data transaction.");
        database.close();
    }
    QSqlDatabase::removeDatabase(schemaConnection);
    const QString blockedIndexParent = searchRoot.filePath("index-parent-blocker");
    {
        QFile blocker(blockedIndexParent);
        passed &= expect(blocker.open(QIODevice::WriteOnly) && blocker.write("not a dir") == 9,
            "Index-parent failure fixture should be a regular file.");
    }
    const QString blockedIndexDatabase = QDir(blockedIndexParent).filePath("index.sqlite");
    passed &= expect(!waitForIndex(metadataIndex.rebuild(blockedIndexDatabase, searchRoot.path()),
            "expected parent-directory failure") && indexFinished && !indexCancelled
            && !indexError.isEmpty() && indexItemCount == 0,
        "A failed index-parent creation must report an error, never successful empty indexing.");
    {
        QFile blocker(blockedIndexParent);
        passed &= expect(blocker.open(QIODevice::ReadOnly) && blocker.readAll() == "not a dir"
                && !QFileInfo::exists(blockedIndexDatabase),
            "Index-parent failure must preserve the blocking file and create no database beneath it.");
    }
    QStringList schemaFields;
    {
        QSqlDatabase database = QSqlDatabase::addDatabase("QSQLITE", schemaConnection);
        database.setDatabaseName(indexDatabase);
        passed &= expect(database.open(), "Metadata schema should be inspectable.");
        QSqlQuery schema("PRAGMA table_info(metadata)", database);
        while (schema.next())
            schemaFields.append(schema.value(1).toString());
        database.close();
    }
    QSqlDatabase::removeDatabase(schemaConnection);
    passed &= expect(!schemaFields.contains("content") && !schemaFields.contains("text"),
        "Index schema contains no file-content field.");
    passed &= expect(waitForIndex(metadataIndex.removeRoot(indexDatabase, searchRoot.path()), "remove"),
        "Metadata index removal should finish before its timeout.");
    indexedHits.clear();
    passed &= expect(waitForIndex(metadataIndex.query(indexDatabase, searchRoot.path(), "alpha", "txt"),
        "post-removal query"), "Post-removal index query should finish before its timeout.");
    passed &= expect(indexedHits.isEmpty(),
        "Disabling a folder index deletes all of its stored metadata.");

    phase("transfer_begin");
    QTemporaryDir transferRoot;
    const QString transferSourceDirectory = transferRoot.filePath("source");
    const QString transferDestinationDirectory = transferRoot.filePath("destination");
    QDir().mkpath(transferSourceDirectory);
    QDir().mkpath(transferDestinationDirectory);
    QFile transferFixture(QDir(transferSourceDirectory).filePath("payload.txt"));
    passed &= expect(transferFixture.open(QIODevice::WriteOnly), "Transfer fixture should open.");
    transferFixture.write("background transfer payload");
    transferFixture.close();

    filemanager::files::TransferController transfer;
    QEventLoop transferLoop;
    QTimer transferTimeout;
    transferTimeout.setSingleShot(true);
    QObject::connect(&transferTimeout, &QTimer::timeout, &transferLoop, &QEventLoop::quit);
    QStringList transferErrors;
    bool transferCancelled = false;
    bool transferCompleted = false;
    quint64 expectedTransferGeneration = 0;
    QObject::connect(&transfer, &filemanager::files::TransferController::finished,
        [&](quint64 generation, bool cancelled, const QStringList& errors) {
            if (generation != expectedTransferGeneration)
                return;
            transferCompleted = true;
            transferCancelled = cancelled;
            transferErrors = errors;
            transferTimeout.stop();
            transferLoop.quit();
        });
    const auto beginTransfer = [&](filemanager::files::TransferRequest request) {
        transferCompleted = false;
        transferTimeout.start(15000);
        expectedTransferGeneration = transfer.start(std::move(request));
    };
    const auto waitForTransfer = [&] {
        if (!transferCompleted)
            transferLoop.exec();
        return transferCompleted;
    };
    beginTransfer({{transferFixture.fileName()}, transferDestinationDirectory,
        filemanager::files::TransferOperation::Copy});
    waitForTransfer();
    const QString copiedPayload = QDir(transferDestinationDirectory).filePath("payload.txt");
    passed &= expect(transferCompleted && !transferCancelled && transferErrors.isEmpty()
            && QFileInfo::exists(copiedPayload),
        "Background transfer copies a file without blocking the event loop.");

#ifdef Q_OS_WIN
    const QString metadataSource = QDir(transferSourceDirectory).filePath("metadata-source.txt");
    const QString metadataDestination =
        QDir(transferDestinationDirectory).filePath("metadata-copy.txt");
    QFile metadataFixture(metadataSource);
    passed &= expect(metadataFixture.open(QIODevice::ReadWrite)
            && metadataFixture.write("metadata payload") == 16,
        "Native metadata-copy fixture should open.");
    const QDateTime preservedTime = QDateTime::currentDateTimeUtc().addDays(-5);
    passed &= expect(metadataFixture.setFileTime(preservedTime, QFileDevice::FileModificationTime),
        "Native metadata-copy fixture should receive a stable modification time.");
    metadataFixture.close();
    QFile sourceZone(metadataSource + QStringLiteral(":Zone.Identifier"));
    passed &= expect(sourceZone.open(QIODevice::WriteOnly)
            && sourceZone.write("[ZoneTransfer]\r\nZoneId=3\r\n") == 26,
        "NTFS Mark-of-the-Web fixture should be written as an alternate stream.");
    sourceZone.close();
    const auto metadataCopy = filemanager::platform::copyFileAtomically(
        metadataSource, metadataDestination, false, [](qint64, qint64) { return true; });
    const auto metadataVolume =
        filemanager::platform::nativeVolumeCapabilities(metadataDestination);
    QFile copiedZone(metadataDestination + QStringLiteral(":Zone.Identifier"));
    passed &= expect(metadataCopy.ok && !metadataCopy.metadataConfirmationRequired
            && metadataVolume.known && metadataVolume.namedStreams
            && metadataVolume.sparseFiles && metadataVolume.persistentAcls
            && copiedZone.open(QIODevice::ReadOnly)
            && copiedZone.readAll() == QByteArray("[ZoneTransfer]\r\nZoneId=3\r\n")
            && QFileInfo(metadataDestination).lastModified().toMSecsSinceEpoch()
                == QFileInfo(metadataSource).lastModified().toMSecsSinceEpoch(),
        "Native atomic copy preserves timestamps and Mark-of-the-Web alternate streams.");
    const auto refusedRelativeOpen = filemanager::platform::openWithSystemShell("relative.exe");
    const auto refusedMissingOpen = filemanager::platform::openWithSystemShell(
        transferRoot.filePath("missing-download.exe"));
    passed &= expect(!refusedRelativeOpen.success && !refusedMissingOpen.success,
        "Native open delegation should refuse ambiguous relative and unavailable paths before Shell execution.");
    copiedZone.close();

    const QString cancelledNativeDestination =
        QDir(transferDestinationDirectory).filePath("native-cancelled.bin");
    const auto cancelledNativeCopy = filemanager::platform::copyFileAtomically(
        metadataSource, cancelledNativeDestination, false,
        [](qint64, qint64) { return false; });
    passed &= expect(cancelledNativeCopy.cancelled
            && !QFileInfo::exists(cancelledNativeDestination)
            && QDir(transferDestinationDirectory).entryList(
                {QStringLiteral(".file-manager-copy-*.tmp")}, QDir::Files | QDir::Hidden).isEmpty(),
        "Cancelled native copy removes its staging file and commits no destination.");

    const auto verifyInjectedTransferFailure = [&](quint32 nativeCode,
            filemanager::files::TransferFailureCategory expectedCategory,
            const char* expectation) {
        QTemporaryDir faultRoot;
        const QString source = faultRoot.filePath("fault-source.bin");
        const QString destinationRoot = faultRoot.filePath("destination");
        QDir().mkpath(destinationRoot);
        QFile payload(source);
        const bool fixtureReady = payload.open(QIODevice::WriteOnly)
            && payload.write(QByteArray(8192, 'f')) == 8192;
        payload.close();
        filemanager::files::TransferFailure observedFailure;
        qint64 observedProgress = 0;
        filemanager::files::TransferController controller(
            [nativeCode](const QString&, const QString&, bool,
                const std::function<bool(qint64, qint64)>& progress, bool) {
                if (progress)
                    progress(4096, 8192);
                return filemanager::platform::NativeCopyResult{
                    false, false, QStringLiteral("Controlled transfer fault."), nativeCode};
            });
        QEventLoop loop;
        QObject::connect(&controller, &filemanager::files::TransferController::progress,
            [&](quint64, qint64 completed, qint64, const QString&) {
                observedProgress = qMax(observedProgress, completed);
            });
        QObject::connect(&controller, &filemanager::files::TransferController::itemFailed,
            [&](quint64, const filemanager::files::TransferFailure& failure) {
                observedFailure = failure;
            });
        QObject::connect(&controller, &filemanager::files::TransferController::finished,
            &loop, &QEventLoop::quit);
        controller.start({{source}, destinationRoot,
            filemanager::files::TransferOperation::Copy});
        const bool completed = runLoopWithTimeout(loop);
        const QString destination = QDir(destinationRoot).filePath("fault-source.bin");
        return expect(fixtureReady && completed && observedProgress == 4096
                && observedFailure.nativeCode == nativeCode
                && observedFailure.category == expectedCategory && observedFailure.retryable
                && QFileInfo::exists(source) && !QFileInfo::exists(destination), expectation);
    };
    passed &= verifyInjectedTransferFailure(112,
        filemanager::files::TransferFailureCategory::InsufficientSpace,
        "A controlled mid-transfer disk-full fault should preserve partial progress, typed retry "
        "evidence and source safety without a committed destination.");
    passed &= verifyInjectedTransferFailure(5,
        filemanager::files::TransferFailureCategory::PermissionDenied,
        "A controlled permission-revocation fault should remain retryable and commit nothing.");
    passed &= verifyInjectedTransferFailure(1167,
        filemanager::files::TransferFailureCategory::DeviceDisconnected,
        "A controlled device-removal fault should remain retryable and commit nothing.");

    QTemporaryDir reconnectRoot;
    const QString reconnectSource = reconnectRoot.filePath("network-retry.bin");
    const QString reconnectDestination = reconnectRoot.filePath("destination");
    QDir().mkpath(reconnectDestination);
    QFile reconnectPayload(reconnectSource);
    passed &= expect(reconnectPayload.open(QIODevice::WriteOnly)
            && reconnectPayload.write(QByteArray(8192, 'n')) == 8192,
        "The controlled mapped/UNC reconnect fixture should be created.");
    reconnectPayload.close();
    std::atomic_bool networkDestinationOnline = false;
    std::atomic_int networkCopyAttempts = 0;
    const filemanager::files::TransferController::CopyBackend reconnectCopy =
        [&networkCopyAttempts](const QString& source, const QString& destination, bool,
            const std::function<bool(qint64, qint64)>& progress, bool) {
            ++networkCopyAttempts;
            if (progress)
                progress(QFileInfo(source).size(), QFileInfo(source).size());
            const bool copied = QFile::copy(source, destination);
            return filemanager::platform::NativeCopyResult{copied, false,
                copied ? QString{} : QStringLiteral("Controlled network copy failed."),
                copied ? 0U : 53U};
        };
    const filemanager::files::TransferController::DestinationProbe reconnectProbe =
        [&networkDestinationOnline](const QString&) {
            return filemanager::files::TransferController::DestinationState{
                networkDestinationOnline.load(), 1024 * 1024};
        };
    filemanager::files::TransferFailure offlineNetworkFailure;
    {
        filemanager::files::TransferController offlineController(
            reconnectCopy, reconnectProbe);
        QEventLoop offlineLoop;
        QObject::connect(&offlineController,
            &filemanager::files::TransferController::itemFailed,
            [&](quint64, const filemanager::files::TransferFailure& failure) {
                offlineNetworkFailure = failure;
            });
        QObject::connect(&offlineController, &filemanager::files::TransferController::finished,
            &offlineLoop, &QEventLoop::quit);
        offlineController.start({{reconnectSource}, reconnectDestination,
            filemanager::files::TransferOperation::Copy});
        passed &= expect(runLoopWithTimeout(offlineLoop)
                && offlineNetworkFailure.category
                    == filemanager::files::TransferFailureCategory::DestinationUnavailable
                && offlineNetworkFailure.retryable && networkCopyAttempts.load() == 0
                && !QFileInfo::exists(QDir(reconnectDestination).filePath("network-retry.bin")),
            "An unavailable mapped/UNC destination should fail once with retry evidence and never "
            "probe credentials, invoke the copy backend or retry automatically.");
    }
    networkDestinationOnline.store(true);
    bool reconnectSucceeded = false;
    {
        filemanager::files::TransferController reconnectedController(
            reconnectCopy, reconnectProbe);
        QEventLoop reconnectedLoop;
        QObject::connect(&reconnectedController,
            &filemanager::files::TransferController::finished,
            &reconnectedLoop, [&](quint64, bool cancelled, const QStringList& errors) {
                reconnectSucceeded = !cancelled && errors.isEmpty();
                reconnectedLoop.quit();
            });
        reconnectedController.start({{reconnectSource}, reconnectDestination,
            filemanager::files::TransferOperation::Copy});
        passed &= expect(runLoopWithTimeout(reconnectedLoop) && reconnectSucceeded
                && networkCopyAttempts.load() == 1
                && QFileInfo::exists(QDir(reconnectDestination).filePath("network-retry.bin")),
            "An explicit retry after mapped/UNC reconnection should rerun preflight and complete "
            "through the normal copy contract.");
    }

    QTemporaryDir resumeFaultRoot;
    const QString resumeSource = resumeFaultRoot.filePath("resume-source.bin");
    const QString resumeDestinationRoot = resumeFaultRoot.filePath("destination");
    QDir().mkpath(resumeDestinationRoot);
    QFile resumePayload(resumeSource);
    passed &= expect(resumePayload.open(QIODevice::WriteOnly)
            && resumePayload.write(QByteArray(8192, 'r')) == 8192,
        "The controlled suspend/resume fixture should be created.");
    resumePayload.close();
    std::atomic_bool releaseResumeBackend = false;
    filemanager::files::TransferController resumeController(
        [&releaseResumeBackend](const QString& source, const QString& destination, bool,
            const std::function<bool(qint64, qint64)>& progress, bool) {
            while (!releaseResumeBackend.load())
                QThread::msleep(1);
            if (progress && !progress(4096, 8192)) {
                return filemanager::platform::NativeCopyResult{
                    false, true, QStringLiteral("Cancelled."), 995};
            }
            const bool copied = QFile::copy(source, destination);
            return filemanager::platform::NativeCopyResult{copied, false,
                copied ? QString{} : QStringLiteral("Controlled copy failed."), copied ? 0U : 5U};
        });
    QEventLoop resumeLoop;
    bool resumeFinished = false;
    QObject::connect(&resumeController, &filemanager::files::TransferController::finished,
        &resumeLoop, [&](quint64, bool cancelled, const QStringList& errors) {
            resumeFinished = !cancelled && errors.isEmpty();
            resumeLoop.quit();
        });
    resumeController.start({{resumeSource}, resumeDestinationRoot,
        filemanager::files::TransferOperation::Copy});
    resumeController.pause();
    releaseResumeBackend.store(true);
    QTimer::singleShot(100, &resumeController, &filemanager::files::TransferController::resume);
    QElapsedTimer resumeElapsed;
    resumeElapsed.start();
    const bool resumedInTime = runLoopWithTimeout(resumeLoop);
    std::fprintf(stderr, "shell_diagnostic resume intime=%d finished=%d elapsed_ms=%lld destination=%d\n",
        resumedInTime, resumeFinished, static_cast<long long>(resumeElapsed.elapsed()),
        QFileInfo::exists(QDir(resumeDestinationRoot).filePath("resume-source.bin")));
    passed &= expect(resumedInTime && resumeFinished && resumeElapsed.elapsed() >= 75
            && QFileInfo::exists(QDir(resumeDestinationRoot).filePath("resume-source.bin")),
        "A controlled system-suspension pause should block the copy callback and continue safely "
        "after resume without being misclassified as a failure.");

    const QString offlineSource = QDir(transferSourceDirectory).filePath("offline-placeholder.txt");
    QFile offlineFixture(offlineSource);
    passed &= expect(offlineFixture.open(QIODevice::WriteOnly)
            && offlineFixture.write("offline") == 7,
        "Offline-placeholder policy fixture should open.");
    offlineFixture.close();
    const DWORD offlineOriginal = GetFileAttributesW(
        reinterpret_cast<LPCWSTR>(offlineSource.utf16()));
    const bool offlineMarked = offlineOriginal != INVALID_FILE_ATTRIBUTES
        && SetFileAttributesW(reinterpret_cast<LPCWSTR>(offlineSource.utf16()),
            offlineOriginal | FILE_ATTRIBUTE_OFFLINE);
    if (offlineMarked) {
        const QString offlineDestination =
            QDir(transferDestinationDirectory).filePath("offline-placeholder.txt");
        const auto offlineCopy = filemanager::platform::copyFileAtomically(
            offlineSource, offlineDestination, false);
        const auto offlineStatus = filemanager::platform::inspectCloudFileStatus(offlineSource);
        passed &= expect(filemanager::platform::nativePathKind(offlineSource)
                == filemanager::platform::NativePathKind::CloudPlaceholder
                && !offlineCopy.ok && !QFileInfo::exists(offlineDestination)
                && offlineStatus.inspected && offlineStatus.providerBacked
                && filemanager::platform::cloudFileStatusText(offlineStatus)
                    .contains("Sync state unavailable"),
            "Offline provider items should expose conservative status without hydration or output.");
        SetFileAttributesW(reinterpret_cast<LPCWSTR>(offlineSource.utf16()), offlineOriginal);
    }

    const QString sparseSource = QDir(transferSourceDirectory).filePath("sparse-source.bin");
    const QString sparseDestination =
        QDir(transferDestinationDirectory).filePath("sparse-copy.bin");
    const QString nativeSparseSource = filemanager::platform::extendedLengthPath(sparseSource);
    const HANDLE sparseHandle = CreateFileW(
        reinterpret_cast<LPCWSTR>(nativeSparseSource.utf16()), GENERIC_READ | GENERIC_WRITE,
        FILE_SHARE_READ, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
    DWORD sparseBytes = 0;
    bool sparseCreated = sparseHandle != INVALID_HANDLE_VALUE
        && DeviceIoControl(sparseHandle, FSCTL_SET_SPARSE, nullptr, 0, nullptr, 0,
            &sparseBytes, nullptr) != FALSE;
    if (sparseCreated) {
        LARGE_INTEGER sparseEnd{};
        sparseEnd.QuadPart = 64LL * 1024 * 1024;
        DWORD written = 0;
        char marker = 's';
        sparseCreated = SetFilePointerEx(sparseHandle, sparseEnd, nullptr, FILE_BEGIN) != FALSE
            && WriteFile(sparseHandle, &marker, 1, &written, nullptr) != FALSE && written == 1;
    }
    if (sparseHandle != INVALID_HANDLE_VALUE)
        CloseHandle(sparseHandle);
    passed &= expect(sparseCreated, "A real NTFS sparse-file fixture should be created.");
    if (sparseCreated) {
        const auto sparseCopy = filemanager::platform::copyFileAtomically(
            sparseSource, sparseDestination, false);
        if (!sparseCopy.ok)
            std::cerr << "Sparse copy error: " << sparseCopy.error.toStdString() << '\n';
        const QString nativeSparseDestination =
            filemanager::platform::extendedLengthPath(sparseDestination);
        const DWORD sparseDestinationAttributes = GetFileAttributesW(
            reinterpret_cast<LPCWSTR>(nativeSparseDestination.utf16()));
        DWORD compressedHigh = 0;
        const DWORD compressedLow = GetCompressedFileSizeW(
            reinterpret_cast<LPCWSTR>(nativeSparseDestination.utf16()), &compressedHigh);
        const quint64 allocatedBytes = (static_cast<quint64>(compressedHigh) << 32)
            | compressedLow;
        passed &= expect(sparseCopy.ok, "Native atomic sparse copy should succeed.");
        passed &= expect((sparseDestinationAttributes & FILE_ATTRIBUTE_SPARSE_FILE) != 0,
            "Native atomic copy preserves the sparse-file attribute.");
        passed &= expect(QFileInfo(sparseDestination).size() == 64LL * 1024 * 1024 + 1,
            "Native atomic copy preserves sparse-file logical size.");
        passed &= expect(allocatedBytes
                < static_cast<quint64>(QFileInfo(sparseDestination).size()),
            "Native atomic copy preserves sparse allocation instead of materializing holes.");
    }

    QString longSourceDirectory = transferRoot.filePath("long-source");
    QString longDestinationDirectory = transferRoot.filePath("long-destination");
    for (int depth = 0; depth < 8; ++depth) {
        const QString component = QStringLiteral("segment-%1-").arg(depth)
            + QString(32, QChar('x'));
        longSourceDirectory = QDir(longSourceDirectory).filePath(component);
        longDestinationDirectory = QDir(longDestinationDirectory).filePath(component);
    }
    passed &= expect(longSourceDirectory.size() > 260
            && QDir().mkpath(longSourceDirectory) && QDir().mkpath(longDestinationDirectory),
        "The long-path-aware test process should create paths beyond MAX_PATH.");
    const QString longSource = QDir(longSourceDirectory).filePath("long-payload.txt");
    const QString longDestination =
        QDir(longDestinationDirectory).filePath("long-payload.txt");
    QFile longFixture(longSource);
    passed &= expect(longFixture.open(QIODevice::WriteOnly)
            && longFixture.write("long path payload") == 17,
        "A file beyond MAX_PATH should be writable.");
    longFixture.close();
    const auto longCopy = filemanager::platform::copyFileAtomically(
        longSource, longDestination, false);
    QFile longCopied(longDestination);
    passed &= expect(longCopy.ok && longCopied.open(QIODevice::ReadOnly)
            && longCopied.readAll() == QByteArray("long path payload"),
        "Native atomic copy should support extended-length source and destination paths.");
    longCopied.close();

    const QString lockedSource = QDir(transferSourceDirectory).filePath("locked-source.txt");
    const QString lockedDestination =
        QDir(transferDestinationDirectory).filePath("locked-copy.txt");
    QFile lockedFixture(lockedSource);
    passed &= expect(lockedFixture.open(QIODevice::WriteOnly)
            && lockedFixture.write("locked") == 6,
        "Locked-source fixture should be created.");
    lockedFixture.close();
    const QString nativeLockedSource = filemanager::platform::extendedLengthPath(lockedSource);
    const HANDLE exclusiveHandle = CreateFileW(
        reinterpret_cast<LPCWSTR>(nativeLockedSource.utf16()), GENERIC_READ, 0, nullptr,
        OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    passed &= expect(exclusiveHandle != INVALID_HANDLE_VALUE,
        "Locked-source fixture should be held without sharing.");
    const auto lockedCopy = filemanager::platform::copyFileAtomically(
        lockedSource, lockedDestination, false);
    if (exclusiveHandle != INVALID_HANDLE_VALUE)
        CloseHandle(exclusiveHandle);
    passed &= expect(!lockedCopy.ok && lockedCopy.nativeCode == ERROR_SHARING_VIOLATION
            && !lockedCopy.error.isEmpty() && !QFileInfo::exists(lockedDestination),
        "Locked sources return the exact Windows sharing failure and commit no destination.");
#endif

    phase("analysis_begin");
    filemanager::files::AnalysisService analysis;
    QEventLoop analysisLoop;
    filemanager::files::AnalysisResult analysisResult;
    QObject::connect(&analysis, &filemanager::files::AnalysisService::finished,
        [&](quint64, const auto& result) {
            analysisResult = result;
            analysisLoop.quit();
        });
    QTimer::singleShot(5000, &analysisLoop, &QEventLoop::quit);
    analysis.start(copiedPayload);
    analysisLoop.exec();
    passed &= expect(analysisResult.statistics.files == 1
            && analysisResult.statistics.error.isEmpty() && analysisResult.sha256.size() == 64,
        "Background analysis returns file size metadata and SHA-256.");

    filemanager::files::AnalysisService::clearFolderCache();
    QTemporaryDir folderAnalysisRoot;
    passed &= expect(folderAnalysisRoot.isValid(), "The folder-analysis cache fixture should open.");

    QTemporaryDir largeEnumerationRoot;
    passed &= expect(largeEnumerationRoot.isValid(),
        "The streaming large-folder fixture should open.");
    for (int item = 0; item < 2048; ++item) {
        QFile fixture(largeEnumerationRoot.filePath(QString("entry-%1.tmp").arg(item)));
        if (!fixture.open(QIODevice::WriteOnly)) {
            passed &= expect(false, "The streaming large-folder fixture should be populated.");
            break;
        }
    }
    std::atomic_bool stopLargeEnumeration{false};
    int visitedLargeEntries = 0;
    const auto cancelledLargeEnumeration = filemanager::files::enumerateRegularTree(
        largeEnumerationRoot.path(), false, &stopLargeEnumeration,
        [&](const QFileInfo&) {
            ++visitedLargeEntries;
            if (visitedLargeEntries == 256)
                stopLargeEnumeration.store(true);
            return true;
        });
    passed &= expect(cancelledLargeEnumeration.cancelled
            && cancelledLargeEnumeration.error.isEmpty() && visitedLargeEntries == 256,
        "Large-folder enumeration should stream entries and honor cancellation without reading the remainder.");

    {
        filemanager::ui::LocalDirectoryModel publishedModel;
        bool loaded = false;
        bool contiguousRanges = true;
        int deliveredRows = 0;
        int publications = 0;
        QObject::connect(&publishedModel, &QAbstractItemModel::rowsInserted, &publishedModel,
            [&](const QModelIndex& parent, int first, int last) {
                contiguousRanges &= !parent.isValid() && first == deliveredRows && last >= first
                    && publishedModel.rowCount() == last + 1;
                deliveredRows = last + 1;
                ++publications;
            });
        QObject::connect(&publishedModel, &filemanager::ui::LocalDirectoryModel::directoryLoaded,
            &publishedModel, [&](const QString& path) { loaded = path == largeEnumerationRoot.path(); });
        publishedModel.setDirectory(largeEnumerationRoot.path());
        QElapsedTimer publicationWait;
        publicationWait.start();
        while (!loaded && publicationWait.elapsed() < 5000) {
            QApplication::processEvents(QEventLoop::AllEvents, 25);
            QThread::msleep(5);
        }
        QSet<QString> publishedPaths;
        for (int row = 0; row < publishedModel.rowCount(); ++row)
            publishedPaths.insert(publishedModel.filePath(publishedModel.index(row, 0)));
        passed &= expect(loaded && !publishedModel.loading() && contiguousRanges
                && publications > 1 && deliveredRows == 2048 && publishedPaths.size() == 2048,
            "Incremental folder publication must deliver contiguous ranges and every unique row exactly once.");
    }
    {
        filemanager::ui::LocalDirectoryModel interruptedModel;
        QTemporaryDir replacementFolder;
        const QString replacementPath = replacementFolder.filePath("replacement.txt");
        QFile replacementFile(replacementPath);
        passed &= expect(replacementFolder.isValid() && replacementFile.open(QIODevice::WriteOnly),
            "The publication-replacement fixture should be created.");
        replacementFile.close();
        bool replacementQueued = false;
        bool replacementStarted = false;
        bool partialObserved = false;
        bool targetLoaded = false;
        bool obsoleteCompletion = false;
        QPersistentModelIndex oldIndex;
        QObject::connect(&interruptedModel, &QAbstractItemModel::rowsInserted, &interruptedModel,
            [&](const QModelIndex&, int, int) {
                if (replacementQueued || interruptedModel.directory() != largeEnumerationRoot.path())
                    return;
                replacementQueued = true;
                partialObserved = interruptedModel.rowCount() > 0 && interruptedModel.rowCount() < 2048;
                oldIndex = interruptedModel.index(0, 0);
                // Replace between publication callbacks, as an input event would, not inside
                // the model's begin/end insertion transaction.
                QTimer::singleShot(0, &interruptedModel, [&] {
                    replacementStarted = true;
                    interruptedModel.setDirectory(replacementFolder.path());
                });
            });
        QObject::connect(&interruptedModel, &filemanager::ui::LocalDirectoryModel::directoryLoaded,
            &interruptedModel, [&](const QString& path) {
                // A queued navigation has not happened until its callback runs.
                // Completion before that callback is legitimate; completion after
                // the model changes directory must be rejected.
                obsoleteCompletion |= replacementStarted && path == largeEnumerationRoot.path();
                targetLoaded |= path == replacementFolder.path();
            });
        interruptedModel.setDirectory(largeEnumerationRoot.path());
        QElapsedTimer replacementWait;
        replacementWait.start();
        while (!targetLoaded && replacementWait.elapsed() < 5000) {
            QApplication::processEvents(QEventLoop::AllEvents, 25);
            QThread::msleep(5);
        }
        QEventLoop publicationDrain;
        QTimer::singleShot(50, &publicationDrain, &QEventLoop::quit);
        publicationDrain.exec();
        // A directory watcher may start a second refresh after the replacement
        // folder first reports loaded. Wait for that bounded refresh to settle;
        // the invariants below still reject stale rows, indexes and completions.
        QElapsedTimer settleWait;
        settleWait.start();
        while (interruptedModel.loading() && settleWait.elapsed() < 5000) {
            QApplication::processEvents(QEventLoop::AllEvents, 25);
            QThread::msleep(5);
        }
        const bool replacementCorrect = replacementStarted && partialObserved && targetLoaded
                && !obsoleteCompletion && !oldIndex.isValid()
                && !interruptedModel.loading() && interruptedModel.rowCount() == 1
                && interruptedModel.filePath(interruptedModel.index(0, 0)) == replacementPath;
        if (!replacementCorrect)
            std::fprintf(stderr, "Publication replacement: started=%d partial=%d loaded=%d "
                "obsolete_completion=%d old_index_valid=%d loading=%d rows=%d path=%s expected=%s\n",
                replacementStarted, partialObserved, targetLoaded, obsoleteCompletion, oldIndex.isValid(),
                interruptedModel.loading(), interruptedModel.rowCount(),
                qPrintable(interruptedModel.filePath(interruptedModel.index(0, 0))), qPrintable(replacementPath));
        passed &= expect(replacementCorrect,
            "Navigation after partial publication must invalidate old indexes and publish only the replacement folder.");
    }

    {
        // Replacing an expanded directory at an unchanged path must retire its
        // descendant indexes and watches, then allow a later directory to load.
        QTemporaryDir replacementRoot;
        const QString listingPath = replacementRoot.filePath("listing");
        const QString itemPath = listingPath + "/item";
        const QString nestedPath = itemPath + "/nested";
        const QString oldLeafPath = nestedPath + "/old.txt";
        bool fixtureReady = replacementRoot.isValid() && QDir().mkpath(nestedPath);
        QFile oldLeaf(oldLeafPath);
        fixtureReady = fixtureReady && oldLeaf.open(QIODevice::WriteOnly)
            && oldLeaf.write("original") == 8;
        oldLeaf.close();
        passed &= expect(fixtureReady, "The directory-type replacement fixture should be created.");
        if (fixtureReady) {
            filemanager::ui::LocalDirectoryModel model;
            // Explicit refresh drives this fixture, independent of watcher delivery order.
            auto* watcher = model.findChild<filemanager::platform::DirectoryWatcher*>();
            passed &= expect(watcher != nullptr, "The local model should own its directory watcher.");
            if (watcher)
                watcher->blockSignals(true);
            const auto waitForModel = [&](const auto& ready) {
                QElapsedTimer deadline;
                deadline.start();
                while (!ready() && deadline.elapsed() < 5000) {
                    QApplication::processEvents(QEventLoop::AllEvents, 25);
                    QThread::msleep(5);
                }
                return ready();
            };
            model.setDirectory(listingPath);
            bool rootReady = waitForModel([&] { return !model.loading() && model.rowCount() == 1; });
            passed &= expect(rootReady, "The replacement fixture root should load.");
            if (rootReady) {
                QPersistentModelIndex oldDirectory = model.indexForPath(itemPath);
                model.fetchMore(oldDirectory);
                const bool nestedReady = waitForModel([&] { return model.indexForPath(nestedPath).isValid(); });
                passed &= expect(nestedReady, "The old directory's child should load.");
                QPersistentModelIndex oldNested = model.indexForPath(nestedPath);
                model.fetchMore(oldNested);
                const bool leafReady = waitForModel([&] { return model.indexForPath(oldLeafPath).isValid(); });
                passed &= expect(leafReady, "The expanded old descendant should load.");
                QPersistentModelIndex oldLeafIndex = model.indexForPath(oldLeafPath);

                // Preserve the payload, then remove the empty watched hierarchy.
                // Keep model signals blocked so stale rows survive until explicit refresh.
                // Shell registration retirement belongs to the later model refresh.
                const bool parked = QFile::rename(oldLeafPath, replacementRoot.filePath("parked-old.txt"));
                const bool nestedRemoved = parked && QDir().rmdir(nestedPath);
                
                const bool directoryRemoved = nestedRemoved && QDir().rmdir(itemPath);
                
                passed &= expect(parked && nestedRemoved
                        && directoryRemoved,
                    "The old payload should be parked and the emptied watched hierarchy removed.");
                QFile replacementFile(itemPath);
                const bool opened = directoryRemoved && replacementFile.open(QIODevice::WriteOnly);
                const bool replaced = opened && replacementFile.write("replacement") == 11;
                if (!replaced)
                    std::fprintf(stderr, "Directory replacement fixture: parked=%d nested_removed=%d "
                        "directory_removed=%d "
                        "opened=%d file_error=%d\n", parked, nestedRemoved,
                        directoryRemoved, opened, static_cast<int>(replacementFile.error()));
                replacementFile.close();
                passed &= expect(replaced, "A regular file should replace the parked directory path.");
                if (replaced) {
                    model.refresh();
                    const bool refreshed = waitForModel([&] { return !model.loading(); });
                    const QModelIndex fileIndex = model.indexForPath(itemPath);
                    passed &= expect(refreshed && fileIndex.isValid() && model.fileInfo(fileIndex).isFile()
                            && !oldDirectory.isValid() && !oldNested.isValid() && !oldLeafIndex.isValid()
                            && model.rowCount(fileIndex) == 0 && !model.hasChildren(fileIndex)
                            && !model.canFetchMore(fileIndex) && !model.indexForPath(oldLeafPath).isValid(),
                        "Directory-to-file refresh must invalidate the old subtree and expose no phantom children.");
                    if (watcher) {
                        passed &= expect(!watcher->directories().contains(itemPath)
                                && !watcher->directories().contains(nestedPath),
                            "Retired directory and descendant watches must not follow a replacement file.");
                    }
                    QPersistentModelIndex oldFileIndex = fileIndex;
                    const QString freshLeafPath = itemPath + "/fresh.txt";
                    bool restored = QFile::rename(itemPath, replacementRoot.filePath("parked-file"))
                        && QDir().mkpath(itemPath);
                    QFile freshLeaf(freshLeafPath);
                    restored = restored && freshLeaf.open(QIODevice::WriteOnly)
                        && freshLeaf.write("fresh") == 5;
                    freshLeaf.close();
                    passed &= expect(restored, "A fresh directory should replace the parked regular file.");
                    if (restored) {
                        model.refresh();
                        const bool refreshedDirectory = waitForModel([&] { return !model.loading(); });
                        const QModelIndex directoryIndex = model.indexForPath(itemPath);
                        passed &= expect(refreshedDirectory && directoryIndex.isValid()
                                && model.fileInfo(directoryIndex).isDir() && !oldFileIndex.isValid()
                                && model.canFetchMore(directoryIndex),
                            "File-to-directory refresh must create a new, fetchable tree item.");
                        model.fetchMore(directoryIndex);
                        const bool freshReady = waitForModel([&] { return model.indexForPath(freshLeafPath).isValid(); });
                        passed &= expect(freshReady && model.rowCount(directoryIndex) == 1
                                && !model.indexForPath(oldLeafPath).isValid(),
                            "The replacement directory must publish only its fresh contents.");
                    }
                }
            }
        }
    }

    for (int item = 0; item < 130; ++item) {
        QFile fixture(folderAnalysisRoot.filePath(QString("item-%1.bin").arg(item)));
        passed &= expect(fixture.open(QIODevice::WriteOnly) && fixture.write("1234567890") == 10,
            "A folder-analysis progress fixture should be created.");
        fixture.close();
    }
    filemanager::files::AnalysisService firstFolderAnalysis;
    QEventLoop firstFolderAnalysisLoop;
    filemanager::files::AnalysisResult firstFolderResult;
    int folderProgressUpdates = 0;
    QObject::connect(&firstFolderAnalysis, &filemanager::files::AnalysisService::progress,
        [&](quint64, const QString&, const auto&) { ++folderProgressUpdates; });
    QObject::connect(&firstFolderAnalysis, &filemanager::files::AnalysisService::finished,
        [&](quint64, const auto& result) {
            firstFolderResult = result;
            firstFolderAnalysisLoop.quit();
        });
    QTimer::singleShot(5000, &firstFolderAnalysisLoop, &QEventLoop::quit);
    firstFolderAnalysis.start(folderAnalysisRoot.path(), true);
    firstFolderAnalysisLoop.exec();
    passed &= expect(!firstFolderResult.fromCache
            && firstFolderResult.statistics.files == 130
            && firstFolderResult.statistics.bytes == 1300
            && folderProgressUpdates >= 3
            && filemanager::files::AnalysisService::folderCacheSize() == 1,
        "A refreshed folder analysis should publish visible progress and populate the bounded cache.");

    filemanager::files::AnalysisService cachedFolderAnalysis;
    QEventLoop cachedFolderAnalysisLoop;
    filemanager::files::AnalysisResult cachedFolderResult;
    QObject::connect(&cachedFolderAnalysis, &filemanager::files::AnalysisService::finished,
        [&](quint64, const auto& result) {
            cachedFolderResult = result;
            cachedFolderAnalysisLoop.quit();
        });
    QTimer::singleShot(5000, &cachedFolderAnalysisLoop, &QEventLoop::quit);
    cachedFolderAnalysis.start(folderAnalysisRoot.path());
    cachedFolderAnalysisLoop.exec();
    passed &= expect(cachedFolderResult.fromCache
            && cachedFolderResult.statistics == firstFolderResult.statistics,
        "A repeated folder analysis should return the disclosed memory-only cached measurement.");

    filemanager::files::AnalysisService cancelledFolderAnalysis;
    QEventLoop cancelledFolderAnalysisLoop;
    filemanager::files::AnalysisResult cancelledFolderResult;
    QObject::connect(&cancelledFolderAnalysis, &filemanager::files::AnalysisService::finished,
        [&](quint64, const auto& result) {
            cancelledFolderResult = result;
            cancelledFolderAnalysisLoop.quit();
        });
    QTimer::singleShot(5000, &cancelledFolderAnalysisLoop, &QEventLoop::quit);
    cancelledFolderAnalysis.start(folderAnalysisRoot.path(), true);
    cancelledFolderAnalysis.cancel();
    cancelledFolderAnalysisLoop.exec();
    passed &= expect(cancelledFolderResult.statistics.cancelled,
        "Folder-size refresh cancellation should stop traversal and never replace the cache.");
    filemanager::files::AnalysisService::clearFolderCache(folderAnalysisRoot.path());
    passed &= expect(filemanager::files::AnalysisService::folderCacheSize() == 0,
        "Folder-size cache entries should support immediate explicit invalidation.");

    QTemporaryDir duplicateRoot;
    const QString duplicateLeftDirectory = duplicateRoot.filePath("left");
    const QString duplicateRightDirectory = duplicateRoot.filePath("right");
    passed &= expect(duplicateRoot.isValid()
            && QDir().mkpath(duplicateLeftDirectory) && QDir().mkpath(duplicateRightDirectory),
        "The isolated duplicate-scan fixture should open.");
    const QString duplicateLeftPath = QDir(duplicateLeftDirectory).filePath("duplicate.txt");
    const QString duplicateRightPath = QDir(duplicateRightDirectory).filePath("duplicate.txt");
    QFile duplicateLeft(duplicateLeftPath);
    QFile duplicateRight(duplicateRightPath);
    passed &= expect(duplicateLeft.open(QIODevice::WriteOnly)
            && duplicateLeft.write("duplicate payload") == 17
            && duplicateRight.open(QIODevice::WriteOnly)
            && duplicateRight.write("duplicate payload") == 17,
        "The isolated duplicate-scan files should be created.");
    duplicateLeft.close();
    duplicateRight.close();
    filemanager::files::DuplicateService duplicateService;
    QEventLoop duplicateLoop;
    filemanager::files::DuplicateScanResult duplicateResult;
    QObject::connect(&duplicateService, &filemanager::files::DuplicateService::finished,
        [&](quint64, const auto& result) {
            duplicateResult = result;
            duplicateLoop.quit();
        });
    QTimer::singleShot(5000, &duplicateLoop, &QEventLoop::quit);
    duplicateService.start(duplicateRoot.path());
    duplicateLoop.exec();
    const bool foundPayloadDuplicates = std::any_of(duplicateResult.groups.cbegin(),
        duplicateResult.groups.cend(), [&](const auto& group) {
            return group.paths.contains(duplicateLeftPath)
                && group.paths.contains(duplicateRightPath);
        });
    passed &= expect(!duplicateResult.cancelled && foundPayloadDuplicates,
        "Background duplicate service verifies identical files in separate folders.");

#ifdef Q_OS_WIN
    const QString metadataFolderSource = QDir(transferSourceDirectory).filePath("metadata-folder");
    QDir().mkpath(metadataFolderSource);
    QFile folderChild(QDir(metadataFolderSource).filePath("child.txt"));
    passed &= expect(folderChild.open(QIODevice::WriteOnly) && folderChild.write("child") == 5,
        "Folder metadata fixture should contain a child.");
    folderChild.close();
    const QDateTime folderTime = QDateTime::currentDateTimeUtc().addDays(-4);
    const quint64 folderTimeTicks = static_cast<quint64>(
        folderTime.toMSecsSinceEpoch() + 11'644'473'600'000LL) * 10'000ULL;
    const FILETIME folderFileTime{static_cast<DWORD>(folderTimeTicks),
        static_cast<DWORD>(folderTimeTicks >> 32)};
    const HANDLE metadataFolderHandle = CreateFileW(
        reinterpret_cast<LPCWSTR>(metadataFolderSource.utf16()), FILE_WRITE_ATTRIBUTES,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
        FILE_FLAG_BACKUP_SEMANTICS, nullptr);
    const bool folderTimeSet = metadataFolderHandle != INVALID_HANDLE_VALUE
        && SetFileTime(metadataFolderHandle, nullptr, nullptr, &folderFileTime) != FALSE;
    if (metadataFolderHandle != INVALID_HANDLE_VALUE)
        CloseHandle(metadataFolderHandle);
    passed &= expect(folderTimeSet,
        "Folder metadata fixture should receive a stable modification time.");
    const DWORD folderOriginalAttributes = GetFileAttributesW(
        reinterpret_cast<LPCWSTR>(metadataFolderSource.utf16()));
    passed &= expect(folderOriginalAttributes != INVALID_FILE_ATTRIBUTES
            && SetFileAttributesW(reinterpret_cast<LPCWSTR>(metadataFolderSource.utf16()),
                folderOriginalAttributes | FILE_ATTRIBUTE_HIDDEN),
        "Folder metadata fixture should receive a preserved attribute.");
    transferErrors.clear();
    beginTransfer({{metadataFolderSource}, transferDestinationDirectory,
        filemanager::files::TransferOperation::Copy});
    waitForTransfer();
    const QString copiedMetadataFolder =
        QDir(transferDestinationDirectory).filePath("metadata-folder");
    const DWORD copiedFolderAttributes = GetFileAttributesW(
        reinterpret_cast<LPCWSTR>(copiedMetadataFolder.utf16()));
    passed &= expect(transferCompleted && transferErrors.isEmpty()
            && QFileInfo(copiedMetadataFolder).lastModified().toMSecsSinceEpoch()
                == QFileInfo(metadataFolderSource).lastModified().toMSecsSinceEpoch()
            && (copiedFolderAttributes & FILE_ATTRIBUTE_HIDDEN) != 0,
        "New folder copies restore source timestamps and supported attributes after children.");

    const QString symbolicLinkSource = QDir(transferSourceDirectory).filePath("payload-link.txt");
    const bool symbolicLinkCreated = CreateSymbolicLinkW(
        reinterpret_cast<LPCWSTR>(symbolicLinkSource.utf16()),
        reinterpret_cast<LPCWSTR>(transferFixture.fileName().utf16()), 0) != FALSE;
    if (symbolicLinkCreated) {
        const QString symbolicLinkDestination =
            QDir(transferDestinationDirectory).filePath("payload-link.txt");
        const auto symbolicLinkCopy = filemanager::platform::copyFileAtomically(
            symbolicLinkSource, symbolicLinkDestination, false);
        passed &= expect(symbolicLinkCopy.ok
                && filemanager::platform::nativePathKind(symbolicLinkDestination)
                    == filemanager::platform::NativePathKind::SymbolicLink,
            "Supported symbolic links are copied as links instead of traversing their targets.");
        const QString movedLinkSource =
            QDir(transferSourceDirectory).filePath("moved-payload-link.txt");
        const bool movedLinkCreated = CreateSymbolicLinkW(
            reinterpret_cast<LPCWSTR>(movedLinkSource.utf16()),
            reinterpret_cast<LPCWSTR>(transferFixture.fileName().utf16()), 0) != FALSE;
        passed &= expect(movedLinkCreated,
            "A second symbolic-link fixture should be available for no-follow move cleanup.");
        if (movedLinkCreated) {
            transferErrors.clear();
            beginTransfer({{movedLinkSource}, transferDestinationDirectory,
                filemanager::files::TransferOperation::Move});
            waitForTransfer();
            const QString movedLinkDestination =
                QDir(transferDestinationDirectory).filePath("moved-payload-link.txt");
            passed &= expect(transferCompleted && transferErrors.isEmpty()
                    && filemanager::platform::nativePathKind(movedLinkSource)
                        == filemanager::platform::NativePathKind::Missing
                    && filemanager::platform::nativePathKind(movedLinkDestination)
                        == filemanager::platform::NativePathKind::SymbolicLink
                    && QFileInfo::exists(transferFixture.fileName()),
                "Moving a symbolic link removes only the link and never its target.");
        }
    } else {
        qInfo("Symbolic-link preservation fixture skipped: Windows denied link creation.");
    }
#endif

    bool promptedForConflict = false;
    bool applyConflictToAll = false;
    int conflictPromptCount = 0;
    auto conflictDecision = filemanager::files::ConflictDecision::KeepBoth;
    QObject::connect(&transfer, &filemanager::files::TransferController::conflict,
        [&](quint64 generation, const QString&, const QString&, bool) {
            promptedForConflict = true;
            ++conflictPromptCount;
            transfer.resolveConflict(generation, conflictDecision, applyConflictToAll);
        });
    transferErrors.clear();
    beginTransfer({{transferFixture.fileName()}, transferDestinationDirectory,
        filemanager::files::TransferOperation::Copy});
    waitForTransfer();
    passed &= expect(transferCompleted && promptedForConflict,
        "An existing destination always raises a conflict prompt.");
    passed &= expect(QFileInfo::exists(QDir(transferDestinationDirectory).filePath("payload (2).txt")),
        "Keep both creates a deterministic unique destination.");

    QFile applyAllFirst(QDir(transferSourceDirectory).filePath("apply-all-first.txt"));
    QFile applyAllSecond(QDir(transferSourceDirectory).filePath("apply-all-second.txt"));
    passed &= expect(applyAllFirst.open(QIODevice::WriteOnly)
            && applyAllFirst.write("first source") == 12,
        "The first apply-to-all conflict source should be created.");
    applyAllFirst.close();
    passed &= expect(applyAllSecond.open(QIODevice::WriteOnly)
            && applyAllSecond.write("second source") == 13,
        "The second apply-to-all conflict source should be created.");
    applyAllSecond.close();
    QFile applyAllFirstDestination(
        QDir(transferDestinationDirectory).filePath("apply-all-first.txt"));
    QFile applyAllSecondDestination(
        QDir(transferDestinationDirectory).filePath("apply-all-second.txt"));
    passed &= expect(applyAllFirstDestination.open(QIODevice::WriteOnly)
            && applyAllFirstDestination.write("existing first") == 14,
        "The first apply-to-all destination conflict should be created.");
    applyAllFirstDestination.close();
    passed &= expect(applyAllSecondDestination.open(QIODevice::WriteOnly)
            && applyAllSecondDestination.write("existing second") == 15,
        "The second apply-to-all destination conflict should be created.");
    applyAllSecondDestination.close();
    conflictDecision = filemanager::files::ConflictDecision::KeepBoth;
    applyConflictToAll = true;
    conflictPromptCount = 0;
    transferErrors.clear();
    beginTransfer({{applyAllFirst.fileName(), applyAllSecond.fileName()},
        transferDestinationDirectory, filemanager::files::TransferOperation::Copy});
    waitForTransfer();
    passed &= expect(transferCompleted && transferErrors.isEmpty() && conflictPromptCount == 1
            && QFileInfo::exists(
                QDir(transferDestinationDirectory).filePath("apply-all-first (2).txt"))
            && QFileInfo::exists(
                QDir(transferDestinationDirectory).filePath("apply-all-second (2).txt")),
        "Apply-to-all should reuse one keep-both decision for every remaining job conflict.");
    applyConflictToAll = false;
    QFile updatedSource(transferFixture.fileName());
    passed &= expect(updatedSource.open(QIODevice::WriteOnly | QIODevice::Truncate),
        "Replacement source should open.");
    updatedSource.write("replacement payload");
    updatedSource.close();
    conflictDecision = filemanager::files::ConflictDecision::Replace;
    beginTransfer({{transferFixture.fileName()}, transferDestinationDirectory,
        filemanager::files::TransferOperation::Copy});
    waitForTransfer();
    QFile replacedDestination(copiedPayload);
    passed &= expect(transferCompleted && replacedDestination.open(QIODevice::ReadOnly)
            && replacedDestination.readAll() == "replacement payload",
        "Explicit replace atomically commits the new file contents.");

    transferErrors.clear();
    promptedForConflict = false;
    beginTransfer({{transferFixture.fileName()}, transferSourceDirectory,
        filemanager::files::TransferOperation::Copy});
    waitForTransfer();
    passed &= expect(transferCompleted && !promptedForConflict && !transferErrors.isEmpty()
            && transferErrors.first().contains("same path"),
        "Transfer preflight rejects a source that resolves to its destination.");

    conflictDecision = filemanager::files::ConflictDecision::Skip;
    transferErrors.clear();
    promptedForConflict = false;
    beginTransfer({{transferFixture.fileName()}, transferDestinationDirectory,
        filemanager::files::TransferOperation::Move});
    waitForTransfer();
    passed &= expect(transferCompleted && promptedForConflict && transferErrors.isEmpty()
            && QFileInfo::exists(transferFixture.fileName()),
        "Skipping a move conflict retains the source without failing the job.");

    const QString cancellationDestination = transferRoot.filePath("cancelled-destination");
    QDir().mkpath(cancellationDestination);
    QFile cancellationFixture(QDir(transferSourceDirectory).filePath("cancel-me.bin"));
    passed &= expect(cancellationFixture.open(QIODevice::WriteOnly),
        "Cancellation fixture should open.");
    cancellationFixture.write(QByteArray(8 * 1024 * 1024, 'x'));
    cancellationFixture.close();
    transferCancelled = false;
    transferErrors.clear();
    beginTransfer({{cancellationFixture.fileName()}, cancellationDestination,
        filemanager::files::TransferOperation::Copy});
    transfer.cancel();
    waitForTransfer();
    passed &= expect(transferCompleted && transferCancelled,
        "A cancellation request reaches the background worker.");
    passed &= expect(!QFileInfo::exists(QDir(cancellationDestination).filePath("cancel-me.bin")),
        "Cancellation leaves no partially committed destination file.");

    const QString queueDestination = transferRoot.filePath("queue-destination");
    QDir().mkpath(queueDestination);
    QFile queueFirst(QDir(transferSourceDirectory).filePath("queue-first.txt"));
    QFile queueSecond(QDir(transferSourceDirectory).filePath("queue-second.txt"));
    passed &= expect(queueFirst.open(QIODevice::WriteOnly), "First queue fixture should open.");
    queueFirst.write("first");
    queueFirst.close();
    passed &= expect(queueSecond.open(QIODevice::WriteOnly), "Second queue fixture should open.");
    queueSecond.write("second");
    queueSecond.close();
    filemanager::files::TransferQueue queue;
    QList<quint64> startedJobs;
    QList<quint64> finishedJobs;
    QEventLoop queueLoop;
    QObject::connect(&queue, &filemanager::files::TransferQueue::jobStarted,
        [&](quint64 id) { startedJobs.append(id); });
    QObject::connect(&queue, &filemanager::files::TransferQueue::jobFinished,
        [&](quint64 id, bool, const QStringList&) {
            finishedJobs.append(id);
            if (finishedJobs.size() == 2)
                queueLoop.quit();
        });
    const quint64 firstJob = queue.enqueue({{queueFirst.fileName()}, queueDestination,
        filemanager::files::TransferOperation::Copy});
    const quint64 secondJob = queue.enqueue({{queueSecond.fileName()}, queueDestination,
        filemanager::files::TransferOperation::Copy});
    QTimer::singleShot(5000, &queueLoop, &QEventLoop::quit);
    queueLoop.exec();
    passed &= expect(startedJobs == QList<quint64>{firstJob, secondJob}
            && finishedJobs == QList<quint64>{firstJob, secondJob},
        "Transfer queue executes jobs serially in insertion order.");
    const auto queueHistory = queue.history();
    passed &= expect(queueHistory.size() == 2
            && queueHistory.at(0).state == filemanager::files::TransferJobState::Succeeded
            && queueHistory.at(1).state == filemanager::files::TransferJobState::Succeeded
            && queueHistory.at(0).startedAt.isValid() && queueHistory.at(0).finishedAt.isValid(),
        "Transfer history records ordered lifecycle timestamps and final states.");
    passed &= expect(queueHistory.at(0).manifest.size() == 1
            && queueHistory.at(0).manifest.first().destinationCreated
            && queueHistory.at(0).manifest.first().identityAvailable
            && queueHistory.at(0).manifest.first().sourceIdentityAvailable,
        "Each newly committed transfer item records source and destination Windows identities.");

    filemanager::files::OperationJournal operationCenterJournal(
        transferRoot.filePath("operation-center-journal.dat"));
    filemanager::ui::OperationCenterDialog operationCenter(&queue, &operationCenterJournal);
    auto* operationCenterTabs =
        operationCenter.findChild<QTabWidget*>("OperationCenterTabs");
    auto* operationCenterTransfers =
        operationCenter.findChild<QTableWidget*>("OperationCenterTransfers");
    auto* operationCenterRecovery =
        operationCenter.findChild<QTableWidget*>("OperationCenterRecovery");
    auto* operationCenterSummary =
        operationCenter.findChild<QLabel*>("OperationCenterSummary");
    auto* operationCenterPause =
        operationCenter.findChild<QPushButton*>("OperationCenterPause");
    auto* operationCenterResume =
        operationCenter.findChild<QPushButton*>("OperationCenterResume");
    auto* operationCenterCancel =
        operationCenter.findChild<QPushButton*>("OperationCenterCancel");
    auto* operationCenterRetry =
        operationCenter.findChild<QPushButton*>("OperationCenterRetry");
    auto* operationCenterReview =
        operationCenter.findChild<QPushButton*>("OperationCenterReviewRecovery");
    passed &= expect(operationCenterTabs && operationCenterTransfers
            && operationCenterRecovery && operationCenterSummary
            && operationCenterPause && operationCenterResume && operationCenterCancel
            && operationCenterRetry && operationCenterReview,
        "The extracted Operation Center exposes stable presentation and action contracts.");
    if (operationCenterTabs && operationCenterTransfers && operationCenterRecovery
            && operationCenterSummary && operationCenterPause && operationCenterResume
            && operationCenterCancel && operationCenterRetry && operationCenterReview) {
        passed &= expect(operationCenterTabs->count() == 2
                && operationCenterTabs->tabText(1) == "Recoverable (0)"
                && operationCenterTransfers->rowCount() == queueHistory.size()
                && operationCenterRecovery->rowCount() == 0
                && operationCenterSummary->text().contains("Completed: 2"),
            "The Operation Center presents transfer history and empty recovery state.");
        operationCenterTransfers->selectRow(0);
        QCoreApplication::processEvents();
        passed &= expect(!operationCenterPause->isEnabled()
                && !operationCenterResume->isEnabled()
                && !operationCenterCancel->isEnabled()
                && !operationCenterRetry->isEnabled()
                && !operationCenterReview->isEnabled(),
            "Completed transfers and empty recovery state expose no unsafe actions.");
    }

    QEventLoop unavailableDestinationLoop;
    QTimer unavailableDestinationTimeout;
    unavailableDestinationTimeout.setSingleShot(true);
    quint64 unavailableDestinationJob = 0;
    QObject::connect(&queue, &filemanager::files::TransferQueue::jobFinished,
        &unavailableDestinationLoop, [&](quint64 id, bool, const QStringList&) {
            if (id == unavailableDestinationJob) {
                unavailableDestinationTimeout.stop();
                unavailableDestinationLoop.quit();
            }
        });
    QObject::connect(&unavailableDestinationTimeout, &QTimer::timeout,
        &unavailableDestinationLoop, &QEventLoop::quit);
    const QString unavailableDestination =
        transferRoot.filePath("disconnected-destination/missing");
    unavailableDestinationJob = queue.enqueue({{queueFirst.fileName(), queueSecond.fileName()},
        unavailableDestination, filemanager::files::TransferOperation::Copy});
    unavailableDestinationTimeout.start(5000);
    unavailableDestinationLoop.exec();
    const auto unavailableDestinationHistory = queue.history();
    const auto unavailableDestinationRecord = std::find_if(
        unavailableDestinationHistory.cbegin(), unavailableDestinationHistory.cend(),
        [unavailableDestinationJob](const auto& record) {
            return record.id == unavailableDestinationJob;
        });
    passed &= expect(unavailableDestinationRecord != unavailableDestinationHistory.cend()
            && unavailableDestinationRecord->state
                == filemanager::files::TransferJobState::Failed
            && unavailableDestinationRecord->failures.size() == 2
            && std::all_of(unavailableDestinationRecord->failures.cbegin(),
                unavailableDestinationRecord->failures.cend(), [](const auto& failure) {
                    return failure.retryable
                        && failure.category
                            == filemanager::files::TransferFailureCategory::DestinationUnavailable;
                })
            && !QFileInfo::exists(unavailableDestination),
        "An unavailable destination records one retryable typed failure per source without mutation.");

    const QString isolatedFailureDestination =
        transferRoot.filePath("isolated-failure-destination");
    QDir().mkpath(isolatedFailureDestination);
    const QString initiallyMissingSource =
        QDir(transferSourceDirectory).filePath("initially-missing.txt");
    const QString independentSource =
        QDir(transferSourceDirectory).filePath("independent-source.txt");
    QFile independentFixture(independentSource);
    passed &= expect(independentFixture.open(QIODevice::WriteOnly)
            && independentFixture.write("independent") == 11,
        "The independent transfer fixture should be created.");
    independentFixture.close();
    QEventLoop isolatedFailureLoop;
    QTimer isolatedFailureTimeout;
    isolatedFailureTimeout.setSingleShot(true);
    quint64 isolatedFailureJob = 0;
    QObject::connect(&queue, &filemanager::files::TransferQueue::jobFinished,
        &isolatedFailureLoop, [&](quint64 id, bool, const QStringList&) {
            if (id == isolatedFailureJob) {
                isolatedFailureTimeout.stop();
                isolatedFailureLoop.quit();
            }
        });
    QObject::connect(&isolatedFailureTimeout, &QTimer::timeout,
        &isolatedFailureLoop, &QEventLoop::quit);
    isolatedFailureJob = queue.enqueue({{initiallyMissingSource, independentSource},
        isolatedFailureDestination, filemanager::files::TransferOperation::Copy});
    isolatedFailureTimeout.start(5000);
    isolatedFailureLoop.exec();
    const auto isolatedFailureHistory = queue.history();
    const auto isolatedFailureRecord = std::find_if(
        isolatedFailureHistory.cbegin(), isolatedFailureHistory.cend(),
        [isolatedFailureJob](const auto& record) { return record.id == isolatedFailureJob; });
    passed &= expect(isolatedFailureRecord != isolatedFailureHistory.cend()
            && isolatedFailureRecord->state == filemanager::files::TransferJobState::Failed
            && isolatedFailureRecord->failures.size() == 1
            && isolatedFailureRecord->failures.first().source == initiallyMissingSource
            && isolatedFailureRecord->failures.first().retryable
            && isolatedFailureRecord->failures.first().category
                == filemanager::files::TransferFailureCategory::SourceUnavailable
            && QFileInfo::exists(
                QDir(isolatedFailureDestination).filePath("independent-source.txt")),
        "One failed root is recorded without preventing later independent roots from copying.");
    passed &= expect(!queue.retryFailed(isolatedFailureJob).has_value(),
        "A failed root is not retryable while its source remains unavailable.");

    QFile restoredMissingFixture(initiallyMissingSource);
    passed &= expect(restoredMissingFixture.open(QIODevice::WriteOnly)
            && restoredMissingFixture.write("restored") == 8,
        "The failed source should become available for retry.");
    restoredMissingFixture.close();
    QEventLoop retryFailedLoop;
    QTimer retryFailedTimeout;
    retryFailedTimeout.setSingleShot(true);
    quint64 retryFailedJob = 0;
    QObject::connect(&queue, &filemanager::files::TransferQueue::jobFinished,
        &retryFailedLoop, [&](quint64 id, bool, const QStringList&) {
            if (id == retryFailedJob) {
                retryFailedTimeout.stop();
                retryFailedLoop.quit();
            }
        });
    QObject::connect(&retryFailedTimeout, &QTimer::timeout,
        &retryFailedLoop, &QEventLoop::quit);
    const auto retryFailedRequest = queue.retryFailed(isolatedFailureJob);
    retryFailedJob = retryFailedRequest.value_or(0);
    retryFailedTimeout.start(5000);
    if (retryFailedJob != 0)
        retryFailedLoop.exec();
    const auto retryFailedHistory = queue.history();
    const auto retryFailedRecord = std::find_if(
        retryFailedHistory.cbegin(), retryFailedHistory.cend(),
        [retryFailedJob](const auto& record) { return record.id == retryFailedJob; });
    passed &= expect(retryFailedRequest.has_value()
            && retryFailedRecord != retryFailedHistory.cend()
            && retryFailedRecord->request.sources == QStringList{initiallyMissingSource}
            && retryFailedRecord->state == filemanager::files::TransferJobState::Succeeded
            && QFileInfo::exists(
                QDir(isolatedFailureDestination).filePath("initially-missing.txt")),
        "Retry queues only the now-available failed root through the normal transfer path.");

    const QString undoMoveSource = QDir(transferSourceDirectory).filePath("undo-move.txt");
    QFile undoMoveFixture(undoMoveSource);
    passed &= expect(undoMoveFixture.open(QIODevice::WriteOnly)
            && undoMoveFixture.write("undo move") == 9,
        "The move-undo transfer fixture should be created.");
    undoMoveFixture.close();
    const QString undoMoveSecondSource =
        QDir(transferSourceDirectory).filePath("undo-move-second.txt");
    QFile undoMoveSecondFixture(undoMoveSecondSource);
    passed &= expect(undoMoveSecondFixture.open(QIODevice::WriteOnly)
            && undoMoveSecondFixture.write("second undo move") == 16,
        "The second move-undo transfer fixture should be created.");
    undoMoveSecondFixture.close();
    QEventLoop undoMoveLoop;
    quint64 undoMoveJob = 0;
    QObject::connect(&queue, &filemanager::files::TransferQueue::jobFinished,
        &undoMoveLoop, [&](quint64 id, bool, const QStringList&) {
            if (id == undoMoveJob)
                undoMoveLoop.quit();
        });
    undoMoveJob = queue.enqueue({{undoMoveSource, undoMoveSecondSource}, queueDestination,
        filemanager::files::TransferOperation::Move});
    QTimer::singleShot(5000, &undoMoveLoop, &QEventLoop::quit);
    undoMoveLoop.exec();
    const auto movedHistory = queue.history();
    const auto movedRecord = std::find_if(movedHistory.cbegin(), movedHistory.cend(),
        [undoMoveJob](const auto& record) { return record.id == undoMoveJob; });
    filemanager::files::SessionUndoStack moveUndo;
    passed &= expect(movedRecord != movedHistory.cend()
            && movedRecord->state == filemanager::files::TransferJobState::Succeeded
            && moveUndo.recordMove(movedRecord->request.sources, movedRecord->manifest)
            && moveUndo.canUndo(),
        "A completed same-volume queued move should create a verified session undo entry.");
    const auto moveUndoResult = moveUndo.undoLast();
    passed &= expect(moveUndoResult.ok && QFileInfo::exists(undoMoveSource)
            && QFileInfo::exists(undoMoveSecondSource)
            && !QFileInfo::exists(QDir(queueDestination).filePath("undo-move.txt"))
            && !QFileInfo::exists(QDir(queueDestination).filePath("undo-move-second.txt")),
        "Move undo should restore every unchanged item in a multi-item operation.");

    const QString queuedUndoSource =
        QDir(transferSourceDirectory).filePath("queued-undo.txt");
    QFile queuedUndoFixture(queuedUndoSource);
    passed &= expect(queuedUndoFixture.open(QIODevice::WriteOnly)
            && queuedUndoFixture.write("queued undo") == 11,
        "The queued move-undo fixture should be created.");
    queuedUndoFixture.close();
    QEventLoop queuedUndoMoveLoop;
    QTimer queuedUndoMoveTimeout;
    queuedUndoMoveTimeout.setSingleShot(true);
    quint64 queuedUndoMoveJob = 0;
    QObject::connect(&queue, &filemanager::files::TransferQueue::jobFinished,
        &queuedUndoMoveLoop, [&](quint64 id, bool, const QStringList&) {
            if (id == queuedUndoMoveJob) {
                queuedUndoMoveTimeout.stop();
                queuedUndoMoveLoop.quit();
            }
        });
    QObject::connect(&queuedUndoMoveTimeout, &QTimer::timeout,
        &queuedUndoMoveLoop, &QEventLoop::quit);
    queuedUndoMoveJob = queue.enqueue({{queuedUndoSource}, queueDestination,
        filemanager::files::TransferOperation::Move});
    queuedUndoMoveTimeout.start(5000);
    if (queue.activeJobId() == queuedUndoMoveJob)
        queuedUndoMoveLoop.exec();
    const auto queuedUndoMoveHistory = queue.history();
    const auto queuedUndoMoveRecord = std::find_if(
        queuedUndoMoveHistory.cbegin(), queuedUndoMoveHistory.cend(),
        [queuedUndoMoveJob](const auto& record) { return record.id == queuedUndoMoveJob; });
    auto crossVolumeManifest = queuedUndoMoveRecord != queuedUndoMoveHistory.cend()
        ? queuedUndoMoveRecord->manifest : QList<filemanager::files::TransferManifestEntry>{};
    for (auto& entry : crossVolumeManifest)
        entry.sourceVolumeSerial = entry.volumeSerial ^ 1U;
    filemanager::files::SessionUndoStack queuedMoveUndo;
    passed &= expect(queuedUndoMoveRecord != queuedUndoMoveHistory.cend()
            && queuedUndoMoveRecord->state == filemanager::files::TransferJobState::Succeeded
            && queuedMoveUndo.recordMove(
                queuedUndoMoveRecord->request.sources, crossVolumeManifest)
            && queuedMoveUndo.canUndo(),
        "A completed cross-volume move should create an identity-validated queued undo entry.");
    const auto queuedMoveUndoResult = queuedMoveUndo.undoLast();
    passed &= expect(queuedMoveUndoResult.ok && queuedMoveUndoResult.queuedRequest.has_value()
            && queuedMoveUndoResult.queuedRequest->sources
                == QStringList{QDir(queueDestination).filePath("queued-undo.txt")}
            && queuedMoveUndoResult.queuedRequest->destinationDirectory
                == QFileInfo(queuedUndoSource).absolutePath()
            && queuedMoveUndoResult.queuedRequest->operation
                == filemanager::files::TransferOperation::Move,
        "Cross-volume undo derives a checked reverse move instead of using an unsafe rename.");
    QEventLoop queuedUndoRestoreLoop;
    QTimer queuedUndoRestoreTimeout;
    queuedUndoRestoreTimeout.setSingleShot(true);
    quint64 queuedUndoRestoreJob = 0;
    QObject::connect(&queue, &filemanager::files::TransferQueue::jobFinished,
        &queuedUndoRestoreLoop, [&](quint64 id, bool, const QStringList&) {
            if (id == queuedUndoRestoreJob) {
                queuedUndoRestoreTimeout.stop();
                queuedUndoRestoreLoop.quit();
            }
        });
    QObject::connect(&queuedUndoRestoreTimeout, &QTimer::timeout,
        &queuedUndoRestoreLoop, &QEventLoop::quit);
    if (queuedMoveUndoResult.queuedRequest) {
        queuedUndoRestoreJob = queue.enqueue(*queuedMoveUndoResult.queuedRequest);
        queuedUndoRestoreTimeout.start(5000);
        if (queue.activeJobId() == queuedUndoRestoreJob)
            queuedUndoRestoreLoop.exec();
    }
    const auto queuedUndoRestoreHistory = queue.history();
    const auto queuedUndoRestoreRecord = std::find_if(
        queuedUndoRestoreHistory.cbegin(), queuedUndoRestoreHistory.cend(),
        [queuedUndoRestoreJob](const auto& record) { return record.id == queuedUndoRestoreJob; });
    passed &= expect(queuedUndoRestoreRecord != queuedUndoRestoreHistory.cend()
            && queuedUndoRestoreRecord->state == filemanager::files::TransferJobState::Succeeded
            && QFileInfo::exists(queuedUndoSource)
            && !QFileInfo::exists(QDir(queueDestination).filePath("queued-undo.txt")),
        "The queued reverse move restores the original path and removes its committed source.");

    const QString refusedQueuedUndoSource =
        QDir(transferSourceDirectory).filePath("refused-queued-undo.txt");
    QFile refusedQueuedUndoFixture(refusedQueuedUndoSource);
    passed &= expect(refusedQueuedUndoFixture.open(QIODevice::WriteOnly)
            && refusedQueuedUndoFixture.write("do not overwrite") == 16,
        "The queued undo refusal fixture should be created.");
    refusedQueuedUndoFixture.close();
    QEventLoop refusedQueuedUndoMoveLoop;
    QTimer refusedQueuedUndoMoveTimeout;
    refusedQueuedUndoMoveTimeout.setSingleShot(true);
    quint64 refusedQueuedUndoMoveJob = 0;
    QObject::connect(&queue, &filemanager::files::TransferQueue::jobFinished,
        &refusedQueuedUndoMoveLoop, [&](quint64 id, bool, const QStringList&) {
            if (id == refusedQueuedUndoMoveJob) {
                refusedQueuedUndoMoveTimeout.stop();
                refusedQueuedUndoMoveLoop.quit();
            }
        });
    QObject::connect(&refusedQueuedUndoMoveTimeout, &QTimer::timeout,
        &refusedQueuedUndoMoveLoop, &QEventLoop::quit);
    refusedQueuedUndoMoveJob = queue.enqueue({{refusedQueuedUndoSource}, queueDestination,
        filemanager::files::TransferOperation::Move});
    refusedQueuedUndoMoveTimeout.start(5000);
    if (queue.activeJobId() == refusedQueuedUndoMoveJob)
        refusedQueuedUndoMoveLoop.exec();
    const auto refusedQueuedUndoHistory = queue.history();
    const auto refusedQueuedUndoMoveRecord = std::find_if(
        refusedQueuedUndoHistory.cbegin(), refusedQueuedUndoHistory.cend(),
        [refusedQueuedUndoMoveJob](const auto& record) {
            return record.id == refusedQueuedUndoMoveJob;
        });
    auto refusedCrossVolumeManifest =
        refusedQueuedUndoMoveRecord != refusedQueuedUndoHistory.cend()
        ? refusedQueuedUndoMoveRecord->manifest
        : QList<filemanager::files::TransferManifestEntry>{};
    for (auto& entry : refusedCrossVolumeManifest)
        entry.sourceVolumeSerial = entry.volumeSerial ^ 1U;
    filemanager::files::SessionUndoStack refusedQueuedMoveUndo;
    passed &= expect(refusedQueuedUndoMoveRecord != refusedQueuedUndoHistory.cend()
            && refusedQueuedMoveUndo.recordMove(
                refusedQueuedUndoMoveRecord->request.sources, refusedCrossVolumeManifest),
        "The collision-refusal case should initially record a queued undo.");
    QFile occupiedOriginal(refusedQueuedUndoSource);
    passed &= expect(occupiedOriginal.open(QIODevice::WriteOnly)
            && occupiedOriginal.write("occupied") == 8,
        "The original location should be occupiable after the move.");
    occupiedOriginal.close();
    const auto refusedQueuedUndoResult = refusedQueuedMoveUndo.undoLast();
    passed &= expect(!refusedQueuedUndoResult.ok
            && !refusedQueuedUndoResult.queuedRequest.has_value()
            && QFileInfo::exists(refusedQueuedUndoSource)
            && QFileInfo::exists(
                QDir(queueDestination).filePath("refused-queued-undo.txt")),
        "Cross-volume undo refuses an occupied original path before queuing any mutation.");

    QTemporaryDir journalRoot;
    const QString journalPath = journalRoot.filePath("operation-journal.dat");
    filemanager::files::TransferRecord journalTransfer = queueHistory.first();
    journalTransfer.durableId = "durable-running-transfer";
    const QString unavailableRetrySource = transferRoot.filePath("unavailable-retry-source.txt");
    journalTransfer.request.sources.append(unavailableRetrySource);
    journalTransfer.state = filemanager::files::TransferJobState::Running;
    journalTransfer.queuedAt = QDateTime::currentDateTimeUtc().addSecs(-10);
    journalTransfer.startedAt = QDateTime::currentDateTimeUtc().addSecs(-5);
    journalTransfer.completedBytes = 5;
    journalTransfer.totalBytes = 5;
    journalTransfer.currentPath = journalTransfer.request.sources.first();
    filemanager::files::OperationJournal journal(journalPath);
    journal.synchronize({journalTransfer});
    const QList<filemanager::files::JournalRecord> codecRecords = journal.records();
    QList<filemanager::files::JournalRecord> decodedCodecRecords;
    int decodedJournalSchema = 0;
    QString codecError;
    const QByteArray schemaOneJournal =
        filemanager::files::serializeOperationJournal(codecRecords, 1);
    passed &= expect(filemanager::files::parseOperationJournal(schemaOneJournal,
            &decodedCodecRecords, &decodedJournalSchema, &codecError)
            && decodedJournalSchema == 1 && decodedCodecRecords.size() == 1
            && decodedCodecRecords.first().manifest.isEmpty(),
        "Operation-journal schema 1 should migrate without inventing recovery manifests.");
    const QByteArray schemaTwoJournal =
        filemanager::files::serializeOperationJournal(codecRecords, 2);
    passed &= expect(filemanager::files::parseOperationJournal(schemaTwoJournal,
            &decodedCodecRecords, &decodedJournalSchema, &codecError)
            && decodedJournalSchema == 2 && decodedCodecRecords.size() == 1
            && decodedCodecRecords.first().manifest.size() == codecRecords.first().manifest.size()
            && decodedCodecRecords.first().manifest.first().destinationPath
                == codecRecords.first().manifest.first().destinationPath,
        "Operation-journal schema 2 should preserve identity-checked recovery manifests.");
    QJsonObject invalidJournal = QJsonDocument::fromJson(schemaTwoJournal).object();
    QJsonObject invalidRecord = invalidJournal.value("records").toArray().first().toObject();
    invalidRecord.insert("sources", QJsonArray{"relative/path.txt"});
    invalidJournal.insert("records", QJsonArray{invalidRecord});
    passed &= expect(!filemanager::files::parseOperationJournal(
            QJsonDocument(invalidJournal).toJson(QJsonDocument::Compact),
            &decodedCodecRecords, &decodedJournalSchema, &codecError),
        "Operation-journal parsing should reject a malformed path before recovery state is exposed.");
    QString journalError;
    const bool journalFlushed = journal.flush(&journalError);
    if (!journalFlushed)
        std::fprintf(stderr, "Operation journal error: %s\n", journalError.toUtf8().constData());
    passed &= expect(journalFlushed,
        "The operation journal should encrypt and persist transfer state.");
    QFile encryptedJournal(journalPath);
    passed &= expect(encryptedJournal.open(QIODevice::ReadOnly),
        "The encrypted operation journal should be readable as an opaque file.");
    const QByteArray encryptedBytes = encryptedJournal.readAll();
    encryptedJournal.close();
    passed &= expect(!encryptedBytes.contains("queue-first.txt")
            && !encryptedBytes.contains("queue-destination")
            && !encryptedBytes.contains("unavailable-retry-source")
            && !encryptedBytes.startsWith('{'),
        "The operation journal must not expose paths or JSON as plaintext.");

    filemanager::files::OperationJournal reloadedJournal(journalPath);
    const bool journalLoaded = reloadedJournal.load(&journalError);
    const auto reloadedRecords = reloadedJournal.records();
    if (!journalLoaded || reloadedRecords.size() != 1)
        std::fprintf(stderr, "Operation journal reload: loaded=%d records=%lld error=%s\n",
            journalLoaded, static_cast<long long>(reloadedRecords.size()),
            journalError.toUtf8().constData());
    passed &= expect(journalLoaded
            && reloadedRecords.size() == 1
            && reloadedRecords.first().state
                == filemanager::files::JournalState::Interrupted
            && reloadedRecords.first().completedBytes == 5
            && reloadedRecords.first().manifest.size() == 1,
        "Restart should decrypt the journal and classify active work as interrupted.");
    QStringList unavailableRetrySources;
    const auto retryRequest = reloadedJournal.retryRequest(
        "durable-running-transfer", &unavailableRetrySources);
    passed &= expect(retryRequest.has_value() && retryRequest->sources.size() == 1
            && retryRequest->sources.first() == journalTransfer.request.sources.first()
            && unavailableRetrySources == QStringList{unavailableRetrySource}
            && retryRequest->destinationDirectory == journalTransfer.request.destinationDirectory
            && retryRequest->operation == journalTransfer.request.operation,
        "Recovery retry derives a new request using only currently available original sources.");
    const QString committedDestination = queueHistory.first().manifest.first().destinationPath;
    passed &= expect(reloadedJournal.verifiedCleanupPaths("durable-running-transfer")
            == QStringList{committedDestination},
        "Recovery cleanup offers only a destination whose identity matches while source exists.");
    const auto cleanupResult =
        reloadedJournal.cleanupVerifiedCreatedItems("durable-running-transfer");
    passed &= expect(cleanupResult.removedPaths == QStringList{committedDestination}
            && cleanupResult.refusedPaths.isEmpty() && cleanupResult.errors.isEmpty()
            && !QFileInfo::exists(committedDestination),
        "Confirmed recovery cleanup removes a verified newly created destination.");
    QFile replacedDestinationIdentity(committedDestination);
    passed &= expect(replacedDestinationIdentity.open(QIODevice::WriteOnly)
            && replacedDestinationIdentity.write("first") == 5,
        "A replacement destination should be creatable for identity-mismatch testing.");
    replacedDestinationIdentity.close();
    QStringList replacedRefused;
    passed &= expect(reloadedJournal.verifiedCleanupPaths(
            "durable-running-transfer", &replacedRefused).isEmpty()
            && replacedRefused == QStringList{committedDestination},
        "Recovery refuses a replacement destination even when its name and contents match.");
    passed &= expect(reloadedJournal.dismissInterrupted({"durable-running-transfer"}, &journalError)
            && reloadedJournal.interruptedRecords().isEmpty()
            && QFileInfo::exists(journalPath),
        "Reviewing an interrupted operation removes only its durable recovery record.");
    filemanager::files::OperationJournal dismissedJournal(journalPath);
    passed &= expect(dismissedJournal.load(&journalError)
            && dismissedJournal.interruptedRecords().isEmpty(),
        "Reviewed recovery records remain dismissed after restart.");
    passed &= expect(reloadedJournal.clear(&journalError)
            && reloadedJournal.records().isEmpty() && !QFileInfo::exists(journalPath),
        "Clearing operation history removes both memory and encrypted storage.");
    const QString corruptJournalPath = journalRoot.filePath("corrupt-operation-journal.dat");
    QFile corruptJournal(corruptJournalPath);
    passed &= expect(corruptJournal.open(QIODevice::WriteOnly)
            && corruptJournal.write("not a DPAPI journal") == 19,
        "The corrupt operation-journal fixture should be writable.");
    corruptJournal.close();
    filemanager::files::OperationJournal recoveredJournal(corruptJournalPath);
    journalError.clear();
    passed &= expect(!recoveredJournal.load(&journalError)
            && !journalError.isEmpty() && recoveredJournal.records().isEmpty()
            && !QFileInfo::exists(corruptJournalPath)
            && !QDir(journalRoot.path()).entryList({"corrupt-operation-journal.dat.corrupt-*"},
                    QDir::Files).isEmpty(),
        "A corrupt encrypted journal should be quarantined out of the active startup path.");
    recoveredJournal.synchronize({journalTransfer});
    passed &= expect(recoveredJournal.flush(&journalError)
            && QFileInfo::exists(corruptJournalPath),
        "New journal history should persist normally after corrupt storage is quarantined.");
    passed &= expect(recoveredJournal.clear(&journalError)
            && !QFileInfo::exists(corruptJournalPath)
            && QDir(journalRoot.path()).entryList(
                {"corrupt-operation-journal.dat.corrupt-*"}, QDir::Files).isEmpty(),
        "Clearing operation history should also remove quarantined encrypted journals.");

    filemanager::files::TransferRecord changedTransfer = queueHistory.at(1);
    changedTransfer.durableId = "changed-destination-transfer";
    changedTransfer.state = filemanager::files::TransferJobState::Running;
    const QString changedJournalPath = journalRoot.filePath("changed-journal.dat");
    filemanager::files::OperationJournal changedJournal(changedJournalPath);
    changedJournal.synchronize({changedTransfer});
    passed &= expect(changedJournal.flush(&journalError),
        "A second recovery manifest should persist for mismatch testing.");
    filemanager::files::OperationJournal changedReload(changedJournalPath);
    passed &= expect(changedReload.load(&journalError),
        "The mismatch recovery manifest should reload.");
    const QString changedDestination = changedTransfer.manifest.first().destinationPath;
    QFile changedFile(changedTransfer.request.sources.first());
    passed &= expect(changedFile.open(QIODevice::Append) && changedFile.write("changed") == 7,
        "The recovery source should be changeable after journaling.");
    changedFile.close();
    QStringList refusedCleanup;
    passed &= expect(changedReload.verifiedCleanupPaths(
            "changed-destination-transfer", &refusedCleanup).isEmpty()
            && refusedCleanup == QStringList{changedDestination},
        "Recovery refuses cleanup when the recorded source metadata no longer matches.");
    const auto refusedResult =
        changedReload.cleanupVerifiedCreatedItems("changed-destination-transfer");
    passed &= expect(refusedResult.removedPaths.isEmpty()
            && refusedResult.refusedPaths == QStringList{changedDestination}
            && QFileInfo::exists(changedDestination),
        "Mismatch refusal leaves the changed destination untouched.");

    const QString recoverySourceRoot = transferRoot.filePath("recovery-source");
    const QString recoveryDestinationRoot = transferRoot.filePath("recovery-destination");
    const QString recoveryFolder = QDir(recoverySourceRoot).filePath("created-folder");
    QDir().mkpath(recoveryFolder);
    QDir().mkpath(recoveryDestinationRoot);
    QFile recoveryPayload(QDir(recoveryFolder).filePath("payload.txt"));
    passed &= expect(recoveryPayload.open(QIODevice::WriteOnly)
            && recoveryPayload.write("payload") == 7,
        "The directory-recovery fixture should be created.");
    recoveryPayload.close();
    filemanager::files::TransferQueue recoveryQueue;
    QEventLoop recoveryQueueLoop;
    QObject::connect(&recoveryQueue, &filemanager::files::TransferQueue::jobFinished,
        &recoveryQueueLoop, &QEventLoop::quit);
    recoveryQueue.enqueue({{recoveryFolder}, recoveryDestinationRoot,
        filemanager::files::TransferOperation::Copy});
    QTimer::singleShot(5000, &recoveryQueueLoop, &QEventLoop::quit);
    recoveryQueueLoop.exec();
    auto directoryTransfer = recoveryQueue.history().first();
    passed &= expect(directoryTransfer.manifest.size() == 2,
        "Directory transfer manifests include the created folder and committed file.");
    directoryTransfer.durableId = "directory-recovery-transfer";
    directoryTransfer.state = filemanager::files::TransferJobState::Running;
    const QString directoryJournalPath = journalRoot.filePath("directory-journal.dat");
    filemanager::files::OperationJournal directoryJournal(directoryJournalPath);
    directoryJournal.synchronize({directoryTransfer});
    passed &= expect(directoryJournal.flush(&journalError),
        "The directory recovery manifest should persist.");
    filemanager::files::OperationJournal directoryReload(directoryJournalPath);
    passed &= expect(directoryReload.load(&journalError),
        "The directory recovery manifest should reload.");
    const QString copiedRecoveryFolder =
        QDir(recoveryDestinationRoot).filePath("created-folder");
    const QString untrackedPath = QDir(copiedRecoveryFolder).filePath("untracked.txt");
    QFile untracked(untrackedPath);
    passed &= expect(untracked.open(QIODevice::WriteOnly) && untracked.write("keep") == 4,
        "An untracked destination item should be added after the commit manifest.");
    untracked.close();
    const auto directoryCleanup =
        directoryReload.cleanupVerifiedCreatedItems("directory-recovery-transfer");
    passed &= expect(directoryCleanup.removedPaths.size() == 1
            && directoryCleanup.errors.size() == 1
            && QFileInfo::exists(untrackedPath) && QFileInfo(copiedRecoveryFolder).isDir(),
        "Recovery removes the verified file but refuses to recursively remove unknown contents.");

    const QDateTime now = QDateTime::currentDateTimeUtc();
    auto completedTransfer = [&](const QString& id,
                                 filemanager::files::TransferOperation operation,
                                 int ageDays) {
        filemanager::files::TransferRecord record;
        record.durableId = id;
        record.request = {{"C:/retention.txt"}, "D:/retention", operation};
        record.state = filemanager::files::TransferJobState::Succeeded;
        record.queuedAt = now.addDays(-ageDays).addSecs(-2);
        record.startedAt = now.addDays(-ageDays).addSecs(-1);
        record.finishedAt = now.addDays(-ageDays);
        return record;
    };
    filemanager::files::OperationJournal retentionJournal(
        journalRoot.filePath("retention-journal.dat"));
    retentionJournal.synchronize({
        completedTransfer("copy-8-days", filemanager::files::TransferOperation::Copy, 8),
        completedTransfer("move-8-days", filemanager::files::TransferOperation::Move, 8),
        completedTransfer("move-31-days", filemanager::files::TransferOperation::Move, 31)});
    const auto retainedRecords = retentionJournal.records();
    passed &= expect(retainedRecords.isEmpty(),
        "Completed recovery records expire after 7 days regardless of operation type.");
    const QString pauseDestination = transferRoot.filePath("pause-destination");
    QDir().mkpath(pauseDestination);
    QEventLoop pauseLoop;
    quint64 pausedJob = 0;
    QObject::connect(&queue, &filemanager::files::TransferQueue::jobFinished,
        &pauseLoop, [&](quint64 id, bool, const QStringList&) {
            if (id == pausedJob)
                pauseLoop.quit();
        });
    pausedJob = queue.enqueue({{cancellationFixture.fileName()}, pauseDestination,
        filemanager::files::TransferOperation::Copy});
    queue.pause(pausedJob);
    const auto pausedHistory = queue.history();
    passed &= expect(pausedHistory.last().state == filemanager::files::TransferJobState::Paused,
        "An active transfer enters the paused state.");
    QTimer::singleShot(50, &queue, [&queue, pausedJob] { queue.resume(pausedJob); });
    QTimer::singleShot(5000, &pauseLoop, &QEventLoop::quit);
    pauseLoop.exec();
    passed &= expect(QFileInfo::exists(QDir(pauseDestination).filePath("cancel-me.bin")),
        "A resumed transfer continues to completion.");

    const QString conflictImagePath = transferRoot.filePath("conflict-preview.png");
    QImage conflictImage(80, 60, QImage::Format_RGB32);
    conflictImage.fill(Qt::green);
    passed &= expect(conflictImage.save(conflictImagePath),
        "Conflict preview fixture should save.");
    const auto conflictImageInfo = filemanager::ui::ConflictDialog::inspect(conflictImagePath);
    passed &= expect(conflictImageInfo.path == QFileInfo(conflictImagePath).absoluteFilePath()
            && conflictImageInfo.name == QStringLiteral("conflict-preview.png")
            && conflictImageInfo.size == QFileInfo(conflictImagePath).size()
            && conflictImageInfo.modified.isValid() && !conflictImageInfo.directory
            && !conflictImageInfo.preview.isNull()
            && conflictImageInfo.preview.width() <= filemanager::ui::ConflictDialog::PreviewWidth
            && conflictImageInfo.preview.height() <= filemanager::ui::ConflictDialog::PreviewHeight,
        "Conflict comparison reports exact file metadata and a bounded image preview.");
    const auto conflictDirectoryInfo = filemanager::ui::ConflictDialog::inspect(transferRoot.path());
    passed &= expect(conflictDirectoryInfo.directory && conflictDirectoryInfo.size == -1
            && conflictDirectoryInfo.preview.isNull(),
        "Conflict comparison keeps folders metadata-only.");
    const QString oversizedConflictPath = transferRoot.filePath("oversized-conflict.png");
    passed &= expect(createSparseSizedFile(oversizedConflictPath,
            filemanager::ui::ConflictDialog::MaximumPreviewSourceBytes + 1),
        "Oversized conflict preview fixture should be created.");
    passed &= expect(filemanager::ui::ConflictDialog::inspect(oversizedConflictPath)
            .preview.isNull(),
        "Conflict comparison refuses previews above its source-byte limit.");
    const QString malformedConflictPath = transferRoot.filePath("malformed-conflict.png");
    QFile malformedConflict(malformedConflictPath);
    passed &= expect(malformedConflict.open(QIODevice::WriteOnly)
            && malformedConflict.write("not an image") == 12,
        "Malformed conflict preview fixture should be created.");
    malformedConflict.close();
    passed &= expect(filemanager::ui::ConflictDialog::inspect(malformedConflictPath)
            .preview.isNull(),
        "Conflict comparison safely falls back to metadata for malformed images.");

    const QString longTextPath = transferRoot.filePath("large-preview.txt");
    QFile longText(longTextPath);
    passed &= expect(longText.open(QIODevice::WriteOnly), "Large preview fixture should open.");
    longText.write(QByteArray(filemanager::preview::PreviewService::MaximumTextBytes + 1024, 'a'));
    longText.close();
    const auto textPreview = filemanager::preview::PreviewService{}.load(longTextPath);
    passed &= expect(textPreview.kind == filemanager::preview::PreviewKind::Text
            && textPreview.text.contains("Preview truncated at 128 KB"),
        "Text preview enforces its documented byte limit.");
    const QString largeImagePath = transferRoot.filePath("large-preview.png");
    QImage largeImage(3000, 100, QImage::Format_RGB32);
    largeImage.fill(Qt::magenta);
    passed &= expect(largeImage.save(largeImagePath), "Large image fixture should save.");
    const auto imagePreview = filemanager::preview::PreviewService{}.load(largeImagePath);
    passed &= expect(imagePreview.kind == filemanager::preview::PreviewKind::Image
            && imagePreview.image.width() <= filemanager::preview::PreviewService::MaximumImageDimension,
        "Image preview decodes to a bounded dimension.");
    filemanager::preview::ThumbnailProvider thumbnails;
    QEventLoop thumbnailLoop;
    QObject::connect(&thumbnails, &filemanager::preview::ThumbnailProvider::thumbnailReady,
        &thumbnailLoop, &QEventLoop::quit);
    const auto pendingThumbnail = thumbnails.request(largeImagePath, QSize(32, 32));
    passed &= expect(!pendingThumbnail.has_value(),
        "First thumbnail request schedules work without blocking.");
    QTimer::singleShot(5000, &thumbnailLoop, &QEventLoop::quit);
    thumbnailLoop.exec();
    passed &= expect(thumbnails.request(largeImagePath, QSize(32, 32)).has_value(),
        "Completed thumbnail is returned from the bounded cache.");
    QImage changedImage(640, 480, QImage::Format_RGB32);
    changedImage.fill(Qt::cyan);
    passed &= expect(changedImage.save(largeImagePath),
        "Changed thumbnail fixture should overwrite the source image.");
    passed &= expect(!thumbnails.request(largeImagePath, QSize(32, 32)).has_value(),
        "Thumbnail cache invalidates when source size or modification time changes.");

    {
        // Exercise the repaint loop with a real rejected image, then replace it
        // with a valid one to ensure the rejection does not outlive its source.
        const QString rejectedPath = transferRoot.filePath("rejected-thumbnail.png");
        QFile rejectedFile(rejectedPath);
        const bool rejectedWritten = rejectedFile.open(QIODevice::WriteOnly)
            && rejectedFile.write("not a PNG") == 9;
        rejectedFile.close();
        passed &= expect(rejectedWritten, "Rejected thumbnail fixture should be written.");
        filemanager::preview::ThumbnailProvider rejectedThumbnails;
        QEventLoop rejectedLoop;
        QTimer deadline;
        deadline.setSingleShot(true);
        QObject::connect(&deadline, &QTimer::timeout, &rejectedLoop, &QEventLoop::quit);
        int completions = 0;
        int failures = 0;
        QString rejectionError;
        QObject::connect(&rejectedThumbnails, &filemanager::preview::ThumbnailProvider::thumbnailFailed,
            &rejectedLoop, [&](const QString&, const QString& error) {
                ++failures;
                rejectionError = error;
            });
        QObject::connect(&rejectedThumbnails, &filemanager::preview::ThumbnailProvider::thumbnailReady,
            &rejectedLoop, [&](const QString&) { ++completions; rejectedLoop.quit(); });
        rejectedThumbnails.request(rejectedPath, QSize(24, 24));
        deadline.start(5000);
        rejectedLoop.exec();
        deadline.stop();
        passed &= expect(completions == 1 && failures == 1 && !rejectionError.isEmpty(),
            "Rejected thumbnails must complete once and expose the preview failure.");
        bool keptFallback = true;
        for (int repaint = 0; repaint < 24; ++repaint)
            keptFallback &= !rejectedThumbnails.request(rejectedPath, QSize(24, 24)).has_value();
        const bool settled = QThreadPool::globalInstance()->waitForDone(10000);
        QApplication::processEvents();
        passed &= expect(settled && keptFallback && completions == 1 && failures == 1,
            "Repeated paints of an unchanged rejected thumbnail must not launch more preview work.");
        passed &= expect(changedImage.save(rejectedPath),
            "Rejected thumbnail fixture should be replaceable with a valid image.");
        passed &= expect(!rejectedThumbnails.request(rejectedPath, QSize(24, 24)).has_value(),
            "Replacing a rejected thumbnail source must schedule a fresh load.");
        deadline.start(5000);
        rejectedLoop.exec();
        deadline.stop();
        passed &= expect(completions == 2 && failures == 1
                && rejectedThumbnails.request(rejectedPath, QSize(24, 24)).has_value(),
            "A repaired thumbnail must become available after its source changes.");
    }

    filemanager::preview::PreviewController previewController;
    QEventLoop previewLoop;
    filemanager::preview::PreviewData asynchronousPreview;
    quint64 requestedPreview = 0;
    QObject::connect(&previewController, &filemanager::preview::PreviewController::ready,
        [&](quint64 generation, const QString&, const auto& previewData) {
            if (generation == requestedPreview) {
                asynchronousPreview = previewData;
                previewLoop.quit();
            }
        });
    requestedPreview = previewController.start(largeImagePath);
    QTimer::singleShot(5000, &previewLoop, &QEventLoop::quit);
    previewLoop.exec();
    passed &= expect(asynchronousPreview.kind == filemanager::preview::PreviewKind::Image
            && !asynchronousPreview.image.isNull(),
        "Inspector previews decode asynchronously and return generation-tagged results.");

#ifdef Q_OS_WIN
    phase("installed_preview_cleanup_begin");
    QString installedCleanupError;
    const QString stagedRuntime = filemanager::preview::installedPreviewRuntimePath();
    const bool stagedRuntimeExisted = QFileInfo::exists(stagedRuntime);
    const bool installedCleanupSucceeded =
        filemanager::preview::removeInstalledPreviewSandbox(&installedCleanupError);
    if (!installedCleanupSucceeded && !installedCleanupError.isEmpty())
        std::fprintf(stderr, "Installed preview cleanup error: %s\n",
            installedCleanupError.toUtf8().constData());
    passed &= expect(stagedRuntimeExisted && installedCleanupSucceeded
            && !QFileInfo::exists(stagedRuntime),
        "The production cleanup should remove the staged test runtime and current-user "
        "AppContainer profile after all sandbox work has stopped.");
#endif

    phase(passed ? "checks_passed_before_teardown" : "checks_failed_before_teardown");
    return passed ? 0 : 1;
}
