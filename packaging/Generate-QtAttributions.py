"""Build a reviewable QtBase attribution appendix from installed Qt 6.11.2 docs.

This reads only the pinned pages below and copies their copyright/license
blocks. It does not decide which components were compiled into a shipped DLL.
"""

from __future__ import annotations

import argparse
import hashlib
import html
import re
from pathlib import Path


PAGES = {
    "qtcore": (
        "blake2", "doubleconversion", "easing", "md4", "md5", "pcre2-sljit",
        "pcre2", "rfc6234", "sha1", "sha3-endian", "sha3-keccak", "siphash",
        "tika-mimetypes", "tinycbor", "tlexpected",
        "unicode-character-database", "unicode-cldr", "zlib",
    ),
    "qtgui": (
        "aglfn", "d3d12memoryallocator", "emoji-segmenter", "freetype-bdf",
        "freetype-pcf", "freetype-zlib", "freetype", "grayraster",
        "harfbuzz-ng", "icc-srgb-color-profile", "libjpeg", "libpng", "md4c",
        "opengl-es2-headers", "opengl-headers", "rhi-miniengine-d3d12-mipmap",
        "smooth-scaling-algorithm", "vulkan-xml-spec",
        "vulkanmemoryallocator", "webgradients", "wintab",
    ),
    "qtnetwork": ("libpsl",),
    "qtsql": ("sqlite",),
}

PRE_BLOCK = re.compile(r'<pre class="cpp plain"[^>]*>(.*?)</pre>', re.DOTALL)
TITLE = re.compile(r'<h1 class="title">(.*?)</h1>', re.DOTALL)
DESCRIPTION = re.compile(r'<meta name="description" content="([^"]*)"')


def block_text(raw: str) -> str:
    # QDoc adds one presentation-space to each line of a <pre> block.
    decoded = html.unescape(raw)
    decoded = re.sub(r"<br\s*/?>", "\n", decoded, flags=re.IGNORECASE)
    lines = decoded.replace("\r\n", "\n").split("\n")
    return "\n".join(line[1:] if line.startswith(" ") else line for line in lines).strip("\n")


def build(doc_root: Path) -> str:
    parts = [
        "Qt 6.11.2 third-party attribution appendix",
        "",
        "Copied from the indicated installed Qt 6.11.2 attribution pages. This",
        "appendix accompanies File Manager's Windows Qt Core, GUI, Network and",
        "SQL modules. It conservatively includes statements for relevant QtBase",
        "features even where a particular Windows binary may not use that code.",
        "Qt Multimedia and FFmpeg have separate notices. Each section retains",
        "the source page's copyright and license blocks.",
        "",
    ]
    for module, names in PAGES.items():
        for name in names:
            filename = f"{module}-attribution-{name}.html"
            path = doc_root / module / filename
            source = path.read_bytes()
            page = source.decode("utf-8")
            title = TITLE.search(page)
            description = DESCRIPTION.search(page)
            blocks = PRE_BLOCK.findall(page)
            if title is None or description is None or not blocks:
                raise ValueError(f"Unexpected Qt attribution page structure: {path}")
            parts.extend(
                (
                    "=" * 72,
                    html.unescape(re.sub(r"<[^>]+>", "", title.group(1))).strip(),
                    f"Qt page: https://doc.qt.io/qt-6.11/{filename}",
                    f"Installed page SHA-256: {hashlib.sha256(source).hexdigest().upper()}",
                    f"Qt license label: {html.unescape(description.group(1))}",
                    "",
                )
            )
            for block in blocks:
                parts.extend((block_text(block), ""))
    return "\n".join(parts).rstrip() + "\n"


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--qt-doc-root", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    output = build(args.qt_doc_root)
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(output, encoding="utf-8", newline="\n")
    print(f"pages={sum(map(len, PAGES.values()))} bytes={args.output.stat().st_size} "
          f"sha256={hashlib.sha256(args.output.read_bytes()).hexdigest().upper()}")


if __name__ == "__main__":
    main()
