#pragma once

#include "platform/shellitem.h"

#include <QList>
#include <QStringList>
#include <QtGlobal>

#include <optional>

class QMimeData;

namespace filemanager::platform {

enum class ShellTransferEffect { Copy, Move };

struct ShellClipboardItems
{
    bool available = false;
    QList<ShellItemReference> items;
    QString error;
};

class ShellClipboard final
{
public:
    static void populateFileTransfer(QMimeData* mimeData, const QStringList& paths,
                                     ShellTransferEffect effect);
    static bool populateShellItems(QMimeData* mimeData,
        const QList<ShellItemReference>& items, QString* error = nullptr);
    static std::optional<ShellTransferEffect> preferredEffect(const QMimeData* mimeData);
    static QString preferredDropEffectMimeType();
    static QString shellIdListMimeType();
    static ShellClipboardItems shellItems(const QMimeData* mimeData,
        int maximumItems = 256);
    static ShellClipboardItems shellItems(int maximumItems = 256);
    static quint64 sequenceNumber();
    static bool publishPerformedEffect(ShellTransferEffect effect,
        quint64 expectedSequenceNumber, QString* error = nullptr);
};

} // namespace filemanager::platform
