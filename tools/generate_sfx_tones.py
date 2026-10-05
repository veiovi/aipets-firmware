#!/usr/bin/env python3
"""Write the generated sound cues and record them in the cue manifest.

Nine of the firmware's 18 cues are plain tones made here: a sine with a quick
attack and a smooth decay, as signed 16-bit little-endian mono PCM at 24 kHz.
Each has a fixed length, so the firmware's size does not change. The tones and
this script are Apache-2.0. Running it again writes the same bytes.

    python3 tools/generate_sfx_tones.py
"""
import hashlib
import json
import math
import struct
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
SFX = ROOT / "firmware/main/sfx"
RATE = 24000
AMPLITUDE = 6000
ATTACK_SAMPLES = 120
# Cue: (samples, frequency in Hz). The five swipe cues climb a pentatonic scale.
TONES = {
    "wake": (12936, 660),
    "no_voice": (3344, 330),
    "settings_open": (6956, 880),
    "settings_close": (14754, 660),
    "face_swipe_0": (22896, 523),
    "face_swipe_1": (22882, 587),
    "face_swipe_2": (22922, 659),
    "face_swipe_3": (22943, 784),
    "face_swipe_4": (22907, 880),
}


def tone(samples: int, frequency: int) -> bytes:
    values = []
    for i in range(samples):
        envelope = min(1.0, i / ATTACK_SAMPLES) * (1.0 - i / samples) ** 3
        values.append(round(AMPLITUDE * envelope * math.sin(2 * math.pi * frequency * i / RATE)))
    return struct.pack(f"<{samples}h", *values)


def main() -> None:
    manifest_path = SFX / "manifest.json"
    manifest = json.loads(manifest_path.read_text(encoding="utf-8"))
    for cue, (samples, frequency) in TONES.items():
        pcm = tone(samples, frequency)
        (SFX / f"{cue}.pcm").write_bytes(pcm)
        manifest["cues"][cue] = {
            "source_kind": "generated",
            "generator": "tools/generate_sfx_tones.py",
            "license": "Apache-2.0",
            "frequency_hz": frequency,
            "samples": samples,
            "duration_ms": round(samples * 1000 / RATE),
            "pcm_sha256": hashlib.sha256(pcm).hexdigest().upper(),
        }
    manifest_path.write_text(json.dumps(manifest, indent=2) + "\n", encoding="utf-8", newline="\n")
    print(f"Wrote {len(TONES)} generated cues to {SFX}")


if __name__ == "__main__":
    main()
