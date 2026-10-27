#include "integration/kaisessiongrants.h"

#include <QCoreApplication>
#include <QDir>
#include <QTemporaryDir>

#include <cstdio>
#include <stdexcept>

using filemanager::integration::KaiCapability;
using filemanager::integration::KaiSessionGrants;

namespace
{

void check(bool condition, const char* message)
{
    if (!condition)
        throw std::runtime_error(message);
}

} // namespace

int main(int argc, char** argv)
{
    QCoreApplication app(argc, argv);
    try
    {
        QTemporaryDir temp;
        check(temp.isValid(), "temporary folder unavailable");
        const QString root = QDir(temp.path()).filePath(QStringLiteral("project"));
        const QString other = QDir(temp.path()).filePath(QStringLiteral("other"));
        check(QDir().mkpath(root) && QDir().mkpath(other), "test folders unavailable");
        KaiSessionGrants grants;
        QString error;
        check(!grants.enabled() && !grants.grant(root, KaiCapability::ReadNavigation, &error) &&
                  !error.isEmpty(),
              "integration must default off");
        grants.setEnabled(true);
        check(!grants.grant(QStringLiteral("relative"), KaiCapability::ReadNavigation),
              "relative folder granted");
        check(!grants.grant(QDir(temp.path()).filePath(QStringLiteral("missing")),
                            KaiCapability::ReadNavigation),
              "missing folder granted");
        check(grants.grant(root, KaiCapability::ReadNavigation) &&
                  grants.allows(root, KaiCapability::ReadNavigation) &&
                  !grants.allows(root, KaiCapability::CreateOnlyMutation) &&
                  !grants.allows(other, KaiCapability::ReadNavigation),
              "read grant leaked mutation or another folder");
        check(grants.grant(root, KaiCapability::CreateOnlyMutation) &&
                  grants.allows(root, KaiCapability::CreateOnlyMutation),
              "explicit mutation grant refused");
        grants.revoke(root, KaiCapability::ReadNavigation);
        check(!grants.allows(root, KaiCapability::ReadNavigation) &&
                  grants.allows(root, KaiCapability::CreateOnlyMutation),
              "revoking read also changed mutation");
        grants.revoke(root, KaiCapability::CreateOnlyMutation);
        check(!grants.allows(root, KaiCapability::CreateOnlyMutation),
              "mutation grant was not revoked");
        check(grants.grant(root, KaiCapability::ReadNavigation), "fresh read grant refused");
        const QString moved = QDir(temp.path()).filePath(QStringLiteral("old-project"));
        check(QDir().rename(root, moved) && QDir().mkpath(root),
              "temporary root replacement setup failed");
        check(!grants.allows(root, KaiCapability::ReadNavigation) &&
                  !grants.grant(root, KaiCapability::CreateOnlyMutation),
              "replaced folder retained authority");
        check(grants.grant(root, KaiCapability::ReadNavigation) &&
                  grants.allows(root, KaiCapability::ReadNavigation),
              "new folder could not receive fresh consent");
        grants.setEnabled(false);
        grants.setEnabled(true);
        check(!grants.allows(root, KaiCapability::ReadNavigation),
              "disabling integration failed to clear session grants");
        grants.grant(root, KaiCapability::ReadNavigation);
        grants.revokeAll();
        check(!grants.allows(root, KaiCapability::ReadNavigation), "revoke-all failed");
        std::puts("Kai session grant tests passed");
        return 0;
    }
    catch (const std::exception& ex)
    {
        std::fprintf(stderr, "Kai session grant test failed: %s\n", ex.what());
        return 1;
    }
}
