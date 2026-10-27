#include "diagnostics/diagnosticbundle.h"

#include <QDateTime>
#include <QJsonArray>
#include <QJsonDocument>
#include <QRegularExpression>
#include <QSaveFile>

namespace filemanager::diagnostics {
namespace {

QJsonArray strings(const QStringList& values)
{
    QJsonArray result;
    for (const QString& value : values)
        result.append(value);
    return result;
}

QStringList safeEventCodes(const QStringList& eventCodes)
{
    QStringList result;
    for (const QString& code : eventCodes) {
        if (isSafeDiagnosticEventCode(code))
            result.append(code);
    }
    return result;
}

} // namespace

bool isSafeDiagnosticEventCode(const QString& code)
{
    static const QRegularExpression allowed(QStringLiteral("^[a-z0-9][a-z0-9._-]{0,63}$"));
    return allowed.match(code).hasMatch();
}

QByteArray createDiagnosticBundle(
    const DiagnosticBundleInput& input, bool includeSensitivePaths)
{
    QJsonObject root{
        {"schema", 1},
        {"generatedAtUtc", QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs)},
        {"productVersion", input.productVersion},
        {"releaseChannel", input.releaseChannel},
        {"operatingSystem", input.operatingSystem},
        {"qtVersion", input.qtVersion},
        {"featureStates", input.featureStates},
        {"eventCodes", strings(safeEventCodes(input.eventCodes))},
        {"openPathCount", input.openPaths.size()},
        {"indexedRootCount", input.indexedRoots.size()},
        {"sensitivePathsIncluded", includeSensitivePaths}
    };
    if (includeSensitivePaths) {
        root.insert("openPaths", strings(input.openPaths));
        root.insert("indexedRoots", strings(input.indexedRoots));
    }
    return QJsonDocument(root).toJson(QJsonDocument::Indented);
}

bool writeDiagnosticBundle(const QString& destination, const DiagnosticBundleInput& input,
    bool includeSensitivePaths, QString* error)
{
    QSaveFile file(destination);
    if (!file.open(QIODevice::WriteOnly)) {
        if (error)
            *error = file.errorString();
        return false;
    }
    const QByteArray content = createDiagnosticBundle(input, includeSensitivePaths);
    if (file.write(content) != content.size() || !file.commit()) {
        if (error)
            *error = file.errorString();
        return false;
    }
    return true;
}

} // namespace filemanager::diagnostics
