#include "commandregistry.h"

#include <QAction>
#include <QKeySequence>
#include <utility>

namespace filemanager::commands {

CommandRegistry::CommandRegistry(QObject* parent)
    : QObject(parent)
{
}

QAction* CommandRegistry::add(Descriptor descriptor, const QKeySequence& shortcut)
{
    Q_ASSERT(!descriptor.id.trimmed().isEmpty());
    Q_ASSERT(!actionsById_.contains(descriptor.id));

    auto* action = new QAction(descriptor.title, this);
    action->setObjectName(descriptor.id);
    action->setShortcut(shortcut);
    actionsById_.insert(descriptor.id, action);
    descriptors_.insert(action, std::move(descriptor));
    return action;
}

QAction* CommandRegistry::action(const QString& id) const
{
    return actionsById_.value(id);
}

QList<QAction*> CommandRegistry::actions() const
{
    return actionsById_.values();
}

Descriptor CommandRegistry::descriptor(const QAction* action) const
{
    return descriptors_.value(action);
}

QString CommandRegistry::conflictingCommand(const QString& id, const QKeySequence& shortcut) const
{
    if (shortcut.isEmpty())
        return {};
    for (auto it = actionsById_.cbegin(); it != actionsById_.cend(); ++it) {
        if (it.key() != id && it.value()->shortcut() == shortcut)
            return it.key();
    }
    return {};
}

bool CommandRegistry::setShortcut(
    const QString& id, const QKeySequence& shortcut, QString* conflictingId)
{
    QAction* command = action(id);
    if (!command)
        return false;
    const QString conflict = conflictingCommand(id, shortcut);
    if (!conflict.isEmpty()) {
        if (conflictingId)
            *conflictingId = conflict;
        return false;
    }
    command->setShortcut(shortcut);
    return true;
}

} // namespace filemanager::commands
