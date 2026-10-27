#pragma once

namespace filemanager::settings {

struct MetadataPermissions
{
    bool allowWindowsShellHandlers = false;
};

MetadataPermissions loadMetadataPermissions();
void saveMetadataPermissions(const MetadataPermissions& permissions);

} // namespace filemanager::settings
