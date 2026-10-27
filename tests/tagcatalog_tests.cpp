#include "tags/tagcatalog.h"

#include "files/sourcesnapshot.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QLockFile>
#include <QTemporaryDir>

#include <iostream>

int runTagCatalogTests()
{
    using namespace filemanager;
    int failures = 0;
    const auto check = [&](bool condition, const char* message) {
        if (!condition) {
            std::cerr << "FAIL: tag catalog: " << message << '\n';
            ++failures;
        }
    };

    QTemporaryDir fixture;
    check(fixture.isValid(), "create disposable catalog fixture");
    if (!fixture.isValid())
        return failures;

    const QString firstPath = fixture.filePath("book.epub");
    const QString oldPath = fixture.filePath("original.epub");
    const QString secondPath = fixture.filePath("second.epub");
    const QString storagePath = fixture.filePath("catalog.dat");
    QFile first(firstPath);
    check(first.open(QIODevice::WriteOnly) && first.write("first") == 5,
        "create selected source");
    first.close();
    const auto firstSnapshot = files::captureSourceSnapshot(firstPath);
    check(firstSnapshot.valid, "capture native file identity");

    tags::TagCatalog catalog(storagePath);
    QString error;
    check(!catalog.ready()
            && !catalog.assign({{firstPath, firstSnapshot, {"Books"}}}, &error)
            && catalog.match(firstPath, firstSnapshot).state == tags::TagMatchState::Unavailable
            && !QFileInfo::exists(storagePath),
        "unloaded catalog refuses writes and exposes no labels");
    error.clear();
    check(catalog.load(&error) && catalog.ready() && catalog.entries().isEmpty(),
        "uncreated local catalog loads empty");
    check(catalog.assign({{firstPath, firstSnapshot, {" Books ", "books", "Série"}}}, &error),
        "assign normalized labels to selected identity");

    // Older encrypted catalogs can contain destination rules. Label edits must
    // keep that data readable without invoking any destination planner or move.
    const QList<tags::DestinationRule> legacyRules{
        {"books", {"Books"}, fixture.filePath("Old destination")}};
    check(catalog.replaceRules(legacyRules, &error),
        "prepare a legacy catalog with a dormant destination rule");
    tags::TagCatalog stalePeer(storagePath);
    check(stalePeer.load(&error), "open second catalog view before label edits");

    QLockFile heldLock(storagePath + ".lock");
    check(heldLock.tryLock(0), "hold catalog write lock");
    check(!catalog.addTags({{firstPath, firstSnapshot}}, {"Blocked"}, &error)
            && catalog.match(firstPath, firstSnapshot).tags
                == QStringList({"Books", "Série"}),
        "busy catalog refuses the edit without changing labels");
    heldLock.unlock();
    check(catalog.addTags({{firstPath, firstSnapshot}}, {"Reading", "books"}, &error)
            && catalog.match(firstPath, firstSnapshot).tags
                == QStringList({"Books", "Série", "Reading"}),
        "add labels without replacing existing labels");
    check(stalePeer.addTags({{firstPath, firstSnapshot}}, {"Peer"}, &error)
            && stalePeer.match(firstPath, firstSnapshot).tags
                == QStringList({"Books", "Série", "Reading", "Peer"})
            && stalePeer.rules() == legacyRules,
        "concurrent edit reloads labels and preserves dormant legacy data");
    check(catalog.removeTags({{firstPath, firstSnapshot}}, {"reading", "peer"}, &error)
            && catalog.match(firstPath, firstSnapshot).tags
                == QStringList({"Books", "Série"}),
        "remove named labels without clearing unrelated labels");

    QFile stored(storagePath);
    check(stored.open(QIODevice::ReadOnly), "read encrypted catalog bytes");
    const QByteArray ciphertext = stored.readAll();
    check(!ciphertext.contains(firstPath.toUtf8()) && !ciphertext.contains("Books"),
        "catalog does not store raw paths or labels");
    stored.close();

    tags::TagCatalog reopened(storagePath);
    check(reopened.load(&error)
            && reopened.match(firstPath, firstSnapshot).state == tags::TagMatchState::Current
            && reopened.match(firstPath, firstSnapshot).tags
                == QStringList({"Books", "Série"})
            && reopened.rules() == legacyRules,
        "encrypted labels round trip without erasing legacy catalog fields");
    check(!reopened.assign({{firstPath, firstSnapshot, {"invalid\nlabel"}}}, &error)
            && reopened.match(firstPath, firstSnapshot).state == tags::TagMatchState::Current,
        "invalid label leaves existing labels unchanged");

    QFile changed(firstPath);
    check(changed.open(QIODevice::Append) && changed.write(" changed") == 8,
        "modify selected file contents after labeling");
    changed.close();
    const auto changedSnapshot = files::captureSourceSnapshot(firstPath);
    check(changedSnapshot.valid && changedSnapshot.fileIndex == firstSnapshot.fileIndex
            && reopened.match(firstPath, changedSnapshot).state == tags::TagMatchState::Current
            && reopened.match(firstPath, changedSnapshot).tags.contains("Books"),
        "same native identity retains labels after a content edit");

    QFile second(secondPath);
    check(second.open(QIODevice::WriteOnly) && second.write("second") == 6,
        "create second selected source");
    second.close();
    const auto secondSnapshot = files::captureSourceSnapshot(secondPath);
    check(QFile::rename(firstPath, oldPath), "retain old identity at another name");
    QFile replacement(firstPath);
    check(replacement.open(QIODevice::WriteOnly)
            && replacement.write("replacement") == 11,
        "replace path with a distinct native file");
    replacement.close();
    const auto replacementSnapshot = files::captureSourceSnapshot(firstPath);
    check(replacementSnapshot.valid && replacementSnapshot.fileIndex != firstSnapshot.fileIndex
            && reopened.match(firstPath, replacementSnapshot).state == tags::TagMatchState::Replaced
            && reopened.match(firstPath, replacementSnapshot).tags.isEmpty(),
        "replacement at the same path does not inherit labels");
    check(!reopened.addTags({{secondPath, secondSnapshot},
                {firstPath, replacementSnapshot}}, {"New"}, &error)
            && reopened.match(secondPath, secondSnapshot).state
                == tags::TagMatchState::Unassigned,
        "replacement refuses an entire bulk label assignment");

    check(reopened.clear(&error) && !QFileInfo::exists(storagePath)
            && reopened.entries().isEmpty() && reopened.rules().isEmpty(),
        "clear removes app-owned encrypted label catalog");

    const QString lifecycleStorage = fixture.filePath("lifecycle-catalog.dat");
    tags::TagCatalog lifecycle(lifecycleStorage);
    check(lifecycle.load(&error), "load disposable label lifecycle catalog");
    const QString renameFrom = fixture.filePath("rename-from.epub");
    const QString renameTo = fixture.filePath("rename-to.epub");
    QFile renamed(renameFrom);
    check(renamed.open(QIODevice::WriteOnly) && renamed.write("rename") == 6,
        "create labeled rename source");
    renamed.close();
    const auto renameBefore = files::captureSourceSnapshot(renameFrom);
    check(lifecycle.assign({{renameFrom, renameBefore, {"Series"}}}, &error),
        "label source before File Manager rename");
    check(QFile::rename(renameFrom, renameTo), "commit disposable rename");
    const auto renameAfter = files::captureSourceSnapshot(renameTo);
    check(lifecycle.relocate({{renameFrom, renameTo, renameBefore, renameAfter, false}}, &error)
            && lifecycle.match(renameTo, renameAfter).tags == QStringList{"Series"}
            && lifecycle.match(renameFrom, renameBefore).state == tags::TagMatchState::Unassigned,
        "label follows committed same-identity rename");
    check(QFile::rename(renameTo, renameFrom), "commit disposable rename undo");
    const auto renameRestored = files::captureSourceSnapshot(renameFrom);
    check(lifecycle.relocate({{renameTo, renameFrom, renameAfter, renameRestored, false}}, &error)
            && lifecycle.match(renameFrom, renameRestored).tags == QStringList{"Series"},
        "label follows completed rename undo");

    const QString copied = fixture.filePath("copied.epub");
    check(QFile::copy(renameFrom, copied), "create independent file copy");
    const auto copiedSnapshot = files::captureSourceSnapshot(copied);
    check(lifecycle.match(copied, copiedSnapshot).state == tags::TagMatchState::Unassigned,
        "copy starts without a label");
    check(QFile::remove(renameFrom), "remove source after committed copy");
    check(lifecycle.relocate({{renameFrom, copied, renameRestored, copiedSnapshot, false}}, &error)
            && lifecycle.match(copied, copiedSnapshot).tags == QStringList{"Series"},
        "label follows a committed move when destination identity changes");

    const QString folderFrom = fixture.filePath("folder-before");
    const QString folderTo = fixture.filePath("folder-after");
    check(QDir().mkpath(folderFrom), "create tagged folder fixture");
    const QString nestedFrom = QDir(folderFrom).filePath("chapter.epub");
    QFile nested(nestedFrom);
    check(nested.open(QIODevice::WriteOnly) && nested.write("chapter") == 7,
        "create nested labeled file");
    nested.close();
    const auto nestedBefore = files::captureSourceSnapshot(nestedFrom);
    check(lifecycle.assign({{nestedFrom, nestedBefore, {"Chapter"}}}, &error),
        "label nested file before folder rename");
    const auto folderBefore = files::captureSourceSnapshot(folderFrom);
    check(QDir().rename(folderFrom, folderTo), "commit tagged folder rename");
    const auto folderAfter = files::captureSourceSnapshot(folderTo);
    const QString nestedTo = QDir(folderTo).filePath("chapter.epub");
    const auto nestedAfter = files::captureSourceSnapshot(nestedTo);
    check(lifecycle.relocate({{folderFrom, folderTo, folderBefore, folderAfter, true}}, &error)
            && lifecycle.match(nestedTo, nestedAfter).tags == QStringList{"Chapter"}
            && lifecycle.match(nestedFrom, nestedBefore).state == tags::TagMatchState::Unassigned,
        "folder rename relocates nested labels without scanning its contents");

    const QString swapA = fixture.filePath("swap-a.epub");
    const QString swapB = fixture.filePath("swap-b.epub");
    const QString swapStage = fixture.filePath("swap-stage.epub");
    QFile a(swapA);
    QFile b(swapB);
    check(a.open(QIODevice::WriteOnly) && a.write("a") == 1,
        "create first batch-swap file");
    check(b.open(QIODevice::WriteOnly) && b.write("b") == 1,
        "create second batch-swap file");
    a.close();
    b.close();
    const auto aBefore = files::captureSourceSnapshot(swapA);
    const auto bBefore = files::captureSourceSnapshot(swapB);
    check(lifecycle.assign({{swapA, aBefore, {"First"}},
                {swapB, bBefore, {"Second"}}}, &error),
        "label both batch-swap sources");
    check(QFile::rename(swapA, swapStage) && QFile::rename(swapB, swapA)
            && QFile::rename(swapStage, swapB),
        "commit disposable two-name swap");
    const auto aAfter = files::captureSourceSnapshot(swapA);
    const auto bAfter = files::captureSourceSnapshot(swapB);
    check(lifecycle.relocate({{swapA, swapB, aBefore, bAfter, false},
                {swapB, swapA, bBefore, aAfter, false}}, &error)
            && lifecycle.match(swapA, aAfter).tags == QStringList{"Second"}
            && lifecycle.match(swapB, bAfter).tags == QStringList{"First"},
        "batch swap follows each native identity atomically");

    const QString staleFrom = fixture.filePath("stale-before.epub");
    const QString staleTo = fixture.filePath("stale-after.epub");
    QFile stale(staleFrom);
    check(stale.open(QIODevice::WriteOnly) && stale.write("stale") == 5,
        "create stale-destination fixture");
    stale.close();
    const auto staleBefore = files::captureSourceSnapshot(staleFrom);
    check(lifecycle.assign({{staleFrom, staleBefore, {"Private"}}}, &error),
        "label stale-destination source");
    check(QFile::rename(staleFrom, staleTo), "commit stale-destination rename");
    const auto expectedDestination = files::captureSourceSnapshot(staleTo);
    const QString staleHeld = fixture.filePath("stale-held.epub");
    check(QFile::rename(staleTo, staleHeld),
        "retain committed identity at another name before replacement");
    QFile replacementAtDestination(staleTo);
    check(replacementAtDestination.open(QIODevice::WriteOnly)
            && replacementAtDestination.write("new identity") == 12,
        "replace committed destination with a distinct file");
    replacementAtDestination.close();
    const auto changedDestination = files::captureSourceSnapshot(staleTo);
    check(!lifecycle.relocate({{staleFrom, staleTo, staleBefore,
                expectedDestination, false}}, &error)
            && lifecycle.match(staleTo, changedDestination).tags.isEmpty(),
        "changed destination cannot inherit labels after a rename");

    return failures;
}
