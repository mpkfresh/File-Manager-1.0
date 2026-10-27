#include "diagnostics/diagnostichost.h"
#include "diagnostics/diagnosticlog.h"
#include "diagnostics/diagnosticbundle.h"
#include "platform/helperinput.h"
#include "platform/helperoutput.h"

#include <QDir>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSaveFile>
#include <cstdio>
#include <memory>
#ifdef Q_OS_WIN
#include <fcntl.h>
#include <io.h>
#endif

namespace filemanager::diagnostics {
int runDiagnosticHost()
{
#ifdef Q_OS_WIN
    if (_setmode(_fileno(stdin), _O_BINARY) == -1
        || _setmode(_fileno(stdout), _O_BINARY) == -1)
        return 20;
#endif
    constexpr qsizetype maximumMessage = 1024 * 1024;
    std::unique_ptr<DiagnosticLog> log;
    bool healthy = false;
    QByteArray line;
    filemanager::platform::HelperInputReader input;
    for (;;) {
        const int next = input.next();
        if (next < 0)
            return next == -1 ? 0 : 25;
        if (next != '\n') {
            if (line.size() >= maximumMessage)
                return 21;
            line.append(static_cast<char>(next));
            continue;
        }
        QJsonParseError parse;
        const QJsonDocument document = QJsonDocument::fromJson(line, &parse);
        line.clear();
        if (parse.error != QJsonParseError::NoError || !document.isObject())
            return 22;
        const QJsonObject request = document.object();
        const QString op = request.value("op").toString();
        QString error;
        bool success = false;
        if (op == "init" && !log) {
            const QString path = request.value("path").toString();
            if (!QDir::isAbsolutePath(path) || path.contains(QChar::Null) || path.size() > 32767)
                return 23;
            log = std::make_unique<DiagnosticLog>(path);
            error = log->initializationError();
            success = healthy = error.isEmpty();
        } else if (op == "clear" && log) {
            success = log->clear(&error);
            if (success)
                healthy = true;
        } else if (op == "record" && log && healthy) {
            success = log->record(request.value("code").toString(), &error);
        } else if (op == "export" && log && healthy) {
            const QString destination = request.value("destination").toString();
            const QFileInfo target(destination);
            const QFileInfo history(log->storagePath());
            // An export must never replace the history it is reporting.
            const bool samePath = target.absoluteFilePath().compare(history.absoluteFilePath(),
                Qt::CaseInsensitive) == 0 || (!target.canonicalFilePath().isEmpty()
                && target.canonicalFilePath().compare(history.canonicalFilePath(), Qt::CaseInsensitive) == 0);
            if (!QDir::isAbsolutePath(destination) || samePath || destination.contains(QChar::Null)) {
                error = QStringLiteral("Choose an export file separate from diagnostic history.");
            } else {
                QJsonObject bundle = request.value("bundle").toObject();
                bundle.insert("eventCodes", QJsonArray::fromStringList(log->eventCodes()));
                const QByteArray content = QJsonDocument(bundle).toJson(QJsonDocument::Indented);
                QSaveFile file(destination);
                success = file.open(QIODevice::WriteOnly)
                    && file.write(content) == content.size() && file.commit();
                if (!success)
                    error = QStringLiteral("The diagnostic bundle could not be saved.");
            }
        } else {
            error = QStringLiteral("Diagnostic history is unavailable; clear it explicitly to recover.");
        }
        const QByteArray reply = QJsonDocument(QJsonObject{
            {"id", request.value("id")}, {"success", success}, {"healthy", healthy},
            {"error", error}, {"events", log ? QJsonArray::fromStringList(log->eventCodes()) : QJsonArray{}}
        }).toJson(QJsonDocument::Compact) + '\n';
        if (!filemanager::platform::writeHelperReply(reply))
            return 24;
    }
}
}
