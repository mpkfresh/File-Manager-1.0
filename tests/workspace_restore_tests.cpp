#include "ui/filepane.h"
#include "ui/localdirectorymodel.h"

#include <QApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QFile>
#include <QSettings>
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

QString normalizedPath(const QString& path)
{
    return QDir::cleanPath(QDir::fromNativeSeparators(path)).toCaseFolded();
}

} // namespace

int main(int argc, char* argv[])
{
    QApplication application(argc, argv);
    application.setOrganizationName("FileManagerTests");
    application.setApplicationName("WorkspaceRestoreTests");
    QTemporaryDir settingsRoot;
    QSettings::setDefaultFormat(QSettings::IniFormat);
    QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, settingsRoot.path());

    bool passed = expect(settingsRoot.isValid(), "The isolated settings root should open.");
    QTemporaryDir root;
    passed &= expect(root.isValid(), "The detailed workspace restore fixture should open.");
    const QString expandedFolder = root.filePath("Expanded");
    QDir().mkdir(expandedFolder);
    QDir().mkdir(QDir(expandedFolder).filePath("Nested"));
    QStringList requestedSelection;
    for (int item = 0; item < 60; ++item) {
        const QString path = root.filePath(
            QStringLiteral("item-%1.txt").arg(item, 3, 10, QLatin1Char('0')));
        QFile fixture(path);
        passed &= expect(fixture.open(QIODevice::WriteOnly),
            "A detailed workspace selection fixture should be created.");
        fixture.close();
        if (item == 7 || item == 41)
            requestedSelection.append(QDir::cleanPath(path));
    }

    FilePane pane;
    auto* paneModel = pane.findChild<filemanager::ui::LocalDirectoryModel*>();
    passed &= expect(paneModel != nullptr, "Restoration fixture should expose its local directory model.");
    pane.resize(420, 220);
    pane.show();
    pane.navigate(root.path());
    QEventLoop populateLoop;
    QTimer::singleShot(500, &populateLoop, &QEventLoop::quit);
    populateLoop.exec();

    FilePaneRestoreState requested;
    requested.selectedPaths = requestedSelection;
    requested.focusedPath = requestedSelection.last();
    requested.expandedPaths = {QDir::cleanPath(expandedFolder)};
    requested.verticalScroll = 12;
    pane.restoreViewState(requested);
    QEventLoop restoreLoop;
    QTimer::singleShot(1000, &restoreLoop, &QEventLoop::quit);
    restoreLoop.exec();

    const FilePaneRestoreState captured = pane.restoreState();
    QStringList selected;
    for (const QString& path : captured.selectedPaths)
        selected.append(normalizedPath(path));
    QStringList expanded;
    for (const QString& path : captured.expandedPaths)
        expanded.append(normalizedPath(path));
    passed &= expect(selected.size() == 2
            && selected.contains(normalizedPath(requestedSelection.first()))
            && selected.contains(normalizedPath(requestedSelection.last())),
        "Detailed workspace restoration should reapply multi-selection.");
    passed &= expect(normalizedPath(captured.focusedPath)
            == normalizedPath(requestedSelection.last()),
        "Detailed workspace restoration should reapply the focused item.");
    passed &= expect(expanded.contains(normalizedPath(expandedFolder)),
        "Detailed workspace restoration should reapply expanded folders.");
    passed &= expect(captured.verticalScroll >= 0,
        "Detailed workspace restoration should keep a valid scroll position.");

    const QString childPath = QDir(expandedFolder).filePath("child.txt");
    const QString nestedPath = QDir(expandedFolder).filePath("Nested/deeper.txt");
    for (const auto& path : {childPath, nestedPath}) {
        QFile nestedFixture(path);
        passed &= expect(nestedFixture.open(QIODevice::WriteOnly), "Nested selection fixtures should open.");
    }
    const QStringList nestedOrder = {nestedPath, requestedSelection.first(), childPath,
        requestedSelection.last()};
    FilePaneRestoreState nestedRequest;
    nestedRequest.selectedPaths = nestedOrder;
    nestedRequest.selectedPaths.append(childPath);
    nestedRequest.expandedPaths = {expandedFolder, QDir(expandedFolder).filePath("Nested")};
    pane.restoreViewState(nestedRequest);
    // Emit more directory notifications than the timed retry budget before
    // processing queued enumerations. These notices must not end restoration.
    // Existing real nested files, selection order and 600ms assertion deadline stay.
    if (paneModel) {
        for (int notification = 0; notification < 50; ++notification)
            emit paneModel->directoryLoaded(root.path());
    }
    QEventLoop nestedRestoreLoop;
    QTimer::singleShot(600, &nestedRestoreLoop, &QEventLoop::quit);
    nestedRestoreLoop.exec();
    const auto nestedSelected = pane.selectedPathsInSelectionOrder();
    std::fprintf(stdout, "Nested restore readiness: expected=%lld selected=%lld child_loaded=%d "
        "nested_loaded=%d root_loading=%d notification_burst=50\n",
        static_cast<long long>(nestedOrder.size()), static_cast<long long>(nestedSelected.size()),
        paneModel && paneModel->indexForPath(childPath).isValid(),
        paneModel && paneModel->indexForPath(nestedPath).isValid(), paneModel && paneModel->loading());
    std::fflush(stdout);
    passed &= expect(nestedSelected == nestedOrder,
        "Batched restoration must preserve sparse order across tree parents without duplicate selections.");
    pane.setViewMode(FilePane::ViewMode::LargeIcons);
    passed &= expect(pane.selectedPathsInSelectionOrder() == nestedOrder,
        "Batched icon selection must not merge rows belonging to different tree parents.");
    pane.setViewMode(FilePane::ViewMode::Details);
    passed &= expect(pane.selectedPathsInSelectionOrder() == nestedOrder,
        "Returning to details must retain selections from expanded subfolders.");

    FilePaneRestoreState unavailableRequest;
    unavailableRequest.selectedPaths = {
        QStringLiteral("\\\\192.0.2.1\\unavailable-test-share\\missing.txt")};
    unavailableRequest.focusedPath = unavailableRequest.selectedPaths.first();
    QElapsedTimer unavailableCall;
    unavailableCall.start();
    pane.restoreViewState(unavailableRequest);
    const qint64 unavailableReturnMs = unavailableCall.elapsed();
    int heartbeats = 0;
    QTimer heartbeat;
    heartbeat.setInterval(5);
    QObject::connect(&heartbeat, &QTimer::timeout, &heartbeat, [&] { ++heartbeats; });
    heartbeat.start();
    QEventLoop unavailableLoop;
    QTimer::singleShot(150, &unavailableLoop, &QEventLoop::quit);
    unavailableLoop.exec();
    heartbeat.stop();
    passed &= expect(unavailableReturnMs < 50 && heartbeats >= 10
            && pane.selectedPaths().isEmpty(),
        "Saved unavailable paths must return immediately, keep the UI responsive and remain unselected.");
    return passed ? 0 : 1;
}
