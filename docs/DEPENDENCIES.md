# Release dependency inventory

This inventory describes source and dependency inputs for File Manager 1.0.
The official GitHub Releases page identifies any accepted package hashes and
support status; a source checkout or review artifact is not a release.
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
does not establish legal completeness, two-profile isolation or a supported release.
An unexplained component or version change stops a release
until the inventory, notices,
licenses and tests are updated.

| Component | Pinned version or revision | Use |
| --- | --- | --- |
| Qt | 6.11.2, MSVC 2022 x64 selected for 1.0; exact package bytes require release acceptance | Dynamically linked Core, Gui, Widgets, Network, Sql, Multimedia and Svg |
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
The seven retained V53 media files are approved as **packaging inputs** and
published with a [focused source bundle](../packaging/v53-media-source.zip)
and [provenance manifest](../packaging/ffmpeg-1.0-v53-approved-input.json).
The later [hosted run 42317566921](https://github.com/mpkfresh/File-Manager-1.0/actions/runs/42317566921)
produced an unsigned 1.0 MSI and portable ZIP that were downloaded and
hash-matched for local review. They remain unreleased; later source and
package-text changes require a new build and exact-byte review.
Complete corresponding source, modifications, build/relink instructions and
remaining shipped-component attributions still need final distribution
review. Qt [identifies](https://doc.qt.io/qt-6.11/windows-graphics.html)
its prebuilt `opengl32sw.dll` as Mesa llvmpipe. The notice
includes [Qt's MIT/Boost attribution](https://doc.qt.io/qt-6.11/qt-attribution-llvmpipe.html);
the exact shipped build/source route remains open. The unsigned 1.0 pair's
three shader compiler DLLs match the
earlier hash-pinned hosted Windows SDK inputs by SHA-256; two differ from this
developer PC's older SDK. The raw hosted source-path report was not retained.
[Microsoft's SDK REDIST list](https://learn.microsoft.com/en-us/legal/windows-sdk/redist)
names the three `Redist/D3D/x64` files for unmodified classic-app distribution,
subject to the SDK license. The package carries the SDK's complete third-party
notice at `licenses/Windows-SDK-Third-Party-Notices.rtf`. The ten bundled VC143
runtime DLLs match the earlier compiler-selected hosted redist inputs by hash;
their raw source-path report was not retained either.
[Microsoft's VS2022 distributable-code list](https://learn.microsoft.com/en-us/visualstudio/releases/2022/redistribution)
conditions distribution on the Visual Studio license. The release review must
confirm applicable rights and preserve the exact source/version evidence.

Before each release, review current upstream security advisories for Qt,
FFmpeg, minizip-ng and zlib-ng, reconcile their exact packaged files and
licenses with the manifest, then run the native, malicious-input, package
inventory and clean-Windows gates on the final bytes. Critical or known
exploitable issues block release. This inventory does not establish
redistribution rights or a complete corresponding source offer; those require
review of every packaged component and its applicable terms.
