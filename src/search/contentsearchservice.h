#pragma once

#include <QObject>
#include <QString>

#include <memory>
#include <atomic>

namespace filemanager::search {

struct ContentSearchHit
{
    QString path;
    QString snippet;
    int line = 0;
};

class ContentSearchService final : public QObject
{
    Q_OBJECT
public:
    explicit ContentSearchService(QObject* parent = nullptr);
    ~ContentSearchService() override;

    quint64 start(const QString& root, const QString& text, const QString& extension = {},
        qint64 maximumFileBytes = 16 * 1024 * 1024, int maximumResults = 1000);
    void cancel();

signals:
    void resultFound(quint64 generation, const filemanager::search::ContentSearchHit& hit);
    void finished(quint64 generation, bool cancelled, int resultCount, int skippedFiles);

private:
    quint64 generation_ = 0;
    std::shared_ptr<std::atomic_bool> cancellation_;
};

} // namespace filemanager::search

Q_DECLARE_METATYPE(filemanager::search::ContentSearchHit)
