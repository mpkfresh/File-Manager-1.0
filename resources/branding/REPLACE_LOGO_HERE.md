# Replace the File Manager logo here

The single editable logo source is the file beside this document:

`app-logo.svg`

To install the final logo:

1. Export a self-contained SVG with vector paths and a transparent background.
   Do not embed bitmaps, scripts or external references.
2. Replace `app-logo.svg` and keep that exact filename.
3. From the repository root, run `python tools/refresh-brand-assets.py` with
   Python/Pillow and Node.js/sharp available (see `README.md` beside this file).
4. Rebuild and test the application and release packages.

The refresh script uses `app-logo.png` as an intermediate raster export to
derive `app-icon.ico`. The build embeds the SVG for the sidebar and empty-preview
artwork, and the ICO supplies the executable, window/taskbar and installer
icons, including the Windows Installed apps entry. The PNG is not embedded or
packaged. Do not edit either generated file independently.

Changing the logo after code signing requires rebuilding and signing the changed
binaries again, so approve the final artwork before the release-signing step.
