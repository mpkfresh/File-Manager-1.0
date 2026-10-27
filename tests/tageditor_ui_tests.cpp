#include "ui/tageditordialog.h"

#include "files/sourcesnapshot.h"
#include "tags/tagcatalog.h"

#include <QApplication>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QFile>
#include <QLabel>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QTemporaryDir>
#include <QThread>

#include <functional>
#include <iostream>

namespace {

bool expect(bool condition, const char* message)
{
    if (!condition)
        std::cerr << "FAIL: tag editor: " << message << '\n';
    return condition;
}

bool waitFor(const std::function<bool()>& predicate, int timeoutMs = 10'000)
{
    QElapsedTimer elapsed;
    elapsed.start();
    while (!predicate() && elapsed.elapsed() < timeoutMs) {
        QApplication::processEvents(QEventLoop::AllEvents, 25);
        QThread::msleep(2);
    }
    return predicate();
}

} // namespace

bool runTagEditorUiTests()
{
    bool passed = true;
    QTemporaryDir fixture;
    if (!expect(fixture.isValid(), "create isolated tag UI fixture"))
        return false;
    const QString firstPath = fixture.filePath("first.epub");
    const QString secondPath = fixture.filePath("second.epub");
    for (const QString& path : {firstPath, secondPath}) {
        QFile file(path);
        if (!expect(file.open(QIODevice::WriteOnly) && file.write("unchanged") == 9,
                "create selected tag source"))
            return false;
    }
    const auto firstBefore = filemanager::files::captureSourceSnapshot(firstPath);
    const auto secondBefore = filemanager::files::captureSourceSnapshot(secondPath);
    const QString catalogPath = fixture.filePath("tags.dat");

    filemanager::ui::TagEditorDialog dialog({firstPath, secondPath}, nullptr, catalogPath);
    auto* input = dialog.findChild<QPlainTextEdit*>("TagEditorLabels");
    auto* inspect = dialog.findChild<QPushButton*>("TagEditorInspect");
    auto* current = dialog.findChild<QPlainTextEdit*>("TagEditorCurrentTags");
    auto* add = dialog.findChild<QPushButton*>("TagEditorAdd");
    auto* remove = dialog.findChild<QPushButton*>("TagEditorRemove");
    auto* status = dialog.findChild<QLabel*>("TagEditorStatus");
    passed &= expect(input && current && inspect && add && remove && status,
        "show current-tag status and explicit add/remove controls");
    if (!passed)
        return false;
    dialog.show();
    QApplication::processEvents();

    input->setPlainText("Books\nSeries");
    passed &= expect(add->isEnabled() && remove->isEnabled(),
        "enable tag actions only with labels and a file selection");
    add->click();
    passed &= expect(waitFor([add] { return add->isEnabled(); }),
        "finish background add-tag job");
    passed &= expect(status->text().contains("Tag addition completed"),
        "add selected labels without a dialog error");

    filemanager::tags::TagCatalog catalog(catalogPath);
    passed &= expect(catalog.load(), "read isolated encrypted UI catalog");
    passed &= expect(catalog.match(firstPath, firstBefore).tags
            == QStringList({"Books", "Series"})
            && catalog.match(secondPath, secondBefore).tags
                == QStringList({"Books", "Series"}),
        "tag both explicitly selected files without changing contents");

    inspect->click();
    passed &= expect(waitFor([inspect] { return inspect->isEnabled(); }),
        "finish background current-tag inspection");
    passed &= expect(current->toPlainText().contains("Current tags: 2 files")
            && current->toPlainText().contains("Books (2)")
            && current->toPlainText().contains("Series (2)"),
        "show current labels and counts for the selected files");

    input->setPlainText("Series");
    remove->click();
    passed &= expect(waitFor([remove] { return remove->isEnabled(); }),
        "finish background remove-tag job");
    passed &= expect(status->text().contains("Tag removal completed"),
        "remove named tags without a dialog error");
    passed &= expect(catalog.load()
            && catalog.match(firstPath, firstBefore).tags == QStringList({"Books"})
            && catalog.match(secondPath, secondBefore).tags == QStringList({"Books"}),
        "remove only the requested label");

    QFile first(firstPath);
    QFile second(secondPath);
    passed &= expect(first.open(QIODevice::ReadOnly) && first.readAll() == "unchanged"
            && second.open(QIODevice::ReadOnly) && second.readAll() == "unchanged",
        "tag edits leave file contents untouched");
    first.close();
    second.close();
    inspect->click();
    passed &= expect(waitFor([inspect] { return inspect->isEnabled(); }),
        "finish current-tag inspection after removal");
    passed &= expect(current->toPlainText().contains("Books (2)")
            && !current->toPlainText().contains("Series (2)"),
        "refresh current-label counts after removal");

    QFile changed(firstPath);
    passed &= expect(changed.open(QIODevice::Append) && changed.write(" changed") == 8,
        "modify one selected file after tagging");
    changed.close();
    inspect->click();
    passed &= expect(waitFor([inspect] { return inspect->isEnabled(); }),
        "finish same-identity edit inspection");
    passed &= expect(current->toPlainText().contains("Current tags: 2 files")
            && current->toPlainText().contains("Books (2)")
            && current->toPlainText().contains("replaced or stale: 0"),
        "show retained tags after an edit to the same native file");
    dialog.close();

    return passed;
}
