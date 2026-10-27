#include "settings/favoritegroups.h"

#include <QDir>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSet>
#include <QSettings>
#include <QUuid>

namespace filemanager::settings {
namespace {

FavoriteGroup defaultGroup()
{
    return {QStringLiteral("favorites"), QStringLiteral("Favorites"), {}};
}

QString normalizedPath(const QString& path)
{
    return QDir::cleanPath(QDir::fromNativeSeparators(path.trimmed()));
}

} // namespace

void FavoriteGroups::normalize()
{
    QSet<QString> ids;
    QSet<QString> names;
    QSet<QString> paths;
    QList<FavoriteGroup> normalized;
    for (FavoriteGroup group : groups) {
        group.name = group.name.trimmed();
        if (group.name.isEmpty() || names.contains(group.name.toCaseFolded()))
            continue;
        if (group.id.trimmed().isEmpty() || ids.contains(group.id))
            group.id = QUuid::createUuid().toString(QUuid::WithoutBraces);
        ids.insert(group.id);
        names.insert(group.name.toCaseFolded());
        QStringList uniquePaths;
        for (const QString& rawPath : group.paths) {
            const QString path = normalizedPath(rawPath);
            const QString key = path.toCaseFolded();
            if (!path.isEmpty() && !paths.contains(key)) {
                paths.insert(key);
                uniquePaths.append(path);
            }
        }
        group.paths = uniquePaths;
        normalized.append(group);
    }
    if (normalized.isEmpty())
        normalized.append(defaultGroup());
    groups = normalized;
}

bool FavoriteGroups::containsPath(const QString& path) const
{
    const QString key = normalizedPath(path).toCaseFolded();
    for (const FavoriteGroup& group : groups) {
        for (const QString& existing : group.paths) {
            if (normalizedPath(existing).toCaseFolded() == key)
                return true;
        }
    }
    return false;
}

bool FavoriteGroups::movePath(const QString& groupId, const QString& path, int offset)
{
    const QString key = normalizedPath(path).toCaseFolded();
    for (FavoriteGroup& group : groups) {
        if (group.id != groupId)
            continue;
        for (int index = 0; index < group.paths.size(); ++index) {
            if (normalizedPath(group.paths.at(index)).toCaseFolded() != key)
                continue;
            const int destination = qBound(0, index + offset, group.paths.size() - 1);
            if (destination == index)
                return false;
            group.paths.move(index, destination);
            return true;
        }
    }
    return false;
}

FavoriteGroups loadFavoriteGroups(QSettings& settings)
{
    FavoriteGroups result;
    const QByteArray encoded = settings.value("navigation/favoriteGroups").toByteArray();
    const QJsonDocument document = QJsonDocument::fromJson(encoded);
    if (document.isObject() && document.object().value("schemaVersion").toInt() == 1) {
        for (const QJsonValue& groupValue : document.object().value("groups").toArray()) {
            const QJsonObject object = groupValue.toObject();
            FavoriteGroup group{object.value("id").toString(), object.value("name").toString(), {}};
            for (const QJsonValue& path : object.value("paths").toArray())
                group.paths.append(path.toString());
            result.groups.append(group);
        }
    } else {
        FavoriteGroup migrated = defaultGroup();
        migrated.paths = settings.value("navigation/favorites").toStringList();
        result.groups.append(migrated);
    }
    result.normalize();
    return result;
}

void saveFavoriteGroups(QSettings& settings, FavoriteGroups favorites)
{
    favorites.normalize();
    QJsonArray groups;
    for (const FavoriteGroup& group : favorites.groups) {
        QJsonArray paths;
        for (const QString& path : group.paths)
            paths.append(QDir::fromNativeSeparators(path));
        groups.append(QJsonObject{{"id", group.id}, {"name", group.name}, {"paths", paths}});
    }
    const QJsonObject root{{"schemaVersion", 1}, {"groups", groups}};
    settings.setValue("navigation/favoriteGroups",
        QJsonDocument(root).toJson(QJsonDocument::Compact));
    settings.remove("navigation/favorites");
}

} // namespace filemanager::settings
