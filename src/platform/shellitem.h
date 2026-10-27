#pragma once

#include <QByteArray>
#include <QList>
#include <QString>
#include <atomic>
#include <memory>

namespace filemanager::platform {

using ShellCancellation = std::shared_ptr<std::atomic_bool>;

enum class BuiltInShellLocation {
    None,
    ThisPc,
    Libraries,
    RecycleBin
};

struct ShellItemReference
{
    bool valid = false;
    bool fileSystem = false;
    bool folder = false;
    long nativeCode = 0;
    QString parsingName;
    QString fileSystemPath;
    QString displayName;
    QByteArray idList;
    QString error;
};

struct ShellItemEnumeration
{
    bool success = false;
    bool truncated = false;
    long nativeCode = 0;
    QList<ShellItemReference> items;
    QString error;
};

class ShellItem final
{
public:
    static bool isValidSerializedIdentity(const QByteArray& bytes)
    {
        if (bytes.size() < 2 || bytes.size() > 1024 * 1024)
            return false;
        qsizetype offset = 0;
        while (offset + 2 <= bytes.size()) {
            const auto* value = reinterpret_cast<const unsigned char*>(bytes.constData() + offset);
            const quint16 length = static_cast<quint16>(value[0]
                | (static_cast<quint16>(value[1]) << 8));
            if (length == 0)
                return offset + 2 == bytes.size();
            if (length < 2 || length > bytes.size() - offset)
                return false;
            offset += length;
        }
        return false;
    }
    static ShellItemReference resolve(const QString& pathOrParsingName);
    static ShellItemReference resolveIsolated(const QString& pathOrParsingName,
        int timeoutMilliseconds = 3000, const QString& helperExecutable = {},
        const ShellCancellation& cancelled = {});
    static ShellItemReference resolveBuiltIn(BuiltInShellLocation location);
    static QString builtInIdentity(BuiltInShellLocation location);
    static QString builtInParsingName(BuiltInShellLocation location);
    static QString builtInDisplayName(BuiltInShellLocation location);
    static BuiltInShellLocation builtInLocationFromIdentity(const QString& identity);
    // Inspect serialized structure only; never bind a Shell provider to enable a UI action.
    static bool hasParentIdentity(const ShellItemReference& item)
    {
        return item.valid && isValidSerializedIdentity(item.idList) && item.idList.size() > 2;
    }
    static ShellItemReference parent(const ShellItemReference& item);
    static ShellItemReference parentIsolated(const ShellItemReference& item,
        int timeoutMilliseconds = 3000, const QString& helperExecutable = {},
        const ShellCancellation& cancelled = {});
    static ShellItemEnumeration enumerateChildren(
        const ShellItemReference& parent, int maximumItems = 512);
    static ShellItemEnumeration enumerateChildrenIsolated(const ShellItemReference& parent,
        int maximumItems = 512, int timeoutMilliseconds = 3000,
        const QString& helperExecutable = {}, const ShellCancellation& cancelled = {});
};

} // namespace filemanager::platform
