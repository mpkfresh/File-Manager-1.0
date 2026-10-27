#include "ui/filepane.h"
#include "ui/tabbedpanel.h"
#include "preview/thumbnailprovider.h"
#include "ui/presentationtiming.h"

#include <QApplication>
#include <QAbstractItemView>
#include <QCache>
#include <QIconEngine>
#include <QPainter>
#include <QDir>
#include <QElapsedTimer>
#include <QEvent>
#include <QEventLoop>
#include <QFile>
#include <QFileSystemModel>
#include <QLoggingCategory>
#include <QPaintEvent>
#include <QSettings>
#include <QPushButton>
#include <QRegularExpression>
#include <QSortFilterProxyModel>
#include <QTabWidget>
#include <QTemporaryDir>
#include <QThread>
#include <QTimer>
#include <QWindow>

#include <algorithm>
#include <array>
#include <atomic>
#include <cstdio>
#include <cstring>
#include <functional>

namespace {

constexpr int SmallEntryCount = 100;
constexpr int LargeEntryCount = 10000;
constexpr int WarmSampleCount = 30;
constexpr int PaneConstructionSampleCount = 20;
constexpr qint64 LoadTimeoutMs = 20000;
std::atomic_uint benchmarkWarnings{0};
QtMessageHandler previousMessageHandler = nullptr;

enum class Phase { None, Construction, FirstSmall, FirstLarge, HiddenLarge, ListLarge,
    ModelControls, WarmNavigation, History, Supersession, Tabs, Count };
constexpr std::array PhaseNames{"none", "construction", "first_small", "first_large",
    "hidden_large", "list_large", "model_controls", "warm_navigation", "history",
    "supersession", "tabs"};
constexpr std::array EventNames{"paint", "timer", "queued_callback", "layout", "other"};
struct EventTiming {
    quint64 count = 0;
    qint64 totalUs = 0;
    qint64 maxUs = 0;
    int maxEventType = 0;
};
std::array<std::array<EventTiming, EventNames.size()>, static_cast<size_t>(Phase::Count)> eventTimings{};
Phase currentPhase = Phase::None;
struct TurnTiming {
    quint64 sequence = 0;
    quint64 notifications = 0;
    qint64 notifyNs = 0;
    qint64 maxNotifyNs = 0;
    int maxEventType = 0;
    qint64 quitUs = 0;
    qint64 wallUs = 0;
};
TurnTiming* measuredTurn = nullptr;
std::array<TurnTiming, static_cast<size_t>(Phase::Count)> longestTurns{};
quint64 measuredTurnSequence = 0;
constexpr std::array PresentationNames{"model_data", "icon_lookup", "type_lookup", "delegate_paint",
    "style_option", "thumbnail_request", "style_draw", "delegate_size_hint",
    "icon_paint", "icon_pixmap", "icon_metrics", "listing_completion", "listing_unchanged_check",
    "listing_refresh", "metadata_size", "metadata_modified", "metadata_change", "metadata_birth",
    "metadata_permissions", "metadata_type", "listing_hints", "listing_start",
    "listing_snapshot", "listing_publish", "pane_restore", "pane_columns_save",
    "pane_empty_state", "icon_notifications", "column_profile_lookup"};
static_assert(PresentationNames.size()
    == static_cast<size_t>(filemanager::ui::diagnostics::PresentationStage::Count));
constexpr std::array RoleNames{"decoration", "name", "size", "type", "modified", "created",
    "extension", "dimensions", "duration", "title", "artists", "pages", "availability", "other"};
struct PresentationTiming {
    quint64 count = 0;
    qint64 totalNs = 0;
    qint64 maxNs = 0;
};
using TurnPresentationTimings = std::array<PresentationTiming, PresentationNames.size()>;
TurnPresentationTimings* measuredPresentations = nullptr;
std::array<TurnPresentationTimings, static_cast<size_t>(Phase::Count)> longestTurnPresentations{};
std::array<unsigned, static_cast<size_t>(Phase::Count)> slowTurnReports{};
std::array<qint64, static_cast<size_t>(Phase::Count)> lastReportedSlowTurn{};
std::array<std::array<std::array<PresentationTiming, RoleNames.size()>, PresentationNames.size()>,
    static_cast<size_t>(Phase::Count)> presentationTimings{};

void recordPresentationTiming(filemanager::ui::diagnostics::PresentationStage stage,
    int role, int column, qint64 nanoseconds)
{
    if (currentPhase == Phase::None)
        return;
    size_t roleKind = RoleNames.size() - 1;
    if (role == Qt::DecorationRole)
        roleKind = 0;
    else if ((role == Qt::DisplayRole || role == Qt::EditRole) && column >= 0 && column < 12)
        roleKind = static_cast<size_t>(column + 1);
    auto& timing = presentationTimings[static_cast<size_t>(currentPhase)]
        [static_cast<size_t>(stage)][roleKind];
    ++timing.count;
    timing.totalNs += nanoseconds;
    timing.maxNs = qMax(timing.maxNs, nanoseconds);
    if (measuredPresentations) {
        auto& sample = (*measuredPresentations)[static_cast<size_t>(stage)];
        ++sample.count;
        sample.totalNs += nanoseconds;
        sample.maxNs = qMax(sample.maxNs, nanoseconds);
    }
}

void beginPhase(Phase phase)
{
    currentPhase = phase;
    std::printf("navigation_phase phase=%s boundary=begin partial_evidence_not_pass=yes\n",
        PhaseNames[static_cast<size_t>(phase)]);
    std::fflush(stdout);
}

void printPresentationTimings()
{
    std::printf("presentation_profile scope=calling_ui_thread stages=inclusive_do_not_sum "
        "data=fixed_stage_role_counts_and_durations observer_overhead=included\n");
    for (size_t phase = 1; phase < presentationTimings.size(); ++phase) {
        for (size_t stage = 0; stage < PresentationNames.size(); ++stage) {
            for (size_t role = 0; role < RoleNames.size(); ++role) {
                const auto& timing = presentationTimings[phase][stage][role];
                if (timing.count == 0)
                    continue;
                std::printf("presentation_phase=%s stage=%s role=%s count=%llu total_us=%lld max_us=%lld\n",
                    PhaseNames[phase], PresentationNames[stage], RoleNames[role],
                    static_cast<unsigned long long>(timing.count),
                    static_cast<long long>(timing.totalNs / 1000),
                    static_cast<long long>(timing.maxNs / 1000));
            }
        }
    }
}

bool hasPresentationSamples()
{
    bool complete = true;
    for (size_t stage = 0; stage < PresentationNames.size(); ++stage) {
        // Styles and short-circuit metadata comparisons choose optional routes.
        // Require a first-small render below and the core listing stages.
        using Stage = filemanager::ui::diagnostics::PresentationStage;
        if (stage >= static_cast<size_t>(Stage::IconPaint)
                && stage != static_cast<size_t>(Stage::ListingCompletion)
                && stage != static_cast<size_t>(Stage::ListingStart)
                && stage != static_cast<size_t>(Stage::ListingPublish))
            continue;
        quint64 count = 0;
        for (const auto& phase : presentationTimings) {
            for (const auto& role : phase[stage])
                count += role.count;
        }
        if (count == 0) {
            std::fprintf(stderr, "Missing presentation timing stage: %s\n", PresentationNames[stage]);
            complete = false;
        }
    }
    const auto& firstSmallDraw = presentationTimings[static_cast<size_t>(Phase::FirstSmall)]
        [static_cast<size_t>(filemanager::ui::diagnostics::PresentationStage::StyleDraw)];
    if (firstSmallDraw[1].count == 0 || firstSmallDraw[2].count == 0) {
        std::fprintf(stderr, "Missing first-small name/size column drawing samples.\n");
        complete = false;
    }
    using Stage = filemanager::ui::diagnostics::PresentationStage;
    const auto& firstSmall = presentationTimings[static_cast<size_t>(Phase::FirstSmall)];
    if (firstSmall[static_cast<size_t>(Stage::IconPaint)][0].count
            + firstSmall[static_cast<size_t>(Stage::IconPixmap)][0].count == 0) {
        std::fprintf(stderr, "Missing first-small forwarded icon rendering samples.\n");
        complete = false;
    }
    return complete;
}

// These forwarding engines exist only in this executable. Reuse each wrapper
// while cached so repeated paints do not manufacture new QIcon cache identities.
class ObservedIconEngine final : public QIconEngine {
public:
    explicit ObservedIconEngine(const QIcon& source) : source_(source) {}
    QIconEngine* clone() const override { return new ObservedIconEngine(source_); }
    QString key() const override { return QStringLiteral("NavigationObservedIcon"); }
    QString iconName() override { return source_.name(); }
    bool isNull() override { return source_.isNull(); }
    void paint(QPainter* painter, const QRect& rect, QIcon::Mode mode, QIcon::State state) override
    {
        const filemanager::ui::diagnostics::PresentationScope timing(
            filemanager::ui::diagnostics::PresentationStage::IconPaint, Qt::DecorationRole, 0);
        source_.paint(painter, rect, Qt::AlignCenter, mode, state);
    }
    QPixmap pixmap(const QSize& size, QIcon::Mode mode, QIcon::State state) override
    {
        const filemanager::ui::diagnostics::PresentationScope timing(
            filemanager::ui::diagnostics::PresentationStage::IconPixmap, Qt::DecorationRole, 0);
        return source_.pixmap(size, mode, state);
    }
    QPixmap scaledPixmap(const QSize& size, QIcon::Mode mode, QIcon::State state, qreal scale) override
    {
        const filemanager::ui::diagnostics::PresentationScope timing(
            filemanager::ui::diagnostics::PresentationStage::IconPixmap, Qt::DecorationRole, 0);
        return source_.pixmap(size, scale, mode, state);
    }
    QSize actualSize(const QSize& size, QIcon::Mode mode, QIcon::State state) override
    {
        const filemanager::ui::diagnostics::PresentationScope timing(
            filemanager::ui::diagnostics::PresentationStage::IconMetrics, Qt::DecorationRole, 0);
        return source_.actualSize(size, mode, state);
    }
    QList<QSize> availableSizes(QIcon::Mode mode, QIcon::State state) override
    {
        const filemanager::ui::diagnostics::PresentationScope timing(
            filemanager::ui::diagnostics::PresentationStage::IconMetrics, Qt::DecorationRole, 0);
        return source_.availableSizes(mode, state);
    }
private:
    QIcon source_;
};

QCache<qint64, QIcon>* observedIcons = nullptr;
QIcon observeIcon(const QIcon& source)
{
    if (!observedIcons || source.isNull())
        return source;
    if (const QIcon* cached = observedIcons->object(source.cacheKey()))
        return *cached;
    QIcon wrapped(new ObservedIconEngine(source));
    wrapped.setIsMask(source.isMask());
    observedIcons->insert(source.cacheKey(), new QIcon(wrapped));
    return wrapped;
}

bool verifyIconForwarding()
{
    // Run after navigation measurements to avoid warming the initial draw.
    QIcon source;
    for (QIcon::Mode mode : {QIcon::Normal, QIcon::Disabled, QIcon::Active, QIcon::Selected}) {
        for (QIcon::State state : {QIcon::Off, QIcon::On}) {
            for (int extent : {16, 32, 64}) {
                QPixmap pixels(extent, extent / 2);
                pixels.fill(QColor(40 + 45 * int(mode), 40 + 90 * int(state), extent));
                source.addPixmap(pixels, mode, state);
            }
        }
    }
    QIcon wrapped(new ObservedIconEngine(source));
    for (QIcon::Mode mode : {QIcon::Normal, QIcon::Disabled, QIcon::Active, QIcon::Selected}) {
        for (QIcon::State state : {QIcon::Off, QIcon::On}) {
            if (source.availableSizes(mode, state) != wrapped.availableSizes(mode, state))
                return false;
            for (QSize size : {QSize(16, 16), QSize(24, 32), QSize(48, 48)}) {
                if (source.actualSize(size, mode, state) != wrapped.actualSize(size, mode, state))
                    return false;
                for (qreal dpr : {1.0, 1.5, 2.0}) {
                    const QPixmap expected = source.pixmap(size, dpr, mode, state);
                    const QPixmap actual = wrapped.pixmap(size, dpr, mode, state);
                    if (expected.devicePixelRatio() != actual.devicePixelRatio()
                            || expected.toImage() != actual.toImage())
                        return false;
                    QImage direct(QSize(80, 80) * dpr, QImage::Format_ARGB32_Premultiplied);
                    direct.setDevicePixelRatio(dpr);
                    direct.fill(Qt::transparent);
                    QImage forwarded = direct;
                    { QPainter painter(&direct); source.paint(&painter, QRect(QPoint(3, 5), size),
                        Qt::AlignCenter, mode, state); }
                    { QPainter painter(&forwarded); wrapped.paint(&painter, QRect(QPoint(3, 5), size),
                        Qt::AlignCenter, mode, state); }
                    if (direct != forwarded)
                        return false;
                }
            }
        }
    }
    return true;
}
constexpr std::array ReceiverNames{"QTreeView", "QListView", "QWidget", "QTimer",
    "QSingleShotTimer", "QEventDispatcherWin32", "QFileSystemModel",
    "directory_listing_future", "icon_future", "other_future", "other"};
struct NestedTiming {
    quint64 count = 0;
    qint64 maxInclusiveUs = 0;
    qint64 maxExclusiveUs = 0;
    int maxExclusiveType = 0;
};
std::array<std::array<std::array<NestedTiming, ReceiverNames.size()>, EventNames.size()>,
    static_cast<size_t>(Phase::Count)> nestedTimings{};

size_t receiverCategory(const QObject* receiver)
{
    const char* name = receiver->metaObject()->className();
    // Fixed categories only: attribute FutureCallOut delivery without recording
    // object names, paths or result payloads, or changing the measured callback.
    if (receiver->inherits("QFutureWatcherBase")) {
        const QObject* owner = receiver->parent();
        if (owner && owner->inherits("filemanager::ui::LocalDirectoryModel"))
            name = "directory_listing_future";
        else if (owner && owner->inherits("filemanager::preview::FileIconScheduler"))
            name = "icon_future";
        else
            name = "other_future";
    }
    for (size_t index = 0; index + 1 < ReceiverNames.size(); ++index) {
        if (std::strcmp(name, ReceiverNames[index]) == 0)
            return index;
    }
    return ReceiverNames.size() - 1;
}

size_t eventCategory(QEvent::Type type)
{
    switch (type) {
    case QEvent::Paint: return 0;
    case QEvent::Timer: return 1;
    case QEvent::MetaCall: return 2;
    case QEvent::LayoutRequest: return 3;
    default: return 4;
    }
}

struct PaintObservation {
    FilePane* pane = nullptr;
    QString expectedPath;
    QElapsedTimer* timer = nullptr;
    qint64 firstPaintUs = 0;
    qint64 completeIconPaintUs = 0;
    quint64 viewportPaints = 0;
    quint64 matchingPathPaints = 0;
    quint64 matchingRowPaints = 0;
    quint64 fullViewportPaints = 0;
    quint64 readyIconRequests = 0;
    quint64 unavailableIconRequests = 0;
};
PaintObservation* pendingPaint = nullptr;
struct IconPaintCounts { int ready = 0; int unavailable = 0; };
IconPaintCounts* currentIconPaint = nullptr;
void recordIconReadiness(bool ready)
{
    if (currentIconPaint) {
        if (ready) ++currentIconPaint->ready;
        else ++currentIconPaint->unavailable;
    }
}

void observeCompletedPaint(QObject* receiver, QEvent* event, const IconPaintCounts& icons)
{
    auto* observation = pendingPaint;
    if (!observation || observation->completeIconPaintUs != 0 || event->type() != QEvent::Paint)
        return;
    auto* view = observation->pane->view();
    if (!view || receiver != view->viewport())
        return;
    ++observation->viewportPaints;
    observation->readyIconRequests += icons.ready;
    observation->unavailableIconRequests += icons.unavailable;
    if (!view->isVisible())
        return;
    // Timestamp immediately after Qt's paint delivery, before qualification overhead.
    const qint64 completedUs = observation->timer->nsecsElapsed() / 1000;
    if (QFileInfo(observation->pane->currentPath()).absoluteFilePath() != observation->expectedPath)
        return;
    ++observation->matchingPathPaints;
    const QModelIndex first = view->model()->index(0, 0, view->rootIndex());
    if (!first.isValid() || first.data(Qt::DisplayRole).toString().isEmpty()
            || QFileInfo(first.data(QFileSystemModel::FilePathRole).toString()).absolutePath()
                != observation->expectedPath)
        return;
    ++observation->matchingRowPaints;
    const QRect visibleRow = view->visualRect(first).intersected(view->viewport()->rect());
    const auto& region = static_cast<QPaintEvent*>(event)->region();
    if (region.contains(view->viewport()->rect()))
        ++observation->fullViewportPaints;
    if (!visibleRow.isEmpty() && region.intersects(visibleRow)) {
        if (observation->firstPaintUs == 0)
            observation->firstPaintUs = qMax<qint64>(1, completedUs);
        // A partial repaint of one ready row must not certify the other rows.
        // Fixtures contain native icons; zero observed raster requests is insufficient.
        if (region.contains(view->viewport()->rect()) && icons.ready > 0 && icons.unavailable == 0)
            observation->completeIconPaintUs = qMax<qint64>(1, completedUs);
    }
}

class BenchmarkApplication final : public QApplication
{
public:
    using QApplication::QApplication;

    bool notify(QObject* receiver, QEvent* event) override
    {
        // QApplication can also deliver worker-thread events; only profile the UI thread.
        if (QThread::currentThread() != thread())
            return QApplication::notify(receiver, event);
        // Preserve outermost totals; independently measure nested delivery's exclusive time.
        // Only fixed receiver class buckets are collected, never identities or paths.
        const bool active = currentPhase != Phase::None;
        const bool measure = notificationDepth_++ == 0 && active;
        const auto phase = static_cast<size_t>(currentPhase);
        const QEvent::Type eventType = event->type();
        const size_t category = eventCategory(eventType);
        const size_t receiverKind = active ? receiverCategory(receiver) : 0;
        Frame frame;
        Frame* previousFrame = currentFrame_;
        currentFrame_ = &frame;
        QElapsedTimer timer;
        if (active)
            timer.start();
        IconPaintCounts icons;
        IconPaintCounts* previousIconPaint = currentIconPaint;
        if (eventType == QEvent::Paint)
            currentIconPaint = &icons;
        const bool result = QApplication::notify(receiver, event);
        const qint64 elapsedNs = active ? timer.nsecsElapsed() : 0;
        observeCompletedPaint(receiver, event, icons);
        currentIconPaint = previousIconPaint;
        currentFrame_ = previousFrame;
        if (previousFrame)
            previousFrame->childrenNs += elapsedNs;
        if (active) {
            NestedTiming& nested = nestedTimings[phase][category][receiverKind];
            const qint64 exclusiveUs = qMax<qint64>(0, elapsedNs - frame.childrenNs) / 1000;
            ++nested.count;
            nested.maxInclusiveUs = qMax(nested.maxInclusiveUs, elapsedNs / 1000);
            if (exclusiveUs > nested.maxExclusiveUs || nested.count == 1) {
                nested.maxExclusiveUs = exclusiveUs;
                nested.maxExclusiveType = static_cast<int>(eventType);
            }
        }
        if (measure) {
            const qint64 elapsedUs = elapsedNs / 1000;
            if (measuredTurn) {
                ++measuredTurn->notifications;
                measuredTurn->notifyNs += elapsedNs;
                if (elapsedNs > measuredTurn->maxNotifyNs) {
                    measuredTurn->maxNotifyNs = elapsedNs;
                    measuredTurn->maxEventType = static_cast<int>(eventType);
                }
            }
            EventTiming& timing = eventTimings[phase][category];
            ++timing.count;
            timing.totalUs += elapsedUs;
            if (elapsedUs > timing.maxUs || timing.count == 1) {
                timing.maxUs = elapsedUs;
                timing.maxEventType = static_cast<int>(eventType);
            }
        }
        --notificationDepth_;
        return result;
    }

private:
    struct Frame { qint64 childrenNs = 0; };
    Frame* currentFrame_ = nullptr;
    int notificationDepth_ = 0;
};

void printEventTimings()
{
    // A paced event-loop turn includes dispatch, waiting and instrumentation,
    // not just notify(). Preserve that wall time while exposing its boundaries.
    std::printf("turn_profile scope=longest_measured_loop_per_phase "
        "unattributed=wall_minus_outermost_notify_includes_wait_dispatch_and_instrumentation\n");
    for (size_t phase = 1; phase < longestTurns.size(); ++phase) {
        const auto& turn = longestTurns[phase];
        if (!turn.sequence)
            continue;
        std::printf("turn_phase=%s sequence=%llu wall_us=%lld quit_requested_us=%lld "
            "after_quit_us=%lld notifications=%llu notify_total_us=%lld "
            "notify_max_us=%lld notify_max_event_type=%d unattributed_us=%lld\n",
            PhaseNames[phase], static_cast<unsigned long long>(turn.sequence),
            static_cast<long long>(turn.wallUs), static_cast<long long>(turn.quitUs),
            static_cast<long long>(turn.wallUs - turn.quitUs),
            static_cast<unsigned long long>(turn.notifications),
            static_cast<long long>(turn.notifyNs / 1000),
            static_cast<long long>(turn.maxNotifyNs / 1000), turn.maxEventType,
            static_cast<long long>(turn.wallUs - turn.notifyNs / 1000));
        for (size_t stage = 0; stage < PresentationNames.size(); ++stage) {
            const auto& sample = longestTurnPresentations[phase][stage];
            if (!sample.count)
                continue;
            std::printf("longest_turn_stage phase=%s sequence=%llu stage=%s count=%llu "
                "total_us=%lld max_us=%lld\n", PhaseNames[phase],
                static_cast<unsigned long long>(turn.sequence), PresentationNames[stage],
                static_cast<unsigned long long>(sample.count),
                static_cast<long long>(sample.totalNs / 1000),
                static_cast<long long>(sample.maxNs / 1000));
        }
    }
    std::printf("event_profile scope=ui_thread_outermost_qapplication_notify nested_delivery=included "
        "direct_calls_and_dispatcher_work=excluded timing_overhead=included\n");
    for (size_t phase = 1; phase < eventTimings.size(); ++phase) {
        for (size_t category = 0; category < EventNames.size(); ++category) {
            const EventTiming& timing = eventTimings[phase][category];
            if (timing.count == 0)
                continue;
            std::printf("event_phase=%s category=%s count=%llu total_us=%lld max_us=%lld max_event_type=%d\n",
                PhaseNames[phase], EventNames[category],
                static_cast<unsigned long long>(timing.count),
                static_cast<long long>(timing.totalUs), static_cast<long long>(timing.maxUs),
                timing.maxEventType);
        }
    }
    std::printf("nested_event_profile exclusive=subtract_nested_notify "
        "receiver=fixed_class_bucket rows=max_inclusive_at_least_1000_us\n");
    for (size_t phase = 1; phase < nestedTimings.size(); ++phase) {
        for (size_t category = 0; category < EventNames.size(); ++category) {
            for (size_t receiver = 0; receiver < ReceiverNames.size(); ++receiver) {
                const NestedTiming& timing = nestedTimings[phase][category][receiver];
                if (timing.maxInclusiveUs < 1000)
                    continue;
                std::printf("nested_phase=%s category=%s receiver=%s count=%llu "
                    "max_inclusive_us=%lld max_exclusive_us=%lld max_exclusive_type=%d\n",
                    PhaseNames[phase], EventNames[category], ReceiverNames[receiver],
                    static_cast<unsigned long long>(timing.count),
                    static_cast<long long>(timing.maxInclusiveUs),
                    static_cast<long long>(timing.maxExclusiveUs), timing.maxExclusiveType);
            }
        }
    }
}

void recordBenchmarkMessage(QtMsgType type, const QMessageLogContext& context, const QString& message)
{
    if (type == QtWarningMsg || type == QtCriticalMsg || type == QtFatalMsg)
        benchmarkWarnings.fetch_add(1, std::memory_order_relaxed);
    if (previousMessageHandler)
        previousMessageHandler(type, context, message);
    else
        std::fprintf(stderr, "%s\n", qFormatLogMessage(type, context, message).toUtf8().constData());
}

struct NavigationTiming
{
    qint64 callMicroseconds = 0;
    qint64 firstContentMicroseconds = 0;
    qint64 firstPaintMicroseconds = 0;
    qint64 completeIconPaintMicroseconds = 0;
    qint64 completeMicroseconds = 0;
    qint64 maxEventTurnMicroseconds = 0;
};

bool createFixture(const QString& path, int count)
{
    if (!QDir().mkpath(path))
        return false;
    for (int index = 0; index < count; ++index) {
        QFile item(QDir(path).filePath(
            QStringLiteral("entry-%1.txt").arg(index, 6, 10, QLatin1Char('0'))));
        if (!item.open(QIODevice::WriteOnly) || item.write("x") != 1)
            return false;
    }
    return true;
}

void processOneTurn()
{
    QEventLoop events;
    // Pace observations so native messages can progress without zero-time polling.
    // This sampling floor is included in reported event turns and content latency.
    QTimer::singleShot(1, Qt::PreciseTimer, &events, &QEventLoop::quit);
    events.exec();
}

void processFor(qint64 milliseconds)
{
    QElapsedTimer timer;
    timer.start();
    while (timer.elapsed() < milliseconds)
        processOneTurn();
}

qint64 processMeasuredTurn()
{
    TurnTiming turn;
    TurnPresentationTimings samples{};
    turn.sequence = ++measuredTurnSequence;
    QEventLoop events;
    QElapsedTimer timer;
    // Same one-millisecond pacing as processOneTurn. Record when quit is
    // requested separately from when the dispatcher actually returns.
    QTimer::singleShot(1, Qt::PreciseTimer, &events, [&] {
        turn.quitUs = timer.nsecsElapsed() / 1000;
        events.quit();
    });
    TurnTiming* previous = measuredTurn;
    auto* previousPresentations = measuredPresentations;
    measuredTurn = &turn;
    measuredPresentations = &samples;
    timer.start();
    events.exec();
    turn.wallUs = timer.nsecsElapsed() / 1000;
    measuredTurn = previous;
    measuredPresentations = previousPresentations;
    const size_t phase = static_cast<size_t>(currentPhase);
    auto& longest = longestTurns[phase];
    if (turn.wallUs > longest.wallUs) {
        longest = turn;
        // Retain the longest completed turn even when it does not double the
        // previous streamed report. Copy after stopping the turn's clock.
        longestTurnPresentations[phase] = samples;
    }
    // Flush bounded evidence after timing the event turn, so a killed benchmark
    // can identify completed slow callbacks. Action elapsed time still includes
    // this diagnostic output; it is not a clean uninstrumented latency baseline.
    if (turn.wallUs > 100000 && slowTurnReports[phase] < 3
            && turn.wallUs >= 2 * lastReportedSlowTurn[phase]) {
        ++slowTurnReports[phase];
        lastReportedSlowTurn[phase] = turn.wallUs;
        std::printf("slow_turn phase=%s sequence=%llu wall_us=%lld notify_max_us=%lld "
            "notify_max_type=%d partial_evidence_not_pass=yes\n", PhaseNames[phase],
            static_cast<unsigned long long>(turn.sequence), static_cast<long long>(turn.wallUs),
            static_cast<long long>(turn.maxNotifyNs / 1000), turn.maxEventType);
        for (size_t stage = 0; stage < samples.size(); ++stage) {
            const auto& sample = samples[stage];
            if (!sample.count)
                continue;
            std::printf("slow_turn_stage sequence=%llu stage=%s count=%llu total_us=%lld max_us=%lld\n",
                static_cast<unsigned long long>(turn.sequence), PresentationNames[stage],
                static_cast<unsigned long long>(sample.count),
                static_cast<long long>(sample.totalNs / 1000), static_cast<long long>(sample.maxNs / 1000));
        }
        std::fflush(stdout);
    }
    return turn.wallUs;
}

NavigationTiming measureAction(FilePane& pane, const std::function<void()>& action,
    const QString& expectedPath, int expectedCount, bool expectPaint = true)
{
    NavigationTiming timing;
    const QString absoluteExpected = QFileInfo(expectedPath).absoluteFilePath();
    if (expectPaint) {
        // Make the target window available for ordinary paint delivery before timing the action.
        // Do not force repaint inside the measured interval.
        pane.window()->raise();
        processOneTurn();
    }
    QElapsedTimer timer;
    PaintObservation paint{&pane, absoluteExpected, &timer};
    struct ResetPaintObservation {
        ~ResetPaintObservation() { pendingPaint = nullptr; }
    } resetPaintObservation;
    pendingPaint = expectPaint ? &paint : nullptr;
    timer.start();
    action();
    timing.callMicroseconds = timer.nsecsElapsed() / 1000;
    while (timer.elapsed() < LoadTimeoutMs) {
        timing.maxEventTurnMicroseconds = qMax(
            timing.maxEventTurnMicroseconds, processMeasuredTurn());
        if (QFileInfo(pane.currentPath()).absoluteFilePath() != absoluteExpected)
            continue;
        const int visible = pane.visibleItemCount();
        if (visible > 0 && timing.firstContentMicroseconds == 0)
            timing.firstContentMicroseconds = timer.nsecsElapsed() / 1000;
        if (visible == expectedCount && timing.completeMicroseconds == 0)
            timing.completeMicroseconds = timer.nsecsElapsed() / 1000;
        if (visible == expectedCount && (!expectPaint || paint.completeIconPaintUs != 0))
            break;
    }
    timing.firstPaintMicroseconds = paint.firstPaintUs;
    timing.completeIconPaintMicroseconds = paint.completeIconPaintUs;
    if (expectPaint) {
        std::printf("icon_completion phase=%s first_text_paint_us=%lld complete_icon_paint_us=%lld\n",
            PhaseNames[static_cast<size_t>(currentPhase)], static_cast<long long>(paint.firstPaintUs),
            static_cast<long long>(paint.completeIconPaintUs));
        std::fflush(stdout);
    }
    if (expectPaint && paint.completeIconPaintUs == 0) {
        std::fprintf(stderr, "No complete native-icon viewport paint in phase %s within %lld ms.\n",
            PhaseNames[static_cast<size_t>(currentPhase)], static_cast<long long>(LoadTimeoutMs));
        const auto* view = pane.view();
        const auto* window = pane.window();
        const auto* handle = window ? window->windowHandle() : nullptr;
        std::fprintf(stderr, "Paint deadline: expected_rows=%d observed_rows=%d path_matches=%d "
            "view_visible=%d window_visible=%d minimized=%d exposed=%d active=%d "
            "viewport_paints=%llu matching_path_paints=%llu matching_row_paints=%llu "
            "full_viewport_paints=%llu ready_icon_requests=%llu unavailable_icon_requests=%llu\n",
            expectedCount, pane.visibleItemCount(),
            QFileInfo(pane.currentPath()).absoluteFilePath() == absoluteExpected,
            view && view->isVisible(), window && window->isVisible(),
            window && window->isMinimized(), handle && handle->isExposed(),
            window && window->isActiveWindow(),
            static_cast<unsigned long long>(paint.viewportPaints),
            static_cast<unsigned long long>(paint.matchingPathPaints),
            static_cast<unsigned long long>(paint.matchingRowPaints),
            static_cast<unsigned long long>(paint.fullViewportPaints),
            static_cast<unsigned long long>(paint.readyIconRequests),
            static_cast<unsigned long long>(paint.unavailableIconRequests));
        timing.completeMicroseconds = 0;
    }
    return timing;
}

NavigationTiming measureNavigation(FilePane& pane, const QString& path, int expectedCount,
    bool expectPaint = true)
{
    return measureAction(pane, [&pane, &path] { pane.navigate(path); }, path, expectedCount, expectPaint);
}

NavigationTiming measurePlainModel(const QString& path, int expectedCount, bool withProxy)
{
    NavigationTiming timing;
    QFileSystemModel model;
    model.setFilter(QDir::AllEntries | QDir::NoDotAndDotDot);
    model.setRootPath({});
    QSortFilterProxyModel proxy;
    if (withProxy) {
        proxy.setSourceModel(&model);
        proxy.setDynamicSortFilter(true);
        proxy.sort(0, Qt::AscendingOrder);
    }
    QElapsedTimer timer;
    timer.start();
    const QModelIndex sourceRoot = model.setRootPath(path);
    timing.callMicroseconds = timer.nsecsElapsed() / 1000;
    while (timer.elapsed() < LoadTimeoutMs) {
        timing.maxEventTurnMicroseconds = qMax(
            timing.maxEventTurnMicroseconds, processMeasuredTurn());
        const int visible = withProxy
            ? proxy.rowCount(proxy.mapFromSource(sourceRoot)) : model.rowCount(sourceRoot);
        if (visible > 0 && timing.firstContentMicroseconds == 0)
            timing.firstContentMicroseconds = timer.nsecsElapsed() / 1000;
        if (visible == expectedCount) {
            timing.completeMicroseconds = timer.nsecsElapsed() / 1000;
            break;
        }
    }
    return timing;
}

QPushButton* navigationButton(FilePane& pane, const QString& accessibleName)
{
    const auto buttons = pane.findChildren<QPushButton*>();
    const auto found = std::find_if(buttons.cbegin(), buttons.cend(),
        [&accessibleName](const QPushButton* button) {
            return button->accessibleName() == accessibleName;
        });
    return found == buttons.cend() ? nullptr : *found;
}

qint64 percentile(QList<qint64> values, double fraction)
{
    if (values.isEmpty())
        return 0;
    std::sort(values.begin(), values.end());
    const qsizetype index = qBound<qsizetype>(0,
        static_cast<qsizetype>((values.size() - 1) * fraction + 0.5), values.size() - 1);
    return values.at(index);
}

void printDistribution(const char* label, const QList<qint64>& values)
{
    std::printf("%s samples=%lld median_us=%lld p95_us=%lld min_us=%lld max_us=%lld\n",
        label, static_cast<long long>(values.size()),
        static_cast<long long>(percentile(values, 0.5)),
        static_cast<long long>(percentile(values, 0.95)),
        static_cast<long long>(percentile(values, 0.0)),
        static_cast<long long>(percentile(values, 1.0)));
}

int runThumbnailRejectionBenchmark()
{
    QTemporaryDir fixture;
    if (!fixture.isValid() || !createFixture(fixture.path(), 1000))
        return 30;
    QStringList paths;
    for (int index = 0; index < 1000; ++index)
        paths.append(QDir(fixture.path()).filePath(
            QStringLiteral("entry-%1.txt").arg(index, 6, 10, QLatin1Char('0'))));
    filemanager::preview::ThumbnailProvider provider;
    QList<qint64> samples;
    // Keep the first request's MIME initialization separate from steady-state rejection.
    QElapsedTimer first;
    first.start();
    if (provider.request(paths.first(), QSize(24, 24)))
        return 31;
    const qint64 firstUs = first.nsecsElapsed() / 1000;
    for (int sample = 0; sample < 30; ++sample) {
        QElapsedTimer timer;
        timer.start();
        for (const QString& path : paths) {
            if (provider.request(path, QSize(24, 24)))
                return 31;
        }
        samples.append(timer.nsecsElapsed() / 1000);
        processOneTurn();
    }
    std::printf("thumbnail_rejection files=1000 samples=30 first_request_us=%lld "
        "cache_state=os_uncontrolled measurement=direct_calls_no_event_pacing\n",
        static_cast<long long>(firstUs));
    printDistribution("thumbnail_rejection_1000_text_files", samples);
    return 0;
}

int runBenchmark()
{
    std::printf("navigation_progress boundary=setup partial_evidence_not_pass=yes\n");
    std::fflush(stdout);
    QCache<qint64, QIcon> iconWrappers(512);
    observedIcons = &iconWrappers;
    struct ResetPresentationObserver {
        ~ResetPresentationObserver() {
            filemanager::ui::diagnostics::presentationObserver = nullptr;
            filemanager::ui::diagnostics::iconDecorator = nullptr;
            filemanager::ui::diagnostics::iconReadinessObserver = nullptr;
            observedIcons = nullptr;
        }
    } resetPresentationObserver;
    filemanager::ui::diagnostics::presentationObserver = recordPresentationTiming;
    filemanager::ui::diagnostics::iconDecorator = observeIcon;
    filemanager::ui::diagnostics::iconReadinessObserver = recordIconReadiness;
    std::printf("icon_profile source=forwarded_qicon wrapper_cache_entries=512 "
        "wrapper_changes_icon_identity=yes application_hook=disabled\n");
    std::printf("navigation_benchmark observation_timer_ms=1 timer_type=precise "
        "first_content=row_count_observed cold=first_pane_visit_os_cache_uncontrolled\n");
    std::printf("navigation_paint observation=completed_viewport_paint_intersecting_target_first_row "
        "presentation=not_measured observer_overhead=included window_raise=before_action "
        "forced_repaint=no hidden_and_plain_controls=not_applicable\n");
    QTemporaryDir fixture;
    QTemporaryDir settings;
    if (!fixture.isValid() || !settings.isValid()) {
        std::fprintf(stderr, "Could not create isolated benchmark directories.\n");
        return 10;
    }
    const QString nativeOrganization = qEnvironmentVariable(
        "FILE_MANAGER_BENCHMARK_NATIVE_SETTINGS_ORGANIZATION");
    if (nativeOrganization.isEmpty()) {
        QSettings::setDefaultFormat(QSettings::IniFormat);
        QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, settings.path());
        std::printf("settings_backend=temporary_ini isolated_namespace=yes\n");
    } else {
#ifdef Q_OS_WIN
        const QRegularExpression allowed(QStringLiteral(
            "\\AFileManagerNavigationBenchmark_[0-9a-f]{32}\\z"));
        if (!allowed.match(nativeOrganization).hasMatch()) {
            std::fprintf(stderr, "Invalid isolated native-settings namespace.\n");
            return 10;
        }
        // The approved runner reserves and cleans this disposable registry
        // namespace, including after timeout. Never use the app's preferences.
        QCoreApplication::setOrganizationName(nativeOrganization);
        QSettings::setDefaultFormat(QSettings::NativeFormat);
        std::printf("settings_backend=native_registry isolated_namespace=yes\n");
#else
        std::fprintf(stderr, "Native registry diagnostics require Windows.\n");
        return 10;
#endif
    }

    const QString smallPath = QDir(fixture.path()).filePath(QStringLiteral("small"));
    const QString largePath = QDir(fixture.path()).filePath(QStringLiteral("large"));
    QElapsedTimer fixtureTimer;
    fixtureTimer.start();
    if (!createFixture(smallPath, SmallEntryCount)
            || !createFixture(largePath, LargeEntryCount)) {
        std::fprintf(stderr, "Could not populate the navigation benchmark fixture.\n");
        return 11;
    }
    const qint64 fixtureMs = fixtureTimer.elapsed();

    beginPhase(Phase::Construction);
    QElapsedTimer constructionTimer;
    constructionTimer.start();
    FilePane pane(FilePane::HomeIdentity);
    pane.resize(1000, 700);
    pane.show();
    processOneTurn();
    const qint64 paneConstructionUs = constructionTimer.nsecsElapsed() / 1000;

    beginPhase(Phase::FirstSmall);
    const NavigationTiming coldSmall = measureNavigation(pane, smallPath, SmallEntryCount);
    beginPhase(Phase::FirstLarge);
    const NavigationTiming coldLarge = measureNavigation(pane, largePath, LargeEntryCount);
    if (coldSmall.firstContentMicroseconds == 0 || coldSmall.completeMicroseconds == 0
            || coldLarge.firstContentMicroseconds == 0 || coldLarge.completeMicroseconds == 0) {
        std::fprintf(stderr,
            "A benchmark folder did not reach its expected row count within %lld ms.\n",
            static_cast<long long>(LoadTimeoutMs));
        return 12;
    }

    beginPhase(Phase::HiddenLarge);
    FilePane hiddenPane(FilePane::HomeIdentity);
    hiddenPane.resize(1000, 700);
    const NavigationTiming firstHiddenLarge = measureNavigation(
        hiddenPane, largePath, LargeEntryCount, false);
    beginPhase(Phase::ListLarge);
    FilePane listPane(FilePane::HomeIdentity);
    listPane.resize(1000, 700);
    listPane.setViewMode(FilePane::ViewMode::List);
    listPane.show();
    processOneTurn();
    const NavigationTiming firstListLarge = measureNavigation(
        listPane, largePath, LargeEntryCount);
    beginPhase(Phase::ModelControls);
    const NavigationTiming firstPlainModelLarge = measurePlainModel(
        largePath, LargeEntryCount, false);
    const NavigationTiming firstPlainProxyLarge = measurePlainModel(
        largePath, LargeEntryCount, true);
    if (firstHiddenLarge.completeMicroseconds == 0
            || firstListLarge.completeMicroseconds == 0
            || firstPlainModelLarge.completeMicroseconds == 0
            || firstPlainProxyLarge.completeMicroseconds == 0) {
        std::fprintf(stderr, "A model-delivery diagnostic did not complete.\n");
        return 20;
    }

    beginPhase(Phase::Construction);
    QList<qint64> paneConstructions;
    for (int sample = 0; sample < PaneConstructionSampleCount; ++sample) {
        QElapsedTimer timer;
        timer.start();
        auto* samplePane = new FilePane(FilePane::HomeIdentity);
        samplePane->resize(1000, 700);
        samplePane->show();
        processOneTurn();
        paneConstructions.append(timer.nsecsElapsed() / 1000);
        delete samplePane;
        processOneTurn();
    }

    beginPhase(Phase::WarmNavigation);
    QList<qint64> warmSmallCalls;
    QList<qint64> warmSmallFirstContent;
    QList<qint64> warmSmallFirstPaint;
    QList<qint64> warmSmallCompleteIcons;
    QList<qint64> warmLargeCalls;
    QList<qint64> warmLargeFirstContent;
    QList<qint64> warmLargeFirstPaint;
    QList<qint64> warmLargeCompleteIcons;
    QList<qint64> warmNavigationStalls;
    for (int sample = 0; sample < WarmSampleCount; ++sample) {
        const NavigationTiming small = measureNavigation(pane, smallPath, SmallEntryCount);
        const NavigationTiming large = measureNavigation(pane, largePath, LargeEntryCount);
        if (small.completeMicroseconds == 0 || large.completeMicroseconds == 0)
            return 13;
        warmSmallCalls.append(small.callMicroseconds);
        warmSmallFirstContent.append(small.firstContentMicroseconds);
        warmSmallFirstPaint.append(small.firstPaintMicroseconds);
        warmSmallCompleteIcons.append(small.completeIconPaintMicroseconds);
        warmLargeCalls.append(large.callMicroseconds);
        warmLargeFirstContent.append(large.firstContentMicroseconds);
        warmLargeFirstPaint.append(large.firstPaintMicroseconds);
        warmLargeCompleteIcons.append(large.completeIconPaintMicroseconds);
        warmNavigationStalls.append(qMax(
            small.maxEventTurnMicroseconds, large.maxEventTurnMicroseconds));
    }

    beginPhase(Phase::History);
    FilePane historyPane(FilePane::HomeIdentity);
    historyPane.resize(1000, 700);
    historyPane.show();
    const NavigationTiming historySmall = measureNavigation(
        historyPane, smallPath, SmallEntryCount);
    const NavigationTiming historyLarge = measureNavigation(
        historyPane, largePath, LargeEntryCount);
    QPushButton* back = navigationButton(historyPane, QStringLiteral("Back"));
    QPushButton* forward = navigationButton(historyPane, QStringLiteral("Forward"));
    FilePane parentPane(FilePane::HomeIdentity);
    parentPane.resize(1000, 700);
    parentPane.show();
    const NavigationTiming parentSmall = measureAction(parentPane,
        [&parentPane, &smallPath] { parentPane.navigate(smallPath, false); },
        smallPath, SmallEntryCount);
    QPushButton* up = navigationButton(parentPane, QStringLiteral("Up one folder"));
    if (!back || !forward || !up || historySmall.completeMicroseconds == 0
            || historyLarge.completeMicroseconds == 0
            || parentSmall.completeMicroseconds == 0) {
        std::fprintf(stderr, "The history-navigation fixtures did not initialize.\n");
        return 14;
    }
    QList<qint64> backFirstContent;
    QList<qint64> forwardFirstContent;
    QList<qint64> upFirstContent;
    QList<qint64> backFirstPaint;
    QList<qint64> forwardFirstPaint;
    QList<qint64> upFirstPaint;
    for (int sample = 0; sample < WarmSampleCount; ++sample) {
        const NavigationTiming backTiming = measureAction(historyPane,
            [back] { back->click(); }, smallPath, SmallEntryCount);
        const NavigationTiming forwardTiming = measureAction(historyPane,
            [forward] { forward->click(); }, largePath, LargeEntryCount);
        parentPane.navigate(smallPath, false);
        const NavigationTiming upTiming = measureAction(parentPane,
            [up] { up->click(); }, fixture.path(), 2);
        if (backTiming.completeMicroseconds == 0 || forwardTiming.completeMicroseconds == 0
                || upTiming.completeMicroseconds == 0) {
            std::fprintf(stderr, "A history or parent navigation did not complete.\n");
            return 15;
        }
        backFirstContent.append(backTiming.firstContentMicroseconds);
        forwardFirstContent.append(forwardTiming.firstContentMicroseconds);
        upFirstContent.append(upTiming.firstContentMicroseconds);
        backFirstPaint.append(backTiming.firstPaintMicroseconds);
        forwardFirstPaint.append(forwardTiming.firstPaintMicroseconds);
        upFirstPaint.append(upTiming.firstPaintMicroseconds);
    }

    beginPhase(Phase::Supersession);
    FilePane supersededPane(FilePane::HomeIdentity);
    supersededPane.resize(1000, 700);
    supersededPane.show();
    const auto supersede = [&supersededPane, &largePath, &smallPath] {
        supersededPane.navigate(largePath, false);
        processOneTurn();
        supersededPane.navigate(smallPath, false);
    };
    const NavigationTiming coldSuperseded = measureAction(
        supersededPane, supersede, smallPath, SmallEntryCount);
    processFor(2500);
    if (coldSuperseded.completeMicroseconds == 0
            || QFileInfo(supersededPane.currentPath()).absoluteFilePath()
                != QFileInfo(smallPath).absoluteFilePath()
            || supersededPane.visibleItemCount() != SmallEntryCount) {
        std::fprintf(stderr, "Superseded folder work replaced the final destination.\n");
        return 16;
    }
    QList<qint64> supersededAcknowledgements;
    QList<qint64> supersededFirstContent;
    QList<qint64> supersededFirstPaint;
    QList<qint64> supersededStalls;
    for (int sample = 0; sample < WarmSampleCount; ++sample) {
        const NavigationTiming timing = measureAction(
            supersededPane, supersede, smallPath, SmallEntryCount);
        if (timing.completeMicroseconds == 0)
            return 17;
        supersededAcknowledgements.append(timing.callMicroseconds);
        supersededFirstContent.append(timing.firstContentMicroseconds);
        supersededFirstPaint.append(timing.firstPaintMicroseconds);
        supersededStalls.append(timing.maxEventTurnMicroseconds);
        processFor(25);
        if (QFileInfo(supersededPane.currentPath()).absoluteFilePath()
                != QFileInfo(smallPath).absoluteFilePath()
                || supersededPane.visibleItemCount() != SmallEntryCount)
            return 18;
    }

    beginPhase(Phase::Tabs);
    TabbedPanel tabs(nullptr, false);
    tabs.resize(1000, 700);
    FilePane* smallTab = tabs.addTab(smallPath, false);
    FilePane* largeTab = tabs.addTab(largePath, true);
    tabs.show();
    QElapsedTimer tabLoadWait;
    tabLoadWait.start();
    while (tabLoadWait.elapsed() < LoadTimeoutMs
            && (smallTab->visibleItemCount() != SmallEntryCount
                || largeTab->visibleItemCount() != LargeEntryCount)) {
        processOneTurn();
    }
    auto* tabWidget = tabs.findChild<QTabWidget*>();
    if (!tabWidget || smallTab->visibleItemCount() != SmallEntryCount
            || largeTab->visibleItemCount() != LargeEntryCount) {
        std::fprintf(stderr, "The tab-switch fixtures did not finish loading.\n");
        return 19;
    }
    QList<qint64> tabSwitches;
    for (int sample = 0; sample < WarmSampleCount; ++sample) {
        const bool small = sample % 2 == 0;
        const NavigationTiming timing = measureAction(small ? *smallTab : *largeTab,
            [tabWidget, sample] { tabWidget->setCurrentIndex(sample % 2); },
            small ? smallPath : largePath, small ? SmallEntryCount : LargeEntryCount);
        if (timing.completeMicroseconds == 0)
            return 22;
        tabSwitches.append(timing.firstPaintMicroseconds);
    }

    beginPhase(Phase::None);
    std::printf("navigation_benchmark fixture_ms=%lld pane_construction_us=%lld\n",
        static_cast<long long>(fixtureMs), static_cast<long long>(paneConstructionUs));
    printDistribution("additional_pane_construction", paneConstructions);
    std::printf("cold_small call_us=%lld first_content_us=%lld complete_us=%lld max_event_turn_us=%lld\n",
        static_cast<long long>(coldSmall.callMicroseconds),
        static_cast<long long>(coldSmall.firstContentMicroseconds),
        static_cast<long long>(coldSmall.completeMicroseconds),
        static_cast<long long>(coldSmall.maxEventTurnMicroseconds));
    std::printf("cold_large call_us=%lld first_content_us=%lld complete_us=%lld max_event_turn_us=%lld\n",
        static_cast<long long>(coldLarge.callMicroseconds),
        static_cast<long long>(coldLarge.firstContentMicroseconds),
        static_cast<long long>(coldLarge.completeMicroseconds),
        static_cast<long long>(coldLarge.maxEventTurnMicroseconds));
    std::printf("first_hidden_large call_us=%lld first_content_us=%lld complete_us=%lld max_event_turn_us=%lld\n",
        static_cast<long long>(firstHiddenLarge.callMicroseconds),
        static_cast<long long>(firstHiddenLarge.firstContentMicroseconds),
        static_cast<long long>(firstHiddenLarge.completeMicroseconds),
        static_cast<long long>(firstHiddenLarge.maxEventTurnMicroseconds));
    std::printf("first_list_large call_us=%lld first_content_us=%lld complete_us=%lld max_event_turn_us=%lld\n",
        static_cast<long long>(firstListLarge.callMicroseconds),
        static_cast<long long>(firstListLarge.firstContentMicroseconds),
        static_cast<long long>(firstListLarge.completeMicroseconds),
        static_cast<long long>(firstListLarge.maxEventTurnMicroseconds));
    std::printf("first_plain_model_large call_us=%lld first_content_us=%lld complete_us=%lld max_event_turn_us=%lld\n",
        static_cast<long long>(firstPlainModelLarge.callMicroseconds),
        static_cast<long long>(firstPlainModelLarge.firstContentMicroseconds),
        static_cast<long long>(firstPlainModelLarge.completeMicroseconds),
        static_cast<long long>(firstPlainModelLarge.maxEventTurnMicroseconds));
    std::printf("first_plain_proxy_large call_us=%lld first_content_us=%lld complete_us=%lld max_event_turn_us=%lld\n",
        static_cast<long long>(firstPlainProxyLarge.callMicroseconds),
        static_cast<long long>(firstPlainProxyLarge.firstContentMicroseconds),
        static_cast<long long>(firstPlainProxyLarge.completeMicroseconds),
        static_cast<long long>(firstPlainProxyLarge.maxEventTurnMicroseconds));
    printDistribution("warm_small_call", warmSmallCalls);
    printDistribution("warm_small_first_content", warmSmallFirstContent);
    printDistribution("warm_large_call", warmLargeCalls);
    printDistribution("warm_large_first_content", warmLargeFirstContent);
    printDistribution("warm_navigation_max_event_turn", warmNavigationStalls);
    printDistribution("warm_back_first_content", backFirstContent);
    printDistribution("warm_forward_first_content", forwardFirstContent);
    printDistribution("warm_up_first_content", upFirstContent);
    std::printf("cold_superseded call_us=%lld first_content_us=%lld complete_us=%lld max_event_turn_us=%lld\n",
        static_cast<long long>(coldSuperseded.callMicroseconds),
        static_cast<long long>(coldSuperseded.firstContentMicroseconds),
        static_cast<long long>(coldSuperseded.completeMicroseconds),
        static_cast<long long>(coldSuperseded.maxEventTurnMicroseconds));
    printDistribution("warm_superseded_acknowledgement", supersededAcknowledgements);
    printDistribution("warm_superseded_first_content", supersededFirstContent);
    printDistribution("warm_superseded_max_event_turn", supersededStalls);
    printDistribution("warm_tab_switch", tabSwitches);
    std::printf("first_paint cold_small_us=%lld cold_large_us=%lld first_list_large_us=%lld "
        "cold_superseded_us=%lld\n", static_cast<long long>(coldSmall.firstPaintMicroseconds),
        static_cast<long long>(coldLarge.firstPaintMicroseconds),
        static_cast<long long>(firstListLarge.firstPaintMicroseconds),
        static_cast<long long>(coldSuperseded.firstPaintMicroseconds));
    printDistribution("warm_small_first_paint", warmSmallFirstPaint);
    printDistribution("warm_small_complete_icons", warmSmallCompleteIcons);
    printDistribution("warm_large_complete_icons", warmLargeCompleteIcons);
    printDistribution("warm_large_first_paint", warmLargeFirstPaint);
    printDistribution("warm_back_first_paint", backFirstPaint);
    printDistribution("warm_forward_first_paint", forwardFirstPaint);
    printDistribution("warm_up_first_paint", upFirstPaint);
    printDistribution("warm_superseded_first_paint", supersededFirstPaint);
    return 0;
}

} // namespace

int main(int argc, char* argv[])
{
    previousMessageHandler = qInstallMessageHandler(recordBenchmarkMessage);
    int result = 0;
    {
        BenchmarkApplication application(argc, argv);
        application.setOrganizationName(QStringLiteral("FileManagerTests"));
        application.setApplicationName(QStringLiteral("NavigationPerformanceBenchmark"));
        if (application.arguments().size() == 2
                && application.arguments().at(1) == QStringLiteral("--thumbnail-rejections")) {
            result = runThumbnailRejectionBenchmark();
        } else if (application.arguments().size() != 1) {
            std::fprintf(stderr, "Use no arguments or --thumbnail-rejections.\n");
            result = 1;
        } else {
            result = runBenchmark();
            currentPhase = Phase::None;
            printEventTimings();
            printPresentationTimings();
            if (result == 0 && !hasPresentationSamples())
                result = 23;
            if (result == 0 && !verifyIconForwarding()) {
                std::fprintf(stderr, "Icon forwarding fidelity check failed; reject timings.\n");
                result = 24;
            } else if (result == 0) {
                std::printf("icon_forwarding_fidelity modes=4 states=2 sizes=3 dpr=1,1.5,2 passed=yes\n");
            }
        }
    }
    qInstallMessageHandler(previousMessageHandler);
    const unsigned warnings = benchmarkWarnings.load(std::memory_order_relaxed);
    if (warnings != 0) {
        std::fprintf(stderr, "INVALID PERFORMANCE RUN: %u Qt warning/critical diagnostics. "
            "Do not accept emitted timings as a clean baseline.\n", warnings);
        return result == 0 ? 21 : result;
    }
    return result;
}
