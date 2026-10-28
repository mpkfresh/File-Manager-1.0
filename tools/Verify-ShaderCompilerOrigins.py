"""Compare Qt's saved deployment-source list with staged shader DLL bytes.

This is local release evidence, not a redistribution-rights decision. Capture
`windeployqt --dry-run --list source` on a clean executable before using this
tool. Run only inside an owner-approved release validation scope.
"""

import argparse
import hashlib
import json
from pathlib import Path, PureWindowsPath
import stat
import sys


SHADER_DLLS = ("d3dcompiler_47.dll", "dxcompiler.dll", "dxil.dll")


def sha256(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest().upper()


def reject_reparse_ancestors(path: Path) -> None:
    current = path.absolute()
    while True:
        if current.exists() or current.is_symlink():
            info = current.lstat()
            attributes = getattr(info, "st_file_attributes", 0)
            if stat.S_ISLNK(info.st_mode) or attributes & getattr(stat, "FILE_ATTRIBUTE_REPARSE_POINT", 0):
                raise ValueError(f"Reparse-backed input or report path: {current}")
        if current.parent == current:
            break
        current = current.parent


def read_regular_file(path: Path) -> bytes:
    reject_reparse_ancestors(path)
    if not path.is_file():
        raise ValueError(f"Missing regular file: {path}")
    return path.read_bytes()


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--source-list", type=Path, required=True)
    parser.add_argument("--source-list-sha256", required=True)
    parser.add_argument("--selected-qt-prefix", type=Path, required=True)
    parser.add_argument("--deploy-tool", type=Path, required=True)
    parser.add_argument("--deploy-tool-sha256", required=True)
    parser.add_argument("--stage", type=Path, required=True)
    parser.add_argument("--report", type=Path, required=True)
    args = parser.parse_args()

    expected_list_hash = args.source_list_sha256.upper()
    expected_tool_hash = args.deploy_tool_sha256.upper()
    for label, value in (("saved source list", expected_list_hash),
                         ("deployment tool", expected_tool_hash)):
        if len(value) != 64 or any(c not in "0123456789ABCDEF" for c in value):
            raise ValueError(f"An exact SHA-256 for the {label} is required")
    if args.report.exists() or args.report.is_symlink():
        raise ValueError("Report already exists; refusing to replace evidence")
    reject_reparse_ancestors(args.report.parent)
    if not args.report.parent.is_dir():
        raise ValueError("Report parent must already exist")
    reject_reparse_ancestors(args.stage)
    if not args.stage.is_dir():
        raise ValueError("Staged payload directory is missing")
    if args.report.resolve(strict=False).is_relative_to(args.stage.resolve(strict=True)):
        raise ValueError("Keep origin evidence outside the staged payload")
    if args.source_list.resolve(strict=False).is_relative_to(args.stage.resolve(strict=True)):
        raise ValueError("Keep the Qt source listing outside the staged payload")

    reject_reparse_ancestors(args.selected_qt_prefix)
    reject_reparse_ancestors(args.deploy_tool)
    if not args.selected_qt_prefix.is_dir():
        raise ValueError("The selected Qt prefix is missing")
    expected_tool = args.selected_qt_prefix / "bin" / "windeployqt.exe"
    reject_reparse_ancestors(expected_tool)
    if args.deploy_tool.resolve(strict=True) != expected_tool.resolve(strict=True):
        raise ValueError("Deployment tool is not the selected Qt kit's windeployqt.exe")
    tool_hash = sha256(read_regular_file(args.deploy_tool))
    if tool_hash != expected_tool_hash:
        raise ValueError("Deployment tool differs from the approved SHA-256")

    listing = read_regular_file(args.source_list)
    if sha256(listing) != expected_list_hash:
        raise ValueError("Saved windeployqt source list differs from the approved hash")
    try:
        lines = listing.decode("utf-8-sig").splitlines()
    except UnicodeError as error:
        raise ValueError("Source list must be saved as UTF-8 text") from error
    matches: dict[str, list[Path]] = {name: [] for name in SHADER_DLLS}
    for raw in lines:
        name = raw.strip().strip('"')
        if not name:
            continue
        basename = PureWindowsPath(name).name.lower()
        if basename in matches:
            source = Path(name)
            if not source.is_absolute():
                raise ValueError(f"Qt listed a nonabsolute shader DLL source: {name}")
            matches[basename].append(source)

    rows = []
    for name in SHADER_DLLS:
        source_selection = "qt-listed"
        if name == "dxil.dll" and len(matches[name]) == 0:
            # Qt 6.11.2 deploys DXIL alongside DXC, but its dry-run source
            # listing can omit DXIL. Require the same source directory and
            # exact staged bytes instead of dropping the provenance check.
            if len(matches["dxcompiler.dll"]) != 1:
                raise ValueError("DXIL needs one Qt-listed DXC source")
            source = matches["dxcompiler.dll"][0].with_name("dxil.dll")
            source_selection = "adjacent-to-qt-listed-dxcompiler"
        elif len(matches[name]) != 1:
            raise ValueError(f"Expected exactly one Qt-listed source for {name}; found {len(matches[name])}")
        else:
            source = matches[name][0]
        staged = args.stage / name
        reject_reparse_ancestors(source)
        reject_reparse_ancestors(staged)
        if source.resolve(strict=True) == staged.resolve(strict=True):
            raise ValueError(f"Qt source and staged path are the same file: {name}")
        source_bytes = read_regular_file(source)
        staged_bytes = read_regular_file(staged)
        source_hash = sha256(source_bytes)
        if len(source_bytes) != len(staged_bytes) or source_hash != sha256(staged_bytes):
            raise ValueError(f"Qt source and staged bytes differ: {name}")
        rows.append({
            "name": name,
            "sourcePath": str(source),
            "stagedPath": str(staged),
            "bytes": len(source_bytes),
            "sha256": source_hash,
            "sourceSelection": source_selection,
        })

    report = {
        "status": "matched",
        "sourceListSha256": expected_list_hash,
        "selectedQtPrefix": str(args.selected_qt_prefix.resolve(strict=True)),
        "deploymentToolPath": str(args.deploy_tool.resolve(strict=True)),
        "deploymentToolSha256": tool_hash,
        "files": rows,
        "redistributionRightsEstablished": False,
    }
    with args.report.open("x", encoding="utf-8", newline="\n") as output:
        json.dump(report, output, indent=2)
        output.write("\n")
    print(json.dumps(report, indent=2))


if __name__ == "__main__":
    try:
        main()
    except (OSError, ValueError, TypeError) as error:
        print(f"Shader compiler origin check failed: {error}", file=sys.stderr)
        sys.exit(1)
