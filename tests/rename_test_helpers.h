#pragma once

#include "ui/batchrenamedialog.h"

#include <QElapsedTimer>
#include <QEventLoop>
#include <QTimer>

namespace filemanager::tests {

inline bool waitForRenamePreview(ui::BatchRenameDialog& dialog, int timeoutMs = 15000)
{
    QElapsedTimer deadline;
    deadline.start();
    while (dialog.previewPending() && deadline.elapsed() < timeoutMs) {
        QEventLoop events;
        QTimer::singleShot(5, &events, &QEventLoop::quit);
        events.exec();
    }
    return !dialog.previewPending();
}

} // namespace filemanager::tests
