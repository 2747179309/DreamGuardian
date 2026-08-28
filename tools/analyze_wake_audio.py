#!/usr/bin/env python3
"""Analyze exported wake-word WAV captures.

Input: a directory containing 16 kHz mono signed 16-bit WAV files exported by
DEBUG_AUDIO_CAPTURE. Output: CSV metrics and waveform/spectrum plots.
"""

from __future__ import annotations

import argparse
import csv
import math
import wave
from pathlib import Path

try:
    import numpy as np
except ImportError as exc:  # pragma: no cover
    raise SystemExit("numpy is required: python -m pip install numpy") from exc

try:
    import matplotlib.pyplot as plt
except ImportError:
    plt = None


def read_wav(path: Path) -> tuple[int, np.ndarray]:
    with wave.open(str(path), "rb") as wav:
        channels = wav.getnchannels()
        width = wav.getsampwidth()
        rate = wav.getframerate()
        frames = wav.getnframes()
        raw = wav.readframes(frames)
    if width != 2:
        raise ValueError(f"{path}: expected 16-bit PCM, got sample width {width}")
    data = np.frombuffer(raw, dtype="<i2").astype(np.float32)
    if channels > 1:
        data = data.reshape(-1, channels)[:, 0]
    return rate, data


def frame_rms(x: np.ndarray, frame: int) -> np.ndarray:
    if len(x) < frame:
        return np.array([], dtype=np.float32)
    n = len(x) // frame
    y = x[: n * frame].reshape(n, frame)
    return np.sqrt(np.mean(y * y, axis=1))


def estimate_snr_db(x: np.ndarray, rate: int) -> float:
    rms = frame_rms(x, max(1, int(rate * 0.02)))
    if len(rms) < 5:
        return float("nan")
    noise = np.percentile(rms, 10)
    speech = np.percentile(rms, 90)
    if noise <= 1e-9:
        return float("inf")
    return 20.0 * math.log10(max(speech, 1e-9) / noise)


def count_glitches(x: np.ndarray, rate: int) -> tuple[int, int]:
    frame = max(1, int(rate * 0.02))
    rms = frame_rms(x, frame)
    if len(rms) < 5:
        return 0, 0
    jumps = np.abs(np.diff(rms))
    median = float(np.median(jumps))
    mad = float(np.median(np.abs(jumps - median))) + 1e-6
    periodic_or_burst = int(np.sum(jumps > median + 10.0 * mad))
    zero_frames = int(np.sum(rms < 1.0))
    return periodic_or_burst, zero_frames


def analyze_file(path: Path, plot_dir: Path | None) -> dict[str, object]:
    rate, x = read_wav(path)
    duration = len(x) / float(rate) if rate else 0.0
    peak = float(np.max(np.abs(x))) if len(x) else 0.0
    rms = float(np.sqrt(np.mean(x * x))) if len(x) else 0.0
    dc = float(np.mean(x)) if len(x) else 0.0
    clipping = float(np.mean(np.abs(x) >= 32760.0)) if len(x) else 0.0
    glitches, zero_frames = count_glitches(x, rate)
    snr = estimate_snr_db(x, rate)

    dom_freq = float("nan")
    if len(x) > 0:
        windowed = x[: min(len(x), rate * 10)]
        spec = np.abs(np.fft.rfft(windowed * np.hanning(len(windowed))))
        freqs = np.fft.rfftfreq(len(windowed), 1.0 / rate)
        if len(spec) > 1:
            idx = int(np.argmax(spec[1:]) + 1)
            dom_freq = float(freqs[idx])

    if plot_dir and plt is not None and len(x) > 0:
        plot_dir.mkdir(parents=True, exist_ok=True)
        t = np.arange(len(x)) / float(rate)
        fig, axes = plt.subplots(2, 1, figsize=(10, 6))
        axes[0].plot(t, x, linewidth=0.6)
        axes[0].set_title(path.name)
        axes[0].set_xlabel("time_s")
        axes[0].set_ylabel("pcm")
        spec = np.abs(np.fft.rfft(x * np.hanning(len(x))))
        freqs = np.fft.rfftfreq(len(x), 1.0 / rate)
        axes[1].plot(freqs, 20.0 * np.log10(spec + 1.0), linewidth=0.6)
        axes[1].set_xlim(0, min(8000, rate / 2))
        axes[1].set_xlabel("frequency_hz")
        axes[1].set_ylabel("db")
        fig.tight_layout()
        fig.savefig(plot_dir / f"{path.stem}.png", dpi=140)
        plt.close(fig)

    return {
        "file": str(path),
        "sample_rate": rate,
        "samples": len(x),
        "duration_s": round(duration, 3),
        "rms": round(rms, 3),
        "rms_dbfs": round(20.0 * math.log10(max(rms, 1e-9) / 32768.0), 2),
        "peak": round(peak, 3),
        "peak_dbfs": round(20.0 * math.log10(max(peak, 1e-9) / 32768.0), 2),
        "dc_offset": round(dc, 3),
        "clipping_ratio": round(clipping, 6),
        "snr_est_db": round(snr, 2) if math.isfinite(snr) else snr,
        "dominant_freq_hz": round(dom_freq, 1) if math.isfinite(dom_freq) else dom_freq,
        "glitch_events": glitches,
        "near_zero_frames_20ms": zero_frames,
    }


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("input", type=Path, help="WAV file or directory")
    parser.add_argument("--out", type=Path, default=Path("wake_audio_analysis.csv"))
    parser.add_argument("--plots", type=Path, default=Path("wake_audio_plots"))
    parser.add_argument("--no-plots", action="store_true")
    args = parser.parse_args()

    paths = [args.input] if args.input.is_file() else sorted(args.input.rglob("*.wav"))
    rows = []
    for path in paths:
        rows.append(analyze_file(path, None if args.no_plots else args.plots))

    args.out.parent.mkdir(parents=True, exist_ok=True)
    with args.out.open("w", newline="", encoding="utf-8") as f:
        writer = csv.DictWriter(f, fieldnames=list(rows[0].keys()) if rows else ["file"])
        writer.writeheader()
        writer.writerows(rows)
    print(f"wrote {args.out} rows={len(rows)}")
    if not args.no_plots and plt is None:
        print("matplotlib not installed; plots skipped")


if __name__ == "__main__":
    main()
