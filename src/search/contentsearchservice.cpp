#include "search/contentsearchservice.h"
#include "files/scopeenumerator.h"

#include <QFile>
#include <QFileInfo>
#include <QPointer>
#include <QThread>

namespace filemanager::search {

ContentSearchService::ContentSearchService(QObject* parent) : QObject(parent) {}
ContentSearchService::~ContentSearchService() { cancel(); }

quint64 ContentSearchService::start(const QString& root, const QString& text,
    const QString& extension, qint64 maximumFileBytes, int maximumResults)
{
    cancel();
    const quint64 generation = ++generation_;
    cancellation_ = std::make_shared<std::atomic_bool>(false);
    const auto cancellation = cancellation_;
    QPointer<ContentSearchService> guard(this);
    const QString suffix = QString(extension).trimmed().remove('.');
    QThread* worker = QThread::create([guard, cancellation, generation, root, text, suffix,
                                         maximumFileBytes, maximumResults] {
        int resultCount = 0;
        int skippedFiles = 0;
        const auto scopeResult = filemanager::files::enumerateRegularTree(
            root, false, cancellation.get(), [&](const QFileInfo& info) {
            if (resultCount >= maximumResults)
                return false;
            if (info.size() > maximumFileBytes
                || (!suffix.isEmpty()
                    && info.suffix().compare(suffix, Qt::CaseInsensitive) != 0)) {
                ++skippedFiles;
                return true;
            }
            QFile file(info.absoluteFilePath());
            if (!file.open(QIODevice::ReadOnly)) {
                ++skippedFiles;
                return true;
            }
            const QByteArray bytes = file.read(maximumFileBytes + 1);
            if (bytes.size() > maximumFileBytes || bytes.contains('\0')) {
                ++skippedFiles;
                return true;
            }
            const QString content = QString::fromUtf8(bytes);
            const qsizetype position = content.indexOf(text, 0, Qt::CaseInsensitive);
            if (position < 0)
                return true;
            const int line = content.left(position).count('\n') + 1;
            const qsizetype start = qMax<qsizetype>(0, position - 60);
            QString snippet = content.mid(start, text.size() + 120).simplified();
            ContentSearchHit hit{info.absoluteFilePath(), snippet, line};
            ++resultCount;
            if (guard) {
                QMetaObject::invokeMethod(guard, [guard, generation, hit] {
                    if (guard)
                        emit guard->resultFound(generation, hit);
                });
            }
            return resultCount < maximumResults;
        });
        skippedFiles += scopeResult.skippedBoundaries;
        const bool wasCancelled = cancellation->load();
        if (guard) {
            QMetaObject::invokeMethod(guard,
                [guard, generation, wasCancelled, resultCount, skippedFiles] {
                    if (guard)
                        emit guard->finished(generation, wasCancelled, resultCount, skippedFiles);
                });
        }
    });
    connect(worker, &QThread::finished, worker, &QObject::deleteLater);
    worker->start();
    return generation;
}

void ContentSearchService::cancel()
{
    if (cancellation_)
        cancellation_->store(true);
}

} // namespace filemanager::search
