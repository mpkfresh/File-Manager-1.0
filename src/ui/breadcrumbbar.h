#pragma once

#include <QWidget>

class QHBoxLayout;
class QLineEdit;
class QStackedWidget;

namespace filemanager::ui {

class BreadcrumbBar final : public QWidget
{
    Q_OBJECT

public:
    explicit BreadcrumbBar(QWidget* parent = nullptr);

    static QString normalizeAddressInput(const QString& input);

    QLineEdit* editor() const { return editor_; }
    QString path() const;
    bool isEditing() const;
    void setPath(const QString& path);
    void setShellLocation(const QString& displayName, const QString& identity);

public slots:
    void beginEditing();
    void showBreadcrumbs();

signals:
    void pathSubmitted(const QString& path);
    void pathActivated(const QString& path);
    void backgroundPathRequested(const QString& path);

protected:
    bool eventFilter(QObject* watched, QEvent* event) override;

private:
    void rebuild(const QString& path, const QString& displayName = {});
    void populateMenuWhenShown(QObject* menuObject, const QString& parentPath);

    QStackedWidget* stack_ = nullptr;
    QWidget* breadcrumbHost_ = nullptr;
    QHBoxLayout* breadcrumbLayout_ = nullptr;
    QLineEdit* editor_ = nullptr;
    QString currentPath_;
};

} // namespace filemanager::ui
