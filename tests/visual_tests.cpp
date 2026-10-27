#include "mainwindow.h"
#include "ui/applicationtheme.h"
#include "ui/brandidentity.h"
#include "ui/batchrenamedialog.h"
#include "ui/breadcrumbbar.h"
#include "platform/pathnavigation.h"
#include "ui/homeview.h"
#include "ui/preferencesdialog.h"
#include "ui/operationcenterdialog.h"
#include "files/transferqueue.h"
#include "files/operationjournal.h"
#include "tags/tagcatalog.h"
#include "files/sourcesnapshot.h"
#include "rename_test_helpers.h"
#include "ui/filepane.h"
#include "ui/filesearchdialog.h"
#include "ui/contentsearchdialog.h"
#include "ui/firstlaunchintegrationdialog.h"
#include "ui/fileitemdelegate.h"
#include "preview/thumbnailprovider.h"
#include "preview/previewcontroller.h"
#include "preview/previewidentity.h"
#include "ui/navigationtree.h"
#include "ui/tabbedpanel.h"
#include "search/savedsearchstore.h"
#include "search/sessionsearchhistory.h"
#include "settings/favoritegroups.h"
#include "settings/sessionstore.h"
#include "settings/shellpermissions.h"
#include "workspace/layoutmodel.h"
#include "workspace/workspaceview.h"

#include <QApplication>
#include <QAbstractItemModel>
#include <QAbstractItemView>
#include <QAction>
#include <QColor>
#include <QCheckBox>
#include <QCompleter>
#include <QComboBox>
#include <QDir>
#include <QDialog>
#include <QDialogButtonBox>
#include <QDragEnterEvent>
#include <QDragMoveEvent>
#include <QDropEvent>
#include <QEventLoop>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QHeaderView>
#include <QImage>
#include <QItemSelectionModel>
#include <QJsonDocument>
#include <QKeyEvent>
#include <QKeySequenceEdit>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QMimeData>
#include <QMouseEvent>
#include <QListWidget>
#include <QMenu>
#include <QPushButton>
#include <QPainter>
#include <QProxyStyle>
#include <QStandardItemModel>
#include <QStandardPaths>
#include <QFileSystemModel>
#include <QPlainTextEdit>
#include <QPersistentModelIndex>
#include <QSpinBox>
#include <QSettings>
#include <QStatusBar>
#include <QTabBar>
#include <QTabWidget>
#include <QTableWidget>
#include <QTemporaryDir>
#include <QThread>
#include <QTimer>
#include <QToolBar>
#include <QToolButton>
#include <QWidget>
#include <QTreeWidget>
#include <QUrl>

#include <cmath>
#include <iostream>

#ifdef Q_OS_WIN
#include <qt_windows.h>
#endif

namespace {

bool expect(bool condition, const char* message)
{
    if (!condition)
        std::cerr << "FAIL: " << message << '\n';
    return condition;
}

bool waitForFocus(QWidget* widget, int timeoutMs = 1500)
{
    if (!widget)
        return false;
    const auto focused = [widget] {
        QWidget* window = widget->window();
        return widget->hasFocus() || (window && window->focusWidget() == widget);
    };
    QElapsedTimer timer;
    timer.start();
    while (!focused() && timer.elapsed() < timeoutMs) {
        QApplication::processEvents(QEventLoop::AllEvents, 25);
        QThread::msleep(1);
    }
    return focused();
}

double luminance(const QColor& color)
{
    const auto channel = [](double value) {
        value /= 255.0;
        return value <= 0.04045 ? value / 12.92 : std::pow((value + 0.055) / 1.055, 2.4);
    };
    return 0.2126 * channel(color.red()) + 0.7152 * channel(color.green())
        + 0.0722 * channel(color.blue());
}

bool verifyDelegateThumbnail()
{
    QTemporaryDir fixture;
    QImage source(48, 48, QImage::Format_RGB32);
    source.fill(Qt::magenta);
    const QString path = fixture.filePath("thumbnail.png");
    if (!expect(fixture.isValid() && source.save(path), "Delegate thumbnail fixture should save."))
        return false;
#ifdef Q_OS_WIN
    // Reproduce an upgrade from read-only optical media. The previous helper
    // differs from this build and must be replaced before the thumbnail loads.
    const QString runtime = QDir(QStandardPaths::writableLocation(
        QStandardPaths::AppLocalDataLocation)).filePath(
            QString::fromLatin1(filemanager::preview::PreviewRuntimeDirectoryName));
    const QString staged = QDir(runtime).filePath(QStringLiteral("FileManagerPreviewHost.exe"));
    const QString nativeStaged = QDir::toNativeSeparators(staged);
    bool seeded = QDir().mkpath(runtime);
    if (seeded && QFileInfo::exists(staged)) {
        const DWORD attributes = GetFileAttributesW(
            reinterpret_cast<LPCWSTR>(nativeStaged.utf16()));
        seeded = attributes != INVALID_FILE_ATTRIBUTES
            && !(attributes & FILE_ATTRIBUTE_REPARSE_POINT)
            && SetFileAttributesW(reinterpret_cast<LPCWSTR>(nativeStaged.utf16()),
                attributes & ~FILE_ATTRIBUTE_READONLY)
            && QFile::remove(staged);
    }
    if (seeded) {
        QFile oldHelper(staged);
        seeded = oldHelper.open(QIODevice::WriteOnly | QIODevice::NewOnly)
            && oldHelper.write("obsolete preview host") == 21;
        oldHelper.close();
        if (seeded) {
            const DWORD attributes = GetFileAttributesW(
                reinterpret_cast<LPCWSTR>(nativeStaged.utf16()));
            seeded = attributes != INVALID_FILE_ATTRIBUTES
                && SetFileAttributesW(reinterpret_cast<LPCWSTR>(nativeStaged.utf16()),
                    attributes | FILE_ATTRIBUTE_READONLY);
        }
    }
    if (!expect(seeded, "A stale read-only private preview helper should be staged safely."))
        return false;
#endif
    class CaptureStyle final : public QProxyStyle {
    public:
        mutable QStyleOptionViewItem drawn;
        mutable int itemDraws = 0;
        void drawControl(ControlElement element, const QStyleOption* option, QPainter* painter,
            const QWidget* widget = nullptr) const override
        {
            if (element == CE_ItemViewItem) {
                if (const auto* item = qstyleoption_cast<const QStyleOptionViewItem*>(option)) {
                    drawn = *item;
                    ++itemDraws;
                }
                return;
            }
            QProxyStyle::drawControl(element, option, painter, widget);
        }
    } style;
    QWidget widget;
    widget.setStyle(&style);
    filemanager::ui::FileItemDelegate delegate;
    QStandardItemModel model(1, 1);
    const QModelIndex item = model.index(0, 0);
    model.setData(item, QStringLiteral("Thumbnail item"), Qt::DisplayRole);
    model.setData(item, path, QFileSystemModel::FilePathRole);
    model.setData(item, Qt::Checked, Qt::CheckStateRole);
    QPixmap fallback(24, 24);
    fallback.fill(Qt::blue);
    model.setData(item, QIcon(fallback), Qt::DecorationRole);
    QStyleOptionViewItem option;
    option.initFrom(&widget);
    option.widget = &widget;
    option.rect = QRect(0, 0, 240, 60);
    option.decorationSize = QSize(24, 24);
    QImage canvas(240, 60, QImage::Format_ARGB32_Premultiplied);
    canvas.fill(Qt::transparent);
    QPainter painter(&canvas);
    bool ready = false;
    QString thumbnailError;
    QEventLoop loop;
    auto* thumbnailProvider = delegate.findChild<filemanager::preview::ThumbnailProvider*>();
    if (!expect(thumbnailProvider != nullptr, "Delegate thumbnail provider should exist."))
        return false;
    QObject::connect(thumbnailProvider, &filemanager::preview::ThumbnailProvider::thumbnailFailed,
        &loop, [&](const QString& failedPath, const QString& error) {
            if (failedPath == path)
                thumbnailError = error;
        });
    QObject::connect(&delegate, &filemanager::ui::FileItemDelegate::thumbnailReady, &loop, [&] {
        ready = true;
        loop.quit();
    });
    delegate.paint(&painter, option, item);
    // The provider permits one retry after a timed-out isolated helper. Allow
    // both bounded helper attempts plus queued-signal time in a cold VM.
    QTimer::singleShot(2 * filemanager::preview::PreviewController::HelperTimeoutMilliseconds
        + 5000, &loop, &QEventLoop::quit);
    if (!ready)
        loop.exec();
    bool passed = expect(ready, "Delegate thumbnail should finish within its deadline.");
    for (const bool nativeIconPresent : {true, false}) {
        if (!nativeIconPresent)
            model.setData(item, QIcon{}, Qt::DecorationRole);
        const int priorDraws = style.itemDraws;
        delegate.paint(&painter, option, item);
        const QImage icon = style.drawn.icon.pixmap(24, 24).toImage();
        const bool captured = style.itemDraws > priorDraws;
        const QColor center = icon.isNull() ? QColor{} : icon.pixelColor(icon.width() / 2, icon.height() / 2);
        const bool decorated = style.drawn.features.testFlag(QStyleOptionViewItem::HasDecoration);
        const bool checked = style.drawn.checkState == Qt::Checked;
        const bool named = style.drawn.text == QStringLiteral("Thumbnail item");
        const bool matches = captured && !icon.isNull() && center == QColor(Qt::magenta)
            && decorated && checked && named;
        if (!matches) {
            std::fprintf(stderr, "Delegate thumbnail state: native_icon=%d ready_signal=%d capture_count=%d "
                "fresh_capture=%d icon=%dx%d center=%s decorated=%d checked=%d named=%d widget_style=%s error=%s\n",
                nativeIconPresent, ready, style.itemDraws, captured, icon.width(), icon.height(),
                qPrintable(center.name(QColor::HexArgb)), decorated, checked, named,
                widget.style()->metaObject()->className(), qPrintable(thumbnailError));
        }
        passed &= expect(matches,
            "The native style must receive the ready thumbnail and item state, with or without a model icon.");
    }
    return passed;
}

bool verifyLexicalNavigationParents()
{
    using filemanager::platform::fileSystemParentPath;
    bool passed = true;
#ifdef Q_OS_WIN
    struct ParentCase { QString path; QString parent; };
    const QList<ParentCase> cases{
        {QStringLiteral("C:/"), {}},
        {QStringLiteral("C:/one"), QStringLiteral("C:/")},
        {QStringLiteral("C:/one/two/"), QStringLiteral("C:/one")},
        {QStringLiteral("//server.invalid/share"), {}},
        {QStringLiteral("//server.invalid/share/"), {}},
        {QStringLiteral("//server.invalid/share/child"), QStringLiteral("//server.invalid/share")},
        {QStringLiteral("//?/C:/"), {}},
        {QStringLiteral("//?/C:/one/two"), QStringLiteral("//?/C:/one")},
        {QStringLiteral("//?/UNC/server.invalid/share"), {}},
        {QStringLiteral("//?/UNC/server.invalid/share/child"),
            QStringLiteral("//?/UNC/server.invalid/share")},
        {QStringLiteral("C:/") + QString(300, QChar('x')) + QStringLiteral("/child"),
            QStringLiteral("C:/") + QString(300, QChar('x'))},
        {QStringLiteral("C:relative"), {}},
        {QStringLiteral("/current-drive-relative"), {}},
        {QStringLiteral("relative/child"), {}},
        {QStringLiteral("C:/bad") + QChar::Null + QStringLiteral("/child"), {}},
        {QStringLiteral("C:/") + QString(32768, QChar('x')), {}},
        {{}, {}}
    };
    for (const auto& test : cases) {
        // Compare native strings directly: Qt's fromNativeSeparators removes extended prefixes.
        const QString actual = fileSystemParentPath(test.path);
        const QString expected = QDir::toNativeSeparators(test.parent);
        const bool matches = actual == expected;
        passed &= expect(matches,
            "Parent navigation must preserve native drive/UNC/extended roots and reject invalid inputs.");
        if (!matches)
            std::cerr << "  Input: " << test.path.left(160).toStdString()
                      << " expected: " << expected.left(160).toStdString()
                      << " actual: " << actual.left(160).toStdString() << '\n';
        const QString nativeInput = QDir::toNativeSeparators(test.path);
        passed &= expect(fileSystemParentPath(nativeInput) == actual,
            "Native and forward separators must retain the same parent identity.");
    }
    // These are string-only controls: never open a dropdown or navigate to the synthetic UNC path.
    filemanager::ui::BreadcrumbBar breadcrumbs;
    breadcrumbs.setPath(QStringLiteral("//server.invalid/share/one/two"));
    const auto segments = breadcrumbs.findChildren<QToolButton*>("BreadcrumbSegment");
    passed &= expect(segments.size() == 3,
        "Breadcrumb ancestors must be available without checking whether a UNC location exists.");
    const QStringList expectedPaths{
        QStringLiteral("//server.invalid/share"), QStringLiteral("//server.invalid/share/one"),
        QStringLiteral("//server.invalid/share/one/two")};
    if (segments.size() == expectedPaths.size()) {
        for (qsizetype index = 0; index < segments.size(); ++index)
            passed &= expect(QDir::fromNativeSeparators(segments.at(index)->toolTip()) == expectedPaths.at(index)
                    && segments.at(index)->menu()->property("loadState").toInt() == 0,
                "UNC breadcrumb construction must stop at the share root and keep menus unopened.");
    }
#endif
    return passed;
}

bool verifyBoundedBreadcrumbMenu(const QString& fixture)
{
    filemanager::ui::BreadcrumbBar breadcrumbs;
    breadcrumbs.setPath(fixture);
    const auto currentMenu = [&]() -> QMenu* {
        const auto segments = breadcrumbs.findChildren<QToolButton*>("BreadcrumbSegment");
        for (auto* segment : segments) {
            if (segment->isEnabled()
                    && QDir::fromNativeSeparators(segment->toolTip())
                        == QDir::fromNativeSeparators(breadcrumbs.path()))
                return segment->menu();
        }
        return nullptr;
    };
    const auto waitForMenu = [](QMenu* menu) {
        QElapsedTimer wait;
        wait.start();
        while (menu->property("loadState").toInt() != 2 && wait.elapsed() < 12000) {
            QApplication::processEvents(QEventLoop::AllEvents, 25);
            QThread::msleep(1);
        }
        return menu->property("loadState").toInt() == 2;
    };
    QMenu* menu = currentMenu();
    if (!menu)
        return expect(false, "The bounded breadcrumb fixture should have a folder menu.");
    bool passed = expect(menu->property("loadState").toInt() == 0,
        "Breadcrumb menus must remain lazy until requested.");
    // Deliver lifecycle signals without a native popup loop so cancellation precedes result delivery.
    QMetaObject::invokeMethod(menu, "aboutToShow", Qt::DirectConnection);
    QMetaObject::invokeMethod(menu, "aboutToHide", Qt::DirectConnection);
    passed &= expect(menu->property("loadState").toInt() == 0,
        "Closing a pending breadcrumb menu must allow a fresh request on reopening.");
    QMetaObject::invokeMethod(menu, "aboutToShow", Qt::DirectConnection);
    passed &= expect(waitForMenu(menu), "A reopened breadcrumb menu should complete its new request.");
    const auto actions = menu->actions();
    passed &= expect(actions.size() == 257,
        "Breadcrumb menus must cap folder rows at 256 and disclose omitted folders.");
    if (actions.size() == 257) {
        for (int index = 0; index < 256; ++index) {
            const QString expected = QStringLiteral("folder-%1").arg(index, 4, 10, QChar('0'));
            passed &= expect(actions.at(index)->isEnabled()
                    && actions.at(index)->text() == expected
                    && QDir::fromNativeSeparators(actions.at(index)->data().toString())
                        == QDir::fromNativeSeparators(QDir(fixture).filePath(expected)),
                "Breadcrumb truncation must keep the first alphabetical immediate folders and exact paths.");
        }
        passed &= expect(!actions.last()->isEnabled() && actions.last()->data().isNull(),
            "The breadcrumb truncation notice must not be a navigation action.");
        QString activated;
        const auto activationConnection = QObject::connect(&breadcrumbs, &filemanager::ui::BreadcrumbBar::pathActivated,
            &breadcrumbs, [&](const QString& path) { activated = path; });
        actions.first()->trigger();
        passed &= expect(QDir::fromNativeSeparators(activated)
                == QDir::fromNativeSeparators(QDir(fixture).filePath("folder-0000")),
            "A bounded breadcrumb action must navigate to its exact folder.");
        QObject::disconnect(activationConnection);
    }

    // Replace a pending menu before queued completion; the new address must remain lazy and independent.
    breadcrumbs.setPath(fixture);
    menu = currentMenu();
    if (!menu)
        return false;
    QMetaObject::invokeMethod(menu, "aboutToShow", Qt::DirectConnection);
    breadcrumbs.setPath(QDir(fixture).filePath("folder-0000/unopened/deep"));
    menu = currentMenu();
    if (!menu)
        return false;
    QApplication::processEvents();
    passed &= expect(menu->property("loadState").toInt() == 0,
        "Replacing the address must not publish an obsolete listing into the new menu.");
    QMetaObject::invokeMethod(menu, "aboutToShow", Qt::DirectConnection);
    passed &= expect(waitForMenu(menu) && menu->actions().size() == 1
            && !menu->actions().first()->isEnabled()
            && menu->actions().first()->data().isNull(),
        "The replacement breadcrumb menu should show its own empty-folder state.");
    {
        filemanager::ui::BreadcrumbBar closing;
        closing.setPath(fixture);
        for (auto* segment : closing.findChildren<QToolButton*>("BreadcrumbSegment")) {
            if (QDir::fromNativeSeparators(segment->toolTip()) == QDir::fromNativeSeparators(fixture))
                QMetaObject::invokeMethod(segment->menu(), "aboutToShow", Qt::DirectConnection);
        }
    }
    QApplication::processEvents();
    return passed;
}

bool verifyBoundedFolderTree()
{
    using filemanager::ui::NavigationTree;
    QTemporaryDir fixture;
    if (!fixture.isValid())
        return expect(false, "The bounded-tree fixture should be available.");
    for (int index = 1055; index >= 0; --index) {
        if (!QDir().mkpath(fixture.filePath(QStringLiteral("folder-%1").arg(index, 4, 10, QChar('0')))))
            return expect(false, "The bounded-tree folder fixture should be created.");
    }
    QDir().mkpath(fixture.filePath("folder-0000/unopened/deep"));
    filemanager::settings::FavoriteGroups favorites;
    favorites.groups.append({QStringLiteral("bounded"), QStringLiteral("Bounded"), {fixture.path()}});
    NavigationTree navigation;
    navigation.rebuild({}, favorites, {});
    const auto fixtureNode = [&]() -> QTreeWidgetItem* {
        for (auto* item : navigation.findItems("*", Qt::MatchWildcard | Qt::MatchRecursive))
            if (item->data(0, NavigationTree::KindRole).toString() == "favorite")
                return item;
        return nullptr;
    };
    auto* initial = fixtureNode();
    if (!initial)
        return expect(false, "The bounded-tree favorite should exist.");
    initial->setExpanded(true);
    // Rebuild while the original request is pending, then load the replacement tree normally.
    navigation.rebuild({}, favorites, {});
    auto* root = fixtureNode();
    if (!root)
        return false;
    root->setExpanded(true);
    QElapsedTimer wait;
    wait.start();
    while (wait.elapsed() < 12000) {
        QApplication::processEvents(QEventLoop::AllEvents, 25);
        if (root->childCount() > 0
                && root->child(0)->data(0, NavigationTree::KindRole).toString() != "loading")
            break;
        QThread::msleep(1);
    }
    bool passed = expect(root->childCount() == 1025,
        "The tree must cap folder rows at 1,024 and add one explicit truncation row.");
    if (root->childCount() == 1025) {
        for (int index = 0; index < 1024; ++index) {
            auto* child = root->child(index);
            passed &= expect(child->text(0) == QStringLiteral("folder-%1").arg(index, 4, 10, QChar('0'))
                    && child->childCount() == 0,
                "The bounded tree must retain alphabetical order without preloading descendants.");
        }
        passed &= expect(root->child(1024)->data(0, NavigationTree::KindRole).toString() == "limit",
            "Truncated folder listings must identify the omitted remainder.");
    }
    passed &= verifyBoundedBreadcrumbMenu(fixture.path());
    return passed;
}

bool verifyShellVisitTree(const QString& fixture)
{
#ifdef Q_OS_WIN
    using filemanager::ui::NavigationTree;
    filemanager::settings::saveShellPermissions({false});
    MainWindow window{QLatin1String(FilePane::HomeIdentity)};
    window.show();
    auto* tree = window.findChild<NavigationTree*>("QuickAccess");
    auto* workspace = window.findChild<filemanager::workspace::WorkspaceView*>();
    if (!tree || !workspace || !workspace->currentPane())
        return expect(false, "The Shell tree fixture needs a connected workspace and navigation tree.");
    auto* pane = workspace->currentPane();
    auto root = filemanager::platform::ShellItem::resolve(fixture);
    root.fileSystem = false; // Exercise the virtual presentation using only a temporary local fixture.
    int requests = 0;
    QObject::connect(pane, &FilePane::shellProviderRequestStarted, pane, [&] { ++requests; });
    const auto nodes = [&] {
        QList<QTreeWidgetItem*> result;
        for (auto* item : tree->findItems("*", Qt::MatchWildcard | Qt::MatchRecursive))
            if (item->data(0, NavigationTree::KindRole).toString() == "shell-visit")
                result.append(item);
        return result;
    };
    pane->navigateShellItem(root);
    bool passed = expect(nodes().isEmpty() && requests == 0,
        "Blocked virtual destinations must not populate provider tree nodes.");
    const auto allowAndWait = [&] {
        auto* allow = pane->findChild<QPushButton*>("AllowShellVisit");
        if (!allow)
            return false;
        allow->click();
        QElapsedTimer wait;
        wait.start();
        while (nodes().size() != 3 && wait.elapsed() < 12000)
            QApplication::processEvents(QEventLoop::AllEvents, 25);
        return nodes().size() == 3;
    };
    passed &= expect(allowAndWait(), "A permitted, completed visit must publish its folder tree.");
    auto loaded = nodes();
    if (loaded.isEmpty())
        return false;
    auto* rootNode = loaded.first();
    passed &= expect(loaded.size() == 3 && rootNode->childCount() == 2 && requests == 1,
        "The provider tree must contain the visited root and its two folders, excluding files.");
    auto oversizedListing = pane->shellNavigationItems();
    if (oversizedListing.size() != 3)
        std::fprintf(stderr, "Completed Shell visit snapshot has %lld entries; expected root plus two folders.\n",
            static_cast<long long>(oversizedListing.size()));
    passed &= expect(oversizedListing.size() == 3,
        "A completed Shell visit must expose its root and both folder children to the navigation tree.");
    if (!oversizedListing.isEmpty()) {
        while (oversizedListing.size() <= 1025)
            oversizedListing.append(oversizedListing.last());
        NavigationTree boundedTree;
        boundedTree.setShellNavigationItems(oversizedListing);
        QTreeWidgetItem* section = nullptr;
        for (int index = 0; index < boundedTree.topLevelItemCount(); ++index) {
            auto* item = boundedTree.topLevelItem(index);
            if (item->data(0, NavigationTree::KindRole).toString() == QStringLiteral("shell-visit-section")) {
                section = item;
                break;
            }
        }
        auto* boundedRoot = section && section->childCount() ? section->child(0) : nullptr;
        auto* boundedChild = boundedRoot && boundedRoot->childCount() ? boundedRoot->child(0) : nullptr;
        if (!boundedChild)
            std::fprintf(stderr, "Bounded Shell tree: top-level=%d section=%d root=%d root-children=%d snapshot=%lld.\n",
                boundedTree.topLevelItemCount(), section != nullptr, boundedRoot != nullptr,
                boundedRoot ? boundedRoot->childCount() : -1,
                static_cast<long long>(oversizedListing.size()));
        passed &= expect(boundedChild, "A bounded visit must retain its first visible child.");
        if (boundedChild) {
            boundedTree.setCurrentItem(boundedChild);
            const QPersistentModelIndex retained(boundedTree.indexFromItem(boundedChild));
            boundedTree.setShellNavigationItems(oversizedListing);
            passed &= expect(retained.isValid() && boundedTree.itemFromIndex(retained) == boundedChild,
                "An unchanged oversized visit must not rebuild the focused tree row.");
        }
    }
    for (auto* node : loaded)
        passed &= expect(node->data(0, NavigationTree::PathRole).toString().isEmpty()
                && !node->flags().testFlag(Qt::ItemIsDropEnabled)
                && !node->flags().testFlag(Qt::ItemIsDragEnabled)
                && !node->text(0).isEmpty(),
            "Provider nodes must be named and kept outside filesystem drag/drop routing.");
    rootNode->setExpanded(false);
    rootNode->setExpanded(true);
    QApplication::processEvents();
    passed &= expect(requests == 1 && rootNode->childCount() == 2,
        "Tree disclosure must reuse the completed visit without provider discovery.");
    if (rootNode->childCount() > 0) {
        auto* selected = rootNode->child(0);
        tree->setCurrentItem(selected);
        pane->itemCountChanged();
        passed &= expect(tree->currentItem() == selected,
            "An unchanged visit listing must preserve tree keyboard selection.");
        const QByteArray selectedIdentity = selected->data(0, NavigationTree::ShellIdentityRole).toByteArray();
        auto renamedListing = pane->shellNavigationItems();
        if (!renamedListing.isEmpty()) {
            renamedListing.last().displayName += QStringLiteral(" updated");
            tree->setShellNavigationItems(renamedListing);
            passed &= expect(tree->currentItem()
                    && tree->currentItem()->data(0, NavigationTree::ShellIdentityRole).toByteArray()
                        == selectedIdentity,
                "A changed visit listing must retain selection by Shell identity.");
            tree->setShellNavigationItems(pane->shellNavigationItems());
            loaded = nodes();
            if (loaded.isEmpty())
                return expect(false, "Restoring the visit must keep its root available.");
            rootNode = loaded.first();
        }
    }
    const QPoint point = tree->visualItemRect(rootNode).center();
    QMimeData mime;
    mime.setUrls({QUrl::fromLocalFile(QDir(fixture).filePath("release-notes.txt"))});
    QDragEnterEvent enter(point, Qt::CopyAction, &mime, Qt::LeftButton, Qt::NoModifier);
    QApplication::sendEvent(tree->viewport(), &enter);
    QDragMoveEvent move(point, Qt::CopyAction, &mime, Qt::LeftButton, Qt::NoModifier);
    QApplication::sendEvent(tree->viewport(), &move);
    passed &= expect(!move.isAccepted(), "Virtual tree rows must reject filesystem drops.");

    auto* panel = workspace->activePanel();
    const int originalIndex = panel->currentIndex();
    const int originalCount = panel->count();
    QMouseEvent middle(QEvent::MouseButtonRelease, point, tree->viewport()->mapToGlobal(point),
        Qt::MiddleButton, Qt::MiddleButton, Qt::NoModifier);
    QApplication::sendEvent(tree->viewport(), &middle);
    auto* background = panel->panes().last();
    passed &= expect(panel->count() == originalCount + 1 && workspace->currentPane() == pane
            && background != pane && !background->hasTemporaryShellVisit()
            && background->shellNavigationItems().isEmpty() && requests == 1,
        "Middle-click must open a background destination without transferring visit consent.");
    panel->tabBar()->setCurrentIndex(panel->count() - 1);
    passed &= expect(nodes().isEmpty(), "Switching to an ungranted tab must clear provider nodes.");
    panel->tabBar()->setCurrentIndex(originalIndex);
    passed &= expect(nodes().size() == 3 && requests == 1,
        "Returning to the still-open visit must reuse its existing listing.");
    const QByteArray staleIdentity = root.idList;
    if (nodes().size() > 1)
        tree->setCurrentItem(nodes().at(1));
    filemanager::settings::saveShellPermissions({false});
    passed &= expect(nodes().isEmpty() && !pane->navigateShellVisitItem(staleIdentity),
        "Revocation must remove the tree and reject retained node identities synchronously.");
    passed &= expect(tree->currentItem()
            && tree->currentItem()->data(0, NavigationTree::KindRole).toString() == "home",
        "Revoking a selected visit must leave a usable Home keyboard selection.");
    passed &= expect(allowAndWait(), "Fresh explicit consent must restore the current visit tree.");
    loaded = nodes();
    if (loaded.size() > 1) {
        auto* child = loaded.at(1);
        tree->setCurrentItem(child);
        tree->setFocus();
        QKeyEvent enterFolder(QEvent::KeyPress, Qt::Key_Return, Qt::NoModifier);
        QApplication::sendEvent(tree, &enterFolder);
        passed &= expect(pane->isFileSystemLocation() && !pane->hasTemporaryShellVisit()
                && nodes().isEmpty() && requests == 2,
            "Enter on a loaded filesystem child must navigate normally and end the provider visit.");
    }
    return passed;
#else
    Q_UNUSED(fixture);
    return true;
#endif
}

bool verifySystemTextSizeInTheme(QApplication& application)
{
    const QFont originalFont = application.font();
    QFont enlarged = originalFont;
    enlarged.setPointSizeF(20.0);
    application.setFont(enlarged);
    std::cerr << "Visual phase: enlarged font set" << std::endl;
    filemanager::ui::applyApplicationTheme(application);

    std::cerr << "Visual phase: enlarged theme applied" << std::endl;
    QWidget ordinary;
    QLabel title;
    title.setObjectName("Title");
    QLabel guidance;
    guidance.setObjectName("EmptyFolderGuidance");
    QLabel section;
    section.setObjectName("Section");
    QWidget* widgets[] = {&ordinary, &title, &guidance, &section};
    for (QWidget* widget : widgets)
        widget->ensurePolished();
    std::cerr << "Visual phase: widgets polished" << std::endl;
    const bool passed = expect(application.font().pointSizeF() >= 20.0
            && ordinary.font().pointSizeF() >= 20.0
            && title.font().pointSizeF() >= 20.0
            && guidance.font().pointSizeF() >= 20.0
            && section.font().pointSizeF() >= 20.0,
        "The custom theme must not reduce the larger Windows text size on ordinary or named widgets.");

    application.setFont(originalFont);
    filemanager::ui::applyApplicationTheme(application);
    return passed;
}
} // namespace

bool runTvOrganizerUiTests(const QString& captureDirectory);
bool runTagEditorUiTests();

bool verifyFileLabelClearControl()
{
    using namespace filemanager;
    QTemporaryDir fixture;
    if (!expect(fixture.isValid(), "Create disposable label-reset fixture."))
        return false;
    const QString sourcePath = fixture.filePath("book.epub");
    const QString catalogPath = fixture.filePath("tags.dat");
    QFile source(sourcePath);
    if (!expect(source.open(QIODevice::WriteOnly) && source.write("chapter") == 7,
            "Create a disposable labeled file."))
        return false;
    source.close();
    const auto identity = files::captureSourceSnapshot(sourcePath);
    tags::TagCatalog catalog(catalogPath);
    QString error;
    const QList<tags::DestinationRule> oldRules{
        {"old-rule", {"Reading"}, fixture.filePath("old-destination")}};
    if (!expect(identity.valid && catalog.load(&error)
            && catalog.assign({{sourcePath, identity, {"Reading"}}}, &error)
            && catalog.replaceRules(oldRules, &error),
            "Seed an encrypted label catalog with dormant legacy data."))
        return false;

    ui::PreferencesDialog preferences({.fileLabelStoragePath = catalogPath});
    preferences.show();
    QApplication::processEvents();
    auto* clear = preferences.findChild<QPushButton*>("ClearAllFileLabels");
    if (!expect(clear && clear->isEnabled()
            && clear->text().contains("Clear all file labels"),
            "Preferences should offer a label reset for an occupied catalog.")) {
        preferences.reject();
        return false;
    }
    bool confirmationShown = false;
    QTimer::singleShot(0, [&] {
        auto* box = qobject_cast<QMessageBox*>(QApplication::activeModalWidget());
        confirmationShown = box && box->windowTitle() == "Clear file labels"
            && box->text().contains("legacy destination rules")
            && box->text().contains("Files and folders will not change");
        if (box && box->button(QMessageBox::Yes))
            box->button(QMessageBox::Yes)->click();
    });
    clear->click();
    tags::TagCatalog reopened(catalogPath);
    const bool catalogRemoved = !QFileInfo::exists(catalogPath);
    const bool reopenedReady = reopened.load(&error);
    QFile original(sourcePath);
    const bool filePreserved = original.open(QIODevice::ReadOnly)
        && original.readAll() == QByteArray("chapter");
    const bool passed = expect(confirmationShown,
            "Label reset should show a specific, fully disclosed confirmation.")
        && expect(!clear->isEnabled() && catalogRemoved,
            "Confirmed label reset should remove the encrypted catalog.")
        && expect(reopenedReady && reopened.entries().isEmpty()
                && reopened.rules().isEmpty(),
            "Reopened label catalog should contain neither labels nor legacy rules.")
        && expect(filePreserved,
            "Clearing labels should not change the selected file.");
    preferences.reject();
    return passed;
}

bool verifyFirstLaunchOffer(const QString& capturePath = {})
{
    const filemanager::platform::FolderRoutingStatus folders{
        filemanager::platform::FolderRoutingState::Off, false, true,
        "Windows folder routing is unchanged."};
    const filemanager::platform::WinEStatus winE{false, false, true, {}};
    filemanager::ui::FirstLaunchIntegrationDialog offer(folders, winE);
    auto* useFolders = offer.findChild<QCheckBox*>("FirstLaunchFolders");
    auto* useWinE = offer.findChild<QCheckBox*>("FirstLaunchWinE");
    auto* startAtSignIn = offer.findChild<QCheckBox*>("FirstLaunchWinEAtSignIn");
    auto* applyChoices = offer.findChild<QPushButton*>("FirstLaunchApply");
    auto* keepWindows = offer.findChild<QPushButton*>("FirstLaunchNotNow");
    auto* winEHelp = offer.findChild<QToolButton*>("WinEHelpToggle");
    auto* winEHelpText = offer.findChild<QLabel*>("WinEHelpText");
    auto* foldersSummary = offer.findChild<QLabel*>("FoldersSummary");
    auto* winESummary = offer.findChild<QLabel*>("WinESummary");
    bool passed = expect(useFolders && useWinE && startAtSignIn && applyChoices
        && keepWindows,
        "First-launch folder and Win+E choices should be separate and discoverable.");
    passed &= expect(winEHelp && winEHelpText && winEHelpText->isHidden(),
        "Win+E details should be available without crowding the initial offer.");
    passed &= expect(foldersSummary && winESummary
        && foldersSummary->text().contains("only for this account", Qt::CaseInsensitive)
        && winESummary->text().contains("never saves or sends typing"),
        "Both choices should show their immediate scope and privacy consequence.");
    offer.show();
    QApplication::processEvents();
    const int collapsedWidth = offer.width();
    if (keepWindows && applyChoices)
        passed &= expect(keepWindows->x() < applyChoices->x(),
            "Keep Windows settings should precede the explicit apply action.");
    if (!capturePath.isEmpty())
        passed &= expect(offer.grab().save(capturePath), "Capture the compact first-launch offer.");
    if (winEHelp && winEHelpText) {
        winEHelp->setChecked(true);
        QApplication::processEvents();
        passed &= expect(offer.width() == collapsedWidth,
            "Opening contextual help should preserve a readable dialog width.");
        passed &= expect(!winEHelpText->isHidden()
            && winEHelpText->text().contains("never records or transmits typing"),
            "Expanded help should disclose the keyboard helper's privacy behavior.");
        passed &= expect(offer.rect().contains(
            winEHelpText->mapTo(&offer, winEHelpText->rect().bottomRight())),
            "Expanded help should remain inside the dialog.");
        if (!capturePath.isEmpty())
            passed &= expect(offer.grab().save(
                QDir(FILE_MANAGER_SOURCE_DIR).filePath("out/first-launch-ui-review-details.png")),
                "Capture the expanded contextual help.");
    }
    if (useFolders && useWinE && startAtSignIn && applyChoices) {
        passed &= expect(!useFolders->isChecked() && !useWinE->isChecked()
            && !startAtSignIn->isEnabled() && startAtSignIn->isHidden()
            && !applyChoices->isEnabled(),
            "First-launch integrations must require an explicit selection.");
        useWinE->setChecked(true);
        passed &= expect(startAtSignIn->isEnabled() && !startAtSignIn->isHidden()
            && applyChoices->isEnabled()
            && offer.choice().winE && !offer.choice().folders,
            "Choosing Win+E must not also choose folder routing.");
        startAtSignIn->setChecked(true);
        useWinE->setChecked(false);
        passed &= expect(!startAtSignIn->isChecked() && !startAtSignIn->isEnabled()
            && startAtSignIn->isHidden() && !applyChoices->isEnabled(),
            "Removing Win+E consent must clear sign-in startup consent.");
        useFolders->setChecked(true);
        passed &= expect(applyChoices->isEnabled() && offer.choice().folders
            && !offer.choice().winE && !offer.choice().winEAtSignIn,
            "Choosing folders alone must keep the keyboard helper off.");
    }
    offer.close();
    return passed;
}

bool verifyMainToolbar(MainWindow& window)
{
    auto* toolbar = window.findChild<QToolBar*>("MainActionsToolbar");
    bool compact = toolbar != nullptr;
    bool contextualHelp = toolbar != nullptr;
    int commands = 0;
    if (toolbar) {
        for (QAction* action : toolbar->actions()) {
            if (action->isSeparator()) continue;
            ++commands;
            compact &= !action->icon().isNull();
            contextualHelp &= !action->toolTip().isEmpty();
        }
    }
    return expect(compact && commands <= 11,
            "The primary toolbar should be compact and avoid text-fallback actions.")
        && expect(contextualHelp,
            "Every icon-only toolbar action should have contextual hover help.");
}

int main(int argc, char* argv[])
{
    std::cerr << "Visual phase: application startup" << std::endl;
    QApplication application(argc, argv);
    std::cerr << "Visual phase: QApplication ready" << std::endl;
    application.setOrganizationName("FileManagerTests");
    application.setApplicationName("VisualTests");
    QTemporaryDir settingsStore;
    QSettings::setDefaultFormat(QSettings::IniFormat);
    QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, settingsStore.path());
    QSettings().clear();
    std::cerr << "Visual phase: settings ready" << std::endl;
    filemanager::ui::applyApplicationTheme(application);
    std::cerr << "Visual phase: theme applied" << std::endl;
    bool passed = expect(settingsStore.isValid(), "The isolated settings store should open.");
    passed &= verifySystemTextSizeInTheme(application);
    if (application.arguments().contains("--first-launch-only"))
        return passed && verifyFirstLaunchOffer(
            QDir(FILE_MANAGER_SOURCE_DIR).filePath("out/first-launch-ui-review.png")) ? 0 : 1;
    if (application.arguments().contains("--toolbar-only")) {
        MainWindow window;
        window.resize(1024, 768);
        window.show();
        QApplication::processEvents();
        passed &= verifyMainToolbar(window);
        passed &= expect(window.grab().save(
            QDir(FILE_MANAGER_SOURCE_DIR).filePath("out/toolbar-ui-review.png")),
            "Capture the compact toolbar for review.");
        return passed ? 0 : 1;
    }
    if (application.arguments().contains("--content-search-only")) {
        QTemporaryDir fixture;
        passed &= expect(fixture.isValid(), "Content search fixture should be disposable.");
        if (!fixture.isValid()) return 1;
        QFile source(fixture.filePath("sample.txt"));
        passed &= expect(source.open(QIODevice::WriteOnly)
                && source.write("a unique content-search phrase") > 0,
            "Content search fixture should contain readable text.");
        source.close();
        filemanager::ui::ContentSearchDialog dialog(fixture.path());
        dialog.show();
        QApplication::processEvents();
        auto* query = dialog.findChild<QLineEdit*>("ContentSearchQuery");
        auto* searchButton = dialog.findChild<QPushButton*>("ContentSearchStart");
        auto* results = dialog.findChild<QListWidget*>("ContentSearchResults");
        auto* actions = dialog.findChild<QWidget*>("SearchResultActions");
        auto* empty = dialog.findChild<QLabel*>("ContentSearchEmpty");
        auto* status = dialog.findChild<QLabel*>("ContentSearchStatus");
        passed &= expect(query && searchButton && results && actions && empty && status
                && empty->isVisible() && !results->isVisible() && !actions->isVisible()
                && empty->geometry().top() > query->geometry().bottom()
                && !status->isVisible() && empty->geometry().bottom() <= dialog.contentsRect().bottom(),
            "Content search guidance should occupy the results area below the search fields.");
        const QString captureDirectory = QDir(FILE_MANAGER_SOURCE_DIR).filePath(
            "out/content-search-ui-review");
        QDir().mkpath(captureDirectory);
        passed &= expect(dialog.grab().save(QDir(captureDirectory).filePath("initial.png")),
            "Capture initial content-search guidance.");
        if (query && searchButton && results && actions && empty && status) {
            query->setText("unique content-search phrase");
            searchButton->click();
            QElapsedTimer foundWait;
            foundWait.start();
            while (results->count() == 0 && foundWait.elapsed() < 10000)
                QApplication::processEvents(QEventLoop::AllEvents, 25);
            passed &= expect(results->count() == 1 && results->isVisible()
                    && actions->isVisible() && !empty->isVisible() && status->isVisible(),
                "A matching search should replace guidance with results and actions.");
            query->setText("absent-content-search-r126");
            searchButton->click();
            QElapsedTimer emptyWait;
            emptyWait.start();
            while (!status->text().startsWith("0 results") && emptyWait.elapsed() < 10000)
                QApplication::processEvents(QEventLoop::AllEvents, 25);
            passed &= expect(status->text().startsWith("0 results")
                    && empty->isVisible() && !results->isVisible() && !actions->isVisible() && !status->isVisible(),
                "A no-match search should show concise guidance without empty result actions.");
            passed &= expect(dialog.grab().save(QDir(captureDirectory).filePath("no-match.png")),
                "Capture content-search no-match guidance.");
        }
        return passed ? 0 : 1;
    }
    if (application.arguments().contains("--operation-center-only")) {
        QTemporaryDir fixture;
        passed &= expect(fixture.isValid(), "Operation Center fixture should be disposable.");
        if (!fixture.isValid()) return 1;
        const QString sourceDirectory = fixture.filePath("source");
        const QString destinationDirectory = fixture.filePath("destination");
        passed &= expect(QDir().mkpath(sourceDirectory) && QDir().mkpath(destinationDirectory),
            "Operation Center fixture directories should be created.");
        const QString sourcePath = QDir(sourceDirectory).filePath("example.txt");
        QFile source(sourcePath);
        passed &= expect(source.open(QIODevice::WriteOnly)
                && source.write("example") == 7,
            "Operation Center source should be a small synthetic file.");
        source.close();
        filemanager::files::TransferQueue queue;
        filemanager::files::OperationJournal journal(fixture.filePath("journal.bin"));
        filemanager::ui::OperationCenterDialog dialog(&queue, &journal);
        dialog.show();
        QApplication::processEvents();
        auto* transfers = dialog.findChild<QTableWidget*>("OperationCenterTransfers");
        auto* empty = dialog.findChild<QLabel*>("OperationCenterTransfersEmpty");
        auto* summary = dialog.findChild<QLabel*>("OperationCenterSummary");
        passed &= expect(transfers && empty && empty->isVisible() && !transfers->isVisible()
                && summary && !summary->isVisible(),
            "Empty transfer view should show one guidance message instead of an empty table.");
        bool finished = false;
        QObject::connect(&queue, &filemanager::files::TransferQueue::jobFinished,
            &dialog, [&finished] { finished = true; });
        filemanager::files::TransferRequest request;
        request.sources = {sourcePath};
        request.destinationDirectory = destinationDirectory;
        queue.enqueue(request);
        QApplication::processEvents();
        passed &= expect(transfers && transfers->rowCount() == 1 && transfers->isVisible()
                && empty && !empty->isVisible() && summary->isVisible(),
            "A queued transfer should replace empty guidance with the populated table.");
        QElapsedTimer transferWait;
        transferWait.start();
        while (!finished && transferWait.elapsed() < 10000)
            QApplication::processEvents(QEventLoop::AllEvents, 25);
        passed &= expect(finished && QFileInfo::exists(
                QDir(destinationDirectory).filePath("example.txt")),
            "The synthetic transfer should finish before its fixture is removed.");
        return passed ? 0 : 1;
    }
    if (application.arguments().contains("--preferences-only")) {
        filemanager::ui::PreferencesDialog preferences({});
        preferences.show();
        QApplication::processEvents();
        auto* sections = preferences.findChild<QTabWidget*>("PreferencesTabs");
        passed &= expect(sections && sections->count() == 4
                && sections->tabText(0) == "General"
                && sections->tabText(1) == "Windows"
                && sections->tabText(2) == "Privacy and data"
                && sections->tabText(3) == "Import and export",
            "Preferences should present four clearly named sections.");
        if (sections) {
            auto* search = preferences.findChild<QLineEdit*>("PreferencesSearch");
            passed &= expect(search && search->placeholderText().contains("Find"),
                "Preferences should provide a findable setting list.");
            if (search) {
                search->setText("Win+E");
                QApplication::processEvents();
                passed &= expect(sections->currentIndex() == 1
                        && sections->widget(1)->findChild<QCheckBox*>("WinEIntegration")->isVisible()
                        && !sections->isTabEnabled(0),
                    "Searching should reveal Win+E settings in the Windows section.");
                search->setText("no-setting-matches-this-term");
                QApplication::processEvents();
                auto* noResults = preferences.findChild<QLabel*>("PreferencesNoResults");
                passed &= expect(noResults && noResults->isVisible() && !sections->isVisible(),
                    "Unmatched preference searches should show a clear empty state.");
                search->clear();
                QApplication::processEvents();
                passed &= expect(sections->isVisible() && noResults && !noResults->isVisible()
                        && sections->isTabEnabled(0)
                        && sections->tabText(0) == "General",
                    "Clearing search should restore all preference sections.");
            }
            passed &= expect(sections->widget(0)->findChild<QComboBox*>("StartupDestination")
                    && sections->widget(1)->findChild<QCheckBox*>("DefaultFolderRouting")
                    && sections->widget(1)->findChild<QCheckBox*>("WinEIntegration")
                    && sections->widget(2)->findChild<QTableWidget*>("PermissionDashboardSummary")
                    && sections->widget(2)->findChild<QWidget*>("StoredLocalDataControls")
                    && sections->widget(3)->findChild<QPushButton*>("ExportPortableSettings"),
                "Each preference should remain discoverable in its named section.");
            const QString captureDirectory = QDir(FILE_MANAGER_SOURCE_DIR).filePath("out/preferences-ui-review");
            QDir().mkpath(captureDirectory);
            for (int index = 0; index < sections->count(); ++index) {
                sections->setCurrentIndex(index);
                QApplication::processEvents();
                passed &= expect(preferences.grab().save(QDir(captureDirectory).filePath(
                    QString("section-%1.png").arg(index))),
                    "Capture a Preferences section for visual review.");
            }
            sections->setCurrentIndex(2);
            auto* summary = sections->widget(2)->findChild<QTableWidget*>("PermissionDashboardSummary");
            auto* detailsToggle = sections->widget(2)->findChild<QPushButton*>("PermissionDetailsToggle");
            auto* details = sections->widget(2)->findChild<QLabel*>("PermissionDetails");
            passed &= expect(summary && summary->columnCount() == 2 && detailsToggle && details
                    && !details->isVisible(),
                "Privacy overview should show concise status before optional details.");
            if (detailsToggle && details) {
                detailsToggle->click();
                QApplication::processEvents();
                passed &= expect(details->isVisible()
                        && details->text().contains("no typing is recorded or transmitted"),
                    "Keyboard-accessible privacy details should disclose Win+E behavior.");
                passed &= expect(preferences.grab().save(QDir(captureDirectory).filePath(
                    "permission-details.png")),
                    "Capture expanded permission details for visual review.");
            }
        }
        return passed ? 0 : 1;
    }
    if (application.arguments().contains("--theme-only"))
        return passed ? 0 : 1;
    if (application.arguments().contains("--tags-only"))
        return passed && runTagEditorUiTests() && verifyFileLabelClearControl() ? 0 : 1;
    passed &= runTvOrganizerUiTests(application.arguments().contains("--capture-organizer-dialogs")
        ? QDir(FILE_MANAGER_SOURCE_DIR).filePath("out/tv-organizer-ui-review") : QString{});
    std::cerr << "Visual phase: media organizer finished" << std::endl;
    if (application.arguments().contains("--media-organizer-only"))
        return passed ? 0 : 1;
    passed &= runTagEditorUiTests();
    passed &= verifyFileLabelClearControl();

    application.setLayoutDirection(Qt::RightToLeft);
    {
        FilePane rtlPane;
        QLineEdit* rtlPath = nullptr;
        for (QLineEdit* edit : rtlPane.findChildren<QLineEdit*>())
            if (edit->accessibleName() == "Folder path") rtlPath = edit;
        passed &= expect(rtlPane.layoutDirection() == Qt::RightToLeft
                && rtlPath && rtlPath->layoutDirection() == Qt::RightToLeft,
            "Pane controls should inherit and mirror the application RTL direction.");
    }
    application.setLayoutDirection(Qt::LeftToRight);

    {
        FilePane consentPane(QString::fromLatin1(FilePane::HomeIdentity));
        consentPane.resize(720, 520);
        int providerRequests = 0;
        QObject::connect(&consentPane, &FilePane::shellProviderRequestStarted, &consentPane,
            [&providerRequests] { ++providerRequests; });
        consentPane.navigateBuiltInShell(filemanager::platform::BuiltInShellLocation::Libraries);
        consentPane.show();
        QApplication::processEvents();
        auto* allow = consentPane.findChild<QPushButton*>("AllowShellVisit");
        auto* handoff = consentPane.findChild<QPushButton*>("OpenShellInWindows");
        auto* cancel = consentPane.findChild<QPushButton*>("CancelShellVisit");
        passed &= expect(providerRequests == 0 && allow && handoff && cancel
                && allow->isVisible() && handoff->isVisible() && cancel->isVisible()
                && !allow->accessibleName().isEmpty() && !handoff->accessibleName().isEmpty()
                && consentPane.contentFocusTarget() == allow
                && consentPane.rect().contains(allow->mapTo(&consentPane, allow->rect().bottomRight()))
                && consentPane.rect().contains(cancel->mapTo(&consentPane, cancel->rect().bottomRight())),
            "Virtual-location consent must expose named, unclipped keyboard controls without provider loading at every tested DPI.");
        consentPane.resize(720, 360);
        QApplication::processEvents();
        passed &= expect(providerRequests == 0 && allow && handoff && cancel
                && consentPane.rect().contains(allow->mapTo(&consentPane, allow->rect().bottomRight()))
                && consentPane.rect().contains(handoff->mapTo(&consentPane, handoff->rect().bottomRight()))
                && consentPane.rect().contains(cancel->mapTo(&consentPane, cancel->rect().bottomRight())),
            "Virtual-location consent choices must remain inside a compact pane before provider access.");
        if (cancel)
            cancel->click();
        passed &= expect(consentPane.isHomeLocation() && providerRequests == 0,
            "Cancelling the consent surface must return to Home without starting a provider.");
    }

    QTemporaryDir fixture;
    QDir().mkpath(fixture.filePath("Projects/Nested/Deep"));
    QDir().mkpath(fixture.filePath("Empty"));
    QFile notes(fixture.filePath("release-notes.txt"));
    passed &= expect(notes.open(QIODevice::WriteOnly), "Visual text fixture should open.");
    notes.write("File Manager visual fixture");
    notes.close();
    QFile image(fixture.filePath("cover.png"));
    passed &= expect(image.open(QIODevice::WriteOnly), "Visual image fixture should open.");
    image.write("fixture");
    image.close();
    const QString unicodePath = fixture.filePath(QStringLiteral("ملف-日本語-😀.txt"));
    QFile unicodeFile(unicodePath);
    passed &= expect(unicodeFile.open(QIODevice::WriteOnly),
        "Visual Unicode filename fixture should open.");
    unicodeFile.write("unicode fixture");
    unicodeFile.close();
    passed &= verifyShellVisitTree(fixture.path());
    passed &= verifyBoundedFolderTree();
    passed &= verifyLexicalNavigationParents();
    passed &= verifyDelegateThumbnail();

    const QString nestedFolder = fixture.filePath("Projects/Nested");
    const QString nativeNestedFolder = QDir::toNativeSeparators(QDir::cleanPath(nestedFolder));
    passed &= expect(filemanager::ui::BreadcrumbBar::normalizeAddressInput(
            QStringLiteral("  \"") + nativeNestedFolder + QStringLiteral("\"  "))
            == nativeNestedFolder,
        "The address editor should normalize a quoted absolute path.");
    const QString uncPath = QStringLiteral("\\\\server\\share\\folder");
    passed &= expect(filemanager::ui::BreadcrumbBar::normalizeAddressInput(uncPath) == uncPath,
        "The address editor should preserve a double-backslash UNC identity.");
    const QString extendedPath = QStringLiteral("\\\\?\\C:\\Folder\\..\\Item");
    passed &= expect(filemanager::ui::BreadcrumbBar::normalizeAddressInput(extendedPath)
            == QStringLiteral("\\\\?\\C:\\Item"),
        "The address editor should preserve an extended-length Windows path prefix.");
    passed &= expect(filemanager::ui::BreadcrumbBar::normalizeAddressInput(
            QStringLiteral("%TEMP%\\literal")) == QStringLiteral("%TEMP%\\literal")
            && filemanager::ui::BreadcrumbBar::normalizeAddressInput(
                QStringLiteral("\"unterminated")).isEmpty(),
        "Address normalization should neither expand environment syntax nor accept unmatched quotes.");

    {
        filemanager::ui::BreadcrumbBar breadcrumbs;
        breadcrumbs.setPath(nestedFolder);
        breadcrumbs.resize(760, 36);
        breadcrumbs.show();
        QApplication::processEvents();
        const auto segments = breadcrumbs.findChildren<QToolButton*>("BreadcrumbSegment");
        QToolButton* projectsSegment = nullptr;
        for (QToolButton* segment : segments) {
            if (segment->text() == QStringLiteral("Projects")) {
                projectsSegment = segment;
                break;
            }
        }
        QString activatedPath;
        QString backgroundPath;
        QObject::connect(&breadcrumbs, &filemanager::ui::BreadcrumbBar::pathActivated,
            [&](const QString& path) { activatedPath = QDir::cleanPath(path); });
        QObject::connect(&breadcrumbs, &filemanager::ui::BreadcrumbBar::backgroundPathRequested,
            [&](const QString& path) { backgroundPath = QDir::cleanPath(path); });
        if (projectsSegment)
            projectsSegment->click();
        passed &= expect(projectsSegment
                && activatedPath == QDir::cleanPath(fixture.filePath("Projects")),
            "Clicking an ancestor breadcrumb should navigate to that exact folder.");
        if (projectsSegment) {
            const QPoint center = projectsSegment->rect().center();
            QMouseEvent middleRelease(QEvent::MouseButtonRelease, center,
                projectsSegment->mapToGlobal(center), Qt::MiddleButton, Qt::MiddleButton,
                Qt::NoModifier);
            QApplication::sendEvent(projectsSegment, &middleRelease);
        }
        passed &= expect(backgroundPath == QDir::cleanPath(fixture.filePath("Projects")),
            "Middle-clicking a breadcrumb should request a background tab.");
        QMenu* folderMenu = projectsSegment ? projectsSegment->menu() : nullptr;
        const bool menuWasLazy = folderMenu && folderMenu->property("loadState").toInt() == 0;
        if (folderMenu) {
            folderMenu->popup(projectsSegment->mapToGlobal(
                QPoint(0, projectsSegment->height())));
            QElapsedTimer menuWait;
            menuWait.start();
            while (folderMenu->property("loadState").toInt() != 2
                    && menuWait.elapsed() < 3000) {
                QApplication::processEvents(QEventLoop::AllEvents, 25);
            }
        }
        bool nestedListed = false;
        if (folderMenu) {
            for (QAction* action : folderMenu->actions())
                nestedListed |= QFileInfo(action->data().toString()).fileName() == "Nested";
            folderMenu->close();
            QApplication::processEvents();
        }
        passed &= expect(menuWasLazy && nestedListed,
            "Breadcrumb dropdowns should enumerate immediate child folders only when opened.");
        breadcrumbs.activateWindow();
        breadcrumbs.raise();
        breadcrumbs.beginEditing();
        QLineEdit* addressEditor = breadcrumbs.editor();
        QElapsedTimer editorFocusWait;
        editorFocusWait.start();
        while ((!addressEditor->hasFocus()
                    || addressEditor->selectedText() != addressEditor->text())
                && editorFocusWait.elapsed() < 1000) {
            QApplication::processEvents(QEventLoop::AllEvents, 25);
        }
        passed &= expect(breadcrumbs.isEditing() && addressEditor->hasFocus()
                && addressEditor->selectedText() == addressEditor->text(),
            "Exact-path editing should select the complete address.");
        QKeyEvent escape(QEvent::KeyPress, Qt::Key_Escape, Qt::NoModifier);
        QApplication::sendEvent(addressEditor, &escape);
        passed &= expect(!breadcrumbs.isEditing(),
            "Escape should leave exact-path editing without navigating.");
    }

    {
        QSettings homeSettings;
        filemanager::settings::FavoriteGroups favorites;
        favorites.groups.append({QStringLiteral("home-test"), QStringLiteral("Pinned"),
            {fixture.filePath("Projects")}});
        filemanager::settings::saveFavoriteGroups(homeSettings, favorites);
        homeSettings.setValue(QStringLiteral("navigation/recents"),
            QStringList{fixture.filePath("Empty")});
        filemanager::search::SavedSearchStore savedSearches;
        QString savedSearchError;
        passed &= expect(savedSearches.clear(&savedSearchError),
            "The isolated Home saved-search store should start empty.");
        filemanager::search::SearchFilters savedFilters;
        savedFilters.name = QStringLiteral("release");
        passed &= expect(savedSearches.save({QStringLiteral("home-search"),
                QStringLiteral("Release files"), fixture.path(), savedFilters, 0, 0},
                &savedSearchError),
            "The isolated Home saved-search fixture should be encrypted successfully.");

        filemanager::ui::HomeView home;
        home.resize(760, 520);
        home.show();
        QApplication::processEvents();
        auto* entries = home.findChild<QTreeWidget*>("HomeLocations");
        auto* privacy = home.findChild<QLabel*>("HomePrivacyDisclosure");
        QTreeWidgetItem* favorite = nullptr;
        QTreeWidgetItem* recent = nullptr;
        QTreeWidgetItem* saved = nullptr;
        if (entries) {
            const auto items = entries->findItems(
                QStringLiteral("*"), Qt::MatchWildcard | Qt::MatchRecursive);
            for (QTreeWidgetItem* item : items) {
                if (item->text(0) == QStringLiteral("Projects"))
                    favorite = item;
                else if (item->text(0) == QStringLiteral("Empty"))
                    recent = item;
                else if (item->text(0) == QStringLiteral("Release files"))
                    saved = item;
            }
        }
        passed &= expect(entries && privacy && privacy->textFormat() == Qt::PlainText
                && privacy->text().contains("does not scan folders")
                && favorite && recent && saved && home.itemCount() == 3,
            "Home should expose only pinned, recent and explicitly saved local entries with disclosure.");

        QString activatedHomePath;
        QString backgroundHomePath;
        QString activatedSearch;
        QObject::connect(&home, &filemanager::ui::HomeView::pathActivated,
            [&](const QString& path) { activatedHomePath = QDir::cleanPath(path); });
        QObject::connect(&home, &filemanager::ui::HomeView::backgroundPathRequested,
            [&](const QString& path) { backgroundHomePath = QDir::cleanPath(path); });
        QObject::connect(&home, &filemanager::ui::HomeView::savedSearchActivated,
            [&](const QString& id) { activatedSearch = id; });
        if (favorite) {
            entries->setCurrentItem(favorite);
            home.activateCurrent();
        }
        if (recent) {
            const QPoint point = entries->visualItemRect(recent).center();
            QMouseEvent middleRelease(QEvent::MouseButtonRelease, point,
                entries->viewport()->mapToGlobal(point), Qt::MiddleButton,
                Qt::MiddleButton, Qt::NoModifier);
            QApplication::sendEvent(entries->viewport(), &middleRelease);
        }
        if (saved) {
            entries->setCurrentItem(saved);
            home.activateCurrent();
        }
        passed &= expect(activatedHomePath == QDir::cleanPath(fixture.filePath("Projects"))
                && backgroundHomePath == QDir::cleanPath(fixture.filePath("Empty"))
                && activatedSearch == QStringLiteral("home-search"),
            "Home entries should support keyboard activation and background-tab middle click.");

        TabbedPanel homeTabs(fixture.path());
        FilePane* homePane = homeTabs.addTab(QLatin1String(FilePane::HomeIdentity));
        FilePane* duplicatedHome = homeTabs.duplicateCurrentTab();
        homeTabs.closeCurrentTab();
        FilePane* reopenedHome = homeTabs.reopenLastClosedTab();
        TabbedPanel restoredHomeTabs(fixture.path(), nullptr, false);
        restoredHomeTabs.restoreTabs({QLatin1String(FilePane::HomeIdentity)}, 0);
        filemanager::search::SessionSearchHistory homeSearchHistory;
        filemanager::ui::FileSearchDialog savedSearchDialog(
            fixture.path(), &homeSearchHistory);
        passed &= expect(homePane && homePane->isHomeLocation()
                && homePane->currentPath().isEmpty()
                && homeTabs.paths().contains(QLatin1String(FilePane::HomeIdentity))
                && duplicatedHome && duplicatedHome->isHomeLocation()
                && reopenedHome && reopenedHome->isHomeLocation()
                && restoredHomeTabs.currentPane()
                && restoredHomeTabs.currentPane()->isHomeLocation()
                && savedSearchDialog.activateSavedSearch(QStringLiteral("home-search")),
            "Home should survive tab duplication, close/reopen and layout restore, and open saved searches.");
        homeSettings.remove(QStringLiteral("navigation/favoriteGroups"));
        homeSettings.remove(QStringLiteral("navigation/recents"));
        homeSettings.sync();
        passed &= expect(savedSearches.clear(&savedSearchError),
            "The isolated Home saved-search fixture should be removable.");
        home.refresh();
        QApplication::processEvents();
        auto* emptyState = home.findChild<QLabel*>("HomeEmptyState");
        passed &= expect(home.itemCount() == 0 && entries
                && entries->topLevelItemCount() == 0 && entries->isHidden()
                && emptyState && emptyState->isVisible() && privacy
                && privacy->isVisible() && home.focusTarget() == emptyState,
            "An empty Home should show concise guidance without empty groups or a table header.");
    }

    {
        MainWindow addressWindow;
        addressWindow.resize(900, 600);
        addressWindow.show();
        addressWindow.activateWindow();
        addressWindow.raise();
        QApplication::processEvents();
        FilePane* pane = nullptr;
        for (FilePane* candidate : addressWindow.findChildren<FilePane*>()) {
            if (candidate->isVisible()) {
                pane = candidate;
                break;
            }
        }
        if (pane)
            pane->navigate(nestedFolder);
        QAction* editAddress = addressWindow.findChild<QAction*>("navigation.address");
        if (editAddress)
            editAddress->trigger();
        auto* integratedBar = pane
            ? pane->findChild<filemanager::ui::BreadcrumbBar*>("BreadcrumbAddressBar") : nullptr;
        passed &= expect(editAddress && pane && integratedBar && integratedBar->isEditing()
                && waitForFocus(integratedBar->editor()),
            "Ctrl+L's command should enter exact-path editing in the active pane.");
        const int panesBeforeBackgroundOpen = addressWindow.findChildren<FilePane*>().size();
        if (pane)
            emit pane->backgroundTabRequested(fixture.filePath("Projects"));
        QApplication::processEvents();
        passed &= expect(pane && pane->isVisible()
                && addressWindow.findChildren<FilePane*>().size() == panesBeforeBackgroundOpen + 1,
            "A breadcrumb background request should add a tab without changing the active pane.");
        QAction* openHome = addressWindow.findChild<QAction*>("navigation.home");
        if (openHome)
            openHome->trigger();
        passed &= expect(openHome && openHome->shortcut() == QKeySequence("Alt+Home")
                && pane && pane->isHomeLocation()
                && pane->findChild<filemanager::ui::HomeView*>("HomePage"),
            "Alt+Home should open the first-class local Home surface in the active tab.");
        addressWindow.close();
    }

    QSettings shortcutMigrationSettings;
    shortcutMigrationSettings.remove("migrations/explorerFunctionKeys");
    shortcutMigrationSettings.setValue("shortcuts/panel.copy_to_other", "F5");
    shortcutMigrationSettings.setValue("shortcuts/panel.move_to_other", "F6");
    MainWindow directStartupWindow(fixture.path());
    passed &= expect(directStartupWindow.findChild<FilePane*>()
            && QFileInfo(directStartupWindow.findChild<FilePane*>()->currentPath())
                .absoluteFilePath() == QFileInfo(fixture.path()).absoluteFilePath(),
        "An explicit launch folder should initialize the first pane directly.");
    directStartupWindow.close();

    {
        MainWindow transferWindow(fixture.path());
        auto* transferWorkspace = transferWindow.findChild<filemanager::workspace::WorkspaceView*>();
        passed &= expect(transferWorkspace != nullptr, "The window must expose its live workspace.");
        if (transferWorkspace) {
            transferWorkspace->splitActive(Qt::Horizontal);
            FilePane* moved = transferWorkspace->currentPane();
            passed &= expect(transferWorkspace->moveActiveTabToAdjacent()
                    && transferWorkspace->moveActiveTabToAdjacent(),
                "A live pane should survive repeated within-window moves.");
            const auto before = transferWorkspace->panes().size();
            emit moved->backgroundTabRequested(fixture.filePath("Projects"));
            passed &= expect(transferWorkspace->panes().size() == before + 1,
                "A moved pane's single command must run exactly once in its owning window.");
        }
        transferWindow.close();
    }

    std::cerr << "Visual phase: main window" << std::endl;
    MainWindow window;
    // The reference is a fixed client-area capture. A resize before first show
    // may be clamped to the available desktop on the contained acceptance VM.
    // Keep the canonical fixture size without changing the product's sizing.
    window.setFixedSize(1280, 800);
    passed &= expect(shortcutMigrationSettings.value("migrations/explorerFunctionKeys").toInt() == 1
            && shortcutMigrationSettings.value("shortcuts/panel.copy_to_other").toString()
                == QStringLiteral("Ctrl+Alt+F5")
            && shortcutMigrationSettings.value("shortcuts/panel.move_to_other").toString()
                == QStringLiteral("Ctrl+Alt+F6"),
        "Saved legacy transfer defaults should migrate away from Explorer's F5/F6 keys once.");
    passed &= expect(window.findChild<FilePane*>()
            && window.findChild<FilePane*>()->isHomeLocation(),
        "A new window should honor Home as the safe default startup destination.");
    for (FilePane* pane : window.findChildren<FilePane*>())
        pane->navigate(fixture.path());
    window.show();
    QEventLoop settle;
    QTimer::singleShot(500, &settle, &QEventLoop::quit);
    settle.exec();

    passed &= verifyMainToolbar(window);

    if (FilePane* unicodePane = window.findChild<FilePane*>()) {
        unicodePane->revealPath(unicodePath);
        QElapsedTimer unicodeWait;
        unicodeWait.start();
        while (!unicodePane->selectedPaths().contains(unicodePath)
                && unicodeWait.elapsed() < 3000) {
            QApplication::processEvents(QEventLoop::AllEvents, 25);
            unicodePane->revealPath(unicodePath);
        }
        passed &= expect(unicodePane->selectedPaths().contains(unicodePath),
            "File panes should reveal RTL, CJK and supplementary Unicode filenames exactly.");
    }

    {
        QAction* focusNext = window.findChild<QAction*>("navigation.focus_next_region");
        QAction* focusPrevious = window.findChild<QAction*>("navigation.focus_previous_region");
        QAction* copyToPanel = window.findChild<QAction*>("panel.copy_to_other");
        QAction* moveToPanel = window.findChild<QAction*>("panel.move_to_other");
        QAction* refresh = window.findChild<QAction*>("view.refresh");
        QAction* navigationPane = window.findChild<QAction*>("view.navigation_pane");
        QAction* inspector = window.findChild<QAction*>("panel.inspector");
        QTabBar* tabs = window.findChild<QTabBar*>("FolderTabBar");
        auto* navigation = window.findChild<filemanager::ui::NavigationTree*>("QuickAccess");
        FilePane* pane = window.findChild<FilePane*>();
        QWidget* content = pane ? pane->contentFocusTarget() : nullptr;
        QWidget* details = window.findChild<QWidget*>("DetailsPane");
        QStatusBar* status = window.findChild<QStatusBar*>("ApplicationStatusBar");
        passed &= expect(focusNext && focusPrevious && copyToPanel && moveToPanel && refresh
                && focusNext->shortcut() == QKeySequence("F6")
                && focusPrevious->shortcut() == QKeySequence("Shift+F6")
                && refresh->shortcut() == QKeySequence::Refresh
                && copyToPanel->shortcut() == QKeySequence("Ctrl+Alt+F5")
                && moveToPanel->shortcut() == QKeySequence("Ctrl+Alt+F6"),
            "Explorer function keys should be unambiguous while panel transfers retain shortcuts.");

        if (tabs && navigation && content && details && status && focusNext && focusPrevious
                && navigationPane && inspector) {
            tabs->setFocus();
            focusNext->trigger();
            passed &= expect(waitForFocus(navigation),
                "F6 should move from folder tabs to the visible navigation pane.");
            focusNext->trigger();
            passed &= expect(waitForFocus(content),
                "F6 should move from navigation to the active content pane.");
            focusNext->trigger();
            passed &= expect(waitForFocus(status),
                "A hidden Details pane should be skipped before the status bar.");
            focusNext->trigger();
            passed &= expect(waitForFocus(tabs),
                "F6 should wrap from the status bar to folder tabs.");
            focusPrevious->trigger();
            passed &= expect(waitForFocus(status),
                "Shift+F6 should cycle through the same regions in reverse.");

            inspector->trigger();
            QApplication::processEvents();
            content->setFocus();
            focusNext->trigger();
            passed &= expect(waitForFocus(details),
                "A visible Details pane should join the F6 cycle after content.");
            focusNext->trigger();
            passed &= expect(waitForFocus(status),
                "The status bar should follow a visible Details pane.");
            inspector->trigger();

            navigationPane->trigger();
            QApplication::processEvents();
            tabs->setFocus();
            focusNext->trigger();
            passed &= expect(waitForFocus(content),
                "A hidden navigation pane should be omitted from region cycling.");
            navigationPane->trigger();

            pane->navigateHome();
            QApplication::processEvents();
            QWidget* homeContent = pane->contentFocusTarget();
            tabs->setFocus();
            focusNext->trigger();
            const bool homeContentFocused = waitForFocus(homeContent);
            focusNext->trigger();
            const bool homeNavigationFocused = waitForFocus(navigation);
            focusNext->trigger();
            passed &= expect(homeContentFocused && homeNavigationFocused && waitForFocus(tabs),
                "Home should follow File Explorer's tabs, content, navigation F6 cycle.");

            pane->navigate(fixture.path());
            QElapsedTimer restoreWait;
            restoreWait.start();
            while (pane->visibleItemCount() < 5 && restoreWait.elapsed() < 3000)
                QApplication::processEvents(QEventLoop::AllEvents, 25);
            passed &= expect(pane->visibleItemCount() == 5,
                "Focus-region testing should restore the original folder before visual capture.");
        } else {
            passed &= expect(false, "Every F6 focus-region target should be discoverable.");
        }
    }

    passed &= expect(QSettings().value("guidance/firstRunHintShown").toBool(),
        "The first-run hint should record only that it has been shown.");
    window.statusBar()->clearMessage();

    passed &= verifyFirstLaunchOffer();

    bool permissionDashboardInspected = false;
    bool winEPointerToggleWorks = false;
    QTimer::singleShot(0, [&] {
        auto* preferences = qobject_cast<QDialog*>(QApplication::activeModalWidget());
        auto* summary = preferences
            ? preferences->findChild<QTableWidget*>("PermissionDashboardSummary") : nullptr;
        auto* clearIndexes = preferences
            ? preferences->findChild<QPushButton*>("ClearAllMetadataIndexes") : nullptr;
        auto* includeDiagnosticPaths = preferences
            ? preferences->findChild<QCheckBox*>("IncludeDiagnosticPaths") : nullptr;
        auto* exportDiagnostics = preferences
            ? preferences->findChild<QPushButton*>("ExportDiagnosticBundle") : nullptr;
        auto* clearDiagnosticHistory = preferences
            ? preferences->findChild<QPushButton*>("ClearDiagnosticHistory") : nullptr;
        auto* exportPortableSettings = preferences
            ? preferences->findChild<QPushButton*>("ExportPortableSettings") : nullptr;
        auto* importPortableSettings = preferences
            ? preferences->findChild<QPushButton*>("ImportPortableSettings") : nullptr;
        auto* storedDataControls = preferences
            ? preferences->findChild<QWidget*>("StoredLocalDataControls") : nullptr;
        auto* startupDestination = preferences
            ? preferences->findChild<QComboBox*>("StartupDestination") : nullptr;
        auto* selectionCheckBoxes = preferences
            ? preferences->findChild<QCheckBox*>("ItemSelectionCheckBoxes") : nullptr;
        auto* winE = preferences ? preferences->findChild<QCheckBox*>("WinEIntegration") : nullptr;
        auto* winEStartup = preferences ? preferences->findChild<QCheckBox*>("WinEAtSignIn") : nullptr;
        auto* defaultFolders = preferences ? preferences->findChild<QCheckBox*>("DefaultFolderRouting") : nullptr;
        auto* routingStatus = preferences ? preferences->findChild<QLabel*>("FolderRoutingStatus") : nullptr;
        auto* restoreRouting = preferences ? preferences->findChild<QPushButton*>("RestoreFolderRouting") : nullptr;
        if (winE && winE->isEnabled() && winE->width() > 50) {
            const bool original = winE->isChecked();
            const auto click = [winE](const QPoint& point) {
                QMouseEvent press(QEvent::MouseButtonPress, point,
                    winE->mapToGlobal(point), Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
                QApplication::sendEvent(winE, &press);
                QMouseEvent release(QEvent::MouseButtonRelease, point,
                    winE->mapToGlobal(point), Qt::LeftButton, Qt::NoButton, Qt::NoModifier);
                QApplication::sendEvent(winE, &release);
            };
            click(QPoint(7, winE->height() / 2));
            const bool indicatorToggled = winE->isChecked() != original;
            click(QPoint(45, winE->height() / 2));
            winEPointerToggleWorks = indicatorToggled && winE->isChecked() == original;
        }
        bool hasOneTimeNetwork = false;
        bool hasOneTimeContent = false;
        if (summary) {
            for (int row = 0; row < summary->rowCount(); ++row) {
                const QString capability = summary->item(row, 0)->text();
                const QString access = summary->item(row, 1)->text();
                hasOneTimeNetwork |= capability == "Network discovery"
                    && access == "One-time only";
                hasOneTimeContent |= capability == "Content search"
                    && access == "One-time only";
            }
        }
        permissionDashboardInspected = preferences
            && preferences->findChild<QWidget*>("PermissionDashboard")
            && summary && summary->rowCount() == 8 && clearIndexes
            && includeDiagnosticPaths && !includeDiagnosticPaths->isChecked()
            && exportDiagnostics && clearDiagnosticHistory
            && exportPortableSettings && importPortableSettings
            && startupDestination && startupDestination->currentText() == "Home"
            && selectionCheckBoxes && !selectionCheckBoxes->isChecked()
            && winE && winEStartup
            && defaultFolders && routingStatus && !routingStatus->text().isEmpty() && restoreRouting
            && (!winEStartup->isEnabled() || winE->isChecked())
            && storedDataControls
            && storedDataControls->findChild<QLabel*>("StoredLocalDataDisclosure")
            && storedDataControls->findChild<QPushButton*>("ClearSavedFavorites")
            && storedDataControls->findChild<QPushButton*>("ClearRecentLocations")
            && storedDataControls->findChild<QPushButton*>(
                "ClearAllSavedSearchesFromPreferences")
            && storedDataControls->findChild<QPushButton*>("ClearAllFileLabels")
            && storedDataControls->findChild<QPushButton*>("ClearSessionSearchHistory")
            && hasOneTimeNetwork && hasOneTimeContent;
        if (preferences)
            preferences->reject();
    });
    if (QAction* preferences = window.findChild<QAction*>("options.open"))
        preferences->trigger();
    passed &= expect(permissionDashboardInspected,
        "Preferences should centralize persistent permissions, revocation and one-time access.");
    passed &= expect(winEPointerToggleWorks,
        "The Win+E checkbox should toggle from both its indicator and label with a pointer.");

    bool interactionPreferencesConfigured = false;
    QTimer::singleShot(0, [&] {
        auto* preferences = qobject_cast<QDialog*>(QApplication::activeModalWidget());
        auto* startupDestination = preferences
            ? preferences->findChild<QComboBox*>("StartupDestination") : nullptr;
        auto* selectionCheckBoxes = preferences
            ? preferences->findChild<QCheckBox*>("ItemSelectionCheckBoxes") : nullptr;
        if (startupDestination)
            startupDestination->setCurrentText("Last folder");
        if (selectionCheckBoxes)
            selectionCheckBoxes->setChecked(true);
        interactionPreferencesConfigured = startupDestination && selectionCheckBoxes;
        if (preferences)
            preferences->accept();
    });
    if (QAction* preferences = window.findChild<QAction*>("options.open"))
        preferences->trigger();
    FilePane* preferencePane = window.findChild<FilePane*>();
    passed &= expect(interactionPreferencesConfigured && preferencePane
            && QSettings().value("navigation/openTo").toString() == "last-folder"
            && QSettings().value("interaction/itemSelectionCheckBoxes").toBool()
            && preferencePane->selectionCheckBoxes(),
        "Accepting Preferences should persist and apply startup and selection-check settings.");
    QSettings().setValue("navigation/openTo", "home");
    QSettings().setValue("interaction/itemSelectionCheckBoxes", false);
    for (FilePane* pane : window.findChildren<FilePane*>())
        pane->setSelectionCheckBoxes(false);

    bool ambiguousDropConfirmed = false;
    QTimer::singleShot(0, [&] {
        auto* confirmation = qobject_cast<QMessageBox*>(QApplication::activeModalWidget());
        ambiguousDropConfirmed = confirmation
            && confirmation->objectName() == "FileDropConfirmationDialog"
            && confirmation->text().contains("No explicit copy/move effect")
            && confirmation->defaultButton()
            && confirmation->defaultButton()->text().contains("Cancel");
        if (confirmation)
            confirmation->reject();
    });
    if (FilePane* pane = window.findChild<FilePane*>())
        emit pane->filesDropped({unicodePath}, fixture.path(), false, true, false);
    passed &= expect(ambiguousDropConfirmed,
        "An ambiguous file drop should require a cancel-default copy/move confirmation.");

    bool aboutSupportInspected = false;
    QTimer::singleShot(0, [&] {
        auto* about = qobject_cast<QDialog*>(QApplication::activeModalWidget());
        auto* details = about ? about->findChild<QLabel*>("AboutSupportDetails") : nullptr;
        auto* topics = about ? about->findChild<QTabWidget*>("AboutSupportTopics") : nullptr;
        auto* dependencies = about
            ? about->findChild<QLabel*>("AboutSupportDependencies") : nullptr;
        auto* diagnostics = about
            ? about->findChild<QLabel*>("AboutSupportDiagnostics") : nullptr;
        auto* copySupport = about
            ? about->findChild<QPushButton*>("CopySupportSummary") : nullptr;
        aboutSupportInspected = about && about->objectName() == "AboutSupportDialog"
            && details && details->text().contains("File Manager")
            && details->text().contains("Qt runtime")
            && topics && topics->count() == 3
            && topics->tabText(0) == "Overview"
            && topics->tabText(1) == "Dependencies"
            && topics->tabText(2) == "Diagnostics"
            && dependencies && dependencies->text().contains("minizip-ng 4.2.2")
            && diagnostics && diagnostics->text().contains("path-free codes only")
            && diagnostics->text().contains("Crash dumps are not collected")
            && copySupport;
        if (about)
            about->reject();
    });
    if (QAction* aboutSupport = window.findChild<QAction*>("help.about_support"))
        aboutSupport->trigger();
    passed &= expect(aboutSupportInspected,
        "About and Support should expose version, dependency and privacy-safe diagnostic details.");

    passed &= expect(!filemanager::ui::BrandIdentity::applicationIcon().isNull(),
        "The centralized application brand icon should load.");
    for (const int side : {16, 20, 24, 32, 40, 48, 64, 128, 256}) {
        const QPixmap taskbarIcon = window.windowIcon().pixmap(QSize(side, side), 1.0);
        passed &= expect(taskbarIcon.size() == QSize(side, side)
            && taskbarIcon.toImage().pixelColor(0, 0).alpha() == 0,
            "Windows window/taskbar icons must have square transparent canvases, not wide logos.");
    }
    passed &= expect(!filemanager::ui::BrandIdentity::logo(QSize(128, 48)).isNull(),
        "The centralized application logo should load.");
    QLabel* brandLogo = window.findChild<QLabel*>("BrandLogo");
    passed &= expect(brandLogo && !brandLogo->pixmap().isNull(),
        "The main window should expose the shared sidebar logo.");
    if (brandLogo) {
        passed &= expect(qFuzzyCompare(brandLogo->pixmap().devicePixelRatio(),
            brandLogo->devicePixelRatioF()),
            "Sidebar branding should use the display's physical pixel density.");
    }
    const QPixmap sourceLogo(filemanager::ui::BrandIdentity::logoResourcePath());
    passed &= expect(filemanager::ui::BrandIdentity::logoResourcePath().endsWith(".svg"),
        "The shared logo should use the vector source rather than its PNG export.");
    const QPixmap largeLogo = filemanager::ui::BrandIdentity::logo(QSize(2048, 2048));
    passed &= expect(!largeLogo.isNull()
        && largeLogo.size() == sourceLogo.size().scaled(QSize(2048, 2048), Qt::KeepAspectRatio),
        "The SVG icon engine should render beyond the source's nominal dimensions.");
    for (const qreal scale : {1.0, 1.25, 1.5, 2.0, 3.0, 4.0}) {
        for (const QSize& bounds : {QSize(148, 44), QSize(220, 72), QSize(220, 220)}) {
            const QPixmap rendered = filemanager::ui::BrandIdentity::logo(bounds, scale);
            const QSize physicalBounds(static_cast<int>(std::ceil(bounds.width() * scale)),
                static_cast<int>(std::ceil(bounds.height() * scale)));
            passed &= expect(!rendered.isNull()
                && qFuzzyCompare(rendered.devicePixelRatio(), scale)
                && rendered.size() == sourceLogo.size().scaled(physicalBounds, Qt::KeepAspectRatio),
                "Every branding surface should preserve aspect ratio at 100 through 400 percent DPI.");
        }
    }
    {
        QLabel scalingLabel;
        filemanager::ui::BrandIdentity::setLogo(&scalingLabel, QSize(148, 44));
        const qint64 beforeScaleChange = scalingLabel.pixmap().cacheKey();
        QEvent scaleChange(QEvent::DevicePixelRatioChange);
        QApplication::sendEvent(&scalingLabel, &scaleChange);
        passed &= expect(scalingLabel.pixmap().cacheKey() != beforeScaleChange,
            "Branding should be rerendered after a display-scale change.");
        QPixmap filePreview(40, 40);
        filePreview.fill(Qt::red);
        scalingLabel.setPixmap(filePreview);
        QApplication::sendEvent(&scalingLabel, &scaleChange);
        passed &= expect(scalingLabel.pixmap().cacheKey() == filePreview.cacheKey(),
            "A scale change must not replace a file preview with the application logo.");
        scalingLabel.clear();
        QApplication::sendEvent(&scalingLabel, &scaleChange);
        passed &= expect(scalingLabel.pixmap().isNull(),
            "A scale change must not restore branding after the preview was cleared.");
    }
    passed &= expect(window.findChild<QAction*>("favorites.groups") != nullptr,
        "Favorite-group management should be registered as a shared command.");
    passed &= expect(window.findChild<QAction*>("navigation.this_pc") != nullptr
            && window.findChild<QAction*>("navigation.libraries") != nullptr
            && window.findChild<QAction*>("navigation.recycle_bin") != nullptr
            && window.findChild<QAction*>("navigation.storage_details") != nullptr,
        "Built-in Windows destinations and retained storage details should be shared commands.");
    passed &= expect(window.findChild<QAction*>("navigation.network") != nullptr,
        "The explicit network surface should be registered as a shared command.");
    QAction* mapNetworkDrive = window.findChild<QAction*>("navigation.map_network_drive");
    QAction* disconnectNetworkDrive =
        window.findChild<QAction*>("navigation.disconnect_network_drive");
    QMenu* networkDriveMenu = window.findChild<QMenu*>("file.network_drives");
    passed &= expect(mapNetworkDrive && disconnectNetworkDrive && networkDriveMenu
            && networkDriveMenu->actions().contains(mapNetworkDrive)
            && networkDriveMenu->actions().contains(disconnectNetworkDrive)
            && mapNetworkDrive->toolTip().contains("Windows")
            && mapNetworkDrive->toolTip().contains("does not store credentials"),
        "Windows-owned map and disconnect dialogs should be explicit shared commands with a "
        "credential-boundary disclosure.");
    QAction* restoreRecycleBin = window.findChild<QAction*>("recycle.restore_selected");
    QAction* emptyRecycleBin = window.findChild<QAction*>("recycle.empty");
    QMenu* recycleBinMenu = window.findChild<QMenu*>("file.recycle_bin");
    passed &= expect(restoreRecycleBin && emptyRecycleBin && recycleBinMenu
            && recycleBinMenu->actions().contains(restoreRecycleBin)
            && recycleBinMenu->actions().contains(emptyRecycleBin)
            && !restoreRecycleBin->isEnabled() && !emptyRecycleBin->isEnabled(),
        "Recycle Bin mutations should be explicit shared commands and remain disabled outside "
        "the Recycle Bin destination.");
    passed &= expect(window.findChild<QAction*>("file.details") != nullptr
            && window.findChild<QAction*>("file.properties") != nullptr,
        "Read-only details and native Windows Properties should remain separate registered commands.");
    passed &= expect(window.findChild<QAction*>("file.create_shortcut") != nullptr,
        "Native Windows shortcut creation should be a shared searchable command.");
    passed &= expect(window.findChild<QAction*>("selection.all") != nullptr
            && window.findChild<QAction*>("selection.none") != nullptr
            && window.findChild<QAction*>("selection.invert") != nullptr,
        "Selection operations should remain shared, searchable commands.");
    QAction* nativeArchive = window.findChild<QAction*>("archive.open_native");
    passed &= expect(nativeArchive
            && nativeArchive->text().contains("RAR/7z/TAR")
            && nativeArchive->toolTip().contains("Windows-selected handler")
            && nativeArchive->toolTip().contains("does not extract"),
        "Archive handoff should be an explicit shared command with an accurate native boundary.");
    passed &= expect(window.findChild<QAction*>("tab.move_left") != nullptr
            && window.findChild<QAction*>("tab.move_right") != nullptr
            && window.findChild<QAction*>("tab.move_to_other") != nullptr
            && window.findChild<QAction*>("favorites.move_up") != nullptr
            && window.findChild<QAction*>("favorites.move_down") != nullptr,
        "Every tab and favorite drag workflow should expose a shared keyboard command.");
    const auto labelByAccessibleName = [&window](const QString& name) -> QLabel* {
        for (QLabel* label : window.findChildren<QLabel*>())
            if (label->accessibleName() == name) return label;
        return nullptr;
    };
    const QLabel* previewTitle = labelByAccessibleName("Preview title");
    const QLabel* previewMetadata = labelByAccessibleName("Preview metadata");
    const QLabel* previewText = labelByAccessibleName("Preview contents");
    passed &= expect(previewTitle && previewMetadata && previewText
            && previewTitle->textFormat() == Qt::PlainText
            && previewMetadata->textFormat() == Qt::PlainText
            && previewText->textFormat() == Qt::PlainText,
        "Inspector labels should render untrusted filenames, metadata and contents as plain text.");
    auto* quickAccess = window.findChild<filemanager::ui::NavigationTree*>("QuickAccess");
    bool foundFavoriteGroup = false;
    bool foundThisPc = false;
    bool foundLibraries = false;
    bool foundRecycleBin = false;
    if (quickAccess) {
        const auto items = quickAccess->findItems(
            QStringLiteral("*"), Qt::MatchWildcard | Qt::MatchRecursive);
        for (QTreeWidgetItem* item : items) {
            foundFavoriteGroup |= item->data(0, filemanager::ui::NavigationTree::KindRole)
                .toString() == "favorite-group";
            foundThisPc |= item->data(0, filemanager::ui::NavigationTree::PathRole)
                .toString() == "__this_pc__";
            foundLibraries |= item->data(0, filemanager::ui::NavigationTree::KindRole)
                .toString() == "libraries";
            foundRecycleBin |= item->data(0, filemanager::ui::NavigationTree::KindRole)
                .toString() == "recycle-bin";
        }
    }
    passed &= expect(foundFavoriteGroup,
        "The sidebar should expose the migrated default favorite group.");
    passed &= expect(foundThisPc && foundLibraries && foundRecycleBin,
        "The sidebar should expose This PC, Libraries and Recycle Bin as first-class locations.");
    QAction* navigationPaneAction = window.findChild<QAction*>("view.navigation_pane");
    passed &= expect(navigationPaneAction && navigationPaneAction->isCheckable()
            && navigationPaneAction->isChecked() && quickAccess && quickAccess->isVisible(),
        "View should expose a persistent navigation-pane visibility control.");
    if (navigationPaneAction && quickAccess) {
        navigationPaneAction->trigger();
        QApplication::processEvents();
        passed &= expect(!quickAccess->isVisible(),
            "Turning off the navigation pane should hide the complete sidebar.");
        navigationPaneAction->trigger();
        QApplication::processEvents();
        passed &= expect(quickAccess->isVisible(),
            "The navigation pane should restore without rebuilding the workspace.");
    }
    if (quickAccess) {
        QTreeWidgetItem* folderItem = nullptr;
        const auto items = quickAccess->findItems(
            QStringLiteral("*"), Qt::MatchWildcard | Qt::MatchRecursive);
        for (QTreeWidgetItem* item : items) {
            if (item->data(0, filemanager::ui::NavigationTree::KindRole).toString()
                    == QStringLiteral("known-folder")) {
                folderItem = item;
                break;
            }
        }
        bool folderMenuComplete = false;
        if (folderItem) {
            QTimer::singleShot(0, [&] {
                auto* menu = qobject_cast<QMenu*>(QApplication::activePopupWidget());
                QStringList labels;
                if (menu) {
                    for (QAction* action : menu->actions())
                        labels.append(action->text());
                    folderMenuComplete = labels.contains("Open")
                        && labels.contains("Open in new tab")
                        && labels.contains("Open in new window")
                        && labels.contains("Pin to favorites")
                        && labels.contains("Properties");
                    menu->close();
                }
            });
            quickAccess->customContextMenuRequested(
                quickAccess->visualItemRect(folderItem).center());
        }
        passed &= expect(folderItem && folderMenuComplete,
            "Navigation folders should expose open, tab, window, pin and Properties actions.");
    }

    {
        filemanager::settings::FavoriteGroups favorites;
        favorites.groups.append({QStringLiteral("test-group"), QStringLiteral("Pinned"),
            {fixture.filePath("Projects")}});
        filemanager::ui::NavigationTree navigation;
        navigation.rebuild({}, favorites, {});
        navigation.resize(320, 420);
        navigation.show();
        QApplication::processEvents();
        QTreeWidgetItem* pinned = nullptr;
        const auto items = navigation.findItems(
            QStringLiteral("*"), Qt::MatchWildcard | Qt::MatchRecursive);
        for (QTreeWidgetItem* item : items) {
            if (item->data(0, filemanager::ui::NavigationTree::KindRole).toString()
                    == QStringLiteral("favorite")) {
                pinned = item;
                break;
            }
        }
        passed &= expect(pinned
                && pinned->childIndicatorPolicy() == QTreeWidgetItem::ShowIndicator
                && pinned->flags().testFlag(Qt::ItemIsDropEnabled),
            "Folder locations should expose disclosure arrows and folder drop targets.");
        QString treeBackgroundPath;
        bool homeBackgroundRequested = false;
        bool thisPcBackgroundRequested = false;
        bool librariesBackgroundRequested = false;
        bool recycleBinBackgroundRequested = false;
        QObject::connect(&navigation,
            &filemanager::ui::NavigationTree::backgroundLocationRequested,
            [&](const QString& path) { treeBackgroundPath = QDir::cleanPath(path); });
        QObject::connect(&navigation,
            &filemanager::ui::NavigationTree::homeBackgroundRequested,
            [&] { homeBackgroundRequested = true; });
        QObject::connect(&navigation,
            &filemanager::ui::NavigationTree::thisPcBackgroundRequested,
            [&] { thisPcBackgroundRequested = true; });
        QObject::connect(&navigation,
            &filemanager::ui::NavigationTree::librariesBackgroundRequested,
            [&] { librariesBackgroundRequested = true; });
        QObject::connect(&navigation,
            &filemanager::ui::NavigationTree::recycleBinBackgroundRequested,
            [&] { recycleBinBackgroundRequested = true; });
        if (pinned) {
            const QPoint middlePoint = navigation.visualItemRect(pinned).center();
            QMouseEvent middleRelease(QEvent::MouseButtonRelease, middlePoint,
                navigation.viewport()->mapToGlobal(middlePoint), Qt::MiddleButton,
                Qt::MiddleButton, Qt::NoModifier);
            QApplication::sendEvent(navigation.viewport(), &middleRelease);
        }
        passed &= expect(treeBackgroundPath == QDir::cleanPath(fixture.filePath("Projects")),
            "Middle-clicking a navigation-tree folder should request a background tab.");
        QTreeWidgetItem* homeItem = nullptr;
        for (QTreeWidgetItem* item : items) {
            if (item->data(0, filemanager::ui::NavigationTree::KindRole).toString()
                    == QStringLiteral("home")) {
                homeItem = item;
                break;
            }
        }
        if (homeItem) {
            const QPoint homePoint = navigation.visualItemRect(homeItem).center();
            QMouseEvent homeMiddleRelease(QEvent::MouseButtonRelease, homePoint,
                navigation.viewport()->mapToGlobal(homePoint), Qt::MiddleButton,
                Qt::MiddleButton, Qt::NoModifier);
            QApplication::sendEvent(navigation.viewport(), &homeMiddleRelease);
        }
        passed &= expect(homeItem && homeBackgroundRequested,
            "Middle-clicking the Home destination should request a background Home tab.");
        const auto middleClickKind = [&](const QString& kind) {
            for (QTreeWidgetItem* item : items) {
                if (item->data(0, filemanager::ui::NavigationTree::KindRole).toString() != kind)
                    continue;
                const QPoint point = navigation.visualItemRect(item).center();
                QMouseEvent release(QEvent::MouseButtonRelease, point,
                    navigation.viewport()->mapToGlobal(point), Qt::MiddleButton,
                    Qt::MiddleButton, Qt::NoModifier);
                QApplication::sendEvent(navigation.viewport(), &release);
                return true;
            }
            return false;
        };
        const bool middleClickedThisPc = middleClickKind(QStringLiteral("this-pc"));
        const bool middleClickedLibraries = middleClickKind(QStringLiteral("libraries"));
        const bool middleClickedRecycleBin = middleClickKind(QStringLiteral("recycle-bin"));
        passed &= expect(middleClickedThisPc && middleClickedLibraries
                && middleClickedRecycleBin && thisPcBackgroundRequested
                && librariesBackgroundRequested && recycleBinBackgroundRequested,
            "Every built-in Windows destination should support background-tab middle click.");
        if (pinned) {
            pinned->setExpanded(true);
            QElapsedTimer wait;
            wait.start();
            QTreeWidgetItem* nested = nullptr;
            while (!nested && wait.elapsed() < 5000) {
                QApplication::processEvents(QEventLoop::AllEvents, 50);
                QThread::msleep(10);
                for (int index = 0; index < pinned->childCount(); ++index) {
                    QTreeWidgetItem* child = pinned->child(index);
                    if (child->text(0) == QStringLiteral("Nested")) {
                        nested = child;
                        break;
                    }
                }
            }
            passed &= expect(nested != nullptr,
                "Expanding a folder should lazily enumerate its immediate subfolders.");
            passed &= expect(nested && nested->childCount() == 0,
                "Expanding a folder must not recursively enumerate unopened descendants.");
            navigation.synchronizeToPath(fixture.filePath("Projects/Nested"));
            passed &= expect(navigation.currentItem() == nested,
                "Navigation should reveal and select the active filesystem path.");

            bool dropRouted = false;
            QString dropDestination;
            QObject::connect(&navigation, &filemanager::ui::NavigationTree::filesDropped,
                [&](const QStringList& paths, const QString& destination, bool move,
                    bool confirmationRequired, bool linkRequested) {
                    dropRouted = paths == QStringList{fixture.filePath("release-notes.txt")}
                        && !move && !confirmationRequired && !linkRequested;
                    dropDestination = destination;
                });
            QMimeData mime;
            mime.setUrls({QUrl::fromLocalFile(fixture.filePath("release-notes.txt"))});
            const QPoint dropPoint = navigation.visualItemRect(pinned).center();
            QDragEnterEvent enter(dropPoint, Qt::CopyAction, &mime,
                Qt::LeftButton, Qt::NoModifier);
            QApplication::sendEvent(navigation.viewport(), &enter);
            QDragMoveEvent move(dropPoint, Qt::CopyAction, &mime,
                Qt::LeftButton, Qt::NoModifier);
            QApplication::sendEvent(navigation.viewport(), &move);
            QDropEvent drop(QPointF(dropPoint),
                Qt::CopyAction, &mime, Qt::LeftButton, Qt::NoModifier);
            drop.setDropAction(Qt::CopyAction);
            QApplication::sendEvent(navigation.viewport(), &drop);
            passed &= expect(dropRouted
                    && QDir::cleanPath(dropDestination)
                        == QDir::cleanPath(fixture.filePath("Projects")),
                "Dropping a local file on a folder node should route an explicit copy effect.");
        }
        passed &= expect(navigation.favoriteOrder().value(QStringLiteral("test-group"))
                == QStringList{fixture.filePath("Projects")},
            "Hierarchical navigation should preserve app-owned bookmark order.");
    }

    const QList<FilePane*> panes = window.findChildren<FilePane*>();
    passed &= expect(!panes.isEmpty(), "Main window should expose an active file pane.");
    if (!panes.isEmpty()) {
        panes.at(0)->setShowHidden(true);
        panes.at(0)->setShowFileExtensions(false);
        panes.at(0)->setViewMode(FilePane::ViewMode::LargeIcons);
        panes.at(0)->setFolderPlacement(FilePane::FolderPlacement::Last);
        panes.at(0)->setGroupBy(FilePane::GroupBy::Name);
        panes.at(0)->setSort(1, Qt::DescendingOrder);
        panes.at(0)->setColumnVisible(6, false);
        emit panes.at(0)->activated(panes.at(0));
        passed &= expect(window.findChild<QAction*>("view.hidden")->isChecked(),
            "Hidden-items command should follow the active tab.");
        passed &= expect(!window.findChild<QAction*>("view.file_extensions")->isChecked(),
            "Extension command should follow the active tab.");
        passed &= expect(window.findChild<QAction*>("view.large_icons")->isChecked(),
            "View-mode command should follow the active tab.");
        passed &= expect(window.findChild<QAction*>("view.folders_last")->isChecked(),
            "Folder-placement command should follow the active tab.");
        passed &= expect(window.findChild<QAction*>("view.group_name")->isChecked(),
            "Group-by Name should follow the active tab.");
        passed &= expect(window.findChild<QAction*>("view.sort_size")->isChecked()
                && window.findChild<QAction*>("view.sort_descending")->isChecked(),
            "Visible sort property and direction commands should follow the active tab.");
        passed &= expect(!window.findChild<QAction*>("view.column_dimensions")->isChecked()
                && window.findChild<QAction*>("view.column_name")->isChecked()
                && !window.findChild<QAction*>("view.column_name")->isEnabled(),
            "The bounded Columns menu should expose visibility while keeping Name mandatory.");
        auto* detailsView = panes.at(0)->findChild<QTreeView*>("FolderDetailsView");
        bool headerMenuInspected = false;
        if (detailsView) {
            panes.at(0)->setViewMode(FilePane::ViewMode::Details);
            panes.at(0)->setColumnVisible(4, false);
            QTimer::singleShot(0, [&] {
                auto* menu = qobject_cast<QMenu*>(QApplication::activePopupWidget());
                QStringList commandIds;
                if (menu) {
                    for (QAction* action : menu->actions())
                        commandIds.append(action->objectName());
                    QAction* created = window.findChild<QAction*>("view.column_created");
                    headerMenuInspected = menu->objectName() == "DetailsColumnMenu"
                        && menu->accessibleName() == "Choose folder detail columns"
                        && menu->actions().size() == FilePane::DetailsColumnCount
                        && commandIds.contains("view.column_modified")
                        && commandIds.contains("view.column_created")
                        && commandIds.contains("view.column_availability")
                        && created && !created->isChecked();
                    if (created)
                        created->trigger();
                    menu->close();
                }
            });
            detailsView->header()->customContextMenuRequested(
                detailsView->header()->rect().center());
        }
        passed &= expect(headerMenuInspected && panes.at(0)->columnVisible(4),
            "Right-clicking the Details header should expose and apply the shared column commands.");
        QAbstractItemModel* groupedModel = panes.at(0)->view()->model();
        const QModelIndex groupedRoot = panes.at(0)->view()->rootIndex();
        bool foundExtensionMetadata = false;
        bool foundVisibleGroup = false;
        for (int row = 0; row < groupedModel->rowCount(groupedRoot); ++row) {
            foundExtensionMetadata |= !groupedModel->index(row, 5, groupedRoot)
                                           .data(Qt::DisplayRole).toString().isEmpty();
            foundVisibleGroup |= !groupedModel->index(row, 0, groupedRoot)
                                      .data(filemanager::ui::FileItemDelegate::GroupLabelRole)
                                      .toString().isEmpty();
        }
        passed &= expect(foundExtensionMetadata,
            "Details rows should expose extension metadata values.");
        passed &= expect(foundVisibleGroup,
            "Grouped rows should expose visible group headings.");

        panes.at(0)->setShowHidden(false);
        panes.at(0)->setShowFileExtensions(true);
        panes.at(0)->setViewMode(FilePane::ViewMode::Details);
        panes.at(0)->setFolderPlacement(FilePane::FolderPlacement::First);
        panes.at(0)->setGroupBy(FilePane::GroupBy::None);
        panes.at(0)->setColumnVisible(6, true);
        panes.at(0)->setSort(0, Qt::AscendingOrder);
        passed &= expect(!window.findChild<QAction*>("view.hidden")->isChecked()
                && window.findChild<QAction*>("view.file_extensions")->isChecked()
                && window.findChild<QAction*>("view.details")->isChecked()
                && window.findChild<QAction*>("view.folders_first")->isChecked()
                && window.findChild<QAction*>("view.group_none")->isChecked()
                && window.findChild<QAction*>("view.sort_name")->isChecked()
                && window.findChild<QAction*>("view.sort_ascending")->isChecked()
                && window.findChild<QAction*>("view.column_dimensions")->isChecked(),
            "View commands should resynchronize after the active tab state changes.");
    }
    for (FilePane* pane : panes) {
        pane->clearSelection();
        pane->view()->selectionModel()->setCurrentIndex({}, QItemSelectionModel::NoUpdate);
        pane->view()->clearFocus();
    }
    if (quickAccess) {
        filemanager::settings::FavoriteGroups visualFavorites;
        visualFavorites.groups.append({QStringLiteral("visual-favorites"),
            QStringLiteral("Favorites"), {}});
        const QVector<filemanager::files::StorageVolume> visualVolumes = {
            {QStringLiteral("C:/"), QStringLiteral("C:/"), false,
                filemanager::files::StorageLocationKind::Fixed, 0, 0},
            {QStringLiteral("D:/"), QStringLiteral("D:/"), false,
                filemanager::files::StorageLocationKind::Fixed, 0, 0},
            {QStringLiteral("E:/"), QStringLiteral("E:/"), false,
                filemanager::files::StorageLocationKind::Fixed, 0, 0}};
        quickAccess->rebuild({}, visualFavorites, visualVolumes);
        QTreeWidgetItem* singleFavoriteGroup = nullptr;
        bool emptySidebarSection = false;
        for (int index = 0; index < quickAccess->topLevelItemCount(); ++index) {
            QTreeWidgetItem* item = quickAccess->topLevelItem(index);
            const QString kind = item->data(0,
                filemanager::ui::NavigationTree::KindRole).toString();
            if (kind == "favorite-group")
                singleFavoriteGroup = item;
            emptySidebarSection |= kind == "bookmarks-section"
                || kind == "recents-section";
        }
        passed &= expect(singleFavoriteGroup && !emptySidebarSection
                && singleFavoriteGroup->flags().testFlag(Qt::ItemIsDropEnabled)
                && !singleFavoriteGroup->toolTip(0).isEmpty(),
            "The first-use sidebar should keep one Favorites drop target without empty headings.");
        quickAccess->rebuild({fixture.path()}, visualFavorites, visualVolumes);
        quickAccess->scrollToTop();
    }
    window.setFocusPolicy(Qt::StrongFocus);
    window.setFocus();
    QApplication::processEvents();

    const QImage screenshot = window.grab().toImage().convertToFormat(QImage::Format_RGBA8888);
    std::fprintf(stdout, "Visual capture: client=%dx%d raster=%dx%d dpr=%.3f\n",
        window.width(), window.height(), screenshot.width(), screenshot.height(), screenshot.devicePixelRatio());
    passed &= expect(!screenshot.isNull() && screenshot.width() >= 1200 && screenshot.height() >= 700,
        "Main-window screenshot should render at the reference size.");

    const QString baselineOverride = qEnvironmentVariable("FILE_MANAGER_VISUAL_BASELINE");
    const QString baselinePath = baselineOverride.isEmpty()
        ? QStringLiteral(FILE_MANAGER_SOURCE_DIR) + "/tests/baselines/main-window-100.png"
        : baselineOverride;
    const bool invariantsOnly = application.arguments().contains("--invariants-only");
    if (application.arguments().contains("--update-baseline")) {
        QDir().mkpath(QFileInfo(baselinePath).absolutePath());
        passed &= expect(screenshot.save(baselinePath), "Visual baseline should save.");
    } else if (!invariantsOnly) {
        const QImage baseline(baselinePath);
        passed &= expect(!baseline.isNull(), "Visual baseline should exist.");
        if (!baseline.isNull() && baseline.size() == screenshot.size()) {
            const QImage expected = baseline.convertToFormat(QImage::Format_RGBA8888);
            // Run-specific tab names, paths, timestamps, and recent-folder labels are
            // excluded from pixel comparison. Keep the raw screenshot for review.
            const auto isVolatilePixel = [](int x, int y) {
                return (x >= 285 && x < 525 && y >= 90 && y < 138)
                    || (x >= 440 && x < 1180 && y >= 140 && y < 205)
                    || (x >= 850 && x < 1135 && y >= 275 && y < 435)
                    || (x >= 20 && x < 285 && y >= 635 && y < 680)
                    || (x >= 130 && y >= 775);
            };
            qsizetype changedPixels = 0;
            qsizetype comparedPixels = 0;
            double totalDifference = 0;
            const qsizetype pixels = screenshot.width() * screenshot.height();
            for (int y = 0; y < screenshot.height(); ++y) {
                const QRgb* actualLine = reinterpret_cast<const QRgb*>(screenshot.constScanLine(y));
                const QRgb* expectedLine = reinterpret_cast<const QRgb*>(expected.constScanLine(y));
                for (int x = 0; x < screenshot.width(); ++x) {
                    if (isVolatilePixel(x, y))
                        continue;
                    ++comparedPixels;
                    const int difference = std::abs(qRed(actualLine[x]) - qRed(expectedLine[x]))
                        + std::abs(qGreen(actualLine[x]) - qGreen(expectedLine[x]))
                        + std::abs(qBlue(actualLine[x]) - qBlue(expectedLine[x]));
                    totalDifference += difference / 3.0;
                    if (difference > 24)
                        ++changedPixels;
                }
            }
            passed &= expect(comparedPixels * 5 > pixels * 4,
                "Visual comparison should retain over eighty percent of the screenshot.");
            const double changedRatio = static_cast<double>(changedPixels) / comparedPixels;
            const double meanDifference = totalDifference / comparedPixels;
            if (changedRatio >= 0.02 || meanDifference >= 2.0) {
                screenshot.save(QDir(QCoreApplication::applicationDirPath())
                    .filePath("visual-regression-actual.png"));
                std::cerr << "Visual difference: changed=" << changedRatio
                          << " mean=" << meanDifference << '\n';
            }
            passed &= expect(changedRatio < 0.02,
                "Visual regression changed more than two percent of pixels.");
            passed &= expect(meanDifference < 2.0,
                "Visual regression mean pixel difference is too high.");
    } else if (!baseline.isNull()) {
            screenshot.save(QDir(QCoreApplication::applicationDirPath())
                .filePath("visual-regression-actual.png"));
            std::fprintf(stderr, "Visual dimensions: actual=%dx%d baseline=%dx%d\n",
                screenshot.width(), screenshot.height(), baseline.width(), baseline.height());
            passed &= expect(false, "Visual baseline dimensions changed.");
        }
    }

    window.openInitialPaths({fixture.path(), fixture.filePath("Projects"), fixture.path()});
    const QList<FilePane*> startupPanes = window.findChildren<FilePane*>();
    passed &= expect(startupPanes.size() == 2,
        "Distinct startup paths should open once across the current tab and new tabs.");
    bool openedProject = false;
    for (FilePane* pane : startupPanes)
        openedProject |= QDir::cleanPath(pane->currentPath())
            == QDir::cleanPath(fixture.filePath("Projects"));
    passed &= expect(openedProject, "An additional startup folder should open in its own tab.");

    bool commandPaletteInspected = false;
    if (QAction* commandPalette = window.findChild<QAction*>("commands.open")) {
        QTimer::singleShot(0, [&] {
            auto* dialog = qobject_cast<QDialog*>(QApplication::activeModalWidget());
            if (!dialog)
                return;
            auto* query = dialog->findChild<QLineEdit*>();
            auto* results = dialog->findChild<QListWidget*>();
            if (query && results) {
                const QString captureDirectory = QDir(FILE_MANAGER_SOURCE_DIR).filePath("out/compact-menu-ui-review");
                QDir().mkpath(captureDirectory);
                dialog->grab().save(QDir(captureDirectory).filePath("commands.png"));
                bool containsFavoriteGroups = false;
                for (int row = 0; row < results->count(); ++row)
                    containsFavoriteGroups |= results->item(row)->data(Qt::UserRole).toString()
                        == "favorites.groups";
                query->setText("deselect");
                commandPaletteInspected = dialog->windowTitle() == "Command palette"
                    && containsFavoriteGroups && results->count() == 1
                    && results->item(0)->data(Qt::UserRole).toString() == "selection.none"
                    && results->item(0)->data(Qt::UserRole + 1).toString().isEmpty()
                    && results->item(0)->data(Qt::AccessibleTextRole).toString() == "Select none"
                    && results->item(0)->data(Qt::AccessibleDescriptionRole).toString()
                        .startsWith("Shortcut: ");
            }
            dialog->reject();
        });
        commandPalette->trigger();
    }
    passed &= expect(commandPaletteInspected,
        "Command palette should list registered commands and match their aliases.");

    bool helpBrowserInspected = false;
    if (QAction* help = window.findChild<QAction*>("help.browse")) {
        QTimer::singleShot(0, [&] {
            auto* dialog = qobject_cast<QDialog*>(QApplication::activeModalWidget());
            auto* search = dialog ? dialog->findChild<QLineEdit*>("HelpSearch") : nullptr;
            auto* topics = dialog ? dialog->findChild<QListWidget*>("HelpTopics") : nullptr;
            if (search && topics) {
                const QString captureDirectory = QDir(FILE_MANAGER_SOURCE_DIR).filePath("out/compact-menu-ui-review");
                QDir().mkpath(captureDirectory);
                dialog->grab().save(QDir(captureDirectory).filePath("help.png"));
                search->setText("rename");
                helpBrowserInspected = topics->count() == 1
                    && topics->item(0)->text().contains("Batch Rename");
            }
            if (dialog) dialog->reject();
        });
        help->trigger();
    }
    passed &= expect(helpBrowserInspected,
        "Help should offer a searchable topic list before opening detailed guides.");

    bool operationCenterInspected = false;
    QTimer::singleShot(0, [&] {
        auto* dialog = qobject_cast<QDialog*>(QApplication::activeModalWidget());
        if (dialog) {
            auto* tabs = dialog->findChild<QTabWidget*>("OperationCenterTabs");
            const auto* transfers = dialog->findChild<QTableWidget*>("OperationCenterTransfers");
            const auto* recovery = dialog->findChild<QTableWidget*>("OperationCenterRecovery");
            operationCenterInspected = dialog->windowTitle() == "Operation center"
                && dialog->objectName() == "OperationCenterDialog"
                && tabs && tabs->count() == 2 && transfers && recovery
                && transfers->accessibleName().contains("active")
                && recovery->accessibleName().contains("Recoverable");
            auto* summary = dialog->findChild<QLabel*>("OperationCenterSummary");
            auto* transfersEmpty = dialog->findChild<QLabel*>("OperationCenterTransfersEmpty");
            auto* recoveryEmpty = dialog->findChild<QLabel*>("OperationCenterRecoveryEmpty");
            if (transfers && recovery && transfers->rowCount() == 0 && recovery->rowCount() == 0) {
                operationCenterInspected &= summary && !summary->isVisible()
                    && summary->text() == "Nothing to review yet."
                    && transfersEmpty && transfersEmpty->isVisible() && !transfers->isVisible();
                tabs->setCurrentIndex(1);
                QApplication::processEvents();
                operationCenterInspected &= recoveryEmpty && recoveryEmpty->isVisible()
                    && !recovery->isVisible();
                tabs->setCurrentIndex(0);
                QApplication::processEvents();
            } else {
                operationCenterInspected &= summary && summary->isVisible()
                    && summary->text().contains("Queued:")
                    && summary->text().contains("Recoverable:");
            }
            if (application.arguments().contains("--capture-operation-center") && tabs) {
                const QString captureDirectory = QDir(FILE_MANAGER_SOURCE_DIR).filePath(
                    "out/operation-center-ui-review");
                QDir().mkpath(captureDirectory);
                tabs->setCurrentIndex(0);
                QApplication::processEvents();
                operationCenterInspected &= dialog->grab().save(
                    QDir(captureDirectory).filePath("transfers.png"));
                tabs->setCurrentIndex(1);
                QApplication::processEvents();
                operationCenterInspected &= dialog->grab().save(
                    QDir(captureDirectory).filePath("recovery.png"));
            }
            dialog->reject();
        }
    });
    if (QAction* operationCenter = window.findChild<QAction*>("transfers.open"))
        operationCenter->trigger();
    passed &= expect(operationCenterInspected,
        "Operation center should unify transfer states and recoverable work in two accessible views.");

    bool goToInspected = false;
    QTimer::singleShot(50, [&] {
        auto* dialog = qobject_cast<QDialog*>(QApplication::activeModalWidget());
        if (!dialog)
            return;
        auto* search = dialog->findChild<QLineEdit*>("GoToSearch");
        auto* results = dialog->findChild<QListWidget*>("GoToResults");
        int headings = 0;
        int destinations = 0;
        bool fuzzyKnownFolder = false;
        bool typedPath = false;
        if (search && results) {
            search->setText("Documents");
            QApplication::processEvents();
            for (int row = 0; row < results->count(); ++row)
                fuzzyKnownFolder |= results->item(row)->text().contains("Documents");
            search->setText(QDir(fixture.path()).filePath("typed-destination"));
            QApplication::processEvents();
            for (int row = 0; row < results->count(); ++row)
                typedPath |= results->item(row)->text() == "Typed path";
            search->clear();
            QApplication::processEvents();
            for (int row = 0; row < results->count(); ++row) {
                const QString kind = results->item(row)->data(Qt::UserRole + 1).toString();
                headings += kind == "heading";
                destinations += kind == "destination";
            }
        }
        goToInspected = dialog->objectName() == "GoToOverlay" && search && results
            && headings >= 3 && destinations >= 3
            && fuzzyKnownFolder && typedPath
            && results->accessibleName() == "Grouped navigation destinations";
        if (!goToInspected) {
            std::cerr << "GoTo invariant details: dialog="
                      << (dialog->objectName() == "GoToOverlay")
                      << " search=" << (search != nullptr)
                      << " results=" << (results != nullptr)
                      << " focus=" << (search && search->hasFocus())
                      << " headings=" << headings
                      << " destinations=" << destinations
                      << " fuzzy=" << fuzzyKnownFolder
                      << " typed=" << typedPath
                      << " accessible=" << (results
                            && results->accessibleName() == "Grouped navigation destinations")
                      << '\n';
        }
        dialog->reject();
    });
    if (QAction* goTo = window.findChild<QAction*>("navigation.goto"))
        goTo->trigger();
    passed &= expect(goToInspected,
        "GoTo should group bounded destinations, filter fuzzily and accept typed absolute paths.");

    QSettings().setValue("interaction/pinnedContextActions",
        QStringList{"file.rename", "clipboard.copy"});
    bool contextMenuInspected = false;
    QTimer::singleShot(0, [&] {
        auto* menu = qobject_cast<QMenu*>(QApplication::activePopupWidget());
        if (!menu)
            return;
        auto* search = menu->findChild<QLineEdit*>("ContextMenuSearch");
        int visiblePins = 0;
        QAction* rename = nullptr;
        QAction* remove = nullptr;
        QAction* createShortcut = nullptr;
        for (QAction* action : menu->actions()) {
            if (action->property("contextPinned").toBool() && action->isVisible())
                ++visiblePins;
            if (!action->property("contextPinned").toBool()
                    && action->property("contextActionId").toString() == "file.rename")
                rename = action;
            if (action->property("contextActionId").toString() == "file.delete")
                remove = action;
            if (action->property("contextActionId").toString() == "file.create_shortcut")
                createShortcut = action;
        }
        if (search) {
            search->setText("rename");
            QApplication::processEvents();
        }
        bool pinsHiddenDuringSearch = true;
        for (QAction* action : menu->actions()) {
            if (action->property("contextPinned").toBool() && action->isVisible())
                pinsHiddenDuringSearch = false;
        }
        contextMenuInspected = menu->objectName() == "FileContextMenu" && search
            && search->accessibleName() == "Search File Manager commands"
            && visiblePins == 2 && rename && rename->isVisible()
            && remove && !remove->isVisible() && createShortcut
            && !createShortcut->isVisible() && pinsHiddenDuringSearch;
        menu->close();
    });
    const QList<FilePane*> contextPanes = window.findChildren<FilePane*>();
    if (!contextPanes.isEmpty())
        contextPanes.first()->contextMenuRequested(contextPanes.first(), QPoint(5, 5));
    passed &= expect(contextMenuInspected,
        "The built-in context menu should render stable-ID pins and filter actions locally.");

    filemanager::settings::Session namedSession;
    namedSession.workspaceLayoutJson = QString::fromUtf8(QJsonDocument(
        filemanager::workspace::LayoutNode::panelNode({{"C:/Private/Layout"}, 0}).toJson())
        .toJson(QJsonDocument::Compact));
    QString namedLayoutError;
    const bool namedLayoutSaved = filemanager::settings::SessionStore{}.saveNamedWorkspace(
        "Research", namedSession, nullptr, &namedLayoutError);
    if (!namedLayoutSaved)
        std::cerr << "Named layout fixture error: " << namedLayoutError.toStdString() << '\n';
    passed &= expect(namedLayoutSaved,
        "The named-layout visual fixture should save.");
    bool namedLayoutsInspected = false;
    QTimer::singleShot(0, [&] {
        auto* dialog = qobject_cast<QDialog*>(QApplication::activeModalWidget());
        if (!dialog)
            return;
        auto* disclosure = dialog->findChild<QLabel*>("NamedLayoutsDisclosure");
        auto* list = dialog->findChild<QListWidget*>("NamedLayoutsList");
        namedLayoutsInspected = dialog->objectName() == "NamedLayoutsDialog"
            && disclosure && disclosure->textFormat() == Qt::PlainText
            && disclosure->text().contains("encrypted")
            && disclosure->text().contains("only while workspace restoration is enabled")
            && list && list->accessibleName() == "Saved named layouts" && list->count() == 1
            && dialog->findChild<QPushButton*>("LoadNamedLayout")
            && dialog->findChild<QPushButton*>("ReplaceNamedLayout")
            && dialog->findChild<QPushButton*>("DeleteNamedLayout")
            && dialog->findChild<QPushButton*>("SaveNamedLayoutAs");
        dialog->reject();
    });
    if (QAction* namedLayouts = window.findChild<QAction*>("layout.load"))
        namedLayouts->trigger();
    passed &= expect(namedLayoutsInspected,
        "Named layouts should expose accessible load, replace, delete and save-as controls.");

    bool permanentDeleteInspected = false;
    if (!contextPanes.isEmpty()) {
        contextPanes.first()->navigate(fixture.path());
        contextPanes.first()->revealPath(fixture.filePath("release-notes.txt"));
        QEventLoop selectionSettle;
        QTimer::singleShot(500, &selectionSettle, &QEventLoop::quit);
        selectionSettle.exec();
        QTimer::singleShot(0, [&] {
            auto* dialog = qobject_cast<QDialog*>(QApplication::activeModalWidget());
            if (!dialog)
                return;
            auto* warning = dialog->findChild<QLabel*>("PermanentDeleteWarning");
            auto* typed = dialog->findChild<QLineEdit*>("PermanentDeleteConfirmation");
            auto* destroy = dialog->findChild<QPushButton*>("ConfirmPermanentDelete");
            const bool initiallyDisabled = destroy && !destroy->isEnabled();
            if (typed) {
                typed->setText("delete");
                QApplication::processEvents();
            }
            const bool wrongPhraseDisabled = destroy && !destroy->isEnabled();
            if (typed) {
                typed->setText("DELETE");
                QApplication::processEvents();
            }
            QAction* command = window.findChild<QAction*>("file.delete_permanently");
            permanentDeleteInspected = dialog->objectName() == "PermanentDeleteDialog"
                && warning && warning->textFormat() == Qt::PlainText
                && warning->text().contains("cannot undo or recover")
                && typed && typed->accessibleName().contains("Type DELETE")
                && initiallyDisabled && wrongPhraseDisabled && destroy && destroy->isEnabled()
                && command && command->shortcut() == QKeySequence("Shift+Delete");
            dialog->reject();
        });
        if (QAction* permanentDelete = window.findChild<QAction*>("file.delete_permanently"))
            permanentDelete->trigger();
    }
    passed &= expect(permanentDeleteInspected && QFileInfo::exists(
            fixture.filePath("release-notes.txt")),
        "Permanent deletion should require exact typed confirmation and cancellation must preserve the item.");

    const auto inspectGuidanceDialog = [&window, &passed](const QString& commandId,
        const QString& objectName, const QString& contentObjectName,
        const QString& expectedText, const QStringList& expectedTabs,
        const char* failureMessage) {
        bool inspected = false;
        QTimer::singleShot(0, [&] {
            auto* dialog = qobject_cast<QDialog*>(QApplication::activeModalWidget());
            if (!dialog)
                return;
            auto* content = dialog->findChild<QLabel*>(contentObjectName);
            bool tabsMatch = expectedTabs.isEmpty();
            if (!expectedTabs.isEmpty()) {
                auto* topics = dialog->findChild<QTabWidget*>();
                tabsMatch = topics && topics->count() == expectedTabs.size()
                    && !topics->accessibleName().isEmpty();
                if (tabsMatch) {
                    for (int index = 0; index < expectedTabs.size(); ++index)
                        tabsMatch &= topics->tabText(index) == expectedTabs.at(index);
                }
            }
            inspected = dialog->objectName() == objectName && content
                && content->textFormat() == Qt::PlainText
                && content->accessibleName().size() > 4
                && content->text().contains(expectedText)
                && tabsMatch
                && dialog->findChild<QDialogButtonBox*>();
            dialog->reject();
        });
        if (QAction* command = window.findChild<QAction*>(commandId))
            command->trigger();
        passed &= expect(inspected, failureMessage);
    };
    inspectGuidanceDialog("help.getting_started", "GettingStartedDialog",
        "GettingStartedContent", "Open RAR/7z/TAR using Windows",
        {"Browse", "File actions", "Privacy"},
        "Getting Started should explain core workflows and privacy defaults accessibly.");
    inspectGuidanceDialog("help.recovery_safety", "RecoverySafetyDialog",
        "RecoverySafetyContent", "Cleanup is never automatic",
        {"Transfers", "Delete & Undo", "Local data"},
        "Recovery guidance should explain conservative recovery and safety boundaries accessibly.");
    inspectGuidanceDialog("help.tv_organizer", "TvOrganizerHelpDialog",
        "TvOrganizerHelpContent", "Operation Center", {},
        "Help must expose the actual confirmed TV organizer workflow.");
    bool paletteDetailsInspected = false;
    QTimer::singleShot(0, [&] {
        auto* dialog = qobject_cast<QDialog*>(QApplication::activeModalWidget());
        auto* results = dialog ? dialog->findChild<QListWidget*>("CommandPaletteResults") : nullptr;
        auto* search = dialog ? dialog->findChild<QLineEdit*>("CommandPaletteSearch") : nullptr;
        auto* noResults = dialog ? dialog->findChild<QLabel*>("CommandPaletteNoResults") : nullptr;
        bool quickHasGuidance = false;
        bool selfExplanatoryIsCompact = false;
        if (results) {
            for (int row = 0; row < results->count(); ++row) {
                const auto* item = results->item(row);
                const QString id = item->data(Qt::UserRole).toString();
                if (id == "navigation.goto")
                    quickHasGuidance = !item->data(Qt::UserRole + 1).toString().isEmpty();
                if (id == "view.details")
                    selfExplanatoryIsCompact = item->data(Qt::UserRole + 1).toString().isEmpty()
                        && item->data(Qt::AccessibleDescriptionRole).toString()
                            .startsWith("Shortcut: ");
            }
        }
        bool emptyStateWorks = false;
        if (search && results && noResults) {
            search->setText("no-command-matches-this-term");
            QApplication::processEvents();
            emptyStateWorks = noResults->isVisible() && !results->isVisible();
            search->clear();
            QApplication::processEvents();
            emptyStateWorks = emptyStateWorks && !noResults->isVisible()
                && results->isVisible() && results->count() > 0;
        }
        paletteDetailsInspected = dialog && dialog->objectName() == "CommandPaletteDialog"
            && quickHasGuidance && selfExplanatoryIsCompact && emptyStateWorks;
        if (dialog)
            dialog->reject();
    });
    if (QAction* palette = window.findChild<QAction*>("commands.open"))
        palette->trigger();
    passed &= expect(paletteDetailsInspected,
        "Command palette should show useful guidance without repeated category filler.");
    if (auto* organizer = window.findChild<QAction*>("tools.tv_organizer")) {
        bool previewOpened = false;
        QTimer::singleShot(0, [&] {
            auto* dialog = qobject_cast<QDialog*>(QApplication::activeModalWidget());
            auto* scan = dialog ? dialog->findChild<QPushButton*>("TvOrganizerScan") : nullptr;
            previewOpened = dialog && dialog->objectName() == "TvOrganizerDialog"
                && scan && !scan->isEnabled();
            if (dialog)
                dialog->reject();
        });
        organizer->trigger();
        passed &= expect(previewOpened && organizer->shortcut().isEmpty(),
            "Organizer command should open an explicit-scope window without scanning or claiming a shortcut.");
    } else {
        passed &= expect(false, "The File menu must expose the organizer preview command.");
    }

    auto* shortcutCommand = window.findChild<QAction*>("options.shortcuts");
    passed &= expect(shortcutCommand, "Workspace must expose the keyboard shortcut editor.");
    if (shortcutCommand) {
        bool editorInspected = false;
        bool conflictSeen = false;
        QTimer::singleShot(0, [&] {
            auto* dialog = qobject_cast<QDialog*>(QApplication::activeModalWidget());
            auto* table = dialog ? dialog->findChild<QTableWidget*>("ShortcutEditorTable") : nullptr;
            auto* reset = dialog
                ? dialog->findChild<QPushButton*>("ShortcutEditorResetDefaults") : nullptr;
            auto* buttons = dialog
                ? dialog->findChild<QDialogButtonBox*>("ShortcutEditorButtons") : nullptr;
            bool sorted = table && table->rowCount() > 1;
            if (table) {
                for (int row = 1; row < table->rowCount(); ++row) {
                    sorted = sorted && table->item(row - 1, 0)->text().localeAwareCompare(
                        table->item(row, 0)->text()) <= 0;
                }
            }
            auto* first = table
                ? qobject_cast<QKeySequenceEdit*>(table->cellWidget(0, 2)) : nullptr;
            auto* second = table && table->rowCount() > 1
                ? qobject_cast<QKeySequenceEdit*>(table->cellWidget(1, 2)) : nullptr;
            bool resetWorked = false;
            if (first && reset) {
                first->setKeySequence(QKeySequence("Ctrl+Alt+9"));
                reset->click();
                const auto* action = window.findChild<QAction*>(
                    table->item(0, 0)->data(Qt::UserRole).toString());
                resetWorked = action && first->keySequence() == QKeySequence::fromString(
                    action->property("defaultShortcut").toString(), QKeySequence::PortableText);
            }
            if (first && second && buttons) {
                first->setKeySequence(QKeySequence("Ctrl+Alt+9"));
                second->setKeySequence(QKeySequence("Ctrl+Alt+9"));
                QTimer::singleShot(0, [&] {
                    auto* warning = qobject_cast<QMessageBox*>(QApplication::activeModalWidget());
                    conflictSeen = warning && warning->windowTitle() == "Shortcut conflict";
                    if (warning)
                        warning->accept();
                });
                buttons->button(QDialogButtonBox::Ok)->click();
            }
            editorInspected = dialog && dialog->objectName() == "ShortcutEditorDialog"
                && sorted && resetWorked && conflictSeen && dialog->isVisible();
            if (dialog)
                dialog->reject();
        });
        shortcutCommand->trigger();
        passed &= expect(editorInspected,
            "Shortcut editor must sort commands, reset defaults and reject duplicate bindings.");
    }

    auto* renameCommand = window.findChild<QAction*>("file.batch_rename");
    auto* renameHelp = window.findChild<QAction*>("help.batch_rename");
    passed &= expect(renameCommand && renameHelp, "Help must expose a dedicated Batch Rename guide.");
    if (renameCommand && renameHelp) {
        const QKeySequence originalBinding = renameCommand->shortcut();
        for (const QString& binding : {QString("Ctrl+F2"), QString("Alt+F8"), QString()}) {
            renameCommand->setShortcut(QKeySequence(binding));
            bool guideInspected = false;
            QTimer::singleShot(0, [&] {
                auto* dialog = qobject_cast<QDialog*>(QApplication::activeModalWidget());
                if (!dialog)
                    return;
                auto* tabs = dialog->findChild<QTabWidget*>("BatchRenameHelpTabs");
                auto* quick = dialog->findChild<QLabel*>("BatchRenameHelpQuickSteps");
                auto* names = dialog->findChild<QLabel*>("BatchRenameHelpNames");
                auto* recovery = dialog->findChild<QLabel*>("BatchRenameHelpRecovery");
                const QString current = binding.isEmpty() ? QString("No shortcut assigned")
                    : QKeySequence(binding).toString(QKeySequence::NativeText);
                guideInspected = dialog->objectName() == "BatchRenameHelpDialog" && tabs
                    && tabs->count() == 3 && tabs->currentIndex() == 0
                    && quick && names && recovery
                    && quick->textFormat() == Qt::PlainText
                    && quick->text().contains("Current shortcut: " + current)
                    && quick->text().contains("Rename cannot be cancelled once it starts")
                    && names->text().contains("Ctrl-click")
                    && names->text().contains("Ctrl+Space")
                    && names->text().contains("NUMBERED NAMES")
                    && names->text().contains("NAMES FROM LIST")
                    && names->text().contains("one name per line")
                    && recovery->text().contains("A successful batch can be undone in this session")
                    && recovery->text().contains("Undo refuses occupied original names or changed files")
                    && recovery->text().contains(".file-manager-rename-*.tmp")
                    && recovery->text().contains("recover them manually")
                    && dialog->findChild<QDialogButtonBox*>();
                if (binding == "Ctrl+F2" && application.arguments().contains("--capture-rename-dialogs")) {
                    QApplication::processEvents();
                    const QString directory = QDir(FILE_MANAGER_SOURCE_DIR).filePath("out/rename-ui-review");
                    QDir().mkpath(directory);
                    passed &= expect(dialog->grab().save(QDir(directory).filePath(
                        QString("help-%1.png").arg(qRound(dialog->devicePixelRatioF() * 100)))),
                        "Batch Rename help should render for visual inspection.");
                }
                dialog->reject();
            });
            renameHelp->trigger();
            passed &= expect(guideInspected,
                "Rename help must explain both workflows and limits, and follow changed or removed shortcuts.");
        }
        renameCommand->setShortcut(originalBinding);
    }
    bool startedRenameHelp = false;
    QTimer::singleShot(0, [&] {
        auto* gettingStartedDialog = qobject_cast<QDialog*>(QApplication::activeModalWidget());
        if (!gettingStartedDialog)
            return;
        auto* guide = gettingStartedDialog->findChild<QPushButton*>("GettingStartedRenameGuide");
        if (guide) {
            QTimer::singleShot(0, [&] {
                auto* nested = qobject_cast<QDialog*>(QApplication::activeModalWidget());
                startedRenameHelp = nested && nested->objectName() == "BatchRenameHelpDialog";
                if (nested)
                    nested->reject();
            });
            guide->click();
        }
        gettingStartedDialog->reject();
    });
    if (QAction* gettingStartedCommand = window.findChild<QAction*>("help.getting_started"))
        gettingStartedCommand->trigger();
    passed &= expect(startedRenameHelp, "Getting Started must open the dedicated rename instructions.");

    FilePane* visiblePane = nullptr;
    for (FilePane* candidate : window.findChildren<FilePane*>())
        if (candidate->isVisible()) visiblePane = candidate;
    if (visiblePane) {
        FilePane* pane = visiblePane;
        QLineEdit* folderPath = nullptr;
        for (QLineEdit* edit : pane->findChildren<QLineEdit*>())
            if (edit->accessibleName() == "Folder path") folderPath = edit;
        pane->navigate(fixture.filePath("folder-that-does-not-exist"));
        passed &= expect(folderPath
                && folderPath->accessibleDescription().startsWith("Error:")
                && !folderPath->toolTip().isEmpty(),
            "An invalid path should expose text and assistive status, not color alone.");
        pane->navigate(fixture.filePath("Empty"));
        QEventLoop emptySettle;
        QTimer::singleShot(250, &emptySettle, &QEventLoop::quit);
        emptySettle.exec();
        QLabel* emptyGuidance = pane->findChild<QLabel*>("EmptyFolderGuidance");
        passed &= expect(emptyGuidance && emptyGuidance->isVisible()
                && emptyGuidance->text().contains("folder is empty")
                && emptyGuidance->textFormat() == Qt::PlainText,
            "An empty folder should expose visible, accessible next-step guidance.");

        pane->navigate(fixture.path());
        QLineEdit* filter = nullptr;
        for (QLineEdit* edit : pane->findChildren<QLineEdit*>())
            if (edit->accessibleName() == "Filter current folder") filter = edit;
        if (filter)
            filter->setText("no-file-can-match-this-filter-47f8");
        QApplication::processEvents();
        passed &= expect(filter && emptyGuidance && emptyGuidance->isVisible()
                && emptyGuidance->text().contains("No items match"),
            "A no-match filter should explain how to restore the folder contents.");
    }

    const auto inspectSearchOperations = [&window, &passed](
        const QString& commandId, const QString& resultsObjectName, bool filenameSearch) {
        bool inspected = false;
        QTimer::singleShot(0, [&] {
            auto* dialog = qobject_cast<QDialog*>(QApplication::activeModalWidget());
            if (!dialog)
                return;
            auto* results = dialog->findChild<QListWidget*>(resultsObjectName);
            auto* actions = dialog->findChild<QWidget*>("SearchResultActions");
            auto* query = dialog->findChild<QLineEdit*>(
                filenameSearch ? "FileSearchQuery" : "ContentSearchQuery");
            inspected = results && actions
                && results->selectionMode() == QAbstractItemView::ExtendedSelection
                && actions->accessibleName() == "Search result operations"
                && actions->findChild<QPushButton*>("SearchResultOpen")
                && actions->findChild<QPushButton*>("SearchResultReveal")
                && actions->findChild<QPushButton*>("SearchResultCopy")
                && actions->findChild<QPushButton*>("SearchResultCut")
                && actions->findChild<QPushButton*>("SearchResultShare")
                && actions->findChild<QPushButton*>("SearchResultRecycle")
                && query
                && (filenameSearch ? query->completer() != nullptr
                                   : query->completer() == nullptr);
            if (!filenameSearch) {
                auto* empty = dialog->findChild<QLabel*>("ContentSearchEmpty");
                inspected = inspected && empty && empty->isVisible()
                    && !results->isVisible() && !actions->isVisible();
            }
            if (filenameSearch) {
                auto* aqs = dialog->findChild<QCheckBox*>("FileSearchUseAqs");
                auto* scope = dialog->findChild<QLabel*>("FileSearchScope");
                auto* disclosure = dialog->findChild<QLabel*>("FileSearchAqsDisclosure");
                inspected = inspected
                    && dialog->findChild<QLabel*>("FileSearchHistoryDisclosure")
                    && dialog->findChild<QCompleter*>("FileSearchHistoryCompleter")
                    && dialog->findChild<QComboBox*>("SavedSearches")
                    && dialog->findChild<QPushButton*>("SaveSearch")
                    && dialog->findChild<QPushButton*>("DeleteSavedSearch")
                    && dialog->findChild<QPushButton*>("ClearSavedSearches")
                    && aqs && scope && disclosure
                    && scope->accessibleName() == "Current Windows Search scope"
                    && disclosure->accessibleName()
                        == "Advanced Query Syntax privacy and scope notice";
            }
            dialog->reject();
        });
        if (QAction* command = window.findChild<QAction*>(commandId))
            command->trigger();
        passed &= expect(inspected,
            "Search dialogs should expose the shared accessible result-operation bar.");
    };
    inspectSearchOperations("search.files", "FileSearchResults", true);
    inspectSearchOperations("search.contents", "ContentSearchResults", false);

    for (QLineEdit* edit : window.findChildren<QLineEdit*>())
        passed &= expect(!edit->placeholderText().isEmpty() || !edit->accessibleName().isEmpty(),
            "Every text field needs an accessible purpose.");
    for (QLineEdit* edit : window.findChildren<QLineEdit*>())
        passed &= expect(edit->height() >= 28, "Text fields need a usable logical touch target.");
    for (QPushButton* button : window.findChildren<QPushButton*>())
        passed &= expect(!button->text().trimmed().isEmpty() || !button->accessibleName().isEmpty(),
            "Every push button needs an accessible name.");
    const double textContrast = (luminance(QColor("#e8eaf0")) + 0.05)
        / (luminance(QColor("#101116")) + 0.05);
    passed &= expect(textContrast >= 4.5, "Primary text contrast should meet WCAG AA.");

    std::cerr << "Visual phase: batch rename" << std::endl;
    QTemporaryDir renameRoot;
    const QStringList renamePaths = {renameRoot.filePath("draft-c.pdf"),
        renameRoot.filePath("draft-a.txt"), renameRoot.filePath("draft-b.pdf")};
    for (const auto& path : renamePaths) {
        QFile chapterFixture(path);
        passed &= expect(chapterFixture.open(QIODevice::WriteOnly), "The rename UI fixture should open.");
        chapterFixture.write("unchanged chapter bytes");
    }
    {
        filemanager::ui::BatchRenameDialog renameDialog(renamePaths);
        const auto settlePreview = [&] {
            passed &= expect(filemanager::tests::waitForRenamePreview(renameDialog),
                "The latest asynchronous rename preview should finish.");
        };
        passed &= expect(renameDialog.previewPending() && !renameDialog.renamePlan().isValid(),
            "A pending preview must not be executable.");
        renameDialog.show();
        if (application.arguments().contains("--capture-rename-dialogs")) {
            const QString captureDirectory = QDir(FILE_MANAGER_SOURCE_DIR).filePath("out/rename-ui-review");
            QDir().mkpath(captureDirectory);
            passed &= expect(renameDialog.grab().save(QDir(captureDirectory).filePath(
                    QString("pending-%1.png").arg(qRound(renameDialog.devicePixelRatioF() * 100)))),
                "The preparing-state inspection image should save.");
        }
        QApplication::processEvents();
        settlePreview();
        auto* mode = renameDialog.findChild<QComboBox*>("RenameMode");
        auto* names = renameDialog.findChild<QPlainTextEdit*>("RenameNames");
        auto* preview = renameDialog.findChild<QTableWidget*>("RenamePreview");
        auto* apply = renameDialog.findChild<QPushButton*>("RenameApply");
        auto* down = renameDialog.findChild<QPushButton*>("RenameMoveDown");
        auto* up = renameDialog.findChild<QPushButton*>("RenameMoveUp");
        auto* keepExtensions = renameDialog.findChild<QCheckBox*>("RenameKeepExtensions");
        passed &= expect(mode && names && preview && apply && down && up && keepExtensions,
            "Batch rename must expose numbered/list modes, explicit order and extension controls.");
        if (mode && names && preview && apply && down && up && keepExtensions) {
            passed &= expect(renameDialog.renamePlan().isValid() && preview->rowCount() == 3
                    && preview->item(0, 0)->text() == "draft-c.pdf"
                    && preview->item(0, 1)->text() == "Chapter 001.pdf",
                "Chapter preview must preserve the supplied selection order.");
            down->click();
            settlePreview();
            passed &= expect(preview->item(0, 0)->text() == "draft-a.txt"
                    && preview->item(0, 1)->text() == "Chapter 001.txt"
                    && preview->currentRow() == 1,
                "Moving a row must regenerate numbering and retain focus on the moved file.");
            up->click();
            settlePreview();
            passed &= expect(preview->item(0, 0)->text() == "draft-c.pdf",
                "Moving the selected row up must restore the chosen file order.");
            const auto capture = [&](const QString& name) {
                if (!application.arguments().contains("--capture-rename-dialogs"))
                    return;
                const QString directory = QDir(FILE_MANAGER_SOURCE_DIR).filePath("out/rename-ui-review");
                QDir().mkpath(directory);
                passed &= expect(renameDialog.grab().save(QDir(directory).filePath(
                    name + QString("-%1.png").arg(qRound(renameDialog.devicePixelRatioF() * 100)))),
                    "The rename dialog inspection image should save.");
            };
            capture("numbered");
            mode->setCurrentIndex(1);
            settlePreview();
            passed &= expect(!apply->isEnabled() && preview->rowCount() == 3,
                "An empty name list must disable Apply without hiding selected files.");
            names->setPlainText("Opening\r\nThe Journey\r\nEnding\r\n");
            settlePreview();
            passed &= expect(apply->isEnabled() && preview->item(1, 1)->text() == "The Journey.txt",
                "A pasted CRLF name column should map one line to each file and preserve extensions.");
            QApplication::processEvents();
            capture("list");
            names->setPlainText("Opening\n\nEnding");
            settlePreview();
            passed &= expect(!apply->isEnabled(), "An internal blank name must block the whole batch.");
            names->setPlainText("Opening\nEnding");
            settlePreview();
            passed &= expect(!apply->isEnabled(), "Too few pasted names must block the whole batch.");
            names->setPlainText("One\nTwo\nThree\nFour");
            settlePreview();
            passed &= expect(!apply->isEnabled(), "Extra pasted names must not be silently discarded.");
            names->setPlainText("Same\nMiddle\nSame");
            settlePreview();
            passed &= expect(!apply->isEnabled(), "Duplicate resulting names must block the whole batch.");
            names->setPlainText("Opening.pdf\nJourney.txt\nEnding.pdf");
            keepExtensions->setChecked(false);
            settlePreview();
            passed &= expect(apply->isEnabled() && preview->item(0, 1)->text() == "Opening.pdf",
                "Full filename mode must use the entered extension without appending another.");
            mode->setCurrentIndex(2);
            settlePreview();
            passed &= expect(!apply->isEnabled(), "Original rule mode must remain available and start unchanged.");
            mode->setCurrentIndex(0);
            renameDialog.findChild<QSpinBox*>("RenameStep")->setValue(0);
            settlePreview();
            passed &= expect(!apply->isEnabled(), "Zero numbering step must be refused in the dialog.");
            renameDialog.findChild<QSpinBox*>("RenameStep")->setValue(1);
            settlePreview();
            passed &= expect(apply->isVisible() && preview->isVisible()
                    && renameDialog.rect().contains(apply->mapTo(&renameDialog, apply->rect().bottomRight()))
                    && renameDialog.rect().contains(preview->mapTo(&renameDialog, preview->rect().bottomRight())),
                "Preview and confirmation must fit the dialog at each tested display scale.");
        }
        renameDialog.reject();
        for (const auto& path : renamePaths)
            passed &= expect(QFileInfo::exists(path), "Cancelling preview must leave all original files intact.");
    }
    {
        QTemporaryDir staleDialogRoot;
        const QString stalePath = staleDialogRoot.filePath("chapter.txt");
        QFile staleDialogFixture(stalePath);
        passed &= expect(staleDialogFixture.open(QIODevice::WriteOnly), "Stale dialog fixture should open.");
        staleDialogFixture.write("original");
        staleDialogFixture.close();
        filemanager::ui::BatchRenameDialog staleDialog({stalePath});
        passed &= expect(filemanager::tests::waitForRenamePreview(staleDialog),
            "Initial source identity must be captured before replacement.");
        passed &= expect(QFile::rename(stalePath, staleDialogRoot.filePath("saved.txt"))
                && staleDialogFixture.open(QIODevice::WriteOnly), "Dialog source should be replaced.");
        staleDialogFixture.write("replacement");
        staleDialogFixture.close();
        passed &= expect(!filemanager::files::executeBatchRename(staleDialog.renamePlan()).ok
                && QFileInfo::exists(stalePath),
            "Applying an unchanged preview must reject a replacement source.");
        auto* staleLabel = staleDialog.findChild<QLineEdit*>("RenameLabel");
        auto* staleMode = staleDialog.findChild<QComboBox*>("RenameMode");
        auto* staleApply = staleDialog.findChild<QPushButton*>("RenameApply");
        auto* staleValidation = staleDialog.findChild<QLabel*>("RenameValidation");
        passed &= expect(staleLabel && staleMode && staleApply && staleValidation,
            "Stale preview must expose naming controls and a validation message.");
        if (staleLabel && staleMode && staleApply && staleValidation) {
            staleLabel->setText("Book");
            passed &= expect(filemanager::tests::waitForRenamePreview(staleDialog),
                "The edited stale preview should finish with validation errors.");
            passed &= expect(!staleApply->isEnabled() && !staleDialog.renamePlan().isValid()
                    && staleValidation->text().contains("close and reopen"),
                "Editing a label must not silently adopt a replacement file.");
            staleMode->setCurrentIndex(2);
            staleMode->setCurrentIndex(0);
            passed &= expect(filemanager::tests::waitForRenamePreview(staleDialog),
                "Mode changes should complete without adopting a replacement source.");
            passed &= expect(!staleApply->isEnabled(), "Switching modes must retain the original source identity.");
        }
        filemanager::ui::BatchRenameDialog reviewedDialog({stalePath});
        passed &= expect(filemanager::tests::waitForRenamePreview(reviewedDialog),
            "Reopened preview should finish capturing its new source.");
        passed &= expect(reviewedDialog.renamePlan().isValid(),
            "Reopening the dialog should allow deliberate review of the replacement.");
    }
    {
        filemanager::ui::BatchRenameDialog rapidDialog(renamePaths);
        auto* rapidLabel = rapidDialog.findChild<QLineEdit*>("RenameLabel");
        auto* rapidApply = rapidDialog.findChild<QPushButton*>("RenameApply");
        auto* rapidPreview = rapidDialog.findChild<QTableWidget*>("RenamePreview");
        passed &= expect(rapidLabel && rapidApply && rapidPreview, "Asynchronous preview controls should exist.");
        if (rapidLabel && rapidApply && rapidPreview) {
            passed &= expect(filemanager::tests::waitForRenamePreview(rapidDialog)
                    && rapidDialog.renamePlan().isValid(),
                "The replacement-preview case must begin with a populated, actionable table.");
            rapidLabel->setText("Old");
            rapidLabel->setText("New");
            passed &= expect(!rapidApply->isEnabled() && !rapidDialog.renamePlan().isValid(),
                "Rapid edits must invalidate the previous plan immediately.");
            bool editedDuringRender = false;
            bool actionStayedDisabled = true;
            const auto renderConnection = QObject::connect(rapidPreview, &QTableWidget::itemChanged,
                &rapidDialog, [&] {
                    actionStayedDisabled &= rapidDialog.previewPending() && !rapidApply->isEnabled()
                        && !rapidDialog.renamePlan().isValid();
                    if (!editedDuringRender) {
                        editedDuringRender = true;
                        rapidLabel->setText("Latest");
                    }
                });
            passed &= expect(filemanager::tests::waitForRenamePreview(rapidDialog)
                    && editedDuringRender && rapidDialog.renamePlan().isValid()
                    && QFileInfo(rapidDialog.renamePlan().items.first().destination).fileName() == "Latest 001.pdf",
                "An edit arriving during rendering must discard old results and publish only the latest mapping.");
            QObject::disconnect(renderConnection);
            passed &= expect(actionStayedDisabled && rapidPreview->isEnabled()
                    && rapidPreview->rowCount() == rapidDialog.renamePlan().items.size(),
                "Replacing a populated preview must never make partially updated rows actionable.");
            if (rapidPreview->rowCount() == rapidDialog.renamePlan().items.size()) {
                for (int row = 0; row < rapidPreview->rowCount(); ++row) {
                    const auto& mapping = rapidDialog.renamePlan().items.at(row);
                    const auto* current = rapidPreview->item(row, 0);
                    const auto* proposed = rapidPreview->item(row, 1);
                    passed &= expect(current && proposed
                            && current->text() == QFileInfo(mapping.source).fileName()
                            && current->toolTip() == QFileInfo(mapping.source).absoluteFilePath()
                            && proposed->text() == QFileInfo(mapping.destination).fileName(),
                        "Every ready preview row must agree with the latest executable mapping.");
                }
            }
            rapidLabel->setText("Never applied");
            rapidDialog.accept();
            passed &= expect(rapidDialog.previewPending() && rapidDialog.result() != QDialog::Accepted,
                "Programmatic acceptance must also reject a pending preview.");
            rapidDialog.reject();
            QEventLoop cancelledEvents;
            QTimer::singleShot(150, &cancelledEvents, &QEventLoop::quit);
            cancelledEvents.exec();
            passed &= expect(!rapidDialog.renamePlan().isValid() && rapidDialog.result() == QDialog::Rejected,
                "A closed preview must not become actionable when a queued result arrives.");
        }
        for (const auto& path : renamePaths)
            passed &= expect(QFileInfo::exists(path), "Cancelling pending preparation must leave every source untouched.");
    }
    {
        QTemporaryDir reopenRoot;
        QStringList reopenPaths;
        passed &= expect(reopenRoot.isValid(), "The close/reopen stress fixture should open.");
        for (int index = 0; index < 512 && reopenRoot.isValid(); ++index) {
            const QString path = reopenRoot.filePath(QString("source-%1.txt").arg(index, 4, 10, QLatin1Char('0')));
            QFile reopenFixture(path);
            if (!reopenFixture.open(QIODevice::WriteOnly)
                    || reopenFixture.write(QByteArray::number(index)) < 0) {
                passed &= expect(false, "Every close/reopen stress file should be created.");
                break;
            }
            reopenPaths.append(path);
        }
        for (int cycle = 0; cycle < 8 && reopenPaths.size() == 512; ++cycle) {
            auto* closing = new filemanager::ui::BatchRenameDialog(reopenPaths);
            closing->show();
            QEventLoop startWindow;
            QTimer::singleShot(82, &startWindow, &QEventLoop::quit);
            startWindow.exec();
            closing->reject();
            delete closing;
        }
        filemanager::ui::BatchRenameDialog reopened(reopenPaths);
        reopened.show();
        passed &= expect(filemanager::tests::waitForRenamePreview(reopened, 30'000)
                && reopened.renamePlan().isValid()
                && reopened.renamePlan().items.size() == reopenPaths.size()
                && QFileInfo(reopened.renamePlan().items.constLast().destination).fileName()
                    == "Chapter 512.txt",
            "Repeated close/reopen cycles must not leak stale results or starve a fresh preview.");
        reopened.reject();
        for (const QString& path : reopenPaths)
            passed &= expect(QFileInfo::exists(path),
                "Close/reopen preview stress must not rename a disposable source.");
    }
    bool acceptedRename = false;
    QTimer::singleShot(0, [&] {
        auto* dialog = qobject_cast<QDialog*>(QApplication::activeModalWidget());
        if (!dialog)
            return;
        auto* mode = dialog->findChild<QComboBox*>("RenameMode");
        auto* names = dialog->findChild<QPlainTextEdit*>("RenameNames");
        auto* apply = dialog->findChild<QPushButton*>("RenameApply");
        if (!mode || !names || !apply) {
            dialog->reject();
            return;
        }
        mode->setCurrentIndex(1);
        names->setPlainText("Opening\nThe Journey\nEnding");
        auto* renameDialog = dynamic_cast<filemanager::ui::BatchRenameDialog*>(dialog);
        if (!renameDialog || !filemanager::tests::waitForRenamePreview(*renameDialog)) {
            dialog->reject();
            return;
        }
        acceptedRename = apply->isEnabled();
        if (acceptedRename)
            apply->click();
        else
            dialog->reject();
    });
    const auto renameResult = filemanager::ui::runBatchRenameDialog(&window, renamePaths);
    passed &= expect(acceptedRename && renameResult.ok && renameResult.renamed == 3
            && QFileInfo::exists(renameRoot.filePath("Opening.pdf"))
            && QFileInfo::exists(renameRoot.filePath("The Journey.txt"))
            && QFileInfo::exists(renameRoot.filePath("Ending.pdf")),
        "Explicit Rename confirmation should apply the reviewed name list to the exact selected files.");

    std::cerr << "Visual phase: complete" << std::endl;
    return passed ? 0 : 1;
}
