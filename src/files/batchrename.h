#pragma once

#include "sourcesnapshot.h"

#include <QString>
#include <QStringList>
#include <QList>
#include <atomic>

namespace filemanager::files {

enum class BatchRenameCase
{
    Preserve,
    Lowercase,
    Uppercase,
    TitleCase
};

struct BatchRenameRule
{
    QString find;
    QString replace;
    bool useRegularExpression = false;
    bool caseSensitive = false;
    QString prefix;
    QString suffix;
    BatchRenameCase caseConversion = BatchRenameCase::Preserve;
    bool addSequence = false;
    int sequenceStart = 1;
    int sequencePadding = 3;
    int sequenceStep = 1;
};

using BatchRenameSource = SourceSnapshot;

struct BatchRenameItem
{
    QString source;
    QString destination;
    bool directory = false;
    BatchRenameSource sourceSnapshot;
};

struct BatchRenamePlan
{
    QList<BatchRenameItem> items;
    QStringList errors;
    bool cancelled = false;
    bool hasChanges() const;
    bool isValid() const { return !cancelled && errors.isEmpty() && hasChanges(); }
};

struct BatchRenameResult
{
    bool ok = false;
    int renamed = 0;
    bool rollbackComplete = true;
    QStringList errors;
    QStringList manualRecoveryPaths;
};

BatchRenamePlan createBatchRenamePlan(const QStringList& paths, const BatchRenameRule& rule,
    const std::atomic_bool* cancellation = nullptr);
BatchRenamePlan createListedRenamePlan(const QStringList& paths, const QStringList& names,
    bool preserveExtensions = true, const std::atomic_bool* cancellation = nullptr);
BatchRenamePlan createNumberedRenamePlan(const QStringList& paths, const QString& label,
    int start = 1, int padding = 3, int step = 1, const std::atomic_bool* cancellation = nullptr);
BatchRenameResult executeBatchRename(const BatchRenamePlan& plan);

} // namespace filemanager::files
