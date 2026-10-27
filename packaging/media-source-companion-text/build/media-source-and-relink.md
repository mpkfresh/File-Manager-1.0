# Media source and relink instructions — review draft

**Internal draft, October 26, 2026.** This is a host-neutral translation of
the isolated FFmpeg R2 and Qt Multimedia R2c build records for the proposed
File Manager 1.0 source companion. It has not been executed as written,
checked against a signed package, or approved for publication. The companion's
`checksums.sha256` lists every delivered file. The table below identifies its
three source archives without relying on internal project notes.

## Inputs

Use the supplied FFmpeg `n7.1.5`, Qt Multimedia 6.11.2 and zlib 1.3.1 source
archives after checking `checksums.sha256`. Use the two supplied patches:
one copied `zconf.h` line that treats `HAVE_UNISTD_H=0` as absent, and the
Qt Multimedia `qt_attribution.json` change from FFmpeg 7.1.3 to 7.1.5.
The original zlib source and library are unchanged by the header copy.

| Source archive in `source/` | SHA-256 |
| --- | --- |
| `ffmpeg-n7.1.5.tar.gz` | `E3963A50831C985933E1A625ED566EC4C7ADB5C012C34FA9F84438E1D61BDACC` |
| `qtmultimedia-everywhere-src-6.11.2.tar.xz` | `967B5E02EC6B793CDB360622CD6E703132836AF983208D678DAE4B50F109CD9F` |
| `zlib-1.3.1.tar.gz` | `9A93B2B7DFDAC77CEBA5A558A580E74667DD6FEDE4585B91EEFB60F03B72DF23` |

The recorded Windows x64 build used Visual Studio 2022 MSVC 14.44.35207,
MSYS2 base 2026-09-27 with make 4.4.1-3, yasm 1.3.0-3 and diffutils 3.12-1,
the Qt 6.11.2 MSVC 2022 kit, and its matching ShaderTools component. Tool
binaries are not proposed for redistribution. Use fresh source, build and
output directories and substitute their absolute paths below. Run `nmake`
from a Visual Studio x64 developer command prompt after `vcvars64.bat`.
Launch MSYS2 Bash with that MSVC environment inherited for the FFmpeg
`configure`/`make` steps. The Qt CMake/Ninja steps run from the developer
command prompt with the matching Qt and ShaderTools `bin` directories on PATH.

## FFmpeg and zlib

Build static `zlib.lib` from the supplied zlib source using the MSVC makefile
and run its upstream `test` target. Copy `zlib.lib`, `zlib.h` and `zconf.h`
into a separate FFmpeg interop directory; apply the supplied one-line patch
only to that copied header. The isolated R2 build record pins the source,
original/patched header and zlib output hashes.

```text
nmake -f win32\Makefile.msc zlib.lib
nmake -f win32\Makefile.msc test
```

Apply `patches/zconf.h.patch` from the interop directory with a unified-diff
tool such as Git for Windows:
`git -c core.autocrlf=input -c core.eol=lf apply --unidiff-zero -p1 <path-to-zconf.h.patch>`.
The supplied patch has zero context lines, so Git requires that explicit
flag. The per-command line-ending settings keep the source archive's LF
endings on Windows without changing global Git configuration. The pinned
original source archive and required before/after hashes guard this narrow edit. It
must change one header line. The original copied header should hash to
`F5134250A67D57459234B63858F0D9D3EF8DCC48E9E1028D3F4FDCF6EAE677AE`;
the patched header should hash to
`132778E37C0B74BFE9F0A4B254FD8675E26543B2C1B69DD4A54FA4318745BAC3`.
The companion preparation reproduced that hash by applying the exact line
substitution in memory. The published patch command still needs a clean-host
check.

Configure FFmpeg from an empty build directory with the following selected
options, replacing the angle-bracket paths with local paths. The `--prefix`
and `--extra-*` path syntax must suit the MSYS2/MSVC environment:

```sh
<ffmpeg-source>/configure --prefix=<ffmpeg-output> \
  --disable-programs --disable-doc --disable-debug --enable-network \
  --disable-lzma --enable-pic --disable-vulkan --disable-v4l2-m2m \
  --disable-decoder=truemotion1 --disable-avdevice --disable-avfilter \
  --enable-zlib --extra-cflags=-I<zlib-interop> \
  --extra-ldflags=-LIBPATH:<zlib-interop> \
  --toolchain=msvc --enable-shared --disable-static
make -j2
make install
```

The recorded build disabled GPL, nonfree and version-3 modes; review the
generated `ffbuild/config.mak` and the five DLLs rather than treating a
source command alone as proof of their final configuration. Retain the five
FFmpeg import libraries for the Qt build.

## Qt Multimedia FFmpeg backend

Extract the Qt Multimedia source into a new directory and apply the supplied
two-field FFmpeg attribution patch. Keep the rest of the official source
unchanged.

Apply `patches/qt-ffmpeg-attribution.patch` from the copied Qt Multimedia
source root with one stripped path component, for example
`git -c core.autocrlf=input -c core.eol=lf apply --unidiff-zero -p1 <path-to-qt-ffmpeg-attribution.patch>`.
It must change only the
FFmpeg download tag and version fields. The patched attribution file should
hash to `24D5FFF2E100FB7C2531771D594A008992F85406387EFC9BA6889D5C51EFC343`.
The companion preparation reproduced that hash by applying the two exact
substitutions in memory. The published patch command still needs a clean-host
check.

After `vcvars64.bat`, configure with the matching Qt kit and ShaderTools
CMake prefixes, the five FFmpeg import libraries and Ninja:

```text
cmake -S <qt-multimedia-source> -B <qt-build> -G Ninja
  -DCMAKE_MAKE_PROGRAM=<ninja> -DCMAKE_BUILD_TYPE=Release
  -DCMAKE_PREFIX_PATH="<shader-tools>;<qt-kit>"
  -DQt6ShaderTools_DIR=<shader-tools>/lib/cmake/Qt6ShaderTools
  -DFFMPEG_DIR=<ffmpeg-output> -DQT_DEPLOY_FFMPEG=OFF
  -DQT_BUILD_EXAMPLES=OFF -DQT_BUILD_TESTS=OFF
  -DCMAKE_INSTALL_PREFIX=<qt-output>
cmake --build <qt-build> --target QFFmpegMediaPlugin --parallel 2
```

The line breaks above are for reading; enter one complete `cmake` command
in the chosen shell. The recorded outputs are `Qt6Multimedia.dll` and
`multimedia/ffmpegmediaplugin.dll`. The isolated build's exact CMake cache,
Ninja graph and incremental recipe hashes belong to provenance review, not
to a portable command that embeds one computer's absolute paths.

## Relink and replace

The proposed Windows payload dynamically links the seven media files: five
FFmpeg DLLs, `Qt6Multimedia.dll`, and
`multimedia/ffmpegmediaplugin.dll`. Build compatible replacements from the
supplied source and patches, close File Manager, preserve copies of the
installed seven files, and replace those files in the application directory
and `multimedia/` subdirectory. A replacement must retain the needed ABI and
Windows x64 architecture. Its hash need not match the distributed build;
the release manifest describes the distributed bytes only.

Before this draft becomes a release source companion, execute its commands
on a clean build host, confirm the patch application and license files,
review every delivered file for private paths, and compare the signed V2
payload with the seven-file provenance record. A download page and any
compliance conclusion require separate owner review.
