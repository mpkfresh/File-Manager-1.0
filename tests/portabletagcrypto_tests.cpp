#include "tags/portabletagcrypto.h"
#include "tags/portabletagfile.h"

#include <QByteArray>
#include <QDir>
#include <QFile>
#include <QString>
#include <QTemporaryDir>

#include <iostream>

int runPortableTagCryptoTests()
{
#ifndef Q_OS_WIN
    return 0;
#else
    using namespace filemanager::tags;
    int failures = 0;
    const auto check = [&](bool condition, const char* message) {
        if (!condition) {
            std::cerr << "FAIL: portable tag crypto: " << message << '\n';
            ++failures;
        }
    };
    const QByteArray plain(
        "{\"path\":\"D:/Books/Alpha.epub\",\"tags\":[\"Series\"]}");
    const QString passphrase = QStringLiteral("Long snowman ☃ passphrase 2026");
    const auto sealed = sealPortableTagPayload(plain, passphrase);
    check(sealed.valid() && sealed.bytes.size() == plain.size() + 64,
        "seal bounded payload into the fixed authenticated envelope");
    if (!sealed.valid())
        return failures;
    check(!sealed.bytes.contains("D:/Books/Alpha.epub"),
        "encrypted bytes do not expose the private path");
    const auto again = sealPortableTagPayload(plain, passphrase);
    check(again.valid() && again.bytes != sealed.bytes,
        "each export uses a distinct random salt and nonce");
    const auto opened = openPortableTagPayload(sealed.bytes, passphrase);
    check(opened.valid() && opened.bytes == plain,
        "Unicode passphrase opens the exact original payload");
    check(!openPortableTagPayload(sealed.bytes,
            QStringLiteral("Different long passphrase 2026")).valid(),
        "wrong passphrase never returns plaintext");
    QByteArray damaged = sealed.bytes;
    damaged[48] = static_cast<char>(damaged[48] ^ 1);
    check(!openPortableTagPayload(damaged, passphrase).valid(),
        "tampered ciphertext fails authentication");
    damaged = sealed.bytes;
    damaged[damaged.size() - 1] = static_cast<char>(damaged.back() ^ 1);
    check(!openPortableTagPayload(damaged, passphrase).valid(),
        "tampered tag fails authentication");
    damaged = sealed.bytes;
    damaged[11] = 1;
    check(!openPortableTagPayload(damaged, passphrase).valid(),
        "unknown header flags fail before decryption");
    damaged = sealed.bytes;
    damaged[12] = 0;
    damaged[13] = 0;
    damaged[14] = 0;
    damaged[15] = 0;
    check(!openPortableTagPayload(damaged, passphrase).valid(),
        "unbounded or invalid KDF work factor is rejected");
    damaged = sealed.bytes;
    damaged[44] = static_cast<char>(0xff);
    damaged[45] = static_cast<char>(0xff);
    damaged[46] = static_cast<char>(0xff);
    damaged[47] = static_cast<char>(0xff);
    check(!openPortableTagPayload(damaged, passphrase).valid(),
        "oversized advertised plaintext is rejected before allocation");
    check(!openPortableTagPayload(sealed.bytes.chopped(1), passphrase).valid(),
        "truncated envelope is rejected");
    check(!sealPortableTagPayload({}, passphrase).valid(),
        "empty export data is rejected");
    check(!sealPortableTagPayload(plain, QStringLiteral("too short")).valid(),
        "short passphrase is rejected");

    QTemporaryDir exportFolder;
    check(exportFolder.isValid(), "disposable export folder is available");
    if (exportFolder.isValid()) {
        const QString target = QDir(exportFolder.path()).filePath(
            QStringLiteral("portable-tags.fmtags"));
        const auto written = writeNewPortableTagFile(target, plain, passphrase);
        if (!written.written)
            std::cerr << "portable tag export detail: "
                      << written.error.toStdString() << '\n';
        check(written.written, "export writes a new encrypted file");
        QFile saved(target);
        check(saved.open(QIODevice::ReadOnly), "encrypted export is readable");
        const QByteArray stored = saved.isOpen() ? saved.readAll() : QByteArray{};
        saved.close();
        check(!stored.contains("D:/Books/Alpha.epub"),
            "export file never stores plaintext paths");
        check(stored.size() == plain.size() + 64,
            "export file contains only the bounded envelope");
        const auto imported = readPortableTagFile(target, passphrase);
        check(imported.valid() && imported.bytes == plain,
            "explicit import read authenticates the exact payload");
        check(!readPortableTagFile(target,
                QStringLiteral("Different long passphrase 2026")).valid(),
            "wrong export passphrase exposes no data");
        check(!writeNewPortableTagFile(target, QByteArray("replacement"),
                passphrase).written,
            "an existing export cannot be overwritten");
        QFile unchanged(target);
        check(unchanged.open(QIODevice::ReadOnly)
                && unchanged.readAll() == stored,
            "failed replacement leaves the first encrypted export unchanged");
        check(QDir(exportFolder.path()).entryList(QDir::Files | QDir::NoDotAndDotDot)
                == QStringList{QStringLiteral("portable-tags.fmtags")},
            "export leaves no staging file behind");
    }
    return failures;
#endif
}
