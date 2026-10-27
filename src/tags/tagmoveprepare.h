#pragma once

#include "tags/tagmoveplan.h"

#include "files/transfercontroller.h"

namespace filemanager::tags {

struct SkippedTagMove
{
    QString source;
    QString reason;
};

struct PreparedTagMoves
{
    QList<filemanager::files::TransferRequest> requests;
    QList<SkippedTagMove> skipped;
    QStringList errors;
    bool cancelled = false;
    bool valid() const { return !cancelled && errors.isEmpty(); }
};

// Rechecks the exact included preview rows. Changed/unavailable items are
// reported and omitted; remaining items are grouped into guarded requests by
// destination root. No queue operation or filesystem mutation occurs here.
PreparedTagMoves prepareSelectedTagMoves(const TagMovePlan& preview,
    const QStringList& includedSources, const std::atomic_bool* cancelled = nullptr);

} // namespace filemanager::tags
