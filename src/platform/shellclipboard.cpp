#include "platform/shellclipboard.h"

#include <QApplication>
#include <QClipboard>
#include <QMimeData>
#include <QUrl>

#include <cstring>

#ifdef Q_OS_WIN
#include <windows.h>
#include <ole2.h>
#include <shlobj.h>
#include <shobjidl.h>
#endif

namespace filemanager::platform {
namespace {

constexpr auto InternalCutMimeType = "application/x-file-manager-cut";
constexpr auto PerformedDropEffectMimeType =
    "application/x-qt-windows-mime;value=\"Performed DropEffect\"";
constexpr auto PasteSucceededMimeType =
    "application/x-qt-windows-mime;value=\"Paste Succeeded\"";
constexpr quint32 DropEffectCopy = 1;
constexpr quint32 DropEffectMove = 2;

QByteArray encodedDropEffect(ShellTransferEffect effect)
{
    const quint32 value = effect == ShellTransferEffect::Move ? DropEffectMove : DropEffectCopy;
    QByteArray bytes(4, Qt::Uninitialized);
    bytes[0] = static_cast<char>(value & 0xffU);
    bytes[1] = static_cast<char>((value >> 8U) & 0xffU);
    bytes[2] = static_cast<char>((value >> 16U) & 0xffU);
    bytes[3] = static_cast<char>((value >> 24U) & 0xffU);
    return bytes;
}

std::optional<quint32> decodedDropEffect(const QByteArray& bytes)
{
    if (bytes.size() < 4)
        return std::nullopt;
    return static_cast<quint32>(static_cast<unsigned char>(bytes[0]))
        | (static_cast<quint32>(static_cast<unsigned char>(bytes[1])) << 8U)
        | (static_cast<quint32>(static_cast<unsigned char>(bytes[2])) << 16U)
        | (static_cast<quint32>(static_cast<unsigned char>(bytes[3])) << 24U);
}

#ifdef Q_OS_WIN
HRESULT getOleClipboardBounded(IDataObject** dataObject)
{
    if (!dataObject)
        return E_POINTER;
    *dataObject = nullptr;
    constexpr int attempts = 6;
    HRESULT status = E_FAIL;
    for (int attempt = 0; attempt < attempts; ++attempt) {
        status = OleGetClipboard(dataObject);
        if (SUCCEEDED(status) && *dataObject)
            return status;
        if (*dataObject) {
            (*dataObject)->Release();
            *dataObject = nullptr;
        }
        if (status != CLIPBRD_E_CANT_OPEN || attempt + 1 == attempts)
            break;
        Sleep(10);
    }
    return status;
}

QString itemDisplayName(IShellItem* item, SIGDN form)
{
    PWSTR value = nullptr;
    const HRESULT status = item->GetDisplayName(form, &value);
    const QString result = SUCCEEDED(status) && value ? QString::fromWCharArray(value) : QString{};
    if (value)
        CoTaskMemFree(value);
    return result;
}

ShellItemReference snapshotShellItem(IShellItem* item)
{
    ShellItemReference result;
    PIDLIST_ABSOLUTE idList = nullptr;
    const HRESULT identityStatus = SHGetIDListFromObject(item, &idList);
    result.nativeCode = identityStatus;
    if (FAILED(identityStatus) || !idList) {
        result.error = QStringLiteral("A clipboard Shell item did not provide an absolute identity.");
        return result;
    }
    const UINT byteCount = ILGetSize(idList);
    if (byteCount >= 2 && byteCount <= 1024U * 1024U)
        result.idList = QByteArray(reinterpret_cast<const char*>(idList), byteCount);
    CoTaskMemFree(idList);
    result.parsingName = itemDisplayName(item, SIGDN_DESKTOPABSOLUTEPARSING);
    result.fileSystemPath = itemDisplayName(item, SIGDN_FILESYSPATH);
    result.displayName = itemDisplayName(item, SIGDN_NORMALDISPLAY);
    result.fileSystem = !result.fileSystemPath.isEmpty();
    SFGAOF attributes = 0;
    if (SUCCEEDED(item->GetAttributes(SFGAO_FOLDER, &attributes)))
        result.folder = (attributes & SFGAO_FOLDER) != 0;
    result.valid = !result.idList.isEmpty() && !result.parsingName.isEmpty();
    if (!result.valid)
        result.error = QStringLiteral("A clipboard Shell item exceeded identity limits or had no stable name.");
    return result;
}

bool boundedIdList(const unsigned char* bytes, qsizetype available, qsizetype* length)
{
    qsizetype offset = 0;
    while (offset + 2 <= available) {
        const quint16 itemSize = static_cast<quint16>(bytes[offset])
            | (static_cast<quint16>(bytes[offset + 1]) << 8U);
        if (itemSize == 0) {
            *length = offset + 2;
            return true;
        }
        if (itemSize < 2 || offset + itemSize > available)
            return false;
        offset += itemSize;
    }
    return false;
}

void extractShellIdListBytes(const unsigned char* bytes, qsizetype byteCount,
    int maximumItems, ShellClipboardItems* result)
{
    result->available = true;
    if (!bytes || byteCount < 12 || byteCount > 16 * 1024 * 1024) {
        result->error = QStringLiteral(
            "The Shell ID-list payload is outside the 16 MiB policy limit.");
        return;
    }
    quint32 count = 0;
    std::memcpy(&count, bytes, sizeof(count));
    const quint64 headerBytes = sizeof(quint32)
        + (static_cast<quint64>(count) + 1U) * sizeof(quint32);
    if (count == 0 || count > static_cast<quint32>(maximumItems)
            || headerBytes > static_cast<quint64>(byteCount)) {
        result->error = QStringLiteral("The Shell ID-list item count is outside policy.");
        return;
    }
    QList<quint32> offsets;
    offsets.reserve(static_cast<qsizetype>(count) + 1);
    for (quint32 index = 0; index <= count; ++index) {
        quint32 value = 0;
        std::memcpy(&value, bytes + sizeof(quint32) + index * sizeof(quint32),
            sizeof(value));
        offsets.append(value);
    }
    qsizetype parentLength = 0;
    const quint32 parentOffset = offsets.constFirst();
    bool valid = parentOffset >= headerBytes
        && parentOffset < static_cast<quint32>(byteCount)
        && boundedIdList(bytes + parentOffset, byteCount - parentOffset, &parentLength);
    for (quint32 index = 1; valid && index <= count; ++index) {
        const quint32 childOffset = offsets.at(index);
        qsizetype childLength = 0;
        valid = childOffset >= headerBytes
            && childOffset < static_cast<quint32>(byteCount)
            && boundedIdList(bytes + childOffset, byteCount - childOffset, &childLength);
        if (!valid)
            break;
        PIDLIST_ABSOLUTE absolute = ILCombine(
            reinterpret_cast<PCIDLIST_ABSOLUTE>(bytes + parentOffset),
            reinterpret_cast<PCUIDLIST_RELATIVE>(bytes + childOffset));
        IShellItem* item = nullptr;
        if (!absolute || FAILED(SHCreateItemFromIDList(absolute, IID_PPV_ARGS(&item)))) {
            if (absolute)
                CoTaskMemFree(absolute);
            valid = false;
            break;
        }
        ShellItemReference snapshot = snapshotShellItem(item);
        item->Release();
        CoTaskMemFree(absolute);
        if (!snapshot.valid) {
            result->error = snapshot.error;
            valid = false;
            break;
        }
        result->items.append(std::move(snapshot));
    }
    if (!valid && result->error.isEmpty())
        result->error = QStringLiteral("The Shell ID-list payload is malformed.");
    if (!result->error.isEmpty())
        result->items.clear();
}

void extractShellIdListArray(IDataObject* dataObject, int maximumItems,
    ShellClipboardItems* result)
{
    const CLIPFORMAT shellIdList = static_cast<CLIPFORMAT>(
        RegisterClipboardFormatW(CFSTR_SHELLIDLIST));
    if (shellIdList == 0)
        return;
    FORMATETC format{shellIdList, nullptr, DVASPECT_CONTENT, -1, TYMED_HGLOBAL};
    if (FAILED(dataObject->QueryGetData(&format)))
        return;
    result->available = true;
    STGMEDIUM medium{};
    const HRESULT dataStatus = dataObject->GetData(&format, &medium);
    if (FAILED(dataStatus) || medium.tymed != TYMED_HGLOBAL || !medium.hGlobal) {
        result->error = QStringLiteral("Windows could not read the Shell ID-list clipboard data (0x%1).")
            .arg(static_cast<quint32>(dataStatus), 8, 16, QLatin1Char('0'));
        if (SUCCEEDED(dataStatus))
            ReleaseStgMedium(&medium);
        return;
    }
    const SIZE_T byteCount = GlobalSize(medium.hGlobal);
    const auto* bytes = static_cast<const unsigned char*>(GlobalLock(medium.hGlobal));
    extractShellIdListBytes(bytes, static_cast<qsizetype>(byteCount), maximumItems, result);
    if (bytes)
        GlobalUnlock(medium.hGlobal);
    ReleaseStgMedium(&medium);
    if (!result->error.isEmpty())
        result->items.clear();
}

bool setDropEffectFormat(IDataObject* dataObject, CLIPFORMAT format, quint32 value,
    QString* error)
{
    HGLOBAL storage = GlobalAlloc(GMEM_MOVEABLE | GMEM_ZEROINIT, sizeof(DWORD));
    if (!storage) {
        if (error)
            *error = QStringLiteral("Windows could not allocate the OLE completion value.");
        return false;
    }
    void* bytes = GlobalLock(storage);
    if (!bytes) {
        GlobalFree(storage);
        if (error)
            *error = QStringLiteral("Windows could not lock the OLE completion value.");
        return false;
    }
    *static_cast<DWORD*>(bytes) = value;
    GlobalUnlock(storage);
    FORMATETC formatDescription{format, nullptr, DVASPECT_CONTENT, -1, TYMED_HGLOBAL};
    STGMEDIUM medium{};
    medium.tymed = TYMED_HGLOBAL;
    medium.hGlobal = storage;
    const HRESULT status = dataObject->SetData(&formatDescription, &medium, TRUE);
    if (FAILED(status)) {
        GlobalFree(storage);
        if (error) {
            *error = QStringLiteral("Windows rejected the OLE completion effect (0x%1).")
                .arg(static_cast<quint32>(status), 8, 16, QLatin1Char('0'));
        }
        return false;
    }
    return true;
}
#endif

} // namespace

QString ShellClipboard::preferredDropEffectMimeType()
{
    // Qt's Windows MIME bridge maps this name to the registered Shell clipboard format.
    return QStringLiteral("application/x-qt-windows-mime;value=\"Preferred DropEffect\"");
}

QString ShellClipboard::shellIdListMimeType()
{
    return QStringLiteral("application/x-qt-windows-mime;value=\"Shell IDList Array\"");
}

ShellClipboardItems ShellClipboard::shellItems(const QMimeData* mimeData, int maximumItems)
{
    ShellClipboardItems result;
    if (!mimeData || !mimeData->hasFormat(shellIdListMimeType()))
        return result;
    if (maximumItems <= 0 || maximumItems > 256) {
        result.error = QStringLiteral("The Shell-item limit is outside policy.");
        return result;
    }
#ifdef Q_OS_WIN
    const QByteArray bytes = mimeData->data(shellIdListMimeType());
    extractShellIdListBytes(reinterpret_cast<const unsigned char*>(bytes.constData()),
        bytes.size(), maximumItems, &result);
#else
    Q_UNUSED(maximumItems);
    result.error = QStringLiteral("Windows Shell items are unavailable on this platform.");
#endif
    return result;
}

void ShellClipboard::populateFileTransfer(QMimeData* mimeData, const QStringList& paths,
                                          ShellTransferEffect effect)
{
    if (!mimeData)
        return;

    QList<QUrl> urls;
    urls.reserve(paths.size());
    for (const QString& path : paths)
        urls.append(QUrl::fromLocalFile(path));
    mimeData->setUrls(urls);
    mimeData->setData(preferredDropEffectMimeType(), encodedDropEffect(effect));
    mimeData->setData(InternalCutMimeType,
        effect == ShellTransferEffect::Move ? QByteArrayLiteral("1") : QByteArrayLiteral("0"));
}

bool ShellClipboard::populateShellItems(QMimeData* mimeData,
    const QList<ShellItemReference>& items, QString* error)
{
    if (!mimeData || items.isEmpty() || items.size() > 256) {
        if (error)
            *error = QStringLiteral("The virtual Shell selection is empty or exceeds 256 items.");
        return false;
    }
#ifdef Q_OS_WIN
    quint64 payloadBytes = sizeof(quint32)
        + static_cast<quint64>(items.size() + 1) * sizeof(quint32) + 2;
    for (const auto& item : items) {
        qsizetype length = 0;
        if (!item.valid || item.idList.size() < 2 || item.idList.size() > 1024 * 1024
                || !boundedIdList(
                    reinterpret_cast<const unsigned char*>(item.idList.constData()),
                    item.idList.size(), &length)
                || length != item.idList.size()) {
            if (error)
                *error = QStringLiteral("A virtual Shell item has an invalid absolute identity.");
            return false;
        }
        payloadBytes += static_cast<quint64>(item.idList.size());
    }
    if (payloadBytes > 16U * 1024U * 1024U) {
        if (error)
            *error = QStringLiteral("The virtual Shell selection exceeds the 16 MiB identity limit.");
        return false;
    }
    QByteArray payload(static_cast<qsizetype>(payloadBytes), Qt::Uninitialized);
    auto* bytes = reinterpret_cast<unsigned char*>(payload.data());
    const quint32 count = static_cast<quint32>(items.size());
    std::memcpy(bytes, &count, sizeof(count));
    quint32 offset = sizeof(quint32)
        + static_cast<quint32>(items.size() + 1) * sizeof(quint32);
    std::memcpy(bytes + sizeof(quint32), &offset, sizeof(offset));
    bytes[offset] = 0;
    bytes[offset + 1] = 0;
    offset += 2;
    for (int index = 0; index < items.size(); ++index) {
        std::memcpy(bytes + sizeof(quint32) + (index + 1) * sizeof(quint32),
            &offset, sizeof(offset));
        const QByteArray& idList = items.at(index).idList;
        std::memcpy(bytes + offset, idList.constData(), static_cast<size_t>(idList.size()));
        offset += static_cast<quint32>(idList.size());
    }
    mimeData->setData(shellIdListMimeType(), payload);
    mimeData->setData(preferredDropEffectMimeType(),
        encodedDropEffect(ShellTransferEffect::Copy));
    mimeData->setData(InternalCutMimeType, QByteArrayLiteral("0"));
    if (error)
        error->clear();
    return true;
#else
    Q_UNUSED(items);
    if (error)
        *error = QStringLiteral("Windows Shell items are unavailable on this platform.");
    return false;
#endif
}

std::optional<ShellTransferEffect> ShellClipboard::preferredEffect(const QMimeData* mimeData)
{
    if (!mimeData)
        return std::nullopt;

    const auto effect = decodedDropEffect(mimeData->data(preferredDropEffectMimeType()));
    if (effect.has_value()) {
        if ((*effect & DropEffectMove) != 0U)
            return ShellTransferEffect::Move;
        if ((*effect & DropEffectCopy) != 0U)
            return ShellTransferEffect::Copy;
    }

    if (mimeData->hasFormat(InternalCutMimeType))
        return mimeData->data(InternalCutMimeType) == QByteArrayLiteral("1")
            ? ShellTransferEffect::Move : ShellTransferEffect::Copy;
    return std::nullopt;
}

ShellClipboardItems ShellClipboard::shellItems(int maximumItems)
{
    ShellClipboardItems result;
#ifdef Q_OS_WIN
    if (maximumItems <= 0 || maximumItems > 256) {
        result.error = QStringLiteral("The clipboard Shell-item limit is outside policy.");
        return result;
    }
    IDataObject* dataObject = nullptr;
    const HRESULT clipboardStatus = getOleClipboardBounded(&dataObject);
    if (FAILED(clipboardStatus) || !dataObject) {
        result.error = QStringLiteral("Windows could not access the OLE clipboard (0x%1).")
            .arg(static_cast<quint32>(clipboardStatus), 8, 16, QLatin1Char('0'));
        return result;
    }
    IShellItemArray* array = nullptr;
    const HRESULT arrayStatus = SHCreateShellItemArrayFromDataObject(
        dataObject, IID_PPV_ARGS(&array));
    if (FAILED(arrayStatus) || !array) {
        extractShellIdListArray(dataObject, maximumItems, &result);
        dataObject->Release();
        return result;
    }
    result.available = true;
    DWORD count = 0;
    HRESULT status = array->GetCount(&count);
    if (SUCCEEDED(status) && count > static_cast<DWORD>(maximumItems)) {
        result.error = QStringLiteral("The clipboard contains more than %1 Shell items.")
            .arg(maximumItems);
        status = HRESULT_FROM_WIN32(ERROR_BUFFER_OVERFLOW);
    }
    qsizetype aggregateIdentityBytes = 0;
    for (DWORD index = 0; SUCCEEDED(status) && index < count; ++index) {
        IShellItem* item = nullptr;
        status = array->GetItemAt(index, &item);
        if (FAILED(status) || !item)
            break;
        ShellItemReference snapshot = snapshotShellItem(item);
        item->Release();
        aggregateIdentityBytes += snapshot.idList.size();
        if (!snapshot.valid || aggregateIdentityBytes > 16 * 1024 * 1024) {
            result.error = snapshot.error.isEmpty()
                ? QStringLiteral("Clipboard Shell-item identities exceed the 16 MiB policy limit.")
                : snapshot.error;
            status = E_INVALIDARG;
            break;
        }
        result.items.append(std::move(snapshot));
    }
    if (FAILED(status)) {
        result.items.clear();
        if (result.error.isEmpty())
            result.error = QStringLiteral("Windows could not enumerate clipboard Shell items (0x%1).")
                .arg(static_cast<quint32>(status), 8, 16, QLatin1Char('0'));
    } else if (result.items.isEmpty()) {
        result.error = QStringLiteral("The clipboard Shell selection is empty.");
    }
    array->Release();
    dataObject->Release();
#else
    Q_UNUSED(maximumItems);
    result.error = QStringLiteral("Windows Shell clipboard items are unavailable on this platform.");
#endif
    return result;
}

quint64 ShellClipboard::sequenceNumber()
{
#ifdef Q_OS_WIN
    return GetClipboardSequenceNumber();
#else
    return 0;
#endif
}

bool ShellClipboard::publishPerformedEffect(ShellTransferEffect effect,
    quint64 expectedSequenceNumber, QString* error)
{
#ifdef Q_OS_WIN
    if (expectedSequenceNumber == 0
            || expectedSequenceNumber != GetClipboardSequenceNumber()) {
        if (error)
            *error = QStringLiteral("Clipboard ownership changed before the transfer completed.");
        return false;
    }
    IDataObject* dataObject = nullptr;
    const HRESULT clipboardStatus = getOleClipboardBounded(&dataObject);
    if (FAILED(clipboardStatus) || !dataObject) {
        if (error) {
            *error = QStringLiteral("Windows could not access the OLE clipboard (0x%1).")
                .arg(static_cast<quint32>(clipboardStatus), 8, 16, QLatin1Char('0'));
        }
        return false;
    }
    if (expectedSequenceNumber != GetClipboardSequenceNumber()) {
        dataObject->Release();
        if (error)
            *error = QStringLiteral("Clipboard ownership changed before the transfer completed.");
        return false;
    }
    const quint32 value = effect == ShellTransferEffect::Move ? DropEffectMove : DropEffectCopy;
    const CLIPFORMAT performed = static_cast<CLIPFORMAT>(
        RegisterClipboardFormatW(CFSTR_PERFORMEDDROPEFFECT));
    const CLIPFORMAT succeeded = static_cast<CLIPFORMAT>(
        RegisterClipboardFormatW(CFSTR_PASTESUCCEEDED));
    bool success = performed != 0 && succeeded != 0
        && setDropEffectFormat(dataObject, performed, value, error)
        && setDropEffectFormat(dataObject, succeeded, value, error);
    if (!success && QApplication::clipboard()->ownsClipboard()) {
        QMimeData* owned = const_cast<QMimeData*>(QApplication::clipboard()->mimeData());
        if (owned) {
            owned->setData(QString::fromLatin1(PerformedDropEffectMimeType),
                encodedDropEffect(effect));
            owned->setData(QString::fromLatin1(PasteSucceededMimeType),
                encodedDropEffect(effect));
            success = true;
            if (error)
                error->clear();
        }
    }
    if (!success && error && error->isEmpty())
        *error = QStringLiteral("Windows could not register the OLE completion formats.");
    dataObject->Release();
    return success;
#else
    Q_UNUSED(effect);
    Q_UNUSED(expectedSequenceNumber);
    if (error)
        *error = QStringLiteral("OLE clipboard completion is unavailable on this platform.");
    return false;
#endif
}

} // namespace filemanager::platform
