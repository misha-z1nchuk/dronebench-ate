"""
Turns a recorded session into findings.

No plotting and no formatting here, and nothing outside the standard library,
so the rules can be tested without a board, without a session on disk and
without matplotlib installed. The rules are the part that decides whether a
drone gets handed back to its owner, and they are the part that must be
provable.

Three statistical choices run through this file, and each is deliberate.

Median, not mean, for comparing motors. A motor's current trace is a spin-up
ramp followed by a steady draw; the mean of that is pulled down by the ramp and
depends on how long the ramp was, so two identical motors commanded for
different durations would appear different. The median lands inside the steady
part and ignores the ramp entirely. It also survives a handful of corrupt
samples, which a mean does not.

Percentile, not maximum, for peak current. A maximum is a single sample. One
ADC glitch, one burst of interference from a switching regulator, and the peak
describes the noise rather than the motor — and it can only ever be revised
upwards, so the metric gets worse the longer the test runs. The 99th percentile
needs one sample in a hundred to agree before it moves.

Absence, not excess, is the hard case. A motor drawing too much announces
itself. A motor drawing nothing looks exactly like a quiet moment in the
recording, so the count of motor segments is checked against the count the
profile expects rather than only their sizes.
"""

from __future__ import annotations

import csv
import math
import statistics
import tomllib
from dataclasses import dataclass, field
from enum import Enum
from pathlib import Path

TBD = "tbd"


class Verdict(Enum):
    """
    Ordered by severity so the overall category is a max().

    INCONCLUSIVE outranks PASS on purpose. A test that could not be judged —
    because a threshold has no baseline behind it, or because the data has
    holes in it — is not a test that passed, and reporting it as one is how a
    bench ends up certifying a drone it never measured.
    """

    PASS = 0
    INCONCLUSIVE = 1
    WARNING = 2
    FAIL = 3

    def __lt__(self, other: "Verdict") -> bool:
        return self.value < other.value


@dataclass(frozen=True)
class Finding:
    verdict: Verdict
    title: str
    detail: str
    cause: str = ""
    next_test: str = ""
    # Where on the time axis this was decided, in seconds from session start.
    # Empty when the finding is about the session as a whole.
    span_s: tuple[float, float] | None = None


@dataclass
class Profile:
    name: str
    sample_rate_hz: int
    current_source: str
    motor_count: int
    limits: dict
    analysis: dict

    @classmethod
    def load(cls, path: Path) -> "Profile":
        with path.open("rb") as handle:
            raw = tomllib.load(handle)
        return cls(
            name=raw["name"],
            sample_rate_hz=raw["sample_rate_hz"],
            current_source=raw["current_source"],
            motor_count=raw["motor_count"],
            limits=raw["limits"],
            analysis=raw["analysis"],
        )

    def limit(self, key: str) -> float | None:
        """None when the profile says tbd, meaning nothing may be judged."""
        value = self.limits.get(key)
        if value is None or (isinstance(value, str) and value.lower() == TBD):
            return None
        return float(value)


@dataclass
class Segment:
    """One motor run, found in the data rather than assumed from a schedule."""

    index: int          # 1-based, in the order they occur
    start_s: float
    end_s: float
    median_a: float
    peak_a: float
    min_voltage_v: float

    @property
    def duration_s(self) -> float:
        return self.end_s - self.start_s


@dataclass
class Session:
    """A recorded stream: the samples, and what the firmware said about them."""

    t_s: list[float] = field(default_factory=list)
    voltage_v: list[float] = field(default_factory=list)
    current_a: list[float] = field(default_factory=list)
    current_ref_a: list[float] = field(default_factory=list)
    summaries: list[dict] = field(default_factory=list)
    source: Path | None = None
    # How many lines the logger refused while recording this session. Read
    # from its errors.log, because nothing in the CSV can show it: a line
    # mangled into "4081.000" instead of "4.081" parses perfectly and lands
    # in the file looking like a measurement.
    link_errors: int = 0

    def __len__(self) -> int:
        return len(self.t_s)

    @property
    def duration_s(self) -> float:
        return self.t_s[-1] - self.t_s[0] if len(self.t_s) > 1 else 0.0

    @property
    def has_reference(self) -> bool:
        """True only if the INA226 actually answered — not merely present."""
        return any(not math.isnan(v) for v in self.current_ref_a)


def load_session(samples_csv: Path) -> Session:
    """
    Reads one stream. Deliberately one stream and not a whole directory: each
    file is a separate power-up with its own clock origin, and concatenating
    them would produce timestamps that run backwards.
    """
    session = Session(source=samples_csv)

    with samples_csv.open(newline="", encoding="utf-8") as handle:
        for row in csv.DictReader(handle):
            session.t_s.append(int(row["t_us"]) / 1e6)
            session.voltage_v.append(float(row["voltage_v"]))
            session.current_a.append(float(row["current_a"]))
            session.current_ref_a.append(float(row["current_ref_a"]))

    if session.t_s:
        origin = session.t_s[0]
        session.t_s = [t - origin for t in session.t_s]

    summaries = samples_csv.with_name(
        samples_csv.name.replace("_samples.csv", "_summaries.csv")
    )
    if summaries.exists():
        with summaries.open(newline="", encoding="utf-8") as handle:
            session.summaries = list(csv.DictReader(handle))

    errors_log = samples_csv.parent / "errors.log"
    if errors_log.exists():
        session.link_errors = sum(
            1 for line in errors_log.read_text(encoding="utf-8").splitlines()
            if line.startswith("line ")
        )

    return session


def percentile(values: list[float], pct: float) -> float:
    """
    Linear interpolation between the two neighbouring samples, so the result
    does not jump as the sample count changes. Written out rather than taken
    from statistics.quantiles, which cuts into fixed buckets and cannot give
    an arbitrary percentile.
    """
    if not values:
        raise ValueError("percentile of an empty series")
    ordered = sorted(values)
    if len(ordered) == 1:
        return ordered[0]

    position = (pct / 100.0) * (len(ordered) - 1)
    low = int(position)
    high = min(low + 1, len(ordered) - 1)
    return ordered[low] + (ordered[high] - ordered[low]) * (position - low)


def find_motor_segments(session: Session, profile: Profile) -> list[Segment]:
    """
    Finds the runs of raised current, without being told when they were
    commanded.

    Detection rather than a fixed schedule because the schedule is not the
    thing being tested: a motor that starts late, stops early or never starts
    is exactly the fault this is looking for, and a fixed window would average
    the fault away with whatever sat next to it.
    """
    if len(session) < 2:
        return []

    pct = profile.analysis["peak_percentile"]
    fraction = profile.analysis["motor_threshold_fraction"]
    min_duration = profile.analysis["motor_min_duration_ms"] / 1000.0

    # The floor is a low percentile rather than a minimum, for the same reason
    # the peak is a high one: one dropout must not define it.
    floor = percentile(session.current_a, 100.0 - pct)
    ceiling = percentile(session.current_a, pct)

    if ceiling - floor <= 0:
        return []  # nothing ever rose above the noise: no motor ran

    threshold = floor + fraction * (ceiling - floor)

    segments: list[Segment] = []
    start_index: int | None = None

    for index, current in enumerate(session.current_a):
        above = current >= threshold
        if above and start_index is None:
            start_index = index
        elif not above and start_index is not None:
            _close_segment(session, segments, start_index, index,
                           min_duration, pct)
            start_index = None

    if start_index is not None:
        _close_segment(session, segments, start_index, len(session),
                       min_duration, pct)

    return segments


def _close_segment(session: Session, segments: list[Segment], start: int,
                   end: int, min_duration: float, pct: float) -> None:
    span = session.t_s[end - 1] - session.t_s[start]
    if span < min_duration:
        return  # a spike, or the edge of something — not a motor run

    window = session.current_a[start:end]
    segments.append(
        Segment(
            index=len(segments) + 1,
            start_s=session.t_s[start],
            end_s=session.t_s[end - 1],
            # Median over the whole window: the spin-up ramp is a small part
            # of it, so the median sits in the steady draw without anyone
            # having to guess where the ramp ended.
            median_a=statistics.median(window),
            peak_a=percentile(window, pct),
            min_voltage_v=min(session.voltage_v[start:end]),
        )
    )


# --- rules ----------------------------------------------------------------


def _check_voltage(session: Session, profile: Profile) -> list[Finding]:
    findings = []
    v_min = min(session.voltage_v)
    v_max = max(session.voltage_v)

    implausible = profile.limit("battery_implausible_v")
    if implausible is not None and v_max > implausible:
        # Stated as a calibration fault rather than a battery one, because
        # sending someone to buy a new pack over a wrong divider ratio is the
        # expensive kind of wrong answer.
        findings.append(Finding(
            Verdict.FAIL,
            "Voltage above anything a 1S cell can reach",
            f"{v_max:.3f} V, and the profile calls anything over "
            f"{implausible:.2f} V impossible.",
            cause="Divider ratio or ADC calibration, not the battery.",
            next_test="Measure the pack with a multimeter and compare against "
                      "the same reading on the bench.",
        ))
        return findings  # every other voltage rule is meaningless now

    minimum = profile.limit("battery_min_v")
    if minimum is None:
        findings.append(Finding(
            Verdict.INCONCLUSIVE, "No undervoltage limit in the profile",
            "battery_min_v has no baseline, so undervoltage was not judged.",
        ))
    elif v_min < minimum:
        index = session.voltage_v.index(v_min)
        findings.append(Finding(
            Verdict.FAIL, "Pack fell below the undervoltage limit",
            f"{v_min:.3f} V against a limit of {minimum:.2f} V.",
            cause="A tired pack, or a connector adding resistance under load. "
                  "Sag is only visible while drawing current, so a pack that "
                  "reads fine at rest is not exonerated.",
            next_test="Repeat with a known good pack. If it still sags, the "
                      "fault is in the harness rather than the battery.",
            span_s=(session.t_s[index], session.t_s[index]),
        ))

    maximum = profile.limit("battery_max_v")
    if maximum is not None and v_max > maximum:
        findings.append(Finding(
            Verdict.WARNING, "Pack above its nominal full charge",
            f"{v_max:.3f} V against {maximum:.2f} V for this chemistry.",
            cause="A LiHV pack read as LiPo, or a charger set one cell high.",
            next_test="Confirm the chemistry printed on the pack.",
        ))

    return findings


def expected_windows(segments: list[Segment], expected: int
                     ) -> list[tuple[float, float]]:
    """
    Where the motors that never ran should have been.

    An absence has no position of its own, and a finding with no position
    cannot be drawn — which leaves the reader to work out from the report text
    which empty stretch of the graph was the guilty one. The commanded
    schedule is not available here (it arrives with MSP on day 18), so the
    cadence is taken from the motors that did run: they are commanded on a
    fixed interval, so the smallest gap between two starts is one slot.

    Marked as expected rather than measured. It is an inference from a
    rhythm, and a graph that presents it as data would be inventing a
    measurement — the exact failure this bench exists to avoid.
    """
    if len(segments) < 2 or len(segments) >= expected:
        return []

    starts = [s.start_s for s in segments]
    spacing = min(b - a for a, b in zip(starts, starts[1:]))
    if spacing <= 0:
        return []
    width = statistics.median(s.duration_s for s in segments)

    windows: list[tuple[float, float]] = []

    # Gaps wide enough for a whole slot mean a motor in the middle was silent.
    for a, b in zip(starts, starts[1:]):
        skipped = round((b - a) / spacing) - 1
        for step in range(1, skipped + 1):
            begin = a + step * spacing
            windows.append((begin, begin + width))

    # Whatever is still unaccounted for was silent at the end, where there is
    # no following burst to bound it.
    trailing = expected - len(segments) - len(windows)
    for step in range(1, trailing + 1):
        begin = starts[-1] + step * spacing
        windows.append((begin, begin + width))

    return windows


def _check_motors(segments: list[Segment], profile: Profile) -> list[Finding]:
    findings = []
    expected = profile.motor_count

    if not segments:
        return [Finding(
            Verdict.FAIL, "No motor ever ran",
            "The current never rose above the idle floor.",
            cause="Nothing was commanded, the ESCs never armed, or the "
                  "current sensor is reading a constant.",
            next_test="Confirm the sensor first: a channel stuck at one value "
                      "and a drone that did nothing look identical here.",
        )]

    if len(segments) < expected:
        # The absence case, and the reason segments are counted at all. A
        # motor drawing nothing is indistinguishable from a quiet stretch of
        # recording unless something is looking for it.
        missing = expected - len(segments)
        windows = expected_windows(segments, expected)
        where = (
            "; expected at "
            + ", ".join(f"{a:.2f}–{b:.2f} s" for a, b in windows)
            if windows else ""
        )
        findings.append(Finding(
            Verdict.FAIL,
            f"{missing} of {expected} motors never drew current",
            f"Only {len(segments)} runs of raised current were found where "
            f"{expected} were commanded{where}.",
            cause="An open motor winding, an unplugged phase, or a dead ESC "
                  "output. All three are silent in the current trace.",
            next_test="Command the missing motor alone and watch the current. "
                      "If it is still flat, swap that motor onto a known good "
                      "ESC output to tell the motor from the driver.",
            # The first missing window, so the graph points at the silence.
            span_s=windows[0] if windows else None,
        ))
    elif len(segments) > expected:
        findings.append(Finding(
            Verdict.WARNING,
            f"{len(segments)} current bursts, {expected} motors commanded",
            "Something drew current outside the commanded windows.",
            cause="A motor restarting mid-run, or the detection threshold "
                  "sitting too low for this pack.",
            next_test="Look at the current graph before trusting the split.",
        ))

    warn_pct = profile.limit("motor_deviation_warn_percent")
    fail_pct = profile.limit("motor_deviation_fail_percent")
    if warn_pct is None or fail_pct is None:
        findings.append(Finding(
            Verdict.INCONCLUSIVE, "No motor deviation limits in the profile",
            "Motors were measured but not judged.",
        ))
        return findings

    reference = statistics.median(s.median_a for s in segments)
    if reference <= 0:
        return findings

    for segment in segments:
        deviation = (segment.median_a - reference) / reference * 100.0
        magnitude = abs(deviation)
        if magnitude < warn_pct:
            continue

        high = deviation > 0
        verdict = Verdict.FAIL if magnitude >= fail_pct else Verdict.WARNING
        findings.append(Finding(
            verdict,
            f"Motor {segment.index} draws {magnitude:.0f}% "
            f"{'above' if high else 'below'} the others",
            f"{segment.median_a:.3f} A against a median of {reference:.3f} A "
            f"across {len(segments)} motors.",
            cause="Bearing friction, a bent shaft or a rubbing bell if it "
                  "draws more; a partial winding fault or a weak ESC output "
                  "if it draws less."
            if high else
            "A partial winding fault, a poor connection, or an ESC output "
            "not driving fully.",
            next_test=f"Spin motor {segment.index} by hand and compare the "
                      f"feel against a known good one, then swap it with a "
                      f"neighbour and repeat — that separates the motor from "
                      f"the ESC output driving it.",
            span_s=(segment.start_s, segment.end_s),
        ))

    return findings


def _check_data_quality(session: Session, profile: Profile) -> list[Finding]:
    """
    Whether the recording is worth drawing conclusions from at all. This runs
    even when everything else passes, because a clean verdict over half the
    data is worse than no verdict: it is wrong and it looks right.
    """
    findings = []

    if not session.summaries:
        findings.append(Finding(
            Verdict.INCONCLUSIVE, "No summaries alongside the samples",
            "The firmware's own counters are missing, so nothing confirms how "
            "much of the session actually reached the host.",
        ))
        return findings

    if session.link_errors:
        # Ahead of every other check, because it changes how the rest of the
        # report should be read. A link that mangled some lines beyond repair
        # also mangled others into values that still parse, and those are
        # already in the CSV wearing the shape of measurements.
        findings.append(Finding(
            Verdict.WARNING, "The link corrupted part of this recording",
            f"The logger refused {session.link_errors} lines. Some corruption "
            f"produces values that still parse — a dropped decimal point "
            f"turns 4.081 into 4081 — so figures below may include readings "
            f"that were never measured.",
            cause="A loose USB connector, a marginal cable, or a bridge "
                  "unable to sustain this rate.",
            next_test="Reseat the cable and record again before believing any "
                      "verdict here. Corruption is not a property of the "
                      "drone.",
        ))

    last = session.summaries[-1]
    counted = int(last["sample_count"])
    refused = (int(last["sensor_failures"]) + int(last["rejected_time"])
               + int(last["rejected_value"]))
    dropped = int(last["dropped"])
    gaps = int(last["gaps"])

    if counted == 0:
        findings.append(Finding(
            Verdict.INCONCLUSIVE, "The bench recorded no valid samples",
            f"{refused} readings were refused and none accepted. Every figure "
            f"below is zero because nothing was measured, not because "
            f"nothing was there.",
            cause="The analog front-end is absent or not answering.",
            next_test="Check the sensor wiring before reading anything else "
                      "in this report.",
        ))
        return findings

    if refused or dropped:
        findings.append(Finding(
            Verdict.WARNING, "Part of the session was not measured",
            f"{refused} readings refused, {dropped} periods missed, "
            f"{gaps} gaps, against {counted} accepted.",
            cause="Refusals point at the sensor; missed periods point at the "
                  "bench not keeping up with its own sample rate.",
            next_test="Compare the two current channels: a fault in one shows "
                      "as a divergence, a fault in both shows as agreement.",
        ))

    # Compare what the host counted against what the firmware counted. The
    # difference is the price of the link, and it is invisible from either
    # side alone — the same argument as section 10.1, applied to sample counts
    # instead of amps.
    if counted > len(session) + 1:
        lost = counted - len(session)
        findings.append(Finding(
            Verdict.WARNING, "Samples were lost between board and host",
            f"The firmware accepted {counted}; {len(session)} reached the "
            f"CSV. {lost} went missing in the link.",
            cause="UART overrun, or the logger started after the session did.",
            next_test="The firmware's own mAh figure is still correct — it "
                      "integrated every sample. Prefer it over one computed "
                      "from this file.",
        ))

    if not session.has_reference:
        findings.append(Finding(
            Verdict.INCONCLUSIVE, "Only one current channel was available",
            "The reference channel read NaN throughout, so the difference "
            "between the two paths — the bench's own measure of how far to "
            "trust either — could not be computed.",
            cause="No INA226 fitted. Expected until day 14.",
            next_test="Section 10.1 requires the deciding channel to resolve "
                      "at least twice as finely as the deviation threshold. "
                      "Until that is measured, motor verdicts carry unknown "
                      "uncertainty.",
        ))

    return findings


@dataclass
class Analysis:
    profile: Profile
    session: Session
    segments: list[Segment]
    findings: list[Finding]

    # Session-level figures, computed here rather than taken from the CSV so
    # the report says where each number came from.
    min_voltage_v: float = 0.0
    max_voltage_v: float = 0.0
    idle_current_a: float = 0.0
    peak_current_a: float = 0.0
    avg_current_a: float = 0.0

    @property
    def verdict(self) -> Verdict:
        return max((f.verdict for f in self.findings), default=Verdict.PASS)

    def firmware_summary(self) -> dict | None:
        return self.session.summaries[-1] if self.session.summaries else None


def analyse(session: Session, profile: Profile) -> Analysis:
    if not len(session):
        return Analysis(profile, session, [], [Finding(
            Verdict.INCONCLUSIVE, "Empty recording",
            "The file holds no samples.",
        )])

    segments = find_motor_segments(session, profile)
    pct = profile.analysis["peak_percentile"]

    findings: list[Finding] = []
    findings += _check_data_quality(session, profile)
    findings += _check_voltage(session, profile)
    findings += _check_motors(segments, profile)

    return Analysis(
        profile=profile,
        session=session,
        segments=segments,
        findings=findings,
        min_voltage_v=min(session.voltage_v),
        max_voltage_v=max(session.voltage_v),
        idle_current_a=percentile(session.current_a, 100.0 - pct),
        peak_current_a=percentile(session.current_a, pct),
        avg_current_a=statistics.fmean(session.current_a),
    )
