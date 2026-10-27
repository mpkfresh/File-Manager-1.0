#pragma once

#include <QString>

namespace filemanager::ui {

inline int transferProgressAnnouncementPercent(qint64 completedBytes, qint64 totalBytes)
{
    if (totalBytes <= 0)
        return -1;
    const qint64 bounded = qBound<qint64>(0, completedBytes, totalBytes);
    const int percent = static_cast<int>((bounded * 100) / totalBytes);
    return qMin(100, (percent / 25) * 25);
}

inline QString transferFinishedAnnouncement(bool move, bool cancelled, int errorCount)
{
    const QString operation = move ? QStringLiteral("Move") : QStringLiteral("Copy");
    if (errorCount > 0) {
        return QStringLiteral("%1 incomplete. %2 item failure(s) require attention.")
            .arg(operation).arg(errorCount);
    }
    if (cancelled)
        return operation + QStringLiteral(" cancelled.");
    return operation + QStringLiteral(" completed.");
}

} // namespace filemanager::ui
