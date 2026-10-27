#include "applicationtheme.h"
#include "appearancesettings.h"

#include <QApplication>
#include <QFont>
#include <QPalette>

#ifdef Q_OS_WIN
#include <windows.h>
#endif

namespace filemanager::ui {

namespace {

SystemAccessibilityPreferences appliedAccessibilityPreferences;

} // namespace

SystemAccessibilityPreferences systemAccessibilityPreferences()
{
    SystemAccessibilityPreferences result;
#ifdef Q_OS_WIN
    HIGHCONTRASTW highContrast{};
    highContrast.cbSize = sizeof(highContrast);
    if (SystemParametersInfoW(SPI_GETHIGHCONTRAST, sizeof(highContrast), &highContrast, 0))
        result.highContrast = (highContrast.dwFlags & HCF_HIGHCONTRASTON) != 0;

    BOOL clientAreaAnimations = TRUE;
    if (SystemParametersInfoW(
            SPI_GETCLIENTAREAANIMATION, 0, &clientAreaAnimations, 0)) {
        result.clientAreaAnimations = clientAreaAnimations != FALSE;
    }
#endif
    return result;
}

bool applicationAnimationsEnabled()
{
    return effectiveAnimationsEnabled(
        loadAppearanceSettings().animations, systemAccessibilityPreferences());
}

bool applicationHighContrastEnabled()
{
    return appliedAccessibilityPreferences.highContrast;
}

void applyApplicationTheme(QApplication& application)
{
    const AppearanceSettings settings = loadAppearanceSettings();
    const SystemAccessibilityPreferences accessibility = systemAccessibilityPreferences();
    appliedAccessibilityPreferences = accessibility;
    QFont font = application.font();
    const qreal basePointSize = effectiveFontPointSize(settings.fontPointSize, font.pointSizeF());
    font.setPointSizeF(basePointSize);
    application.setFont(font);
    const qreal titlePointSize = qMax<qreal>(15.0, basePointSize + 5.5);
    const qreal guidancePointSize = qMax<qreal>(10.0, basePointSize);
    const qreal sectionPointSize = qMax<qreal>(8.0, basePointSize);
    const int controlPadding = settings.density == "Spacious" ? 9
        : settings.density == "Comfortable" ? 6 : 5;
    const int itemPadding = settings.density == "Spacious" ? 7
        : settings.density == "Comfortable" ? 5 : 4;
    if (accessibility.highContrast) {
        // Let the native Windows style and palette carry the user's system colors. Even an
        // apparently color-neutral Qt stylesheet can suppress High Contrast rendering.
        application.setStyleSheet({});
        return;
    }

    application.setStyle("Fusion");
    QPalette palette;
    palette.setColor(QPalette::Window, QColor("#101116"));
    palette.setColor(QPalette::WindowText, QColor("#e8eaf0"));
    palette.setColor(QPalette::Base, QColor("#15171e"));
    palette.setColor(QPalette::AlternateBase, QColor("#1a1d26"));
    palette.setColor(QPalette::Text, QColor("#e8eaf0"));
    palette.setColor(QPalette::Button, QColor("#1a1d26"));
    palette.setColor(QPalette::ButtonText, QColor("#e8eaf0"));
    palette.setColor(QPalette::Highlight, QColor("#7c5cff"));
    palette.setColor(QPalette::HighlightedText, Qt::white);
    application.setPalette(palette);
    application.setStyleSheet(QString(R"(
        QWidget { font-family: "Segoe UI Variable", "Segoe UI"; font-size: %1pt; color:#d9dbe2; }
        QMainWindow { background:#0d0e11; }
        QMenuBar { background:#0d0e11; border-bottom:1px solid #25262b; padding:2px 6px; }
        QMenuBar::item { padding:5px 9px; border-radius:4px; }
        QMenuBar::item:selected { background:#26272d; }
        QToolBar { background:#111216; border:0; border-bottom:1px solid #25262b; padding:6px 10px; spacing:4px; }
        QToolBar::separator { background:#2a2c34; width:1px; margin:5px 7px; }
        QToolButton, QPushButton { background:transparent; border:1px solid transparent; border-radius:6px; padding:%2px 8px; }
        QToolButton:hover, QPushButton:hover { background:#27282e; border-color:#34353c; }
        QToolButton#BreadcrumbSegment { padding-right:26px; border-color:transparent; }
        QToolButton#BreadcrumbSegment::menu-button { width:24px; border:0; border-radius:4px; background:transparent; }
        QToolButton#BreadcrumbSegment::menu-button:hover { background:#30323a; }
        QToolButton#BreadcrumbSegment::menu-arrow { image:url(:/icons/breadcrumb-chevron.svg); width:12px; height:12px; }
        QToolButton#BreadcrumbOverflow::menu-indicator { image:none; width:0px; }
        QLineEdit { background:#17181c; border:1px solid #313238; border-radius:4px; padding:6px 8px; selection-background-color:#5b63d3; }
        QLineEdit:focus { border-color:#6c72d9; }
        QTreeView { background:#101115; border:0; alternate-background-color:#131419; outline:0; }
        QTreeView::item { padding:%3px 4px; border:0; }
        QTreeView::item:hover { background:#202126; }
        QTreeView::item:selected { background:#3b3f73; }
        QHeaderView::section { background:#15161a; color:#9497a1; border:0; border-right:1px solid #25262b; border-bottom:1px solid #292a30; padding:6px 8px; }
        QSplitter::handle { background:#292a30; width:1px; height:1px; }
        QLabel#Title { font-size:%4pt; font-weight:600; }
        QLabel#Muted { color:#898c96; }
        QLabel#EmptyFolderGuidance { color:#aeb2bf; background:rgba(23,24,28,220); border:1px solid #31343e; border-radius:12px; padding:28px; font-size:%5pt; }
        QLabel#Section { color:#777a84; font-size:%6pt; font-weight:600; padding:12px 8px 3px 8px; }
        QTreeWidget#QuickAccess { background:#111216; border:0; outline:0; padding:4px; }
        QTreeWidget#QuickAccess::item { border-radius:4px; padding:5px 5px; margin:1px 2px; }
        QTreeWidget#QuickAccess::item:hover { background:#25262b; }
        QTreeWidget#QuickAccess::item:selected { background:#30323a; color:#fff; }
        QTabWidget::pane { border:0; }
        QTabBar::tab { background:#15171e; color:#9da4b6; padding:9px 18px; border-radius:7px; margin:3px; }
        QTabBar::tab:selected { background:#29243d; color:#ffffff; }
        QStatusBar { background:#111216; color:#858892; border-top:1px solid #25262b; min-height:20px; }
        QMenu { background:#191b23; border:1px solid #303441; padding:5px; }
        QMenu::item { padding:7px 26px 7px 10px; border-radius:5px; }
        QMenu::item:selected { background:#6549d8; }
        QToolTip { background:#242630; color:#f1f2f6; border:1px solid #3a3d48; padding:5px 7px; }
        QScrollBar:vertical { background:#111216; width:10px; margin:0; }
        QScrollBar::handle:vertical { background:#373a44; border-radius:5px; min-height:28px; }
        QScrollBar::add-line:vertical, QScrollBar::sub-line:vertical { height:0; }
    )").arg(basePointSize).arg(controlPadding).arg(itemPadding)
        .arg(titlePointSize).arg(guidancePointSize).arg(sectionPointSize));
}

} // namespace filemanager::ui
