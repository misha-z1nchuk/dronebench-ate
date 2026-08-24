"""
Renders an Analysis as the report laid out in section 11 of the plan.

Every heading from that list appears, including the ones nothing can fill yet.
A section quietly omitted reads as a section that passed; "not available"
reads as what it is. The same applies to figures behind a threshold nobody has
measured — those say so instead of showing a tick.

Text rather than HTML because the report is meant to be read in a terminal
next to the bench, pasted into a message, and diffed against the previous run.
"""

from __future__ import annotations

from datetime import datetime

from analysis import Analysis, Verdict

WIDTH = 74
RULE = "=" * WIDTH
THIN = "-" * WIDTH

# Filled in by the runner once MSP is wired up, on day 18.
UNAVAILABLE = "not available — needs the MSP link (phase 3)"


def _heading(text: str) -> str:
    return f"\n{text}\n{THIN}"


def _verdict_line(verdict: Verdict) -> str:
    return {
        Verdict.PASS: "PASS",
        Verdict.WARNING: "WARNING — usable, but something is off",
        Verdict.FAIL: "FAIL",
        Verdict.INCONCLUSIVE:
            "INCONCLUSIVE — the bench could not judge this run",
    }[verdict]


def _amps(value: float | None) -> str:
    return "—" if value is None else f"{value:.3f} A"


def render(analysis: Analysis, plots: list[str] | None = None) -> str:
    profile = analysis.profile
    session = analysis.session
    lines: list[str] = []

    lines.append(RULE)
    lines.append("DroneBench ATE — test report".center(WIDTH))
    lines.append(RULE)
    lines.append(f"generated   {datetime.now():%Y-%m-%d %H:%M:%S}")
    if session.source is not None:
        lines.append(f"source      {session.source}")

    lines.append(_heading("Device ID"))
    lines.append(f"  {UNAVAILABLE}")

    lines.append(_heading("Firmware"))
    lines.append(f"  {UNAVAILABLE}")

    lines.append(_heading("Test profile"))
    lines.append(f"  {profile.name}")
    lines.append(f"  sample rate      {profile.sample_rate_hz} Hz")
    lines.append(f"  current source   {profile.current_source}")
    lines.append(f"  samples          {len(session)} over "
                 f"{session.duration_s:.2f} s")

    lines.append(_heading("Battery"))
    lines.append(f"  minimum          {analysis.min_voltage_v:.3f} V")
    lines.append(f"  maximum          {analysis.max_voltage_v:.3f} V")
    lines.append(f"  sag under load   "
                 f"{analysis.max_voltage_v - analysis.min_voltage_v:.3f} V")
    limit = profile.limit("battery_min_v")
    lines.append(f"  undervoltage at  "
                 f"{'not set' if limit is None else f'{limit:.2f} V'}")

    lines.append(_heading("Power-on result"))
    startup = profile.limit("startup_current_max_a")
    if startup is None:
        lines.append("  no startup-current baseline yet — nothing to compare "
                     "against (day 17)")
    else:
        lines.append(f"  peak at start    {_amps(analysis.peak_current_a)} "
                     f"against {_amps(startup)}")

    lines.append(_heading("Idle current"))
    idle_limit = profile.limit("idle_current_max_a")
    lines.append(f"  measured         {_amps(analysis.idle_current_a)}")
    if idle_limit is None:
        lines.append("  limit            not set — no baseline yet (day 17)")
    else:
        lines.append(f"  limit            {_amps(idle_limit)}")

    lines.append(_heading("Motor results"))
    if not analysis.segments:
        lines.append("  no motor runs found in this recording")
    else:
        import statistics
        reference = statistics.median(s.median_a for s in analysis.segments)
        lines.append(f"  {'#':>2}  {'window':>16}  {'median':>9}  "
                     f"{'peak p99':>9}  {'vs median':>10}")
        for segment in analysis.segments:
            deviation = (segment.median_a - reference) / reference * 100.0
            lines.append(
                f"  {segment.index:>2}  "
                f"{segment.start_s:6.2f}–{segment.end_s:6.2f} s  "
                f"{segment.median_a:8.3f} A  {segment.peak_a:8.3f} A  "
                f"{deviation:+9.1f}%"
            )
        lines.append("")
        lines.append(f"  reference        {reference:.3f} A "
                     f"(median across {len(analysis.segments)} motors)")
        lines.append(f"  expected motors  {profile.motor_count}")
        # Said out loud because it is the choice most likely to be questioned,
        # and the answer is not obvious from the numbers alone.
        lines.append("  medians, not means: the spin-up ramp drags a mean "
                     "down by however")
        lines.append("  long it lasted, so two identical motors commanded for "
                     "different times")
        lines.append("  would read differently.")

    failures = [f for f in analysis.findings if f.verdict is not Verdict.PASS]

    lines.append(_heading("Observed failures"))
    if not failures:
        lines.append("  none")
    for finding in failures:
        lines.append(f"  [{finding.verdict.name}] {finding.title}")
        lines.append(f"      {finding.detail}")
        if finding.span_s is not None:
            start, end = finding.span_s
            lines.append(f"      at {start:.2f}–{end:.2f} s")

    lines.append(_heading("Likely causes"))
    causes = [f for f in failures if f.cause]
    if not causes:
        lines.append("  none to name")
    for finding in causes:
        lines.append(f"  {finding.title}:")
        for chunk in _wrap(finding.cause, WIDTH - 6):
            lines.append(f"      {chunk}")

    lines.append(_heading("Recommended next test"))
    steps = [f for f in failures if f.next_test]
    if not steps:
        lines.append("  nothing outstanding")
    for number, finding in enumerate(steps, start=1):
        for index, chunk in enumerate(_wrap(finding.next_test, WIDTH - 6)):
            prefix = f"  {number}. " if index == 0 else "     "
            lines.append(f"{prefix}{chunk}")

    lines.append(_heading("Measurement uncertainty"))
    summary = analysis.firmware_summary()
    if session.has_reference:
        lines.append("  two current channels available; their difference is "
                     "the bench's own")
        lines.append("  measure of trust — see section 10.1.")
    else:
        lines.append("  one current channel only. The reference (INA226) read "
                     "NaN throughout,")
        lines.append("  so the error budget in section 10.1 cannot be "
                     "evaluated and every")
        lines.append("  current figure here carries unquantified uncertainty.")
    if summary is not None:
        refused = (int(summary["sensor_failures"])
                   + int(summary["rejected_time"])
                   + int(summary["rejected_value"]))
        lines.append("")
        lines.append(f"  firmware counted {summary['sample_count']} accepted, "
                     f"{refused} refused,")
        lines.append(f"  {summary['dropped']} periods missed, "
                     f"{summary['gaps']} gaps.")
        lines.append(f"  its own integration: {summary['consumed_mah']} mAh, "
                     f"{summary['consumed_wh']} Wh")
        lines.append("  — computed from every sample it took, including any "
                     "the link lost.")

    if plots:
        lines.append(_heading("Graphs"))
        for path in plots:
            lines.append(f"  {path}")

    lines.append("")
    lines.append(RULE)
    lines.append(f"Overall category: {_verdict_line(analysis.verdict)}")
    lines.append(RULE)
    lines.append("")

    return "\n".join(lines)


def _wrap(text: str, width: int) -> list[str]:
    words = text.split()
    out: list[str] = []
    line = ""
    for word in words:
        candidate = f"{line} {word}".strip()
        if len(candidate) > width and line:
            out.append(line)
            line = word
        else:
            line = candidate
    if line:
        out.append(line)
    return out
