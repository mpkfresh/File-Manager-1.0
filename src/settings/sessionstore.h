#pragma once

#include <QByteArray>
#include <QDateTime>
#include <QList>
#include <QString>

namespace filemanager::settings {

struct Session
{
    static constexpr int CurrentVersion = 3;
    int version = CurrentVersion;
    QString workspaceLayoutJson;
    QByteArray geometry;
    QByteArray splitterState;
    bool inspectorVisible = false;
};

struct NamedWorkspace
{
    QString id;
    QString name;
    Session session;
    QDateTime updatedAt;
};

class SessionStore
{
public:
    Session load(QString* error = nullptr) const;
    bool save(const Session& session, QString* error = nullptr) const;
    bool restorationEnabled() const;
    void setRestorationEnabled(bool enabled) const;
    bool hasSavedSession() const;
    QList<NamedWorkspace> namedWorkspaces(QString* error = nullptr) const;
    bool saveNamedWorkspace(const QString& name, const Session& session,
        QString* savedId = nullptr, QString* error = nullptr) const;
    bool updateNamedWorkspace(const QString& id, const Session& session,
        QString* error = nullptr) const;
    bool removeNamedWorkspace(const QString& id, QString* error = nullptr) const;
    QString activeNamedWorkspaceId() const;
    bool setActiveNamedWorkspaceId(const QString& id, QString* error = nullptr) const;
    bool clear(QString* error = nullptr) const;
};

} // namespace filemanager::settings
