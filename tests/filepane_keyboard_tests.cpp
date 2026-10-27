#include "ui/filepane.h"
#include "ui/batchrenamedialog.h"
#include "rename_test_helpers.h"

#include <QAbstractItemView>
#include <QApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QFile>
#include <QKeyEvent>
#include <QLineEdit>
#include <QMouseEvent>
#include <QSettings>
#include <QStyle>
#include <QStyleOptionViewItem>
#include <QTemporaryDir>
#include <QTimer>

#include <cstdio>

namespace {

bool expect(bool condition, const char* message)
{
    if (!condition)
        std::fprintf(stderr, "FAIL: %s\n", message);
    return condition;
}

void sendKey(QAbstractItemView* view, int key,
    Qt::KeyboardModifiers modifiers = Qt::NoModifier, const QString& text = {})
{
    QKeyEvent press(QEvent::KeyPress, key, modifiers, text);
    QApplication::sendEvent(view, &press);
    QKeyEvent release(QEvent::KeyRelease, key, modifiers, text);
    QApplication::sendEvent(view, &release);
    QApplication::processEvents();
}

QString normalizedFileName(const QString& path)
{
    return QFileInfo(path).fileName().toCaseFolded();
}

bool waitForRevealedPath(FilePane& pane, const QString& expectedPath)
{
    const QString expected = QDir::cleanPath(expectedPath).toCaseFolded();
    for (int attempt = 0; attempt < 20; ++attempt) {
        if (QDir::cleanPath(pane.selectedPath()).toCaseFolded() == expected
            && pane.view()->currentIndex().isValid())
            return true;
        QEventLoop loop;
        QTimer::singleShot(25, &loop, &QEventLoop::quit);
        loop.exec();
    }
    return false;
}

void sendClick(QAbstractItemView* view, const QModelIndex& index,
    Qt::KeyboardModifiers modifiers = Qt::NoModifier)
{
    const QPoint local = view->visualRect(index).center();
    const QPoint global = view->viewport()->mapToGlobal(local);
    QMouseEvent press(QEvent::MouseButtonPress, QPointF(local), QPointF(global),
        Qt::LeftButton, Qt::LeftButton, modifiers);
    QApplication::sendEvent(view->viewport(), &press);
    QMouseEvent release(QEvent::MouseButtonRelease, QPointF(local), QPointF(global),
        Qt::LeftButton, Qt::NoButton, modifiers);
    QApplication::sendEvent(view->viewport(), &release);
    QApplication::processEvents();
}

void sendCheckClick(QAbstractItemView* view, const QModelIndex& index)
{
    QStyleOptionViewItem option;
    option.initFrom(view);
    option.rect = view->visualRect(index);
    option.features |= QStyleOptionViewItem::HasCheckIndicator;
    option.checkState = static_cast<Qt::CheckState>(
        index.data(Qt::CheckStateRole).toInt());
    const QRect indicator = view->style()->subElementRect(
        QStyle::SE_ItemViewItemCheckIndicator, &option, view);
    const QPoint local = indicator.center();
    const QPoint global = view->viewport()->mapToGlobal(local);
    QMouseEvent press(QEvent::MouseButtonPress, QPointF(local), QPointF(global),
        Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
    QApplication::sendEvent(view->viewport(), &press);
    QMouseEvent release(QEvent::MouseButtonRelease, QPointF(local), QPointF(global),
        Qt::LeftButton, Qt::NoButton, Qt::NoModifier);
    QApplication::sendEvent(view->viewport(), &release);
    QApplication::processEvents();
}

bool checkLargeSelection(int count)
{
    QTemporaryDir largeRoot;
    if (!expect(largeRoot.isValid(), "The large-selection fixture directory should open."))
        return false;
    QStringList paths;
    for (int row = 0; row < count; ++row) {
        const QString path = largeRoot.filePath(QString("chapter-%1.txt").arg(row, 6, 10, QChar('0')));
        QFile item(path);
        if (!expect(item.open(QIODevice::WriteOnly), "Each large-selection fixture should open."))
            return false;
        paths.append(path);
    }
    FilePane largePane;
    largePane.resize(640, 400);
    largePane.show();
    largePane.navigate(largeRoot.path());
    QElapsedTimer populateTimer;
    populateTimer.start();
    while (largePane.visibleItemCount() != count && populateTimer.elapsed() < 15000) {
        QEventLoop events;
        QTimer::singleShot(20, &events, &QEventLoop::quit);
        events.exec();
    }
    if (!expect(largePane.visibleItemCount() == count, "All large-selection rows should load."))
        return false;
    auto* largeView = largePane.view();
    const auto atRow = [largeView](int row) {
        return largeView->model()->index(row, 0, largeView->rootIndex());
    };
    largeView->selectionModel()->select(atRow(count - 1), QItemSelectionModel::ClearAndSelect | QItemSelectionModel::Rows);
    largeView->selectionModel()->select(atRow(0), QItemSelectionModel::Select | QItemSelectionModel::Rows);
    QStringList expected = {paths.last(), paths.first()};
    expected.append(paths.mid(1, count - 2));
    QElapsedTimer timer;
    timer.start();
    largeView->selectAll();
    const qint64 selectionMs = timer.elapsed();
    timer.restart();
    bool passed = expect(largePane.selectedPathsInSelectionOrder() == expected,
        "Select all must append row order after individually selected files, without duplicates.");
    const qint64 orderMs = timer.elapsed();
    int selectionNotifications = 0;
    const auto selectionConnection = QObject::connect(&largePane, &FilePane::selectionChanged, &largePane,
        [&selectionNotifications] { ++selectionNotifications; });
    timer.restart();
    largePane.setViewMode(FilePane::ViewMode::LargeIcons);
    const qint64 iconsMs = timer.elapsed();
    passed &= expect(largePane.selectedPathsInSelectionOrder() == expected,
        "Large selection order must survive switching to icons.");
    const int iconNotifications = selectionNotifications;
    selectionNotifications = 0;
    timer.restart();
    largePane.setViewMode(FilePane::ViewMode::Details);
    const qint64 detailsMs = timer.elapsed();
    passed &= expect(largePane.selectedPathsInSelectionOrder() == expected,
        "Large selection order must survive returning to details.");
    passed &= expect(iconNotifications <= 1 && selectionNotifications <= 1,
        "View switching must restore selection in one batch, not notify once per file.");
    const int detailsNotifications = selectionNotifications;
    for (const auto mode : {FilePane::ViewMode::SmallIcons, FilePane::ViewMode::List,
            FilePane::ViewMode::MediumIcons, FilePane::ViewMode::ExtraLargeIcons,
            FilePane::ViewMode::Details}) {
        selectionNotifications = 0;
        largePane.setViewMode(mode);
        passed &= expect(largePane.selectedPathsInSelectionOrder() == expected
                && selectionNotifications <= 1,
            "Every filesystem display mode must retain order with at most one selection update.");
    }
    selectionNotifications = 0;
    timer.restart();
    largePane.invertSelection();
    passed &= expect(largePane.selectedPathsInSelectionOrder().isEmpty(),
        "Inverting all selected files must clear the ordered selection.");
    largePane.invertSelection();
    const qint64 invertMs = timer.elapsed();
    passed &= expect(largePane.selectedPathsInSelectionOrder() == paths && selectionNotifications == 2,
        "Inverting twice must use one notification per action and restore selection in row order.");
    QElapsedTimer heartbeatClock;
    heartbeatClock.start();
    qint64 maximumEventGap = 0;
    QTimer heartbeat;
    QObject::connect(&heartbeat, &QTimer::timeout, &heartbeat, [&] {
        maximumEventGap = qMax(maximumEventGap, heartbeatClock.restart());
    });
    heartbeat.start(5);
    FilePaneRestoreState savedState;
    savedState.selectedPaths = expected.mid(0, qMin(256, expected.size()));
    savedState.focusedPath = savedState.selectedPaths.constLast();
    largePane.clearSelection();
    timer.restart();
    largePane.restoreViewState(savedState);
    const qint64 restoreCallMs = timer.elapsed();
    while (largePane.selectedPathsInSelectionOrder().size() != savedState.selectedPaths.size()
            && timer.elapsed() < 5000) {
        QEventLoop events;
        QTimer::singleShot(5, &events, &QEventLoop::quit);
        events.exec();
    }
    const qint64 restoreReadyMs = timer.elapsed();
    passed &= expect(largePane.selectedPathsInSelectionOrder() == savedState.selectedPaths,
        "Saved-path restoration must preserve its bounded order from already loaded model rows.");
    timer.restart();
    filemanager::ui::BatchRenameDialog preview(expected);
    const qint64 previewMs = timer.elapsed();
    preview.show();
    passed &= expect(preview.previewPending() && !preview.renamePlan().isValid(),
        "Preview construction must return before background validation becomes actionable.");
    passed &= expect(filemanager::tests::waitForRenamePreview(preview), "Background preview should finish.");
    const qint64 previewReadyMs = timer.elapsed();
    auto* label = preview.findChild<QLineEdit*>("RenameLabel");
    timer.restart();
    if (label)
        label->setText(label->text() == "Book" ? "Volume" : "Book");
    const qint64 editMs = timer.elapsed();
    passed &= expect(!preview.renamePlan().isValid(), "Editing must immediately invalidate the previous plan.");
    passed &= expect(filemanager::tests::waitForRenamePreview(preview), "Updated background preview should finish.");
    const qint64 editReadyMs = timer.elapsed();
    heartbeat.stop();
    passed &= expect(label && preview.renamePlan().isValid()
            && preview.renamePlan().items.size() == count
            && preview.renamePlan().items.first().source == expected.first(),
        "A large rename preview must retain the ordered mapping after editing its label.");
    std::printf("selection_entries=%d select_all_ms=%lld order_ms=%lld icons_ms=%lld details_ms=%lld "
        "icons_notifications=%d details_notifications=%d invert_twice_ms=%lld preview_open_ms=%lld "
        "restore_call_ms=%lld restore_ready_ms=%lld preview_ready_ms=%lld label_edit_ms=%lld "
        "label_ready_ms=%lld max_event_gap_ms=%lld\n",
        count, static_cast<long long>(selectionMs), static_cast<long long>(orderMs),
        static_cast<long long>(iconsMs), static_cast<long long>(detailsMs), iconNotifications,
        detailsNotifications, static_cast<long long>(invertMs), static_cast<long long>(previewMs),
        static_cast<long long>(restoreCallMs), static_cast<long long>(restoreReadyMs),
        static_cast<long long>(previewReadyMs), static_cast<long long>(editMs),
        static_cast<long long>(editReadyMs), static_cast<long long>(maximumEventGap));
    QObject::disconnect(selectionConnection);
    return passed;
}

} // namespace

int main(int argc, char* argv[])
{
    QApplication application(argc, argv);
    application.setOrganizationName("FileManagerTests");
    application.setApplicationName("FilePaneKeyboardTests");
    QTemporaryDir settingsRoot;
    QSettings::setDefaultFormat(QSettings::IniFormat);
    QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, settingsRoot.path());

    if (application.arguments().contains("--benchmark-selection"))
        return checkLargeSelection(1000) ? 0 : 1;
    if (application.arguments().contains("--benchmark-selection-large"))
        return checkLargeSelection(10000) ? 0 : 1;

    bool passed = expect(settingsRoot.isValid(), "The isolated settings root should open.");
    QTemporaryDir root;
    passed &= expect(root.isValid(), "The keyboard-selection fixture should open.");
    const QStringList names = {"alpha.txt", "beta.txt", "bravo.txt"};
    for (const QString& name : names) {
        QFile fixture(root.filePath(name));
        passed &= expect(fixture.open(QIODevice::WriteOnly),
            "A keyboard-selection fixture file should be created.");
        fixture.close();
    }

    FilePane pane;
    pane.resize(520, 320);
    pane.show();
    pane.navigate(root.path());
    QElapsedTimer populateDeadline;
    populateDeadline.start();
    while (pane.visibleItemCount() != names.size() && populateDeadline.elapsed() < 5000) {
        QEventLoop populateLoop;
        QTimer::singleShot(20, &populateLoop, &QEventLoop::quit);
        populateLoop.exec();
    }
    passed &= expect(pane.visibleItemCount() == names.size(),
        "Keyboard checks require the complete fixture listing.");
    QAbstractItemView* view = pane.view();
    view->setFocus();

    pane.revealPath(root.filePath("alpha.txt"));
    passed &= expect(waitForRevealedPath(pane, root.filePath("alpha.txt")),
        "The starting keyboard selection must finish revealing before input.");
    sendKey(view, Qt::Key_B, Qt::NoModifier, "b");
    passed &= expect(normalizedFileName(pane.selectedPath()) == QStringLiteral("beta.txt"),
        "Typing a filename prefix should select the first matching item.");
    sendKey(view, Qt::Key_R, Qt::NoModifier, "r");
    passed &= expect(normalizedFileName(pane.selectedPath()) == QStringLiteral("bravo.txt"),
        "Rapid typing should incrementally refine type-to-select.");

    sendKey(view, Qt::Key_A, Qt::ControlModifier);
    passed &= expect(pane.selectedPaths().size() == names.size(),
        "Ctrl+A should select every visible item.");
    sendKey(view, Qt::Key_Space, Qt::ControlModifier, " ");
    passed &= expect(pane.selectedPaths().size() == names.size() - 1,
        "Ctrl+Space should toggle the focused item without clearing other selections.");
    sendKey(view, Qt::Key_Space, Qt::ControlModifier, " ");
    passed &= expect(pane.selectedPaths().size() == names.size(),
        "Ctrl+Space should restore the focused item to the selection.");

    sendKey(view, Qt::Key_Home);
    passed &= expect(normalizedFileName(pane.selectedPath()) == QStringLiteral("alpha.txt")
            && pane.selectedPaths().size() == 1,
        "Home should move focus and selection to the first item.");
    sendKey(view, Qt::Key_End, Qt::ShiftModifier);
    passed &= expect(normalizedFileName(pane.selectedPath()) == QStringLiteral("bravo.txt")
            && pane.selectedPaths().size() == names.size(),
        "Shift+End should extend selection through the last item.");
    sendKey(view, Qt::Key_Home, Qt::ControlModifier);
    passed &= expect(normalizedFileName(pane.selectedPath()) == QStringLiteral("alpha.txt")
            && pane.selectedPaths().size() == names.size(),
        "Ctrl+Home should move focus without discarding the extended selection.");

    pane.clearSelection();
    passed &= expect(pane.selectedPaths().isEmpty(),
        "The shared Select none action should clear selection.");
    pane.invertSelection();
    passed &= expect(pane.selectedPaths().size() == names.size(),
        "The shared Invert selection action should select every previously unselected item.");

    const auto selectionRow = [view](int row) {
        return view->model()->index(row, 0, view->rootIndex());
    };
    pane.setSelectionCheckBoxes(true);
    pane.clearSelection();
    view->selectionModel()->setCurrentIndex(selectionRow(0), QItemSelectionModel::NoUpdate);
    passed &= expect(selectionRow(0).flags().testFlag(Qt::ItemIsUserCheckable)
            && selectionRow(0).data(Qt::CheckStateRole).toInt() == Qt::Unchecked,
        "The optional selection control should expose an unchecked, accessible model state.");
    sendCheckClick(view, selectionRow(0));
    passed &= expect(pane.selectedPaths().size() == 1
            && selectionRow(0).data(Qt::CheckStateRole).toInt() == Qt::Checked,
        "Clicking the visible check indicator should select its item.");
    sendCheckClick(view, selectionRow(0));
    passed &= expect(pane.selectedPaths().isEmpty()
            && selectionRow(0).data(Qt::CheckStateRole).toInt() == Qt::Unchecked,
        "Clicking a checked indicator should remove only that item from the selection.");
    sendKey(view, Qt::Key_Space);
    passed &= expect(pane.selectedPaths().size() == 1
            && selectionRow(0).data(Qt::CheckStateRole).toInt() == Qt::Checked,
        "Space should check the focused item and add it to the existing selection model.");
    sendKey(view, Qt::Key_Space);
    passed &= expect(pane.selectedPaths().isEmpty()
            && selectionRow(0).data(Qt::CheckStateRole).toInt() == Qt::Unchecked,
        "Space should uncheck the focused item without changing file contents.");
    passed &= expect(view->model()->setData(selectionRow(1), Qt::Checked, Qt::CheckStateRole)
            && pane.selectedPaths().size() == 1
            && selectionRow(1).data(Qt::CheckStateRole).toInt() == Qt::Checked,
        "The check-state accessibility action should drive the same selection model.");
    pane.setSelectionCheckBoxes(false);
    passed &= expect(!selectionRow(0).flags().testFlag(Qt::ItemIsUserCheckable)
            && !selectionRow(0).data(Qt::CheckStateRole).isValid(),
        "Disabling item check boxes should remove only the check control, not selection support.");
    pane.clearSelection();

    pane.setViewMode(FilePane::ViewMode::LargeIcons);
    view = pane.view();
    view->setFocus();
    pane.clearSelection();
    sendKey(view, Qt::Key_B, Qt::NoModifier, "b");
    passed &= expect(normalizedFileName(pane.selectedPath()) == QStringLiteral("beta.txt"),
        "Type-to-select should behave identically in icon views.");
    sendKey(view, Qt::Key_A, Qt::ControlModifier);
    passed &= expect(pane.selectedPaths().size() == names.size(),
        "Ctrl+A should select every visible item in icon views.");
    sendKey(view, Qt::Key_Space, Qt::ControlModifier, " ");
    passed &= expect(pane.selectedPaths().size() == names.size() - 1,
        "Ctrl+Space should toggle only the focused item in icon views.");

    pane.setViewMode(FilePane::ViewMode::Details);
    view = pane.view();
    pane.clearSelection();
    const auto rowIndex = [&](int row) { return view->model()->index(row, 0, view->rootIndex()); };
    sendClick(view, rowIndex(2));
    sendClick(view, rowIndex(0), Qt::ControlModifier);
    sendClick(view, rowIndex(1), Qt::ControlModifier);
    const QStringList clickedOrder = {root.filePath("bravo.txt"), root.filePath("alpha.txt"),
        root.filePath("beta.txt")};
    passed &= expect(pane.selectedPathsInSelectionOrder() == clickedOrder,
        "Ctrl-click order should be preserved for chapter numbering, not replaced by row order.");
    sendClick(view, rowIndex(0), Qt::ControlModifier);
    sendClick(view, rowIndex(0), Qt::ControlModifier);
    const QStringList reselectedOrder = {root.filePath("bravo.txt"), root.filePath("beta.txt"),
        root.filePath("alpha.txt")};
    passed &= expect(pane.selectedPathsInSelectionOrder() == reselectedOrder,
        "Deselecting and reselecting a file should put it at the end of the chosen order.");
    pane.setViewMode(FilePane::ViewMode::LargeIcons);
    passed &= expect(pane.selectedPathsInSelectionOrder() == reselectedOrder,
        "Switching views should preserve the explicit rename selection order.");
    pane.clearSelection();
    passed &= expect(pane.selectedPathsInSelectionOrder().isEmpty(),
        "Clearing the selection must discard its rename order.");

    const QString activationFolder = root.filePath("activate-folder");
    QDir().mkdir(activationFolder);
    pane.setViewMode(FilePane::ViewMode::Details);
    pane.navigate(root.path());
    QEventLoop activationPopulateLoop;
    QTimer::singleShot(300, &activationPopulateLoop, &QEventLoop::quit);
    activationPopulateLoop.exec();
    pane.revealPath(activationFolder);
    passed &= expect(waitForRevealedPath(pane, activationFolder),
        "The default-activation fixture should reveal its target item.");
    QModelIndex activationIndex = pane.view()->currentIndex();
    QMetaObject::invokeMethod(pane.view(), "clicked", Qt::DirectConnection,
        Q_ARG(QModelIndex, activationIndex));
    passed &= expect(QDir::cleanPath(pane.currentPath()) == QDir::cleanPath(root.path()),
        "Double-click activation should remain the default.");
    QMetaObject::invokeMethod(pane.view(), "doubleClicked", Qt::DirectConnection,
        Q_ARG(QModelIndex, activationIndex));
    passed &= expect(QDir::cleanPath(pane.currentPath()) == QDir::cleanPath(activationFolder),
        "Double-click should open an item while the default mode is active.");
    pane.navigate(root.path());
    QEventLoop singleClickPopulateLoop;
    QTimer::singleShot(300, &singleClickPopulateLoop, &QEventLoop::quit);
    singleClickPopulateLoop.exec();
    pane.revealPath(activationFolder);
    passed &= expect(waitForRevealedPath(pane, activationFolder),
        "The single-click fixture should reveal its target item.");
    activationIndex = pane.view()->currentIndex();
    pane.setSingleClickActivation(true);
    pane.setSelectionCheckBoxes(true);
    sendCheckClick(pane.view(), activationIndex);
    passed &= expect(QDir::cleanPath(pane.currentPath()) == QDir::cleanPath(root.path()),
        "A selection-check click must not open an item in single-click activation mode.");
    pane.clearSelection();
    sendClick(pane.view(), activationIndex, Qt::ControlModifier);
    passed &= expect(QDir::cleanPath(pane.currentPath()) == QDir::cleanPath(root.path()),
        "Modifier-assisted clicks should remain selection-only in single-click mode.");
    sendClick(pane.view(), activationIndex);
    passed &= expect(QDir::cleanPath(pane.currentPath()) == QDir::cleanPath(activationFolder),
        "The explicit single-click preference should open an item with one click.");
    passed &= checkLargeSelection(256);
    return passed ? 0 : 1;
}
