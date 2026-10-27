#include "files/duplicateservice.h"

#include <QPointer>
#include <QThread>

namespace filemanager::files {

DuplicateService::DuplicateService(QObject* parent) : QObject(parent) {}
DuplicateService::~DuplicateService() { cancel(); }

quint64 DuplicateService::start(const QString& root)
{
    cancel();
    const quint64 generation = ++generation_;
    cancellation_ = std::make_shared<std::atomic_bool>(false);
    const auto cancellation = cancellation_;
    QPointer<DuplicateService> guard(this);
    QThread* worker = QThread::create([guard, cancellation, generation, root] {
        const DuplicateScanResult result = findDuplicates(root, cancellation.get());
        if (guard) {
            QMetaObject::invokeMethod(guard, [guard, generation, result] {
                if (guard)
                    emit guard->finished(generation, result);
            });
        }
    });
    connect(worker, &QThread::finished, worker, &QObject::deleteLater);
    worker->start();
    return generation;
}

void DuplicateService::cancel()
{
    if (cancellation_)
        cancellation_->store(true);
}

} // namespace filemanager::files
