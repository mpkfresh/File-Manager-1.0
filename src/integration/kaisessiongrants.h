#pragma once

#include "files/sourcesnapshot.h"

#include <QHash>
#include <QString>

namespace filemanager::integration
{

enum class KaiCapability
{
    ReadNavigation,
    CreateOnlyMutation
};

// Session-only integration policy. This is an application gate, not an OS ACL
// or a substitute for handle-relative final target validation at commit.
class KaiSessionGrants final
{
  public:
    void setEnabled(bool enabled);
    bool enabled() const;
    bool grant(const QString& folder, KaiCapability capability, QString* error = nullptr);
    bool allows(const QString& folder, KaiCapability capability) const;
    void revoke(const QString& folder, KaiCapability capability);
    void revokeAll();

  private:
    struct Grant
    {
        QString folder;
        files::SourceSnapshot identity;
        bool read = false;
        bool createOnly = false;
    };
    bool enabled_ = false;
    QHash<QString, Grant> grants_;
};

} // namespace filemanager::integration
