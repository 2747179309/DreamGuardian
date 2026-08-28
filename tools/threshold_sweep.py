#!/usr/bin/env python3
"""Sweep wake-score thresholds from a labeled score log.

Expected input columns:
  file,label,score,duration_s

label must be 1 for positive wake-word samples and 0 for negative samples.
For continuous negative recordings, set duration_s to the recording length so
false accepts per hour can be estimated.
"""

from __future__ import annotations

import argparse
import csv
from pathlib import Path


def load_rows(path: Path) -> list[dict[str, str]]:
    with path.open("r", newline="", encoding="utf-8") as f:
        return list(csv.DictReader(f))


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("scores", type=Path)
    parser.add_argument("--out", type=Path, default=Path("experiments/threshold_sweep.csv"))
    parser.add_argument("--start", type=float, default=0.20)
    parser.add_argument("--stop", type=float, default=0.95)
    parser.add_argument("--step", type=float, default=0.01)
    args = parser.parse_args()

    rows = load_rows(args.scores)
    positives = [r for r in rows if int(r["label"]) == 1]
    negatives = [r for r in rows if int(r["label"]) == 0]
    neg_hours = sum(float(r.get("duration_s") or 0.0) for r in negatives) / 3600.0

    args.out.parent.mkdir(parents=True, exist_ok=True)
    with args.out.open("w", newline="", encoding="utf-8") as f:
        fieldnames = [
            "threshold",
            "true_accept",
            "false_reject",
            "false_accept",
            "true_reject",
            "recall",
            "false_reject_rate",
            "false_accept_rate",
            "false_accepts_per_hour",
        ]
        writer = csv.DictWriter(f, fieldnames=fieldnames)
        writer.writeheader()
        n = int(round((args.stop - args.start) / args.step)) + 1
        for i in range(n):
            thr = args.start + i * args.step
            ta = sum(float(r["score"]) >= thr for r in positives)
            fr = len(positives) - ta
            fa = sum(float(r["score"]) >= thr for r in negatives)
            tr = len(negatives) - fa
            recall = ta / len(positives) if positives else 0.0
            frr = fr / len(positives) if positives else 0.0
            far = fa / len(negatives) if negatives else 0.0
            writer.writerow(
                {
                    "threshold": round(thr, 4),
                    "true_accept": ta,
                    "false_reject": fr,
                    "false_accept": fa,
                    "true_reject": tr,
                    "recall": round(recall, 6),
                    "false_reject_rate": round(frr, 6),
                    "false_accept_rate": round(far, 6),
                    "false_accepts_per_hour": round(fa / neg_hours, 6) if neg_hours > 0 else "",
                }
            )
    print(f"wrote {args.out}")


if __name__ == "__main__":
    main()
