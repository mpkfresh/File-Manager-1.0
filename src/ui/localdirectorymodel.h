#pragma once

#include <QAbstractItemModel>
#include <QDir>
#include <QFileIconProvider>
#include <QFileInfo>
#include <QHash>
#include <QIcon>
#include <QQueue>
#include <QSet>
#include <QTimer>

#include <memory>

namespace filemanager::platform { class DirectoryWatcher; }

namespace filemanager::ui {
class BackgroundFileIcons;

class LocalDirectoryModel : public QAbstractItemModel
{
    Q_OBJECT

public:
    explicit LocalDirectoryModel(QObject* parent = nullptr);
    ~LocalDirectoryModel() override;

    QModelIndex index(int row, int column, const QModelIndex& parent = {}) const override;
    QModelIndex parent(const QModelIndex& child) const override;
    int rowCount(const QModelIndex& parent = {}) const override;
    int columnCount(const QModelIndex& parent = {}) const override;
    bool hasChildren(const QModelIndex& parent = {}) const override;
    bool canFetchMore(const QModelIndex& parent) const override;
    void fetchMore(const QModelIndex& parent) override;
    QVariant data(const QModelIndex& index, int role = Qt::DisplayRole) const override;
    QVariant headerData(int section, Qt::Orientation orientation,
        int role = Qt::DisplayRole) const override;
    Qt::ItemFlags flags(const QModelIndex& index) const override;
    QStringList mimeTypes() const override;
    QMimeData* mimeData(const QModelIndexList& indexes) const override;
    Qt::DropActions supportedDropActions() const override;

    void setDirectory(const QString& path);
    QString directory() const { return directory_; }
    void refresh();
    void setFilters(QDir::Filters filters);
    QDir::Filters filters() const { return filters_; }
    QString filePath(const QModelIndex& index) const;
    QFileInfo fileInfo(const QModelIndex& index) const;
    QModelIndex indexForPath(const QString& path, int column = 0) const;
    bool loading() const;

signals:
    void directoryLoaded(const QString& path);
    void directoryLoadFailed(const QString& path, const QString& error);
    void directoryMonitoringChanged(const QString& path, bool available);
    void iconsChanged();

private:
    struct Node;
    Node* nodeForIndex(const QModelIndex& index) const;
    Node* findNode(const QString& path) const;
    QModelIndex indexForNode(const Node* node, int column = 0) const;
    void startEnumeration(Node* node, bool clearExisting, bool retainTypeIcons = false,
        bool refreshHint = false, bool registerWatch = true);
    void applyRefresh(Node* node, const QFileInfoList& incoming,
        const QSet<QString>& changedMetadata);
    void forgetSubtree(Node* node, bool includeSelf);
    void schedulePublication(Node* node);
    void publishNextBatch();
    QString cacheKey(const QFileInfo& info) const;

    QString directory_;
    std::unique_ptr<Node> root_;
    quint64 rootGeneration_ = 0;
    quint64 enumerationGeneration_ = 0;
    QDir::Filters filters_ = QDir::AllEntries | QDir::NoDotAndDotDot;
    platform::DirectoryWatcher* watcher_ = nullptr;
    QTimer publishTimer_;
    QTimer refreshTimer_;
    QQueue<QString> publishQueue_;
    QSet<QString> changedDirectories_;
    QSet<QString> monitoringChecks_;
    mutable QFileIconProvider iconProvider_;
    BackgroundFileIcons* backgroundIcons_ = nullptr;
    mutable QHash<QString, QIcon> iconCache_;
    mutable QHash<QString, QString> typeCache_;
};

} // namespace filemanager::ui
