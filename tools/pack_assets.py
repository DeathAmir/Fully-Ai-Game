from __future__ import annotations

import argparse
import hashlib
import json
import os
import sys
import zipfile
from pathlib import Path


def digest(path: Path) -> str:
    hasher = hashlib.sha256()
    with path.open("rb") as stream:
        for block in iter(lambda: stream.read(1024 * 1024), b""):
            hasher.update(block)
    return hasher.hexdigest()


def extension(index: int) -> str:
    if index < 9:
        return f".na{index + 1}"
    index -= 9
    if index < 26:
        return f".na{chr(ord('a') + index)}"
    return f".na{index + 10}"


def groups(files: list[Path], root: Path, maximum: int):
    current = []
    size = 0
    for path in files:
        file_size = path.stat().st_size
        if current and size + file_size > maximum:
            yield current
            current = []
            size = 0
        current.append(path)
        size += file_size
    if current:
        yield current


def pack(source: Path, output: Path, chunk_megabytes: int):
    files = sorted(path for path in source.rglob("*") if path.is_file())
    output.mkdir(parents=True, exist_ok=True)
    maximum = max(8, chunk_megabytes) * 1024 * 1024
    manifest = {"format": "iRx-NAUPK-1", "chunks": [], "files": []}
    for chunk_index, chunk_files in enumerate(groups(files, source, maximum)):
        name = "assets" + extension(chunk_index)
        target = output / name
        with zipfile.ZipFile(target, "w", compression=zipfile.ZIP_DEFLATED, compresslevel=9, allowZip64=True) as archive:
            for path in chunk_files:
                relative = path.relative_to(source).as_posix()
                info = zipfile.ZipInfo(relative, (2026, 1, 1, 0, 0, 0))
                info.compress_type = zipfile.ZIP_DEFLATED
                info.external_attr = 0o644 << 16
                data = path.read_bytes()
                archive.writestr(info, data, compress_type=zipfile.ZIP_DEFLATED, compresslevel=9)
                manifest["files"].append({"path": relative, "size": len(data), "sha256": hashlib.sha256(data).hexdigest(), "chunk": name})
        manifest["chunks"].append({"name": name, "size": target.stat().st_size, "sha256": digest(target)})
    manifest_path = output / "assets.naupk"
    manifest_path.write_text(json.dumps(manifest, ensure_ascii=False, separators=(",", ":")), encoding="utf-8")
    return manifest_path


def verify(manifest_path: Path):
    manifest = json.loads(manifest_path.read_text(encoding="utf-8"))
    if manifest.get("format") != "iRx-NAUPK-1":
        raise ValueError("unsupported package format")
    root = manifest_path.parent
    for chunk in manifest["chunks"]:
        path = root / chunk["name"]
        if not path.is_file() or path.stat().st_size != chunk["size"] or digest(path) != chunk["sha256"]:
            raise ValueError(f"invalid chunk: {chunk['name']}")
    lookup = {item["path"]: item for item in manifest["files"]}
    for chunk in manifest["chunks"]:
        with zipfile.ZipFile(root / chunk["name"], "r") as archive:
            for info in archive.infolist():
                data = archive.read(info)
                item = lookup[info.filename]
                if len(data) != item["size"] or hashlib.sha256(data).hexdigest() != item["sha256"]:
                    raise ValueError(f"invalid asset: {info.filename}")
    return len(lookup), len(manifest["chunks"])


def main():
    parser = argparse.ArgumentParser(prog="iRx asset packer")
    subcommands = parser.add_subparsers(dest="command", required=True)
    create = subcommands.add_parser("create")
    create.add_argument("source", type=Path)
    create.add_argument("output", type=Path)
    create.add_argument("--chunk-mb", type=int, default=96)
    check = subcommands.add_parser("verify")
    check.add_argument("manifest", type=Path)
    args = parser.parse_args()
    if args.command == "create":
        manifest = pack(args.source.resolve(), args.output.resolve(), args.chunk_mb)
        files, chunks = verify(manifest)
    else:
        manifest = args.manifest.resolve()
        files, chunks = verify(manifest)
    print(f"verified {files} assets in {chunks} chunks: {manifest}")


if __name__ == "__main__":
    main()
