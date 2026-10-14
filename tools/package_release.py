#!/usr/bin/env python3
"""Turn board builds into release files.

    python3 tools/package_release.py --version 1.0.0 --commit <sha> --out dist \\
        waveshare-esp32-s3-touch-lcd-1.85b=build/waveshare-esp32-s3-touch-lcd-1.85b ...

For each board it writes:

    aipets-firmware-<board>-<version>-full.bin  bootloader, partition table and
        application, with all other partition bytes erased, from address 0x0.
        This replaces settings, Wi-Fi, linking, sounds and pets.
    aipets-firmware-<board>-<version>-app.bin   the app alone. Only use it on a
        board with the identical partition table. Offsets come from the build,
        not from an older release's instructions.

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
    previous_end = 0
    for offset, data in sorted(parts):
        if offset < previous_end or offset + len(data) > end:
            raise SystemExit("Overlapping or out-of-range firmware image regions")
        image[offset:offset + len(data)] = data
        previous_end = offset + len(data)
    return bytes(image)


def package(board: str, build: Path, version: str, out: Path) -> dict:
    app = (build / "aipet_firmware.bin").read_bytes()
    table = (build / "partition_table/partition-table.bin").read_bytes()
    regions = partitions(table)
    if "factory" in regions and not any(name in regions for name in ("ota_0", "ota_1", "otadata")):
        app_slots = [regions["factory"]]
        scheme = "factory"
    elif "factory" not in regions and all(name in regions for name in ("ota_0", "ota_1", "otadata")):
        app_slots = [regions["ota_0"], regions["ota_1"]]
        scheme = "dual-ota"
    else:
        raise SystemExit(f"{board}: unsupported application partition layout")
    if any(len(app) > size for _, size in app_slots):
        raise SystemExit(f"{board}: the app does not fit its slot")
    parts = [
        (0x0, (build / "bootloader/bootloader.bin").read_bytes()),
        (0x8000, table),
        *[(offset, app) for offset, _ in app_slots],
    ]
    if scheme == "dual-ota":
        ota_data = (build / "ota_data_initial.bin").read_bytes()
        if len(ota_data) > regions["otadata"][1]:
            raise SystemExit(f"{board}: boot selection exceeds its partition")
        parts.append((regions["otadata"][0], ota_data))
    end = max(offset + size for offset, size in regions.values())
    if end > 16 * 1024 * 1024:
        raise SystemExit(f"{board}: partition layout exceeds the supported 16 MiB region")
    files = {
        "full": (f"aipets-firmware-{board}-{version}-full.bin", full_image(parts, end), [0x0]),
        "app": (f"aipets-firmware-{board}-{version}-app.bin", app, [offset for offset, _ in app_slots]),
    }
    entry = {"board": board, "chip": "esp32s3", "app_version": app_version(app),
             "application_scheme": scheme, "partition_table_sha256": hashlib.sha256(table).hexdigest(),
             "partitions": {name: {"offset": hex(offset), "bytes": size}
                            for name, (offset, size) in regions.items()},
             "full_image_erases_owner_data": True, "app_requires_identical_partition_table": True,
             "physical_verification": "not-performed"}
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
