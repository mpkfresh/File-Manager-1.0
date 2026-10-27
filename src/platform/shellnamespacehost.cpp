#include "platform/shellitem.h"
#include "platform/recyclebinoperation.h"
#include "platform/fileicon.h"
#include "platform/helperoutput.h"
#include "platform/helperinput.h"
#include "platform/directorywatcher.h"
#include "diagnostics/diagnostichost.h"

#include <QCoreApplication>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>

#include <cstdio>
#ifdef Q_OS_WIN
#include <fcntl.h>
#include <io.h>
#endif

int main(int argc, char** argv)
{
    QCoreApplication application(argc, argv);
    const QStringList arguments = application.arguments();
    if (arguments.size() == 2 && arguments.at(1) == QStringLiteral("--diagnostic-storage"))
        return filemanager::diagnostics::runDiagnosticHost();
    if (arguments.size() == 2 && arguments.at(1) == QStringLiteral("--watch-directories"))
        return filemanager::platform::runDirectoryWatchHost();
    if (arguments.size() == 5 && arguments.at(1) == QStringLiteral("--file-icon")) {
        bool sizeOk = false;
        const int pixelExtent = arguments.at(3).toInt(&sizeOk);
        const QByteArray token = arguments.at(4).toLatin1();
        if (!sizeOk || token.size() != 32
                || !filemanager::platform::validFileIconRequest(arguments.at(2), pixelExtent))
            return 10;
        const QImage icon = filemanager::platform::readFileIconInProcess(arguments.at(2), pixelExtent);
        const QByteArray reply = filemanager::platform::encodeFileIconReply(icon, token);
        if (reply.isEmpty())
            return 11;
#ifdef Q_OS_WIN
        if (_setmode(_fileno(stdout), _O_BINARY) == -1)
            return 12;
#endif
        return filemanager::platform::writeHelperReply(reply) ? 0 : 13;
    }
    if (arguments.size() == 3 && arguments.at(1) == QStringLiteral("--restore-selection")) {
        bool ownerOk = false;
        const quintptr owner = static_cast<quintptr>(arguments.at(2).toULongLong(&ownerOk));
        QByteArray payload;
        char buffer[4096];
        while (payload.size() <= 6 * 1024 * 1024) {
            const qint64 bytes = filemanager::platform::readHelperInput(buffer, sizeof(buffer));
            if (bytes < 0)
                return 6;
            if (bytes == 0)
                break;
            payload.append(buffer, static_cast<qsizetype>(bytes));
        }
        QJsonParseError parseError;
        const QJsonDocument request = QJsonDocument::fromJson(payload, &parseError);
        if (!ownerOk || payload.size() > 6 * 1024 * 1024
                || parseError.error != QJsonParseError::NoError || !request.isObject())
            return 6;
        const QJsonArray identities = request.object().value(QStringLiteral("items")).toArray();
        if (identities.isEmpty() || identities.size() > 256)
            return 7;
        QList<filemanager::platform::ShellItemReference> items;
        for (const auto& value : identities) {
            const QByteArray idList = QByteArray::fromBase64(value.toString().toLatin1(),
                QByteArray::Base64UrlEncoding);
            filemanager::platform::ShellItemReference item;
            item.valid = true;
            item.idList = idList;
            items.append(std::move(item));
        }
        const auto result = filemanager::platform::RecycleBinOperation::
            restoreSelectedShellItemsInProcess(items, owner);
        const QByteArray response = QJsonDocument(QJsonObject{
            {"success", result.success}, {"aborted", result.aborted},
            {"nativeCode", static_cast<qint64>(result.nativeCode)}, {"error", result.error},
            {"requestedItems", result.requestedItems}, {"restoredItems", result.restoredItems}})
            .toJson(QJsonDocument::Compact);
        if (!filemanager::platform::writeHelperReply(response))
            return 8;
        return result.success ? 0 : 9;
    }
    if (arguments.size() != 3)
        return 2;
    const bool parentRequest = arguments.at(1) == QStringLiteral("--parent");
    const bool resolveRequest = arguments.at(1) == QStringLiteral("--resolve");
    const QByteArray idList = QByteArray::fromBase64(arguments.at(parentRequest ? 2 : 1).toLatin1(),
        QByteArray::Base64UrlEncoding);
    bool limitOk = parentRequest || resolveRequest;
    const int maximumItems = parentRequest || resolveRequest ? 1 : arguments.at(2).toInt(&limitOk);
    if (!resolveRequest && (!limitOk || maximumItems <= 0 || maximumItems > 4096
            || !filemanager::platform::ShellItem::isValidSerializedIdentity(idList)))
        return 3;
    if (resolveRequest && (arguments.at(2).trimmed().isEmpty() || arguments.at(2).size() > 8192))
        return 3;

    filemanager::platform::ShellItemReference parent;
    parent.valid = true;
    parent.folder = true;
    parent.idList = idList;
    filemanager::platform::ShellItemEnumeration result;
    if (parentRequest || resolveRequest) {
        const auto builtIn = filemanager::platform::ShellItem::builtInLocationFromIdentity(arguments.at(2));
        const auto resolved = parentRequest ? filemanager::platform::ShellItem::parent(parent)
            : builtIn != filemanager::platform::BuiltInShellLocation::None
                ? filemanager::platform::ShellItem::resolveBuiltIn(builtIn)
                : filemanager::platform::ShellItem::resolve(arguments.at(2));
        result.success = resolved.valid && (resolveRequest || resolved.folder);
        result.nativeCode = resolved.nativeCode;
        result.error = resolved.error;
        if (result.success)
            result.items.append(resolved);
    } else {
        result = filemanager::platform::ShellItem::enumerateChildren(parent, maximumItems);
    }
    QJsonArray items;
    for (const auto& item : result.items) {
        items.append(QJsonObject{{"valid", item.valid}, {"fileSystem", item.fileSystem},
            {"folder", item.folder}, {"nativeCode", static_cast<qint64>(item.nativeCode)},
            {"parsingName", item.parsingName}, {"fileSystemPath", item.fileSystemPath},
            {"displayName", item.displayName},
            {"idList", QString::fromLatin1(item.idList.toBase64(
                QByteArray::Base64UrlEncoding | QByteArray::OmitTrailingEquals))},
            {"error", item.error}});
    }
    const QByteArray payload = QJsonDocument(QJsonObject{{"success", result.success},
        {"truncated", result.truncated}, {"nativeCode", static_cast<qint64>(result.nativeCode)},
        {"error", result.error}, {"items", items}}).toJson(QJsonDocument::Compact);
    if (!filemanager::platform::writeHelperReply(payload))
        return 4;
    return result.success ? 0 : 5;
}
