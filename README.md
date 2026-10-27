# File Manager

**Local public-source draft; not approved for publication.** File Manager is
a Windows file manager written in C++20 and Qt 6. The last broadly verified
development build is `0.9.5-rc.7`; later source and affected checks do not
establish a supported 1.0 release.
The October 24 development build passed affected Preferences and Visual
checks plus a synthetic Cloud Files offline-refusal check. Its app SHA-256 is
`5F262A3012F22C51BFF6580517B8CC24414E3DB138661EACD5CC15F4AD0085AD`.
The last broad Windows native and installed-package checks used older app
bytes. The current build has no final native, matching-package, installed or
real-provider acceptance, so it does not establish a 1.0 result.

It provides tabs, split panels, local and direct UNC browsing, file operations
with preview and supported session Undo, on-demand search, previews, favorites,
and reviewed TV and book-series organizing. File labels help identify files;
they do not generate destination folders or moves. Opening supported folders
or Win+E with File Manager is separately optional and off by default. Windows
continues to provide file associations, property sheets and Recycle Bin
operations.

## Moving from 0.x to 1.0

The proposed 1.0 installer uses a new Kiatami identity. If a 0.x MSI is
installed, uninstall it through Windows Settings before installing 1.0; the
1.0 installer is designed to stop and explain this if the old MSI remains.
For a 0.x portable copy, turn off its optional Windows folder and Win+E
integrations before retiring that copy; there is no MSI for 1.0 to detect.
File Manager 1.0 starts with fresh settings and file labels. It does not
import or delete the old per-user data. If you want to clear old data, use
the old version's Preferences controls before uninstalling it. This behavior
is still awaiting the final installed-package checks.

## Build from source

Use Windows with Visual Studio 2022 C++ tools, CMake 3.25 or newer and the
open-source Qt 6.11.2 MSVC x64 kit. The checked-in preset expects Qt at
`C:/Qt/6.11.2/msvc2022_64`. If yours is elsewhere, pass
`-DCMAKE_PREFIX_PATH="D:/path/to/Qt/6.11.2/msvc2022_64"` to the configure
command; that command-line value overrides the preset's cache value.
Qt 6.11.2 is the selected input for the proposed 1.0 source build. The owner
accepted its known JSON-parser and HTTP/2 crash risks for 1.0 with explicit
release-page disclosure and a later fixed-Qt update. Final package bytes,
corresponding source and notices still require review; this draft does not
claim a verified release.
The first configure fetches the exact minizip-ng and zlib-ng revisions pinned
in `CMakeLists.txt`. An offline build can supply these source trees through
CMake's `FETCHCONTENT_SOURCE_DIR_*` variables. Those overrides must still
match the pinned commits and file contents; the prepared release verifier
reads CMake's consumed-source record for that purpose. It has not run on a
final source commit.

```powershell
cmake --preset windows-release
cmake --build --preset windows-release
ctest --preset windows-release -LE focused
```

Run the native suite in a disposable Windows user profile: it creates
synthetic files and uses temporary Windows integration fixtures. The
`focused` entries repeat subsets of the full Core and Visual targets, so
the command above omits those duplicate entry points while retaining the
full targets and the separate DPI checks.

The checked-in source now defaults to `1.0.0` with an empty release channel;
an affected development build passed five executable identity checks, full
Core and Visual invariants. Final release verification remains open. A
development build labeled
`0.9.5-rc.7` uses a separate build directory and the CMake cache values
`FILE_MANAGER_BUILD_VERSION=0.9.5` and
`FILE_MANAGER_RELEASE_CHANNEL=rc.7`. A fresh build has new bytes: older local
test or package results cannot verify it. The preset outputs to
`out/build/windows-release/Release`.

A separate `windows-release-reviewed` preset selects Visual Studio 2022,
requires an explicit `FILE_MANAGER_QT_PREFIX` for the matching MSVC 2022 Qt
kit, and sets proposed 1.0 identity. It has not completed a clean source
build. The selected Qt 6.11.2 component and compiler inputs still need
exact-byte review in the final source/build gate.

Before this draft becomes the 1.0 repository README, V1 must build these
defaults from the reviewed source commit and inspect all executable and
package identities. An affected local development build and its approved
no-rebuild continuation do not replace the final reviewed-source V1 build.

## Privacy and security

File Manager has no account, advertising, telemetry, analytics or automatic
updater. Content search and network discovery run when requested. Persistent
indexing, workspace restoration, Windows Shell handlers, folder routing and
Win+E integration start off. Read the [privacy draft](PRIVACY.md) for local
storage, permissions and clearing. The [security policy](SECURITY.md) explains
reporting status and supported-release limits. Do not attach private data to
public issues.

## Licenses and contribution

The application source is under the [MIT License](LICENSE). Bundled components
have separate obligations; see the [dependency inventory](docs/DEPENDENCIES.md)
and [third-party notices](THIRD_PARTY_NOTICES.md).
The [brand asset guide](resources/branding/README.md) documents replacing the
source logo and generating release icons. For a source change, keep UI code
out of file/search/settings services, add an appropriate deterministic test
when it verifies behavior, build with warnings treated as errors, and review
recovery, path validation, threading and keyboard access. Run affected tests
for each change; the complete native suite belongs to the exact-byte release
gate in an interactive Windows user session, since a restricted sandbox
cannot exercise every Windows API. Clean-machine installed acceptance is
also required before 1.0.

This draft source tree is not an official download location. No unsigned
prerelease or signed 1.0 package has been published or authorized here.
