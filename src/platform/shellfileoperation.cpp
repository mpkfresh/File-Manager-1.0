#include "platform/shellfileoperation.h"

#include <future>
#include <thread>

#ifdef Q_OS_WIN
#include <windows.h>
#include <shobjidl.h>
#endif

namespace filemanager::platform {
namespace {

bool validAbsoluteIdList(const QByteArray& bytes)
{
    if (bytes.size() < 2 || bytes.size() > 1024 * 1024)
        return false;
    qsizetype offset = 0;
    while (offset + 2 <= bytes.size()) {
        const auto* data = reinterpret_cast<const unsigned char*>(bytes.constData() + offset);
        const quint16 itemSize = static_cast<quint16>(data[0])
            | (static_cast<quint16>(data[1]) << 8U);
        if (itemSize == 0)
            return offset + 2 == bytes.size();
        if (itemSize < 2 || offset + itemSize > bytes.size())
            return false;
        offset += itemSize;
    }
    return false;
}

#ifdef Q_OS_WIN
QString systemMessage(HRESULT result)
{
    wchar_t* buffer = nullptr;
    const DWORD length = FormatMessageW(FORMAT_MESSAGE_ALLOCATE_BUFFER
            | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
        nullptr, static_cast<DWORD>(result), 0, reinterpret_cast<wchar_t*>(&buffer), 0, nullptr);
    const QString message = length && buffer
        ? QString::fromWCharArray(buffer, static_cast<qsizetype>(length)).trimmed()
        : QStringLiteral("Windows Shell operation failed (0x%1).")
            .arg(static_cast<quint32>(result), 8, 16, QLatin1Char('0'));
    if (buffer)
        LocalFree(buffer);
    return message;
}

ShellFileOperationResult performSta(ShellFileOperationKind kind,
    const QList<QByteArray>& sourceIdLists, const QByteArray& destinationIdList,
    ShellFileOperationOptions options)
{
    ShellFileOperationResult result;
    const HRESULT initialized = CoInitializeEx(
        nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
    if (FAILED(initialized)) {
        result.nativeCode = initialized;
        result.error = systemMessage(initialized);
        return result;
    }
    IFileOperation* operation = nullptr;
    IShellItem* destinationItem = nullptr;
    QString stage = QStringLiteral("create IFileOperation");
    HRESULT status = CoCreateInstance(CLSID_FileOperation, nullptr, CLSCTX_INPROC_SERVER,
        IID_PPV_ARGS(&operation));
    if (SUCCEEDED(status)) {
        stage = QStringLiteral("set operation flags");
        DWORD flags =
            FOF_SILENT | FOF_NOERRORUI | FOF_NOCONFIRMMKDIR | FOF_RENAMEONCOLLISION
            | FOFX_EARLYFAILURE;
        if (options.requestElevationPrompt)
            flags |= FOFX_SHOWELEVATIONPROMPT;
        status = operation->SetOperationFlags(flags);
    }
    if (SUCCEEDED(status) && options.ownerWindow != 0)
        status = operation->SetOwnerWindow(reinterpret_cast<HWND>(options.ownerWindow));
    if (SUCCEEDED(status)) {
        stage = QStringLiteral("bind destination Shell item identity");
        status = SHCreateItemFromIDList(
            reinterpret_cast<PCIDLIST_ABSOLUTE>(destinationIdList.constData()),
            IID_PPV_ARGS(&destinationItem));
    }
    for (int index = 0; SUCCEEDED(status) && index < sourceIdLists.size(); ++index) {
        IShellItem* sourceItem = nullptr;
        stage = QStringLiteral("bind source Shell item identity %1").arg(index + 1);
        status = SHCreateItemFromIDList(
            reinterpret_cast<PCIDLIST_ABSOLUTE>(sourceIdLists.at(index).constData()),
            IID_PPV_ARGS(&sourceItem));
        if (SUCCEEDED(status)) {
            stage = QStringLiteral("queue Shell operation item %1").arg(index + 1);
            status = kind == ShellFileOperationKind::Move
                ? operation->MoveItem(sourceItem, destinationItem, nullptr, nullptr)
                : operation->CopyItem(sourceItem, destinationItem, nullptr, nullptr);
        }
        if (sourceItem)
            sourceItem->Release();
    }
    if (SUCCEEDED(status)) {
        stage = QStringLiteral("perform Shell operation");
        status = operation->PerformOperations();
    }
    BOOL aborted = FALSE;
    if (operation)
        operation->GetAnyOperationsAborted(&aborted);
    result.aborted = aborted != FALSE;
    result.nativeCode = status;
    result.success = SUCCEEDED(status) && !result.aborted;
    if (!result.success)
        result.error = result.aborted ? QStringLiteral("The Windows Shell operation was aborted.")
                                      : stage + QStringLiteral(": ") + systemMessage(status);
    if (destinationItem)
        destinationItem->Release();
    if (operation)
        operation->Release();
    CoUninitialize();
    return result;
}
#endif

} // namespace

ShellFileOperationResult ShellFileOperation::perform(
    ShellFileOperationKind kind, const QString& source, const QString& destinationDirectory)
{
    const ShellItemReference sourceItem = ShellItem::resolve(source);
    if (!sourceItem.valid)
        return {false, false, sourceItem.nativeCode,
            QStringLiteral("Source Shell item is unavailable: %1").arg(sourceItem.error)};
    const ShellItemReference destinationItem = ShellItem::resolve(destinationDirectory);
    if (!destinationItem.valid || !destinationItem.folder)
        return {false, false, destinationItem.nativeCode,
            QStringLiteral("Destination Shell folder is unavailable: %1")
                .arg(destinationItem.error)};
    return perform(kind, sourceItem, destinationItem);
}

ShellFileOperationResult ShellFileOperation::perform(ShellFileOperationKind kind,
    const ShellItemReference& source, const ShellItemReference& destinationFolder)
{
    return perform(kind, QList<ShellItemReference>{source}, destinationFolder);
}

ShellFileOperationResult ShellFileOperation::perform(ShellFileOperationKind kind,
    const QList<ShellItemReference>& sources, const ShellItemReference& destinationFolder,
    const ShellFileOperationOptions& options)
{
    if (sources.isEmpty() || sources.size() > 32)
        return {false, false, 0, QStringLiteral("Shell operation selection is outside policy.")};
    if (options.requestElevationPrompt && kind != ShellFileOperationKind::Copy) {
        return {false, false, 0,
            QStringLiteral("Elevated Shell retries are restricted to non-destructive copies.")};
    }
    QList<QByteArray> sourceIdLists;
    sourceIdLists.reserve(sources.size());
    for (const auto& source : sources) {
        if (!source.valid || !validAbsoluteIdList(source.idList))
            return {false, false, 0, QStringLiteral("Source Shell item identity is invalid.")};
        sourceIdLists.append(source.idList);
    }
    if (!destinationFolder.valid || !destinationFolder.folder
            || !validAbsoluteIdList(destinationFolder.idList)) {
        return {false, false, 0, QStringLiteral("Destination Shell folder identity is invalid.")};
    }
#ifdef Q_OS_WIN
    std::promise<ShellFileOperationResult> completion;
    std::future<ShellFileOperationResult> future = completion.get_future();
    std::thread worker([kind, sourceIdLists = std::move(sourceIdLists),
                           destinationIdList = destinationFolder.idList, options,
                           completion = std::move(completion)]() mutable {
        completion.set_value(performSta(kind, sourceIdLists, destinationIdList, options));
    });
    worker.join();
    return future.get();
#else
    Q_UNUSED(kind);
    return {false, false, 0, QStringLiteral("Windows Shell file operations are unavailable.")};
#endif
}

} // namespace filemanager::platform
