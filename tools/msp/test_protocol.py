"""
Tests for the MSP frame layer. No flight controller, no serial port.

Every one of these is a mistake that a board would report as silence. A wrong
checksum, a payload read one byte short, a refusal mistaken for data — none of
them produce an error message from the far end, they produce no reply at all,
or a plausible one. Debugging that against hardware means guessing which of
the two ends is wrong; here the answer is known in advance.
"""

from __future__ import annotations

import struct
import unittest

import protocol as p


def reply_v1(command: int, payload: bytes = b"") -> bytes:
    """A frame shaped as the flight controller's answer, for decode() tests."""
    body = bytes([len(payload), command]) + payload
    return b"$M>" + body + bytes([p.xor_over(body)])


def reply_v2(command: int, payload: bytes = b"", flag: int = 0) -> bytes:
    body = struct.pack("<BHH", flag, command, len(payload)) + payload
    return b"$X>" + body + bytes([p.crc8_over(body)])


class TestChecksums(unittest.TestCase):
    def test_crc8_single_byte_matches_hand_calculation(self):
        # 0x01 shifted left seven times reaches 0x80; the eighth shift sets
        # the high bit, so the polynomial is applied once: 0x00 ^ 0xD5.
        self.assertEqual(p.crc8_dvb_s2(0, 0x01), 0xD5)

    def test_crc8_of_nothing_is_the_seed(self):
        self.assertEqual(p.crc8_over(b""), 0)

    def test_xor_is_order_independent(self):
        self.assertEqual(p.xor_over(b"\x01\x02\x03"),
                         p.xor_over(b"\x03\x01\x02"))


class TestEncode(unittest.TestCase):
    def test_v1_empty_request(self):
        frame = p.encode_v1(p.Cmd.API_VERSION.value)
        self.assertEqual(frame, b"$M<\x00\x01\x01")

    def test_v1_carries_payload_and_checksum(self):
        frame = p.encode_v1(214, b"\xe8\x03")
        self.assertEqual(frame[:5], b"$M<\x02\xd6")
        self.assertEqual(frame[-1], 0x02 ^ 0xD6 ^ 0xE8 ^ 0x03)

    def test_v1_refuses_a_payload_it_cannot_describe(self):
        with self.assertRaises(p.MspError):
            p.encode_v1(1, b"\x00" * 256)

    def test_v1_refuses_a_command_it_cannot_address(self):
        with self.assertRaises(p.MspError):
            p.encode_v1(256)

    def test_v2_carries_a_command_above_255(self):
        frame = p.encode_v2(0x1F00)
        self.assertEqual(frame[:3], b"$X<")
        self.assertEqual(struct.unpack_from("<H", frame, 4)[0], 0x1F00)


class TestDecode(unittest.TestCase):
    def test_v1_round_trip(self):
        frame = p.decode(reply_v1(2, b"BTFL"))
        self.assertEqual(frame.command, 2)
        self.assertEqual(frame.payload, b"BTFL")
        self.assertEqual(frame.version, 1)

    def test_v2_round_trip(self):
        frame = p.decode(reply_v2(0x1F00, b"\x01\x02\x03"))
        self.assertEqual(frame.command, 0x1F00)
        self.assertEqual(frame.payload, b"\x01\x02\x03")
        self.assertEqual(frame.version, 2)

    def test_dollar_inside_payload_does_not_end_the_frame(self):
        # The reason framing is by declared length and never by scanning for
        # a delimiter: 0x24 is a perfectly ordinary payload byte.
        payload = b"\x24\x4d\x3e\x24"
        self.assertEqual(p.decode(reply_v1(104, payload)).payload, payload)

    def test_refusal_raises_instead_of_decoding_to_empty(self):
        # '$M!' differs from '$M>' by one byte and carries no payload. Read as
        # a reply it is a well-formed frame whose fields all decode to zero.
        with self.assertRaises(p.MspRefused):
            p.decode(b"$M!\x00\x63\x63")

    def test_refusal_names_the_command(self):
        with self.assertRaises(p.MspRefused) as caught:
            p.decode(b"$M!\x00\xd6\xd6")
        self.assertIn("214", str(caught.exception))

    def test_request_direction_is_not_a_reply(self):
        with self.assertRaises(p.MspError):
            p.decode(p.encode_v1(1))

    def test_bad_checksum_is_refused(self):
        frame = bytearray(reply_v1(2, b"BTFL"))
        frame[-1] ^= 0xFF
        with self.assertRaises(p.MspError):
            p.decode(bytes(frame))

    def test_truncated_payload_is_refused(self):
        # Declares four bytes, carries three. Without the length check this
        # decodes to a short string and looks like a different flight stack.
        with self.assertRaises(p.MspError):
            p.decode(b"$M>\x04\x02BTF\x00")

    def test_unknown_header_is_refused(self):
        with self.assertRaises(p.MspError):
            p.decode(b"$Z>\x00\x01\x01")


class TestPayloads(unittest.TestCase):
    def test_api_version(self):
        version = p.decode_api_version(b"\x00\x01\x2e")
        self.assertEqual((version.msp_protocol, version.major, version.minor),
                         (0, 1, 46))

    def test_fc_variant(self):
        self.assertEqual(p.decode_fc_variant(b"BTFL"), "BTFL")

    def test_fc_version(self):
        self.assertEqual(str(p.decode_fc_version(b"\x04\x05\x01")), "4.5.1")

    def test_short_payload_is_refused_not_padded(self):
        with self.assertRaises(p.MspError):
            p.decode_fc_variant(b"BT")

    def test_status_decodes_sensors_and_keeps_the_tail(self):
        payload = struct.pack("<HHHIB", 312, 0, 0b100001, 0, 1) + b"\xaa\xbb"
        status = p.decode_status(payload)
        self.assertEqual(status.cycle_time_us, 312)
        self.assertTrue(status.sensors()["accelerometer"])
        self.assertTrue(status.sensors()["gyroscope"])
        self.assertFalse(status.sensors()["gps"])
        # The undecoded tail survives, so a newer firmware is inspectable
        # rather than silently truncated.
        self.assertTrue(status.raw.endswith(b"\xaa\xbb"))

    def test_status_reports_armed(self):
        armed = struct.pack("<HHHIB", 312, 0, 0, 1, 0)
        self.assertTrue(p.decode_status(armed).armed)

    def test_motor_reads_eight_slots(self):
        payload = struct.pack("<8H", 1000, 1100, 1200, 1300, 0, 0, 0, 0)
        self.assertEqual(p.decode_motor(payload)[:4], (1000, 1100, 1200, 1300))


class TestSetMotor(unittest.TestCase):
    def test_short_list_is_padded_with_stopped_not_zero(self):
        payload = p.encode_set_motor([1000, 1000, 1000, 1000])
        self.assertEqual(struct.unpack("<8H", payload),
                         (1000, 1000, 1000, 1000, 1000, 1000, 1000, 1000))

    def test_out_of_range_is_refused(self):
        with self.assertRaises(p.MspError):
            p.encode_set_motor([2500])

    def test_more_than_eight_is_refused(self):
        with self.assertRaises(p.MspError):
            p.encode_set_motor([1000] * 9)


if __name__ == "__main__":
    unittest.main()
