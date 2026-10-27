#include "conflictdialog.h"

#include <QCheckBox>
#include <QDialog>
#include <QDialogButtonBox>
#include <QDir>
#include <QFileIconProvider>
#include <QFileInfo>
#include <QGridLayout>
#include <QGroupBox>
#include <QImageReader>
#include <QLabel>
#include <QLocale>
#include <QPushButton>
#include <QVBoxLayout>

namespace filemanager::ui {
namespace {

QString displaySize(const ConflictItemInfo& item)
{
    return item.directory ? QStringLiteral("Folder")
                          : QLocale().formattedDataSize(item.size);
}

QWidget* itemCard(const QString& heading, const ConflictItemInfo& item, QWidget* parent)
{
    auto* box = new QGroupBox(heading, parent);
    auto* layout = new QGridLayout(box);
    int row = 0;
    if (!item.preview.isNull()) {
        auto* preview = new QLabel(box);
        preview->setPixmap(QPixmap::fromImage(item.preview));
        preview->setAlignment(Qt::AlignCenter);
        preview->setMinimumHeight(ConflictDialog::PreviewHeight);
        preview->setAccessibleName(heading + QStringLiteral(" preview"));
        layout->addWidget(preview, row++, 0, 1, 2);
    }
    const auto addValue = [&](const QString& label, const QString& value) {
        auto* valueLabel = new QLabel(value, box);
        valueLabel->setTextInteractionFlags(Qt::TextSelectableByMouse);
        valueLabel->setWordWrap(true);
        layout->addWidget(new QLabel(label, box), row, 0, Qt::AlignTop);
        layout->addWidget(valueLabel, row++, 1);
    };
    addValue(QStringLiteral("Name"), item.name);
    addValue(QStringLiteral("Location"), QDir::toNativeSeparators(item.path));
    addValue(QStringLiteral("Type"), item.type);
    addValue(QStringLiteral("Size"), displaySize(item));
    addValue(QStringLiteral("Modified"), item.modified.isValid()
        ? QLocale().toString(item.modified, QLocale::ShortFormat)
        : QStringLiteral("Unknown"));
    layout->setColumnStretch(1, 1);
    return box;
}

} // namespace

ConflictItemInfo ConflictDialog::inspect(const QString& path)
{
    const QFileInfo info(path);
    ConflictItemInfo result;
    result.path = info.absoluteFilePath();
    result.name = info.fileName();
    result.directory = info.isDir();
    result.size = result.directory ? -1 : info.size();
    result.modified = info.lastModified();
    result.type = QFileIconProvider().type(info);

    if (!info.isFile() || info.size() < 0 || info.size() > MaximumPreviewSourceBytes)
        return result;
    QImageReader reader(path);
    reader.setAutoTransform(true);
    const QSize sourceSize = reader.size();
    if (!reader.canRead() || !sourceSize.isValid() || sourceSize.width() <= 0
            || sourceSize.height() <= 0
            || static_cast<qint64>(sourceSize.width()) * sourceSize.height()
                > MaximumPreviewPixels) {
        return result;
    }
    QSize target(PreviewWidth, PreviewHeight);
    target.scale(sourceSize, Qt::KeepAspectRatio);
    reader.setScaledSize(target);
    result.preview = reader.read();
    return result;
}

ConflictDialogResult ConflictDialog::run(QWidget* parent, const QString& source,
    const QString& destination, bool directory)
{
    QDialog dialog(parent);
    dialog.setObjectName(QStringLiteral("TransferConflictDialog"));
    dialog.setWindowTitle(QStringLiteral("Destination conflict"));
    dialog.setAccessibleName(QStringLiteral("Transfer destination conflict"));
    dialog.setAccessibleDescription(QStringLiteral(
        "Compare the source and destination, then choose replace, keep both, skip, or cancel."));
    dialog.setModal(true);
    dialog.setMinimumWidth(720);
    auto* layout = new QVBoxLayout(&dialog);
    auto* heading = new QLabel(QStringLiteral("An item with this name already exists."), &dialog);
    heading->setAccessibleName(QStringLiteral("Transfer conflict summary"));
    QFont headingFont = heading->font();
    headingFont.setBold(true);
    heading->setFont(headingFont);
    layout->addWidget(heading);

    auto* comparison = new QGridLayout;
    comparison->addWidget(itemCard(QStringLiteral("Source"), inspect(source), &dialog), 0, 0);
    comparison->addWidget(itemCard(QStringLiteral("Destination"), inspect(destination), &dialog), 0, 1);
    comparison->setColumnStretch(0, 1);
    comparison->setColumnStretch(1, 1);
    layout->addLayout(comparison);

    auto* applyToAll = new QCheckBox(
        QStringLiteral("Do this for all remaining conflicts in this transfer"), &dialog);
    layout->addWidget(applyToAll);
    auto* buttons = new QDialogButtonBox(&dialog);
    auto* replace = buttons->addButton(directory ? QStringLiteral("Merge")
                                                 : QStringLiteral("Replace"),
        QDialogButtonBox::DestructiveRole);
    auto* keepBoth = buttons->addButton(QStringLiteral("Keep both"), QDialogButtonBox::ActionRole);
    auto* skip = buttons->addButton(QStringLiteral("Skip"), QDialogButtonBox::ActionRole);
    auto* cancel = buttons->addButton(QStringLiteral("Cancel transfer"), QDialogButtonBox::RejectRole);
    keepBoth->setDefault(true);
    keepBoth->setFocus();
    layout->addWidget(buttons);

    files::ConflictDecision decision = files::ConflictDecision::Cancel;
    const auto choose = [&](files::ConflictDecision selected) {
        decision = selected;
        dialog.accept();
    };
    QObject::connect(replace, &QPushButton::clicked, &dialog,
        [&] { choose(files::ConflictDecision::Replace); });
    QObject::connect(keepBoth, &QPushButton::clicked, &dialog,
        [&] { choose(files::ConflictDecision::KeepBoth); });
    QObject::connect(skip, &QPushButton::clicked, &dialog,
        [&] { choose(files::ConflictDecision::Skip); });
    QObject::connect(cancel, &QPushButton::clicked, &dialog, &QDialog::reject);
    dialog.exec();
    return {decision, applyToAll->isChecked()
        && decision != files::ConflictDecision::Cancel};
}

} // namespace filemanager::ui
