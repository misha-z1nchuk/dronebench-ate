"""
Serial transport for MSP. Everything that touches the port lives here; the
frame rules live in protocol.py and are tested without one.

The flight controller appears as a USB CDC device, so the baud rate is a
formality — the number below is what every other MSP tool sends and is
ignored by both ends. What is not a formality is the framing: MSP has no
delimiter, any byte may appear in a payload, and the only way to know where a
frame ends is to believe its declared length. So the reader takes exact byte
counts and never scans, except when it has lost sync and is looking for the
next '$' to start again.

Three outcomes are kept apart on purpose, because in phase 1b they mean
different things:

    MspTimeout   nothing came back        — link, port, or the FC is not listening
    MspRefused   '$M!' came back          — link works, command does not
    MspError     something came back wrong — link works and is corrupting data

Collapsing them into "it did not work" would throw away the only information
that says which end to look at.
"""

from __future__ import annotations

import time
from dataclasses import dataclass

import serial
from serial.tools import list_ports

import protocol as p

# Ignored by a USB CDC endpoint, but pyserial requires a number and every
# other MSP implementation sends this one.
BAUD = 115200

# One request-response round trip on USB CDC takes single-digit milliseconds.
# A tenth of a second is two orders of margin, and short enough that the
# override-timeout experiment can tell "late" from "never".
REPLY_TIMEOUT_S = 0.1

# Bytes to discard while hunting for the start of a frame before giving up.
# Larger than any MSP payload, so a mid-frame resync always completes; small
# enough that a port full of some other protocol fails fast instead of
# scanning forever.
RESYNC_LIMIT = 1024


class MspTimeout(p.MspError):
    """No reply within the timeout. Says nothing about whether it was heard."""


@dataclass(frozen=True)
class Identity:
    """What the flight controller says it is. Goes into the report header."""

    api: p.ApiVersion
    variant: str
    version: p.FcVersion
    board: str

    def __str__(self) -> str:
        return (f"{self.variant} {self.version} on {self.board} "
                f"({self.api})")


def find_port() -> str:
    """Guess the flight controller's port.

    Betaflight enumerates with STMicroelectronics' vendor id (0x0483) in DFU
    and as a CDC device otherwise. The bench's own ESP32 is a CH340/CP210x, so
    the two are distinguishable — which matters, because sending MSP to the
    ESP32 would produce silence and look exactly like a dead flight
    controller.
    """
    candidates = []
    for port in list_ports.comports():
        name = (port.description or "") + " " + (port.manufacturer or "")
        if port.vid == 0x0483:
            candidates.append((0, port.device))
        elif "STM" in name.upper() or "BETAFLIGHT" in name.upper():
            candidates.append((1, port.device))

    if not candidates:
        seen = ", ".join(pt.device for pt in list_ports.comports()) or "none"
        raise MspTimeout(
            f"no flight controller port found. Ports present: {seen}. "
            "Is the drone connected by USB, and is the cable a data cable?")

    candidates.sort()
    return candidates[0][1]


class MspClient:
    def __init__(self, port: str, timeout: float = REPLY_TIMEOUT_S):
        self.port_name = port
        self._timeout = timeout
        self._serial = serial.Serial(port, BAUD, timeout=timeout)
        # Whatever the previous session left in the driver buffer is not part
        # of any frame this client will send, and reading it as one would put
        # the reader out of step for the rest of the run.
        self._drain()

    def close(self) -> None:
        self._serial.close()

    def __enter__(self) -> "MspClient":
        return self

    def __exit__(self, *_exc) -> None:
        self.close()

    def _drain(self) -> None:
        deadline = time.monotonic() + 0.2
        while time.monotonic() < deadline and self._serial.in_waiting:
            self._serial.read(self._serial.in_waiting)

    def _read_exact(self, count: int) -> bytes:
        data = self._serial.read(count)
        if len(data) != count:
            raise MspTimeout(
                f"wanted {count} bytes, got {len(data)} in {self._timeout}s")
        return data

    def _read_frame(self) -> p.Frame:
        """Read one frame, resynchronising if the stream is mid-frame."""
        for _ in range(RESYNC_LIMIT):
            if self._read_exact(1) == b"$":
                break
        else:
            raise MspTimeout(
                f"no frame start in {RESYNC_LIMIT} bytes — the port is "
                "carrying something that is not MSP")

        kind = self._read_exact(1)
        direction = self._read_exact(1)
        head = b"$" + kind + direction

        if kind == b"M":
            size_and_cmd = self._read_exact(2)
            rest = self._read_exact(size_and_cmd[0] + 1)
        elif kind == b"X":
            size_and_cmd = self._read_exact(5)
            payload_size = int.from_bytes(size_and_cmd[3:5], "little")
            rest = self._read_exact(payload_size + 1)
        else:
            raise p.MspError(f"frame start '$' followed by {kind!r}")

        return p.decode(head + size_and_cmd + rest)

    def request(self, command: int, payload: bytes = b"",
                v2: bool = False) -> p.Frame:
        """Send one command and return its reply.

        Raises MspRefused if the FC declined, MspTimeout if it said nothing.
        """
        frame = (p.encode_v2(command, payload) if v2
                 else p.encode_v1(command, payload))
        self._serial.write(frame)
        self._serial.flush()

        reply = self._read_frame()
        if reply.command != command:
            # A reply to some earlier request, still in flight. Reporting it
            # as the answer to this one would attach the wrong payload to the
            # wrong question — the kind of mistake that reads as a firmware
            # bug for an hour.
            raise p.MspError(
                f"asked for command {command}, got a reply to {reply.command}")
        return reply

    def send(self, command: int, payload: bytes = b"") -> None:
        """Send a command whose reply carries nothing, and consume that reply.

        Measured on Betaflight 4.5.3, 2026-08-28: SET_MOTOR is acknowledged
        with an empty '$M>' frame carrying command 214. The first version of
        this method wrote and returned without reading, on the assumption that
        a setter produces no answer. It does, and the unread acknowledgement
        stayed in the buffer to be collected by the next request() — which
        then held an empty payload under the wrong command id.

        That failure was caught only because request() compares the reply's
        command against the one it asked for. Without that check, an empty
        payload decoded as MSP_MOTOR reads as eight motors at zero: a
        well-formed, plausible, entirely fictional measurement.

        The acknowledgement costs a round trip, a few milliseconds on USB CDC.
        That is small against the repeat periods this spike uses, and it is
        not optional anyway — the bytes exist whether or not they are read.
        """
        self._serial.write(p.encode_v1(command, payload))
        self._serial.flush()

        ack = self._read_frame()
        if ack.command != command:
            raise p.MspError(
                f"sent command {command}, acknowledgement names "
                f"{ack.command}")

    # -- convenience wrappers, one per thing phase 1b step 1 asks for --------

    def identity(self) -> Identity:
        return Identity(
            api=p.decode_api_version(
                self.request(p.Cmd.API_VERSION.value).payload),
            variant=p.decode_fc_variant(
                self.request(p.Cmd.FC_VARIANT.value).payload),
            version=p.decode_fc_version(
                self.request(p.Cmd.FC_VERSION.value).payload),
            board=p.decode_board_info(
                self.request(p.Cmd.BOARD_INFO.value).payload),
        )

    def status(self) -> p.Status:
        return p.decode_status(self.request(p.Cmd.STATUS.value).payload)

    def motors(self) -> tuple[int, ...]:
        return p.decode_motor(self.request(p.Cmd.MOTOR.value).payload)
