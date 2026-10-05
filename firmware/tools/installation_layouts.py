#!/usr/bin/env python3
"""Export device-owned geometry and ESP-IDF-verified partition artifacts.

Build-machine tool only. It never opens a device, signs a release, creates boot
health evidence, or authorizes a flash write. No sibling-repository dependency.
"""
from __future__ import annotations

import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import struct
import subprocess
import sys
import tempfile

ROOT = Path(__file__).resolve().parents[2]
LAYOUT_SOURCES = (
    "firmware/main/pet_flash_layout.c",
    "firmware/main/pet_flash_layout.h",
    "firmware/tools/export_flash_layouts.c",
)
IDF_GENERATOR = "components/partition_table/gen_esp32part.py"
IDF_GENERATOR_SHA256 = "02f6c1c5f012393d47987ee64bc50ba319a2eda48cbcec754d8cfaf780cce529"
IDF_COMMIT = "2c211b236707889e8400c4dc5644dd5c4ee071e0"


class LayoutError(ValueError):
    pass


def sha256(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()


def command(argv: list[str], *, cwd: Path | None = None) -> bytes:
    try:
        return subprocess.run(argv, cwd=cwd, check=True, capture_output=True, timeout=60).stdout
    except (OSError, subprocess.SubprocessError) as error:
        # No arbitrary tool stdout/stderr is copied into a release manifest.
        raise LayoutError(f"Command failed: {Path(argv[0]).name}") from error


def source_identity(root: Path) -> dict:
    commit = command(["git", "rev-parse", "HEAD"], cwd=root).decode().strip()
    if not re.fullmatch(r"[0-9a-f]{40}", commit):
        raise LayoutError("Invalid device source commit")
    return {
        "commit": commit,
        "dirty": bool(command(["git", "status", "--porcelain", "--untracked-files=all"], cwd=root)),
        "files": [{"path": name, "sha256": sha256((root / name).read_bytes())}
                  for name in (*LAYOUT_SOURCES, "firmware/tools/installation_layouts.py")],
    }


def compile_exporter(work: Path, root: Path = ROOT, cc: str = "cc") -> Path:
    binary = work / ("layout-export.exe" if os.name == "nt" else "layout-export")
    command([cc, "-std=c11", "-Wall", "-Wextra", "-Werror", "-I", str(root / "firmware/main"),
             str(root / LAYOUT_SOURCES[0]), str(root / LAYOUT_SOURCES[2]), "-o", str(binary)])
    return binary


def read_geometry(binary: Path, app_bytes: int | None = None) -> dict:
    if app_bytes is not None and (type(app_bytes) is not int or not 0 < app_bytes <= 0xffffffff):
        raise LayoutError("Application length must be a positive uint32")
    args = [str(binary)] + ([] if app_bytes is None else [str(app_bytes)])
    try:
        value = json.loads(command(args))
    except (ValueError, UnicodeError) as error:
        raise LayoutError("Invalid native geometry export") from error
    # These are wire/ESP-IDF format invariants, not independently chosen layouts.
    if (value["flashBytes"] != 0x1000000 or value["sectorBytes"] != 0x1000 or
            value["partitionTableOffset"] != 0x8000 or value["partitionTableBytes"] != 0xc00 or
            value["bootloaderOffset"] != 0 or value["bootloaderRegionBytes"] != 0x8000 or
            value["manifestReservationBytes"] != 0x2000 or len(value["layouts"]) != 7):
        raise LayoutError("Unsupported geometry export version")
    return value


def partition_csv(layout: dict) -> bytes:
    rows = ["# Generated from pet_flash_layout.c; do not maintain a second layout.",
            "# Name, Type, SubType, Offset, Size, Flags"]
    for region in layout["regions"]:
        if not re.fullmatch(r"[a-z][a-z0-9_]{0,14}", region["name"]) or region["flags"] != 0:
            raise LayoutError("Unsupported partition name/flags")
        rows.append(f'{region["name"]},0x{region["type"]:02x},0x{region["subtype"]:02x},'
                    f'0x{region["offset"]:x},0x{region["bytes"]:x},')
    return ("\n".join(rows) + "\n").encode("ascii")


def encode_partition(layout: dict, geometry: dict) -> bytes:
    """Independent serializer cross-checked byte-for-byte against pinned IDF."""
    entries = bytearray()
    cursor = geometry["partitionTableOffset"] + geometry["sectorBytes"]
    names = set()
    for r in layout["regions"]:
        if r["name"] in names or r["flags"] or r["offset"] < cursor or r["bytes"] <= 0:
            raise LayoutError("Overlapping/duplicate/unsupported region")
        if r["offset"] % (0x10000 if r["type"] == 0 else geometry["sectorBytes"]):
            raise LayoutError("Unaligned region")
        cursor = r["offset"] + r["bytes"]
        if cursor > geometry["flashBytes"] or r["bytes"] % geometry["sectorBytes"]:
            raise LayoutError("Region exceeds flash or erase alignment")
        name = r["name"].encode("ascii")
        if len(name) > 15 or b"\0" in name:
            raise LayoutError("Invalid partition label")
        names.add(r["name"])
        entries.extend(struct.pack("<HBBII16sI", 0x50aa, r["type"], r["subtype"],
                                   r["offset"], r["bytes"], name, r["flags"]))
    # MD5 is the IDF table-format checksum, not release authentication.
    entries.extend(b"\xeb\xeb" + b"\xff" * 14 + hashlib.md5(entries, usedforsecurity=False).digest())
    if len(entries) > geometry["partitionTableBytes"]:
        raise LayoutError("Partition table too large")
    return bytes(entries).ljust(geometry["partitionTableBytes"], b"\xff")


def pinned_generator(idf: Path, work: Path) -> Path:
    data = (idf / IDF_GENERATOR).read_bytes()
    if sha256(data) != IDF_GENERATOR_SHA256:
        raise LayoutError("Partition generator does not match ESP-IDF v5.5.3 pin")
    # Execute precisely the hashed bytes, not a mutable external file after the check.
    copied = work / "gen_esp32part.py"
    copied.write_bytes(data)
    return copied


def create_export(idf: Path, work: Path, *, root: Path = ROOT, cc: str = "cc",
                  app_bytes: int | None = None) -> tuple[dict, dict[str, bytes]]:
    before = source_identity(root)
    generator = pinned_generator(idf, work)
    geometry = read_geometry(compile_exporter(work, root, cc), app_bytes)
    files: dict[str, bytes] = {}
    layouts = []
    for layout in geometry["layouts"]:
        leaf = layout["id"]
        if not re.fullmatch(r"[a-z][a-z0-9-]{0,40}", leaf):
            raise LayoutError("Invalid layout identifier")
        csv = partition_csv(layout)
        expected = encode_partition(layout, geometry)
        csv_path = work / f"{leaf}.csv"
        csv_path.write_bytes(csv)
        actual = command([sys.executable, str(generator), "--quiet", "--flash-size", "16MB",
                          "--offset", hex(geometry["partitionTableOffset"]), str(csv_path)])
        if actual != expected:
            raise LayoutError(f"ESP-IDF partition byte mismatch: {leaf}")
        files[f"{leaf}.csv"] = csv
        files[f"{leaf}.bin"] = actual
        layouts.append({**layout, "partitionTableSha256": sha256(actual),
                        "partitionTableFile": f"{leaf}.bin", "partitionCsvFile": f"{leaf}.csv"})
    after = source_identity(root)
    if before != after:
        raise LayoutError("Source changed during geometry export")
    result = {
        "kind": "device-flash-layout-export", "version": 1,
        "status": "geometry-only-not-installation-authority",
        "source": before,
        "oracle": {"name": "ESP-IDF", "version": "5.5.3", "commit": IDF_COMMIT,
                   "path": IDF_GENERATOR, "sha256": IDF_GENERATOR_SHA256},
        **{k: v for k, v in geometry.items() if k != "layouts"}, "layouts": layouts,
    }
    return result, files


def write_export(output: Path, manifest: dict, files: dict[str, bytes]) -> None:
    # Never replace an existing bundle, baseline or input directory. Manifest is
    # the final completion marker; a partial directory has no usable manifest.
    output.mkdir()
    for name, data in files.items():
        with (output / name).open("xb") as stream:
            stream.write(data)
    with (output / "layouts.json").open("xb") as stream:
        stream.write((json.dumps(manifest, indent=2) + "\n").encode("utf-8"))


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--idf-path", type=Path, default=os.environ.get("IDF_PATH"))
    parser.add_argument("--output", type=Path, required=True, help="New directory; parent must exist")
    parser.add_argument("--cc", default=os.environ.get("CC", "cc"), help="One compiler executable, not shell flags")
    parser.add_argument("--app-bytes", type=int, help="Optional measured image length; native smallest-slot selector")
    args = parser.parse_args()
    try:
        if not args.idf_path:
            raise LayoutError("Set IDF_PATH or pass --idf-path")
        if args.output.exists() or args.output.is_symlink():
            raise LayoutError("Output already exists; select a new directory")
        with tempfile.TemporaryDirectory(prefix="aipet-layout-export-") as scratch:
            manifest, files = create_export(args.idf_path, Path(scratch), cc=args.cc, app_bytes=args.app_bytes)
        write_export(args.output, manifest, files)
    except (OSError, LayoutError) as error:
        print(f"Layout export failed: {error}", file=sys.stderr)
        return 1
    print(json.dumps({"manifest": str(args.output / "layouts.json"),
                      "layouts": len(manifest["layouts"]), "selection": manifest["selection"],
                      "status": manifest["status"]}))
    return 0


if __name__ == "__main__":
    sys.exit(main())
