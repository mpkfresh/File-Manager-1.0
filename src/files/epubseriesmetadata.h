#pragma once

#include <QString>
#include <atomic>

namespace filemanager::files {

struct EpubSeriesMetadata
{
    QString series;
    int number = 0;
    QString title;
    QString error;
    // An explicit but unusable series claim, an unreadable package, or a source
    // changed while reading must not fall back to a filename-derived proposal.
    bool blocksFilenameFallback = false;
    bool valid() const { return error.isEmpty() && !series.isEmpty() && number > 0 && !title.isEmpty(); }
};

// Bounded EPUB 3 package-metadata inspection on an explicitly selected file.
// This reads container.xml and one OPF package document, never publication
// content or an online metadata service. Selected NAS/mapped storage may involve
// filesystem network I/O.
// Only one explicitly typed series collection with an integer group-position is accepted.
// Cancellation is checked between bounded archive/metadata operations; it cannot
// interrupt a Windows filesystem call that is already in progress.
EpubSeriesMetadata readEpubSeriesMetadata(const QString& path,
    const std::atomic_bool* cancelled = nullptr);

} // namespace filemanager::files
