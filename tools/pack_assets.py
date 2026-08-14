import argparse
import hashlib
import os
import struct
import zlib
from pathlib import Path

MAGIC = b"IRXPK1\0"
EXTENSIONS = ["na1", "na2", "na3", "na4", "na5", "na6", "na7", "na8", "na9", "na10", "naupk"]


def files(root: Path):
    for p in sorted(root.rglob("*")):
        if p.is_file():
            yield p


def pack(src: Path, out_dir: Path, chunk_mb: int):
    out_dir.mkdir(parents=True, exist_ok=True)
    entries = []
    for p in files(src):
        rel = p.relative_to(src).as_posix().encode("utf-8")
        raw = p.read_bytes()
        comp = zlib.compress(raw, 9)
        digest = hashlib.sha256(raw).digest()
        entries.append(struct.pack("<HII", len(rel), len(raw), len(comp)) + rel + digest + comp)
    blob = MAGIC + struct.pack("<I", len(entries)) + b"".join(entries)
    size = max(1, chunk_mb) * 1024 * 1024
    total = (len(blob) + size - 1) // size
    if total > len(EXTENSIONS):
        raise SystemExit(f"archive needs {total} chunks, max supported is {len(EXTENSIONS)}")
    for i in range(total):
        ext = EXTENSIONS[i]
        part = blob[i * size:(i + 1) * size]
        (out_dir / f"assets.{ext}").write_bytes(part)
    manifest = hashlib.sha256(blob).hexdigest()
    (out_dir / "assets.sha256").write_text(manifest + "\n", encoding="ascii")
    print(f"packed {len(entries)} files into {total} chunk(s), {len(blob)} bytes")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("source")
    ap.add_argument("output")
    ap.add_argument("--chunk-mb", type=int, default=48)
    args = ap.parse_args()
    pack(Path(args.source), Path(args.output), args.chunk_mb)


if __name__ == "__main__":
    main()
