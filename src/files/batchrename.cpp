#include "batchrename.h"

#include "filenamevalidator.h"
#include "platform/guardedfilerename.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QHash>
#include <QRegularExpression>
#include <QSet>

#include <limits>

#ifdef Q_OS_WIN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace filemanager::files {
namespace {

bool stopPreview(BatchRenamePlan& plan, const std::atomic_bool* cancellation)
{
    if (!cancellation || !cancellation->load(std::memory_order_relaxed))
        return false;
    plan.items.clear();
    plan.errors = {"Preview cancelled."};
    plan.cancelled = true;
    return true;
}

QString pathKey(const QString& path)
{
    return QDir::cleanPath(QFileInfo(path).absoluteFilePath()).toCaseFolded();
}

BatchRenameSource captureSource(const QString& path)
{
    return captureSourceSnapshot(path);
}

bool sourceMatches(const BatchRenameItem& item, const QString& path)
{
    return item.sourceSnapshot.valid && item.sourceSnapshot == captureSource(path);
}

QString convertCase(const QString& value, BatchRenameCase conversion)
{
    if (conversion == BatchRenameCase::Lowercase)
        return value.toLower();
    if (conversion == BatchRenameCase::Uppercase)
        return value.toUpper();
    if (conversion != BatchRenameCase::TitleCase)
        return value;

    QString converted = value.toLower();
    bool capitalize = true;
    for (qsizetype index = 0; index < converted.size(); ++index) {
        QChar character = converted.at(index);
        if (character.isLetterOrNumber()) {
            if (capitalize)
                converted[index] = character.toUpper();
            capitalize = false;
        } else {
            capitalize = true;
        }
    }
    return converted;
}

} // namespace

bool BatchRenamePlan::hasChanges() const
{
    for (const BatchRenameItem& item : items) {
        if (item.source != item.destination)
            return true;
    }
    return false;
}

BatchRenamePlan createBatchRenamePlan(const QStringList& paths, const BatchRenameRule& rule,
    const std::atomic_bool* cancellation)
{
    BatchRenamePlan plan;
    if (stopPreview(plan, cancellation))
        return plan;
    if (rule.addSequence && (rule.sequencePadding < 1 || rule.sequencePadding > 12
            || rule.sequenceStep == 0)) {
        plan.errors.append("Sequence padding must be from 1 to 12 and step cannot be zero.");
        return plan;
    }
    QRegularExpression expression;
    if (rule.useRegularExpression) {
        if (rule.find.isEmpty()) {
            plan.errors.append("Enter a regular expression to find.");
            return plan;
        }
        expression.setPattern(rule.find);
        expression.setPatternOptions(rule.caseSensitive
            ? QRegularExpression::NoPatternOption
            : QRegularExpression::CaseInsensitiveOption);
        if (!expression.isValid()) {
            plan.errors.append(QString("Invalid regular expression at position %1: %2")
                .arg(expression.patternErrorOffset()).arg(expression.errorString()));
            return plan;
        }
    }
    QSet<QString> sourceKeys;
    QSet<QString> destinationKeys;
    qint64 sequence = rule.sequenceStart;

    for (const QString& path : paths) {
        if (stopPreview(plan, cancellation))
            return plan;
        const QFileInfo info(path);
        if (!info.exists()) {
            plan.errors.append("Source no longer exists: " + path);
            continue;
        }
        const QString sourceKey = pathKey(path);
        if (sourceKeys.contains(sourceKey)) {
            plan.errors.append("Source is selected more than once: " + path);
            continue;
        }
        sourceKeys.insert(sourceKey);

        QString base = info.isDir() ? info.fileName() : info.completeBaseName();
        if (rule.useRegularExpression)
            base.replace(expression, rule.replace);
        else if (!rule.find.isEmpty())
            base.replace(rule.find, rule.replace,
                rule.caseSensitive ? Qt::CaseSensitive : Qt::CaseInsensitive);
        base = convertCase(base, rule.caseConversion);
        base = rule.prefix + base + rule.suffix;
        if (rule.addSequence) {
            base += QString("_%1").arg(sequence, rule.sequencePadding, 10, QChar('0'));
            if ((rule.sequenceStep > 0
                    && sequence > std::numeric_limits<qint64>::max() - rule.sequenceStep)
                || (rule.sequenceStep < 0
                    && sequence < std::numeric_limits<qint64>::lowest() - rule.sequenceStep)) {
                plan.errors.append("Sequence numbers exceed the supported range.");
                return plan;
            }
            sequence += rule.sequenceStep;
        }
        const QString extension = info.isDir() || info.suffix().isEmpty()
            ? QString() : "." + info.suffix();
        const ValidationResult validation = validateWindowsName(base + extension);
        if (!validation.valid) {
            plan.errors.append(info.fileName() + ": " + validation.error);
            continue;
        }
        const QString destination = info.dir().filePath(validation.normalizedName);
        const QString destinationKey = pathKey(destination);
        if (destinationKeys.contains(destinationKey)) {
            plan.errors.append("Multiple items would become: " + validation.normalizedName);
            continue;
        }
        destinationKeys.insert(destinationKey);
        const auto snapshot = captureSource(info.absoluteFilePath());
        if (!snapshot.valid)
            plan.errors.append("Could not verify source identity: " + path);
        plan.items.append({info.absoluteFilePath(), QFileInfo(destination).absoluteFilePath(),
            info.isDir(), snapshot});
    }

    for (const BatchRenameItem& item : plan.items) {
        if (stopPreview(plan, cancellation))
            return plan;
        const QString destinationKey = pathKey(item.destination);
        if (destinationKey == pathKey(item.source))
            continue;
        if (QFileInfo::exists(item.destination) && !sourceKeys.contains(destinationKey))
            plan.errors.append("Destination already exists: " + item.destination);
    }
    stopPreview(plan, cancellation);
    return plan;
}

BatchRenamePlan createListedRenamePlan(const QStringList& paths, const QStringList& names,
    bool preserveExtensions, const std::atomic_bool* cancellation)
{
    BatchRenamePlan plan;
    if (stopPreview(plan, cancellation))
        return plan;
    if (paths.isEmpty() || names.size() != paths.size()) {
        plan.errors.append("Enter exactly one name for each selected item.");
        return plan;
    }
    QSet<QString> sources;
    QSet<QString> destinations;
    for (qsizetype index = 0; index < paths.size(); ++index) {
        if (stopPreview(plan, cancellation))
            return plan;
        const QFileInfo source(paths[index]);
        const QString sourceKey = pathKey(paths[index]);
        if (!source.exists() || sources.contains(sourceKey)) {
            plan.errors.append("Source is missing or selected more than once: " + paths[index]);
            continue;
        }
        sources.insert(sourceKey);
        const QString extension = preserveExtensions && !source.isDir() && !source.suffix().isEmpty()
            ? "." + source.suffix() : QString();
        const auto baseValidation = validateWindowsName(names[index]);
        const auto validation = validateWindowsName(names[index] + extension);
        if (!baseValidation.valid || !validation.valid) {
            plan.errors.append(QString("Row %1: %2").arg(index + 1).arg(
                !baseValidation.valid ? baseValidation.error : validation.error));
            continue;
        }
        const QString destination = source.dir().absoluteFilePath(validation.normalizedName);
        const QString destinationKey = pathKey(destination);
        if (destinations.contains(destinationKey))
            plan.errors.append("Multiple items would become: " + validation.normalizedName);
        destinations.insert(destinationKey);
        const auto snapshot = captureSource(source.absoluteFilePath());
        if (!snapshot.valid)
            plan.errors.append("Could not verify source identity: " + paths[index]);
        plan.items.append({source.absoluteFilePath(), destination, source.isDir(), snapshot});
    }
    for (const auto& item : plan.items) {
        if (stopPreview(plan, cancellation))
            return plan;
        if (QFileInfo::exists(item.destination) && !sources.contains(pathKey(item.destination)))
            plan.errors.append("Destination already exists: " + item.destination);
    }
    stopPreview(plan, cancellation);
    return plan;
}

BatchRenamePlan createNumberedRenamePlan(const QStringList& paths, const QString& label,
    int start, int padding, int step, const std::atomic_bool* cancellation)
{
    BatchRenamePlan plan;
    if (stopPreview(plan, cancellation))
        return plan;
    if (!validateWindowsName(label).valid || padding < 1 || padding > 12 || step == 0) {
        plan.errors.append("Enter a valid name, 1 to 12 minimum digits, and a nonzero step.");
        return plan;
    }
    QStringList names;
    qint64 number = start;
    for (qsizetype index = 0; index < paths.size(); ++index) {
        if (stopPreview(plan, cancellation))
            return plan;
        names.append(label + QString(" %1").arg(number, padding, 10, QChar('0')));
        if ((step > 0 && number > std::numeric_limits<qint64>::max() - step)
            || (step < 0 && number < std::numeric_limits<qint64>::lowest() - step)) {
            plan.errors.append("Sequence numbers exceed the supported range.");
            return plan;
        }
        number += step;
    }
    return createListedRenamePlan(paths, names, true, cancellation);
}

BatchRenameResult executeBatchRename(const BatchRenamePlan& plan)
{
    BatchRenameResult result;
    if (!plan.isValid()) {
        result.errors = plan.errors.isEmpty() ? QStringList{"The rename plan is empty."} : plan.errors;
        return result;
    }

    // Recheck the complete mapping before staging any item; preview may have been open a while.
    QStringList sources;
    QStringList names;
    for (const auto& item : plan.items) {
        const QFileInfo source(item.source);
        const QFileInfo destination(item.destination);
        if (!sourceMatches(item, item.source)) {
            result.errors.append("Source changed or cannot be verified; close and reopen Batch rename: "
                + item.source);
            return result;
        }
        if (pathKey(source.absolutePath()) != pathKey(destination.absolutePath())
            || source.isDir() != item.directory) {
            result.errors.append("The rename source or destination changed; review the plan again.");
            return result;
        }
        sources.append(item.source);
        names.append(destination.fileName());
    }
    const auto checked = createListedRenamePlan(sources, names, false);
    if (!checked.isValid()) {
        result.errors = checked.errors;
        if (result.errors.isEmpty())
            result.errors.append("There are no valid changes to apply.");
        return result;
    }

    QList<platform::GuardedBatchRenameItem> guarded;
    for (const BatchRenameItem& item : plan.items) {
        if (pathKey(item.source) == pathKey(item.destination)
            && item.source == item.destination)
            continue;
        if (item.directory) {
            for (const BatchRenameItem& other : plan.items) {
                if (pathKey(item.source) != pathKey(other.source)
                        && isPathInside(other.source, item.source)) {
                    result.errors.append("A folder and an item inside it cannot be renamed in one batch: "
                        + item.source);
                    return result;
                }
            }
        }
        guarded.append({item.source, item.destination, item.directory, item.sourceSnapshot});
    }
    const auto native = platform::guardedBatchRename(guarded);
    result.ok = native.ok;
    result.renamed = native.renamed;
    result.rollbackComplete = native.rollbackComplete;
    result.errors = native.errors;
    result.manualRecoveryPaths = native.manualRecoveryPaths;
    return result;
}

} // namespace filemanager::files
