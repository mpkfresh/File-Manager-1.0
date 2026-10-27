#include "settings/portablepreferences.h"

#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QKeySequence>
#include <QSaveFile>

#include <utility>

namespace filemanager::settings {
namespace {

constexpr qsizetype maximumPortableSettingsBytes = 256 * 1024;

bool fail(QString* error, const QString& message)
{
    if (error)
        *error = message;
    return false;
}

bool hasOnlyKeys(const QJsonObject& object, const QSet<QString>& allowed)
{
    for (auto it = object.constBegin(); it != object.constEnd(); ++it) {
        if (!allowed.contains(it.key()))
            return false;
    }
    return true;
}

} // namespace

QByteArray createPortablePreferences(const PortablePreferences& preferences)
{
    QJsonObject shortcuts;
    QStringList shortcutIds = preferences.shortcuts.keys();
    shortcutIds.sort(Qt::CaseSensitive);
    for (const QString& id : shortcutIds)
        shortcuts.insert(id, preferences.shortcuts.value(id));
    QJsonArray pinned;
    for (const QString& id : preferences.pinnedContextActions)
        pinned.append(id);
    const QJsonObject root{
        {"schema", 1},
        {"product", "FileManager"},
        {"appearance", QJsonObject{
             {"fontPointSize", preferences.fontPointSize},
             {"density", preferences.density},
             {"animations", preferences.animations}}},
        {"interaction", QJsonObject{
             {"singleClickActivation", preferences.singleClickActivation},
             {"pinnedContextActions", pinned}}},
        {"shortcuts", shortcuts}};
    return QJsonDocument(root).toJson(QJsonDocument::Indented);
}

bool parsePortablePreferences(const QByteArray& content,
    const QSet<QString>& allowedCommandIds, PortablePreferences* preferences, QString* error)
{
    if (!preferences)
        return fail(error, QStringLiteral("No settings destination was provided."));
    if (content.size() > maximumPortableSettingsBytes)
        return fail(error, QStringLiteral("The settings file exceeds the 256 KiB limit."));
    QJsonParseError parseError;
    const QJsonDocument document = QJsonDocument::fromJson(content, &parseError);
    if (parseError.error != QJsonParseError::NoError || !document.isObject())
        return fail(error, QStringLiteral("The settings file is not valid JSON."));
    const QJsonObject root = document.object();
    if (!hasOnlyKeys(root, {"schema", "product", "appearance", "interaction", "shortcuts"})
        || root.value("schema").toInt(-1) != 1
        || root.value("product").toString() != QStringLiteral("FileManager")
        || !root.value("appearance").isObject()
        || !root.value("interaction").isObject()
        || !root.value("shortcuts").isObject()) {
        return fail(error, QStringLiteral("The settings schema or product identity is unsupported."));
    }
    const QJsonObject appearance = root.value("appearance").toObject();
    const QJsonObject interaction = root.value("interaction").toObject();
    if (!hasOnlyKeys(appearance, {"fontPointSize", "density", "animations"})
        || !hasOnlyKeys(interaction, {"singleClickActivation", "pinnedContextActions"})
        || !appearance.value("fontPointSize").isDouble()
        || !appearance.value("density").isString()
        || !appearance.value("animations").isBool()
        || !interaction.value("singleClickActivation").isBool()
        || !interaction.value("pinnedContextActions").isArray()) {
        return fail(error, QStringLiteral("The settings file contains invalid field types."));
    }
    PortablePreferences decoded;
    decoded.fontPointSize = appearance.value("fontPointSize").toDouble();
    decoded.density = appearance.value("density").toString();
    decoded.animations = appearance.value("animations").toBool();
    decoded.singleClickActivation = interaction.value("singleClickActivation").toBool();
    if (decoded.fontPointSize < 8.0 || decoded.fontPointSize > 18.0
        || !QStringList{"Compact", "Comfortable", "Spacious"}.contains(decoded.density)) {
        return fail(error, QStringLiteral("The appearance values are outside supported bounds."));
    }
    const QJsonObject shortcutObject = root.value("shortcuts").toObject();
    if (shortcutObject.size() > allowedCommandIds.size())
        return fail(error, QStringLiteral("The settings file contains too many shortcuts."));
    QSet<QString> assignedSequences;
    for (auto it = shortcutObject.constBegin(); it != shortcutObject.constEnd(); ++it) {
        if (!allowedCommandIds.contains(it.key()) || !it.value().isString())
            return fail(error, QStringLiteral("The settings file names an unknown command."));
        const QString sequence = it.value().toString();
        if (sequence.size() > 64)
            return fail(error, QStringLiteral("A shortcut is too long."));
        const QKeySequence parsed = QKeySequence::fromString(sequence, QKeySequence::PortableText);
        const QString normalized = parsed.toString(QKeySequence::PortableText);
        if ((!sequence.isEmpty() && parsed.isEmpty())
            || (!normalized.isEmpty() && assignedSequences.contains(normalized))) {
            return fail(error, QStringLiteral("A shortcut is invalid or assigned more than once."));
        }
        if (!normalized.isEmpty())
            assignedSequences.insert(normalized);
        decoded.shortcuts.insert(it.key(), normalized);
    }
    const QJsonArray pinned = interaction.value("pinnedContextActions").toArray();
    if (pinned.size() > 32)
        return fail(error, QStringLiteral("The settings file contains too many pinned commands."));
    QSet<QString> pinnedIds;
    for (const QJsonValue& value : pinned) {
        const QString id = value.toString();
        if (!value.isString() || !allowedCommandIds.contains(id) || pinnedIds.contains(id))
            return fail(error, QStringLiteral("A pinned command is invalid, unknown or duplicated."));
        pinnedIds.insert(id);
        decoded.pinnedContextActions.append(id);
    }
    *preferences = std::move(decoded);
    return true;
}

bool writePortablePreferences(const QString& destination,
    const PortablePreferences& preferences, QString* error)
{
    QSaveFile file(destination);
    const QByteArray content = createPortablePreferences(preferences);
    if (!file.open(QIODevice::WriteOnly) || file.write(content) != content.size()
        || !file.commit()) {
        if (error)
            *error = file.errorString();
        return false;
    }
    return true;
}

bool readPortablePreferences(const QString& source, const QSet<QString>& allowedCommandIds,
    PortablePreferences* preferences, QString* error)
{
    QFile file(source);
    if (!file.open(QIODevice::ReadOnly))
        return fail(error, file.errorString());
    if (file.size() > maximumPortableSettingsBytes)
        return fail(error, QStringLiteral("The settings file exceeds the 256 KiB limit."));
    return parsePortablePreferences(file.read(maximumPortableSettingsBytes + 1),
        allowedCommandIds, preferences, error);
}

} // namespace filemanager::settings
