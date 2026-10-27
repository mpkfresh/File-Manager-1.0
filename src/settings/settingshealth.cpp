#include "settings/settingshealth.h"

#include <QDateTime>
#include <QDir>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QKeySequence>
#include <QMetaType>
#include <QRegularExpression>
#include <QSet>
#include <QSettings>
#include <QtNumeric>

namespace filemanager::settings {
namespace {

constexpr qsizetype maximumPathLength = 32767;
constexpr qsizetype maximumFavoriteBytes = 256 * 1024;
constexpr qsizetype maximumColumnStateBytes = 256 * 1024;

bool isBoolean(const QVariant& value)
{
    return value.metaType().id() == QMetaType::Bool;
}

void reset(QSettings& settings, const QString& key, const QString& category,
    SettingsHealthReport& report)
{
    settings.remove(key);
    if (!report.resetCategories.contains(category))
        report.resetCategories.append(category);
}

bool isAbsolutePath(const QString& path)
{
    return !path.isEmpty() && path.size() <= maximumPathLength && QDir::isAbsolutePath(path);
}

bool validPathList(const QVariant& value, qsizetype maximumCount)
{
    if (value.metaType().id() != QMetaType::QStringList)
        return false;
    const QStringList paths = value.toStringList();
    if (paths.size() > maximumCount)
        return false;
    QSet<QString> seen;
    for (const QString& path : paths) {
        const QString normalized = QDir::cleanPath(QDir::fromNativeSeparators(path));
        const QString folded = normalized.toCaseFolded();
        if (!isAbsolutePath(normalized) || seen.contains(folded))
            return false;
        seen.insert(folded);
    }
    return true;
}

bool validCommandId(const QString& id)
{
    static const QRegularExpression expression(
        QStringLiteral("^[a-z0-9][a-z0-9._-]{0,63}$"));
    return expression.match(id).hasMatch();
}

bool validPinnedActions(const QVariant& value)
{
    if (value.metaType().id() != QMetaType::QStringList)
        return false;
    const QStringList ids = value.toStringList();
    if (ids.size() > 32)
        return false;
    QSet<QString> seen;
    for (const QString& id : ids) {
        if (!validCommandId(id) || seen.contains(id))
            return false;
        seen.insert(id);
    }
    return true;
}

bool validFavoriteGroups(const QVariant& value)
{
    if (value.metaType().id() != QMetaType::QByteArray)
        return false;
    const QByteArray encoded = value.toByteArray();
    if (encoded.isEmpty() || encoded.size() > maximumFavoriteBytes)
        return false;
    QJsonParseError error;
    const QJsonDocument document = QJsonDocument::fromJson(encoded, &error);
    if (error.error != QJsonParseError::NoError || !document.isObject())
        return false;
    const QJsonObject root = document.object();
    if (root.size() != 2 || root.value("schemaVersion").toInt(-1) != 1
        || !root.value("groups").isArray())
        return false;
    const QJsonArray groups = root.value("groups").toArray();
    if (groups.isEmpty() || groups.size() > 64)
        return false;
    QSet<QString> ids;
    QSet<QString> names;
    QSet<QString> paths;
    qsizetype pathCount = 0;
    for (const QJsonValue& groupValue : groups) {
        if (!groupValue.isObject())
            return false;
        const QJsonObject group = groupValue.toObject();
        if (group.size() != 3 || !group.value("id").isString()
            || !group.value("name").isString() || !group.value("paths").isArray())
            return false;
        const QString id = group.value("id").toString();
        const QString name = group.value("name").toString().trimmed();
        if (id.isEmpty() || id.size() > 128 || name.isEmpty() || name.size() > 128
            || ids.contains(id) || names.contains(name.toCaseFolded()))
            return false;
        ids.insert(id);
        names.insert(name.toCaseFolded());
        for (const QJsonValue& pathValue : group.value("paths").toArray()) {
            if (!pathValue.isString() || ++pathCount > 256)
                return false;
            const QString path = QDir::cleanPath(
                QDir::fromNativeSeparators(pathValue.toString()));
            const QString folded = path.toCaseFolded();
            if (!isAbsolutePath(path) || paths.contains(folded))
                return false;
            paths.insert(folded);
        }
    }
    return true;
}

} // namespace

SettingsHealthReport repairSettings(QSettings& settings)
{
    SettingsHealthReport report;
    const auto repairBoolean = [&](const QString& key, const QString& category) {
        if (settings.contains(key) && !isBoolean(settings.value(key)))
            reset(settings, key, category, report);
    };

    if (settings.contains("appearance/fontSize")) {
        bool numeric = false;
        const double size = settings.value("appearance/fontSize").toDouble(&numeric);
        if (!numeric || !qIsFinite(size) || size < 8.0 || size > 18.0)
            reset(settings, "appearance/fontSize", "appearance", report);
    }
    if (settings.contains("appearance/density")
        && !QStringList{"Compact", "Comfortable", "Spacious"}.contains(
            settings.value("appearance/density").toString()))
        reset(settings, "appearance/density", "appearance", report);
    repairBoolean("appearance/animations", "appearance");
    repairBoolean("privacy/allowWindowsShellMetadataHandlers", "permissions");
    repairBoolean("privacy/allowThirdPartyShellNamespaces", "permissions");
    repairBoolean("privacy/allowWindowsContextMenuExtensions", "permissions");
    repairBoolean("privacy/restorePreviousWorkspace", "permissions");
    repairBoolean("interaction/singleClickActivation", "interaction");
    repairBoolean("interaction/itemSelectionCheckBoxes", "interaction");
    repairBoolean("guidance/firstRunHintShown", "guidance");

    if (settings.contains("navigation/openTo")
        && !QStringList{"home", "this-pc", "last-folder"}.contains(
            settings.value("navigation/openTo").toString()))
        reset(settings, "navigation/openTo", "navigation", report);

    if (settings.contains("navigation/recents")
        && !validPathList(settings.value("navigation/recents"), 20))
        reset(settings, "navigation/recents", "recent-locations", report);
    if (settings.contains("search/indexedRoots")
        && !validPathList(settings.value("search/indexedRoots"), 64))
        reset(settings, "search/indexedRoots", "index-roots", report);
    if (settings.contains("navigation/favorites")
        && !validPathList(settings.value("navigation/favorites"), 256))
        reset(settings, "navigation/favorites", "favorites", report);
    if (settings.contains("navigation/favoriteGroups")
        && !validFavoriteGroups(settings.value("navigation/favoriteGroups")))
        reset(settings, "navigation/favoriteGroups", "favorites", report);
    if (settings.contains("interaction/pinnedContextActions")
        && !validPinnedActions(settings.value("interaction/pinnedContextActions")))
        reset(settings, "interaction/pinnedContextActions", "pinned-actions", report);

    settings.beginGroup("shortcuts");
    QSet<QString> sequences;
    for (const QString& id : settings.childKeys()) {
        const QVariant value = settings.value(id);
        const QString raw = value.toString();
        const QKeySequence sequence = QKeySequence::fromString(raw, QKeySequence::PortableText);
        const QString canonical = sequence.toString(QKeySequence::PortableText);
        if (value.metaType().id() != QMetaType::QString || !validCommandId(id) || raw.size() > 64
            || (!raw.isEmpty() && sequence.isEmpty())
            || raw != canonical || (!canonical.isEmpty() && sequences.contains(canonical))) {
            settings.remove(id);
            if (!report.resetCategories.contains("shortcuts"))
                report.resetCategories.append("shortcuts");
        } else if (!canonical.isEmpty()) {
            sequences.insert(canonical);
        }
    }
    settings.endGroup();

    settings.beginGroup("view/columnProfiles");
    for (const QString& profile : settings.childKeys()) {
        const QVariant value = settings.value(profile);
        if (!validCommandId(profile) || value.metaType().id() != QMetaType::QByteArray
            || value.toByteArray().isEmpty()
            || value.toByteArray().size() > maximumColumnStateBytes) {
            settings.remove(profile);
            if (!report.resetCategories.contains("column-layouts"))
                report.resetCategories.append("column-layouts");
        }
    }
    settings.endGroup();

    if (report.repaired()) {
        settings.setValue("recovery/settings/lastResetUtc",
            QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs));
        settings.setValue("recovery/settings/resetCategories", report.resetCategories);
        settings.setValue("recovery/settings/resetCount", report.resetCount());
    }
    settings.sync();
    return report;
}

SettingsHealthReport repairSettings()
{
    QSettings settings;
    return repairSettings(settings);
}

} // namespace filemanager::settings
