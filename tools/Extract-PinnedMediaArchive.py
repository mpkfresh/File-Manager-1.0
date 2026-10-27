"""Extract one hash-pinned media source archive into a fresh directory.

This helper is source preparation for the reviewed Windows build. Running it
is a validation/build action and requires the owner's separate approval.
"""

from __future__ import annotations

import argparse
import hashlib
import json
import tarfile
from pathlib import Path
from typing import BinaryIO


MAX_MEMBERS = 100_000
MAX_FILE_BYTES = 512 * 1024 * 1024
MAX_TOTAL_BYTES = 2 * 1024 * 1024 * 1024
WINDOWS_RESERVED = {
    "CON", "PRN", "AUX", "NUL", *(f"COM{i}" for i in range(1, 10)),
    *(f"LPT{i}" for i in range(1, 10)),
}


def sha256_file(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as source:
        for block in iter(lambda: source.read(1024 * 1024), b""):
            digest.update(block)
    return digest.hexdigest().upper()


def checked_parts(name: str, expected_root: str) -> tuple[str, ...]:
    if "\\" in name or name.startswith("/") or "\x00" in name:
        raise ValueError("Archive contains an unsafe path separator or prefix")
    parts = tuple(part for part in name.split("/") if part not in ("", "."))
    if not parts or parts[0] != expected_root:
        raise ValueError("Archive member is outside the expected source root")
    for part in parts:
        if (
            part == ".."
            or ":" in part
            or part.endswith((".", " "))
            or any(ord(character) < 32 for character in part)
            or part.split(".", 1)[0].upper() in WINDOWS_RESERVED
        ):
            raise ValueError("Archive contains an unsafe Windows path component")
    return parts


def ordinary_parent(path: Path) -> None:
    for ancestor in (path, *path.parents):
        if ancestor.is_symlink():
            raise ValueError("Extraction destination crosses a link")
        if not ancestor.exists():
            continue
        attributes = getattr(ancestor.stat(), "st_file_attributes", 0)
        if attributes & 0x400:
            raise ValueError("Extraction destination crosses a reparse point")
        if not ancestor.is_dir():
            raise ValueError("Extraction destination parent is not a directory")


def copy_and_hash(source: BinaryIO, destination: Path, expected_size: int) -> str:
    digest = hashlib.sha256()
    written = 0
    with destination.open("xb") as target:
        while block := source.read(1024 * 1024):
            written += len(block)
            if written > expected_size:
                raise ValueError("Archive member exceeded its stated size")
            target.write(block)
            digest.update(block)
    if written != expected_size:
        raise ValueError("Archive member was shorter than its stated size")
    return digest.hexdigest().upper()


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--archive", required=True, type=Path)
    parser.add_argument("--sha256", required=True)
    parser.add_argument("--destination", required=True, type=Path)
    parser.add_argument("--source-root", required=True)
    parser.add_argument("--report", required=True, type=Path)
    args = parser.parse_args()

    archive = args.archive.absolute()
    destination = args.destination.absolute()
    report = args.report.absolute()
    expected_hash = args.sha256.upper()
    if (len(args.source_root.split("/")) != 1 or
            checked_parts(args.source_root, args.source_root) != (args.source_root,)):
        raise ValueError("Expected source root must be one safe path component")
    if len(expected_hash) != 64 or any(c not in "0123456789ABCDEF" for c in expected_hash):
        raise ValueError("Expected SHA-256 must have 64 hexadecimal characters")
    if (not archive.is_file() or archive.is_symlink() or
            getattr(archive.stat(), "st_file_attributes", 0) & 0x400):
        raise ValueError("Archive must be an ordinary file")
    if destination.exists():
        raise ValueError("Destination must be absent before extraction")
    if report.exists():
        raise ValueError("Extraction report must be absent")
    if report == destination or destination in report.parents:
        raise ValueError("Extraction report must be outside the source tree")
    ordinary_parent(destination.parent)
    ordinary_parent(report.parent)
    if sha256_file(archive) != expected_hash:
        raise ValueError("Source archive SHA-256 mismatch")

    with tarfile.open(archive, mode="r:*") as bundle:
        members = bundle.getmembers()
        if not members or len(members) > MAX_MEMBERS:
            raise ValueError("Unexpected source archive member count")
        seen: set[str] = set()
        total_bytes = 0
        checked: list[tuple[tarfile.TarInfo, tuple[str, ...]]] = []
        for member in members:
            parts = checked_parts(member.name, args.source_root)
            folded = "/".join(parts).casefold()
            if folded in seen:
                raise ValueError("Duplicate or case-colliding archive path")
            seen.add(folded)
            if not (member.isfile() or member.isdir()):
                raise ValueError("Archive link or special entry is forbidden")
            if member.isfile():
                if member.size < 0 or member.size > MAX_FILE_BYTES:
                    raise ValueError("Archive member exceeds the file size limit")
                total_bytes += member.size
                if total_bytes > MAX_TOTAL_BYTES:
                    raise ValueError("Archive exceeds the total extraction limit")
            checked.append((member, parts))

        destination.mkdir(parents=True)
        file_records: list[dict[str, object]] = []
        for member, parts in checked:
            target = destination.joinpath(*parts)
            if member.isdir():
                target.mkdir(parents=True, exist_ok=True)
                continue
            target.parent.mkdir(parents=True, exist_ok=True)
            source = bundle.extractfile(member)
            if source is None:
                raise ValueError("Archive file could not be opened")
            with source:
                file_hash = copy_and_hash(source, target, member.size)
            file_records.append({
                "path": "/".join(parts),
                "bytes": member.size,
                "sha256": file_hash,
            })

    record = {
        "archiveSha256": expected_hash,
        "sourceRoot": args.source_root,
        "fileCount": len(file_records),
        "files": file_records,
    }
    report.write_text(json.dumps(record, separators=(",", ":")), encoding="utf-8")
    print(f"Extracted {len(file_records)} verified source files")


if __name__ == "__main__":
    main()
