#pragma once

#include <QDateTime>
#include <QLocale>
#include <QString>

namespace filemanager::ui {

inline QString localizedInteger(qint64 value, const QLocale& locale = QLocale())
{
    return locale.toString(value);
}

inline QString localizedCount(qint64 value, const QString& singular,
    const QString& plural, const QLocale& locale = QLocale())
{
    return QStringLiteral("%1 %2")
        .arg(locale.toString(value), value == 1 ? singular : plural);
}

inline QString localizedItems(qint64 value, const QLocale& locale = QLocale())
{
    return localizedCount(value, QStringLiteral("item"), QStringLiteral("items"), locale);
}

inline QString localizedFiles(qint64 value, const QLocale& locale = QLocale())
{
    return localizedCount(value, QStringLiteral("file"), QStringLiteral("files"), locale);
}

inline QString localizedEntries(qint64 value, const QLocale& locale = QLocale())
{
    return localizedCount(value, QStringLiteral("entry"), QStringLiteral("entries"), locale);
}

inline QString localizedDateTime(const QDateTime& value,
    QLocale::FormatType format = QLocale::ShortFormat, const QLocale& locale = QLocale())
{
    return value.isValid() ? locale.toString(value.toLocalTime(), format) : QString();
}

inline QString localizedDataSize(qint64 bytes, const QLocale& locale = QLocale())
{
    return locale.formattedDataSize(bytes);
}

} // namespace filemanager::ui
