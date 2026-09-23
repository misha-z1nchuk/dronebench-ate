#!/usr/bin/env python3
"""
Day 13: how noisy is a channel, and what does filtering it cost.

    python3 tools/report_generator/noise.py data/sessions/session_.../stream_001_samples.csv
    python3 tools/report_generator/noise.py <csv> --column voltage_v
    python3 tools/report_generator/noise.py <csv> --skip 1 --seconds 10

Record with nothing changing — no current through the ACS724, or a steady
voltage — so that every wiggle in the column is noise. Ten seconds at 500 Hz
is 5000 samples, plenty for a standard deviation to settle.

WHAT IT PRINTS

The noise itself: RMS about the mean (the number for the error budget and for
the INA226 decision in plan section 3.3.1) and peak-to-peak (what one unlucky
sample can do).

Then moving average against IIR at matched smoothing. The two are compared at
alpha = 2 / (N + 1), the usual pairing — it gives both filters the same mean
delay, (N - 1) / 2 samples, so neither wins by simply being slower.
What differs is the shape:

  - noise left after filtering, measured on this recording rather than assumed
  - time to reach 90 % of a step, the number that decides whether a motor
    spin-up still looks like one after filtering

A moving average reaches 90 % of a step at 0.9 N samples and 100 % at
exactly N, and needs N samples of memory. An IIR starts moving twice as
steeply, reaches 90 % only at ~1.15 N, never quite gets to 100 %, and needs
one float of memory. For white noise the two leave exactly the same RMS.
On a microcontroller that trade is usually settled by the memory; on this
bench the latency is what the diagnostic rules will feel.

Standard library only, like the rest of this directory's rules.
"""

from __future__ import annotations

import argparse
import csv
import math
import statistics
import sys
from dataclasses import dataclass
from pathlib import Path

WINDOWS = (4, 16, 64, 256)


@dataclass
class Noise:
    n: int
    rate_hz: float
    mean: float
    rms: float
    peak_to_peak: float


@dataclass
class FilterRow:
    window: int
    alpha: float
    ma_rms: float
    iir_rms: float
    ma_t90_ms: float
    iir_t90_ms: float


def load(path: Path, column: str) -> tuple[list[int], list[float]]:
    times: list[int] = []
    values: list[float] = []
    with path.open(newline="") as f:
        for row in csv.DictReader(f):
            if column not in row:
                raise SystemExit(f"no column {column!r} in {path}")
            value = float(row[column])
            # A NAN is a channel that was absent, not a reading of zero.
            if math.isfinite(value):
                times.append(int(row["t_us"]))
                values.append(value)
    return times, values


def measure(times: list[int], values: list[float]) -> Noise:
    if len(values) < 2:
        raise ValueError("need at least two samples")
    span_s = (times[-1] - times[0]) / 1e6
    return Noise(
        n=len(values),
        rate_hz=(len(values) - 1) / span_s if span_s > 0 else float("nan"),
        mean=statistics.fmean(values),
        # Population, not sample, deviation: this is the noise of this record,
        # not an estimate for some larger population of records.
        rms=statistics.pstdev(values),
        peak_to_peak=max(values) - min(values),
    )


def moving_average(values: list[float], window: int) -> list[float]:
    """Output only once the window is full; the ramp-in is not noise."""
    out = []
    acc = sum(values[:window])
    out.append(acc / window)
    for i in range(window, len(values)):
        acc += values[i] - values[i - window]
        out.append(acc / window)
    return out


def iir(values: list[float], alpha: float, settle: int) -> list[float]:
    """y += alpha * (x - y), seeded with the first sample.

    The first `settle` outputs are dropped for the same reason the moving
    average drops its ramp-in — so both are judged on the same part of the
    record.
    """
    y = values[0]
    out = []
    for i, x in enumerate(values):
        y += alpha * (x - y)
        if i >= settle:
            out.append(y)
    return out


def samples_to_90(step_response: list[float]) -> int:
    for i, y in enumerate(step_response):
        if y >= 0.9:
            return i + 1
    return len(step_response)


def compare(values: list[float], rate_hz: float,
            windows: tuple[int, ...] = WINDOWS) -> list[FilterRow]:
    rows = []
    for n in windows:
        if n * 4 > len(values):
            break
        alpha = 2.0 / (n + 1)
        settle = 5 * n  # an IIR is within e^-10 of its input by then

        step = [0.0] * n + [1.0] * (settle + n)
        ma_step = moving_average(step, n)
        iir_step = iir(step, alpha, 0)[n:]
        period_ms = 1000.0 / rate_hz

        rows.append(FilterRow(
            window=n,
            alpha=alpha,
            ma_rms=statistics.pstdev(moving_average(values, n)[settle:]),
            iir_rms=statistics.pstdev(iir(values, alpha, settle + n - 1)),
            ma_t90_ms=samples_to_90(ma_step[1:]) * period_ms,
            iir_t90_ms=samples_to_90(iir_step) * period_ms,
        ))
    return rows


def main(argv: list[str] | None = None) -> int:
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("csv", type=Path)
    ap.add_argument("--column", default="current_a")
    ap.add_argument("--skip", type=float, default=0.0,
                    help="seconds to drop at the start")
    ap.add_argument("--seconds", type=float, default=0.0,
                    help="seconds to keep after --skip (0 = all)")
    args = ap.parse_args(argv)

    times, values = load(args.csv, args.column)
    if not values:
        print(f"no finite values in {args.column}", file=sys.stderr)
        return 1

    t0 = times[0] + int(args.skip * 1e6)
    t1 = t0 + int(args.seconds * 1e6) if args.seconds > 0 else None
    kept = [(t, v) for t, v in zip(times, values)
            if t >= t0 and (t1 is None or t < t1)]
    times = [t for t, _ in kept]
    values = [v for _, v in kept]

    noise = measure(times, values)
    unit = "A" if args.column.startswith("current") else "V"
    print(f"{args.csv.name}  column {args.column}")
    print(f"  samples       {noise.n}  at {noise.rate_hz:.1f} Hz "
          f"({noise.n / noise.rate_hz:.1f} s)")
    print(f"  mean          {noise.mean:+.4f} {unit}")
    print(f"  RMS noise     {noise.rms * 1000:.2f} m{unit}")
    print(f"  peak-to-peak  {noise.peak_to_peak * 1000:.2f} m{unit}")
    print()
    print(f"  {'N':>4} {'alpha':>7}   {'MA rms':>9} {'IIR rms':>9}   "
          f"{'MA t90':>8} {'IIR t90':>8}")
    for r in compare(values, noise.rate_hz):
        print(f"  {r.window:>4} {r.alpha:>7.4f}   "
              f"{r.ma_rms * 1000:>7.2f}m{unit} {r.iir_rms * 1000:>7.2f}m{unit}   "
              f"{r.ma_t90_ms:>6.1f}ms {r.iir_t90_ms:>6.1f}ms")
    return 0


if __name__ == "__main__":
    sys.exit(main())
