#!/usr/bin/env python3
"""Compare PC and ESP feature dumps for golden wake-word samples.

This script compares CSV vectors exported by PC and ESP with the same basename:
  pc_dir/sample01_logmel.csv
  esp_dir/sample01_logmel.csv

Each CSV may contain one vector per line or a single comma-separated vector.
"""

from __future__ import annotations

import argparse
import csv
import math
from pathlib import Path

import numpy as np


def load_vector(path: Path) -> np.ndarray:
    values: list[float] = []
    with path.open("r", encoding="utf-8") as f:
        for line in f:
            line = line.strip()
            if not line:
                continue
            for part in line.split(","):
                part = part.strip()
                if part:
                    values.append(float(part))
    return np.array(values, dtype=np.float64)


def cosine(a: np.ndarray, b: np.ndarray) -> float:
    denom = float(np.linalg.norm(a) * np.linalg.norm(b))
    if denom <= 1e-12:
        return 0.0
    return float(np.dot(a, b) / denom)


def quant_saturation_ratio(v: np.ndarray) -> float:
    if len(v) == 0:
        return 0.0
    return float(np.mean((v <= -128.0) | (v >= 127.0)))


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("--pc-dir", type=Path, required=True)
    parser.add_argument("--esp-dir", type=Path, required=True)
    parser.add_argument("--out", type=Path, default=Path("experiments/feature_parity_report.csv"))
    args = parser.parse_args()

    rows = []
    for pc_path in sorted(args.pc_dir.glob("*.csv")):
        esp_path = args.esp_dir / pc_path.name
        if not esp_path.exists():
            rows.append({"feature": pc_path.name, "status": "missing_esp"})
            continue
        pc = load_vector(pc_path)
        esp = load_vector(esp_path)
        if pc.shape != esp.shape:
            rows.append(
                {
                    "feature": pc_path.name,
                    "status": "shape_mismatch",
                    "pc_elements": len(pc),
                    "esp_elements": len(esp),
                }
            )
            continue
        diff = np.abs(pc - esp)
        rows.append(
            {
                "feature": pc_path.name,
                "status": "ok",
                "pc_elements": len(pc),
                "esp_elements": len(esp),
                "max_abs_error": float(np.max(diff)) if len(diff) else 0.0,
                "mean_abs_error": float(np.mean(diff)) if len(diff) else 0.0,
                "cosine_similarity": cosine(pc, esp),
                "pc_quant_saturation_ratio": quant_saturation_ratio(pc),
                "esp_quant_saturation_ratio": quant_saturation_ratio(esp),
            }
        )

    args.out.parent.mkdir(parents=True, exist_ok=True)
    fieldnames = [
        "feature",
        "status",
        "pc_elements",
        "esp_elements",
        "max_abs_error",
        "mean_abs_error",
        "cosine_similarity",
        "pc_quant_saturation_ratio",
        "esp_quant_saturation_ratio",
    ]
    with args.out.open("w", newline="", encoding="utf-8") as f:
        writer = csv.DictWriter(f, fieldnames=fieldnames)
        writer.writeheader()
        writer.writerows(rows)
    print(f"wrote {args.out} rows={len(rows)}")


if __name__ == "__main__":
    main()
