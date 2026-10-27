#include "platform/windowsshare.h"

#include "platform/cloudfilestatus.h"

#include <QDir>
#include <QFileInfo>
#include <QMetaObject>
#include <QPointer>
#include <QSet>
#include <QThread>
#include <QTimer>
#include <vector>

#ifdef Q_OS_WIN
#include <windows.h>
#include <roapi.h>
#include <shobjidl_core.h>
#include <winrt/Windows.ApplicationModel.DataTransfer.h>
#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/Windows.Storage.h>
#include <winrt/base.h>
#endif

namespace filemanager::platform {

namespace {

constexpr int maximumShareItems = 32;
constexpr qsizetype maximumPathBytes = 24 * 1024;

#ifdef Q_OS_WIN
using StorageItem = winrt::Windows::Storage::IStorageItem;

struct PreparedShare final
{
    std::vector<StorageItem> items;
    int providerBackedItems = 0;
    QString error;
};

QString winrtErrorText(const winrt::hresult_error& error)
{
    const QString message = QString::fromWCharArray(error.message().c_str()).trimmed();
    return message.isEmpty()
        ? QString("Windows sharing failed (0x%1).")
              .arg(static_cast<quint32>(error.code()), 8, 16, QLatin1Char('0'))
        : message;
}

PreparedShare prepareShare(const QStringList& paths)
{
    PreparedShare result;
    try {
        winrt::init_apartment(winrt::apartment_type::multi_threaded);
        struct ApartmentGuard final {
            ~ApartmentGuard() { winrt::uninit_apartment(); }
        } guard;
        result.items.reserve(static_cast<size_t>(paths.size()));
        for (const QString& path : paths) {
            const std::wstring nativePath = QDir::toNativeSeparators(path).toStdWString();
            if (QFileInfo(path).isDir()) {
                result.items.emplace_back(
                    winrt::Windows::Storage::StorageFolder::GetFolderFromPathAsync(nativePath).get());
            } else {
                result.items.emplace_back(
                    winrt::Windows::Storage::StorageFile::GetFileFromPathAsync(nativePath).get());
            }
        }
    } catch (const winrt::hresult_error& error) {
        result.items.clear();
        result.error = winrtErrorText(error);
    } catch (...) {
        result.items.clear();
        result.error = "Windows could not prepare the selected items for sharing.";
    }
    return result;
}
#endif

} // namespace

ShareSelectionValidation validateWindowsShareSelection(const QStringList& paths)
{
    if (paths.isEmpty() || paths.size() > maximumShareItems)
        return {false, {}, 0, "Share requires between 1 and 32 selected items."};

    QStringList normalized;
    QSet<QString> seen;
    qsizetype pathBytes = 0;
    int providerBackedItems = 0;
    for (const QString& path : paths) {
        const QFileInfo info(QDir::cleanPath(QDir::fromNativeSeparators(path)));
        if (!info.isAbsolute() || !info.exists())
            return {false, {}, 0, "A selected item is unavailable or not an absolute path."};
        const QString absolute = info.absoluteFilePath();
        pathBytes += absolute.size() * 2;
        if (pathBytes > maximumPathBytes)
            return {false, {}, 0, "The selected paths exceed the Windows sharing limit."};
        const QString comparison = QDir::toNativeSeparators(absolute).toCaseFolded();
        if (seen.contains(comparison))
            continue;
        seen.insert(comparison);
        normalized.append(absolute);
        if (inspectCloudFileStatus(absolute).providerBacked)
            ++providerBackedItems;
    }
    return {true, normalized, providerBackedItems, {}};
}

bool windowsShareContractAvailable(QString* error)
{
#ifdef Q_OS_WIN
    bool uninitialize = false;
    const HRESULT initialized = RoInitialize(RO_INIT_SINGLETHREADED);
    if (SUCCEEDED(initialized))
        uninitialize = true;
    else if (initialized != RPC_E_CHANGED_MODE) {
        if (error)
            *error = QString("Windows sharing could not initialize (0x%1).")
                .arg(static_cast<quint32>(initialized), 8, 16, QLatin1Char('0'));
        return false;
    }
    try {
        bool available = false;
        {
            const auto interop = winrt::get_activation_factory<
                winrt::Windows::ApplicationModel::DataTransfer::DataTransferManager,
                IDataTransferManagerInterop>();
            available = static_cast<bool>(interop);
        }
        if (uninitialize)
            RoUninitialize();
        return available;
    } catch (const winrt::hresult_error& failure) {
        if (uninitialize)
            RoUninitialize();
        if (error)
            *error = winrtErrorText(failure);
        return false;
    }
#else
    if (error)
        *error = "Windows sharing is available only on Windows.";
    return false;
#endif
}

struct WindowsShareController::State final
{
#ifdef Q_OS_WIN
    winrt::Windows::ApplicationModel::DataTransfer::DataTransferManager manager{nullptr};
    winrt::event_token dataRequested{};
#endif
    quint64 generation = 0;
    bool roInitialized = false;
};

WindowsShareController::WindowsShareController(QObject* parent)
    : QObject(parent), state_(std::make_unique<State>())
{
}

WindowsShareController::~WindowsShareController()
{
#ifdef Q_OS_WIN
    ++state_->generation;
    if (state_->manager && state_->dataRequested.value != 0)
        state_->manager.DataRequested(state_->dataRequested);
    state_->manager = nullptr;
    if (state_->roInitialized)
        RoUninitialize();
#endif
}

void WindowsShareController::showShareUi(quintptr windowId, const QStringList& paths)
{
    if (paths.isEmpty() || paths.size() > maximumShareItems) {
        emit shareFailed("Share requires between 1 and 32 selected items.");
        return;
    }
    if (windowId == 0) {
        emit shareFailed("Windows sharing requires a visible application window.");
        return;
    }

#ifdef Q_OS_WIN
    const quint64 generation = ++state_->generation;
    if (state_->manager && state_->dataRequested.value != 0)
        state_->manager.DataRequested(state_->dataRequested);
    state_->dataRequested = {};
    state_->manager = nullptr;
    QPointer<WindowsShareController> guard(this);
    QThread* worker = QThread::create([guard, generation, windowId, selected = paths] {
        const ShareSelectionValidation validation = validateWindowsShareSelection(selected);
        PreparedShare prepared = validation.valid ? prepareShare(validation.absolutePaths)
                                                  : PreparedShare{{}, 0, validation.error};
        prepared.providerBackedItems = validation.providerBackedItems;
        if (!guard)
            return;
        QMetaObject::invokeMethod(guard, [guard, generation, windowId,
                                            prepared = std::move(prepared)]() mutable {
            if (!guard || guard->state_->generation != generation)
                return;
            if (!prepared.error.isEmpty() || prepared.items.empty()) {
                emit guard->shareFailed(prepared.error.isEmpty()
                        ? "Windows could not prepare the selected items for sharing."
                        : prepared.error);
                return;
            }
            try {
                if (!guard->state_->roInitialized) {
                    const HRESULT initialized = RoInitialize(RO_INIT_SINGLETHREADED);
                    if (FAILED(initialized) && initialized != RPC_E_CHANGED_MODE)
                        winrt::check_hresult(initialized);
                    guard->state_->roInitialized = SUCCEEDED(initialized);
                }
                if (guard->state_->manager && guard->state_->dataRequested.value != 0)
                    guard->state_->manager.DataRequested(guard->state_->dataRequested);
                guard->state_->dataRequested = {};
                guard->state_->manager = nullptr;

                const auto interop = winrt::get_activation_factory<
                    winrt::Windows::ApplicationModel::DataTransfer::DataTransferManager,
                    IDataTransferManagerInterop>();
                winrt::Windows::ApplicationModel::DataTransfer::DataTransferManager manager{nullptr};
                winrt::check_hresult(interop->GetForWindow(
                    reinterpret_cast<HWND>(windowId),
                    winrt::guid_of<winrt::Windows::ApplicationModel::DataTransfer::
                        IDataTransferManager>(),
                    winrt::put_abi(manager)));
                const auto items = winrt::single_threaded_vector<StorageItem>(
                    std::move(prepared.items));
                const int itemCount = static_cast<int>(items.Size());
                const int providerBackedItems = prepared.providerBackedItems;
                const winrt::event_token token = manager.DataRequested(
                    [items, guard, generation](const auto&, const auto& args) {
                        auto request = args.Request();
                        try {
                            auto package = request.Data();
                            package.Properties().Title(L"Share from File Manager");
                            package.SetStorageItems(items, true);
                        } catch (...) {
                            request.FailWithDisplayText(
                                L"File Manager could not provide the selected items.");
                        }
                        QMetaObject::invokeMethod(guard, [guard, generation] {
                            if (!guard || guard->state_->generation != generation)
                                return;
                            if (guard->state_->manager
                                    && guard->state_->dataRequested.value != 0) {
                                guard->state_->manager.DataRequested(
                                    guard->state_->dataRequested);
                            }
                            guard->state_->dataRequested = {};
                            guard->state_->manager = nullptr;
                        }, Qt::QueuedConnection);
                    });
                guard->state_->manager = manager;
                guard->state_->dataRequested = token;
                winrt::check_hresult(
                    interop->ShowShareUIForWindow(reinterpret_cast<HWND>(windowId)));
                emit guard->shareUiShown(itemCount, providerBackedItems);

                QTimer::singleShot(5 * 60 * 1000, guard, [guard, generation] {
                    if (!guard || guard->state_->generation != generation)
                        return;
                    if (guard->state_->manager && guard->state_->dataRequested.value != 0)
                        guard->state_->manager.DataRequested(guard->state_->dataRequested);
                    guard->state_->dataRequested = {};
                    guard->state_->manager = nullptr;
                });
            } catch (const winrt::hresult_error& error) {
                if (guard->state_->manager && guard->state_->dataRequested.value != 0)
                    guard->state_->manager.DataRequested(guard->state_->dataRequested);
                guard->state_->dataRequested = {};
                guard->state_->manager = nullptr;
                emit guard->shareFailed(winrtErrorText(error));
            } catch (...) {
                if (guard->state_->manager && guard->state_->dataRequested.value != 0)
                    guard->state_->manager.DataRequested(guard->state_->dataRequested);
                guard->state_->dataRequested = {};
                guard->state_->manager = nullptr;
                emit guard->shareFailed("Windows could not open the Share panel.");
            }
        }, Qt::QueuedConnection);
    });
    connect(worker, &QThread::finished, worker, &QObject::deleteLater);
    worker->start();
#else
    Q_UNUSED(windowId);
    emit shareFailed("Windows sharing is available only on Windows.");
#endif
}

} // namespace filemanager::platform
