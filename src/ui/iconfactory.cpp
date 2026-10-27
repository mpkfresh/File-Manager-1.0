#include "iconfactory.h"

#include <QPainter>
#include <QPainterPath>
#include <QPixmap>

namespace filemanager::ui {
namespace {

void drawSymbol(QPainter& painter, Symbol symbol, qreal scale)
{
    const auto point = [scale](qreal x, qreal y) { return QPointF(x * scale, y * scale); };
    switch (symbol) {
    case Symbol::Folder:
    case Symbol::NewFolder: {
        QPainterPath folder;
        folder.moveTo(point(4, 9));
        folder.lineTo(point(11, 9));
        folder.lineTo(point(14, 12));
        folder.lineTo(point(28, 12));
        folder.lineTo(point(28, 25));
        folder.quadTo(point(28, 27), point(26, 27));
        folder.lineTo(point(6, 27));
        folder.quadTo(point(4, 27), point(4, 25));
        folder.closeSubpath();
        painter.drawPath(folder);
        if (symbol == Symbol::NewFolder) {
            painter.drawLine(point(20, 16), point(20, 23));
            painter.drawLine(point(16.5, 19.5), point(23.5, 19.5));
        }
        break;
    }
    case Symbol::Search:
        painter.drawEllipse(QRectF(point(6, 5), point(19, 18)));
        painter.drawLine(point(19, 19), point(27, 27));
        break;
    case Symbol::ContentSearch:
        painter.drawEllipse(QRectF(point(5, 4), point(21, 20)));
        painter.drawLine(point(9, 10), point(17, 10));
        painter.drawLine(point(9, 15), point(16, 15));
        painter.drawLine(point(21, 21), point(27, 27));
        break;
    case Symbol::Command:
        painter.drawRoundedRect(QRectF(point(4, 6), point(28, 26)), 4 * scale, 4 * scale);
        painter.drawLine(point(9, 12), point(14, 16));
        painter.drawLine(point(14, 16), point(9, 20));
        painter.drawLine(point(17, 21), point(23, 21));
        break;
    case Symbol::File:
    case Symbol::Rename:
    case Symbol::Batch: {
        QPainterPath file;
        file.moveTo(point(8, 4)); file.lineTo(point(20, 4)); file.lineTo(point(27, 11));
        file.lineTo(point(27, 28)); file.lineTo(point(8, 28)); file.closeSubpath();
        painter.drawPath(file);
        painter.drawLine(point(20, 4), point(20, 11)); painter.drawLine(point(20, 11), point(27, 11));
        if (symbol == Symbol::Rename) {
            painter.drawLine(point(12, 23), point(23, 12));
            painter.drawLine(point(11, 24), point(15, 23));
        } else if (symbol == Symbol::Batch) {
            painter.drawLine(point(12, 16), point(23, 16));
            painter.drawLine(point(12, 21), point(21, 21));
        }
        break;
    }
    case Symbol::Undo: {
        QPainterPath curve;
        curve.moveTo(point(8, 15));
        curve.cubicTo(point(13, 6), point(25, 8), point(26, 17));
        curve.cubicTo(point(27, 23), point(22, 27), point(16, 26));
        painter.drawPath(curve);
        painter.drawLine(point(8, 15), point(8, 8));
        painter.drawLine(point(8, 15), point(15, 15));
        break;
    }
    case Symbol::Copy:
        painter.drawRoundedRect(QRectF(point(9, 9), point(27, 27)), 2 * scale, 2 * scale);
        painter.drawRoundedRect(QRectF(point(5, 5), point(23, 23)), 2 * scale, 2 * scale);
        break;
    case Symbol::Move:
        painter.drawLine(point(5, 16), point(27, 16));
        painter.drawLine(point(20, 9), point(27, 16));
        painter.drawLine(point(20, 23), point(27, 16));
        break;
    case Symbol::Trash:
        painter.drawRoundedRect(QRectF(point(9, 10), point(25, 28)), 2 * scale, 2 * scale);
        painter.drawLine(point(6, 9), point(28, 9));
        painter.drawLine(point(13, 5), point(21, 5));
        painter.drawLine(point(14, 14), point(14, 24));
        painter.drawLine(point(20, 14), point(20, 24));
        break;
    case Symbol::Open:
        painter.drawRoundedRect(QRectF(point(5, 5), point(27, 27)), 3 * scale, 3 * scale);
        painter.drawLine(point(14, 18), point(25, 7));
        painter.drawLine(point(18, 7), point(25, 7));
        painter.drawLine(point(25, 7), point(25, 14));
        break;
    case Symbol::Inspector:
        painter.drawRoundedRect(QRectF(point(4, 5), point(28, 27)), 3 * scale, 3 * scale);
        painter.drawLine(point(20, 5), point(20, 27));
        painter.drawLine(point(23, 11), point(26, 11));
        painter.drawLine(point(23, 16), point(26, 16));
        break;
    case Symbol::Split:
        painter.drawRoundedRect(QRectF(point(4, 5), point(28, 27)), 3 * scale, 3 * scale);
        painter.drawLine(point(16, 5), point(16, 27));
        break;
    case Symbol::Transfers:
        painter.drawLine(point(5, 11), point(25, 11));
        painter.drawLine(point(20, 6), point(25, 11));
        painter.drawLine(point(25, 21), point(7, 21));
        painter.drawLine(point(12, 16), point(7, 21));
        break;
    }
}

} // namespace

QIcon symbolIcon(Symbol symbol, const QColor& color)
{
    QIcon icon;
    for (const int size : {16, 20, 24, 32}) {
        const qreal scale = size / 32.0;
        QPixmap pixmap(size, size);
        pixmap.fill(Qt::transparent);
        QPainter painter(&pixmap);
        painter.setRenderHint(QPainter::Antialiasing);
        QPen pen(color, qMax(1.2, 2.2 * scale), Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin);
        painter.setPen(pen);
        painter.setBrush(Qt::NoBrush);
        drawSymbol(painter, symbol, scale);
        icon.addPixmap(pixmap);
    }
    return icon;
}

} // namespace filemanager::ui
