"""Check exact local package inputs and private byte cues before any upload.

The report records cue indexes and counts only. Keep private cue values in a
local invocation or private file, never in a published workflow or report.
"""

from __future__ import annotations

import argparse
import hashlib
import json
from pathlib import Path
import stat
import sys


def digest(path: Path) -> str:
    return hashlib.sha256(path.read_bytes()).hexdigest().upper()


def ordinary_file(path: Path) -> bool:
    try:
        info = path.lstat()
    except FileNotFoundError:
        return False
    return path.is_file() and not path.is_symlink() and not (
        getattr(info, "st_file_attributes", 0)
        & getattr(stat, "FILE_ATTRIBUTE_REPARSE_POINT", 0)
    )


def within(child: Path, parent: Path) -> bool:
    try:
        child.resolve(strict=True).relative_to(parent.resolve(strict=True))
        return True
    except (ValueError, FileNotFoundError):
        return False


def ordinary_directory_chain(path: Path) -> bool:
    current = path.absolute()
    while True:
        try:
            info = current.lstat()
        except FileNotFoundError:
            return False
        if not current.is_dir() or current.is_symlink() or (
            getattr(info, "st_file_attributes", 0)
            & getattr(stat, "FILE_ATTRIBUTE_REPARSE_POINT", 0)
        ):
            return False
        if current.parent == current:
            return True
        current = current.parent


def parse_asset(value: str) -> tuple[str, str]:
    name, separator, sha256 = value.rpartition("=")
    if not separator or not name or len(sha256) != 64:
        raise ValueError("Each asset must be NAME=64_HEX_SHA256")
    if name in {".", ".."} or "/" in name or "\\" in name or ":" in name:
        raise ValueError("An asset name must be one safe filename")
    int(sha256, 16)
    return name, sha256.upper()


def cue_matches(path: Path, cues: list[str]) -> dict[str, int]:
    content = path.read_bytes().lower()
    matches: dict[str, int] = {}
    for index, cue in enumerate(cues, start=1):
        if not cue or len(cue) > 200:
            raise ValueError("Cue values must be 1 to 200 characters")
        for encoding in ("utf-8", "utf-16le"):
            needle = cue.encode(encoding).lower()
            count = content.count(needle)
            if count:
                matches[f"cue-{index}/{encoding}"] = count
    return matches


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--payload-root", type=Path, required=True)
    parser.add_argument("--payload-manifest", type=Path, required=True)
    parser.add_argument("--manifest-sha256", required=True)
    parser.add_argument("--artifact-dir", type=Path, required=True)
    parser.add_argument("--asset", action="append", required=True,
                        help="Exact allowed NAME=SHA256; may be repeated")
    parser.add_argument("--forbidden-cue", action="append", default=[],
                        help="Private literal to reject; never printed")
    parser.add_argument("--report", type=Path, required=True)
    args = parser.parse_args()

    if args.report.exists() or args.report.is_symlink():
        raise ValueError("Refusing to overwrite a privacy report")
    if not ordinary_directory_chain(args.payload_root) or not ordinary_directory_chain(args.artifact_dir):
        raise ValueError("A release input directory crosses a link or reparse point")
    report_parent = args.report.parent.resolve(strict=True)
    if report_parent == args.artifact_dir.resolve(strict=True) or within(report_parent, args.artifact_dir) or (
        report_parent == args.payload_root.resolve(strict=True)
        or within(report_parent, args.payload_root)
    ):
        raise ValueError("Privacy evidence must stay outside payload and upload folders")
    if len(args.manifest_sha256) != 64:
        raise ValueError("Expected a 64-character manifest SHA-256")
    int(args.manifest_sha256, 16)
    if not ordinary_file(args.payload_manifest) or digest(args.payload_manifest) != args.manifest_sha256.upper():
        raise ValueError("The pinned payload manifest is missing or changed")
    contract = json.loads(args.payload_manifest.read_text(encoding="utf-8"))
    entries = contract.get("files")
    if not isinstance(entries, list) or len(entries) != contract.get("fileCount"):
        raise ValueError("Payload manifest count differs")
    expected_payload: dict[str, dict] = {}
    for entry in entries:
        name = entry.get("path")
        if not isinstance(name, str) or name.startswith("/") or "\\" in name or ":" in name or any(
            part in {"", ".", ".."} for part in name.split("/")
        ) or name in expected_payload:
            raise ValueError("Payload manifest contains unsafe or duplicate paths")
        expected_payload[name] = entry
    for directory in args.payload_root.rglob("*"):
        if directory.is_dir() and not ordinary_directory_chain(directory):
            raise ValueError("Staged payload contains a linked or reparse-backed directory")
    actual_payload = {
        path.relative_to(args.payload_root).as_posix(): path
        for path in args.payload_root.rglob("*") if path.is_file() or path.is_symlink()
    }
    if set(actual_payload) != set(expected_payload):
        raise ValueError("Staged payload differs from the pinned path set")
    for name, path in actual_payload.items():
        entry = expected_payload[name]
        if not within(path, args.payload_root) or not ordinary_file(path):
            raise ValueError("A staged payload file is linked or reparse-backed")
        if path.stat().st_size != entry.get("bytes") or digest(path) != str(entry.get("sha256", "")).upper():
            raise ValueError("A staged payload file differs from its pinned size/hash")

    assets = dict(parse_asset(raw) for raw in args.asset)
    if len(assets) != len(args.asset):
        raise ValueError("Duplicate asset name")
    artifact_entries = list(args.artifact_dir.iterdir())
    if any(not ordinary_file(path) for path in artifact_entries):
        raise ValueError("Artifact folder contains a directory or linked item")
    actual_assets = {path.name: path for path in artifact_entries}
    if set(actual_assets) != set(assets):
        raise ValueError("Artifact folder differs from the exact upload allowlist")
    for name, path in actual_assets.items():
        if not within(path, args.artifact_dir) or not ordinary_file(path) or digest(path) != assets[name]:
            raise ValueError("An allowed artifact is linked or differs from its SHA-256")

    findings: dict[str, dict[str, int]] = {}
    for name, path in list(actual_payload.items()) + [
        (f"artifact/{name}", path) for name, path in actual_assets.items()
    ]:
        matches = cue_matches(path, args.forbidden_cue)
        if matches:
            findings[name] = matches
    report = {
        "status": "blocked" if findings else "passed",
        "payloadCount": len(actual_payload),
        "artifactCount": len(actual_assets),
        "manifestSha256": args.manifest_sha256.upper(),
        "cueCount": len(args.forbidden_cue),
        "cueLabelsAndCountsOnly": findings,
        "uploaded": False,
    }
    args.report.parent.mkdir(parents=True, exist_ok=True)
    args.report.write_text(json.dumps(report, indent=2) + "\n", encoding="utf-8")
    print(json.dumps({"status": report["status"], "payloadCount": len(actual_payload),
                      "artifactCount": len(actual_assets), "flaggedFiles": len(findings),
                      "reportSha256": digest(args.report)}))
    return 1 if findings else 0


if __name__ == "__main__":
    try:
        sys.exit(main())
    except (OSError, ValueError, KeyError, TypeError, UnicodeError, json.JSONDecodeError) as error:
        print(f"Release artifact privacy check stopped: {error}", file=sys.stderr)
        sys.exit(2)
