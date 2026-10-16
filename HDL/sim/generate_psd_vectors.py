"""Deterministic 125 MS/s integer reference data; no hardware dependencies."""
import csv
import math
import random
from pathlib import Path

out = Path(__file__).resolve().parents[2] / "test_vectors"
out.mkdir(exist_ok=True)
rng = random.Random(125)
cases = [
    ("positive", 150, 0, 7, 150, 8192, 1),
    ("negative_pre", 150, 8, -3, 40, 12000, -1),
    ("max_window", 1250, 16, 7, 1234, 6000, 1),
    ("single_pre_point", 6, 5, -5, -4, 8192, 1),
]
with (out / "psd_reference_vectors.csv").open("w", newline="", encoding="utf-8") as f, \
     (out / "psd_reference_expected.csv").open("w", newline="", encoding="utf-8") as g:
    wave = csv.writer(f)
    results = csv.writer(g)
    wave.writerow(["case", "relative_sample", "time_ns", "adc_code", "overrange"])
    results.writerow(["case", "N", "P", "S", "E", "baseline", "polarity_sign", "qtotal", "qtail"])
    for name, n, pre, start, end, baseline, sign in cases:
        total = tail = 0
        for k in range(-pre, n-pre):
            # Generic prompt+tail waveform for arithmetic verification, not a fitted detector model.
            signal = 0 if k < 0 else round(900 * (.8 * math.exp(-k*8/24) + .2 * math.exp(-k*8/180)))
            adc = min(16383, max(0, baseline + sign*signal + rng.randint(-3, 3)))
            delta = sign * (adc-baseline)
            total += delta
            if start <= k < end:
                tail += delta
            wave.writerow([name, k, k*8, adc, 0])
        results.writerow([name, n, pre, start, end, baseline, sign, total, tail])
print(out)
