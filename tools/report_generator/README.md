# report_generator

Turns a recorded session into the report laid out in section 11 of the plan,
plus graphs with the findings drawn on them.

```
make report SESSION=data/baselines/motor4_fail
make report SESSION=data/sessions/session_20260824_183054 ARGS="--no-plots"
make pytest                       # rules only; no board, no matplotlib
```

Exit code carries the verdict, so this can gate something later:
`0` pass, `1` warning, `2` fail, `3` inconclusive.

## Files

| | |
|---|---|
| `analysis.py` | Segmentation, statistics, rules. Standard library only. |
| `report.py` | Renders section 11. Text, so it diffs against the last run. |
| `plots.py` | matplotlib. Imported only when graphs are wanted. |
| `generate.py` | CLI. |
| `test_analysis.py` | 31 tests, on data built in the test rather than recorded. |

Thresholds live in `test_profiles/*.toml`, not in the code.

## What it decides, and what it refuses to decide

There are four verdicts, and **INCONCLUSIVE outranks PASS**. A run that could
not be judged is not a run that passed, and reporting it as one is how a bench
ends up certifying a drone it never measured. It is returned when:

- a threshold in the profile still reads `tbd` — no baseline, nothing to judge
- the firmware accepted no samples, so every figure reads `0.000` because
  nothing was measured rather than because nothing was there
- only one current channel answered, so section 10.1 cannot be evaluated

The report also reads the logger's `errors.log`. A link that mangled some
lines beyond repair mangled others into values that still parse — a dropped
decimal point turns `4.081` into `4081` — and those are already in the CSV
wearing the shape of measurements. That happened on 2026-08-24, and without
this check the report blamed the ADC calibration for a loose USB connector.

## Three statistical choices

**Median, not mean, for comparing motors.** Nine corrupted samples in a window
of a thousand move a mean by 46% and condemn a healthy motor. They do not move
a median. There is a test that fails if the code is switched to a mean.

**99th percentile, not maximum, for peak current.** A maximum is one sample, so
one ADC glitch defines it — and it can only ever be revised upwards, which
makes the metric worse the longer the test runs.

**Segments are counted, not just measured.** A motor drawing too much announces
itself; a motor drawing nothing looks exactly like a quiet stretch of
recording. The number of current bursts is checked against the number the
profile expects, and the missing one is placed on the graph by extrapolating
the cadence of the ones that ran — marked as expected, never as measured.

## Baselines

`data/baselines/motor4_fail/` is a synthetic recording with a known defect,
committed on purpose: a fixture whose right answer is known in advance, and
which needs neither a board nor a broken drone to reproduce.
