#!/usr/bin/env python3
"""USB images that install up to three pets on a Pocket Terminal.

A Pocket Terminal keeps its pets in its three-pet store. Over USB, each pack
is written straight into its slot and the inventory journal names the slots
ready. This tool writes that journal image and a flash plan for the pet
regions; it never opens a port.

On its first boot the firmware hashes each slot against the inventory and shows
the first pet from flash. The records here mirror pet_slot_inventory.c (PSI3
v2), pet_replace_journal.c (PRS2) and pet_journal.c (PJR1), so the firmware's
own store opens the output.
"""
import argparse
import hashlib
import json
from pathlib import Path
import struct
import sys
import zlib

JOURNAL_OFFSET = 0x730000
JOURNAL_BYTES = 0x10000
SECTOR_BYTES = 0x1000
STORE_OFFSET = 0x740000
SLOT_PACK_BYTES = 0x2DD000
SLOT_BYTES = SLOT_PACK_BYTES + 0x2000
SLOT_COUNT = 3
# FP_VALIDATOR_REVISION in frame_player.h: the validator each slot's record names.
# A mismatch is safe: the firmware validates every frame of the pack once more
# and records its own revision.
VALIDATOR_REVISION = 3
PACK_MAGIC = b"AIPFRAME"


def text(value, size):
    raw = value.encode("ascii")
    if len(raw) >= size:
        raise ValueError(f"{value!r} does not fit {size} bytes")
    return raw + bytes(size - len(raw))


def uuid_from_sha(sha):
    return f"{sha[0:8]}-{sha[8:12]}-{sha[12:16]}-{sha[16:20]}-{sha[20:32]}"


def check_pack(path):
    data = Path(path).read_bytes()
    if data[:8] != PACK_MAGIC:
        raise ValueError(f"{path}: not an .aipetframes pack")
    version, = struct.unpack_from("<H", data, 8)
    length, = struct.unpack_from("<I", data, 16)
    flags, = struct.unpack_from("<H", data, 28)
    if version != 2 or length != len(data):
        raise ValueError(f"{path}: expected a complete 240 px (format 2) pack")
    if not flags & 1:
        raise ValueError(f"{path}: pack is not approved; the firmware only shows approved packs")
    if flags & 0x40:  # FP_FEATURE_MOVE
        raise ValueError(f"{path}: a move pack is not a pet; it plays beside its pet's core pack")
    if len(data) > SLOT_PACK_BYTES:
        raise ValueError(f"{path}: {len(data)} bytes exceed a slot's {SLOT_PACK_BYTES}")
    sha = hashlib.sha256(data).hexdigest()
    return {"path": str(path), "bytes": len(data), "sha256": sha, "buildId": uuid_from_sha(sha)}


def empty_machine():
    """PRS2 record of an EMPTY single-slot machine: no pet bound yet."""
    record = b"PRS2" + struct.pack("<III", SLOT_PACK_BYTES, 0, 0)
    record += bytes(2 * (37 + 65 + 4))                  # active and target packs
    record += bytes(3 * 37 + 2 * 81 + 37 + 81 + 65)     # ids, revisions, relationship, config, prefix
    assert len(record) == 684
    return record


def inventory(packs, active):
    record = b"PSI3" + struct.pack("<HBB", 2, SLOT_COUNT, active)
    record += struct.pack("<I", 1) + bytes([0xFF, 0xFF, 0, 0])   # selection revision; no target, none bound
    for slot in range(SLOT_COUNT):
        pack = packs[slot] if slot < len(packs) else None
        if pack:
            record += struct.pack("<BBHII", 2, 0, VALIDATOR_REVISION, pack["bytes"], 1 if slot == active else 0)
            record += text(pack["buildId"], 37) + text(pack["sha256"], 65)
        else:
            record += struct.pack("<BBHII", 0, 0, 0, 0, 0) + bytes(37 + 65)
    record += empty_machine()
    assert len(record) == 1042
    return record


def journal_image(payload):
    """Two committed copies (generations 1 and 2), as a new firmware journal starts."""
    image = bytearray(b"\xff" * JOURNAL_BYTES)
    for generation in (1, 2):
        sector = bytearray(b"\xff" * SECTOR_BYTES)
        sector[0:20] = b"PJR1" + struct.pack("<IQI", 1, generation, len(payload))
        sector[20:20 + len(payload)] = payload
        sector[-4:] = struct.pack("<I", zlib.crc32(bytes(sector[:-4])) & 0xFFFFFFFF)
        image[(generation - 1) * SECTOR_BYTES:generation * SECTOR_BYTES] = sector
    return bytes(image)


def main(argv=None):
    parser = argparse.ArgumentParser(prog="pocket_install_images.py", description=__doc__.splitlines()[0])
    parser.add_argument("--pack", action="append", required=True, help="pack for the next slot (1 to 3, in order)")
    parser.add_argument("--active", type=int, default=0, help="slot shown first")
    parser.add_argument("--output", required=True, help="new directory for the journal and flash plan")
    args = parser.parse_args(argv)
    if not 1 <= len(args.pack) <= SLOT_COUNT or not 0 <= args.active < len(args.pack):
        parser.error("give one to three packs and an active slot among them")
    output = Path(args.output)
    if output.exists():
        parser.error(f"{output} already exists")
    packs = [check_pack(path) for path in args.pack]
    journal = journal_image(inventory(packs, args.active))
    output.mkdir(parents=True)
    (output / "pet_journal.bin").write_bytes(journal)
    writes = [{"offset": hex(JOURNAL_OFFSET), "file": str((output / "pet_journal.bin").resolve()),
               "bytes": len(journal), "sha256": hashlib.sha256(journal).hexdigest()}]
    for slot, pack in enumerate(packs):
        writes.append({"offset": hex(STORE_OFFSET + slot * SLOT_BYTES), "file": str(Path(pack["path"]).resolve()),
                       "bytes": pack["bytes"], "sha256": pack["sha256"], "slot": slot, "buildId": pack["buildId"]})
    plan = {"layout": "three-pet-3p5m-v3", "activeSlot": args.active, "writes": writes,
            "esptoolArgs": " ".join(f"{w['offset']} {w['file']}" for w in writes)}
    (output / "pet-flash-plan.json").write_text(json.dumps(plan, indent=2) + "\n")
    print(plan["esptoolArgs"])
    return 0


if __name__ == "__main__":
    sys.exit(main())
