#pragma once

#include <QHash>
#include <QKeySequence>
#include <QList>
#include <QObject>
#include <QString>
#include <QStringList>

class QAction;

namespace filemanager::commands {

struct Descriptor
{
    QString id;
    QString title;
    QString category;
    QStringList aliases;
};

class CommandRegistry final : public QObject
{
    Q_OBJECT

public:
    explicit CommandRegistry(QObject* parent = nullptr);

    QAction* add(Descriptor descriptor, const QKeySequence& shortcut = {});
    QAction* action(const QString& id) const;
    QList<QAction*> actions() const;
    Descriptor descriptor(const QAction* action) const;
    QString conflictingCommand(const QString& id, const QKeySequence& shortcut) const;
    bool setShortcut(const QString& id, const QKeySequence& shortcut, QString* conflictingId = nullptr);

private:
    QHash<QString, QAction*> actionsById_;
    QHash<const QAction*, Descriptor> descriptors_;
};

} // namespace filemanager::commands
