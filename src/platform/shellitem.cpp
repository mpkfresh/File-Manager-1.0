#include "platform/shellitem.h"

#include <QDir>
#include <QFileInfo>
#include <QCoreApplication>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QProcess>
#include <QElapsedTimer>
#include <cstdio>

#include <future>
#include <thread>

#ifdef Q_OS_WIN
#include <windows.h>
#include <knownfolders.h>
#include <shlobj.h>
#include <shobjidl.h>
#endif

namespace filemanager::platform {
namespace {

#ifdef Q_OS_WIN
QString systemMessage(HRESULT result)
{
    wchar_t* buffer = nullptr;
    const DWORD length = FormatMessageW(FORMAT_MESSAGE_ALLOCATE_BUFFER
            | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
        nullptr, static_cast<DWORD>(result), 0, reinterpret_cast<wchar_t*>(&buffer), 0, nullptr);
    const QString message = length && buffer
        ? QString::fromWCharArray(buffer, static_cast<qsizetype>(length)).trimmed()
        : QStringLiteral("Windows Shell item resolution failed (0x%1).")
            .arg(static_cast<quint32>(result), 8, 16, QLatin1Char('0'));
    if (buffer)
        LocalFree(buffer);
    return message;
}

QString nameFromIdList(PCIDLIST_ABSOLUTE idList, SIGDN form)
{
    PWSTR value = nullptr;
    const HRESULT status = SHGetNameFromIDList(idList, form, &value);
    const QString result = SUCCEEDED(status) && value ? QString::fromWCharArray(value) : QString{};
    if (value)
        CoTaskMemFree(value);
    return result;
}

ShellItemReference referenceFromIdList(PCIDLIST_ABSOLUTE idList)
{
    ShellItemReference result;
    if (!idList) {
        result.error = QStringLiteral("The Shell item identifier is unavailable.");
        return result;
    }
    const UINT byteCount = ILGetSize(idList);
    if (byteCount > 0)
        result.idList = QByteArray(reinterpret_cast<const char*>(idList),
            static_cast<qsizetype>(byteCount));
    result.parsingName = nameFromIdList(idList, SIGDN_DESKTOPABSOLUTEPARSING);
    result.fileSystemPath = nameFromIdList(idList, SIGDN_FILESYSPATH);
    result.displayName = nameFromIdList(idList, SIGDN_NORMALDISPLAY);
    result.fileSystem = !result.fileSystemPath.isEmpty();
    IShellItem* item = nullptr;
    if (SUCCEEDED(SHCreateItemFromIDList(idList, IID_PPV_ARGS(&item)))) {
        SFGAOF attributes = 0;
        if (SUCCEEDED(item->GetAttributes(SFGAO_FOLDER, &attributes)))
            result.folder = (attributes & SFGAO_FOLDER) != 0;
        item->Release();
    }
    result.valid = !result.idList.isEmpty() && !result.parsingName.isEmpty();
    if (!result.valid)
        result.error = QStringLiteral("The Shell item did not provide a stable absolute identity.");
    return result;
}

ShellItemReference resolveSta(const QString& input)
{
    ShellItemReference result;
    const HRESULT initialized = CoInitializeEx(
        nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
    if (FAILED(initialized)) {
        result.nativeCode = initialized;
        result.error = systemMessage(initialized);
        return result;
    }

    PIDLIST_ABSOLUTE idList = nullptr;
    SFGAOF attributes = 0;
    const HRESULT status = SHParseDisplayName(
        reinterpret_cast<LPCWSTR>(input.utf16()), nullptr, &idList, 0, &attributes);
    result.nativeCode = status;
    if (SUCCEEDED(status) && idList) {
        result = referenceFromIdList(idList);
        result.nativeCode = status;
    } else {
        result.error = systemMessage(status);
    }
    if (idList)
        CoTaskMemFree(idList);
    CoUninitialize();
    return result;
}

ShellItemReference resolveBuiltInSta(BuiltInShellLocation location)
{
    ShellItemReference result;
    const HRESULT initialized = CoInitializeEx(
        nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
    if (FAILED(initialized)) {
        result.nativeCode = initialized;
        result.error = systemMessage(initialized);
        return result;
    }

    IShellItem* item = nullptr;
    HRESULT status = E_INVALIDARG;
    switch (location) {
    case BuiltInShellLocation::ThisPc:
        status = SHGetKnownFolderItem(FOLDERID_ComputerFolder, KF_FLAG_DEFAULT,
            nullptr, IID_PPV_ARGS(&item));
        break;
    case BuiltInShellLocation::Libraries:
        status = SHGetKnownFolderItem(FOLDERID_UsersLibraries, KF_FLAG_DEFAULT,
            nullptr, IID_PPV_ARGS(&item));
        break;
    case BuiltInShellLocation::RecycleBin:
        status = SHGetKnownFolderItem(FOLDERID_RecycleBinFolder, KF_FLAG_DEFAULT,
            nullptr, IID_PPV_ARGS(&item));
        break;
    case BuiltInShellLocation::None:
        break;
    }
    PIDLIST_ABSOLUTE idList = nullptr;
    if (SUCCEEDED(status) && item)
        status = SHGetIDListFromObject(item, &idList);
    result.nativeCode = status;
    if (SUCCEEDED(status) && idList) {
        result = referenceFromIdList(idList);
        result.nativeCode = status;
    } else {
        result.error = systemMessage(status);
    }
    if (idList)
        CoTaskMemFree(idList);
    if (item)
        item->Release();
    CoUninitialize();
    return result;
}

ShellItemEnumeration enumerateSta(const QByteArray& parentBytes, int maximumItems)
{
    ShellItemEnumeration result;
    const HRESULT initialized = CoInitializeEx(
        nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
    if (FAILED(initialized)) {
        result.nativeCode = initialized;
        result.error = systemMessage(initialized);
        return result;
    }

    const auto* parentIdList = reinterpret_cast<PCIDLIST_ABSOLUTE>(parentBytes.constData());
    IShellItem* parentItem = nullptr;
    IShellFolder* folder = nullptr;
    IEnumIDList* enumerator = nullptr;
    HRESULT status = SHCreateItemFromIDList(parentIdList, IID_PPV_ARGS(&parentItem));
    if (SUCCEEDED(status))
        status = parentItem->BindToHandler(nullptr, BHID_SFObject, IID_PPV_ARGS(&folder));
    if (SUCCEEDED(status)) {
        status = folder->EnumObjects(nullptr,
            SHCONTF_FOLDERS | SHCONTF_NONFOLDERS | SHCONTF_INCLUDEHIDDEN, &enumerator);
    }
    if (SUCCEEDED(status)) {
        for (;;) {
            PITEMID_CHILD child = nullptr;
            ULONG fetched = 0;
            const HRESULT next = enumerator->Next(1, &child, &fetched);
            if (next == S_FALSE || fetched == 0)
                break;
            if (FAILED(next)) {
                status = next;
                break;
            }
            if (result.items.size() >= maximumItems) {
                result.truncated = true;
                CoTaskMemFree(child);
                break;
            }
            PIDLIST_ABSOLUTE absolute = ILCombine(parentIdList, child);
            CoTaskMemFree(child);
            if (absolute) {
                ShellItemReference reference = referenceFromIdList(absolute);
                if (reference.valid)
                    result.items.append(std::move(reference));
                CoTaskMemFree(absolute);
            }
        }
    }
    result.nativeCode = status;
    result.success = SUCCEEDED(status);
    if (!result.success)
        result.error = systemMessage(status);
    if (enumerator)
        enumerator->Release();
    if (folder)
        folder->Release();
    if (parentItem)
        parentItem->Release();
    CoUninitialize();
    return result;
}
#endif

} // namespace

ShellItemReference ShellItem::resolve(const QString& pathOrParsingName)
{
    const QString trimmed = pathOrParsingName.trimmed();
    if (trimmed.isEmpty())
        return {false, false, false, 0, {}, {}, {}, {}, QStringLiteral("Shell item is empty.")};

#ifdef Q_OS_WIN
    const QFileInfo fileInfo(trimmed);
    const QString input = fileInfo.exists()
        ? QDir::toNativeSeparators(fileInfo.absoluteFilePath()) : trimmed;
    std::promise<ShellItemReference> completion;
    std::future<ShellItemReference> future = completion.get_future();
    std::thread worker([input, completion = std::move(completion)]() mutable {
        completion.set_value(resolveSta(input));
    });
    worker.join();
    return future.get();
#else
    const QFileInfo fileInfo(trimmed);
    if (!fileInfo.exists())
        return {false, false, false, 0, {}, {}, {}, {},
            QStringLiteral("Shell namespace items are unavailable on this platform.")};
    const QString path = fileInfo.absoluteFilePath();
    return {true, true, fileInfo.isDir(), 0, path, path, fileInfo.fileName(), {}, {}};
#endif
}

ShellItemReference ShellItem::resolveBuiltIn(BuiltInShellLocation location)
{
    if (location == BuiltInShellLocation::None)
        return {false, false, false, 0, {}, {}, {}, {},
            QStringLiteral("No built-in Windows location was selected.")};
#ifdef Q_OS_WIN
    std::promise<ShellItemReference> completion;
    std::future<ShellItemReference> future = completion.get_future();
    std::thread worker([location, completion = std::move(completion)]() mutable {
        completion.set_value(resolveBuiltInSta(location));
    });
    worker.join();
    return future.get();
#else
    Q_UNUSED(location);
    return {false, false, false, 0, {}, {}, {}, {},
        QStringLiteral("Built-in Windows locations are unavailable on this platform.")};
#endif
}

QString ShellItem::builtInIdentity(BuiltInShellLocation location)
{
    switch (location) {
    case BuiltInShellLocation::ThisPc:
        return QStringLiteral("filemanager:shell:this-pc");
    case BuiltInShellLocation::Libraries:
        return QStringLiteral("filemanager:shell:libraries");
    case BuiltInShellLocation::RecycleBin:
        return QStringLiteral("filemanager:shell:recycle-bin");
    case BuiltInShellLocation::None:
        return {};
    }
    return {};
}

QString ShellItem::builtInDisplayName(BuiltInShellLocation location)
{
    switch (location) {
    case BuiltInShellLocation::ThisPc:
        return QStringLiteral("This PC");
    case BuiltInShellLocation::Libraries:
        return QStringLiteral("Libraries");
    case BuiltInShellLocation::RecycleBin:
        return QStringLiteral("Recycle Bin");
    case BuiltInShellLocation::None:
        return {};
    }
    return {};
}

QString ShellItem::builtInParsingName(BuiltInShellLocation location)
{
    switch (location) {
    case BuiltInShellLocation::ThisPc: return QStringLiteral("shell:MyComputerFolder");
    case BuiltInShellLocation::Libraries: return QStringLiteral("shell:Libraries");
    case BuiltInShellLocation::RecycleBin: return QStringLiteral("shell:RecycleBinFolder");
    case BuiltInShellLocation::None: return {};
    }
    return {};
}

BuiltInShellLocation ShellItem::builtInLocationFromIdentity(const QString& identity)
{
    const QString value = identity.trimmed();
    for (const auto location : {BuiltInShellLocation::ThisPc,
             BuiltInShellLocation::Libraries, BuiltInShellLocation::RecycleBin}) {
        if (value.compare(builtInIdentity(location), Qt::CaseInsensitive) == 0)
            return location;
    }
    return BuiltInShellLocation::None;
}

ShellItemReference ShellItem::parent(const ShellItemReference& item)
{
    if (!item.valid || !isValidSerializedIdentity(item.idList))
        return {false, false, false, 0, {}, {}, {}, {},
            QStringLiteral("Shell item has no usable absolute identity.")};
#ifdef Q_OS_WIN
    std::promise<ShellItemReference> completion;
    auto future = completion.get_future();
    std::thread worker([bytes = item.idList, completion = std::move(completion)]() mutable {
        ShellItemReference result;
        const HRESULT initialized = CoInitializeEx(
            nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
        if (FAILED(initialized)) {
            result.nativeCode = initialized;
            result.error = systemMessage(initialized);
        } else {
            const auto* source = reinterpret_cast<PCIDLIST_ABSOLUTE>(bytes.constData());
            PIDLIST_ABSOLUTE parentIdList = ILCloneFull(source);
            if (!parentIdList) {
                result.nativeCode = E_OUTOFMEMORY;
                result.error = QStringLiteral("Windows could not copy the Shell item identity.");
            } else {
                if (ILIsEmpty(parentIdList) || !ILRemoveLastID(parentIdList))
                    result.error = QStringLiteral("This Shell location has no parent.");
                else
                    result = referenceFromIdList(parentIdList);
                CoTaskMemFree(parentIdList);
            }
            CoUninitialize();
        }
        completion.set_value(std::move(result));
    });
    worker.join();
    return future.get();
#else
    if (!item.fileSystem)
        return {false, false, false, 0, {}, {}, {}, {},
            QStringLiteral("Shell namespace parents are unavailable on this platform.")};
    const QFileInfo info(item.fileSystemPath);
    return resolve(info.absolutePath());
#endif
}

ShellItemEnumeration ShellItem::enumerateChildren(
    const ShellItemReference& parent, int maximumItems)
{
    if (!parent.valid || !parent.folder || !isValidSerializedIdentity(parent.idList))
        return {false, false, 0, {}, QStringLiteral("Shell parent is not an enumerable folder.")};
    if (maximumItems <= 0)
        return {false, false, 0, {}, QStringLiteral("Shell enumeration limit must be positive.")};
#ifdef Q_OS_WIN
    std::promise<ShellItemEnumeration> completion;
    std::future<ShellItemEnumeration> future = completion.get_future();
    std::thread worker([bytes = parent.idList, maximumItems,
                           completion = std::move(completion)]() mutable {
        completion.set_value(enumerateSta(bytes, maximumItems));
    });
    worker.join();
    return future.get();
#else
    return {false, false, 0, {},
        QStringLiteral("Shell namespace enumeration is unavailable on this platform.")};
#endif
}

namespace {
ShellItemEnumeration runIsolatedShellRequest(const QStringList& arguments,
    int maximumItems, int timeoutMilliseconds, const QString& helperExecutable,
    const ShellCancellation& cancelled)
{
    if (cancelled && cancelled->load())
        return {false, false, 0, {}, QStringLiteral("The Shell request was cancelled.")};
    if (timeoutMilliseconds < 100 || timeoutMilliseconds > 30000)
        return {false, false, 0, {}, QStringLiteral("Shell helper timeout is outside policy.")};

    const QString executable = helperExecutable.isEmpty()
        ? QDir(QCoreApplication::applicationDirPath()).filePath(
#ifdef Q_OS_WIN
            QStringLiteral("FileManagerShellHost.exe"))
#else
            QStringLiteral("FileManagerShellHost"))
#endif
        : helperExecutable;
    QProcess helper;
    helper.setProgram(executable);
    helper.setArguments(arguments);
    helper.start(QIODevice::ReadOnly);
    if (!helper.waitForStarted(qMin(timeoutMilliseconds, 1000)))
        return {false, false, 0, {}, QStringLiteral("The isolated Shell helper could not start.")};
    QElapsedTimer deadline;
    deadline.start();
    QByteArray payload;
    qsizetype receivedBytes = 0;
    while (helper.state() != QProcess::NotRunning) {
        helper.waitForFinished(25);
        const auto output = helper.readAllStandardOutput();
        receivedBytes += output.size() + helper.readAllStandardError().size();
        if ((cancelled && cancelled->load()) || receivedBytes > 8 * 1024 * 1024
                || deadline.elapsed() >= timeoutMilliseconds) {
            helper.kill();
            helper.waitForFinished(1000);
            return {false, false, 0, {}, cancelled && cancelled->load()
                ? QStringLiteral("The Shell request was cancelled.")
                : receivedBytes > 8 * 1024 * 1024
                    ? QStringLiteral("The isolated Shell reply exceeded its limit.")
                    : QStringLiteral("The isolated Shell helper timed out.")};
        }
        payload.append(output);
    }
    payload.append(helper.readAllStandardOutput());
    if (cancelled && cancelled->load())
        return {false, false, 0, {}, QStringLiteral("The Shell request was cancelled.")};
    if (helper.exitStatus() != QProcess::NormalExit || helper.exitCode() != 0)
        return {false, false, helper.exitCode(), {},
            QStringLiteral("The isolated Shell helper failed or crashed.")};
    if (payload.size() > 8 * 1024 * 1024)
        return {false, false, 0, {}, QStringLiteral("The isolated Shell reply exceeded its limit.")};

    QJsonParseError parseError;
    const QJsonDocument document = QJsonDocument::fromJson(payload, &parseError);
    if (parseError.error != QJsonParseError::NoError || !document.isObject()) {
        std::fprintf(stderr, "Shell protocol diagnostic: bytes=%lld parse=%d offset=%lld object=%d\n",
            static_cast<long long>(payload.size()), static_cast<int>(parseError.error),
            static_cast<long long>(parseError.offset), document.isObject());
        return {false, false, 0, {}, QStringLiteral("The isolated Shell reply was malformed.")};
    }
    const QJsonObject root = document.object();
    if (!root.value("success").isBool() || !root.value("items").isArray()) {
        std::fprintf(stderr, "Shell protocol diagnostic: bytes=%lld success_type=%d items_type=%d\n",
            static_cast<long long>(payload.size()), static_cast<int>(root.value("success").type()),
            static_cast<int>(root.value("items").type()));
        return {false, false, 0, {}, QStringLiteral("The isolated Shell reply was malformed.")};
    }
    ShellItemEnumeration result;
    result.success = root.value("success").toBool();
    result.truncated = root.value("truncated").toBool();
    result.nativeCode = root.value("nativeCode").toInteger();
    result.error = root.value("error").toString();
    const QJsonArray items = root.value("items").toArray();
    if (items.size() > maximumItems)
        return {false, false, 0, {}, QStringLiteral("The isolated Shell reply violated its item limit.")};
    for (const QJsonValue& value : items) {
        const QJsonObject object = value.toObject();
        ShellItemReference item;
        item.valid = object.value("valid").toBool();
        item.fileSystem = object.value("fileSystem").toBool();
        item.folder = object.value("folder").toBool();
        item.nativeCode = object.value("nativeCode").toInteger();
        item.parsingName = object.value("parsingName").toString();
        item.fileSystemPath = object.value("fileSystemPath").toString();
        item.displayName = object.value("displayName").toString();
        item.idList = QByteArray::fromBase64(object.value("idList").toString().toLatin1(),
            QByteArray::Base64UrlEncoding);
        item.error = object.value("error").toString();
        if (!item.valid || !ShellItem::isValidSerializedIdentity(item.idList)
                || item.parsingName.isEmpty()) {
            return {false, false, 0, {},
                QStringLiteral("The isolated Shell reply contained an invalid item identity.")};
        }
        result.items.append(std::move(item));
    }
    if (!result.success && result.error.isEmpty())
        result.error = QStringLiteral("The isolated Shell enumeration failed.");
    return result;
}
} // namespace

ShellItemEnumeration ShellItem::enumerateChildrenIsolated(const ShellItemReference& parent,
    int maximumItems, int timeoutMilliseconds, const QString& helperExecutable,
    const ShellCancellation& cancelled)
{
    if (!parent.valid || !parent.folder || !isValidSerializedIdentity(parent.idList))
        return {false, false, 0, {}, QStringLiteral("Shell parent is not an enumerable folder.")};
    if (maximumItems <= 0 || maximumItems > 4096)
        return {false, false, 0, {}, QStringLiteral("Shell enumeration limit is outside policy.")};
    return runIsolatedShellRequest({QString::fromLatin1(parent.idList.toBase64(
        QByteArray::Base64UrlEncoding | QByteArray::OmitTrailingEquals)),
        QString::number(maximumItems)}, maximumItems, timeoutMilliseconds, helperExecutable, cancelled);
}

ShellItemReference ShellItem::parentIsolated(const ShellItemReference& item,
    int timeoutMilliseconds, const QString& helperExecutable, const ShellCancellation& cancelled)
{
    if (!hasParentIdentity(item))
        return {false, false, false, 0, {}, {}, {}, {},
            QStringLiteral("This Shell item has no usable parent identity.")};
    const auto response = runIsolatedShellRequest({QStringLiteral("--parent"),
        QString::fromLatin1(item.idList.toBase64(
            QByteArray::Base64UrlEncoding | QByteArray::OmitTrailingEquals))},
        1, timeoutMilliseconds, helperExecutable, cancelled);
    if (!response.success)
        return {false, false, false, response.nativeCode, {}, {}, {}, {}, response.error};

    // The helper must return precisely the requested item's parent, not another valid PIDL.
    qsizetype offset = 0;
    qsizetype lastItem = 0;
    while (offset < item.idList.size() - 2) {
        lastItem = offset;
        const auto* bytes = reinterpret_cast<const unsigned char*>(item.idList.constData() + offset);
        offset += bytes[0] | (static_cast<quint16>(bytes[1]) << 8);
    }
    const QByteArray expected = item.idList.left(lastItem) + QByteArray(2, '\0');
    if (response.truncated || response.items.size() != 1 || !response.items.first().folder
            || response.items.first().idList != expected)
        return {false, false, false, 0, {}, {}, {}, {},
            QStringLiteral("The isolated Shell reply did not identify the requested parent.")};
    return response.items.first();
}

ShellItemReference ShellItem::resolveIsolated(const QString& input,
    int timeoutMilliseconds, const QString& helperExecutable, const ShellCancellation& cancelled)
{
    if (input.trimmed().isEmpty() || input.size() > 8192 || input.contains(QChar::Null))
        return {false, false, false, 0, {}, {}, {}, {},
            QStringLiteral("Shell location is outside the request policy.")};
    const auto response = runIsolatedShellRequest({QStringLiteral("--resolve"), input},
        1, timeoutMilliseconds, helperExecutable, cancelled);
    if (!response.success)
        return {false, false, false, response.nativeCode, {}, {}, {}, {}, response.error};
    if (response.truncated || response.items.size() != 1)
        return {false, false, false, 0, {}, {}, {}, {},
            QStringLiteral("The isolated Shell reply did not identify one location.")};
    return response.items.first();
}

} // namespace filemanager::platform
