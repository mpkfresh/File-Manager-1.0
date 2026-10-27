#include "ui/backgroundfileicons.h"
#include "ui/fileiconimage.h"
#include "ui/presentationtiming.h"

#include <QPointer>
#include <QThread>
#include <cmath>
#include <utility>

namespace filemanager::ui {
namespace {
void observe(bool ready)
{
    if (diagnostics::iconReadinessObserver)
        diagnostics::iconReadinessObserver(ready);
}
}

BackgroundFileIcons::BackgroundFileIcons(QObject* parent)
    : QObject(parent), scheduler_(new preview::FileIconScheduler(this))
{ initialize(); }

BackgroundFileIcons::BackgroundFileIcons(preview::FileIconScheduler::Loader loader, QObject* parent)
    : QObject(parent), scheduler_(new preview::FileIconScheduler(std::move(loader), this))
{ initialize(); }

void BackgroundFileIcons::initialize()
{
    repaintTimer_.setSingleShot(true);
    repaintTimer_.setInterval(16);
    connect(&repaintTimer_, &QTimer::timeout, this, [this] {
        const diagnostics::PresentationScope timing(diagnostics::PresentationStage::IconNotifications);
        emit iconsChanged();
    });
    // Capacity also wakes views whose requests were refused at the queue limit.
    // Coalesce completions without continually postponing the next repaint.
    const auto repaint = [this] { if (!repaintTimer_.isActive()) repaintTimer_.start(); };
    connect(scheduler_, &preview::FileIconScheduler::requestFinished, this, repaint);
    connect(scheduler_, &preview::FileIconScheduler::capacityAvailable, this, repaint);
}

QIcon BackgroundFileIcons::icon(const QString& path, const QList<QSize>& nativeSizes, const QString& typeKey)
{
    const QPointer<BackgroundFileIcons> owner(this);
    const quint64 generation = generation_;
    const QString identity = typeKey.isEmpty() ? QStringLiteral("path:") + path
                                             : QStringLiteral("type:") + typeKey;
    return fileIconFromLoader(nativeSizes,
        [owner, path, identity, generation](const QSize& logical, qreal dpr) {
            const qreal physical = logical.width() * dpr;
            if (!owner || !std::isfinite(physical) || physical < 0.5
                    || physical > platform::MaximumFileIconExtent) {
                observe(false);
                return QPixmap();
            }
            return owner->pixels(path, identity, qRound(physical), generation);
        });
}

void BackgroundFileIcons::clear()
{
    Q_ASSERT(thread() == QThread::currentThread());
    ++generation_;
    pixels_.clear();
    scheduler_->clear();
}

void BackgroundFileIcons::navigate()
{
    Q_ASSERT(thread() == QThread::currentThread());
    ++generation_;
    for (const QString& key : pixels_.keys()) {
        if (!key.startsWith(QStringLiteral("type:")))
            pixels_.remove(key);
    }
    // Do not retain helper failures, pending work or path-specific image results.
    scheduler_->clear();
}

QPixmap BackgroundFileIcons::pixels(const QString& path, const QString& identity, int extent, quint64 generation)
{
    Q_ASSERT(thread() == QThread::currentThread());
    if (generation != generation_) {
        observe(false);
        return {};
    }
    const QString key = identity + QLatin1Char('|') + QString::number(extent);
    if (const QPixmap* cached = pixels_.object(key)) {
        observe(true);
        return *cached;
    }
    const auto lookup = scheduler_->request(path, extent);
    if (lookup.state != preview::FileIconScheduler::State::Cached
            || lookup.result.status != platform::FileIconStatus::Ready) {
        observe(false);
        return {};
    }
    QPixmap pixels = QPixmap::fromImage(lookup.result.image);
    pixels.setDevicePixelRatio(1.0);
    if (pixels.isNull()) {
        observe(false);
        return {};
    }
    const qsizetype bytes = lookup.result.image.sizeInBytes()
        + key.size() * qsizetype(sizeof(QChar)) + qsizetype(sizeof(QPixmap));
    const int cost = static_cast<int>(qMax(qsizetype(16 * 1024), bytes));
    pixels_.insert(key, new QPixmap(pixels), cost);
    observe(true);
    return pixels;
}
}
