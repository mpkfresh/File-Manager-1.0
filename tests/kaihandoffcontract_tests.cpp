#include "integration/kaihandoffcontract.h"

#include <QCoreApplication>
#include <QCryptographicHash>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>

#include <cstdio>
#include <stdexcept>

using filemanager::integration::KaiHandoffRequest;
using filemanager::integration::parseKaiHandoffRequest;

namespace
{

void check(bool condition, const char* message)
{
    if (!condition)
        throw std::runtime_error(message);
}

QJsonObject file(const QString& path, const QByteArray& bytes)
{
    return {
        {QStringLiteral("path"), path},
        {QStringLiteral("bytesBase64"), QString::fromLatin1(bytes.toBase64())},
        {QStringLiteral("sha256"),
         QString::fromLatin1(QCryptographicHash::hash(bytes, QCryptographicHash::Sha256).toHex())}};
}

QByteArray wire(const QJsonArray& files)
{
    return QJsonDocument(
               QJsonObject{{QStringLiteral("schema"), 1}, {QStringLiteral("files"), files}})
        .toJson(QJsonDocument::Compact);
}

bool accepts(const QByteArray& bytes, KaiHandoffRequest* result = nullptr)
{
    KaiHandoffRequest local;
    return parseKaiHandoffRequest(bytes, result ? result : &local);
}

} // namespace

int main(int argc, char** argv)
{
    QCoreApplication app(argc, argv);
    try
    {
        KaiHandoffRequest parsed;
        const QByteArray content("Kai\0UI", 6);
        check(accepts(wire({file(QStringLiteral("site/index.html"), content)}), &parsed) &&
                  parsed.files.size() == 1 &&
                  parsed.files.front().relativePath == QStringLiteral("site/index.html") &&
                  parsed.files.front().bytes == content && parsed.files.front().sha256.size() == 32,
              "valid exact-byte proposal refused");
        check(!accepts(wire({file(QStringLiteral("../escape.txt"), "x")})),
              "parent traversal accepted");
        check(!accepts(wire({file(QStringLiteral("site\\escape.txt"), "x")})),
              "backslash path accepted");
        check(!accepts(wire({file(QStringLiteral("CON.txt"), "x")})), "reserved name accepted");
        check(!accepts(wire({file(QStringLiteral("site//file.txt"), "x")})),
              "empty segment accepted");
        check(!accepts(
                  wire({file(QStringLiteral("A.txt"), "x"), file(QStringLiteral("a.TXT"), "y")})),
              "case-insensitive duplicate accepted");
        QJsonObject changed = file(QStringLiteral("ok.txt"), "x");
        changed.insert(QStringLiteral("sha256"), QString(64, QLatin1Char('0')));
        check(!accepts(wire({changed})), "hash mismatch accepted");
        changed = file(QStringLiteral("ok.txt"), "x");
        changed.insert(QStringLiteral("bytesBase64"), QStringLiteral("eA"));
        check(!accepts(wire({changed})), "noncanonical base64 accepted");
        changed = file(QStringLiteral("ok.txt"), "x");
        changed.insert(QStringLiteral("destination"), QStringLiteral("C:/outside"));
        check(!accepts(wire({changed})), "sender-provided destination authority accepted");
        check(!accepts(wire({})), "empty proposal accepted");
        check(!accepts(wire({file(QStringLiteral("a"), "a"), file(QStringLiteral("b"), "b"),
                             file(QStringLiteral("c"), "c"), file(QStringLiteral("d"), "d")})),
              "file-count limit bypassed");
        check(accepts(wire({file(QStringLiteral("max.txt"), QByteArray(1024 * 1024, 'x'))})),
              "one MiB boundary refused");
        check(
            !accepts(wire({file(QStringLiteral("too-big.txt"), QByteArray(1024 * 1024 + 1, 'x'))})),
            "file-size limit bypassed");
        check(!accepts(QByteArray(4 * 1024 * 1024 + 4097, 'x')), "wire-size limit bypassed");
        std::puts("Kai handoff contract tests passed");
        return 0;
    }
    catch (const std::exception& ex)
    {
        std::fprintf(stderr, "Kai handoff contract test failed: %s\n", ex.what());
        return 1;
    }
}
