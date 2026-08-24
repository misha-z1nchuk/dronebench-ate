"""
Tests for the diagnostic rules.

These decide whether a drone is handed back as healthy, so they are tested
against data built here rather than against a recording — a recording can only
contain the faults that happened to occur, and the interesting ones are the
faults nobody has produced yet.

Three of these tests are about the statistics rather than the plumbing: that a
percentile ignores a spike a maximum cannot, that a median ignores a spin-up
ramp a mean cannot, and — the one that matters most — that swapping the median
for a mean makes the rules condemn a healthy motor on nine corrupt samples.
The first two would still pass if the code used the wrong one; only the third
proves which is actually wired in.

    python3 -m unittest discover -s tools/report_generator

Needs no board and no matplotlib.
"""

import math
import statistics
import unittest
from pathlib import Path

from analysis import (
    Finding,
    expected_windows,
    Profile,
    Session,
    Verdict,
    analyse,
    find_motor_segments,
    percentile,
)

PROFILE_PATH = Path(__file__).resolve().parents[2] / \
    "test_profiles" / "meteor75pro.toml"

# The simulator's model, from firmware/core/measurements/simulator.c.
SETTLE_MS, MOTOR_MS, SLOT_MS = 1000, 2000, 2500
IDLE_A, MOTOR_A, SPINUP_MS = 0.38, 1.60, 150
V_OPEN, R_INTERNAL = 4.20, 0.060
DT_MS = 2


def build_session(motor_currents=(MOTOR_A,) * 4, tail_ms=1000,
                  v_open=V_OPEN, resistance=R_INTERNAL,
                  summaries=None, link_errors=0) -> Session:
    """
    A synthetic recording of the standard four-motor sequence.

    motor_currents gives each motor its own steady draw, so a dead motor is
    0.0 and a stiff one is larger. Everything else follows the simulator, and
    the voltage follows from the current through the pack's internal
    resistance — which is what makes sag appear only under load.
    """
    session = Session()
    total_ms = SETTLE_MS + len(motor_currents) * SLOT_MS + tail_ms

    for step in range(0, total_ms, DT_MS):
        current = IDLE_A
        if step >= SETTLE_MS:
            offset = step - SETTLE_MS
            index = offset // SLOT_MS
            into = offset % SLOT_MS
            if index < len(motor_currents) and into < MOTOR_MS:
                scale = 1.0 if into >= SPINUP_MS else into / SPINUP_MS
                current += motor_currents[index] * scale

        session.t_s.append(step / 1000.0)
        session.current_a.append(current)
        session.voltage_v.append(v_open - current * resistance)
        session.current_ref_a.append(math.nan)

    session.summaries = summaries if summaries is not None else [clean_summary(
        len(session.t_s))]
    session.link_errors = link_errors
    return session


def clean_summary(count: int) -> dict:
    return {
        "sample_count": str(count), "consumed_mah": "1.0",
        "consumed_wh": "0.004", "min_voltage_v": "4.081",
        "max_current_a": "1.980", "sensor_failures": "0",
        "rejected_time": "0", "rejected_value": "0", "gaps": "0",
        "dropped": "0",
    }


def titles(findings: list[Finding], verdict: Verdict) -> list[str]:
    return [f.title for f in findings if f.verdict is verdict]


class TestPercentile(unittest.TestCase):
    def test_it_interpolates_between_neighbours(self):
        self.assertAlmostEqual(percentile([0, 10], 50), 5.0)
        self.assertAlmostEqual(percentile([0, 1, 2, 3, 4], 50), 2.0)

    def test_the_ends_are_the_ends(self):
        self.assertEqual(percentile([3, 1, 2], 0), 1)
        self.assertEqual(percentile([3, 1, 2], 100), 3)

    def test_one_value_is_its_own_percentile(self):
        self.assertEqual(percentile([7.5], 99), 7.5)

    def test_an_empty_series_has_no_percentile(self):
        # Returning 0.0 would be a current reading of zero amps, which is a
        # perfectly plausible measurement and therefore the worst possible
        # answer to "there was no data".
        with self.assertRaises(ValueError):
            percentile([], 50)


class TestWhyPercentileNotMaximum(unittest.TestCase):
    """The theory of day 9, made falsifiable."""

    def test_one_bad_sample_moves_the_maximum_and_not_the_percentile(self):
        clean = [2.0] * 1000
        glitched = clean.copy()
        glitched[500] = 47.0        # one ADC sample, or one burst of EMI

        self.assertEqual(max(clean), 2.0)
        self.assertEqual(max(glitched), 47.0)          # ruined by one sample

        self.assertAlmostEqual(percentile(clean, 99), 2.0)
        self.assertAlmostEqual(percentile(glitched, 99), 2.0)  # unmoved

    def test_a_median_keeps_a_healthy_motor_out_of_the_report(self):
        """
        The choice, exercised through the rules rather than in isolation.

        Nine corrupted samples inside one motor's window — the kind this
        project produced for real on 2026-08-24, when a loose USB connector
        turned 4.081 into 4081. A mean over that window puts the motor 46%
        above its neighbours and the report condemns a part that is fine. The
        median does not move, because nine samples in a thousand cannot move
        it.
        """
        profile = Profile.load(PROFILE_PATH)
        session = build_session()

        window = [i for i, t in enumerate(session.t_s)
                  if 3.6 <= t <= 5.5]                   # inside motor 2
        for index in window[100:109]:
            session.current_a[index] = 100.0

        result = analyse(session, profile)
        self.assertEqual(
            [t for t in titles(result.findings, Verdict.FAIL)
             if "Motor" in t], [],
            "a healthy motor was condemned on nine corrupt samples")

    def test_the_percentile_still_follows_a_real_change(self):
        # Not merely insensitive: a peak that genuinely lasted moves it.
        real = [2.0] * 950 + [9.0] * 50
        self.assertGreater(percentile(real, 99), 8.0)


class TestWhyMedianNotMean(unittest.TestCase):
    """The other half of the theory."""

    def test_the_ramp_drags_the_mean_and_leaves_the_median(self):
        # Same motor, same steady draw, commanded for different lengths. Only
        # the amount of ramp in the window differs.
        def window(steady_ms):
            ramp = [MOTOR_A * (t / SPINUP_MS)
                    for t in range(0, SPINUP_MS, DT_MS)]
            return ramp + [MOTOR_A] * (steady_ms // DT_MS)

        short, long = window(400), window(2000)

        # The mean says these are different motors. They are not.
        self.assertGreater(
            abs(statistics.fmean(long) - statistics.fmean(short)), 0.15)

        # The median says they are the same, which is the truth.
        self.assertAlmostEqual(statistics.median(short),
                               statistics.median(long), delta=0.01)


class TestSegmentation(unittest.TestCase):
    def setUp(self):
        self.profile = Profile.load(PROFILE_PATH)

    def test_four_healthy_motors_are_found(self):
        segments = find_motor_segments(build_session(), self.profile)
        self.assertEqual(len(segments), 4)
        for segment in segments:
            self.assertAlmostEqual(segment.median_a, IDLE_A + MOTOR_A,
                                   delta=0.02)

    def test_a_dead_motor_leaves_a_gap_rather_than_a_low_reading(self):
        # The whole reason segments are counted. Motor 3 draws nothing, so
        # there is no third burst to measure — only an absence to notice.
        segments = find_motor_segments(
            build_session((MOTOR_A, MOTOR_A, 0.0, MOTOR_A)), self.profile)
        self.assertEqual(len(segments), 3)

    def test_a_brief_spike_is_not_a_motor(self):
        session = build_session()
        # 20 ms of raised current in the middle of an idle stretch.
        for index in range(10):
            session.current_a[index] = 5.0
        segments = find_motor_segments(session, self.profile)
        self.assertEqual(len(segments), 4)

    def test_a_flat_recording_yields_nothing(self):
        session = build_session()
        session.current_a = [IDLE_A] * len(session.t_s)
        self.assertEqual(find_motor_segments(session, self.profile), [])


class TestMotorRules(unittest.TestCase):
    def setUp(self):
        self.profile = Profile.load(PROFILE_PATH)

    def test_four_matched_motors_pass(self):
        result = analyse(build_session(), self.profile)
        self.assertEqual(titles(result.findings, Verdict.FAIL), [])
        self.assertEqual(titles(result.findings, Verdict.WARNING), [])

    def test_a_dead_motor_fails_and_names_the_absence(self):
        result = analyse(
            build_session((MOTOR_A, MOTOR_A, MOTOR_A, 0.0)), self.profile)
        failures = titles(result.findings, Verdict.FAIL)
        self.assertEqual(len(failures), 1)
        self.assertIn("never drew current", failures[0])
        self.assertIs(result.verdict, Verdict.FAIL)

    def test_the_missing_motor_gets_a_place_on_the_graph(self):
        # An absence has no position of its own, so it is inferred from the
        # cadence of the motors that did run. Without it the reader has to
        # work out which empty stretch of the graph the report meant.
        result = analyse(
            build_session((MOTOR_A, MOTOR_A, MOTOR_A, 0.0)), self.profile)
        missing = next(f for f in result.findings
                       if "never drew current" in f.title)
        self.assertIsNotNone(missing.span_s)
        start, _ = missing.span_s
        # Fourth slot: 1 s of settle plus three 2.5 s slots.
        self.assertAlmostEqual(start, 8.5, delta=0.3)

    def test_a_silent_motor_in_the_middle_is_placed_from_the_gap(self):
        # Harder than a missing last motor: the cadence has to be recovered
        # from a run that is twice as long as it should be.
        segments = find_motor_segments(
            build_session((MOTOR_A, 0.0, MOTOR_A, MOTOR_A)), self.profile)
        windows = expected_windows(segments, 4)
        self.assertEqual(len(windows), 1)
        self.assertAlmostEqual(windows[0][0], 3.58, delta=0.3)

    def test_nothing_is_invented_when_there_is_no_cadence(self):
        # One burst gives no interval, so there is no rhythm to extrapolate
        # from — and guessing one would put a measurement on the graph that
        # nobody made.
        self.assertEqual(expected_windows([], 4), [])
        segments = find_motor_segments(
            build_session((MOTOR_A, 0.0, 0.0, 0.0)), self.profile)
        self.assertEqual(expected_windows(segments, 4), [])

    def test_a_motor_drawing_far_too_much_fails(self):
        # 1.60 -> 2.60 A on one motor is well past the 30% fail threshold once
        # the idle floor is included.
        result = analyse(
            build_session((MOTOR_A, 2.60, MOTOR_A, MOTOR_A)), self.profile)
        failures = titles(result.findings, Verdict.FAIL)
        self.assertTrue(any("Motor 2" in t and "above" in t
                            for t in failures), failures)

    def test_a_mild_deviation_warns_without_failing(self):
        # +20% on the motor's own draw: past warn (15) and short of fail (30)
        # once the shared idle current is taken into account.
        result = analyse(
            build_session((MOTOR_A, MOTOR_A * 1.28, MOTOR_A, MOTOR_A)),
            self.profile)
        self.assertEqual(titles(result.findings, Verdict.FAIL), [])
        self.assertTrue(any("Motor 2" in t
                            for t in titles(result.findings, Verdict.WARNING)))

    def test_the_verdict_marks_where_it_was_decided(self):
        # A finding without a span cannot be drawn on the graph, and a report
        # whose graph does not show what it claims stops being believed.
        result = analyse(
            build_session((MOTOR_A, 2.60, MOTOR_A, MOTOR_A)), self.profile)
        motor = next(f for f in result.findings if "Motor 2" in f.title)
        self.assertIsNotNone(motor.span_s)
        start, end = motor.span_s
        self.assertGreater(end, start)


class TestVoltageRules(unittest.TestCase):
    def setUp(self):
        self.profile = Profile.load(PROFILE_PATH)

    def test_a_sagging_pack_fails_on_undervoltage(self):
        # Five times the internal resistance: the pack holds its open-circuit
        # voltage and collapses under load, which is why sag can only be
        # measured while drawing current.
        result = analyse(
            build_session(v_open=3.60, resistance=R_INTERNAL * 5),
            self.profile)
        self.assertTrue(any("undervoltage" in t.lower()
                            for t in titles(result.findings, Verdict.FAIL)))

    def test_an_impossible_voltage_is_blamed_on_the_bench(self):
        # Sending someone to buy a battery over a wrong divider ratio is the
        # expensive kind of wrong answer, so this must not read as a battery
        # fault.
        result = analyse(build_session(v_open=12.0), self.profile)
        failure = next(f for f in result.findings
                       if f.verdict is Verdict.FAIL)
        self.assertIn("calibration", failure.cause.lower())
        self.assertNotIn("undervoltage", failure.title.lower())

    def test_an_impossible_voltage_silences_the_other_voltage_rules(self):
        result = analyse(build_session(v_open=12.0), self.profile)
        voltage_findings = [f for f in result.findings
                            if "olt" in f.title or "harge" in f.title]
        self.assertEqual(len(voltage_findings), 1)


class TestHonesty(unittest.TestCase):
    """
    The rules that stop a green report from being produced over data that
    cannot support one. Every one of these would be easier to leave out, and
    leaving them out is how a bench certifies a drone it never measured.
    """

    def setUp(self):
        self.profile = Profile.load(PROFILE_PATH)

    def test_inconclusive_outranks_pass(self):
        self.assertGreater(Verdict.INCONCLUSIVE, Verdict.PASS)
        self.assertGreater(Verdict.FAIL, Verdict.WARNING)

    def test_a_threshold_with_no_baseline_cannot_pass_anything(self):
        profile = Profile.load(PROFILE_PATH)
        profile.limits["motor_deviation_warn_percent"] = "tbd"
        result = analyse(build_session(), profile)
        self.assertIs(result.verdict, Verdict.INCONCLUSIVE)

    def test_a_session_with_no_accepted_samples_is_not_a_pass(self):
        # What `simulate off` produces today: every reading refused, so every
        # figure reads 0.000 — including a minimum voltage that looks exactly
        # like a flat pack.
        dead = clean_summary(0)
        dead["sensor_failures"] = "6000"
        dead["min_voltage_v"] = "0.000"
        result = analyse(build_session(summaries=[dead]), self.profile)
        self.assertTrue(any("no valid samples" in f.detail.lower()
                            or "recorded no valid samples" in f.title.lower()
                            for f in result.findings))
        self.assertGreater(result.verdict, Verdict.PASS)

    def test_a_corrupted_link_is_reported_before_anything_is_believed(self):
        result = analyse(build_session(link_errors=201), self.profile)
        first = result.findings[0]
        self.assertIn("corrupted", first.title.lower())
        self.assertIn("Reseat", first.next_test)

    def test_a_missing_reference_channel_is_stated_not_assumed(self):
        result = analyse(build_session(), self.profile)
        self.assertTrue(any("one current channel" in f.title.lower()
                            for f in result.findings))

    def test_refused_samples_cost_trust_even_when_the_drone_is_fine(self):
        noisy = clean_summary(6000)
        noisy["sensor_failures"] = "12"
        result = analyse(build_session(summaries=[noisy]), self.profile)
        self.assertTrue(any("not measured" in t.lower()
                            for t in titles(result.findings, Verdict.WARNING)))

    def test_samples_lost_in_the_link_are_noticed_from_the_count(self):
        # The firmware counted more than reached the file. Invisible from
        # either side alone — the same argument as section 10.1, applied to
        # sample counts instead of amps.
        session = build_session()
        session.summaries = [clean_summary(len(session.t_s) + 500)]
        result = analyse(session, self.profile)
        self.assertTrue(any("lost between board and host" in t
                            for t in titles(result.findings, Verdict.WARNING)))

    def test_an_empty_recording_is_inconclusive_rather_than_clean(self):
        result = analyse(Session(), self.profile)
        self.assertIs(result.verdict, Verdict.INCONCLUSIVE)


if __name__ == "__main__":
    unittest.main()
