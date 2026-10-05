"""Import a user-provided sound into the ESP32 firmware cue set.

The original file is copied into ``firmware/main/sfx/custom_sources`` for
reproducibility. The embedded output is signed 16-bit little-endian mono PCM at
24 kHz. A four-second safety cap protects application flash; shorter sounds are
preserved at their full duration.

Example:

    python tools/import_custom_sfx.py --cue wake --input C:\\Downloads\\wake.mp3
"""

from __future__ import annotations

import argparse
import hashlib
import json
from pathlib import Path
import shutil

import numpy as np
import soundfile as sf


SAMPLE_RATE = 24_000
MAX_DURATION_SECONDS = 4.0


def sha256(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest().upper()


def resample(samples: np.ndarray, source_rate: int) -> np.ndarray:
    if source_rate == SAMPLE_RATE:
        return samples
    output_length = round(len(samples) * SAMPLE_RATE / source_rate)
    source_positions = np.arange(output_length, dtype=np.float64) * source_rate / SAMPLE_RATE
    source_positions = np.minimum(source_positions, len(samples) - 1)
    return np.interp(source_positions, np.arange(len(samples)), samples).astype(np.float32)


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("--cue", required=True, help="Firmware cue basename, such as wake")
    parser.add_argument("--input", required=True, type=Path)
    args = parser.parse_args()

    if not args.cue.replace("_", "").isalnum() or args.cue.startswith("_"):
        raise ValueError("cue must contain only letters, digits, and underscores")
    source = args.input.resolve()
    if not source.is_file():
        raise FileNotFoundError(source)

    root = Path(__file__).resolve().parents[1]
    output_dir = root / "firmware" / "main" / "sfx"
    source_dir = output_dir / "custom_sources"
    output_dir.mkdir(parents=True, exist_ok=True)
    source_dir.mkdir(parents=True, exist_ok=True)

    retained_source = source_dir / f"{args.cue}{source.suffix.lower()}"
    shutil.copyfile(source, retained_source)
    source_bytes = retained_source.read_bytes()

    samples, source_rate = sf.read(retained_source, dtype="float32", always_2d=True)
    mono = samples.mean(axis=1)
    mono = resample(mono, source_rate)
    limit = round(MAX_DURATION_SECONDS * SAMPLE_RATE)
    was_truncated = len(mono) > limit
    mono = mono[:limit]
    pcm = np.round(np.clip(mono, -1.0, 1.0) * 32767.0).astype("<i2")
    destination = output_dir / f"{args.cue}.pcm"
    destination.write_bytes(pcm.tobytes())

    manifest_path = output_dir / "manifest.json"
    manifest = json.loads(manifest_path.read_text(encoding="utf-8"))
    manifest["source"] = "UISFX with per-cue user-provided overrides"
    manifest["license_scope"] = (
        "Top-level CC0 license applies to UISFX cues; per-cue overrides apply otherwise."
    )
    manifest["cues"][args.cue] = {
        "source_kind": "user-provided",
        "source_file": retained_source.relative_to(output_dir).as_posix(),
        "original_filename": source.name,
        "source_sha256": sha256(source_bytes),
        "source_sample_rate_hz": source_rate,
        "source_channels": samples.shape[1],
        "license": "user-provided; distribution rights not recorded",
        "samples": len(pcm),
        "duration_ms": round(len(pcm) * 1000 / SAMPLE_RATE),
        "truncated_to_four_seconds": was_truncated,
        "pcm_sha256": sha256(destination.read_bytes()),
    }
    manifest_path.write_text(
        json.dumps(manifest, indent=2) + "\n", encoding="utf-8", newline="\n"
    )
    print(
        f"{args.cue}: {len(pcm) * 1000 / SAMPLE_RATE:.1f} ms, "
        f"{destination.stat().st_size} bytes, {manifest['cues'][args.cue]['pcm_sha256']}"
    )


if __name__ == "__main__":
    main()
