#pragma once

#include <QDialog>
#include <QList>
#include <QStringList>

class QTableWidget;
class QPushButton;

namespace filemanager::ui {

struct FileDetailRow
{
    QString label;
    QString value;
};

struct FileDetailsSnapshot
{
    QString title;
    QStringList paths;
    QList<FileDetailRow> rows;
    QString notice;
    bool available = false;
};

class FileDetailsInspector final
{
public:
    static constexpr int MaximumItems = 1024;
    static FileDetailsSnapshot inspect(const QStringList& paths,
        const QString& tagStoragePath = {});
};

class FileDetailsDialog final : public QDialog
{
    Q_OBJECT

public:
    explicit FileDetailsDialog(QStringList paths, QWidget* parent = nullptr);

signals:
    void windowsPropertiesRequested(const QString& path);
    void detailsReady();

private:
    void populate(const FileDetailsSnapshot& snapshot);

    QTableWidget* details_ = nullptr;
    QPushButton* copy_ = nullptr;
    QPushButton* windowsProperties_ = nullptr;
    QStringList paths_;
};

} // namespace filemanager::ui
