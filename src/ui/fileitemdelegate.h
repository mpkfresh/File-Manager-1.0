#pragma once

#include <QStyledItemDelegate>

namespace filemanager::preview { class ThumbnailProvider; }

namespace filemanager::ui {

class FileItemDelegate final : public QStyledItemDelegate
{
    Q_OBJECT
public:
    enum DataRole { GroupLabelRole = Qt::UserRole + 100 };
    explicit FileItemDelegate(QObject* parent = nullptr);
    void paint(QPainter* painter, const QStyleOptionViewItem& option,
        const QModelIndex& index) const override;
    QSize sizeHint(const QStyleOptionViewItem& option, const QModelIndex& index) const override;

signals:
    void thumbnailReady();

private:
    filemanager::preview::ThumbnailProvider* thumbnails_ = nullptr;
};

} // namespace filemanager::ui
