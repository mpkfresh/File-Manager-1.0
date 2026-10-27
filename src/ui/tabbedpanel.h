#pragma once

#include "filepane.h"

#include <QWidget>
#include <QList>
#include <QString>

class QTabWidget;
class QTabBar;

class TabbedPanel final : public QWidget
{
    Q_OBJECT

public:
    static constexpr auto TabMimeType = "application/x-file-manager-tab";
    explicit TabbedPanel(QWidget* parent = nullptr, bool createDefaultTab = true);
    explicit TabbedPanel(const QString& initialPath, QWidget* parent = nullptr,
        bool createDefaultTab = true);

    FilePane* currentPane() const;
    QList<FilePane*> panes() const;
    int count() const;
    QStringList paths() const;
    QList<int> viewModes() const;
    QList<bool> hiddenStates() const;
    QList<int> folderPlacements() const;
    QList<bool> extensionStates() const;
    QList<int> groupings() const;
    QList<FilePaneRestoreState> restoreStates() const;

    FilePane* addTab(const QString& path, bool makeCurrent = true);
    FilePane* takeCurrentTab();
    FilePane* takeTab(int index);
    void adoptTab(FilePane* pane, bool makeCurrent = true);
    bool transferTabFrom(TabbedPanel* source, int sourceIndex, int destinationIndex);
    QByteArray tabDragPayload(int index) const;
    static TabbedPanel* resolveTabDrag(const QByteArray& payload, const QWidget* target,
        int* sourceIndex);
    bool moveCurrentTab(int offset);
    void closeCurrentTab();
    FilePane* duplicateCurrentTab();
    FilePane* reopenLastClosedTab();
    bool hasClosedTabs() const { return !closedTabs_.isEmpty(); }
    void restoreTabs(const QStringList& paths, int currentIndex,
        const QList<int>& viewModes = {}, const QList<bool>& hiddenStates = {},
        const QList<int>& folderPlacements = {}, const QList<bool>& extensionStates = {},
        const QList<int>& groupings = {}, const QList<FilePaneRestoreState>& restoreStates = {});
    int currentIndex() const;
    QTabBar* tabBar() const;

signals:
    void activePaneChanged(FilePane* pane);
    void paneCreated(FilePane* pane);
    void tabsChanged();
    void lastTabCloseRequested();

private:
    void attachPane(FilePane* pane, bool makeCurrent);
    void updateTabTitle(FilePane* pane, const QString& path);
    void closeTab(int index);

    QTabWidget* tabs_ = nullptr;
    struct ClosedTab {
        QString path;
        QString serializedLocation;
        filemanager::platform::ShellItemReference shellLocation;
        bool homeLocation = false;
        int viewMode = 0;
        bool showHidden = false;
        int folderPlacement = 0;
        bool showFileExtensions = true;
        int grouping = 0;
        FilePaneRestoreState restoreState;
    };
    QList<ClosedTab> closedTabs_;
    QString defaultPath_;
};
