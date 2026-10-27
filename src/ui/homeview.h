#pragma once

#include <QWidget>

class QLabel;
class QTreeWidget;
class QTreeWidgetItem;

namespace filemanager::ui {

class HomeView final : public QWidget
{
    Q_OBJECT

public:
    explicit HomeView(QWidget* parent = nullptr);

    void refresh();
    QString selectedPath() const;
    QString selectedSavedSearchId() const;
    int itemCount() const;
    QWidget* focusTarget() const;
    void activateCurrent();

signals:
    void pathActivated(const QString& path);
    void backgroundPathRequested(const QString& path);
    void savedSearchActivated(const QString& id);
    void selectionChanged();

protected:
    bool eventFilter(QObject* watched, QEvent* event) override;

private:
    void activateItem(QTreeWidgetItem* item);

    QTreeWidget* locations_ = nullptr;
    QLabel* emptyState_ = nullptr;
    QLabel* status_ = nullptr;
    int itemCount_ = 0;
};

} // namespace filemanager::ui
