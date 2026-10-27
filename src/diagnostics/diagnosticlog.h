#pragma once

#include <QDateTime>
#include <QString>
#include <QStringList>
#include <QVector>

namespace filemanager::diagnostics {

class DiagnosticLog final
{
public:
    static constexpr int retentionDays = 7;
    static constexpr int maximumEventCount = 512;

    explicit DiagnosticLog(QString storagePath = {});

    bool record(const QString& eventCode, QString* error = nullptr);
    bool clear(QString* error = nullptr);
    QStringList eventCodes() const;
    int eventCount() const;
    QString storagePath() const;
    QString initializationError() const { return initializationError_; }

private:
    struct Event
    {
        QDateTime timestampUtc;
        QString code;
    };

    void load();
    void prune(const QDateTime& nowUtc);
    bool save(QString* error);

    QString storagePath_;
    QVector<Event> events_;
    QString initializationError_;
};

} // namespace filemanager::diagnostics
