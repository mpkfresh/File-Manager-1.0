#pragma once

#include "search/searchservice.h"

#include <QList>

namespace filemanager::search {

struct SavedSearch
{
    QString id;
    QString name;
    QString root;
    SearchFilters filters;
    int sizePreset = 0;
    int datePreset = 0;
};

class SavedSearchStore final
{
public:
    static constexpr int MaximumEntries = 100;
    static constexpr qint64 MaximumStorageBytes = 2 * 1024 * 1024;

    explicit SavedSearchStore(QString storagePath = {});

    bool load(QString* error = nullptr);
    bool save(const SavedSearch& search, QString* error = nullptr);
    bool remove(const QString& id, QString* error = nullptr);
    bool clear(QString* error = nullptr);

    const QList<SavedSearch>& entries() const;
    QString storagePath() const;

private:
    bool write(const QList<SavedSearch>& entries, QString* error) const;

    QString storagePath_;
    QList<SavedSearch> entries_;
};

} // namespace filemanager::search
