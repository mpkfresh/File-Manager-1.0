#pragma once

#include <QIcon>
#include <QImage>
#include <QPixmap>
#include <functional>

namespace filemanager::ui {
// GUI-thread conversion of validated native pixels. Never performs Shell lookup.
// nativeSizes comes from the original icon's availableSizes(), which describes
// logical sizing independently of the raster selected for this request.
QIcon fileIconFromImage(const QImage& image, const QList<QSize>& nativeSizes);
// Called only on the GUI thread. A loader must return immediately: an empty
// pixmap reserves the native layout slot until a later repaint supplies pixels.
using FileIconPixelLoader = std::function<QPixmap(const QSize&, qreal)>;
QIcon fileIconFromLoader(const QList<QSize>& nativeSizes, FileIconPixelLoader loader);
}
