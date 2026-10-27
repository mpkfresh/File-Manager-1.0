#include "epub_test_fixture.h"

#include <QFile>
#include <QList>
#include <QPair>

namespace bookTestFixtures {

bool writeEpub(const QString& path, const QByteArray& containerXml, const QByteArray& packageXml)
{
    // The product builds minizip in decompression-only mode. A two-entry stored ZIP
    // fixture keeps the tests independent of any archive writer or external tool.
    const QList<QPair<QByteArray, QByteArray>> entries{
        {"META-INF/container.xml", containerXml}, {"EPUB/package.opf", packageXml}};
    QByteArray archive;
    QByteArray directory;
    const auto append16 = [](QByteArray& bytes, quint16 value) {
        bytes.append(static_cast<char>(value & 0xff));
        bytes.append(static_cast<char>((value >> 8) & 0xff));
    };
    const auto append32 = [](QByteArray& bytes, quint32 value) {
        for (int index = 0; index < 4; ++index)
            bytes.append(static_cast<char>((value >> (index * 8)) & 0xff));
    };
    const auto crc32 = [](const QByteArray& bytes) {
        quint32 crc = 0xffffffffU;
        for (const char character : bytes) {
            crc ^= static_cast<quint8>(character);
            for (int bit = 0; bit < 8; ++bit)
                crc = (crc >> 1) ^ ((crc & 1U) ? 0xedb88320U : 0U);
        }
        return ~crc;
    };
    for (const auto& entry : entries) {
        const quint32 offset = static_cast<quint32>(archive.size());
        const quint32 size = static_cast<quint32>(entry.second.size());
        const quint16 nameSize = static_cast<quint16>(entry.first.size());
        const quint32 crc = crc32(entry.second);
        append32(archive, 0x04034b50U);
        append16(archive, 20); append16(archive, 0); append16(archive, 0);
        append16(archive, 0); append16(archive, 0);
        append32(archive, crc); append32(archive, size); append32(archive, size);
        append16(archive, nameSize); append16(archive, 0);
        archive.append(entry.first);
        archive.append(entry.second);

        append32(directory, 0x02014b50U);
        append16(directory, 20); append16(directory, 20);
        append16(directory, 0); append16(directory, 0);
        append16(directory, 0); append16(directory, 0);
        append32(directory, crc); append32(directory, size); append32(directory, size);
        append16(directory, nameSize); append16(directory, 0); append16(directory, 0);
        append16(directory, 0); append16(directory, 0);
        append32(directory, 0); append32(directory, offset);
        directory.append(entry.first);
    }
    const quint32 directoryOffset = static_cast<quint32>(archive.size());
    archive.append(directory);
    append32(archive, 0x06054b50U);
    append16(archive, 0); append16(archive, 0);
    append16(archive, static_cast<quint16>(entries.size()));
    append16(archive, static_cast<quint16>(entries.size()));
    append32(archive, static_cast<quint32>(directory.size()));
    append32(archive, directoryOffset);
    append16(archive, 0);
    QFile file(path);
    return file.open(QIODevice::WriteOnly) && file.write(archive) == archive.size();
}

} // namespace bookTestFixtures
