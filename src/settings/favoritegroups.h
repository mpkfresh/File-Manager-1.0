#pragma once

#include <QList>
#include <QString>
#include <QStringList>

class QSettings;

namespace filemanager::settings {

struct FavoriteGroup
{
    QString id;
    QString name;
    QStringList paths;

    bool operator==(const FavoriteGroup&) const = default;
};

struct FavoriteGroups
{
    QList<FavoriteGroup> groups;

    void normalize();
    bool containsPath(const QString& path) const;
    bool movePath(const QString& groupId, const QString& path, int offset);
    bool operator==(const FavoriteGroups&) const = default;
};

FavoriteGroups loadFavoriteGroups(QSettings& settings);
void saveFavoriteGroups(QSettings& settings, FavoriteGroups favorites);

} // namespace filemanager::settings
