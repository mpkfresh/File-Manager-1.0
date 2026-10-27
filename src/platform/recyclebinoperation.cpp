#include "platform/recyclebinoperation.h"

#include <QDir>
#include <QCoreApplication>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLocale>
#include <QProcess>
#include <QSet>

#include <future>
#include <atomic>
#include <thread>

#ifdef Q_OS_WIN
#include <windows.h>
#include <shobjidl.h>
#include <shlobj.h>
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
        : QStringLiteral("Windows Recycle Bin operation failed (0x%1).")
            .arg(static_cast<quint32>(result), 8, 16, QLatin1Char('0'));
    if (buffer)
        LocalFree(buffer);
    return message;
}

class ProgressSink final : public IFileOperationProgressSink
{
public:
    explicit ProgressSink(QString guardedRestorePath = {})
        : guardedRestorePath_(std::move(guardedRestorePath))
    {
    }

    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID id, void** object) override
    {
        if (!object)
            return E_POINTER;
        *object = nullptr;
        if (id == IID_IUnknown || id == IID_IFileOperationProgressSink) {
            *object = static_cast<IFileOperationProgressSink*>(this);
            AddRef();
            return S_OK;
        }
        return E_NOINTERFACE;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return ++references_; }
    ULONG STDMETHODCALLTYPE Release() override
    {
        const ULONG remaining = --references_;
        if (remaining == 0)
            delete this;
        return remaining;
    }

    HRESULT STDMETHODCALLTYPE StartOperations() override { return S_OK; }
    HRESULT STDMETHODCALLTYPE FinishOperations(HRESULT) override { return S_OK; }
    HRESULT STDMETHODCALLTYPE PreRenameItem(DWORD, IShellItem*, LPCWSTR) override { return S_OK; }
    HRESULT STDMETHODCALLTYPE PostRenameItem(
        DWORD, IShellItem*, LPCWSTR, HRESULT, IShellItem*) override { return S_OK; }
    HRESULT STDMETHODCALLTYPE PreMoveItem(DWORD, IShellItem*, IShellItem*, LPCWSTR) override
    {
        if (!guardedRestorePath_.isEmpty() && QFileInfo::exists(guardedRestorePath_))
            return HRESULT_FROM_WIN32(ERROR_FILE_EXISTS);
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE PostMoveItem(
        DWORD, IShellItem*, IShellItem*, LPCWSTR, HRESULT result, IShellItem*) override
    {
        itemResult = result;
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE PreCopyItem(DWORD, IShellItem*, IShellItem*, LPCWSTR) override
    {
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE PostCopyItem(
        DWORD, IShellItem*, IShellItem*, LPCWSTR, HRESULT, IShellItem*) override { return S_OK; }
    HRESULT STDMETHODCALLTYPE PreDeleteItem(DWORD, IShellItem*) override { return S_OK; }
    HRESULT STDMETHODCALLTYPE PostDeleteItem(
        DWORD, IShellItem*, HRESULT result, IShellItem* recycledItem) override
    {
        itemResult = result;
        if (FAILED(result) && SUCCEEDED(firstItemFailure))
            firstItemFailure = result;
        if (SUCCEEDED(result) && recycledItem) {
            PIDLIST_ABSOLUTE pidl = nullptr;
            if (SUCCEEDED(SHGetIDListFromObject(recycledItem, &pidl)) && pidl) {
                const UINT bytes = ILGetSize(pidl);
                absolutePidl = QByteArray(reinterpret_cast<const char*>(pidl), bytes);
                CoTaskMemFree(pidl);
            }
        }
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE PreNewItem(DWORD, IShellItem*, LPCWSTR) override { return S_OK; }
    HRESULT STDMETHODCALLTYPE PostNewItem(
        DWORD, IShellItem*, LPCWSTR, LPCWSTR, DWORD, HRESULT, IShellItem*) override { return S_OK; }
    HRESULT STDMETHODCALLTYPE UpdateProgress(UINT, UINT) override { return S_OK; }
    HRESULT STDMETHODCALLTYPE ResetTimer() override { return S_OK; }
    HRESULT STDMETHODCALLTYPE PauseTimer() override { return S_OK; }
    HRESULT STDMETHODCALLTYPE ResumeTimer() override { return S_OK; }

    QByteArray absolutePidl;
    HRESULT itemResult = E_PENDING;
    HRESULT firstItemFailure = S_OK;

private:
    std::atomic<ULONG> references_{1};
    QString guardedRestorePath_;
};

RecycleBinResult recycleSta(const QString& path)
{
    RecycleBinResult result;
    result.item.originalPath = path;
    const HRESULT initialized = CoInitializeEx(
        nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
    if (FAILED(initialized)) {
        result.nativeCode = initialized;
        result.error = systemMessage(initialized);
        return result;
    }
    IFileOperation* operation = nullptr;
    IShellItem* source = nullptr;
    auto* sink = new ProgressSink;
    HRESULT status = CoCreateInstance(CLSID_FileOperation, nullptr, CLSCTX_INPROC_SERVER,
        IID_PPV_ARGS(&operation));
    if (SUCCEEDED(status)) {
        status = operation->SetOperationFlags(FOF_SILENT | FOF_NOERRORUI | FOF_NOCONFIRMATION
            | FOF_ALLOWUNDO | FOFX_RECYCLEONDELETE | FOFX_EARLYFAILURE);
    }
    const QString nativePath = QDir::toNativeSeparators(path);
    if (SUCCEEDED(status))
        status = SHCreateItemFromParsingName(reinterpret_cast<LPCWSTR>(nativePath.utf16()), nullptr,
            IID_PPV_ARGS(&source));
    if (SUCCEEDED(status))
        status = operation->DeleteItem(source, sink);
    if (SUCCEEDED(status))
        status = operation->PerformOperations();
    BOOL aborted = FALSE;
    if (operation)
        operation->GetAnyOperationsAborted(&aborted);
    const HRESULT itemStatus = sink->itemResult;
    result.aborted = aborted != FALSE;
    result.nativeCode = FAILED(status) ? status : itemStatus;
    result.item.absolutePidl = sink->absolutePidl;
    result.success = SUCCEEDED(status) && SUCCEEDED(itemStatus) && !result.aborted;
    if (!result.success) {
        result.error = result.aborted
            ? QStringLiteral("The Recycle Bin operation was aborted.")
            : systemMessage(result.nativeCode);
    }
    sink->Release();
    if (source)
        source->Release();
    if (operation)
        operation->Release();
    CoUninitialize();
    return result;
}

RecycleBinResult restoreSta(const RecycleBinItem& item)
{
    RecycleBinResult result;
    result.item = item;
    const HRESULT initialized = CoInitializeEx(
        nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
    if (FAILED(initialized)) {
        result.nativeCode = initialized;
        result.error = systemMessage(initialized);
        return result;
    }
    IFileOperation* operation = nullptr;
    IShellItem* recycled = nullptr;
    IShellItem* destination = nullptr;
    auto* sink = new ProgressSink(item.originalPath);
    HRESULT status = item.absolutePidl.size() >= static_cast<int>(sizeof(USHORT) * 2)
        ? S_OK : E_INVALIDARG;
    if (SUCCEEDED(status)) {
        status = SHCreateItemFromIDList(
            reinterpret_cast<PCIDLIST_ABSOLUTE>(item.absolutePidl.constData()),
            IID_PPV_ARGS(&recycled));
    }
    if (SUCCEEDED(status)) {
        status = CoCreateInstance(CLSID_FileOperation, nullptr, CLSCTX_INPROC_SERVER,
            IID_PPV_ARGS(&operation));
    }
    if (SUCCEEDED(status)) {
        status = operation->SetOperationFlags(
            FOF_SILENT | FOF_NOERRORUI | FOF_NOCONFIRMATION | FOFX_EARLYFAILURE);
    }
    const QFileInfo original(item.originalPath);
    const QString nativeParent = QDir::toNativeSeparators(original.absolutePath());
    if (SUCCEEDED(status)) {
        status = SHCreateItemFromParsingName(reinterpret_cast<LPCWSTR>(nativeParent.utf16()), nullptr,
            IID_PPV_ARGS(&destination));
    }
    if (SUCCEEDED(status) && QFileInfo::exists(item.originalPath))
        status = HRESULT_FROM_WIN32(ERROR_FILE_EXISTS);
    if (SUCCEEDED(status)) {
        const std::wstring originalName = original.fileName().toStdWString();
        status = operation->MoveItem(
            recycled, destination, originalName.c_str(), sink);
    }
    if (SUCCEEDED(status))
        status = operation->PerformOperations();
    BOOL aborted = FALSE;
    if (operation)
        operation->GetAnyOperationsAborted(&aborted);
    const HRESULT itemStatus = sink->itemResult;
    result.aborted = aborted != FALSE;
    result.nativeCode = FAILED(status) ? status : itemStatus;
    result.success = SUCCEEDED(status) && SUCCEEDED(itemStatus) && !result.aborted
        && QFileInfo::exists(item.originalPath);
    if (!result.success) {
        result.error = result.aborted ? QStringLiteral("The Recycle Bin restore was aborted.")
            : systemMessage(result.nativeCode);
    }
    sink->Release();
    if (destination)
        destination->Release();
    if (recycled)
        recycled->Release();
    if (operation)
        operation->Release();
    CoUninitialize();
    return result;
}

bool availableSta(const RecycleBinItem& item)
{
    const HRESULT initialized = CoInitializeEx(
        nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
    if (FAILED(initialized))
        return false;
    IShellItem* recycled = nullptr;
    const HRESULT status = item.absolutePidl.size() >= static_cast<int>(sizeof(USHORT) * 2)
        ? SHCreateItemFromIDList(
            reinterpret_cast<PCIDLIST_ABSOLUTE>(item.absolutePidl.constData()),
            IID_PPV_ARGS(&recycled))
        : E_INVALIDARG;
    SFGAOF attributes = 0;
    const bool available = SUCCEEDED(status) && recycled
        && SUCCEEDED(recycled->GetAttributes(SFGAO_VALIDATE, &attributes));
    if (recycled)
        recycled->Release();
    CoUninitialize();
    return available;
}

RecycleBinBatchResult restoreSelectionSta(
    const QList<ShellItemReference>& items, quintptr ownerWindow)
{
    RecycleBinBatchResult result;
    result.requestedItems = items.size();
    const HRESULT initialized = CoInitializeEx(
        nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
    if (FAILED(initialized)) {
        result.nativeCode = initialized;
        result.error = systemMessage(initialized);
        return result;
    }

    const HWND owner = reinterpret_cast<HWND>(ownerWindow);
    PIDLIST_ABSOLUTE recycleRoot = nullptr;
    IShellFolder* desktopFolder = nullptr;
    IShellFolder* recycleFolder = nullptr;
    QString failureStage = QStringLiteral("opening the Windows Recycle Bin");
    HRESULT status = SHGetDesktopFolder(&desktopFolder);
    if (SUCCEEDED(status)) {
        status = SHGetKnownFolderIDList(
            FOLDERID_RecycleBinFolder, KF_FLAG_DEFAULT, nullptr, &recycleRoot);
    }
    if (SUCCEEDED(status)) {
        status = desktopFolder->BindToObject(
            recycleRoot, nullptr, IID_PPV_ARGS(&recycleFolder));
    }
    for (qsizetype index = 0; index < items.size() && SUCCEEDED(status); ++index) {
        const auto* absolute = reinterpret_cast<PCIDLIST_ABSOLUTE>(
            items.at(index).idList.constData());
        IEnumIDList* enumerator = nullptr;
        PITEMID_CHILD recycleChild = nullptr;
        failureStage = QStringLiteral("locating selected item %1 in the Recycle Bin")
            .arg(index + 1);
        status = recycleFolder->EnumObjects(
            nullptr, SHCONTF_FOLDERS | SHCONTF_NONFOLDERS | SHCONTF_INCLUDEHIDDEN, &enumerator);
        if (SUCCEEDED(status)) {
            ULONG fetched = 0;
            PITEMID_CHILD candidate = nullptr;
            while (enumerator->Next(1, &candidate, &fetched) == S_OK) {
                const HRESULT compared = recycleFolder->CompareIDs(
                    SHCIDS_CANONICALONLY, ILFindLastID(absolute), candidate);
                if (SUCCEEDED(compared)
                        && static_cast<short>(HRESULT_CODE(compared)) == 0) {
                    recycleChild = candidate;
                    break;
                }
                CoTaskMemFree(candidate);
                candidate = nullptr;
            }
            if (!recycleChild)
                status = HRESULT_FROM_WIN32(ERROR_NOT_FOUND);
        }
        if (enumerator)
            enumerator->Release();

        IContextMenu* context = nullptr;
        HMENU commandMenu = nullptr;
        if (SUCCEEDED(status)) {
            failureStage = QStringLiteral("requesting Restore for selected item %1")
                .arg(index + 1);
            PCUITEMID_CHILD child = recycleChild;
            status = recycleFolder->GetUIObjectOf(owner, 1, &child, IID_IContextMenu,
                nullptr, reinterpret_cast<void**>(&context));
        }
        if (SUCCEEDED(status)) {
            commandMenu = CreatePopupMenu();
            if (!commandMenu) {
                status = HRESULT_FROM_WIN32(GetLastError());
            } else {
                status = context->QueryContextMenu(
                    commandMenu, 0, 1, 0x7fff, CMF_NORMAL);
            }
        }
        if (SUCCEEDED(status)) {
            failureStage = QStringLiteral("invoking Restore for selected item %1")
                .arg(index + 1);
            CMINVOKECOMMANDINFO invocation{};
            invocation.cbSize = sizeof(invocation);
            invocation.hwnd = owner;
            invocation.lpVerb = "undelete";
            invocation.nShow = SW_SHOWNORMAL;
            status = context->InvokeCommand(&invocation);
        }
        if (commandMenu)
            DestroyMenu(commandMenu);
        if (context)
            context->Release();
        if (recycleChild)
            CoTaskMemFree(recycleChild);

        if (SUCCEEDED(status)) {
            failureStage = QStringLiteral("verifying restored item %1").arg(index + 1);
            bool stillAvailable = true;
            for (int attempt = 0; attempt < 100 && stillAvailable; ++attempt) {
                IShellItem* shellItem = nullptr;
                const HRESULT resolved = SHCreateItemFromIDList(
                    absolute, IID_PPV_ARGS(&shellItem));
                SFGAOF attributes = 0;
                stillAvailable = SUCCEEDED(resolved) && shellItem
                    && SUCCEEDED(shellItem->GetAttributes(SFGAO_VALIDATE, &attributes));
                if (shellItem)
                    shellItem->Release();
                if (stillAvailable)
                    Sleep(50);
            }
            if (stillAvailable) {
                status = HRESULT_FROM_WIN32(ERROR_GEN_FAILURE);
            } else {
                ++result.restoredItems;
            }
        }
    }
    if (recycleFolder)
        recycleFolder->Release();
    if (recycleRoot)
        CoTaskMemFree(recycleRoot);
    if (desktopFolder)
        desktopFolder->Release();

    result.nativeCode = status;
    result.success = SUCCEEDED(status) && result.restoredItems == result.requestedItems;
    if (!result.success) {
        result.error = result.restoredItems > 0
                ? QStringLiteral("Windows restored %1 of %2 selected items.")
                    .arg(result.restoredItems).arg(result.requestedItems)
                : QStringLiteral("Recycle Bin restore failed while %1: %2")
                    .arg(failureStage, systemMessage(status));
    }
    CoUninitialize();
    return result;
}

PermanentDeleteResult permanentlyDeleteSta(const QStringList& paths)
{
    PermanentDeleteResult result;
    const HRESULT initialized = CoInitializeEx(
        nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
    if (FAILED(initialized)) {
        result.nativeCode = initialized;
        result.error = systemMessage(initialized);
        result.remainingPaths = paths;
        return result;
    }
    IFileOperation* operation = nullptr;
    auto* sink = new ProgressSink;
    HRESULT status = CoCreateInstance(CLSID_FileOperation, nullptr, CLSCTX_INPROC_SERVER,
        IID_PPV_ARGS(&operation));
    if (SUCCEEDED(status)) {
        status = operation->SetOperationFlags(
            FOF_SILENT | FOF_NOERRORUI | FOF_NOCONFIRMATION | FOFX_EARLYFAILURE);
    }
    for (const QString& path : paths) {
        if (FAILED(status))
            break;
        IShellItem* source = nullptr;
        const QString nativePath = QDir::toNativeSeparators(path);
        status = SHCreateItemFromParsingName(
            reinterpret_cast<LPCWSTR>(nativePath.utf16()), nullptr, IID_PPV_ARGS(&source));
        if (SUCCEEDED(status))
            status = operation->DeleteItem(source, sink);
        if (source)
            source->Release();
    }
    if (SUCCEEDED(status))
        status = operation->PerformOperations();
    BOOL aborted = FALSE;
    if (operation)
        operation->GetAnyOperationsAborted(&aborted);
    result.aborted = aborted != FALSE;
    for (const QString& path : paths) {
        const QFileInfo item(path);
        if (item.exists() || item.isSymLink())
            result.remainingPaths.append(path);
        else
            result.deletedPaths.append(path);
    }
    result.nativeCode = FAILED(status) ? status : sink->firstItemFailure;
    result.success = SUCCEEDED(status) && SUCCEEDED(sink->firstItemFailure)
        && !result.aborted && result.remainingPaths.isEmpty();
    if (!result.success) {
        if (result.aborted) {
            result.error = QStringLiteral("The permanent delete operation was aborted.");
        } else if (!result.remainingPaths.isEmpty()
                && SUCCEEDED(status) && SUCCEEDED(sink->firstItemFailure)) {
            result.nativeCode = HRESULT_FROM_WIN32(ERROR_GEN_FAILURE);
            result.error = QStringLiteral(
                "Windows reported completion, but %1 selected %2 remain.")
                .arg(QLocale().toString(result.remainingPaths.size()),
                    result.remainingPaths.size() == 1 ? "item" : "items");
        } else {
            result.error = systemMessage(result.nativeCode);
        }
    }
    sink->Release();
    if (operation)
        operation->Release();
    CoUninitialize();
    return result;
}
#endif

template<typename Function>
RecycleBinResult runSta(Function&& function)
{
#ifdef Q_OS_WIN
    std::promise<RecycleBinResult> completion;
    auto future = completion.get_future();
    std::thread worker([function = std::forward<Function>(function),
                           completion = std::move(completion)]() mutable {
        completion.set_value(function());
    });
    worker.join();
    return future.get();
#else
    Q_UNUSED(function);
    return {false, false, 0, QStringLiteral("The Windows Recycle Bin is unavailable."), {}};
#endif
}

} // namespace

RecycleBinResult RecycleBinOperation::recycle(const QString& path)
{
    const QFileInfo source(path);
    if (!source.exists())
        return {false, false, 0, QStringLiteral("Item no longer exists: %1").arg(path),
            {source.absoluteFilePath(), {}}};
#ifdef Q_OS_WIN
    return runSta([absolutePath = source.absoluteFilePath()] { return recycleSta(absolutePath); });
#else
    return runSta([] { return RecycleBinResult{}; });
#endif
}

RecycleBinResult RecycleBinOperation::restore(const RecycleBinItem& item)
{
    if (item.originalPath.isEmpty() || item.absolutePidl.isEmpty())
        return {false, false, E_INVALIDARG, QStringLiteral("Recycle Bin identity is unavailable."), item};
    if (QFileInfo::exists(item.originalPath))
        return {false, false, HRESULT_FROM_WIN32(ERROR_FILE_EXISTS),
            QStringLiteral("The original path is occupied: %1").arg(item.originalPath), item};
#ifdef Q_OS_WIN
    return runSta([item] { return restoreSta(item); });
#else
    return runSta([] { return RecycleBinResult{}; });
#endif
}

bool RecycleBinOperation::isAvailable(const RecycleBinItem& item)
{
    if (item.originalPath.isEmpty() || item.absolutePidl.isEmpty()
            || QFileInfo::exists(item.originalPath))
        return false;
#ifdef Q_OS_WIN
    std::promise<bool> completion;
    auto future = completion.get_future();
    std::thread worker([item, completion = std::move(completion)]() mutable {
        completion.set_value(availableSta(item));
    });
    worker.join();
    return future.get();
#else
    return false;
#endif
}

PermanentDeleteResult RecycleBinOperation::permanentlyDelete(const QStringList& rawPaths)
{
    if (rawPaths.isEmpty() || rawPaths.size() > 256) {
        return {false, false, 0,
            QStringLiteral("Permanent deletion requires between 1 and 256 selected items."),
            {}, rawPaths};
    }
    QStringList paths;
    QSet<QString> seen;
    for (const QString& path : rawPaths) {
        const QFileInfo source(path);
        if (!source.isAbsolute()) {
            return {false, false, 0,
                QStringLiteral("Permanent deletion requires absolute item paths."), {}, rawPaths};
        }
        const QString absolutePath = QDir::cleanPath(source.absoluteFilePath());
        if (absolutePath.size() > 32767 || (!source.exists() && !source.isSymLink())) {
            return {false, false, 0,
                QStringLiteral("Item no longer exists: %1").arg(path), {}, rawPaths};
        }
        if (source.isDir() && QDir(absolutePath).isRoot()) {
            return {false, false, 0,
                QStringLiteral("Filesystem roots cannot be permanently deleted."), {}, rawPaths};
        }
        const QString key = absolutePath.toCaseFolded();
        if (!seen.contains(key)) {
            seen.insert(key);
            paths.append(absolutePath);
        }
    }
#ifdef Q_OS_WIN
    return permanentlyDeleteSta(paths);
#else
    return {false, false, 0, QStringLiteral("Windows permanent deletion is unavailable."),
        {}, paths};
#endif
}

RecycleBinBatchResult RecycleBinOperation::restoreSelectedShellItemsInProcess(
    const QList<ShellItemReference>& items, quintptr ownerWindow)
{
    qsizetype aggregateBytes = 0;
    if (items.isEmpty() || items.size() > 256) {
        return {false, false, E_INVALIDARG,
            QStringLiteral("Restore requires between 1 and 256 selected Recycle Bin items."),
            static_cast<int>(items.size()), 0};
    }
    for (const auto& item : items) {
        aggregateBytes += item.idList.size();
        if (!item.valid || !ShellItem::isValidSerializedIdentity(item.idList)
                || aggregateBytes > 4 * 1024 * 1024) {
            return {false, false, E_INVALIDARG,
                QStringLiteral("A selected Recycle Bin identity is invalid or exceeds the limit."),
                static_cast<int>(items.size()), 0};
        }
    }
#ifdef Q_OS_WIN
    return restoreSelectionSta(items, ownerWindow);
#else
    Q_UNUSED(ownerWindow);
    return {false, false, 0, QStringLiteral("The Windows Recycle Bin is unavailable."),
        static_cast<int>(items.size()), 0};
#endif
}

RecycleBinBatchResult RecycleBinOperation::restoreSelectedShellItemsIsolated(
    const QList<ShellItemReference>& items, quintptr ownerWindow,
    int timeoutMilliseconds, const QString& helperExecutable)
{
    if (timeoutMilliseconds < 1000 || timeoutMilliseconds > 300000) {
        return {false, false, E_INVALIDARG,
            QStringLiteral("The Recycle Bin helper timeout is outside the supported range."),
            static_cast<int>(items.size()), 0};
    }
    QJsonArray identities;
    qsizetype aggregateBytes = 0;
    for (const auto& item : items) {
        aggregateBytes += item.idList.size();
        if (!item.valid || !ShellItem::isValidSerializedIdentity(item.idList)
                || aggregateBytes > 4 * 1024 * 1024) {
            return {false, false, E_INVALIDARG,
                QStringLiteral("A selected Recycle Bin identity is invalid or exceeds the limit."),
                static_cast<int>(items.size()), 0};
        }
        identities.append(QString::fromLatin1(item.idList.toBase64(
            QByteArray::Base64UrlEncoding | QByteArray::OmitTrailingEquals)));
    }
    if (identities.isEmpty() || identities.size() > 256) {
        return {false, false, E_INVALIDARG,
            QStringLiteral("Restore requires between 1 and 256 selected Recycle Bin items."),
            static_cast<int>(items.size()), 0};
    }
    QString helper = helperExecutable;
    if (helper.isEmpty()) {
#ifdef Q_OS_WIN
        helper = QDir(QCoreApplication::applicationDirPath())
            .filePath(QStringLiteral("FileManagerShellHost.exe"));
#else
        helper = QDir(QCoreApplication::applicationDirPath())
            .filePath(QStringLiteral("FileManagerShellHost"));
#endif
    }
    QProcess process;
    process.start(helper,
        {QStringLiteral("--restore-selection"), QString::number(ownerWindow)},
        QIODevice::ReadWrite);
    if (!process.waitForStarted(1000)) {
        return {false, false, 0,
            QStringLiteral("The isolated Recycle Bin helper could not start: %1")
                .arg(process.errorString()), static_cast<int>(items.size()), 0};
    }
    process.write(QJsonDocument(QJsonObject{{QStringLiteral("items"), identities}})
        .toJson(QJsonDocument::Compact));
    process.closeWriteChannel();
    if (!process.waitForFinished(timeoutMilliseconds)) {
        process.kill();
        process.waitForFinished(3000);
        return {false, true, HRESULT_FROM_WIN32(ERROR_TIMEOUT),
            QStringLiteral("The isolated Recycle Bin restore timed out and was stopped."),
            static_cast<int>(items.size()), 0};
    }
    const QByteArray output = process.readAllStandardOutput();
    QJsonParseError parseError;
    const QJsonDocument document = QJsonDocument::fromJson(output, &parseError);
    if (process.exitStatus() != QProcess::NormalExit || output.size() > 64 * 1024
            || parseError.error != QJsonParseError::NoError || !document.isObject()) {
        return {false, false, process.exitCode(),
            QStringLiteral("The isolated Recycle Bin helper returned an invalid response."),
            static_cast<int>(items.size()), 0};
    }
    const QJsonObject object = document.object();
    return {object.value(QStringLiteral("success")).toBool(),
        object.value(QStringLiteral("aborted")).toBool(),
        static_cast<long>(object.value(QStringLiteral("nativeCode")).toInteger()),
        object.value(QStringLiteral("error")).toString(),
        object.value(QStringLiteral("requestedItems")).toInt(),
        object.value(QStringLiteral("restoredItems")).toInt()};
}

RecycleBinSummary RecycleBinOperation::summary()
{
#ifdef Q_OS_WIN
    SHQUERYRBINFO information{sizeof(information)};
    const HRESULT status = SHQueryRecycleBinW(nullptr, &information);
    if (FAILED(status))
        return {false, status, systemMessage(status), 0, 0};
    return {true, status, {}, static_cast<quint64>(information.i64NumItems),
        static_cast<quint64>(information.i64Size)};
#else
    return {false, 0, QStringLiteral("The Windows Recycle Bin is unavailable."), 0, 0};
#endif
}

RecycleBinBatchResult RecycleBinOperation::empty(quintptr ownerWindow)
{
#ifdef Q_OS_WIN
    const auto before = summary();
    if (!before.success)
        return {false, false, before.nativeCode, before.error, 0, 0};
    const HRESULT status = SHEmptyRecycleBinW(reinterpret_cast<HWND>(ownerWindow), nullptr,
        SHERB_NOCONFIRMATION);
    return {SUCCEEDED(status), status == HRESULT_FROM_WIN32(ERROR_CANCELLED), status,
        SUCCEEDED(status) ? QString{} : systemMessage(status),
        static_cast<int>(qMin<quint64>(before.itemCount, static_cast<quint64>(INT_MAX))), 0};
#else
    Q_UNUSED(ownerWindow);
    return {false, false, 0, QStringLiteral("The Windows Recycle Bin is unavailable."), 0, 0};
#endif
}

} // namespace filemanager::platform
