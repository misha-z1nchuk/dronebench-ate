"""
Tests for the day 13 noise tool. Standard library only.

    python3 -m unittest discover -s tools/report_generator
"""

import random
import statistics
import unittest

from noise import compare, measure, moving_average, samples_to_90


class NoiseTest(unittest.TestCase):
    def test_measure_reports_rms_about_the_mean_not_about_zero(self):
        # An offset is not noise. A zero 0.1 A off with no wiggle at all must
        # read as zero noise, or the zero error gets counted twice.
        times = [i * 2000 for i in range(100)]
        n = measure(times, [0.1] * 100)
        self.assertAlmostEqual(n.mean, 0.1)
        self.assertAlmostEqual(n.rms, 0.0)
        self.assertAlmostEqual(n.rate_hz, 500.0)

    def test_moving_average_of_a_step_reaches_the_top_in_exactly_n(self):
        step = [0.0] * 8 + [1.0] * 8
        out = moving_average(step, 8)
        self.assertEqual(out[0], 0.0)
        self.assertEqual(out[8], 1.0)
        self.assertEqual(samples_to_90(out[1:]), 8)  # 7.2 of 8 rounds up

    def test_both_filters_cut_white_noise_by_about_root_n(self):
        rng = random.Random(13)
        values = [rng.gauss(0.0, 1.0) for _ in range(20000)]
        for row in compare(values, 500.0, windows=(16, 64)):
            expected = 1.0 / row.window ** 0.5
            self.assertAlmostEqual(row.ma_rms, expected, delta=expected * 0.2)
            # For white noise the matched IIR is not merely close: its gain
            # is sqrt(alpha / (2 - alpha)), which at alpha = 2/(N+1) is
            # 1/sqrt(N) exactly. Equal noise, equal mean delay, different
            # shape — the test below.
            self.assertAlmostEqual(row.iir_rms, expected, delta=expected * 0.2)

    def test_the_iir_is_slower_to_90_percent_than_the_moving_average(self):
        # Same mean delay by construction, different shape. The exponential
        # starts moving twice as steeply (alpha ~ 2/N against 1/N) and then
        # pays for it with a tail: 90 % at ~1.15 N samples against 0.9 N.
        # This is the trade the tool exists to show.
        rows = compare([0.0] * 2000, 500.0, windows=(64,))
        self.assertGreater(rows[0].iir_t90_ms, rows[0].ma_t90_ms)
        self.assertAlmostEqual(rows[0].ma_t90_ms, 58 * 2.0)


if __name__ == "__main__":
    unittest.main()
