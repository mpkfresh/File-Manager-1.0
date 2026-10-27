#include "ui/fileiconimage.h"
#include "platform/fileicon.h"

#include <QPixmap>
#include <QIconEngine>
#include <QPainter>
#include <QGuiApplication>
#include <utility>

namespace filemanager::ui {
namespace {
class NativeImageIconEngine final : public QIconEngine {
public:
    NativeImageIconEngine(FileIconPixelLoader loader, QList<QSize> sizes)
        : m_loader(std::move(loader)), m_sizes(std::move(sizes)) {}

    QIconEngine* clone() const override { return new NativeImageIconEngine(*this); }
    bool isNull() override { return !m_loader; }
    QList<QSize> availableSizes(QIcon::Mode, QIcon::State) override { return m_sizes; }

    QSize actualSize(const QSize& requested, QIcon::Mode, QIcon::State) override
    {
        // Native logical metrics use the available icon sizes, not the raster
        // selected for a particular physical-size request.
        QSize selected;
        const auto area = [](QSize size) { return qint64(size.width()) * size.height(); };
        for (const QSize& candidate : m_sizes) {
            if (selected.isEmpty()
                    || (area(candidate) >= area(requested)
                        && (area(selected) < area(requested) || area(candidate) < area(selected)))
                    || (area(selected) < area(requested) && area(candidate) > area(selected)))
                selected = candidate;
        }
        return selected.isEmpty() ? QSize() : selected.scaled(requested, Qt::KeepAspectRatio)
            .boundedTo(selected);
    }

    QPixmap pixmap(const QSize& size, QIcon::Mode mode, QIcon::State state) override
    { return scaledPixmap(size, mode, state, 1.0); }

    QPixmap scaledPixmap(const QSize& size, QIcon::Mode, QIcon::State, qreal scale) override
    {
        // Qt's Windows file engine returns the native raster without mode
        // tinting or resampling. QIcon assigns its resulting device pixel ratio.
        return size.isValid() ? m_loader(size, scale) : QPixmap();
    }

    void paint(QPainter* painter, const QRect& rect, QIcon::Mode, QIcon::State) override
    {
        const qreal dpr = painter->device() ? painter->device()->devicePixelRatioF()
                                           : qGuiApp->devicePixelRatio();
        painter->drawPixmap(rect, m_loader(rect.size(), dpr));
    }

private:
    FileIconPixelLoader m_loader;
    QList<QSize> m_sizes;
};
}

QIcon fileIconFromImage(const QImage& image, const QList<QSize>& nativeSizes)
{
    if (image.isNull() || image.width() > platform::MaximumFileIconExtent
            || image.height() > platform::MaximumFileIconExtent || nativeSizes.isEmpty())
        return {};
    QPixmap pixels = QPixmap::fromImage(image);
    pixels.setDevicePixelRatio(1.0);
    return fileIconFromLoader(nativeSizes, [pixels](const QSize&, qreal) { return pixels; });
}

QIcon fileIconFromLoader(const QList<QSize>& nativeSizes, FileIconPixelLoader loader)
{
    if (!loader || nativeSizes.isEmpty())
        return {};
    for (const QSize& size : nativeSizes) {
        if (size.isEmpty() || size.width() > platform::MaximumFileIconExtent
                || size.height() > platform::MaximumFileIconExtent)
            return {};
    }
    return QIcon(new NativeImageIconEngine(std::move(loader), nativeSizes));
}
}
