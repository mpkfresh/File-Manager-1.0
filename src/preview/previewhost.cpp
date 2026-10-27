#include "preview/previewservice.h"
#include "preview/documentmetadataprovider.h"

#include <QBuffer>
#include <QCoreApplication>
#include <QFileInfo>
#include <QImageReader>
#include <QIODevice>
#include <QJsonDocument>
#include <QJsonObject>
#include <QMediaMetaData>
#include <QMediaPlayer>
#include <QTimer>
#include <QUrl>
#include <QtEndian>

#include <cstdio>
#include <optional>

#ifdef Q_OS_WIN
#include <qt_windows.h>
#endif

namespace {
#ifdef Q_OS_WIN
class WinHandleDevice final : public QIODevice
{
public:
    WinHandleDevice(HANDLE handle, qint64 size)
        : handle_(handle), size_(size)
    {
        open(QIODevice::ReadOnly);
    }

    qint64 size() const override { return size_; }
    bool isSequential() const override { return false; }
    bool seek(qint64 position) override
    {
        LARGE_INTEGER distance{};
        distance.QuadPart = position;
        return SetFilePointerEx(handle_, distance, nullptr, FILE_BEGIN)
            && QIODevice::seek(position);
    }

protected:
    qint64 readData(char* data, qint64 maximumSize) override
    {
        DWORD read = 0;
        const DWORD request = static_cast<DWORD>(qMin<qint64>(maximumSize, MAXDWORD));
        return ReadFile(handle_, data, request, &read, nullptr) ? static_cast<qint64>(read) : -1;
    }
    qint64 writeData(const char*, qint64) override { return -1; }

private:
    HANDLE handle_ = nullptr;
    qint64 size_ = 0;
};
#endif

bool writePayload(const QJsonObject& object, int maximumBytes)
{
    const QByteArray payload = QJsonDocument(object).toJson(QJsonDocument::Compact);
    if (payload.size() > maximumBytes)
        return false;
#ifdef Q_OS_WIN
    const HANDLE output = GetStdHandle(STD_OUTPUT_HANDLE);
    qsizetype offset = 0;
    while (output && output != INVALID_HANDLE_VALUE && offset < payload.size()) {
        DWORD written = 0;
        const DWORD requested = static_cast<DWORD>(qMin<qsizetype>(payload.size() - offset,
            64 * 1024));
        if (!WriteFile(output, payload.constData() + offset, requested, &written, nullptr)
                || written == 0)
            return false;
        offset += written;
    }
    return offset == payload.size();
#else
    return std::fwrite(payload.constData(), 1, static_cast<size_t>(payload.size()), stdout)
        == static_cast<size_t>(payload.size());
#endif
}

QString artists(const QMediaMetaData& metadata)
{
    QStringList values;
    const QList<QMediaMetaData::Key> keys = {QMediaMetaData::AlbumArtist,
        QMediaMetaData::ContributingArtist, QMediaMetaData::LeadPerformer, QMediaMetaData::Author};
    for (const auto key : keys) {
        const QVariant value = metadata.value(key);
        if (value.metaType() == QMetaType::fromType<QStringList>()) values.append(value.toStringList());
        else if (!value.toString().trimmed().isEmpty()) values.append(value.toString().trimmed());
    }
    values.removeDuplicates();
    return values.join(", ").left(32768);
}

std::optional<QJsonObject> waveMetadata(QIODevice* source, qint64 fileSize)
{
    if (!source || fileSize < 44 || !source->seek(0))
        return std::nullopt;
    const QByteArray header = source->read(12);
    if (header.size() != 12 || header.first(4) != "RIFF" || header.sliced(8, 4) != "WAVE")
        return std::nullopt;
    quint32 byteRate = 0;
    quint32 dataBytes = 0;
    int chunks = 0;
    qint64 inspected = 12;
    while (inspected + 8 <= fileSize && inspected <= 1024 * 1024 && ++chunks <= 64) {
        const QByteArray chunk = source->read(8);
        if (chunk.size() != 8)
            return std::nullopt;
        inspected += 8;
        const quint32 chunkSize = qFromLittleEndian<quint32>(
            reinterpret_cast<const uchar*>(chunk.constData() + 4));
        if (chunkSize > static_cast<quint64>(fileSize - inspected))
            return std::nullopt;
        if (chunk.first(4) == "fmt ") {
            if (chunkSize < 16 || chunkSize > 4096)
                return std::nullopt;
            const QByteArray format = source->read(chunkSize);
            if (format.size() != static_cast<qsizetype>(chunkSize))
                return std::nullopt;
            byteRate = qFromLittleEndian<quint32>(
                reinterpret_cast<const uchar*>(format.constData() + 8));
        } else if (chunk.first(4) == "data") {
            dataBytes = chunkSize;
            if (byteRate != 0)
                break;
            if (!source->seek(source->pos() + chunkSize))
                return std::nullopt;
        } else if (!source->seek(source->pos() + chunkSize)) {
            return std::nullopt;
        }
        inspected += chunkSize;
        if ((chunkSize & 1U) != 0) {
            if (!source->seek(source->pos() + 1))
                return std::nullopt;
            ++inspected;
        }
    }
    if (byteRate == 0 || dataBytes == 0)
        return std::nullopt;
    return QJsonObject{{"durationMs", static_cast<qint64>(dataBytes) * 1000 / byteRate},
        {"title", QString{}}, {"artists", QString{}}, {"width", 0}, {"height", 0}};
}
}

int main(int argc, char** argv)
{
    QCoreApplication application(argc, argv);
    const QStringList arguments = application.arguments();
#ifdef Q_OS_WIN
    if (arguments.size() != 6 || (arguments.at(1) != "preview-handle"
            && arguments.at(1) != "media-handle" && arguments.at(1) != "probe-handle"
            && arguments.at(1) != "image-metadata-handle"
            && arguments.at(1) != "document-metadata-handle"))
        return 2;
    bool handleOk = false;
    const quintptr handleValue = arguments.at(2).toULongLong(&handleOk);
    bool sizeOk = false;
    const qint64 fileSize = arguments.at(4).toLongLong(&sizeOk);
    bool modifiedOk = false;
    const qint64 modifiedMilliseconds = arguments.at(5).toLongLong(&modifiedOk);
    if (!handleOk || !handleValue || !sizeOk || fileSize < 0 || !modifiedOk)
        return 3;
    const HANDLE nativeHandle = reinterpret_cast<HANDLE>(handleValue);
    WinHandleDevice source(nativeHandle, fileSize);
    const QString fileName = QFileInfo(arguments.at(3)).fileName();
    if (fileName.isEmpty())
        return 3;
    if (arguments.at(1) == "probe-handle") {
        HANDLE token = nullptr;
        DWORD isAppContainer = 0;
        DWORD returned = 0;
        BOOL inJob = FALSE;
        DWORD capabilityBytes = 0;
        DWORD capabilityCount = 0;
        const bool tokenOpened = OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token);
        const bool appContainerRead = tokenOpened && GetTokenInformation(token,
            TokenIsAppContainer, &isAppContainer, sizeof(isAppContainer), &returned);
        if (tokenOpened)
            GetTokenInformation(token, TokenCapabilities, nullptr, 0, &capabilityBytes);
        QByteArray capabilityStorage(static_cast<qsizetype>(capabilityBytes), Qt::Uninitialized);
        if (tokenOpened && capabilityBytes >= sizeof(TOKEN_GROUPS)
                && GetTokenInformation(token, TokenCapabilities, capabilityStorage.data(),
                    capabilityBytes, &returned)) {
            capabilityCount = reinterpret_cast<const TOKEN_GROUPS*>(
                capabilityStorage.constData())->GroupCount;
        }
        if (token) CloseHandle(token);
        IsProcessInJob(GetCurrentProcess(), nullptr, &inJob);
        DWORD written = 0;
        const bool writeDenied = !WriteFile(nativeHandle, "x", 1, &written, nullptr);
        return writePayload({{"appContainer", appContainerRead && isAppContainer != 0},
            {"job", inJob != FALSE}, {"capabilityCount", static_cast<qint64>(capabilityCount)},
            {"readable", source.peek(1).size() <= 1}, {"writeDenied", writeDenied}},
            1024 * 1024) ? 0 : 5;
    }
    if (arguments.at(1) == "image-metadata-handle") {
        if (fileSize > 64LL * 1024 * 1024)
            return 4;
        const QByteArray suffix = QFileInfo(fileName).suffix().toLatin1().toLower();
        QSize dimensions;
        if (QImageReader::supportedImageFormats().contains(suffix)) {
            QImageReader reader(&source, suffix);
            reader.setAutoTransform(true);
            dimensions = reader.size();
        }
        return writePayload({{"width", dimensions.width()},
            {"height", dimensions.height()}}, 16 * 1024) ? 0 : 5;
    }
    if (arguments.at(1) == "document-metadata-handle") {
        const auto metadata = filemanager::preview::readDocumentMetadataFromDevice(
            &source, fileName, fileSize);
        return writePayload({{"pageCount", metadata.pageCount},
            {"title", metadata.title.left(32768)},
            {"authors", metadata.authors.left(32768)}}, 128 * 1024) ? 0 : 5;
    }
    if (arguments.at(1) == "media-handle") {
        if (fileName.endsWith(".wav", Qt::CaseInsensitive)) {
            const auto metadata = waveMetadata(&source, fileSize);
            return metadata && writePayload(*metadata, 1024 * 1024) ? 0 : 4;
        }
        QMediaPlayer player;
        bool completed = false;
        const auto finish = [&](const QJsonObject& object) {
            if (completed) return;
            completed = true;
            QJsonObject reply = object;
            // Report module presence only, never local paths or file names.
            // The focused integration check uses this to reject a native-
            // backend fallback that happened to read the synthetic fixture.
            reply.insert("ffmpegPluginLoaded",
                GetModuleHandleW(L"ffmpegmediaplugin.dll") != nullptr);
            reply.insert("ffmpegLibrariesLoaded",
                GetModuleHandleW(L"avcodec-61.dll") != nullptr
                && GetModuleHandleW(L"avformat-61.dll") != nullptr
                && GetModuleHandleW(L"avutil-59.dll") != nullptr
                && GetModuleHandleW(L"swresample-5.dll") != nullptr
                && GetModuleHandleW(L"swscale-8.dll") != nullptr);
            writePayload(reply, 1024 * 1024);
            application.quit();
        };
        QObject::connect(&player, &QMediaPlayer::mediaStatusChanged, &application,
            [&](QMediaPlayer::MediaStatus status) {
                if (status == QMediaPlayer::LoadedMedia || status == QMediaPlayer::BufferedMedia) {
                    const auto metadata = player.metaData();
                    const QSize resolution = metadata.value(QMediaMetaData::Resolution).toSize();
                    finish({{"durationMs", player.duration()}, {"title", metadata.stringValue(QMediaMetaData::Title).left(32768)},
                        {"artists", artists(metadata)}, {"width", resolution.width()}, {"height", resolution.height()}});
                } else if (status == QMediaPlayer::InvalidMedia) finish({});
            });
        QTimer::singleShot(5000, &application, [&] { finish({}); });
        // This URL is only a format hint: the media bytes come from the
        // already-opened handle. Set the path as decoded text so legal file
        // names containing '#' or '%' cannot become URL syntax.
        QUrl mediaHint;
        mediaHint.setScheme(QStringLiteral("file"));
        mediaHint.setPath(QStringLiteral("/") + fileName);
        player.setSourceDevice(&source, mediaHint);
        return application.exec();
    }
    const auto preview = filemanager::preview::PreviewService{}.load(&source, fileName, fileSize,
        QDateTime::fromMSecsSinceEpoch(modifiedMilliseconds));
#else
    if (arguments.size() != 3 || (arguments.at(1) != "preview" && arguments.at(1) != "media"))
        return 2;
    const QFileInfo info(arguments.at(2));
    if (!info.isAbsolute() || (!info.exists() && !arguments.at(2).isEmpty()))
        return 3;
    if (arguments.at(1) == "media") {
        QMediaPlayer player;
        bool completed = false;
        const auto finish = [&](const QJsonObject& object) {
            if (completed) return;
            completed = true;
            writePayload(object, 1024 * 1024);
            application.quit();
        };
        QObject::connect(&player, &QMediaPlayer::mediaStatusChanged, &application,
            [&](QMediaPlayer::MediaStatus status) {
                if (status == QMediaPlayer::LoadedMedia || status == QMediaPlayer::BufferedMedia) {
                    const auto metadata = player.metaData();
                    const QSize resolution = metadata.value(QMediaMetaData::Resolution).toSize();
                    finish({{"durationMs", player.duration()}, {"title", metadata.stringValue(QMediaMetaData::Title).left(32768)},
                        {"artists", artists(metadata)}, {"width", resolution.width()}, {"height", resolution.height()}});
                } else if (status == QMediaPlayer::InvalidMedia) finish({});
            });
        QTimer::singleShot(5000, &application, [&] { finish({}); });
        player.setSource(QUrl::fromLocalFile(info.absoluteFilePath()));
        return application.exec();
    }
    const auto preview = filemanager::preview::PreviewService{}.load(arguments.at(2));
#endif
    QByteArray imageBytes;
    if (!preview.image.isNull()) {
        QBuffer buffer(&imageBytes);
        if (!buffer.open(QIODevice::WriteOnly) || !preview.image.save(&buffer, "PNG")
                || imageBytes.size() > 24 * 1024 * 1024)
            return 4;
    }
    const QJsonObject object{{"kind", static_cast<int>(preview.kind)}, {"title", preview.title},
        {"metadata", preview.metadata}, {"text", preview.text},
        {"image", QString::fromLatin1(imageBytes.toBase64())}};
    if (!writePayload(object, 32 * 1024 * 1024))
        return 5;
    return 0;
}
