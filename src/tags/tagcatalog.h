#pragma once

#include "files/sourcesnapshot.h"

#include <QList>
#include <QMap>
#include <QString>
#include <QStringList>

namespace filemanager::tags {

// The catalog never enumerates folders or reads a file. Callers must supply
// a snapshot obtained during an explicitly selected, user-visible operation.
struct SelectedFile
{
    QString path;
    filemanager::files::SourceSnapshot snapshot;
};

struct TaggedFile
{
    QString path;
    filemanager::files::SourceSnapshot snapshot;
    QStringList tags;
};

// One committed File Manager path change. The caller supplies the pre-operation
// identity and the observed result; the catalog rechecks the destination.
// Directory changes are same-volume renames of a root, not folder scans.
struct TagPathChange
{
    QString sourcePath;
    QString destinationPath;
    filemanager::files::SourceSnapshot sourceSnapshot;
    filemanager::files::SourceSnapshot destinationSnapshot;
    bool directory = false;
};

struct DestinationRule
{
    QString id;
    QStringList requiredTags;
    QString destinationDirectory;
    bool operator==(const DestinationRule&) const = default;
};

enum class TagMatchState { Unavailable, Unassigned, Current, Modified, Replaced };

struct TagMatch
{
    TagMatchState state = TagMatchState::Unassigned;
    QStringList tags;
    bool actionable() const { return state == TagMatchState::Current && !tags.isEmpty(); }
};

class TagCatalog final
{
public:
    static constexpr int MaximumRecords = 100'000;
    static constexpr int MaximumTagsPerFile = 32;
    static constexpr int MaximumRules = 256;
    static constexpr qsizetype MaximumPlainBytes = 64 * 1024 * 1024;

    explicit TagCatalog(QString storagePath = {});

    bool load(QString* error = nullptr);
    bool assign(const QList<TaggedFile>& files, QString* error = nullptr);
    bool addTags(const QList<SelectedFile>& files, const QStringList& tags,
        QString* error = nullptr);
    bool removeTags(const QList<SelectedFile>& files, const QStringList& tags,
        QString* error = nullptr);
    bool remove(const QString& path, QString* error = nullptr);
    bool relocate(const QList<TagPathChange>& changes, QString* error = nullptr);
    bool replaceRules(const QList<DestinationRule>& rules, QString* error = nullptr,
        const QList<DestinationRule>* expectedRules = nullptr);
    bool clear(QString* error = nullptr);

    TagMatch match(const QString& path,
        const filemanager::files::SourceSnapshot& current) const;
    QList<TaggedFile> entries() const;
    QList<DestinationRule> rules() const;
    QString storagePath() const;
    bool ready() const;

private:
    bool changeTags(const QList<SelectedFile>& files, const QStringList& tags,
        bool remove, QString* error);
    bool write(const QMap<QString, TaggedFile>& files,
        const QList<DestinationRule>& rules, QString* error) const;

    QString storagePath_;
    QMap<QString, TaggedFile> entries_;
    QList<DestinationRule> rules_;
    bool loaded_ = false;
};

} // namespace filemanager::tags
