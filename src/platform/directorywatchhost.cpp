#include "directorywatcher.h"
#include "platform/helperinput.h"
#include "platform/helperoutput.h"

#include <QByteArray>
#include <QDir>
#include <QJsonDocument>
#include <QJsonObject>
#include <QMap>
#include <QString>
#include <algorithm>
#include <array>
#include <map>
#include <memory>
#ifdef Q_OS_WIN
#include <windows.h>
#endif

namespace filemanager::platform {
#ifdef Q_OS_WIN
namespace {
QString normalized(const QString& path)
{
    return QDir::cleanPath(QDir::fromNativeSeparators(path)).toCaseFolded();
}
bool within(const QString& path, const QString& root)
{
    return path == root || path.startsWith(root.endsWith('/') ? root : root + '/');
}
bool covered(const QString& path, const QString& root)
{
    if (!within(normalized(path), normalized(root)))
        return false;
    if (normalized(path) == normalized(root))
        return true;
    // Recursive notifications do not establish coverage across a reparse boundary.
    // Inspect only components of an already displayed path, never enumerate them.
    QString prefix = root;
    for (const QString& component : path.mid(root.size()).split('/', Qt::SkipEmptyParts)) {
        if (!prefix.endsWith('/'))
            prefix += '/';
        prefix += component;
        const QString native = QDir::toNativeSeparators(prefix);
        const DWORD attributes = GetFileAttributesW(reinterpret_cast<LPCWSTR>(native.utf16()));
        if (attributes == INVALID_FILE_ATTRIBUTES || !(attributes & FILE_ATTRIBUTE_DIRECTORY)
                || (attributes & FILE_ATTRIBUTE_REPARSE_POINT))
            return false;
    }
    return true;
}
QString finalPath(HANDLE handle)
{
    std::array<wchar_t, 32768> path{};
    const DWORD count = GetFinalPathNameByHandleW(handle, path.data(), DWORD(path.size()), FILE_NAME_NORMALIZED);
    return count && count < path.size() ? normalized(QString::fromWCharArray(path.data(), int(count))) : QString{};
}
struct RootWatch {
    HANDLE directory = INVALID_HANDLE_VALUE;
    HANDLE event = nullptr;
    OVERLAPPED operation{};
    alignas(DWORD) std::array<char, 32768> buffer{};
    QString path;
    QString requestedPath;
    QString identity;
    bool pending = false;

    bool arm()
    {
        ResetEvent(event);
        operation = {};
        operation.hEvent = event;
        pending = ReadDirectoryChangesW(directory, buffer.data(), DWORD(buffer.size()), TRUE,
            FILE_NOTIFY_CHANGE_FILE_NAME | FILE_NOTIFY_CHANGE_DIR_NAME | FILE_NOTIFY_CHANGE_ATTRIBUTES
                | FILE_NOTIFY_CHANGE_SIZE | FILE_NOTIFY_CHANGE_LAST_WRITE | FILE_NOTIFY_CHANGE_SECURITY,
            nullptr, &operation, nullptr) != FALSE;
        return pending;
    }
    bool open(const QString& requested)
    {
        path = normalized(requested);
        requestedPath = requested;
        const QString native = QDir::toNativeSeparators(requested);
        directory = CreateFileW(reinterpret_cast<LPCWSTR>(native.utf16()), FILE_LIST_DIRECTORY,
            FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
            FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OVERLAPPED, nullptr);
        if (directory == INVALID_HANDLE_VALUE)
            return false;
        identity = finalPath(directory);
        event = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        return !identity.isEmpty() && event && arm();
    }
    bool unchangedRoot() const
    {
        FILE_STANDARD_INFO info{};
        if (!GetFileInformationByHandleEx(directory, FileStandardInfo, &info, sizeof(info))
                || !info.Directory || info.DeletePending || finalPath(directory) != identity)
            return false;
        // A junction (including an intermediate component) can be retargeted
        // without changing the object held by the existing watch handle.
        const QString native = QDir::toNativeSeparators(requestedPath);
        HANDLE current = CreateFileW(reinterpret_cast<LPCWSTR>(native.utf16()), 0,
            FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
            FILE_FLAG_BACKUP_SEMANTICS, nullptr);
        if (current == INVALID_HANDLE_VALUE)
            return false;
        BY_HANDLE_FILE_INFORMATION heldInfo{}, currentInfo{};
        const bool same = GetFileInformationByHandle(directory, &heldInfo)
            && GetFileInformationByHandle(current, &currentInfo)
            && heldInfo.dwVolumeSerialNumber == currentInfo.dwVolumeSerialNumber
            && heldInfo.nFileIndexHigh == currentInfo.nFileIndexHigh
            && heldInfo.nFileIndexLow == currentInfo.nFileIndexLow
            && finalPath(current) == identity;
        CloseHandle(current);
        return same;
    }
    ~RootWatch()
    {
        if (pending) {
            CancelIoEx(directory, &operation);
            DWORD bytes = 0;
            // Keep request memory alive through cancellation. The GUI watchdog/job
            // bounds a stuck driver without waiting on the GUI thread.
            GetOverlappedResult(directory, &operation, &bytes, TRUE);
        }
        if (directory != INVALID_HANDLE_VALUE)
            CloseHandle(directory);
        if (event)
            CloseHandle(event);
    }
};
struct Listing {
    QString path;
    QString requested;
    QString coverage;
    bool root = false;
    bool ready = false;
    bool dirty = false;
    bool retired = false;
    bool resync = false;
};
using Listings = std::map<QByteArray, Listing>;

bool reply(const QByteArray& bytes)
{
    if (bytes.isEmpty())
        return true;
    return writeHelperReply(bytes);
}
void invalidate(Listings& listings, const QString& root)
{
    for (auto& [id, listing] : listings) {
        Q_UNUSED(id);
        if (listing.coverage == root) {
            listing.dirty = true;
            listing.resync = true;
        }
    }
}
bool consume(RootWatch& watch, DWORD bytes, Listings& listings)
{
    if (!bytes) {
        // Overflow loses detail. Reconcile only already displayed listings.
        invalidate(listings, watch.path);
        return true;
    }
    if (bytes > watch.buffer.size())
        return false;
    DWORD offset = 0;
    while (offset < bytes) {
        constexpr DWORD header = offsetof(FILE_NOTIFY_INFORMATION, FileName);
        if (bytes - offset < header)
            return false;
        const auto* record = reinterpret_cast<const FILE_NOTIFY_INFORMATION*>(watch.buffer.data() + offset);
        if (record->FileNameLength % sizeof(wchar_t) || record->FileNameLength > bytes - offset - header
                || record->Action < FILE_ACTION_ADDED || record->Action > FILE_ACTION_RENAMED_NEW_NAME)
            return false;
        const QString relative = QDir::fromNativeSeparators(QString::fromWCharArray(record->FileName,
            int(record->FileNameLength / sizeof(wchar_t))));
        const QStringList components = relative.split('/');
        if (relative.isEmpty() || relative.contains(QChar::Null) || relative.contains(':')
                || relative.startsWith('/') || components.contains("..") || components.contains(".")
                || components.contains(QString{}))
            return false;
        const QString changed = normalized(watch.path + '/' + relative);
        const qsizetype slash = changed.lastIndexOf('/');
        QString parent = changed.left(slash);
        if (parent.endsWith(':'))
            parent += '/';
        for (auto& [id, listing] : listings) {
            Q_UNUSED(id);
            if (listing.coverage != watch.path)
                continue;
            if (listing.path == parent || listing.path == changed
                    || ((record->Action == FILE_ACTION_REMOVED || record->Action == FILE_ACTION_RENAMED_OLD_NAME)
                        && within(listing.path, changed)))
                listing.dirty = true;
            if ((record->Action == FILE_ACTION_REMOVED || record->Action == FILE_ACTION_RENAMED_OLD_NAME)
                    && within(listing.path, changed))
                listing.retired = true;
        }
        // Changed names are discarded here. Only dirty ids persist; no filenames
        // or event history leave the helper.
        if (!record->NextEntryOffset)
            return true;
        if (record->NextEntryOffset < header + record->FileNameLength
                || record->NextEntryOffset >= bytes - offset || record->NextEntryOffset % 4)
            return false;
        offset += record->NextEntryOffset;
    }
    return false;
}
}
#endif

int runDirectoryWatchHost()
{
#ifndef Q_OS_WIN
    return 30;
#else
    HANDLE input = GetStdHandle(STD_INPUT_HANDLE);
    HANDLE output = GetStdHandle(STD_OUTPUT_HANDLE);
    if (GetFileType(input) != FILE_TYPE_PIPE || GetFileType(output) != FILE_TYPE_PIPE)
        return 31;
    Listings listings;
    std::map<QString, std::unique_ptr<RootWatch>> roots;
    QByteArray pending;
    ULONGLONG flushed = GetTickCount64();
    ULONGLONG heartbeat = 0;
    ULONGLONG identityCheck = GetTickCount64();
    bool topologyChanged = false;
    while (true) {
        DWORD available = 0;
        if (!PeekNamedPipe(input, nullptr, 0, nullptr, &available, nullptr))
            return GetLastError() == ERROR_BROKEN_PIPE ? 0 : 34;
        if (available) {
            char buffer[32768];
            const qint64 read = readHelperInput(buffer, (std::min)(available, DWORD(sizeof(buffer))));
            if (read <= 0)
                return 35;
            pending.append(buffer, qsizetype(read));
        }
        for (int count = 0; count < 32; ++count) {
            const qsizetype newline = pending.indexOf('\n');
            if (newline < 0)
                break;
            if (newline > 65536)
                return 36;
            QJsonParseError error{};
            const QJsonDocument document = QJsonDocument::fromJson(pending.first(newline), &error);
            pending.remove(0, newline + 1);
            if (error.error != QJsonParseError::NoError || !document.isObject())
                return 37;
            const QJsonObject command = document.object();
            const QString idText = command.value("id").toString();
            bool validId = false;
            const quint64 number = idText.toULongLong(&validId);
            if (!validId || !number || idText != QString::number(number))
                return 38;
            const QByteArray id = idText.toLatin1();
            const QString operation = command.value("op").toString();
            if (operation == "remove") {
                listings.erase(id);
            } else if (operation == "root") {
                if (!command.value("root").isBool())
                    return 39;
                if (auto found = listings.find(id); found != listings.end())
                    found->second.root = command.value("root").toBool();
            } else if (operation == "add") {
                const QString path = command.value("path").toString();
                if (listings.size() >= 512 || listings.contains(id) || path.isEmpty() || path.size() > 8192
                        || path.contains(QChar::Null) || !QDir::isAbsolutePath(path)
                        || !command.value("root").isBool())
                    return 40;
                listings.emplace(id, Listing{normalized(path), path, {}, command.value("root").toBool()});
            } else {
                return 39;
            }
            topologyChanged = true;
        }
        if (pending.size() > 1024 * 1024)
            return 42;
        if (topologyChanged && !pending.contains('\n')) {
            QMap<QString, QString> selected;
            for (const auto& [id, listing] : listings) {
                Q_UNUSED(id);
                if (listing.root)
                    selected.insert(listing.path, listing.requested);
            }
            QStringList ordered = selected.keys();
            std::sort(ordered.begin(), ordered.end(), [](const QString& a, const QString& b) {
                return a.size() < b.size();
            });
            QStringList desired;
            for (const auto& path : ordered) {
                if (std::none_of(desired.cbegin(), desired.cend(), [&](const QString& existing) {
                        return covered(selected.value(path), selected.value(existing)); }))
                    desired.append(path);
            }
            // Release obsolete descendant handles before installing broader coverage.
            for (auto iterator = roots.begin(); iterator != roots.end();) {
                if (!desired.contains(iterator->first))
                    iterator = roots.erase(iterator);
                else
                    ++iterator;
            }
            for (const auto& path : desired) {
                if (roots.contains(path))
                    continue;
                auto watch = std::make_unique<RootWatch>();
                if (watch->open(selected.value(path)))
                    roots.emplace(path, std::move(watch));
            }
            QByteArray status;
            for (auto iterator = listings.begin(); iterator != listings.end();) {
                auto& listing = iterator->second;
                QString coverage;
                for (const auto& [path, watch] : roots) {
                    Q_UNUSED(watch);
                    if (covered(listing.requested, selected.value(path))) {
                        coverage = path;
                        break;
                    }
                }
                if (coverage.isEmpty()) {
                    status += "failed " + iterator->first + '\n';
                    iterator = listings.erase(iterator);
                    continue;
                }
                if (!listing.ready)
                    status += "ready " + iterator->first + '\n';
                else if (listing.coverage != coverage)
                    listing.dirty = true;
                listing.coverage = coverage;
                listing.ready = true;
                ++iterator;
            }
            if (!reply(status))
                return 41;
            topologyChanged = false;
        }
        QByteArray retired;
        const ULONGLONG now = GetTickCount64();
        for (auto iterator = roots.begin(); iterator != roots.end();) {
            auto& watch = *iterator->second;
            bool healthy = now - identityCheck < 1000 || watch.unchangedRoot();
            if (healthy && WaitForSingleObject(watch.event, 0) == WAIT_OBJECT_0) {
                DWORD bytes = 0;
                const bool completed = GetOverlappedResult(watch.directory, &watch.operation, &bytes, FALSE) != FALSE;
                const DWORD error = completed ? ERROR_SUCCESS : GetLastError();
                watch.pending = !completed && error == ERROR_IO_INCOMPLETE;
                if (completed)
                    healthy = consume(watch, bytes, listings);
                else if (error == ERROR_NOTIFY_ENUM_DIR)
                    invalidate(listings, watch.path);
                else
                    healthy = false;
                if (!watch.pending)
                    SecureZeroMemory(watch.buffer.data(), watch.buffer.size());
                if (healthy)
                    healthy = watch.arm();
            }
            if (!healthy) {
                for (auto item = listings.begin(); item != listings.end();) {
                    if (item->second.coverage == watch.path) {
                        retired += "retired " + item->first + '\n';
                        item = listings.erase(item);
                    } else {
                        ++item;
                    }
                }
                iterator = roots.erase(iterator);
            } else {
                ++iterator;
            }
        }
        if (now - identityCheck >= 1000)
            identityCheck = now;
        for (auto iterator = listings.begin(); iterator != listings.end();) {
            if (iterator->second.retired) {
                retired += "retired " + iterator->first + '\n';
                iterator = listings.erase(iterator);
                topologyChanged = true;
            } else {
                ++iterator;
            }
        }
        if (!reply(retired))
            return 43;
        if (now - flushed >= 75) {
            QByteArray changes;
            for (auto& [id, listing] : listings) {
                if (listing.dirty) {
                    changes += (listing.resync ? "resync " : "changed ") + id + '\n';
                    listing.dirty = false;
                    listing.resync = false;
                }
            }
            if (!reply(changes))
                return 43;
            flushed = now;
        }
        if (now - heartbeat >= 1000) {
            if (!reply("heartbeat\n"))
                return 44;
            heartbeat = now;
        }
        Sleep(20);
    }
#endif
}
}
