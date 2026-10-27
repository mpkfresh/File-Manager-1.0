File Manager portable package
=============================

Run FileManager.exe from this folder. Keep all DLLs and plugin folders beside it.

"Portable" describes the application files: no installer or machine-wide registration is
required. File Manager still uses the current Windows user's normal application-data and settings
locations for privacy-protected saved state, operation recovery, diagnostics, and explicit
default-off integrations. It does not silently redirect sensitive data into the portable folder.

Use Preferences to export/import non-sensitive portable settings. Before removing the package,
turn off any enabled Windows choices in Preferences: opening supported folders with File Manager,
Win+E (which also stops its helper and removes owned sign-in startup), the separate Open in File
Manager folder command, and Windows App Paths. Then clear saved application data there if desired.
