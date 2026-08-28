"""
MultiWii Serial Protocol, host side. Frames only — no serial port here, so
every rule below is testable without a flight controller attached.

MSP is how the bench asks the Meteor75 Pro's flight controller what it is and
tells it to spin a motor. It is somebody else's protocol implemented by
somebody else's firmware, which makes it the one part of this project whose
behaviour is not ours to decide. That is precisely why phase 1b exists: to
find out what Betaflight actually does before three days of work are designed
around an assumption about it.

Two frame formats are in use at once, and both are needed:

  v1  $M<  size cmd payload... crc      crc = XOR of size, cmd, payload
  v2  $X<  flag cmd:u16 size:u16 payload... crc8

v1 carries a single byte of length, so a payload of 256 bytes cannot be
expressed and a command above 255 cannot be addressed. v2 lifts both limits
and swaps the XOR for CRC8/DVB-S2. Betaflight answers v1 for the classic
commands and requires v2 for newer ones, so a client that speaks only one of
them meets a wall at an arbitrary point.

The direction byte matters more than it looks. `<` is a request to the flight
controller, `>` is its reply, and `!` is its refusal — same size, same command,
same shape, and the only thing separating "here is your data" from "I do not
know that command" is one character. Reading `!` as `>` yields a well-formed
frame carrying an empty payload, which decodes to plausible zeros. So the
refusal is raised, never returned.

Framing follows the same rule as tools/serial_logger/protocol.py: read exact
byte counts, never a line. There are no newlines in MSP and any byte value can
appear inside a payload, including 0x24 ('$').
"""

from __future__ import annotations

import struct
from dataclasses import dataclass
from enum import Enum

MSP_HEADER = b"$M"
MSP2_HEADER = b"$X"

DIR_TO_FC = ord("<")
DIR_FROM_FC = ord(">")
DIR_ERROR = ord("!")

# v1 spends one byte on length, so this is a hard ceiling, not a policy.
V1_MAX_PAYLOAD = 255


class MspError(Exception):
    """A frame that could not be interpreted, or a refusal from the FC."""


class MspRefused(MspError):
    """The FC answered '$M!' — it received the frame and declined the command.

    Separate from MspError because the two mean opposite things about the
    link: a refusal proves the serial path works and the command does not,
    which is exactly the distinction phase 1b is trying to draw.
    """


class Cmd(Enum):
    """Command ids as Betaflight numbers them.

    Only the ones phase 1b needs. Values come from the MSP command table
    shared by Cleanflight/Betaflight/iNav; a wrong value here produces a
    refusal rather than wrong data, which is the good failure.
    """

    API_VERSION = 1
    FC_VARIANT = 2
    FC_VERSION = 3
    BOARD_INFO = 4
    BUILD_INFO = 5

    STATUS = 101
    MOTOR = 104
    ANALOG = 110
    BATTERY_STATE = 130
    STATUS_EX = 150
    UID = 160

    SET_MOTOR = 214


def crc8_dvb_s2(crc: int, byte: int) -> int:
    """One byte into a DVB-S2 CRC8, polynomial 0xD5. Used by MSP v2 only."""
    crc ^= byte
    for _ in range(8):
        crc = ((crc << 1) ^ 0xD5) & 0xFF if crc & 0x80 else (crc << 1) & 0xFF
    return crc


def crc8_over(data: bytes, crc: int = 0) -> int:
    for byte in data:
        crc = crc8_dvb_s2(crc, byte)
    return crc


def xor_over(data: bytes, crc: int = 0) -> int:
    for byte in data:
        crc ^= byte
    return crc


@dataclass(frozen=True)
class Frame:
    command: int
    payload: bytes
    version: int          # 1 or 2, as received

    @property
    def size(self) -> int:
        return len(self.payload)


def encode_v1(command: int, payload: bytes = b"") -> bytes:
    if not 0 <= command <= 255:
        raise MspError(f"command {command} does not fit MSP v1; use v2")
    if len(payload) > V1_MAX_PAYLOAD:
        raise MspError(
            f"payload of {len(payload)} bytes does not fit MSP v1; use v2")

    body = bytes([len(payload), command]) + payload
    return MSP_HEADER + bytes([DIR_TO_FC]) + body + bytes([xor_over(body)])


def encode_v2(command: int, payload: bytes = b"", flag: int = 0) -> bytes:
    if not 0 <= command <= 0xFFFF:
        raise MspError(f"command {command} does not fit MSP v2")

    body = struct.pack("<BHH", flag, command, len(payload)) + payload
    return MSP2_HEADER + bytes([DIR_TO_FC]) + body + bytes([crc8_over(body)])


def decode(frame: bytes) -> Frame:
    """Parse one complete frame. Raises rather than guessing.

    Written against whole frames so it can be tested byte-for-byte; the
    incremental reader that feeds it lives in client.py, where the port is.
    """
    if len(frame) < 6:
        raise MspError(f"frame of {len(frame)} bytes is too short to be MSP")

    magic, direction = frame[:2], frame[2]

    if direction == DIR_ERROR:
        # Decoded far enough to name the command, then refused. Which command
        # was rejected is the whole content of the message.
        command = frame[4] if magic == MSP_HEADER else struct.unpack_from(
            "<H", frame, 5)[0]
        raise MspRefused(f"flight controller refused command {command}")

    if direction != DIR_FROM_FC:
        raise MspError(f"direction byte {direction:#04x} is not '>' or '!'")

    if magic == MSP_HEADER:
        return _decode_v1(frame)
    if magic == MSP2_HEADER:
        return _decode_v2(frame)
    raise MspError(f"header {magic!r} is neither $M nor $X")


def _decode_v1(frame: bytes) -> Frame:
    size, command = frame[3], frame[4]
    expected = 5 + size + 1
    if len(frame) != expected:
        raise MspError(
            f"v1 frame declares {size}-byte payload, so it should be "
            f"{expected} bytes; got {len(frame)}")

    payload = frame[5:5 + size]
    want = xor_over(frame[3:5 + size])
    if frame[-1] != want:
        raise MspError(f"v1 checksum {frame[-1]:#04x}, computed {want:#04x}")
    return Frame(command=command, payload=payload, version=1)


def _decode_v2(frame: bytes) -> Frame:
    if len(frame) < 9:
        raise MspError(f"v2 frame of {len(frame)} bytes is too short")

    _flag, command, size = struct.unpack_from("<BHH", frame, 3)
    expected = 8 + size + 1
    if len(frame) != expected:
        raise MspError(
            f"v2 frame declares {size}-byte payload, so it should be "
            f"{expected} bytes; got {len(frame)}")

    payload = frame[8:8 + size]
    want = crc8_over(frame[3:8 + size])
    if frame[-1] != want:
        raise MspError(f"v2 crc8 {frame[-1]:#04x}, computed {want:#04x}")
    return Frame(command=command, payload=payload, version=2)


# ---------------------------------------------------------------------------
# Payload decoders
#
# Only the leading fields are decoded, and the rest of the payload is kept.
# MSP payloads grew fields over Betaflight releases and the tail of a struct
# is not stable across versions; decoding the whole thing against one release
# would turn "this firmware is newer than my table" into wrong numbers rather
# than into a short read. The leading fields have not moved.
# ---------------------------------------------------------------------------


@dataclass(frozen=True)
class ApiVersion:
    msp_protocol: int
    major: int
    minor: int

    def __str__(self) -> str:
        return f"MSP v{self.msp_protocol}, API {self.major}.{self.minor}"


def decode_api_version(payload: bytes) -> ApiVersion:
    if len(payload) < 3:
        raise MspError(f"API_VERSION payload is {len(payload)} bytes, want 3")
    return ApiVersion(*payload[:3])


def decode_fc_variant(payload: bytes) -> str:
    """Four ASCII characters: 'BTFL' for Betaflight, 'INAV', 'CLFL'."""
    if len(payload) < 4:
        raise MspError(f"FC_VARIANT payload is {len(payload)} bytes, want 4")
    return payload[:4].decode("ascii", errors="replace")


@dataclass(frozen=True)
class FcVersion:
    major: int
    minor: int
    patch: int

    def __str__(self) -> str:
        return f"{self.major}.{self.minor}.{self.patch}"


def decode_fc_version(payload: bytes) -> FcVersion:
    if len(payload) < 3:
        raise MspError(f"FC_VERSION payload is {len(payload)} bytes, want 3")
    return FcVersion(*payload[:3])


def decode_board_info(payload: bytes) -> str:
    """Board identifier only — four characters, e.g. 'S411'.

    Everything after it moved between releases, so it stays undecoded.
    """
    if len(payload) < 4:
        raise MspError(f"BOARD_INFO payload is {len(payload)} bytes, want 4")
    return payload[:4].decode("ascii", errors="replace")


# Bit positions in the MSP_STATUS sensor mask.
SENSORS = (
    (0, "accelerometer"),
    (1, "barometer"),
    (2, "magnetometer"),
    (3, "gps"),
    (4, "rangefinder"),
    (5, "gyroscope"),
)


@dataclass(frozen=True)
class Status:
    cycle_time_us: int
    i2c_errors: int
    sensor_mask: int
    flight_mode_flags: int
    pid_profile: int
    raw: bytes

    def sensors(self) -> dict[str, bool]:
        return {name: bool(self.sensor_mask & (1 << bit))
                for bit, name in SENSORS}

    @property
    def armed(self) -> bool:
        """Bit 0 of the flight mode flags is the ARM box.

        Reported for information only. Nothing in this project decides safety
        from it — the propellers being off is what makes a test safe, and that
        is checked by looking at the drone, not by reading a flag.
        """
        return bool(self.flight_mode_flags & 1)


def decode_status(payload: bytes) -> Status:
    if len(payload) < 11:
        raise MspError(f"STATUS payload is {len(payload)} bytes, want 11+")
    cycle, i2c, sensors, modes, profile = struct.unpack_from("<HHHIB", payload)
    return Status(cycle, i2c, sensors, modes, profile, payload)


def decode_motor(payload: bytes) -> tuple[int, ...]:
    """Eight motor outputs, whatever the airframe actually has.

    MSP always reports eight slots; a quad leaves four of them at zero. The
    count is not carried in the frame, so the caller cannot tell "motor 5 is
    stopped" from "motor 5 does not exist" — which is why the number of motors
    comes from the test profile and not from here.
    """
    if len(payload) < 16:
        raise MspError(f"MOTOR payload is {len(payload)} bytes, want 16")
    return struct.unpack_from("<8H", payload)


def encode_set_motor(values: tuple[int, ...] | list[int]) -> bytes:
    """Payload for SET_MOTOR: eight 16-bit values, always all eight.

    Values are raw output units, normally 1000 (stopped) to 2000 (full). A
    short list is padded with 1000 rather than with 0, because 0 is not
    'stopped' on every protocol — on DShot it is, on PWM it is below the
    minimum pulse and means undefined.
    """
    if len(values) > 8:
        raise MspError(f"{len(values)} motors given, MSP carries 8")
    padded = list(values) + [1000] * (8 - len(values))
    for i, value in enumerate(padded):
        if not 0 <= value <= 2000:
            raise MspError(f"motor {i} value {value} is outside 0..2000")
    return struct.pack("<8H", *padded)
