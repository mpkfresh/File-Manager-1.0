#pragma once

#include <QWidget>
#include <QStringList>

class QListWidget;
class QPushButton;

namespace filemanager::ui {

class SearchResultBar final : public QWidget
{
    Q_OBJECT

public:
    explicit SearchResultBar(QListWidget* results, QWidget* parent = nullptr);

    QString currentPath() const;
    QStringList selectedPaths() const;

signals:
    void openRequested(const QString& path);
    void revealRequested(const QString& path);
    void clipboardRequested(const QStringList& paths, bool cut);
    void shareRequested(const QStringList& paths);
    void recycleRequested(const QStringList& paths);

private:
    void updateActions();

    QListWidget* results_ = nullptr;
    QPushButton* open_ = nullptr;
    QPushButton* reveal_ = nullptr;
    QPushButton* copy_ = nullptr;
    QPushButton* cut_ = nullptr;
    QPushButton* share_ = nullptr;
    QPushButton* recycle_ = nullptr;
};

} // namespace filemanager::ui
