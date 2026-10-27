#include "epub_test_fixture.h"
#include "files/bookorganizer.h"
#include "files/sourcesnapshot.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QSet>
#include <QTemporaryDir>
#include <iostream>

namespace {

bool writeFixture(const QString& path)
{
    if (!QDir().mkpath(QFileInfo(path).absolutePath()))
        return false;
    QFile file(path);
    return file.open(QIODevice::WriteOnly) && file.write("book fixture") == 12;
}

} // namespace

int runBookOrganizerTests()
{
    using namespace filemanager::files;
    int failures = 0;
    const auto check = [&](bool passed, const char* message) {
        if (!passed) {
            std::cerr << "FAIL: book organizer: " << message << '\n';
            ++failures;
        }
    };
    const auto parsed = parseBookSeriesName(QString::fromUtf8("Étoiles - 02 - The Return.epub"));
    check(parsed && parsed->series == QString::fromUtf8("Étoiles") && parsed->number == 2
            && parsed->title == "The Return", "strict Unicode series and number parsing");
    for (const QString& name : QStringList{"Novel.epub", "Series 02 Title.epub",
            "Series - 00 - Title.pdf", "Series - 02 - Title.txt", "CON - 02 - Title.epub",
            "Series - 02 - CON.pdf", "Series - 02 - Title - Part 1.epub"}) {
        check(!parseBookSeriesName(name), qPrintable("exclude uncertain name: " + name));
    }

    QTemporaryDir fixture;
    check(fixture.isValid(), "temporary fixture directory");
    if (!fixture.isValid())
        return failures;
    const QDir base(fixture.path());
    const QString input = base.filePath("Input");
    const QString books = base.filePath("Input/Books");
    check(QDir().mkpath(books), "create chosen Books destination");
    const QString epub = base.filePath("Input/Saga - 01 - Arrival.epub");
    const QString pdf = base.filePath("Input/Saga - 01 - Arrival.pdf");
    const QString cover = base.filePath("Input/Saga - 01 - Arrival.cover.jpg");
    const QString audio = base.filePath("Input/Nested/Other - 07 - Finale.m4b");
    const QString ambiguous = base.filePath("Input/Unknown.epub");
    const QString conflict1 = base.filePath("Input/Conflict - 03 - Red.epub");
    const QString conflict2 = base.filePath("Input/Conflict - 03 - Blue.pdf");
    const QString existing = base.filePath("Input/Books/Existing - 01 - Safe.epub");
    for (const QString& path : QStringList{epub, pdf, cover, audio, ambiguous,
            conflict1, conflict2, existing, base.filePath("Input/notes.txt")})
        check(writeFixture(path), "write book fixture");

    // A readable EPUB with no series claim may use its clear filename. The
    // generic text fixture above is deliberately replaced with a real ZIP/OPF
    // package so this grouping test does not rely on unsafe fallback.
    const QByteArray containerXml = R"(<?xml version="1.0"?>
<container xmlns="urn:oasis:names:tc:opendocument:xmlns:container" version="1.0">
 <rootfiles><rootfile full-path="EPUB/package.opf" media-type="application/oebps-package+xml"/></rootfiles>
</container>)";
    const QByteArray packageXml = R"(<package xmlns="http://www.idpf.org/2007/opf" version="3.0">
<metadata xmlns:dc="http://purl.org/dc/elements/1.1/"><dc:title>Arrival</dc:title></metadata>
</package>)";
    for (const QString& path : QStringList{epub, conflict1, existing})
        check(bookTestFixtures::writeEpub(path, containerXml, packageXml),
            "write readable book EPUB fixture");

    const auto plan = planBookOrganization({input, input, base.filePath("Input/Nested")}, books);
    check(plan.complete() && plan.candidates.size() == 4, "group two formats, one cover and one audiobook");
    check(plan.review.size() == 3 && plan.ignoredFiles == 1,
        "exclude unclear book and conflicting series-number claims; ignore unrelated format");
    QSet<QString> sources;
    for (const auto& candidate : plan.candidates) {
        sources.insert(candidate.source);
        check(candidate.sourceSnapshot.valid
                && candidate.sourceSnapshot == captureSourceSnapshot(candidate.source),
            "retain each candidate's native source identity");
        check(QFileInfo(candidate.source).fileName() == QFileInfo(candidate.destination).fileName(),
            "preserve each source filename");
    }
    check(sources.contains(epub) && sources.contains(pdf) && sources.contains(cover)
            && sources.contains(audio) && !sources.contains(existing),
        "place related formats together and exclude destination subtree");
    SourceSnapshot unknownSnapshot;
    for (const auto& item : plan.review) {
        if (item.path == ambiguous)
            unknownSnapshot = item.sourceSnapshot;
    }
    check(unknownSnapshot.valid, "retain reviewed source identity for explicit correction");
    const BookCorrection correction{ambiguous, unknownSnapshot, {"Saga", 1, "Arrival"}};
    const auto assigned = planBookOrganization({input}, books, nullptr, 100'000, {correction});
    check(assigned.complete() && assigned.candidates.size() == 5
            && assigned.review.size() == 2,
        "one-review correction joins an unclear filename to its related book files");
    bool assignedPath = false;
    for (const auto& candidate : assigned.candidates) {
        if (candidate.source == ambiguous) {
            assignedPath = candidate.destination
                == books + "/Saga/01 - Arrival/Unknown.epub"
                && candidate.evidence.contains("Explicit one-review");
        }
    }
    check(assignedPath, "manual assignment retains the original filename and evidence");
    BookCorrection stale = correction;
    ++stale.sourceSnapshot.fileIndex;
    const auto changed = planBookOrganization({input}, books, nullptr, 100'000, {stale});
    check(changed.complete() && changed.candidates.size() == 4
            && changed.review.size() == 3,
        "stale correction refuses a changed source identity");
    BookCorrection unsafe = correction;
    unsafe.name.number = 0;
    const auto rejected = planBookOrganization({input}, books, nullptr, 100'000, {unsafe});
    check(!rejected.complete() && rejected.candidates.isEmpty(),
        "unsafe correction is rejected before the scan can propose moves");
    for (const auto& candidate : plan.candidates) {
        if (candidate.source == epub || candidate.source == pdf || candidate.source == cover)
            check(QFileInfo(candidate.destination).absolutePath() == books + "/Saga/01 - Arrival",
                "series and numbered title folder");
        if (candidate.source == cover)
            check(candidate.companion && candidate.groupSource == epub,
                "cover follows the retained book group");
    }

    check(writeFixture(books + "/Saga/01 - Arrival/Saga - 01 - Arrival.pdf"),
        "create destination collision fixture");
    const auto collision = planBookOrganization({input}, books);
    check(collision.complete() && collision.candidates.size() == 1
            && collision.review.size() >= 6,
        "existing destination excludes the complete book group");
    const auto limited = planBookOrganization({input}, books, nullptr, 1);
    check(limited.limitReached && limited.candidates.isEmpty(),
        "truncated scan cannot yield a partial move proposal");
    std::atomic_bool cancelled{true};
    const auto stopped = planBookOrganization({input}, books, &cancelled);
    check(stopped.cancelled && stopped.candidates.isEmpty(), "cancelled scan yields no proposals");
    return failures;
}
