#include "transferprogressdialog.h"

#include <QDir>
#include <QFileInfo>
#include <QFontMetrics>
#include <QFormLayout>
#include <QGuiApplication>
#include <QHBoxLayout>
#include <QLabel>
#include <QLocale>
#include <QPainter>
#include <QPainterPath>
#include <QPaintEvent>
#include <QProgressBar>
#include <QPushButton>
#include <QScreen>
#include <QScrollArea>
#include <QSettings>
#include <QSizePolicy>
#include <QVBoxLayout>
#include <QVector>

#include <algorithm>

namespace filemanager::ui {
namespace {

QString remainingText(qint64 seconds)
{
    if (seconds < 0)
        return QStringLiteral("Calculating…");
    if (seconds == 0)
        return QStringLiteral("Finishing…");
    const qint64 hours = seconds / 3600;
    const qint64 minutes = (seconds % 3600) / 60;
    const qint64 remainder = seconds % 60;
    if (hours)
        return QStringLiteral("%1h %2m remaining").arg(hours).arg(minutes);
    if (minutes)
        return QStringLiteral("%1m %2s remaining").arg(minutes).arg(remainder);
    return QStringLiteral("%1s remaining").arg(remainder);
}

QString commonSourceLocation(const QStringList& sources)
{
    if (sources.isEmpty())
        return {};
    const QString first = QFileInfo(sources.constFirst()).absolutePath();
    for (const QString& source : sources) {
        if (QFileInfo(source).absolutePath().compare(first, Qt::CaseInsensitive) != 0)
            return QStringLiteral("Multiple locations");
    }
    return QDir::toNativeSeparators(first);
}

QLabel* pathValue(const QString& path, QWidget* parent)
{
    auto* value = new QLabel(parent);
    value->setText(QFontMetrics(value->font()).elidedText(path, Qt::ElideMiddle, 420));
    value->setToolTip(path);
    value->setTextInteractionFlags(Qt::TextSelectableByMouse);
    value->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
    return value;
}

} // namespace

class SpeedGraph final : public QWidget
{
public:
    explicit SpeedGraph(QWidget* parent = nullptr) : QWidget(parent)
    {
        setObjectName("TransferSpeedGraph");
        setAccessibleName("Recent transfer speed");
        setAccessibleDescription("Graph of recent transfer speed samples");
        setMinimumHeight(118);
    }

    void addSample(qint64 bytesPerSecond)
    {
        samples_.append(std::max<qint64>(0, bytesPerSecond));
        if (samples_.size() > 60)
            samples_.remove(0, samples_.size() - 60);
        update();
    }

protected:
    void paintEvent(QPaintEvent*) override
    {
        QPainter painter(this);
        painter.setRenderHint(QPainter::Antialiasing);
        const QRectF plot = rect().adjusted(8, 8, -8, -8);
        painter.fillRect(rect(), palette().base());
        QColor grid = palette().mid().color();
        grid.setAlpha(90);
        painter.setPen(grid);
        for (int row = 1; row < 4; ++row) {
            const qreal y = plot.top() + plot.height() * row / 4;
            painter.drawLine(QPointF(plot.left(), y), QPointF(plot.right(), y));
        }
        if (samples_.isEmpty()) {
            painter.setPen(palette().mid().color());
            painter.drawText(plot, Qt::AlignCenter, QStringLiteral("Waiting for speed data"));
            return;
        }
        const qreal maximum = std::max<qreal>(1,
            *std::max_element(samples_.cbegin(), samples_.cend()) * 1.25);
        QPainterPath line;
        const qreal step = samples_.size() > 1
            ? plot.width() / (samples_.size() - 1) : 0;
        for (qsizetype index = 0; index < samples_.size(); ++index) {
            const qreal x = plot.left() + step * index;
            const qreal y = plot.bottom() - plot.height()
                * static_cast<qreal>(samples_.at(index)) / maximum;
            if (index == 0)
                line.moveTo(x, y);
            else
                line.lineTo(x, y);
        }
        if (samples_.size() == 1)
            line.lineTo(plot.right(), line.currentPosition().y());
        QPainterPath fill = line;
        const qreal endX = samples_.size() == 1
            ? plot.right() : plot.left() + step * (samples_.size() - 1);
        fill.lineTo(endX, plot.bottom());
        fill.lineTo(plot.left(), plot.bottom());
        fill.closeSubpath();
        QColor accent = palette().highlight().color();
        QColor shade = accent;
        shade.setAlpha(55);
        painter.fillPath(fill, shade);
        painter.setPen(QPen(accent, 2));
        painter.drawPath(line);
    }

private:
    QVector<qint64> samples_;
};

TransferProgressDialog::TransferProgressDialog(bool move, const QStringList& sources,
    const QString& destination, bool removableStorage, QWidget* parent)
    : QDialog(parent)
{
    setWindowTitle(move ? QStringLiteral("Move items") : QStringLiteral("Copy items"));
    setObjectName("TransferProgressDialog");
    setAccessibleName(move ? QStringLiteral("Move transfer progress")
                           : QStringLiteral("Copy transfer progress"));
    setAccessibleDescription(
        "Shows progress, recent speed, remaining time, Pause, Resume and Cancel.");
    setWindowModality(Qt::NonModal);
    setMinimumWidth(320);

    auto* root = new QVBoxLayout(this);
    root->setContentsMargins(18, 16, 18, 16);
    root->setSpacing(10);
    auto* heading = new QLabel(move ? QStringLiteral("Moving items")
                                    : QStringLiteral("Copying items"), this);
    heading->setObjectName("TransferHeading");
    QFont headingFont = heading->font();
    headingFont.setPointSizeF(headingFont.pointSizeF() + 2);
    headingFont.setWeight(QFont::DemiBold);
    heading->setFont(headingFont);
    root->addWidget(heading);

    currentFile_ = new QLabel(removableStorage
        ? QStringLiteral("Preparing removable storage — do not eject it")
        : QStringLiteral("Preparing transfer…"), this);
    currentFile_->setObjectName("TransferCurrentItem");
    currentFile_->setAccessibleName("Current item");
    currentFile_->setMinimumWidth(0);
    root->addWidget(currentFile_);

    bar_ = new QProgressBar(this);
    bar_->setObjectName("TransferProgressBar");
    bar_->setAccessibleName("Transfer percent complete");
    bar_->setTextVisible(false);
    bar_->setRange(0, 0);
    root->addWidget(bar_);

    summary_ = new QLabel(QStringLiteral("Calculating transfer size…"), this);
    summary_->setObjectName("TransferProgressSummary");
    summary_->setWordWrap(true);
    summary_->setMinimumWidth(0);
    root->addWidget(summary_);

    detailsScroll_ = new QScrollArea(this);
    detailsScroll_->setObjectName("TransferDetailsScroll");
    detailsScroll_->setFrameShape(QFrame::NoFrame);
    detailsScroll_->setWidgetResizable(true);
    detailsScroll_->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    detailsScroll_->setMinimumHeight(0);
    detailsScroll_->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Ignored);
    details_ = new QWidget(detailsScroll_);
    details_->setObjectName("TransferDetailsPanel");
    auto* detailsLayout = new QVBoxLayout(details_);
    detailsLayout->setContentsMargins(0, 4, 0, 0);
    detailsLayout->setSpacing(10);
    auto* fields = new QFormLayout;
    fields->setContentsMargins(0, 0, 0, 0);
    fields->setLabelAlignment(Qt::AlignLeft);
    fields->addRow(QStringLiteral("From:"), pathValue(commonSourceLocation(sources), details_));
    fields->addRow(QStringLiteral("To:"), pathValue(QDir::toNativeSeparators(destination), details_));
    transferred_ = new QLabel(QStringLiteral("Calculating…"), details_);
    speed_ = new QLabel(QStringLiteral("Calculating…"), details_);
    remaining_ = new QLabel(QStringLiteral("Calculating…"), details_);
    transferred_->setObjectName("TransferBytesDetail");
    speed_->setObjectName("TransferSpeedDetail");
    remaining_->setObjectName("TransferRemainingDetail");
    fields->addRow(QStringLiteral("Transferred:"), transferred_);
    fields->addRow(QStringLiteral("Speed:"), speed_);
    fields->addRow(QStringLiteral("Time remaining:"), remaining_);
    detailsLayout->addLayout(fields);
    graph_ = new SpeedGraph(details_);
    detailsLayout->addWidget(graph_);
    detailsScroll_->setWidget(details_);
    root->addWidget(detailsScroll_, 1);

    auto* actions = new QHBoxLayout;
    pause_ = new QPushButton(QStringLiteral("Pause"), this);
    pause_->setObjectName("TransferPauseResume");
    pause_->setAccessibleName("Pause transfer");
    cancel_ = new QPushButton(QStringLiteral("Cancel"), this);
    cancel_->setObjectName("TransferCancel");
    detailsToggle_ = new QPushButton(QStringLiteral("More details"), this);
    detailsToggle_->setObjectName("TransferDetailsToggle");
    detailsToggle_->setCheckable(true);
    actions->addWidget(pause_);
    actions->addWidget(cancel_);
    actions->addStretch(1);
    actions->addWidget(detailsToggle_);
    root->addLayout(actions);

    connect(pause_, &QPushButton::clicked, this, [this] {
        if (state_ == filemanager::files::TransferJobState::Paused)
            emit resumeRequested();
        else if (state_ == filemanager::files::TransferJobState::Running)
            emit pauseRequested();
    });
    connect(cancel_, &QPushButton::clicked, this, [this] {
        cancel_->setEnabled(false);
        emit cancelRequested();
    });
    connect(detailsToggle_, &QPushButton::toggled, this, [this](bool expanded) {
        setExpanded(expanded);
        QSettings().setValue(QStringLiteral("ui/transferDetailsExpanded"), expanded);
    });
    refreshTimer_.setSingleShot(true);
    refreshTimer_.setInterval(100);
    connect(&refreshTimer_, &QTimer::timeout, this,
        &TransferProgressDialog::renderProgress);
    setJobState(filemanager::files::TransferJobState::Queued);
    const bool expanded = QSettings().value(
        QStringLiteral("ui/transferDetailsExpanded"), false).toBool();
    detailsToggle_->setChecked(expanded);
    setExpanded(expanded);
}

void TransferProgressDialog::setProgress(
    const filemanager::files::TransferProgress& progress)
{
    latest_ = progress;
    hasProgress_ = true;
    if (!refreshTimer_.isActive())
        refreshTimer_.start();
}

void TransferProgressDialog::setJobState(filemanager::files::TransferJobState state)
{
    state_ = state;
    const bool paused = state == filemanager::files::TransferJobState::Paused;
    pause_->setText(paused ? QStringLiteral("Resume") : QStringLiteral("Pause"));
    pause_->setAccessibleName(paused ? QStringLiteral("Resume transfer")
                                    : QStringLiteral("Pause transfer"));
    pause_->setEnabled(paused || state == filemanager::files::TransferJobState::Running);
}

void TransferProgressDialog::renderProgress()
{
    if (!hasProgress_)
        return;
    const qint64 done = latest_.completedBytes;
    const qint64 total = latest_.totalBytes;
    int percent = -1;
    if (total > 0) {
        percent = static_cast<int>(std::clamp(
            static_cast<double>(done) / static_cast<double>(total) * 100.0,
            0.0, 100.0));
        bar_->setRange(0, 100);
        bar_->setValue(percent);
    } else {
        bar_->setRange(0, 0);
    }
    const QString fullPath = QDir::toNativeSeparators(latest_.currentPath);
    const QString name = QFileInfo(fullPath).fileName();
    if (!name.isEmpty()) {
        currentFile_->setText(QFontMetrics(currentFile_->font()).elidedText(
            name, Qt::ElideMiddle, std::max(120, currentFile_->width() - 8)));
        currentFile_->setToolTip(fullPath);
    }
    const QString transferred = total > 0
        ? QStringLiteral("%1 of %2").arg(QLocale().formattedDataSize(done),
            QLocale().formattedDataSize(total))
        : QLocale().formattedDataSize(done);
    const QString speed = latest_.bytesPerSecond > 0
        ? QLocale().formattedDataSize(latest_.bytesPerSecond) + QStringLiteral("/s")
        : QStringLiteral("Calculating…");
    const QString remaining = remainingText(latest_.remainingSeconds);
    summary_->setText(percent >= 0
        ? QStringLiteral("%1%  •  %2  •  %3  •  %4")
            .arg(percent).arg(transferred, speed, remaining)
        : QStringLiteral("%1  •  %2  •  %3")
            .arg(transferred, speed, remaining));
    transferred_->setText(transferred);
    speed_->setText(speed);
    remaining_->setText(remaining);
    if (!graphClock_.isValid() || graphClock_.elapsed() >= 500) {
        graph_->addSample(latest_.bytesPerSecond);
        graphClock_.restart();
    }
}

void TransferProgressDialog::setExpanded(bool expanded)
{
    detailsScroll_->setVisible(expanded);
    detailsToggle_->setText(expanded ? QStringLiteral("Fewer details")
                                     : QStringLiteral("More details"));
    detailsToggle_->setAccessibleName(detailsToggle_->text());
    const QScreen* target = parentWidget() ? parentWidget()->screen() : screen();
    if (!target)
        target = QGuiApplication::primaryScreen();
    const QSize available = target ? target->availableGeometry().size() : QSize(800, 600);
    const int maximumWidth = std::max(1, available.width() - 24);
    const int maximumHeight = std::max(1, available.height() - 24);
    setMinimumWidth(std::min(360, maximumWidth));
    adjustSize();
    resize(std::min(expanded ? 640 : 460, maximumWidth),
           std::min(expanded ? 390 : 190, maximumHeight));
}

} // namespace filemanager::ui
