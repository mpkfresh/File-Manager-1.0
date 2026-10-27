#include "workspaceview.h"

#include "ui/filepane.h"
#include "ui/tabbedpanel.h"

#include <QDir>
#include <QDragEnterEvent>
#include <QDragLeaveEvent>
#include <QDropEvent>
#include <QFileInfo>
#include <QMimeData>
#include <QPainter>
#include <QSplitter>
#include <QVBoxLayout>

namespace filemanager::workspace {

WorkspaceView::WorkspaceView(QWidget* parent)
    : WorkspaceView(QDir::homePath(), parent)
{
}

WorkspaceView::WorkspaceView(const QString& initialPath, QWidget* parent)
    : QWidget(parent)
{
    const bool appLocation = initialPath == QLatin1String(FilePane::HomeIdentity)
        || filemanager::platform::ShellItem::builtInLocationFromIdentity(initialPath)
            != filemanager::platform::BuiltInShellLocation::None;
    initialPath_ = appLocation || QFileInfo(initialPath).isDir()
        ? initialPath : QDir::homePath();
    rootLayout_ = new QVBoxLayout(this);
    rootLayout_->setContentsMargins(0, 0, 0, 0);
    rootLayout_->setSpacing(0);
    setAcceptDrops(true);
    rootWidget_ = createPanel();
    rootLayout_->addWidget(rootWidget_);
}

FilePane* WorkspaceView::currentPane() const
{
    return activePanel_ ? activePanel_->currentPane() : nullptr;
}

QList<TabbedPanel*> WorkspaceView::panels() const
{
    QList<TabbedPanel*> result;
    if (!rootWidget_)
        return result;
    if (auto* panel = qobject_cast<TabbedPanel*>(rootWidget_))
        result.append(panel);
    result.append(rootWidget_->findChildren<TabbedPanel*>());
    return result;
}

QList<FilePane*> WorkspaceView::panes() const
{
    QList<FilePane*> result;
    for (TabbedPanel* panel : panels())
        result.append(panel->panes());
    return result;
}

FilePane* WorkspaceView::adjacentPane() const
{
    const QList<TabbedPanel*> allPanels = panels();
    if (allPanels.size() < 2)
        return nullptr;
    const int activeIndex = allPanels.indexOf(activePanel_);
    return allPanels.at((activeIndex + 1) % allPanels.size())->currentPane();
}

FilePane* WorkspaceView::addTab(const QString& path)
{
    if (!activePanel_)
        return nullptr;
    return activePanel_->addTab(path);
}

void WorkspaceView::closeCurrentTab()
{
    if (activePanel_)
        activePanel_->closeCurrentTab();
}

bool WorkspaceView::moveActiveTabToAdjacent()
{
    const QList<TabbedPanel*> allPanels = panels();
    if (!activePanel_ || allPanels.size() < 2)
        return false;
    const int activeIndex = allPanels.indexOf(activePanel_);
    TabbedPanel* destination = allPanels.at((activeIndex + 1) % allPanels.size());
    if (activePanel_->count() == 1)
        activePanel_->addTab(initialPath_, false);
    FilePane* pane = activePanel_->takeCurrentTab();
    if (!pane)
        return false;
    destination->adoptTab(pane);
    setActivePanel(destination);
    emit activePaneChanged(pane);
    emit layoutChanged();
    return true;
}

TabbedPanel* WorkspaceView::createPanel(bool createDefaultTab)
{
    auto* panel = new TabbedPanel(initialPath_, this, createDefaultTab);
    connect(panel, &TabbedPanel::paneCreated, this, &WorkspaceView::paneCreated);
    connect(panel, &TabbedPanel::activePaneChanged, this, [this, panel](FilePane* pane) {
        setActivePanel(panel);
        emit activePaneChanged(pane);
    });
    connect(panel, &TabbedPanel::tabsChanged, this, &WorkspaceView::layoutChanged);
    connect(panel, &TabbedPanel::lastTabCloseRequested, this, [this, panel] {
        setActivePanel(panel);
        if (panels().size() > 1)
            closeActivePanel();
        else
            emit finalTabCloseRequested();
    });
    for (FilePane* pane : panel->panes())
        emit paneCreated(pane);
    if (!activePanel_)
        activePanel_ = panel;
    return panel;
}

void WorkspaceView::setActivePanel(TabbedPanel* panel)
{
    if (panel)
        activePanel_ = panel;
}

void WorkspaceView::splitActive(Qt::Orientation orientation, bool insertBefore)
{
    if (!activePanel_)
        return;

    TabbedPanel* original = activePanel_;
    TabbedPanel* added = createPanel();
    auto* parentSplitter = qobject_cast<QSplitter*>(original->parentWidget());

    if (parentSplitter && parentSplitter->orientation() == orientation) {
        const int index = parentSplitter->indexOf(original);
        parentSplitter->insertWidget(index + (insertBefore ? 0 : 1), added);
    } else {
        auto* splitter = new QSplitter(orientation);
        if (parentSplitter) {
            const int index = parentSplitter->indexOf(original);
            parentSplitter->replaceWidget(index, splitter);
        } else {
            rootLayout_->replaceWidget(original, splitter);
            rootWidget_ = splitter;
        }
        if (insertBefore) {
            splitter->addWidget(added);
            splitter->addWidget(original);
        } else {
            splitter->addWidget(original);
            splitter->addWidget(added);
        }
        splitter->setStretchFactor(0, 1);
        splitter->setStretchFactor(1, 1);
        connect(splitter, &QSplitter::splitterMoved, this, &WorkspaceView::layoutChanged);
    }

    setActivePanel(added);
    emit activePaneChanged(added->currentPane());
    emit layoutChanged();
}

void WorkspaceView::closeActivePanel()
{
    if (!activePanel_ || panels().size() <= 1)
        return;
    auto* parentSplitter = qobject_cast<QSplitter*>(activePanel_->parentWidget());
    if (!parentSplitter)
        return;
    TabbedPanel* closing = activePanel_;
    activePanel_ = nullptr;
    closing->setParent(nullptr);
    closing->deleteLater();
    collapseSplitter(parentSplitter);
    const QList<TabbedPanel*> remaining = panels();
    if (!remaining.isEmpty())
        setActivePanel(remaining.first());
    emit activePaneChanged(currentPane());
    emit layoutChanged();
}

void WorkspaceView::collapseSplitter(QSplitter* splitter)
{
    if (!splitter || splitter->count() != 1)
        return;
    QWidget* survivor = splitter->widget(0);
    survivor->setParent(nullptr);
    auto* parentSplitter = qobject_cast<QSplitter*>(splitter->parentWidget());
    if (parentSplitter) {
        const int index = parentSplitter->indexOf(splitter);
        parentSplitter->replaceWidget(index, survivor);
    } else {
        rootLayout_->replaceWidget(splitter, survivor);
        rootWidget_ = survivor;
    }
    splitter->deleteLater();
    collapseSplitter(parentSplitter);
}

LayoutNode WorkspaceView::layoutState() const
{
    return serializeWidget(rootWidget_);
}

LayoutNode WorkspaceView::serializeWidget(QWidget* widget) const
{
    if (auto* panel = qobject_cast<TabbedPanel*>(widget)) {
        PanelState state{panel->paths(), panel->currentIndex(), panel->viewModes(),
            panel->hiddenStates(), panel->folderPlacements(), panel->extensionStates(),
            panel->groupings()};
        for (const FilePaneRestoreState& restored : panel->restoreStates()) {
            state.selectedPaths.append(restored.selectedPaths);
            state.focusedPaths.append(restored.focusedPath);
            state.expandedPaths.append(restored.expandedPaths);
            state.verticalScrolls.append(restored.verticalScroll);
            state.horizontalScrolls.append(restored.horizontalScroll);
        }
        return LayoutNode::panelNode(std::move(state));
    }
    auto* splitter = qobject_cast<QSplitter*>(widget);
    QList<LayoutNode> children;
    for (int index = 0; splitter && index < splitter->count(); ++index)
        children.append(serializeWidget(splitter->widget(index)));
    return LayoutNode::splitNode(
        splitter && splitter->orientation() == Qt::Vertical
            ? SplitDirection::Vertical
            : SplitDirection::Horizontal,
        children,
        splitter ? splitter->sizes() : QList<int>{});
}

bool WorkspaceView::restoreLayout(const LayoutNode& state, QString* error)
{
    if (!state.isValid(error))
        return false;
    QWidget* restored = restoreWidget(state);
    if (!restored) {
        if (error)
            *error = "The workspace widget tree could not be created.";
        return false;
    }
    if (rootWidget_) {
        rootLayout_->removeWidget(rootWidget_);
        rootWidget_->deleteLater();
    }
    rootWidget_ = restored;
    rootLayout_->addWidget(rootWidget_);
    const QList<TabbedPanel*> restoredPanels = panels();
    activePanel_ = restoredPanels.isEmpty() ? nullptr : restoredPanels.first();
    emit activePaneChanged(currentPane());
    emit layoutChanged();
    return true;
}

QWidget* WorkspaceView::restoreWidget(const LayoutNode& node)
{
    if (node.type == NodeType::Panel) {
        TabbedPanel* panel = createPanel(false);
        QList<FilePaneRestoreState> restoredStates;
        for (int index = 0; index < node.panel.tabPaths.size(); ++index) {
            restoredStates.append({node.panel.selectedPaths.value(index),
                node.panel.focusedPaths.value(index), node.panel.expandedPaths.value(index),
                node.panel.verticalScrolls.value(index),
                node.panel.horizontalScrolls.value(index)});
        }
        panel->restoreTabs(node.panel.tabPaths, node.panel.currentTab,
            node.panel.viewModes, node.panel.showHidden, node.panel.folderPlacements,
            node.panel.showFileExtensions, node.panel.groupings, restoredStates);
        return panel;
    }
    auto* splitter = new QSplitter(
        node.direction == SplitDirection::Horizontal ? Qt::Horizontal : Qt::Vertical, this);
    connect(splitter, &QSplitter::splitterMoved, this, &WorkspaceView::layoutChanged);
    for (const LayoutNode& child : node.children)
        splitter->addWidget(restoreWidget(child));
    if (node.sizes.size() == splitter->count())
        splitter->setSizes(node.sizes);
    return splitter;
}

void WorkspaceView::dragEnterEvent(QDragEnterEvent* event)
{
    int sourceIndex = -1;
    if (event->possibleActions().testFlag(Qt::MoveAction)
            && TabbedPanel::resolveTabDrag(
                event->mimeData()->data(TabbedPanel::TabMimeType), this, &sourceIndex)) {
        updateSplitPreview(event->position().toPoint());
        event->acceptProposedAction();
    }
}

void WorkspaceView::dragMoveEvent(QDragMoveEvent* event)
{
    int sourceIndex = -1;
    if (event->possibleActions().testFlag(Qt::MoveAction)
            && TabbedPanel::resolveTabDrag(
                event->mimeData()->data(TabbedPanel::TabMimeType), this, &sourceIndex)) {
        updateSplitPreview(event->position().toPoint());
        event->acceptProposedAction();
    }
}

void WorkspaceView::dragLeaveEvent(QDragLeaveEvent* event)
{
    clearSplitPreview();
    event->accept();
}

void WorkspaceView::dropEvent(QDropEvent* event)
{
    int sourceIndex = -1;
    auto* source = TabbedPanel::resolveTabDrag(
        event->mimeData()->data(TabbedPanel::TabMimeType), this, &sourceIndex);
    if (source && event->possibleActions().testFlag(Qt::MoveAction)) {
        // Recompute the target at drop time, before any layout mutation.
        updateSplitPreview(event->position().toPoint());
    }
    if (source && event->possibleActions().testFlag(Qt::MoveAction) && dropTargetPanel_) {
        setActivePanel(dropTargetPanel_);
        splitActive(dropOrientation_, dropBefore_);
        if (activePanel_->transferTabFrom(source, sourceIndex, 0)) {
            event->setDropAction(Qt::MoveAction);
            event->accept();
        }
    }
    clearSplitPreview();
}

void WorkspaceView::paintEvent(QPaintEvent* event)
{
    QWidget::paintEvent(event);
    if (dropPreviewRect_.isEmpty())
        return;
    QPainter painter(this);
    const QColor highlight = palette().color(QPalette::Highlight);
    painter.setPen(QPen(highlight, 3));
    painter.setBrush(QBrush(highlight, Qt::Dense4Pattern));
    painter.drawRect(dropPreviewRect_.adjusted(1, 1, -2, -2));
}

void WorkspaceView::updateSplitPreview(const QPoint& position)
{
    QWidget* candidate = childAt(position);
    while (candidate && candidate != this && !qobject_cast<TabbedPanel*>(candidate))
        candidate = candidate->parentWidget();
    dropTargetPanel_ = qobject_cast<TabbedPanel*>(candidate);
    if (!dropTargetPanel_) {
        clearSplitPreview();
        return;
    }
    const QRect panelRect(dropTargetPanel_->mapTo(this, QPoint()), dropTargetPanel_->size());
    const QPoint local = position - panelRect.topLeft();
    const int left = local.x();
    const int right = panelRect.width() - local.x();
    const int top = local.y();
    const int bottom = panelRect.height() - local.y();
    const int nearest = qMin(qMin(left, right), qMin(top, bottom));
    dropOrientation_ = (nearest == left || nearest == right) ? Qt::Horizontal : Qt::Vertical;
    dropBefore_ = nearest == left || nearest == top;
    dropPreviewRect_ = panelRect;
    if (dropOrientation_ == Qt::Horizontal)
        dropPreviewRect_.setWidth(panelRect.width() / 2);
    else
        dropPreviewRect_.setHeight(panelRect.height() / 2);
    if (!dropBefore_)
        dropPreviewRect_.moveBottomRight(panelRect.bottomRight());
    update();
}

void WorkspaceView::clearSplitPreview()
{
    dropTargetPanel_ = nullptr;
    dropPreviewRect_ = {};
    update();
}

} // namespace filemanager::workspace
