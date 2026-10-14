#!/usr/bin/env python3
"""Write the story's sound loops, firmware/main/atmospheres/<name>.adpcm.

    python3 firmware/tools/make_atmospheres.py               # generated placeholders
    python3 firmware/tools/make_atmospheres.py --input DIR   # DIR/<name>.wav, the names it has

A loop is 12 kHz mono IMA ADPCM in 256-byte blocks of 504 samples. Each block
starts with its own coder state, as firmware/main/pet_ima_adpcm.h reads it, and
the last flows into the first. A PCM WAV file of any rate, mono or stereo, is
mixed to mono, resampled, cut to at most 8 s, made seamless by crossfading its
next half second into its start, and normalised to a -6 dBFS peak. The
placeholders are synthesised here: no third-party audio. Standard library only.
"""
import argparse
import math
import random
import wave
from array import array
from pathlib import Path

NAMES = ("calm", "forest", "water", "night", "cave", "storm", "danger", "battle", "magic", "victory")
RATE = 12000
BLOCK_BYTES = 256
BLOCK_SAMPLES = (BLOCK_BYTES - 4) * 2
MAX_SECONDS = 8.0
SEAM_SECONDS = 0.5
PEAK = 16384
OUTPUT = Path(__file__).resolve().parents[1] / "main/atmospheres"

STEPS = (
    7, 8, 9, 10, 11, 12, 13, 14, 16, 17, 19, 21, 23, 25, 28, 31, 34, 37, 41, 45, 50, 55, 60, 66, 73, 80, 88,
    97, 107, 118, 130, 143, 157, 173, 190, 209, 230, 253, 279, 307, 337, 371, 408, 449, 494, 544, 598, 658,
    724, 796, 876, 963, 1060, 1166, 1282, 1411, 1552, 1707, 1878, 2066, 2272, 2499, 2749, 3024, 3327, 3660,
    4026, 4428, 4871, 5358, 5894, 6484, 7132, 7845, 8630, 9493, 10442, 11487, 12635, 13899, 15289, 16818,
    18500, 20350, 22385, 24623, 27086, 29794, 32767,
)
INDEX_STEP = (-1, -1, -1, -1, 2, 4, 6, 8)


def decode_nibble(nibble: int, predictor: int, index: int) -> tuple[int, int]:
    """One sample, exactly as pet_ima_adpcm.c decodes it."""
    step = STEPS[index]
    delta = step >> 3
    if nibble & 4:
        delta += step
    if nibble & 2:
        delta += step >> 1
    if nibble & 1:
        delta += step >> 2
    predictor = max(-32768, min(32767, predictor - delta if nibble & 8 else predictor + delta))
    return predictor, max(0, min(88, index + INDEX_STEP[nibble & 7]))


def encode_block(samples: list[int], predictor: int, index: int) -> tuple[bytes, int, int]:
    """One block from the coder state it starts in, and the state it ends in."""
    nibbles = []
    header = predictor.to_bytes(2, "little", signed=True) + bytes((index, 0))
    for sample in samples:
        step = STEPS[index]
        difference = sample - predictor
        nibble = 8 if difference < 0 else 0
        difference = abs(difference)
        for bit, part in ((4, step), (2, step >> 1), (1, step >> 2)):
            if difference >= part:
                nibble |= bit
                difference -= part
        predictor, index = decode_nibble(nibble, predictor, index)
        nibbles.append(nibble)
    body = bytes(low | high << 4 for low, high in zip(nibbles[::2], nibbles[1::2]))
    return header + body, predictor, index


def encode(samples: list[int]) -> bytes:
    """A loop of whole blocks. The first block starts in the state the last one
    ends in, so the coder runs on across the seam."""
    if not samples or len(samples) % BLOCK_SAMPLES:
        raise ValueError(f"a loop is whole blocks of {BLOCK_SAMPLES} samples")
    state = (samples[-1], 0)
    for _ in range(2):
        blocks = []
        predictor, index = state
        for start in range(0, len(samples), BLOCK_SAMPLES):
            block, predictor, index = encode_block(samples[start:start + BLOCK_SAMPLES], predictor, index)
            blocks.append(block)
        state = (predictor, index)
    return b"".join(blocks)


def read_wav(path: Path) -> tuple[list[float], int]:
    """Mono samples at full scale +-32768, and their rate."""
    with wave.open(str(path), "rb") as source:
        width, channels, rate = source.getsampwidth(), source.getnchannels(), source.getframerate()
        frames = source.readframes(source.getnframes())
    if width == 1:
        values = [(byte - 128) * 256 for byte in frames]
    elif width == 3:
        values = [int.from_bytes(frames[i:i + 3], "little", signed=True) / 256 for i in range(0, len(frames), 3)]
    else:
        values = [value / (1 << (8 * width - 16)) for value in array({2: "h", 4: "i"}[width], frames)]
    return [sum(values[i:i + channels]) / channels for i in range(0, len(values), channels)], rate


def resample(samples: list[float], rate: int) -> list[float]:
    """Windowed-sinc resampling to RATE, low-passed below the lower Nyquist."""
    if rate == RATE:
        return samples
    scale = min(1.0, RATE / rate)
    half = int(8 / scale)
    # Only what a loop can use: 8 s and its seam.
    length = min(int(len(samples) * RATE / rate), int((MAX_SECONDS + SEAM_SECONDS) * RATE) + BLOCK_SAMPLES)
    output = []
    for n in range(length):
        position = n * rate / RATE
        total = 0.0
        for k in range(max(0, int(position) - half + 1), min(len(samples), int(position) + half + 1)):
            x = (position - k) * scale
            window = 0.5 + 0.5 * math.cos(math.pi * (position - k) / half)
            total += samples[k] * scale * window * (math.sin(math.pi * x) / (math.pi * x) if x else 1.0)
        output.append(total)
    return output


def make_loop(samples: list[float]) -> list[int]:
    """At most MAX_SECONDS of whole blocks; the half second after them fades in
    over the start, so the end flows into it. Peak at -6 dBFS."""
    seam = int(SEAM_SECONDS * RATE)
    length = min(len(samples) - seam, int(MAX_SECONDS * RATE)) // BLOCK_SAMPLES * BLOCK_SAMPLES
    if length <= 0:
        raise SystemExit(f"a loop needs at least {(BLOCK_SAMPLES + seam) / RATE:.2f} s of audio")
    loop = samples[:length]
    for i in range(seam):
        angle = 0.5 * math.pi * i / seam
        loop[i] = loop[i] * math.sin(angle) + samples[length + i] * math.cos(angle)
    peak = max(abs(sample) for sample in loop) or 1.0
    return [round(sample * PEAK / peak) for sample in loop]


# ---- Placeholders: gentle, so speech stays clear on a small speaker ----------

def tone(frequency: float, i: int) -> float:
    return math.sin(2 * math.pi * frequency * i / RATE)


def lowpass(samples: list[float], cutoff: float) -> list[float]:
    alpha = 1 - math.exp(-2 * math.pi * cutoff / RATE)
    output, level = [], 0.0
    for sample in samples:
        level += alpha * (sample - level)
        output.append(level)
    return output


def band_noise(rng: random.Random, n: int, low: float, high: float) -> list[float]:
    noise = lowpass([rng.uniform(-1, 1) for _ in range(n)], high)
    return [sample - slow for sample, slow in zip(noise, lowpass(noise, low))]


def events(rng: random.Random, n: int, gaps: tuple[float, float], seconds: float, sound) -> list[float]:
    """`sound(t)`, t from 0 to 1 over `seconds`, at random gaps."""
    output = [0.0] * n
    count = int(seconds * RATE)
    at = int(rng.uniform(*gaps) * RATE)
    while at < n:
        for i in range(min(count, n - at)):
            output[at + i] += sound(i / count)
        at += int(rng.uniform(*gaps) * RATE)
    return output


def sweep(start: float, end: float, seconds: float, t: float) -> float:
    """A tone gliding from `start` to `end` Hz, at t from 0 to 1."""
    time = t * seconds
    return math.sin(2 * math.pi * (start * time + (end - start) * time * t / 2))


def calm(rng: random.Random, n: int) -> list[float]:
    chord = (220.0, 277.18, 329.63, 440.0, 554.37)
    return [sum((tone(f, i) + tone(f * 1.004, i)) * (0.7 + 0.3 * tone(0.13 + 0.05 * k, i + 900 * k))
                for k, f in enumerate(chord)) for i in range(n)]


def forest(rng: random.Random, n: int) -> list[float]:
    leaves = band_noise(rng, n, 900, 3500)
    birds = events(rng, n, (0.6, 2.2), 0.09, lambda t: math.sin(math.pi * t) * sweep(2400, 3600, 0.09, t))
    return [0.25 * leaf * (0.5 + 0.5 * tone(0.21, i) * tone(0.07, i)) + 0.6 * bird
            for i, (leaf, bird) in enumerate(zip(leaves, birds))]


def water(rng: random.Random, n: int) -> list[float]:
    wash = band_noise(rng, n, 150, 700)
    return [sample * (0.35 + 0.65 * (0.5 + 0.5 * tone(0.37, i)) ** 2) * (0.8 + 0.2 * tone(0.09, i))
            for i, sample in enumerate(wash)]


def night(rng: random.Random, n: int) -> list[float]:
    def chirps(i: int, offset: float) -> float:
        """Three soft 25 ms pulses at the start of every 0.55 s."""
        t = (i / RATE + offset) % 0.55
        pulse = t % 0.045
        return math.sin(math.pi * pulse / 0.025) ** 2 if t < 0.13 and pulse < 0.025 else 0.0

    air = band_noise(rng, n, 200, 1500)
    return [0.5 * tone(2900, i) * chirps(i, 0.0) + 0.35 * tone(3300, i) * chirps(i, 0.27) + 0.15 * air[i]
            for i in range(n)]


def cave(rng: random.Random, n: int) -> list[float]:
    drips = events(rng, n, (0.9, 2.4), 0.12, lambda t: math.exp(-8 * t) * sweep(1400, 800, 0.12, t))
    echo = [drip + 0.4 * drips[i - 2200] if i >= 2200 else drip for i, drip in enumerate(drips)]
    return [0.7 * drip + 0.25 * (tone(110, i) + 0.6 * tone(165.5, i)) * (0.8 + 0.2 * tone(0.1, i))
            for i, drip in enumerate(echo)]


def storm(rng: random.Random, n: int) -> list[float]:
    rain = band_noise(rng, n, 1200, 5000)
    rumble = lowpass(band_noise(rng, n, 40, 300), 200)
    return [0.35 * drop + 3.0 * low * max(0.0, tone(0.125, i)) ** 3 for i, (drop, low) in enumerate(zip(rain, rumble))]


def danger(rng: random.Random, n: int) -> list[float]:
    def beat(i: int) -> float:
        """A heartbeat: two thumps every 0.8 s."""
        t = (i / RATE) % 0.8
        return math.exp(-12 * t) + (0.6 * math.exp(-12 * (t - 0.22)) if t > 0.22 else 0.0)

    return [(tone(110, i) + 0.5 * tone(155.6, i) + 0.3 * tone(220, i)) * (0.25 + 0.75 * beat(i)) for i in range(n)]


def battle(rng: random.Random, n: int) -> list[float]:
    snare = band_noise(rng, n, 1500, 4500)
    output = []
    for i in range(n):
        t = (i / RATE) % 0.5
        drum = math.exp(-14 * t) * math.sin(2 * math.pi * (140 + 80 * math.exp(-20 * t)) * t)
        hit = (i / RATE) % 1.0 - 0.5
        output.append(drum + (0.5 * math.exp(-30 * hit) * snare[i] if hit >= 0 else 0.0))
    return output


def arpeggio(n: int, notes: tuple[float, ...], seconds: float, echo: float) -> list[float]:
    """Plucked notes that soften in and die out within their step, with an echo."""
    output = []
    for i in range(n):
        step, t = divmod(i / RATE, seconds)
        frequency = notes[int(step) % len(notes)]
        envelope = (1 - math.exp(-200 * t)) * math.exp(-25 * t)
        output.append(envelope * (tone(frequency, i) + 0.15 * tone(2 * frequency, i)))
    delay = int(0.23 * RATE)
    return [sample + echo * output[i - delay] if i >= delay else sample for i, sample in enumerate(output)]


def magic(rng: random.Random, n: int) -> list[float]:
    return arpeggio(n, (1046.5, 1318.5, 1480.0, 1568.0, 1975.5, 1568.0, 1480.0, 1318.5), 0.15, 0.5)


def victory(rng: random.Random, n: int) -> list[float]:
    return arpeggio(n, (523.25, 659.25, 783.99, 1046.5, 783.99, 659.25), 0.18, 0.3)


PLACEHOLDERS = {name: globals()[name] for name in NAMES}


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--input", type=Path, help="directory of <name>.wav files; other loops stay as they are")
    args = parser.parse_args()
    OUTPUT.mkdir(parents=True, exist_ok=True)
    written = 0
    length = int((MAX_SECONDS + SEAM_SECONDS) * RATE) + BLOCK_SAMPLES
    for name in NAMES:
        if args.input:
            source = args.input / f"{name}.wav"
            if not source.is_file():
                continue
            samples = resample(*read_wav(source))
        else:
            samples = PLACEHOLDERS[name](random.Random(name), length)
        data = encode(make_loop(samples))
        (OUTPUT / f"{name}.adpcm").write_bytes(data)
        written += 1
        print(f"{name}: {len(data) // BLOCK_BYTES * BLOCK_SAMPLES / RATE:.2f} s, {len(data):,} bytes")
    if not written:
        raise SystemExit(f"no <name>.wav in {args.input}; the names are {', '.join(NAMES)}")
    total = sum(path.stat().st_size for path in OUTPUT.glob("*.adpcm"))
    print(f"{total:,} bytes of loops in {OUTPUT}")


if __name__ == "__main__":
    main()
