#pragma once

#include <QByteArray>
#include <QHash>
#include <QSet>
#include <QString>
#include <QStringList>

namespace filemanager::settings {

struct PortablePreferences final
{
    qreal fontPointSize = 9.5;
    QString density = QStringLiteral("Comfortable");
    bool animations = true;
    bool singleClickActivation = false;
    QHash<QString, QString> shortcuts;
    QStringList pinnedContextActions;
};

QByteArray createPortablePreferences(const PortablePreferences& preferences);
bool parsePortablePreferences(const QByteArray& content, const QSet<QString>& allowedCommandIds,
    PortablePreferences* preferences, QString* error = nullptr);
bool writePortablePreferences(const QString& destination,
    const PortablePreferences& preferences, QString* error = nullptr);
bool readPortablePreferences(const QString& source, const QSet<QString>& allowedCommandIds,
    PortablePreferences* preferences, QString* error = nullptr);

} // namespace filemanager::settings
