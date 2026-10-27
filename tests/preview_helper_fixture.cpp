#include <QCoreApplication>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QThread>
#include "../src/platform/helperoutput.h"

#include <cstdio>

int main(int argc, char** argv)
{
    QCoreApplication application(argc, argv);
    const QStringList arguments = application.arguments();
    if (arguments.size() == 5 && arguments.at(1) == QStringLiteral("--file-icon")) {
        const QString mode = QFileInfo(arguments.at(2)).fileName();
        if (mode == QStringLiteral("icon-exit.fixture"))
            return 17;
        if (mode == QStringLiteral("icon-stderr.fixture")) {
            return filemanager::platform::writeHelperReply(QByteArrayView("fixture diagnostic"), stderr) ? 0 : 3;
        }
        if (mode == QStringLiteral("icon-hang.fixture")) {
            QThread::msleep(3000);
            return 0;
        }
        if (mode == QStringLiteral("icon-oversize.fixture")) {
            const QByteArray block(1024 * 1024, 'x');
            for (int i = 0; i < 3; ++i)
                if (!filemanager::platform::writeHelperReply(block))
                    return 3;
            return 0;
        }
        const QByteArray payload = mode == QStringLiteral("icon-wrong-token.fixture")
            ? QByteArray("FMICON01") + QByteArray(32, 'z')
                + QByteArray::fromHex("0100000001000000010203ff")
            : QByteArray("not an icon reply");
        return filemanager::platform::writeHelperReply(payload) ? 0 : 4;
    }
    if (arguments.size() != 3)
        return 2;
    if (arguments.at(1) == QStringLiteral("--parent")) {
        const QByteArray requested = QByteArray::fromBase64(arguments.at(2).toLatin1(),
            QByteArray::Base64UrlEncoding);
        if (requested.contains("hang-parent")) {
            QThread::msleep(1000);
            return 0;
        }
        const QByteArray replyIdentity = requested.contains("invalid-parent")
            ? QByteArray::fromHex("ffff0000") : requested;
        const QJsonArray items{QJsonObject{{"valid", true}, {"folder", true},
            {"parsingName", "fixture"},
            {"idList", QString::fromLatin1(replyIdentity.toBase64(QByteArray::Base64UrlEncoding))}}};
        const QByteArray reply = QJsonDocument(QJsonObject{{"success", true}, {"items", items}})
            .toJson(QJsonDocument::Compact);
        return filemanager::platform::writeHelperReply(reply) ? 0 : 4;
    }
    const QString name = QFileInfo(arguments.at(2)).fileName();
    if (name == "hang.fixture") {
        QThread::msleep(500);
        return 0;
    }
    if (name == "oversize.fixture") {
        const QByteArray block(1024 * 1024, 'x');
        for (int index = 0; index < 33; ++index)
            if (!filemanager::platform::writeHelperReply(block))
                return 3;
        return 0;
    }
    constexpr char malformed[] = "not-json";
    return filemanager::platform::writeHelperReply(QByteArrayView(malformed, sizeof(malformed) - 1)) ? 0 : 4;
}
