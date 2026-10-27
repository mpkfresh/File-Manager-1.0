#pragma once

#include <QIcon>
#include <QPixmap>
#include <QString>

class QLabel;

namespace filemanager::ui {

class BrandIdentity final
{
public:
    static QString productName();
    static QString logoResourcePath();
    static QIcon applicationIcon();
    static QPixmap logo(const QSize& logicalSize, qreal devicePixelRatio = 1.0);
    static void setLogo(QLabel* label, const QSize& logicalSize);
};

} // namespace filemanager::ui
