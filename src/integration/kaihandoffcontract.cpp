#include "kaihandoffcontract.h"

#include "files/filenamevalidator.h"

#include <QCryptographicHash>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonParseError>
#include <QSet>

#include <algorithm>
#include <utility>

namespace filemanager::integration
{
namespace
{

constexpr qsizetype maximumFiles = 3;
constexpr qsizetype maximumFileBytes = 1024 * 1024;
constexpr qsizetype maximumWireBytes = 4 * 1024 * 1024 + 4096;

bool fail(QString* error, const QString& message)
{
    if (error)
        *error = message;
    return false;
}

bool exactKeys(const QJsonObject& object, const QSet<QString>& expected)
{
    const auto keys = object.keys();
    return keys.size() == expected.size() &&
           std::all_of(keys.cbegin(), keys.cend(),
                       [&](const QString& key) { return expected.contains(key); });
}

bool validRelativePath(const QString& path)
{
    if (path.isEmpty() || path.size() > 4096 || path.contains('\\') ||
        QString::fromUtf8(path.toUtf8()) != path)
        return false;
    const auto segments = path.split('/', Qt::KeepEmptyParts);
    for (const auto& segment : segments)
    {
        if (!filemanager::files::validateWindowsName(segment).valid)
            return false;
    }
    return true;
}

bool validLowerHexDigest(const QString& digest)
{
    if (digest.size() != 64)
        return false;
    for (const QChar ch : digest)
    {
        if (!((ch >= QLatin1Char('0') && ch <= QLatin1Char('9')) ||
              (ch >= QLatin1Char('a') && ch <= QLatin1Char('f'))))
            return false;
    }
    return true;
}

} // namespace

bool parseKaiHandoffRequest(QByteArrayView wire, KaiHandoffRequest* request, QString* error)
{
    if (!request)
        return fail(error, QStringLiteral("A result object is required."));
    *request = {};
    if (wire.isEmpty() || wire.size() > maximumWireBytes)
        return fail(error, QStringLiteral("The handoff exceeds its bounded message size."));
    QJsonParseError parseError;
    const auto document =
        QJsonDocument::fromJson(QByteArray(wire.data(), wire.size()), &parseError);
    if (parseError.error != QJsonParseError::NoError || !document.isObject())
        return fail(error, QStringLiteral("The handoff is not a JSON object."));
    const auto root = document.object();
    if (!exactKeys(root, {QStringLiteral("schema"), QStringLiteral("files")}) ||
        !root.value(QStringLiteral("schema")).isDouble() ||
        root.value(QStringLiteral("schema")).toDouble() != 1.0 ||
        !root.value(QStringLiteral("files")).isArray())
        return fail(error, QStringLiteral("The handoff schema or fields are unsupported."));
    const auto files = root.value(QStringLiteral("files")).toArray();
    if (files.isEmpty() || files.size() > maximumFiles)
        return fail(error, QStringLiteral("The handoff file count is outside its limit."));
    QSet<QString> paths;
    KaiHandoffRequest parsed;
    for (const auto& value : files)
    {
        if (!value.isObject())
            return fail(error, QStringLiteral("A proposed file is not an object."));
        const auto file = value.toObject();
        if (!exactKeys(file, {QStringLiteral("path"), QStringLiteral("bytesBase64"),
                              QStringLiteral("sha256")}) ||
            !file.value(QStringLiteral("path")).isString() ||
            !file.value(QStringLiteral("bytesBase64")).isString() ||
            !file.value(QStringLiteral("sha256")).isString())
            return fail(error, QStringLiteral("A proposed file has unsupported fields."));
        const QString path = file.value(QStringLiteral("path")).toString();
        const QString folded = path.toCaseFolded();
        if (!validRelativePath(path) || paths.contains(folded))
            return fail(error, QStringLiteral("A proposed path is unsafe or duplicated."));
        paths.insert(folded);
        const QString digest = file.value(QStringLiteral("sha256")).toString();
        if (!validLowerHexDigest(digest))
            return fail(error, QStringLiteral("A proposed digest is malformed."));
        const QByteArray encoded = file.value(QStringLiteral("bytesBase64")).toString().toLatin1();
        if (encoded.size() > ((maximumFileBytes + 2) / 3) * 4)
            return fail(error, QStringLiteral("A proposed file exceeds its size limit."));
        const QByteArray decoded = QByteArray::fromBase64(encoded);
        if (decoded.size() > maximumFileBytes || decoded.toBase64() != encoded)
            return fail(error, QStringLiteral("A proposed file has invalid or oversized bytes."));
        const QByteArray hash = QCryptographicHash::hash(decoded, QCryptographicHash::Sha256);
        if (hash.toHex() != digest.toLatin1())
            return fail(error, QStringLiteral("A proposed file digest does not match its bytes."));
        parsed.files.push_back({path, decoded, hash});
    }
    *request = std::move(parsed);
    if (error)
        error->clear();
    return true;
}

} // namespace filemanager::integration
