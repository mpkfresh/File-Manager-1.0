#pragma once

#include "diagnostics/diagnosticbundle.h"
#include <QObject>
#include <QJsonObject>
#include <QPointer>
#include <QQueue>
#include <QStringList>
#include <QTimer>

class QProcess;
namespace filemanager::diagnostics {
class AsyncDiagnosticLog final : public QObject
{
    Q_OBJECT
public:
    static constexpr int maximumPendingRecords = 64;
    explicit AsyncDiagnosticLog(QString storagePath = {}, QObject* parent = nullptr,
        QString helperProgram = {}, QStringList helperArguments = {});
    ~AsyncDiagnosticLog() override;
    // True means accepted as pending; committed state is published separately.
    bool record(const QString& code);
    bool clear();
    bool exportBundle(const QString& destination, const DiagnosticBundleInput& input, bool includePaths);
    int eventCount() const { return events_.size(); }
    QStringList eventCodes() const { return events_; }
    int pendingCount() const { return pendingRecords_; }
    bool isLoading() const { return loading_; }
    bool isClearing() const { return clearing_; }
    bool isExporting() const { return exporting_; }
    bool isAvailable() const { return available_; }
    QString errorString() const { return error_; }
    void shutdown();
signals:
    void stateChanged();
    void clearFinished(bool success, const QString& error);
    void exportFinished(bool success, const QString& error);
    void shutdownFinished();
private:
    struct Request { QJsonObject message; quint64 generation = 0; };
    void start(QString program, QStringList arguments);
    void enqueue(QJsonObject message);
    void pump();
    void readReplies();
    void fail(const QString& error);
    void stopProcess();
    void finishShutdown();
    bool reject(const QString& error);
    QPointer<QProcess> process_;
    QQueue<Request> queue_;
    Request active_;
    QByteArray replies_;
    QStringList events_;
    QString error_;
    QString storagePath_;
    QTimer shutdownTimer_;
    quint64 serial_ = 0;
    quint64 generation_ = 0;
    int pendingRecords_ = 0;
    bool busy_ = false;
    bool started_ = false;
    bool loading_ = true;
    bool clearing_ = false;
    bool exporting_ = false;
    bool available_ = false;
    bool stopping_ = false;
    bool stopped_ = false;
    void* job_ = nullptr;
};
}
