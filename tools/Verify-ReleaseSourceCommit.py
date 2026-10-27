"""Bind a reviewed source manifest to an exact, clean Git commit.

This is a release-gate tool, not an authorization to publish. Run it only
inside an approved V1 validation scope, before creating build output.
"""

import argparse
import hashlib
import json
import os
from pathlib import Path
import stat
import subprocess
import sys


def sha256(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest().upper()


def git(repo: Path, *args: str) -> bytes:
    # A local replace ref can make Git read another commit's tree while HEAD
    # still prints the reviewed object ID. Bind the manifest to the original
    # object graph, even when the caller's Git environment enables replaces.
    # Ambient GIT_DIR, GIT_WORK_TREE, GIT_INDEX_FILE and configuration overrides
    # must not redirect any read away from the explicit repository argument.
    # A repository-local fsmonitor hook must not run during this read-only gate.
    env = {name: value for name, value in os.environ.items()
           if not name.upper().startswith("GIT_")}
    env.update(GIT_OPTIONAL_LOCKS="0", GIT_NO_REPLACE_OBJECTS="1",
               GIT_CONFIG_NOSYSTEM="1", GIT_CONFIG_GLOBAL=os.devnull)
    result = subprocess.run(
        ["git", "-c", "core.fsmonitor=false", "-C", str(repo), *args],
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        env=env,
        timeout=60,
        check=False,
    )
    if result.returncode:
        message = result.stderr.decode("utf-8", "replace").strip()
        raise ValueError(f"Git {args[0]} failed: {message}")
    return result.stdout


def reject_reparse_ancestors(path: Path) -> None:
    current = path.absolute()
    while True:
        if current.exists() or current.is_symlink():
            info = current.lstat()
            flags = getattr(info, "st_file_attributes", 0)
            if stat.S_ISLNK(info.st_mode) or flags & getattr(stat, "FILE_ATTRIBUTE_REPARSE_POINT", 0):
                raise ValueError(f"Reparse-backed source or evidence path: {current}")
        if current.parent == current:
            break
        current = current.parent


def validate_relative_path(value: str) -> str:
    if not value or "\\" in value or ":" in value or "\x00" in value:
        raise ValueError("Manifest contains an unsafe path")
    parts = value.split("/")
    if any(part in ("", ".", "..") for part in parts) or parts[0] == ".git":
        raise ValueError("Manifest contains an unsafe path")
    return value


def git_tree(repo: Path, commit: str) -> dict[str, str]:
    entries: dict[str, str] = {}
    for record in git(repo, "ls-tree", "-r", "-z", "--full-tree", commit).split(b"\x00"):
        if not record:
            continue
        metadata, sep, raw_path = record.partition(b"\t")
        if not sep:
            raise ValueError("Malformed Git tree entry")
        fields = metadata.split(b" ")
        if len(fields) != 3 or fields[0] not in (b"100644", b"100755") or fields[1] != b"blob":
            raise ValueError("Git tree contains a symlink, submodule or unsupported entry")
        path = validate_relative_path(raw_path.decode("utf-8", "strict"))
        if path in entries:
            raise ValueError("Git tree contains a duplicate path")
        entries[path] = fields[2].decode("ascii", "strict")
    return entries


def worktree_paths(repo: Path) -> set[str]:
    actual: set[str] = set()
    def refuse_walk_error(error: OSError) -> None:
        raise ValueError(f"Could not inspect the complete source tree: {error}") from error

    for folder, dirs, files in os.walk(repo, topdown=True, followlinks=False,
                                       onerror=refuse_walk_error):
        base = Path(folder)
        for name in list(dirs):
            if base == repo and name == ".git":
                dirs.remove(name)
                continue
            reject_reparse_ancestors(base / name)
        for name in files:
            path = base / name
            reject_reparse_ancestors(path)
            actual.add(validate_relative_path(path.relative_to(repo).as_posix()))
    return actual


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--repository", type=Path, required=True)
    parser.add_argument("--manifest", type=Path, required=True)
    parser.add_argument("--manifest-sha256", required=True)
    parser.add_argument("--commit", required=True)
    parser.add_argument("--report", type=Path, required=True)
    args = parser.parse_args()

    expected_manifest_hash = args.manifest_sha256.upper()
    expected_commit = args.commit.lower()
    if len(expected_manifest_hash) != 64 or any(c not in "0123456789ABCDEF" for c in expected_manifest_hash):
        raise ValueError("An exact SHA-256 manifest hash is required")
    if len(expected_commit) not in (40, 64) or any(c not in "0123456789abcdef" for c in expected_commit):
        raise ValueError("A full Git commit ID is required")

    for path in (args.repository, args.manifest, args.report.parent):
        reject_reparse_ancestors(path)
    repo = args.repository.resolve(strict=True)
    if not repo.is_dir() or not args.report.parent.is_dir():
        raise ValueError("Repository and report parent must be existing directories")
    if args.report.exists() or args.report.is_symlink():
        raise ValueError("The report already exists; refusing to replace evidence")
    if args.report.resolve().is_relative_to(repo):
        raise ValueError("Keep release evidence outside the source checkout")

    manifest_bytes = args.manifest.read_bytes()
    if sha256(manifest_bytes) != expected_manifest_hash:
        raise ValueError("Reviewed source manifest SHA-256 differs")
    manifest = json.loads(manifest_bytes)
    if manifest.get("privatePatternFlags") != [] or not isinstance(manifest.get("files"), list):
        raise ValueError("Manifest has private flags or no file list")

    expected: dict[str, tuple[int, str]] = {}
    folded: set[str] = set()
    for entry in manifest["files"]:
        path = validate_relative_path(entry["path"])
        if path.casefold() in folded:
            raise ValueError("Manifest contains case-equivalent duplicate paths")
        folded.add(path.casefold())
        size = entry["bytes"]
        digest = entry["sha256"].upper()
        if not isinstance(size, int) or size < 0 or len(digest) != 64 or any(c not in "0123456789ABCDEF" for c in digest):
            raise ValueError("Manifest contains an invalid file record")
        expected[path] = (size, digest)
    if manifest.get("fileCount") != len(expected):
        raise ValueError("Manifest file count does not match its records")

    top = Path(os.fsdecode(git(repo, "rev-parse", "--show-toplevel").strip())).resolve(strict=True)
    if top != repo:
        raise ValueError("Repository argument is not the Git root")
    if not (repo / ".git").is_dir():
        raise ValueError("Expected an ordinary local Git directory")
    reject_reparse_ancestors(repo / ".git")
    head = git(repo, "rev-parse", "HEAD").decode("ascii", "strict").strip().lower()
    if head != expected_commit:
        raise ValueError("Git HEAD differs from the approved commit")
    if git(repo, "status", "--porcelain=v1", "-z", "--untracked-files=all"):
        raise ValueError("Git working tree or index is not clean")

    tree = git_tree(repo, head)
    if set(tree) != set(expected):
        raise ValueError("Git tracked paths differ from the reviewed manifest")
    if worktree_paths(repo) != set(expected):
        raise ValueError("Working source files differ from the reviewed manifest")
    for path, (size, digest) in expected.items():
        source = repo.joinpath(*path.split("/"))
        reject_reparse_ancestors(source)
        content = source.read_bytes()
        if len(content) != size or sha256(content) != digest:
            raise ValueError(f"Working source differs from the reviewed manifest: {path}")
        blob = git(repo, "cat-file", "blob", tree[path])
        if len(blob) != size or sha256(blob) != digest:
            raise ValueError(f"Commit blob differs from the reviewed manifest: {path}")
    if git(repo, "status", "--porcelain=v1", "-z", "--untracked-files=all"):
        raise ValueError("Git working tree changed during verification")

    report = {
        "status": "matched",
        "commit": head,
        "manifestSha256": expected_manifest_hash,
        "fileCount": len(expected),
        "trackedPathCount": len(tree),
        "publicationAuthorized": False,
    }
    with args.report.open("x", encoding="utf-8", newline="\n") as output:
        json.dump(report, output, indent=2)
        output.write("\n")
    print(json.dumps(report, indent=2))


if __name__ == "__main__":
    try:
        main()
    except (OSError, ValueError, subprocess.TimeoutExpired, UnicodeError, KeyError, TypeError) as error:
        print(f"Release source binding failed: {error}", file=sys.stderr)
        sys.exit(1)
