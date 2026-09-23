#!/usr/bin/env python3
"""
Draws three pictures from a Betaflight blackbox log, each answering one
question.

  python3 tools/blackbox/plot_log.py flight.BBL
  python3 tools/blackbox/plot_log.py flight.BBL --list
  python3 tools/blackbox/plot_log.py flight.BBL --session 4

The stock viewer draws every trace at once, which is why the first look at a
log tells you nothing: there is no question on the screen for the graph to
answer. Each plot here has exactly one.

    step_response.png   did the craft do what the sticks asked?
    spectrum.png        what do the filters actually remove?
    motors.png          is the work shared evenly between the four motors?

Most of this file is not drawing. It decides which samples are allowed to
count, because numbers taken raw out of a log are contaminated from three
directions, and every one of them produced a confidently wrong answer before
it was excluded:

    impacts     a wall strike is not a step response. One 43 deg/s command
                appeared to overshoot by 3995%; that was the craft hitting
                something, and the arithmetic had no way to know.
    the ground  motors spinning up before takeoff are not flight.
    the pilot   stopping a roll with opposite stick looks identical to the
                craft bouncing back on its own, unless the setpoint during
                the settle is checked. Two measurements out of eight were
                this, and they were the two that looked worst.

None of this is specific to one aircraft. The thresholds are in degrees per
second and throttle counts, which mean the same thing on any quad.
"""

from __future__ import annotations

import argparse
import sys
from dataclasses import dataclass
from pathlib import Path

import numpy as np

# Plots are written to files, never shown, so there is no display to find.
import matplotlib

matplotlib.use("Agg")
import matplotlib.pyplot as plt  # noqa: E402

# Below this the craft is on the ground or descending on idle: the motors are
# turning but nothing they do is flight, and the gyro is reading the floor.
THROTTLE_AIRBORNE = 1150

# A rate this far beyond any stick command is an impact, not a response. The
# gyro on this class of board rails around 2000 deg/s, so a genuine flick and
# a tumble are far apart and the line between them is not delicate.
IMPACT_RATE_DPS = 1000

# How much either side of an impact to distrust. The craft is still ringing
# after a strike, and was already disturbed before it.
IMPACT_GUARD_S = 0.3

# Smaller commands than this exist in every log and cannot be measured: at
# 40-75 deg/s the spread of the overshoot figure came out at 36 points around
# a median of 30, so the number says more about the disturbance than about the
# controller. Above 150 the same measurement repeats to within 4 points.
MIN_FLICK_DPS = 40
RELIABLE_FLICK_DPS = 150

# The settle window after the stick returns to centre. Long enough for a
# bounce to appear, short enough that the next input is usually not inside it.
FLICK_LEAD_S = 0.1
FLICK_TAIL_S = 0.5

# "Returned to centre" and "commanded the other way", both as a fraction of
# the peak, so they scale with the size of the input.
CENTRE_FRACTION = 0.2

# 1024 samples at ~1 kHz is roughly a second per window: fine enough to
# separate the motor peak from its neighbours, coarse enough to average
# several windows and get a stable picture.
NFFT = 1024

AXES = ("roll", "pitch", "yaw")


# --- loading ---------------------------------------------------------------


@dataclass
class Session:
    """One arm-to-disarm flight, with the derived masks it is read through."""

    index: int
    time_s: np.ndarray
    throttle: np.ndarray
    setpoint: list[np.ndarray]
    gyro: list[np.ndarray]
    gyro_unfilt: list[np.ndarray]
    motor: list[np.ndarray]
    erpm_mean: np.ndarray
    sample_rate_hz: float
    headers: dict
    trustworthy: np.ndarray  # airborne, and not near an impact

    @property
    def duration_s(self) -> float:
        return float(self.time_s[-1] - self.time_s[0])

    @property
    def motor_hz(self) -> float:
        """
        Electrical eRPM in the log is scaled by 100, and a brushless motor
        turns once per pole-pair, so the shaft frequency is what the airframe
        actually shakes at.
        """
        poles = header_int(self.headers, "motor_poles", 14)
        airborne = self.erpm_mean[self.trustworthy]
        if airborne.size == 0:
            return 0.0
        return float(airborne.mean() * 100.0 * 2.0 / poles / 60.0)


def header_int(headers: dict, name: str, default: int) -> int:
    """Headers arrive as text and a missing one must not be fatal."""
    raw = headers.get(name)
    if raw is None:
        return default
    try:
        return int(str(raw).split(",")[0])
    except (TypeError, ValueError):
        return default


def count_sessions(path: Path) -> int:
    """
    Every arm writes a fresh header block, so a file holds as many sessions as
    it holds headers. Counted from the bytes because a session late in the
    file can be truncated, and a truncated session must not hide the ones
    after it.
    """
    return path.read_bytes().count(b"H Product:Blackbox")


def load_session(path: Path, index: int) -> Session:
    try:
        from orangebox import Parser
    except ImportError:  # pragma: no cover - depends on the environment
        raise SystemExit(
            "orangebox is not installed. Run: make venv"
        )

    parser = Parser.load(str(path), log_index=index)
    names = list(parser.field_names)
    at = {name: i for i, name in enumerate(names)}

    def column(name: str) -> list:
        i = at[name]
        return [frame.data[i] for frame in rows]

    rows = list(parser.frames())
    if not rows:
        raise ValueError("session holds no frames")

    time_us = np.array([r.data[at["time"]] for r in rows], dtype=float)
    time_s = (time_us - time_us[0]) / 1e6
    throttle = np.array(column("rcCommand[3]"), dtype=float)

    setpoint = [np.array(column(f"setpoint[{a}]"), dtype=float) for a in range(3)]
    gyro = [np.array(column(f"gyroADC[{a}]"), dtype=float) for a in range(3)]
    unfilt = [np.array(column(f"gyroUnfilt[{a}]"), dtype=float) for a in range(3)]
    motor = [np.array(column(f"motor[{m}]"), dtype=float) for m in range(4)]
    erpm = np.mean([np.array(column(f"eRPM[{m}]"), dtype=float) for m in range(4)], axis=0)

    rate = 1.0 / float(np.diff(time_s).mean())

    # An impact disturbs the samples around it, not only the ones above the
    # threshold, so the exclusion is widened before it is applied.
    worst = np.max(np.abs(np.vstack(gyro)), axis=0)
    guard = int(IMPACT_GUARD_S * rate)
    hit = np.convolve((worst > IMPACT_RATE_DPS).astype(float),
                      np.ones(2 * guard + 1), mode="same") > 0
    trustworthy = (throttle > THROTTLE_AIRBORNE) & ~hit

    return Session(
        index=index, time_s=time_s, throttle=throttle, setpoint=setpoint,
        gyro=gyro, gyro_unfilt=unfilt, motor=motor, erpm_mean=erpm,
        sample_rate_hz=rate, headers=dict(parser.headers),
        trustworthy=trustworthy,
    )


# --- measuring -------------------------------------------------------------


@dataclass
class Flick:
    """One stick movement, and how faithfully the craft followed it."""

    axis: int
    at_s: float
    index: int
    target_dps: float
    gyro_peak_dps: float
    overshoot_pct: float
    bounce_dps: float
    pilot_counter_input: bool

    @property
    def reliable(self) -> bool:
        return abs(self.target_dps) >= RELIABLE_FLICK_DPS and not self.pilot_counter_input


def find_flicks(session: Session, axis: int) -> list[Flick]:
    setpoint = session.setpoint[axis]
    gyro = session.gyro[axis]
    rate = session.sample_rate_hz
    lead, tail = int(FLICK_LEAD_S * rate), int(FLICK_TAIL_S * rate)

    commanded = np.abs(setpoint) > MIN_FLICK_DPS
    found: list[Flick] = []

    for start, stop in contiguous_runs(commanded):
        peak = start + int(np.argmax(np.abs(setpoint[start:stop + 1])))
        lo, hi = max(0, peak - lead), min(len(setpoint), peak + tail)
        if not session.trustworthy[lo:hi].all():
            continue

        target = setpoint[peak]
        sign = np.sign(target)
        # The peak rate reached in the commanded direction, not the largest
        # absolute value: a swing the other way is not an overshoot of this.
        reached = sign * np.max(sign * gyro[lo:hi])
        overshoot = 100.0 * (abs(reached) - abs(target)) / abs(target)

        after_sp, after_gy = setpoint[peak:hi], gyro[peak:hi]
        centred = np.abs(after_sp) < CENTRE_FRACTION * abs(target)
        if not centred.any():
            continue  # the stick never came back inside this window
        settle = int(np.argmax(centred))

        # Whether the pilot actively steered the other way during the settle.
        # Without this the pilot's own correction is credited to the craft.
        counter = bool(((-sign * after_sp[settle:]) > CENTRE_FRACTION * abs(target)).any())
        bounce = max(0.0, float((-sign * after_gy[settle:]).max()))

        found.append(Flick(
            axis=axis, at_s=float(session.time_s[peak]), index=peak,
            target_dps=float(target), gyro_peak_dps=float(reached),
            overshoot_pct=float(overshoot), bounce_dps=bounce,
            pilot_counter_input=counter,
        ))
    return found


def contiguous_runs(mask: np.ndarray) -> list[tuple[int, int]]:
    """Inclusive [start, stop] index pairs for each run of True."""
    runs, i = [], 0
    while i < len(mask):
        if mask[i]:
            j = i
            while j + 1 < len(mask) and mask[j + 1]:
                j += 1
            runs.append((i, j))
            i = j + 1
        else:
            i += 1
    return runs


def averaged_spectrum(signal: np.ndarray, rate: float) -> tuple[np.ndarray, np.ndarray]:
    """Hann-windowed magnitude spectrum, averaged over overlapping windows."""
    signal = signal - signal.mean()
    if len(signal) < NFFT:
        return np.array([]), np.array([])
    window = np.hanning(NFFT)
    total, count = None, 0
    for start in range(0, len(signal) - NFFT, NFFT // 2):
        block = np.abs(np.fft.rfft(signal[start:start + NFFT] * window))
        total = block if total is None else total + block
        count += 1
    return np.fft.rfftfreq(NFFT, 1.0 / rate), total / count


def moving_average(values: np.ndarray, window: int) -> np.ndarray:
    """
    Centred running mean. The ends are averaged over fewer samples than the
    middle, which would pull them toward zero, so they are divided by how many
    samples actually contributed rather than by the window.
    """
    if window < 2 or values.size < window:
        return values
    kernel = np.ones(window)
    weight = np.convolve(np.ones_like(values), kernel, mode="same")
    return np.convolve(values, kernel, mode="same") / weight


# --- drawing ---------------------------------------------------------------


def plot_step_response(session: Session, directory: Path) -> list[str]:
    """
    The one picture that says whether the tune is any good: what was asked
    against what happened, around the sharpest usable movement.

    One file per axis, not one per log. The three axes are tuned separately
    and answer separately, and the largest movement in any flight is almost
    always yaw — picking a single winner per log would bury roll and pitch
    every time, which is exactly what the interesting question is about.
    """
    notes: list[str] = []
    rate = session.sample_rate_hz

    for axis in range(3):
        usable = [f for f in find_flicks(session, axis) if f.reliable]
        if not usable:
            continue
        best = max(usable, key=lambda f: abs(f.target_dps))

        lo = max(0, best.index - int(FLICK_LEAD_S * rate))
        hi = min(len(session.time_s), best.index + int(FLICK_TAIL_S * rate))
        t = (session.time_s[lo:hi] - session.time_s[best.index]) * 1000.0
        setpoint = session.setpoint[axis][lo:hi]
        gyro = session.gyro[axis][lo:hi]

        # Label the moment the craft peaked, which comes later than the moment
        # the stick did. Anchoring on the command's peak instead put the arrow
        # on an empty part of the chart, pointing at nothing.
        sign = 1.0 if best.target_dps > 0 else -1.0
        peak_i = int(np.argmax(sign * gyro))

        fig, ax = plt.subplots(figsize=(10, 5))
        ax.plot(t, setpoint, label="setpoint — what the stick asked for", lw=2)
        ax.plot(t, gyro, label="gyro — what the craft did", lw=1.4)
        ax.axhline(0, color="0.7", lw=0.8)
        ax.axhline(best.target_dps, color="0.6", ls=":", lw=1)
        # Offset toward zero, not along the command. A negative command peaks
        # at the bottom of the chart, and a label pushed further that way
        # landed on top of the x-axis tick labels.
        ax.annotate(
            f"overshoot {best.overshoot_pct:+.0f}%",
            xy=(t[peak_i], gyro[peak_i]),
            xytext=(18, -26 * sign),
            textcoords="offset points",
            color="0.15",
            bbox=dict(boxstyle="round,pad=0.25", fc="white", ec="none", alpha=0.75),
            arrowprops=dict(arrowstyle="->", color="0.35"),
        )
        ax.set_xlabel("milliseconds from the peak of the command")
        ax.set_ylabel("rate, deg/s")
        ax.set_title(
            f"session {session.index} · {AXES[axis]} · "
            f"command {best.target_dps:.0f} deg/s · reached {best.gyro_peak_dps:.0f} · "
            f"settles back past zero by {best.bounce_dps:.0f}"
        )
        ax.legend(loc="best")
        ax.grid(alpha=0.25)
        fig.tight_layout()
        fig.savefig(directory / f"step_response_{AXES[axis]}.png", dpi=130)
        plt.close(fig)

        notes.append(
            f"{AXES[axis]} {best.target_dps:+.0f} deg/s -> "
            f"{best.overshoot_pct:+.0f}% overshoot, {best.bounce_dps:.0f} deg/s bounce"
        )
    return notes


def plot_spectrum(session: Session, directory: Path) -> list[str]:
    """
    What the filters remove, and what they leave alone. The useful signal is
    the low end; everything above it is the airframe shaking.
    """
    good = session.trustworthy
    if good.sum() < NFFT * 2:
        return []
    freq, before = averaged_spectrum(session.gyro_unfilt[0][good], session.sample_rate_hz)
    _, after = averaged_spectrum(session.gyro[0][good], session.sample_rate_hz)
    if freq.size == 0:
        return []

    fig, ax = plt.subplots(figsize=(10, 5))
    ax.semilogy(freq, before, label="gyroUnfilt — before the filters", lw=1.2)
    ax.semilogy(freq, after, label="gyroADC — after the filters", lw=1.2)

    # Both markers sit on the floor of the chart and point away from each
    # other: the two lines can be only a few Hz apart, and labels anchored to
    # the frame got clipped by it.
    motor_hz = session.motor_hz
    if motor_hz > 0:
        ax.axvline(motor_hz, color="crimson", ls="--", lw=1.2)
        ax.annotate(f"motors ~{motor_hz:.0f} Hz",
                    xy=(motor_hz, 0.02), xycoords=("data", "axes fraction"),
                    xytext=(5, 0), textcoords="offset points",
                    color="crimson", va="bottom", ha="left", fontsize=9)
    notch_max = header_int(session.headers, "dyn_notch_max_hz", 0)
    if notch_max:
        ax.axvline(notch_max, color="darkgreen", ls=":", lw=1.2)
        ax.annotate(f"dyn notch stops at {notch_max} Hz",
                    xy=(notch_max, 0.02), xycoords=("data", "axes fraction"),
                    xytext=(-5, 0), textcoords="offset points",
                    color="darkgreen", va="bottom", ha="right", fontsize=9)

    nyquist = session.sample_rate_hz / 2
    ax.set_xlim(0, nyquist)
    ax.set_xlabel(f"Hz — the log samples at {session.sample_rate_hz:.0f} Hz, "
                  f"so nothing above {nyquist:.0f} Hz can be believed")
    ax.set_ylabel("amplitude (log scale)")
    ax.set_title(f"session {session.index} · roll gyro, before and after filtering")
    ax.legend(loc="upper right")
    ax.grid(alpha=0.25, which="both")
    fig.tight_layout()
    fig.savefig(directory / "spectrum.png", dpi=130)
    plt.close(fig)

    band = (freq >= 300) & (freq < 400)
    kept = after[band].sum() / before[band].sum() if before[band].sum() else float("nan")
    return [f"motors ~{motor_hz:.0f} Hz; 300-400 Hz band left at {100 * kept:.0f}% of raw"]


def plot_motors(session: Session, directory: Path) -> list[str]:
    """
    Four motors holding the craft up should work about equally hard. A gap
    that keeps the same shape all flight is the airframe, not the tune.

    Drawn as a half-second average over a faint raw trace. Sixty thousand
    points per motor at full weight is four traces on top of each other, and
    the gap between them is the entire question.
    """
    good = session.trustworthy
    if good.sum() == 0:
        return []
    t = session.time_s[good]
    window = max(2, int(0.5 * session.sample_rate_hz))

    fig, ax = plt.subplots(figsize=(10, 5))
    means, smoothed = [], []
    for m in range(4):
        values = session.motor[m][good]
        means.append(values.mean())
        colour = f"C{m}"
        ax.plot(t, values, lw=0.4, alpha=0.12, color=colour)
        trend = moving_average(values, window)
        smoothed.append(trend)
        ax.plot(t, trend, lw=1.9, color=colour,
                label=f"motor {m}  (mean {values.mean():.0f})")

    # Scaled to the averages. Takeoff and landing spikes span the whole output
    # range and would squash the hover — which is the part being compared —
    # into a band a few pixels tall.
    stack = np.concatenate(smoothed)
    margin = 0.15 * (stack.max() - stack.min() + 1)
    ax.set_ylim(stack.min() - margin, stack.max() + margin)

    spread = max(means) - min(means)
    ax.set_xlabel("seconds airborne  (faint: every sample · bold: half-second average)")
    ax.set_ylabel("motor output")
    ax.set_title(
        f"session {session.index} · spread between hardest and easiest motor: "
        f"{spread:.0f} counts ({100 * spread / np.mean(means):.1f}%)"
    )
    ax.legend(loc="best", fontsize=9)
    ax.grid(alpha=0.25)
    fig.tight_layout()
    fig.savefig(directory / "motors.png", dpi=130)
    plt.close(fig)
    return [f"motor means {[round(v) for v in means]}, "
            f"spread {100 * spread / np.mean(means):.1f}%"]


# --- driving ---------------------------------------------------------------


def describe(session: Session) -> str:
    counts = []
    for axis in range(3):
        flicks = find_flicks(session, axis)
        counts.append(f"{AXES[axis]} {sum(1 for f in flicks if f.reliable)}/{len(flicks)}")
    return (f"session {session.index}: {session.duration_s:5.1f} s, "
            f"{100 * session.trustworthy.mean():3.0f}% usable, "
            f"measurable flicks (reliable/all) — {', '.join(counts)}")


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[1])
    parser.add_argument("log", type=Path, help="a .BBL/.BFL file")
    parser.add_argument("--session", type=int, help="only this one (1-based)")
    parser.add_argument("--list", action="store_true",
                        help="say what is in the file and stop")
    parser.add_argument("-o", "--out", type=Path, default=Path("data/blackbox"),
                        help="where the plots go (default: data/blackbox/)")
    args = parser.parse_args()

    if not args.log.exists():
        raise SystemExit(f"no such file: {args.log}")

    total = count_sessions(args.log)
    if total == 0:
        raise SystemExit(f"{args.log} holds no blackbox headers — is it a log?")
    wanted = [args.session] if args.session else list(range(1, total + 1))
    print(f"{args.log.name}: {total} session(s)", file=sys.stderr)

    drawn = 0
    for index in wanted:
        try:
            session = load_session(args.log, index)
        except Exception as exc:
            # A session cut short by a full flash or a crash must not stop the
            # ones after it; this is common and is not an error in the tool.
            print(f"session {index}: unreadable ({exc.__class__.__name__}: {exc})",
                  file=sys.stderr)
            continue

        print(describe(session), file=sys.stderr)
        if args.list:
            continue

        directory = args.out / f"session_{index:02d}"
        directory.mkdir(parents=True, exist_ok=True)
        for name, draw in (("step_response", plot_step_response),
                           ("spectrum", plot_spectrum),
                           ("motors", plot_motors)):
            notes = draw(session, directory)
            if not notes:
                print(f"  {name}: nothing measurable in this session", file=sys.stderr)
                continue
            for note in notes:
                print(f"  {name}: {note}", file=sys.stderr)
            drawn += len(notes)
        print(f"  -> {directory}", file=sys.stderr)

    if not args.list:
        print(args.out)
    return 0 if (args.list or drawn) else 1


if __name__ == "__main__":
    raise SystemExit(main())
