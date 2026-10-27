#include "filedetailsdialog.h"

#include "platform/nativefilecopy.h"
#include "platform/cloudfilestatus.h"
#include "files/sourcesnapshot.h"
#include "tags/tagcatalog.h"

#include <QApplication>
#include <QClipboard>
#include <QDateTime>
#include <QDialogButtonBox>
#include <QDir>
#include <QFileInfo>
#include <QHeaderView>
#include <QLabel>
#include <QLocale>
#include <QMimeDatabase>
#include <QPointer>
#include <QPushButton>
#include <QSet>
#include <QTableWidget>
#include <QThread>
#include <QVBoxLayout>

#include <limits>
#include <utility>

#ifdef Q_OS_WIN
#include <windows.h>
#endif

namespace filemanager::ui {
namespace {

QString formattedDate(const QDateTime& value)
{
    return value.isValid() ? QLocale().toString(value, QLocale::LongFormat)
                           : QStringLiteral("Unavailable");
}

QString permissionsText(const QFileInfo& info)
{
    QStringList values;
    if (info.isReadable()) values.append(QStringLiteral("Read"));
    if (info.isWritable()) values.append(QStringLiteral("Write"));
    if (info.isExecutable()) values.append(QStringLiteral("Execute"));
    return values.isEmpty() ? QStringLiteral("None reported") : values.join(", ");
}

QString pathKindText(const QString& path)
{
    using filemanager::platform::NativePathKind;
    switch (filemanager::platform::nativePathKind(path)) {
    case NativePathKind::Regular: return QStringLiteral("Regular filesystem item");
    case NativePathKind::SymbolicLink: return QStringLiteral("Symbolic link (not traversed)");
    case NativePathKind::CloudPlaceholder: return QStringLiteral("Offline/cloud placeholder");
    case NativePathKind::UnsupportedReparse: return QStringLiteral("Unsupported reparse point");
    case NativePathKind::Missing: return QStringLiteral("Unavailable");
    }
    return QStringLiteral("Unavailable");
}

QString attributesText(const QString& path, const QFileInfo& info)
{
    QStringList attributes;
    if (info.isHidden()) attributes.append(QStringLiteral("Hidden"));
    if (info.isSymLink()) attributes.append(QStringLiteral("Symbolic link"));
#ifdef Q_OS_WIN
    const QString native = filemanager::platform::extendedLengthPath(path);
    const DWORD value = GetFileAttributesW(reinterpret_cast<LPCWSTR>(native.utf16()));
    if (value != INVALID_FILE_ATTRIBUTES) {
        if (value & FILE_ATTRIBUTE_READONLY) attributes.append(QStringLiteral("Read-only"));
        if (value & FILE_ATTRIBUTE_SYSTEM) attributes.append(QStringLiteral("System"));
        if (value & FILE_ATTRIBUTE_ARCHIVE) attributes.append(QStringLiteral("Archive"));
        if (value & FILE_ATTRIBUTE_COMPRESSED) attributes.append(QStringLiteral("Compressed"));
        if (value & FILE_ATTRIBUTE_ENCRYPTED) attributes.append(QStringLiteral("Encrypted"));
        if (value & FILE_ATTRIBUTE_SPARSE_FILE) attributes.append(QStringLiteral("Sparse"));
        if (value & FILE_ATTRIBUTE_OFFLINE) attributes.append(QStringLiteral("Offline"));
        if (value & FILE_ATTRIBUTE_REPARSE_POINT) attributes.append(QStringLiteral("Reparse point"));
    }
#else
    Q_UNUSED(path);
#endif
    attributes.removeDuplicates();
    return attributes.isEmpty() ? QStringLiteral("None reported") : attributes.join(", ");
}

QString itemType(const QFileInfo& info)
{
    if (info.isDir())
        return QStringLiteral("Folder");
    const QString description = QMimeDatabase().mimeTypeForFile(
        info, QMimeDatabase::MatchExtension).comment();
    return description.isEmpty() ? QStringLiteral("File") : description;
}

} // namespace

FileDetailsSnapshot FileDetailsInspector::inspect(const QStringList& inputPaths,
    const QString& tagStoragePath)
{
    FileDetailsSnapshot result;
    QSet<QString> seen;
    for (const QString& input : inputPaths) {
        const QFileInfo info(input);
        if (!info.isAbsolute())
            continue;
        const QString path = QDir::cleanPath(info.absoluteFilePath());
        const QString key = QDir::toNativeSeparators(path).toCaseFolded();
        if (!seen.contains(key)) {
            seen.insert(key);
            result.paths.append(path);
        }
        if (result.paths.size() >= MaximumItems)
            break;
    }
    if (result.paths.isEmpty()) {
        result.title = QStringLiteral("Details unavailable");
        result.notice = QStringLiteral("No absolute filesystem item was selected.");
        return result;
    }

    if (result.paths.size() > 1) {
        int files = 0;
        int folders = 0;
        int unavailable = 0;
        qint64 totalFileBytes = 0;
        QString commonParent;
        bool sameParent = true;
        for (const QString& path : result.paths) {
            const QFileInfo info(path);
            if (!info.exists()) {
                ++unavailable;
                continue;
            }
            if (info.isDir())
                ++folders;
            else {
                ++files;
                if (info.size() >= 0
                        && info.size() <= (std::numeric_limits<qint64>::max)() - totalFileBytes)
                    totalFileBytes += info.size();
            }
            if (commonParent.isEmpty())
                commonParent = info.absolutePath();
            else if (commonParent.compare(info.absolutePath(), Qt::CaseInsensitive) != 0)
                sameParent = false;
        }
        result.title = QStringLiteral("%1 selected items").arg(result.paths.size());
        result.rows = {{QStringLiteral("Selected"), QString::number(result.paths.size())},
            {QStringLiteral("Files"), QString::number(files)},
            {QStringLiteral("Folders"), QString::number(folders)},
            {QStringLiteral("Unavailable"), QString::number(unavailable)},
            {QStringLiteral("Selected file bytes"), QLocale().formattedDataSize(totalFileBytes)},
            {QStringLiteral("Location"), sameParent && !commonParent.isEmpty()
                    ? QDir::toNativeSeparators(commonParent)
                    : QStringLiteral("Multiple locations")}};
        result.notice = QStringLiteral(
            "Folder contents are not traversed. The size shown totals selected files only.");
        result.available = files + folders > 0;
        return result;
    }

    const QFileInfo info(result.paths.constFirst());
    result.title = info.fileName().isEmpty() ? result.paths.constFirst() : info.fileName();
    if (!info.exists()) {
        result.notice = QStringLiteral("The selected item is no longer available.");
        result.rows = {{QStringLiteral("Path"), QDir::toNativeSeparators(result.paths.constFirst())},
            {QStringLiteral("Status"), QStringLiteral("Unavailable")}};
        return result;
    }
    result.available = true;
    const auto cloudStatus = filemanager::platform::inspectCloudFileStatus(
        info.absoluteFilePath());
    result.rows = {{QStringLiteral("Name"), result.title},
        {QStringLiteral("Type"), itemType(info)},
        {QStringLiteral("Location"), QDir::toNativeSeparators(info.absolutePath())},
        {QStringLiteral("Full path"), QDir::toNativeSeparators(info.absoluteFilePath())},
        {QStringLiteral("Extension"), info.isFile() && !info.suffix().isEmpty()
                ? QStringLiteral(".") + info.suffix() : QStringLiteral("None")},
        {QStringLiteral("Size"), info.isFile() ? QLocale().formattedDataSize(info.size())
                                                : QStringLiteral("Not calculated")},
        {QStringLiteral("Created"), formattedDate(info.birthTime())},
        {QStringLiteral("Modified"), formattedDate(info.lastModified())},
        {QStringLiteral("Accessed"), formattedDate(info.lastRead())},
        {QStringLiteral("Permissions"), permissionsText(info)},
        {QStringLiteral("Attributes"), attributesText(info.absoluteFilePath(), info)},
        {QStringLiteral("Storage/link status"), pathKindText(info.absoluteFilePath())},
        {QStringLiteral("Cloud Files status"),
            filemanager::platform::cloudFileStatusText(cloudStatus, true)}};
    if (info.isFile()) {
        QString tagsText = QStringLiteral("Unavailable for this file");
        if (filemanager::platform::nativePathKind(info.absoluteFilePath())
                == filemanager::platform::NativePathKind::Regular) {
            filemanager::tags::TagCatalog catalog(tagStoragePath);
            QString catalogError;
            if (catalog.load(&catalogError)) {
                const auto identity = filemanager::files::captureSourceSnapshot(
                    info.absoluteFilePath());
                const auto match = catalog.match(info.absoluteFilePath(), identity);
                if (match.state == filemanager::tags::TagMatchState::Current)
                    tagsText = match.tags.join(QStringLiteral(", "));
                else if (match.state == filemanager::tags::TagMatchState::Unassigned)
                    tagsText = QStringLiteral("None");
                else if (match.state == filemanager::tags::TagMatchState::Replaced)
                    tagsText = QStringLiteral("File replaced since tagging; labels withheld");
            }
        }
        result.rows.insert(1, {QStringLiteral("File Manager tags"), tagsText});
    }
    result.notice = QStringLiteral(
        "Read-only details. Use Windows Properties to edit advanced attributes, permissions or ownership.");
    return result;
}

FileDetailsDialog::FileDetailsDialog(QStringList paths, QWidget* parent)
    : QDialog(parent), paths_(std::move(paths))
{
    setWindowTitle(QStringLiteral("Details inspector"));
    setObjectName(QStringLiteral("FileDetailsDialog"));
    setMinimumSize(680, 480);
    auto* layout = new QVBoxLayout(this);
    auto* heading = new QLabel(QStringLiteral("Loading read-only details…"), this);
    heading->setObjectName(QStringLiteral("FileDetailsHeading"));
    heading->setAccessibleName(QStringLiteral("Details inspector status"));
    heading->setTextFormat(Qt::PlainText);
    heading->setWordWrap(true);
    layout->addWidget(heading);
    details_ = new QTableWidget(this);
    details_->setObjectName(QStringLiteral("FileDetailsTable"));
    details_->setAccessibleName(QStringLiteral("Read-only file details"));
    details_->setColumnCount(2);
    details_->setHorizontalHeaderLabels({QStringLiteral("Property"), QStringLiteral("Value")});
    details_->horizontalHeader()->setStretchLastSection(true);
    details_->verticalHeader()->hide();
    details_->setEditTriggers(QAbstractItemView::NoEditTriggers);
    details_->setSelectionBehavior(QAbstractItemView::SelectRows);
    layout->addWidget(details_, 1);
    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Close, this);
    copy_ = buttons->addButton(QStringLiteral("Copy details"), QDialogButtonBox::ActionRole);
    copy_->setObjectName(QStringLiteral("CopyFileDetails"));
    copy_->setEnabled(false);
    windowsProperties_ = buttons->addButton(
        QStringLiteral("Windows Properties…"), QDialogButtonBox::ActionRole);
    windowsProperties_->setObjectName(QStringLiteral("WindowsFileProperties"));
    windowsProperties_->setEnabled(paths_.size() == 1);
    layout->addWidget(buttons);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    connect(windowsProperties_, &QPushButton::clicked, this, [this] {
        if (paths_.size() == 1)
            emit windowsPropertiesRequested(paths_.constFirst());
    });
    connect(copy_, &QPushButton::clicked, this, [this] {
        QStringList lines;
        for (int row = 0; row < details_->rowCount(); ++row)
            lines.append(details_->item(row, 0)->text() + '\t' + details_->item(row, 1)->text());
        QApplication::clipboard()->setText(lines.join('\n'));
    });
    QPointer<FileDetailsDialog> guard(this);
    const QStringList inspectedPaths = paths_;
    QThread* worker = QThread::create([guard, inspectedPaths] {
        const FileDetailsSnapshot snapshot = FileDetailsInspector::inspect(inspectedPaths);
        if (guard)
            QMetaObject::invokeMethod(guard, [guard, snapshot] {
                if (guard)
                    guard->populate(snapshot);
            });
    });
    connect(worker, &QThread::finished, worker, &QObject::deleteLater);
    worker->start();
}

void FileDetailsDialog::populate(const FileDetailsSnapshot& snapshot)
{
    auto* heading = findChild<QLabel*>(QStringLiteral("FileDetailsHeading"));
    heading->setText(snapshot.title + (snapshot.notice.isEmpty()
        ? QString() : QStringLiteral("\n") + snapshot.notice));
    details_->setRowCount(snapshot.rows.size());
    for (int row = 0; row < snapshot.rows.size(); ++row) {
        auto* label = new QTableWidgetItem(snapshot.rows.at(row).label);
        auto* value = new QTableWidgetItem(snapshot.rows.at(row).value);
        label->setFlags(label->flags() & ~Qt::ItemIsEditable);
        value->setFlags(value->flags() & ~Qt::ItemIsEditable);
        details_->setItem(row, 0, label);
        details_->setItem(row, 1, value);
    }
    details_->resizeColumnToContents(0);
    copy_->setEnabled(!snapshot.rows.isEmpty());
    windowsProperties_->setEnabled(snapshot.paths.size() == 1 && snapshot.available);
    emit detailsReady();
}

} // namespace filemanager::ui
