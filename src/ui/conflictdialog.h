#pragma once

#include "files/transfercontroller.h"

#include <QDateTime>
#include <QImage>
#include <QString>

class QWidget;

namespace filemanager::ui {

struct ConflictItemInfo
{
    QString path;
    QString name;
    QString type;
    qint64 size = -1;
    QDateTime modified;
    bool directory = false;
    QImage preview;
};

struct ConflictDialogResult
{
    files::ConflictDecision decision = files::ConflictDecision::Cancel;
    bool applyToAll = false;
};

class ConflictDialog final
{
public:
    static constexpr qint64 MaximumPreviewSourceBytes = 16 * 1024 * 1024;
    static constexpr qint64 MaximumPreviewPixels = 40'000'000;
    static constexpr int PreviewWidth = 192;
    static constexpr int PreviewHeight = 128;

    static ConflictItemInfo inspect(const QString& path);
    static ConflictDialogResult run(QWidget* parent, const QString& source,
        const QString& destination, bool directory);
};

} // namespace filemanager::ui
