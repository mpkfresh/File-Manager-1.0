#include "tabbedpanel.h"
#include "applicationtheme.h"

#include "filepane.h"

#include <QApplication>
#include <QDir>
#include <QDrag>
#include <QDragEnterEvent>
#include <QDropEvent>
#include <QFileInfo>
#include <QHash>
#include <QMimeData>
#include <QMouseEvent>
#include <QPointer>
#include <QTabBar>
#include <QTabWidget>
#include <QToolButton>
#include <QVBoxLayout>
#include <QUuid>

#include <functional>

namespace {

QHash<QByteArray, QPointer<TabbedPanel>>& registeredPanels()
{
    static QHash<QByteArray, QPointer<TabbedPanel>> panels;
    return panels;
}

class TransferTabBar final : public QTabBar
{
public:
    explicit TransferTabBar(TabbedPanel* owner)
        : QTabBar(owner), owner_(owner), token_(QUuid::createUuid().toByteArray())
    {
        setAcceptDrops(true);
        registeredPanels().insert(token_, owner);
    }
    ~TransferTabBar() override { registeredPanels().remove(token_); }

    std::function<bool(TabbedPanel*, int, int)> transfer;
    QByteArray payload(const QByteArray& paneToken) const { return token_ + ':' + paneToken; }

protected:
    void mousePressEvent(QMouseEvent* event) override
    {
        pressedPosition_ = event->position().toPoint();
        pressedIndex_ = tabAt(pressedPosition_);
        QTabBar::mousePressEvent(event);
    }

    void mouseMoveEvent(QMouseEvent* event) override
    {
        if (!(event->buttons() & Qt::LeftButton) || pressedIndex_ < 0
            || (event->position().toPoint() - pressedPosition_).manhattanLength()
                < QApplication::startDragDistance()) {
            QTabBar::mouseMoveEvent(event);
            return;
        }
        auto* drag = new QDrag(this);
        auto* mime = new QMimeData;
        mime->setData(TabbedPanel::TabMimeType, owner_->tabDragPayload(pressedIndex_));
        drag->setMimeData(mime);
        drag->setPixmap(grab(tabRect(pressedIndex_)));
        drag->exec(Qt::MoveAction);
        pressedIndex_ = -1;
    }

    void dragEnterEvent(QDragEnterEvent* event) override
    {
        int sourceIndex = -1;
        if (event->possibleActions().testFlag(Qt::MoveAction)
                && TabbedPanel::resolveTabDrag(
                    event->mimeData()->data(TabbedPanel::TabMimeType), this, &sourceIndex))
            event->acceptProposedAction();
    }

    void dragMoveEvent(QDragMoveEvent* event) override
    {
        int sourceIndex = -1;
        if (event->possibleActions().testFlag(Qt::MoveAction)
                && TabbedPanel::resolveTabDrag(
                    event->mimeData()->data(TabbedPanel::TabMimeType), this, &sourceIndex))
            event->acceptProposedAction();
    }

    void dropEvent(QDropEvent* event) override
    {
        int sourceIndex = -1;
        TabbedPanel* source = TabbedPanel::resolveTabDrag(
            event->mimeData()->data(TabbedPanel::TabMimeType), this, &sourceIndex);
        if (!source || !transfer || !event->possibleActions().testFlag(Qt::MoveAction))
            return;
        int destinationIndex = tabAt(event->position().toPoint());
        if (destinationIndex < 0)
            destinationIndex = count();
        if (transfer(source, sourceIndex, destinationIndex)) {
            event->setDropAction(Qt::MoveAction);
            event->accept();
        }
    }

private:
    TabbedPanel* owner_ = nullptr;
    QByteArray token_;
    QPoint pressedPosition_;
    int pressedIndex_ = -1;
};

class TransferTabWidget final : public QTabWidget
{
public:
    using QTabWidget::QTabWidget;

    void installTabBar(QTabBar* tabBar) { setTabBar(tabBar); }
};

} // namespace

TabbedPanel::TabbedPanel(QWidget* parent, bool createDefaultTab)
    : TabbedPanel(QDir::homePath(), parent, createDefaultTab)
{
}

TabbedPanel::TabbedPanel(const QString& initialPath, QWidget* parent, bool createDefaultTab)
    : QWidget(parent)
{
    const bool appLocation = initialPath == QLatin1String(FilePane::HomeIdentity)
        || filemanager::platform::ShellItem::builtInLocationFromIdentity(initialPath)
            != filemanager::platform::BuiltInShellLocation::None;
    defaultPath_ = appLocation || QFileInfo(initialPath).isDir()
        ? initialPath : QDir::homePath();
    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);

    auto* transferTabs = new TransferTabWidget(this);
    tabs_ = transferTabs;
    auto* tabBar = new TransferTabBar(this);
    transferTabs->installTabBar(tabBar);
    tabBar->setObjectName("FolderTabBar");
    tabBar->setAccessibleName("Folder tabs");
    tabBar->setAccessibleDescription(
        "Open folder tabs; use the arrow keys to choose a tab");
    tabBar->setFocusPolicy(Qt::StrongFocus);
    tabs_->setDocumentMode(true);
    tabs_->setMovable(true);
    tabs_->setTabsClosable(false);
    tabs_->tabBar()->setElideMode(Qt::ElideMiddle);
    layout->addWidget(tabs_);
    tabBar->transfer = [this](TabbedPanel* source, int sourceIndex, int destinationIndex) {
        return transferTabFrom(source, sourceIndex, destinationIndex);
    };

    connect(tabs_, &QTabWidget::currentChanged, this, [this](int) {
        emit activePaneChanged(currentPane());
        emit tabsChanged();
    });

    if (createDefaultTab)
        addTab(defaultPath_);
}

FilePane* TabbedPanel::currentPane() const
{
    return qobject_cast<FilePane*>(tabs_->currentWidget());
}

QList<FilePane*> TabbedPanel::panes() const
{
    QList<FilePane*> result;
    for (int index = 0; index < tabs_->count(); ++index) {
        if (auto* pane = qobject_cast<FilePane*>(tabs_->widget(index)))
            result.append(pane);
    }
    return result;
}

int TabbedPanel::count() const { return tabs_->count(); }
int TabbedPanel::currentIndex() const { return tabs_->currentIndex(); }
QTabBar* TabbedPanel::tabBar() const { return tabs_->tabBar(); }

QStringList TabbedPanel::paths() const
{
    QStringList result;
    for (FilePane* pane : panes()) {
        const QString identity = pane->serializedLocation();
        result.append(identity.isEmpty() ? defaultPath_ : identity);
    }
    return result;
}

QList<int> TabbedPanel::viewModes() const
{
    QList<int> result;
    for (FilePane* pane : panes())
        result.append(static_cast<int>(pane->viewMode()));
    return result;
}

QList<bool> TabbedPanel::hiddenStates() const
{
    QList<bool> result;
    for (FilePane* pane : panes())
        result.append(pane->showHidden());
    return result;
}

QList<int> TabbedPanel::folderPlacements() const
{
    QList<int> result;
    for (FilePane* pane : panes())
        result.append(static_cast<int>(pane->folderPlacement()));
    return result;
}

QList<bool> TabbedPanel::extensionStates() const
{
    QList<bool> result;
    for (FilePane* pane : panes())
        result.append(pane->showFileExtensions());
    return result;
}

QList<int> TabbedPanel::groupings() const
{
    QList<int> result;
    for (FilePane* pane : panes())
        result.append(static_cast<int>(pane->groupBy()));
    return result;
}

QList<FilePaneRestoreState> TabbedPanel::restoreStates() const
{
    QList<FilePaneRestoreState> result;
    for (FilePane* pane : panes())
        result.append(pane->restoreState());
    return result;
}

FilePane* TabbedPanel::addTab(const QString& path, bool makeCurrent)
{
    auto* pane = new FilePane(path, tabs_);
    attachPane(pane, makeCurrent);
    return pane;
}

void TabbedPanel::attachPane(FilePane* pane, bool makeCurrent)
{
    if (pane->property("tabDragIdentity").toByteArray().isEmpty())
        pane->setProperty("tabDragIdentity", QUuid::createUuid().toByteArray());
    const int index = tabs_->addTab(pane, QString());
    auto* close = new QToolButton(tabs_->tabBar());
    close->setText("×");
    close->setToolTip("Close tab");
    close->setAccessibleName("Close tab");
    close->setAutoRaise(true);
    close->setFixedSize(20, 20);
    if (!filemanager::ui::applicationHighContrastEnabled()) {
        close->setStyleSheet("QToolButton{color:#9296a3;border:0;border-radius:4px;padding:0;}"
                             "QToolButton:hover{background:#3a304d;color:#fff;}");
    }
    tabs_->tabBar()->setTabButton(index, QTabBar::RightSide, close);
    connect(close, &QToolButton::clicked, this, [this, pane] {
        closeTab(tabs_->indexOf(pane));
    });
    updateTabTitle(pane, pane->isFileSystemLocation()
        ? pane->currentPath() : pane->currentLocation());
    connect(pane, &FilePane::pathChanged, this, [this, pane](const QString& newPath) {
        updateTabTitle(pane, newPath);
        emit tabsChanged();
    });
    connect(pane, &FilePane::locationChanged, this,
        [this, pane](const QString& displayLocation) {
            if (!pane->isFileSystemLocation()) {
                updateTabTitle(pane, displayLocation);
                emit tabsChanged();
            }
        });
    connect(pane, &FilePane::viewStateChanged, this, &TabbedPanel::tabsChanged);
    if (makeCurrent)
        tabs_->setCurrentIndex(index);
    emit paneCreated(pane);
    emit tabsChanged();
}

FilePane* TabbedPanel::takeCurrentTab()
{
    return takeTab(tabs_->currentIndex());
}

FilePane* TabbedPanel::takeTab(int index)
{
    auto* pane = qobject_cast<FilePane*>(tabs_->widget(index));
    if (!pane)
        return nullptr;
    if (tabs_->count() == 1)
        addTab(defaultPath_, false);
    disconnect(pane, nullptr, this, nullptr);
    tabs_->removeTab(index);
    pane->setParent(nullptr);
    emit tabsChanged();
    return pane;
}

void TabbedPanel::adoptTab(FilePane* pane, bool makeCurrent)
{
    if (!pane)
        return;
    pane->setParent(tabs_);
    attachPane(pane, makeCurrent);
}

bool TabbedPanel::transferTabFrom(TabbedPanel* source, int sourceIndex, int destinationIndex)
{
    // Operation queues, recovery and command connections belong to the containing window.
    if (!source || source->window() != window()
            || sourceIndex < 0 || sourceIndex >= source->count())
        return false;
    if (source == this) {
        tabs_->tabBar()->moveTab(sourceIndex, qBound(0, destinationIndex, tabs_->count() - 1));
        emit tabsChanged();
        return true;
    }
    FilePane* pane = source->takeTab(sourceIndex);
    if (!pane)
        return false;
    adoptTab(pane);
    const int current = tabs_->indexOf(pane);
    tabs_->tabBar()->moveTab(current, qBound(0, destinationIndex, tabs_->count() - 1));
    return true;
}

QByteArray TabbedPanel::tabDragPayload(int index) const
{
    if (index < 0 || index >= count())
        return {};
    return static_cast<TransferTabBar*>(tabs_->tabBar())->payload(
        tabs_->widget(index)->property("tabDragIdentity").toByteArray());
}

TabbedPanel* TabbedPanel::resolveTabDrag(const QByteArray& payload, const QWidget* target,
    int* sourceIndex)
{
    if (sourceIndex)
        *sourceIndex = -1;
    if (!target || payload.size() > 96)
        return nullptr;
    const auto parts = payload.split(':');
    if (parts.size() != 2)
        return nullptr;
    TabbedPanel* source = registeredPanels().value(parts.at(0));
    if (!source || source->window() != target->window() || parts.at(1).isEmpty())
        return nullptr;
    int index = -1;
    for (int candidate = 0; candidate < source->count(); ++candidate) {
        if (source->tabs_->widget(candidate)->property("tabDragIdentity").toByteArray()
                == parts.at(1)) {
            index = candidate;
            break;
        }
    }
    if (index < 0)
        return nullptr;
    if (sourceIndex)
        *sourceIndex = index;
    return source;
}

bool TabbedPanel::moveCurrentTab(int offset)
{
    const int source = tabs_->currentIndex();
    const int destination = qBound(0, source + offset, tabs_->count() - 1);
    if (source < 0 || destination == source)
        return false;
    tabs_->tabBar()->moveTab(source, destination);
    tabs_->setCurrentIndex(destination);
    emit tabsChanged();
    return true;
}

void TabbedPanel::closeCurrentTab()
{
    closeTab(tabs_->currentIndex());
}

FilePane* TabbedPanel::duplicateCurrentTab()
{
    FilePane* source = currentPane();
    if (!source)
        return nullptr;
    const QString path = source->currentPath();
    const QString serializedLocation = source->serializedLocation();
    const FilePane::ViewMode viewMode = source->viewMode();
    const bool showHidden = source->showHidden();
    const FilePane::FolderPlacement folderPlacement = source->folderPlacement();
    const bool showFileExtensions = source->showFileExtensions();
    const FilePane::GroupBy grouping = source->groupBy();
    const FilePaneRestoreState restoreState = source->restoreState();
    FilePane* copy = addTab(serializedLocation.isEmpty()
        ? (path.isEmpty() ? defaultPath_ : path) : serializedLocation);
    copy->setViewMode(viewMode);
    copy->setShowHidden(showHidden);
    copy->setFolderPlacement(folderPlacement);
    copy->setShowFileExtensions(showFileExtensions);
    copy->setGroupBy(grouping);
    copy->restoreViewState(restoreState);
    if (!source->isFileSystemLocation() && serializedLocation.isEmpty())
        copy->navigateShellItem(source->currentShellLocation());
    return copy;
}

FilePane* TabbedPanel::reopenLastClosedTab()
{
    if (closedTabs_.isEmpty())
        return nullptr;
    const ClosedTab state = closedTabs_.takeLast();
    const QString initialLocation = !state.serializedLocation.isEmpty()
        ? state.serializedLocation
        : QFileInfo(state.path).isDir() ? state.path : defaultPath_;
    FilePane* pane = addTab(initialLocation);
    pane->setViewMode(static_cast<FilePane::ViewMode>(qBound(0, state.viewMode, 5)));
    pane->setShowHidden(state.showHidden);
    pane->setFolderPlacement(static_cast<FilePane::FolderPlacement>(
        qBound(0, state.folderPlacement, 2)));
    pane->setShowFileExtensions(state.showFileExtensions);
    pane->setGroupBy(static_cast<FilePane::GroupBy>(qBound(0, state.grouping, 4)));
    pane->restoreViewState(state.restoreState);
    if (state.shellLocation.valid && state.serializedLocation.isEmpty())
        pane->navigateShellItem(state.shellLocation);
    return pane;
}

void TabbedPanel::closeTab(int index)
{
    if (index < 0)
        return;
    if (tabs_->count() == 1) {
        emit lastTabCloseRequested();
        return;
    }
    auto* pane = qobject_cast<FilePane*>(tabs_->widget(index));
    QWidget* page = pane;
    if (pane) {
        closedTabs_.append({pane->currentPath(), pane->serializedLocation(),
            pane->currentShellLocation(),
            pane->isHomeLocation(),
            static_cast<int>(pane->viewMode()),
            pane->showHidden(), static_cast<int>(pane->folderPlacement()),
            pane->showFileExtensions(), static_cast<int>(pane->groupBy()),
            pane->restoreState()});
        if (closedTabs_.size() > 20)
            closedTabs_.removeFirst();
    }
    tabs_->removeTab(index);
    page->deleteLater();
    emit tabsChanged();
}

void TabbedPanel::restoreTabs(const QStringList& paths, int currentIndex,
    const QList<int>& viewModes, const QList<bool>& hiddenStates,
    const QList<int>& folderPlacements, const QList<bool>& extensionStates,
    const QList<int>& groupings, const QList<FilePaneRestoreState>& restoreStates)
{
    while (tabs_->count() > 0) {
        QWidget* page = tabs_->widget(0);
        tabs_->removeTab(0);
        page->deleteLater();
    }
    const QStringList safePaths = paths.isEmpty() ? QStringList{defaultPath_} : paths;
    for (int index = 0; index < safePaths.size(); ++index) {
        const bool appLocation = safePaths[index] == QLatin1String(FilePane::HomeIdentity)
            || filemanager::platform::ShellItem::builtInLocationFromIdentity(safePaths[index])
                != filemanager::platform::BuiltInShellLocation::None;
        FilePane* pane = addTab(appLocation || QFileInfo(safePaths[index]).isDir()
                ? safePaths[index] : defaultPath_, false);
        pane->setViewMode(static_cast<FilePane::ViewMode>(qBound(0, viewModes.value(index, 0), 5)));
        pane->setShowHidden(hiddenStates.value(index, false));
        pane->setFolderPlacement(static_cast<FilePane::FolderPlacement>(
            qBound(0, folderPlacements.value(index, 0), 2)));
        pane->setShowFileExtensions(extensionStates.value(index, true));
        pane->setGroupBy(static_cast<FilePane::GroupBy>(qBound(0, groupings.value(index, 0), 4)));
        pane->restoreViewState(restoreStates.value(index));
    }
    tabs_->setCurrentIndex(qBound(0, currentIndex, tabs_->count() - 1));
    emit activePaneChanged(currentPane());
}

void TabbedPanel::updateTabTitle(FilePane* pane, const QString& path)
{
    const int index = tabs_->indexOf(pane);
    if (index < 0)
        return;
    QString title;
    if (pane && !pane->isFileSystemLocation()) {
        title = path.trimmed();
    } else {
        QFileInfo info(path);
        title = info.fileName();
        if (title.isEmpty())
            title = QDir::toNativeSeparators(info.absoluteFilePath());
    }
    if (title.isEmpty())
        title = QStringLiteral("Windows location");
    tabs_->setTabText(index, title);
    tabs_->setTabToolTip(index, pane && !pane->isFileSystemLocation()
        ? pane->currentLocation() : QDir::toNativeSeparators(path));
}
