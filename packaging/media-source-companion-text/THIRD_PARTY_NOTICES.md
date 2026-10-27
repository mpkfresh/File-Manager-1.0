# Third-party software notices

File Manager's Windows package redistributes dynamically linked Qt libraries and related runtime
components and statically links the exact minizip-ng and zlib-ng revisions listed below. This file
is informational; each component remains governed by its license.

## Windows system components

File Manager uses Windows platform APIs, including the PathCch path-string functions through the
Windows SDK import library. Windows supplies this runtime component; File Manager does not
redistribute it as third-party source or as an additional bundled DLL.

## Windows SDK graphics redistributables

The Windows payload includes `d3dcompiler_47.dll`, `dxcompiler.dll` and
`dxil.dll`. Microsoft's [Windows SDK REDIST list](https://learn.microsoft.com/en-us/legal/windows-sdk/redist)
lists these files under `Redist\D3D\x64` for unmodified distribution with
classic Windows applications, subject to the SDK license terms. The bundled
`licenses/Windows-SDK-Third-Party-Notices.rtf` preserves the installed SDK's
third-party notices, including its DirectX Shader Compiler and LLVM sections.
The three files remain Microsoft and third-party components under their own
terms; File Manager's MIT license does not replace those terms.

## Qt 6.11.2

Copyright © The Qt Company Ltd. and other contributors.

This build uses the open-source Qt distribution and dynamically links the Qt Core, Gui, Widgets,
Network, Sql, Multimedia and Svg modules. Most used Qt functionality is available under GNU Lesser
General Public License version 3; individual modules and bundled components may offer additional
license choices. The 1.0 release payload retains the Qt license bundle and
QtBase and QtSvg CycloneDX manifests. Its rebuilt Qt Multimedia DLL and
FFmpeg plugin, together with five source-built FFmpeg DLLs, are identified
in `manifest/media-provenance.json`. That File Manager source/build and
binary-hash record is not a Qt-supplied SBOM. The vendor Qt Multimedia
CycloneDX file is omitted from this payload because it names FFmpeg 7.1.3
instead of the source-built 7.1.5 libraries.
Users may replace the dynamically linked Qt libraries with compatible modified versions;
the installer must not prohibit that right.
The package includes a conservative set of QtBase third-party copyright and
license statements at `licenses/Qt-ThirdParty-Attributions.txt`, generated from
the installed Qt 6.11.2 attribution pages. The Qt Multimedia, FFmpeg, SVG and
Public Suffix List notices below supplement that appendix.

Project and licensing information: https://www.qt.io/licensing/open-source-lgpl-obligations

## Mesa llvmpipe via Qt's Windows package

Qt identifies the `opengl32sw.dll` shipped in its prebuilt Windows packages
as Mesa llvmpipe. Qt's 6.11.2 attribution lists MIT and Boost Software
License 1.0 terms. This notice does not establish the source revision or
build record for the exact DLL in File Manager's final package.

Copyright (C) 1999–2007 Brian Paul.

MIT License

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in
all copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL
BRIAN PAUL BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN
AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN
CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.

Copyright (c) 2013–2016 The Khronos Group Inc.

Permission is hereby granted, free of charge, to any person obtaining a
copy of this software and/or associated documentation files (the
"Materials"), to deal in the Materials without restriction, including
without limitation the rights to use, copy, modify, merge, publish,
distribute, sublicense, and/or sell copies of the Materials, and to permit
persons to whom the Materials are furnished to do so, subject to the
following conditions:

The above copyright notice and this permission notice shall be included
in all copies or substantial portions of the Materials.

THE MATERIALS ARE PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS
OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE MATERIALS OR THE USE OR OTHER DEALINGS IN
THE MATERIALS.

Qt also identifies a C11 `<threads.h>` emulation library, copyright (c)
2012 yohhoy, under the Boost Software License 1.0. That license's complete
terms are reproduced in the FFmpeg backend Boost section below.

Qt 6.11.2 attribution: https://doc.qt.io/qt-6.11/qt-attribution-llvmpipe.html

## FFmpeg via Qt Multimedia

Copyright © 2000–2025 the FFmpeg developers.

The five FFmpeg shared libraries in the 1.0 media set were built from the
upstream `n7.1.5` source archive. File Manager uses them with a rebuilt
Qt Multimedia 6.11.2 DLL and FFmpeg media plugin. The copied Qt source
changes the FFmpeg attribution version and source fields from 7.1.3 to
7.1.5. The project's `manifest/media-provenance.json` records the exact
seven binary hashes, source archives, attribution edit, build results and
Qt build-graph inputs. The FFmpeg build record has GPL, nonfree, version-3,
AVDevice and AVFilter modes disabled, and zlib enabled. The package does
not deploy an OpenSSL backend.
The release payload contract includes the full LGPLv2.1 text at
`licenses/FFmpeg-LGPL-2.1.txt`.
The final release page must provide the corresponding FFmpeg and Qt source,
actual modifications and build/relink instructions for these exact
binaries. The Boost, IJG/zlib and Signalsmith notices below also apply
where their code is present. Exact source delivery and final package bytes
remain subject to the release review.

Project and source information: https://ffmpeg.org/
Qt 6.11.2 attribution: https://doc.qt.io/qt-6.11/qtmultimedia-attribution-ffmpeg.html

### FFmpeg backend: Boost math code

Copyright John Maddock 2006. Copyright (c) 2006 Xiaogang Zhang.
Qt identifies Boost math algorithms used by the FFmpeg backend under the
Boost Software License 1.0:

Boost Software License - Version 1.0 - August 17th, 2003

Permission is hereby granted, free of charge, to any person or organization
obtaining a copy of the software and accompanying documentation covered by
this license (the "Software") to use, reproduce, display, distribute,
execute, and transmit the Software, and to prepare derivative works of the
Software, and to permit third-parties to whom the Software is furnished to
do so, all subject to the following:

The copyright notices in the Software and this entire statement, including
the above license grant, this restriction and the following disclaimer, must
be included in all copies of the Software, in whole or in part, and all
derivative works of the Software, unless such copies or derivative works are
solely in the form of machine-executable object code generated by a source
language processor.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE, TITLE AND NON-INFRINGEMENT. IN NO EVENT
SHALL THE COPYRIGHT HOLDERS OR ANYONE DISTRIBUTING THE SOFTWARE BE LIABLE FOR
ANY DAMAGES OR OTHER LIABILITY, WHETHER IN CONTRACT, TORT OR OTHERWISE,
ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
DEALINGS IN THE SOFTWARE.

Qt 6.11.2 attribution: https://doc.qt.io/qt-6.11/qtmultimedia-attribution-ffmpeg-boost.html

### FFmpeg backend: IJG and zlib-derived code

Qt attributes modified IJG code in FFmpeg to Thomas G. Lane and Guido
Vollbeding (1991–2020), and Adler-32 code to Jean-loup Gailly and Mark Adler
(1995–2022). The IJG acknowledgement in the JPEG section below also applies
to this FFmpeg code. The zlib license terms are reproduced in the zlib-ng
section below. The final source offer must identify Qt's modifications to
the FFmpeg/IJG code.
Qt 6.11.2 attributions:
https://doc.qt.io/qt-6.11/qtmultimedia-attribution-ffmpeg-libjpeg.html and
https://doc.qt.io/qt-6.11/qtmultimedia-attribution-ffmpeg-zlib.html

## Signalsmith Stretch 1.0.0

Copyright (c) 2022 Geraint Luff / Signalsmith Audio Ltd.
Qt's FFmpeg media backend uses this pitch-compensation code. Its MIT license
text is included at `licenses/Signalsmith-MIT.txt`.
Qt 6.11.2 attribution: https://doc.qt.io/qt-6.11/qtmultimedia-attribution-signalsmith-stretch.html

## Qt Multimedia: DR Libs 0.14.5

Qt uses DR Libs for `QSoundEffect` WAV decoding. It offers a choice between
the Unlicense and MIT No Attribution; this package records the MIT No
Attribution terms:

Copyright 2020 David Reid

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
SOFTWARE.

Qt 6.11.2 attribution: https://doc.qt.io/qt-6.11/qtmultimedia-attribution-dr-libs.html

## Qt Multimedia: Two-Level Segregated Fit v3.1

Copyright (c) 2006–2016, Matthew Conte.
Qt uses this allocator in `QSoundEffect`. It is under the BSD 3-Clause terms:

Redistribution and use in source and binary forms, with or without
modification, are permitted provided that the following conditions are met:

1. Redistributions of source code must retain the above copyright notice,
   this list of conditions and the following disclaimer.
2. Redistributions in binary form must reproduce the above copyright notice,
   this list of conditions and the following disclaimer in the documentation
   and/or other materials provided with the distribution.
3. Neither the name of the copyright holder nor the names of its contributors
   may be used to endorse or promote products derived from this software
   without specific prior written permission.

THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE
LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
POSSIBILITY OF SUCH DAMAGE.

Qt 6.11.2 attribution: https://doc.qt.io/qt-6.11/qtmultimedia-attribution-tlfs.html

## Qt SVG: XSVG-derived arc handling

Copyright 2002 USC/Information Sciences Institute.
Qt SVG incorporates XSVG-derived arc-handling code under the Historical
Permission Notice and Disclaimer (sell variant):

Permission to use, copy, modify, distribute, and sell this software and its
documentation for any purpose is hereby granted without fee, provided that
the above copyright notice appear in all copies and that both that copyright
notice and this permission notice appear in supporting documentation, and
that the name of Information Sciences Institute not be used in advertising
or publicity pertaining to distribution of the software without specific,
written prior permission. Information Sciences Institute makes no
representations about the suitability of this software for any purpose. It
is provided "as is" without express or implied warranty.

INFORMATION SCIENCES INSTITUTE DISCLAIMS ALL WARRANTIES WITH REGARD TO THIS
SOFTWARE, INCLUDING ALL IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS,
IN NO EVENT SHALL INFORMATION SCIENCES INSTITUTE BE LIABLE FOR ANY SPECIAL,
INDIRECT OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES WHATSOEVER RESULTING FROM
LOSS OF USE, DATA OR PROFITS, WHETHER IN AN ACTION OF CONTRACT, NEGLIGENCE
OR OTHER TORTIOUS ACTION, ARISING OUT OF OR IN CONNECTION WITH THE USE OR
PERFORMANCE OF THIS SOFTWARE.

Qt 6.11.2 attribution: https://doc.qt.io/qt-6.11/qtsvg-attribution-xsvg.html

## Public Suffix List data in Qt Network

Qt Network's versioned attribution identifies Public Suffix List data dated
2026-05-14. The list was originally provided by Jo Hermans and is now
maintained by the publicsuffix/list community. Qt locates its copy at
`qtbase/src/3rdparty/libpsl/psl_data.cpp`. The data is under Mozilla Public
License 2.0; the full license text is in
`licenses/Mozilla-Public-License-2.0.txt`. The final release source record must
identify the exact corresponding source for the deployed Qt Network binary.
Qt 6.11.2 attribution: https://doc.qt.io/qt-6.11/qtnetwork-attribution-psl-data.html

## JPEG image plugin

The packaged Qt JPEG image plugin uses libjpeg-turbo 3.2.0. This software is
based in part on the work of the Independent JPEG Group.
Qt 6.11.2 attribution: https://doc.qt.io/qt-6.11/qtgui-attribution-libjpeg.html

## minizip-ng 4.2.2

The exact source revision is `7b2387161c542fa9f427352dcdef76097d0d692b`.

Condition of use and distribution are the same as zlib:

This software is provided 'as-is', without any express or implied warranty. In no event will the
authors be held liable for any damages arising from the use of this software.

Permission is granted to anyone to use this software for any purpose, including commercial
applications, and to alter it and redistribute it freely, subject to the following restrictions:

1. The origin of this software must not be misrepresented; you must not claim that you wrote the
   original software. If you use this software in a product, an acknowledgement in the product
   documentation would be appreciated but is not required.
2. Altered source versions must be plainly marked as such, and must not be misrepresented as being
   the original software.
3. This notice may not be removed or altered from any source distribution.

## zlib-ng 2.3.3

Copyright © 1995–2024 Jean-loup Gailly and Mark Adler. The exact source revision is
`12731092979c6d07f42da27da673a9f6c7b13586`.

This software is provided 'as-is', without any express or implied warranty. In no event will the
authors be held liable for any damages arising from the use of this software.

Permission is granted to anyone to use this software for any purpose, including commercial
applications, and to alter it and redistribute it freely, subject to the following restrictions:

1. The origin of this software must not be misrepresented; you must not claim that you wrote the
   original software. If you use this software in a product, an acknowledgment in the product
   documentation would be appreciated but is not required.
2. Altered source versions must be plainly marked as such, and must not be misrepresented as being
   the original software.
3. This notice may not be removed or altered from any source distribution.
