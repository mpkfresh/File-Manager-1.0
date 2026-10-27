#include "epub_test_fixture.h"
#include "ui/tvorganizerdialog.h"
#include "files/transferqueue.h"

#include <QAbstractButton>
#include <QApplication>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QDir>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QFile>
#include <QKeyEvent>
#include <QItemSelectionModel>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QPointer>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QSpinBox>
#include <QTableView>
#include <QTemporaryDir>
#include <QTimer>
#include <functional>
#include <iostream>

namespace {

bool waitUntil(const std::function<bool()>& ready, int timeout = 15'000)
{
    QElapsedTimer elapsed;
    elapsed.start();
    while (!ready() && elapsed.elapsed() < timeout) {
        QEventLoop events;
        QTimer::singleShot(5, &events, &QEventLoop::quit);
        events.exec();
    }
    return ready();
}

} // namespace

bool runTvOrganizerUiTests(const QString& captureDirectory)
{
    using filemanager::ui::TvOrganizerDialog;
    bool passed = true;
    const auto check = [&](bool condition, const char* message) {
        if (!condition) {
            std::cerr << "FAIL: TV organizer UI: " << message << '\n';
            passed = false;
        }
    };
    QTemporaryDir fixture;
    check(fixture.isValid(), "disposable fixture directory");
    if (!fixture.isValid())
        return false;
    const QString input = fixture.filePath("Episodes");
    const QString destination = fixture.filePath("Shows");
    check(QDir().mkpath(input) && QDir().mkpath(destination), "create source and destination fixtures");
    for (const QString& name : QStringList{"Series.S01E02.mkv", "Series.S01E02.en.srt",
            "Other.S02E03.mp4", "Uncertain.mp4"}) {
        QFile file(QDir(input).filePath(name));
        const bool opened = file.open(QIODevice::WriteOnly);
        check(opened && file.write("fixture") == 7, "write media-name fixtures");
    }
    TvOrganizerDialog dialog(input);
    auto* scan = dialog.findChild<QPushButton*>("TvOrganizerScan");
    auto* cancel = dialog.findChild<QPushButton*>("TvOrganizerCancel");
    auto* table = dialog.findChild<QTableView*>("TvOrganizerReview");
    auto* status = dialog.findChild<QLabel*>("TvOrganizerStatus");
    auto* details = dialog.findChild<QPlainTextEdit*>("TvOrganizerSelectedMapping");
    auto* limit = dialog.findChild<QSpinBox*>("TvOrganizerLimit");
    auto* exclude = dialog.findChild<QPushButton*>("TvOrganizerExcludeAll");
    auto* include = dialog.findChild<QPushButton*>("TvOrganizerIncludeAll");
    auto* move = dialog.findChild<QPushButton*>("TvOrganizerMove");
    check(scan && cancel && table && status && details && limit && exclude && include && move,
        "named accessible controls");
    if (!scan || !cancel || !table || !status || !details || !limit || !exclude || !include || !move)
        return false;
    check(!dialog.scanPending() && !scan->isEnabled() && !dialog.reviewedPlan().complete()
            && dialog.reviewedPlan().entriesVisited == 0,
        "opening does not implicitly scan current folder");
    dialog.setScope({input}, destination);
    check(scan->isEnabled() && table->model()->rowCount() == 0, "scope selection still requires explicit scan");
    dialog.show();
    QApplication::processEvents();
    const auto capture = [&](const QString& state) {
        if (captureDirectory.isEmpty())
            return;
        check(QDir().mkpath(captureDirectory), "create UI capture directory");
        const int scale = qRound(dialog.devicePixelRatioF() * 100.0);
        check(dialog.grab().save(QDir(captureDirectory).filePath(QString("%1-%2.png").arg(state).arg(scale))),
            "save organizer UI capture");
    };
    capture("idle");
    scan->click();
    check(dialog.scanPending() && !scan->isEnabled() && cancel->isEnabled()
            && dialog.selectedCandidates().isEmpty(), "scan starts asynchronously with no actionable selection");
    dialog.accept();
    check(dialog.isVisible(), "programmatic acceptance cannot apply a pending plan");
    capture("pending");
    check(waitUntil([&] { return !dialog.scanPending(); }), "background scan completes");
    check(dialog.reviewedPlan().complete() && dialog.reviewedPlan().candidates.size() == 3
            && dialog.reviewedPlan().review.size() == 1 && table->model()->rowCount() == 4,
        "review exposes proposed paths and uncertain files");
    check(dialog.selectedCandidates().size() == 3 && status->text().contains("No transfer queue")
            && !move->isEnabled(),
        "standalone review cannot bypass the application transfer queue");
    capture("ready");
    int episodeRow = -1;
    int companionRow = -1;
    int reviewRow = -1;
    for (int row = 0; row < table->model()->rowCount(); ++row) {
        const QString source = table->model()->index(row, 0).data().toString();
        if (source.endsWith("Series.S01E02.mkv")) episodeRow = row;
        if (source.endsWith("Series.S01E02.en.srt")) companionRow = row;
        if (source.endsWith("Uncertain.mp4")) reviewRow = row;
    }
    check(episodeRow >= 0 && companionRow >= 0 && reviewRow >= 0, "find review rows by source");
    if (episodeRow >= 0 && companionRow >= 0 && reviewRow >= 0) {
        table->setCurrentIndex(table->model()->index(episodeRow, 0));
        check(details->isReadOnly() && details->toPlainText().contains(
                QDir::toNativeSeparators(QDir(input).filePath("Series.S01E02.mkv")))
                && details->toPlainText().contains(QDir::toNativeSeparators(
                    QDir(destination).filePath("Series/Season 01/Series.S01E02.mkv"))),
            "selected row exposes complete, selectable source and destination paths");
        table->setFocus();
        QKeyEvent space(QEvent::KeyPress, Qt::Key_Space, Qt::NoModifier);
        QApplication::sendEvent(table, &space);
        check(dialog.selectedCandidates().size() == 1
                && table->model()->index(companionRow, 0).data(Qt::CheckStateRole).toInt() == Qt::Unchecked,
            "Space excludes the entire episode/companion group");
        check(!table->model()->setData(table->model()->index(reviewRow, 0), Qt::Checked, Qt::CheckStateRole),
            "uncertain rows cannot be checked into a candidate group");
        check(table->model()->setData(table->model()->index(companionRow, 0), Qt::Checked, Qt::CheckStateRole)
                && dialog.selectedCandidates().size() == 3, "checking a companion includes its episode group");
    }
    exclude->click();
    check(dialog.selectedCandidates().isEmpty(), "exclude all removes every group from selection");
    include->click();
    check(dialog.selectedCandidates().size() == 3, "include all restores only eligible groups");
    dialog.accept();
    check(dialog.isVisible(), "a standalone review cannot accept without an attached transfer queue");
    limit->setValue(1);
    check(table->model()->rowCount() == 0 && dialog.selectedCandidates().isEmpty() && details->toPlainText().isEmpty(),
        "changing scan limits invalidates the previous review");
    scan->click();
    check(waitUntil([&] { return !dialog.scanPending(); }) && dialog.reviewedPlan().limitReached
            && dialog.selectedCandidates().isEmpty() && status->text().contains("Entry limit reached"),
        "truncated scan is clearly reported with no proposals");
    capture("limited");
    limit->setValue(100'000);
    scan->click();
    cancel->click();
    check(dialog.selectedCandidates().isEmpty() && !cancel->isEnabled(), "Stop clears pending review immediately");
    check(waitUntil([&] { return !dialog.scanPending(); }) && table->model()->rowCount() == 0
            && status->text().contains("cancelled"), "late scan result cannot replace cancelled review");
    scan->click();
    dialog.findChild<QLineEdit*>("TvOrganizerDestination")->setText(fixture.filePath("missing"));
    check(waitUntil([&] { return !dialog.scanPending(); }) && dialog.selectedCandidates().isEmpty(),
        "programmatic destination changes invalidate an in-flight scan");
    dialog.setScope({input}, fixture.filePath("missing"));
    scan->click();
    check(waitUntil([&] { return !dialog.scanPending(); }) && !dialog.reviewedPlan().errors.isEmpty()
            && table->model()->rowCount() > 0 && dialog.selectedCandidates().isEmpty(),
        "scan errors are visible and cannot yield selected moves");
    dialog.reject();
    check(dialog.selectedCandidates().isEmpty(), "closing discards retained review");

    QPointer<TvOrganizerDialog> closing = new TvOrganizerDialog(input);
    closing->setScope({input}, destination);
    closing->findChild<QPushButton*>("TvOrganizerScan")->click();
    check(closing->scanPending(), "launch a scan immediately before destruction");
    delete closing.data();
    check(closing.isNull(), "destruction does not wait for a worker");
    QEventLoop drain;
    QTimer::singleShot(150, &drain, &QEventLoop::quit);
    drain.exec();
    bool helpSeen = false;
    QTimer::singleShot(0, [&] {
        auto* help = qobject_cast<QDialog*>(QApplication::activeModalWidget());
        auto* text = help ? help->findChild<QLabel*>("TvOrganizerHelpContent") : nullptr;
        helpSeen = text && text->text().contains("Move selected") && text->text().contains("Space")
            && text->text().contains("no default keyboard shortcut")
            && text->text().contains("Operation Center") && text->text().contains("not one atomic undo");
        if (help) {
            if (!captureDirectory.isEmpty()) {
                check(help->grab().save(QDir(captureDirectory).filePath(
                    QString("help-%1.png").arg(qRound(help->devicePixelRatioF() * 100.0)))), "save Help capture");
            }
            help->reject();
        }
    });
    filemanager::ui::showTvOrganizerHelp(nullptr);
    check(helpSeen, "Help documents confirmation, progress, shortcut and partial-result limits");
    QFile original(QDir(input).filePath("Series.S01E02.mkv"));
    check(original.open(QIODevice::ReadOnly) && original.readAll() == "fixture"
            && QDir(destination).entryList(QDir::AllEntries | QDir::NoDotAndDotDot).isEmpty(),
        "scan, review, Help and cancelled acceptance leave files unchanged");
    original.close();

    const QString bookInput = fixture.filePath("BookInput");
    const QString bookDestination = fixture.filePath("Books");
    check(QDir().mkpath(bookInput) && QDir().mkpath(bookDestination),
        "create book scan and destination folders");
    for (const QString& name : QStringList{"Saga - 01 - Arrival.epub",
            "Saga - 01 - Arrival.pdf", "Saga - 01 - Arrival.cover.jpg",
            "Unknown.epub", "Unknown.pdf", "Unknown.m4b"}) {
        QFile file(QDir(bookInput).filePath(name));
        check(file.open(QIODevice::WriteOnly) && file.write("fixture") == 7,
            "write book-name fixtures");
    }
    // EPUB safety rejects a text file with an .epub suffix. Use a readable
    // package without a series claim so the clear filename may be reviewed.
    const QByteArray containerXml = R"(<?xml version="1.0"?>
<container xmlns="urn:oasis:names:tc:opendocument:xmlns:container" version="1.0">
 <rootfiles><rootfile full-path="EPUB/package.opf" media-type="application/oebps-package+xml"/></rootfiles>
</container>)";
    const QByteArray packageXml = R"(<package xmlns="http://www.idpf.org/2007/opf" version="3.0">
<metadata xmlns:dc="http://purl.org/dc/elements/1.1/"><dc:title>Arrival</dc:title></metadata>
</package>)";
    check(bookTestFixtures::writeEpub(QDir(bookInput).filePath("Saga - 01 - Arrival.epub"),
              containerXml, packageXml),
        "write readable book UI EPUB fixture");
    TvOrganizerDialog bookDialog(bookInput);
    auto* kind = bookDialog.findChild<QComboBox*>("MediaOrganizerKind");
    check(kind && kind->count() == 2, "book-series mode is available in Media Organizer");
    if (kind) {
        kind->setCurrentIndex(1);
        bookDialog.setScope({bookInput}, bookDestination);
        auto* bookScan = bookDialog.findChild<QPushButton*>("TvOrganizerScan");
        check(bookScan && bookScan->isEnabled(), "book scan still requires explicit chosen scope");
        if (bookScan) {
            bookScan->click();
            check(waitUntil([&] { return !bookDialog.scanPending(); })
                    && bookDialog.reviewedPlan().complete()
                    && bookDialog.selectedCandidates().size() == 3
                    && bookDialog.reviewedPlan().review.size() == 3,
                "book preview groups formats and excludes three uncertain files");
            check(!bookDialog.findChild<QPushButton*>("TvOrganizerMove")->isEnabled()
                    && QDir(bookDestination).entryList(QDir::AllEntries | QDir::NoDotAndDotDot).isEmpty(),
                "standalone book preview cannot move files");
            auto* bookTable = bookDialog.findChild<QTableView*>("TvOrganizerReview");
            auto* assign = bookDialog.findChild<QPushButton*>("MediaOrganizerAssignBook");
            QList<int> unclearRows;
            if (bookTable) {
                for (int row = 0; row < bookTable->model()->rowCount(); ++row) {
                    const QString source = bookTable->model()->index(row, 0).data().toString();
                    if (source.endsWith("Unknown.epub") || source.endsWith("Unknown.pdf")
                            || source.endsWith("Unknown.m4b"))
                        unclearRows.append(row);
                }
            }
            check(bookTable && assign && unclearRows.size() == 3,
                "three related unclear files offer one manual book assignment");
            if (bookTable && assign && unclearRows.size() == 3) {
                bookTable->clearSelection();
                for (int row : unclearRows) {
                    bookTable->selectionModel()->select(bookTable->model()->index(row, 0),
                        QItemSelectionModel::Select | QItemSelectionModel::Rows);
                }
                check(bookTable->selectionModel()->selectedRows().size() == 3,
                    "one-review correction contains only the three explicitly selected files");
                bool correctionSeen = false;
                QTimer::singleShot(0, [&] {
                    auto* correction = qobject_cast<QDialog*>(QApplication::activeModalWidget());
                    auto* seriesField = correction
                        ? correction->findChild<QLineEdit*>("MediaOrganizerCorrectionSeries") : nullptr;
                    auto* numberField = correction
                        ? correction->findChild<QSpinBox*>("MediaOrganizerCorrectionNumber") : nullptr;
                    auto* titleField = correction
                        ? correction->findChild<QLineEdit*>("MediaOrganizerCorrectionTitle") : nullptr;
                    correctionSeen = seriesField && numberField && titleField;
                    if (correctionSeen) {
                        seriesField->setText("Saga");
                        numberField->setValue(2);
                        titleField->setText("Next Chapter");
                        correction->accept();
                    }
                });
                check(assign->isEnabled(), "assigning a reviewed book requires retained file identity");
                assign->click();
                const bool assignmentFinished = waitUntil([&] { return !bookDialog.scanPending(); });
                const auto& assignedPlan = bookDialog.reviewedPlan();
                const bool assignmentReady = assignmentFinished && correctionSeen
                    && assignedPlan.complete() && bookDialog.selectedCandidates().size() == 6
                    && assignedPlan.review.isEmpty();
                if (!assignmentReady) {
                    std::cerr << "Book assignment diagnostic: finished=" << assignmentFinished
                        << " dialog=" << correctionSeen
                        << " complete=" << assignedPlan.complete()
                        << " candidates=" << assignedPlan.candidates.size()
                        << " selected=" << bookDialog.selectedCandidates().size()
                        << " review=" << assignedPlan.review.size() << '\n';
                    for (const auto& item : assignedPlan.review)
                        std::cerr << qPrintable(item.path) << ": "
                            << qPrintable(item.reason) << '\n';
                }
                check(assignmentReady,
                    "one-review assignment groups three related files without moving them");
                int assignedFiles = 0;
                for (const auto& candidate : bookDialog.selectedCandidates()) {
                    if (candidate.source.contains("Unknown.")) {
                        assignedFiles += candidate.destination.contains(
                            "Saga/02 - Next Chapter/Unknown.") ? 1 : 0;
                    }
                }
                check(assignedFiles == 3, "EPUB, PDF and audiobook share the numbered book folder");
            }
        }
    }
    bookDialog.reject();

    filemanager::files::TransferQueue transferQueue;
    TvOrganizerDialog live(input, nullptr, &transferQueue);
    live.setScope({input}, destination);
    live.show();
    auto* liveScan = live.findChild<QPushButton*>("TvOrganizerScan");
    auto* liveMove = live.findChild<QPushButton*>("TvOrganizerMove");
    check(liveScan && liveMove && !liveMove->isEnabled(),
        "execution-enabled dialog still requires an explicit scan");
    liveScan->click();
    check(waitUntil([&] { return !live.scanPending(); }) && live.reviewedPlan().complete()
            && liveMove->isEnabled(),
        "complete retained review enables the explicit Move selected action");

    bool cancellationSeen = false;
    QTimer cancellationTimer;
    cancellationTimer.setInterval(5);
    QObject::connect(&cancellationTimer, &QTimer::timeout, [&] {
        auto* confirmation = qobject_cast<QMessageBox*>(QApplication::activeModalWidget());
        if (!confirmation || confirmation->objectName() != "TvOrganizerConfirmation")
            return;
        cancellationSeen = true;
        confirmation->reject();
        cancellationTimer.stop();
    });
    cancellationTimer.start();
    liveMove->click();
    check(waitUntil([&] { return cancellationSeen && !live.reviewPending(); })
            && live.queuedJobId() == 0 && transferQueue.history().isEmpty()
            && QFileInfo::exists(QDir(input).filePath("Series.S01E02.mkv")),
        "cancelling the final confirmation queues nothing and leaves every source unchanged");

    bool confirmationSeen = false;
    bool confirmationDetailed = false;
    QTimer confirmationTimer;
    confirmationTimer.setInterval(5);
    QObject::connect(&confirmationTimer, &QTimer::timeout, [&] {
        auto* confirmation = qobject_cast<QMessageBox*>(QApplication::activeModalWidget());
        if (!confirmation || confirmation->objectName() != "TvOrganizerConfirmation")
            return;
        confirmationSeen = true;
        confirmationDetailed = confirmation->informativeText().contains("not one atomic undoable batch")
            && confirmation->detailedText().contains("Series.S01E02.mkv")
            && confirmation->detailedText().contains(QDir::toNativeSeparators(destination));
        for (QAbstractButton* button : confirmation->buttons()) {
            if (confirmation->buttonRole(button) == QMessageBox::AcceptRole) {
                button->click();
                confirmationTimer.stop();
                return;
            }
        }
    });
    confirmationTimer.start();
    liveMove->click();
    check(live.reviewPending(), "Move selected starts background identity and metadata revalidation");
    check(waitUntil([&] { return live.queuedJobId() != 0; })
            && confirmationSeen && confirmationDetailed && live.result() == QDialog::Accepted,
        "only the detailed final confirmation queues the retained batch");
    check(waitUntil([&] {
        const auto records = transferQueue.history();
        return !records.isEmpty() && (records.first().state == filemanager::files::TransferJobState::Succeeded
            || records.first().state == filemanager::files::TransferJobState::Failed
            || records.first().state == filemanager::files::TransferJobState::Cancelled);
    }), "confirmed organizer batch reaches a terminal queue state");
    const auto records = transferQueue.history();
    check(records.size() == 1 && records.first().state == filemanager::files::TransferJobState::Succeeded
            && records.first().request.reviewedMove && records.first().manifest.size() == 7,
        "confirmed UI request uses the guarded queue and records four folders plus three files");
    check(!QFileInfo::exists(QDir(input).filePath("Series.S01E02.mkv"))
            && !QFileInfo::exists(QDir(input).filePath("Series.S01E02.en.srt"))
            && !QFileInfo::exists(QDir(input).filePath("Other.S02E03.mp4"))
            && QFileInfo::exists(QDir(input).filePath("Uncertain.mp4"))
            && QFileInfo::exists(QDir(destination).filePath("Series/Season 01/Series.S01E02.mkv"))
            && QFileInfo::exists(QDir(destination).filePath("Other/Season 02/Other.S02E03.mp4")),
        "confirmed execution moves only reviewed candidate groups and leaves uncertain media untouched");

    filemanager::files::TransferQueue bookQueue;
    TvOrganizerDialog liveBooks(bookInput, nullptr, &bookQueue);
    auto* liveKind = liveBooks.findChild<QComboBox*>("MediaOrganizerKind");
    auto* liveBookScan = liveBooks.findChild<QPushButton*>("TvOrganizerScan");
    auto* liveBookMove = liveBooks.findChild<QPushButton*>("TvOrganizerMove");
    check(liveKind && liveBookScan && liveBookMove, "book mode exposes review controls");
    if (liveKind && liveBookScan && liveBookMove) {
        liveKind->setCurrentIndex(1);
        liveBooks.setScope({bookInput}, bookDestination);
        liveBookScan->click();
        check(waitUntil([&] { return !liveBooks.scanPending(); })
                && liveBooks.reviewedPlan().complete() && liveBookMove->isEnabled(),
            "book mode exposes only complete reviewed group for confirmation");
        bool bookConfirmationSeen = false;
        QTimer bookConfirmationTimer;
        bookConfirmationTimer.setInterval(5);
        QObject::connect(&bookConfirmationTimer, &QTimer::timeout, [&] {
            auto* confirmation = qobject_cast<QMessageBox*>(QApplication::activeModalWidget());
            if (!confirmation || confirmation->objectName() != "TvOrganizerConfirmation")
                return;
            bookConfirmationSeen = confirmation->windowTitle().contains("book-series")
                && confirmation->detailedText().contains("Saga - 01 - Arrival.epub")
                && confirmation->detailedText().contains("Saga\\01 - Arrival");
            for (QAbstractButton* button : confirmation->buttons()) {
                if (confirmation->buttonRole(button) == QMessageBox::AcceptRole) {
                    button->click();
                    bookConfirmationTimer.stop();
                    return;
                }
            }
        });
        bookConfirmationTimer.start();
        liveBookMove->click();
        check(waitUntil([&] { return liveBooks.queuedJobId() != 0; }) && bookConfirmationSeen,
            "book move requires detailed batch confirmation");
        check(waitUntil([&] {
            const auto jobs = bookQueue.history();
            return !jobs.isEmpty() && jobs.first().state == filemanager::files::TransferJobState::Succeeded;
        }), "confirmed book group completes through guarded transfer queue");
        check(QFileInfo::exists(QDir(bookDestination).filePath(
                    "Saga/01 - Arrival/Saga - 01 - Arrival.epub"))
                && QFileInfo::exists(QDir(bookDestination).filePath(
                    "Saga/01 - Arrival/Saga - 01 - Arrival.pdf"))
                && QFileInfo::exists(QDir(bookDestination).filePath(
                    "Saga/01 - Arrival/Saga - 01 - Arrival.cover.jpg"))
                && QFileInfo::exists(QDir(bookInput).filePath("Unknown.epub"))
                && QFileInfo::exists(QDir(bookInput).filePath("Unknown.pdf"))
                && QFileInfo::exists(QDir(bookInput).filePath("Unknown.m4b")),
            "book move preserves original filenames and does not retain one-review assignments");
    }
    return passed;
}
