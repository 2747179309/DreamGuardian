#!/usr/bin/env python3
"""Collect official WakeNet baseline results.

Default condition matrix:
  distance: 0.3m, 1m, 2m
  speed: fast, normal, slow
  orientation: front, side
  repetitions: 20

The script can run in manual mode, or with pyserial installed it can listen for
serial lines containing "DG Wake: source=wakenet".
"""

from __future__ import annotations

import argparse
import csv
import itertools
import time
from pathlib import Path


def open_serial(port: str, baud: int):
    try:
        import serial
    except ImportError as exc:
        raise SystemExit("pyserial is required for serial mode: python -m pip install pyserial") from exc
    ser = serial.Serial(port, baudrate=baud, timeout=0.05)
    time.sleep(0.5)
    return ser


def send_line(ser, text: str) -> None:
    ser.write((text + "\n").encode("utf-8"))
    ser.flush()


def wait_wake(ser, timeout_s: float) -> tuple[bool, str]:
    deadline = time.time() + timeout_s
    lines: list[str] = []
    while time.time() < deadline:
        raw = ser.readline()
        if not raw:
            continue
        line = raw.decode("utf-8", errors="replace").strip()
        if line:
            lines.append(line)
        if "DG Wake: source=wakenet" in line and "ignored=generic" not in line:
            return True, " | ".join(lines[-8:])
    return False, " | ".join(lines[-8:])


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("--port", help="Serial port, for example COM12. Omit for manual mode.")
    parser.add_argument("--baud", type=int, default=115200)
    parser.add_argument("--out", type=Path, default=Path("experiments/wakeword_baseline.csv"))
    parser.add_argument("--speaker", default="unknown")
    parser.add_argument("--environment", default="quiet")
    parser.add_argument("--reps", type=int, default=20)
    parser.add_argument("--timeout", type=float, default=3.0)
    args = parser.parse_args()

    ser = open_serial(args.port, args.baud) if args.port else None
    if ser:
        send_line(ser, "waketpl off")
        print("sent: waketpl off")

    args.out.parent.mkdir(parents=True, exist_ok=True)
    fieldnames = [
        "timestamp",
        "wakeword",
        "model",
        "distance",
        "speed",
        "orientation",
        "speaker",
        "environment",
        "trial",
        "success",
        "latency_s",
        "notes",
    ]
    existing = args.out.exists()
    with args.out.open("a", newline="", encoding="utf-8") as f:
        writer = csv.DictWriter(f, fieldnames=fieldnames)
        if not existing:
            writer.writeheader()
        for distance, speed, orientation in itertools.product(
            ["0.3m", "1m", "2m"], ["fast", "normal", "slow"], ["front", "side"]
        ):
            successes = 0
            for trial in range(1, args.reps + 1):
                print(f"{distance} {speed} {orientation} trial {trial}/{args.reps}: say official wake word now")
                input("press Enter to start listening...")
                start = time.time()
                if ser:
                    success, notes = wait_wake(ser, args.timeout)
                else:
                    answer = input("detected? y/N: ").strip().lower()
                    success, notes = answer == "y", "manual"
                latency = time.time() - start
                successes += int(success)
                writer.writerow(
                    {
                        "timestamp": int(time.time()),
                        "wakeword": "Hi ESP",
                        "model": "WakeNet9 official hiesp",
                        "distance": distance,
                        "speed": speed,
                        "orientation": orientation,
                        "speaker": args.speaker,
                        "environment": args.environment,
                        "trial": trial,
                        "success": int(success),
                        "latency_s": round(latency, 3),
                        "notes": notes,
                    }
                )
                f.flush()
            recall = successes / float(args.reps)
            print(f"condition summary: {distance} {speed} {orientation} {successes}/{args.reps} recall={recall:.3f}")


if __name__ == "__main__":
    main()
