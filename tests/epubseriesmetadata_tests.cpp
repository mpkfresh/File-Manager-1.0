#include "epub_test_fixture.h"
#include "files/bookorganizer.h"
#include "files/epubseriesmetadata.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QList>
#include <QPair>
#include <QTemporaryDir>
#include <atomic>
#include <iostream>

namespace bookTestFixtures {

const QByteArray container = R"(<?xml version="1.0"?>
<container xmlns="urn:oasis:names:tc:opendocument:xmlns:container" version="1.0">
 <rootfiles><rootfile full-path="EPUB/package.opf" media-type="application/oebps-package+xml"/></rootfiles>
</container>)";

QByteArray package(const QByteArray& metadata)
{
    return QByteArray(R"(<package xmlns="http://www.idpf.org/2007/opf" version="3.0">
<metadata xmlns:dc="http://purl.org/dc/elements/1.1/">
<dc:title>Arrival</dc:title>
)") + metadata + R"(</metadata></package>)";
}

const QByteArray series = R"(<meta property="belongs-to-collection" id="s1">Saga</meta>
<meta refines="#s1" property="collection-type">series</meta>
<meta refines="#s1" property="group-position">1</meta>)";

} // namespace bookTestFixtures

int runEpubSeriesMetadataTests()
{
    using namespace filemanager::files;
    using namespace bookTestFixtures;
    int failures = 0;
    const auto check = [&](bool passed, const char* message) {
        if (!passed) {
            std::cerr << "FAIL: EPUB series metadata: " << message << '\n';
            ++failures;
        }
    };
    QTemporaryDir fixture;
    check(fixture.isValid(), "temporary fixture directory");
    if (!fixture.isValid())
        return failures;
    const QString books = fixture.filePath("Books");
    const QString input = fixture.filePath("Input");
    check(QDir().mkpath(books) && QDir().mkpath(input), "create disposable scan folders");
    const QString valid = QDir(input).filePath("Opaque name.epub");
    check(writeEpub(valid, container, package(series)), "write valid package-metadata fixture");
    const auto metadata = readEpubSeriesMetadata(valid);
    check(metadata.valid() && metadata.series == "Saga" && metadata.number == 1
            && metadata.title == "Arrival", "read explicit EPUB series, position and title");
    std::atomic_bool cancelled{true};
    const auto stopped = readEpubSeriesMetadata(valid, &cancelled);
    check(!stopped.valid() && stopped.blocksFilenameFallback,
        "a cancelled EPUB inspection cannot fall back to a filename move");
    const auto plan = planBookOrganization({input}, books);
    check(plan.complete() && plan.candidates.size() == 1
            && plan.candidates.first().source == valid
            && plan.candidates.first().destination == books + "/Saga/01 - Arrival/Opaque name.epub",
        "metadata alone proposes the numbered book folder while preserving filename");

    const QString conflict = QDir(input).filePath("Saga - 02 - Arrival.epub");
    check(writeEpub(conflict, container, package(series)), "write conflicting filename fixture");
    const auto disagreement = planBookOrganization({input}, books);
    check(disagreement.complete() && disagreement.candidates.size() == 1
            && disagreement.review.size() == 1 && disagreement.review.first().path == conflict,
        "filename and embedded sequence conflict remains excluded");

    const QString multiple = QDir(input).filePath("Saga - 03 - Another.epub");
    const QByteArray secondSeries = R"(<meta property="belongs-to-collection" id="s2">Other</meta>
<meta refines="#s2" property="collection-type">series</meta>
<meta refines="#s2" property="group-position">2</meta>)";
    check(writeEpub(multiple, container, package(series + secondSeries)),
        "write multiple-series fixture");
    check(!readEpubSeriesMetadata(multiple).valid(), "multiple series cannot establish one folder");

    const QString decimal = QDir(input).filePath("Saga - 04 - Another.epub");
    QByteArray decimalSeries = series;
    decimalSeries.replace(">1</meta>", ">2.2</meta>");
    check(writeEpub(decimal, container, package(decimalSeries)), "write fractional-position fixture");
    check(!readEpubSeriesMetadata(decimal).valid(), "fractional series position needs review");

    const QString conflictingTitle = QDir(input).filePath("Saga - 05 - Arrival.epub");
    check(writeEpub(conflictingTitle, container,
            package(series + QByteArray("<dc:title>Another title</dc:title>"))),
        "write conflicting-title fixture");
    const auto titleMetadata = readEpubSeriesMetadata(conflictingTitle);
    check(!titleMetadata.valid() && titleMetadata.blocksFilenameFallback,
        "conflicting EPUB titles cannot silently choose a destination or filename fallback");

    const QString doctype = QDir(input).filePath("DTD.epub");
    check(writeEpub(doctype, container, QByteArray("<!DOCTYPE package>") + package(series)),
        "write DTD fixture");
    check(!readEpubSeriesMetadata(doctype).valid(), "DTD in package metadata is rejected");

    const QString unsafe = QDir(input).filePath("Unsafe.epub");
    QByteArray unsafeContainer = container;
    unsafeContainer.replace("EPUB/package.opf", "../package.opf");
    check(writeEpub(unsafe, unsafeContainer, package(series)), "write unsafe package-path fixture");
    check(!readEpubSeriesMetadata(unsafe).valid(), "unsafe OPF path is rejected");
    const QString unsafeNamed = QDir(input).filePath("Saga - 06 - Arrival.epub");
    check(writeEpub(unsafeNamed, unsafeContainer, package(series)),
        "write unsafe package with an otherwise clear filename");
    const auto unreadable = readEpubSeriesMetadata(unsafeNamed);
    check(!unreadable.valid() && unreadable.blocksFilenameFallback,
        "an unsafe EPUB package cannot be silently sorted by filename");
    const QString filenameOnly = QDir(input).filePath("Saga - 07 - Arrival.epub");
    check(writeEpub(filenameOnly, container, package(QByteArray{})),
        "write readable EPUB without an explicit series claim");
    const auto noSeries = readEpubSeriesMetadata(filenameOnly);
    check(!noSeries.valid() && !noSeries.blocksFilenameFallback,
        "a readable EPUB with no series claim may use its clear filename");
    const auto ambiguousPlan = planBookOrganization({input}, books);
    bool multipleExcluded = false;
    bool decimalExcluded = false;
    bool titleExcluded = false;
    bool unsafeNamedExcluded = false;
    bool filenameOnlyIncluded = false;
    for (const auto& item : ambiguousPlan.review) {
        multipleExcluded = multipleExcluded || item.path == multiple;
        decimalExcluded = decimalExcluded || item.path == decimal;
        titleExcluded = titleExcluded || item.path == conflictingTitle;
        unsafeNamedExcluded = unsafeNamedExcluded || item.path == unsafeNamed;
    }
    for (const auto& item : ambiguousPlan.candidates)
        filenameOnlyIncluded = filenameOnlyIncluded || item.source == filenameOnly;
    check(ambiguousPlan.complete() && multipleExcluded && decimalExcluded && titleExcluded
            && unsafeNamedExcluded && filenameOnlyIncluded,
        "unusable EPUB metadata is reviewed while readable no-series metadata permits filename fallback");
    return failures;
}
