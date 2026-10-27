#pragma once

#include <QObject>
#include <QString>
#include <QStringList>
#include <QtTypes>
#include <memory>

namespace filemanager::platform {

struct ShareSelectionValidation final
{
    bool valid = false;
    QStringList absolutePaths;
    int providerBackedItems = 0;
    QString error;
};

ShareSelectionValidation validateWindowsShareSelection(const QStringList& paths);
bool windowsShareContractAvailable(QString* error = nullptr);

class WindowsShareController final : public QObject
{
    Q_OBJECT

public:
    explicit WindowsShareController(QObject* parent = nullptr);
    ~WindowsShareController() override;

    void showShareUi(quintptr windowId, const QStringList& paths);

signals:
    void shareUiShown(int itemCount, int providerBackedItems);
    void shareFailed(const QString& error);

private:
    struct State;
    std::unique_ptr<State> state_;
};

} // namespace filemanager::platform
