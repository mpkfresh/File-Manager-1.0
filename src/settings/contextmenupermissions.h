#pragma once

class QSettings;

namespace filemanager::settings {

struct ContextMenuPermissions final { bool allowWindowsExtensions = false; };

ContextMenuPermissions loadContextMenuPermissions();
ContextMenuPermissions loadContextMenuPermissions(const QSettings& settings);
void saveContextMenuPermissions(const ContextMenuPermissions& permissions);
void saveContextMenuPermissions(QSettings& settings, const ContextMenuPermissions& permissions);

} // namespace filemanager::settings
