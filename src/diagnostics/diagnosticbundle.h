#pragma once

#include <QByteArray>
#include <QJsonObject>
#include <QString>
#include <QStringList>

namespace filemanager::diagnostics {

struct DiagnosticBundleInput
{
    QString productVersion;
    QString releaseChannel;
    QString operatingSystem;
    QString qtVersion;
    QJsonObject featureStates;
    QStringList openPaths;
    QStringList indexedRoots;
    QStringList eventCodes;
};

bool isSafeDiagnosticEventCode(const QString& code);
QByteArray createDiagnosticBundle(
    const DiagnosticBundleInput& input, bool includeSensitivePaths = false);
bool writeDiagnosticBundle(const QString& destination, const DiagnosticBundleInput& input,
    bool includeSensitivePaths, QString* error = nullptr);

} // namespace filemanager::diagnostics
