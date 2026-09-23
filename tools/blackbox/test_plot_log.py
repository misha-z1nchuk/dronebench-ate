#!/usr/bin/env python3
"""
Tests for the blackbox plotter's judgement, not its drawing.

Every case here builds a synthetic flight in memory, so none of them needs a
log file or the decoder that reads one — the same habit as the logger's tests,
which need neither a board nor pyserial. numpy is the one dependency, and it
arrives with `make venv`.

What is worth testing is not the arithmetic of an overshoot, which is one
subtraction. It is the three exclusions, because each of them was added after
it produced a confident wrong answer:

    an impact must not be read as a step response
    the ground must not be read as flight
    the pilot's own correction must not be credited to the craft
"""

from __future__ import annotations

import sys
import unittest
from pathlib import Path

import numpy as np

sys.path.insert(0, str(Path(__file__).resolve().parent))

from plot_log import (  # noqa: E402
    MIN_FLICK_DPS,
    Session,
    contiguous_runs,
    find_flicks,
    header_int,
    moving_average,
)

RATE_HZ = 1000.0
LENGTH = 3000
ROLL = 0


def make_session(setpoint: np.ndarray, gyro: np.ndarray,
                 trustworthy: np.ndarray | None = None) -> Session:
    """A flight that is airborne and undisturbed unless a test says otherwise."""
    zeros = np.zeros(LENGTH)
    if trustworthy is None:
        trustworthy = np.ones(LENGTH, dtype=bool)
    return Session(
        index=1,
        time_s=np.arange(LENGTH) / RATE_HZ,
        throttle=np.full(LENGTH, 1400.0),
        setpoint=[setpoint, zeros.copy(), zeros.copy()],
        gyro=[gyro, zeros.copy(), zeros.copy()],
        gyro_unfilt=[gyro.copy(), zeros.copy(), zeros.copy()],
        motor=[np.full(LENGTH, 900.0) for _ in range(4)],
        erpm_mean=np.full(LENGTH, 1100.0),
        sample_rate_hz=RATE_HZ,
        headers={"motor_poles": "12", "dyn_notch_max_hz": "300"},
        trustworthy=trustworthy,
    )


def flick(amplitude: float, peak_at: int = 1000, width: int = 100,
          overshoot: float = 0.10) -> tuple[np.ndarray, np.ndarray]:
    """
    A stick pulse and a response that overshoots it by a known fraction, then
    returns to zero without crossing it.
    """
    setpoint = np.zeros(LENGTH)
    setpoint[peak_at - width // 2: peak_at + width // 2] = amplitude

    gyro = np.zeros(LENGTH)
    rise = np.linspace(0.0, amplitude * (1 + overshoot), width)
    gyro[peak_at - width // 2: peak_at + width // 2] = rise
    fall = np.linspace(amplitude * (1 + overshoot), 0.0, 150)
    gyro[peak_at + width // 2: peak_at + width // 2 + 150] = fall
    return setpoint, gyro


class ContiguousRuns(unittest.TestCase):
    def test_finds_each_run_inclusively(self):
        mask = np.array([0, 1, 1, 0, 0, 1, 0, 1, 1, 1], dtype=bool)
        self.assertEqual(contiguous_runs(mask), [(1, 2), (5, 5), (7, 9)])

    def test_empty_mask_has_no_runs(self):
        self.assertEqual(contiguous_runs(np.zeros(10, dtype=bool)), [])


class MovingAverage(unittest.TestCase):
    def test_constant_signal_survives_its_own_edges(self):
        """
        The bug this guards: convolving with a plain window divides the ends
        by the full width even though fewer samples reached them, which drags
        the first and last half-window toward zero and invents a dip at the
        start of every flight.
        """
        flat = np.full(500, 900.0)
        smoothed = moving_average(flat, 50)
        self.assertTrue(np.allclose(smoothed, 900.0))

    def test_shorter_than_window_is_returned_unchanged(self):
        short = np.array([1.0, 2.0, 3.0])
        self.assertTrue(np.array_equal(moving_average(short, 50), short))


class HeaderInt(unittest.TestCase):
    def test_reads_a_plain_value(self):
        self.assertEqual(header_int({"motor_poles": "14"}, "motor_poles", 0), 14)

    def test_takes_the_first_of_a_list(self):
        # Several headers are comma-separated pairs, like "100,600".
        self.assertEqual(header_int({"dyn_notch": "100,600"}, "dyn_notch", 0), 100)

    def test_missing_or_unparsable_falls_back(self):
        self.assertEqual(header_int({}, "absent", 7), 7)
        self.assertEqual(header_int({"x": "none"}, "x", 7), 7)


class FindFlicks(unittest.TestCase):
    def test_measures_a_clean_flick(self):
        setpoint, gyro = flick(200.0, overshoot=0.10)
        found = find_flicks(make_session(setpoint, gyro), ROLL)
        self.assertEqual(len(found), 1)
        self.assertAlmostEqual(found[0].overshoot_pct, 10.0, delta=1.0)
        self.assertFalse(found[0].pilot_counter_input)
        self.assertEqual(found[0].bounce_dps, 0.0)
        self.assertTrue(found[0].reliable)

    def test_a_command_too_small_to_measure_is_not_reliable(self):
        """
        Below 150 deg/s the same measurement scattered by 36 points, so it is
        still reported but must not be treated as a result.
        """
        setpoint, gyro = flick(100.0)
        found = find_flicks(make_session(setpoint, gyro), ROLL)
        self.assertEqual(len(found), 1)
        self.assertFalse(found[0].reliable)

    def test_a_command_below_the_floor_is_not_a_flick_at_all(self):
        setpoint, gyro = flick(MIN_FLICK_DPS - 10.0)
        self.assertEqual(find_flicks(make_session(setpoint, gyro), ROLL), [])

    def test_pilot_steering_back_is_not_the_craft_bouncing(self):
        """
        Two of eight real measurements were this, and they were the two that
        looked worst. Without the check the pilot's correction is read as the
        craft failing to settle.
        """
        setpoint, gyro = flick(200.0)
        setpoint[1150:1250] = -120.0   # stopping the roll with opposite stick
        gyro[1150:1250] = -120.0
        found = find_flicks(make_session(setpoint, gyro), ROLL)
        self.assertTrue(found[0].pilot_counter_input)
        self.assertFalse(found[0].reliable, "a contaminated flick must not count")

    def test_a_flick_touching_an_impact_is_discarded(self):
        setpoint, gyro = flick(200.0)
        trustworthy = np.ones(LENGTH, dtype=bool)
        trustworthy[1200:1300] = False      # inside the settle window
        found = find_flicks(make_session(setpoint, gyro, trustworthy), ROLL)
        self.assertEqual(found, [], "an impact nearby invalidates the whole window")

    def test_genuine_bounce_past_zero_is_reported(self):
        setpoint, gyro = flick(200.0)
        gyro[1150:1250] = -60.0             # craft swings back on its own
        found = find_flicks(make_session(setpoint, gyro), ROLL)
        self.assertFalse(found[0].pilot_counter_input)
        self.assertAlmostEqual(found[0].bounce_dps, 60.0, delta=1.0)


if __name__ == "__main__":
    unittest.main()
