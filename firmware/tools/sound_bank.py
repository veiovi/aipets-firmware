#!/usr/bin/env python3
"""Build the sound bank (sound-bank-v1) a Pocket keeps in its pet_sound partition.

    python3 firmware/tools/sound_bank.py SOUNDS.json OUTPUT_DIRECTORY

SOUNDS.json is a list of sounds, each {"id", "kind": "one-shot" or "loop",
"rate": 12000 or 16000, "adpcm": a path, relative to the list}, with any of
"seconds", "role", "mood", "title" and "fallback". An ADPCM file is whole
256-byte IMA ADPCM blocks (firmware/main/pet_ima_adpcm.h), at most 64 KiB, the
most the Pocket copies out to play.

Writes OUTPUT_DIRECTORY/sound-bank.bin, the SBK1 file
(firmware/main/pet_sound_bank.h), and sound-bank.json, its manifest:
{"version": 1, "sha256", "bytes", "sounds": [{"id", "kind", "rate", and the
fields given of "seconds", "role", "mood", "title", "fallback"}]}, sounds in
id order as in the bank. The same list always writes the same bytes. Standard
library only.
"""
import argparse
import hashlib
import json
import re
import struct
from pathlib import Path

HEADER = struct.Struct("<4sHHII")
ENTRY = struct.Struct("<24sIIHBB")
BLOCK_BYTES = 256
SOUND_MAX_BYTES = 0x10000
# The pet_sound partition, PET_SOUND_BANK_MAX_BYTES in firmware/main/pet_session_wire.h.
BANK_MAX_BYTES = 0x200000
SOUNDS_MAX = 128
KINDS = {"one-shot": 0, "loop": 1}
RATES = (12000, 16000)
ID = re.compile(r"[a-z0-9-]{1,23}")
COPIED = ("seconds", "role", "mood", "title", "fallback")


def blocks(path: Path) -> bytes:
    """The ADPCM file's bytes: whole blocks, each starting with a coder state
    the firmware decodes (step index at most 88, then a zero byte)."""
    data = path.read_bytes()
    if not data or len(data) % BLOCK_BYTES or len(data) > SOUND_MAX_BYTES:
        raise ValueError(f"{path}: {len(data)} bytes, not 1 to 256 whole 256-byte blocks")
    for at in range(0, len(data), BLOCK_BYTES):
        if data[at + 2] > 88 or data[at + 3]:
            raise ValueError(f"{path}: the block at {at} is not IMA ADPCM")
    return data


def build(sounds: list, base: Path) -> tuple[bytes, list]:
    """The bank's bytes and its sounds in id order, each with its data."""
    if not isinstance(sounds, list) or not 1 <= len(sounds) <= SOUNDS_MAX:
        raise ValueError(f"a bank holds 1 to {SOUNDS_MAX} sounds")
    ordered = sorted(sounds, key=lambda sound: str(sound.get("id")))
    for before, sound in zip([None] + ordered, ordered):
        if not ID.fullmatch(str(sound.get("id"))) or (before and before["id"] == sound["id"]):
            raise ValueError(f"sound id {sound.get('id')!r} is not 1 to 23 of a-z, 0-9 and '-', or is not unique")
        if sound.get("kind") not in KINDS or sound.get("rate") not in RATES:
            raise ValueError(f"{sound['id']}: kind {sound.get('kind')!r} or rate {sound.get('rate')!r}")
    datas = [blocks(base / sound["adpcm"]) for sound in ordered]
    data_offset = HEADER.size + ENTRY.size * len(ordered)
    header = HEADER.pack(b"SBK1", 1, len(ordered), data_offset, sum(map(len, datas)))
    entries, offset = [], 0
    for sound, data in zip(ordered, datas):
        entries.append(ENTRY.pack(sound["id"].encode(), offset, len(data), sound["rate"], KINDS[sound["kind"]], 0))
        offset += len(data)
    bank = header + b"".join(entries) + b"".join(datas)
    if len(bank) > BANK_MAX_BYTES:
        raise ValueError(f"the bank is {len(bank)} bytes, more than {BANK_MAX_BYTES}")
    return bank, ordered


def manifest(bank: bytes, ordered: list) -> dict:
    sounds = []
    for sound in ordered:
        entry = {"id": sound["id"], "kind": sound["kind"], "rate": sound["rate"]}
        entry.update({key: sound[key] for key in COPIED if key in sound})
        sounds.append(entry)
    return {"version": 1, "sha256": hashlib.sha256(bank).hexdigest(), "bytes": len(bank), "sounds": sounds}


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    parser.add_argument("sounds", type=Path, help="the JSON list of sounds")
    parser.add_argument("output", type=Path, help="the directory for sound-bank.bin and sound-bank.json")
    args = parser.parse_args()
    bank, ordered = build(json.loads(args.sounds.read_text(encoding="utf-8")), args.sounds.parent)
    args.output.mkdir(parents=True, exist_ok=True)
    (args.output / "sound-bank.bin").write_bytes(bank)
    text = json.dumps(manifest(bank, ordered), indent=2, ensure_ascii=False) + "\n"
    (args.output / "sound-bank.json").write_text(text, encoding="utf-8")
    print(f"sound-bank.bin: {len(ordered)} sounds, {len(bank)} bytes, sha256 {hashlib.sha256(bank).hexdigest()}")


if __name__ == "__main__":
    main()
