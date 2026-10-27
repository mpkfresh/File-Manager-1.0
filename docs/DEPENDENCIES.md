# Release dependency inventory

**Local public-source draft; review exact final package before publication.**
The proposed Windows x64 1.0 source selects Qt 6.11.2 with the matching MSVC
2022 kit. The owner accepted the known Qt 6.11.2 JSON-parser and HTTP/2
crash risks for 1.0 with disclosure and a later fixed-Qt update; the final
release page must describe them against the exact shipped bytes. This choice
does not establish that a package is safe or complete. The earlier R213
unsigned 57-file MSI and portable package used app `B0A904D9…7EACAD07B`
and different media bytes. They are historical evidence, not the 1.0 payload.
R214 passed the same
app's 11 Windows native targets and smoke, plus sampled installed behavior and
clean uninstall; original-user isolation was not completed.
The `packaging/release-payload.txt` contract includes QtSvg's SBOM, the
Signalsmith MIT text, the MPL 2.0 text attributed to Qt Network's Public
Suffix List data, and a QtBase attribution appendix. This host package pass
does not establish legal completeness, two-profile isolation or a signed release.
An unexplained component or version change stops a release
until the inventory, notices,
licenses and tests are updated.

| Component | Pinned version or revision | Use |
| --- | --- | --- |
| Qt | 6.11.2, MSVC 2022 x64 selected for proposed 1.0; final package bytes pending | Dynamically linked Core, Gui, Widgets, Network, Sql, Multimedia and Svg |
| minizip-ng | [4.2.2 source](https://github.com/zlib-ng/minizip-ng/commit/7b2387161c542fa9f427352dcdef76097d0d692b); `7b2387161c542fa9f427352dcdef76097d0d692b` | Statically linked ZIP reading |
| zlib-ng | [2.3.3 source](https://github.com/zlib-ng/zlib-ng/commit/12731092979c6d07f42da27da673a9f6c7b13586); `12731092979c6d07f42da27da673a9f6c7b13586` | Statically linked compression support for minizip-ng |
| FFmpeg | Five isolated DLLs built from upstream `n7.1.5`; matching Qt Multimedia module/plugin rebuilt from 6.11.2 source; final package pending | Shared media libraries used by Qt Multimedia's FFmpeg backend |

The Qt distribution's CycloneDX manifests identify further runtime
components, including SQLite, PCRE2, FreeType, HarfBuzz, libjpeg-turbo and
libpng. The Windows deployment includes selected Qt platform, image, SVG,
SQLite, network/TLS and multimedia plugins plus Qt-supplied graphics/media
runtime DLLs. It does not deploy an OpenSSL backend. The proposed 58-path 1.0
payload includes QtBase and QtSvg SBOMs, Qt's license bundle, the LGPLv2.1
text at `licenses/FFmpeg-LGPL-2.1.txt`, and a File Manager
`manifest/media-provenance.json` for the seven source-built media files.
It omits Qt's vendor Multimedia SBOM because that file names FFmpeg 7.1.3,
while the proposed source-built libraries are 7.1.5. The project record is
**not** a Qt-supplied SBOM. Component copyrights and licensing are described
in [third-party notices](../THIRD_PARTY_NOTICES.md) and upstream source.
The local source companion and provenance record are review candidates; no
real seven-file manifest has been release-approved or placed in a package.
Complete corresponding source, modifications, build/relink instructions and
remaining shipped-component attributions still need final distribution
review. Qt [identifies](https://doc.qt.io/qt-6.11/windows-graphics.html)
its prebuilt `opengl32sw.dll` as Mesa llvmpipe. The working notice now
includes [Qt's MIT/Boost attribution](https://doc.qt.io/qt-6.11/qt-attribution-llvmpipe.html),
and a development build copied that correction; the exact shipped build/source route
remains open. Microsoft's
[DirectX Shader Compiler](https://github.com/microsoft/DirectXShaderCompiler)
ships DLLs named `dxcompiler.dll` and `dxil.dll`, but the same names do not
establish the origin of File Manager's copies. Review `d3dcompiler_47.dll`
separately. The historical R213 copies of all three graphics compiler DLLs
match the installed Windows SDK `Redist/D3D/x64` files by SHA-256, and
[Microsoft's REDIST list](https://learn.microsoft.com/en-us/legal/windows-sdk/redist)
includes those SDK-relative paths for unmodified classic-app distribution
subject to its license. The working package contract adds the SDK's complete
third-party notice at `licenses/Windows-SDK-Third-Party-Notices.rtf`. V1/V2 must
still bind the final deployment source paths and hashes to final package bytes
and verify that notice is present. The ten bundled MSVC runtime DLLs require a match to the
actual toolchain's redistributable files and license; Microsoft's
[2026 distributable-code list](https://learn.microsoft.com/en-us/visualstudio/releases/2026/redistribution)
excludes preview components. Windows SDK functions and Windows system
components are not vendored application source.

Before each release, review current upstream security advisories for Qt,
FFmpeg, minizip-ng and zlib-ng, reconcile their exact packaged files and
licenses with the manifest, then run the native, malicious-input, package
inventory and clean-Windows gates on the final bytes. Critical or known
exploitable issues block release. This inventory does not establish SignPath
Foundation eligibility; that requires a separate review of every packaged
component and its licensing.
