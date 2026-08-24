"""
Voltage, current and power against time, with the findings drawn on.

Kept apart from analysis.py so the rules stay testable without matplotlib
installed — and because a graph is an illustration of a verdict, never the
source of one. Anything the eye can see here the rules must have found
already; if a plot shows something the report does not mention, that is a
missing rule, not a better graph.

Every finding that knows where it happened is marked. A report saying "motor 3
draws 34% below the others" and a graph with nothing on it makes the reader do
the alignment by hand, and that is where they stop trusting it.
"""

from __future__ import annotations

from pathlib import Path

import matplotlib

# Chosen before pyplot is imported: the bench has no display, and the default
# interactive backend would fail on a headless machine or block waiting for a
# window that nobody will close.
matplotlib.use("Agg")

import matplotlib.pyplot as plt  # noqa: E402

from analysis import Analysis, Verdict  # noqa: E402

SEVERITY_COLOUR = {
    Verdict.FAIL: "#c0392b",
    Verdict.WARNING: "#e67e22",
    Verdict.INCONCLUSIVE: "#7f8c8d",
    Verdict.PASS: "#27ae60",
}


def write_plots(analysis: Analysis, out_dir: Path) -> list[Path]:
    session = analysis.session
    if not len(session):
        return []

    power_w = [v * i for v, i in zip(session.voltage_v, session.current_a)]

    panels = [
        ("voltage", "Voltage", "V", session.voltage_v, "#2980b9"),
        ("current", "Current", "A", session.current_a, "#8e44ad"),
        ("power", "Power", "W", power_w, "#16a085"),
    ]

    written: list[Path] = []
    for stem, title, unit, series, colour in panels:
        path = out_dir / f"{stem}.png"
        _one_panel(analysis, title, unit, series, colour, path)
        written.append(path)

    combined = out_dir / "session.png"
    _all_panels(analysis, panels, combined)
    written.append(combined)
    return written


def _annotate(axis, analysis: Analysis, series: list[float]) -> None:
    """Motor windows behind the trace, findings on top of it."""
    for segment in analysis.segments:
        axis.axvspan(segment.start_s, segment.end_s, color="#000000",
                     alpha=0.04, zorder=0)
        axis.text(
            (segment.start_s + segment.end_s) / 2, max(series),
            f"M{segment.index}", ha="center", va="top", fontsize=8,
            color="#555555",
        )

    for finding in analysis.findings:
        if finding.span_s is None:
            continue
        start, end = finding.span_s
        colour = SEVERITY_COLOUR[finding.verdict]
        if end - start < 1e-6:
            axis.axvline(start, color=colour, linewidth=1.2, alpha=0.9)
        else:
            axis.axvspan(start, end, color=colour, alpha=0.18, zorder=1)


def _limits(axis, analysis: Analysis, unit: str) -> None:
    """
    Draws only limits that exist. A dashed line at a threshold nobody measured
    would be read as a limit that was met.
    """
    if unit != "V":
        return
    for key, colour, label in (
        ("battery_min_v", "#c0392b", "undervoltage"),
        ("battery_max_v", "#e67e22", "full charge"),
    ):
        value = analysis.profile.limit(key)
        if value is None:
            continue
        axis.axhline(value, color=colour, linestyle="--", linewidth=0.9,
                     alpha=0.7)
        axis.text(axis.get_xlim()[0], value, f" {label} {value:g} V",
                  fontsize=7, color=colour, va="bottom")


def _one_panel(analysis: Analysis, title: str, unit: str,
               series: list[float], colour: str, path: Path) -> None:
    figure, axis = plt.subplots(figsize=(11, 4))
    axis.plot(analysis.session.t_s, series, linewidth=0.9, color=colour)
    axis.set_title(f"{title} — {analysis.profile.name}")
    axis.set_xlabel("time (s)")
    axis.set_ylabel(f"{title.lower()} ({unit})")
    axis.grid(alpha=0.25)
    _annotate(axis, analysis, series)
    _limits(axis, analysis, unit)
    figure.tight_layout()
    figure.savefig(path, dpi=130)
    plt.close(figure)


def _all_panels(analysis: Analysis, panels, path: Path) -> None:
    """
    One figure with a shared time axis. Voltage and current are two views of
    the same instant, and reading a sag against the current that caused it
    only works when they line up vertically.
    """
    figure, axes = plt.subplots(len(panels), 1, figsize=(11, 9), sharex=True)

    for axis, (_, title, unit, series, colour) in zip(axes, panels):
        axis.plot(analysis.session.t_s, series, linewidth=0.9, color=colour)
        axis.set_ylabel(f"{title} ({unit})")
        axis.grid(alpha=0.25)
        _annotate(axis, analysis, series)
        _limits(axis, analysis, unit)

    axes[-1].set_xlabel("time (s)")
    axes[0].set_title(
        f"{analysis.profile.name} — {analysis.verdict.name}",
        color=SEVERITY_COLOUR[analysis.verdict],
    )
    figure.tight_layout()
    figure.savefig(path, dpi=130)
    plt.close(figure)
