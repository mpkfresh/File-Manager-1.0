#include "breadcrumbbar.h"
#include "platform/pathnavigation.h"

#include <QDir>
#include <QDirIterator>
#include <QEvent>
#include <QFileInfo>
#include <QFutureWatcher>
#include <QHBoxLayout>
#include <QKeyEvent>
#include <QLineEdit>
#include <QMenu>
#include <QMouseEvent>
#include <QScrollArea>
#include <QStackedWidget>
#include <QToolButton>
#include <QtConcurrentRun>

#include <algorithm>
#include <atomic>
#include <memory>
#include <queue>
#include <vector>

namespace filemanager::ui {
namespace {

constexpr int MaximumMenuFolders = 256;

struct FolderListing
{
    QStringList children;
    bool truncated = false;
};

class CrumbButton final : public QToolButton
{
    Q_OBJECT

public:
    explicit CrumbButton(QWidget* parent = nullptr) : QToolButton(parent) {}

signals:
    void middleClicked();

protected:
    void mouseReleaseEvent(QMouseEvent* event) override
    {
        if (event->button() == Qt::MiddleButton && rect().contains(event->position().toPoint())) {
            emit middleClicked();
            event->accept();
            return;
        }
        QToolButton::mouseReleaseEvent(event);
    }
};

class FolderMenu final : public QMenu
{
    Q_OBJECT

public:
    explicit FolderMenu(QWidget* parent = nullptr) : QMenu(parent)
    {
        connect(this, &QMenu::aboutToHide, this, [this] {
            cancelListing();
            if (property("loadState").toInt() == 1)
                setProperty("loadState", 0);
        });
    }

    ~FolderMenu() override { cancelListing(); }

    void cancelListing()
    {
        if (listingCancelled_)
            listingCancelled_->store(true);
    }

    std::shared_ptr<std::atomic_bool> beginListing()
    {
        cancelListing();
        listingCancelled_ = std::make_shared<std::atomic_bool>(false);
        return listingCancelled_;
    }

signals:
    void backgroundPathRequested(const QString& path);

protected:
    void mouseReleaseEvent(QMouseEvent* event) override
    {
        if (event->button() == Qt::MiddleButton) {
            QAction* item = actionAt(event->position().toPoint());
            const QString path = item ? item->data().toString() : QString{};
            if (!path.isEmpty()) {
                emit backgroundPathRequested(path);
                close();
                event->accept();
                return;
            }
        }
        QMenu::mouseReleaseEvent(event);
    }

private:
    std::shared_ptr<std::atomic_bool> listingCancelled_;
};

QStringList pathAncestors(const QString& path)
{
    QString cursor = QDir::cleanPath(QDir::fromNativeSeparators(path));
    QStringList reversed;
    for (int depth = 0; !cursor.isEmpty() && depth < 128; ++depth) {
        reversed.prepend(cursor);
        const QString parent = QDir::fromNativeSeparators(
            filemanager::platform::fileSystemParentPath(cursor));
        if (parent.isEmpty())
            break;
        if (parent.compare(cursor, Qt::CaseInsensitive) == 0)
            break;
        cursor = parent;
    }
    return reversed;
}

QString segmentLabel(const QString& path, bool root)
{
    if (root)
        return QDir::toNativeSeparators(path);
    const QString name = QFileInfo(path).fileName();
    const QString label = name.isEmpty() ? QDir::toNativeSeparators(path) : name;
    return label.size() > 24 ? label.left(21) + QStringLiteral("…") : label;
}

} // namespace

BreadcrumbBar::BreadcrumbBar(QWidget* parent)
    : QWidget(parent)
{
    setObjectName("BreadcrumbAddressBar");
    setAccessibleName("Folder address");
    setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);

    auto* root = new QHBoxLayout(this);
    root->setContentsMargins(0, 0, 0, 0);
    stack_ = new QStackedWidget(this);
    stack_->setObjectName("BreadcrumbAddressStack");

    auto* scroll = new QScrollArea(stack_);
    scroll->setObjectName("BreadcrumbViewport");
    scroll->setFrameShape(QFrame::NoFrame);
    scroll->setWidgetResizable(true);
    scroll->setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    breadcrumbHost_ = new QWidget(scroll);
    breadcrumbLayout_ = new QHBoxLayout(breadcrumbHost_);
    breadcrumbLayout_->setContentsMargins(1, 1, 1, 1);
    breadcrumbLayout_->setSpacing(1);
    breadcrumbLayout_->addStretch(1);
    scroll->setWidget(breadcrumbHost_);
    stack_->addWidget(scroll);

    editor_ = new QLineEdit(stack_);
    editor_->setObjectName("AddressPathEditor");
    editor_->setAccessibleName("Folder path");
    editor_->setClearButtonEnabled(true);
    editor_->installEventFilter(this);
    stack_->addWidget(editor_);
    root->addWidget(stack_);

    connect(editor_, &QLineEdit::returnPressed, this, [this] {
        const QString normalized = normalizeAddressInput(editor_->text());
        if (!normalized.isEmpty())
            emit pathSubmitted(normalized);
    });
}

QString BreadcrumbBar::normalizeAddressInput(const QString& input)
{
    QString value = input.trimmed();
    if (value.isEmpty() || value.contains(QChar::Null)
            || value.contains('\n') || value.contains('\r'))
        return {};
    const bool startsQuote = value.startsWith('"') || value.startsWith('\'');
    const bool endsQuote = value.endsWith('"') || value.endsWith('\'');
    if (startsQuote != endsQuote || (startsQuote && value.front() != value.back()))
        return {};
    if (startsQuote) {
        if (value.size() < 2)
            return {};
        value = value.mid(1, value.size() - 2).trimmed();
    }
    if (value.isEmpty())
        return {};
    const QString nativeValue = QDir::toNativeSeparators(value);
    if (nativeValue.startsWith(QStringLiteral("\\\\?\\"), Qt::CaseInsensitive)) {
        const QString extendedTail = nativeValue.mid(4);
        if (extendedTail.startsWith(QStringLiteral("UNC\\"), Qt::CaseInsensitive)) {
            const QString uncTail = QDir::fromNativeSeparators(extendedTail.mid(4));
            QString cleaned = QDir::toNativeSeparators(QDir::cleanPath("//" + uncTail));
            if (cleaned.startsWith(QStringLiteral("\\\\")))
                cleaned.remove(0, 2);
            return QStringLiteral("\\\\?\\UNC\\") + cleaned;
        }
        return QStringLiteral("\\\\?\\") + QDir::toNativeSeparators(
            QDir::cleanPath(QDir::fromNativeSeparators(extendedTail)));
    }
    return QDir::toNativeSeparators(
        QDir::cleanPath(QDir::fromNativeSeparators(value)));
}

QString BreadcrumbBar::path() const
{
    return currentPath_;
}

bool BreadcrumbBar::isEditing() const
{
    return stack_->currentWidget() == editor_;
}

void BreadcrumbBar::setPath(const QString& path)
{
    currentPath_ = QDir::toNativeSeparators(
        QDir::cleanPath(QDir::fromNativeSeparators(path)));
    editor_->setText(currentPath_);
    rebuild(currentPath_);
    showBreadcrumbs();
}

void BreadcrumbBar::setShellLocation(const QString& displayName, const QString& identity)
{
    currentPath_ = identity;
    editor_->setText(identity);
    rebuild({}, displayName.isEmpty() ? identity : displayName);
    showBreadcrumbs();
}

void BreadcrumbBar::beginEditing()
{
    stack_->setCurrentWidget(editor_);
    editor_->setFocus(Qt::ShortcutFocusReason);
    editor_->selectAll();
}

void BreadcrumbBar::showBreadcrumbs()
{
    stack_->setCurrentIndex(0);
}

bool BreadcrumbBar::eventFilter(QObject* watched, QEvent* event)
{
    if (watched == editor_ && event->type() == QEvent::KeyPress) {
        auto* key = static_cast<QKeyEvent*>(event);
        if (key->key() == Qt::Key_Escape) {
            editor_->setText(currentPath_);
            editor_->setStyleSheet({});
            editor_->setToolTip({});
            editor_->setAccessibleDescription({});
            showBreadcrumbs();
            return true;
        }
    }
    return QWidget::eventFilter(watched, event);
}

void BreadcrumbBar::rebuild(const QString& path, const QString& displayName)
{
    while (QLayoutItem* item = breadcrumbLayout_->takeAt(0)) {
        if (QWidget* widget = item->widget()) {
            for (auto* menu : widget->findChildren<FolderMenu*>()) {
                menu->cancelListing();
                menu->close();
            }
            widget->setEnabled(false);
            widget->hide();
            widget->deleteLater();
        }
        delete item;
    }

    if (path.isEmpty()) {
        auto* location = new QToolButton(breadcrumbHost_);
        location->setObjectName("BreadcrumbSegment");
        location->setText(displayName);
        location->setToolTip(currentPath_);
        location->setAccessibleName(QStringLiteral("Current Windows location: %1").arg(displayName));
        breadcrumbLayout_->addWidget(location);
        breadcrumbLayout_->addStretch(1);
        return;
    }

    const QStringList ancestors = pathAncestors(path);
    QList<int> visible;
    if (ancestors.size() <= 6) {
        for (int index = 0; index < ancestors.size(); ++index)
            visible.append(index);
    } else {
        visible.append(0);
        visible.append(-1);
        for (int index = ancestors.size() - 4; index < ancestors.size(); ++index)
            visible.append(index);
    }
    for (const int index : visible) {
        if (index < 0) {
            auto* overflow = new CrumbButton(breadcrumbHost_);
            overflow->setObjectName("BreadcrumbOverflow");
            overflow->setText(QStringLiteral("…"));
            overflow->setToolTip("Earlier folders");
            overflow->setAccessibleName("Earlier folders in this path");
            overflow->setAutoRaise(true);
            overflow->setPopupMode(QToolButton::InstantPopup);
            auto* menu = new FolderMenu(overflow);
            menu->setObjectName("BreadcrumbOverflowMenu");
            for (int omitted = 1; omitted < ancestors.size() - 4; ++omitted) {
                const QString omittedPath = QDir::toNativeSeparators(ancestors.at(omitted));
                QAction* action = menu->addAction(segmentLabel(omittedPath, false));
                action->setData(omittedPath);
                connect(action, &QAction::triggered, this,
                    [this, omittedPath] { emit pathActivated(omittedPath); });
            }
            connect(menu, &FolderMenu::backgroundPathRequested,
                this, &BreadcrumbBar::backgroundPathRequested);
            overflow->setMenu(menu);
            breadcrumbLayout_->addWidget(overflow);
            continue;
        }
        const QString ancestor = QDir::toNativeSeparators(ancestors.at(index));
        auto* button = new CrumbButton(breadcrumbHost_);
        button->setObjectName("BreadcrumbSegment");
        button->setText(segmentLabel(ancestor, index == 0));
        button->setToolTip(ancestor);
        button->setAccessibleName(QStringLiteral("Open folder %1").arg(ancestor));
        button->setAutoRaise(true);
        button->setPopupMode(QToolButton::MenuButtonPopup);
        auto* menu = new FolderMenu(button);
        menu->setObjectName("BreadcrumbFolderMenu");
        menu->setAccessibleName(QStringLiteral("Folders in %1").arg(ancestor));
        auto* placeholder = menu->addAction("Show folders");
        placeholder->setEnabled(false);
        menu->setProperty("loadState", 0);
        button->setMenu(menu);
        connect(button, &QToolButton::clicked, this,
            [this, ancestor] { emit pathActivated(ancestor); });
        connect(button, &CrumbButton::middleClicked, this,
            [this, ancestor] { emit backgroundPathRequested(ancestor); });
        connect(menu, &FolderMenu::backgroundPathRequested,
            this, &BreadcrumbBar::backgroundPathRequested);
        connect(menu, &QMenu::aboutToShow, this,
            [this, menu, ancestor] { populateMenuWhenShown(menu, ancestor); });
        breadcrumbLayout_->addWidget(button);
    }
    breadcrumbLayout_->addStretch(1);
}

void BreadcrumbBar::populateMenuWhenShown(QObject* menuObject, const QString& parentPath)
{
    auto* menu = qobject_cast<FolderMenu*>(menuObject);
    if (!menu || menu->property("loadState").toInt() != 0)
        return;
    menu->setProperty("loadState", 1);
    menu->clear();
    QAction* loading = menu->addAction("Loading folders…");
    loading->setEnabled(false);

    const auto cancelled = menu->beginListing();
    auto* watcher = new QFutureWatcher<FolderListing>(menu);
    connect(watcher, &QFutureWatcher<FolderListing>::finished, menu,
        [this, menu, watcher, cancelled] {
            watcher->deleteLater();
            if (cancelled->load())
                return;
            const FolderListing result = watcher->result();
            menu->clear();
            if (result.children.isEmpty()) {
                QAction* empty = menu->addAction("No folders");
                empty->setEnabled(false);
            } else {
                for (const QString& child : result.children) {
                    QAction* action = menu->addAction(QFileInfo(child).fileName());
                    action->setData(QDir::toNativeSeparators(child));
                    connect(action, &QAction::triggered, this,
                        [this, child] { emit pathActivated(child); });
                }
            }
            if (result.truncated) {
                QAction* more = menu->addAction(
                    tr("More than %1 folders; browse the main pane").arg(MaximumMenuFolders));
                more->setEnabled(false);
            }
            menu->setProperty("loadState", 2);
        });
    watcher->setFuture(QtConcurrent::run([parentPath, cancelled] {
        FolderListing result;
        if (cancelled->load())
            return result;
        const auto nameLess = [](const QString& left, const QString& right) {
            const int comparison = QString::compare(left, right, Qt::CaseInsensitive);
            return comparison == 0 ? left < right : comparison < 0;
        };
        // Scan immediate folders but retain only the alphabetical first 256 paths.
        // Cancellation is cooperative and cannot interrupt an already blocked filesystem call.
        std::priority_queue<QString, std::vector<QString>, decltype(nameLess)> kept(nameLess);
        QDirIterator entries(parentPath,
            QDir::Dirs | QDir::NoDotAndDotDot | QDir::Hidden | QDir::System,
            QDirIterator::NoIteratorFlags);
        while (!cancelled->load() && entries.hasNext()) {
            if (cancelled->load())
                return result;
            const QString child = entries.nextFileInfo().absoluteFilePath();
            if (kept.size() < static_cast<size_t>(MaximumMenuFolders)) {
                kept.push(child);
            } else {
                result.truncated = true;
                if (nameLess(child, kept.top())) {
                    kept.pop();
                    kept.push(child);
                }
            }
        }
        if (cancelled->load())
            return FolderListing{};
        result.children.reserve(static_cast<qsizetype>(kept.size()));
        while (!kept.empty()) {
            result.children.append(kept.top());
            kept.pop();
        }
        std::reverse(result.children.begin(), result.children.end());
        return result;
    }));
}

} // namespace filemanager::ui

#include "breadcrumbbar.moc"
