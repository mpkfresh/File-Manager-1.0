#include "fileitemdelegate.h"
#include "preview/thumbnailprovider.h"
#include "applicationtheme.h"
#include "presentationtiming.h"

#include <QFileSystemModel>
#include <QApplication>
#include <QPainter>
#include <QStyle>
#include <QWidget>

namespace filemanager::ui {

FileItemDelegate::FileItemDelegate(QObject* parent)
    : QStyledItemDelegate(parent)
{
    thumbnails_ = new filemanager::preview::ThumbnailProvider(this);
    connect(thumbnails_, &filemanager::preview::ThumbnailProvider::thumbnailReady,
        this, &FileItemDelegate::thumbnailReady);
}

void FileItemDelegate::paint(QPainter* painter, const QStyleOptionViewItem& option,
    const QModelIndex& index) const
{
    const diagnostics::PresentationScope timing(diagnostics::PresentationStage::DelegatePaint,
        Qt::DisplayRole, index.column());
    QStyleOptionViewItem content(option);
    const QString group = index.data(GroupLabelRole).toString();
    const QString previousGroup = index.row() > 0
        ? index.sibling(index.row() - 1, index.column()).data(GroupLabelRole).toString()
        : QString();
    const bool startsGroup = !group.isEmpty() && group != previousGroup;
    // The OS preference is sampled when the application theme is applied. Do not call
    // SystemParametersInfo from this per-cell paint path.
    const bool highContrast = applicationHighContrastEnabled();
    if (startsGroup) {
        content.rect.adjust(0, 26, 0, 0);
        if (index.column() == 0) {
            painter->save();
            QFont groupFont = option.font;
            groupFont.setBold(true);
            painter->setFont(groupFont);
            painter->setPen(highContrast ? option.palette.color(QPalette::Text)
                                         : QColor("#b7a8e5"));
            painter->drawText(option.rect.adjusted(6, 3, -4, -3),
                Qt::AlignLeft | Qt::AlignTop, group);
            painter->setPen(highContrast ? option.palette.color(QPalette::Mid)
                                         : QColor(255, 255, 255, 24));
            painter->drawLine(option.rect.left() + 6, option.rect.top() + 23,
                option.rect.right() - 4, option.rect.top() + 23);
            painter->restore();
        }
    }
    {
        const diagnostics::PresentationScope styleOption(diagnostics::PresentationStage::StyleOption,
            Qt::DisplayRole, index.column());
        initStyleOption(&content, index);
    }
    if (index.column() == 0) {
        const QString path = index.data(QFileSystemModel::FilePathRole).toString();
        const diagnostics::PresentationScope request(diagnostics::PresentationStage::ThumbnailRequest,
            Qt::DecorationRole, index.column());
        if (const auto thumbnail = thumbnails_->request(path, QSize(24, 24))) {
            content.icon = *thumbnail;
            content.features |= QStyleOptionViewItem::HasDecoration;
        }
    }
    painter->save();
    painter->setRenderHint(QPainter::Antialiasing);
    if (!highContrast && option.state & QStyle::State_Selected) {
        painter->setPen(Qt::NoPen);
        painter->setBrush(QColor(76, 73, 126, 210));
        painter->drawRoundedRect(option.rect.adjusted(2, 2, -2, -2), 5, 5);
        content.state &= ~QStyle::State_Selected;
    } else if (!highContrast && option.state & QStyle::State_MouseOver) {
        painter->setPen(Qt::NoPen);
        painter->setBrush(QColor(255, 255, 255, 13));
        painter->drawRoundedRect(option.rect.adjusted(2, 2, -2, -2), 5, 5);
        content.state &= ~QStyle::State_MouseOver;
    }
    painter->restore();
    // The option is already initialized. The base paint method would initialize it again,
    // repeat model-role work and replace the completed thumbnail with the model icon.
    const QWidget* widget = content.widget;
    QStyle* style = widget ? widget->style() : QApplication::style();
    if (diagnostics::iconDecorator)
        content.icon = diagnostics::iconDecorator(content.icon);
    {
        const diagnostics::PresentationScope draw(diagnostics::PresentationStage::StyleDraw,
            Qt::DisplayRole, index.column());
        style->drawControl(QStyle::CE_ItemViewItem, &content, painter, widget);
    }
}

QSize FileItemDelegate::sizeHint(
    const QStyleOptionViewItem& option, const QModelIndex& index) const
{
    const diagnostics::PresentationScope timing(diagnostics::PresentationStage::DelegateSizeHint,
        Qt::DisplayRole, index.column());
    QSize size = QStyledItemDelegate::sizeHint(option, index);
    size.setHeight(qMax(size.height(), 32));
    const QString group = index.data(GroupLabelRole).toString();
    const QString previousGroup = index.row() > 0
        ? index.sibling(index.row() - 1, index.column()).data(GroupLabelRole).toString()
        : QString();
    if (!group.isEmpty() && group != previousGroup)
        size.rheight() += 26;
    return size;
}

} // namespace filemanager::ui
