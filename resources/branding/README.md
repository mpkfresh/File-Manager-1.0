# File Manager branding assets

**Clearly marked integration point:** replace `app-logo.svg`, located in this
folder beside `REPLACE_LOGO_HERE.md`, to update the logo throughout File Manager.
Keep the filename unchanged, use a self-contained SVG with vector paths, then run
`python tools/refresh-brand-assets.py` and rebuild. The script uses
`app-logo.png` as an intermediate raster export to derive `app-icon.ico`.
The PNG is not embedded in the app or staged in release packages. The app
renders the SVG directly through its existing Qt SVG icon plugin; the ICO
supplies Windows icon surfaces.
The running window/taskbar uses the generated square ICO, matching the executable
and installer icon; passing the wide SVG directly to Windows can distort its proportions.
Do not edit the PNG or ICO independently.

The current logo is the user-supplied `File-Manager.svg`, integrated on 2026-08-31.
It contains three vector paths, no embedded bitmap and no external references, and
is stored unchanged as `app-logo.svg`. Non-square artwork is
centered on transparent square icon frames without stretching or cropping.
On 2026-10-17 the owner confirmed ownership and approved publishing this SVG
and its generated PNG/ICO in the proposed public source repository under the
repository's MIT license. This grants the stated asset license; publication
of the repository remains a separate release decision.

Recommended source artwork:

- Use vector paths and a viewBox; do not embed a PNG inside an SVG wrapper.
- Keep the background transparent. Do not include scripts or external references.
- Keep important details away from the outermost 5% of the canvas.
- Check the result against the dark application background.

The same source artwork supplies Windows executable icons, the application and
taskbar icon, sidebar brand area, empty preview state, and the release package's
Windows Installed apps identity. The UI falls back to the product name if the
embedded image cannot be loaded. Product name, publisher, and release channel are
separate centralized CMake cache values, so replacing the artwork cannot
accidentally rename the app.

See `REPLACE_LOGO_HERE.md` for the short release handoff checklist.

The export tool needs Python with Pillow and Node.js with sharp. These are developer
tools only, not application runtime dependencies. If sharp is not already available:

```powershell
npm install --prefix out/branding-tools --no-save sharp
python tools/refresh-brand-assets.py
```

The Node helper searches that isolated tools folder and the normal module paths.

## Clarity checks

Run `python tools/inspect-brand-assets.py` to generate `out/branding-review.png`.
Inspect the 16, 24, 32, 48, 64, 128 and 256 px frames at 100% zoom on both light
and dark backgrounds. Enlarged pixel samples are diagnostic, not display-size previews.
The UI tests check physical rendering resolution and aspect ratio for all three logo
surfaces from 100% through 400% scaling. Logo labels refresh on display-scale changes.

The 2026-08-31 review found that the folder remains recognizable, but the small M
loses definition at 16–24 px and the black outline blends into dark backgrounds.
These are artwork/detail and contrast limits, not only resampling issues. The supplied
SVG removes the raster source-resolution limit but does not make every detail readable
at 16 px. On 2026-08-31 the owner approved retaining the current artwork unchanged for 1.0,
including this known small-size/dark-background limitation. Do not claim clarity at every size
or alter the supplied design without a new request.

Packaging derives its display identity from the compiled executables and rejects mismatched
overrides. Internal installer registry keys and AppContainer identifiers are intentionally stable,
not logo or publisher customization points. Verify the final executable and installer identity
together before distribution; changing publisher metadata also needs a settings-migration review.
