"""Bind CMake's consumed FetchContent trees to their pinned Git commits.

Run only within an owner-approved V1 validation. This reads the complete two
dependency source trees and Git objects; it does not fetch or modify them.
"""

import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import stat
import subprocess
import sys


PINNED_COMMITS = {
    "zlib_ng": "12731092979c6d07f42da27da673a9f6c7b13586",
    "minizip_ng": "7b2387161c542fa9f427352dcdef76097d0d692b",
}
RECORD_KEYS = {
    "zlib_ng_source", "zlib_ng_commit",
    "minizip_ng_source", "minizip_ng_commit",
}


def sha256(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest().upper()


def reject_reparse_ancestors(path: Path) -> None:
    current = path.absolute()
    while True:
        if current.exists() or current.is_symlink():
            info = current.lstat()
            flags = getattr(info, "st_file_attributes", 0)
            if stat.S_ISLNK(info.st_mode) or flags & getattr(stat, "FILE_ATTRIBUTE_REPARSE_POINT", 0):
                raise ValueError(f"Reparse-backed dependency or evidence path: {current}")
        if current.parent == current:
            break
        current = current.parent


def git(repo: Path, *args: str) -> bytes:
    # Keep every Git read tied to the explicit source tree. In particular,
    # inherited GIT_DIR, GIT_WORK_TREE or GIT_INDEX_FILE must not redirect it.
    env = {name: value for name, value in os.environ.items()
           if not name.upper().startswith("GIT_")}
    env.update(GIT_OPTIONAL_LOCKS="0", GIT_NO_REPLACE_OBJECTS="1",
               GIT_CONFIG_NOSYSTEM="1", GIT_CONFIG_GLOBAL=os.devnull)
    result = subprocess.run(
        ["git", "-c", "core.fsmonitor=false", "-C", str(repo), *args],
        stdout=subprocess.PIPE, stderr=subprocess.PIPE, env=env, timeout=60,
        check=False,
    )
    if result.returncode:
        message = result.stderr.decode("utf-8", "replace").strip()
        raise ValueError(f"Git {args[0]} failed: {message}")
    return result.stdout


def safe_relative_path(raw: bytes) -> str:
    name = raw.decode("utf-8", "strict")
    parts = name.split("/")
    if (not name or "\\" in name or ":" in name or "\x00" in name or
            any(part in ("", ".", "..") for part in parts) or parts[0] == ".git"):
        raise ValueError("Dependency Git tree has an unsafe path")
    return name


def read_record(path: Path, expected_hash: str) -> dict[str, str]:
    reject_reparse_ancestors(path)
    if not path.is_file():
        raise ValueError("CMake dependency source record is missing")
    data = path.read_bytes()
    if sha256(data) != expected_hash:
        raise ValueError("CMake dependency source record differs from approved SHA-256")
    rows: dict[str, str] = {}
    for line in data.decode("utf-8-sig").splitlines():
        key, separator, value = line.partition("=")
        if not separator or key not in RECORD_KEYS or key in rows or not value:
            raise ValueError("CMake dependency source record has an invalid or duplicate field")
        rows[key] = value
    if set(rows) != RECORD_KEYS:
        raise ValueError("CMake dependency source record has missing fields")
    for name, expected_commit in PINNED_COMMITS.items():
        if rows[f"{name}_commit"].lower() != expected_commit:
            raise ValueError(f"CMake requested an unapproved {name} commit")
    return rows


def source_inventory(repo: Path, commit: str) -> tuple[int, str, str]:
    reject_reparse_ancestors(repo)
    if not repo.is_dir() or not (repo / ".git").is_dir():
        raise ValueError(f"Dependency source is not an ordinary Git checkout: {repo}")
    reject_reparse_ancestors(repo / ".git")
    root = Path(os.fsdecode(git(repo, "rev-parse", "--show-toplevel").strip())).resolve(strict=True)
    if root != repo:
        raise ValueError(f"Dependency path is not its Git root: {repo}")
    head = git(repo, "rev-parse", "HEAD").decode("ascii", "strict").strip().lower()
    if head != commit:
        raise ValueError(f"Dependency HEAD differs from the pinned commit: {repo}")
    if git(repo, "status", "--porcelain=v1", "-z", "--untracked-files=all"):
        raise ValueError(f"Dependency source or index is dirty: {repo}")
    tree_id = git(repo, "rev-parse", "HEAD^{tree}").decode("ascii", "strict").strip().lower()
    if not re.fullmatch(r"[0-9a-f]{40}|[0-9a-f]{64}", tree_id):
        raise ValueError("Dependency Git tree has an invalid object ID")

    blobs: dict[str, str] = {}
    for record in git(repo, "ls-tree", "-r", "-z", "--full-tree", head).split(b"\x00"):
        if not record:
            continue
        metadata, separator, raw_path = record.partition(b"\t")
        fields = metadata.split(b" ")
        if (not separator or len(fields) != 3 or fields[0] not in (b"100644", b"100755")
                or fields[1] != b"blob"):
            raise ValueError("Dependency tree contains a symlink, submodule or malformed entry")
        name = safe_relative_path(raw_path)
        if name in blobs:
            raise ValueError("Dependency tree contains a duplicate path")
        blobs[name] = fields[2].decode("ascii", "strict")

    actual: set[str] = set()
    def refuse_walk_error(error: OSError) -> None:
        raise ValueError(f"Cannot inventory complete dependency tree: {error}") from error
    for folder, dirs, files in os.walk(repo, topdown=True, followlinks=False,
                                       onerror=refuse_walk_error):
        base = Path(folder)
        for name in list(dirs):
            if base == repo and name == ".git":
                dirs.remove(name)
                continue
            reject_reparse_ancestors(base / name)
        for name in files:
            file = base / name
            reject_reparse_ancestors(file)
            actual.add(safe_relative_path(file.relative_to(repo).as_posix().encode("utf-8")))
    if actual != set(blobs):
        raise ValueError(f"Dependency source files differ from its committed tree: {repo}")

    rows = []
    for name in sorted(blobs):
        file = repo.joinpath(*name.split("/"))
        content = file.read_bytes()
        blob = git(repo, "cat-file", "blob", blobs[name])
        if content != blob:
            raise ValueError(f"Dependency file differs from its committed blob: {name}")
        rows.append((name, len(content), sha256(content)))
    if git(repo, "status", "--porcelain=v1", "-z", "--untracked-files=all"):
        raise ValueError(f"Dependency source changed during verification: {repo}")
    inventory_hash = sha256(json.dumps(rows, ensure_ascii=False,
                                       separators=(",", ":")).encode("utf-8"))
    return len(rows), tree_id, inventory_hash


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--record", type=Path, required=True)
    parser.add_argument("--record-sha256", required=True)
    parser.add_argument("--report", type=Path, required=True)
    args = parser.parse_args()
    expected_hash = args.record_sha256.upper()
    if not re.fullmatch(r"[0-9A-F]{64}", expected_hash):
        raise ValueError("An exact SHA-256 for the CMake record is required")
    if args.report.exists() or args.report.is_symlink():
        raise ValueError("Report already exists; refusing to replace evidence")
    reject_reparse_ancestors(args.report.parent)
    if not args.report.parent.is_dir():
        raise ValueError("Report parent must already exist")
    record = read_record(args.record, expected_hash)
    sources = [Path(record[f"{name}_source"]) for name in PINNED_COMMITS]
    if any(not source.is_absolute() for source in sources) or sources[0] == sources[1]:
        raise ValueError("CMake dependency paths must be distinct absolute paths")
    for source in sources:
        reject_reparse_ancestors(source)
        if args.report.resolve(strict=False).is_relative_to(source.resolve(strict=True)):
            raise ValueError("Keep origin evidence outside dependency source trees")

    results = []
    for name, source in zip(PINNED_COMMITS, sources):
        count, tree_id, inventory_hash = source_inventory(source.resolve(strict=True),
                                                          PINNED_COMMITS[name])
        results.append({"name": name, "sourcePath": str(source.resolve(strict=True)),
                        "commit": PINNED_COMMITS[name], "gitTree": tree_id,
                        "fileCount": count, "inventorySha256": inventory_hash})
    report = {"status": "matched", "recordSha256": expected_hash,
              "dependencies": results, "publicationAuthorized": False}
    with args.report.open("x", encoding="utf-8", newline="\n") as output:
        json.dump(report, output, indent=2)
        output.write("\n")
    print(json.dumps(report, indent=2))


if __name__ == "__main__":
    try:
        main()
    except (OSError, ValueError, subprocess.TimeoutExpired, UnicodeError, TypeError) as error:
        print(f"Dependency source origin check failed: {error}", file=sys.stderr)
        sys.exit(1)
