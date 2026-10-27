#pragma once

#include "files/fileanalysis.h"

#include <QObject>
#include <QDateTime>

#include <memory>

namespace filemanager::files {

struct AnalysisResult
{
    QString path;
    PathStatistics statistics;
    QString sha256;
    bool fromCache = false;
    QDateTime measuredAt;
};

class AnalysisService final : public QObject
{
    Q_OBJECT

public:
    explicit AnalysisService(QObject* parent = nullptr);
    ~AnalysisService() override;

    quint64 start(const QString& path, bool refresh = false);
    void cancel();
    static void clearFolderCache(const QString& path = {});
    static int folderCacheSize();

signals:
    void finished(quint64 generation, const filemanager::files::AnalysisResult& result);
    void progress(quint64 generation, const QString& path,
        const filemanager::files::PathStatistics& partial);

private:
    quint64 generation_ = 0;
    std::shared_ptr<std::atomic_bool> cancellation_;
};

} // namespace filemanager::files

Q_DECLARE_METATYPE(filemanager::files::AnalysisResult)
