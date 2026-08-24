"""
Host side of the telemetry protocol. The wire format is defined by
firmware/core/protocol/telemetry.c, and this file mirrors it deliberately —
including the refusals.

The firmware rejects a line it cannot fully interpret rather than salvaging
the fields that survived, because a wrong number shaped like a measurement
gets believed. A reader that is more forgiving than the writer undoes that:
it would accept, and log to CSV, exactly the corrupt lines the firmware went
to the trouble of never producing.

So the rules here are the same ones:

  * nothing is interpreted before a valid header
  * a version other than the one below is refused, in both directions
  * every field must be present and must parse completely
  * voltage and current must be finite; only the reference channel may be NaN

Deliberately free of pyserial, so the parsing can be tested without a board.
"""

from __future__ import annotations

import math
from dataclasses import dataclass
from enum import Enum

# Must match TELEMETRY_VERSION in firmware/core/include/dronebench/telemetry.h.
# v1 put three fields on an S line and one combined refusal counter on a U
# line, so reading a v1 log as v2 would silently lose the reference channel.
PROTOCOL_VERSION = 2

# Must match SAMPLE_FIELDS in firmware/core/protocol/telemetry.c.
SAMPLE_FIELDS = 4

# Must match TELEMETRY_LINE_MAX. Anything longer never came from the firmware.
LINE_MAX = 192


class ProtocolError(Exception):
    """A line that could not be interpreted. Carries why, for the error log."""


class LineKind(Enum):
    HEADER = "header"
    SAMPLE = "sample"
    SUMMARY = "summary"
    BLANK = "blank"
    CONSOLE = "console"   # an OK,/ERR, reply — the CLI shares this UART


@dataclass(frozen=True)
class Header:
    version: int
    rate_hz: int


@dataclass(frozen=True)
class Sample:
    t_us: int
    voltage_v: float
    current_a: float
    current_ref_a: float  # NaN while no INA226 is fitted

    CSV_COLUMNS = ("t_us", "voltage_v", "current_a", "current_ref_a")

    def csv_row(self) -> tuple:
        return (self.t_us, self.voltage_v, self.current_a, self.current_ref_a)


@dataclass(frozen=True)
class Summary:
    sample_count: int
    consumed_mah: float
    consumed_wh: float
    min_voltage_v: float
    max_current_a: float
    sensor_failures: int
    rejected_time: int
    rejected_value: int
    gaps: int
    dropped: int

    CSV_COLUMNS = (
        "sample_count",
        "consumed_mah",
        "consumed_wh",
        "min_voltage_v",
        "max_current_a",
        "sensor_failures",
        "rejected_time",
        "rejected_value",
        "gaps",
        "dropped",
    )

    def csv_row(self) -> tuple:
        return tuple(getattr(self, name) for name in self.CSV_COLUMNS)

    @property
    def refused(self) -> int:
        """Every attempt that produced no sample, whatever the cause."""
        return self.sensor_failures + self.rejected_time + self.rejected_value

    def is_trustworthy(self) -> bool:
        """
        False when the figures describe less than they appear to.

        sample_count is the field that makes this answerable at all: without
        it, min_voltage_v of 0.000 from a session whose sensors never answered
        is indistinguishable from a flat pack.
        """
        return self.sample_count > 0 and self.refused == 0 and self.dropped == 0


# --- field parsing --------------------------------------------------------
#
# int() and float() already refuse trailing rubbish, which is the check the C
# side has to do by hand with an end pointer. What they do not refuse is
# "inf" and "nan", so those are handled explicitly below.


def _u32(text: str, field: str) -> int:
    try:
        value = int(text, 10)
    except ValueError:
        raise ProtocolError(f"{field}: {text!r} is not an integer") from None
    if not 0 <= value <= 0xFFFFFFFF:
        raise ProtocolError(f"{field}: {value} does not fit in uint32")
    return value


def _u64(text: str, field: str) -> int:
    try:
        value = int(text, 10)
    except ValueError:
        raise ProtocolError(f"{field}: {text!r} is not an integer") from None
    if not 0 <= value <= 0xFFFFFFFFFFFFFFFF:
        raise ProtocolError(f"{field}: {value} does not fit in uint64")
    return value


def _finite(text: str, field: str) -> float:
    try:
        value = float(text)
    except ValueError:
        raise ProtocolError(f"{field}: {text!r} is not a number") from None
    if not math.isfinite(value):
        # float("1e400") is inf with no error raised, which is exactly the
        # shape of a divide-by-zero calibration coefficient reaching the wire.
        raise ProtocolError(f"{field}: {text!r} is not finite")
    return value


def _finite_or_nan(text: str, field: str) -> float:
    """
    For the reference channel only. NaN means "no reading", which has to stay
    distinguishable from a reading of zero: a shorted shunt and a missing chip
    are not the same fault. Infinity is still a refusal.
    """
    try:
        value = float(text)
    except ValueError:
        raise ProtocolError(f"{field}: {text!r} is not a number") from None
    if math.isinf(value):
        raise ProtocolError(f"{field}: {text!r} is infinite")
    return value


def _named_fields(body: str) -> dict[str, str]:
    fields: dict[str, str] = {}
    for part in body.split(","):
        key, sep, value = part.partition("=")
        if not sep:
            raise ProtocolError(f"field {part!r} is not key=value")
        if key in fields:
            raise ProtocolError(f"field {key!r} appears twice")
        fields[key] = value
    return fields


def _require(fields: dict[str, str], keys: tuple[str, ...]) -> None:
    missing = [k for k in keys if k not in fields]
    if missing:
        raise ProtocolError(f"missing field(s): {', '.join(missing)}")


# --- line parsers ---------------------------------------------------------

_SUMMARY_KEYS = (
    "n",
    "mah",
    "wh",
    "vmin",
    "imax",
    "sensor",
    "rej_time",
    "rej_val",
    "gaps",
    "drop",
)


def _parse_header(body: str) -> Header:
    fields = _named_fields(body)
    _require(fields, ("v", "rate_hz"))

    version = _u32(fields["v"], "v")
    if version != PROTOCOL_VERSION:
        raise ProtocolError(
            f"protocol version {version}, this reader speaks "
            f"{PROTOCOL_VERSION}"
        )
    # Unknown keys are ignored on purpose, matching the firmware: a later
    # version adding a field must not break a reader that predates it.
    return Header(version=version, rate_hz=_u32(fields["rate_hz"], "rate_hz"))


def _parse_sample(body: str) -> Sample:
    parts = body.split(",")
    if len(parts) != SAMPLE_FIELDS:
        raise ProtocolError(
            f"expected {SAMPLE_FIELDS} fields, got {len(parts)}"
        )
    return Sample(
        t_us=_u64(parts[0], "t_us"),
        voltage_v=_finite(parts[1], "voltage_v"),
        current_a=_finite(parts[2], "current_a"),
        current_ref_a=_finite_or_nan(parts[3], "current_ref_a"),
    )


def _parse_summary(body: str) -> Summary:
    fields = _named_fields(body)
    _require(fields, _SUMMARY_KEYS)
    return Summary(
        sample_count=_u32(fields["n"], "n"),
        consumed_mah=_finite(fields["mah"], "mah"),
        consumed_wh=_finite(fields["wh"], "wh"),
        min_voltage_v=_finite(fields["vmin"], "vmin"),
        max_current_a=_finite(fields["imax"], "imax"),
        sensor_failures=_u32(fields["sensor"], "sensor"),
        rejected_time=_u32(fields["rej_time"], "rej_time"),
        rejected_value=_u32(fields["rej_val"], "rej_val"),
        gaps=_u32(fields["gaps"], "gaps"),
        dropped=_u32(fields["drop"], "drop"),
    )


class Parser:
    """
    Stateful because the header is: until one arrives there is no way to know
    what the positional fields of an S line mean, and guessing would produce
    numbers rather than errors.
    """

    def __init__(self) -> None:
        self.header: Header | None = None
        self.lines_parsed = 0
        self.lines_rejected = 0
        self.lines_console = 0

    def reset(self) -> None:
        """Called when the link drops: the next stream negotiates again."""
        self.header = None

    def feed_line(self, line: str) -> tuple[LineKind, object]:
        """
        Returns (kind, value). Raises ProtocolError for anything it will not
        interpret; the caller is expected to log that rather than ignore it.
        """
        line = line.rstrip("\r\n")

        if len(line) >= LINE_MAX:
            self.lines_rejected += 1
            raise ProtocolError(
                f"line of {len(line)} chars exceeds the {LINE_MAX} the "
                f"firmware can emit"
            )

        # Blank lines come from terminal echo and line noise. They carry
        # nothing, but they are not corruption — counting them as rejections
        # would make the rejection count useless for judging link quality.
        if not line:
            return (LineKind.BLANK, None)

        # The console shares this wire, so OK,/ERR, replies are a normal part
        # of the stream — not telemetry, and emphatically not corruption.
        # Filing them as parse errors would bury the real ones. They are also
        # the only place a refusal ever explains itself, so they are kept.
        if line.startswith("OK,") or line.startswith("ERR,"):
            self.lines_console += 1
            return (LineKind.CONSOLE, line)

        try:
            if line.startswith("TLM,"):
                header = _parse_header(line[4:])
                self.header = header
                self.lines_parsed += 1
                return (LineKind.HEADER, header)

            if self.header is None:
                raise ProtocolError("data before a valid header")

            if line.startswith("S,"):
                value = _parse_sample(line[2:])
                self.lines_parsed += 1
                return (LineKind.SAMPLE, value)

            if line.startswith("U,"):
                value = _parse_summary(line[2:])
                self.lines_parsed += 1
                return (LineKind.SUMMARY, value)

            raise ProtocolError(f"unknown line type: {line[:16]!r}")
        except ProtocolError:
            self.lines_rejected += 1
            raise


class LineAssembler:
    """
    Turns arbitrary byte chunks into complete lines.

    This exists instead of serial.readline() for one reason: readline() with a
    timeout returns whatever bytes it has when the timeout expires, and the
    caller cannot tell that result from a line that genuinely ended. A serial
    port delivers bytes, not lines — a 28-byte sample line routinely arrives
    as 11 bytes and then 17, and at 460800 baud with a 500 Hz stream it
    happens constantly. Feeding a half line to the parser would produce a
    rejection where nothing was actually wrong.

    So a line is only released once its terminating newline has been seen.
    """

    def __init__(self, limit: int = LINE_MAX * 4) -> None:
        self._buf = bytearray()
        self._limit = limit
        self.overruns = 0

    def feed(self, chunk: bytes):
        """Yields complete lines, decoded. The unterminated tail is kept."""
        self._buf.extend(chunk)

        while True:
            index = self._buf.find(b"\n")
            if index < 0:
                break
            raw = bytes(self._buf[:index])
            del self._buf[: index + 1]
            # Line noise is not valid ASCII. Replacing rather than raising
            # keeps one corrupt byte from killing the reader; the substituted
            # character then fails to parse as a number, which is correct.
            yield raw.decode("ascii", errors="replace")

        if len(self._buf) > self._limit:
            # No newline in far more bytes than a line can hold: the stream is
            # not this protocol, or the port is producing noise. Dropping the
            # buffer silently would stall the reader forever with no symptom.
            self.overruns += 1
            self._buf.clear()

    def pending(self) -> int:
        return len(self._buf)
