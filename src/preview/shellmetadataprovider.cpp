#include "preview/shellmetadataprovider.h"
#include "settings/metadatapermissions.h"
#include "platform/nativefilecopy.h"

#include <QFileInfo>
#include <QMetaObject>
#include <QPointer>
#include <QThreadPool>

#ifdef Q_OS_WIN
#include <combaseapi.h>
#include <propkey.h>
#include <propsys.h>
#include <propvarutil.h>
#include <shobjidl_core.h>
#endif

namespace filemanager::preview {
namespace {

#ifdef Q_OS_WIN
QString stringProperty(IPropertyStore* store, const PROPERTYKEY& key)
{
    PROPVARIANT value;
    PropVariantInit(&value);
    QString result;
    if (SUCCEEDED(store->GetValue(key, &value))) {
        PWSTR text = nullptr;
        if (SUCCEEDED(PropVariantToStringAlloc(value, &text))) {
            result = QString::fromWCharArray(text).trimmed();
            CoTaskMemFree(text);
        }
    }
    PropVariantClear(&value);
    return result;
}

quint64 unsignedProperty(IPropertyStore* store, const PROPERTYKEY& key)
{
    PROPVARIANT value;
    PropVariantInit(&value);
    ULONGLONG result = 0;
    if (SUCCEEDED(store->GetValue(key, &value)))
        PropVariantToUInt64(value, &result);
    PropVariantClear(&value);
    return result;
}

MediaMetadata readShellMetadata(const QString& path)
{
    MediaMetadata metadata;
    const HRESULT initialized = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    if (FAILED(initialized) && initialized != RPC_E_CHANGED_MODE)
        return metadata;
    IPropertyStore* store = nullptr;
    const HRESULT opened = SHGetPropertyStoreFromParsingName(
        reinterpret_cast<LPCWSTR>(path.utf16()), nullptr, GPS_BESTEFFORT,
        IID_PPV_ARGS(&store));
    if (SUCCEEDED(opened) && store) {
        metadata.title = stringProperty(store, PKEY_Title);
        metadata.artists = stringProperty(store, PKEY_Author);
        metadata.durationMs = static_cast<qint64>(
            unsignedProperty(store, PKEY_Media_Duration) / 10000);
        const int width = static_cast<int>(unsignedProperty(store, PKEY_Video_FrameWidth));
        const int height = static_cast<int>(unsignedProperty(store, PKEY_Video_FrameHeight));
        if (width > 0 && height > 0)
            metadata.resolution = QSize(width, height);
        store->Release();
    }
    if (SUCCEEDED(initialized))
        CoUninitialize();
    return metadata;
}
#endif

} // namespace

ShellMetadataProvider::ShellMetadataProvider(QObject* parent)
    : QObject(parent)
{
}

QString ShellMetadataProvider::cacheKey(const QString& path) const
{
    const QFileInfo info(path);
    return info.absoluteFilePath() + '|' + QString::number(info.size()) + '|'
        + QString::number(info.lastModified().toMSecsSinceEpoch());
}

std::optional<MediaMetadata> ShellMetadataProvider::request(const QString& path)
{
    if (!filemanager::settings::loadMetadataPermissions().allowWindowsShellHandlers)
        return std::nullopt;
    const QFileInfo info(path);
    if (!info.isFile() || info.size() > MaximumSourceBytes
            || filemanager::platform::nativePathKind(path)
                != filemanager::platform::NativePathKind::Regular)
        return std::nullopt;
    const QString key = cacheKey(path);
    if (const MediaMetadata* cached = cache_.object(key))
        return *cached;
    if (pending_.contains(key))
        return std::nullopt;
    pending_.insert(key);
    QPointer<ShellMetadataProvider> guard(this);
    const QString absolutePath = info.absoluteFilePath();
    QThreadPool::globalInstance()->start([guard, key, absolutePath] {
        MediaMetadata metadata;
#ifdef Q_OS_WIN
        if (filemanager::platform::nativePathKind(absolutePath)
                == filemanager::platform::NativePathKind::Regular)
            metadata = readShellMetadata(absolutePath);
#endif
        if (guard) {
            QMetaObject::invokeMethod(guard, [guard, key, absolutePath, metadata] {
                if (!guard)
                    return;
                guard->pending_.remove(key);
                if (filemanager::settings::loadMetadataPermissions().allowWindowsShellHandlers)
                    guard->cache_.insert(key, new MediaMetadata(metadata));
                emit guard->metadataReady(absolutePath);
            });
        }
    });
    return std::nullopt;
}

} // namespace filemanager::preview
