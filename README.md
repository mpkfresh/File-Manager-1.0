# File Manager

**File Manager 1.0 is in release verification; no supported 1.0 binary has
been published.** It is a Windows file manager written in C++20 and Qt 6.
The source defaults to the proposed 1.0.0/Kiatami identity. The reviewed
source passed a [hosted Windows build and affected tests](https://github.com/mpkfresh/File-Manager-1.0/actions/runs/40676287316)
without uploading binaries. Final package, signing, clean Windows and manual
acceptance checks remain open. Locally prepared unsigned binaries are withheld
from publication because they contain build paths.

It provides tabs, split panels, local and direct UNC browsing, file operations
with preview and supported session Undo, on-demand search, previews, favorites,
and reviewed TV and book-series organizing. File labels help identify files;
they do not generate destination folders or moves. Opening supported folders
or Win+E with File Manager is separately optional and off by default. Windows
continues to provide file associations, property sheets and Recycle Bin
operations.

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
reads CMake's consumed-source record for that purpose. The hosted build checked
those trees on its recorded commit; the final source commit remains open.

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

The checked-in source defaults to `1.0.0` with an empty release channel.
The preset outputs to `out/build/windows-release/Release`. Each build needs
verification on its exact binaries before it can be treated as a release.

A separate `windows-release-reviewed` preset selects Visual Studio 2022,
requires an explicit `FILE_MANAGER_QT_PREFIX` for the matching MSVC 2022 Qt
kit, and sets proposed 1.0 identity. The hosted V1 build passed on its reviewed
source commit, including source-bound media and affected tests. Its temporary
binaries were not uploaded; exact final package bytes still need verification.

The reviewed-source build, executable identities and matching package remain
part of 1.0 verification.

## Privacy and security

File Manager has no account, advertising, telemetry, analytics or automatic
updater. Content search and network discovery run when requested. Persistent
indexing, workspace restoration, Windows Shell handlers, folder routing and
Win+E integration start off. Read the [privacy policy](PRIVACY.md) for local
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

This repository publishes development source, not an official binary download.
No unsigned prerelease or signed 1.0 package has been published or authorized
here.
