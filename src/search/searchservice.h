#pragma once

#include <QObject>
#include <QString>
#include <QDateTime>

#include <atomic>
#include <memory>

class QThread;
class QFileInfo;

namespace filemanager::search {

enum class SearchItemType { Any, Files, Folders };

struct SearchFilters
{
    QString name;
    QString extension;
    SearchItemType itemType = SearchItemType::Any;
    qint64 minimumSize = -1;
    qint64 maximumSize = -1;
    QDateTime modifiedAfter;
    QDateTime modifiedBefore;

    bool isDefault() const;
};

bool matchesSearchFilters(const QFileInfo& info, const SearchFilters& filters, int* score = nullptr);

struct SearchHit
{
    QString path;
    QString name;
    bool directory = false;
    int score = 0;
};

// Highest relevance first, with stable natural-name and path tie breakers.
bool searchHitPrecedes(const SearchHit& left, const SearchHit& right);

class SearchService final : public QObject
{
    Q_OBJECT

public:
    explicit SearchService(QObject* parent = nullptr);
    ~SearchService() override;

    quint64 start(const QString& root, const QString& query, const QString& extension = {},
        int maximumResults = 2000);
    quint64 start(const QString& root, SearchFilters filters, int maximumResults = 2000);
    quint64 startAqs(const QString& root, const QString& advancedQuery,
        int maximumResults = 2000);
    void cancel();

signals:
    void resultFound(quint64 generation, const filemanager::search::SearchHit& hit);
    void backendSelected(quint64 generation, const QString& backend);
    void errorOccurred(quint64 generation, const QString& error);
    void finished(quint64 generation, bool cancelled, int resultCount);

private:
    quint64 generation_ = 0;
    std::shared_ptr<std::atomic_bool> cancellation_;
};

} // namespace filemanager::search
