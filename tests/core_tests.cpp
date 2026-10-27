#include "search/fuzzymatcher.h"
#include "files/fileoperations.h"
#include "files/fileanalysis.h"
#include "files/batchrename.h"
#include "files/duplicatefinder.h"
#include "files/nameordering.h"
#include "files/storagevolumes.h"
#include "files/sessionundo.h"
#include "files/transfercontroller.h"
#include "files/filenamevalidator.h"
#include "workspace/layoutmodel.h"
#include "settings/favoritegroups.h"
#include "settings/portablepreferences.h"
#include "settings/settingshealth.h"
#include "platform/recyclebinoperation.h"
#include "platform/nativefilecopy.h"
#include "platform/guardedfilerename.h"
#include "platform/shelllink.h"
#include "platform/cloudfilestatus.h"
#include "ui/transferannouncements.h"
#include "ui/applicationtheme.h"
#include "ui/localizedformat.h"
#include "diagnostics/diagnosticbundle.h"
#include "diagnostics/diagnosticlog.h"

#include <QCoreApplication>
#include <QDateTime>
#include <QElapsedTimer>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSet>
#include <QTemporaryDir>
#include <QSettings>
#include <iostream>

#ifdef Q_OS_WIN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace {

int failures = 0;

void expect(bool condition, const char* message)
{
    if (!condition) {
        std::cerr << "FAIL: " << message << '\n';
        ++failures;
    }
}

} // namespace

int runTvOrganizerTests();
int runBookOrganizerTests();
int runEpubSeriesMetadataTests();
int runTagCatalogTests();
int runTagMetadataTests();

int main(int argc, char** argv)
{
    QElapsedTimer phaseTime;
    phaseTime.start();
    const auto phase = [&phaseTime](const char* name) {
        std::cout << "Core phase: " << name << " elapsed_ms=" << phaseTime.elapsed() << std::endl;
    };
    phase("main_entered");
    QCoreApplication app(argc, argv);
    if (app.arguments().contains("--tags-only")) {
        failures += runTagCatalogTests();
        failures += runTagMetadataTests();
        if (failures == 0)
            std::cout << "Tag catalog core tests passed.\n";
        return failures == 0 ? 0 : 1;
    }
    phase("application_ready_media_organizer_begin");
    failures += runTvOrganizerTests();
    failures += runBookOrganizerTests();
    failures += runEpubSeriesMetadataTests();
    if (app.arguments().contains("--media-organizer-only")) {
        if (failures == 0)
            std::cout << "Media Organizer core tests passed.\n";
        return failures == 0 ? 0 : 1;
    }
    phase("media_organizer_finished_formatting_begin");
    failures += runTagCatalogTests();
    failures += runTagMetadataTests();
    using filemanager::search::match;

    {
        filemanager::platform::CloudFileStatus local;
        local.inspected = true;
        expect(filemanager::platform::cloudFileStatusText(local).isEmpty(),
            "Ordinary local items should not add noise to the Availability column");
        expect(filemanager::platform::cloudFileStatusText(local, true) == "Local item",
            "The details inspector should identify an ordinary local item");

        filemanager::platform::CloudFileStatus partial;
        partial.inspected = true;
        partial.providerBacked = true;
        partial.cloudFilesPlaceholder = true;
        partial.partiallyOnDisk = true;
        partial.inSync = true;
        expect(filemanager::platform::cloudFileStatusText(partial)
                == "Partly on this device · In sync",
            "Cloud status formatting should report availability and sync state together");

        filemanager::platform::CloudFileStatus pending;
        pending.inspected = true;
        pending.providerBacked = true;
        pending.cloudFilesPlaceholder = true;
        expect(filemanager::platform::cloudFileStatusText(pending)
                == "Content not ready · Not reported in sync",
            "Cloud status formatting must not claim an unreported sync state");
        pending.pinned = true;
        expect(filemanager::platform::cloudFileStatusText(pending)
                == "Content not ready · Not reported in sync",
            "A pinned item with unavailable content must not be shown as locally ready");

        filemanager::platform::CloudFileStatus offlineProvider;
        offlineProvider.inspected = true;
        offlineProvider.providerBacked = true;
        offlineProvider.pinned = true;
        expect(filemanager::platform::cloudFileStatusText(offlineProvider)
                == "Offline provider item · Sync state unavailable",
            "A pinned offline provider item must not be labeled always available");

        pending.contentReady = true;
        expect(filemanager::platform::cloudFileStatusText(pending)
                == "Always available on this device · Not reported in sync",
            "A pinned placeholder should be labeled available only once its content is ready");
    }

#ifdef Q_OS_WIN
    {
        phase("shell_links_begin");
        QTemporaryDir shortcutRoot;
        expect(shortcutRoot.isValid(), "Shell-link fixture directory should be available");
        const QString target = shortcutRoot.filePath("chapter 01.txt");
        QFile targetFile(target);
        expect(targetFile.open(QIODevice::WriteOnly), "Shell-link target fixture should open");
        targetFile.write("chapter");
        targetFile.close();

        const auto first = filemanager::platform::createShellLinks({target}, shortcutRoot.path());
        const auto second = filemanager::platform::createShellLinks({target}, shortcutRoot.path());
        expect(first.success() && first.createdPaths.size() == 1
                && QFileInfo(first.createdPaths.first()).size() > 0,
            "IShellLink/IPersistFile should create a non-empty shortcut");
        expect(second.success() && second.createdPaths.size() == 1
                && second.createdPaths.first() != first.createdPaths.first()
                && QFileInfo::exists(first.createdPaths.first()),
            "A shortcut name collision should keep the first link and choose a distinct name");
        const auto copiedLink = filemanager::platform::createShellLinks(
            {first.createdPaths.first()}, shortcutRoot.path());
        QFile originalLink(first.createdPaths.first());
        QFile copiedLinkFile(copiedLink.createdPaths.value(0));
        const bool linksOpened = originalLink.open(QIODevice::ReadOnly)
            && copiedLinkFile.open(QIODevice::ReadOnly);
        expect(copiedLink.success() && copiedLink.createdPaths.size() == 1 && linksOpened
                && originalLink.readAll() == copiedLinkFile.readAll(),
            "Creating a shortcut from an existing .lnk should copy the Shell link payload");
        const QString missing = shortcutRoot.filePath("missing.txt");
        const int entriesBeforeFailure = QDir(shortcutRoot.path()).entryList(
            QDir::Files | QDir::NoDotAndDotDot).size();
        const auto invalid = filemanager::platform::createShellLinks(
            {target, missing}, shortcutRoot.path());
        expect(!invalid.success() && invalid.createdPaths.isEmpty()
                && QDir(shortcutRoot.path()).entryList(
                    QDir::Files | QDir::NoDotAndDotDot).size() == entriesBeforeFailure,
            "Invalid shortcut batches should fail preflight without partial output");
    }
#endif

    expect(match("fp", "File Pilot").matched, "ordered characters should match");
    expect(!match("pf", "File Pilot").matched, "out-of-order characters should not match");
    expect(match("file", "File Pilot").score > match("file", "profile-data").score,
        "word-boundary match should rank higher");
    expect(match("", "anything").matched, "empty query should match");

    using filemanager::ui::SystemAccessibilityPreferences;
    expect(filemanager::ui::effectiveAnimationsEnabled(
            true, SystemAccessibilityPreferences{false, true}),
        "User-enabled animation should remain active when Windows allows it");
    expect(!filemanager::ui::effectiveAnimationsEnabled(
            true, SystemAccessibilityPreferences{true, true})
            && !filemanager::ui::effectiveAnimationsEnabled(
                true, SystemAccessibilityPreferences{false, false}),
        "High Contrast and reduced-motion system settings should override animation");
    expect(!filemanager::ui::effectiveAnimationsEnabled(
            false, SystemAccessibilityPreferences{false, true}),
        "A disabled user animation preference should remain disabled");
    expect(filemanager::ui::effectiveFontPointSize(9.5, 12.0) == 12.0
            && filemanager::ui::effectiveFontPointSize(14.0, 12.0) == 14.0,
        "Theme font sizing should preserve the larger of the app and Windows UI fonts");

    using filemanager::files::naturalNameCompare;
    expect(naturalNameCompare(u"file2.txt", u"file10.txt") < 0,
        "natural ordering should compare numeric runs by value");
    expect(naturalNameCompare(u"File2.txt", u"file2.txt") != 0,
        "natural ordering should use case as a deterministic tie breaker");
    expect(naturalNameCompare(u"alpha", u"beta") < 0,
        "natural ordering should preserve ordinary alphabetic ordering");
    expect(naturalNameCompare(u"ملف2.txt", u"ملف10.txt") < 0,
        "natural ordering should recognize numeric runs in right-to-left Unicode names");
    const int emojiOrder = naturalNameCompare(u"📁-α.txt", u"📁-β.txt");
    expect(emojiOrder != 0
            && emojiOrder == -naturalNameCompare(u"📁-β.txt", u"📁-α.txt"),
        "Unicode natural ordering should remain deterministic and antisymmetric");
    const QLocale german(QLocale::German, QLocale::Germany);
    expect(filemanager::ui::localizedInteger(1234567, german)
                == german.toString(qint64{1234567})
            && filemanager::ui::localizedItems(1, german).endsWith(" item")
            && filemanager::ui::localizedItems(2, german).endsWith(" items"),
        "Counts should use locale digits and correct singular/plural labels");
    const QDateTime localizedDate(QDate(2026, 8, 28), QTime(14, 5));
    expect(filemanager::ui::localizedDateTime(localizedDate, QLocale::ShortFormat, german)
                == german.toString(localizedDate, QLocale::ShortFormat)
            && filemanager::ui::localizedDataSize(1536, german)
                == german.formattedDataSize(1536),
        "Dates and file sizes should delegate formatting to the active locale");

    const QString sensitiveDiagnosticPath = QStringLiteral(
        "C:/Users/Private Person/Secret Project/quarterly-plan.txt");
    filemanager::diagnostics::DiagnosticBundleInput diagnosticInput;
    diagnosticInput.productVersion = "0.7.0";
    diagnosticInput.releaseChannel = "Alpha";
    diagnosticInput.openPaths = {sensitiveDiagnosticPath};
    diagnosticInput.indexedRoots = {QStringLiteral("D:/Confidential")};
    diagnosticInput.eventCodes = {"app.started", "invalid event with path " + sensitiveDiagnosticPath};
    const QByteArray redactedBundle =
        filemanager::diagnostics::createDiagnosticBundle(diagnosticInput);
    expect(!redactedBundle.contains(sensitiveDiagnosticPath.toUtf8())
            && !redactedBundle.contains("quarterly-plan.txt")
            && !redactedBundle.contains("D:/Confidential")
            && redactedBundle.contains("\"openPathCount\": 1")
            && redactedBundle.contains("\"sensitivePathsIncluded\": false")
            && redactedBundle.contains("app.started")
            && !redactedBundle.contains("invalid event"),
        "Default diagnostics should expose counts and validated event codes without paths or names");
    const QByteArray sensitiveBundle =
        filemanager::diagnostics::createDiagnosticBundle(diagnosticInput, true);
    expect(sensitiveBundle.contains(sensitiveDiagnosticPath.toUtf8())
            && sensitiveBundle.contains("D:/Confidential")
            && sensitiveBundle.contains("\"sensitivePathsIncluded\": true"),
        "Diagnostic paths should appear only after explicit sensitive-data inclusion");

    phase("diagnostic_retention_begin");
    QTemporaryDir diagnosticDirectory;
    const QString diagnosticLogPath = diagnosticDirectory.filePath("events.json");
    QFile diagnosticFixture(diagnosticLogPath);
    expect(diagnosticFixture.open(QIODevice::WriteOnly),
        "The diagnostic retention fixture should be writable");
    const QJsonArray diagnosticEvents{
        QJsonObject{{"timestampUtc", QDateTime::currentDateTimeUtc().addDays(-8)
                                             .toString(Qt::ISODateWithMs)},
            {"code", "expired.event"}},
        QJsonObject{{"timestampUtc", QDateTime::currentDateTimeUtc()
                                             .toString(Qt::ISODateWithMs)},
            {"code", "current.event"}}};
    diagnosticFixture.write(QJsonDocument(QJsonObject{{"schema", 1},
                                      {"events", diagnosticEvents}})
                                .toJson(QJsonDocument::Compact));
    diagnosticFixture.close();
    phase("diagnostic_log_load_begin");
    filemanager::diagnostics::DiagnosticLog diagnosticLog(diagnosticLogPath);
    phase("diagnostic_log_load_finished");
    expect(diagnosticLog.eventCodes() == QStringList{"current.event"},
        "Diagnostic history should prune events older than seven days when loaded");
    QString diagnosticError;
    expect(!diagnosticLog.record("invalid." + sensitiveDiagnosticPath, &diagnosticError)
            && !diagnosticError.isEmpty(),
        "Diagnostic history should reject arbitrary messages and path-bearing event codes");
    phase("diagnostic_record_loop_begin");
    QElapsedTimer diagnosticRecordTime;
    qint64 diagnosticRecordTotalMs = 0;
    qint64 diagnosticRecordMaximumMs = 0;
    for (int event = 0;
         event < filemanager::diagnostics::DiagnosticLog::maximumEventCount + 4; ++event) {
        diagnosticRecordTime.start();
        const bool recorded = diagnosticLog.record(QString("test.event.%1").arg(event));
        const qint64 recordMs = diagnosticRecordTime.elapsed();
        diagnosticRecordTotalMs += recordMs;
        diagnosticRecordMaximumMs = qMax(diagnosticRecordMaximumMs, recordMs);
        expect(recorded,
            "A valid diagnostic event should be persisted atomically");
    }
    std::cout << "Diagnostic record timing: count="
              << filemanager::diagnostics::DiagnosticLog::maximumEventCount + 4
              << " total_ms=" << diagnosticRecordTotalMs
              << " maximum_ms=" << diagnosticRecordMaximumMs << std::endl;
    phase("diagnostic_record_loop_finished");
    expect(diagnosticLog.eventCount()
            == filemanager::diagnostics::DiagnosticLog::maximumEventCount,
        "Diagnostic history should enforce its hard event-count cap");
    QFile persistedDiagnostics(diagnosticLogPath);
    expect(persistedDiagnostics.open(QIODevice::ReadOnly)
            && !persistedDiagnostics.readAll().contains(sensitiveDiagnosticPath.toUtf8()),
        "A rejected diagnostic message must never reach persistent storage");
    persistedDiagnostics.close();
    phase("diagnostic_clear_begin");
    expect(diagnosticLog.clear(&diagnosticError) && diagnosticLog.eventCount() == 0
            && !QFileInfo::exists(diagnosticLogPath),
        "Clearing diagnostic history should remove the local file immediately");
    phase("diagnostic_clear_finished_preferences_begin");

    filemanager::settings::PortablePreferences portablePreferences;
    portablePreferences.fontPointSize = 12.0;
    portablePreferences.density = "Spacious";
    portablePreferences.animations = false;
    portablePreferences.singleClickActivation = true;
    portablePreferences.shortcuts = {{"file.copy", "Ctrl+C"}, {"file.rename", "F2"}};
    portablePreferences.pinnedContextActions = {"file.copy", "file.rename"};
    const QSet<QString> portableCommands{"file.copy", "file.rename", "file.delete"};
    const QByteArray portableBytes =
        filemanager::settings::createPortablePreferences(portablePreferences);
    filemanager::settings::PortablePreferences importedPreferences;
    QString portableError;
    expect(filemanager::settings::parsePortablePreferences(portableBytes, portableCommands,
               &importedPreferences, &portableError)
            && importedPreferences.fontPointSize == 12.0
            && importedPreferences.density == "Spacious"
            && importedPreferences.shortcuts == portablePreferences.shortcuts
            && importedPreferences.pinnedContextActions
                == portablePreferences.pinnedContextActions
            && !portableBytes.contains("privacy")
            && !portableBytes.contains(sensitiveDiagnosticPath.toUtf8()),
        "Portable settings should round-trip only the bounded appearance and interaction schema");
    QJsonObject unknownCommandDocument = QJsonDocument::fromJson(portableBytes).object();
    QJsonObject unknownShortcuts = unknownCommandDocument.value("shortcuts").toObject();
    unknownShortcuts.insert("unknown.command", "Ctrl+Alt+X");
    unknownCommandDocument.insert("shortcuts", unknownShortcuts);
    expect(!filemanager::settings::parsePortablePreferences(
               QJsonDocument(unknownCommandDocument).toJson(), portableCommands,
               &importedPreferences, &portableError),
        "Portable settings should reject unknown command identifiers before applying anything");
    QJsonObject secretDocument = QJsonDocument::fromJson(portableBytes).object();
    secretDocument.insert("privacy", QJsonObject{{"allowWindowsShellHandlers", true}});
    expect(!filemanager::settings::parsePortablePreferences(
               QJsonDocument(secretDocument).toJson(), portableCommands,
               &importedPreferences, &portableError),
        "Portable settings should reject privacy permissions and unknown top-level fields");
    const QString portablePath = diagnosticDirectory.filePath("preferences.json");
    expect(filemanager::settings::writePortablePreferences(
               portablePath, portablePreferences, &portableError)
            && filemanager::settings::readPortablePreferences(portablePath, portableCommands,
                &importedPreferences, &portableError),
        "Portable settings should use the atomic file boundary for export and validated import");

    phase("preferences_finished_storage_discovery_begin");
    const auto storageVolumes = filemanager::files::mountedStorageVolumes();
    phase("storage_discovery_finished");
    expect(!storageVolumes.isEmpty(), "At least one ready storage volume should be discoverable");
    QSet<QString> storageRoots;
    for (const auto& volume : storageVolumes) {
        expect(!volume.rootPath.isEmpty() && !volume.displayName.isEmpty(),
            "Discovered storage volumes should have a root and display label");
        storageRoots.insert(volume.rootPath.toCaseFolded());
    }
    expect(storageRoots.size() == storageVolumes.size(), "Storage volume roots should be unique");
    using filemanager::files::StorageLocationKind;
    expect(filemanager::files::classifyStorageDriveType(2).kind
                == StorageLocationKind::Removable
            && filemanager::files::classifyStorageDriveType(2).safeEjectRecommended,
        "Windows removable drives should advertise safe-eject guidance");
    expect(filemanager::files::classifyStorageDriveType(4).kind
                == StorageLocationKind::Network
            && filemanager::files::classifyStorageDriveType(4).reconnectable,
        "Windows mapped drives should retain reconnectable network semantics");
    expect(filemanager::files::storageLocationTraits(
               QStringLiteral("\\\\offline-server\\share\\folder")).kind
                == StorageLocationKind::Network
            && filemanager::files::storageLocationTraits(
                QStringLiteral("\\\\offline-server\\share\\folder")).reconnectable,
        "UNC paths should be classified syntactically without probing or authenticating to a host");

    const auto transferProgress = filemanager::files::TransferProgress::calculate(
        25 * 1024 * 1024, 100 * 1024 * 1024, 5000, "payload.bin");
    expect(transferProgress.bytesPerSecond == 5 * 1024 * 1024,
        "transfer throughput should be derived from copied bytes and elapsed time");
    expect(transferProgress.remainingSeconds == 15,
        "transfer ETA should round remaining bytes to whole seconds");
    const auto unknownProgress = filemanager::files::TransferProgress::calculate(0, 100, 0);
    expect(unknownProgress.bytesPerSecond == 0 && unknownProgress.remainingSeconds == -1,
        "transfer throughput and ETA should remain unknown before progress begins");
    expect(filemanager::files::hasSufficientTransferSpace(1024, 1024),
        "transfer preflight should accept exactly sufficient free space");
    expect(!filemanager::files::hasSufficientTransferSpace(1025, 1024),
        "transfer preflight should reject a known free-space shortfall");
    using filemanager::files::TransferFailureCategory;
    using filemanager::files::classifyNativeTransferFailure;
    expect(classifyNativeTransferFailure(112) == TransferFailureCategory::InsufficientSpace,
        "Windows disk-full failures should retain a typed category");
    expect(classifyNativeTransferFailure(5) == TransferFailureCategory::PermissionDenied,
        "Windows access-denied failures should retain a typed category");
    expect(classifyNativeTransferFailure(32) == TransferFailureCategory::SharingViolation,
        "Windows sharing violations should retain a typed category");
    expect(classifyNativeTransferFailure(1167) == TransferFailureCategory::DeviceDisconnected,
        "Windows disconnected-device failures should retain a typed category");
    expect(classifyNativeTransferFailure(995) == TransferFailureCategory::Cancelled,
        "Windows operation cancellation should retain a typed category");
    expect(filemanager::files::isRetryableTransferFailure(
            TransferFailureCategory::DeviceDisconnected)
            && !filemanager::files::isRetryableTransferFailure(
                TransferFailureCategory::InvalidRequest),
        "Only failures that can change without changing the request should be retryable");
    filemanager::files::TransferRequest elevationCopy{
        {QStringLiteral("C:/source.txt")}, QStringLiteral("C:/protected"),
        filemanager::files::TransferOperation::Copy};
    filemanager::files::TransferFailure permissionFailure{
        QStringLiteral("C:/source.txt"), QStringLiteral("C:/protected/source.txt"),
        QStringLiteral("Access denied"), true, TransferFailureCategory::PermissionDenied, 5};
    expect(filemanager::files::isElevationEligibleCopy(elevationCopy, {permissionFailure}),
        "Only an explicitly failed copy with permission-denied evidence should be elevation-eligible");
    auto writeProtectedFailure = permissionFailure;
    writeProtectedFailure.nativeCode = 19;
    expect(!filemanager::files::isElevationEligibleCopy(elevationCopy, {writeProtectedFailure}),
        "Elevation should not be offered for write-protected media");
    auto sharingFailure = permissionFailure;
    sharingFailure.category = TransferFailureCategory::SharingViolation;
    expect(!filemanager::files::isElevationEligibleCopy(elevationCopy, {sharingFailure}),
        "Elevation should never be offered as an in-use file workaround");
    elevationCopy.operation = filemanager::files::TransferOperation::Move;
    expect(!filemanager::files::isElevationEligibleCopy(elevationCopy, {permissionFailure}),
        "Destructive move retries should never be elevation-eligible");
    expect(filemanager::ui::transferProgressAnnouncementPercent(0, 100) == 0
            && filemanager::ui::transferProgressAnnouncementPercent(24, 100) == 0
            && filemanager::ui::transferProgressAnnouncementPercent(25, 100) == 25
            && filemanager::ui::transferProgressAnnouncementPercent(99, 100) == 75
            && filemanager::ui::transferProgressAnnouncementPercent(100, 100) == 100
            && filemanager::ui::transferProgressAnnouncementPercent(1, 0) == -1,
        "Transfer progress announcements should be bounded to 25-percent milestones");
    expect(filemanager::ui::transferFinishedAnnouncement(false, false, 0)
                == QStringLiteral("Copy completed.")
            && filemanager::ui::transferFinishedAnnouncement(true, true, 0)
                == QStringLiteral("Move cancelled.")
            && filemanager::ui::transferFinishedAnnouncement(false, false, 2)
                .contains(QStringLiteral("2 item failure")),
        "Transfer completion announcements should distinguish success, cancellation and failure");

    using filemanager::files::validateWindowsName;
    expect(validateWindowsName("notes.txt").valid, "ordinary Windows name should be valid");
    expect(!validateWindowsName("CON.txt").valid, "reserved Windows device name should fail");
    expect(!validateWindowsName("CONIN$.log").valid,
        "reserved Windows console device name should fail");
    expect(!validateWindowsName(QString::fromUtf8("COM¹.txt")).valid,
        "reserved Windows superscript device name should fail");
    expect(!validateWindowsName("bad:name.txt").valid, "invalid Windows character should fail");
    expect(!validateWindowsName("trailing. ").valid, "trailing period or space should fail");
    expect(!validateWindowsName(QString(256, 'a')).valid,
        "Windows component length should be bounded without imposing legacy MAX_PATH");

    phase("filesystem_operations_begin");
    QTemporaryDir temporary;
    expect(temporary.isValid(), "temporary test directory should be created");
    const QString source = temporary.filePath("source.txt");
    QFile sourceFile(source);
    expect(sourceFile.open(QIODevice::WriteOnly), "source fixture should open");
    sourceFile.write("verified payload");
    sourceFile.close();
    const QString copy = temporary.filePath("copy.txt");
    expect(filemanager::files::copy(source, copy).ok, "file copy should succeed");
    QFile copied(copy);
    expect(copied.open(QIODevice::ReadOnly), "copied fixture should open");
    expect(copied.readAll() == "verified payload", "file copy should preserve contents");
    expect(!filemanager::files::copy(source, copy).ok, "copy should not overwrite an existing target");

    const QString caseOriginal = temporary.filePath("CaseOnly.txt");
    const QString caseDestination = temporary.filePath("caseonly.txt");
    QFile caseFixture(caseOriginal);
    expect(caseFixture.open(QIODevice::WriteOnly), "case-only rename fixture should open");
    caseFixture.write("case retained");
    caseFixture.close();
    const auto caseRename = filemanager::files::renameItem(caseOriginal, caseDestination);
    const QStringList caseEntries = QDir(temporary.path()).entryList(
        {QStringLiteral("caseonly.txt")}, QDir::Files, QDir::Name);
    expect(caseRename.ok && QFileInfo::exists(caseDestination)
            && caseEntries.contains(QStringLiteral("caseonly.txt"), Qt::CaseSensitive),
        "single-item case-only rename should stage safely and preserve requested casing");
    const auto invalidRename = filemanager::files::renameItem(
        caseDestination, temporary.filePath("NUL.txt"));
    expect(!invalidRename.ok && QFileInfo::exists(caseDestination),
        "rename should reject a reserved destination without mutating its source");

    QString hashError;
    expect(filemanager::files::sha256File(source, &hashError)
            == "3aac0a1146ffe55bac7c05f61401fb1e7e4e6a94110b91585c646fe8cf745f28",
        "SHA-256 should match the known fixture digest");
    expect(hashError.isEmpty(), "successful hashing should not report an error");

    const QString folder = temporary.filePath("folder");
    QDir().mkpath(folder);
    QFile nestedFile(QDir(folder).filePath("nested.bin"));
    expect(nestedFile.open(QIODevice::WriteOnly), "nested analysis fixture should open");
    nestedFile.write(QByteArray(37, 'x'));
    nestedFile.close();
    const auto statistics = filemanager::files::measurePath(folder);
    expect(statistics.error.isEmpty(), "folder analysis should succeed");
    expect(statistics.files == 1 && statistics.bytes == 37,
        "folder analysis should count files and bytes");
    const QString duplicate = QDir(folder).filePath("duplicate.bin");
    expect(filemanager::files::copy(nestedFile.fileName(), duplicate).ok,
        "duplicate fixture should be copied");
    QFile different(QDir(folder).filePath("different.bin"));
    expect(different.open(QIODevice::WriteOnly), "different fixture should open");
    different.write(QByteArray(37, 'y'));
    different.close();
    const auto duplicates = filemanager::files::findDuplicates(folder);
    expect(duplicates.filesScanned == 3, "duplicate scan should inspect each regular file");
    expect(duplicates.groups.size() == 1 && duplicates.groups.first().paths.size() == 2,
        "duplicate scan should require full-content equality after size grouping");
    const quint64 reclaimable = filemanager::files::duplicateReclaimableBytes(duplicates);
    expect(reclaimable == 37,
        "duplicate review should report only redundant copies as potentially reclaimable bytes");
    filemanager::files::DuplicateScanResult reportFixture;
    reportFixture.groups = {{42, "abcdef", {"=formula", "C:/two,\"quoted\".txt"}}};
    const QByteArray csv = filemanager::files::duplicateReportCsv(reportFixture);
    expect(csv.startsWith("\xEF\xBB\xBFGroup,Bytes per file,SHA-256,Path\r\n")
            && csv.contains("\"'=formula\"")
            && csv.contains("\"C:/two,\"\"quoted\"\".txt\""),
        "duplicate CSV should be UTF-8, quote fields correctly and neutralize spreadsheet formulas");
    QFile originalDuplicateSource(nestedFile.fileName());
    QFile originalDuplicateCopy(duplicate);
    expect(originalDuplicateSource.open(QIODevice::ReadOnly)
            && originalDuplicateCopy.open(QIODevice::ReadOnly),
        "duplicate report mutation fixtures should be readable");
    const QByteArray sourceBeforeExport = originalDuplicateSource.readAll();
    const QByteArray copyBeforeExport = originalDuplicateCopy.readAll();
    originalDuplicateSource.close();
    originalDuplicateCopy.close();
    const QString duplicateReportPath = temporary.filePath("duplicate-report.csv");
    QString duplicateReportError;
    expect(filemanager::files::writeDuplicateReportCsv(
            duplicateReportPath, duplicates, &duplicateReportError)
            && duplicateReportError.isEmpty() && QFileInfo(duplicateReportPath).size() > 0,
        "duplicate review should atomically export a deterministic CSV report");
    expect(originalDuplicateSource.open(QIODevice::ReadOnly)
            && originalDuplicateCopy.open(QIODevice::ReadOnly)
            && originalDuplicateSource.readAll() == sourceBeforeExport
            && originalDuplicateCopy.readAll() == copyBeforeExport,
        "duplicate report export must not modify or delete any scanned file");
    originalDuplicateSource.close();
    originalDuplicateCopy.close();
    expect(!filemanager::files::copy(folder, QDir(folder).filePath("nested")).ok,
        "folder copy into itself should fail before mutation");
    expect(!filemanager::files::move(folder, folder).ok,
        "folder move onto itself should fail before mutation");
    const auto missingTrash = filemanager::files::moveToTrashDetailed(
        temporary.filePath("missing-trash-item.txt"));
    expect(!missingTrash.ok && !missingTrash.originalPath.isEmpty()
            && !missingTrash.error.isEmpty(),
        "Recycle Bin operations should reject a missing source with a typed result.");
    const auto emptyPermanentDelete =
        filemanager::platform::RecycleBinOperation::permanentlyDelete({});
    expect(!emptyPermanentDelete.success && !emptyPermanentDelete.error.isEmpty(),
        "Permanent deletion should reject an empty selection before entering Windows.");
    const auto missingPermanentDelete =
        filemanager::platform::RecycleBinOperation::permanentlyDelete(
            {temporary.filePath("missing-permanent-delete.txt")});
    expect(!missingPermanentDelete.success
            && missingPermanentDelete.deletedPaths.isEmpty()
            && missingPermanentDelete.remainingPaths.size() == 1,
        "Permanent deletion should reject unavailable items without reporting deletion.");
    const auto rootPermanentDelete =
        filemanager::platform::RecycleBinOperation::permanentlyDelete({QDir::rootPath()});
    expect(!rootPermanentDelete.success
            && rootPermanentDelete.error.contains("roots cannot"),
        "Permanent deletion should categorically reject a filesystem root.");
    filemanager::platform::NativeVolumeCapabilities limitedVolume;
    limitedVolume.known = true;
    limitedVolume.fileSystemName = "FAT32";
    const QStringList metadataLoss = filemanager::platform::unsupportedNativeMetadata(
        true, true, true, true, limitedVolume);
    expect(metadataLoss.size() == 4
            && metadataLoss.join(' ').contains("EFS")
            && metadataLoss.join(' ').contains("alternate data streams", Qt::CaseInsensitive)
            && metadataLoss.join(' ').contains("Sparse")
            && metadataLoss.join(' ').contains("permissions"),
        "Cross-filesystem preflight should enumerate every known metadata degradation.");
    limitedVolume.encryption = true;
    limitedVolume.namedStreams = true;
    limitedVolume.sparseFiles = true;
    limitedVolume.persistentAcls = true;
    expect(filemanager::platform::unsupportedNativeMetadata(
            true, true, true, true, limitedVolume).isEmpty(),
        "A capable destination should not require metadata-degradation confirmation.");
#if QT_VERSION >= QT_VERSION_CHECK(6, 9, 0)
    expect(QFile::supportsMoveToTrash(),
        "The supported Windows build should expose the native Recycle Bin operation.");
#endif

    const QString renameFolder = temporary.filePath("rename-fixtures");
    QDir().mkpath(renameFolder);
    const QString renameFirst = QDir(renameFolder).filePath("alpha.txt");
    const QString renameSecond = QDir(renameFolder).filePath("beta.txt");
    QFile firstRenameFile(renameFirst);
    QFile secondRenameFile(renameSecond);
    expect(firstRenameFile.open(QIODevice::WriteOnly), "first rename fixture should open");
    firstRenameFile.write("alpha");
    firstRenameFile.close();
    expect(secondRenameFile.open(QIODevice::WriteOnly), "second rename fixture should open");
    secondRenameFile.write("beta");
    secondRenameFile.close();
    const auto renamePlan = filemanager::files::createBatchRenamePlan(
        {renameFirst, renameSecond}, {.prefix = "reviewed-"});
    expect(renamePlan.isValid() && renamePlan.items.size() == 2,
        "batch rename should produce a validated preview plan");
    const auto renameResult = filemanager::files::executeBatchRename(renamePlan);
    expect(renameResult.ok && renameResult.renamed == 2
            && QFileInfo::exists(QDir(renameFolder).filePath("reviewed-alpha.txt"))
            && QFileInfo::exists(QDir(renameFolder).filePath("reviewed-beta.txt")),
        "batch rename should commit every staged item");
    const auto collisionPlan = filemanager::files::createBatchRenamePlan(
        {QDir(renameFolder).filePath("reviewed-alpha.txt"),
         QDir(renameFolder).filePath("reviewed-beta.txt")},
        {.find = "reviewed-alpha", .replace = "reviewed-beta"});
    expect(!collisionPlan.isValid() && !collisionPlan.errors.isEmpty(),
        "batch rename should reject collisions before mutation");
    const QString batchCaseSource = QDir(renameFolder).filePath("MixedCase.txt");
    QFile batchCaseFixture(batchCaseSource);
    expect(batchCaseFixture.open(QIODevice::WriteOnly), "batch case-only fixture should open");
    batchCaseFixture.write("mixed");
    batchCaseFixture.close();
    const auto batchCasePlan = filemanager::files::createBatchRenamePlan(
        {batchCaseSource}, {.find = "MixedCase", .replace = "mixedcase"});
    const auto batchCaseResult = filemanager::files::executeBatchRename(batchCasePlan);
    const QStringList batchCaseEntries = QDir(renameFolder).entryList(
        {QStringLiteral("mixedcase.txt")}, QDir::Files, QDir::Name);
    expect(batchCaseResult.ok
            && batchCaseEntries.contains(QStringLiteral("mixedcase.txt"), Qt::CaseSensitive),
        "batch rename should preserve a requested case-only change through staging");

    const QString batchRulesFolder = temporary.filePath("batch-rule-fixtures");
    QDir().mkpath(batchRulesFolder);
    const QString batchRulesFirst = QDir(batchRulesFolder).filePath("draft Report 20.txt");
    const QString batchRulesSecond = QDir(batchRulesFolder).filePath("draft notes 30.txt");
    for (const QString& path : {batchRulesFirst, batchRulesSecond}) {
        QFile fixture(path);
        expect(fixture.open(QIODevice::WriteOnly), "batch rule fixture should open");
        fixture.write("rename rules");
        fixture.close();
    }
    const auto completeRulesPlan = filemanager::files::createBatchRenamePlan(
        {batchRulesFirst, batchRulesSecond},
        {.find = QStringLiteral("^draft\\s+(.+)\\s+(\\d+)$"),
         .replace = QStringLiteral("\\1-v\\2"),
         .useRegularExpression = true,
         .caseSensitive = false,
         .prefix = QStringLiteral("approved-"),
         .caseConversion = filemanager::files::BatchRenameCase::TitleCase,
         .addSequence = true,
         .sequenceStart = 7,
         .sequencePadding = 2,
         .sequenceStep = 2});
    expect(completeRulesPlan.isValid() && completeRulesPlan.items.size() == 2
            && QFileInfo(completeRulesPlan.items[0].destination).fileName()
                == QStringLiteral("approved-Report-V20_07.txt")
            && QFileInfo(completeRulesPlan.items[1].destination).fileName()
                == QStringLiteral("approved-Notes-V30_09.txt"),
        "batch rename should preview regex captures, title case and configurable numbering");
    const auto completeRulesResult = filemanager::files::executeBatchRename(completeRulesPlan);
    expect(completeRulesResult.ok && completeRulesResult.renamed == 2,
        "complete batch rules should commit through the existing staged rename engine");
    const auto invalidExpressionPlan = filemanager::files::createBatchRenamePlan(
        {completeRulesPlan.items[0].destination},
        {.find = QStringLiteral("(["), .useRegularExpression = true});
    expect(!invalidExpressionPlan.isValid() && !invalidExpressionPlan.errors.isEmpty(),
        "invalid batch-rename regular expressions should fail before filesystem mutation");

    const QString unicodeSource = QDir(batchRulesFolder).filePath(
        QStringLiteral("ملف-日本語-😀.txt"));
    QFile unicodeFixture(unicodeSource);
    expect(unicodeFixture.open(QIODevice::WriteOnly),
        "Unicode filename fixture should be created without lossy conversion");
    unicodeFixture.write("unicode path");
    unicodeFixture.close();
    const auto unicodeRenamePlan = filemanager::files::createBatchRenamePlan(
        {unicodeSource}, {.prefix = QStringLiteral("مراجعة-")});
    const auto unicodeRenameResult = filemanager::files::executeBatchRename(unicodeRenamePlan);
    expect(unicodeRenameResult.ok
            && QFileInfo::exists(QDir(batchRulesFolder).filePath(
                QStringLiteral("مراجعة-ملف-日本語-😀.txt"))),
        "Batch rename should preserve RTL, CJK and supplementary Unicode filename text");

    const QStringList sequenceSources = {
        completeRulesPlan.items[0].destination, completeRulesPlan.items[1].destination,
        unicodeRenamePlan.items[0].destination};
    for (const int direction : {1, -1}) {
        const auto wideSequencePlan = filemanager::files::createBatchRenamePlan(
            sequenceSources, {.addSequence = true,
                .sequenceStart = direction * 999999999,
                .sequencePadding = 3,
                .sequenceStep = direction * 999999999});
        expect(wideSequencePlan.isValid() && wideSequencePlan.items.size() == 3,
            "large sequence values allowed by the dialog should produce a valid preview");
        if (wideSequencePlan.items.size() == 3) {
            for (int index = 0; index < 3; ++index) {
                const qint64 expectedNumber = qint64(direction) * 999999999 * (index + 1);
                expect(wideSequencePlan.items[index].destination.endsWith(
                           QStringLiteral("_%1.txt").arg(expectedNumber)),
                    "batch sequence should remain exact beyond the signed 32-bit range");
                expect(QFileInfo::exists(wideSequencePlan.items[index].source)
                        && !QFileInfo::exists(wideSequencePlan.items[index].destination),
                    "large-number preview must not rename any source");
            }
        }
    }

    const QString orderedFolder = temporary.filePath("ordered-rename");
    QDir().mkpath(orderedFolder);
    const QStringList orderedSources = {QDir(orderedFolder).filePath("last.pdf"),
        QDir(orderedFolder).filePath("first.txt"), QDir(orderedFolder).filePath("middle")};
    for (int index = 0; index < orderedSources.size(); ++index) {
        QFile fixture(orderedSources[index]);
        expect(fixture.open(QIODevice::WriteOnly), "ordered rename fixture should open");
        fixture.write(QByteArray::number(index));
    }
    const auto chapterPlan = filemanager::files::createNumberedRenamePlan(
        orderedSources, "Chapter", 7, 3, 2);
    expect(chapterPlan.isValid() && chapterPlan.items.size() == 3
            && chapterPlan.items[0].destination.endsWith("Chapter 007.pdf")
            && chapterPlan.items[1].destination.endsWith("Chapter 009.txt")
            && chapterPlan.items[2].destination.endsWith("Chapter 011"),
        "chapter numbering must follow explicit input order and preserve each extension");
    expect(!filemanager::files::createNumberedRenamePlan(orderedSources, "", 1, 3, 1).isValid()
            && !filemanager::files::createNumberedRenamePlan(orderedSources, "Chapter", 1, 3, 0).isValid(),
        "chapter naming should reject an empty label and zero step");
    expect(!filemanager::files::createListedRenamePlan(orderedSources, {"One"}).isValid(),
        "a name list must have exactly as many rows as selected sources");
    for (const QString& invalid : {QString(), QString(".."), QString("../outside"),
             QString("NUL"), QString("trailing ")}) {
        expect(!filemanager::files::createListedRenamePlan(
                   orderedSources, {"One", invalid, "Three"}).isValid(),
            "invalid list rows must block the entire rename plan");
    }
    const auto listedPlan = filemanager::files::createListedRenamePlan(
        orderedSources, {QString::fromUtf8("البداية"), "The Journey", "Ending"});
    expect(listedPlan.isValid() && listedPlan.items[0].destination.endsWith(QString::fromUtf8("البداية.pdf"))
            && listedPlan.items[1].destination.endsWith("The Journey.txt"),
        "name lists should preserve Unicode and per-file extensions");
    expect(!filemanager::files::createListedRenamePlan(
               orderedSources, {"Same.txt", "same.txt", "Other.txt"}, false).isValid(),
        "full-name list duplicates should be refused case insensitively");
    QFile lateConflict(chapterPlan.items[1].destination);
    expect(lateConflict.open(QIODevice::WriteOnly), "late collision fixture should open");
    lateConflict.write("unrelated");
    lateConflict.close();
    const auto lateResult = filemanager::files::executeBatchRename(chapterPlan);
    expect(!lateResult.ok && lateResult.renamed == 0
            && QFileInfo::exists(orderedSources[0]) && QFileInfo::exists(orderedSources[1])
            && QFileInfo::exists(orderedSources[2]),
        "a collision appearing after preview must fail before staging any source");
    const auto listedResult = filemanager::files::executeBatchRename(listedPlan);
    expect(listedResult.ok && listedResult.renamed == 3,
        "the reviewed list should commit all names through staged renaming");
    for (int index = 0; index < listedPlan.items.size(); ++index) {
        QFile renamed(listedPlan.items[index].destination);
        expect(renamed.open(QIODevice::ReadOnly) && renamed.readAll() == QByteArray::number(index),
            "list renaming must preserve the exact file-to-name mapping and bytes");
    }
    const QStringList swapSources = {listedPlan.items[0].destination, listedPlan.items[1].destination};
    const auto swapPlan = filemanager::files::createListedRenamePlan(swapSources,
        {QFileInfo(swapSources[1]).fileName(), QFileInfo(swapSources[0]).fileName()}, false);
    const auto swapResult = filemanager::files::executeBatchRename(swapPlan);
    expect(swapResult.ok && swapResult.renamed == 2,
        "explicit full-name swaps should succeed through the shared staging engine");
    QFile swapped(swapSources[1]);
    expect(swapped.open(QIODevice::ReadOnly) && swapped.readAll() == "0",
        "a staged name swap must not overwrite either selected file");
    swapped.close();

    phase("stale_rename_guards_begin");
    for (int staleMode = 0; staleMode < 3; ++staleMode) {
        QTemporaryDir staleRoot;
        const QString first = staleRoot.filePath("first.txt");
        const QString second = staleRoot.filePath("second.txt");
        for (const auto& path : {first, second}) {
            QFile staleFixture(path);
            expect(staleFixture.open(QIODevice::WriteOnly), "stale rename fixture should open");
            staleFixture.write("original");
        }
        // Normalize to a whole millisecond before capture, so replacement metadata matches
        // even though the native snapshot retains Windows' full timestamp precision.
        const QDateTime originalTime = QFileInfo(second).lastModified();
        QFile timestampFixture(second);
        expect(timestampFixture.open(QIODevice::ReadWrite)
                && timestampFixture.setFileTime(originalTime, QFileDevice::FileModificationTime),
            "original fixture timestamp should be normalized before preview");
        timestampFixture.close();
        const auto stalePlan = staleMode == 0
            ? filemanager::files::createBatchRenamePlan({first, second}, {.prefix = "renamed-"})
            : staleMode == 1
                ? filemanager::files::createNumberedRenamePlan({first, second}, "Chapter")
                : filemanager::files::createListedRenamePlan({first, second}, {"One", "Two"});
        expect(QFile::rename(second, staleRoot.filePath("preserved-original.txt")),
            "the source replacement fixture must preserve the original under another name");
        QFile replacement(second);
        expect(replacement.open(QIODevice::WriteOnly), "replacement fixture should open");
        replacement.write("original");
        replacement.close();
        expect(replacement.open(QIODevice::ReadWrite)
                && replacement.setFileTime(originalTime, QFileDevice::FileModificationTime),
            "replacement should share the old source size and timestamp");
        replacement.close();
        const auto replacementPlan = filemanager::files::createNumberedRenamePlan({second}, "Replacement");
        const auto& expectedSnapshot = stalePlan.items[1].sourceSnapshot;
        const auto& replacementSnapshot = replacementPlan.items[0].sourceSnapshot;
        expect(expectedSnapshot.valid && replacementSnapshot.valid
                && expectedSnapshot.size == replacementSnapshot.size
                && expectedSnapshot.lastWriteTime == replacementSnapshot.lastWriteTime
                && expectedSnapshot.attributes == replacementSnapshot.attributes
                && expectedSnapshot.fileIndex != replacementSnapshot.fileIndex,
            "replacement must have identical metadata but a distinct native file identity");
        const auto staleResult = filemanager::files::executeBatchRename(stalePlan);
        expect(!staleResult.ok && staleResult.renamed == 0
                && QFileInfo::exists(first) && QFileInfo::exists(second)
                && !QFileInfo::exists(stalePlan.items[0].destination)
                && !QFileInfo::exists(stalePlan.items[1].destination),
            "all rename modes must reject a replaced source before touching any file, even with matching metadata");
    }
    {
        QTemporaryDir editedRoot;
        const QString editedPath = editedRoot.filePath("edited.txt");
        QFile editedFixture(editedPath);
        expect(editedFixture.open(QIODevice::WriteOnly), "edited source fixture should open");
        editedFixture.write("before");
        editedFixture.close();
        const auto editedPlan = filemanager::files::createNumberedRenamePlan({editedPath}, "Chapter");
        expect(editedFixture.open(QIODevice::Append), "source should be editable after preview");
        editedFixture.write(" and after");
        editedFixture.close();
        const auto editedResult = filemanager::files::executeBatchRename(editedPlan);
        expect(!editedResult.ok && editedResult.renamed == 0 && QFileInfo::exists(editedPath),
            "a source edited after preview must require a fresh rename review");
    }

    {
        phase("directory_rename_begin");
        QTemporaryDir directoryRenameRoot;
        const QString directoryPath = directoryRenameRoot.filePath("original-folder");
        expect(QDir().mkdir(directoryPath), "directory rename fixture should be created");
        const auto directoryPlan = filemanager::files::createNumberedRenamePlan({directoryPath}, "Folder");
        const auto directoryResult = filemanager::files::executeBatchRename(directoryPlan);
        expect(directoryResult.ok && directoryResult.renamed == 1
                && QFileInfo(directoryRenameRoot.filePath("Folder 001")).isDir(),
            "unchanged directories should retain their identity through staging and commit");
        const QString replacedDirectory = directoryRenameRoot.filePath("Folder 001");
        const auto replacedDirectoryPlan = filemanager::files::createNumberedRenamePlan(
            {replacedDirectory}, "Replacement");
        expect(QDir().rename(replacedDirectory, directoryRenameRoot.filePath("saved-folder"))
                && QDir().mkdir(replacedDirectory),
            "directory replacement fixture should preserve the old directory");
        const auto replacedDirectoryResult = filemanager::files::executeBatchRename(replacedDirectoryPlan);
        expect(!replacedDirectoryResult.ok && replacedDirectoryResult.renamed == 0
                && QFileInfo(replacedDirectory).isDir(),
            "batch rename must reject a replacement directory");
        auto missingSnapshotPlan = filemanager::files::createNumberedRenamePlan({replacedDirectory}, "Verified");
        missingSnapshotPlan.items[0].sourceSnapshot = {};
        expect(!filemanager::files::executeBatchRename(missingSnapshotPlan).ok
                && QFileInfo(replacedDirectory).isDir(),
            "a plan without a verified source snapshot must fail closed");
    }

#ifdef Q_OS_WIN
    {
        QTemporaryDir lockedRenameRoot;
        const QStringList lockedPaths = {lockedRenameRoot.filePath("first.txt"),
            lockedRenameRoot.filePath("locked.txt")};
        for (const auto& path : lockedPaths) {
            QFile lockedFixture(path);
            expect(lockedFixture.open(QIODevice::WriteOnly), "locked rename fixture should open");
            lockedFixture.write("keep these bytes");
        }
        const auto lockedPlan = filemanager::files::createNumberedRenamePlan(lockedPaths, "Chapter");
        const QString nativeLocked = QDir::toNativeSeparators(lockedPaths[1]);
        const HANDLE renameLock = CreateFileW(reinterpret_cast<LPCWSTR>(nativeLocked.utf16()),
            GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0, nullptr);
        expect(renameLock != INVALID_HANDLE_VALUE, "fixture must prevent deletion/rename but allow metadata checks");
        const QString lockProbe = QDir::toNativeSeparators(lockedRenameRoot.filePath("lock-probe.txt"));
        expect(!MoveFileExW(reinterpret_cast<LPCWSTR>(nativeLocked.utf16()),
                reinterpret_cast<LPCWSTR>(lockProbe.utf16()), 0)
                && GetLastError() == ERROR_SHARING_VIOLATION,
            "fixture must prove its live read handle actually denies native rename");
        const auto lockedResult = filemanager::files::executeBatchRename(lockedPlan);
        if (renameLock != INVALID_HANDLE_VALUE)
            CloseHandle(renameLock);
        expect(!lockedResult.ok && lockedResult.renamed == 0
                && lockedResult.errors.join('\n').contains("Could not retain")
                && !lockedResult.errors.join('\n').contains("Manual recovery")
                && QDir(lockedRenameRoot.path()).entryList(QDir::Files | QDir::Hidden).size() == 2,
            "a locked source should refuse the batch before any item is staged");
        for (const auto& path : lockedPaths) {
            QFile restoredFixture(path);
            expect(restoredFixture.open(QIODevice::ReadOnly) && restoredFixture.readAll() == "keep these bytes",
                "rollback after a sharing violation must restore original names and bytes");
        }
    }
    {
        QTemporaryDir retainedRoot;
        const QString retainedSource = retainedRoot.filePath("source.txt");
        const QString diverted = retainedRoot.filePath("diverted.txt");
        QFile fixture(retainedSource);
        expect(fixture.open(QIODevice::WriteOnly), "retained-handle fixture should open");
        fixture.write("retained object");
        fixture.close();
        const auto plan = filemanager::files::createNumberedRenamePlan({retainedSource}, "Chapter");
        const auto& planned = plan.items.first();
        bool pathInterferenceBlocked = false;
        const auto guarded = filemanager::platform::guardedBatchRename(
            {{planned.source, planned.destination, planned.directory, planned.sourceSnapshot}},
            {[&] {
                const QString nativeSource = QDir::toNativeSeparators(retainedSource);
                const QString nativeDiverted = QDir::toNativeSeparators(diverted);
                pathInterferenceBlocked = !MoveFileExW(
                    reinterpret_cast<LPCWSTR>(nativeSource.utf16()),
                    reinterpret_cast<LPCWSTR>(nativeDiverted.utf16()), 0)
                    && GetLastError() == ERROR_SHARING_VIOLATION;
            }, {}});
        QFile destination(planned.destination);
        expect(pathInterferenceBlocked && guarded.ok && guarded.renamed == 1
                && !QFileInfo::exists(retainedSource) && !QFileInfo::exists(diverted)
                && destination.open(QIODevice::ReadOnly)
                && destination.readAll() == "retained object",
            "retained source handles should close the final-check-to-staging replacement race");
    }
    {
        QTemporaryDir collisionRoot;
        const QStringList sources{collisionRoot.filePath("first.txt"),
            collisionRoot.filePath("second.txt")};
        for (int index = 0; index < sources.size(); ++index) {
            QFile fixture(sources.at(index));
            expect(fixture.open(QIODevice::WriteOnly), "late-commit collision fixture should open");
            fixture.write(QByteArray::number(index));
        }
        const auto plan = filemanager::files::createNumberedRenamePlan(sources, "Chapter");
        QList<filemanager::platform::GuardedBatchRenameItem> items;
        for (const auto& item : plan.items)
            items.append({item.source, item.destination, item.directory, item.sourceSnapshot});
        bool collisionCreated = false;
        const auto guarded = filemanager::platform::guardedBatchRename(items,
            {{}, [&](qsizetype index) {
                if (index != 1)
                    return;
                QFile collision(plan.items.at(1).destination);
                collisionCreated = collision.open(QIODevice::WriteOnly)
                    && collision.write("unrelated") == 9;
            }});
        bool originalsRestored = true;
        for (int index = 0; index < sources.size(); ++index) {
            QFile original(sources.at(index));
            originalsRestored = originalsRestored && original.open(QIODevice::ReadOnly)
                && original.readAll() == QByteArray::number(index);
        }
        expect(collisionCreated && !guarded.ok && guarded.renamed == 0
                && guarded.rollbackComplete && guarded.manualRecoveryPaths.isEmpty()
                && originalsRestored
                && QDir(collisionRoot.path()).entryList(
                    {".file-manager-rename-*.tmp"}, QDir::Files | QDir::Hidden).isEmpty(),
            "a destination created after staging should fail without replacement and fully roll back");
    }
    {
        phase("rename_recovery_begin");
        QTemporaryDir recoveryRoot;
        const QStringList sources{recoveryRoot.filePath("first.txt"),
            recoveryRoot.filePath("second.txt")};
        for (int index = 0; index < sources.size(); ++index) {
            QFile fixture(sources.at(index));
            expect(fixture.open(QIODevice::WriteOnly), "manual-recovery fixture should open");
            fixture.write(QByteArray("original-") + QByteArray::number(index));
        }
        const auto plan = filemanager::files::createNumberedRenamePlan(sources, "Chapter");
        QList<filemanager::platform::GuardedBatchRenameItem> items;
        for (const auto& item : plan.items)
            items.append({item.source, item.destination, item.directory, item.sourceSnapshot});
        bool interferenceCreated = false;
        const auto guarded = filemanager::platform::guardedBatchRename(items,
            {{}, [&](qsizetype index) {
                if (index != 1)
                    return;
                QFile destinationCollision(plan.items.at(1).destination);
                QFile originalCollision(sources.at(0));
                interferenceCreated = destinationCollision.open(QIODevice::WriteOnly)
                    && destinationCollision.write("unrelated") == 9
                    && originalCollision.open(QIODevice::WriteOnly)
                    && originalCollision.write("replacement") == 11;
            }});
        QFile replacement(sources.at(0));
        const QStringList staging = QDir(recoveryRoot.path()).entryList(
            {".file-manager-rename-*.tmp"}, QDir::Files | QDir::Hidden);
        QFile retained(staging.isEmpty() ? QString{} : recoveryRoot.filePath(staging.first()));
        expect(interferenceCreated && !guarded.ok && guarded.renamed == 0
                && !guarded.rollbackComplete && guarded.manualRecoveryPaths.size() == 1
                && guarded.errors.join('\n').contains("Manual recovery required")
                && replacement.open(QIODevice::ReadOnly) && replacement.readAll() == "replacement"
                && staging.size() == 1 && retained.open(QIODevice::ReadOnly)
                && retained.readAll() == "original-0"
                && QFileInfo::exists(sources.at(1)),
            "an occupied original path should preserve both objects and report the exact recovery path");
    }
#endif

    const QString undoOriginal = temporary.filePath("undo-original.txt");
    {
        std::atomic_bool cancelPreview{true};
        const auto cancelledRules = filemanager::files::createBatchRenamePlan(
            {undoOriginal}, {.prefix = "Cancelled-"}, &cancelPreview);
        const auto cancelledList = filemanager::files::createListedRenamePlan(
            {undoOriginal}, {"Cancelled"}, true, &cancelPreview);
        const auto cancelledNumbers = filemanager::files::createNumberedRenamePlan(
            {undoOriginal}, "Cancelled", 1, 3, 1, &cancelPreview);
        for (const auto& cancelledPlan : {cancelledRules, cancelledList, cancelledNumbers}) {
            expect(cancelledPlan.cancelled && !cancelledPlan.isValid() && cancelledPlan.items.isEmpty()
                    && !filemanager::files::executeBatchRename(cancelledPlan).ok,
                "cancelled preview preparation must never expose an executable partial plan");
        }
    }
    const QString undoRenamed = temporary.filePath("undo-renamed.txt");
    QFile undoFixture(undoOriginal);
    expect(undoFixture.open(QIODevice::WriteOnly), "rename-undo fixture should open");
    undoFixture.write("identity retained");
    undoFixture.close();
    filemanager::files::SessionUndoStack undoStack;
    expect(QFile::rename(undoOriginal, undoRenamed), "rename-undo fixture should be renamed");
    expect(undoStack.recordRename(undoOriginal, undoRenamed) && undoStack.canUndo(),
        "A completed rename should create an identity-validated session undo entry");
    const auto undoResult = undoStack.undoLast();
    expect(undoResult.ok && QFileInfo::exists(undoOriginal) && !QFileInfo::exists(undoRenamed),
        "Rename undo should restore the original path for the same unchanged file");
    expect(undoResult.restoredPathChanges
            == QList<QPair<QString, QString>>{{undoRenamed, undoOriginal}},
        "rename undo reports only its committed inverse path for label relocation");

    const QString replacedOriginal = temporary.filePath("replaced-original.txt");
    const QString replacedCurrent = temporary.filePath("replaced-current.txt");
    QFile replacedFixture(replacedOriginal);
    expect(replacedFixture.open(QIODevice::WriteOnly), "replacement-undo fixture should open");
    replacedFixture.write("original identity");
    replacedFixture.close();
    expect(QFile::rename(replacedOriginal, replacedCurrent),
        "replacement-undo fixture should be renamed");
    expect(undoStack.recordRename(replacedOriginal, replacedCurrent),
        "A second rename should enter session undo history");
    expect(QFile::remove(replacedCurrent), "The renamed fixture should be removable for replacement");
    QFile replacement(replacedCurrent);
    expect(replacement.open(QIODevice::WriteOnly), "The replacement fixture should open");
    replacement.write("different identity");
    replacement.close();
    const auto refusedUndo = undoStack.undoLast();
    expect(!refusedUndo.ok && QFileInfo::exists(replacedCurrent)
            && !QFileInfo::exists(replacedOriginal),
        "Rename undo should refuse a changed or replaced item without mutating either path");

    const QString batchUndoA = temporary.filePath("batch-undo-a.txt");
    const QString batchUndoB = temporary.filePath("batch-undo-b.txt");
    {
        QFile first(batchUndoA);
        QFile second(batchUndoB);
        expect(first.open(QIODevice::WriteOnly) && first.write("alpha") == 5,
            "first batch-undo fixture should be created");
        expect(second.open(QIODevice::WriteOnly) && second.write("beta") == 4,
            "second batch-undo fixture should be created");
    }
    const auto undoSwapPlan = filemanager::files::createListedRenamePlan(
        {batchUndoA, batchUndoB}, {"batch-undo-b.txt", "batch-undo-a.txt"}, false);
    const auto undoSwapResult = filemanager::files::executeBatchRename(undoSwapPlan);
    filemanager::files::SessionUndoStack batchUndo;
    expect(undoSwapResult.ok && batchUndo.recordBatchRename(undoSwapPlan) && batchUndo.canUndo(),
        "a completed cyclic batch rename should offer one session undo entry");
    const quint64 firstBatchEntryId = batchUndo.topEntryId();
    expect(firstBatchEntryId != 0,
        "a batch rename should expose a nonzero identity for its temporary Undo control");
    const auto swapUndo = batchUndo.undoLast();
    QFile restoredA(batchUndoA);
    QFile restoredB(batchUndoB);
    expect(swapUndo.ok && restoredA.open(QIODevice::ReadOnly)
            && restoredA.readAll() == "alpha" && restoredB.open(QIODevice::ReadOnly)
            && restoredB.readAll() == "beta",
        "batch undo should restore a naming cycle through guarded staging");
    expect(swapUndo.restoredPathChanges.size() == 2
            && swapUndo.restoredPathChanges.contains({batchUndoB, batchUndoA})
            && swapUndo.restoredPathChanges.contains({batchUndoA, batchUndoB}),
        "batch undo reports both committed inverse paths for atomic label relocation");
    restoredA.close();
    restoredB.close();
    expect(batchUndo.topEntryId() == 0,
        "the temporary Undo control must not retain an entry after that batch is undone");

    const auto changedPlan = filemanager::files::createListedRenamePlan(
        {batchUndoA, batchUndoB}, {"batch-undo-c.txt", "batch-undo-d.txt"}, false);
    const auto changedResult = filemanager::files::executeBatchRename(changedPlan);
    filemanager::files::SessionUndoStack changedBatchUndo;
    expect(changedResult.ok && changedBatchUndo.recordBatchRename(changedPlan),
        "a second completed batch should enter session undo");
    expect(batchUndo.recordBatchRename(changedPlan)
            && batchUndo.topEntryId() != firstBatchEntryId,
        "a later batch must have a different identity even when its Undo description matches");
    QFile changedDestination(temporary.filePath("batch-undo-c.txt"));
    expect(changedDestination.open(QIODevice::Append) && changedDestination.write("!") == 1,
        "the changed-file refusal fixture should alter a renamed item");
    changedDestination.close();
    const auto refusedBatchUndo = changedBatchUndo.undoLast();
    expect(!refusedBatchUndo.ok && QFileInfo::exists(changedDestination.fileName())
            && QFileInfo::exists(temporary.filePath("batch-undo-d.txt"))
            && !QFileInfo::exists(batchUndoA) && !QFileInfo::exists(batchUndoB),
        "batch undo should refuse a changed member without moving any batch item");

    const QString occupiedA = temporary.filePath("batch-occupied-a.txt");
    const QString occupiedB = temporary.filePath("batch-occupied-b.txt");
    {
        QFile first(occupiedA);
        QFile second(occupiedB);
        expect(first.open(QIODevice::WriteOnly) && first.write("first") == 5,
            "first occupied-original fixture should be created");
        expect(second.open(QIODevice::WriteOnly) && second.write("second") == 6,
            "second occupied-original fixture should be created");
    }
    const auto occupiedPlan = filemanager::files::createListedRenamePlan(
        {occupiedA, occupiedB}, {"batch-occupied-c.txt", "batch-occupied-d.txt"}, false);
    const auto occupiedResult = filemanager::files::executeBatchRename(occupiedPlan);
    filemanager::files::SessionUndoStack occupiedBatchUndo;
    expect(occupiedResult.ok && occupiedBatchUndo.recordBatchRename(occupiedPlan),
        "an occupied-original batch should enter session undo before the collision");
    QFile intruder(occupiedA);
    expect(intruder.open(QIODevice::WriteOnly) && intruder.write("intruder") == 8,
        "occupied-original fixture should create a new file at the old path");
    intruder.close();
    const auto refusedOccupiedUndo = occupiedBatchUndo.undoLast();
    QFile occupiedCurrentC(temporary.filePath("batch-occupied-c.txt"));
    QFile occupiedCurrentD(temporary.filePath("batch-occupied-d.txt"));
    expect(!refusedOccupiedUndo.ok && intruder.open(QIODevice::ReadOnly)
            && intruder.readAll() == "intruder"
            && occupiedCurrentC.open(QIODevice::ReadOnly)
            && occupiedCurrentC.readAll() == "first"
            && occupiedCurrentD.open(QIODevice::ReadOnly)
            && occupiedCurrentD.readAll() == "second"
            && !QFileInfo::exists(occupiedB),
        "batch undo should refuse an occupied original without changing either result");
    intruder.close();
    occupiedCurrentC.close();
    occupiedCurrentD.close();

    using filemanager::workspace::LayoutNode;
    using filemanager::workspace::PanelState;
    using filemanager::workspace::SplitDirection;
    const LayoutNode workspace = LayoutNode::splitNode(SplitDirection::Horizontal, {
        LayoutNode::panelNode({{"C:/TestData/Alpha", "D:/TestData/Beta"}, 1}),
        LayoutNode::panelNode({{"C:/"}, 0})
    }, {700, 300});
    QString workspaceError;
    expect(workspace.isValid(&workspaceError), "representative workspace tree should be valid");
    const auto restoredWorkspace = LayoutNode::fromJson(workspace.toJson(), &workspaceError);
    expect(restoredWorkspace.has_value(), "workspace JSON should deserialize");
    expect(restoredWorkspace && *restoredWorkspace == workspace,
        "workspace JSON should round-trip without losing state");
    const LayoutNode groupedWorkspace = LayoutNode::panelNode(
        {{"C:/"}, 0, {0}, {false}, {0}, {true}, {4}});
    const auto restoredGroupedWorkspace = LayoutNode::fromJson(groupedWorkspace.toJson());
    expect(restoredGroupedWorkspace && *restoredGroupedWorkspace == groupedWorkspace,
        "workspace JSON should preserve per-tab group-by state");
    QJsonObject legacyWorkspace = groupedWorkspace.toJson();
    legacyWorkspace["schemaVersion"] = 5;
    legacyWorkspace.remove("groupings");
    const auto migratedLegacyWorkspace = LayoutNode::fromJson(legacyWorkspace);
    expect(migratedLegacyWorkspace && migratedLegacyWorkspace->panel.groupings == QList<int>{0},
        "schema 5 workspaces should migrate to ungrouped tabs");
    PanelState detailedState{{"C:/"}, 0, {0}, {false}, {0}, {true}, {0}};
    detailedState.selectedPaths = {{"C:/alpha.txt", "C:/beta.txt"}};
    detailedState.focusedPaths = {"C:/beta.txt"};
    detailedState.expandedPaths = {{"C:/Folder", "C:/Folder/Nested"}};
    detailedState.verticalScrolls = {37};
    detailedState.horizontalScrolls = {9};
    const LayoutNode detailedWorkspace = LayoutNode::panelNode(detailedState);
    const auto restoredDetailedWorkspace = LayoutNode::fromJson(detailedWorkspace.toJson());
    expect(restoredDetailedWorkspace && *restoredDetailedWorkspace == detailedWorkspace,
        "workspace JSON should preserve selection, focus, expansion, and scroll state");
    QJsonObject completeWorkspaceSchema = detailedWorkspace.toJson();
    completeWorkspaceSchema["viewModes"] = QJsonArray{3};
    completeWorkspaceSchema["showHidden"] = QJsonArray{true};
    completeWorkspaceSchema["folderPlacements"] = QJsonArray{2};
    completeWorkspaceSchema["showFileExtensions"] = QJsonArray{false};
    completeWorkspaceSchema["groupings"] = QJsonArray{3};
    for (int schema = 1; schema <= LayoutNode::CurrentSchemaVersion; ++schema) {
        QJsonObject fixture = completeWorkspaceSchema;
        fixture["schemaVersion"] = schema;
        if (schema < 7) {
            fixture.remove("selectedPaths");
            fixture.remove("focusedPaths");
            fixture.remove("expandedPaths");
            fixture.remove("verticalScrolls");
            fixture.remove("horizontalScrolls");
        }
        if (schema < 6)
            fixture.remove("groupings");
        if (schema < 4)
            fixture.remove("showFileExtensions");
        if (schema < 3)
            fixture.remove("folderPlacements");
        if (schema < 2) {
            fixture.remove("viewModes");
            fixture.remove("showHidden");
        }
        const auto migrated = LayoutNode::fromJson(fixture, &workspaceError);
        expect(migrated && migrated->panel.viewModes == QList<int>{schema >= 2 ? 3 : 0}
                && migrated->panel.showHidden == QList<bool>{schema >= 2}
                && migrated->panel.folderPlacements == QList<int>{schema >= 3 ? 2 : 0}
                && migrated->panel.showFileExtensions == QList<bool>{schema < 4}
                && migrated->panel.groupings == QList<int>{schema >= 6 ? 3 : 0}
                && migrated->panel.selectedPaths.size() == 1
                && (schema >= 7 ? migrated->panel.selectedPaths.first()
                        == detailedState.selectedPaths.first()
                                : migrated->panel.selectedPaths.first().isEmpty()),
            "Every released workspace schema should migrate deterministically to current defaults");
    }
    PanelState excessiveSelection = detailedState;
    excessiveSelection.selectedPaths[0] = QStringList(257, "C:/item");
    expect(!LayoutNode::panelNode(excessiveSelection).isValid(),
        "workspace validation should reject excessive restored selections");
    PanelState excessiveExpansion = detailedState;
    excessiveExpansion.expandedPaths[0] = QStringList(129, "C:/folder");
    expect(!LayoutNode::panelNode(excessiveExpansion).isValid(),
        "workspace validation should reject excessive restored expansions");
    PanelState invalidScroll = detailedState;
    invalidScroll.verticalScrolls = {-1};
    expect(!LayoutNode::panelNode(invalidScroll).isValid(),
        "workspace validation should reject negative restored scroll positions");
    PanelState invalidGrouping = detailedState;
    invalidGrouping.groupings = {5};
    expect(!LayoutNode::panelNode(invalidGrouping).isValid(),
        "workspace validation should reject group-by modes outside the bounded command set");
    expect(!LayoutNode::splitNode(SplitDirection::Vertical, {
        LayoutNode::panelNode({{"C:/"}, 0})
    }).isValid(), "a split with one child should be rejected");

    phase("favorite_settings_begin");
    QTemporaryDir favoriteSettingsDirectory;
    const QString favoriteSettingsPath = favoriteSettingsDirectory.filePath("favorites.ini");
    QSettings favoriteSettings(favoriteSettingsPath, QSettings::IniFormat);

    const QString healthSettingsPath = favoriteSettingsDirectory.filePath("health.ini");
    QSettings healthSettings(healthSettingsPath, QSettings::IniFormat);
    healthSettings.setValue("appearance/fontSize", 40.0);
    healthSettings.setValue("appearance/density", "Impossible");
    healthSettings.setValue("appearance/animations", "not-a-boolean");
    healthSettings.setValue("privacy/allowWindowsShellMetadataHandlers", true);
    healthSettings.setValue("navigation/recents",
        QStringList{"C:/Valid", sensitiveDiagnosticPath, "relative/secret"});
    healthSettings.setValue("search/indexedRoots", QStringList{"D:/Index", "d:/index"});
    healthSettings.setValue("navigation/favoriteGroups", QByteArray("not-json"));
    healthSettings.setValue("interaction/pinnedContextActions",
        QStringList{"file.copy", "file.copy"});
    healthSettings.setValue("interaction/itemSelectionCheckBoxes", "not-a-boolean");
    healthSettings.setValue("navigation/openTo", "somewhere-else");
    healthSettings.setValue("shortcuts/file.copy", "Ctrl+C");
    healthSettings.setValue("shortcuts/file.rename", "Ctrl+C");
    healthSettings.setValue("shortcuts/invalid command", "F2");
    healthSettings.setValue("view/columnProfiles/general", QByteArray());
    const auto healthReport = filemanager::settings::repairSettings(healthSettings);
    expect(healthReport.repaired() && healthReport.resetCount() == 9
            && healthSettings.value("privacy/allowWindowsShellMetadataHandlers").toBool()
            && !healthSettings.contains("appearance/fontSize")
            && !healthSettings.contains("navigation/recents")
            && !healthSettings.contains("search/indexedRoots")
            && !healthSettings.contains("interaction/itemSelectionCheckBoxes")
            && !healthSettings.contains("navigation/openTo")
            && healthSettings.value("shortcuts/file.copy").toString() == "Ctrl+C"
            && !healthSettings.contains("shortcuts/file.rename")
            && !healthSettings.contains("shortcuts/invalid command")
            && !healthSettings.contains("view/columnProfiles/general"),
        "Settings health repair should remove malformed bounded values and preserve valid consent");
    const QStringList recoveryKeys = healthSettings.allKeys().filter("recovery/settings/");
    QByteArray recoveryValues;
    for (const QString& key : recoveryKeys)
        recoveryValues.append(healthSettings.value(key).toString().toUtf8());
    expect(recoveryKeys.size() == 3
            && !recoveryValues.contains(sensitiveDiagnosticPath.toUtf8())
            && !recoveryValues.contains("relative/secret")
            && !filemanager::settings::repairSettings(healthSettings).repaired(),
        "Settings recovery markers should remain path-free and repair should be idempotent");

    favoriteSettings.setValue("navigation/favorites", QStringList{"C:/One", "C:/Two"});
    auto favoriteGroups = filemanager::settings::loadFavoriteGroups(favoriteSettings);
    expect(favoriteGroups.groups.size() == 1
            && favoriteGroups.groups.first().name == "Favorites"
            && favoriteGroups.groups.first().paths.size() == 2,
        "flat favorites should migrate into the default named group");
    favoriteGroups.groups.append({"work", "Work", {"D:/Projects"}});
    filemanager::settings::saveFavoriteGroups(favoriteSettings, favoriteGroups);
    expect(!favoriteSettings.contains("navigation/favorites"),
        "saving named groups should retire the legacy flat setting");
    const auto restoredFavoriteGroups = filemanager::settings::loadFavoriteGroups(favoriteSettings);
    expect(restoredFavoriteGroups == favoriteGroups
            && restoredFavoriteGroups.containsPath("d:/projects"),
        "named favorite groups should round-trip with case-insensitive path lookup");
    filemanager::settings::FavoriteGroups reorderableFavorites{{{"work", "Work",
        {"C:/First", "C:/Second", "C:/Third"}}}};
    expect(reorderableFavorites.movePath("work", "c:/second", -1)
            && reorderableFavorites.groups.first().paths
                == QStringList{"C:/Second", "C:/First", "C:/Third"},
        "favorite paths should move upward within their own group case-insensitively");
    expect(reorderableFavorites.movePath("work", "C:/Second", 2)
            && reorderableFavorites.groups.first().paths
                == QStringList{"C:/First", "C:/Third", "C:/Second"},
        "favorite paths should move downward within their own group deterministically");
    expect(!reorderableFavorites.movePath("work", "C:/Second", 1)
            && !reorderableFavorites.movePath("missing", "C:/First", -1),
        "favorite keyboard reordering should refuse group edges and unknown groups");

    phase("checks_finished_before_teardown");
    if (failures == 0)
        std::cout << "All core tests passed.\n";
    return failures == 0 ? 0 : 1;
}
