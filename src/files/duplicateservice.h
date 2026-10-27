#pragma once

#include "files/duplicatefinder.h"

#include <QObject>
#include <memory>

namespace filemanager::files {

class DuplicateService final : public QObject
{
    Q_OBJECT
public:
    explicit DuplicateService(QObject* parent = nullptr);
    ~DuplicateService() override;
    quint64 start(const QString& root);
    void cancel();

signals:
    void finished(quint64 generation, const filemanager::files::DuplicateScanResult& result);

private:
    quint64 generation_ = 0;
    std::shared_ptr<std::atomic_bool> cancellation_;
};

} // namespace filemanager::files

Q_DECLARE_METATYPE(filemanager::files::DuplicateScanResult)
