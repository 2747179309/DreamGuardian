#!/usr/bin/env python3
import argparse
import math
import struct
import wave
from pathlib import Path

SAMPLE_RATE = 16000
BITS = 16
BINS = 32
MIN_SAMPLES = (SAMPLE_RATE * 35) // 100
MAX_SAMPLES = SAMPLE_RATE * 4
MAX_TEMPLATES = 64
MAGIC = b"DGWT"
VERSION = 1


def read_wav_mono(path: Path):
    with wave.open(str(path), "rb") as wf:
        channels = wf.getnchannels()
        rate = wf.getframerate()
        width = wf.getsampwidth()
        frames = wf.getnframes()
        if rate != SAMPLE_RATE or width != BITS // 8 or channels < 1:
            raise ValueError(f"need {SAMPLE_RATE}Hz 16-bit WAV, got {rate}Hz {width * 8}-bit {channels}ch")
        raw = wf.readframes(min(frames, MAX_SAMPLES))
    samples = []
    stride = channels * 2
    for i in range(0, len(raw) - stride + 1, stride):
        samples.append(struct.unpack_from("<h", raw, i)[0])
    return samples


def extract_features(samples):
    if len(samples) < MIN_SAMPLES:
        raise ValueError("too short")

    peak = max(abs(x) for x in samples)
    if peak < 300:
        raise ValueError(f"too quiet peak={peak}")

    threshold = max(peak // 8, 350)
    start = 0
    end = len(samples)
    while start < len(samples) and abs(samples[start]) < threshold:
        start += 1
    while end > start and abs(samples[end - 1]) < threshold:
        end -= 1

    pad = SAMPLE_RATE // 10
    start = max(0, start - pad)
    end = min(len(samples), end + pad)
    if end <= start or end - start < MIN_SAMPLES:
        raise ValueError("speech segment too short after trim")

    if end - start > MAX_SAMPLES:
        start = end - MAX_SAMPLES
    length = end - start

    bins = []
    zcr = []
    for b in range(BINS):
        a = start + (b * length) // BINS
        z = start + ((b + 1) * length) // BINS
        if z <= a:
            z = a + 1
        seg = samples[a:min(z, len(samples))]
        if not seg:
            seg = [0]
        rms = math.sqrt(sum((x / 32768.0) ** 2 for x in seg) / len(seg))
        bins.append(math.log10(rms + 0.00003))
        crosses = 0
        prev = seg[0]
        for x in seg[1:]:
            if (prev < 0 <= x) or (prev >= 0 > x):
                crosses += 1
            prev = x
        zcr.append(crosses / max(1, len(seg) - 1))

    mean = sum(bins) / BINS
    bins = [x - mean for x in bins]
    scale = math.sqrt(sum(x * x for x in bins)) + 0.0001
    bins = [x / scale for x in bins]
    return length, bins, zcr, peak


def sort_key(path: Path):
    stem = path.stem
    digits = "".join(ch for ch in stem if ch.isdigit())
    return (int(digits) if digits else 0, path.name)


def write_templates(out_path: Path, entries):
    with out_path.open("wb") as f:
        f.write(MAGIC)
        f.write(struct.pack("<HHHH", VERSION, BINS, len(entries), 0))
        for name, samples, bins, zcr, peak in entries:
            encoded = name.encode("utf-8")[:31]
            f.write(struct.pack("<I", samples))
            f.write(struct.pack("<H", peak))
            f.write(encoded + b"\0" * (32 - len(encoded)))
            f.write(struct.pack("<" + "f" * BINS, *bins))
            f.write(struct.pack("<" + "f" * BINS, *zcr))


def main():
    parser = argparse.ArgumentParser(description="Train compact DreamGuardian wake templates from WAV files.")
    parser.add_argument("input_dir", type=Path, help="Directory containing hi xiaomeng WAV samples")
    parser.add_argument("-o", "--output", type=Path, default=Path("dreamguardian2/spiffs/wake_xm.tmpl"))
    parser.add_argument("--max", type=int, default=MAX_TEMPLATES)
    args = parser.parse_args()

    wavs = sorted(args.input_dir.glob("*.wav"), key=sort_key)
    entries = []
    rejected = []
    for path in wavs:
        if len(entries) >= args.max:
            break
        try:
            samples = read_wav_mono(path)
            trimmed, bins, zcr, peak = extract_features(samples)
            entries.append((path.name, trimmed, bins, zcr, peak))
        except Exception as exc:
            rejected.append((path.name, str(exc)))

    if not entries:
        raise SystemExit("no usable wake templates")

    args.output.parent.mkdir(parents=True, exist_ok=True)
    write_templates(args.output, entries)

    print(f"wrote {args.output} templates={len(entries)} size={args.output.stat().st_size} bytes")
    if rejected:
        print("rejected:")
        for name, reason in rejected:
            print(f"  {name}: {reason}")
    print("accepted:")
    for name, trimmed, _, _, peak in entries:
        print(f"  {name}: samples={trimmed} peak={peak}")


if __name__ == "__main__":
    main()
