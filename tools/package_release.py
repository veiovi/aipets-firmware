#!/usr/bin/env python3
"""Turn board builds into release files.

    python3 tools/package_release.py --version 1.0.0 --commit <sha> --out dist \\
        waveshare-esp32-s3-touch-lcd-1.85b=build/waveshare-esp32-s3-touch-lcd-1.85b ...

For each board it writes:

    aipets-firmware-<board>-<version>-full.bin  bootloader, partition table, boot
        selection, the app in both app slots and an empty pet journal, from
        address 0x0. Written to a board it replaces everything the firmware uses:
        settings, Wi-Fi and pets are gone.
    aipets-firmware-<board>-<version>-app.bin   the app alone, for both app slots
        (0x30000 and 0x3b0000) of a board that already runs this firmware.

plus manifest.json, which describes every file, and SHA256SUMS.
"""
import argparse
import hashlib
import json
import struct
from pathlib import Path

ENTRY = struct.Struct("<2sBBII16sI")
APP_DESC_MAGIC = 0xABCD5432


def partitions(table: bytes) -> dict[str, tuple[int, int]]:
    """Label -> (offset, size) of each entry of an ESP-IDF partition table."""
    found = {}
    for start in range(0, len(table), ENTRY.size):
        magic, _, _, offset, size, label, _ = ENTRY.unpack_from(table, start)
        if magic != b"\xaa\x50":
            break
        found[label.rstrip(b"\0").decode()] = (offset, size)
    return found


def app_version(app: bytes) -> str:
    if app[0] != 0xE9 or struct.unpack_from("<I", app, 32)[0] != APP_DESC_MAGIC:
        raise SystemExit("aipet_firmware.bin is not an ESP-IDF app image")
    return app[48:80].split(b"\0", 1)[0].decode()


def full_image(parts: list[tuple[int, bytes]], end: int) -> bytes:
    """Each (offset, data) in one image; the rest erased (0xFF) as on blank flash."""
    image = bytearray(b"\xff" * end)
    for offset, data in parts:
        image[offset:offset + len(data)] = data
    return bytes(image)


def package(board: str, build: Path, version: str, out: Path) -> dict:
    app = (build / "aipet_firmware.bin").read_bytes()
    table = (build / "partition_table/partition-table.bin").read_bytes()
    regions = partitions(table)
    app_a, app_b, journal = regions["ota_0"], regions["ota_1"], regions["pet_journal"]
    if len(app) > app_a[1]:
        raise SystemExit(f"{board}: the app does not fit its slot")
    parts = [
        (0x0, (build / "bootloader/bootloader.bin").read_bytes()),
        (0x8000, table),
        (regions["otadata"][0], (build / "ota_data_initial.bin").read_bytes()),
        (app_a[0], app),
        (app_b[0], app),
    ]
    files = {
        "full": (f"aipets-firmware-{board}-{version}-full.bin", full_image(parts, sum(journal)), [0x0]),
        "app": (f"aipets-firmware-{board}-{version}-app.bin", app, [app_a[0], app_b[0]]),
    }
    entry = {"board": board, "chip": "esp32s3", "app_version": app_version(app)}
    for role, (name, data, offsets) in files.items():
        (out / name).write_bytes(data)
        entry[role] = {"file": name, "bytes": len(data), "sha256": hashlib.sha256(data).hexdigest(),
                       "offsets": [hex(offset) for offset in offsets]}
    return entry


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--version", required=True, help="release version, without the v")
    parser.add_argument("--commit", required=True)
    parser.add_argument("--out", type=Path, required=True)
    parser.add_argument("builds", nargs="+", metavar="BOARD=BUILD_DIRECTORY")
    args = parser.parse_args()
    args.out.mkdir(parents=True, exist_ok=False)
    builds = []
    for pair in args.builds:
        board, _, directory = pair.partition("=")
        builds.append(package(board, Path(directory), args.version, args.out))
    manifest = {"name": "AI Pets firmware", "version": args.version, "commit": args.commit, "builds": builds}
    (args.out / "manifest.json").write_text(json.dumps(manifest, indent=2) + "\n")
    sums = [f"{hashlib.sha256(path.read_bytes()).hexdigest()}  {path.name}" for path in sorted(args.out.iterdir())]
    (args.out / "SHA256SUMS").write_text("\n".join(sums) + "\n")
    print("\n".join(sums))


if __name__ == "__main__":
    main()
