"""
Tests for the host parser.

Weighted towards corrupt input on purpose, the same way the firmware's tests
are: the happy path runs 500 times a second and announces its own failures,
while the broken paths run exactly when something has already gone wrong and
nobody is watching.

    python3 -m unittest discover -s tools/serial_logger

Needs no board and no pyserial.
"""

import math
import unittest

from protocol import (
    LINE_MAX,
    LineAssembler,
    LineKind,
    Parser,
    ProtocolError,
    Summary,
)

HEADER = "TLM,v=2,rate_hz=500,fields=t_us|v|i|i_ref"

# Copied verbatim from a board running the normal profile on 2026-08-24.
# Synthetic input can only ever contain the cases someone thought of.
REAL_LINES = [
    HEADER,
    "S,40118329,4.081,1.980,nan",
    "S,40120329,4.081,1.980,nan",
    "U,n=1000,mah=0.621,wh=0.0025,vmin=4.081,imax=1.980,"
    "sensor=0,rej_time=0,rej_val=0,gaps=0,drop=0",
]


def ready() -> Parser:
    parser = Parser()
    parser.feed_line(HEADER)
    return parser


class TestHeader(unittest.TestCase):
    def test_a_valid_header_is_accepted_and_remembered(self):
        parser = Parser()
        kind, header = parser.feed_line(HEADER)
        self.assertIs(kind, LineKind.HEADER)
        self.assertEqual(header.version, 2)
        self.assertEqual(header.rate_hz, 500)
        self.assertIsNotNone(parser.header)

    def test_an_unknown_version_is_refused_in_both_directions(self):
        # v1 put three fields on an S line, so reading a v1 log as v2 would
        # silently lose the reference channel rather than fail.
        for version in (1, 3):
            parser = Parser()
            with self.assertRaises(ProtocolError):
                parser.feed_line(f"TLM,v={version},rate_hz=500,fields=x")
            self.assertIsNone(parser.header)
            self.assertEqual(parser.lines_rejected, 1)

    def test_unknown_keys_are_ignored_so_a_later_version_can_add_them(self):
        parser = Parser()
        kind, header = parser.feed_line(f"{HEADER},future_field=7")
        self.assertIs(kind, LineKind.HEADER)
        self.assertEqual(header.rate_hz, 500)

    def test_data_before_a_header_is_refused(self):
        parser = Parser()
        with self.assertRaises(ProtocolError):
            parser.feed_line("S,1000,4.000,1.000,0.000")
        self.assertEqual(parser.lines_rejected, 1)

    def test_a_reset_forces_the_next_stream_to_negotiate_again(self):
        parser = ready()
        parser.reset()
        with self.assertRaises(ProtocolError):
            parser.feed_line("S,1000,4.000,1.000,0.000")


class TestSample(unittest.TestCase):
    def test_a_sample_carries_four_fields(self):
        _, sample = ready().feed_line("S,40118329,4.081,1.980,1.975")
        self.assertEqual(sample.t_us, 40118329)
        self.assertAlmostEqual(sample.voltage_v, 4.081)
        self.assertAlmostEqual(sample.current_a, 1.980)
        self.assertAlmostEqual(sample.current_ref_a, 1.975)

    def test_the_wrong_number_of_fields_is_refused(self):
        parser = ready()
        for body in ("S,1000,4.000,1.000", "S,1000,4.000",
                     "S,1000,4.000,1.000,0.000,9.999", "S,1000", "S"):
            with self.assertRaises(ProtocolError, msg=body):
                parser.feed_line(body)

    def test_a_non_numeric_field_is_refused(self):
        parser = ready()
        for body in ("S,1000,banana,1.000,0.000", "S,abc,4.000,1.000,0.000",
                     "S,1000,,1.000,0.000", "S,1000,4.000,1.000,"):
            with self.assertRaises(ProtocolError, msg=body):
                parser.feed_line(body)

    def test_a_line_cut_off_mid_number_is_refused(self):
        # What a link problem actually looks like: the line simply stops, and
        # "4.0" on its own is a perfectly valid number.
        parser = ready()
        for body in ("S,1000,4.000,1.0", "S,10"):
            with self.assertRaises(ProtocolError, msg=body):
                parser.feed_line(body)

    def test_an_absent_reference_reads_as_nan_not_as_zero(self):
        # The normal case until day 14. A shorted shunt reading 0 A and a
        # missing chip are different faults and must stay distinguishable.
        _, sample = ready().feed_line("S,1000,4.000,1.000,nan")
        self.assertTrue(math.isnan(sample.current_ref_a))

        _, sample = ready().feed_line("S,1000,4.000,1.000,0.000")
        self.assertFalse(math.isnan(sample.current_ref_a))
        self.assertEqual(sample.current_ref_a, 0.0)

    def test_nan_is_refused_on_the_required_channels(self):
        parser = ready()
        for body in ("S,1000,nan,1.000,0.000", "S,1000,4.000,nan,0.000"):
            with self.assertRaises(ProtocolError, msg=body):
                parser.feed_line(body)

    def test_infinity_is_refused_everywhere(self):
        # float("1e400") is inf and raises nothing — the shape a calibration
        # coefficient divided by zero takes on its way to the wire.
        parser = ready()
        for body in ("S,1000,inf,1.000,0.000", "S,1000,4.000,1e400,0.000",
                     "S,1000,4.000,1.000,-inf"):
            with self.assertRaises(ProtocolError, msg=body):
                parser.feed_line(body)

    def test_negative_current_parses_because_the_sensor_is_bidirectional(self):
        _, sample = ready().feed_line("S,1000,4.000,-0.250,-0.249")
        self.assertAlmostEqual(sample.current_a, -0.250)


class TestSummary(unittest.TestCase):
    BODY = ("U,n=1000,mah=0.621,wh=0.0025,vmin=4.081,imax=1.980,"
            "sensor=0,rej_time=0,rej_val=0,gaps=0,drop=0")

    def test_a_summary_carries_all_ten_fields(self):
        _, summary = ready().feed_line(self.BODY)
        self.assertEqual(summary.sample_count, 1000)
        self.assertAlmostEqual(summary.consumed_mah, 0.621)
        self.assertAlmostEqual(summary.consumed_wh, 0.0025)
        self.assertEqual(summary.dropped, 0)

    def test_field_order_is_not_part_of_the_contract(self):
        _, summary = ready().feed_line(
            "U,drop=1,gaps=2,rej_val=3,rej_time=4,sensor=5,imax=8.200,"
            "vmin=3.710,wh=0.0510,mah=12.400,n=498"
        )
        self.assertEqual(summary.sample_count, 498)
        self.assertEqual(summary.dropped, 1)
        self.assertEqual(summary.refused, 12)

    def test_a_missing_field_is_refused(self):
        parser = ready()
        with self.assertRaises(ProtocolError):
            parser.feed_line("U,mah=12.400,wh=0.0510")

    def test_a_repeated_field_is_refused(self):
        # Two values for one field means the line is not what it claims; the
        # later one silently winning is how a wrong number gets logged.
        parser = ready()
        with self.assertRaises(ProtocolError):
            parser.feed_line(self.BODY + ",n=7")

    def test_a_garbled_value_is_refused_even_when_nothing_is_missing(self):
        parser = ready()
        with self.assertRaises(ProtocolError):
            parser.feed_line(self.BODY.replace("mah=0.621", "mah=x"))

    def test_a_field_without_an_equals_sign_is_refused(self):
        parser = ready()
        with self.assertRaises(ProtocolError):
            parser.feed_line(self.BODY + ",oops")


class TestTrustworthiness(unittest.TestCase):
    def make(self, **kw) -> Summary:
        base = dict(sample_count=1000, consumed_mah=0.6, consumed_wh=0.002,
                    min_voltage_v=4.08, max_current_a=1.98,
                    sensor_failures=0, rejected_time=0, rejected_value=0,
                    gaps=0, dropped=0)
        base.update(kw)
        return Summary(**base)

    def test_a_session_with_no_samples_is_not_trustworthy(self):
        # The whole reason sample_count is on the wire: with n=0 the firmware
        # reports vmin=0.000, which is indistinguishable from a flat pack.
        self.assertFalse(self.make(sample_count=0, min_voltage_v=0.0,
                                   sensor_failures=500).is_trustworthy())

    def test_any_refusal_or_drop_costs_trust(self):
        self.assertTrue(self.make().is_trustworthy())
        for field in ("sensor_failures", "rejected_time", "rejected_value",
                      "dropped"):
            self.assertFalse(self.make(**{field: 1}).is_trustworthy(),
                             msg=field)


class TestLineFraming(unittest.TestCase):
    def test_a_line_split_across_chunks_is_reassembled(self):
        # The reason readline() is not used: a 28-byte line routinely arrives
        # as two reads, and half of it parses as nothing.
        assembler = LineAssembler()
        self.assertEqual(list(assembler.feed(b"S,1000,4.0")), [])
        self.assertEqual(list(assembler.feed(b"00,1.000,0.000\n")),
                         ["S,1000,4.000,1.000,0.000"])

    def test_several_lines_in_one_chunk_all_come_out(self):
        assembler = LineAssembler()
        lines = list(assembler.feed(b"one\ntwo\nthree\n"))
        self.assertEqual(lines, ["one", "two", "three"])

    def test_an_unterminated_tail_is_held_not_emitted(self):
        assembler = LineAssembler()
        self.assertEqual(list(assembler.feed(b"done\npartial")), ["done"])
        self.assertEqual(assembler.pending(), len("partial"))

    def test_bytes_with_no_line_ending_are_dropped_and_counted(self):
        # Wrong baud rate produces exactly this: plenty of bytes, no newlines.
        assembler = LineAssembler(limit=64)
        list(assembler.feed(b"x" * 200))
        self.assertEqual(assembler.overruns, 1)
        self.assertEqual(assembler.pending(), 0)

    def test_invalid_bytes_do_not_kill_the_reader(self):
        assembler = LineAssembler()
        lines = list(assembler.feed(b"S,1000,4.0\xff0,1.000,0.000\nS,2\n"))
        self.assertEqual(len(lines), 2)
        with self.assertRaises(ProtocolError):
            ready().feed_line(lines[0])

    def test_carriage_returns_are_tolerated(self):
        _, sample = ready().feed_line("S,1000,4.000,1.000,0.000\r\n")
        self.assertEqual(sample.t_us, 1000)


class TestCounters(unittest.TestCase):
    def test_blank_lines_are_neither_parsed_nor_rejected(self):
        parser = ready()
        for line in ("", "\n", "\r\n"):
            kind, _ = parser.feed_line(line)
            self.assertIs(kind, LineKind.BLANK)
        self.assertEqual(parser.lines_rejected, 0)

    def test_an_over_long_line_is_refused_without_being_parsed(self):
        parser = ready()
        with self.assertRaises(ProtocolError):
            parser.feed_line("S," + "9" * LINE_MAX)
        self.assertEqual(parser.lines_rejected, 1)

    def test_the_parser_counts_what_it_kept_and_what_it_threw_away(self):
        parser = Parser()
        for line in REAL_LINES:
            parser.feed_line(line)
        with self.assertRaises(ProtocolError):
            parser.feed_line("S,bad")
        parser.feed_line("")

        # The header counts as parsed; the blank line counts as neither.
        self.assertEqual(parser.lines_parsed, len(REAL_LINES))
        self.assertEqual(parser.lines_rejected, 1)


class TestAgainstRealBoardOutput(unittest.TestCase):
    def test_captured_output_parses_byte_by_byte(self):
        """
        The bytes as a board actually produced them, fed one at a time — the
        worst fragmentation a port can inflict.
        """
        raw = ("\n".join(REAL_LINES) + "\n").encode()
        assembler = LineAssembler()
        parser = Parser()
        kinds = []

        for index in range(len(raw)):
            for line in assembler.feed(raw[index : index + 1]):
                kinds.append(parser.feed_line(line)[0])

        self.assertEqual(kinds, [LineKind.HEADER, LineKind.SAMPLE,
                                 LineKind.SAMPLE, LineKind.SUMMARY])
        self.assertEqual(parser.lines_rejected, 0)


if __name__ == "__main__":
    unittest.main()
