#!/usr/bin/env python3
"""
Turns a recorded session into a report and a set of graphs.

  python3 tools/report_generator/generate.py data/sessions/session_20260824_183054
  python3 tools/report_generator/generate.py <dir> --profile test_profiles/x.toml
  python3 tools/report_generator/generate.py <dir> --no-plots   # no matplotlib

A session directory holds one stream per power-up, and each is reported
separately: the streams have different clock origins and, in general, are
different tests. Merging them would average a fault in one run with a clean
run beside it.

The exit code carries the verdict, so this can gate something later:

    0  pass
    1  warning
    2  fail
    3  inconclusive — the bench could not judge the run
"""

from __future__ import annotations

import argparse
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))

from analysis import Profile, Verdict, analyse, load_session  # noqa: E402
from report import render  # noqa: E402

DEFAULT_PROFILE = Path("test_profiles/meteor75pro.toml")

EXIT_CODE = {
    Verdict.PASS: 0,
    Verdict.WARNING: 1,
    Verdict.FAIL: 2,
    Verdict.INCONCLUSIVE: 3,
}


def find_streams(session_dir: Path) -> list[Path]:
    return sorted(session_dir.glob("stream_*_samples.csv"))


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[1])
    parser.add_argument("session", type=Path,
                        help="a session directory, or one *_samples.csv")
    parser.add_argument("--profile", type=Path, default=DEFAULT_PROFILE)
    parser.add_argument("--no-plots", action="store_true",
                        help="skip the graphs; needs no matplotlib")
    args = parser.parse_args()

    if not args.profile.exists():
        raise SystemExit(f"no such profile: {args.profile}")
    profile = Profile.load(args.profile)

    if args.session.is_file():
        streams = [args.session]
    else:
        streams = find_streams(args.session)
    if not streams:
        raise SystemExit(f"no stream_*_samples.csv under {args.session}")

    worst = Verdict.PASS

    for samples_csv in streams:
        session = load_session(samples_csv)
        analysis = analyse(session, profile)

        plots: list[str] = []
        if not args.no_plots:
            out_dir = samples_csv.parent / (
                samples_csv.name.replace("_samples.csv", "_plots")
            )
            out_dir.mkdir(exist_ok=True)
            # Imported here rather than at the top so --no-plots works on a
            # machine without matplotlib, which is most of them.
            from plots import write_plots
            plots = [str(p) for p in write_plots(analysis, out_dir)]

        text = render(analysis, plots)
        destination = samples_csv.with_name(
            samples_csv.name.replace("_samples.csv", "_report.txt")
        )
        destination.write_text(text, encoding="utf-8")

        print(text)
        print(f"written: {destination}")

        worst = max(worst, analysis.verdict)

    return EXIT_CODE[worst]


if __name__ == "__main__":
    raise SystemExit(main())
