#pragma once

#include "layoutmodel.h"

#include <QString>
#include <QWidget>

class FilePane;
class QSplitter;
class QVBoxLayout;
class QDragEnterEvent;
class QDragMoveEvent;
class QDragLeaveEvent;
class QDropEvent;
class QPaintEvent;
class TabbedPanel;

namespace filemanager::workspace {

class WorkspaceView final : public QWidget
{
    Q_OBJECT

public:
    explicit WorkspaceView(QWidget* parent = nullptr);
    explicit WorkspaceView(const QString& initialPath, QWidget* parent = nullptr);

    FilePane* currentPane() const;
    FilePane* adjacentPane() const;
    TabbedPanel* activePanel() const { return activePanel_; }
    QList<TabbedPanel*> panels() const;
    QList<FilePane*> panes() const;

    FilePane* addTab(const QString& path);
    void closeCurrentTab();
    bool moveActiveTabToAdjacent();
    void splitActive(Qt::Orientation orientation, bool insertBefore = false);
    void closeActivePanel();

    LayoutNode layoutState() const;
    bool restoreLayout(const LayoutNode& state, QString* error = nullptr);

signals:
    void paneCreated(FilePane* pane);
    void activePaneChanged(FilePane* pane);
    void layoutChanged();
    void finalTabCloseRequested();

protected:
    void dragEnterEvent(QDragEnterEvent* event) override;
    void dragMoveEvent(QDragMoveEvent* event) override;
    void dragLeaveEvent(QDragLeaveEvent* event) override;
    void dropEvent(QDropEvent* event) override;
    void paintEvent(QPaintEvent* event) override;

private:
    TabbedPanel* createPanel(bool createDefaultTab = true);
    void setActivePanel(TabbedPanel* panel);
    LayoutNode serializeWidget(QWidget* widget) const;
    QWidget* restoreWidget(const LayoutNode& node);
    void collapseSplitter(QSplitter* splitter);
    void updateSplitPreview(const QPoint& position);
    void clearSplitPreview();

    QVBoxLayout* rootLayout_ = nullptr;
    QWidget* rootWidget_ = nullptr;
    TabbedPanel* activePanel_ = nullptr;
    TabbedPanel* dropTargetPanel_ = nullptr;
    QRect dropPreviewRect_;
    Qt::Orientation dropOrientation_ = Qt::Horizontal;
    bool dropBefore_ = false;
    QString initialPath_;
};

} // namespace filemanager::workspace
