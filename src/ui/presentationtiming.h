#pragma once

#include <QElapsedTimer>
#include <QIcon>

namespace filemanager::ui::diagnostics {

enum class PresentationStage {
    ModelData, IconLookup, TypeLookup, DelegatePaint, StyleOption,
    ThumbnailRequest, StyleDraw, DelegateSizeHint, IconPaint, IconPixmap, IconMetrics,
    ListingCompletion, ListingUnchangedCheck, ListingRefresh,
    MetadataSize, MetadataModified, MetadataChange, MetadataBirth, MetadataPermissions, MetadataType,
    ListingHints, ListingStart, ListingSnapshot, ListingPublish,
    PaneRestore, PaneColumnsSave, PaneEmptyState, IconNotifications,
    ColumnProfileLookup,
    Count
};

// Internal benchmark hook only: no paths, values, model identities or persistence.
// The application never installs an observer. Null observers do not start clocks.
using PresentationObserver = void (*)(PresentationStage, int role, int column, qint64 nanoseconds);
inline thread_local PresentationObserver presentationObserver = nullptr;
// A benchmark may wrap icons to time deferred rendering. The application leaves
// this null and continues passing the original icon directly to the style.
using IconDecorator = QIcon (*)(const QIcon&);
inline thread_local IconDecorator iconDecorator = nullptr;
// Benchmark-only readiness of each native icon raster requested during paint.
// False includes pending, queue overflow, unavailable and invalidated artwork.
using IconReadinessObserver = void (*)(bool ready);
inline thread_local IconReadinessObserver iconReadinessObserver = nullptr;

class PresentationScope final {
public:
    explicit PresentationScope(PresentationStage stage, int role = -1, int column = -1)
        : observer_(presentationObserver), stage_(stage), role_(role), column_(column)
    {
        if (observer_)
            timer_.start();
    }
    ~PresentationScope()
    {
        if (observer_)
            observer_(stage_, role_, column_, timer_.nsecsElapsed());
    }
    PresentationScope(const PresentationScope&) = delete;
    PresentationScope& operator=(const PresentationScope&) = delete;

private:
    PresentationObserver observer_;
    PresentationStage stage_;
    int role_;
    int column_;
    QElapsedTimer timer_;
};

} // namespace filemanager::ui::diagnostics
