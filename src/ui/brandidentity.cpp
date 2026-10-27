#include "ui/brandidentity.h"

#include <QSize>
#include <QResource>
#include <QEvent>
#include <QLabel>
#include <QtMath>

#include <cmath>

static void ensureBrandingResources()
{
    static const bool initialized = [] {
        Q_INIT_RESOURCE(resources);
        return true;
    }();
    Q_UNUSED(initialized);
}

namespace filemanager::ui {

namespace {

class LogoScaleFilter final : public QObject
{
public:
    LogoScaleFilter(QLabel* label, const QSize& logicalSize)
        : QObject(label), label_(label), logicalSize_(logicalSize)
    {
        refresh();
        label_->installEventFilter(this);
    }

protected:
    bool eventFilter(QObject* watched, QEvent* event) override
    {
        if (event->type() == QEvent::DevicePixelRatioChange || event->type() == QEvent::Show) {
            // The preview label may now show file content. Never replace that with branding.
            if (label_->pixmap().cacheKey() == logoCacheKey_)
                refresh();
        }
        return QObject::eventFilter(watched, event);
    }

private:
    void refresh()
    {
        const QPixmap pixmap = BrandIdentity::logo(logicalSize_, label_->devicePixelRatioF());
        label_->setPixmap(pixmap);
        logoCacheKey_ = pixmap.cacheKey();
    }

    QLabel* label_;
    QSize logicalSize_;
    qint64 logoCacheKey_ = 0;
};

} // namespace

QString BrandIdentity::productName()
{
    return QStringLiteral(FILE_MANAGER_PRODUCT_NAME);
}

QString BrandIdentity::logoResourcePath()
{
    ensureBrandingResources();
    return QStringLiteral(":/branding/app-logo.svg");
}

QIcon BrandIdentity::applicationIcon()
{
    ensureBrandingResources();
    // Windows expects square icon canvases; keep the artwork's proportions in padding.
    return QIcon(QStringLiteral(":/branding/app-icon.ico"));
}

QPixmap BrandIdentity::logo(const QSize& logicalSize, qreal devicePixelRatio)
{
    if (logicalSize.isEmpty())
        return {};
    if (!std::isfinite(devicePixelRatio) || devicePixelRatio <= 0.0)
        devicePixelRatio = 1.0;
    const QSize pixelSize(qCeil(logicalSize.width() * devicePixelRatio),
        qCeil(logicalSize.height() * devicePixelRatio));
    // Render vectors at the required physical size; loading a QPixmap first would
    // rasterize at the SVG's nominal size before scaling.
    QPixmap result = QIcon(logoResourcePath()).pixmap(pixelSize, 1.0);
    result.setDevicePixelRatio(devicePixelRatio);
    return result;
}

void BrandIdentity::setLogo(QLabel* label, const QSize& logicalSize)
{
    if (label)
        new LogoScaleFilter(label, logicalSize);
}

} // namespace filemanager::ui
