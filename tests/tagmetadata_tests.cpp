#include "files/sourcesnapshot.h"
#include "preview/documentmetadataprovider.h"
#include "preview/sandboxedpreviewprocess.h"

#include <QDir>
#include <QFile>
#include <QTemporaryDir>

#include <atomic>
#include <iostream>

int runTagMetadataTests()
{
#ifndef Q_OS_WIN
    return 0;
#else
    int failures = 0;
    const auto check = [&](bool condition, const char* message) {
        if (!condition) {
            std::cerr << "FAIL: selected tag metadata: " << message << '\n';
            ++failures;
        }
    };
    QTemporaryDir fixture;
    check(fixture.isValid(), "disposable selected-document folder exists");
    if (!fixture.isValid())
        return failures;
    const QString path = QDir(fixture.path()).filePath(QStringLiteral("book.rtf"));
    QFile file(path);
    check(file.open(QIODevice::WriteOnly), "selected RTF fixture opens");
    if (!file.isOpen())
        return failures;
    const QByteArray rtf = R"({\rtf1{\info{\title Example Book}{\author Ada Reader}}})";
    check(file.write(rtf) == rtf.size(), "selected RTF fixture is complete");
    file.close();
    const auto snapshot = filemanager::files::captureSourceSnapshot(path);
    check(snapshot.valid && snapshot.fileIndex != 0,
        "selected document has a native identity");
    const auto metadata = filemanager::preview::readSelectedDocumentMetadata(
        path, snapshot);
    if (!metadata.available) {
        const auto helper = filemanager::preview::runSandboxedPreviewHost(
            path, QStringLiteral("document-metadata"), 6000, 128 * 1024);
        std::cerr << "Selected metadata unavailable: "
                  << metadata.error.toStdString()
                  << "; helper success=" << helper.success
                  << " sandboxed=" << helper.sandboxed
                  << " job=" << helper.jobRestricted
                  << " exit=" << helper.exitCode
                  << " reply=" << helper.standardOutput.toStdString()
                  << "; helper error=" << helper.error.toStdString() << '\n';
    }
    check(metadata.available && metadata.error.isEmpty()
            && metadata.metadata.title == QStringLiteral("Example Book")
            && metadata.metadata.authors == QStringLiteral("Ada Reader"),
        "bounded local document metadata uses the selected identity");

    std::atomic_bool cancelled{true};
    const auto stopped = filemanager::preview::readSelectedDocumentMetadata(
        path, snapshot, &cancelled);
    check(!stopped.available && !stopped.error.isEmpty(),
        "cancelled metadata review returns no fields");

    QFile changed(path);
    check(changed.open(QIODevice::Append) && changed.write("changed") == 7,
        "selected document can change after preview");
    changed.close();
    const auto stale = filemanager::preview::readSelectedDocumentMetadata(
        path, snapshot);
    check(!stale.available && stale.metadata.title.isEmpty(),
        "a changed selected file cannot supply rule evidence");

    const QString unsupported = QDir(fixture.path()).filePath(QStringLiteral("book.txt"));
    check(QFile::copy(path, unsupported), "unsupported extension fixture exists");
    const auto unsupportedSnapshot = filemanager::files::captureSourceSnapshot(
        unsupported);
    check(!filemanager::preview::readSelectedDocumentMetadata(
            unsupported, unsupportedSnapshot).available,
        "unsupported document types supply no rule evidence");
    return failures;
#endif
}
