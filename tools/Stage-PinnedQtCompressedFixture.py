"""Stage one hash-pinned Qt test MP4 for a hosted media acceptance check.

The source archive is downloaded only when --archive is omitted. No other
archive member is extracted or added to a release package.
"""

from __future__ import annotations

import argparse
import hashlib
import json
from pathlib import Path
import tarfile
from urllib.request import urlopen


URL = ("https://download.qt.io/official_releases/qt/6.11/6.11.2/submodules/"
       "qtmultimedia-everywhere-src-6.11.2.tar.xz")
ARCHIVE_NAME = "qtmultimedia-everywhere-src-6.11.2.tar.xz"
ARCHIVE_BYTES = 10_255_120
ARCHIVE_SHA256 = "967B5E02EC6B793CDB360622CD6E703132836AF983208D678DAE4B50F109CD9F"
MEMBER = ("qtmultimedia-everywhere-src-6.11.2/tests/auto/integration/"
          "qmediaplayerbackend/testdata/3colors_with_sound_1s.mp4")
MP4_SHA256 = "0593922D7CBFC91FC773940D00FFE32A060A83C8A1986B7BEDEF5EB3A74C5F39"
MAX_MP4_BYTES = 1_000_000


def file_sha(path: Path) -> str:
    with path.open("rb") as stream:
        return hashlib.file_digest(stream, "sha256").hexdigest().upper()


def download_archive(path: Path) -> None:
    if path.exists() or path.is_symlink():
        raise ValueError("Refusing an existing Qt source download path")
    total = 0
    with urlopen(URL, timeout=180) as source, path.open("xb") as target:
        while block := source.read(1024 * 1024):
            total += len(block)
            if total > ARCHIVE_BYTES:
                raise ValueError("Qt source download exceeded its pinned size")
            target.write(block)
    if total != ARCHIVE_BYTES:
        raise ValueError("Qt source download differs from its pinned size")


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--archive", type=Path)
    parser.add_argument("--output", required=True, type=Path)
    parser.add_argument("--report", required=True, type=Path)
    args = parser.parse_args()
    output = args.output.absolute()
    report = args.report.absolute()
    if not output.parent.is_dir() or not report.parent.is_dir():
        raise ValueError("Fixture and report parents must already exist")
    if output.exists() or output.is_symlink() or report.exists() or report.is_symlink():
        raise ValueError("Fixture and report paths must be fresh")
    if output == report:
        raise ValueError("Fixture and report must use different paths")
    archive = args.archive.absolute() if args.archive else output.parent / ARCHIVE_NAME
    if args.archive is None:
        download_archive(archive)
    if archive.stat().st_size != ARCHIVE_BYTES or file_sha(archive) != ARCHIVE_SHA256:
        raise ValueError("Qt Multimedia source archive differs from the published pin")

    with tarfile.open(archive, mode="r:xz") as bundle:
        matches = [item for item in bundle if item.name == MEMBER]
        if len(matches) != 1 or not matches[0].isfile() or not (0 < matches[0].size <= MAX_MP4_BYTES):
            raise ValueError("The pinned Qt compressed-video fixture is absent or unsafe")
        source = bundle.extractfile(matches[0])
        if source is None:
            raise ValueError("Qt fixture could not be read")
        with source:
            data = source.read(MAX_MP4_BYTES + 1)
    if len(data) != matches[0].size or hashlib.sha256(data).hexdigest().upper() != MP4_SHA256:
        raise ValueError("The Qt compressed-video fixture differs from its pin")
    output.write_bytes(data)
    record = {"status": "staged-test-fixture-only", "sourceArchiveSha256": ARCHIVE_SHA256,
              "member": MEMBER, "bytes": len(data), "sha256": MP4_SHA256,
              "uploaded": False, "packaged": False}
    report.write_text(json.dumps(record, indent=2) + "\n", encoding="utf-8")
    print(json.dumps({"status": record["status"], "bytes": record["bytes"],
                      "sha256": MP4_SHA256}))


if __name__ == "__main__":
    main()
