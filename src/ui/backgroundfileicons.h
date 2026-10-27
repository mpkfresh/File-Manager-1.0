#pragma once

#include "preview/fileiconscheduler.h"
#include <QCache>
#include <QIcon>
#include <QPixmap>
#include <QTimer>

namespace filemanager::ui {

// Own one per folder model. All methods and pixmap conversions run on the GUI
// thread; only the scheduler's loader invokes the native helper on workers.
class BackgroundFileIcons final : public QObject {
    Q_OBJECT
public:
    explicit BackgroundFileIcons(QObject* parent = nullptr);
    explicit BackgroundFileIcons(preview::FileIconScheduler::Loader loader, QObject* parent = nullptr);
    // typeKey is supplied only for artwork the model already shares by file type.
    QIcon icon(const QString& path, const QList<QSize>& nativeSizes, const QString& typeKey = {});
    void clear();
    // Revoke handles/pending work; retain only completed type-shared pixels.
    void navigate();
    static constexpr int PixmapCacheBudget = 8 * 1024 * 1024;
    int pixmapCacheCost() const { return static_cast<int>(pixels_.totalCost()); }

signals:
    void iconsChanged();

private:
    void initialize();
    QPixmap pixels(const QString& path, const QString& identity, int extent, quint64 generation);
    preview::FileIconScheduler* scheduler_;
    QCache<QString, QPixmap> pixels_{PixmapCacheBudget};
    QTimer repaintTimer_;
    quint64 generation_ = 0;
};
}
